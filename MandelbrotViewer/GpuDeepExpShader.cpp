#include "pch.h"
#include "GpuFractalShaders.h"

namespace Mandelbrot::detail
{
    namespace
    {
        // Shader 4: Ultra-Deep FloatExp Perturbation + FloatExp BLA (zoom >= 1e30 .. 1e300).
        constexpr const char* kPerturbDeepExpShaderSrc = R"(
#define DEEP_LDS_FAST_SIZE 1024u
#define DEEP_LDS_EXP_SIZE 512u

cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint smoothFlag;
    uint useBla;
    uint blaNumLevels;
    uint rowOffset;
    uint subpixelTotal;
    float blaMaxRadMant;
    int blaMaxRadExp;
    float refCx;
    float refCy;
    uint outRowOffset;
    uint stageIterLimit;
    uint subRayOffset;
};

struct GpuOrbitPointExp
{
    float rmx, rmy;
    int rExp;
    int pad0;
};

struct GpuCoordExp
{
    float mant;
    int exp;
};

struct GpuBlaBlockExp
{
    float ar, ai;
    float br, bi;
    float radMant;
    int aExp, bExp, radExp;
};

struct GpuPerturbState
{
    float x, y;
    int exp;
    uint m;
    uint iter;
};

Buffer<float2> OrbitFast : register(t0);
StructuredBuffer<GpuOrbitPointExp> OrbitFull : register(t1);
StructuredBuffer<GpuCoordExp> ColCoordsExp : register(t2);
StructuredBuffer<GpuCoordExp> RowCoordsExp : register(t3);
StructuredBuffer<GpuBlaBlockExp> BlaBlocks : register(t4);
StructuredBuffer<uint2> BlaMeta : register(t5);
Buffer<uint> SubpixelIndices : register(t6);
Buffer<uint> SubpixelCountSrv : register(t7);
Buffer<uint> SubpixelPixels : register(t8);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);
RWStructuredBuffer<GpuPerturbState> ActiveState : register(u2);
RWBuffer<uint> OutActiveIndices : register(u3);
RWBuffer<uint> OutActiveCount : register(u4);

groupshared float2 s_orbitFast[DEEP_LDS_FAST_SIZE];
groupshared float2 s_orbitRm[DEEP_LDS_EXP_SIZE];
groupshared int s_orbitExp[DEEP_LDS_EXP_SIZE];
groupshared uint s_groupCount;
groupshared uint s_groupBase;

void Renorm2(inout float mx, inout float my, inout int e)
{
    float a = max(abs(mx), abs(my));
    if (!(a > 0.0f)) { mx = 0.0f; my = 0.0f; e = -1073741824; return; }
    if (a < 1.1754944e-38f)
    {
        mx *= 16777216.0f;
        my *= 16777216.0f;
        e -= 24;
        a = max(abs(mx), abs(my));
        if (!(a > 0.0f)) { mx = 0.0f; my = 0.0f; e = -1073741824; return; }
    }
    uint bits = asuint(a);
    int biasedExp = int((bits >> 23) & 255u);
    if (biasedExp >= 255) { mx = 0.0f; my = 0.0f; e = -1073741824; return; }
    if (biasedExp >= 253)
    {
        mx *= 0.25f;
        my *= 0.25f;
        e += 2;
        biasedExp -= 2;
    }
    int rawExp = biasedExp - 126;
    int scaleBiased = clamp(253 - biasedExp, 1, 254);
    float scale = asfloat(uint(scaleBiased) << 23);
    mx *= scale;
    my *= scale;
    e += rawExp;
}

void FloatExpAdd(float ax, float ay, int ae, float bx, float by, int be, out float ox, out float oy, out int oe)
{
    if (ae <= -100000000) { ox = bx; oy = by; oe = be; Renorm2(ox, oy, oe); return; }
    if (be <= -100000000) { ox = ax; oy = ay; oe = ae; Renorm2(ox, oy, oe); return; }
    if (ae >= be)
    {
        int diff = ae - be;
        if (diff >= 30) { ox = ax; oy = ay; oe = ae; Renorm2(ox, oy, oe); return; }
        float s = asfloat(uint(127 - diff) << 23);
        ox = ax + bx * s; oy = ay + by * s; oe = ae;
    }
    else
    {
        int diff = be - ae;
        if (diff >= 30) { ox = bx; oy = by; oe = be; Renorm2(ox, oy, oe); return; }
        float s = asfloat(uint(127 - diff) << 23);
        ox = bx + ax * s; oy = by + ay * s; oe = be;
    }
    Renorm2(ox, oy, oe);
}

bool FloatExpLess(float dmx, float dmy, int de, float radMant, int radExp)
{
    if (radExp <= -100000000) return false;
    if (de <= -100000000) return true;
    int diff = de - radExp;
    if (diff < -1) return true;
    if (diff > 1) return false;
    float s = asfloat(uint(127 + diff) << 23);
    return (dmx * dmx + dmy * dmy) * (s * s) < (radMant * radMant);
}

bool StepDeepExp(
    GpuCoordExp cx, GpuCoordExp cy, uint fastLim, uint expLim, uint iterLimit,
    inout float dmx, inout float dmy, inout int dExp, inout uint m, inout uint iter,
    out float outResult)
{
    outResult = -1.0f;

    float dcMx = 0.0f, dcMy = 0.0f;
    int dcExp = -1073741824;
    if (cx.exp > -100000000 || cy.exp > -100000000)
    {
        dcExp = max(cx.exp, cy.exp);
        dcMx = (cx.exp > -100000000 && (dcExp - cx.exp) < 30)
            ? (cx.mant * asfloat(uint(127 - (dcExp - cx.exp)) << 23))
            : 0.0f;
        dcMy = (cy.exp > -100000000 && (dcExp - cy.exp) < 30)
            ? (cy.mant * asfloat(uint(127 - (dcExp - cy.exp)) << 23))
            : 0.0f;
        Renorm2(dcMx, dcMy, dcExp);
    }

    float dcxF = (dcExp > -126) ? (dcMx * asfloat(uint(clamp(127 + dcExp, 1, 254)) << 23)) : 0.0f;
    float dcyF = (dcExp > -126) ? (dcMy * asfloat(uint(clamp(127 + dcExp, 1, 254)) << 23)) : 0.0f;
    float blaMaxRadF = (useBla != 0u && blaMaxRadExp >= -120 && blaMaxRadExp <= 10)
        ? (blaMaxRadMant * asfloat(uint(127 + blaMaxRadExp) << 23))
        : 0.0f;
    float fastRebaseExitFloor = max(blaMaxRadF, 1e-12f);

    uint stopIter = min(iterLimit, maxIter);
    float escZx = 0.0f, escZy = 0.0f;
    int escExp = 0;
    bool escaped = false;

    // Exact cycle check on m == 0 rebases (zero instructions inside tight inner loops)
    float chkMx = 2.0f, chkMy = 2.0f;
    int chkExp = -1073741823;
    uint rebasePeriod = 4u, rebasesSince = 0u;

    [loop]
    while (iter < stopIter)
    {
        // Only enter the un-accelerated float32 fast loop once |dz| is above all BLA block radii.
        if (dExp >= -45 && dExp <= 10 && m > 0u &&
            (useBla == 0u || !FloatExpLess(dmx, dmy, dExp, blaMaxRadMant, blaMaxRadExp)))
        {
            float initScale = asfloat(uint(127 + dExp) << 23);
            float dx = dmx * initScale;
            float dy = dmy * initScale;
            bool leftFast = false;
            [loop]
            while (iter < stopIter)
            {
                float2 r = (m < fastLim) ? s_orbitFast[m] : OrbitFast[m];
                float rx = r.x, ry = r.y;
                float zx = rx + dx;
                float zy = ry + dy;
                float mod2 = zx * zx + zy * zy;
                if (mod2 > 16.0f) { escZx = zx; escZy = zy; escaped = true; break; }
                float dz2 = dx * dx + dy * dy;
                if (m == refLen || (m > 0u && mod2 < dz2))
                {
                    dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
                    if (max(abs(dx), abs(dy)) < fastRebaseExitFloor)
                    {
                        dmx = dx; dmy = dy; dExp = 0;
                        Renorm2(dmx, dmy, dExp);
                        leftFast = true;
                        break;
                    }
                }
                float sx = rx + zx;
                float sy = ry + zy;
                float nextDx = sx * dx - sy * dy + dcxF;
                dy = sx * dy + sy * dx + dcyF;
                dx = nextDx;
                ++m;
                ++iter;
                if (max(abs(dx), abs(dy)) < 2.842171e-14f)
                {
                    dmx = dx; dmy = dy; dExp = 0;
                    Renorm2(dmx, dmy, dExp);
                    leftFast = true;
                    break;
                }
            }
            if (escaped) break;
            if (!leftFast)
            {
                // Stage limit reached while inside float32 fast loop: pack back into FloatExp state.
                dmx = dx; dmy = dy; dExp = 0;
                Renorm2(dmx, dmy, dExp);
                break;
            }
            continue;
        }

        float rmx, rmy;
        int rExp;
        if (m < expLim)
        {
            float2 rm = s_orbitRm[m];
            rmx = rm.x;
            rmy = rm.y;
            rExp = s_orbitExp[m];
        }
        else
        {
            GpuOrbitPointExp op = OrbitFull[m];
            rmx = op.rmx;
            rmy = op.rmy;
            rExp = op.rExp;
        }

        if (dExp > 10)
        {
            escZx = dmx;
            escZy = dmy;
            escExp = dExp;
            escaped = true;
            break;
        }
        if (rExp >= 1 || dExp >= 1)
        {
            float rScale = (rExp >= -125 && rExp <= 10) ? asfloat(uint(127 + rExp) << 23) : 0.0f;
            float dScale = (dExp >= -125 && dExp <= 10) ? asfloat(uint(127 + dExp) << 23) : 0.0f;
            float zx = rmx * rScale + dmx * dScale;
            float zy = rmy * rScale + dmy * dScale;
            if (zx * zx + zy * zy > 16.0f) { escZx = zx; escZy = zy; escExp = 0; escaped = true; break; }
        }

        bool doRebase = (m == refLen);
        float zmx = 0.0f, zmy = 0.0f;
        int zExp = -1073741824;
        if (doRebase || (m > 0u && rExp <= dExp + 2))
        {
            FloatExpAdd(rmx, rmy, rExp, dmx, dmy, dExp, zmx, zmy, zExp);
            if (!doRebase && m > 0u)
            {
                int cExp = max(zExp, dExp);
                if (cExp > -100000000)
                {
                    float zs = (zExp > -100000000 && (cExp - zExp) < 30) ? asfloat(uint(127 - (cExp - zExp)) << 23) : 0.0f;
                    float ds = (dExp > -100000000 && (cExp - dExp) < 30) ? asfloat(uint(127 - (cExp - dExp)) << 23) : 0.0f;
                    float zxS = zmx * zs, zyS = zmy * zs;
                    float dxS = dmx * ds, dyS = dmy * ds;
                    if (zxS * zxS + zyS * zyS < dxS * dxS + dyS * dyS) doRebase = true;
                }
            }
        }
        if (doRebase)
        {
            dmx = zmx; dmy = zmy; dExp = zExp; m = 0u;
            rmx = 0.0f; rmy = 0.0f; rExp = -1073741824;
            if (dExp == chkExp && dmx == chkMx && dmy == chkMy)
            {
                iter = maxIter;
                return true;
            }
            if (++rebasesSince >= rebasePeriod)
            {
                chkMx = dmx; chkMy = dmy; chkExp = dExp;
                rebasesSince = 0u;
                if (rebasePeriod < 256u) rebasePeriod <<= 1u;
            }
        }

        if (useBla != 0u && m >= 1u && (maxIter - iter) >= 4u && ((m - 1u) & 3u) == 0u && m + 4u <= refLen &&
            FloatExpLess(dmx, dmy, dExp, blaMaxRadMant, blaMaxRadExp))
        {
            uint start = m - 1u;
            uint aligned = (start != 0u) ? firstbitlow(start) : 30u;
            uint top = min(aligned, blaNumLevels - 1u);
            uint skipped = 0u;
            for (int lv = int(top); lv >= 2; --lv)
            {
                uint len = 1u << uint(lv);
                if (len > (maxIter - iter) || m + len > refLen) continue;
                uint2 meta = BlaMeta[lv];
                uint idx = start >> uint(lv);
                if (idx >= meta.y) continue;
                GpuBlaBlockExp blk = BlaBlocks[meta.x + idx];
                if (!FloatExpLess(dmx, dmy, dExp, blk.radMant, blk.radExp)) continue;

                float azx = blk.ar * dmx - blk.ai * dmy;
                float azy = blk.ar * dmy + blk.ai * dmx;
                int azExp = (dExp > -100000000 && blk.aExp > -100000000) ? (blk.aExp + dExp) : -1073741824;

                float bcx = blk.br * dcMx - blk.bi * dcMy;
                float bcy = blk.br * dcMy + blk.bi * dcMx;
                int bcExp = (dcExp > -100000000 && blk.bExp > -100000000) ? (blk.bExp + dcExp) : -1073741824;

                FloatExpAdd(azx, azy, azExp, bcx, bcy, bcExp, dmx, dmy, dExp);
                skipped = len;
                break;
            }
            if (skipped != 0u) { m += skipped; iter += skipped; continue; }
        }

        float smx = dmx, smy = dmy;
        int sExp = dExp;
        if (rExp > -100000000)
        {
            FloatExpAdd(rmx, rmy, rExp + 1, dmx, dmy, dExp, smx, smy, sExp);
        }
        float prodX = smx * dmx - smy * dmy;
        float prodY = smx * dmy + smy * dmx;
        int prodExp = (sExp > -100000000 && dExp > -100000000) ? (sExp + dExp) : -1073741824;
        FloatExpAdd(prodX, prodY, prodExp, dcMx, dcMy, dcExp, dmx, dmy, dExp);
        ++m;
        ++iter;
    }

    if (!escaped)
    {
        if (iter >= maxIter) return true;
        return false;
    }

    if (smoothFlag == 0u)
    {
        outResult = float(iter);
        return true;
    }

    if (escExp > 0)
    {
        float log2Mod = float(escExp) + 0.5f * log2(max(escZx * escZx + escZy * escZy, 1e-30f));
        float logZ = max(log2Mod * 0.69314718f, 1e-6f);
        outResult = max(0.0f, float(iter) + 1.0f - log2(logZ));
        return true;
    }

    float zr = escZx, zi = escZy;
    float zr2 = zr * zr, zi2 = zi * zi;
    uint sIter = iter;
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        zi = 2.0f * zr * zi + refCy;
        zr = zr2 - zi2 + refCx;
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }
    outResult = max(0.0f, float(sIter) + 1.0f - log2(log(zr2 + zi2) * 0.5f));
    return true;
}

float EvalDeepExp(GpuCoordExp cx, GpuCoordExp cy, uint fastLim, uint expLim)
{
    float dmx = 0.0f, dmy = 0.0f;
    int dExp = -1073741824;
    uint m = 0u, iter = 0u;
    float res = -1.0f;
    StepDeepExp(cx, cy, fastLim, expLim, maxIter, dmx, dmy, dExp, m, iter, res);
    return res;
}

void LoadDeepLds(uint gi, out uint fastLim, out uint expLim)
{
    fastLim = min(refLen + 1u, DEEP_LDS_FAST_SIZE);
    expLim = min(refLen + 1u, DEEP_LDS_EXP_SIZE);
    for (uint i = gi; i < fastLim; i += 256u)
        s_orbitFast[i] = OrbitFast[i];
    for (uint j = gi; j < expLim; j += 256u)
    {
        GpuOrbitPointExp pt = OrbitFull[j];
        s_orbitRm[j] = float2(pt.rmx, pt.rmy);
        s_orbitExp[j] = pt.rExp;
    }
}

[numthreads(16, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint fastLim, expLim;
    LoadDeepLds(gi, fastLim, expLim);
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[(tid.y + outRowOffset) * width + tid.x] = EvalDeepExp(ColCoordsExp[tid.x * 3u + 1u], RowCoordsExp[py * 3u + 1u], fastLim, expLim);
}

[numthreads(16, 16, 1)]
void CSPrimaryStage0(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    uint fastLim, expLim;
    LoadDeepLds(gi, fastLim, expLim);
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    bool valid = (tid.x < width && py < height);
    bool keepAlive = false;
    uint fullIdx = py * width + tid.x;

    if (valid)
    {
        float dmx = 0.0f, dmy = 0.0f;
        int dExp = -1073741824;
        uint m = 0u, iter = 0u;
        float res = -1.0f;
        bool done = StepDeepExp(ColCoordsExp[tid.x * 3u + 1u], RowCoordsExp[py * 3u + 1u], fastLim, expLim, stageIterLimit, dmx, dmy, dExp, m, iter, res);
        if (done)
        {
            OutIters[fullIdx] = res;
        }
        else
        {
            GpuPerturbState st;
            st.x = dmx; st.y = dmy; st.exp = dExp; st.m = m; st.iter = iter;
            ActiveState[fullIdx] = st;
            keepAlive = true;
        }
    }

    uint localOffset = 0u;
    if (keepAlive)
    {
        InterlockedAdd(s_groupCount, 1u, localOffset);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0u && s_groupCount > 0u)
    {
        InterlockedAdd(OutActiveCount[0], s_groupCount, s_groupBase);
    }
    GroupMemoryBarrierWithGroupSync();

    if (keepAlive)
    {
        OutActiveIndices[s_groupBase + localOffset] = fullIdx;
    }
}

[numthreads(256, 1, 1)]
void CSPrimaryResume(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    uint fastLim, expLim;
    LoadDeepLds(gi, fastLim, expLim);
    GroupMemoryBarrierWithGroupSync();

    uint activeTotal = SubpixelCountSrv[0];
    uint activeIdx = (gid.y * 32768u + gid.x) * 256u + gi;
    bool valid = (activeIdx < activeTotal);
    bool keepAlive = false;
    uint pix = 0u;

    if (valid)
    {
        pix = SubpixelIndices[activeIdx];
        uint py = pix / width;
        uint px = pix - py * width;
        GpuPerturbState st = ActiveState[pix];
        float dmx = st.x, dmy = st.y;
        int dExp = st.exp;
        uint m = st.m, iter = st.iter;
        float res = -1.0f;
        bool done = StepDeepExp(ColCoordsExp[px * 3u + 1u], RowCoordsExp[py * 3u + 1u], fastLim, expLim, stageIterLimit, dmx, dmy, dExp, m, iter, res);
        if (done)
        {
            OutIters[pix] = res;
        }
        else
        {
            st.x = dmx; st.y = dmy; st.exp = dExp; st.m = m; st.iter = iter;
            ActiveState[pix] = st;
            keepAlive = true;
        }
    }

    if (stageIterLimit < maxIter)
    {
        uint localOffset = 0u;
        if (keepAlive)
        {
            InterlockedAdd(s_groupCount, 1u, localOffset);
        }
        GroupMemoryBarrierWithGroupSync();

        if (gi == 0u && s_groupCount > 0u)
        {
            InterlockedAdd(OutActiveCount[0], s_groupCount, s_groupBase);
        }
        GroupMemoryBarrierWithGroupSync();

        if (keepAlive)
        {
            OutActiveIndices[s_groupBase + localOffset] = pix;
        }
    }
}

[numthreads(256, 1, 1)]
void CSSubpixels(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint fastLim, expLim;
    LoadDeepLds(gi, fastLim, expLim);
    GroupMemoryBarrierWithGroupSync();

    uint activeTotal = (subpixelTotal == 0xFFFFFFFFu) ? (SubpixelCountSrv[0] * 4u) : subpixelTotal;
    uint sIdx = (gid.y * 32768u + gid.x) * 256u + gi;
    if (sIdx >= activeTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalDeepExp(ColCoordsExp[px * 3u + kx], RowCoordsExp[py * 3u + ky], fastLim, expLim);
}

[numthreads(256, 1, 1)]
void CSSubpixelsStage0(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    uint fastLim, expLim;
    LoadDeepLds(gi, fastLim, expLim);
    GroupMemoryBarrierWithGroupSync();

    uint totalRays = SubpixelCountSrv[0] * 4u;
    uint rem = (totalRays > subRayOffset) ? (totalRays - subRayOffset) : 0u;
    uint batchRays = (subpixelTotal == 0xFFFFFFFFu) ? rem : min(rem, subpixelTotal);
    uint localIdx = (gid.y * 32768u + gid.x) * 256u + gi;
    bool valid = (localIdx < batchRays);
    bool keepAlive = false;
    uint sIdx = subRayOffset + localIdx;

    if (valid)
    {
        uint pix = SubpixelIndices[sIdx >> 2];
        uint corner = sIdx & 3u;
        uint py = pix / width;
        uint px = pix - py * width;
        uint kx = (corner & 1u) ? 2u : 0u;
        uint ky = (corner & 2u) ? 2u : 0u;
        float dmx = 0.0f, dmy = 0.0f;
        int dExp = -1073741824;
        uint m = 0u, iter = 0u;
        float res = -1.0f;
        bool done = StepDeepExp(ColCoordsExp[px * 3u + kx], RowCoordsExp[py * 3u + ky], fastLim, expLim, stageIterLimit, dmx, dmy, dExp, m, iter, res);
        if (done)
        {
            OutSubpixelsFlat[sIdx] = res;
        }
        else
        {
            GpuPerturbState st;
            st.x = dmx; st.y = dmy; st.exp = dExp; st.m = m; st.iter = iter;
            ActiveState[localIdx] = st;
            keepAlive = true;
        }
    }

    uint localOffset = 0u;
    if (keepAlive)
    {
        InterlockedAdd(s_groupCount, 1u, localOffset);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0u && s_groupCount > 0u)
    {
        InterlockedAdd(OutActiveCount[0], s_groupCount, s_groupBase);
    }
    GroupMemoryBarrierWithGroupSync();

    if (keepAlive)
    {
        OutActiveIndices[s_groupBase + localOffset] = sIdx;
    }
}

[numthreads(256, 1, 1)]
void CSSubpixelsResume(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    uint fastLim, expLim;
    LoadDeepLds(gi, fastLim, expLim);
    GroupMemoryBarrierWithGroupSync();

    uint activeTotal = SubpixelCountSrv[0];
    uint activeIdx = (gid.y * 32768u + gid.x) * 256u + gi;
    bool valid = (activeIdx < activeTotal);
    bool keepAlive = false;
    uint sIdx = 0u;

    if (valid)
    {
        sIdx = SubpixelIndices[activeIdx];
        uint localIdx = sIdx - subRayOffset;
        uint pix = SubpixelPixels[sIdx >> 2];
        uint corner = sIdx & 3u;
        uint py = pix / width;
        uint px = pix - py * width;
        uint kx = (corner & 1u) ? 2u : 0u;
        uint ky = (corner & 2u) ? 2u : 0u;
        GpuPerturbState st = ActiveState[localIdx];
        float dmx = st.x, dmy = st.y;
        int dExp = st.exp;
        uint m = st.m, iter = st.iter;
        float res = -1.0f;
        bool done = StepDeepExp(ColCoordsExp[px * 3u + kx], RowCoordsExp[py * 3u + ky], fastLim, expLim, stageIterLimit, dmx, dmy, dExp, m, iter, res);
        if (done)
        {
            OutSubpixelsFlat[sIdx] = res;
        }
        else
        {
            st.x = dmx; st.y = dmy; st.exp = dExp; st.m = m; st.iter = iter;
            ActiveState[localIdx] = st;
            keepAlive = true;
        }
    }

    if (stageIterLimit < maxIter)
    {
        uint localOffset = 0u;
        if (keepAlive)
        {
            InterlockedAdd(s_groupCount, 1u, localOffset);
        }
        GroupMemoryBarrierWithGroupSync();

        if (gi == 0u && s_groupCount > 0u)
        {
            InterlockedAdd(OutActiveCount[0], s_groupCount, s_groupBase);
        }
        GroupMemoryBarrierWithGroupSync();

        if (keepAlive)
        {
            OutActiveIndices[s_groupBase + localOffset] = sIdx;
        }
    }
}
)";
    }

    const char* GetPerturbDeepExpShaderSource() noexcept
    {
        return kPerturbDeepExpShaderSrc;
    }
}
