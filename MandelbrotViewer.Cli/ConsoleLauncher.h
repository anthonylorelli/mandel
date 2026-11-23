#pragma once
#include <string>
#include <string_view>

namespace Mandelbrot::CliLauncher
{
    inline std::wstring BuildSiblingExePath(
        std::wstring_view launcherPath,
        std::wstring_view targetExeName = L"MandelbrotViewer.exe")
    {
        const size_t sep = launcherPath.find_last_of(L"\\/");
        if (sep == std::wstring_view::npos)
        {
            return std::wstring(targetExeName);
        }
        std::wstring result(launcherPath.substr(0, sep + 1));
        result.append(targetExeName);
        return result;
    }

    // Extracts the raw argument substring after argv[0] from a Win32 command line,
    // preserving all quoting and spacing verbatim for CreateProcessW forwarding.
    inline std::wstring_view ExtractCommandLineArguments(std::wstring_view rawCommandLine) noexcept
    {
        size_t pos = 0;
        const size_t len = rawCommandLine.size();
        while (pos < len && (rawCommandLine[pos] == L' ' || rawCommandLine[pos] == L'\t'))
        {
            ++pos;
        }
        if (pos >= len)
        {
            return {};
        }

        if (rawCommandLine[pos] == L'"')
        {
            ++pos;
            while (pos < len && rawCommandLine[pos] != L'"')
            {
                ++pos;
            }
            if (pos < len)
            {
                ++pos; // consume closing quote
            }
        }
        else
        {
            while (pos < len && rawCommandLine[pos] != L' ' && rawCommandLine[pos] != L'\t')
            {
                ++pos;
            }
        }

        while (pos < len && (rawCommandLine[pos] == L' ' || rawCommandLine[pos] == L'\t'))
        {
            ++pos;
        }
        return rawCommandLine.substr(pos);
    }

    inline std::wstring BuildChildCommandLine(std::wstring_view exePath, std::wstring_view rawArgs)
    {
        std::wstring cmd;
        cmd.reserve(exePath.size() + rawArgs.size() + 3);
        cmd.push_back(L'"');
        cmd.append(exePath);
        cmd.push_back(L'"');
        if (!rawArgs.empty())
        {
            cmd.push_back(L' ');
            cmd.append(rawArgs);
        }
        return cmd;
    }

    inline constexpr bool ShouldWaitForChild(std::wstring_view rawArgs) noexcept
    {
        return !rawArgs.empty();
    }
}
