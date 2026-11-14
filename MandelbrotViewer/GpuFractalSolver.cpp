#include "GpuFractalSolverImpl.h"
#include "RenderPass.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <omp.h>
#include <stdexcept>
#include <vector>

namespace Mandelbrot
{
    using namespace detail;

    bool GpuFractalSolver::Impl::SyncReferenceAndBla(
        const FractalConfig& config,
        const DeepReferenceState& refState,
        double cMax,
        const std::atomic<bool>* cancelToken,
        bool& outRunDeepExp)
    {
        const size_t orbitCount = static_cast<size_t>(refState.refLen) + 1;
        const bool zoomDeepExp = (config.zoom >= kGpuDeepExpThreshold);

        if (refState.orbitVersion != uploadedOrbitVersion ||
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
            uploadedOrbitVersion = refState.orbitVersion;
            uploadedRefLen = refState.refLen;
            uploadedType = config.fractalType;
            uploadedHasDeepExpOrbit = false;
        }

        if (config.fractalType == FractalType::Mandelbrot)
        {
            if (refState.orbitVersion != uploadedBlaOrbitVersion ||
                refState.refLen != uploadedBlaRefLen ||
                cMax > uploadedBlaCMax * (1.0 + 1e-12) ||
                cMax < uploadedBlaCMax * 0.25)
            {
                blaBuilder.Build(refState.orbit, refState.refLen, cMax, cancelToken);
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    return false;
                }
                uploadedBlaOrbitVersion = refState.orbitVersion;
                uploadedBlaRefLen = refState.refLen;
                uploadedBlaCMax = cMax;
                uploadedHasBlaF32 = false;
                uploadedHasBlaExp = false;

                if (blaBuilder.numLevels >= 3)
                {
                    if (!UploadBufferDiscard(blaMetaBuf.Get(), blaBuilder.levelMeta.data(), 32 * 8))
                    {
                        return false;
                    }
                }
            }
        }
        else
        {
            blaBuilder.Clear();
            uploadedBlaOrbitVersion = 0;
            uploadedBlaRefLen = -1;
            uploadedBlaCMax = 0.0;
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
                hostOrbitExp[m].rx = static_cast<float>(rx);
                hostOrbitExp[m].ry = static_cast<float>(ry);
                SplitFrexp2(rx, ry, hostOrbitExp[m].rmx, hostOrbitExp[m].rmy, hostOrbitExp[m].rExp);
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

            hostColCoords.resize(static_cast<size_t>(width) * 3);
            for (int x = 0; x < width; ++x)
            {
                hostColCoords[static_cast<size_t>(x) * 3 + 0] = static_cast<float>(cx0 + ((x + 0.25) - halfW) * step);
                hostColCoords[static_cast<size_t>(x) * 3 + 1] = static_cast<float>(cx0 + ((x + 0.50) - halfW) * step);
                hostColCoords[static_cast<size_t>(x) * 3 + 2] = static_cast<float>(cx0 + ((x + 0.75) - halfW) * step);
            }
            hostRowCoords.resize(static_cast<size_t>(height) * 3);
            for (int y = 0; y < height; ++y)
            {
                hostRowCoords[static_cast<size_t>(y) * 3 + 0] = static_cast<float>(cy0 + (halfH - (y + 0.25)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 1] = static_cast<float>(cy0 + (halfH - (y + 0.50)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 2] = static_cast<float>(cy0 + (halfH - (y + 0.75)) * step);
            }

            if (!EnsureTypedSrvBuffer(
                    hostColCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    colCoordsBuf, colCoordsSrv, colCoordsCapFloats) ||
                !EnsureTypedSrvBuffer(
                    hostRowCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    rowCoordsBuf, rowCoordsSrv, rowCoordsCapFloats) ||
                !UploadBufferDiscard(
                    colCoordsBuf.Get(), hostColCoords.data(), hostColCoords.size() * sizeof(float)) ||
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
        if (!SyncReferenceAndBla(config, outRefState, cMax, cancelToken, runDeepExp))
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
                    hostColCoordsExp.size(), sizeof(GpuCoordExp),
                    colCoordsExpBuf, colCoordsExpSrv, colCoordsExpCapElems) ||
                !EnsureStructuredSrvBuffer(
                    hostRowCoordsExp.size(), sizeof(GpuCoordExp),
                    rowCoordsExpBuf, rowCoordsExpSrv, rowCoordsExpCapElems) ||
                !UploadBufferDiscard(
                    colCoordsExpBuf.Get(), hostColCoordsExp.data(),
                    hostColCoordsExp.size() * sizeof(GpuCoordExp)) ||
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

            hostColCoords.resize(static_cast<size_t>(width) * 3);
            for (int x = 0; x < width; ++x)
            {
                hostColCoords[static_cast<size_t>(x) * 3 + 0] = static_cast<float>(((x - 0.25) - outRefState.refPosX) * step);
                hostColCoords[static_cast<size_t>(x) * 3 + 1] = static_cast<float>((static_cast<double>(x) - outRefState.refPosX) * step);
                hostColCoords[static_cast<size_t>(x) * 3 + 2] = static_cast<float>(((x + 0.25) - outRefState.refPosX) * step);
            }
            hostRowCoords.resize(static_cast<size_t>(height) * 3);
            for (int y = 0; y < height; ++y)
            {
                hostRowCoords[static_cast<size_t>(y) * 3 + 0] = static_cast<float>((outRefState.refPosY - (y - 0.25)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 1] = static_cast<float>((outRefState.refPosY - static_cast<double>(y)) * step);
                hostRowCoords[static_cast<size_t>(y) * 3 + 2] = static_cast<float>((outRefState.refPosY - (y + 0.25)) * step);
            }

            if (!EnsureTypedSrvBuffer(
                    hostColCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    colCoordsBuf, colCoordsSrv, colCoordsCapFloats) ||
                !EnsureTypedSrvBuffer(
                    hostRowCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    rowCoordsBuf, rowCoordsSrv, rowCoordsCapFloats) ||
                !UploadBufferDiscard(
                    colCoordsBuf.Get(), hostColCoords.data(), hostColCoords.size() * sizeof(float)) ||
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
        uint32_t subTotal)
    {
        switch (mode)
        {
        case GpuShaderMode::Shallow:
        {
            ShallowCb cb{};
            cb.width = static_cast<uint32_t>(config.width);
            cb.height = static_cast<uint32_t>(config.height);
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
            cb.height = static_cast<uint32_t>(config.height);
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
            cb.height = static_cast<uint32_t>(config.height);
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
            return UploadBufferDiscard(constantBuffer.Get(), &cb, sizeof(cb));
        }
        case GpuShaderMode::PerturbDeepExp:
        default:
        {
            const bool useBla = (config.fractalType == FractalType::Mandelbrot && blaBuilder.numLevels >= 3);
            PerturbDeepExpCb cb{};
            cb.width = static_cast<uint32_t>(config.width);
            cb.height = static_cast<uint32_t>(config.height);
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

    GpuFractalSolver::GpuFractalSolver(bool allowSoftwareAdapter)
        : m_impl(std::make_unique<Impl>(allowSoftwareAdapter))
    {
    }

    GpuFractalSolver::~GpuFractalSolver() = default;

    bool GpuFractalSolver::IsAvailable() const noexcept
    {
        return m_impl && m_impl->available;
    }

    bool GpuFractalSolver::IsUsingSoftwareAdapter() const noexcept
    {
        return m_impl && m_impl->usingSoftwareAdapter;
    }

    bool GpuFractalSolver::CanSupportConfig(const FractalConfig& config) const noexcept
    {
        return m_impl && m_impl->CanSupportConfig(config);
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

        BigFloat hpX = config.hpCenterX;
        BigFloat hpY = config.hpCenterY;
        if (std::abs(hpX.ToDouble() - config.centerX) > 1e-14 ||
            std::abs(hpY.ToDouble() - config.centerY) > 1e-14)
        {
            hpX = BigFloat(config.centerX);
            hpY = BigFloat(config.centerY);
        }

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
        m_impl->SyncReferenceAndBla(config, refState, cMax, cancelToken, unusedRunDeepExp);
    }

    bool GpuFractalSolver::TryCompute(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        DeepReferenceCache& referenceCache,
        std::vector<float>& sampleWeights,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken,
        std::vector<SubpixelSamples>* subpixelSamples,
        double& outElapsedMs)
    {
        outElapsedMs = 0.0;
        if (!m_impl || !m_impl->CanSupportConfig(config))
        {
            return false;
        }

        std::lock_guard lock(m_impl->mutex);
        const auto startTime = std::chrono::high_resolution_clock::now();
        auto canceled = [&] { return cancelToken && cancelToken->load(std::memory_order_relaxed); };

        if (subpixelSamples) subpixelSamples->clear();
        const int width = config.width;
        const int height = config.height;
        const int maxIter = config.maxIterations;
        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);

        if (subpixelSamples && totalPixels > std::numeric_limits<uint32_t>::max())
        {
            throw std::length_error("Supersampling image is too large.");
        }

        if (outIterations.size() != totalPixels)
        {
            outIterations.resize(totalPixels);
        }

        if (maxIter == 0)
        {
            std::fill(outIterations.begin(), outIterations.end(), -1.0f);
            outElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - startTime).count();
            return true;
        }

        if (canceled())
        {
            return true;
        }

        GpuShaderMode mode = GpuShaderMode::Shallow;
        DeepReferenceState refState{};
        double juliaCx = 0.0, juliaCy = 0.0, juliaBailout2 = 16.0;

        const auto prep = m_impl->PrepareModeAndCoords(
            config, centerX, centerY, referenceCache, cancelToken,
            mode, refState, juliaCx, juliaCy, juliaBailout2);
        if (prep == Impl::PrepResult::Canceled) return true;
        if (prep != Impl::PrepResult::Ok) return false;

        const size_t maxRowsByMem = std::max<size_t>(1, kMaxPrimaryChunkPixels / static_cast<size_t>(width));
        const size_t maxRowsByTdr = std::max<size_t>(
            1,
            static_cast<size_t>(kMaxPrimaryIterBudget /
                (static_cast<uint64_t>(width) * static_cast<uint64_t>(std::max(1, maxIter)))));
        const int maxChunkRows = static_cast<int>(std::min<size_t>({ static_cast<size_t>(height), maxRowsByMem, maxRowsByTdr }));
        const size_t primaryChunkCap = static_cast<size_t>(width) * static_cast<size_t>(maxChunkRows);

        if (!m_impl->EnsureStructuredUavSrvBuffer(
                primaryChunkCap, sizeof(float),
                m_impl->outItersBuf, m_impl->outItersUav, m_impl->outItersSrv, m_impl->outItersCapElems) ||
            !m_impl->EnsureStagingReadBuffer(
                primaryChunkCap * sizeof(float), m_impl->outItersStaging, m_impl->outItersStagingCapBytes))
        {
            return false;
        }

        ID3D11Buffer* cbs[]{ m_impl->constantBuffer.Get() };
        m_impl->context->CSSetConstantBuffers(0, 1, cbs);
        m_impl->UnbindComputeResources();

        // Pass 1: Primary center pixel evaluation (chunked by rows for TDR safety and bounded VRAM)
        m_impl->BindFractalSrvs(mode, nullptr, nullptr);
        ID3D11UnorderedAccessView* primUavs[3]{ m_impl->outItersUav.Get(), nullptr, nullptr };
        m_impl->context->CSSetUnorderedAccessViews(0, 3, primUavs, nullptr);
        m_impl->SetPrimaryShader(mode);

        const UINT groupX = (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla)
            ? static_cast<UINT>((width + 31) / 32)
            : static_cast<UINT>((width + 15) / 16);

        for (int rowStart = 0; rowStart < height; rowStart += maxChunkRows)
        {
            if (canceled())
            {
                m_impl->UnbindComputeResources();
                return true;
            }
            const int chunkRows = std::min(maxChunkRows, height - rowStart);
            const size_t chunkFloats = static_cast<size_t>(width) * static_cast<size_t>(chunkRows);

            if (!m_impl->UploadFractalCb(
                mode, config, refState, juliaCx, juliaCy, juliaBailout2,
                static_cast<uint32_t>(rowStart), 0u, 0u))
            {
                m_impl->UnbindComputeResources();
                return false;
            }

            const UINT groupY = static_cast<UINT>((chunkRows + 15) / 16);
            m_impl->context->Dispatch(groupX, groupY, 1);

            D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(chunkFloats * sizeof(float)), 1, 1 };
            m_impl->context->CopySubresourceRegion(
                m_impl->outItersStaging.Get(), 0, 0, 0, 0, m_impl->outItersBuf.Get(), 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_impl->context->Map(m_impl->outItersStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                m_impl->UnbindComputeResources();
                return false;
            }
            std::memcpy(
                outIterations.data() + static_cast<size_t>(rowStart) * static_cast<size_t>(width),
                mapped.pData,
                chunkFloats * sizeof(float));
            m_impl->context->Unmap(m_impl->outItersStaging.Get(), 0);
        }

        // Pass 2: Optional 4x adaptive boundary subpixel refinement
        if (subpixelSamples && !canceled())
        {
            const bool smooth = config.smooth;
            const FractalType type = config.fractalType;
            sampleWeights.resize(totalPixels);
            std::vector<size_t> rowOffsets(static_cast<size_t>(height) + 1, 0);

            #pragma omp parallel for schedule(static)
            for (int y = 0; y < height; ++y)
            {
                if (canceled()) continue;
                size_t selected = 0;
                const int y0 = std::max(0, y - 1), y1 = std::min(height - 1, y + 1);
                for (int x = 0; x < width; ++x)
                {
                    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                    const float center = outIterations[index];
                    const int x0 = std::max(0, x - 1), x1 = std::min(width - 1, x + 1);
                    if (center < 0.0f)
                    {
                        bool boundary = false;
                        for (int ny = y0; ny <= y1 && !boundary; ++ny)
                        {
                            const float* nRow = outIterations.data() + static_cast<size_t>(ny) * static_cast<size_t>(width);
                            for (int nx = x0; nx <= x1; ++nx)
                            {
                                if (nRow[nx] >= 0.0f) { boundary = true; break; }
                            }
                        }
                        sampleWeights[index] = boundary ? 1.0f : 0.0f;
                        if (boundary) ++selected;
                        continue;
                    }
                    float variation = 0.0f;
                    bool boundary = false;
                    const float a = (type == FractalType::Newton) ? std::fmod(center, 1000000.0f) : center;
                    for (int ny = y0; ny <= y1; ++ny)
                    {
                        const float* nRow = outIterations.data() + static_cast<size_t>(ny) * static_cast<size_t>(width);
                        for (int nx = x0; nx <= x1; ++nx)
                        {
                            if (nx == x && ny == y) continue;
                            const float neighbor = nRow[nx];
                            if (neighbor < 0.0f) { boundary = true; continue; }
                            if (!SameBasin(center, neighbor, type) || (!smooth && center != neighbor))
                            {
                                boundary = true;
                            }
                            const float b = (type == FractalType::Newton) ? std::fmod(neighbor, 1000000.0f) : neighbor;
                            variation = std::max(variation, std::abs(a - b) / (10.0f + std::min(a, b)));
                        }
                    }
                    const float t = std::clamp((variation - 0.015f) / 0.065f, 0.0f, 1.0f);
                    const float weight = boundary ? 1.0f : t * t * (3.0f - 2.0f * t);
                    sampleWeights[index] = weight;
                    if (weight > 0.0f) ++selected;
                }
                rowOffsets[static_cast<size_t>(y) + 1] = selected;
            }

            if (canceled())
            {
                subpixelSamples->clear();
                m_impl->UnbindComputeResources();
                return true;
            }

            for (int y = 0; y < height; ++y)
            {
                rowOffsets[static_cast<size_t>(y) + 1] += rowOffsets[static_cast<size_t>(y)];
            }
            const size_t subCount = rowOffsets.back();

            if (subCount > 0)
            {
                subpixelSamples->resize(subCount);
                m_impl->hostSlotOfPixel.resize(totalPixels);

                #pragma omp parallel for schedule(static)
                for (int y = 0; y < height; ++y)
                {
                    if (rowOffsets[static_cast<size_t>(y)] == rowOffsets[static_cast<size_t>(y) + 1]) continue;
                    size_t output = rowOffsets[static_cast<size_t>(y)];
                    for (int x = 0; x < width; ++x)
                    {
                        const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                        if (sampleWeights[index] <= 0.0f) continue;
                        m_impl->hostSlotOfPixel[index] = static_cast<uint32_t>(output);
                        auto& sample = (*subpixelSamples)[output++];
                        sample.pixelIndex = static_cast<uint32_t>(index);
                        sample.weight = sampleWeights[index];
                    }
                }

                // Group active pixels into 16x16 spatial tiles for wave coherence while preserving sorted output slots
                const int tilesX = (width + 15) / 16;
                const int tilesY = (height + 15) / 16;
                const int numTiles = tilesX * tilesY;
                std::vector<size_t> tileOffsets(static_cast<size_t>(numTiles) + 1, 0);

                #pragma omp parallel for schedule(static)
                for (int ty = 0; ty < tilesY; ++ty)
                {
                    const int y0 = ty * 16, y1 = std::min(height, y0 + 16);
                    for (int tx = 0; tx < tilesX; ++tx)
                    {
                        const int x0 = tx * 16, x1 = std::min(width, x0 + 16);
                        size_t cnt = 0;
                        for (int y = y0; y < y1; ++y)
                        {
                            const float* wRow = sampleWeights.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
                            for (int x = x0; x < x1; ++x)
                            {
                                if (wRow[x] > 0.0f) ++cnt;
                            }
                        }
                        tileOffsets[static_cast<size_t>(ty * tilesX + tx) + 1] = cnt;
                    }
                }
                for (int t = 0; t < numTiles; ++t)
                {
                    tileOffsets[static_cast<size_t>(t) + 1] += tileOffsets[static_cast<size_t>(t)];
                }

                m_impl->hostTiledPixels.resize(subCount);
                m_impl->hostTiledSlots.resize(subCount);

                #pragma omp parallel for schedule(static)
                for (int ty = 0; ty < tilesY; ++ty)
                {
                    const int y0 = ty * 16, y1 = std::min(height, y0 + 16);
                    for (int tx = 0; tx < tilesX; ++tx)
                    {
                        const size_t t = static_cast<size_t>(ty * tilesX + tx);
                        size_t wp = tileOffsets[t];
                        if (wp == tileOffsets[t + 1]) continue;
                        const int x0 = tx * 16, x1 = std::min(width, x0 + 16);
                        for (int y = y0; y < y1; ++y)
                        {
                            for (int x = x0; x < x1; ++x)
                            {
                                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                                if (sampleWeights[idx] <= 0.0f) continue;
                                m_impl->hostTiledPixels[wp] = static_cast<uint32_t>(idx);
                                m_impl->hostTiledSlots[wp] = m_impl->hostSlotOfPixel[idx];
                                ++wp;
                            }
                        }
                    }
                }

                const size_t maxBatchPix = std::min(subCount, kMaxSubpixelBatchPixels);
                if (!m_impl->EnsureTypedSrvBuffer(
                        maxBatchPix, 4, DXGI_FORMAT_R32_UINT,
                        m_impl->subIdxBuf, m_impl->subIdxSrv, m_impl->subIdxCapElems) ||
                    !m_impl->EnsureStructuredUavSrvBuffer(
                        maxBatchPix * 4, sizeof(float),
                        m_impl->subOutBuf, m_impl->subOutUav, m_impl->subOutSrv, m_impl->subOutCapFloats) ||
                    !m_impl->EnsureStagingReadBuffer(
                        maxBatchPix * 4 * sizeof(float), m_impl->subOutStaging, m_impl->subOutStagingCapBytes))
                {
                    subpixelSamples->clear();
                    m_impl->UnbindComputeResources();
                    return false;
                }

                ID3D11UnorderedAccessView* subUavs[3]{ nullptr, m_impl->subOutUav.Get(), nullptr };
                m_impl->context->CSSetUnorderedAccessViews(0, 3, subUavs, nullptr);
                m_impl->BindFractalSrvs(mode, m_impl->subIdxSrv.Get(), nullptr);
                m_impl->SetSubpixelShader(mode);

                const uint32_t subGroupThreads =
                    (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla) ? 512u : 256u;

                for (size_t batchStart = 0; batchStart < subCount; batchStart += kMaxSubpixelBatchPixels)
                {
                    if (canceled())
                    {
                        subpixelSamples->clear();
                        m_impl->UnbindComputeResources();
                        return true;
                    }
                    const size_t batchPix = std::min(kMaxSubpixelBatchPixels, subCount - batchStart);
                    const uint32_t subTotal = static_cast<uint32_t>(batchPix * 4);

                    if (!m_impl->UploadBufferDiscard(
                            m_impl->subIdxBuf.Get(),
                            m_impl->hostTiledPixels.data() + batchStart,
                            batchPix * sizeof(uint32_t)) ||
                        !m_impl->UploadFractalCb(
                            mode, config, refState, juliaCx, juliaCy, juliaBailout2,
                            0u, 0u, subTotal))
                    {
                        subpixelSamples->clear();
                        m_impl->UnbindComputeResources();
                        return false;
                    }

                    const UINT groups = static_cast<UINT>((subTotal + subGroupThreads - 1u) / subGroupThreads);
                    m_impl->context->Dispatch(groups, 1, 1);

                    D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(subTotal * sizeof(float)), 1, 1 };
                    m_impl->context->CopySubresourceRegion(
                        m_impl->subOutStaging.Get(), 0, 0, 0, 0, m_impl->subOutBuf.Get(), 0, &box);

                    m_impl->hostSubFloats.resize(subTotal);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (FAILED(m_impl->context->Map(m_impl->subOutStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        subpixelSamples->clear();
                        m_impl->UnbindComputeResources();
                        return false;
                    }
                    std::memcpy(m_impl->hostSubFloats.data(), mapped.pData, subTotal * sizeof(float));
                    m_impl->context->Unmap(m_impl->subOutStaging.Get(), 0);

                    #pragma omp parallel for schedule(static)
                    for (intptr_t i = 0; i < static_cast<intptr_t>(batchPix); ++i)
                    {
                        const uint32_t slot = m_impl->hostTiledSlots[batchStart + static_cast<size_t>(i)];
                        auto& dst = (*subpixelSamples)[slot];
                        dst.iterations[0] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 0];
                        dst.iterations[1] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 1];
                        dst.iterations[2] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 2];
                        dst.iterations[3] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 3];
                    }
                }

                if (canceled())
                {
                    subpixelSamples->clear();
                    m_impl->UnbindComputeResources();
                    return true;
                }
            }
        }

        m_impl->UnbindComputeResources();
        outElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - startTime).count();
        return true;
    }
}
