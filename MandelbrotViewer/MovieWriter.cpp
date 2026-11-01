#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MovieWriter.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace Mandelbrot
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        void Check(HRESULT hr)
        {
            if (FAILED(hr))
            {
                std::ostringstream text;
                text << "Movie image encoding failed (0x" << std::hex << static_cast<uint32_t>(hr) << ").";
                throw std::runtime_error(text.str());
            }
        }
        struct Apartment
        {
            HRESULT status{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
            Apartment() { if (status != RPC_E_CHANGED_MODE) Check(status); }
            ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
        };
    }

    struct MovieWriter::Impl
    {
        Apartment apartment;
        ComPtr<IWICImagingFactory> factory;
        std::fstream file;
        int width, height, fps;
        uint64_t media{};
        bool finished{};
        struct Entry { uint64_t offset; uint32_t bytes; };
        std::vector<Entry> entries;

        uint64_t Pos() { return static_cast<uint64_t>(file.tellp()); }
        void Bytes(const void* data, size_t n) { file.write(static_cast<const char*>(data), static_cast<std::streamsize>(n)); }
        void Four(const char* four) { Bytes(four, 4); }
        void U32(uint32_t n) { const uint8_t b[]{static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)}; Bytes(b, 4); }
        void U16(uint16_t n) { const uint8_t b[]{static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)}; Bytes(b, 2); }
        void U64(uint64_t n) { U32(static_cast<uint32_t>(n >> 32)); U32(static_cast<uint32_t>(n)); }
        void Zero(size_t n) { std::vector<char> zeros(n); Bytes(zeros.data(), n); }
        void Patch(uint64_t pos, uint32_t n) { const auto end = Pos(); file.seekp(pos); U32(n); file.seekp(end); }
        uint64_t Box(const char* kind) { const auto p = Pos(); U32(0); Four(kind); return p; }
        void End(uint64_t p) { Patch(p, static_cast<uint32_t>(Pos() - p)); }
        void Matrix() { U32(1 << 16); Zero(12); U32(1 << 16); Zero(12); U32(1 << 30); }
        // Windows' JPEG video path assumes the HD matrix at HD sizes. Match it and
        // describe the same matrix explicitly in the MP4 color information box.
        bool HD() const { return width > 1024 || height > 576; }

        Impl(const std::wstring& path, int w, int h, int rate)
            : width(w), height(h), fps(rate)
        {
            if (w < 1 || h < 1 || w > 7680 || h > 4320 || rate < 1 || rate > 120)
                throw std::invalid_argument("Invalid movie writer dimensions or frame rate.");
            Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
            const auto ftyp = Box("ftyp"); Four("isom"); U32(512); Four("isom"); Four("mp42"); End(ftyp);
            media = Pos(); U32(1); Four("mdat"); U64(0); // Extended size supports files above 4 GB.
        }

        std::vector<uint8_t> Encode(const std::vector<uint32_t>& pixels)
        {
            ComPtr<IStream> stream;
            Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
            ComPtr<IWICBitmapEncoder> encoder;
            Check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
            Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> properties;
            Check(encoder->CreateNewFrame(&frame, &properties));
            PROPBAG2 options[3]{};
            options[0].pstrName = const_cast<wchar_t*>(L"ImageQuality");
            options[1].pstrName = const_cast<wchar_t*>(L"JpegYCrCbSubsampling");
            options[2].pstrName = const_cast<wchar_t*>(L"SuppressApp0");
            VARIANT values[3]{};
            values[0].vt = VT_R4; values[0].fltVal = 0.98f;
            values[1].vt = VT_UI1; values[1].bVal = WICJpegYCrCbSubsampling422;
            values[2].vt = VT_BOOL; values[2].boolVal = VARIANT_TRUE;
            Check(properties->Write(3, options, values));
            Check(frame->Initialize(properties.Get()));
            Check(frame->SetSize(width, height));
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            Check(frame->SetPixelFormat(&format));
            if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("JPEG encoder does not support full-color movie frames.");
            ComPtr<IWICPlanarBitmapFrameEncode> planar; Check(frame.As(&planar));
            const int chromaWidth = (width + 1) / 2;
            std::vector<uint8_t> luma(width), cb(chromaWidth), cr(chromaWidth);
            const double kr = HD() ? .2126 : .299, kb = HD() ? .0722 : .114;
            const auto byte = [](double value) { return static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l)); };
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; x += 2)
                {
                    double blueDifference = 0, redDifference = 0;
                    for (int j = 0; j < 2; ++j)
                    {
                        const int sampleX = std::min(x + j, width - 1);
                        const auto color = pixels[static_cast<size_t>(y) * width + sampleX];
                        const double b = color & 255, g = (color >> 8) & 255, r = (color >> 16) & 255;
                        const double value = kr * r + (1 - kr - kb) * g + kb * b;
                        luma[sampleX] = byte(value);
                        blueDifference += (b - value) / (2 * (1 - kb));
                        redDifference += (r - value) / (2 * (1 - kr));
                    }
                    cb[x / 2] = byte(128 + blueDifference / 2);
                    cr[x / 2] = byte(128 + redDifference / 2);
                }
                WICBitmapPlane planes[]{
                    {GUID_WICPixelFormat8bppY, luma.data(), static_cast<UINT>(width), static_cast<UINT>(luma.size())},
                    {GUID_WICPixelFormat8bppCb, cb.data(), static_cast<UINT>(chromaWidth), static_cast<UINT>(cb.size())},
                    {GUID_WICPixelFormat8bppCr, cr.data(), static_cast<UINT>(chromaWidth), static_cast<UINT>(cr.size())}};
                Check(planar->WritePixels(1, planes, 3));
            }
            Check(frame->Commit()); Check(encoder->Commit());
            STATSTG stat{}; Check(stream->Stat(&stat, STATFLAG_NONAME));
            if (stat.cbSize.QuadPart >= 0x80000000ull) throw std::runtime_error("Encoded movie frame is too large.");
            std::vector<uint8_t> jpeg(static_cast<size_t>(stat.cbSize.QuadPart));
            LARGE_INTEGER zero{}; Check(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
            ULONG read{}; Check(stream->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read));
            if (read != jpeg.size()) throw std::runtime_error("Could not read the complete encoded frame.");
            return jpeg;
        }

        void Write(const std::vector<uint32_t>& pixels)
        {
            if (finished || pixels.size() != static_cast<size_t>(width) * height)
                throw std::invalid_argument("Movie frame dimensions do not match the stream.");
            if (entries.size() == 3600u * 120u) throw std::runtime_error("Movie exceeds the supported duration.");
            auto jpeg = Encode(pixels);
            entries.push_back({Pos(), static_cast<uint32_t>(jpeg.size())}); Bytes(jpeg.data(), jpeg.size());
        }

        void Finish()
        {
            if (finished) return;
            if (entries.empty()) throw std::runtime_error("Cannot save a movie with no frames.");
            const auto end = Pos(); file.seekp(media + 8); U64(end - media); file.seekp(end);
            const auto count = static_cast<uint32_t>(entries.size());
            const auto moov = Box("moov");
            const auto mvhd = Box("mvhd"); U32(0); Zero(8); U32(fps); U32(count); U32(1 << 16); U16(0x100); Zero(10); Matrix(); Zero(24); U32(2); End(mvhd);
            const auto trak = Box("trak");
            const auto tkhd = Box("tkhd"); U32(3); Zero(8); U32(1); U32(0); U32(count); Zero(16); Matrix(); U32(width << 16); U32(height << 16); End(tkhd);
            const auto mdia = Box("mdia");
            const auto mdhd = Box("mdhd"); U32(0); Zero(8); U32(fps); U32(count); U16(0x55c4); U16(0); End(mdhd);
            const auto hdlr = Box("hdlr"); U32(0); U32(0); Four("vide"); Zero(12); Bytes("Fractal\0", 8); End(hdlr);
            const auto minf = Box("minf");
            const auto vmhd = Box("vmhd"); U32(1); Zero(8); End(vmhd);
            const auto dinf = Box("dinf"); const auto dref = Box("dref"); U32(0); U32(1);
            const auto url = Box("url "); U32(1); End(url); End(dref); End(dinf);
            const auto stbl = Box("stbl"); const auto stsd = Box("stsd"); U32(0); U32(1);
            const auto sample = Box("jpeg"); Zero(6); U16(1); Zero(16); U16(static_cast<uint16_t>(width)); U16(static_cast<uint16_t>(height));
            U32(72 << 16); U32(72 << 16); U32(0); U16(1); Zero(32); U16(24); U16(0xffff);
            const auto colr = Box("colr"); Four("nclx"); U16(1); U16(13); U16(HD() ? 1 : 6); Bytes("\x80", 1); End(colr); // sRGB transfer, full range.
            End(sample); End(stsd);
            const auto stts = Box("stts"); U32(0); U32(1); U32(count); U32(1); End(stts);
            const auto stsc = Box("stsc"); U32(0); U32(1); U32(1); U32(1); U32(1); End(stsc);
            const auto stsz = Box("stsz"); U32(0); U32(0); U32(count); for (const auto& e : entries) U32(e.bytes); End(stsz);
            const auto co64 = Box("co64"); U32(0); U32(count); for (const auto& e : entries) U64(e.offset); End(co64);
            End(stbl); End(minf); End(mdia); End(trak); End(moov);
            file.flush(); file.close(); finished = true;
        }
    };

    MovieWriter::MovieWriter(const std::wstring& path, int width, int height, int fps)
        : m_impl(std::make_unique<Impl>(path, width, height, fps)) {}
    MovieWriter::~MovieWriter() = default;
    void MovieWriter::WriteFrame(const std::vector<uint32_t>& bgra) { m_impl->Write(bgra); }
    void MovieWriter::Finish() { m_impl->Finish(); }
}
