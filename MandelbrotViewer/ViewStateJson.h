#pragma once
#include "ColorTheme.h"
#include "FractalCatalog.h"
#include "MoviePlan.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Mandelbrot::detail
{
    std::wstring Utf8ToWide(std::string_view utf8);
    bool TryWideToAscii(std::wstring_view wide, std::string& ascii);
    std::string WideToAsciiLossy(std::wstring_view wide);
    std::string NormalizeToken(std::string_view raw);
    std::string NormalizeToken(std::wstring_view raw);
    std::string TrimAscii(std::string_view s);

    bool TryParseColor(std::string_view raw, uint32_t& out);
    bool TryParseFractalType(std::string_view text, FractalType& outType, std::optional<bool>& outJulia);
    bool TryParsePresetTheme(std::string_view text, PresetTheme& outTheme);
    bool TryParseColoringMode(std::string_view text, ColoringMode& outMode);
}
