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

TEST_CASE(Movie, ZoomMotionComputesEndZoomFromDirectionSpeedAndDuration)
{
    auto p = SmallMovie(); p.start.zoom = 3.0; p.seconds = 4;
    p.SetZoomMotion(true, 1.5);
    ASSERT_EQ(3.0 * 64.0, p.endZoom);
    ASSERT_EQ(p.endZoom, p.CameraAt(p.FrameCount() - 1).zoom);

    p.start.zoom = 192.0; p.seconds = 3;
    p.SetZoomMotion(false, 2.0);
    ASSERT_EQ(3.0, p.endZoom);

    ASSERT_EQ(MandelbrotEngine::MaxZoom, MoviePlan::EndZoomForSpeed(1.0, true, MaxMovieZoomSpeed, 3600));
    ASSERT_EQ(MandelbrotEngine::MinZoom, MoviePlan::EndZoomForSpeed(1.0, false, MaxMovieZoomSpeed, 3600));

    for (double badSpeed : {0.0, 0.1, 5.1, std::numeric_limits<double>::quiet_NaN()})
    {
        bool rejected = false;
        try { MoviePlan::EndZoomForSpeed(1.0, true, badSpeed, 10); }
        catch (const std::invalid_argument&) { rejected = true; }
        ASSERT_TRUE(rejected);
    }
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

    auto deep = SmallMovie(); deep.start.SetCenter(-0.7436438870371587, 0.1318259042053119);
    deep.targetX = deep.start.hpCenterX; deep.targetY = deep.start.hpCenterY;
    deep.start.zoom = 1e15; deep.endZoom = 2e15; deep.start.maxIterations = 600;
    MandelbrotEngine shared;
    const auto deepFrame0 = RenderMovieFrame(deep, 0, cancel, {}, 6, &shared);
    const auto deepFrame1 = RenderMovieFrame(deep, 1, cancel, {}, 6, &shared);
    const auto deepWhole1 = RenderMovieFrame(deep, 1, cancel, {}, deep.height);
    ASSERT_EQ(deepFrame0.size(), deepFrame1.size());
    for (size_t i = 0; i < deepFrame1.size(); ++i)
        for (int shift : {0, 8, 16}) ASSERT_NEAR((deepFrame1[i] >> shift) & 255, (deepWhole1[i] >> shift) & 255, 1);

    cancel = true; ASSERT_TRUE(RenderMovieFrame(p, 4, cancel).empty());
}

TEST_CASE(Movie, AutomaticTargetsAreRepeatableAcrossFrameRatesAndSubframeRequests)
{
    auto p = SmallMovie(); p.autoRetarget = true; p.targetSeed = 101; p.seconds = 3; p.endZoom = 1e6;
    std::atomic<bool> cancel{}; ASSERT_TRUE(p.PrepareAutoTargets(cancel));
    auto other = p; other.fps = 60; other.width = 1280; other.height = 960;
    ASSERT_TRUE(other.PrepareAutoTargets(cancel));
    ASSERT_TRUE(p.CameraAt(0).hpCenterX == p.start.hpCenterX);
    ASSERT_TRUE(p.CameraAt(0).hpCenterY == p.start.hpCenterY);
    ASSERT_EQ(p.endZoom, p.CameraAt(p.FrameCount() - 1).zoom);
    for (double t : {1.0, .25, .75, 0.0, .5, .25})
    {
        const auto a = p.CameraAt((p.FrameCount() - 1) * t);
        const auto b = other.CameraAt((other.FrameCount() - 1) * t);
        ASSERT_TRUE(a.hpCenterX == b.hpCenterX); ASSERT_TRUE(a.hpCenterY == b.hpCenterY);
        ASSERT_EQ(a.zoom, b.zoom); ASSERT_EQ(p.start.maxIterations, a.maxIterations);
    }
    const auto frame = RenderMovieFrame(p, 12.25, cancel, {}, 4);
    ASSERT_TRUE(frame == RenderMovieFrame(p, 12.25, cancel, {}, 4));
    const auto whole = RenderMovieFrame(p, 12.25, cancel, {}, p.height);
    for (size_t i = 0; i < frame.size(); ++i) for (int shift : {0, 8, 16})
        ASSERT_NEAR((frame[i] >> shift) & 255, (whole[i] >> shift) & 255, 1);
}

TEST_CASE(Movie, AutomaticRetargetsPreservePositionAndVelocity)
{
    auto p = SmallMovie(); p.autoRetarget = true; p.targetSeed = 101; p.seconds = 8; p.endZoom = 1e12;
    std::atomic<bool> cancel{}; ASSERT_TRUE(p.PrepareAutoTargets(cancel));
    ASSERT_GT(p.AutoTargets().size(), 1u);
    const double range = std::log(p.endZoom / p.start.zoom), epsilon = 1e-7;
    for (const auto& target : p.AutoTargets())
    {
        if (target.progress == 0) continue;
        const double frame = (p.FrameCount() - 1) * target.progress;
        const double dt = (p.FrameCount() - 1) * epsilon / range;
        const auto at = p.CameraAt(frame), left = p.CameraAt(frame - dt), right = p.CameraAt(frame + dt);
        const double span = 3.0 / at.zoom;
        ASSERT_LT(std::abs((at.hpCenterX - target.x).ToDouble()) / span, 1e-10);
        ASSERT_LT(std::abs((at.hpCenterY - target.y).ToDouble()) / span, 1e-10);
        for (auto values : {std::array<BigFloat, 3>{left.hpCenterX, at.hpCenterX, right.hpCenterX},
            std::array<BigFloat, 3>{left.hpCenterY, at.hpCenterY, right.hpCenterY}})
        {
            const double before = (values[1] - values[0]).ToDouble() / epsilon;
            const double after = (values[2] - values[1]).ToDouble() / epsilon;
            ASSERT_LT(std::abs(before - after) / span, 1e-4);
        }
    }
}

TEST_CASE(Movie, AutomaticTargetsUseLiveInterestChecksAcrossFractalTypes)
{
    std::atomic<bool> cancel{}; MandelbrotEngine engine;
    for (auto type : GetAllFractalTypes())
    {
        auto p = SmallMovie(); p.autoRetarget = true; p.targetSeed = 101; p.start.fractalType = type;
        const auto& overview = GetBookmarks(type).front();
        p.start.SetCenter(overview.centerX, overview.centerY); p.start.zoom = overview.zoom; p.endZoom = p.start.zoom * 100;
        ASSERT_TRUE(p.PrepareAutoTargets(cancel)); ASSERT_FALSE(p.AutoTargets().empty());
        auto probe = p.CameraAt(0); probe.width = 144; probe.height = 108;
        std::vector<float> iterations; engine.Compute(probe, iterations);
        const auto& selected = p.AutoTargets().front();
        ASSERT_FALSE(MandelbrotEngine::ShouldRetargetZoom(probe, iterations, selected.targetX, selected.targetY));
    }
}

TEST_CASE(Movie, AutomaticZoomRetainsDeepCoordinatesAndAvoidsDistantFallbackJumps)
{
    auto p = SmallMovie(); p.autoRetarget = true; p.targetSeed = 101;
    p.start.SetCenter(0.0, 1.0); p.start.zoom = 1e25; p.endZoom = 1e28;
    std::atomic<bool> cancel{}; ASSERT_TRUE(p.PrepareAutoTargets(cancel));
    const auto next = p.CameraAt(5.25);
    ASSERT_EQ(p.start.centerY, next.centerY);
    ASSERT_FALSE((next.hpCenterY - p.start.hpCenterY).IsZero());
    ASSERT_LT(std::abs((next.hpCenterY - p.start.hpCenterY).ToDouble()), 3.0 / p.start.zoom);

    p.start.SetCenter(0.0, 0.0); p.start.zoom = 1e25;
    ASSERT_TRUE(p.PrepareAutoTargets(cancel));
    const auto last = p.CameraAt(p.FrameCount() - 1);
    ASSERT_TRUE(last.hpCenterX == p.start.hpCenterX); ASSERT_TRUE(last.hpCenterY == p.start.hpCenterY);
}

TEST_CASE(Movie, AutomaticTargetPlanningIsCancelableAndRequiresZoomIn)
{
    auto p = SmallMovie(); p.autoRetarget = true;
    std::atomic<bool> cancel{true}; ASSERT_FALSE(p.PrepareAutoTargets(cancel)); ASSERT_TRUE(p.AutoTargets().empty());
    cancel = false; p.endZoom = p.start.zoom;
    bool rejected = false; try { p.PrepareAutoTargets(cancel); } catch (const std::invalid_argument&) { rejected = true; }
    ASSERT_TRUE(rejected);
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
    const auto sizes = std::search(bytes.begin(), bytes.end(), "stsz", "stsz" + 4);
    ASSERT_TRUE(sizes != bytes.end()); const auto sizesAt = static_cast<size_t>(sizes - bytes.begin());
    ASSERT_EQ(3u, U32(bytes, sizesAt + 12));
    const auto table = std::search(bytes.begin(), bytes.end(), "co64", "co64" + 4);
    ASSERT_TRUE(table != bytes.end()); const auto at = static_cast<size_t>(table - bytes.begin());
    ASSERT_EQ(3u, U32(bytes, at + 8));
    for (int i = 0; i < 3; ++i)
    {
        const uint32_t sampleBytes = U32(bytes, sizesAt + 16 + i * 4);
        const uint64_t offset = (static_cast<uint64_t>(U32(bytes, at + 12 + i * 8)) << 32) | U32(bytes, at + 16 + i * 8);
        ASSERT_GT(sampleBytes, 4u);
        size_t pos = static_cast<size_t>(offset);
        const size_t end = pos + sampleBytes;
        ASSERT_LE(end, bytes.size());
        while (pos < end)
        {
            ASSERT_LE(pos + 4, end);
            const uint32_t nalBytes = U32(bytes, pos);
            ASSERT_GT(nalBytes, 0u);
            pos += 4 + static_cast<size_t>(nalBytes);
        }
        ASSERT_EQ(end, pos);
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
    // 4:2:0 averages neighboring chroma samples. Compare the image after a small
    // footprint filter so normal color-edge reconstruction is allowed, while broad
    // ghost shapes, hue/range errors, and displaced fractal geometry still fail.
    ASSERT_LT(error, 5.0);

    File hevcFile; { MovieWriter writer(hevcFile.path.wstring(), p.width, p.height, p.fps, MovieCodec::Hevc); writer.WriteFrame(expected); writer.Finish(); }
    const auto hevcPlayback = DecodePlayerFrame(hevcFile.path, p.width, p.height);
    ASSERT_LT(FilteredPixelError(expected, hevcPlayback, p.width, p.height), 5.0);
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

TEST_CASE(Movie, VideoWriterSupportsBothCodecsAndEightKFrames)
{
    const std::array<uint32_t, 8> colors{0xFF000000, 0xFFFFFFFF, 0xFFFF2020, 0xFF20FF20,
        0xFF2020FF, 0xFF808080, 0xFF65A0D0, 0xFFCE5448};
    std::vector<uint32_t> hdExpected(static_cast<size_t>(1920) * 1080);
    for (int y = 0; y < 1080; ++y) for (int x = 0; x < 1920; ++x)
        hdExpected[static_cast<size_t>(y) * 1920 + x] = colors[(x * 8 / 1920 + (y >= 540 ? 4 : 0)) % 8];

    for (MovieCodec codec : {MovieCodec::H264, MovieCodec::Hevc})
    {
        File hdFile;
        MovieCodec active = codec;
        {
            MovieWriter writer(hdFile.path.wstring(), 1920, 1080, 30, codec);
            active = writer.ActiveCodec();
            writer.WriteFrame(hdExpected);
            writer.Finish();
        }
        const auto hdBytes = Read(hdFile.path);
        const char* configTag = (active == MovieCodec::Hevc) ? "hvcC" : "avcC";
        ASSERT_TRUE(std::search(hdBytes.begin(), hdBytes.end(), configTag, configTag + 4) != hdBytes.end());
        const auto playback = DecodePlayerFrame(hdFile.path, 1920, 1080);
        for (int y : {270, 810}) for (int patch = 0; patch < 8; ++patch)
        {
            const size_t i = static_cast<size_t>(y) * 1920 + (2 * patch + 1) * 1920 / 16;
            for (int shift : {0, 8, 16}) ASSERT_NEAR((hdExpected[i] >> shift) & 255, (playback[i] >> shift) & 255, 4);
        }
    }

    File file; std::vector<uint32_t> pixels(static_cast<size_t>(7680) * 4320, 0xFF27496D);
    { MovieWriter writer(file.path.wstring(), 7680, 4320, 60, MovieCodec::Hevc); writer.WriteFrame(pixels); writer.Finish(); }
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ASSERT_TRUE(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE);
    ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));
    {
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFAttributes> attributes; MFCreateAttributes(&attributes, 1);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(file.path.c_str(), attributes.Get(), &reader)));
        const DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        ComPtr<IMFMediaType> mediaType;
        ASSERT_TRUE(SUCCEEDED(reader->GetCurrentMediaType(videoStream, &mediaType)));
        UINT32 width{}, height{};
        ASSERT_TRUE(SUCCEEDED(MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height)));
        ASSERT_EQ(7680u, width); ASSERT_EQ(4320u, height);
        DWORD flags{}; LONGLONG timestamp{}; ComPtr<IMFSample> sample;
        ASSERT_TRUE(SUCCEEDED(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, &sample)));
        ASSERT_TRUE(sample != nullptr);
    }
    MFShutdown();
    if (SUCCEEDED(apartment)) CoUninitialize();
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

TEST_CASE(Movie, BackgroundExportPreparesAutomaticTargets)
{
    File file; auto p = SmallMovie(); p.autoRetarget = true;
    MovieExporter exporter(p, file.path.wstring()); WaitFor(exporter);
    const auto status = exporter.Progress(); ASSERT_TRUE(status.error.empty()); ASSERT_FALSE(status.canceled);
    ASSERT_FALSE(status.planning); ASSERT_EQ(p.FrameCount(), status.completed);
    ASSERT_TRUE(exporter.TakePreview().has_value()); ASSERT_FALSE(Read(file.path).empty());
}

TEST_CASE(Movie, CancelPreservesAnExistingDestination)
{
    for (bool automatic : {false, true})
    {
        File file; { std::ofstream stream(file.path); stream << "original"; }
        auto p = SmallMovie(); p.width = 1920; p.height = 1080; p.start.maxIterations = 10000; p.autoRetarget = automatic;
        MovieExporter exporter(p, file.path.wstring()); exporter.Cancel(); WaitFor(exporter);
        ASSERT_TRUE(exporter.Progress().canceled); ASSERT_FALSE(exporter.Progress().planning);
        const auto bytes = Read(file.path); ASSERT_EQ(std::string("original"), std::string(bytes.begin(), bytes.end()));
    }
}
