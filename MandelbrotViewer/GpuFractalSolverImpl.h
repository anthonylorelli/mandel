#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "GpuFractalSolver.h"
#include "GpuMandelbrotBla.h"
#include "GpuSolverTypes.h"

#include <array>
#include <mutex>
#include <vector>

namespace Mandelbrot
{
    enum class GpuShaderId : size_t
    {
        ShallowPrim = 0,
        ShallowSub,
        LdsPrim,
        LdsSub,
        LdsBlaPrim,
        LdsBlaSub,
        LdsBlaStage0,
        LdsBlaResume,
        LdsBlaSubStage0,
        LdsBlaSubResume,
        DeepExpPrim,
        DeepExpSub,
        DeepExpStage0,
        DeepExpResume,
        DeepExpSubStage0,
        DeepExpSubResume,
        PostDetect,
        PostBuildIndirect,
        PostBuildActiveIndirect,
        PostResolve,
        PostShutter,
        Count
    };

    inline constexpr size_t kGpuShaderCount = static_cast<size_t>(GpuShaderId::Count);

    struct GpuFractalSolver::Impl
    {
        template <typename T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        enum class PrepResult
        {
            Ok,
            Canceled,
            Fallback
        };

        std::mutex mutex;
        bool available{ false };
        bool usingSoftwareAdapter{ false };
        bool multiPassSubpixelsEnabled{ true };
        size_t multiPassSubpixelBatchLimit{ detail::kMaxMultiPassSubRays };

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        std::array<ComPtr<ID3D11ComputeShader>, kGpuShaderCount> shaders;

        ComPtr<ID3D11Buffer> constantBuffer;

        ComPtr<ID3D11Buffer> outItersBuf, outItersStaging;
        ComPtr<ID3D11UnorderedAccessView> outItersUav;
        ComPtr<ID3D11ShaderResourceView> outItersSrv;
        size_t outItersCapElems{ 0 };
        size_t outItersStagingCapBytes{ 0 };

        ComPtr<ID3D11Buffer> subIdxBuf;
        ComPtr<ID3D11ShaderResourceView> subIdxSrv;
        size_t subIdxCapElems{ 0 };

        ComPtr<ID3D11Buffer> subOutBuf, subOutStaging;
        ComPtr<ID3D11UnorderedAccessView> subOutUav;
        ComPtr<ID3D11ShaderResourceView> subOutSrv;
        size_t subOutCapFloats{ 0 };
        size_t subOutStagingCapBytes{ 0 };

        // Fused GPU post-processing & interactive readback resources
        ComPtr<ID3D11Buffer> subCounterBuf, subCounterStaging;
        ComPtr<ID3D11UnorderedAccessView> subCounterUav;
        ComPtr<ID3D11ShaderResourceView> subCounterSrv;
        size_t subCounterStagingCapBytes{ 0 };

        // Multi-pass active-pixel compaction resources for high-iteration deep zooms
        ComPtr<ID3D11Buffer> activeStateBuf;
        ComPtr<ID3D11UnorderedAccessView> activeStateUav;
        size_t activeStateCapElems{ 0 };

        ComPtr<ID3D11Buffer> activeIdxBuf[2];
        ComPtr<ID3D11UnorderedAccessView> activeIdxUav[2];
        ComPtr<ID3D11ShaderResourceView> activeIdxSrv[2];
        size_t activeIdxCapElems[2]{ 0, 0 };

        ComPtr<ID3D11Buffer> activeCountBuf[2];
        ComPtr<ID3D11UnorderedAccessView> activeCountUav[2];
        ComPtr<ID3D11ShaderResourceView> activeCountSrv[2];

        ComPtr<ID3D11Buffer> indirectArgsBuf;
        ComPtr<ID3D11UnorderedAccessView> indirectArgsUav;

        ComPtr<ID3D11Buffer> paletteLutBuf;
        ComPtr<ID3D11ShaderResourceView> paletteLutSrv;
        uint64_t uploadedLutHash{ 0 };
        bool hasUploadedLut{ false };

        ComPtr<ID3D11Buffer> gpuSubIdxBuf;
        ComPtr<ID3D11UnorderedAccessView> gpuSubIdxUav;
        ComPtr<ID3D11ShaderResourceView> gpuSubIdxSrv;
        size_t gpuSubIdxCapElems{ 0 };

        ComPtr<ID3D11Buffer> pixelSubInfoBuf, pixelSubInfoStaging;
        ComPtr<ID3D11UnorderedAccessView> pixelSubInfoUav;
        ComPtr<ID3D11ShaderResourceView> pixelSubInfoSrv;
        size_t pixelSubInfoCapElems{ 0 };
        size_t pixelSubInfoStagingCapBytes{ 0 };

        ComPtr<ID3D11Buffer> accumPixelsBuf, accumPixelsStaging;
        ComPtr<ID3D11UnorderedAccessView> accumPixelsUav;
        ComPtr<ID3D11ShaderResourceView> accumPixelsSrv;
        size_t accumPixelsCapElems{ 0 };
        size_t accumPixelsStagingCapBytes{ 0 };
        bool movieFullFrameAccum{ false };
        bool movieShutterBlur{ false };
        int movieFrameWidth{ 0 };
        int movieFrameHeight{ 0 };

        ComPtr<ID3D11Buffer> shutterPixelsBuf;
        ComPtr<ID3D11UnorderedAccessView> shutterPixelsUav;
        size_t shutterPixelsCapElems{ 0 };

        ComPtr<ID3D11Buffer> colCoordsBuf, rowCoordsBuf;
        ComPtr<ID3D11ShaderResourceView> colCoordsSrv, rowCoordsSrv;
        size_t colCoordsCapFloats{ 0 }, rowCoordsCapFloats{ 0 };

        ComPtr<ID3D11Buffer> colCoordsExpBuf, rowCoordsExpBuf;
        ComPtr<ID3D11ShaderResourceView> colCoordsExpSrv, rowCoordsExpSrv;
        size_t colCoordsExpCapElems{ 0 }, rowCoordsExpCapElems{ 0 };

        // Cached column coordinate upload key to skip redundant per-band uploads within a frame
        bool hasUploadedCols{ false };
        detail::GpuShaderMode uploadedColMode{ detail::GpuShaderMode::Shallow };
        int uploadedColWidth{ 0 };
        double uploadedColOrigin{ 0.0 };
        double uploadedColStep{ 0.0 };

        ComPtr<ID3D11Buffer> orbitFastBuf;
        ComPtr<ID3D11ShaderResourceView> orbitFastSrv;
        size_t orbitFastCapElems{ 0 };

        ComPtr<ID3D11Buffer> orbitFullBuf;
        ComPtr<ID3D11ShaderResourceView> orbitFullSrv;
        size_t orbitFullCapElems{ 0 };

        ComPtr<ID3D11Buffer> blaF32Buf;
        ComPtr<ID3D11ShaderResourceView> blaF32Srv;
        size_t blaF32CapElems{ 0 };

        ComPtr<ID3D11Buffer> blaExpBuf;
        ComPtr<ID3D11ShaderResourceView> blaExpSrv;
        size_t blaExpCapElems{ 0 };

        ComPtr<ID3D11Buffer> blaMetaBuf;
        ComPtr<ID3D11ShaderResourceView> blaMetaSrv;

        // Cached orbit & BLA state uploaded to GPU
        const DeepReferenceCache* uploadedCachePtr{ nullptr };
        uint64_t uploadedOrbitVersion{ 0 };
        int uploadedRefLen{ -1 };
        FractalType uploadedType{ FractalType::Mandelbrot };
        bool uploadedHasDeepExpOrbit{ false };

        const DeepReferenceCache* uploadedBlaCachePtr{ nullptr };
        uint64_t uploadedBlaOrbitVersion{ 0 };
        int uploadedBlaRefLen{ -1 };
        double uploadedBlaCMax{ 0.0 };
        bool uploadedBlaForceDeepExp{ false };
        bool uploadedHasBlaF32{ false };
        bool uploadedHasBlaExp{ false };
        detail::GpuMandelbrotBlaBuilder blaBuilder;

        // Reusable host staging vectors to avoid heap churn and WC memory stalls
        std::vector<float> hostColCoords, hostRowCoords;
        std::vector<detail::GpuCoordExp> hostColCoordsExp, hostRowCoordsExp;
        std::vector<float> hostOrbitFast;
        std::vector<detail::GpuOrbitPointExp> hostOrbitExp;
        std::vector<uint32_t> hostSlotOfPixel;
        std::vector<uint32_t> hostTiledPixels;
        std::vector<uint32_t> hostTiledSlots;
        std::vector<float> hostSubFloats;

        explicit Impl(bool allowSoftwareAdapter);

        bool CanSupportConfig(const FractalConfig& config) const noexcept;
        void CheckDeviceLost() noexcept;

        bool EnsureBufferImpl(
            size_t neededElems,
            UINT elemBytes,
            DXGI_FORMAT format,
            bool structured,
            bool dynamicUpload,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11UnorderedAccessView>* uav,
            ComPtr<ID3D11ShaderResourceView>* srv,
            size_t& capElems);

        bool EnsureTypedSrvBuffer(
            size_t neededElems,
            UINT elemBytes,
            DXGI_FORMAT format,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11ShaderResourceView>& srv,
            size_t& capElems);

        bool EnsureStructuredSrvBuffer(
            size_t neededElems,
            UINT strideBytes,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11ShaderResourceView>& srv,
            size_t& capElems);

        bool EnsureStructuredUavSrvBuffer(
            size_t neededElems,
            UINT strideBytes,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11UnorderedAccessView>& uav,
            ComPtr<ID3D11ShaderResourceView>& srv,
            size_t& capElems);

        bool EnsureTypedUavSrvBuffer(
            size_t neededElems,
            UINT elemBytes,
            DXGI_FORMAT format,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11UnorderedAccessView>& uav,
            ComPtr<ID3D11ShaderResourceView>& srv,
            size_t& capElems);

        bool EnsureStructuredUavBuffer(
            size_t neededElems,
            UINT strideBytes,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11UnorderedAccessView>& uav,
            size_t& capElems);

        bool EnsureStagingReadBuffer(
            size_t neededBytes,
            ComPtr<ID3D11Buffer>& stagingBuf,
            size_t& capBytes);

        bool UploadBufferDiscard(ID3D11Buffer* buf, const void* src, size_t bytes);
        bool EnsurePaletteLutUploaded(const uint32_t* lutData);

        bool SyncReferenceAndBla(
            const FractalConfig& config,
            const DeepReferenceCache* cacheKey,
            const DeepReferenceState& refState,
            double cMax,
            const std::atomic<bool>* cancelToken,
            bool& outRunDeepExp);

        PrepResult PrepareModeAndCoords(
            const FractalConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            DeepReferenceCache& referenceCache,
            const std::atomic<bool>* cancelToken,
            detail::GpuShaderMode& outMode,
            DeepReferenceState& outRefState,
            double& outJuliaCx,
            double& outJuliaCy,
            double& outJuliaBailout2);

        bool UploadFractalCb(
            detail::GpuShaderMode mode,
            const FractalConfig& config,
            const DeepReferenceState& refState,
            double juliaCx,
            double juliaCy,
            double juliaBailout2,
            uint32_t rowOffset,
            uint32_t outRowOffset,
            uint32_t subTotal,
            uint32_t stageIterLimit = 0u,
            uint32_t heightLimit = 0u,
            uint32_t subRayOffset = 0u);

        void BindFractalSrvs(
            detail::GpuShaderMode mode,
            ID3D11ShaderResourceView* activeSubIdxSrv,
            ID3D11ShaderResourceView* activeSubCountSrv,
            ID3D11ShaderResourceView* subpixelPixelsSrv = nullptr);

        ID3D11ComputeShader* EnsureShader(GpuShaderId id);
        bool SetPrimaryShader(detail::GpuShaderMode mode);
        bool SetSubpixelShader(detail::GpuShaderMode mode);
        void UnbindComputeResources() noexcept;

        // Runs the ping-pong active-compaction resume stages after Stage 0 for either primary
        // pixels (isSubpixelPass = false) or boundary subpixel rays (isSubpixelPass = true).
        bool DispatchMultiPassResumeLoop(
            detail::GpuShaderMode mode,
            const FractalConfig& config,
            const DeepReferenceState& refState,
            double juliaCx,
            double juliaCy,
            double juliaBailout2,
            ID3D11ComputeShader* csActiveInd,
            ID3D11ComputeShader* csResume,
            uint32_t stage0Limit,
            bool isSubpixelPass,
            uint32_t subRayOffset,
            bool flushEveryStage,
            const std::atomic<bool>* cancelToken,
            bool& outCanceled);

        // Dispatches primary center-pixel evaluation into outItersBuf using multi-pass
        // active-pixel compaction when eligible, or TDR-safe single-pass row chunks otherwise.
        bool DispatchPrimaryPass(
            detail::GpuShaderMode mode,
            const FractalConfig& config,
            const DeepReferenceState& refState,
            double juliaCx,
            double juliaCy,
            double juliaBailout2,
            const std::atomic<bool>* cancelToken,
            bool& outCanceled);

        // Runs GPU 3x3 boundary detection, indirect-dispatch argument building, and
        // DispatchIndirect 4x subpixel evaluation (with multi-pass active-ray compaction
        // when eligible) for outItersBuf -> subOutBuf.
        bool DispatchGpuSubpixelPass(
            detail::GpuShaderMode mode,
            const FractalConfig& config,
            const DeepReferenceState& refState,
            double juliaCx,
            double juliaCy,
            double juliaBailout2,
            const detail::PostProcessCb& postCb,
            const std::atomic<bool>* cancelToken,
            bool& outCanceled);

        // Copies `bytes` from `srcBuf` to `stagingBuf`, maps, copies to `dst`, and unmaps.
        // Calls CheckDeviceLost() on Map failure.
        bool ReadbackBuffer(
            ID3D11Buffer* srcBuf,
            ID3D11Buffer* stagingBuf,
            void* dst,
            size_t bytes) noexcept;
    };
}
