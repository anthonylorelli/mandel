#include "MovieCli.h"
#include "ViewStateJson.h"
#include "JuliaView.h"
#include "ViewportNavigation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Mandelbrot
{
    namespace
    {
        bool TryParseInt(std::wstring_view text, int& out)
        {
            std::string ascii;
            if (!detail::TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = detail::TrimAscii(ascii);
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
            if (!detail::TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = detail::TrimAscii(ascii);
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
            if (!detail::TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = detail::TrimAscii(ascii);
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

        bool TryParseCodec(std::wstring_view text, MovieCodec& outCodec)
        {
            const std::string key = detail::NormalizeToken(text);
            if (key == "h264" || key == "avc") { outCodec = MovieCodec::H264; return true; }
            if (key == "hevc" || key == "h265") { outCodec = MovieCodec::Hevc; return true; }
            return false;
        }

        bool TryParseAsciiColorMode(std::wstring_view text, AsciiColorMode& outMode)
        {
            const std::string key = detail::NormalizeToken(text);
            if (key == "auto") { outMode = AsciiColorMode::Auto; return true; }
            if (key == "none" || key == "off" || key == "mono" || key == "monochrome") { outMode = AsciiColorMode::None; return true; }
            if (key == "256" || key == "ansi256" || key == "8bit") { outMode = AsciiColorMode::Ansi256; return true; }
            if (key == "truecolor" || key == "24bit" || key == "rgb" || key == "full" || key == "on" || key == "always")
            {
                outMode = AsciiColorMode::TrueColor;
                return true;
            }
            return false;
        }

        bool TryParseAsciiCharset(
            std::wstring_view text,
            AsciiCharsetPreset& outPreset,
            std::string& outCustom)
        {
            const std::string key = detail::NormalizeToken(text);
            if (key == "standard" || key == "default") { outPreset = AsciiCharsetPreset::Standard; outCustom.clear(); return true; }
            if (key == "dense") { outPreset = AsciiCharsetPreset::Dense; outCustom.clear(); return true; }
            if (key == "detailed" || key == "detail" || key == "high") { outPreset = AsciiCharsetPreset::Detailed; outCustom.clear(); return true; }
            if (key == "simple" || key == "minimal") { outPreset = AsciiCharsetPreset::Simple; outCustom.clear(); return true; }

            std::string ascii;
            if (!detail::TryWideToAscii(text, ascii) || !IsValidAsciiCharset(ascii))
            {
                return false;
            }
            outPreset = AsciiCharsetPreset::Custom;
            outCustom = std::move(ascii);
            return true;
        }

        bool TryParseResolution(std::wstring_view text, int& outWidth, int& outHeight)
        {
            const std::string key = detail::NormalizeToken(text);
            if (key == "720p") { outWidth = 1280; outHeight = 720; return true; }
            if (key == "1080p") { outWidth = 1920; outHeight = 1080; return true; }
            if (key == "1440p") { outWidth = 2560; outHeight = 1440; return true; }
            if (key == "4k" || key == "2160p" || key == "4kuhd") { outWidth = 3840; outHeight = 2160; return true; }
            if (key == "8k" || key == "4320p" || key == "8kuhd") { outWidth = 7680; outHeight = 4320; return true; }

            std::string ascii;
            if (!detail::TryWideToAscii(text, ascii)) return false;
            const std::string trimmed = detail::TrimAscii(ascii);
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
            const std::string needle = detail::NormalizeToken(name);
            if (needle.empty()) return false;

            auto searchType = [&](FractalType type) -> bool
            {
                for (const auto& bm : GetBookmarks(type))
                {
                    if (detail::NormalizeToken(bm.name) == needle)
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
        bool asciiRequested = false;
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
        std::optional<bool> asciiAutoZoom;
        bool asciiOnce = false;
        std::optional<int> asciiFrames;
        std::optional<int> asciiCols;
        std::optional<int> asciiRows;
        std::optional<double> asciiCharAspect;
        std::optional<AsciiCharsetPreset> asciiCharsetPreset;
        std::string asciiCustomCharset;
        std::optional<AsciiColorMode> asciiColorMode;
        std::optional<bool> asciiHud;

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
                        throw std::invalid_argument("Option '" + detail::WideToAsciiLossy(flagView) + "' does not take a value.");
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
                    else if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != L'-' && args[i + 1] != L"/?")
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
                    if (!detail::TryWideToAscii(val, ascii) || !detail::TryParseFractalType(ascii, ft, impJulia))
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
                    const std::string key = detail::NormalizeToken(val);
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
                    if (!TryParseInt(val, v) || v < MinConsoleFps || v > MaxConsoleFps)
                    {
                        return fail(L"Invalid --fps (expected 1..60): " + std::wstring(val));
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
                    if (!detail::TryWideToAscii(val, ascii) || !detail::TryParsePresetTheme(ascii, preset) || preset == PresetTheme::Custom)
                        return fail(L"Unknown color theme: " + std::wstring(val));
                    themePreset = preset;
                }
                else if (flagView == L"--frequency")
                {
                    double v = 0.0;
                    const auto val = takeValue("--frequency");
                    const bool ok = TryParseDouble(val, v);
                    const float f = static_cast<float>(v);
                    if (!ok || !std::isfinite(f) || !(f > 0.0f))
                        return fail(L"Invalid --frequency (expected positive number): " + std::wstring(val));
                    frequency = f;
                }
                else if (flagView == L"--phase")
                {
                    double v = 0.0;
                    const auto val = takeValue("--phase");
                    const bool ok = TryParseDouble(val, v);
                    const float f = static_cast<float>(v);
                    if (!ok || !std::isfinite(f))
                        return fail(L"Invalid --phase (expected finite number): " + std::wstring(val));
                    phase = f;
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
                    if (!detail::TryWideToAscii(val, ascii) || !detail::TryParseColor(ascii, color))
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
                else if (flagView == L"--ascii" || flagView == L"--console")
                {
                    requireNoValue();
                    asciiRequested = true;
                }
                else if (flagView == L"--auto-zoom")
                {
                    requireNoValue();
                    asciiAutoZoom = true;
                }
                else if (flagView == L"--no-auto-zoom")
                {
                    requireNoValue();
                    asciiAutoZoom = false;
                }
                else if (flagView == L"--once")
                {
                    requireNoValue();
                    asciiOnce = true;
                }
                else if (flagView == L"--frames")
                {
                    int v = 0;
                    const auto val = takeValue("--frames");
                    if (!TryParseInt(val, v) || v < 1 || v > 1000000)
                        return fail(L"Invalid --frames (expected 1..1000000): " + std::wstring(val));
                    asciiFrames = v;
                }
                else if (flagView == L"--cols")
                {
                    int v = 0;
                    const auto val = takeValue("--cols");
                    if (!TryParseInt(val, v) || v < MinConsoleCols || v > MaxConsoleCols)
                        return fail(L"Invalid --cols (expected 10..512): " + std::wstring(val));
                    asciiCols = v;
                }
                else if (flagView == L"--rows")
                {
                    int v = 0;
                    const auto val = takeValue("--rows");
                    if (!TryParseInt(val, v) || v < MinConsoleRows || v > MaxConsoleRows)
                        return fail(L"Invalid --rows (expected 5..256): " + std::wstring(val));
                    asciiRows = v;
                }
                else if (flagView == L"--char-aspect")
                {
                    double v = 0.0;
                    const auto val = takeValue("--char-aspect");
                    if (!TryParseDouble(val, v) || v < MinConsoleCharAspect || v > MaxConsoleCharAspect)
                        return fail(L"Invalid --char-aspect (expected 0.5..4.0): " + std::wstring(val));
                    asciiCharAspect = v;
                }
                else if (flagView == L"--charset")
                {
                    const auto val = takeValue("--charset");
                    AsciiCharsetPreset preset = AsciiCharsetPreset::Standard;
                    std::string custom;
                    if (!TryParseAsciiCharset(val, preset, custom))
                        return fail(L"Invalid --charset (use standard, dense, detailed, simple, or >= 2 printable ASCII characters): " + std::wstring(val));
                    asciiCharsetPreset = preset;
                    asciiCustomCharset = std::move(custom);
                }
                else if (flagView == L"--color")
                {
                    const auto val = takeValue("--color");
                    AsciiColorMode mode = AsciiColorMode::Auto;
                    if (!TryParseAsciiColorMode(val, mode))
                        return fail(L"Invalid --color (expected auto, truecolor, 256, or none): " + std::wstring(val));
                    asciiColorMode = mode;
                }
                else if (flagView == L"--no-color")
                {
                    requireNoValue();
                    asciiColorMode = AsciiColorMode::None;
                }
                else if (flagView == L"--hud")
                {
                    requireNoValue();
                    asciiHud = true;
                }
                else if (flagView == L"--no-hud")
                {
                    requireNoValue();
                    asciiHud = false;
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
            return fail(detail::Utf8ToWide(e.what()));
        }

        if (exportRequested && asciiRequested)
        {
            return fail(L"Cannot combine --ascii with --export-movie.");
        }
        if (!exportRequested && !asciiRequested)
        {
            return fail(L"CLI options require --export-movie <output.mp4> or --ascii.");
        }

        const bool hasAsciiOnlyFlag =
            asciiAutoZoom.has_value() || asciiOnce || asciiFrames.has_value() ||
            asciiCols.has_value() || asciiRows.has_value() || asciiCharAspect.has_value() ||
            asciiCharsetPreset.has_value() || asciiColorMode.has_value() || asciiHud.has_value();

        if (exportRequested)
        {
            if (hasAsciiOnlyFlag)
            {
                return fail(L"Console ASCII options cannot be combined with --export-movie.");
            }
            if (outputPath.empty())
            {
                return fail(L"Missing output file path for --export-movie.");
            }
            if (fps && std::find(MovieFrameRates.begin(), MovieFrameRates.end(), *fps) == MovieFrameRates.end())
            {
                return fail(L"Invalid --fps for movie export (expected 24, 25, 30, 48, 50, or 60).");
            }
            if (endZoom.has_value() && (zoomInDirection.has_value() || zoomSpeed.has_value()))
            {
                return fail(L"Cannot combine --end-zoom with --direction or --zoom-speed.");
            }
            if (autoTarget.value_or(false) && (targetX.has_value() || targetY.has_value()))
            {
                return fail(L"Cannot combine --auto-target with explicit --target-x or --target-y.");
            }
        }
        else
        {
            if (!outputPath.empty() || resWidth.has_value() || resHeight.has_value() ||
                codec.has_value() || motionSmoothing.has_value())
            {
                return fail(L"Movie-only options (--output, --resolution, --codec, --motion-smoothing) cannot be combined with --ascii.");
            }
            if (overrideWidth && (*overrideWidth < MinConsoleCols || *overrideWidth > MaxConsoleCols))
            {
                return fail(L"Invalid --width for --ascii (expected 10..512 columns).");
            }
            if (overrideHeight && (*overrideHeight < MinConsoleRows || *overrideHeight > MaxConsoleRows))
            {
                return fail(L"Invalid --height for --ascii (expected 5..256 rows).");
            }
            if (asciiOnce && asciiAutoZoom.value_or(false))
            {
                return fail(L"Cannot combine --once with --auto-zoom.");
            }
            if (asciiOnce && asciiFrames.has_value() && *asciiFrames > 1)
            {
                return fail(L"Cannot combine --once with --frames > 1.");
            }
            if (asciiOnce && duration.has_value())
            {
                return fail(L"Cannot combine --once with --duration.");
            }
            if (autoTarget.value_or(false) && (targetX.has_value() || targetY.has_value()))
            {
                return fail(L"Cannot combine --auto-target with explicit --target-x or --target-y.");
            }
            if (autoTarget.value_or(false) && zoomInDirection.has_value() && !*zoomInDirection)
            {
                return fail(L"--auto-target requires zooming in.");
            }
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
            const double resolvedCharAspect = asciiCharAspect.value_or(DefaultConsoleCharAspect);
            const int resolvedAsciiCols = asciiCols.value_or(overrideWidth.value_or(0));
            const int resolvedAsciiRows = asciiRows.value_or(overrideHeight.value_or(0));
            if (exportRequested)
            {
                if (resWidth && resHeight)
                {
                    plan.width = *resWidth;
                    plan.height = *resHeight;
                }
                if (overrideWidth) plan.width = *overrideWidth;
                if (overrideHeight) plan.height = *overrideHeight;
                plan.start.width = plan.width;
                plan.start.height = plan.height;
            }
            else
            {
                const int previewCols = (resolvedAsciiCols > 0) ? resolvedAsciiCols : DefaultConsoleCols;
                const int previewRows = (resolvedAsciiRows > 0) ? resolvedAsciiRows : DefaultConsoleRows;
                plan.start.width = previewCols;
                plan.start.height = ComputeAsciiPixelHeight(previewRows, resolvedCharAspect);
            }

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
                if (juliaRequested || (fractalImpliedJulia && *fractalImpliedJulia))
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

            // 8. Rendering overrides.
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

            if (asciiRequested)
            {
                ConsoleAsciiOptions consoleOpts;
                consoleOpts.start = plan.start;
                consoleOpts.theme = plan.theme;
                consoleOpts.frequency = plan.frequency;
                consoleOpts.phase = plan.phase;
                consoleOpts.invert = plan.invert;
                consoleOpts.interior = plan.interior;

                consoleOpts.cols = resolvedAsciiCols;
                consoleOpts.rows = resolvedAsciiRows;
                consoleOpts.charAspect = resolvedCharAspect;
                if (asciiCharsetPreset)
                {
                    consoleOpts.charsetPreset = *asciiCharsetPreset;
                    consoleOpts.customCharset = std::move(asciiCustomCharset);
                }
                if (asciiColorMode)
                {
                    consoleOpts.colorMode = *asciiColorMode;
                }
                consoleOpts.showHud = asciiHud.value_or(true) && !quiet;
                consoleOpts.quiet = quiet;

                consoleOpts.autoScaleIterations = plan.autoScaleIterations;
                consoleOpts.baseIterations = plan.baseIterations;
                consoleOpts.useGpu = useGpu.value_or(false);
                consoleOpts.fps = fps.value_or(DefaultConsoleFps);
                consoleOpts.maxDurationSeconds = duration.value_or(0);
                consoleOpts.maxFrames = asciiFrames.value_or(0);
                consoleOpts.once = asciiOnce;
                consoleOpts.zoomSpeed = zoomSpeed.value_or(1.0);
                consoleOpts.endZoom = endZoom;

                if (endZoom.has_value())
                {
                    if (zoomInDirection.has_value())
                    {
                        if (*zoomInDirection && *endZoom < consoleOpts.start.zoom)
                        {
                            return fail(L"--end-zoom must be >= --start-zoom when --direction is 'in'.");
                        }
                        if (!*zoomInDirection && *endZoom > consoleOpts.start.zoom)
                        {
                            return fail(L"--end-zoom must be <= --start-zoom when --direction is 'out'.");
                        }
                        consoleOpts.zoomIn = *zoomInDirection;
                    }
                    else
                    {
                        consoleOpts.zoomIn = (*endZoom >= consoleOpts.start.zoom);
                    }
                }
                else
                {
                    consoleOpts.zoomIn = zoomInDirection.value_or(true);
                }

                consoleOpts.autoTarget = autoTarget.value_or(false);
                if (consoleOpts.autoTarget)
                {
                    if (consoleOpts.start.juliaSet)
                    {
                        return fail(L"--auto-target is not supported in Julia mode.");
                    }
                    if (!consoleOpts.zoomIn)
                    {
                        return fail(L"--auto-target requires zooming in.");
                    }
                    consoleOpts.targetX = consoleOpts.start.hpCenterX;
                    consoleOpts.targetY = consoleOpts.start.hpCenterY;
                    consoleOpts.targetSeed = seed.value_or(0);
                }
                else
                {
                    consoleOpts.targetX = targetX.value_or(consoleOpts.start.hpCenterX);
                    consoleOpts.targetY = targetY.value_or(consoleOpts.start.hpCenterY);
                }

                if (asciiAutoZoom.has_value())
                {
                    consoleOpts.autoZoom = *asciiAutoZoom;
                }
                else if (consoleOpts.once || consoleOpts.maxFrames == 1)
                {
                    consoleOpts.autoZoom = false;
                }
                else if (consoleOpts.autoTarget || consoleOpts.endZoom.has_value() ||
                         consoleOpts.maxDurationSeconds > 0 || consoleOpts.maxFrames > 1)
                {
                    consoleOpts.autoZoom = true;
                }
                else
                {
                    consoleOpts.autoZoom = false;
                }

                result.action = CliAction::ConsoleAscii;
                result.consoleOptions = std::move(consoleOpts);
                return result;
            }

            // Movie-specific encoding overrides.
            if (fps) plan.fps = *fps;
            if (duration) plan.seconds = *duration;
            if (codec) plan.codec = *codec;
            if (motionSmoothing) plan.motionSmoothing = *motionSmoothing;

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
            return fail(detail::Utf8ToWide(e.what()));
        }

        result.action = CliAction::ExportMovie;
        result.options.plan = std::move(plan);
        result.options.outputPath = std::move(outputPath);
        result.options.quiet = quiet;
        return result;
    }
}
