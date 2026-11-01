#pragma once
#include "TestHarness.h"
#include <filesystem>
#include <thread>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mf.lib")
namespace MoviePlayback
{
using Microsoft::WRL::ComPtr;
struct MovieNotify : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaEngineNotify>
{
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD, DWORD_PTR, DWORD) override { return S_OK; }
};
// Exercise the playback engine and its video renderer. A Source Reader's legacy
// RGB converter uses different range/matrix defaults and can hide playback bugs.
inline std::vector<uint32_t> DecodePlayerFrame(const std::filesystem::path& path, int width, int height)
{
    struct Runtime
    {
        HRESULT apartment{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
        bool started{};
        ~Runtime() { if (started) MFShutdown(); if (SUCCEEDED(apartment)) CoUninitialize(); }
    } runtime;
    ASSERT_TRUE(SUCCEEDED(runtime.apartment));
    runtime.started = SUCCEEDED(MFStartup(MF_VERSION)); ASSERT_TRUE(runtime.started);
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        ASSERT_EQ(S_OK, D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<IMFDXGIDeviceManager> manager; UINT token{};
        ASSERT_EQ(S_OK, MFCreateDXGIDeviceManager(&token, &manager));
        ASSERT_EQ(S_OK, manager->ResetDevice(device.Get(), token));
        ComPtr<IMFAttributes> attrs; ASSERT_EQ(S_OK, MFCreateAttributes(&attrs, 4));
        auto notify = Microsoft::WRL::Make<MovieNotify>();
        ASSERT_EQ(S_OK, attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get()));
        ASSERT_EQ(S_OK, attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager.Get()));
        ASSERT_EQ(S_OK, attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM));
        ComPtr<IMFMediaEngineClassFactory> factory;
        ASSERT_EQ(S_OK, CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        ComPtr<IMFMediaEngine> player; ASSERT_EQ(S_OK, factory->CreateInstance(0, attrs.Get(), &player));
        struct StopPlayer { IMFMediaEngine* player; ~StopPlayer() { player->Shutdown(); } } stop{player.Get()};
        std::unique_ptr<OLECHAR, decltype(&SysFreeString)> url(SysAllocString(path.c_str()), SysFreeString);
        ASSERT_TRUE(url != nullptr); ASSERT_EQ(S_OK, player->SetSource(url.get()));
        ASSERT_TRUE(SUCCEEDED(player->Play()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        LONGLONG tick{}; bool ready = false;
        while (!(ready = player->OnVideoStreamTick(&tick) == S_OK) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_TRUE(ready); ASSERT_TRUE(SUCCEEDED(player->Pause()));
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target; ASSERT_EQ(S_OK, device->CreateTexture2D(&desc, nullptr, &target));
        MFVideoNormalizedRect src{0, 0, 1, 1}; RECT dst{0, 0, width, height}; MFARGB border{};
        ASSERT_EQ(S_OK, player->TransferVideoFrame(target.Get(), &src, &dst, &border));
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback; ASSERT_EQ(S_OK, device->CreateTexture2D(&desc, nullptr, &readback));
        context->CopyResource(readback.Get(), target.Get()); D3D11_MAPPED_SUBRESOURCE data{};
        ASSERT_EQ(S_OK, context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &data));
        for (int y = 0; y < height; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * width,
                static_cast<BYTE*>(data.pData) + static_cast<size_t>(y) * data.RowPitch, width * 4);
        context->Unmap(readback.Get(), 0);
    }
    return pixels;
}
}
