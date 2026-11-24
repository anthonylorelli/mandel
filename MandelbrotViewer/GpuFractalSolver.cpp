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

    void GpuFractalSolver::SetMultiPassSubpixelsEnabled(bool enabled) noexcept
    {
        if (!m_impl) return;
        std::lock_guard lock(m_impl->mutex);
        m_impl->multiPassSubpixelsEnabled = enabled;
    }

    void GpuFractalSolver::SetMultiPassSubpixelBatchLimit(size_t maxRays) noexcept
    {
        if (!m_impl) return;
        std::lock_guard lock(m_impl->mutex);
        m_impl->multiPassSubpixelBatchLimit = (maxRays == 0)
            ? kMaxMultiPassSubRays
            : std::clamp<size_t>(maxRays, 1, kMaxMultiPassSubRays);
    }

    bool GpuFractalSolver::Impl::DispatchPrimaryPass(
        GpuShaderMode mode,
        const FractalConfig& config,
        const DeepReferenceState& refState,
        double juliaCx,
        double juliaCy,
        double juliaBailout2,
        const std::atomic<bool>* cancelToken,
        bool& outCanceled)
    {
        outCanceled = false;
        auto canceled = [&] { return cancelToken && cancelToken->load(std::memory_order_relaxed); };
        const int width = config.width;
        const int height = config.height;
        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        const UINT zeros[4]{ 0, 0, 0, 0 };
        const UINT primGroupX = (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla)
            ? static_cast<UINT>((width + 31) / 32)
            : static_cast<UINT>((width + 15) / 16);

        const uint32_t maxIterU = static_cast<uint32_t>(config.maxIterations);
        const bool useMultiPass =
            (mode == GpuShaderMode::PerturbLdsBla || mode == GpuShaderMode::PerturbDeepExp) &&
            maxIterU > kMultiPassMinMaxIter &&
            EnsureStructuredUavBuffer(
                totalPixels, sizeof(GpuPerturbState),
                activeStateBuf, activeStateUav, activeStateCapElems) &&
            EnsureTypedUavSrvBuffer(
                totalPixels, sizeof(uint32_t), DXGI_FORMAT_R32_UINT,
                activeIdxBuf[0], activeIdxUav[0], activeIdxSrv[0], activeIdxCapElems[0]) &&
            EnsureTypedUavSrvBuffer(
                totalPixels, sizeof(uint32_t), DXGI_FORMAT_R32_UINT,
                activeIdxBuf[1], activeIdxUav[1], activeIdxSrv[1], activeIdxCapElems[1]);

        if (useMultiPass)
        {
            ID3D11ComputeShader* csStage0 = EnsureShader(
                (mode == GpuShaderMode::PerturbLdsBla) ? GpuShaderId::LdsBlaStage0 : GpuShaderId::DeepExpStage0);
            ID3D11ComputeShader* csActiveInd = EnsureShader(GpuShaderId::PostBuildActiveIndirect);
            ID3D11ComputeShader* csResume = EnsureShader(
                (mode == GpuShaderMode::PerturbLdsBla) ? GpuShaderId::LdsBlaResume : GpuShaderId::DeepExpResume);
            if (!csStage0 || !csActiveInd || !csResume)
            {
                return false;
            }

            const uint32_t stage0Limit = std::min<uint32_t>(1024u, maxIterU);
            context->ClearUnorderedAccessViewUint(activeCountUav[0].Get(), zeros);

            BindFractalSrvs(mode, nullptr, nullptr);
            ID3D11UnorderedAccessView* stage0Uavs[5]{
                outItersUav.Get(),
                nullptr,
                activeStateUav.Get(),
                activeIdxUav[0].Get(),
                activeCountUav[0].Get()
            };
            context->CSSetUnorderedAccessViews(0, 5, stage0Uavs, nullptr);
            context->CSSetShader(csStage0, nullptr, 0);

            if (!UploadFractalCb(
                mode, config, refState, juliaCx, juliaCy, juliaBailout2,
                0u, 0u, 0u, stage0Limit))
            {
                UnbindComputeResources();
                return false;
            }
            context->Dispatch(primGroupX, static_cast<UINT>((height + 15) / 16), 1);

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
                    UnbindComputeResources();
                    outCanceled = true;
                    return true;
                }
                const uint32_t nextLimit = std::min<uint32_t>(maxIterU, prevLimit + stageStep);
                const bool isFinalStage = (nextLimit >= maxIterU);
                const int activeOut = 1 - activeIn;

                // Build DispatchIndirect args for surviving active pixels
                UnbindComputeResources();
                if (!UploadBufferDiscard(constantBuffer.Get(), &activeIndCb, sizeof(activeIndCb)))
                {
                    return false;
                }
                ID3D11UnorderedAccessView* indUavs[5]{ indirectArgsUav.Get(), nullptr, nullptr, nullptr, nullptr };
                context->CSSetUnorderedAccessViews(0, 5, indUavs, nullptr);
                ID3D11ShaderResourceView* indSrvs[8]{};
                indSrvs[5] = activeCountSrv[activeIn].Get();
                context->CSSetShaderResources(0, 8, indSrvs);
                context->CSSetShader(csActiveInd, nullptr, 0);
                context->Dispatch(1, 1, 1);

                // Resume only surviving active pixels in densely packed warps
                UnbindComputeResources();
                if (!isFinalStage)
                {
                    context->ClearUnorderedAccessViewUint(activeCountUav[activeOut].Get(), zeros);
                }

                BindFractalSrvs(
                    mode,
                    activeIdxSrv[activeIn].Get(),
                    activeCountSrv[activeIn].Get());
                ID3D11UnorderedAccessView* resumeUavs[5]{
                    outItersUav.Get(),
                    nullptr,
                    activeStateUav.Get(),
                    isFinalStage ? nullptr : activeIdxUav[activeOut].Get(),
                    isFinalStage ? nullptr : activeCountUav[activeOut].Get()
                };
                context->CSSetUnorderedAccessViews(0, 5, resumeUavs, nullptr);
                context->CSSetShader(csResume, nullptr, 0);

                if (!UploadFractalCb(
                    mode, config, refState, juliaCx, juliaCy, juliaBailout2,
                    0u, 0u, 0u, nextLimit))
                {
                    UnbindComputeResources();
                    return false;
                }
                context->DispatchIndirect(indirectArgsBuf.Get(), 0);
                context->Flush();

                activeIn = activeOut;
                prevLimit = nextLimit;
                stageStep = (stageStep < 8192u) ? 8192u : 12288u;
            }
            UnbindComputeResources();
            return true;
        }

        BindFractalSrvs(mode, nullptr, nullptr);
        ID3D11UnorderedAccessView* primUavs[3]{ outItersUav.Get(), nullptr, nullptr };
        context->CSSetUnorderedAccessViews(0, 3, primUavs, nullptr);
        if (!SetPrimaryShader(mode))
        {
            UnbindComputeResources();
            return false;
        }

        const size_t maxRowsByTdr = std::max<size_t>(
            1,
            static_cast<size_t>(kMaxPrimaryIterBudget /
                (static_cast<uint64_t>(width) * static_cast<uint64_t>(std::max(1, config.maxIterations)))));
        size_t chunkRowsCap = std::min<size_t>(static_cast<size_t>(height), maxRowsByTdr);
        if (chunkRowsCap < static_cast<size_t>(height) && chunkRowsCap >= 16)
        {
            chunkRowsCap &= ~static_cast<size_t>(15);
        }
        const int maxChunkRows = static_cast<int>(chunkRowsCap);

        for (int rowStart = 0; rowStart < height; rowStart += maxChunkRows)
        {
            if (canceled())
            {
                UnbindComputeResources();
                outCanceled = true;
                return true;
            }
            const int chunkRows = std::min(maxChunkRows, height - rowStart);
            if (!UploadFractalCb(
                mode, config, refState, juliaCx, juliaCy, juliaBailout2,
                static_cast<uint32_t>(rowStart), static_cast<uint32_t>(rowStart), 0u, 0u,
                static_cast<uint32_t>(rowStart + chunkRows)))
            {
                UnbindComputeResources();
                return false;
            }
            context->Dispatch(primGroupX, static_cast<UINT>((chunkRows + 15) / 16), 1);
        }
        UnbindComputeResources();
        return true;
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

        // Fast interactive path (<= 4M pixels): keeps primary evaluation (with multi-pass active-pixel
        // compaction), GPU 3x3 boundary detection, and DispatchIndirect 4x subpixel evaluation entirely
        // in VRAM with a single end-of-frame GPU sync point.
        if (totalPixels <= kMaxSingleSyncInteractivePixels)
        {
            if (!m_impl->EnsureStructuredUavSrvBuffer(
                    totalPixels, sizeof(float),
                    m_impl->outItersBuf, m_impl->outItersUav, m_impl->outItersSrv, m_impl->outItersCapElems) ||
                !m_impl->EnsureStagingReadBuffer(
                    totalPixels * sizeof(float), m_impl->outItersStaging, m_impl->outItersStagingCapBytes))
            {
                return false;
            }

            if (subpixelSamples)
            {
                if (!m_impl->EnsureTypedUavSrvBuffer(
                        totalPixels, sizeof(uint32_t), DXGI_FORMAT_R32_UINT,
                        m_impl->gpuSubIdxBuf, m_impl->gpuSubIdxUav, m_impl->gpuSubIdxSrv, m_impl->gpuSubIdxCapElems) ||
                    !m_impl->EnsureStructuredUavSrvBuffer(
                        totalPixels, sizeof(GpuPixelSubInfo),
                        m_impl->pixelSubInfoBuf, m_impl->pixelSubInfoUav, m_impl->pixelSubInfoSrv, m_impl->pixelSubInfoCapElems) ||
                    !m_impl->EnsureStructuredUavSrvBuffer(
                        totalPixels * 4, sizeof(float),
                        m_impl->subOutBuf, m_impl->subOutUav, m_impl->subOutSrv, m_impl->subOutCapFloats) ||
                    !m_impl->EnsureStagingReadBuffer(
                        sizeof(uint32_t), m_impl->subCounterStaging, m_impl->subCounterStagingCapBytes))
                {
                    return false;
                }
            }

            ID3D11Buffer* cbs[]{ m_impl->constantBuffer.Get() };
            m_impl->context->CSSetConstantBuffers(0, 1, cbs);
            m_impl->UnbindComputeResources();

            // Pass 1: Primary evaluation (multi-pass active-pixel compaction when maxIter > 2048)
            bool primaryCanceled = false;
            if (!m_impl->DispatchPrimaryPass(
                    mode, config, refState, juliaCx, juliaCy, juliaBailout2,
                    cancelToken, primaryCanceled))
            {
                return false;
            }
            if (primaryCanceled)
            {
                return true;
            }

            // Pass 2: GPU-driven 3x3 boundary detection + 16x16 spatial compaction + DispatchIndirect 4x subpixels
            bool ranSubpixelPass = false;
            if (subpixelSamples && !canceled())
            {
                PostProcessCb postCb{};
                postCb.width = static_cast<uint32_t>(width);
                postCb.bandHeight = static_cast<uint32_t>(height);
                postCb.visibleRows = static_cast<uint32_t>(height);
                postCb.topHalo = 0u;
                postCb.destRowOffset = 0u;
                postCb.fractalType = static_cast<uint32_t>(config.fractalType);
                postCb.smoothFlag = config.smooth ? 1u : 0u;
                postCb.subGroupThreads =
                    (mode == GpuShaderMode::PerturbLds || mode == GpuShaderMode::PerturbLdsBla) ? 512u : 256u;

                bool subCanceled = false;
                if (!m_impl->DispatchGpuSubpixelPass(
                    mode, config, refState, juliaCx, juliaCy, juliaBailout2, postCb, cancelToken, subCanceled))
                {
                    return false;
                }
                if (subCanceled)
                {
                    subpixelSamples->clear();
                    return true;
                }

                // Queue counter copy alongside primary iteration copy so there is only one GPU wait
                D3D11_BOX countBox{ 0, 0, 0, sizeof(uint32_t), 1, 1 };
                m_impl->context->CopySubresourceRegion(
                    m_impl->subCounterStaging.Get(), 0, 0, 0, 0, m_impl->subCounterBuf.Get(), 0, &countBox);
                ranSubpixelPass = true;
            }

            if (canceled())
            {
                if (subpixelSamples) subpixelSamples->clear();
                return true;
            }

            // Read back primary iteration buffer (single frame sync point)
            if (!m_impl->ReadbackBuffer(
                m_impl->outItersBuf.Get(),
                m_impl->outItersStaging.Get(),
                outIterations.data(),
                totalPixels * sizeof(float)))
            {
                return false;
            }

            if (ranSubpixelPass && !canceled())
            {
                D3D11_MAPPED_SUBRESOURCE mappedCount{};
                if (FAILED(m_impl->context->Map(m_impl->subCounterStaging.Get(), 0, D3D11_MAP_READ, 0, &mappedCount)))
                {
                    m_impl->CheckDeviceLost();
                    subpixelSamples->clear();
                    return false;
                }
                const uint32_t rawSubCount = *static_cast<const uint32_t*>(mappedCount.pData);
                m_impl->context->Unmap(m_impl->subCounterStaging.Get(), 0);

                const size_t subCount = std::min<size_t>(rawSubCount, totalPixels);
                if (subCount > 0)
                {
                    const size_t infoBytes = totalPixels * sizeof(GpuPixelSubInfo);
                    const size_t subBytes = subCount * 4 * sizeof(float);
                    if (!m_impl->EnsureStagingReadBuffer(
                            infoBytes, m_impl->pixelSubInfoStaging, m_impl->pixelSubInfoStagingCapBytes) ||
                        !m_impl->EnsureStagingReadBuffer(
                            subBytes, m_impl->subOutStaging, m_impl->subOutStagingCapBytes))
                    {
                        subpixelSamples->clear();
                        return false;
                    }

                    D3D11_BOX infoBox{ 0, 0, 0, static_cast<UINT>(infoBytes), 1, 1 };
                    m_impl->context->CopySubresourceRegion(
                        m_impl->pixelSubInfoStaging.Get(), 0, 0, 0, 0, m_impl->pixelSubInfoBuf.Get(), 0, &infoBox);
                    D3D11_BOX subBox{ 0, 0, 0, static_cast<UINT>(subBytes), 1, 1 };
                    m_impl->context->CopySubresourceRegion(
                        m_impl->subOutStaging.Get(), 0, 0, 0, 0, m_impl->subOutBuf.Get(), 0, &subBox);

                    D3D11_MAPPED_SUBRESOURCE mappedInfo{};
                    if (FAILED(m_impl->context->Map(m_impl->pixelSubInfoStaging.Get(), 0, D3D11_MAP_READ, 0, &mappedInfo)))
                    {
                        m_impl->CheckDeviceLost();
                        subpixelSamples->clear();
                        return false;
                    }
                    D3D11_MAPPED_SUBRESOURCE mappedSub{};
                    if (FAILED(m_impl->context->Map(m_impl->subOutStaging.Get(), 0, D3D11_MAP_READ, 0, &mappedSub)))
                    {
                        m_impl->context->Unmap(m_impl->pixelSubInfoStaging.Get(), 0);
                        m_impl->CheckDeviceLost();
                        subpixelSamples->clear();
                        return false;
                    }

                    const auto* infoPtr = static_cast<const GpuPixelSubInfo*>(mappedInfo.pData);
                    const auto* subPtr = static_cast<const float*>(mappedSub.pData);

                    // Emit SubpixelSamples in strictly ascending pixelIndex order for ColorTheme::Recolor.
                    std::vector<size_t> rowOffsets(static_cast<size_t>(height) + 1, 0);
                    #pragma omp parallel for schedule(static)
                    for (int y = 0; y < height; ++y)
                    {
                        const GpuPixelSubInfo* row = infoPtr + static_cast<size_t>(y) * static_cast<size_t>(width);
                        size_t cnt = 0;
                        for (int x = 0; x < width; ++x)
                        {
                            if (row[x].weight > 0.0f && row[x].slot < subCount) ++cnt;
                        }
                        rowOffsets[static_cast<size_t>(y) + 1] = cnt;
                    }
                    for (int y = 0; y < height; ++y)
                    {
                        rowOffsets[static_cast<size_t>(y) + 1] += rowOffsets[static_cast<size_t>(y)];
                    }

                    subpixelSamples->resize(rowOffsets.back());
                    #pragma omp parallel for schedule(static)
                    for (int y = 0; y < height; ++y)
                    {
                        size_t dstIdx = rowOffsets[static_cast<size_t>(y)];
                        if (dstIdx == rowOffsets[static_cast<size_t>(y) + 1]) continue;
                        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(width);
                        for (int x = 0; x < width; ++x)
                        {
                            const size_t pixIdx = rowBase + static_cast<size_t>(x);
                            const GpuPixelSubInfo info = infoPtr[pixIdx];
                            if (info.weight <= 0.0f || info.slot >= subCount) continue;
                            auto& dst = (*subpixelSamples)[dstIdx++];
                            dst.pixelIndex = static_cast<uint32_t>(pixIdx);
                            dst.weight = info.weight;
                            const float* s4 = subPtr + static_cast<size_t>(info.slot) * 4;
                            dst.iterations[0] = s4[0];
                            dst.iterations[1] = s4[1];
                            dst.iterations[2] = s4[2];
                            dst.iterations[3] = s4[3];
                        }
                    }

                    m_impl->context->Unmap(m_impl->subOutStaging.Get(), 0);
                    m_impl->context->Unmap(m_impl->pixelSubInfoStaging.Get(), 0);
                }
            }

            if (canceled() && subpixelSamples)
            {
                subpixelSamples->clear();
            }

            outElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - startTime).count();
            return true;
        }

        // Large-frame (> 4M pixels) chunked/batched fallback path
        const size_t maxRowsByMem = std::max<size_t>(1, kMaxPrimaryChunkPixels / static_cast<size_t>(width));
        const size_t maxRowsByTdr = std::max<size_t>(
            1,
            static_cast<size_t>(kMaxPrimaryIterBudget /
                (static_cast<uint64_t>(width) * static_cast<uint64_t>(std::max(1, maxIter)))));
        size_t chunkRowsCap = std::min<size_t>({ static_cast<size_t>(height), maxRowsByMem, maxRowsByTdr });
        if (chunkRowsCap < static_cast<size_t>(height) && chunkRowsCap >= 16)
        {
            chunkRowsCap &= ~static_cast<size_t>(15);
        }
        const int maxChunkRows = static_cast<int>(chunkRowsCap);
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
                static_cast<uint32_t>(rowStart), 0u, 0u, 0u,
                static_cast<uint32_t>(rowStart + chunkRows)))
            {
                m_impl->UnbindComputeResources();
                return false;
            }

            const UINT groupY = static_cast<UINT>((chunkRows + 15) / 16);
            m_impl->context->Dispatch(groupX, groupY, 1);

            if (!m_impl->ReadbackBuffer(
                m_impl->outItersBuf.Get(),
                m_impl->outItersStaging.Get(),
                outIterations.data() + static_cast<size_t>(rowStart) * static_cast<size_t>(width),
                chunkFloats * sizeof(float)))
            {
                m_impl->UnbindComputeResources();
                return false;
            }
        }

        // Pass 2: Optional 4x adaptive boundary subpixel refinement
        if (subpixelSamples && !canceled())
        {
            if (!BuildSubpixelMask(
                width, height, config.smooth, config.fractalType,
                outIterations, sampleWeights, *subpixelSamples, cancelToken, &m_impl->hostSlotOfPixel))
            {
                subpixelSamples->clear();
                m_impl->UnbindComputeResources();
                return true;
            }
            const size_t subCount = subpixelSamples->size();

            if (subCount > 0)
            {
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
                if (!m_impl->SetSubpixelShader(mode))
                {
                    subpixelSamples->clear();
                    m_impl->UnbindComputeResources();
                    return false;
                }

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

                    m_impl->hostSubFloats.resize(subTotal);
                    if (!m_impl->ReadbackBuffer(
                        m_impl->subOutBuf.Get(),
                        m_impl->subOutStaging.Get(),
                        m_impl->hostSubFloats.data(),
                        subTotal * sizeof(float)))
                    {
                        subpixelSamples->clear();
                        m_impl->UnbindComputeResources();
                        return false;
                    }

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
