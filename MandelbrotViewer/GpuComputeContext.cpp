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

        struct CompiledShaderCache
        {
            ComPtr<ID3DBlob> shallowPrim, shallowSub;
            ComPtr<ID3DBlob> ldsPrim, ldsSub;
            ComPtr<ID3DBlob> ldsBlaPrim, ldsBlaSub, ldsBlaStage0, ldsBlaResume;
            ComPtr<ID3DBlob> deepExpPrim, deepExpSub, deepExpStage0, deepExpResume;
            ComPtr<ID3DBlob> postDetect, postBuildIndirect, postBuildActiveIndirect, postResolve, postShutter;
            bool ok{ false };
        };

        const CompiledShaderCache& GetCompiledShaderCache()
        {
            static CompiledShaderCache cache;
            static std::once_flag flag;
            std::call_once(flag, []()
            {
                auto compileOne = [](const char* src, const char* entry, ComPtr<ID3DBlob>& outBlob) -> bool
                {
                    ComPtr<ID3DBlob> err;
                    const HRESULT hr = D3DCompile(
                        src,
                        std::strlen(src),
                        entry,
                        nullptr,
                        nullptr,
                        entry,
                        "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3,
                        0,
                        &outBlob,
                        &err);
                    return SUCCEEDED(hr) && outBlob != nullptr;
                };

                const char* shallowSrc = GetShallowShaderSource();
                const char* ldsSrc = GetPerturbLdsShaderSource();
                const char* ldsBlaSrc = GetPerturbLdsBlaShaderSource();
                const char* deepExpSrc = GetPerturbDeepExpShaderSource();
                const char* detectSrc = GetMovieDetectShaderSource();
                const char* resolveSrc = GetMovieResolveShaderSource();
                const char* shutterSrc = GetMovieShutterShaderSource();

                cache.ok =
                    compileOne(shallowSrc, "CSPrimary", cache.shallowPrim) &&
                    compileOne(shallowSrc, "CSSubpixels", cache.shallowSub) &&
                    compileOne(ldsSrc, "CSPrimary", cache.ldsPrim) &&
                    compileOne(ldsSrc, "CSSubpixels", cache.ldsSub) &&
                    compileOne(ldsBlaSrc, "CSPrimary", cache.ldsBlaPrim) &&
                    compileOne(ldsBlaSrc, "CSSubpixels", cache.ldsBlaSub) &&
                    compileOne(ldsBlaSrc, "CSPrimaryStage0", cache.ldsBlaStage0) &&
                    compileOne(ldsBlaSrc, "CSPrimaryResume", cache.ldsBlaResume) &&
                    compileOne(deepExpSrc, "CSPrimary", cache.deepExpPrim) &&
                    compileOne(deepExpSrc, "CSSubpixels", cache.deepExpSub) &&
                    compileOne(deepExpSrc, "CSPrimaryStage0", cache.deepExpStage0) &&
                    compileOne(deepExpSrc, "CSPrimaryResume", cache.deepExpResume) &&
                    compileOne(detectSrc, "CSDetectBoundaries", cache.postDetect) &&
                    compileOne(detectSrc, "CSBuildIndirectArgs", cache.postBuildIndirect) &&
                    compileOne(detectSrc, "CSBuildActiveIndirectArgs", cache.postBuildActiveIndirect) &&
                    compileOne(resolveSrc, "CSResolveAndBlend", cache.postResolve) &&
                    compileOne(shutterSrc, "CSShutterMotionBlur", cache.postShutter);
            });
            return cache;
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
        const auto& shaders = GetCompiledShaderCache();
        if (!shaders.ok) return;

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

        auto makeShader = [&](const ComPtr<ID3DBlob>& blob, ComPtr<ID3D11ComputeShader>& outCs) -> bool
        {
            return SUCCEEDED(device->CreateComputeShader(
                blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &outCs)) && outCs != nullptr;
        };

        if (!makeShader(shaders.shallowPrim, csShallowPrim) ||
            !makeShader(shaders.shallowSub, csShallowSub) ||
            !makeShader(shaders.ldsPrim, csLdsPrim) ||
            !makeShader(shaders.ldsSub, csLdsSub) ||
            !makeShader(shaders.ldsBlaPrim, csLdsBlaPrim) ||
            !makeShader(shaders.ldsBlaSub, csLdsBlaSub) ||
            !makeShader(shaders.ldsBlaStage0, csLdsBlaStage0) ||
            !makeShader(shaders.ldsBlaResume, csLdsBlaResume) ||
            !makeShader(shaders.deepExpPrim, csDeepExpPrim) ||
            !makeShader(shaders.deepExpSub, csDeepExpSub) ||
            !makeShader(shaders.deepExpStage0, csDeepExpStage0) ||
            !makeShader(shaders.deepExpResume, csDeepExpResume) ||
            !makeShader(shaders.postDetect, csPostDetect) ||
            !makeShader(shaders.postBuildIndirect, csPostBuildIndirect) ||
            !makeShader(shaders.postBuildActiveIndirect, csPostBuildActiveIndirect) ||
            !makeShader(shaders.postResolve, csPostResolve) ||
            !makeShader(shaders.postShutter, csPostShutter))
        {
            return;
        }

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = 128;
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

        available = true;
    }

    bool GpuFractalSolver::Impl::CanSupportConfig(const FractalConfig& config) const noexcept
    {
        if (!available || config.width <= 0 || config.height <= 0 || config.maxIterations < 0)
        {
            return false;
        }
        if (config.renderStride > 1 && config.width >= 8 && config.height >= 8)
        {
            return false;
        }

        if (config.juliaSet)
        {
            if (config.zoom >= GpuPerturbationThreshold) return false;
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

    bool GpuFractalSolver::Impl::EnsureTypedSrvBuffer(
        size_t neededElems,
        UINT elemBytes,
        DXGI_FORMAT format,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        if (neededElems <= capElems && buf && srv) return true;
        const size_t currentBytes = capElems * elemBytes;
        const size_t neededBytes = std::max<size_t>(1, neededElems) * elemBytes;
        if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
        const size_t allocElems = allocBytes / elemBytes;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(allocElems * elemBytes);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11ShaderResourceView> newSrv;
        if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;

        buf = std::move(newBuf);
        srv = std::move(newSrv);
        capElems = allocElems;
        return true;
    }

    bool GpuFractalSolver::Impl::EnsureStructuredSrvBuffer(
        size_t neededElems,
        UINT strideBytes,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        if (neededElems <= capElems && buf && srv) return true;
        const size_t currentBytes = capElems * strideBytes;
        const size_t neededBytes = std::max<size_t>(1, neededElems) * strideBytes;
        if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
        const size_t allocElems = allocBytes / strideBytes;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(allocElems * strideBytes);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = strideBytes;

        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11ShaderResourceView> newSrv;
        if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;

        buf = std::move(newBuf);
        srv = std::move(newSrv);
        capElems = allocElems;
        return true;
    }

    bool GpuFractalSolver::Impl::EnsureStructuredUavSrvBuffer(
        size_t neededElems,
        UINT strideBytes,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11UnorderedAccessView>& uav,
        ComPtr<ID3D11ShaderResourceView>& srv,
        size_t& capElems)
    {
        if (neededElems <= capElems && buf && uav && srv) return true;
        const size_t currentBytes = capElems * strideBytes;
        const size_t neededBytes = std::max<size_t>(1, neededElems) * strideBytes;
        if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
        const size_t allocElems = allocBytes / strideBytes;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(allocElems * strideBytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = strideBytes;

        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11UnorderedAccessView> newUav;
        if (FAILED(device->CreateUnorderedAccessView(newBuf.Get(), &uavDesc, &newUav))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11ShaderResourceView> newSrv;
        if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;

        buf = std::move(newBuf);
        uav = std::move(newUav);
        srv = std::move(newSrv);
        capElems = allocElems;
        return true;
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
        if (neededElems <= capElems && buf && uav && srv) return true;
        const size_t currentBytes = capElems * elemBytes;
        const size_t neededBytes = std::max<size_t>(1, neededElems) * elemBytes;
        if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
        const size_t allocElems = allocBytes / elemBytes;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(allocElems * elemBytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11UnorderedAccessView> newUav;
        if (FAILED(device->CreateUnorderedAccessView(newBuf.Get(), &uavDesc, &newUav))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11ShaderResourceView> newSrv;
        if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;

        buf = std::move(newBuf);
        uav = std::move(newUav);
        srv = std::move(newSrv);
        capElems = allocElems;
        return true;
    }

    bool GpuFractalSolver::Impl::EnsureStructuredUavBuffer(
        size_t neededElems,
        UINT strideBytes,
        ComPtr<ID3D11Buffer>& buf,
        ComPtr<ID3D11UnorderedAccessView>& uav,
        size_t& capElems)
    {
        if (neededElems <= capElems && buf && uav) return true;
        const size_t currentBytes = capElems * strideBytes;
        const size_t neededBytes = std::max<size_t>(1, neededElems) * strideBytes;
        if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
        const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
        const size_t allocElems = allocBytes / strideBytes;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(allocElems * strideBytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = strideBytes;

        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

        ComPtr<ID3D11UnorderedAccessView> newUav;
        if (FAILED(device->CreateUnorderedAccessView(newBuf.Get(), &uavDesc, &newUav))) return false;

        buf = std::move(newBuf);
        uav = std::move(newUav);
        capElems = allocElems;
        return true;
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

    bool GpuFractalSolver::Impl::UploadBufferDiscard(ID3D11Buffer* buf, const void* src, size_t bytes)
    {
        if (bytes == 0) return true;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
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

    void GpuFractalSolver::Impl::SetPrimaryShader(GpuShaderMode mode)
    {
        switch (mode)
        {
        case GpuShaderMode::Shallow:
            context->CSSetShader(csShallowPrim.Get(), nullptr, 0);
            break;
        case GpuShaderMode::PerturbLds:
            context->CSSetShader(csLdsPrim.Get(), nullptr, 0);
            break;
        case GpuShaderMode::PerturbLdsBla:
            context->CSSetShader(csLdsBlaPrim.Get(), nullptr, 0);
            break;
        case GpuShaderMode::PerturbDeepExp:
            context->CSSetShader(csDeepExpPrim.Get(), nullptr, 0);
            break;
        }
    }

    void GpuFractalSolver::Impl::SetSubpixelShader(GpuShaderMode mode)
    {
        switch (mode)
        {
        case GpuShaderMode::Shallow:
            context->CSSetShader(csShallowSub.Get(), nullptr, 0);
            break;
        case GpuShaderMode::PerturbLds:
            context->CSSetShader(csLdsSub.Get(), nullptr, 0);
            break;
        case GpuShaderMode::PerturbLdsBla:
            context->CSSetShader(csLdsBlaSub.Get(), nullptr, 0);
            break;
        case GpuShaderMode::PerturbDeepExp:
            context->CSSetShader(csDeepExpSub.Get(), nullptr, 0);
            break;
        }
    }

    void GpuFractalSolver::Impl::UnbindComputeResources() noexcept
    {
        ID3D11UnorderedAccessView* nullUavs[5]{};
        context->CSSetUnorderedAccessViews(0, 5, nullUavs, nullptr);
        ID3D11ShaderResourceView* nullSrvs[8]{};
        context->CSSetShaderResources(0, 8, nullSrvs);
    }
}
