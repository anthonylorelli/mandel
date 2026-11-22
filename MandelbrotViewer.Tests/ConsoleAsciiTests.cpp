#include "pch.h"
#include "TestHarness.h"
#include "ConsoleAscii.h"
#include "MovieCli.h"
#include "ViewState.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Mandelbrot;

namespace
{
    std::string StripAnsiEscapes(const std::string& input)
    {
        std::string out;
        out.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == '\x1b' && i + 1 < input.size() && input[i + 1] == '[')
            {
                i += 2;
                while (i < input.size() && !((input[i] >= 'A' && input[i] <= 'Z') || (input[i] >= 'a' && input[i] <= 'z')))
                {
                    ++i;
                }
                continue;
            }
            out.push_back(input[i]);
        }
        return out;
    }

    std::vector<std::string> SplitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < text.size())
        {
            const size_t nl = text.find('\n', pos);
            if (nl == std::string::npos)
            {
                lines.push_back(text.substr(pos));
                break;
            }
            lines.push_back(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return lines;
    }

    struct TempJsonFile
    {
        std::filesystem::path path;
        TempJsonFile()
        {
            static std::atomic<uint64_t> seq{};
            path = std::filesystem::temp_directory_path() /
                (L"mandel-ascii-test-" + std::to_wstring(++seq) + L".json");
        }
        ~TempJsonFile()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };
}

TEST_CASE(ConsoleAscii, ParsesDefaultsAliasesAndUsageText)
{
    const std::wstring usage = MovieCliUsage();
    ASSERT_TRUE(usage.find(L"--ascii") != std::wstring::npos);
    ASSERT_TRUE(usage.find(L"--auto-zoom") != std::wstring::npos);
    ASSERT_TRUE(usage.find(L"--charset") != std::wstring::npos);

    const auto a = ParseMovieCli({ L"--ascii" });
    ASSERT_TRUE(a.action == CliAction::ConsoleAscii);
    ASSERT_EQ(0, a.consoleOptions.cols);
    ASSERT_EQ(0, a.consoleOptions.rows);
    ASSERT_NEAR(DefaultConsoleCharAspect, a.consoleOptions.charAspect, 1e-12);
    ASSERT_EQ(DefaultConsoleFps, a.consoleOptions.fps);
    ASSERT_TRUE(a.consoleOptions.charsetPreset == AsciiCharsetPreset::Standard);
    ASSERT_TRUE(a.consoleOptions.colorMode == AsciiColorMode::Auto);
    ASSERT_TRUE(a.consoleOptions.showHud);
    ASSERT_FALSE(a.consoleOptions.autoZoom);
    ASSERT_TRUE(a.consoleOptions.zoomIn);
    ASSERT_FALSE(a.consoleOptions.autoTarget);
    ASSERT_TRUE(a.consoleOptions.autoScaleIterations);
    ASSERT_NEAR(1.0, a.consoleOptions.start.zoom, 1e-12);

    const auto b = ParseMovieCli({ L"--console" });
    ASSERT_TRUE(b.action == CliAction::ConsoleAscii);
}

TEST_CASE(ConsoleAscii, ParsesAllConsoleOptionsSharedViewFlagsAndConfigJson)
{
    const std::wstring deepX = L"-0.743643887037158704752191506114774";
    const std::wstring deepY = L"0.131825904205311970493132056385139";

    const auto parsed = ParseMovieCli({
        L"--ascii",
        L"--auto-zoom",
        L"--auto-target",
        L"--seed", L"1337",
        L"--fractal", L"burning-ship",
        L"--center-x", deepX,
        L"--center-y", deepY,
        L"--start-zoom", L"4.0",
        L"--end-zoom", L"4000",
        L"--zoom-speed", L"2.5",
        L"--cols", L"120",
        L"--rows", L"40",
        L"--char-aspect", L"1.8",
        L"--charset", L" .oO@#",
        L"--color", L"256",
        L"--fps", L"20",
        L"--duration", L"15",
        L"--frames", L"300",
        L"--iterations", L"800",
        L"--no-smooth",
        L"--cpu",
        L"--theme", L"inferno",
        L"--frequency", L"1.5",
        L"--phase", L"0.2",
        L"--invert",
        L"--interior-color", L"#102030",
        L"--no-hud"
    });
    ASSERT_TRUE(parsed.action == CliAction::ConsoleAscii);
    const ConsoleAsciiOptions& o = parsed.consoleOptions;
    ASSERT_TRUE(o.autoZoom);
    ASSERT_TRUE(o.autoTarget);
    ASSERT_EQ(1337ull, o.targetSeed);
    ASSERT_TRUE(o.start.fractalType == FractalType::BurningShip);
    ASSERT_NEAR(4.0, o.start.zoom, 1e-12);
    ASSERT_TRUE(o.endZoom.has_value());
    ASSERT_NEAR(4000.0, *o.endZoom, 1e-9);
    ASSERT_TRUE(o.zoomIn);
    ASSERT_NEAR(2.5, o.zoomSpeed, 1e-12);
    ASSERT_EQ(120, o.cols);
    ASSERT_EQ(40, o.rows);
    ASSERT_NEAR(1.8, o.charAspect, 1e-12);
    ASSERT_TRUE(o.charsetPreset == AsciiCharsetPreset::Custom);
    ASSERT_TRUE(o.customCharset == " .oO@#");
    ASSERT_TRUE(o.colorMode == AsciiColorMode::Ansi256);
    ASSERT_EQ(20, o.fps);
    ASSERT_EQ(15, o.maxDurationSeconds);
    ASSERT_EQ(300, o.maxFrames);
    ASSERT_EQ(800, o.start.maxIterations);
    ASSERT_FALSE(o.autoScaleIterations);
    ASSERT_FALSE(o.start.smooth);
    ASSERT_FALSE(o.useGpu);
    ASSERT_TRUE(o.theme.CurrentPreset() == PresetTheme::Inferno);
    ASSERT_NEAR(1.5f, o.frequency, 1e-6f);
    ASSERT_NEAR(0.2f, o.phase, 1e-6f);
    ASSERT_TRUE(o.invert);
    ASSERT_EQ(0xFF102030u, o.interior);
    ASSERT_FALSE(o.showHud);

    // Implied auto-zoom when --auto-target or --end-zoom is specified without --no-auto-zoom.
    const auto implied = ParseMovieCli({ L"--ascii", L"--auto-target" });
    ASSERT_TRUE(implied.action == CliAction::ConsoleAscii);
    ASSERT_TRUE(implied.consoleOptions.autoZoom);
    ASSERT_TRUE(implied.consoleOptions.autoTarget);

    // Explicit --no-auto-zoom starts paused even with --auto-target.
    const auto paused = ParseMovieCli({ L"--ascii", L"--auto-target", L"--no-auto-zoom" });
    ASSERT_TRUE(paused.action == CliAction::ConsoleAscii);
    ASSERT_FALSE(paused.consoleOptions.autoZoom);
    ASSERT_TRUE(paused.consoleOptions.autoTarget);

    // Zooming out inferred from --end-zoom < --start-zoom.
    const auto zoomOut = ParseMovieCli({ L"--ascii", L"--start-zoom", L"100", L"--end-zoom", L"2" });
    ASSERT_TRUE(zoomOut.action == CliAction::ConsoleAscii);
    ASSERT_TRUE(zoomOut.consoleOptions.autoZoom);
    ASSERT_FALSE(zoomOut.consoleOptions.zoomIn);

    // Loading a SerializeViewState JSON config file works seamlessly in --ascii mode.
    MandelbrotConfig cfg;
    cfg.fractalType = FractalType::Tricorn;
    cfg.SetCenter(-0.25, 0.5);
    cfg.zoom = 12.0;
    cfg.maxIterations = 900;
    ColorTheme theme(PresetTheme::Cyberpunk);
    const std::wstring jsonW = SerializeViewState(cfg, theme, 2.0f, 0.4f, true, 0xFF010203u);
    TempJsonFile tempCfg;
    {
        std::ofstream f(tempCfg.path, std::ios::binary);
        for (wchar_t wc : jsonW) f.put(static_cast<char>(wc));
    }

    const auto fromJson = ParseMovieCli({
        L"--ascii",
        L"--config", tempCfg.path.wstring(),
        L"--once",
        L"--width", L"80",
        L"--height", L"24"
    });
    ASSERT_TRUE(fromJson.action == CliAction::ConsoleAscii);
    ASSERT_TRUE(fromJson.consoleOptions.start.fractalType == FractalType::Tricorn);
    ASSERT_NEAR(-0.25, fromJson.consoleOptions.start.centerX, 1e-12);
    ASSERT_NEAR(0.5, fromJson.consoleOptions.start.centerY, 1e-12);
    ASSERT_NEAR(12.0, fromJson.consoleOptions.start.zoom, 1e-12);
    ASSERT_EQ(900, fromJson.consoleOptions.start.maxIterations);
    ASSERT_TRUE(fromJson.consoleOptions.theme.CurrentPreset() == PresetTheme::Cyberpunk);
    ASSERT_EQ(80, fromJson.consoleOptions.cols);
    ASSERT_EQ(24, fromJson.consoleOptions.rows);
    ASSERT_TRUE(fromJson.consoleOptions.once);
    ASSERT_FALSE(fromJson.consoleOptions.autoZoom);
}

TEST_CASE(ConsoleAscii, RejectsInvalidAndConflictingArguments)
{
    const std::vector<std::vector<std::wstring>> invalidCases = {
        { L"--ascii", L"--export-movie", L"out.mp4" },
        { L"--ascii", L"--codec", L"hevc" },
        { L"--ascii", L"--resolution", L"1080p" },
        { L"--ascii", L"--motion-smoothing" },
        { L"--ascii", L"-o", L"out.mp4" },
        { L"--export-movie", L"out.mp4", L"--auto-zoom" },
        { L"--export-movie", L"out.mp4", L"--charset", L"dense" },
        { L"--export-movie", L"out.mp4", L"--no-color" },
        { L"--export-movie", L"out.mp4", L"--cols", L"80" },
        { L"--ascii", L"--once", L"--auto-zoom" },
        { L"--ascii", L"--once", L"--frames", L"5" },
        { L"--ascii", L"--once", L"--duration", L"5" },
        { L"--ascii", L"--auto-target", L"--direction", L"out" },
        { L"--ascii", L"--auto-target", L"--target-x", L"0" },
        { L"--ascii", L"--auto-target", L"--julia", L"-0.75,0.1" },
        { L"--ascii", L"--start-zoom", L"10", L"--end-zoom", L"2", L"--direction", L"in" },
        { L"--ascii", L"--start-zoom", L"2", L"--end-zoom", L"10", L"--direction", L"out" },
        { L"--ascii", L"--cols", L"5" },
        { L"--ascii", L"--cols", L"1000" },
        { L"--ascii", L"--rows", L"2" },
        { L"--ascii", L"--rows", L"500" },
        { L"--ascii", L"--width", L"5" },
        { L"--ascii", L"--height", L"2" },
        { L"--ascii", L"--char-aspect", L"0.1" },
        { L"--ascii", L"--char-aspect", L"9.0" },
        { L"--ascii", L"--charset", L"#" },
        { L"--ascii", L"--color", L"invalid" },
        { L"--ascii", L"--fps", L"0" },
        { L"--ascii", L"--fps", L"120" },
        { L"--ascii", L"--frames", L"0" },
        { L"--ascii=1" }
    };

    for (const auto& args : invalidCases)
    {
        const auto res = ParseMovieCli(args);
        ASSERT_TRUE(res.action == CliAction::Error);
        ASSERT_FALSE(res.errorMessage.empty());
    }
}

TEST_CASE(ConsoleAscii, RenderAsciiFrameDistinguishesInteriorExteriorAspectAndColors)
{
    ASSERT_EQ(64, ComputeAsciiPixelHeight(32, 2.0));
    ASSERT_EQ(32, ComputeAsciiPixelHeight(32, 1.0));
    ASSERT_TRUE(IsValidAsciiCharset(" .:-=+*#%@"));
    ASSERT_FALSE(IsValidAsciiCharset("X"));
    ASSERT_FALSE(IsValidAsciiCharset("a\nb"));

    // ANSI 256 color mapping sanity checks.
    ASSERT_EQ(16u, static_cast<unsigned>(RgbToAnsi256(0, 0, 0)));
    ASSERT_EQ(231u, static_cast<unsigned>(RgbToAnsi256(255, 255, 255)));
    ASSERT_EQ(196u, static_cast<unsigned>(RgbToAnsi256(255, 0, 0)));
    ASSERT_EQ(46u, static_cast<unsigned>(RgbToAnsi256(0, 255, 0)));
    ASSERT_EQ(21u, static_cast<unsigned>(RgbToAnsi256(0, 0, 255)));

    // Render a 40x16 Mandelbrot view at 2x vertical pixel aspect (40x32 pixels).
    const int cols = 40;
    const int rows = 16;
    const int pxH = ComputeAsciiPixelHeight(rows, 2.0);
    FractalConfig cfg;
    cfg.fractalType = FractalType::Mandelbrot;
    cfg.SetCenter(-0.75, 0.0);
    cfg.zoom = 1.0;
    cfg.width = cols;
    cfg.height = pxH;
    cfg.maxIterations = 200;

    MandelbrotEngine engine(ExecutionMode::Automatic);
    std::vector<float> iters;
    engine.Compute(cfg, iters);

    ColorTheme theme(PresetTheme::ElectricBlue);
    std::vector<uint32_t> pixels(iters.size());
    theme.Recolor(iters, pixels.data(), pixels.size(), 1.0f, 0.0f, false, 0xFF000000u, cfg.fractalType, cfg.width);

    const std::string ramp = GetAsciiCharset(AsciiCharsetPreset::Standard);
    const std::string mono = RenderAsciiFrame(iters, pixels, cols, pxH, cols, rows, ramp, AsciiColorMode::None);
    ASSERT_TRUE(mono.find('\x1b') == std::string::npos);

    const auto monoLines = SplitLines(mono);
    ASSERT_EQ(static_cast<size_t>(rows), monoLines.size());
    bool sawSpace = false;
    bool sawNonSpace = false;
    for (const auto& line : monoLines)
    {
        ASSERT_EQ(static_cast<size_t>(cols), line.size());
        for (char c : line)
        {
            ASSERT_TRUE(static_cast<unsigned char>(c) >= 0x20u && static_cast<unsigned char>(c) <= 0x7Eu);
            if (c == ' ') sawSpace = true;
            else sawNonSpace = true;
        }
    }
    ASSERT_TRUE(sawSpace);
    ASSERT_TRUE(sawNonSpace);

    // TrueColor mode emits 24-bit ANSI sequences and strips cleanly back to the exact monochrome ASCII frame.
    const std::string tc = RenderAsciiFrame(iters, pixels, cols, pxH, cols, rows, ramp, AsciiColorMode::TrueColor);
    ASSERT_TRUE(tc.find("\x1b[38;2;") != std::string::npos);
    ASSERT_TRUE(tc.find("\x1b[0m") != std::string::npos);
    ASSERT_TRUE(StripAnsiEscapes(tc) == mono);

    // 256-color mode emits 8-bit ANSI sequences and strips cleanly back to the exact monochrome ASCII frame.
    const std::string c256 = RenderAsciiFrame(iters, pixels, cols, pxH, cols, rows, ramp, AsciiColorMode::Ansi256);
    ASSERT_TRUE(c256.find("\x1b[38;5;") != std::string::npos);
    ASSERT_TRUE(c256.find("\x1b[0m") != std::string::npos);
    ASSERT_TRUE(StripAnsiEscapes(c256) == mono);
}

TEST_CASE(ConsoleAscii, AutoZoomAndSeededAutoTargetAdvanceDeterministically)
{
    ConsoleAsciiOptions opts;
    opts.start.fractalType = FractalType::Mandelbrot;
    opts.start.SetCenter(-0.75, 0.0);
    opts.start.zoom = 1.0;
    opts.autoZoom = true;
    opts.zoomIn = true;
    opts.zoomSpeed = 2.0;
    opts.autoTarget = true;
    opts.targetSeed = 424242ULL;
    opts.autoScaleIterations = true;
    opts.baseIterations = 300;
    opts.useGpu = false;
    opts.fps = 15;

    ConsoleAsciiSession sessionA(opts, 48, 18);
    ConsoleAsciiSession sessionB(opts, 48, 18);

    std::string lastFrameA;
    std::string lastFrameB;
    double prevZoom = sessionA.Config().zoom;
    for (int i = 0; i < 12; ++i)
    {
        lastFrameA = sessionA.RenderFrame(static_cast<double>(i) / 15.0, 15.0, AsciiColorMode::None);
        lastFrameB = sessionB.RenderFrame(static_cast<double>(i) / 15.0, 15.0, AsciiColorMode::None);
        ASSERT_TRUE(lastFrameA == lastFrameB);
        ASSERT_TRUE(sessionA.StepAutoZoom(0.15));
        ASSERT_TRUE(sessionB.StepAutoZoom(0.15));
        ASSERT_GT(sessionA.Config().zoom, prevZoom);
        prevZoom = sessionA.Config().zoom;
    }

    // Verify zoom advanced significantly, iterations scaled up, and auto-target moved away from default center.
    ASSERT_GT(sessionA.Config().zoom, 10.0);
    ASSERT_GT(sessionA.Config().maxIterations, 300);
    ASSERT_NEAR(sessionA.TargetX().ToDouble(), sessionB.TargetX().ToDouble(), 1e-15);
    ASSERT_NEAR(sessionA.TargetY().ToDouble(), sessionB.TargetY().ToDouble(), 1e-15);
    const double movedDist = std::hypot(
        sessionA.TargetX().ToDouble() - (-0.75),
        sessionA.TargetY().ToDouble() - 0.0);
    ASSERT_GT(movedDist, 1e-4);

    // Explicit --end-zoom stops cleanly on arrival.
    ConsoleAsciiOptions bounded;
    bounded.start.SetCenter(-0.7436438870371587, 0.1318259042053119);
    bounded.start.zoom = 1.0;
    bounded.targetX = bounded.start.hpCenterX;
    bounded.targetY = bounded.start.hpCenterY;
    bounded.autoZoom = true;
    bounded.zoomIn = true;
    bounded.zoomSpeed = 4.0;
    bounded.endZoom = 8.0;
    bounded.useGpu = false;

    ConsoleAsciiSession boundedSession(bounded, 40, 14);
    int steps = 0;
    while (boundedSession.IsZooming() && steps < 50)
    {
        boundedSession.StepAutoZoom(0.1);
        ++steps;
    }
    ASSERT_FALSE(boundedSession.IsZooming());
    ASSERT_TRUE(boundedSession.ReachedZoomBound());
    ASSERT_NEAR(8.0, boundedSession.Config().zoom, 1e-9);
}

TEST_CASE(ConsoleAscii, InteractiveKeyActionsNavigateAndToggleLiveState)
{
    ConsoleAsciiOptions opts;
    opts.start.SetCenter(-0.75, 0.0);
    opts.start.zoom = 2.0;
    opts.targetX = opts.start.hpCenterX;
    opts.targetY = opts.start.hpCenterY;
    opts.autoZoom = false;
    opts.useGpu = false;

    ConsoleAsciiSession session(opts, 40, 14);
    ASSERT_FALSE(session.RenderFrame(0.0, 0.0, AsciiColorMode::None).empty());

    // Play / pause auto-zoom.
    ASSERT_FALSE(session.IsZooming());
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ToggleAutoZoom));
    ASSERT_TRUE(session.IsZooming());
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ToggleAutoZoom));
    ASSERT_FALSE(session.IsZooming());

    // Toggle auto-target & direction.
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ToggleAutoTarget));
    ASSERT_TRUE(session.IsAutoTarget());
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ToggleDirection));
    ASSERT_FALSE(session.IsZoomIn());

    // Speed adjustments.
    const double initialSpeed = session.ZoomSpeed();
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::SpeedUp));
    ASSERT_GT(session.ZoomSpeed(), initialSpeed);
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::SpeedDown));
    ASSERT_NEAR(initialSpeed, session.ZoomSpeed(), 1e-12);

    // Step zoom & pan.
    const double beforeZoom = session.Config().zoom;
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ZoomInStep));
    ASSERT_NEAR(beforeZoom * 2.0, session.Config().zoom, 1e-9);
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ZoomOutStep));
    ASSERT_NEAR(beforeZoom, session.Config().zoom, 1e-9);

    const double beforeX = session.Config().centerX;
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::PanRight));
    ASSERT_GT(session.Config().centerX, beforeX);

    // Cycle fractal, theme, color mode, charset, HUD, reset, quit.
    const FractalType initialFractal = session.Config().fractalType;
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::NextFractal));
    ASSERT_TRUE(session.Config().fractalType != initialFractal);

    const PresetTheme initialTheme = session.Theme().CurrentPreset();
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::NextTheme));
    ASSERT_TRUE(session.Theme().CurrentPreset() != initialTheme);

    const std::string initialCharset(session.ActiveCharset());
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::CycleCharset));
    ASSERT_TRUE(std::string(session.ActiveCharset()) != initialCharset);

    ASSERT_TRUE(session.ShowHud());
    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ToggleHud));
    ASSERT_FALSE(session.ShowHud());

    ASSERT_TRUE(session.ApplyKeyAction(ConsoleKeyAction::ResetView));
    ASSERT_TRUE(session.Config().fractalType == FractalType::Mandelbrot);
    ASSERT_NEAR(-0.75, session.Config().centerX, 1e-12);
    ASSERT_NEAR(2.0, session.Config().zoom, 1e-12);

    ASSERT_FALSE(session.ApplyKeyAction(ConsoleKeyAction::Quit));
}

TEST_CASE(ConsoleAscii, EndToEndRunConsoleAsciiCliSnapshotsLiveZoomAndCancel)
{
    // 1. Single static ASCII snapshot.
    const auto snapParsed = ParseMovieCli({
        L"--ascii",
        L"--once",
        L"--cols", L"50",
        L"--rows", L"16",
        L"--no-color",
        L"--cpu"
    });
    ASSERT_TRUE(snapParsed.action == CliAction::ConsoleAscii);

    std::ostringstream snapOut;
    std::wostringstream snapErr;
    ASSERT_EQ(0, RunConsoleAsciiCli(snapParsed.consoleOptions, snapOut, snapErr, nullptr, false));
    ASSERT_TRUE(snapErr.str().empty());
    const auto snapLines = SplitLines(snapOut.str());
    // 16 fractal lines + 1 HUD summary line.
    ASSERT_EQ(17u, snapLines.size());
    ASSERT_EQ(50u, snapLines[0].size());
    ASSERT_TRUE(snapLines[16].find("Mandelbrot") != std::string::npos);
    ASSERT_TRUE(snapLines[16].find("Zoom: 1.00x") != std::string::npos);

    // 2. Multi-frame non-interactive auto-zoom with auto-target.
    const auto zoomParsed = ParseMovieCli({
        L"--ascii",
        L"--auto-zoom",
        L"--auto-target",
        L"--seed", L"77",
        L"--cols", L"72",
        L"--rows", L"12",
        L"--frames", L"4",
        L"--fps", L"10",
        L"--zoom-speed", L"3.0",
        L"--no-color",
        L"--cpu"
    });
    ASSERT_TRUE(zoomParsed.action == CliAction::ConsoleAscii);

    std::ostringstream zoomOut;
    std::wostringstream zoomErr;
    ASSERT_EQ(0, RunConsoleAsciiCli(zoomParsed.consoleOptions, zoomOut, zoomErr, nullptr, false));
    ASSERT_TRUE(zoomErr.str().empty());
    const std::string zoomText = zoomOut.str();
    ASSERT_TRUE(zoomText.find("AUTO-ZOOM IN") != std::string::npos);
    ASSERT_TRUE(zoomText.find("AUTO-TARGET") != std::string::npos);
    ASSERT_TRUE(zoomText.find("Zoom: 1.00x") != std::string::npos);
    ASSERT_TRUE(zoomText.find("Zoom: 1.87x") != std::string::npos || zoomText.find("Zoom: 1.52x") != std::string::npos);

    // 3. Interactive terminal mode emits cursor-home, TrueColor ANSI, and controls HUD.
    const auto ttyParsed = ParseMovieCli({
        L"--ascii",
        L"--auto-zoom",
        L"--cols", L"44",
        L"--rows", L"14",
        L"--frames", L"2",
        L"--fps", L"60",
        L"--color", L"truecolor",
        L"--cpu"
    });
    ASSERT_TRUE(ttyParsed.action == CliAction::ConsoleAscii);
    std::ostringstream ttyOut;
    std::wostringstream ttyErr;
    ASSERT_EQ(0, RunConsoleAsciiCli(ttyParsed.consoleOptions, ttyOut, ttyErr, nullptr, true));
    ASSERT_TRUE(ttyErr.str().empty());
    const std::string ttyText = ttyOut.str();
    ASSERT_TRUE(ttyText.find("\x1b[H") != std::string::npos);
    ASSERT_TRUE(ttyText.find("\x1b[38;2;") != std::string::npos);
    ASSERT_TRUE(ttyText.find("[Space] Zoom") != std::string::npos);

    // 4. Quiet mode suppresses HUD chrome.
    const auto quietParsed = ParseMovieCli({
        L"--ascii",
        L"--once",
        L"--cols", L"30",
        L"--rows", L"10",
        L"--no-color",
        L"--quiet",
        L"--cpu"
    });
    std::ostringstream quietOut;
    std::wostringstream quietErr;
    ASSERT_EQ(0, RunConsoleAsciiCli(quietParsed.consoleOptions, quietOut, quietErr, nullptr, false));
    ASSERT_EQ(10u, SplitLines(quietOut.str()).size());

    // 5. External cancellation exits cleanly with code 130.
    std::atomic<bool> cancel{ true };
    std::ostringstream cancelOut;
    std::wostringstream cancelErr;
    ASSERT_EQ(130, RunConsoleAsciiCli(zoomParsed.consoleOptions, cancelOut, cancelErr, &cancel, false));
}
