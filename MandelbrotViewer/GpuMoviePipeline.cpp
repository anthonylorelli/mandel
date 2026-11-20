#include "GpuFractalSolverImpl.h"

#include <algorithm>
#include <cstring>

namespace Mandelbrot
{
    using namespace detail;

    bool GpuFractalSolver::BeginMovieFrame(int width, int height, bool enableShutterBlur)
    {
        if (!m_impl || !m_impl->available || width <= 0 || height <= 0)
        {
            return false;
        }

        std::lock_guard lock(m_impl->mutex);
        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (totalPixels <= kMaxFullFrameAccumPixels)
        {
            if (!m_impl->EnsureStructuredUavSrvBuffer(
                    totalPixels, sizeof(uint32_t),
                    m_impl->accumPixelsBuf, m_impl->accumPixelsUav, m_impl->accumPixelsSrv, m_impl->accumPixelsCapElems) ||
                (enableShutterBlur && !m_impl->EnsureStructuredUavBuffer(
                    totalPixels, sizeof(uint32_t),
                    m_impl->shutterPixelsBuf, m_impl->shutterPixelsUav, m_impl->shutterPixelsCapElems)) ||
                !m_impl->EnsureStagingReadBuffer(
                    totalPixels * sizeof(uint32_t),
                    m_impl->accumPixelsStaging, m_impl->accumPixelsStagingCapBytes))
            {
                return false;
            }
            m_impl->movieFullFrameAccum = true;
            m_impl->movieShutterBlur = enableShutterBlur;
        }
        else
        {
            m_impl->movieFullFrameAccum = false;
            m_impl->movieShutterBlur = false;
        }
        return true;
    }

    bool GpuFractalSolver::RenderMovieBand(
        const FractalConfig& bandConfig,
        const BigFloat& centerX,
        const BigFloat& centerY,
        DeepReferenceCache& referenceCache,
        int topHalo,
        int visibleRows,
        int destRowOffset,
        int temporalStep,
        const GpuMovieColorParams& colorParams,
        uint32_t* ioBandPixels,
        const std::atomic<bool>* cancelToken)
    {
        if (!m_impl || !m_impl->CanSupportConfig(bandConfig))
        {
            return false;
        }
        if (bandConfig.maxIterations <= 0 || visibleRows <= 0 || topHalo < 0 || destRowOffset < 0)
        {
            return false;
        }

        const int width = bandConfig.width;
        const int bandHeight = bandConfig.height;
        if (topHalo + visibleRows > bandHeight || !colorParams.lutData)
        {
            return false;
        }

        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->movieFullFrameAccum && !ioBandPixels)
        {
            return false;
        }

        auto canceled = [&] { return cancelToken && cancelToken->load(std::memory_order_relaxed); };
        if (canceled())
        {
            return false;
        }

        if (!m_impl->EnsurePaletteLutUploaded(colorParams.lutData))
        {
            return false;
        }

        GpuShaderMode mode = GpuShaderMode::Shallow;
        DeepReferenceState refState{};
        double juliaCx = 0.0, juliaCy = 0.0, juliaBailout2 = 16.0;

        if (m_impl->PrepareModeAndCoords(
                bandConfig, centerX, centerY, referenceCache, cancelToken,
                mode, refState, juliaCx, juliaCy, juliaBailout2) != Impl::PrepResult::Ok)
        {
            return false;
        }

        const size_t bandPixels = static_cast<size_t>(width) * static_cast<size_t>(bandHeight);
        const size_t visiblePixels = static_cast<size_t>(width) * static_cast<size_t>(visibleRows);

        if (!m_impl->EnsureStructuredUavSrvBuffer(
                bandPixels, sizeof(float),
                m_impl->outItersBuf, m_impl->outItersUav, m_impl->outItersSrv, m_impl->outItersCapElems) ||
            !m_impl->EnsureTypedUavSrvBuffer(
                visiblePixels, sizeof(uint32_t), DXGI_FORMAT_R32_UINT,
                m_impl->gpuSubIdxBuf, m_impl->gpuSubIdxUav, m_impl->gpuSubIdxSrv, m_impl->gpuSubIdxCapElems) ||
            !m_impl->EnsureStructuredUavSrvBuffer(
                visiblePixels, sizeof(GpuPixelSubInfo),
                m_impl->pixelSubInfoBuf, m_impl->pixelSubInfoUav, m_impl->pixelSubInfoSrv, m_impl->pixelSubInfoCapElems) ||
            !m_impl->EnsureStructuredUavSrvBuffer(
                visiblePixels * 4, sizeof(float),
                m_impl->subOutBuf, m_impl->subOutUav, m_impl->subOutSrv, m_impl->subOutCapFloats))
        {
            return false;
        }

        if (!m_impl->movieFullFrameAccum)
        {
            if (!m_impl->EnsureStructuredUavSrvBuffer(
                    visiblePixels, sizeof(uint32_t),
                    m_impl->accumPixelsBuf, m_impl->accumPixelsUav, m_impl->accumPixelsSrv, m_impl->accumPixelsCapElems) ||
                !m_impl->EnsureStagingReadBuffer(
                    visiblePixels * sizeof(uint32_t),
                    m_impl->accumPixelsStaging, m_impl->accumPixelsStagingCapBytes))
            {
                return false;
            }
            if (temporalStep > 0)
            {
                if (!m_impl->EnsureStructuredSrvBuffer(
                        visiblePixels, sizeof(uint32_t),
                        m_impl->prevBandBuf, m_impl->prevBandSrv, m_impl->prevBandCapElems) ||
                    !m_impl->UploadBufferDiscard(
                        m_impl->prevBandBuf.Get(), ioBandPixels, visiblePixels * sizeof(uint32_t)))
                {
                    return false;
                }
            }
        }

        ID3D11Buffer* cbs[]{ m_impl->constantBuffer.Get() };
        m_impl->context->CSSetConstantBuffers(0, 1, cbs);
        m_impl->UnbindComputeResources();

        // Step A: Evaluate primary center pixels across the band (including halo rows) into OutIters in VRAM
        const UINT zeros[4]{ 0, 0, 0, 0 };
        const UINT primGroupX = (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla)
            ? static_cast<UINT>((width + 31) / 32)
            : static_cast<UINT>((width + 15) / 16);

        const uint32_t maxIterU = static_cast<uint32_t>(bandConfig.maxIterations);
        const bool useMultiPass =
            (mode == GpuShaderMode::PerturbLdsBla || mode == GpuShaderMode::PerturbDeepExp) &&
            maxIterU > kMultiPassMinMaxIter &&
            m_impl->EnsureStructuredUavBuffer(
                bandPixels, sizeof(GpuPerturbState),
                m_impl->activeStateBuf, m_impl->activeStateUav, m_impl->activeStateCapElems) &&
            m_impl->EnsureTypedUavSrvBuffer(
                bandPixels, sizeof(uint32_t), DXGI_FORMAT_R32_UINT,
                m_impl->activeIdxBuf[0], m_impl->activeIdxUav[0], m_impl->activeIdxSrv[0], m_impl->activeIdxCapElems[0]) &&
            m_impl->EnsureTypedUavSrvBuffer(
                bandPixels, sizeof(uint32_t), DXGI_FORMAT_R32_UINT,
                m_impl->activeIdxBuf[1], m_impl->activeIdxUav[1], m_impl->activeIdxSrv[1], m_impl->activeIdxCapElems[1]);

        if (useMultiPass)
        {
            const uint32_t stage0Limit = std::min<uint32_t>(1024u, maxIterU);
            m_impl->context->ClearUnorderedAccessViewUint(m_impl->activeCountUav[0].Get(), zeros);

            m_impl->BindFractalSrvs(mode, nullptr, nullptr);
            ID3D11UnorderedAccessView* stage0Uavs[5]{
                m_impl->outItersUav.Get(),
                nullptr,
                m_impl->activeStateUav.Get(),
                m_impl->activeIdxUav[0].Get(),
                m_impl->activeCountUav[0].Get()
            };
            m_impl->context->CSSetUnorderedAccessViews(0, 5, stage0Uavs, nullptr);
            m_impl->context->CSSetShader(
                (mode == GpuShaderMode::PerturbLdsBla) ? m_impl->csLdsBlaStage0.Get() : m_impl->csDeepExpStage0.Get(),
                nullptr, 0);

            if (!m_impl->UploadFractalCb(
                mode, bandConfig, refState, juliaCx, juliaCy, juliaBailout2,
                0u, 0u, 0u, stage0Limit))
            {
                m_impl->UnbindComputeResources();
                return false;
            }
            m_impl->context->Dispatch(primGroupX, static_cast<UINT>((bandHeight + 15) / 16), 1);

            const uint32_t resumeGroupThreads = (mode == GpuShaderMode::PerturbLdsBla) ? 512u : 256u;
            PostProcessCb activeIndCb{};
            activeIndCb.subGroupThreads = resumeGroupThreads;

            int activeIn = 0;
            uint32_t prevLimit = stage0Limit;
            uint32_t stageStep = 3072u;
            while (prevLimit < maxIterU)
            {
                if (canceled())
                {
                    m_impl->UnbindComputeResources();
                    return false;
                }
                const uint32_t nextLimit = std::min<uint32_t>(maxIterU, prevLimit + stageStep);
                const bool isFinalStage = (nextLimit >= maxIterU);
                const int activeOut = 1 - activeIn;

                // Build DispatchIndirect args for surviving active pixels
                m_impl->UnbindComputeResources();
                if (!m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &activeIndCb, sizeof(activeIndCb)))
                {
                    return false;
                }
                ID3D11UnorderedAccessView* indUavs[5]{ m_impl->indirectArgsUav.Get(), nullptr, nullptr, nullptr, nullptr };
                m_impl->context->CSSetUnorderedAccessViews(0, 5, indUavs, nullptr);
                ID3D11ShaderResourceView* indSrvs[8]{};
                indSrvs[5] = m_impl->activeCountSrv[activeIn].Get();
                m_impl->context->CSSetShaderResources(0, 8, indSrvs);
                m_impl->context->CSSetShader(m_impl->csPostBuildActiveIndirect.Get(), nullptr, 0);
                m_impl->context->Dispatch(1, 1, 1);

                // Resume only surviving active pixels in densely packed warps
                m_impl->UnbindComputeResources();
                if (!isFinalStage)
                {
                    m_impl->context->ClearUnorderedAccessViewUint(m_impl->activeCountUav[activeOut].Get(), zeros);
                }

                m_impl->BindFractalSrvs(
                    mode,
                    m_impl->activeIdxSrv[activeIn].Get(),
                    m_impl->activeCountSrv[activeIn].Get());
                ID3D11UnorderedAccessView* resumeUavs[5]{
                    m_impl->outItersUav.Get(),
                    nullptr,
                    m_impl->activeStateUav.Get(),
                    isFinalStage ? nullptr : m_impl->activeIdxUav[activeOut].Get(),
                    isFinalStage ? nullptr : m_impl->activeCountUav[activeOut].Get()
                };
                m_impl->context->CSSetUnorderedAccessViews(0, 5, resumeUavs, nullptr);
                m_impl->context->CSSetShader(
                    (mode == GpuShaderMode::PerturbLdsBla) ? m_impl->csLdsBlaResume.Get() : m_impl->csDeepExpResume.Get(),
                    nullptr, 0);

                if (!m_impl->UploadFractalCb(
                    mode, bandConfig, refState, juliaCx, juliaCy, juliaBailout2,
                    0u, 0u, 0u, nextLimit))
                {
                    m_impl->UnbindComputeResources();
                    return false;
                }
                m_impl->context->DispatchIndirect(m_impl->indirectArgsBuf.Get(), 0);

                activeIn = activeOut;
                prevLimit = nextLimit;
                stageStep = (stageStep < 8192u) ? 8192u : 12288u;
            }
            m_impl->UnbindComputeResources();
        }
        else
        {
            m_impl->BindFractalSrvs(mode, nullptr, nullptr);
            ID3D11UnorderedAccessView* primUavs[3]{ m_impl->outItersUav.Get(), nullptr, nullptr };
            m_impl->context->CSSetUnorderedAccessViews(0, 3, primUavs, nullptr);
            m_impl->SetPrimaryShader(mode);

            const size_t maxRowsByTdr = std::max<size_t>(
                1,
                static_cast<size_t>(kMaxPrimaryIterBudget /
                    (static_cast<uint64_t>(width) * static_cast<uint64_t>(std::max(1, bandConfig.maxIterations)))));
            const int maxChunkRows = static_cast<int>(std::min<size_t>(static_cast<size_t>(bandHeight), maxRowsByTdr));

            for (int rowStart = 0; rowStart < bandHeight; rowStart += maxChunkRows)
            {
                if (canceled())
                {
                    m_impl->UnbindComputeResources();
                    return false;
                }
                const int chunkRows = std::min(maxChunkRows, bandHeight - rowStart);
                if (!m_impl->UploadFractalCb(
                    mode, bandConfig, refState, juliaCx, juliaCy, juliaBailout2,
                    static_cast<uint32_t>(rowStart), static_cast<uint32_t>(rowStart), 0u))
                {
                    m_impl->UnbindComputeResources();
                    return false;
                }
                m_impl->context->Dispatch(primGroupX, static_cast<UINT>((chunkRows + 15) / 16), 1);
            }
        }

        // Step B: GPU 3x3 boundary detection + 16x16 LDS spatial compaction
        m_impl->context->ClearUnorderedAccessViewUint(m_impl->subCounterUav.Get(), zeros);

        ID3D11UnorderedAccessView* detectUavs[3]{
            m_impl->gpuSubIdxUav.Get(),
            m_impl->pixelSubInfoUav.Get(),
            m_impl->subCounterUav.Get()
        };
        m_impl->context->CSSetUnorderedAccessViews(0, 3, detectUavs, nullptr);

        ID3D11ShaderResourceView* detectSrvs[8]{ m_impl->outItersSrv.Get() };
        m_impl->context->CSSetShaderResources(0, 8, detectSrvs);

        PostProcessCb postCb{};
        postCb.width = static_cast<uint32_t>(width);
        postCb.bandHeight = static_cast<uint32_t>(bandHeight);
        postCb.visibleRows = static_cast<uint32_t>(visibleRows);
        postCb.topHalo = static_cast<uint32_t>(topHalo);
        postCb.destRowOffset = m_impl->movieFullFrameAccum ? static_cast<uint32_t>(destRowOffset) : 0u;
        postCb.fractalType = static_cast<uint32_t>(bandConfig.fractalType);
        postCb.smoothFlag = bandConfig.smooth ? 1u : 0u;
        postCb.invertFlag = colorParams.invert ? 1u : 0u;
        postCb.frequency = colorParams.frequency;
        postCb.phaseOffset = colorParams.phaseOffset;
        postCb.interiorColor = colorParams.interiorColor;
        postCb.avgRgb = colorParams.avgRgb;
        postCb.temporalMode = (temporalStep == 0) ? 0u : (m_impl->movieFullFrameAccum ? 1u : 2u);
        postCb.subGroupThreads =
            (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla) ? 512u : 256u;

        if (!m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &postCb, sizeof(postCb)))
        {
            m_impl->UnbindComputeResources();
            return false;
        }

        const UINT visGroupX = static_cast<UINT>((width + 15) / 16);
        const UINT visGroupY = static_cast<UINT>((visibleRows + 15) / 16);
        m_impl->context->CSSetShader(m_impl->csPostDetect.Get(), nullptr, 0);
        m_impl->context->Dispatch(visGroupX, visGroupY, 1);

        // Step C: Build DispatchIndirect thread-group count from compacted boundary counter
        ID3D11ShaderResourceView* nullSrvs8[8]{};
        m_impl->context->CSSetShaderResources(0, 8, nullSrvs8);

        ID3D11UnorderedAccessView* buildIndUavs[3]{ m_impl->indirectArgsUav.Get(), nullptr, nullptr };
        m_impl->context->CSSetUnorderedAccessViews(0, 3, buildIndUavs, nullptr);

        ID3D11ShaderResourceView* buildIndSrvs[8]{};
        buildIndSrvs[5] = m_impl->subCounterSrv.Get();
        m_impl->context->CSSetShaderResources(0, 8, buildIndSrvs);
        m_impl->context->CSSetShader(m_impl->csPostBuildIndirect.Get(), nullptr, 0);
        m_impl->context->Dispatch(1, 1, 1);

        // Step D: GPU-driven 4x subpixel evaluation via DispatchIndirect (zero CPU round-trip)
        ID3D11UnorderedAccessView* subUavs[3]{ nullptr, m_impl->subOutUav.Get(), nullptr };
        m_impl->context->CSSetUnorderedAccessViews(0, 3, subUavs, nullptr);
        m_impl->BindFractalSrvs(mode, m_impl->gpuSubIdxSrv.Get(), m_impl->subCounterSrv.Get());

        if (!m_impl->UploadFractalCb(
            mode, bandConfig, refState, juliaCx, juliaCy, juliaBailout2,
            0u, 0u, kIndirectSubpixelSentinel))
        {
            m_impl->UnbindComputeResources();
            return false;
        }
        m_impl->SetSubpixelShader(mode);
        m_impl->context->DispatchIndirect(m_impl->indirectArgsBuf.Get(), 0);

        // Step E: Fused 4x supersampled palette coloring + linear-light temporal shutter blend
        m_impl->context->CSSetShaderResources(0, 8, nullSrvs8);
        ID3D11UnorderedAccessView* resolveUavs[3]{ m_impl->accumPixelsUav.Get(), nullptr, nullptr };
        m_impl->context->CSSetUnorderedAccessViews(0, 3, resolveUavs, nullptr);

        ID3D11ShaderResourceView* resolveSrvs[8]{
            m_impl->outItersSrv.Get(),
            m_impl->pixelSubInfoSrv.Get(),
            m_impl->subOutSrv.Get(),
            m_impl->paletteLutSrv.Get(),
            (!m_impl->movieFullFrameAccum && temporalStep > 0) ? m_impl->prevBandSrv.Get() : nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        m_impl->context->CSSetShaderResources(0, 8, resolveSrvs);

        if (!m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &postCb, sizeof(postCb)))
        {
            m_impl->UnbindComputeResources();
            return false;
        }
        m_impl->context->CSSetShader(m_impl->csPostResolve.Get(), nullptr, 0);
        m_impl->context->Dispatch(visGroupX, visGroupY, 1);

        m_impl->UnbindComputeResources();

        // For 8K+ frames exceeding single-buffer full-frame accumulation, read back only this strip;
        // otherwise flush queued GPU dispatches immediately so CPU reference/BLA prep for the next
        // temporal sample overlaps with GPU execution.
        if (!m_impl->movieFullFrameAccum)
        {
            const size_t stripBytes = visiblePixels * sizeof(uint32_t);
            D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(stripBytes), 1, 1 };
            m_impl->context->CopySubresourceRegion(
                m_impl->accumPixelsStaging.Get(), 0, 0, 0, 0, m_impl->accumPixelsBuf.Get(), 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_impl->context->Map(m_impl->accumPixelsStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                return false;
            }
            std::memcpy(ioBandPixels, mapped.pData, stripBytes);
            m_impl->context->Unmap(m_impl->accumPixelsStaging.Get(), 0);
        }
        else
        {
            m_impl->context->Flush();
        }

        return true;
    }

    bool GpuFractalSolver::EndMovieFrame(
        int width,
        int height,
        uint32_t* outFramePixels,
        const GpuMovieShutterParams* shutterParams)
    {
        if (!m_impl || !m_impl->available || !outFramePixels || width <= 0 || height <= 0)
        {
            return false;
        }

        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->movieFullFrameAccum)
        {
            return true;
        }

        ID3D11Buffer* readbackSrc = m_impl->accumPixelsBuf.Get();
        if (m_impl->movieShutterBlur && shutterParams && shutterParams->tapCount > 1)
        {
            const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
            if (!m_impl->EnsureStructuredUavBuffer(
                    totalPixels, sizeof(uint32_t),
                    m_impl->shutterPixelsBuf, m_impl->shutterPixelsUav, m_impl->shutterPixelsCapElems))
            {
                return false;
            }

            ShutterCb shutterCb{};
            shutterCb.width = static_cast<uint32_t>(width);
            shutterCb.height = static_cast<uint32_t>(height);
            shutterCb.tapCount = std::min<uint32_t>(shutterParams->tapCount, kMovieShutterTapCount);
            for (uint32_t k = 0; k < shutterCb.tapCount; ++k)
            {
                shutterCb.taps[k].shiftX = shutterParams->shiftX[k];
                shutterCb.taps[k].shiftY = shutterParams->shiftY[k];
                shutterCb.taps[k].scale = shutterParams->scale[k];
            }

            if (!m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &shutterCb, sizeof(shutterCb)))
            {
                return false;
            }

            ID3D11Buffer* cbs[]{ m_impl->constantBuffer.Get() };
            m_impl->context->CSSetConstantBuffers(0, 1, cbs);
            m_impl->UnbindComputeResources();

            ID3D11UnorderedAccessView* uavs[3]{ m_impl->shutterPixelsUav.Get(), nullptr, nullptr };
            m_impl->context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            ID3D11ShaderResourceView* srvs[8]{ m_impl->accumPixelsSrv.Get() };
            m_impl->context->CSSetShaderResources(0, 8, srvs);
            m_impl->context->CSSetShader(m_impl->csPostShutter.Get(), nullptr, 0);
            m_impl->context->Dispatch(
                static_cast<UINT>((width + 15) / 16),
                static_cast<UINT>((height + 15) / 16),
                1);
            m_impl->UnbindComputeResources();
            readbackSrc = m_impl->shutterPixelsBuf.Get();
        }

        const size_t totalBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);
        D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(totalBytes), 1, 1 };
        m_impl->context->CopySubresourceRegion(
            m_impl->accumPixelsStaging.Get(), 0, 0, 0, 0, readbackSrc, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_impl->context->Map(m_impl->accumPixelsStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            return false;
        }
        std::memcpy(outFramePixels, mapped.pData, totalBytes);
        m_impl->context->Unmap(m_impl->accumPixelsStaging.Get(), 0);
        return true;
    }
}
