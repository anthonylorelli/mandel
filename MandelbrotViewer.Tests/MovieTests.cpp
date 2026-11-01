#include "pch.h"
#include "TestHarness.h"
#include "MoviePlan.h"
#include "MovieWriter.h"
#include "MovieExport.h"
#include <filesystem>
#include <fstream>
#include <wincodec.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace Mandelbrot;
using Microsoft::WRL::ComPtr;
#include "MoviePlayback.h"
using MoviePlayback::DecodePlayerFrame;

namespace
{
    MoviePlan SmallMovie()
    {
        MoviePlan p; p.width = 32; p.height = 24; p.seconds = 1; p.fps = 24;
        p.start.SetCenter(-0.75, 0); p.start.zoom = 1; p.start.maxIterations = 128;
        p.targetX = BigFloat(-0.73); p.targetY = BigFloat(0.1); p.endZoom = 4;
        p.motionSmoothing = false; return p;
    }
    struct File
    {
        std::filesystem::path path;
        File()
        {
            static std::atomic<uint64_t> next{};
            path = std::filesystem::temp_directory_path() / (L"mandel-movie-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++next) + L".mp4");
        }
        ~File() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    };
    std::vector<uint8_t> Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    uint32_t U32(const std::vector<uint8_t>& b, size_t at)
    {
        ASSERT_LE(at + 4, b.size()); return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3];
    }
    void WaitFor(MovieExporter& exporter)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!exporter.Progress().finished && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ASSERT_TRUE(exporter.Progress().finished);
    }

    void DumpBitmap(const wchar_t* name, int width, int height, const std::vector<uint32_t>& pixels)
    {
        std::filesystem::create_directories(L"ARM64/MovieDiagnostics");
        std::ofstream file(std::filesystem::path(L"ARM64/MovieDiagnostics") / name, std::ios::binary);
        BITMAPFILEHEADER header{}; header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * 4);
        BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header)); file.write(reinterpret_cast<const char*>(&info), sizeof(info));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 4));
    }

    double FilteredPixelError(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int width, int height)
    {
        double squared = 0;
        for (int y = 1; y < height - 1; ++y) for (int x = 1; x < width - 1; ++x)
            for (int shift : {0, 8, 16})
            {
                double delta = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                {
                    const size_t i = static_cast<size_t>(y + dy) * width + x + dx;
                    delta += static_cast<int>((a[i] >> shift) & 255) - static_cast<int>((b[i] >> shift) & 255);
                }
                squared += delta * delta / 81;
            }
        return std::sqrt(squared / ((width - 2.0) * (height - 2.0) * 3));
    }
}

TEST_CASE(Movie, CameraUsesExactEndpointsAndAnUnchangingIterationBudget)
{
    auto p = SmallMovie(); p.Validate();
    const auto first = p.CameraAt(0), last = p.CameraAt(p.FrameCount() - 1);
    ASSERT_TRUE(first.hpCenterX == p.start.hpCenterX); ASSERT_EQ(p.start.zoom, first.zoom);
    ASSERT_TRUE(last.hpCenterX == p.targetX); ASSERT_TRUE(last.hpCenterY == p.targetY); ASSERT_EQ(p.endZoom, last.zoom);
    std::vector<std::pair<int, MandelbrotConfig>> cameras;
    for (int i : {17, 3, 22, 0, 11})
    {
        const auto camera = p.CameraAt(i);
        ASSERT_EQ(128, camera.maxIterations); ASSERT_EQ(1, camera.renderStride);
        cameras.emplace_back(i, camera);
    }
    for (auto i = cameras.rbegin(); i != cameras.rend(); ++i)
    { const auto camera = p.CameraAt(i->first); ASSERT_TRUE(camera.hpCenterX == i->second.hpCenterX); ASSERT_EQ(i->second.zoom, camera.zoom); }
    ASSERT_NEAR(std::sqrt(p.start.zoom * p.endZoom), p.CameraAt((p.FrameCount() - 1) * .5).zoom, 1e-12);
}

TEST_CASE(Movie, CameraRetainsTinyDisplacementsAtExtremeZoomInBothDirections)
{
    auto p = SmallMovie(); p.endZoom = 1e200; p.targetX = BigFloat(-0.75); p.targetY = BigFloat(0.1);
    const auto penultimate = p.CameraAt(p.FrameCount() - 2);
    ASSERT_FALSE((penultimate.hpCenterY - p.targetY).IsZero());
    ASSERT_EQ(p.targetY.ToDouble(), penultimate.centerY);
    p.start.SetCenter(p.targetX, p.targetY); p.start.zoom = 1e200; p.endZoom = 1;
    p.targetY = BigFloat(0.2);
    const auto second = p.CameraAt(1);
    ASSERT_FALSE((second.hpCenterY - p.start.hpCenterY).IsZero());
    ASSERT_EQ(p.start.centerY, second.centerY);
}

TEST_CASE(Movie, ResolutionChoicesIncludeEightKAndValidateLimits)
{
    ASSERT_EQ(7680, MovieResolutions.back().width); ASSERT_EQ(4320, MovieResolutions.back().height);
    for (const auto& r : MovieResolutions) for (int fps : MovieFrameRates)
    { auto p = SmallMovie(); p.width = r.width; p.height = r.height; p.fps = fps; p.Validate(); ASSERT_EQ(static_cast<uint32_t>(fps), p.FrameCount()); }
    auto p = SmallMovie(); p.endZoom = std::numeric_limits<double>::infinity();
    bool rejected = false; try { p.Validate(); } catch (const std::invalid_argument&) { rejected = true; } ASSERT_TRUE(rejected);
}

TEST_CASE(Movie, FixedSamplingIsRepeatableAndIndependentOfRenderBands)
{
    const auto p = SmallMovie(); std::atomic<bool> cancel{};
    const auto first = RenderMovieFrame(p, 4, cancel, {}, 4);
    ASSERT_TRUE(first == RenderMovieFrame(p, 4, cancel, {}, 4));
    const auto whole = RenderMovieFrame(p, 4, cancel, {}, p.height);
    ASSERT_EQ(first.size(), whole.size());
    for (size_t i = 0; i < first.size(); ++i)
        for (int shift : {0, 8, 16}) ASSERT_NEAR((first[i] >> shift) & 255, (whole[i] >> shift) & 255, 1);
    cancel = true; ASSERT_TRUE(RenderMovieFrame(p, 4, cancel).empty());
}

TEST_CASE(Movie, SupersamplingRetainsDeepSubpixelBoundaries)
{
    auto p = SmallMovie(); p.width = 8; p.height = 4; p.start.SetCenter(1.0, 0.0);
    p.start.juliaSet = true; p.start.juliaX = BigFloat(0.0); p.start.juliaY = BigFloat(0.0);
    p.start.zoom = 1e25; p.endZoom = 2e25; p.targetX = BigFloat(1.0); p.targetY = BigFloat(0.0);
    p.start.maxIterations = 300;
    std::atomic<bool> cancel{}; const auto pixels = RenderMovieFrame(p, 0, cancel, {}, 2);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            ASSERT_EQ(x < p.width / 2, pixels[y * p.width + x] == p.interior);
}

TEST_CASE(Movie, Mp4SampleTablesPreserveFramesAndTiming)
{
    File file;
    const std::vector<uint32_t> red(32 * 24, 0xFFFF2020), blue(32 * 24, 0xFF2020FF);
    { MovieWriter writer(file.path.wstring(), 32, 24, 24); writer.WriteFrame(red); writer.WriteFrame(blue); writer.WriteFrame(red); writer.Finish(); }
    const auto bytes = Read(file.path);
    ASSERT_GE(bytes.size(), 8u);
    ASSERT_TRUE(std::memcmp(bytes.data() + 4, "ftyp", 4) == 0);
    const auto media = U32(bytes, 0);
    ASSERT_EQ(1u, U32(bytes, media)); // 64-bit mdat size, even for small movies.
    ASSERT_TRUE(std::memcmp(bytes.data() + media + 4, "mdat", 4) == 0);
    const auto table = std::search(bytes.begin(), bytes.end(), "co64", "co64" + 4);
    ASSERT_TRUE(table != bytes.end()); const auto at = static_cast<size_t>(table - bytes.begin());
    ASSERT_EQ(3u, U32(bytes, at + 8));
    for (int i = 0; i < 3; ++i)
    {
        const uint64_t offset = (static_cast<uint64_t>(U32(bytes, at + 12 + i * 8)) << 32) | U32(bytes, at + 16 + i * 8);
        ASSERT_LT(offset + 2, bytes.size()); ASSERT_EQ(255, bytes[offset]); ASSERT_EQ(216, bytes[offset + 1]);
    }
    // Independent Windows media reader verifies frame count, timing, and decodability.
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED); ASSERT_TRUE(SUCCEEDED(apartment));
    ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));
    {
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFAttributes> attributes; MFCreateAttributes(&attributes, 1);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(file.path.c_str(), attributes.Get(), &reader)));
        const DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        int frames = 0;
        for (;;)
        {
            DWORD flags{}; LONGLONG timestamp{}; ComPtr<IMFSample> sample;
            ASSERT_TRUE(SUCCEEDED(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, &sample)));
            if (sample)
            {
                ASSERT_NEAR(frames * 10000000.0 / 24, timestamp, 2);
                ++frames;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        ASSERT_EQ(3, frames);
    }
    MFShutdown(); CoUninitialize();
}

TEST_CASE(Movie, PlaybackPreservesFractalGeometryAndColors)
{
    auto p = SmallMovie(); p.width = 320; p.height = 180;
    p.start.SetCenter(-0.75, 0.12); // Asymmetry also catches flipped or shifted decoded rows.
    p.targetX = p.start.hpCenterX; p.targetY = p.start.hpCenterY; p.endZoom = p.start.zoom;
    std::atomic<bool> cancel{}; const auto expected = RenderMovieFrame(p, 0, cancel);
    File file; { MovieWriter writer(file.path.wstring(), p.width, p.height, p.fps); writer.WriteFrame(expected); writer.Finish(); }
    const auto playback = DecodePlayerFrame(file.path, p.width, p.height);
    const double error = FilteredPixelError(expected, playback, p.width, p.height);
    if (error >= 5.0)
    {
        DumpBitmap(L"expected.bmp", p.width, p.height, expected);
        DumpBitmap(L"playback.bmp", p.width, p.height, playback);
    }
    // 4:2:2 averages neighboring chroma samples. Compare the image after a small
    // footprint filter so normal color-edge reconstruction is allowed, while broad
    // ghost shapes, hue/range errors, and displaced fractal geometry still fail.
    ASSERT_LT(error, 5.0);
}

TEST_CASE(Movie, PlaybackPreservesColorPatchesThroughEightK)
{
    const std::array<uint32_t, 8> colors{0xFF000000, 0xFFFFFFFF, 0xFFFF2020, 0xFF20FF20,
        0xFF2020FF, 0xFF808080, 0xFF65A0D0, 0xFFCE5448};
    for (const auto& resolution : MovieResolutions)
    {
        const int width = resolution.width, height = resolution.height;
        std::vector<uint32_t> expected(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            expected[static_cast<size_t>(y) * width + x] = colors[(x * 8 / width + (y >= height / 2 ? 4 : 0)) % 8];
        File file; { MovieWriter writer(file.path.wstring(), width, height, 30); writer.WriteFrame(expected); writer.Finish(); }
        const auto bytes = Read(file.path);
        const auto colorBox = std::search(bytes.begin(), bytes.end(), "colrnclx", "colrnclx" + 8);
        ASSERT_TRUE(colorBox != bytes.end());
        const auto colorAt = static_cast<size_t>(colorBox - bytes.begin());
        ASSERT_LE(colorAt + 15, bytes.size());
        ASSERT_EQ(1, bytes[colorAt + 9]); // BT.709/sRGB primaries.
        ASSERT_EQ(13, bytes[colorAt + 11]); // sRGB transfer function.
        ASSERT_EQ(1, bytes[colorAt + 13]); // BT.709 YCbCr matrix.
        ASSERT_EQ(0x80, bytes[colorAt + 14]); // Full range, including true black/white.
        const auto playback = DecodePlayerFrame(file.path, width, height);
        for (int y : {height / 4, 3 * height / 4}) for (int patch = 0; patch < 8; ++patch)
        {
            const size_t i = static_cast<size_t>(y) * width + (2 * patch + 1) * width / 16;
            for (int shift : {0, 8, 16}) ASSERT_NEAR((expected[i] >> shift) & 255, (playback[i] >> shift) & 255, 4);
        }
    }
}

TEST_CASE(Movie, JpegWriterSupportsEightKFrames)
{
    File file; std::vector<uint32_t> pixels(static_cast<size_t>(7680) * 4320, 0xFF27496D);
    { MovieWriter writer(file.path.wstring(), 7680, 4320, 60); writer.WriteFrame(pixels); writer.Finish(); }
    const auto bytes = Read(file.path);
    // Decode the JPEG payload independently of the MP4 writer (the first JPEG SOI).
    const std::array<uint8_t, 3> soi{255, 216, 255};
    const auto jpeg = std::search(bytes.begin(), bytes.end(), soi.begin(), soi.end()); ASSERT_TRUE(jpeg != bytes.end());
    ASSERT_TRUE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    {
        ComPtr<IWICImagingFactory> factory; ASSERT_TRUE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
        ComPtr<IWICStream> stream; factory->CreateStream(&stream);
        ASSERT_TRUE(SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(&*jpeg), static_cast<DWORD>(bytes.end() - jpeg))));
        ComPtr<IWICBitmapDecoder> decoder; ASSERT_TRUE(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)));
        ComPtr<IWICBitmapFrameDecode> frame; ASSERT_TRUE(SUCCEEDED(decoder->GetFrame(0, &frame)));
        UINT width{}, height{}; frame->GetSize(&width, &height); ASSERT_EQ(7680u, width); ASSERT_EQ(4320u, height);
    }
    CoUninitialize();
}

TEST_CASE(Movie, FullEightKRenderAndPreviewPreserveUniformRegions)
{
    auto p = SmallMovie(); p.width = 7680; p.height = 4320;
    p.start.SetCenter(0.0, 0.0); p.start.zoom = 10000; p.endZoom = 10000;
    p.targetX = BigFloat(0.0); p.targetY = BigFloat(0.0); p.motionSmoothing = true;
    std::atomic<bool> cancel{}; float completed = 0;
    const auto pixels = RenderMovieFrame(p, 0, cancel, [&](float progress)
    { ASSERT_GE(progress, completed); completed = progress; });
    ASSERT_EQ(static_cast<size_t>(7680) * 4320, pixels.size()); ASSERT_EQ(1.0f, completed);
    ASSERT_TRUE(std::all_of(pixels.begin(), pixels.end(), [&](auto color) { return color == p.interior; }));
    const auto preview = MakeMoviePreview(pixels, p.width, p.height, 1);
    ASSERT_EQ(960, preview.width); ASSERT_EQ(540, preview.height);
    ASSERT_TRUE(std::all_of(preview.pixels.begin(), preview.pixels.end(), [&](auto color) { return color == p.interior; }));
}

TEST_CASE(Movie, BackgroundExportReportsFootageAndPublishesTheLastPreview)
{
    File file; const auto p = SmallMovie(); MovieExporter exporter(p, file.path.wstring()); WaitFor(exporter);
    const auto status = exporter.Progress(); ASSERT_TRUE(status.error.empty()); ASSERT_FALSE(status.canceled);
    ASSERT_EQ(p.FrameCount(), status.completed);
    const auto preview = exporter.TakePreview(); ASSERT_TRUE(preview.has_value()); ASSERT_EQ(p.FrameCount(), preview->frame);
    ASSERT_FALSE(Read(file.path).empty());
}

TEST_CASE(Movie, CancelPreservesAnExistingDestination)
{
    File file; { std::ofstream stream(file.path); stream << "original"; }
    auto p = SmallMovie(); p.width = 1920; p.height = 1080; p.start.maxIterations = 10000;
    MovieExporter exporter(p, file.path.wstring()); exporter.Cancel(); WaitFor(exporter);
    ASSERT_TRUE(exporter.Progress().canceled);
    const auto bytes = Read(file.path); ASSERT_EQ(std::string("original"), std::string(bytes.begin(), bytes.end()));
}
