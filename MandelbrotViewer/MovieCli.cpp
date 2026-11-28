#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "shell32.lib")
#endif

#include "MovieCli.h"
#include "MovieExport.h"
#include "ViewStateJson.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace Mandelbrot
{
    namespace
    {
        std::wstring FormatTenthsTimecode(uint64_t tenths)
        {
            std::wostringstream text;
            text.imbue(std::locale::classic());
            text << std::setfill(L'0') << std::setw(2) << (tenths / 36000) << L":"
                 << std::setw(2) << ((tenths / 600) % 60) << L":"
                 << std::setw(2) << ((tenths / 10) % 60) << L"."
                 << (tenths % 10);
            return text.str();
        }

        std::wstring FormatElapsedTime(double seconds)
        {
            return FormatTenthsTimecode(static_cast<uint64_t>(std::max(0.0, std::floor(seconds * 10.0 + 0.5))));
        }

        std::wstring FormatZoomShort(double zoom)
        {
            std::wostringstream ss;
            ss.imbue(std::locale::classic());
            if (zoom < 10000.0 && zoom >= 0.01)
            {
                ss << std::fixed << std::setprecision(zoom < 10.0 ? 2 : 1) << zoom << L"x";
            }
            else
            {
                ss << std::scientific << std::setprecision(2) << zoom << L"x";
            }
            return ss.str();
        }

        size_t QueryInteractiveConsoleWidth() noexcept
        {
#ifdef _WIN32
            const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info))
            {
                const int cols = static_cast<int>(info.srWindow.Right) - static_cast<int>(info.srWindow.Left) + 1;
                if (cols > 1)
                {
                    return static_cast<size_t>(cols - 1);
                }
            }
#endif
            return 0;
        }

#ifdef _WIN32
        std::atomic<bool> g_consoleCancelRequested{ false };

        BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept
        {
            switch (ctrlType)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
                g_consoleCancelRequested.store(true, std::memory_order_relaxed);
                return TRUE;
            default:
                return FALSE;
            }
        }

        bool IsValidStdHandle(HANDLE handle) noexcept
        {
            return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
        }

        bool IsConsoleHandle(HANDLE handle) noexcept
        {
            DWORD mode = 0;
            return IsValidStdHandle(handle) && GetConsoleMode(handle, &mode) != 0;
        }

        bool IsRedirectedStdHandle(HANDLE handle, FILE* stream) noexcept
        {
            if (!IsValidStdHandle(handle) || !stream || _fileno(stream) < 0)
            {
                return false;
            }
            const DWORD fileType = GetFileType(handle);
            return fileType == FILE_TYPE_DISK || fileType == FILE_TYPE_PIPE || !IsConsoleHandle(handle);
        }

        struct ScopedParentConsole
        {
            bool attached{ false };
            bool stdoutIsConsole{ false };

            explicit ScopedParentConsole(bool allowAllocConsole = false) noexcept
            {
                const bool redirectedStdout = IsRedirectedStdHandle(GetStdHandle(STD_OUTPUT_HANDLE), stdout);
                const bool redirectedStderr = IsRedirectedStdHandle(GetStdHandle(STD_ERROR_HANDLE), stderr);
                const bool redirectedStdin = IsRedirectedStdHandle(GetStdHandle(STD_INPUT_HANDLE), stdin);
                if (GetConsoleWindow() == nullptr)
                {
                    bool acquired = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
                    if (!acquired && allowAllocConsole && !redirectedStdout)
                    {
                        acquired = AllocConsole() != 0;
                    }
                    if (acquired)
                    {
                        attached = true;
                        FILE* stream = nullptr;
                        if (!redirectedStdout && freopen_s(&stream, "CONOUT$", "w", stdout) == 0)
                        {
                            std::setvbuf(stdout, nullptr, _IONBF, 0);
                            std::cout.clear();
                            std::wcout.clear();
                        }
                        if (!redirectedStderr && freopen_s(&stream, "CONOUT$", "w", stderr) == 0)
                        {
                            std::setvbuf(stderr, nullptr, _IONBF, 0);
                            std::cerr.clear();
                            std::wcerr.clear();
                        }
                        if (!redirectedStdin && freopen_s(&stream, "CONIN$", "r", stdin) == 0)
                        {
                            std::cin.clear();
                            std::wcin.clear();
                        }
                    }
                }
                // ConsoleAscii writes narrow UTF-8 bytes to std::cout; wide CLI modes write to std::wcout.
                if (!allowAllocConsole && _fileno(stdout) >= 0) (void)_setmode(_fileno(stdout), _O_U8TEXT);
                if (_fileno(stderr) >= 0) (void)_setmode(_fileno(stderr), _O_U8TEXT);
                const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                stdoutIsConsole = IsConsoleHandle(hOut);
                if (attached && stdoutIsConsole && !allowAllocConsole)
                {
                    // If invoked directly via the GUI .exe, the parent shell may have already printed
                    // its prompt on the current row; move to a fresh line before writing output.
                    CONSOLE_SCREEN_BUFFER_INFO info{};
                    if (GetConsoleScreenBufferInfo(hOut, &info) && info.dwCursorPosition.X > 0)
                    {
                        std::wcout << L"\n";
                        std::wcout.flush();
                    }
                }
            }

            ~ScopedParentConsole() noexcept
            {
                std::cout.flush();
                std::wcout.flush();
                std::wcerr.flush();
                std::fflush(stdout);
                std::fflush(stderr);
                if (attached)
                {
                    FreeConsole();
                }
            }
        };
#endif
    }

    std::wstring MovieCliUsage()
    {
        return
            L"MandelbrotViewer - Command-Line Movie Export & Console ASCII Mode\n"
            L"\n"
            L"Usage:\n"
            L"  MandelbrotViewer                                   Launch interactive GUI\n"
            L"  MandelbrotViewer --ascii [options]                 Render live ASCII fractal in terminal\n"
            L"  MandelbrotViewer --export-movie <out.mp4> [options]\n"
            L"  MandelbrotViewer --help | -h | /?\n"
            L"\n"
            L"Console ASCII Options:\n"
            L"  --ascii, --console           Render the fractal in ASCII characters in the terminal\n"
            L"  --auto-zoom / --no-auto-zoom Start with live auto-zoom enabled (or paused)\n"
            L"  --once                       Render a single ASCII frame and exit\n"
            L"  --frames <count>             Render a fixed number of frames (1..1000000) and exit\n"
            L"  --cols <n>                   Terminal width in columns (10..512, default: auto)\n"
            L"  --rows <n>                   Terminal height in rows (5..256, default: auto)\n"
            L"  --char-aspect <float>        Character height/width aspect ratio (0.5..4.0, default: 2.0)\n"
            L"  --charset <preset|chars>     ASCII ramp: standard (default), dense, detailed, simple,\n"
            L"                               or a custom dark-to-bright ASCII string (>= 2 chars)\n"
            L"  --color <mode>               Color mode: auto (default), truecolor, 256, or none\n"
            L"  --no-color                   Disable ANSI color escape codes (plain ASCII)\n"
            L"  --hud / --no-hud             Show (default) or hide live status/telemetry bar\n"
            L"\n"
            L"Movie Export Options:\n"
            L"  --export-movie <path>        Export a zoom movie headlessly to <path>\n"
            L"  -o, --output <path>          Output MP4 file path (alternative to positional path)\n"
            L"  --resolution <preset|WxH>    720p, 1080p (default), 1440p, 4k, 8k, or WxH (max 7680x4320)\n"
            L"  --width <n>                  Override output width (px for movie, cols for --ascii)\n"
            L"  --height <n>                 Override output height (px for movie, rows for --ascii)\n"
            L"  --fps <rate>                 Frame rate: 24/25/30/48/50/60 (movie) or 1..60 (--ascii, default: 15)\n"
            L"  --duration <seconds>         Length in whole seconds (1..3600; movie default: 30, --ascii default: unlimited)\n"
            L"  --codec <h264|hevc>          Video codec: h264/avc (default) or hevc/h265\n"
            L"  --motion-smoothing           Enable analytical motion-blur shutter (default)\n"
            L"  --no-motion-smoothing        Disable motion-blur shutter\n"
            L"  --cycle-speed <cycles/s>     Animate palette phase during movie (-5.0..5.0, default: 0.0)\n"
            L"\n"
            L"View & Source Options (shared by --ascii and --export-movie):\n"
            L"  --config <file.json>         Load view & theme JSON from 'Copy Coordinates & Config'\n"
            L"  --fractal <name>             mandelbrot (default), burning-ship, tricorn, celtic,\n"
            L"                               buffalo, cubic-multibrot, phoenix, newton\n"
            L"  --bookmark <name>            Load starting view from a built-in landmark bookmark\n"
            L"  --center-x <number>          Starting center X (supports deep high-precision decimals)\n"
            L"  --center-y <number>          Starting center Y (supports deep high-precision decimals)\n"
            L"  --start-zoom, --zoom <val>   Starting magnification (0.15..1e300, default: 1.0)\n"
            L"  --julia <x,y>                Render quadratic Julia set for parameter c = x + i*y\n"
            L"  --julia-x <number>           Enable Julia set and set parameter real part\n"
            L"  --julia-y <number>           Enable Julia set and set parameter imaginary part\n"
            L"\n"
            L"Zoom Motion & Target Options:\n"
            L"  --target-x <number>          Target center X (default: starting center X)\n"
            L"  --target-y <number>          Target center Y (default: starting center Y)\n"
            L"  --direction <in|out>         Zoom direction (default: in)\n"
            L"  --zoom-speed <octaves/s>     Zoom speed in octaves/sec (0.2..5.0, default: 1.0)\n"
            L"  --end-zoom <val>             Explicit ending magnification (0.15..1e300)\n"
            L"  --auto-target                Automatically pick detailed regions while zooming in\n"
            L"  --no-auto-target             Use fixed target coordinates (default)\n"
            L"  --seed <uint64>              Deterministic seed for --auto-target\n"
            L"\n"
            L"Rendering & Color Options:\n"
            L"  --iterations <count>         Fixed iteration budget (or base floor with --auto-iterations)\n"
            L"  --auto-iterations            Scale iteration budget automatically with zoom depth\n"
            L"  --no-auto-iterations         Keep iteration budget fixed across frames\n"
            L"  --smooth / --no-smooth       Smooth continuous coloring (default) or discrete bands\n"
            L"  --coloring <style>           escape-time (default), stripe, trap-point, trap-cross, trap-ring\n"
            L"  --gpu / --no-gpu (--cpu)     Use GPU (movie default) or CPU (--ascii default) rendering\n"
            L"  --theme <name>               electric-blue (default), inferno, psychedelic, cyberpunk,\n"
            L"                               viridis, twilight-ocean, sunset-glow, monochrome\n"
            L"  --frequency <float>          Palette cycle frequency (> 0, default: 1.0)\n"
            L"  --phase <float>              Palette phase offset (default: 0.0)\n"
            L"  --invert / --no-invert       Invert palette gradient direction\n"
            L"  --interior-color <hex>       Interior color (#RRGGBB, #AARRGGBB, or 0xAARRGGBB)\n"
            L"  --histogram / --no-histogram Enable or disable auto-contrast histogram equalization (default: off)\n"
            L"  --lighting / --no-lighting   Enable or disable 3D directional relief lighting (default: off)\n"
            L"  --light-azimuth <deg>        3D light angle in degrees (default: 135, top-left)\n"
            L"  --light-elevation <deg>      3D light elevation in degrees (5..85, default: 45)\n"
            L"  --light-strength <float>     3D relief bump strength (0..2.5, default: 1.0)\n"
            L"  -q, --quiet                  Suppress progress / HUD output (errors still written to stderr)\n"
            L"\n"
            L"Interactive Console Keys (--ascii):\n"
            L"  Space / P     Start or pause live auto-zoom    E             Toggle auto-pick target\n"
            L"  V             Flip zoom direction (in / out)   [ / ]         Decrease / increase zoom speed\n"
            L"  WASD / Arrows Pan viewport                     + / -         Step zoom in / out\n"
            L"  F / T / C / M Cycle fractal / theme / color / charset\n"
            L"  H / R         Toggle HUD / reset view          Q / Esc       Quit\n"
            L"\n"
            L"Examples:\n"
            L"  MandelbrotViewer --ascii --auto-zoom --auto-target\n"
            L"  MandelbrotViewer --ascii --bookmark \"Seahorse Valley\" --start-zoom 1 --auto-zoom --theme inferno\n"
            L"  MandelbrotViewer --ascii --once --cols 100 --rows 32 --no-color\n"
            L"  MandelbrotViewer --export-movie zoom.mp4 --bookmark \"Seahorse Valley\" --start-zoom 1 --end-zoom 85000 --duration 15\n"
            L"  MandelbrotViewer --export-movie auto.mp4 --auto-target --resolution 4k --fps 60 --duration 30 --theme inferno\n"
            L"  MandelbrotViewer --export-movie custom.mp4 --config view.json --direction in --zoom-speed 1.5 --duration 20\n";
    }

    std::wstring FormatMovieCliProgressLine(const MovieProgress& progress, const MoviePlan& plan)
    {
        const uint32_t total = (progress.total > 0) ? progress.total : plan.FrameCount();
        const uint32_t completed = std::min(progress.completed, total);
        const double exactFrames = progress.finished
            ? static_cast<double>(completed)
            : std::min(
                static_cast<double>(total),
                static_cast<double>(completed) + static_cast<double>(std::clamp(progress.frameFraction, 0.0f, 1.0f)));
        const double pct = (total > 0) ? (100.0 * exactFrames / static_cast<double>(total)) : 100.0;

        double zoom = progress.currentZoom;
        if (!std::isfinite(zoom) || zoom < MandelbrotEngine::MinZoom || zoom > MandelbrotEngine::MaxZoom)
        {
            const double frameIdx = (completed == 0) ? 0.0 : static_cast<double>(completed - 1);
            zoom = plan.ZoomAt(frameIdx);
        }
        else if (completed >= total && total > 0)
        {
            zoom = plan.endZoom;
        }

        std::wostringstream line;
        line.imbue(std::locale::classic());
        if (progress.planning)
        {
            line << L"Planning auto-targets... | Frames: " << completed << L"/" << total
                 << L" | Zoom: " << FormatZoomShort(zoom)
                 << L" | Elapsed: " << FormatElapsedTime(progress.elapsedSeconds);
        }
        else if (!progress.finished && completed >= total && total > 0)
        {
            line << L"Finalizing MP4... | Frames: " << completed << L"/" << total
                 << L" (100.0%) | Zoom: " << FormatZoomShort(zoom)
                 << L" | Elapsed: " << FormatElapsedTime(progress.elapsedSeconds);
        }
        else
        {
            line << L"Frames: " << completed << L"/" << total
                 << L" (" << std::fixed << std::setprecision(1) << std::setw(5) << pct << L"%) | Zoom: "
                 << FormatZoomShort(zoom)
                 << L" | Elapsed: " << FormatElapsedTime(progress.elapsedSeconds);
        }
        return line.str();
    }

    int RunMovieExportCli(
        const MovieCliOptions& options,
        std::wostream& out,
        std::wostream& err,
        const std::atomic<bool>* externalCancel,
        bool interactiveTerminal)
    {
        const MoviePlan& plan = options.plan;
        if (!options.quiet)
        {
            const std::wstring fractalName = plan.start.juliaSet
                ? (plan.start.fractalType == FractalType::BurningShip ? L"Burning Ship Julia Set" : L"Julia Set")
                : GetFractalTypeName(plan.start.fractalType);
            const wchar_t* codecName = (plan.codec == MovieCodec::Hevc) ? L"HEVC" : L"H.264";
            std::wostringstream hdr;
            hdr.imbue(std::locale::classic());
            hdr << L"Exporting zoom movie: " << options.outputPath << L"\n"
                << L"  Fractal: " << fractalName
                << L" | " << plan.width << L"x" << plan.height
                << L" @ " << plan.fps << L" fps | " << plan.seconds << L"s (" << plan.FrameCount() << L" frames) | "
                << codecName << L"\n"
                << L"  Zoom: " << FormatZoomShort(plan.start.zoom) << L" -> " << FormatZoomShort(plan.endZoom);
            if (plan.autoRetarget)
            {
                hdr << L" | Target: Auto-Pick (seed " << plan.targetSeed << L")\n";
            }
            else
            {
                hdr << L" | Target: (" << plan.targetX.ToWString(12) << L", " << plan.targetY.ToWString(12) << L")\n";
            }
            out << hdr.str();
            out.flush();
        }

        try
        {
            MovieExporter exporter(plan, options.outputPath);
            if (externalCancel && externalCancel->load(std::memory_order_relaxed))
            {
                exporter.Cancel();
            }

            auto lastReport = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            int lastPercentBucket = -1;
            int lastPhase = -1; // 0 = planning, 1 = rendering, 2 = finalizing
            uint32_t lastReportedCompleted = std::numeric_limits<uint32_t>::max();
            size_t lastLineLen = 0;
            bool cancelSent = false;
            MovieProgress progress{};

            auto writeProgressLine = [&](const MovieProgress& current)
            {
                std::wstring text = FormatMovieCliProgressLine(current, plan);
                if (interactiveTerminal)
                {
                    const size_t maxCols = QueryInteractiveConsoleWidth();
                    if (maxCols > 0 && text.size() > maxCols)
                    {
                        text.resize(maxCols);
                    }
                    out << L"\r" << text;
                    if (text.size() < lastLineLen)
                    {
                        out << std::wstring(lastLineLen - text.size(), L' ');
                    }
                    lastLineLen = text.size();
                }
                else
                {
                    out << text << L"\n";
                }
                out.flush();
            };

            for (;;)
            {
                if (!cancelSent && externalCancel && externalCancel->load(std::memory_order_relaxed))
                {
                    exporter.Cancel();
                    cancelSent = true;
                }

                progress = exporter.Progress();
                const int phase = progress.planning ? 0 : (progress.completed >= progress.total ? 2 : 1);
                const double exactFrames = std::min(
                    static_cast<double>(progress.total),
                    static_cast<double>(progress.completed) + static_cast<double>(progress.frameFraction));
                const double pct = (progress.total > 0) ? (100.0 * exactFrames / static_cast<double>(progress.total)) : 100.0;
                const int pctBucket = static_cast<int>(pct / 10.0);
                const auto now = std::chrono::steady_clock::now();

                if (!options.quiet && !progress.finished)
                {
                    const bool frameAdvanced = (progress.completed != lastReportedCompleted);
                    const bool shouldPrint = interactiveTerminal
                        ? (phase != lastPhase ||
                           (frameAdvanced && now - lastReport >= std::chrono::milliseconds(100)) ||
                           now - lastReport >= std::chrono::milliseconds(250))
                        : (phase != lastPhase ||
                           pctBucket != lastPercentBucket ||
                           now - lastReport >= std::chrono::seconds(2));

                    if (shouldPrint)
                    {
                        writeProgressLine(progress);
                        lastReport = now;
                        lastPhase = phase;
                        lastPercentBucket = pctBucket;
                        lastReportedCompleted = progress.completed;
                    }
                }

                if (progress.finished)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            exporter.Stop();
            progress = exporter.Progress();

            if (!options.quiet && progress.error.empty() && !progress.canceled)
            {
                writeProgressLine(progress);
            }

            if (interactiveTerminal && !options.quiet && lastLineLen > 0)
            {
                out << L"\n";
                out.flush();
            }

            if (!progress.error.empty())
            {
                err << L"Export failed: " << progress.error << L"\n";
                err.flush();
                return 1;
            }
            if (progress.canceled)
            {
                err << L"Export canceled. No partial movie was saved.\n";
                err.flush();
                return 130;
            }

            if (!options.quiet)
            {
                out << L"Export complete: " << options.outputPath
                    << L" | Frames: " << progress.completed << L"/" << progress.total
                    << L" | Zoom: " << FormatZoomShort(plan.start.zoom) << L" -> " << FormatZoomShort(plan.endZoom)
                    << L" | Elapsed: " << FormatElapsedTime(progress.elapsedSeconds) << L"\n";
                out.flush();
            }
            return 0;
        }
        catch (const std::exception& e)
        {
            err << L"Export failed: " << detail::Utf8ToWide(e.what()) << L"\n";
            err.flush();
            return 1;
        }
        catch (...)
        {
            err << L"Export failed: Unexpected error.\n";
            err.flush();
            return 1;
        }
    }

#ifdef _WIN32
    std::vector<std::wstring> SplitWindowsCommandLine(const wchar_t* commandLine)
    {
        std::vector<std::wstring> args;
        if (!commandLine || *commandLine == L'\0') return args;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(commandLine, &argc);
        if (!argv) return args;

        struct LocalFreeGuard
        {
            LPWSTR* ptr;
            ~LocalFreeGuard() noexcept { if (ptr) LocalFree(ptr); }
        } guard{ argv };

        if (argc > 1)
        {
            args.reserve(static_cast<size_t>(argc - 1));
            for (int i = 1; i < argc; ++i)
            {
                args.emplace_back(argv[i]);
            }
        }
        return args;
    }

    std::optional<int> TryRunMovieCliFromWinMain()
    {
        const auto args = SplitWindowsCommandLine(GetCommandLineW());
        if (args.empty())
        {
            return std::nullopt;
        }

        const CliParseResult parsed = ParseMovieCli(args);
        if (parsed.action == CliAction::LaunchGui)
        {
            return std::nullopt;
        }

        ScopedParentConsole console(parsed.action == CliAction::ConsoleAscii);
        if (parsed.action == CliAction::ShowHelp)
        {
            std::wcout << MovieCliUsage();
            return 0;
        }
        if (parsed.action == CliAction::Error)
        {
            std::wcerr << L"Error: " << parsed.errorMessage << L"\n"
                       << L"Run with --help for usage.\n";
            return 1;
        }

        g_consoleCancelRequested.store(false, std::memory_order_relaxed);
        const BOOL handlerInstalled = SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        struct CtrlHandlerGuard
        {
            BOOL installed;
            ~CtrlHandlerGuard() noexcept
            {
                if (installed) SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
            }
        } handlerGuard{ handlerInstalled };

        if (parsed.action == CliAction::ConsoleAscii)
        {
            return RunConsoleAsciiCli(
                parsed.consoleOptions,
                std::cout,
                std::wcerr,
                &g_consoleCancelRequested,
                console.stdoutIsConsole);
        }

        return RunMovieExportCli(
            parsed.options,
            std::wcout,
            std::wcerr,
            &g_consoleCancelRequested,
            console.stdoutIsConsole);
    }
#endif
}
