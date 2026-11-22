#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "ConsoleAscii.h"
#include "FractalCatalog.h"
#include "JuliaView.h"
#include "ViewportNavigation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace Mandelbrot
{
    namespace
    {
        std::string WideToAsciiLossy(std::wstring_view wide)
        {
            std::string ascii;
            ascii.reserve(wide.size());
            for (wchar_t wc : wide)
            {
                const uint32_t cp = static_cast<uint32_t>(wc);
                ascii.push_back(cp <= 127u ? static_cast<char>(cp) : '?');
            }
            return ascii;
        }

        void AppendUint(std::string& out, unsigned value)
        {
            char buf[16];
            size_t n = 0;
            do
            {
                buf[n++] = static_cast<char>('0' + (value % 10u));
                value /= 10u;
            } while (value > 0u);
            while (n > 0u)
            {
                out.push_back(buf[--n]);
            }
        }

        void AppendTrueColorFg(std::string& out, uint8_t r, uint8_t g, uint8_t b)
        {
            out.append("\x1b[38;2;");
            AppendUint(out, r);
            out.push_back(';');
            AppendUint(out, g);
            out.push_back(';');
            AppendUint(out, b);
            out.push_back('m');
        }

        void AppendAnsi256Fg(std::string& out, uint8_t code)
        {
            out.append("\x1b[38;5;");
            AppendUint(out, code);
            out.push_back('m');
        }

        uint64_t SplitMix64(uint64_t x) noexcept
        {
            x += 0x9E3779B97F4A7C15ULL;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            return x ^ (x >> 31);
        }

        std::string FormatZoomAscii(double zoom)
        {
            std::ostringstream ss;
            ss.imbue(std::locale::classic());
            if (zoom < 10000.0 && zoom >= 0.01)
            {
                ss << std::fixed << std::setprecision(zoom < 10.0 ? 2 : 1) << zoom << "x";
            }
            else
            {
                ss << std::scientific << std::setprecision(2) << zoom << "x";
            }
            return ss.str();
        }

        std::string FormatCoordCompact(const BigFloat& value, double zoom)
        {
            const double depth = std::log10(std::max(1.0, zoom));
            const size_t decimals = static_cast<size_t>(std::clamp(std::ceil(depth) + 5.0, 6.0, 18.0));
            return value.ToString(decimals);
        }

        std::string ClipToWidth(std::string text, int maxCols)
        {
            if (maxCols <= 0) return {};
            const size_t limit = static_cast<size_t>(maxCols);
            if (text.size() <= limit) return text;
            if (limit <= 3u)
            {
                text.resize(limit);
                return text;
            }
            text.resize(limit - 3u);
            text.append("...");
            return text;
        }

        bool QueryRealTerminalSize(int& outCols, int& outRows) noexcept
        {
#ifdef _WIN32
            const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info))
            {
                const int cols = static_cast<int>(info.srWindow.Right) - static_cast<int>(info.srWindow.Left) + 1;
                const int rows = static_cast<int>(info.srWindow.Bottom) - static_cast<int>(info.srWindow.Top) + 1;
                if (cols >= MinConsoleCols && rows >= MinConsoleRows)
                {
                    outCols = std::clamp(cols, MinConsoleCols, MaxConsoleCols);
                    outRows = std::clamp(rows, MinConsoleRows, MaxConsoleRows);
                    return true;
                }
            }
#else
            struct winsize ws{};
            if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
                ws.ws_col >= MinConsoleCols && ws.ws_row >= MinConsoleRows)
            {
                outCols = std::clamp(static_cast<int>(ws.ws_col), MinConsoleCols, MaxConsoleCols);
                outRows = std::clamp(static_cast<int>(ws.ws_row), MinConsoleRows, MaxConsoleRows);
                return true;
            }
#endif
            return false;
        }

        ConsoleKeyAction MapAsciiCharToKeyAction(char ch) noexcept
        {
            switch (ch)
            {
            case 3:   // Ctrl+C
            case 27:  // Escape
            case 'q':
            case 'Q':
                return ConsoleKeyAction::Quit;
            case ' ':
            case 'p':
            case 'P':
                return ConsoleKeyAction::ToggleAutoZoom;
            case 'e':
            case 'E':
                return ConsoleKeyAction::ToggleAutoTarget;
            case 'v':
            case 'V':
                return ConsoleKeyAction::ToggleDirection;
            case '+':
            case '=':
                return ConsoleKeyAction::ZoomInStep;
            case '-':
            case '_':
                return ConsoleKeyAction::ZoomOutStep;
            case ']':
            case '}':
                return ConsoleKeyAction::SpeedUp;
            case '[':
            case '{':
                return ConsoleKeyAction::SpeedDown;
            case 'w':
            case 'W':
                return ConsoleKeyAction::PanUp;
            case 's':
            case 'S':
                return ConsoleKeyAction::PanDown;
            case 'a':
            case 'A':
                return ConsoleKeyAction::PanLeft;
            case 'd':
            case 'D':
                return ConsoleKeyAction::PanRight;
            case 'f':
            case 'F':
                return ConsoleKeyAction::NextFractal;
            case 't':
            case 'T':
                return ConsoleKeyAction::NextTheme;
            case 'c':
            case 'C':
                return ConsoleKeyAction::CycleColorMode;
            case 'm':
            case 'M':
                return ConsoleKeyAction::CycleCharset;
            case 'h':
            case 'H':
                return ConsoleKeyAction::ToggleHud;
            case 'r':
            case 'R':
                return ConsoleKeyAction::ResetView;
            default:
                return ConsoleKeyAction::None;
            }
        }

        class ScopedTerminalSession
        {
        public:
            ScopedTerminalSession(std::ostream& out, bool interactive, bool realStdout) noexcept
                : m_out(out)
                , m_interactive(interactive)
                , m_realStdout(realStdout)
            {
                if (!m_interactive) return;

                if (!m_realStdout)
                {
                    // Stream-backed interactive mode (e.g. unit tests): enable VT sequences without touching OS handles.
                    m_vtEnabled = true;
                    return;
                }

#ifdef _WIN32
                m_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                if (m_hOut != nullptr && m_hOut != INVALID_HANDLE_VALUE && GetConsoleMode(m_hOut, &m_savedOutMode))
                {
                    m_hasSavedOutMode = true;
                    DWORD newMode = m_savedOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
#ifdef DISABLE_NEWLINE_AUTO_WRAP
                    if (SetConsoleMode(m_hOut, newMode | DISABLE_NEWLINE_AUTO_WRAP))
                    {
                        m_vtEnabled = true;
                    }
                    else
#endif
                    if (SetConsoleMode(m_hOut, newMode))
                    {
                        m_vtEnabled = true;
                    }
                }

                m_hIn = GetStdHandle(STD_INPUT_HANDLE);
                if (m_hIn != nullptr && m_hIn != INVALID_HANDLE_VALUE && GetConsoleMode(m_hIn, &m_savedInMode))
                {
                    m_hasSavedInMode = true;
                    const DWORD newInMode = (m_savedInMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) |
                        ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT;
                    SetConsoleMode(m_hIn, newInMode);
                }
#else
                if (isatty(STDOUT_FILENO))
                {
                    m_vtEnabled = true;
                }
                if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &m_savedTermios) == 0)
                {
                    m_hasSavedTermios = true;
                    termios raw = m_savedTermios;
                    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
                    raw.c_cc[VMIN] = 0;
                    raw.c_cc[VTIME] = 0;
                    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
                }
#endif

                if (m_vtEnabled)
                {
                    m_out << "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
                    m_out.flush();
                    m_altScreenEntered = true;
                }
            }

            ~ScopedTerminalSession() noexcept
            {
                if (m_altScreenEntered)
                {
                    m_out << "\x1b[0m\x1b[?25h\x1b[?1049l";
                    m_out.flush();
                }
#ifdef _WIN32
                if (m_hasSavedInMode && m_hIn != nullptr && m_hIn != INVALID_HANDLE_VALUE)
                {
                    SetConsoleMode(m_hIn, m_savedInMode);
                }
                if (m_hasSavedOutMode && m_hOut != nullptr && m_hOut != INVALID_HANDLE_VALUE)
                {
                    SetConsoleMode(m_hOut, m_savedOutMode);
                }
#else
                if (m_hasSavedTermios)
                {
                    tcsetattr(STDIN_FILENO, TCSANOW, &m_savedTermios);
                }
#endif
            }

            bool VtEnabled() const noexcept { return m_vtEnabled; }

            void MoveCursorHomeFallback() const noexcept
            {
#ifdef _WIN32
                if (m_realStdout && !m_vtEnabled && m_hOut != nullptr && m_hOut != INVALID_HANDLE_VALUE)
                {
                    COORD origin{ 0, 0 };
                    SetConsoleCursorPosition(m_hOut, origin);
                }
#endif
            }

            ConsoleKeyAction PollKey() const noexcept
            {
                if (!m_interactive || !m_realStdout) return ConsoleKeyAction::None;

#ifdef _WIN32
                if (m_hIn == nullptr || m_hIn == INVALID_HANDLE_VALUE) return ConsoleKeyAction::None;
                DWORD available = 0;
                if (!GetNumberOfConsoleInputEvents(m_hIn, &available) || available == 0)
                {
                    return ConsoleKeyAction::None;
                }

                while (available > 0)
                {
                    INPUT_RECORD record{};
                    DWORD readCount = 0;
                    if (!ReadConsoleInputW(m_hIn, &record, 1, &readCount) || readCount == 0)
                    {
                        break;
                    }
                    --available;

                    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
                    {
                        continue;
                    }

                    const WORD vk = record.Event.KeyEvent.wVirtualKeyCode;
                    switch (vk)
                    {
                    case VK_ESCAPE: return ConsoleKeyAction::Quit;
                    case VK_UP:     return ConsoleKeyAction::PanUp;
                    case VK_DOWN:   return ConsoleKeyAction::PanDown;
                    case VK_LEFT:   return ConsoleKeyAction::PanLeft;
                    case VK_RIGHT:  return ConsoleKeyAction::PanRight;
                    case VK_ADD:    return ConsoleKeyAction::ZoomInStep;
                    case VK_SUBTRACT: return ConsoleKeyAction::ZoomOutStep;
                    default: break;
                    }

                    const wchar_t wc = record.Event.KeyEvent.uChar.UnicodeChar;
                    if (wc > 0 && wc <= 127)
                    {
                        const ConsoleKeyAction action = MapAsciiCharToKeyAction(static_cast<char>(wc));
                        if (action != ConsoleKeyAction::None) return action;
                    }
                }
#else
                if (!isatty(STDIN_FILENO)) return ConsoleKeyAction::None;
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(STDIN_FILENO, &readSet);
                timeval tv{};
                if (select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &tv) <= 0)
                {
                    return ConsoleKeyAction::None;
                }
                char buf[8]{};
                const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) return ConsoleKeyAction::None;
                if (buf[0] == '\x1b')
                {
                    if (n >= 3 && buf[1] == '[')
                    {
                        switch (buf[2])
                        {
                        case 'A': return ConsoleKeyAction::PanUp;
                        case 'B': return ConsoleKeyAction::PanDown;
                        case 'C': return ConsoleKeyAction::PanRight;
                        case 'D': return ConsoleKeyAction::PanLeft;
                        default: return ConsoleKeyAction::None;
                        }
                    }
                    return ConsoleKeyAction::Quit;
                }
                return MapAsciiCharToKeyAction(buf[0]);
#endif
                return ConsoleKeyAction::None;
            }

        private:
            std::ostream& m_out;
            bool m_interactive{ false };
            bool m_realStdout{ false };
            bool m_vtEnabled{ false };
            bool m_altScreenEntered{ false };
#ifdef _WIN32
            HANDLE m_hOut{ nullptr };
            HANDLE m_hIn{ nullptr };
            DWORD m_savedOutMode{ 0 };
            DWORD m_savedInMode{ 0 };
            bool m_hasSavedOutMode{ false };
            bool m_hasSavedInMode{ false };
#else
            termios m_savedTermios{};
            bool m_hasSavedTermios{ false };
#endif
        };

        int ComputeHudRowCount(int termRows, bool showHud, bool interactive) noexcept
        {
            if (!showHud) return 0;
            if (!interactive) return 1;
            if (termRows >= 8) return 2;
            if (termRows >= 5) return 1;
            return 0;
        }

        AsciiColorMode ResolveColorMode(AsciiColorMode preferred, bool interactive, bool vtEnabled) noexcept
        {
            if (preferred == AsciiColorMode::Auto)
            {
                return (interactive && vtEnabled) ? AsciiColorMode::TrueColor : AsciiColorMode::None;
            }
            if (interactive && !vtEnabled)
            {
                return AsciiColorMode::None;
            }
            return preferred;
        }
    }

    std::string GetAsciiCharset(AsciiCharsetPreset preset)
    {
        switch (preset)
        {
        case AsciiCharsetPreset::Standard:
            return " .:-=+*#%@";
        case AsciiCharsetPreset::Dense:
            return " .,:;i1tfLCG08@";
        case AsciiCharsetPreset::Detailed:
            return " .'`^\",:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
        case AsciiCharsetPreset::Simple:
            return " .oO@#";
        case AsciiCharsetPreset::Custom:
        default:
            return " .:-=+*#%@";
        }
    }

    const char* GetAsciiCharsetPresetName(AsciiCharsetPreset preset) noexcept
    {
        switch (preset)
        {
        case AsciiCharsetPreset::Standard: return "standard";
        case AsciiCharsetPreset::Dense:    return "dense";
        case AsciiCharsetPreset::Detailed: return "detailed";
        case AsciiCharsetPreset::Simple:   return "simple";
        case AsciiCharsetPreset::Custom:   return "custom";
        default:                           return "standard";
        }
    }

    const char* GetAsciiColorModeName(AsciiColorMode mode) noexcept
    {
        switch (mode)
        {
        case AsciiColorMode::Auto:      return "auto";
        case AsciiColorMode::None:      return "mono";
        case AsciiColorMode::Ansi256:   return "256";
        case AsciiColorMode::TrueColor: return "truecolor";
        default:                        return "mono";
        }
    }

    bool IsValidAsciiCharset(std::string_view chars) noexcept
    {
        if (chars.size() < 2u) return false;
        for (char c : chars)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20u || uc > 0x7Eu) return false;
        }
        return true;
    }

    uint8_t RgbToAnsi256(uint8_t r, uint8_t g, uint8_t b) noexcept
    {
        // Compare closest 6x6x6 color-cube entry against closest 24-level grayscale ramp entry.
        static constexpr int kCubeLevels[6] = { 0, 95, 135, 175, 215, 255 };
        auto nearestCubeIdx = [](uint8_t channel) noexcept -> int
        {
            int bestIdx = 0;
            int bestDist = 1000;
            for (int i = 0; i < 6; ++i)
            {
                const int d = std::abs(static_cast<int>(channel) - kCubeLevels[i]);
                if (d < bestDist)
                {
                    bestDist = d;
                    bestIdx = i;
                }
            }
            return bestIdx;
        };

        const int ri = nearestCubeIdx(r);
        const int gi = nearestCubeIdx(g);
        const int bi = nearestCubeIdx(b);
        const int cr = kCubeLevels[ri];
        const int cg = kCubeLevels[gi];
        const int cb = kCubeLevels[bi];
        const int cubeDist =
            (static_cast<int>(r) - cr) * (static_cast<int>(r) - cr) +
            (static_cast<int>(g) - cg) * (static_cast<int>(g) - cg) +
            (static_cast<int>(b) - cb) * (static_cast<int>(b) - cb);

        const int grayAvg = (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) / 3;
        const int grayIdx = std::clamp((grayAvg - 8 + 5) / 10, 0, 23);
        const int gv = 8 + grayIdx * 10;
        const int grayDist =
            (static_cast<int>(r) - gv) * (static_cast<int>(r) - gv) +
            (static_cast<int>(g) - gv) * (static_cast<int>(g) - gv) +
            (static_cast<int>(b) - gv) * (static_cast<int>(b) - gv);

        if (grayDist < cubeDist)
        {
            return static_cast<uint8_t>(232 + grayIdx);
        }
        return static_cast<uint8_t>(16 + 36 * ri + 6 * gi + bi);
    }

    int ComputeAsciiPixelHeight(int rows, double charAspect) noexcept
    {
        const int safeRows = std::max(1, rows);
        const double clampedAspect = std::clamp(charAspect, MinConsoleCharAspect, MaxConsoleCharAspect);
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(safeRows) * clampedAspect)));
    }

    std::string RenderAsciiFrame(
        const std::vector<float>& iterations,
        const std::vector<uint32_t>& bgraPixels,
        int pixelWidth,
        int pixelHeight,
        int cols,
        int rows,
        std::string_view charset,
        AsciiColorMode resolvedColorMode)
    {
        if (pixelWidth <= 0 || pixelHeight <= 0 || cols <= 0 || rows <= 0)
        {
            return {};
        }
        const size_t expectedPixels = static_cast<size_t>(pixelWidth) * static_cast<size_t>(pixelHeight);
        if (iterations.size() < expectedPixels || bgraPixels.size() < expectedPixels)
        {
            return {};
        }

        const std::string fallbackCharset = GetAsciiCharset(AsciiCharsetPreset::Standard);
        const std::string_view ramp = IsValidAsciiCharset(charset) ? charset : std::string_view(fallbackCharset);
        const size_t maxRampIdx = ramp.size() - 1u;
        const size_t minExteriorIdx = (ramp[0] == ' ' && ramp.size() > 2u) ? 1u : 0u;
        const auto& srgb = detail::LinearSrgbTables::Instance();

        const bool useTrueColor = (resolvedColorMode == AsciiColorMode::TrueColor);
        const bool useAnsi256 = (resolvedColorMode == AsciiColorMode::Ansi256);
        const bool useColor = useTrueColor || useAnsi256;

        std::string out;
        const size_t bytesPerCell = useTrueColor ? 16u : (useAnsi256 ? 8u : 1u);
        out.reserve(static_cast<size_t>(cols) * static_cast<size_t>(rows) * bytesPerCell + static_cast<size_t>(rows) * 8u);

        for (int ry = 0; ry < rows; ++ry)
        {
            const int y0 = (ry * pixelHeight) / rows;
            const int y1 = std::min(pixelHeight, std::max(y0 + 1, ((ry + 1) * pixelHeight) / rows));

            bool hasActiveColor = false;
            uint8_t lastR = 0;
            uint8_t lastG = 0;
            uint8_t lastB = 0;
            uint8_t lastAnsi = 0;

            for (int cx = 0; cx < cols; ++cx)
            {
                const int x0 = (cx * pixelWidth) / cols;
                const int x1 = std::min(pixelWidth, std::max(x0 + 1, ((cx + 1) * pixelWidth) / cols));

                float linR = 0.0f;
                float linG = 0.0f;
                float linB = 0.0f;
                int totalSamples = 0;
                int exteriorSamples = 0;

                for (int py = y0; py < y1; ++py)
                {
                    const size_t rowOffset = static_cast<size_t>(py) * static_cast<size_t>(pixelWidth);
                    for (int px = x0; px < x1; ++px)
                    {
                        const size_t idx = rowOffset + static_cast<size_t>(px);
                        const float iter = iterations[idx];
                        if (iter >= 0.0f && !std::isnan(iter))
                        {
                            ++exteriorSamples;
                        }
                        const uint32_t bgra = bgraPixels[idx];
                        const uint8_t b = static_cast<uint8_t>(bgra & 0xFFu);
                        const uint8_t g = static_cast<uint8_t>((bgra >> 8) & 0xFFu);
                        const uint8_t r = static_cast<uint8_t>((bgra >> 16) & 0xFFu);
                        linR += srgb.decode[r];
                        linG += srgb.decode[g];
                        linB += srgb.decode[b];
                        ++totalSamples;
                    }
                }

                const float invSamples = 1.0f / static_cast<float>(std::max(1, totalSamples));
                const float avgLinR = std::clamp(linR * invSamples, 0.0f, 1.0f);
                const float avgLinG = std::clamp(linG * invSamples, 0.0f, 1.0f);
                const float avgLinB = std::clamp(linB * invSamples, 0.0f, 1.0f);

                const size_t maxEnc = srgb.encode.size() - 1u;
                const uint8_t outR = srgb.encode[static_cast<size_t>(avgLinR * static_cast<float>(maxEnc) + 0.5f)];
                const uint8_t outG = srgb.encode[static_cast<size_t>(avgLinG * static_cast<float>(maxEnc) + 0.5f)];
                const uint8_t outB = srgb.encode[static_cast<size_t>(avgLinB * static_cast<float>(maxEnc) + 0.5f)];

                const float luma = (0.2126f * static_cast<float>(outR) +
                                    0.7152f * static_cast<float>(outG) +
                                    0.0722f * static_cast<float>(outB)) / 255.0f;

                size_t rampIdx = 0u;
                if (exteriorSamples == 0)
                {
                    rampIdx = static_cast<size_t>(std::lround(std::clamp(luma, 0.0f, 1.0f) * static_cast<float>(maxRampIdx)));
                }
                else
                {
                    const float scaled = static_cast<float>(minExteriorIdx) +
                        std::clamp(luma, 0.0f, 1.0f) * static_cast<float>(maxRampIdx - minExteriorIdx);
                    rampIdx = std::clamp(static_cast<size_t>(std::lround(scaled)), minExteriorIdx, maxRampIdx);
                }

                const char ch = ramp[rampIdx];
                if (useColor && ch != ' ')
                {
                    if (useTrueColor)
                    {
                        if (!hasActiveColor || outR != lastR || outG != lastG || outB != lastB)
                        {
                            AppendTrueColorFg(out, outR, outG, outB);
                            lastR = outR;
                            lastG = outG;
                            lastB = outB;
                            hasActiveColor = true;
                        }
                    }
                    else
                    {
                        const uint8_t code = RgbToAnsi256(outR, outG, outB);
                        if (!hasActiveColor || code != lastAnsi)
                        {
                            AppendAnsi256Fg(out, code);
                            lastAnsi = code;
                            hasActiveColor = true;
                        }
                    }
                }
                out.push_back(ch);
            }

            if (hasActiveColor)
            {
                out.append("\x1b[0m");
            }
            out.push_back('\n');
        }

        return out;
    }

    std::string FormatConsoleHudPrimaryLine(
        const FractalConfig& config,
        const ColorTheme& theme,
        const ConsoleFrameStats& stats,
        int cols)
    {
        const std::wstring wideName = config.juliaSet
            ? (config.fractalType == FractalType::BurningShip ? L"Burning Ship Julia" : L"Julia")
            : GetFractalTypeName(config.fractalType);
        const std::string fractalName = WideToAsciiLossy(wideName);
        const std::string themeName = WideToAsciiLossy(GetPresetName(theme.CurrentPreset()));

        std::ostringstream ss;
        ss.imbue(std::locale::classic());
        ss << fractalName
           << " | Zoom: " << FormatZoomAscii(config.zoom)
           << " | ";

        if (stats.isZooming)
        {
            ss << "AUTO-ZOOM " << (stats.zoomIn ? "IN" : "OUT") << " "
               << std::fixed << std::setprecision(1) << stats.zoomSpeed << "x/s";
            if (stats.autoTarget && stats.zoomIn && !config.juliaSet)
            {
                ss << " [AUTO-TARGET]";
            }
        }
        else
        {
            ss << "PAUSED";
        }

        ss << " | Iter: " << config.maxIterations
           << " | Center: (" << FormatCoordCompact(config.hpCenterX, config.zoom)
           << ", " << FormatCoordCompact(config.hpCenterY, config.zoom) << ")"
           << " | " << themeName
           << " | " << std::fixed << std::setprecision(1) << (stats.computeMs + stats.colorMs) << "ms";
        if (stats.actualFps > 0.05)
        {
            ss << " (" << std::fixed << std::setprecision(1) << stats.actualFps << " fps)";
        }

        return ClipToWidth(ss.str(), cols);
    }

    std::string FormatConsoleHudControlsLine(int cols)
    {
        const std::string full =
            "[Space] Zoom  [E] Auto-Target  [V] Dir  [+/-] Step  [[/]] Speed  [WASD/Arrows] Pan  [F] Fractal  [T] Theme  [C] Color  [M] Chars  [R] Reset  [Q] Quit";
        if (cols >= static_cast<int>(full.size()))
        {
            return full;
        }
        const std::string medium =
            "[Space] Zoom [E] AutoTarget [V] Dir [+/-] Step [[/]] Spd [WASD] Pan [F] Frac [T] Theme [C] Color [R] Reset [Q] Quit";
        if (cols >= static_cast<int>(medium.size()))
        {
            return medium;
        }
        return ClipToWidth("[Space] Zoom [E] AutoTarget [+/-] Step [WASD] Pan [F/T/C/R] [Q] Quit", cols);
    }

    ConsoleAsciiSession::ConsoleAsciiSession(const ConsoleAsciiOptions& options, int initialCols, int initialRows)
        : m_options(options)
        , m_config(options.start)
        , m_displayedConfig(options.start)
        , m_theme(options.theme)
        , m_engine(options.useGpu ? ExecutionMode::Gpu : ExecutionMode::Automatic)
        , m_isZooming(options.autoZoom)
        , m_zoomIn(options.zoomIn)
        , m_autoTarget(options.autoTarget && !options.start.juliaSet)
        , m_showHud(options.showHud && !options.quiet)
        , m_zoomSpeed(std::clamp(options.zoomSpeed, MinConsoleZoomSpeed, MaxConsoleZoomSpeed))
        , m_targetX(options.targetX)
        , m_targetY(options.targetY)
        , m_colorMode(options.colorMode)
        , m_charsetPreset(options.charsetPreset)
    {
        m_config.renderStride = 1;
        m_config.edgeRefine = false;
        m_config.ResolveHpCenter(m_config.hpCenterX, m_config.hpCenterY);
        RefreshCharset();
        Resize(initialCols, initialRows);
        SyncRecommendedIterations();
        m_displayedConfig = m_config;
    }

    void ConsoleAsciiSession::Resize(int cols, int rows)
    {
        m_cols = std::clamp(cols, MinConsoleCols, MaxConsoleCols);
        m_rows = std::clamp(rows, MinConsoleRows, MaxConsoleRows);
        m_config.width = m_cols;
        m_config.height = ComputeAsciiPixelHeight(m_rows, m_options.charAspect);
    }

    void ConsoleAsciiSession::RefreshCharset()
    {
        if (m_charsetPreset == AsciiCharsetPreset::Custom && IsValidAsciiCharset(m_options.customCharset))
        {
            m_charset = m_options.customCharset;
        }
        else
        {
            m_charset = GetAsciiCharset(m_charsetPreset);
        }
    }

    void ConsoleAsciiSession::SyncRecommendedIterations()
    {
        if (m_options.autoScaleIterations)
        {
            m_config.maxIterations = MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_options.baseIterations);
        }
    }

    uint64_t ConsoleAsciiSession::NextRetargetSeed() noexcept
    {
        ++m_retargetCount;
        if (m_options.targetSeed == 0)
        {
            return 0;
        }
        const uint64_t mixed = SplitMix64(m_options.targetSeed ^ (m_retargetCount * 0x9E3779B97F4A7C15ULL));
        return (mixed == 0) ? 1ULL : mixed;
    }

    void ConsoleAsciiSession::PickAndApplyAutoTarget()
    {
        if (!m_hasRenderedFrame || m_iterations.empty() || m_displayedConfig.juliaSet)
        {
            m_targetX = m_config.hpCenterX;
            m_targetY = m_config.hpCenterY;
            m_hasActiveRandomTarget = false;
            return;
        }

        m_hasActiveRandomTarget = MandelbrotEngine::PickInterestingTarget(
            m_displayedConfig,
            m_iterations,
            m_targetX,
            m_targetY,
            NextRetargetSeed());
        m_retargetPanProgress = 0.0;
        m_framesSinceRetarget = 0;
    }

    std::string ConsoleAsciiSession::RenderFrame(
        double elapsedSeconds,
        double actualFps,
        AsciiColorMode resolvedColorMode,
        const std::atomic<bool>* cancelToken)
    {
        const size_t pixelCount = static_cast<size_t>(m_config.width) * static_cast<size_t>(m_config.height);
        if (m_iterations.size() != pixelCount)
        {
            m_iterations.resize(pixelCount);
        }
        if (m_pixels.size() != pixelCount)
        {
            m_pixels.resize(pixelCount);
        }

        const double computeMs = m_engine.Compute(m_config, m_iterations, cancelToken, nullptr);
        if (cancelToken && cancelToken->load(std::memory_order_relaxed))
        {
            return {};
        }

        const auto colorStart = std::chrono::steady_clock::now();
        m_theme.Recolor(
            m_iterations,
            m_pixels.data(),
            pixelCount,
            m_options.frequency,
            m_options.phase,
            m_options.invert,
            m_options.interior,
            m_config.fractalType,
            m_config.width,
            nullptr,
            true);
        const std::string ascii = RenderAsciiFrame(
            m_iterations,
            m_pixels,
            m_config.width,
            m_config.height,
            m_cols,
            m_rows,
            m_charset,
            resolvedColorMode);
        const auto colorEnd = std::chrono::steady_clock::now();
        const double colorMs = std::chrono::duration<double, std::milli>(colorEnd - colorStart).count();

        m_displayedConfig = m_config;
        m_hasRenderedFrame = true;
        ++m_frameIndex;

        // Match GUI auto-retarget logic (MainWindow.xaml.cpp): check after completed frames with cooldown.
        if (m_isZooming && m_autoTarget && m_zoomIn && !m_config.juliaSet)
        {
            ++m_framesSinceRetarget;
            const int retargetCooldown = std::clamp(m_options.fps / 2, 5, 15);
            if (!m_hasActiveRandomTarget ||
                (m_framesSinceRetarget >= retargetCooldown &&
                 MandelbrotEngine::ShouldRetargetZoom(m_displayedConfig, m_iterations, m_targetX, m_targetY)))
            {
                PickAndApplyAutoTarget();
            }
        }

        m_lastStats.cols = m_cols;
        m_lastStats.rows = m_rows;
        m_lastStats.pixelWidth = m_config.width;
        m_lastStats.pixelHeight = m_config.height;
        m_lastStats.computeMs = computeMs;
        m_lastStats.colorMs = colorMs;
        m_lastStats.actualFps = actualFps;
        m_lastStats.elapsedSeconds = elapsedSeconds;
        m_lastStats.frameIndex = m_frameIndex;
        m_lastStats.isZooming = m_isZooming;
        m_lastStats.zoomIn = m_zoomIn;
        m_lastStats.autoTarget = m_autoTarget;
        m_lastStats.zoomSpeed = m_zoomSpeed;
        m_lastStats.activeColorMode = resolvedColorMode;

        return ascii;
    }

    bool ConsoleAsciiSession::StepAutoZoom(double dtSeconds)
    {
        if (!m_isZooming)
        {
            return false;
        }

        const double dt = std::clamp(dtSeconds, 0.005, 0.25);
        const double boundZoom = m_options.endZoom.value_or(m_zoomIn ? MandelbrotEngine::MaxZoom : MandelbrotEngine::MinZoom);

        if ((m_zoomIn && m_config.zoom >= boundZoom - 1e-12 * std::max(1.0, boundZoom)) ||
            (!m_zoomIn && m_config.zoom <= boundZoom + 1e-12 * std::max(1.0, boundZoom)))
        {
            m_isZooming = false;
            m_reachedZoomBound = true;
            return false;
        }

        const double exponent = (m_zoomIn ? 1.0 : -1.0) * m_zoomSpeed * dt;
        double factor = std::pow(2.0, exponent);
        bool hitExplicitEnd = false;

        if (m_options.endZoom.has_value())
        {
            const double targetEnd = *m_options.endZoom;
            const double candidateZoom = m_config.zoom * factor;
            if (m_zoomIn && candidateZoom >= targetEnd)
            {
                factor = targetEnd / std::max(m_config.zoom, MandelbrotEngine::MinZoom);
                hitExplicitEnd = true;
            }
            else if (!m_zoomIn && candidateZoom <= targetEnd)
            {
                factor = targetEnd / std::max(m_config.zoom, MandelbrotEngine::MinZoom);
                hitExplicitEnd = true;
            }
        }

        // Smoothstep pan blending after auto-retarget, matching GUI MainWindow.xaml.cpp.
        m_retargetPanProgress = std::min(1.0, m_retargetPanProgress + dt / 0.40);
        const double s = m_retargetPanProgress;
        const double panBlend = std::clamp(s * s * (3.0 - 2.0 * s), 0.08, 1.0);

        const bool engineCanContinue = MandelbrotEngine::StepZoomAnimation(
            m_config, m_targetX, m_targetY, factor, panBlend);
        SyncRecommendedIterations();

        const bool stillGoing = engineCanContinue && !hitExplicitEnd;
        if (!stillGoing)
        {
            m_isZooming = false;
            m_reachedZoomBound = true;
        }
        return stillGoing;
    }

    bool ConsoleAsciiSession::ApplyKeyAction(ConsoleKeyAction action)
    {
        switch (action)
        {
        case ConsoleKeyAction::None:
            return true;

        case ConsoleKeyAction::Quit:
            return false;

        case ConsoleKeyAction::ToggleAutoZoom:
            m_isZooming = !m_isZooming;
            m_reachedZoomBound = false;
            if (m_isZooming)
            {
                if (m_autoTarget && m_zoomIn && !m_config.juliaSet)
                {
                    if (!m_hasActiveRandomTarget ||
                        (m_hasRenderedFrame &&
                         MandelbrotEngine::ShouldRetargetZoom(m_displayedConfig, m_iterations, m_targetX, m_targetY)))
                    {
                        PickAndApplyAutoTarget();
                    }
                }
            }
            return true;

        case ConsoleKeyAction::ToggleAutoTarget:
            if (!m_config.juliaSet)
            {
                m_autoTarget = !m_autoTarget;
                if (m_autoTarget && m_zoomIn && m_hasRenderedFrame)
                {
                    PickAndApplyAutoTarget();
                }
                else if (!m_autoTarget)
                {
                    m_targetX = m_config.hpCenterX;
                    m_targetY = m_config.hpCenterY;
                    m_hasActiveRandomTarget = false;
                    m_retargetPanProgress = 1.0;
                }
            }
            return true;

        case ConsoleKeyAction::ToggleDirection:
            m_zoomIn = !m_zoomIn;
            m_reachedZoomBound = false;
            if (m_zoomIn && m_autoTarget && !m_config.juliaSet && m_hasRenderedFrame)
            {
                PickAndApplyAutoTarget();
            }
            return true;

        case ConsoleKeyAction::ZoomInStep:
            MandelbrotEngine::ZoomViewport(
                m_config, 2.0,
                static_cast<double>(m_config.width) * 0.5,
                static_cast<double>(m_config.height) * 0.5);
            m_targetX = m_config.hpCenterX;
            m_targetY = m_config.hpCenterY;
            m_hasActiveRandomTarget = false;
            m_reachedZoomBound = false;
            SyncRecommendedIterations();
            return true;

        case ConsoleKeyAction::ZoomOutStep:
            MandelbrotEngine::ZoomViewport(
                m_config, 0.5,
                static_cast<double>(m_config.width) * 0.5,
                static_cast<double>(m_config.height) * 0.5);
            m_targetX = m_config.hpCenterX;
            m_targetY = m_config.hpCenterY;
            m_hasActiveRandomTarget = false;
            m_reachedZoomBound = false;
            SyncRecommendedIterations();
            return true;

        case ConsoleKeyAction::SpeedUp:
            m_zoomSpeed = std::clamp(m_zoomSpeed + 0.25, MinConsoleZoomSpeed, MaxConsoleZoomSpeed);
            return true;

        case ConsoleKeyAction::SpeedDown:
            m_zoomSpeed = std::clamp(m_zoomSpeed - 0.25, MinConsoleZoomSpeed, MaxConsoleZoomSpeed);
            return true;

        case ConsoleKeyAction::PanLeft:
        case ConsoleKeyAction::PanRight:
        case ConsoleKeyAction::PanUp:
        case ConsoleKeyAction::PanDown:
        {
            const double panStepX = std::max(1.0, static_cast<double>(m_config.width) * 0.12);
            const double panStepY = std::max(1.0, static_cast<double>(m_config.height) * 0.12);
            double dxPx = 0.0;
            double dyPx = 0.0;
            if (action == ConsoleKeyAction::PanLeft)  dxPx = panStepX;
            if (action == ConsoleKeyAction::PanRight) dxPx = -panStepX;
            if (action == ConsoleKeyAction::PanUp)    dyPx = panStepY;
            if (action == ConsoleKeyAction::PanDown)  dyPx = -panStepY;

            const BigFloat startX = m_config.hpCenterX;
            const BigFloat startY = m_config.hpCenterY;
            MandelbrotEngine::PanViewport(m_config, startX, startY, dxPx, dyPx);
            m_targetX = m_config.hpCenterX;
            m_targetY = m_config.hpCenterY;
            m_hasActiveRandomTarget = false;
            m_retargetPanProgress = 1.0;
            return true;
        }

        case ConsoleKeyAction::NextFractal:
        {
            const auto& all = GetAllFractalTypes();
            if (!all.empty())
            {
                auto it = std::find(all.begin(), all.end(), m_config.fractalType);
                const size_t nextIdx = (it == all.end()) ? 0u : ((static_cast<size_t>(it - all.begin()) + 1u) % all.size());
                m_config.fractalType = all[nextIdx];
                m_config.juliaSet = false;
                const auto& bms = GetBookmarks(m_config.fractalType);
                if (!bms.empty())
                {
                    m_config.SetCenter(bms[0].centerX, bms[0].centerY);
                    m_config.zoom = bms[0].zoom;
                }
                else
                {
                    m_config.SetCenter(-0.75, 0.0);
                    m_config.zoom = 1.0;
                }
                m_targetX = m_config.hpCenterX;
                m_targetY = m_config.hpCenterY;
                m_hasActiveRandomTarget = false;
                m_hasRenderedFrame = false;
                m_reachedZoomBound = false;
                m_engine.InvalidateDeepCache();
                SyncRecommendedIterations();
            }
            return true;
        }

        case ConsoleKeyAction::NextTheme:
        {
            const auto& presets = GetAllPresets();
            if (!presets.empty())
            {
                auto it = std::find(presets.begin(), presets.end(), m_theme.CurrentPreset());
                const size_t nextIdx = (it == presets.end()) ? 0u : ((static_cast<size_t>(it - presets.begin()) + 1u) % presets.size());
                m_theme.SetPreset(presets[nextIdx]);
            }
            return true;
        }

        case ConsoleKeyAction::CycleColorMode:
            switch (m_colorMode)
            {
            case AsciiColorMode::Auto:
            case AsciiColorMode::TrueColor:
                m_colorMode = AsciiColorMode::Ansi256;
                break;
            case AsciiColorMode::Ansi256:
                m_colorMode = AsciiColorMode::None;
                break;
            case AsciiColorMode::None:
            default:
                m_colorMode = AsciiColorMode::TrueColor;
                break;
            }
            return true;

        case ConsoleKeyAction::CycleCharset:
            switch (m_charsetPreset)
            {
            case AsciiCharsetPreset::Standard: m_charsetPreset = AsciiCharsetPreset::Dense; break;
            case AsciiCharsetPreset::Dense:    m_charsetPreset = AsciiCharsetPreset::Detailed; break;
            case AsciiCharsetPreset::Detailed: m_charsetPreset = AsciiCharsetPreset::Simple; break;
            case AsciiCharsetPreset::Simple:
            case AsciiCharsetPreset::Custom:
            default:                           m_charsetPreset = AsciiCharsetPreset::Standard; break;
            }
            RefreshCharset();
            return true;

        case ConsoleKeyAction::ToggleHud:
            m_showHud = !m_showHud;
            return true;

        case ConsoleKeyAction::ResetView:
        {
            m_config = m_options.start;
            m_config.renderStride = 1;
            m_config.edgeRefine = false;
            m_config.width = m_cols;
            m_config.height = ComputeAsciiPixelHeight(m_rows, m_options.charAspect);
            m_targetX = m_options.targetX;
            m_targetY = m_options.targetY;
            m_hasActiveRandomTarget = false;
            m_hasRenderedFrame = false;
            m_reachedZoomBound = false;
            m_retargetPanProgress = 1.0;
            m_engine.InvalidateDeepCache();
            SyncRecommendedIterations();
            return true;
        }

        default:
            return true;
        }
    }

    int RunConsoleAsciiCli(
        const ConsoleAsciiOptions& options,
        std::ostream& out,
        std::wostream& err,
        const std::atomic<bool>* externalCancel,
        bool interactiveTerminal)
    {
        try
        {
            if (externalCancel && externalCancel->load(std::memory_order_relaxed))
            {
                return 130;
            }

            const bool isRealStdout = (&out == &std::cout);
            const bool singleSnapshot = options.once ||
                options.maxFrames == 1 ||
                (!interactiveTerminal && !options.autoZoom && options.maxFrames == 0 && options.maxDurationSeconds == 0);

            const bool useInteractiveScreen = interactiveTerminal && !singleSnapshot;
            ScopedTerminalSession terminal(out, useInteractiveScreen, isRealStdout);
            const bool vtEnabled = terminal.VtEnabled();

            auto resolveDimensions = [&](bool showHud, int& outCols, int& outCanvasRows, int& outHudRows)
            {
                int termCols = DefaultConsoleCols;
                int termRows = DefaultConsoleRows;
                if (isRealStdout && interactiveTerminal)
                {
                    QueryRealTerminalSize(termCols, termRows);
                }

                const int targetCols = (options.cols > 0)
                    ? std::clamp(options.cols, MinConsoleCols, MaxConsoleCols)
                    : termCols;
                const int totalRows = (options.rows > 0)
                    ? std::clamp(options.rows, MinConsoleRows, MaxConsoleRows)
                    : termRows;

                outCols = targetCols;
                if (useInteractiveScreen)
                {
                    outHudRows = ComputeHudRowCount(totalRows, showHud, true);
                    outCanvasRows = std::max(MinConsoleRows, totalRows - outHudRows);
                    if (outCanvasRows + outHudRows > totalRows)
                    {
                        outHudRows = std::max(0, totalRows - outCanvasRows);
                    }
                }
                else
                {
                    outHudRows = showHud ? 1 : 0;
                    outCanvasRows = totalRows;
                }
            };

            int cols = DefaultConsoleCols;
            int canvasRows = DefaultConsoleRows;
            int hudRows = 0;
            resolveDimensions(options.showHud && !options.quiet, cols, canvasRows, hudRows);

            ConsoleAsciiSession session(options, cols, canvasRows);

            if (singleSnapshot)
            {
                const AsciiColorMode colorMode = ResolveColorMode(
                    session.PreferredColorMode(), interactiveTerminal, interactiveTerminal ? vtEnabled : true);
                const std::string frame = session.RenderFrame(0.0, 0.0, colorMode, externalCancel);
                if (externalCancel && externalCancel->load(std::memory_order_relaxed))
                {
                    return 130;
                }
                out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
                if (session.ShowHud())
                {
                    const std::string hud = FormatConsoleHudPrimaryLine(
                        session.DisplayedConfig(), session.Theme(), session.LastStats(), session.Cols());
                    out << hud << "\n";
                }
                out.flush();
                return 0;
            }

            const auto startTime = std::chrono::steady_clock::now();
            auto lastStepTime = startTime;
            const int targetFps = std::clamp(options.fps, MinConsoleFps, MaxConsoleFps);
            const auto frameBudget = std::chrono::microseconds(1000000 / targetFps);
            const bool realTimePacing = interactiveTerminal || options.maxDurationSeconds > 0;
            double smoothedFps = static_cast<double>(targetFps);

            int renderedFrames = 0;
            for (;;)
            {
                if (externalCancel && externalCancel->load(std::memory_order_relaxed))
                {
                    return 130;
                }

                // Poll all queued keyboard inputs before rendering the next frame.
                if (useInteractiveScreen)
                {
                    for (;;)
                    {
                        const ConsoleKeyAction action = terminal.PollKey();
                        if (action == ConsoleKeyAction::None) break;
                        if (!session.ApplyKeyAction(action))
                        {
                            return 0;
                        }
                    }
                }

                // Track dynamic terminal resize and HUD visibility changes.
                if (useInteractiveScreen)
                {
                    int newCols = cols;
                    int newCanvasRows = canvasRows;
                    int newHudRows = hudRows;
                    resolveDimensions(session.ShowHud(), newCols, newCanvasRows, newHudRows);
                    if (newCols != cols || newCanvasRows != canvasRows || newHudRows != hudRows)
                    {
                        cols = newCols;
                        canvasRows = newCanvasRows;
                        hudRows = newHudRows;
                        session.Resize(cols, canvasRows);
                    }
                }

                const auto frameStart = std::chrono::steady_clock::now();
                const double elapsedSeconds = realTimePacing
                    ? std::chrono::duration<double>(frameStart - startTime).count()
                    : static_cast<double>(renderedFrames) / static_cast<double>(targetFps);

                const AsciiColorMode colorMode = ResolveColorMode(
                    session.PreferredColorMode(), useInteractiveScreen, useInteractiveScreen ? vtEnabled : true);

                const std::string asciiFrame = session.RenderFrame(elapsedSeconds, smoothedFps, colorMode, externalCancel);
                if (externalCancel && externalCancel->load(std::memory_order_relaxed))
                {
                    return 130;
                }

                if (useInteractiveScreen)
                {
                    std::string screen;
                    screen.reserve(asciiFrame.size() + static_cast<size_t>(cols) * 4u + 128u);
                    if (vtEnabled)
                    {
                        screen.append("\x1b[H");
                    }
                    else
                    {
                        terminal.MoveCursorHomeFallback();
                    }

                    // Emit fractal lines with clear-to-EOL so resizing or shorter lines never leave artifacts.
                    if (vtEnabled)
                    {
                        size_t pos = 0;
                        int lineCount = 0;
                        while (pos < asciiFrame.size())
                        {
                            const size_t nl = asciiFrame.find('\n', pos);
                            const size_t end = (nl == std::string::npos) ? asciiFrame.size() : nl;
                            screen.append(asciiFrame, pos, end - pos);
                            screen.append("\x1b[K");
                            ++lineCount;
                            if (lineCount < canvasRows || hudRows > 0)
                            {
                                screen.push_back('\n');
                            }
                            if (nl == std::string::npos) break;
                            pos = nl + 1;
                        }
                    }
                    else
                    {
                        screen.append(asciiFrame);
                    }

                    if (hudRows >= 1)
                    {
                        const std::string line1 = FormatConsoleHudPrimaryLine(
                            session.DisplayedConfig(), session.Theme(), session.LastStats(), cols);
                        if (vtEnabled) screen.append("\x1b[0m\x1b[7m");
                        screen.append(line1);
                        if (static_cast<int>(line1.size()) < cols)
                        {
                            screen.append(static_cast<size_t>(cols - static_cast<int>(line1.size())), ' ');
                        }
                        if (vtEnabled) screen.append("\x1b[0m\x1b[K");
                    }
                    if (hudRows >= 2)
                    {
                        screen.push_back('\n');
                        const std::string line2 = FormatConsoleHudControlsLine(cols);
                        if (vtEnabled) screen.append("\x1b[2m");
                        screen.append(line2);
                        if (vtEnabled) screen.append("\x1b[0m\x1b[K");
                    }

                    out.write(screen.data(), static_cast<std::streamsize>(screen.size()));
                    out.flush();
                }
                else
                {
                    if (renderedFrames > 0)
                    {
                        out.put('\n');
                    }
                    out.write(asciiFrame.data(), static_cast<std::streamsize>(asciiFrame.size()));
                    if (session.ShowHud())
                    {
                        const std::string hud = FormatConsoleHudPrimaryLine(
                            session.DisplayedConfig(), session.Theme(), session.LastStats(), cols);
                        out << hud << "\n";
                    }
                    out.flush();
                }

                ++renderedFrames;

                if (options.maxFrames > 0 && renderedFrames >= options.maxFrames)
                {
                    break;
                }

                if (options.maxDurationSeconds > 0)
                {
                    const double nowElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
                    if (nowElapsed >= static_cast<double>(options.maxDurationSeconds))
                    {
                        break;
                    }
                }

                // If auto-zoom reached an explicit --end-zoom or zoom limit:
                // in non-interactive or bounded runs exit cleanly; in interactive terminal stay open so user can inspect/quit.
                if (session.ReachedZoomBound() && (!useInteractiveScreen || !isRealStdout || options.endZoom.has_value()))
                {
                    break;
                }

                // Frame pacing with responsive input polling during sleep.
                if (realTimePacing)
                {
                    const auto nextTarget = frameStart + frameBudget;
                    while (std::chrono::steady_clock::now() < nextTarget)
                    {
                        if (externalCancel && externalCancel->load(std::memory_order_relaxed))
                        {
                            return 130;
                        }
                        if (useInteractiveScreen)
                        {
                            const ConsoleKeyAction action = terminal.PollKey();
                            if (action != ConsoleKeyAction::None)
                            {
                                if (!session.ApplyKeyAction(action))
                                {
                                    return 0;
                                }
                                break;
                            }
                        }
                        const auto remaining = nextTarget - std::chrono::steady_clock::now();
                        if (remaining <= std::chrono::microseconds::zero()) break;
                        const auto slice = std::min(remaining, std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(15)));
                        std::this_thread::sleep_for(slice);
                    }
                    const auto afterPace = std::chrono::steady_clock::now();
                    const double stepDt = std::chrono::duration<double>(afterPace - lastStepTime).count();
                    lastStepTime = afterPace;

                    const double frameSecs = std::chrono::duration<double>(afterPace - frameStart).count();
                    if (frameSecs > 1e-4)
                    {
                        const double instFps = 1.0 / frameSecs;
                        smoothedFps = (renderedFrames == 1) ? instFps : (0.8 * smoothedFps + 0.2 * instFps);
                    }

                    if (session.IsZooming())
                    {
                        session.StepAutoZoom(stepDt);
                    }
                }
                else
                {
                    const double fixedDt = 1.0 / static_cast<double>(targetFps);
                    if (session.IsZooming())
                    {
                        session.StepAutoZoom(fixedDt);
                    }
                }

                // In non-real-time / non-interactive stream tests without maxFrames/duration/endZoom, don't spin forever.
                if (!isRealStdout && options.maxFrames == 0 && options.maxDurationSeconds == 0 && !options.endZoom.has_value())
                {
                    break;
                }
            }

            return 0;
        }
        catch (const std::exception& e)
        {
            const std::string msg = e.what();
            std::wstring wmsg;
            wmsg.reserve(msg.size());
            for (char c : msg)
            {
                wmsg.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            err << L"Console ASCII mode failed: " << wmsg << L"\n";
            err.flush();
            return 1;
        }
        catch (...)
        {
            err << L"Console ASCII mode failed: Unexpected error.\n";
            err.flush();
            return 1;
        }
    }
}
