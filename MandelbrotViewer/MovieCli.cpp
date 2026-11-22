#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

#include "MovieCli.h"
#include "MovieExport.h"
#include "JuliaView.h"
#include "ViewportNavigation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <variant>

namespace Mandelbrot
{
    namespace
    {
        std::wstring Utf8ToWide(std::string_view utf8)
        {
            std::wstring out;
            out.reserve(utf8.size());
            size_t i = 0;
            while (i < utf8.size())
            {
                const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
                if (b0 < 0x80)
                {
                    out.push_back(static_cast<wchar_t>(b0));
                    ++i;
                }
                else if ((b0 & 0xE0) == 0xC0 && i + 1 < utf8.size())
                {
                    const uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
                    out.push_back(static_cast<wchar_t>(cp));
                    i += 2;
                }
                else if ((b0 & 0xF0) == 0xE0 && i + 2 < utf8.size())
                {
                    const uint32_t cp = ((b0 & 0x0Fu) << 12) |
                        ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 6) |
                        (static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu);
                    out.push_back(static_cast<wchar_t>(cp));
                    i += 3;
                }
                else if ((b0 & 0xF8) == 0xF0 && i + 3 < utf8.size())
                {
                    const uint32_t cp = ((b0 & 0x07u) << 18) |
                        ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 12) |
                        ((static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu) << 6) |
                        (static_cast<uint8_t>(utf8[i + 3]) & 0x3Fu);
                    if constexpr (sizeof(wchar_t) == 2)
                    {
                        if (cp >= 0x10000u && cp <= 0x10FFFFu)
                        {
                            const uint32_t v = cp - 0x10000u;
                            out.push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
                            out.push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
                        }
                    }
                    else
                    {
                        out.push_back(static_cast<wchar_t>(cp));
                    }
                    i += 4;
                }
                else
                {
                    out.push_back(L'?');
                    ++i;
                }
            }
            return out;
        }

        bool TryWideToAscii(std::wstring_view wide, std::string& ascii)
        {
            ascii.clear();
            ascii.reserve(wide.size());
            for (wchar_t wc : wide)
            {
                if (static_cast<uint32_t>(wc) > 127u) return false;
                ascii.push_back(static_cast<char>(wc));
            }
            return true;
        }

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

        std::string NormalizeToken(std::string_view raw)
        {
            std::string normalized;
            normalized.reserve(raw.size());
            for (char c : raw)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    normalized.push_back(static_cast<char>(c - 'A' + 'a'));
                }
                else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                {
                    normalized.push_back(c);
                }
            }
            return normalized;
        }

        std::string NormalizeToken(std::wstring_view raw)
        {
            std::string ascii;
            if (!TryWideToAscii(raw, ascii)) return {};
            return NormalizeToken(ascii);
        }

        std::string TrimAscii(std::string_view s)
        {
            size_t first = 0;
            while (first < s.size() && (s[first] == ' ' || s[first] == '\t' || s[first] == '\r' || s[first] == '\n'))
                ++first;
            size_t last = s.size();
            while (last > first && (s[last - 1] == ' ' || s[last - 1] == '\t' || s[last - 1] == '\r' || s[last - 1] == '\n'))
                --last;
            return std::string(s.substr(first, last - first));
        }

        bool TryParseInt(std::wstring_view text, int& out)
        {
            std::string ascii;
            if (!TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = TrimAscii(ascii);
            if (trimmed.empty()) return false;
            const char* begin = trimmed.data();
            const char* end = trimmed.data() + trimmed.size();
            if (*begin == '+')
            {
                ++begin;
                if (begin == end || *begin == '-') return false;
            }
            int value = 0;
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{} || ptr != end) return false;
            out = value;
            return true;
        }

        bool TryParseUint64(std::wstring_view text, uint64_t& out)
        {
            std::string ascii;
            if (!TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = TrimAscii(ascii);
            if (trimmed.empty()) return false;
            const char* begin = trimmed.data();
            const char* end = trimmed.data() + trimmed.size();
            if (*begin == '+')
            {
                ++begin;
                if (begin == end) return false;
            }
            uint64_t value = 0;
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{} || ptr != end) return false;
            out = value;
            return true;
        }

        bool TryParseDouble(std::wstring_view text, double& out)
        {
            std::string ascii;
            if (!TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = TrimAscii(ascii);
            if (trimmed.empty()) return false;
            const char* begin = trimmed.data();
            const char* end = trimmed.data() + trimmed.size();
            if (*begin == '+')
            {
                ++begin;
                if (begin == end || *begin == '-') return false;
            }
            double value = 0.0;
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{} || ptr != end || !std::isfinite(value)) return false;
            out = value;
            return true;
        }

        bool TryParseColor(std::string_view raw, uint32_t& out)
        {
            const std::string s = TrimAscii(raw);
            if (s.empty()) return false;

            std::string_view hex;
            bool isHex = false;
            if (s[0] == '#')
            {
                hex = std::string_view(s).substr(1);
                isHex = true;
            }
            else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            {
                hex = std::string_view(s).substr(2);
                isHex = true;
            }

            if (isHex)
            {
                if (hex.size() != 6 && hex.size() != 8) return false;
                uint32_t value = 0;
                const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
                if (ec != std::errc{} || ptr != hex.data() + hex.size()) return false;
                out = (hex.size() == 6) ? (0xFF000000u | value) : value;
                return true;
            }

            uint64_t decimal = 0;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), decimal, 10);
            if (ec != std::errc{} || ptr != s.data() + s.size() || decimal > 0xFFFFFFFFull) return false;
            out = static_cast<uint32_t>(decimal);
            return true;
        }

        bool TryParseFractalType(std::string_view text, FractalType& outType, std::optional<bool>& outJulia)
        {
            const std::string key = NormalizeToken(text);
            if (key == "juliaset" || key == "julia")
            {
                outType = FractalType::Mandelbrot;
                outJulia = true;
                return true;
            }
            if (key == "burningshipjuliaset" || key == "burningshipjulia")
            {
                outType = FractalType::BurningShip;
                outJulia = true;
                return true;
            }
            if (key == "mandelbrot" || key == "mandelbrotset") { outType = FractalType::Mandelbrot; return true; }
            if (key == "burningship") { outType = FractalType::BurningShip; return true; }
            if (key == "tricorn" || key == "mandelbar" || key == "tricornmandelbar") { outType = FractalType::Tricorn; return true; }
            if (key == "celtic" || key == "celticmandelbrot") { outType = FractalType::Celtic; return true; }
            if (key == "buffalo" || key == "buffalofractal") { outType = FractalType::Buffalo; return true; }
            if (key == "cubic" || key == "cubicmultibrot" || key == "multibrot") { outType = FractalType::CubicMultibrot; return true; }
            if (key == "phoenix" || key == "phoenixfractal") { outType = FractalType::Phoenix; return true; }
            if (key == "newton" || key == "newtonsfractal" || key == "newtonfractal") { outType = FractalType::Newton; return true; }
            return false;
        }

        bool TryParsePresetTheme(std::string_view text, PresetTheme& outTheme)
        {
            const std::string key = NormalizeToken(text);
            if (key == "electricblue" || key == "blue") { outTheme = PresetTheme::ElectricBlue; return true; }
            if (key == "inferno" || key == "infernofire" || key == "fire") { outTheme = PresetTheme::Inferno; return true; }
            if (key == "psychedelic" || key == "psychedelicrainbow" || key == "rainbow") { outTheme = PresetTheme::Psychedelic; return true; }
            if (key == "cyberpunk" || key == "cyberpunkneon" || key == "neon") { outTheme = PresetTheme::Cyberpunk; return true; }
            if (key == "viridis") { outTheme = PresetTheme::Viridis; return true; }
            if (key == "twilightocean" || key == "twilight" || key == "ocean") { outTheme = PresetTheme::TwilightOcean; return true; }
            if (key == "sunsetglow" || key == "sunset") { outTheme = PresetTheme::SunsetGlow; return true; }
            if (key == "monochrome" || key == "mono" || key == "grayscale" || key == "greyscale") { outTheme = PresetTheme::Monochrome; return true; }
            if (key == "custom" || key == "custompalette") { outTheme = PresetTheme::Custom; return true; }
            return false;
        }

        bool TryParseCodec(std::wstring_view text, MovieCodec& outCodec)
        {
            const std::string key = NormalizeToken(text);
            if (key == "h264" || key == "avc") { outCodec = MovieCodec::H264; return true; }
            if (key == "hevc" || key == "h265") { outCodec = MovieCodec::Hevc; return true; }
            return false;
        }

        bool TryParseResolution(std::wstring_view text, int& outWidth, int& outHeight)
        {
            const std::string key = NormalizeToken(text);
            if (key == "720p") { outWidth = 1280; outHeight = 720; return true; }
            if (key == "1080p") { outWidth = 1920; outHeight = 1080; return true; }
            if (key == "1440p") { outWidth = 2560; outHeight = 1440; return true; }
            if (key == "4k" || key == "2160p" || key == "4kuhd") { outWidth = 3840; outHeight = 2160; return true; }
            if (key == "8k" || key == "4320p" || key == "8kuhd") { outWidth = 7680; outHeight = 4320; return true; }

            std::string ascii;
            if (!TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = TrimAscii(ascii);
            size_t sep = trimmed.find('x');
            if (sep == std::string::npos) sep = trimmed.find('X');
            if (sep == std::string::npos || sep == 0 || sep + 1 >= trimmed.size()) return false;
            if (trimmed.find('x', sep + 1) != std::string::npos || trimmed.find('X', sep + 1) != std::string::npos) return false;

            int w = 0, h = 0;
            const auto [pw, ew] = std::from_chars(trimmed.data(), trimmed.data() + sep, w);
            const auto [ph, eh] = std::from_chars(trimmed.data() + sep + 1, trimmed.data() + trimmed.size(), h);
            if (ew != std::errc{} || pw != trimmed.data() + sep ||
                eh != std::errc{} || ph != trimmed.data() + trimmed.size())
            {
                return false;
            }
            if (w < 1 || h < 1 || w > 7680 || h > 4320) return false;
            outWidth = w;
            outHeight = h;
            return true;
        }

        bool TryFindBookmark(
            std::wstring_view name,
            std::optional<FractalType> restrictType,
            FractalType& outType,
            BookmarkLocation& outBookmark)
        {
            const std::string needle = NormalizeToken(name);
            if (needle.empty()) return false;

            auto searchType = [&](FractalType type) -> bool
            {
                for (const auto& bm : GetBookmarks(type))
                {
                    if (NormalizeToken(bm.name) == needle)
                    {
                        outType = type;
                        outBookmark = bm;
                        return true;
                    }
                }
                return false;
            };

            if (restrictType)
            {
                return searchType(*restrictType);
            }
            for (FractalType type : GetAllFractalTypes())
            {
                if (searchType(type)) return true;
            }
            return false;
        }

        // Minimal, strict JSON value representation for loading SerializeViewState() documents.
        struct JsonValue
        {
            struct Number { std::string raw; double numeric{}; };
            using Array = std::vector<JsonValue>;
            using Object = std::map<std::string, JsonValue>;
            using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

            Storage data{ nullptr };
        };

        class JsonParser
        {
        public:
            explicit JsonParser(std::string_view input) : m_text(input) {}

            JsonValue Parse()
            {
                SkipWhitespace();
                JsonValue root = ParseValue();
                SkipWhitespace();
                if (m_pos != m_text.size())
                {
                    throw std::invalid_argument("Unexpected trailing characters in JSON config.");
                }
                return root;
            }

        private:
            void SkipWhitespace()
            {
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++m_pos;
                    else break;
                }
            }

            JsonValue ParseValue()
            {
                SkipWhitespace();
                if (m_pos >= m_text.size())
                {
                    throw std::invalid_argument("Unexpected end of JSON config.");
                }
                const char c = m_text[m_pos];
                if (c == '{') return ParseObject();
                if (c == '[') return ParseArray();
                if (c == '"') return JsonValue{ ParseString() };
                if (c == 't') { ConsumeLiteral("true"); return JsonValue{ true }; }
                if (c == 'f') { ConsumeLiteral("false"); return JsonValue{ false }; }
                if (c == 'n') { ConsumeLiteral("null"); return JsonValue{ nullptr }; }
                if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
                throw std::invalid_argument("Invalid token in JSON config.");
            }

            void ConsumeLiteral(std::string_view lit)
            {
                if (m_text.substr(m_pos, lit.size()) != lit)
                {
                    throw std::invalid_argument("Invalid literal in JSON config.");
                }
                m_pos += lit.size();
            }

            std::string ParseString()
            {
                if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                {
                    throw std::invalid_argument("Expected string in JSON config.");
                }
                ++m_pos;
                std::string result;
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos++];
                    if (c == '"') return result;
                    if (c == '\\')
                    {
                        if (m_pos >= m_text.size()) throw std::invalid_argument("Unterminated escape in JSON string.");
                        const char esc = m_text[m_pos++];
                        switch (esc)
                        {
                        case '"': result.push_back('"'); break;
                        case '\\': result.push_back('\\'); break;
                        case '/': result.push_back('/'); break;
                        case 'b': result.push_back('\b'); break;
                        case 'f': result.push_back('\f'); break;
                        case 'n': result.push_back('\n'); break;
                        case 'r': result.push_back('\r'); break;
                        case 't': result.push_back('\t'); break;
                        case 'u':
                        {
                            if (m_pos + 4 > m_text.size()) throw std::invalid_argument("Invalid \\u escape in JSON string.");
                            uint32_t cp = 0;
                            const auto [ptr, ec] = std::from_chars(m_text.data() + m_pos, m_text.data() + m_pos + 4, cp, 16);
                            if (ec != std::errc{} || ptr != m_text.data() + m_pos + 4)
                                throw std::invalid_argument("Invalid hex digits in \\u escape in JSON string.");
                            m_pos += 4;
                            if (cp < 0x80) result.push_back(static_cast<char>(cp));
                            else if (cp < 0x800)
                            {
                                result.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                                result.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                            }
                            else
                            {
                                result.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                                result.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                                result.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                            }
                            break;
                        }
                        default:
                            throw std::invalid_argument("Unsupported escape sequence in JSON string.");
                        }
                    }
                    else if (static_cast<unsigned char>(c) < 0x20)
                    {
                        throw std::invalid_argument("Unescaped control character in JSON string.");
                    }
                    else
                    {
                        result.push_back(c);
                    }
                }
                throw std::invalid_argument("Unterminated string in JSON config.");
            }

            JsonValue ParseNumber()
            {
                const size_t start = m_pos;
                if (m_pos < m_text.size() && m_text[m_pos] == '-') ++m_pos;
                if (m_pos >= m_text.size()) throw std::invalid_argument("Invalid number in JSON config.");
                if (m_text[m_pos] == '0')
                {
                    ++m_pos;
                }
                else if (m_text[m_pos] >= '1' && m_text[m_pos] <= '9')
                {
                    while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') ++m_pos;
                }
                else
                {
                    throw std::invalid_argument("Invalid number in JSON config.");
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '.')
                {
                    ++m_pos;
                    if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                        throw std::invalid_argument("Invalid fraction in JSON number.");
                    while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') ++m_pos;
                }
                if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
                {
                    ++m_pos;
                    if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) ++m_pos;
                    if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                        throw std::invalid_argument("Invalid exponent in JSON number.");
                    while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') ++m_pos;
                }
                const std::string raw(m_text.substr(start, m_pos - start));
                double value = 0.0;
                const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                if (ec != std::errc{} || ptr != raw.data() + raw.size() || !std::isfinite(value))
                {
                    throw std::invalid_argument("Out-of-range number in JSON config.");
                }
                return JsonValue{ JsonValue::Number{ raw, value } };
            }

            JsonValue ParseArray()
            {
                ++m_pos; // '['
                JsonValue::Array arr;
                SkipWhitespace();
                if (m_pos < m_text.size() && m_text[m_pos] == ']')
                {
                    ++m_pos;
                    return JsonValue{ std::move(arr) };
                }
                for (;;)
                {
                    arr.push_back(ParseValue());
                    SkipWhitespace();
                    if (m_pos >= m_text.size()) throw std::invalid_argument("Unterminated array in JSON config.");
                    if (m_text[m_pos] == ']') { ++m_pos; break; }
                    if (m_text[m_pos] != ',') throw std::invalid_argument("Expected ',' or ']' in JSON array.");
                    ++m_pos;
                }
                return JsonValue{ std::move(arr) };
            }

            JsonValue ParseObject()
            {
                ++m_pos; // '{'
                JsonValue::Object obj;
                SkipWhitespace();
                if (m_pos < m_text.size() && m_text[m_pos] == '}')
                {
                    ++m_pos;
                    return JsonValue{ std::move(obj) };
                }
                for (;;)
                {
                    SkipWhitespace();
                    std::string key = ParseString();
                    SkipWhitespace();
                    if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                        throw std::invalid_argument("Expected ':' after key in JSON object.");
                    ++m_pos;
                    obj[std::move(key)] = ParseValue();
                    SkipWhitespace();
                    if (m_pos >= m_text.size()) throw std::invalid_argument("Unterminated object in JSON config.");
                    if (m_text[m_pos] == '}') { ++m_pos; break; }
                    if (m_text[m_pos] != ',') throw std::invalid_argument("Expected ',' or '}' in JSON object.");
                    ++m_pos;
                }
                return JsonValue{ std::move(obj) };
            }

            std::string_view m_text;
            size_t m_pos{ 0 };
        };

        bool TryExtractJsonBigFloat(const JsonValue& v, BigFloat& out)
        {
            if (const auto* s = std::get_if<std::string>(&v.data))
            {
                return BigFloat::TryParse(*s, out);
            }
            if (const auto* n = std::get_if<JsonValue::Number>(&v.data))
            {
                return BigFloat::TryParse(n->raw, out);
            }
            return false;
        }

        bool StopsEqualPreset(const std::vector<GradientStop>& a, const std::vector<GradientStop>& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b ||
                    std::abs(a[i].position - b[i].position) > 1e-5f)
                {
                    return false;
                }
            }
            return true;
        }

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

        struct ScopedParentConsole
        {
            bool attached{ false };
            bool stdoutIsConsole{ false };

            ScopedParentConsole() noexcept
            {
                const bool hadStdout = IsValidStdHandle(GetStdHandle(STD_OUTPUT_HANDLE));
                const bool hadStderr = IsValidStdHandle(GetStdHandle(STD_ERROR_HANDLE));
                if (!hadStdout || !hadStderr)
                {
                    if (AttachConsole(ATTACH_PARENT_PROCESS))
                    {
                        attached = true;
                        FILE* stream = nullptr;
                        if (!hadStdout && freopen_s(&stream, "CONOUT$", "w", stdout) == 0)
                        {
                            std::setvbuf(stdout, nullptr, _IONBF, 0);
                            std::cout.clear();
                            std::wcout.clear();
                        }
                        if (!hadStderr && freopen_s(&stream, "CONOUT$", "w", stderr) == 0)
                        {
                            std::setvbuf(stderr, nullptr, _IONBF, 0);
                            std::cerr.clear();
                            std::wcerr.clear();
                        }
                    }
                }
                stdoutIsConsole = IsConsoleHandle(GetStdHandle(STD_OUTPUT_HANDLE));
            }

            ~ScopedParentConsole() noexcept
            {
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
            L"MandelbrotViewer - Command-Line Zoom Movie Export\n"
            L"\n"
            L"Usage:\n"
            L"  MandelbrotViewer.exe                               Launch interactive GUI\n"
            L"  MandelbrotViewer.exe --export-movie <out.mp4> [options]\n"
            L"  MandelbrotViewer.exe --help | -h | /?\n"
            L"\n"
            L"Output Options:\n"
            L"  --export-movie <path>        Export a zoom movie headlessly to <path>\n"
            L"  -o, --output <path>          Output MP4 file path (alternative to positional path)\n"
            L"  --resolution <preset|WxH>    720p, 1080p (default), 1440p, 4k, 8k, or WxH (max 7680x4320)\n"
            L"  --width <px>                 Override output width (1..7680)\n"
            L"  --height <px>                Override output height (1..4320)\n"
            L"  --fps <rate>                 Frame rate: 24, 25, 30 (default), 48, 50, or 60\n"
            L"  --duration <seconds>         Movie length in whole seconds (1..3600, default: 30)\n"
            L"  --codec <h264|hevc>          Video codec: h264/avc (default) or hevc/h265\n"
            L"  --motion-smoothing           Enable analytical motion-blur shutter (default)\n"
            L"  --no-motion-smoothing        Disable motion-blur shutter\n"
            L"\n"
            L"View & Source Options:\n"
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
            L"  --gpu / --no-gpu (--cpu)     Use GPU acceleration (default) or CPU-only rendering\n"
            L"  --theme <name>               electric-blue (default), inferno, psychedelic, cyberpunk,\n"
            L"                               viridis, twilight-ocean, sunset-glow, monochrome\n"
            L"  --frequency <float>          Palette cycle frequency (> 0, default: 1.0)\n"
            L"  --phase <float>              Palette phase offset (default: 0.0)\n"
            L"  --invert / --no-invert       Invert palette gradient direction\n"
            L"  --interior-color <hex>       Interior color (#RRGGBB, #AARRGGBB, or 0xAARRGGBB)\n"
            L"  -q, --quiet                  Suppress progress output (errors still written to stderr)\n"
            L"\n"
            L"Examples:\n"
            L"  MandelbrotViewer.exe --export-movie zoom.mp4 --bookmark \"Seahorse Valley\" --start-zoom 1 --end-zoom 85000 --duration 15\n"
            L"  MandelbrotViewer.exe --export-movie auto.mp4 --auto-target --resolution 4k --fps 60 --duration 30 --theme inferno\n"
            L"  MandelbrotViewer.exe --export-movie custom.mp4 --config view.json --direction in --zoom-speed 1.5 --duration 20\n";
    }

    void ApplyViewStateJson(const std::string& jsonUtf8, MoviePlan& plan, bool& outLoadedIterations)
    {
        outLoadedIterations = false;
        std::string_view view(jsonUtf8);
        if (view.size() >= 3 &&
            static_cast<uint8_t>(view[0]) == 0xEF &&
            static_cast<uint8_t>(view[1]) == 0xBB &&
            static_cast<uint8_t>(view[2]) == 0xBF)
        {
            view.remove_prefix(3);
        }

        JsonParser parser(view);
        const JsonValue root = parser.Parse();
        const auto* obj = std::get_if<JsonValue::Object>(&root.data);
        if (!obj)
        {
            throw std::invalid_argument("Config JSON root must be an object.");
        }

        if (const auto it = obj->find("fractalType"); it != obj->end())
        {
            const auto* name = std::get_if<std::string>(&it->second.data);
            if (!name) throw std::invalid_argument("Config JSON 'fractalType' must be a string.");
            FractalType parsedType = FractalType::Mandelbrot;
            std::optional<bool> parsedJulia;
            if (!TryParseFractalType(*name, parsedType, parsedJulia))
            {
                throw std::invalid_argument("Unknown 'fractalType' in config JSON: " + *name);
            }
            plan.start.fractalType = parsedType;
            if (parsedJulia) plan.start.juliaSet = *parsedJulia;
        }

        BigFloat cx = plan.start.hpCenterX;
        BigFloat cy = plan.start.hpCenterY;
        bool hasCenter = false;
        if (const auto it = obj->find("centerX"); it != obj->end())
        {
            if (!TryExtractJsonBigFloat(it->second, cx))
                throw std::invalid_argument("Invalid 'centerX' in config JSON.");
            hasCenter = true;
        }
        if (const auto it = obj->find("centerY"); it != obj->end())
        {
            if (!TryExtractJsonBigFloat(it->second, cy))
                throw std::invalid_argument("Invalid 'centerY' in config JSON.");
            hasCenter = true;
        }
        if (hasCenter)
        {
            plan.start.SetCenter(cx, cy);
        }

        if (const auto it = obj->find("zoom"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            if (!n || !std::isfinite(n->numeric) || n->numeric < MandelbrotEngine::MinZoom || n->numeric > MandelbrotEngine::MaxZoom)
                throw std::invalid_argument("Invalid 'zoom' in config JSON.");
            plan.start.zoom = n->numeric;
        }

        bool hasJuliaParam = false;
        if (const auto it = obj->find("juliaParameterX"); it != obj->end())
        {
            if (!TryExtractJsonBigFloat(it->second, plan.start.juliaX))
                throw std::invalid_argument("Invalid 'juliaParameterX' in config JSON.");
            hasJuliaParam = true;
        }
        if (const auto it = obj->find("juliaParameterY"); it != obj->end())
        {
            if (!TryExtractJsonBigFloat(it->second, plan.start.juliaY))
                throw std::invalid_argument("Invalid 'juliaParameterY' in config JSON.");
            hasJuliaParam = true;
        }
        if (hasJuliaParam)
        {
            plan.start.juliaSet = true;
        }
        if (plan.start.juliaSet && !SupportsJuliaCompanion(plan.start.fractalType))
        {
            throw std::invalid_argument("Julia mode in config JSON is only supported for Mandelbrot and Burning Ship.");
        }

        if (const auto it = obj->find("maxIterations"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            if (!n || n->numeric < 1.0 || n->numeric > 100000000.0 || std::floor(n->numeric) != n->numeric)
                throw std::invalid_argument("Invalid 'maxIterations' in config JSON.");
            plan.start.maxIterations = static_cast<int>(n->numeric);
            plan.baseIterations = plan.start.maxIterations;
            outLoadedIterations = true;
        }

        if (const auto it = obj->find("smooth"); it != obj->end())
        {
            const auto* b = std::get_if<bool>(&it->second.data);
            if (!b) throw std::invalid_argument("Config JSON 'smooth' must be a boolean.");
            plan.start.smooth = *b;
        }

        if (const auto it = obj->find("frequency"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            if (!n || !std::isfinite(n->numeric) || n->numeric <= 0.0)
                throw std::invalid_argument("Invalid 'frequency' in config JSON.");
            plan.frequency = static_cast<float>(n->numeric);
        }

        if (const auto it = obj->find("phaseOffset"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            if (!n || !std::isfinite(n->numeric))
                throw std::invalid_argument("Invalid 'phaseOffset' in config JSON.");
            plan.phase = static_cast<float>(n->numeric);
        }

        if (const auto it = obj->find("invert"); it != obj->end())
        {
            const auto* b = std::get_if<bool>(&it->second.data);
            if (!b) throw std::invalid_argument("Config JSON 'invert' must be a boolean.");
            plan.invert = *b;
        }

        if (const auto it = obj->find("interiorColor"); it != obj->end())
        {
            if (const auto* n = std::get_if<JsonValue::Number>(&it->second.data))
            {
                if (n->numeric < 0.0 || n->numeric > 4294967295.0 || std::floor(n->numeric) != n->numeric)
                    throw std::invalid_argument("Invalid 'interiorColor' in config JSON.");
                plan.interior = static_cast<uint32_t>(n->numeric);
            }
            else if (const auto* s = std::get_if<std::string>(&it->second.data))
            {
                if (!TryParseColor(*s, plan.interior))
                    throw std::invalid_argument("Invalid 'interiorColor' string in config JSON.");
            }
            else
            {
                throw std::invalid_argument("Config JSON 'interiorColor' must be an integer or color string.");
            }
        }

        std::optional<PresetTheme> parsedPreset;
        if (const auto it = obj->find("theme"); it != obj->end())
        {
            const auto* name = std::get_if<std::string>(&it->second.data);
            if (!name) throw std::invalid_argument("Config JSON 'theme' must be a string.");
            PresetTheme preset = PresetTheme::ElectricBlue;
            if (!TryParsePresetTheme(*name, preset))
                throw std::invalid_argument("Unknown 'theme' in config JSON: " + *name);
            parsedPreset = preset;
            if (preset != PresetTheme::Custom)
            {
                plan.theme.SetPreset(preset);
            }
        }

        if (const auto it = obj->find("gradientStops"); it != obj->end())
        {
            const auto* arr = std::get_if<JsonValue::Array>(&it->second.data);
            if (!arr) throw std::invalid_argument("Config JSON 'gradientStops' must be an array.");
            std::vector<GradientStop> stops;
            stops.reserve(arr->size());
            for (const auto& entry : *arr)
            {
                const auto* stopObj = std::get_if<JsonValue::Object>(&entry.data);
                if (!stopObj) throw std::invalid_argument("Each entry in 'gradientStops' must be an object.");
                auto getNum = [&](const char* key) -> double
                {
                    const auto f = stopObj->find(key);
                    if (f == stopObj->end()) throw std::invalid_argument(std::string("Missing '") + key + "' in gradientStop.");
                    const auto* n = std::get_if<JsonValue::Number>(&f->second.data);
                    if (!n || !std::isfinite(n->numeric))
                        throw std::invalid_argument(std::string("Invalid '") + key + "' in gradientStop.");
                    return n->numeric;
                };
                const double pos = getNum("position");
                const double r = getNum("r");
                const double g = getNum("g");
                const double b = getNum("b");
                if (pos < 0.0 || pos > 1.0 || r < 0.0 || r > 255.0 || g < 0.0 || g > 255.0 || b < 0.0 || b > 255.0)
                {
                    throw std::invalid_argument("Gradient stop values out of range in config JSON.");
                }
                stops.push_back(GradientStop{
                    static_cast<float>(pos),
                    static_cast<uint8_t>(std::lround(r)),
                    static_cast<uint8_t>(std::lround(g)),
                    static_cast<uint8_t>(std::lround(b))
                });
            }
            if (!stops.empty())
            {
                const bool matchesPreset = parsedPreset && *parsedPreset != PresetTheme::Custom &&
                    StopsEqualPreset(stops, GetPresetStops(*parsedPreset));
                if (!matchesPreset)
                {
                    plan.theme.SetStops(stops);
                }
            }
            else if (parsedPreset && *parsedPreset == PresetTheme::Custom)
            {
                throw std::invalid_argument("Custom theme in config JSON requires at least one gradient stop.");
            }
        }
        else if (parsedPreset && *parsedPreset == PresetTheme::Custom)
        {
            throw std::invalid_argument("Custom theme in config JSON requires 'gradientStops'.");
        }
    }

    CliParseResult ParseMovieCli(const std::vector<std::wstring>& args)
    {
        CliParseResult result;
        if (args.empty())
        {
            result.action = CliAction::LaunchGui;
            return result;
        }

        auto fail = [&](std::wstring message) -> CliParseResult
        {
            CliParseResult err;
            err.action = CliAction::Error;
            err.errorMessage = std::move(message);
            return err;
        };

        bool exportRequested = false;
        std::wstring outputPath;
        std::optional<std::wstring> configPath;
        std::optional<FractalType> fractalType;
        std::optional<bool> fractalImpliedJulia;
        std::optional<std::wstring> bookmarkName;
        std::optional<BigFloat> centerX;
        std::optional<BigFloat> centerY;
        std::optional<BigFloat> targetX;
        std::optional<BigFloat> targetY;
        std::optional<double> startZoom;
        std::optional<double> endZoom;
        std::optional<bool> zoomInDirection;
        std::optional<double> zoomSpeed;
        std::optional<bool> autoTarget;
        std::optional<uint64_t> seed;
        std::optional<int> resWidth;
        std::optional<int> resHeight;
        std::optional<int> overrideWidth;
        std::optional<int> overrideHeight;
        std::optional<int> fps;
        std::optional<int> duration;
        std::optional<MovieCodec> codec;
        std::optional<bool> motionSmoothing;
        std::optional<int> iterations;
        std::optional<bool> autoIterations;
        std::optional<bool> smooth;
        std::optional<bool> useGpu;
        std::optional<PresetTheme> themePreset;
        std::optional<float> frequency;
        std::optional<float> phase;
        std::optional<bool> invert;
        std::optional<uint32_t> interiorColor;
        std::optional<BigFloat> juliaX;
        std::optional<BigFloat> juliaY;
        bool juliaRequested = false;
        bool quiet = false;

        try
        {
            for (size_t i = 0; i < args.size(); ++i)
            {
                const std::wstring& token = args[i];
                if (token == L"--help" || token == L"-h" || token == L"/?")
                {
                    result.action = CliAction::ShowHelp;
                    return result;
                }

                std::wstring_view flagView = token;
                std::optional<std::wstring_view> inlineValue;
                if (flagView.size() >= 2 && flagView[0] == L'-')
                {
                    const size_t eq = flagView.find(L'=');
                    if (eq != std::wstring_view::npos)
                    {
                        inlineValue = flagView.substr(eq + 1);
                        flagView = flagView.substr(0, eq);
                    }
                }

                auto requireNoValue = [&]()
                {
                    if (inlineValue.has_value())
                    {
                        throw std::invalid_argument("Option '" + WideToAsciiLossy(flagView) + "' does not take a value.");
                    }
                };

                auto takeValue = [&](const char* optionName) -> std::wstring_view
                {
                    if (inlineValue.has_value())
                    {
                        if (inlineValue->empty())
                        {
                            throw std::invalid_argument(std::string("Missing value for option '") + optionName + "'.");
                        }
                        return *inlineValue;
                    }
                    if (i + 1 >= args.size())
                    {
                        throw std::invalid_argument(std::string("Missing value for option '") + optionName + "'.");
                    }
                    return args[++i];
                };

                if (flagView == L"--export-movie")
                {
                    exportRequested = true;
                    if (inlineValue.has_value())
                    {
                        if (inlineValue->empty()) return fail(L"Missing output file path for --export-movie.");
                        if (!outputPath.empty()) return fail(L"Output path specified more than once.");
                        outputPath.assign(*inlineValue);
                    }
                    else if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != L'-')
                    {
                        if (!outputPath.empty()) return fail(L"Output path specified more than once.");
                        outputPath = args[++i];
                    }
                }
                else if (flagView == L"-o" || flagView == L"--output")
                {
                    const auto val = takeValue("--output");
                    if (!outputPath.empty()) return fail(L"Output path specified more than once.");
                    outputPath.assign(val);
                }
                else if (flagView == L"--config")
                {
                    configPath = std::wstring(takeValue("--config"));
                }
                else if (flagView == L"--fractal")
                {
                    std::string ascii;
                    const auto val = takeValue("--fractal");
                    FractalType ft = FractalType::Mandelbrot;
                    std::optional<bool> impJulia;
                    if (!TryWideToAscii(val, ascii) || !TryParseFractalType(ascii, ft, impJulia))
                        return fail(L"Unknown fractal type: " + std::wstring(val));
                    fractalType = ft;
                    if (impJulia) fractalImpliedJulia = impJulia;
                }
                else if (flagView == L"--bookmark")
                {
                    bookmarkName = std::wstring(takeValue("--bookmark"));
                }
                else if (flagView == L"--center-x")
                {
                    BigFloat v;
                    const auto val = takeValue("--center-x");
                    if (!BigFloat::TryParse(val, v) || !std::isfinite(v.ToDouble()))
                        return fail(L"Invalid --center-x coordinate: " + std::wstring(val));
                    centerX = v;
                }
                else if (flagView == L"--center-y")
                {
                    BigFloat v;
                    const auto val = takeValue("--center-y");
                    if (!BigFloat::TryParse(val, v) || !std::isfinite(v.ToDouble()))
                        return fail(L"Invalid --center-y coordinate: " + std::wstring(val));
                    centerY = v;
                }
                else if (flagView == L"--target-x")
                {
                    BigFloat v;
                    const auto val = takeValue("--target-x");
                    if (!BigFloat::TryParse(val, v) || !std::isfinite(v.ToDouble()))
                        return fail(L"Invalid --target-x coordinate: " + std::wstring(val));
                    targetX = v;
                }
                else if (flagView == L"--target-y")
                {
                    BigFloat v;
                    const auto val = takeValue("--target-y");
                    if (!BigFloat::TryParse(val, v) || !std::isfinite(v.ToDouble()))
                        return fail(L"Invalid --target-y coordinate: " + std::wstring(val));
                    targetY = v;
                }
                else if (flagView == L"--start-zoom" || flagView == L"--zoom")
                {
                    double v = 0.0;
                    const auto val = takeValue("--start-zoom");
                    if (!TryParseDouble(val, v) || v < MandelbrotEngine::MinZoom || v > MandelbrotEngine::MaxZoom)
                        return fail(L"Invalid --start-zoom value: " + std::wstring(val));
                    startZoom = v;
                }
                else if (flagView == L"--end-zoom")
                {
                    double v = 0.0;
                    const auto val = takeValue("--end-zoom");
                    if (!TryParseDouble(val, v) || v < MandelbrotEngine::MinZoom || v > MandelbrotEngine::MaxZoom)
                        return fail(L"Invalid --end-zoom value: " + std::wstring(val));
                    endZoom = v;
                }
                else if (flagView == L"--direction")
                {
                    const auto val = takeValue("--direction");
                    const std::string key = NormalizeToken(val);
                    if (key == "in") zoomInDirection = true;
                    else if (key == "out") zoomInDirection = false;
                    else return fail(L"Invalid --direction (expected 'in' or 'out'): " + std::wstring(val));
                }
                else if (flagView == L"--zoom-speed")
                {
                    double v = 0.0;
                    const auto val = takeValue("--zoom-speed");
                    if (!TryParseDouble(val, v) || v < MinMovieZoomSpeed || v > MaxMovieZoomSpeed)
                        return fail(L"Invalid --zoom-speed (expected 0.2 to 5.0): " + std::wstring(val));
                    zoomSpeed = v;
                }
                else if (flagView == L"--auto-target")
                {
                    requireNoValue();
                    autoTarget = true;
                }
                else if (flagView == L"--no-auto-target")
                {
                    requireNoValue();
                    autoTarget = false;
                }
                else if (flagView == L"--seed")
                {
                    uint64_t v = 0;
                    const auto val = takeValue("--seed");
                    if (!TryParseUint64(val, v))
                        return fail(L"Invalid --seed (expected unsigned 64-bit integer): " + std::wstring(val));
                    seed = v;
                }
                else if (flagView == L"--resolution")
                {
                    int w = 0, h = 0;
                    const auto val = takeValue("--resolution");
                    if (!TryParseResolution(val, w, h))
                        return fail(L"Invalid --resolution (use 720p, 1080p, 1440p, 4k, 8k, or WxH up to 7680x4320): " + std::wstring(val));
                    resWidth = w;
                    resHeight = h;
                }
                else if (flagView == L"--width")
                {
                    int w = 0;
                    const auto val = takeValue("--width");
                    if (!TryParseInt(val, w) || w < 1 || w > 7680)
                        return fail(L"Invalid --width (expected 1..7680): " + std::wstring(val));
                    overrideWidth = w;
                }
                else if (flagView == L"--height")
                {
                    int h = 0;
                    const auto val = takeValue("--height");
                    if (!TryParseInt(val, h) || h < 1 || h > 4320)
                        return fail(L"Invalid --height (expected 1..4320): " + std::wstring(val));
                    overrideHeight = h;
                }
                else if (flagView == L"--fps")
                {
                    int v = 0;
                    const auto val = takeValue("--fps");
                    if (!TryParseInt(val, v) ||
                        std::find(MovieFrameRates.begin(), MovieFrameRates.end(), v) == MovieFrameRates.end())
                    {
                        return fail(L"Invalid --fps (expected 24, 25, 30, 48, 50, or 60): " + std::wstring(val));
                    }
                    fps = v;
                }
                else if (flagView == L"--duration")
                {
                    int v = 0;
                    const auto val = takeValue("--duration");
                    if (!TryParseInt(val, v) || v < 1 || v > 3600)
                        return fail(L"Invalid --duration (expected whole seconds 1..3600): " + std::wstring(val));
                    duration = v;
                }
                else if (flagView == L"--codec")
                {
                    MovieCodec c = MovieCodec::H264;
                    const auto val = takeValue("--codec");
                    if (!TryParseCodec(val, c))
                        return fail(L"Invalid --codec (expected h264 or hevc): " + std::wstring(val));
                    codec = c;
                }
                else if (flagView == L"--motion-smoothing")
                {
                    requireNoValue();
                    motionSmoothing = true;
                }
                else if (flagView == L"--no-motion-smoothing")
                {
                    requireNoValue();
                    motionSmoothing = false;
                }
                else if (flagView == L"--iterations" || flagView == L"--max-iterations")
                {
                    int v = 0;
                    const auto val = takeValue("--iterations");
                    if (!TryParseInt(val, v) || v < 1)
                        return fail(L"Invalid --iterations (expected positive integer): " + std::wstring(val));
                    iterations = v;
                }
                else if (flagView == L"--auto-iterations")
                {
                    requireNoValue();
                    autoIterations = true;
                }
                else if (flagView == L"--no-auto-iterations")
                {
                    requireNoValue();
                    autoIterations = false;
                }
                else if (flagView == L"--smooth")
                {
                    requireNoValue();
                    smooth = true;
                }
                else if (flagView == L"--no-smooth")
                {
                    requireNoValue();
                    smooth = false;
                }
                else if (flagView == L"--gpu")
                {
                    requireNoValue();
                    useGpu = true;
                }
                else if (flagView == L"--no-gpu" || flagView == L"--cpu")
                {
                    requireNoValue();
                    useGpu = false;
                }
                else if (flagView == L"--theme")
                {
                    std::string ascii;
                    const auto val = takeValue("--theme");
                    PresetTheme preset = PresetTheme::ElectricBlue;
                    if (!TryWideToAscii(val, ascii) || !TryParsePresetTheme(ascii, preset) || preset == PresetTheme::Custom)
                        return fail(L"Unknown color theme: " + std::wstring(val));
                    themePreset = preset;
                }
                else if (flagView == L"--frequency")
                {
                    double v = 0.0;
                    const auto val = takeValue("--frequency");
                    if (!TryParseDouble(val, v) || v <= 0.0)
                        return fail(L"Invalid --frequency (expected positive number): " + std::wstring(val));
                    frequency = static_cast<float>(v);
                }
                else if (flagView == L"--phase")
                {
                    double v = 0.0;
                    const auto val = takeValue("--phase");
                    if (!TryParseDouble(val, v))
                        return fail(L"Invalid --phase (expected finite number): " + std::wstring(val));
                    phase = static_cast<float>(v);
                }
                else if (flagView == L"--invert")
                {
                    requireNoValue();
                    invert = true;
                }
                else if (flagView == L"--no-invert")
                {
                    requireNoValue();
                    invert = false;
                }
                else if (flagView == L"--interior-color")
                {
                    std::string ascii;
                    uint32_t color = 0;
                    const auto val = takeValue("--interior-color");
                    if (!TryWideToAscii(val, ascii) || !TryParseColor(ascii, color))
                        return fail(L"Invalid --interior-color (use #RRGGBB, #AARRGGBB, or 0xAARRGGBB): " + std::wstring(val));
                    interiorColor = color;
                }
                else if (flagView == L"--julia")
                {
                    const auto val = takeValue("--julia");
                    const size_t comma = val.find(L',');
                    if (comma == std::wstring_view::npos || val.find(L',', comma + 1) != std::wstring_view::npos)
                        return fail(L"Invalid --julia parameter (expected x,y): " + std::wstring(val));
                    BigFloat jx, jy;
                    if (!BigFloat::TryParse(val.substr(0, comma), jx) || !BigFloat::TryParse(val.substr(comma + 1), jy) ||
                        !std::isfinite(jx.ToDouble()) || !std::isfinite(jy.ToDouble()))
                    {
                        return fail(L"Invalid --julia coordinates: " + std::wstring(val));
                    }
                    juliaX = jx;
                    juliaY = jy;
                    juliaRequested = true;
                }
                else if (flagView == L"--julia-x")
                {
                    BigFloat v;
                    const auto val = takeValue("--julia-x");
                    if (!BigFloat::TryParse(val, v) || !std::isfinite(v.ToDouble()))
                        return fail(L"Invalid --julia-x coordinate: " + std::wstring(val));
                    juliaX = v;
                    juliaRequested = true;
                }
                else if (flagView == L"--julia-y")
                {
                    BigFloat v;
                    const auto val = takeValue("--julia-y");
                    if (!BigFloat::TryParse(val, v) || !std::isfinite(v.ToDouble()))
                        return fail(L"Invalid --julia-y coordinate: " + std::wstring(val));
                    juliaY = v;
                    juliaRequested = true;
                }
                else if (flagView == L"-q" || flagView == L"--quiet")
                {
                    requireNoValue();
                    quiet = true;
                }
                else
                {
                    return fail(L"Unknown command-line argument: " + token);
                }
            }
        }
        catch (const std::exception& e)
        {
            return fail(Utf8ToWide(e.what()));
        }

        if (!exportRequested)
        {
            return fail(L"Movie export options require --export-movie <output.mp4>.");
        }
        if (outputPath.empty())
        {
            return fail(L"Missing output file path for --export-movie.");
        }
        if (endZoom.has_value() && (zoomInDirection.has_value() || zoomSpeed.has_value()))
        {
            return fail(L"Cannot combine --end-zoom with --direction or --zoom-speed.");
        }
        if (autoTarget.value_or(false) && (targetX.has_value() || targetY.has_value()))
        {
            return fail(L"Cannot combine --auto-target with explicit --target-x or --target-y.");
        }

        MoviePlan plan;
        bool configLoadedIterations = false;
        bool configWasJulia = false;

        try
        {
            // 1. Optional JSON config file (base view & theme).
            if (configPath)
            {
                std::ifstream file{ std::filesystem::path(*configPath), std::ios::binary };
                if (!file)
                {
                    return fail(L"Cannot open config file: " + *configPath);
                }
                std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (contents.empty())
                {
                    return fail(L"Config file is empty: " + *configPath);
                }
                ApplyViewStateJson(contents, plan, configLoadedIterations);
                configWasJulia = plan.start.juliaSet;
            }

            // 2. Output dimensions (needed before ResetJuliaView so Julia fits target aspect).
            if (resWidth && resHeight)
            {
                plan.width = *resWidth;
                plan.height = *resHeight;
            }
            if (overrideWidth) plan.width = *overrideWidth;
            if (overrideHeight) plan.height = *overrideHeight;
            plan.start.width = plan.width;
            plan.start.height = plan.height;

            // 3. Fractal formula selection.
            if (fractalType)
            {
                plan.start.fractalType = *fractalType;
                if (fractalImpliedJulia)
                {
                    plan.start.juliaSet = *fractalImpliedJulia;
                }
                else if (!configPath && !bookmarkName && !juliaRequested)
                {
                    const auto& bms = GetBookmarks(*fractalType);
                    if (!bms.empty())
                    {
                        plan.start.SetCenter(bms[0].centerX, bms[0].centerY);
                        plan.start.zoom = bms[0].zoom;
                    }
                }
            }

            // 4. Optional named landmark bookmark.
            if (bookmarkName)
            {
                FractalType bmType = plan.start.fractalType;
                BookmarkLocation bm{};
                if (!TryFindBookmark(*bookmarkName, fractalType, bmType, bm))
                {
                    return fail(L"Unknown bookmark: " + *bookmarkName);
                }
                plan.start.fractalType = bmType;
                plan.start.SetCenter(bm.centerX, bm.centerY);
                plan.start.zoom = bm.zoom;
                plan.start.maxIterations = bm.maxIterations;
                if (!juliaRequested) plan.start.juliaSet = false;
            }

            // 5. Explicit Julia configuration.
            if (juliaRequested || plan.start.juliaSet)
            {
                if (!SupportsJuliaCompanion(plan.start.fractalType))
                {
                    return fail(L"Julia mode is only supported for Mandelbrot and Burning Ship.");
                }
                if (juliaRequested)
                {
                    plan.start.juliaSet = true;
                    if (juliaX) plan.start.juliaX = *juliaX;
                    if (juliaY) plan.start.juliaY = *juliaY;
                    if (!configWasJulia)
                    {
                        ResetJuliaView(plan.start);
                    }
                }
            }

            // 6. Explicit camera coordinates & view overrides.
            if (centerX || centerY)
            {
                plan.start.SetCenter(
                    centerX.value_or(plan.start.hpCenterX),
                    centerY.value_or(plan.start.hpCenterY));
            }
            if (startZoom) plan.start.zoom = *startZoom;
            if (smooth) plan.start.smooth = *smooth;

            // 7. Color & theme overrides.
            if (themePreset) plan.theme.SetPreset(*themePreset);
            if (frequency) plan.frequency = *frequency;
            if (phase) plan.phase = *phase;
            if (invert) plan.invert = *invert;
            if (interiorColor) plan.interior = *interiorColor;

            // 8. Movie encoding & rendering overrides.
            if (fps) plan.fps = *fps;
            if (duration) plan.seconds = *duration;
            if (codec) plan.codec = *codec;
            if (motionSmoothing) plan.motionSmoothing = *motionSmoothing;
            if (useGpu) plan.useGpu = *useGpu;

            // 9. Iteration budget & auto-scaling.
            if (autoIterations)
            {
                plan.autoScaleIterations = *autoIterations;
            }
            else if (iterations || configLoadedIterations)
            {
                plan.autoScaleIterations = false;
            }
            else
            {
                plan.autoScaleIterations = true;
            }

            if (iterations)
            {
                plan.baseIterations = *iterations;
                plan.start.maxIterations = *iterations;
            }
            else if (configLoadedIterations || bookmarkName)
            {
                plan.baseIterations = plan.start.maxIterations;
            }
            else
            {
                plan.baseIterations = 500;
                plan.start.maxIterations = 500;
            }

            if (plan.autoScaleIterations)
            {
                plan.start.maxIterations = RecommendMaxIterations(
                    plan.start.zoom, plan.start.fractalType, plan.baseIterations);
            }

            // 10. Zoom motion & target.
            if (endZoom)
            {
                plan.endZoom = *endZoom;
            }
            else
            {
                plan.SetZoomMotion(zoomInDirection.value_or(true), zoomSpeed.value_or(1.0));
            }

            plan.autoRetarget = autoTarget.value_or(false);
            if (plan.autoRetarget)
            {
                plan.targetX = plan.start.hpCenterX;
                plan.targetY = plan.start.hpCenterY;
                if (seed)
                {
                    plan.targetSeed = *seed;
                }
                else
                {
                    std::random_device rd;
                    const uint64_t generated = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
                    plan.targetSeed = (generated == 0) ? 1ULL : generated;
                }
            }
            else
            {
                plan.targetX = targetX.value_or(plan.start.hpCenterX);
                plan.targetY = targetY.value_or(plan.start.hpCenterY);
            }

            plan.Validate();
        }
        catch (const std::exception& e)
        {
            return fail(Utf8ToWide(e.what()));
        }

        result.action = CliAction::ExportMovie;
        result.options.plan = std::move(plan);
        result.options.outputPath = std::move(outputPath);
        result.options.quiet = quiet;
        return result;
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
            out << L"Exporting zoom movie: " << options.outputPath << L"\n"
                << L"  Fractal: " << fractalName
                << L" | " << plan.width << L"x" << plan.height
                << L" @ " << plan.fps << L" fps | " << plan.seconds << L"s (" << plan.FrameCount() << L" frames) | "
                << codecName << L"\n"
                << L"  Zoom: " << FormatZoomShort(plan.start.zoom) << L" -> " << FormatZoomShort(plan.endZoom);
            if (plan.autoRetarget)
            {
                out << L" | Target: Auto-Pick (seed " << plan.targetSeed << L")\n";
            }
            else
            {
                out << L" | Target: (" << plan.targetX.ToWString(12) << L", " << plan.targetY.ToWString(12) << L")\n";
            }
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
            err << L"Export failed: " << Utf8ToWide(e.what()) << L"\n";
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

        ScopedParentConsole console;
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

        return RunMovieExportCli(
            parsed.options,
            std::wcout,
            std::wcerr,
            &g_consoleCancelRequested,
            console.stdoutIsConsole);
    }
#endif
}
