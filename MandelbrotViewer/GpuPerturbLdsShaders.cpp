#include "pch.h"
#include "GpuFractalShaders.h"

namespace Mandelbrot::detail
{
    namespace
    {
        // Shader 2: High-occupancy 512-thread LDS Perturbation (16.0 <= zoom < 1e32, without BLA).
        constexpr const char* kPerturbLdsShaderSrc = R"(
#define LDS_ORBIT_SIZE 2048u

cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint fractalType;
    uint smoothFlag;
    uint checkCardioid;
    uint needScaleCheck;
    uint rowOffset;
    uint subpixelTotal;
    float refCx;
    float refCy;
    uint outRowOffset, pad1, pad2, pad3;
};

Buffer<float2> OrbitFast : register(t0);
Buffer<float> ColCoords : register(t1);
Buffer<float> RowCoords : register(t2);
Buffer<uint> SubpixelIndices : register(t4);
Buffer<uint> SubpixelCountSrv : register(t7);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

groupshared float2 s_orbit[LDS_ORBIT_SIZE];

float DiffAbsF(float A, float a)
{
    float sum = A + a;
    if ((A >= 0.0f && sum >= 0.0f) || (A < 0.0f && sum < 0.0f))
        return (A >= 0.0f) ? a : -a;
    return abs(sum) - abs(A);
}

bool ShouldRebaseScaled(float rx, float ry, float dx, float dy)
{
    float maxD = max(abs(dx), abs(dy));
    if (!(maxD > 0.0f) || max(abs(rx), abs(ry)) >= 3.0f * maxD)
        return false;
    float s = (maxD < 1e-15f) ? 1.0842022e19f : 1.0f;
    float szx = rx * s + dx * s;
    float szy = ry * s + dy * s;
    float sdx = dx * s;
    float sdy = dy * s;
    return (szx * szx + szy * szy) < (sdx * sdx + sdy * sdy);
}

float FinishPerturbSmooth(uint fType, uint iter, float zr, float zi, float cxApprox, float cyApprox)
{
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    uint sIter = iter;

    [loop]
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        if (fType == 0u)
        {
            zi = 2.0f * zr * zi + cyApprox;
            zr = zr2 - zi2 + cxApprox;
        }
        else if (fType == 1u)
        {
            zi = -2.0f * abs(zr * zi) + cyApprox;
            zr = zr2 - zi2 + cxApprox;
        }
        else if (fType == 2u)
        {
            zi = -2.0f * zr * zi + cyApprox;
            zr = zr2 - zi2 + cxApprox;
        }
        else
        {
            zi = -2.0f * abs(zr * zi) + cyApprox;
            zr = abs(zr2 - zi2) + cxApprox;
        }
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }

    float logZn = log(zr2 + zi2) * 0.5f;
    return max(0.0f, float(sIter) + 1.0f - log2(logZn));
}

float EvalPerturbLds(float dcx, float dcy, uint sharedLim)
{
    if (checkCardioid != 0u)
    {
        float cx = refCx + dcx;
        float cy = refCy + dcy;
        float xm = cx - 0.25f;
        float y2 = cy * cy;
        float q = xm * xm + y2;
        if (q * (q + xm) <= 0.25f * y2 - 2e-6f || ((cx + 1.0f) * (cx + 1.0f) + y2) <= 0.0625f - 2e-6f)
            return -1.0f;
    }

    float dx = 0.0f, dy = 0.0f;
    uint m = 0u;
    uint iter = 0u;
    float escZx = 0.0f, escZy = 0.0f;
    uint ldsStop = min(refLen, LDS_ORBIT_SIZE);

    if (fractalType == 0u)
    {
        [loop]
        while (iter < maxIter)
        {
            if (needScaleCheck == 0u && m > 0u && m < ldsStop)
            {
                uint endIter = iter + min(maxIter - iter, ldsStop - m);
                [loop]
                while (iter < endIter)
                {
                    float2 r = s_orbit[m];
                    float zx = r.x + dx;
                    float zy = r.y + dy;
                    float mod2 = zx * zx + zy * zy;
                    float dz2 = dx * dx + dy * dy;
                    if (mod2 > 16.0f || mod2 < dz2) break;
                    float sx = r.x + zx;
                    float sy = r.y + zy;
                    float nextDx = sx * dx - sy * dy + dcx;
                    dy = sx * dy + sy * dx + dcy;
                    dx = nextDx;
                    ++m;
                    ++iter;
                }
                if (iter >= maxIter) break;
            }

            float2 r = (m < sharedLim) ? s_orbit[m] : OrbitFast[m];
            float rx = r.x, ry = r.y;
            float zx = rx + dx;
            float zy = ry + dy;
            float mod2 = zx * zx + zy * zy;
            if (mod2 > 16.0f) { escZx = zx; escZy = zy; break; }

            bool doRebase = (m == refLen);
            if (!doRebase && m > 0u)
            {
                doRebase = (needScaleCheck != 0u)
                    ? ShouldRebaseScaled(rx, ry, dx, dy)
                    : (mod2 < (dx * dx + dy * dy));
            }
            if (doRebase)
            {
                dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
            }
            float sx = rx + zx;
            float sy = ry + zy;
            float nextDx = sx * dx - sy * dy + dcx;
            dy = sx * dy + sy * dx + dcy;
            dx = nextDx;
            ++m;
            ++iter;
        }
    }
    else
    {
        [loop]
        while (iter < maxIter)
        {
            if (needScaleCheck == 0u && m > 0u && m < ldsStop)
            {
                uint endIter = iter + min(maxIter - iter, ldsStop - m);
                if (fractalType == 1u)
                {
                    [loop]
                    while (iter < endIter)
                    {
                        float2 r = s_orbit[m];
                        float zx = r.x + dx, zy = r.y + dy;
                        float mod2 = zx * zx + zy * zy;
                        if (mod2 > 16.0f || mod2 < (dx * dx + dy * dy)) break;
                        float sx = r.x + zx, sy = r.y + zy;
                        float daX = DiffAbsF(r.x, dx), daY = DiffAbsF(r.y, dy);
                        float nextDx = sx * dx - sy * dy + dcx;
                        dy = -2.0f * (abs(r.x) * daY + abs(r.y) * daX + daX * daY) + dcy;
                        dx = nextDx;
                        ++m; ++iter;
                    }
                }
                else if (fractalType == 2u)
                {
                    [loop]
                    while (iter < endIter)
                    {
                        float2 r = s_orbit[m];
                        float zx = r.x + dx, zy = r.y + dy;
                        float mod2 = zx * zx + zy * zy;
                        if (mod2 > 16.0f || mod2 < (dx * dx + dy * dy)) break;
                        float sx = r.x + zx, sy = r.y + zy;
                        float nextDx = sx * dx - sy * dy + dcx;
                        dy = -(sx * dy + sy * dx) + dcy;
                        dx = nextDx;
                        ++m; ++iter;
                    }
                }
                else
                {
                    [loop]
                    while (iter < endIter)
                    {
                        float2 r = s_orbit[m];
                        float zx = r.x + dx, zy = r.y + dy;
                        float mod2 = zx * zx + zy * zy;
                        if (mod2 > 16.0f || mod2 < (dx * dx + dy * dy)) break;
                        float sx = r.x + zx, sy = r.y + zy;
                        float daX = DiffAbsF(r.x, dx), daY = DiffAbsF(r.y, dy);
                        float nextDx = DiffAbsF((r.x - r.y) * (r.x + r.y), sx * dx - sy * dy) + dcx;
                        dy = -2.0f * (abs(r.x) * daY + abs(r.y) * daX + daX * daY) + dcy;
                        dx = nextDx;
                        ++m; ++iter;
                    }
                }
                if (iter >= maxIter) break;
            }

            float2 r = (m < sharedLim) ? s_orbit[m] : OrbitFast[m];
            float rx = r.x, ry = r.y;
            float zx = rx + dx;
            float zy = ry + dy;
            float mod2 = zx * zx + zy * zy;
            if (mod2 > 16.0f) { escZx = zx; escZy = zy; break; }

            bool doRebase = (m == refLen);
            if (!doRebase && m > 0u)
            {
                doRebase = (needScaleCheck != 0u)
                    ? ShouldRebaseScaled(rx, ry, dx, dy)
                    : (mod2 < (dx * dx + dy * dy));
            }
            if (doRebase)
            {
                dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
            }

            float sx = rx + zx;
            float sy = ry + zy;
            if (fractalType == 1u)
            {
                float daX = DiffAbsF(rx, dx);
                float daY = DiffAbsF(ry, dy);
                float nextDx = sx * dx - sy * dy + dcx;
                dy = -2.0f * (abs(rx) * daY + abs(ry) * daX + daX * daY) + dcy;
                dx = nextDx;
            }
            else if (fractalType == 2u)
            {
                float nextDx = sx * dx - sy * dy + dcx;
                dy = -(sx * dy + sy * dx) + dcy;
                dx = nextDx;
            }
            else
            {
                float daX = DiffAbsF(rx, dx);
                float daY = DiffAbsF(ry, dy);
                float nextDx = DiffAbsF((rx - ry) * (rx + ry), sx * dx - sy * dy) + dcx;
                dy = -2.0f * (abs(rx) * daY + abs(ry) * daX + daX * daY) + dcy;
                dx = nextDx;
            }
            ++m;
            ++iter;
        }
    }

    if (iter >= maxIter) return -1.0f;
    if (smoothFlag == 0u) return float(iter);
    return FinishPerturbSmooth(fractalType, iter, escZx, escZy, refCx + dcx, refCy + dcy);
}

[numthreads(32, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[(tid.y + outRowOffset) * width + tid.x] = EvalPerturbLds(ColCoords[tid.x * 3u + 1u], RowCoords[py * 3u + 1u], sharedLim);
}

[numthreads(512, 1, 1)]
void CSSubpixels(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint activeTotal = (subpixelTotal == 0xFFFFFFFFu) ? (SubpixelCountSrv[0] * 4u) : subpixelTotal;
    uint sIdx = (gid.y * 32768u + gid.x) * 512u + gi;
    if (sIdx >= activeTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalPerturbLds(ColCoords[px * 3u + kx], RowCoords[py * 3u + ky], sharedLim);
}
)";

        // Shader 3: High-occupancy 512-thread LDS Perturbation + float32 BLA (Mandelbrot, zoom < 1e32).
        constexpr const char* kPerturbLdsBlaShaderSrc = R"(
#define LDS_ORBIT_SIZE 2048u

cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint smoothFlag;
    uint blaNumLevels;
    uint rowOffset;
    uint subpixelTotal;
    float blaMaxRadius;
    float refCx;
    float refCy;
    float pad0;
    uint checkCardioid, outRowOffset, stageIterLimit, pad4;
};

struct GpuBlaBlockF32
{
    float ar, ai;
    float br, bi;
    float radius;
    float pad0, pad1, pad2;
};

struct GpuPerturbState
{
    float x, y;
    int exp;
    uint m;
    uint iter;
};

Buffer<float2> OrbitFast : register(t0);
Buffer<float> ColCoords : register(t1);
Buffer<float> RowCoords : register(t2);
StructuredBuffer<GpuBlaBlockF32> BlaBlocks : register(t3);
StructuredBuffer<uint2> BlaMeta : register(t4);
Buffer<uint> SubpixelIndices : register(t5);
Buffer<uint> SubpixelCountSrv : register(t7);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);
RWStructuredBuffer<GpuPerturbState> ActiveState : register(u2);
RWBuffer<uint> OutActiveIndices : register(u3);
RWBuffer<uint> OutActiveCount : register(u4);

groupshared float2 s_orbit[LDS_ORBIT_SIZE];
groupshared uint s_groupCount;
groupshared uint s_groupBase;

bool StepPerturbLdsBla(
    float dcx, float dcy, uint sharedLim, uint iterLimit,
    inout float dx, inout float dy, inout uint m, inout uint iter,
    out float outResult)
{
    outResult = -1.0f;
    if (iter == 0u && checkCardioid != 0u)
    {
        float cx = refCx + dcx;
        float cy = refCy + dcy;
        float xm = cx - 0.25f;
        float y2 = cy * cy;
        float q = xm * xm + y2;
        if (q * (q + xm) <= 0.25f * y2 - 2e-6f || ((cx + 1.0f) * (cx + 1.0f) + y2) <= 0.0625f - 2e-6f)
            return true;
    }

    float escZx = 0.0f, escZy = 0.0f;
    bool escaped = false;
    uint ldsStop = min(refLen, LDS_ORBIT_SIZE);
    float blaBurstFloor = max(blaMaxRadius, 1e-15f);
    uint stopIter = min(iterLimit, maxIter);

    // Exact cycle check on m == 0 rebases (zero instructions inside tight inner loops)
    float chkDx = 1e30f, chkDy = 1e30f;
    uint rebasePeriod = 4u, rebasesSince = 0u;

    [loop]
    while (iter < stopIter)
    {
        float maxD = max(abs(dx), abs(dy));
        if (maxD >= blaBurstFloor && m > 0u && m < ldsStop)
        {
            uint endIter = iter + min(stopIter - iter, ldsStop - m);
            [loop]
            while (iter < endIter)
            {
                float2 r = s_orbit[m];
                float zx = r.x + dx;
                float zy = r.y + dy;
                float mod2 = zx * zx + zy * zy;
                float dz2 = dx * dx + dy * dy;
                if (mod2 > 16.0f || mod2 < dz2 || dz2 < 1e-30f) break;
                float sx = r.x + zx;
                float sy = r.y + zy;
                float nextDx = sx * dx - sy * dy + dcx;
                dy = sx * dy + sy * dx + dcy;
                dx = nextDx;
                ++m;
                ++iter;
            }
            if (iter >= stopIter) break;
            maxD = max(abs(dx), abs(dy));
        }

        float2 r = (m < sharedLim) ? s_orbit[m] : OrbitFast[m];
        float rx = r.x, ry = r.y;
        float zx = rx + dx;
        float zy = ry + dy;
        float mod2 = zx * zx + zy * zy;
        if (mod2 > 16.0f) { escZx = zx; escZy = zy; escaped = true; break; }

        bool doRebase = (m == refLen);
        if (!doRebase && m > 0u && max(abs(rx), abs(ry)) < 3.0f * maxD && maxD > 0.0f)
        {
            float s = (maxD < 1e-15f) ? 1.0842022e19f : 1.0f;
            float szx = rx * s + dx * s;
            float szy = ry * s + dy * s;
            float sdx = dx * s;
            float sdy = dy * s;
            if (szx * szx + szy * szy < sdx * sdx + sdy * sdy)
                doRebase = true;
        }
        if (doRebase)
        {
            dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
            maxD = max(abs(dx), abs(dy));
            if (dx == chkDx && dy == chkDy)
            {
                iter = maxIter;
                return true;
            }
            if (++rebasesSince >= rebasePeriod)
            {
                chkDx = dx; chkDy = dy;
                rebasesSince = 0u;
                if (rebasePeriod < 256u) rebasePeriod <<= 1u;
            }
        }

        if (m >= 1u && (maxIter - iter) >= 4u && ((m - 1u) & 3u) == 0u && m + 4u <= refLen && maxD < blaMaxRadius)
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
                GpuBlaBlockF32 blk = BlaBlocks[meta.x + idx];
                if (!(maxD * 1.41421356f < blk.radius)) continue;
                float nx = (blk.ar * dx - blk.ai * dy) + (blk.br * dcx - blk.bi * dcy);
                float ny = (blk.ar * dy + blk.ai * dx) + (blk.br * dcy + blk.bi * dcx);
                dx = nx; dy = ny;
                skipped = len;
                break;
            }
            if (skipped != 0u)
            {
                m += skipped;
                iter += skipped;
                continue;
            }
        }

        float sx = rx + zx;
        float sy = ry + zy;
        float nextDx = sx * dx - sy * dy + dcx;
        dy = sx * dy + sy * dx + dcy;
        dx = nextDx;
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

    float cxApprox = refCx + dcx;
    float cyApprox = refCy + dcy;
    float zr = escZx, zi = escZy;
    float zr2 = zr * zr, zi2 = zi * zi;
    uint sIter = iter;
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        zi = 2.0f * zr * zi + cyApprox;
        zr = zr2 - zi2 + cxApprox;
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }
    outResult = max(0.0f, float(sIter) + 1.0f - log2(log(zr2 + zi2) * 0.5f));
    return true;
}

float EvalPerturbLdsBla(float dcx, float dcy, uint sharedLim)
{
    float dx = 0.0f, dy = 0.0f;
    uint m = 0u, iter = 0u;
    float res = -1.0f;
    StepPerturbLdsBla(dcx, dcy, sharedLim, maxIter, dx, dy, m, iter, res);
    return res;
}

[numthreads(32, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[(tid.y + outRowOffset) * width + tid.x] = EvalPerturbLdsBla(ColCoords[tid.x * 3u + 1u], RowCoords[py * 3u + 1u], sharedLim);
}

[numthreads(32, 16, 1)]
void CSPrimaryStage0(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    bool valid = (tid.x < width && py < height);
    bool keepAlive = false;
    uint fullIdx = py * width + tid.x;

    if (valid)
    {
        float dx = 0.0f, dy = 0.0f;
        uint m = 0u, iter = 0u;
        float res = -1.0f;
        bool done = StepPerturbLdsBla(ColCoords[tid.x * 3u + 1u], RowCoords[py * 3u + 1u], sharedLim, stageIterLimit, dx, dy, m, iter, res);
        if (done)
        {
            OutIters[fullIdx] = res;
        }
        else
        {
            GpuPerturbState st;
            st.x = dx; st.y = dy; st.exp = 0; st.m = m; st.iter = iter;
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

[numthreads(512, 1, 1)]
void CSPrimaryResume(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint activeTotal = SubpixelCountSrv[0];
    uint activeIdx = (gid.y * 32768u + gid.x) * 512u + gi;
    bool valid = (activeIdx < activeTotal);
    bool keepAlive = false;
    uint pix = 0u;

    if (valid)
    {
        pix = SubpixelIndices[activeIdx];
        uint py = pix / width;
        uint px = pix - py * width;
        GpuPerturbState st = ActiveState[pix];
        float dx = st.x, dy = st.y;
        uint m = st.m, iter = st.iter;
        float res = -1.0f;
        bool done = StepPerturbLdsBla(ColCoords[px * 3u + 1u], RowCoords[py * 3u + 1u], sharedLim, stageIterLimit, dx, dy, m, iter, res);
        if (done)
        {
            OutIters[pix] = res;
        }
        else
        {
            st.x = dx; st.y = dy; st.m = m; st.iter = iter;
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

[numthreads(512, 1, 1)]
void CSSubpixels(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint activeTotal = (subpixelTotal == 0xFFFFFFFFu) ? (SubpixelCountSrv[0] * 4u) : subpixelTotal;
    uint sIdx = (gid.y * 32768u + gid.x) * 512u + gi;
    if (sIdx >= activeTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalPerturbLdsBla(ColCoords[px * 3u + kx], RowCoords[py * 3u + ky], sharedLim);
}
)";
    }

    const char* GetPerturbLdsShaderSource() noexcept
    {
        return kPerturbLdsShaderSrc;
    }

    const char* GetPerturbLdsBlaShaderSource() noexcept
    {
        return kPerturbLdsBlaShaderSrc;
    }
}
