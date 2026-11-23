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
        m_impl->movieFullFrameAccum = false;
        m_impl->movieShutterBlur = false;
        m_impl->movieFrameWidth = 0;
        m_impl->movieFrameHeight = 0;
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
            m_impl->movieFrameWidth = width;
            m_impl->movieFrameHeight = height;
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

        if (m_impl->movieFullFrameAccum)
        {
            if (width != m_impl->movieFrameWidth ||
                destRowOffset + visibleRows > m_impl->movieFrameHeight ||
                static_cast<size_t>(destRowOffset + visibleRows) * static_cast<size_t>(width) > m_impl->accumPixelsCapElems)
            {
                return false;
            }
        }
        else
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
        }

        ID3D11Buffer* cbs[]{ m_impl->constantBuffer.Get() };
        m_impl->context->CSSetConstantBuffers(0, 1, cbs);
        m_impl->UnbindComputeResources();

        // Step A: Evaluate primary center pixels across the band (including halo rows) into OutIters in VRAM
        bool primaryCanceled = false;
        if (!m_impl->DispatchPrimaryPass(
                mode, bandConfig, refState, juliaCx, juliaCy, juliaBailout2,
                cancelToken, primaryCanceled) || primaryCanceled)
        {
            return false;
        }

        // Steps B-D: GPU 3x3 boundary detection, indirect-args build, and 4x subpixel evaluation
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
        postCb.subGroupThreads =
            (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla) ? 512u : 256u;

        if (!m_impl->DispatchGpuSubpixelPass(
            mode, bandConfig, refState, juliaCx, juliaCy, juliaBailout2, postCb))
        {
            return false;
        }

        // Step E: Fused 4x supersampled palette coloring into the full-frame (or strip) BGRA buffer
        const UINT visGroupX = static_cast<UINT>((width + 15) / 16);
        const UINT visGroupY = static_cast<UINT>((visibleRows + 15) / 16);
        ID3D11ShaderResourceView* nullSrvs8[8]{};
        m_impl->context->CSSetShaderResources(0, 8, nullSrvs8);
        ID3D11UnorderedAccessView* resolveUavs[3]{ m_impl->accumPixelsUav.Get(), nullptr, nullptr };
        m_impl->context->CSSetUnorderedAccessViews(0, 3, resolveUavs, nullptr);

        ID3D11ShaderResourceView* resolveSrvs[8]{
            m_impl->outItersSrv.Get(),
            m_impl->pixelSubInfoSrv.Get(),
            m_impl->subOutSrv.Get(),
            m_impl->paletteLutSrv.Get(),
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        m_impl->context->CSSetShaderResources(0, 8, resolveSrvs);

        ID3D11ComputeShader* csResolve = m_impl->EnsureShader(GpuShaderId::PostResolve);
        if (!csResolve || !m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &postCb, sizeof(postCb)))
        {
            m_impl->UnbindComputeResources();
            return false;
        }
        m_impl->context->CSSetShader(csResolve, nullptr, 0);
        m_impl->context->Dispatch(visGroupX, visGroupY, 1);

        m_impl->UnbindComputeResources();

        // For 8K+ frames exceeding single-buffer full-frame accumulation, read back only this strip;
        // otherwise flush queued GPU dispatches immediately so CPU band setup overlaps with GPU execution.
        if (!m_impl->movieFullFrameAccum)
        {
            const size_t stripBytes = visiblePixels * sizeof(uint32_t);
            if (!m_impl->ReadbackBuffer(
                m_impl->accumPixelsBuf.Get(), m_impl->accumPixelsStaging.Get(), ioBandPixels, stripBytes))
            {
                return false;
            }
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

        const bool useShutter = m_impl->movieShutterBlur;
        const int expectedW = m_impl->movieFrameWidth;
        const int expectedH = m_impl->movieFrameHeight;
        m_impl->movieFullFrameAccum = false;
        m_impl->movieShutterBlur = false;
        m_impl->movieFrameWidth = 0;
        m_impl->movieFrameHeight = 0;

        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        const size_t totalBytes = totalPixels * sizeof(uint32_t);
        if (width != expectedW || height != expectedH ||
            totalPixels > m_impl->accumPixelsCapElems ||
            totalBytes > m_impl->accumPixelsStagingCapBytes)
        {
            return false;
        }

        ID3D11Buffer* readbackSrc = m_impl->accumPixelsBuf.Get();
        if (useShutter && shutterParams && shutterParams->tapCount > 1)
        {
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

            ID3D11ComputeShader* csShutter = m_impl->EnsureShader(GpuShaderId::PostShutter);
            if (!csShutter)
            {
                return false;
            }
            ID3D11UnorderedAccessView* uavs[3]{ m_impl->shutterPixelsUav.Get(), nullptr, nullptr };
            m_impl->context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            ID3D11ShaderResourceView* srvs[8]{ m_impl->accumPixelsSrv.Get() };
            m_impl->context->CSSetShaderResources(0, 8, srvs);
            m_impl->context->CSSetShader(csShutter, nullptr, 0);
            m_impl->context->Dispatch(
                static_cast<UINT>((width + 15) / 16),
                static_cast<UINT>((height + 15) / 16),
                1);
            m_impl->UnbindComputeResources();
            readbackSrc = m_impl->shutterPixelsBuf.Get();
        }

        return m_impl->ReadbackBuffer(
            readbackSrc, m_impl->accumPixelsStaging.Get(), outFramePixels, totalBytes);
    }
}
