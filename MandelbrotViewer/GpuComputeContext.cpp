#include "GpuFractalSolverImpl.h"
#include "GpuFractalShaders.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace Mandelbrot
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        using namespace detail;

        struct LazyShaderBlob
        {
            std::once_flag flag;
            ComPtr<ID3DBlob> blob;
        };

        struct ShaderSpec
        {
            const char* (*sourceFn)();
            const char* entryPoint;
        };

        const ShaderSpec& GetShaderSpec(GpuShaderId id) noexcept
        {
            static constexpr std::array<ShaderSpec, kGpuShaderCount> kSpecs{ {
                { &GetShallowShaderSource,        "CSPrimary" },
                { &GetShallowShaderSource,        "CSSubpixels" },
                { &GetPerturbLdsShaderSource,     "CSPrimary" },
                { &GetPerturbLdsShaderSource,     "CSSubpixels" },
                { &GetPerturbLdsBlaShaderSource,  "CSPrimary" },
                { &GetPerturbLdsBlaShaderSource,  "CSSubpixels" },
                { &GetPerturbLdsBlaShaderSource,  "CSPrimaryStage0" },
                { &GetPerturbLdsBlaShaderSource,  "CSPrimaryResume" },
                { &GetPerturbLdsBlaShaderSource,  "CSSubpixelsStage0" },
                { &GetPerturbLdsBlaShaderSource,  "CSSubpixelsResume" },
                { &GetPerturbDeepExpShaderSource, "CSPrimary" },
                { &GetPerturbDeepExpShaderSource, "CSSubpixels" },
                { &GetPerturbDeepExpShaderSource, "CSPrimaryStage0" },
                { &GetPerturbDeepExpShaderSource, "CSPrimaryResume" },
                { &GetPerturbDeepExpShaderSource, "CSSubpixelsStage0" },
                { &GetPerturbDeepExpShaderSource, "CSSubpixelsResume" },
                { &GetMovieDetectShaderSource,    "CSDetectBoundaries" },
                { &GetMovieDetectShaderSource,    "CSBuildIndirectArgs" },
                { &GetMovieDetectShaderSource,    "CSBuildActiveIndirectArgs" },
                { &GetMovieResolveShaderSource,   "CSResolveAndBlend" },
                { &GetMovieShutterShaderSource,   "CSShutterMotionBlur" }
            } };
            return kSpecs[static_cast<size_t>(id)];
        }

        ID3DBlob* GetCompiledShaderBlob(GpuShaderId id)
        {
            static std::array<LazyShaderBlob, kGpuShaderCount> cache;
            auto& slot = cache[static_cast<size_t>(id)];
            std::call_once(slot.flag, [id, &slot]()
            {
                const auto& spec = GetShaderSpec(id);
                const char* src = spec.sourceFn();
                ComPtr<ID3DBlob> err;
                const HRESULT hr = D3DCompile(
                    src,
                    std::strlen(src),
                    spec.entryPoint,
                    nullptr,
                    nullptr,
                    spec.entryPoint,
                    "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                    0,
                    &slot.blob,
                    &err);
#if defined(_DEBUG) && defined(_WIN32)
                if (FAILED(hr) && err && err->GetBufferPointer())
                {
                    OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
                }
#endif
                if (FAILED(hr))
                {
                    slot.blob.Reset();
                }
            });
            return slot.blob.Get();
        }

        size_t GrowCapacityBytes(size_t currentBytes, size_t neededBytes) noexcept
        {
            if (neededBytes <= currentBytes) return currentBytes;
            if (neededBytes >= 64ULL * 1024 * 1024) return neededBytes;
            const size_t grown = std::max(neededBytes, currentBytes + (currentBytes >> 1) + 4096);
            return std::min(grown, kD3D11SafeMaxBufferBytes);
        }
    }

    GpuFractalSolver::Impl::Impl(bool allowSoftwareAdapter)
    {
        const D3D_FEATURE_LEVEL featureLevels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL flOut{};

        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory)
        {
            for (UINT idx = 0;; ++idx)
            {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc))) continue;
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

                if (SUCCEEDED(D3D11CreateDevice(
                    adapter.Get(),
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    D3D11_CREATE_DEVICE_SINGLETHREADED,
                    featureLevels,
                    static_cast<UINT>(std::size(featureLevels)),
                    D3D11_SDK_VERSION,
                    &device,
                    &flOut,
                    &context)))
                {
                    usingSoftwareAdapter = false;
                    break;
                }
            }
        }

        if (!device && allowSoftwareAdapter)
        {
            if (SUCCEEDED(D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_SINGLETHREADED,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &device,
                &flOut,
                &context)))
            {
                usingSoftwareAdapter = true;
            }
        }

        if (!device || !context) return;

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = kGpuConstantBufferBytes;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &constantBuffer)))
        {
            return;
        }

        D3D11_BUFFER_DESC metaDesc{};
        metaDesc.ByteWidth = 32 * 8;
        metaDesc.Usage = D3D11_USAGE_DYNAMIC;
        metaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        metaDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        metaDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        metaDesc.StructureByteStride = 8;
        if (FAILED(device->CreateBuffer(&metaDesc, nullptr, &blaMetaBuf)))
        {
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC metaSrvDesc{};
        metaSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        metaSrvDesc.Buffer.NumElements = 32;
        if (FAILED(device->CreateShaderResourceView(blaMetaBuf.Get(), &metaSrvDesc, &blaMetaSrv)))
        {
            return;
        }

        // Fixed small GPU-resident buffers for fused movie post-processing
        D3D11_BUFFER_DESC counterDesc{};
        counterDesc.ByteWidth = 16;
        counterDesc.Usage = D3D11_USAGE_DEFAULT;
        counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateBuffer(&counterDesc, nullptr, &subCounterBuf)))
        {
            return;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC counterUavDesc{};
        counterUavDesc.Format = DXGI_FORMAT_R32_UINT;
        counterUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        counterUavDesc.Buffer.NumElements = 4;
        if (FAILED(device->CreateUnorderedAccessView(subCounterBuf.Get(), &counterUavDesc, &subCounterUav)))
        {
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC counterSrvDesc{};
        counterSrvDesc.Format = DXGI_FORMAT_R32_UINT;
        counterSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        counterSrvDesc.Buffer.NumElements = 4;
        if (FAILED(device->CreateShaderResourceView(subCounterBuf.Get(), &counterSrvDesc, &subCounterSrv)))
        {
            return;
        }

        for (int i = 0; i < 2; ++i)
        {
            if (FAILED(device->CreateBuffer(&counterDesc, nullptr, &activeCountBuf[i])) ||
                FAILED(device->CreateUnorderedAccessView(activeCountBuf[i].Get(), &counterUavDesc, &activeCountUav[i])) ||
                FAILED(device->CreateShaderResourceView(activeCountBuf[i].Get(), &counterSrvDesc, &activeCountSrv[i])))
            {
                return;
            }
        }

        D3D11_BUFFER_DESC indirectDesc{};
        indirectDesc.ByteWidth = 16;
        indirectDesc.Usage = D3D11_USAGE_DEFAULT;
        indirectDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        indirectDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        if (FAILED(device->CreateBuffer(&indirectDesc, nullptr, &indirectArgsBuf)) ||
            FAILED(device->CreateUnorderedAccessView(indirectArgsBuf.Get(), &counterUavDesc, &indirectArgsUav)))
        {
            return;
        }

        D3D11_BUFFER_DESC lutDesc{};
        lutDesc.ByteWidth = static_cast<UINT>(kPaletteLutSize * sizeof(uint32_t));
        lutDesc.Usage = D3D11_USAGE_DYNAMIC;
        lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        lutDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&lutDesc, nullptr, &paletteLutBuf)))
        {
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc{};
        lutSrvDesc.Format = DXGI_FORMAT_R32_UINT;
        lutSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        lutSrvDesc.Buffer.NumElements = static_cast<UINT>(kPaletteLutSize);
        if (FAILED(device->CreateShaderResourceView(paletteLutBuf.Get(), &lutSrvDesc, &paletteLutSrv)))
        {
            return;
        }

        D3D11_BUFFER_DESC histDesc{};
        histDesc.ByteWidth = static_cast<UINT>(kHistogramCdfSize * sizeof(float));
        histDesc.Usage = D3D11_USAGE_DYNAMIC;
        histDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        histDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&histDesc, nullptr, &histCdfBuf)))
        {
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC histSrvDesc{};
        histSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        histSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        histSrvDesc.Buffer.NumElements = static_cast<UINT>(kHistogramCdfSize);
        if (FAILED(device->CreateShaderResourceView(histCdfBuf.Get(), &histSrvDesc, &histCdfSrv)))
        {
            return;
        }

        available = true;
    }

    bool GpuFractalSolver::Impl::CanSupportConfig(const FractalConfig& config) const noexcept
    {
        if (!available || config.width <= 0 || config.height <= 0 || config.maxIterations < 0 ||
            !std::isfinite(config.zoom) || !(config.zoom > 0.0))
        {
            return false;
        }

        if (config.juliaSet)
        {
            if (config.zoom >= GpuPerturbationThreshold) return false;
            if (!std::isfinite(config.centerX) || !std::isfinite(config.centerY)) return false;
            if (config.fractalType != FractalType::Mandelbrot && config.fractalType != FractalType::BurningShip)
                return false;
            const double jx = config.juliaX.ToDouble();
            const double jy = config.juliaY.ToDouble();
            if (!std::isfinite(jx) || !std::isfinite(jy)) return false;
            const double bailout2 = std::max(16.0, 4.0 * std::hypot(jx, jy));
            return std::isfinite(bailout2) && bailout2 <= 1e18;
        }

        if (config.zoom < GpuPerturbationThreshold)
        {
            return std::isfinite(config.centerX) && std::isfinite(config.centerY);
        }

        switch (config.fractalType)
        {
        case FractalType::Mandelbrot:
            return config.zoom <= MaxZoom;
        case FractalType::Tricorn:
            return config.zoom < kGpuTricornMaxZoom;
        case FractalType::BurningShip:
        case FractalType::Buffalo:
            return config.zoom < DeepZoomThreshold;
        case FractalType::Celtic:
        case FractalType::CubicMultibrot:
        case FractalType::Phoenix:
        case FractalType::Newton:
        default:
            return false;
        }
    }

    bool GpuFractalSolver::Impl::EnsureBufferImpl(
        size_t neededElems,
        UINT elemBytes,
        DXGI_FORMAT format,
        bool structured,
        bool dynamicUpload,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11UnorderedAccessView>* uav,
        ComPtr<ID3D11ShaderResourceView>* srv,
        size_t& capElems)
    {
        if (neededElems <= capElems && buf && (!uav || *uav) && (!srv || *srv)) return true;
        const size_t currentBytes = capElems * elemBytes;
        const size_t neededBytes = std::max<size_t>(1, neededElems) * elemBytes;
        if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
        const size_t allocElems = allocBytes / elemBytes;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(allocElems * elemBytes);
        desc.Usage = dynamicUpload ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = dynamicUpload ? D3D11_CPU_ACCESS_WRITE : 0u;
        desc.BindFlags =
            (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u) |
            (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
        if (structured)
        {
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = elemBytes;
        }

        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

        const DXGI_FORMAT viewFormat = structured ? DXGI_FORMAT_UNKNOWN : format;

        ComPtr<ID3D11UnorderedAccessView> newUav;
        if (uav)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = viewFormat;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = static_cast<UINT>(allocElems);
            if (FAILED(device->CreateUnorderedAccessView(newBuf.Get(), &uavDesc, &newUav))) return false;
        }

        ComPtr<ID3D11ShaderResourceView> newSrv;
        if (srv)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = viewFormat;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);
            if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;
        }

        buf = std::move(newBuf);
        if (uav) *uav = std::move(newUav);
        if (srv) *srv = std::move(newSrv);
        capElems = allocElems;
        return true;
    }

    bool GpuFractalSolver::Impl::EnsureTypedSrvBuffer(
        size_t neededElems,
        UINT elemBytes,
        DXGI_FORMAT format,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        return EnsureBufferImpl(neededElems, elemBytes, format, false, true, buf, nullptr, &srv, capElems);
    }

    bool GpuFractalSolver::Impl::EnsureStructuredSrvBuffer(
        size_t neededElems,
        UINT strideBytes,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        return EnsureBufferImpl(neededElems, strideBytes, DXGI_FORMAT_UNKNOWN, true, true, buf, nullptr, &srv, capElems);
    }

    bool GpuFractalSolver::Impl::EnsureStructuredUavSrvBuffer(
        size_t neededElems,
        UINT strideBytes,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11UnorderedAccessView>& uav,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        return EnsureBufferImpl(neededElems, strideBytes, DXGI_FORMAT_UNKNOWN, true, false, buf, &uav, &srv, capElems);
    }

    bool GpuFractalSolver::Impl::EnsureTypedUavSrvBuffer(
        size_t neededElems,
        UINT elemBytes,
        DXGI_FORMAT format,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11UnorderedAccessView>& uav,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        return EnsureBufferImpl(neededElems, elemBytes, format, false, false, buf, &uav, &srv, capElems);
    }

    bool GpuFractalSolver::Impl::EnsureStructuredUavBuffer(
        size_t neededElems,
        UINT strideBytes,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11UnorderedAccessView>& uav,
        size_t& capElems)
    {
        return EnsureBufferImpl(neededElems, strideBytes, DXGI_FORMAT_UNKNOWN, true, false, buf, &uav, nullptr, capElems);
    }

    bool GpuFractalSolver::Impl::EnsureStagingReadBuffer(
        size_t neededBytes,
        ComPtr<ID3D11Buffer>& stagingBuf,
        size_t& capBytes)
    {
        if (neededBytes <= capBytes && stagingBuf) return true;
        const size_t reqBytes = std::max<size_t>(4, neededBytes);
        if (reqBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(capBytes, reqBytes);

        D3D11_BUFFER_DESC stageDesc{};
        stageDesc.ByteWidth = static_cast<UINT>(allocBytes);
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Buffer> newStaging;
        if (FAILED(device->CreateBuffer(&stageDesc, nullptr, &newStaging))) return false;

        stagingBuf = std::move(newStaging);
        capBytes = allocBytes;
        return true;
    }

    void GpuFractalSolver::Impl::CheckDeviceLost() noexcept
    {
        if (device && FAILED(device->GetDeviceRemovedReason()))
        {
            available = false;
        }
    }

    bool GpuFractalSolver::Impl::UploadBufferDiscard(ID3D11Buffer* buf, const void* src, size_t bytes)
    {
        if (bytes == 0) return true;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            CheckDeviceLost();
            return false;
        }
        std::memcpy(mapped.pData, src, bytes);
        context->Unmap(buf, 0);
        return true;
    }

    bool GpuFractalSolver::Impl::EnsurePaletteLutUploaded(const uint32_t* lutData)
    {
        if (!lutData) return false;
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < kPaletteLutSize; ++i)
        {
            hash ^= lutData[i];
            hash *= 1099511628211ULL;
        }
        if (hasUploadedLut && hash == uploadedLutHash)
        {
            return true;
        }
        if (!UploadBufferDiscard(paletteLutBuf.Get(), lutData, kPaletteLutSize * sizeof(uint32_t)))
        {
            return false;
        }
        uploadedLutHash = hash;
        hasUploadedLut = true;
        return true;
    }

    bool GpuFractalSolver::Impl::EnsureHistCdfUploaded(const float* histData)
    {
        if (!histData || !histCdfBuf) return false;
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < kHistogramCdfSize; ++i)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &histData[i], sizeof(uint32_t));
            hash ^= bits;
            hash *= 1099511628211ULL;
        }
        if (hasUploadedHist && hash == uploadedHistHash)
        {
            return true;
        }
        if (!UploadBufferDiscard(histCdfBuf.Get(), histData, kHistogramCdfSize * sizeof(float)))
        {
            return false;
        }
        uploadedHistHash = hash;
        hasUploadedHist = true;
        return true;
    }

    ID3D11ComputeShader* GpuFractalSolver::Impl::EnsureShader(GpuShaderId id)
    {
        auto& slot = shaders[static_cast<size_t>(id)];
        if (slot)
        {
            return slot.Get();
        }
        if (!device)
        {
            return nullptr;
        }
        ID3DBlob* blob = GetCompiledShaderBlob(id);
        if (!blob)
        {
            return nullptr;
        }
        if (FAILED(device->CreateComputeShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &slot)) || !slot)
        {
            CheckDeviceLost();
            slot.Reset();
            return nullptr;
        }
        return slot.Get();
    }

    bool GpuFractalSolver::Impl::SetPrimaryShader(GpuShaderMode mode)
    {
        GpuShaderId id = GpuShaderId::ShallowPrim;
        switch (mode)
        {
        case GpuShaderMode::Shallow:       id = GpuShaderId::ShallowPrim; break;
        case GpuShaderMode::PerturbLds:    id = GpuShaderId::LdsPrim; break;
        case GpuShaderMode::PerturbLdsBla: id = GpuShaderId::LdsBlaPrim; break;
        case GpuShaderMode::PerturbDeepExp:id = GpuShaderId::DeepExpPrim; break;
        }
        ID3D11ComputeShader* cs = EnsureShader(id);
        if (!cs) return false;
        context->CSSetShader(cs, nullptr, 0);
        return true;
    }

    bool GpuFractalSolver::Impl::SetSubpixelShader(GpuShaderMode mode)
    {
        GpuShaderId id = GpuShaderId::ShallowSub;
        switch (mode)
        {
        case GpuShaderMode::Shallow:       id = GpuShaderId::ShallowSub; break;
        case GpuShaderMode::PerturbLds:    id = GpuShaderId::LdsSub; break;
        case GpuShaderMode::PerturbLdsBla: id = GpuShaderId::LdsBlaSub; break;
        case GpuShaderMode::PerturbDeepExp:id = GpuShaderId::DeepExpSub; break;
        }
        ID3D11ComputeShader* cs = EnsureShader(id);
        if (!cs) return false;
        context->CSSetShader(cs, nullptr, 0);
        return true;
    }

    void GpuFractalSolver::Impl::UnbindComputeResources() noexcept
    {
        ID3D11UnorderedAccessView* nullUavs[5]{};
        context->CSSetUnorderedAccessViews(0, 5, nullUavs, nullptr);
        ID3D11ShaderResourceView* nullSrvs[9]{};
        context->CSSetShaderResources(0, 9, nullSrvs);
    }
}
