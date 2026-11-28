#include "pch.h"
#include "TestHarness.h"
#include "ConsoleLauncher.h"
#include "MovieCli.h"
#include "MovieExport.h"
#include "ViewState.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    struct TempFile
    {
        std::filesystem::path path;
        explicit TempFile(const wchar_t* ext)
        {
            static std::atomic<uint64_t> seq{};
            path = std::filesystem::temp_directory_path() /
                (L"mandel-cli-test-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(GetTickCount64()) +
                 L"-" + std::to_wstring(++seq) + ext);
        }
        ~TempFile()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };

    std::string ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    std::string WideToUtf8Ascii(const std::wstring& w)
    {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c));
        return s;
    }
}

TEST_CASE(MovieCli, NoArgsLaunchesGuiAndHelpShowsUsage)
{
    const auto empty = ParseMovieCli({});
    ASSERT_TRUE(empty.action == CliAction::LaunchGui);

    for (const wchar_t* flag : { L"--help", L"-h", L"/?" })
    {
        const auto help = ParseMovieCli({ flag });
        ASSERT_TRUE(help.action == CliAction::ShowHelp);
    }

    const std::wstring usage = MovieCliUsage();
    ASSERT_TRUE(usage.find(L"--export-movie") != std::wstring::npos);
    ASSERT_TRUE(usage.find(L"--resolution") != std::wstring::npos);
    ASSERT_TRUE(usage.find(L"--config") != std::wstring::npos);
    ASSERT_TRUE(usage.find(L"--auto-target") != std::wstring::npos);
}

TEST_CASE(MovieCli, ParsesMinimalExportMovieAndDefaults)
{
    const auto a = ParseMovieCli({ L"--export-movie", L"zoom.mp4" });
    ASSERT_TRUE(a.action == CliAction::ExportMovie);
    ASSERT_TRUE(a.options.outputPath == L"zoom.mp4");
    ASSERT_EQ(1920, a.options.plan.width);
    ASSERT_EQ(1080, a.options.plan.height);
    ASSERT_EQ(30, a.options.plan.fps);
    ASSERT_EQ(30, a.options.plan.seconds);
    ASSERT_TRUE(a.options.plan.codec == MovieCodec::H264);
    ASSERT_TRUE(a.options.plan.motionSmoothing);
    ASSERT_TRUE(a.options.plan.useGpu);
    ASSERT_TRUE(a.options.plan.autoScaleIterations);
    ASSERT_FALSE(a.options.plan.autoRetarget);
    ASSERT_NEAR(1.0, a.options.plan.start.zoom, 1e-12);
    ASSERT_NEAR(std::exp2(30.0), a.options.plan.endZoom, 1e-6);

    const auto b = ParseMovieCli({ L"--export-movie=out.mp4" });
    ASSERT_TRUE(b.action == CliAction::ExportMovie);
    ASSERT_TRUE(b.options.outputPath == L"out.mp4");

    const auto c = ParseMovieCli({ L"--export-movie", L"-o", L"alt.mp4" });
    ASSERT_TRUE(c.action == CliAction::ExportMovie);
    ASSERT_TRUE(c.options.outputPath == L"alt.mp4");
}

TEST_CASE(MovieCli, ParsesAllVideoMotionRenderAndColorOptions)
{
    const std::wstring deepX = L"-0.743643887037158704752191506114774";
    const std::wstring deepY = L"0.131825904205311970493132056385139";

    const auto parsed = ParseMovieCli({
        L"--export-movie", L"deep.mp4",
        L"--fractal", L"burning-ship",
        L"--center-x", deepX,
        L"--center-y", deepY,
        L"--target-x=-1.75",
        L"--target-y=+0.03",
        L"--start-zoom", L"2.5",
        L"--end-zoom", L"25000",
        L"--resolution", L"4k",
        L"--fps", L"60",
        L"--duration", L"12",
        L"--codec", L"hevc",
        L"--no-motion-smoothing",
        L"--iterations", L"1200",
        L"--no-smooth",
        L"--cpu",
        L"--theme", L"inferno",
        L"--frequency", L"2.5",
        L"--phase", L"0.25",
        L"--invert",
        L"--interior-color", L"#112233",
        L"--quiet"
    });
    ASSERT_TRUE(parsed.action == CliAction::ExportMovie);
    const MoviePlan& p = parsed.options.plan;
    ASSERT_TRUE(p.start.fractalType == FractalType::BurningShip);
    BigFloat expectedX, expectedY;
    ASSERT_TRUE(BigFloat::TryParse(deepX, expectedX));
    ASSERT_TRUE(BigFloat::TryParse(deepY, expectedY));
    ASSERT_TRUE((p.start.hpCenterX - expectedX).Abs().ToDouble() < 1e-30);
    ASSERT_TRUE((p.start.hpCenterY - expectedY).Abs().ToDouble() < 1e-30);
    ASSERT_NEAR(-1.75, p.targetX.ToDouble(), 1e-12);
    ASSERT_NEAR(0.03, p.targetY.ToDouble(), 1e-12);
    ASSERT_NEAR(2.5, p.start.zoom, 1e-12);
    ASSERT_NEAR(25000.0, p.endZoom, 1e-9);
    ASSERT_EQ(3840, p.width);
    ASSERT_EQ(2160, p.height);
    ASSERT_EQ(60, p.fps);
    ASSERT_EQ(12, p.seconds);
    ASSERT_TRUE(p.codec == MovieCodec::Hevc);
    ASSERT_FALSE(p.motionSmoothing);
    ASSERT_EQ(1200, p.start.maxIterations);
    ASSERT_FALSE(p.autoScaleIterations);
    ASSERT_FALSE(p.start.smooth);
    ASSERT_FALSE(p.useGpu);
    ASSERT_TRUE(p.theme.CurrentPreset() == PresetTheme::Inferno);
    ASSERT_NEAR(2.5f, p.frequency, 1e-6f);
    ASSERT_NEAR(0.25f, p.phase, 1e-6f);
    ASSERT_TRUE(p.invert);
    ASSERT_EQ(0xFF112233u, p.interior);
    ASSERT_TRUE(parsed.options.quiet);

    // Custom WxH resolution plus individual width/height overrides.
    const auto customRes = ParseMovieCli({
        L"--export-movie", L"small.mp4",
        L"--resolution", L"640x360",
        L"--width", L"320"
    });
    ASSERT_TRUE(customRes.action == CliAction::ExportMovie);
    ASSERT_EQ(320, customRes.options.plan.width);
    ASSERT_EQ(360, customRes.options.plan.height);
}

TEST_CASE(MovieCli, ParsesDirectionZoomSpeedAndAutoTarget)
{
    const auto zoomOut = ParseMovieCli({
        L"--export-movie", L"out.mp4",
        L"--start-zoom", L"64",
        L"--direction", L"out",
        L"--zoom-speed", L"2.0",
        L"--duration", L"3"
    });
    ASSERT_TRUE(zoomOut.action == CliAction::ExportMovie);
    ASSERT_NEAR(64.0, zoomOut.options.plan.start.zoom, 1e-12);
    ASSERT_NEAR(1.0, zoomOut.options.plan.endZoom, 1e-12);

    const auto autoPick = ParseMovieCli({
        L"--export-movie", L"auto.mp4",
        L"--auto-target",
        L"--seed", L"123456789",
        L"--duration", L"5"
    });
    ASSERT_TRUE(autoPick.action == CliAction::ExportMovie);
    ASSERT_TRUE(autoPick.options.plan.autoRetarget);
    ASSERT_EQ(123456789ull, autoPick.options.plan.targetSeed);
}

TEST_CASE(MovieCli, ParsesBookmarksAndExplicitOverrides)
{
    const auto seahorse = ParseMovieCli({
        L"--export-movie", L"seahorse.mp4",
        L"--bookmark", L"seahorse-valley",
        L"--start-zoom", L"1",
        L"--end-zoom", L"85000",
        L"--duration", L"10"
    });
    ASSERT_TRUE(seahorse.action == CliAction::ExportMovie);
    ASSERT_TRUE(seahorse.options.plan.start.fractalType == FractalType::Mandelbrot);
    ASSERT_NEAR(-0.7436438870371587, seahorse.options.plan.start.centerX, 1e-14);
    ASSERT_NEAR(0.1318259042053119, seahorse.options.plan.start.centerY, 1e-14);
    ASSERT_NEAR(1.0, seahorse.options.plan.start.zoom, 1e-12);
    ASSERT_NEAR(85000.0, seahorse.options.plan.endZoom, 1e-9);
    ASSERT_NEAR(seahorse.options.plan.start.centerX, seahorse.options.plan.targetX.ToDouble(), 1e-14);

    // Bookmark lookup without --fractal automatically selects the owning fractal.
    const auto grandMast = ParseMovieCli({
        L"--export-movie", L"mast.mp4",
        L"--bookmark", L"The Grand Mast"
    });
    ASSERT_TRUE(grandMast.action == CliAction::ExportMovie);
    ASSERT_TRUE(grandMast.options.plan.start.fractalType == FractalType::BurningShip);
    ASSERT_NEAR(-1.75, grandMast.options.plan.start.centerX, 1e-12);
    ASSERT_NEAR(35.0, grandMast.options.plan.start.zoom, 1e-12);
}

TEST_CASE(MovieCli, ParsesJuliaModeAndRejectsUnsupportedFractals)
{
    const auto julia = ParseMovieCli({
        L"--export-movie", L"julia.mp4",
        L"--julia", L"-0.75,0.1",
        L"--duration", L"4"
    });
    ASSERT_TRUE(julia.action == CliAction::ExportMovie);
    ASSERT_TRUE(julia.options.plan.start.juliaSet);
    ASSERT_TRUE(julia.options.plan.start.fractalType == FractalType::Mandelbrot);
    ASSERT_NEAR(-0.75, julia.options.plan.start.juliaX.ToDouble(), 1e-12);
    ASSERT_NEAR(0.1, julia.options.plan.start.juliaY.ToDouble(), 1e-12);
    ASSERT_NEAR(0.0, julia.options.plan.start.centerX, 1e-12);
    ASSERT_NEAR(0.0, julia.options.plan.start.centerY, 1e-12);

    const auto badFractal = ParseMovieCli({
        L"--export-movie", L"bad.mp4",
        L"--fractal", L"newton",
        L"--julia", L"-0.5,0.2"
    });
    ASSERT_TRUE(badFractal.action == CliAction::Error);

    const auto badAutoTarget = ParseMovieCli({
        L"--export-movie", L"bad.mp4",
        L"--julia", L"-0.75,0.1",
        L"--auto-target"
    });
    ASSERT_TRUE(badAutoTarget.action == CliAction::Error);
}

TEST_CASE(MovieCli, LoadsConfigJsonFromSerializeViewStateWithOverrides)
{
    MandelbrotConfig cfg;
    cfg.fractalType = FractalType::Mandelbrot;
    BigFloat hpX, hpY;
    ASSERT_TRUE(BigFloat::TryParse("-0.743643887037158704752191506114774", hpX));
    ASSERT_TRUE(BigFloat::TryParse("+0.131825904205311970493132056385139", hpY));
    cfg.SetCenter(hpX, hpY);
    cfg.zoom = 1e25;
    cfg.width = 800;
    cfg.height = 600;
    cfg.maxIterations = 2400;
    cfg.smooth = false;

    ColorTheme customTheme;
    customTheme.SetStops({
        { 0.0f, 10, 20, 30 },
        { 0.5f, 120, 180, 240 },
        { 1.0f, 255, 250, 240 }
    });

    DirectionalLightingParams lit{};
    lit.enabled = true;
    lit.azimuthDeg = 225.0f;
    lit.elevationDeg = 32.0f;
    lit.strength = 1.6f;

    const std::wstring jsonW = SerializeViewState(cfg, customTheme, 1.75f, 0.35f, true, 0xFF050A14u, lit);
    TempFile configFile(L".json");
    {
        std::ofstream out(configFile.path, std::ios::binary);
        const std::string utf8 = WideToUtf8Ascii(jsonW);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }

    // Load config without overrides: movie keeps 1080p default (not GUI 800x600) while preserving deep coords & custom stops.
    const auto loaded = ParseMovieCli({
        L"--export-movie", L"from_cfg.mp4",
        L"--config", configFile.path.wstring(),
        L"--duration", L"2"
    });
    ASSERT_TRUE(loaded.action == CliAction::ExportMovie);
    const MoviePlan& p = loaded.options.plan;
    ASSERT_EQ(1920, p.width);
    ASSERT_EQ(1080, p.height);
    ASSERT_TRUE((p.start.hpCenterX - hpX).Abs().ToDouble() < 1e-30);
    ASSERT_TRUE((p.start.hpCenterY - hpY).Abs().ToDouble() < 1e-30);
    ASSERT_NEAR(1e25, p.start.zoom, 1e13);
    ASSERT_EQ(2400, p.start.maxIterations);
    ASSERT_FALSE(p.autoScaleIterations);
    ASSERT_FALSE(p.start.smooth);
    ASSERT_NEAR(1.75f, p.frequency, 1e-5f);
    ASSERT_NEAR(0.35f, p.phase, 1e-5f);
    ASSERT_TRUE(p.invert);
    ASSERT_EQ(0xFF050A14u, p.interior);
    ASSERT_TRUE(p.lighting.enabled);
    ASSERT_NEAR(225.0f, p.lighting.azimuthDeg, 1e-4f);
    ASSERT_NEAR(32.0f, p.lighting.elevationDeg, 1e-4f);
    ASSERT_NEAR(1.6f, p.lighting.strength, 1e-4f);
    ASSERT_TRUE(p.theme.CurrentPreset() == PresetTheme::Custom);
    ASSERT_EQ(3u, p.theme.Stops().size());
    ASSERT_EQ(120u, static_cast<unsigned>(p.theme.Stops()[1].r));

    // Explicit CLI options override fields from --config.
    const auto overridden = ParseMovieCli({
        L"--export-movie", L"override.mp4",
        L"--config", configFile.path.wstring(),
        L"--theme", L"viridis",
        L"--auto-iterations",
        L"--no-lighting",
        L"--duration", L"2"
    });
    ASSERT_TRUE(overridden.action == CliAction::ExportMovie);
    ASSERT_TRUE(overridden.options.plan.theme.CurrentPreset() == PresetTheme::Viridis);
    ASSERT_TRUE(overridden.options.plan.autoScaleIterations);
    ASSERT_FALSE(overridden.options.plan.lighting.enabled);

    // Explicit light parameter flags enable lighting automatically and propagate to --ascii too.
    const auto cliLit = ParseMovieCli({
        L"--ascii", L"--once",
        L"--light-azimuth", L"45",
        L"--light-elevation", L"60",
        L"--light-strength", L"1.25"
    });
    ASSERT_TRUE(cliLit.action == CliAction::ConsoleAscii);
    ASSERT_TRUE(cliLit.consoleOptions.lighting.enabled);
    ASSERT_NEAR(45.0f, cliLit.consoleOptions.lighting.azimuthDeg, 1e-4f);
    ASSERT_NEAR(60.0f, cliLit.consoleOptions.lighting.elevationDeg, 1e-4f);
    ASSERT_NEAR(1.25f, cliLit.consoleOptions.lighting.strength, 1e-4f);
}

TEST_CASE(MovieCli, RejectsInvalidAndConflictingArguments)
{
    const std::vector<std::vector<std::wstring>> invalidCases = {
        { L"--unknown-flag" },
        { L"--duration", L"10" }, // missing --export-movie
        { L"--export-movie" }, // missing output path
        { L"--export-movie=" },
        { L"--export-movie", L"a.mp4", L"-o", L"b.mp4" }, // duplicate output
        { L"--export-movie", L"a.mp4", L"--end-zoom", L"10", L"--zoom-speed", L"2" },
        { L"--export-movie", L"a.mp4", L"--end-zoom", L"10", L"--direction", L"out" },
        { L"--export-movie", L"a.mp4", L"--auto-target", L"--target-x", L"0" },
        { L"--export-movie", L"a.mp4", L"--auto-target", L"--direction", L"out" },
        { L"--export-movie", L"a.mp4", L"--fps", L"29" },
        { L"--export-movie", L"a.mp4", L"--duration", L"0" },
        { L"--export-movie", L"a.mp4", L"--duration", L"3601" },
        { L"--export-movie", L"a.mp4", L"--zoom-speed", L"0.1" },
        { L"--export-movie", L"a.mp4", L"--zoom-speed", L"5.5" },
        { L"--export-movie", L"a.mp4", L"--resolution", L"9999x9999" },
        { L"--export-movie", L"a.mp4", L"--resolution", L"not-a-res" },
        { L"--export-movie", L"a.mp4", L"--codec", L"vp9" },
        { L"--export-movie", L"a.mp4", L"--fractal", L"unknown" },
        { L"--export-movie", L"a.mp4", L"--theme", L"unknown" },
        { L"--export-movie", L"a.mp4", L"--bookmark", L"Nonexistent Place" },
        { L"--export-movie", L"a.mp4", L"--interior-color", L"#123" },
        { L"--export-movie", L"a.mp4", L"--quiet=1" },
        { L"--export-movie", L"a.mp4", L"--iterations", L"0" },
        { L"--export-movie", L"a.mp4", L"--lighting=1" },
        { L"--export-movie", L"a.mp4", L"--light-elevation", L"2" },
        { L"--export-movie", L"a.mp4", L"--light-elevation", L"90" },
        { L"--export-movie", L"a.mp4", L"--light-strength", L"-0.1" },
        { L"--export-movie", L"a.mp4", L"--light-strength", L"3.0" },
        { L"--export-movie", L"a.mp4", L"--cycle-speed", L"5.5" },
        { L"--export-movie", L"a.mp4", L"--cycle-speed", L"-5.5" },
        { L"--export-movie", L"a.mp4", L"--cycle-speed", L"fast" },
        { L"--ascii", L"--once", L"--cycle-speed", L"0.2" },
        { L"--export-movie", L"a.mp4", L"--histogram=1" },
        { L"--export-movie", L"a.mp4", L"--config", L"C:\\does-not-exist-mandel-cli.json" }
    };

    for (const auto& args : invalidCases)
    {
        const auto res = ParseMovieCli(args);
        ASSERT_TRUE(res.action == CliAction::Error);
        ASSERT_FALSE(res.errorMessage.empty());
    }

    // Valid positive and negative --cycle-speed values parse cleanly for --export-movie.
    {
        const auto pos = ParseMovieCli({ L"--export-movie", L"flow.mp4", L"--cycle-speed", L"0.5" });
        ASSERT_TRUE(pos.action == CliAction::ExportMovie);
        ASSERT_NEAR(0.5f, pos.options.plan.cycleSpeed, 1e-6f);

        const auto neg = ParseMovieCli({ L"--export-movie", L"flow.mp4", L"--cycle-speed=-1.25" });
        ASSERT_TRUE(neg.action == CliAction::ExportMovie);
        ASSERT_NEAR(-1.25f, neg.options.plan.cycleSpeed, 1e-6f);

        ASSERT_TRUE(MovieCliUsage().find(L"--cycle-speed") != std::wstring::npos);
    }

    // --histogram / --auto-contrast / --no-histogram work for both --export-movie and --ascii, and round-trip via JSON config.
    {
        const auto movieHist = ParseMovieCli({ L"--export-movie", L"h.mp4", L"--histogram" });
        ASSERT_TRUE(movieHist.action == CliAction::ExportMovie);
        ASSERT_TRUE(movieHist.options.plan.histogramEqualize);

        const auto asciiHist = ParseMovieCli({ L"--ascii", L"--once", L"--auto-contrast" });
        ASSERT_TRUE(asciiHist.action == CliAction::ConsoleAscii);
        ASSERT_TRUE(asciiHist.consoleOptions.histogramEqualize);

        MandelbrotConfig cfg;
        ColorTheme theme(PresetTheme::Inferno);
        const std::wstring jsonW = SerializeViewState(cfg, theme, 1.0f, 0.0f, false, 0xFF000000u, {}, true);
        TempFile histCfg(L".json");
        {
            std::ofstream out(histCfg.path, std::ios::binary);
            const std::string utf8 = WideToUtf8Ascii(jsonW);
            out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }
        const auto fromJson = ParseMovieCli({ L"--export-movie", L"h.mp4", L"--config", histCfg.path.wstring() });
        ASSERT_TRUE(fromJson.action == CliAction::ExportMovie);
        ASSERT_TRUE(fromJson.options.plan.histogramEqualize);

        const auto overridden = ParseMovieCli({ L"--export-movie", L"h.mp4", L"--config", histCfg.path.wstring(), L"--no-histogram" });
        ASSERT_TRUE(overridden.action == CliAction::ExportMovie);
        ASSERT_FALSE(overridden.options.plan.histogramEqualize);

        ASSERT_TRUE(MovieCliUsage().find(L"--histogram") != std::wstring::npos);
    }

    // Malformed JSON config is rejected cleanly.
    MoviePlan plan;
    bool loadedIters = false;
    bool threw = false;
    try { ApplyViewStateJson("{ broken json", plan, loadedIters); }
    catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
}

TEST_CASE(MovieCli, SplitsWindowsCommandLineWithQuotes)
{
    const auto args = SplitWindowsCommandLine(
        L"\"C:\\Program Files\\MandelbrotViewer.exe\" --export-movie \"C:\\My Movies\\zoom out.mp4\" --bookmark \"Seahorse Valley\" --center-x -0.75");
    ASSERT_EQ(6u, args.size());
    ASSERT_TRUE(args[0] == L"--export-movie");
    ASSERT_TRUE(args[1] == L"C:\\My Movies\\zoom out.mp4");
    ASSERT_TRUE(args[2] == L"--bookmark");
    ASSERT_TRUE(args[3] == L"Seahorse Valley");
    ASSERT_TRUE(args[4] == L"--center-x");
    ASSERT_TRUE(args[5] == L"-0.75");

    const auto noArgs = SplitWindowsCommandLine(L"\"C:\\Program Files\\MandelbrotViewer.exe\"");
    ASSERT_TRUE(noArgs.empty());
}

TEST_CASE(MovieCli, FormatsProgressStatsWithFramesZoomAndElapsedTime)
{
    MoviePlan plan;
    plan.fps = 30;
    plan.seconds = 10; // 300 frames
    plan.start.zoom = 1.0;
    plan.endZoom = 1.0e6;
    ASSERT_NEAR(1.0, plan.ZoomAt(0.0), 1e-12);
    ASSERT_NEAR(1.0e3, plan.ZoomAt(149.5), 1e-6);
    ASSERT_NEAR(1.0e6, plan.ZoomAt(299.0), 1e-6);

    MovieProgress planning{};
    planning.completed = 0;
    planning.total = 300;
    planning.planning = true;
    planning.elapsedSeconds = 1.2;
    planning.currentZoom = 1.0;
    const std::wstring planLine = FormatMovieCliProgressLine(planning, plan);
    ASSERT_TRUE(planLine.find(L"Planning auto-targets") != std::wstring::npos);
    ASSERT_TRUE(planLine.find(L"Frames: 0/300") != std::wstring::npos);
    ASSERT_TRUE(planLine.find(L"Zoom: 1.00x") != std::wstring::npos);
    ASSERT_TRUE(planLine.find(L"Elapsed: 00:00:01.2") != std::wstring::npos);

    MovieProgress rendering{};
    rendering.completed = 150;
    rendering.total = 300;
    rendering.frameFraction = 0.0f;
    rendering.elapsedSeconds = 65.4;
    rendering.currentZoom = 15000.0;
    const std::wstring renderLine = FormatMovieCliProgressLine(rendering, plan);
    ASSERT_TRUE(renderLine.find(L"Frames: 150/300") != std::wstring::npos);
    ASSERT_TRUE(renderLine.find(L"50.0%") != std::wstring::npos);
    ASSERT_TRUE(renderLine.find(L"Zoom: 1.50e+04x") != std::wstring::npos);
    ASSERT_TRUE(renderLine.find(L"Elapsed: 00:01:05.4") != std::wstring::npos);

    MovieProgress finalizing{};
    finalizing.completed = 300;
    finalizing.total = 300;
    finalizing.elapsedSeconds = 120.0;
    finalizing.currentZoom = 1.0e6;
    const std::wstring finLine = FormatMovieCliProgressLine(finalizing, plan);
    ASSERT_TRUE(finLine.find(L"Finalizing MP4") != std::wstring::npos);
    ASSERT_TRUE(finLine.find(L"Frames: 300/300") != std::wstring::npos);
    ASSERT_TRUE(finLine.find(L"Zoom: 1.00e+06x") != std::wstring::npos);
    ASSERT_TRUE(finLine.find(L"Elapsed: 00:02:00.0") != std::wstring::npos);
}

TEST_CASE(MovieCli, EndToEndHeadlessExportWritesValidMovieAndReportsProgress)
{
    TempFile movieFile(L".mp4");
    const auto parsed = ParseMovieCli({
        L"--export-movie", movieFile.path.wstring(),
        L"--resolution", L"64x48",
        L"--fps", L"24",
        L"--duration", L"1",
        L"--center-x", L"-0.75",
        L"--center-y", L"0.0",
        L"--target-x", L"-0.73",
        L"--target-y", L"0.1",
        L"--start-zoom", L"1",
        L"--end-zoom", L"4",
        L"--iterations", L"128",
        L"--no-motion-smoothing"
    });
    ASSERT_TRUE(parsed.action == CliAction::ExportMovie);

    std::wostringstream out, err;
    const int exitCode = RunMovieExportCli(parsed.options, out, err, nullptr, false);
    ASSERT_EQ(0, exitCode);
    ASSERT_TRUE(err.str().empty());
    const std::wstring text = out.str();
    ASSERT_TRUE(text.find(L"Exporting zoom movie:") != std::wstring::npos);
    ASSERT_TRUE(text.find(L"Frames: 24/24") != std::wstring::npos);
    ASSERT_TRUE(text.find(L"Zoom: 4.00x") != std::wstring::npos);
    ASSERT_TRUE(text.find(L"Elapsed: ") != std::wstring::npos);
    ASSERT_TRUE(text.find(L"Export complete:") != std::wstring::npos);
    ASSERT_FALSE(ReadBytes(movieFile.path).empty());

    // Verify the exported MP4 decodes cleanly through Windows Media playback.
    const auto decoded = DecodePlayerFrame(movieFile.path, 64, 48);
    ASSERT_EQ(static_cast<size_t>(64 * 48), decoded.size());

    // Interactive console mode uses carriage-return in-place updates and reports stats.
    TempFile ttyFile(L".mp4");
    auto ttyOptions = parsed.options;
    ttyOptions.outputPath = ttyFile.path.wstring();
    std::wostringstream ttyOut, ttyErr;
    ASSERT_EQ(0, RunMovieExportCli(ttyOptions, ttyOut, ttyErr, nullptr, true));
    ASSERT_TRUE(ttyErr.str().empty());
    ASSERT_TRUE(ttyOut.str().find(L"\rFrames: ") != std::wstring::npos);
    ASSERT_TRUE(ttyOut.str().find(L"Frames: 24/24") != std::wstring::npos);
    ASSERT_TRUE(ttyOut.str().find(L"Zoom: 4.00x") != std::wstring::npos);
    ASSERT_TRUE(ttyOut.str().find(L"Elapsed: ") != std::wstring::npos);

    // Quiet mode suppresses stdout while still succeeding.
    TempFile quietFile(L".mp4");
    auto quietOptions = parsed.options;
    quietOptions.outputPath = quietFile.path.wstring();
    quietOptions.quiet = true;
    std::wostringstream qOut, qErr;
    ASSERT_EQ(0, RunMovieExportCli(quietOptions, qOut, qErr, nullptr, false));
    ASSERT_TRUE(qOut.str().empty());
    ASSERT_TRUE(qErr.str().empty());
    ASSERT_FALSE(ReadBytes(quietFile.path).empty());
}

TEST_CASE(MovieCli, EndToEndHeadlessExportCancellationPreservesExistingFile)
{
    TempFile movieFile(L".mp4");
    const std::string sentinel = "existing-movie-content";
    {
        std::ofstream f(movieFile.path, std::ios::binary);
        f.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
    }

    const auto parsed = ParseMovieCli({
        L"--export-movie", movieFile.path.wstring(),
        L"--resolution", L"128x96",
        L"--fps", L"30",
        L"--duration", L"10",
        L"--iterations", L"500"
    });
    ASSERT_TRUE(parsed.action == CliAction::ExportMovie);

    std::atomic<bool> cancel{ true };
    std::wostringstream out, err;
    const int exitCode = RunMovieExportCli(parsed.options, out, err, &cancel, false);
    ASSERT_EQ(130, exitCode);
    ASSERT_TRUE(err.str().find(L"canceled") != std::wstring::npos);
    ASSERT_TRUE(ReadBytes(movieFile.path) == sentinel);
}

TEST_CASE(MovieCli, EdgeCaseRegressionsHelpJuliaResetFloatBoundsAndJsonDepth)
{
    // --export-movie /? must display help instead of treating /? as an output filename.
    const auto helpParsed = ParseMovieCli({ L"--export-movie", L"/?" });
    ASSERT_TRUE(helpParsed.action == CliAction::ShowHelp);

    // --fractal julia must reset the camera to Julia's centered view rather than Mandelbrot (-0.75, 0).
    const auto juliaParsed = ParseMovieCli({ L"--export-movie", L"julia.mp4", L"--fractal", L"julia" });
    ASSERT_TRUE(juliaParsed.action == CliAction::ExportMovie);
    ASSERT_TRUE(juliaParsed.options.plan.start.juliaSet);
    ASSERT_NEAR(0.0, juliaParsed.options.plan.start.centerX, 1e-12);
    ASSERT_NEAR(0.0, juliaParsed.options.plan.start.centerY, 1e-12);

    // --frequency 1e-50 underflows to 0.0f and must be rejected during CLI parsing.
    const auto underflowFreq = ParseMovieCli({ L"--export-movie", L"out.mp4", L"--frequency", L"1e-50" });
    ASSERT_TRUE(underflowFreq.action == CliAction::Error);

    // --frequency 1e100 overflows float to +inf and must be rejected.
    const auto overflowFreq = ParseMovieCli({ L"--export-movie", L"out.mp4", L"--frequency", L"1e100" });
    ASSERT_TRUE(overflowFreq.action == CliAction::Error);

    // Deeply nested JSON config must fail cleanly with a nesting depth error instead of overflowing the stack.
    TempFile deepJson(L".json");
    {
        std::string nested(100, '[');
        nested += std::string(100, ']');
        std::ofstream f(deepJson.path, std::ios::binary);
        f.write(nested.data(), static_cast<std::streamsize>(nested.size()));
    }
    const auto deepParsed = ParseMovieCli({ L"--export-movie", L"out.mp4", L"--config", deepJson.path.wstring() });
    ASSERT_TRUE(deepParsed.action == CliAction::Error);
}

TEST_CASE(MovieCli, ConsoleLauncherBuildsSiblingExePathAndForwardsRawArguments)
{
    using namespace Mandelbrot::CliLauncher;

    ASSERT_TRUE(BuildSiblingExePath(L"C:\\Users\\demo\\MandelbrotViewer.com") == L"C:\\Users\\demo\\MandelbrotViewer.exe");
    ASSERT_TRUE(BuildSiblingExePath(L"C:/Program Files/Fractal App/MandelbrotViewer.com") == L"C:/Program Files/Fractal App/MandelbrotViewer.exe");
    ASSERT_TRUE(BuildSiblingExePath(L"MandelbrotViewer.com") == L"MandelbrotViewer.exe");

    ASSERT_TRUE(ExtractCommandLineArguments(L"MandelbrotViewer.com --help") == L"--help");
    ASSERT_TRUE(ExtractCommandLineArguments(L"\"C:\\Program Files\\MandelbrotViewer.com\"   --bookmark \"Seahorse Valley\" --zoom 10")
        == L"--bookmark \"Seahorse Valley\" --zoom 10");
    ASSERT_TRUE(ExtractCommandLineArguments(L"\"C:\\Apps\\MandelbrotViewer.com\"") == L"");
    ASSERT_TRUE(ExtractCommandLineArguments(L"  MandelbrotViewer.com   \t ") == L"");
    ASSERT_TRUE(ExtractCommandLineArguments(L"") == L"");

    ASSERT_FALSE(ShouldWaitForChild(L""));
    ASSERT_TRUE(ShouldWaitForChild(L"--help"));

    ASSERT_TRUE(BuildChildCommandLine(L"C:\\Program Files\\MandelbrotViewer.exe", L"")
        == L"\"C:\\Program Files\\MandelbrotViewer.exe\"");
    ASSERT_TRUE(BuildChildCommandLine(L"C:\\Program Files\\MandelbrotViewer.exe", L"--ascii --once")
        == L"\"C:\\Program Files\\MandelbrotViewer.exe\" --ascii --once");
}
