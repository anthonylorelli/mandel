#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ConsoleLauncher.h"

#include <cstdio>
#include <string>

namespace
{
    std::wstring GetCurrentModulePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0)
            {
                return {};
            }
            if (copied < buffer.size() - 1)
            {
                buffer.resize(copied);
                return buffer;
            }
            if (buffer.size() >= 32768)
            {
                buffer.resize(copied);
                return buffer;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    BOOL WINAPI IgnoreCtrlHandler(DWORD ctrlType) noexcept
    {
        // Let the child MandelbrotViewer.exe handle Ctrl+C / Ctrl+Break so it can flush output,
        // restore terminal state (--ascii), and exit cleanly with code 130 before this shim returns.
        return (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) ? TRUE : FALSE;
    }
}

int wmain()
{
    const std::wstring selfPath = GetCurrentModulePath();
    const std::wstring exePath = Mandelbrot::CliLauncher::BuildSiblingExePath(selfPath);
    const std::wstring_view rawArgs = Mandelbrot::CliLauncher::ExtractCommandLineArguments(GetCommandLineW());
    std::wstring commandLine = Mandelbrot::CliLauncher::BuildChildCommandLine(exePath, rawArgs);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    const bool waitForChild = Mandelbrot::CliLauncher::ShouldWaitForChild(rawArgs);
    const BOOL inheritHandles = waitForChild ? TRUE : FALSE;
    const DWORD creationFlags = waitForChild ? 0u : DETACHED_PROCESS;

    if (waitForChild)
    {
        SetConsoleCtrlHandler(IgnoreCtrlHandler, TRUE);
    }

    const BOOL created = CreateProcessW(
        exePath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        inheritHandles,
        creationFlags,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    if (!created)
    {
        const DWORD err = GetLastError();
        if (waitForChild)
        {
            SetConsoleCtrlHandler(IgnoreCtrlHandler, FALSE);
        }
        std::fwprintf(stderr, L"Error: Failed to launch '%ls' (Win32 error %lu).\n", exePath.c_str(), err);
        std::fflush(stderr);
        return 1;
    }

    CloseHandle(processInfo.hThread);

    DWORD exitCode = 0;
    if (waitForChild)
    {
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        SetConsoleCtrlHandler(IgnoreCtrlHandler, FALSE);
    }

    CloseHandle(processInfo.hProcess);
    return static_cast<int>(exitCode);
}
