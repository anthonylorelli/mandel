#include "ViewStateJson.h"
#include "MovieCli.h"
#include "JuliaView.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>
#include <variant>
#include <vector>

namespace Mandelbrot
{
    namespace detail
    {
        std::wstring Utf8ToWide(std::string_view utf8)
        {
            std::wstring out;
            out.reserve(utf8.size());
            auto isCont = [](uint8_t b) noexcept { return (b & 0xC0u) == 0x80u; };
            size_t i = 0;
            while (i < utf8.size())
            {
                const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
                if (b0 < 0x80)
                {
                    out.push_back(static_cast<wchar_t>(b0));
                    ++i;
                }
                else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size() &&
                         isCont(static_cast<uint8_t>(utf8[i + 1])))
                {
                    const uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
                    if (cp < 0x80u) { out.push_back(L'?'); ++i; continue; }
                    out.push_back(static_cast<wchar_t>(cp));
                    i += 2;
                }
                else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size() &&
                         isCont(static_cast<uint8_t>(utf8[i + 1])) &&
                         isCont(static_cast<uint8_t>(utf8[i + 2])))
                {
                    const uint32_t cp = ((b0 & 0x0Fu) << 12) |
                        ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 6) |
                        (static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu);
                    if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) { out.push_back(L'?'); ++i; continue; }
                    out.push_back(static_cast<wchar_t>(cp));
                    i += 3;
                }
                else if ((b0 & 0xF8u) == 0xF0u && i + 3 < utf8.size() &&
                         isCont(static_cast<uint8_t>(utf8[i + 1])) &&
                         isCont(static_cast<uint8_t>(utf8[i + 2])) &&
                         isCont(static_cast<uint8_t>(utf8[i + 3])))
                {
                    const uint32_t cp = ((b0 & 0x07u) << 18) |
                        ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 12) |
                        ((static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu) << 6) |
                        (static_cast<uint8_t>(utf8[i + 3]) & 0x3Fu);
                    if (cp < 0x10000u || cp > 0x10FFFFu) { out.push_back(L'?'); ++i; continue; }
                    if constexpr (sizeof(wchar_t) == 2)
                    {
                        const uint32_t v = cp - 0x10000u;
                        out.push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
                        out.push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
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
    }

    namespace
    {
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
            static constexpr int kMaxNestingDepth = 64;

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
                            auto parseHex4 = [&]() -> uint32_t
                            {
                                if (m_pos + 4 > m_text.size()) throw std::invalid_argument("Invalid \\u escape in JSON string.");
                                uint32_t v = 0;
                                const auto [ptr, ec] = std::from_chars(m_text.data() + m_pos, m_text.data() + m_pos + 4, v, 16);
                                if (ec != std::errc{} || ptr != m_text.data() + m_pos + 4)
                                    throw std::invalid_argument("Invalid hex digits in \\u escape in JSON string.");
                                m_pos += 4;
                                return v;
                            };
                            uint32_t cp = parseHex4();
                            if (cp >= 0xD800u && cp <= 0xDBFFu)
                            {
                                if (m_pos + 2 > m_text.size() || m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u')
                                    throw std::invalid_argument("Unpaired high surrogate in JSON string.");
                                m_pos += 2;
                                const uint32_t lo = parseHex4();
                                if (lo < 0xDC00u || lo > 0xDFFFu)
                                    throw std::invalid_argument("Invalid low surrogate in JSON string.");
                                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                            }
                            else if (cp >= 0xDC00u && cp <= 0xDFFFu)
                            {
                                throw std::invalid_argument("Unpaired low surrogate in JSON string.");
                            }
                            if (cp < 0x80u) result.push_back(static_cast<char>(cp));
                            else if (cp < 0x800u)
                            {
                                result.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                                result.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                            }
                            else if (cp < 0x10000u)
                            {
                                result.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                                result.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                                result.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                            }
                            else
                            {
                                result.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                                result.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
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
                if (ec == std::errc::result_out_of_range && ptr == raw.data() + raw.size() && value == 0.0)
                {
                    // Sub-double underflow (e.g., 1e-350): retain exact raw text for BigFloat fields.
                    return JsonValue{ JsonValue::Number{ raw, 0.0 } };
                }
                if (ec != std::errc{} || ptr != raw.data() + raw.size() || !std::isfinite(value))
                {
                    throw std::invalid_argument("Out-of-range number in JSON config.");
                }
                return JsonValue{ JsonValue::Number{ raw, value } };
            }

            JsonValue ParseArray()
            {
                if (++m_depth > kMaxNestingDepth) throw std::invalid_argument("JSON config exceeds maximum nesting depth.");
                struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{ m_depth };
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
                if (++m_depth > kMaxNestingDepth) throw std::invalid_argument("JSON config exceeds maximum nesting depth.");
                struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{ m_depth };
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
            int m_depth{ 0 };
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
            if (!detail::TryParseFractalType(*name, parsedType, parsedJulia))
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
            const float f = n ? static_cast<float>(n->numeric) : 0.0f;
            if (!n || !std::isfinite(f) || !(f > 0.0f))
                throw std::invalid_argument("Invalid 'frequency' in config JSON.");
            plan.frequency = f;
        }

        if (const auto it = obj->find("phaseOffset"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            const float f = n ? static_cast<float>(n->numeric) : 0.0f;
            if (!n || !std::isfinite(f))
                throw std::invalid_argument("Invalid 'phaseOffset' in config JSON.");
            plan.phase = f;
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
                if (!detail::TryParseColor(*s, plan.interior))
                    throw std::invalid_argument("Invalid 'interiorColor' string in config JSON.");
            }
            else
            {
                throw std::invalid_argument("Config JSON 'interiorColor' must be an integer or color string.");
            }
        }

        if (const auto it = obj->find("histogramEqualize"); it != obj->end())
        {
            const auto* b = std::get_if<bool>(&it->second.data);
            if (!b) throw std::invalid_argument("Config JSON 'histogramEqualize' must be a boolean.");
            plan.histogramEqualize = *b;
        }

        if (const auto it = obj->find("lighting3d"); it != obj->end())
        {
            const auto* b = std::get_if<bool>(&it->second.data);
            if (!b) throw std::invalid_argument("Config JSON 'lighting3d' must be a boolean.");
            plan.lighting.enabled = *b;
        }
        if (const auto it = obj->find("lightAzimuth"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            const float f = n ? static_cast<float>(n->numeric) : 0.0f;
            if (!n || !std::isfinite(f))
                throw std::invalid_argument("Invalid 'lightAzimuth' in config JSON.");
            plan.lighting.azimuthDeg = f;
        }
        if (const auto it = obj->find("lightElevation"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            const float f = n ? static_cast<float>(n->numeric) : 0.0f;
            if (!n || !std::isfinite(f) ||
                f < DirectionalLightingParams::MinElevationDeg ||
                f > DirectionalLightingParams::MaxElevationDeg)
                throw std::invalid_argument("Invalid 'lightElevation' in config JSON.");
            plan.lighting.elevationDeg = f;
        }
        if (const auto it = obj->find("lightStrength"); it != obj->end())
        {
            const auto* n = std::get_if<JsonValue::Number>(&it->second.data);
            const float f = n ? static_cast<float>(n->numeric) : 0.0f;
            if (!n || !std::isfinite(f) ||
                f < DirectionalLightingParams::MinStrength ||
                f > DirectionalLightingParams::MaxStrength)
                throw std::invalid_argument("Invalid 'lightStrength' in config JSON.");
            plan.lighting.strength = f;
        }

        std::optional<PresetTheme> parsedPreset;
        if (const auto it = obj->find("theme"); it != obj->end())
        {
            const auto* name = std::get_if<std::string>(&it->second.data);
            if (!name) throw std::invalid_argument("Config JSON 'theme' must be a string.");
            PresetTheme preset = PresetTheme::ElectricBlue;
            if (!detail::TryParsePresetTheme(*name, preset))
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
}
