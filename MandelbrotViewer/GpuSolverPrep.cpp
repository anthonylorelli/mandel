#include "GpuFractalSolverImpl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Mandelbrot
{
    using namespace detail;

    bool GpuFractalSolver::Impl::SyncReferenceAndBla(
        const FractalConfig& config,
        const DeepReferenceCache* cacheKey,
        const DeepReferenceState& refState,
        double cMax,
        const std::atomic<bool>* cancelToken,
        bool& outRunDeepExp)
    {
        const size_t orbitCount = static_cast<size_t>(refState.refLen) + 1;
        const bool zoomDeepExp = (config.zoom >= kGpuDeepExpThreshold);

        if (cacheKey != uploadedCachePtr ||
            refState.orbitVersion != uploadedOrbitVersion ||
            refState.refLen > uploadedRefLen ||
            config.fractalType != uploadedType)
        {
            if (!EnsureTypedSrvBuffer(
                orbitCount, 8, DXGI_FORMAT_R32G32_FLOAT, orbitFastBuf, orbitFastSrv, orbitFastCapElems))
            {
                return false;
            }
            hostOrbitFast.resize(orbitCount * 2);
            for (size_t m = 0; m < orbitCount; ++m)
            {
                hostOrbitFast[m * 2 + 0] = static_cast<float>(refState.orbit[m].x);
                hostOrbitFast[m * 2 + 1] = static_cast<float>(refState.orbit[m].y);
            }
            if (!UploadBufferDiscard(orbitFastBuf.Get(), hostOrbitFast.data(), orbitCount * 8))
            {
                return false;
            }
            uploadedCachePtr = cacheKey;
            uploadedOrbitVersion = refState.orbitVersion;
            uploadedRefLen = refState.refLen;
            uploadedType = config.fractalType;
            uploadedHasDeepExpOrbit = false;
        }

        if (config.fractalType == FractalType::Mandelbrot)
        {
            if (cacheKey != uploadedBlaCachePtr ||
                refState.orbitVersion != uploadedBlaOrbitVersion ||
                refState.refLen != uploadedBlaRefLen ||
                uploadedBlaForceDeepExp != zoomDeepExp ||
                cMax > uploadedBlaCMax * (1.0 + 1e-12) ||
                cMax < uploadedBlaCMax * 0.25)
            {
                // Invalidate cached BLA keys before Build() clears the previous table so
                // cancellation or upload failure can never leave stale metadata enabled.
                uploadedBlaCachePtr = nullptr;
                uploadedBlaOrbitVersion = 0;
                uploadedBlaRefLen = -1;
                uploadedBlaCMax = 0.0;
                uploadedBlaForceDeepExp = false;
                uploadedHasBlaF32 = false;
                uploadedHasBlaExp = false;

                // Add cMax headroom at deep zooms so zoom-out trajectories and small pans reuse
                // the same BLA table across many frames instead of rebuilding every frame.
                const double headroomCMax = (cMax < 1e-6) ? (cMax * 1.5) : cMax;
                double builtCMax = headroomCMax;
                blaBuilder.Build(refState.orbit, refState.refLen, headroomCMax, cancelToken, zoomDeepExp);
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    return false;
                }
                if (blaBuilder.numLevels < 3 && headroomCMax > cMax)
                {
                    blaBuilder.Build(refState.orbit, refState.refLen, cMax, cancelToken, zoomDeepExp);
                    builtCMax = cMax;
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        return false;
                    }
                }

                if (blaBuilder.numLevels >= 3)
                {
                    if (!UploadBufferDiscard(blaMetaBuf.Get(), blaBuilder.levelMeta.data(), 32 * 8))
                    {
                        return false;
                    }
                }

                uploadedBlaCachePtr = cacheKey;
                uploadedBlaOrbitVersion = refState.orbitVersion;
                uploadedBlaRefLen = refState.refLen;
                uploadedBlaCMax = builtCMax;
                uploadedBlaForceDeepExp = zoomDeepExp;
            }
        }
        else
        {
            blaBuilder.Clear();
            uploadedBlaCachePtr = nullptr;
            uploadedBlaOrbitVersion = 0;
            uploadedBlaRefLen = -1;
            uploadedBlaCMax = 0.0;
            uploadedBlaForceDeepExp = false;
            uploadedHasBlaF32 = false;
            uploadedHasBlaExp = false;
        }

        const bool hasBla = (config.fractalType == FractalType::Mandelbrot && blaBuilder.numLevels >= 3);
        outRunDeepExp = zoomDeepExp || (hasBla && !blaBuilder.fitsFloat32);

        if (outRunDeepExp && !uploadedHasDeepExpOrbit)
        {
            if (!EnsureStructuredSrvBuffer(
                orbitCount, sizeof(GpuOrbitPointExp), orbitFullBuf, orbitFullSrv, orbitFullCapElems))
            {
                return false;
            }
            hostOrbitExp.resize(orbitCount);
            for (size_t m = 0; m < orbitCount; ++m)
            {
                const double rx = refState.orbit[m].x;
                const double ry = refState.orbit[m].y;
                SplitFrexp2(rx, ry, hostOrbitExp[m].rmx, hostOrbitExp[m].rmy, hostOrbitExp[m].rExp);
                hostOrbitExp[m].pad0 = 0;
            }
            if (!UploadBufferDiscard(orbitFullBuf.Get(), hostOrbitExp.data(), orbitCount * sizeof(GpuOrbitPointExp)))
            {
                return false;
            }
            uploadedHasDeepExpOrbit = true;
        }

        if (hasBla)
        {
            if (outRunDeepExp)
            {
                if (!uploadedHasBlaExp)
                {
                    if (!EnsureStructuredSrvBuffer(
                        blaBuilder.blocksExp.size(), sizeof(GpuBlaBlockExp), blaExpBuf, blaExpSrv, blaExpCapElems))
                    {
                        return false;
                    }
                    if (!UploadBufferDiscard(
                        blaExpBuf.Get(),
                        blaBuilder.blocksExp.data(),
                        blaBuilder.blocksExp.size() * sizeof(GpuBlaBlockExp)))
                    {
                        return false;
                    }
                    uploadedHasBlaExp = true;
                }
            }
            else
            {
                if (!uploadedHasBlaF32)
                {
                    if (!EnsureStructuredSrvBuffer(
                        blaBuilder.blocksF32.size(), sizeof(GpuBlaBlockF32), blaF32Buf, blaF32Srv, blaF32CapElems))
                    {
                        return false;
                    }
                    if (!UploadBufferDiscard(
                        blaF32Buf.Get(),
                        blaBuilder.blocksF32.data(),
                        blaBuilder.blocksF32.size() * sizeof(GpuBlaBlockF32)))
                    {
                        return false;
                    }
                    uploadedHasBlaF32 = true;
                }
            }
        }

        return true;
    }

    GpuFractalSolver::Impl::PrepResult GpuFractalSolver::Impl::PrepareModeAndCoords(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        DeepReferenceCache& referenceCache,
        const std::atomic<bool>* cancelToken,
        GpuShaderMode& outMode,
        DeepReferenceState& outRefState,
        double& outJuliaCx,
        double& outJuliaCy,
        double& outJuliaBailout2)
    {
        const int width = config.width;
        const int height = config.height;
        outJuliaCx = 0.0;
        outJuliaCy = 0.0;
        outJuliaBailout2 = 16.0;

        if (config.zoom < GpuPerturbationThreshold)
        {
            outMode = GpuShaderMode::Shallow;
            const double effectiveZoom = config.juliaSet
                ? std::clamp(config.zoom, MinZoom, MaxZoom)
                : (config.zoom > 1e-12 ? config.zoom : 1e-12);
            const double step = (3.0 / effectiveZoom) / static_cast<double>(width);
            const double halfW = static_cast<double>(width) * 0.5;
            const double halfH = static_cast<double>(height) * 0.5;
            const double cx0 = config.juliaSet ? centerX.ToDouble() : config.centerX;
            const double cy0 = config.juliaSet ? centerY.ToDouble() : config.centerY;

            if (config.juliaSet)
            {
                outJuliaCx = config.juliaX.ToDouble();
                outJuliaCy = config.juliaY.ToDouble();
                outJuliaBailout2 = std::max(16.0, 4.0 * std::hypot(outJuliaCx, outJuliaCy));
            }

            const bool reuseCols = hasUploadedCols && colCoordsBuf &&
                uploadedColMode == outMode && uploadedColWidth == width &&
                uploadedColOrigin == cx0 && uploadedColStep == step;
            if (!reuseCols)
            {
                hostColCoords.resize(static_cast<size_t>(width) * 3);
                for (int x = 0; x < width; ++x)
                {
                    hostColCoords[static_cast<size_t>(x) * 3 + 0] = static_cast<float>(cx0 + ((x + 0.25) - halfW) * step);
                    hostColCoords[static_cast<size_t>(x) * 3 + 1] = static_cast<float>(cx0 + ((x + 0.50) - halfW) * step);
                    hostColCoords[static_cast<size_t>(x) * 3 + 2] = static_cast<float>(cx0 + ((x + 0.75) - halfW) * step);
                }
                if (!EnsureTypedSrvBuffer(
                        hostColCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                        colCoordsBuf, colCoordsSrv, colCoordsCapFloats) ||
                    !UploadBufferDiscard(
                        colCoordsBuf.Get(), hostColCoords.data(), hostColCoords.size() * sizeof(float)))
                {
                    return PrepResult::Fallback;
                }
                hasUploadedCols = true;
                uploadedColMode = outMode;
                uploadedColWidth = width;
                uploadedColOrigin = cx0;
                uploadedColStep = step;
            }

            hostRowCoords.resize(static_cast<size_t>(height) * 3);
            for (int y = 0; y < height; ++y)
            {
                hostRowCoords[static_cast<size_t>(y) * 3 + 0] = static_cast<float>(cy0 + (halfH - (y + 0.25)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 1] = static_cast<float>(cy0 + (halfH - (y + 0.50)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 2] = static_cast<float>(cy0 + (halfH - (y + 0.75)) * step);
            }

            if (!EnsureTypedSrvBuffer(
                    hostRowCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    rowCoordsBuf, rowCoordsSrv, rowCoordsCapFloats) ||
                !UploadBufferDiscard(
                    rowCoordsBuf.Get(), hostRowCoords.data(), hostRowCoords.size() * sizeof(float)))
            {
                return PrepResult::Fallback;
            }
            return PrepResult::Ok;
        }

        if (!referenceCache.Ensure(config, centerX, centerY, ExecutionMode::Gpu, outRefState, cancelToken))
        {
            return (cancelToken && cancelToken->load(std::memory_order_relaxed))
                ? PrepResult::Canceled
                : PrepResult::Fallback;
        }

        const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double step = (3.0 / clampedZoom) / static_cast<double>(width);
        const double maxCx = std::max(std::abs(-0.25 - outRefState.refPosX), std::abs(width - 0.75 - outRefState.refPosX)) * step;
        const double maxCy = std::max(std::abs(outRefState.refPosY + 0.25), std::abs(outRefState.refPosY - (height - 0.75))) * step;
        const double cMax = std::hypot(maxCx, maxCy);

        bool runDeepExp = false;
        if (!SyncReferenceAndBla(config, &referenceCache, outRefState, cMax, cancelToken, runDeepExp))
        {
            return (cancelToken && cancelToken->load(std::memory_order_relaxed))
                ? PrepResult::Canceled
                : PrepResult::Fallback;
        }

        if (runDeepExp)
        {
            outMode = GpuShaderMode::PerturbDeepExp;
            int stepExp = 0;
            const double stepMantD = std::frexp(step, &stepExp);

            const bool reuseCols = hasUploadedCols && colCoordsExpBuf &&
                uploadedColMode == outMode && uploadedColWidth == width &&
                uploadedColOrigin == outRefState.refPosX && uploadedColStep == step;
            if (!reuseCols)
            {
                hostColCoordsExp.resize(static_cast<size_t>(width) * 3);
                for (int x = 0; x < width; ++x)
                {
                    SplitFrexpSigned(((x - 0.25) - outRefState.refPosX) * stepMantD, stepExp,
                        hostColCoordsExp[static_cast<size_t>(x) * 3 + 0].mant,
                        hostColCoordsExp[static_cast<size_t>(x) * 3 + 0].exp);
                    SplitFrexpSigned((static_cast<double>(x) - outRefState.refPosX) * stepMantD, stepExp,
                        hostColCoordsExp[static_cast<size_t>(x) * 3 + 1].mant,
                        hostColCoordsExp[static_cast<size_t>(x) * 3 + 1].exp);
                    SplitFrexpSigned(((x + 0.25) - outRefState.refPosX) * stepMantD, stepExp,
                        hostColCoordsExp[static_cast<size_t>(x) * 3 + 2].mant,
                        hostColCoordsExp[static_cast<size_t>(x) * 3 + 2].exp);
                }
                if (!EnsureStructuredSrvBuffer(
                        hostColCoordsExp.size(), sizeof(GpuCoordExp),
                        colCoordsExpBuf, colCoordsExpSrv, colCoordsExpCapElems) ||
                    !UploadBufferDiscard(
                        colCoordsExpBuf.Get(), hostColCoordsExp.data(),
                        hostColCoordsExp.size() * sizeof(GpuCoordExp)))
                {
                    return PrepResult::Fallback;
                }
                hasUploadedCols = true;
                uploadedColMode = outMode;
                uploadedColWidth = width;
                uploadedColOrigin = outRefState.refPosX;
                uploadedColStep = step;
            }

            hostRowCoordsExp.resize(static_cast<size_t>(height) * 3);
            for (int y = 0; y < height; ++y)
            {
                SplitFrexpSigned((outRefState.refPosY - (y - 0.25)) * stepMantD, stepExp,
                    hostRowCoordsExp[static_cast<size_t>(y) * 3 + 0].mant,
                    hostRowCoordsExp[static_cast<size_t>(y) * 3 + 0].exp);
                SplitFrexpSigned((outRefState.refPosY - static_cast<double>(y)) * stepMantD, stepExp,
                    hostRowCoordsExp[static_cast<size_t>(y) * 3 + 1].mant,
                    hostRowCoordsExp[static_cast<size_t>(y) * 3 + 1].exp);
                SplitFrexpSigned((outRefState.refPosY - (y + 0.25)) * stepMantD, stepExp,
                    hostRowCoordsExp[static_cast<size_t>(y) * 3 + 2].mant,
                    hostRowCoordsExp[static_cast<size_t>(y) * 3 + 2].exp);
            }

            if (!EnsureStructuredSrvBuffer(
                    hostRowCoordsExp.size(), sizeof(GpuCoordExp),
                    rowCoordsExpBuf, rowCoordsExpSrv, rowCoordsExpCapElems) ||
                !UploadBufferDiscard(
                    rowCoordsExpBuf.Get(), hostRowCoordsExp.data(),
                    hostRowCoordsExp.size() * sizeof(GpuCoordExp)))
            {
                return PrepResult::Fallback;
            }
        }
        else
        {
            const bool hasBla = (config.fractalType == FractalType::Mandelbrot && blaBuilder.numLevels >= 3);
            outMode = hasBla ? GpuShaderMode::PerturbLdsBla : GpuShaderMode::PerturbLds;

            const bool reuseCols = hasUploadedCols && colCoordsBuf &&
                (uploadedColMode == GpuShaderMode::PerturbLds || uploadedColMode == GpuShaderMode::PerturbLdsBla) &&
                uploadedColWidth == width &&
                uploadedColOrigin == outRefState.refPosX && uploadedColStep == step;
            if (!reuseCols)
            {
                hostColCoords.resize(static_cast<size_t>(width) * 3);
                for (int x = 0; x < width; ++x)
                {
                    hostColCoords[static_cast<size_t>(x) * 3 + 0] = static_cast<float>(((x - 0.25) - outRefState.refPosX) * step);
                    hostColCoords[static_cast<size_t>(x) * 3 + 1] = static_cast<float>((static_cast<double>(x) - outRefState.refPosX) * step);
                    hostColCoords[static_cast<size_t>(x) * 3 + 2] = static_cast<float>(((x + 0.25) - outRefState.refPosX) * step);
                }
                if (!EnsureTypedSrvBuffer(
                        hostColCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                        colCoordsBuf, colCoordsSrv, colCoordsCapFloats) ||
                    !UploadBufferDiscard(
                        colCoordsBuf.Get(), hostColCoords.data(), hostColCoords.size() * sizeof(float)))
                {
                    return PrepResult::Fallback;
                }
                hasUploadedCols = true;
                uploadedColMode = outMode;
                uploadedColWidth = width;
                uploadedColOrigin = outRefState.refPosX;
                uploadedColStep = step;
            }

            hostRowCoords.resize(static_cast<size_t>(height) * 3);
            for (int y = 0; y < height; ++y)
            {
                hostRowCoords[static_cast<size_t>(y) * 3 + 0] = static_cast<float>((outRefState.refPosY - (y - 0.25)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 1] = static_cast<float>((outRefState.refPosY - static_cast<double>(y)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 2] = static_cast<float>((outRefState.refPosY - (y + 0.25)) * step);
            }

            if (!EnsureTypedSrvBuffer(
                    hostRowCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    rowCoordsBuf, rowCoordsSrv, rowCoordsCapFloats) ||
                !UploadBufferDiscard(
                    rowCoordsBuf.Get(), hostRowCoords.data(), hostRowCoords.size() * sizeof(float)))
            {
                return PrepResult::Fallback;
            }
        }

        return PrepResult::Ok;
    }

    bool GpuFractalSolver::Impl::UploadFractalCb(
        GpuShaderMode mode,
        const FractalConfig& config,
        const DeepReferenceState& refState,
        double juliaCx,
        double juliaCy,
        double juliaBailout2,
        uint32_t rowOffset,
        uint32_t outRowOffset,
        uint32_t subTotal,
        uint32_t stageIterLimit,
        uint32_t heightLimit)
    {
        const uint32_t effectiveHeight = (heightLimit > 0u)
            ? std::min<uint32_t>(heightLimit, static_cast<uint32_t>(config.height))
            : static_cast<uint32_t>(config.height);

        switch (mode)
        {
        case GpuShaderMode::Shallow:
        {
            ShallowCb cb{};
            cb.width = static_cast<uint32_t>(config.width);
            cb.height = effectiveHeight;
            cb.maxIter = static_cast<uint32_t>(config.maxIterations);
            cb.smoothFlag = config.smooth ? 1u : 0u;
            cb.fractalType = static_cast<uint32_t>(config.fractalType);
            cb.juliaFlag = config.juliaSet ? 1u : 0u;
            cb.rowOffset = rowOffset;
            cb.subpixelTotal = subTotal;
            cb.juliaCx = static_cast<float>(juliaCx);
            cb.juliaCy = static_cast<float>(juliaCy);
            cb.bailout2 = static_cast<float>(juliaBailout2);
            cb.outRowOffset = outRowOffset;
            return UploadBufferDiscard(constantBuffer.Get(), &cb, sizeof(cb));
        }
        case GpuShaderMode::PerturbLds:
        {
            PerturbLdsCb cb{};
            cb.width = static_cast<uint32_t>(config.width);
            cb.height = effectiveHeight;
            cb.maxIter = static_cast<uint32_t>(config.maxIterations);
            cb.refLen = static_cast<uint32_t>(refState.refLen);
            cb.fractalType = static_cast<uint32_t>(config.fractalType);
            cb.smoothFlag = config.smooth ? 1u : 0u;
            cb.checkCardioid = (config.fractalType == FractalType::Mandelbrot && config.zoom < 1e6) ? 1u : 0u;
            cb.needScaleCheck = (config.zoom >= 1e15) ? 1u : 0u;
            cb.rowOffset = rowOffset;
            cb.subpixelTotal = subTotal;
            cb.refCx = static_cast<float>(refState.refCxDouble);
            cb.refCy = static_cast<float>(refState.refCyDouble);
            cb.outRowOffset = outRowOffset;
            return UploadBufferDiscard(constantBuffer.Get(), &cb, sizeof(cb));
        }
        case GpuShaderMode::PerturbLdsBla:
        {
            PerturbLdsBlaCb cb{};
            cb.width = static_cast<uint32_t>(config.width);
            cb.height = effectiveHeight;
            cb.maxIter = static_cast<uint32_t>(config.maxIterations);
            cb.refLen = static_cast<uint32_t>(refState.refLen);
            cb.smoothFlag = config.smooth ? 1u : 0u;
            cb.blaNumLevels = blaBuilder.numLevels;
            cb.rowOffset = rowOffset;
            cb.subpixelTotal = subTotal;
            cb.blaMaxRadius = static_cast<float>(blaBuilder.maxRadD);
            cb.refCx = static_cast<float>(refState.refCxDouble);
            cb.refCy = static_cast<float>(refState.refCyDouble);
            cb.checkCardioid = (config.zoom < 1e6) ? 1u : 0u;
            cb.outRowOffset = outRowOffset;
            cb.stageIterLimit = (stageIterLimit > 0u) ? stageIterLimit : cb.maxIter;
            return UploadBufferDiscard(constantBuffer.Get(), &cb, sizeof(cb));
        }
        case GpuShaderMode::PerturbDeepExp:
        default:
        {
            const bool useBla = (config.fractalType == FractalType::Mandelbrot && blaBuilder.numLevels >= 3);
            PerturbDeepExpCb cb{};
            cb.width = static_cast<uint32_t>(config.width);
            cb.height = effectiveHeight;
            cb.maxIter = static_cast<uint32_t>(config.maxIterations);
            cb.refLen = static_cast<uint32_t>(refState.refLen);
            cb.smoothFlag = config.smooth ? 1u : 0u;
            cb.useBla = useBla ? 1u : 0u;
            cb.blaNumLevels = blaBuilder.numLevels;
            cb.rowOffset = rowOffset;
            cb.subpixelTotal = subTotal;
            cb.blaMaxRadMant = blaBuilder.maxRadMant;
            cb.blaMaxRadExp = blaBuilder.maxRadExp;
            cb.refCx = static_cast<float>(refState.refCxDouble);
            cb.refCy = static_cast<float>(refState.refCyDouble);
            cb.outRowOffset = outRowOffset;
            cb.stageIterLimit = (stageIterLimit > 0u) ? stageIterLimit : cb.maxIter;
            return UploadBufferDiscard(constantBuffer.Get(), &cb, sizeof(cb));
        }
        }
    }

    void GpuFractalSolver::Impl::BindFractalSrvs(
        GpuShaderMode mode,
        ID3D11ShaderResourceView* activeSubIdxSrv,
        ID3D11ShaderResourceView* activeSubCountSrv)
    {
        ID3D11ShaderResourceView* srvs[8]{};
        switch (mode)
        {
        case GpuShaderMode::Shallow:
            srvs[0] = colCoordsSrv.Get();
            srvs[1] = rowCoordsSrv.Get();
            srvs[4] = activeSubIdxSrv;
            break;
        case GpuShaderMode::PerturbLds:
            srvs[0] = orbitFastSrv.Get();
            srvs[1] = colCoordsSrv.Get();
            srvs[2] = rowCoordsSrv.Get();
            srvs[4] = activeSubIdxSrv;
            break;
        case GpuShaderMode::PerturbLdsBla:
            srvs[0] = orbitFastSrv.Get();
            srvs[1] = colCoordsSrv.Get();
            srvs[2] = rowCoordsSrv.Get();
            srvs[3] = blaF32Srv.Get();
            srvs[4] = blaMetaSrv.Get();
            srvs[5] = activeSubIdxSrv;
            break;
        case GpuShaderMode::PerturbDeepExp:
            srvs[0] = orbitFastSrv.Get();
            srvs[1] = orbitFullSrv.Get();
            srvs[2] = colCoordsExpSrv.Get();
            srvs[3] = rowCoordsExpSrv.Get();
            srvs[4] = blaExpSrv.Get();
            srvs[5] = blaMetaSrv.Get();
            srvs[6] = activeSubIdxSrv;
            break;
        }
        srvs[7] = activeSubCountSrv;
        context->CSSetShaderResources(0, 8, srvs);
    }

    bool GpuFractalSolver::Impl::DispatchGpuSubpixelPass(
        GpuShaderMode mode,
        const FractalConfig& config,
        const DeepReferenceState& refState,
        double juliaCx,
        double juliaCy,
        double juliaBailout2,
        const PostProcessCb& postCb)
    {
        ID3D11ComputeShader* csDetect = EnsureShader(GpuShaderId::PostDetect);
        ID3D11ComputeShader* csBuildInd = EnsureShader(GpuShaderId::PostBuildIndirect);
        if (!csDetect || !csBuildInd)
        {
            return false;
        }

        const UINT zeros[4]{ 0, 0, 0, 0 };
        context->ClearUnorderedAccessViewUint(subCounterUav.Get(), zeros);

        ID3D11UnorderedAccessView* detectUavs[3]{
            gpuSubIdxUav.Get(),
            pixelSubInfoUav.Get(),
            subCounterUav.Get()
        };
        context->CSSetUnorderedAccessViews(0, 3, detectUavs, nullptr);
        ID3D11ShaderResourceView* detectSrvs[8]{ outItersSrv.Get() };
        context->CSSetShaderResources(0, 8, detectSrvs);

        if (!UploadBufferDiscard(constantBuffer.Get(), &postCb, sizeof(postCb)))
        {
            UnbindComputeResources();
            return false;
        }
        context->CSSetShader(csDetect, nullptr, 0);
        context->Dispatch(
            static_cast<UINT>((postCb.width + 15u) / 16u),
            static_cast<UINT>((postCb.visibleRows + 15u) / 16u),
            1);

        // Build DispatchIndirect thread-group count from compacted boundary counter.
        ID3D11ShaderResourceView* nullSrvs8[8]{};
        context->CSSetShaderResources(0, 8, nullSrvs8);
        ID3D11UnorderedAccessView* buildIndUavs[3]{ indirectArgsUav.Get(), nullptr, nullptr };
        context->CSSetUnorderedAccessViews(0, 3, buildIndUavs, nullptr);
        ID3D11ShaderResourceView* buildIndSrvs[8]{};
        buildIndSrvs[5] = subCounterSrv.Get();
        context->CSSetShaderResources(0, 8, buildIndSrvs);
        context->CSSetShader(csBuildInd, nullptr, 0);
        context->Dispatch(1, 1, 1);

        // Evaluate 4x subpixels directly on GPU via DispatchIndirect (zero CPU round-trip).
        ID3D11UnorderedAccessView* subUavs[3]{ nullptr, subOutUav.Get(), nullptr };
        context->CSSetUnorderedAccessViews(0, 3, subUavs, nullptr);
        BindFractalSrvs(mode, gpuSubIdxSrv.Get(), subCounterSrv.Get());

        if (!UploadFractalCb(
            mode, config, refState, juliaCx, juliaCy, juliaBailout2,
            0u, 0u, kIndirectSubpixelSentinel) ||
            !SetSubpixelShader(mode))
        {
            UnbindComputeResources();
            return false;
        }
        context->DispatchIndirect(indirectArgsBuf.Get(), 0);
        return true;
    }

    bool GpuFractalSolver::Impl::ReadbackBuffer(
        ID3D11Buffer* srcBuf,
        ID3D11Buffer* stagingBuf,
        void* dst,
        size_t bytes) noexcept
    {
        if (!srcBuf || !stagingBuf || !dst || bytes == 0)
        {
            return false;
        }
        D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(bytes), 1, 1 };
        context->CopySubresourceRegion(stagingBuf, 0, 0, 0, 0, srcBuf, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(stagingBuf, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            CheckDeviceLost();
            return false;
        }
        std::memcpy(dst, mapped.pData, bytes);
        context->Unmap(stagingBuf, 0);
        return true;
    }

    void GpuFractalSolver::PrepareReference(
        const FractalConfig& config,
        DeepReferenceCache& referenceCache,
        const std::atomic<bool>* cancelToken)
    {
        if (!m_impl || !m_impl->CanSupportConfig(config) ||
            config.juliaSet || config.zoom < GpuPerturbationThreshold)
        {
            return;
        }

        std::lock_guard lock(m_impl->mutex);

        BigFloat hpX;
        BigFloat hpY;
        config.ResolveHpCenter(hpX, hpY);

        DeepReferenceState refState{};
        if (!referenceCache.Ensure(config, hpX, hpY, ExecutionMode::Gpu, refState, cancelToken))
        {
            return;
        }

        const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double step = (3.0 / clampedZoom) / static_cast<double>(config.width);
        const double maxCx = std::max(std::abs(-0.25 - refState.refPosX), std::abs(config.width - 0.75 - refState.refPosX)) * step;
        const double maxCy = std::max(std::abs(refState.refPosY + 0.25), std::abs(refState.refPosY - (config.height - 0.75))) * step;
        const double cMax = std::hypot(maxCx, maxCy);

        bool unusedRunDeepExp = false;
        m_impl->SyncReferenceAndBla(config, &referenceCache, refState, cMax, cancelToken, unusedRunDeepExp);
    }
}
