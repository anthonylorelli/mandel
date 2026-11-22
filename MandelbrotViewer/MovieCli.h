#pragma once
#include "ConsoleAscii.h"
#include "MoviePlan.h"
#include <atomic>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace Mandelbrot
{
    struct MovieProgress;

    enum class CliAction
    {
        LaunchGui,
        ShowHelp,
        ExportMovie,
        ConsoleAscii,
        Error
    };

    struct MovieCliOptions
    {
        MoviePlan plan;
        std::wstring outputPath;
        bool quiet{ false };
    };

    struct CliParseResult
    {
        CliAction action{ CliAction::LaunchGui };
        MovieCliOptions options;
        ConsoleAsciiOptions consoleOptions;
        std::wstring errorMessage;
    };

    // Formats CLI help/usage text.
    std::wstring MovieCliUsage();

    // Formats a single console progress status line showing frames, percentage, zoom level, and elapsed time.
    std::wstring FormatMovieCliProgressLine(const MovieProgress& progress, const MoviePlan& plan);

    // Loads view, coordinates, and palette settings from a JSON document produced by SerializeViewState().
    // Throws std::invalid_argument on malformed JSON or invalid values.
    void ApplyViewStateJson(const std::string& jsonUtf8, MoviePlan& plan, bool& outLoadedIterations);

    // Parses command-line arguments (excluding the executable name argv[0]).
    CliParseResult ParseMovieCli(const std::vector<std::wstring>& args);

    // Runs a headless zoom movie export, reporting progress to `out` and errors to `err`.
    // Returns 0 on success, 1 on failure, or 130 if canceled via `externalCancel`.
    int RunMovieExportCli(
        const MovieCliOptions& options,
        std::wostream& out,
        std::wostream& err,
        const std::atomic<bool>* externalCancel = nullptr,
        bool interactiveTerminal = false);

#ifdef _WIN32
    // Splits a full Windows command line (from GetCommandLineW()) into arguments excluding argv[0].
    std::vector<std::wstring> SplitWindowsCommandLine(const wchar_t* commandLine);

    // Handles command-line arguments from wWinMain.
    // Returns an exit code when handled in CLI mode, or std::nullopt to launch the GUI.
    std::optional<int> TryRunMovieCliFromWinMain();
#endif
}
