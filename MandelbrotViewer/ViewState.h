#pragma once
#include "MandelbrotEngine.h"
#include "ColorTheme.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace Mandelbrot
{
    inline size_t CoordinateDecimalPlaces(double zoom)
    {
        // Keep several guard digits beyond the viewport's pixel scale, up to BigFloat precision.
        const double depth = std::ceil(std::log10(std::clamp(zoom, 1.0, MandelbrotEngine::MaxZoom)));
        return static_cast<size_t>(std::clamp(depth + 8.0, 16.0, 308.0));
    }

    inline std::wstring SerializeViewState(
        const MandelbrotConfig& config, const ColorTheme& theme,
        float frequency, float phaseOffset, bool invert, uint32_t interiorColor)
    {
        std::wostringstream ss;
        ss.imbue(std::locale::classic());
        ss << std::setprecision(std::numeric_limits<double>::max_digits10);
        const size_t decimals = CoordinateDecimalPlaces(config.zoom);
        ss << L"{\n"
           << L"  \"fractalType\": \"" << (config.juliaSet ? L"Julia Set" : GetFractalTypeName(config.fractalType)) << L"\",\n"
           << L"  \"centerX\": \"" << config.hpCenterX.ToWString(decimals) << L"\",\n"
           << L"  \"centerY\": \"" << config.hpCenterY.ToWString(decimals) << L"\",\n"
           << L"  \"zoom\": " << config.zoom << L",\n";
        if (config.juliaSet)
            ss << L"  \"juliaParameterX\": \"" << config.juliaX.ToWString(308) << L"\",\n"
               << L"  \"juliaParameterY\": \"" << config.juliaY.ToWString(308) << L"\",\n";
        ss
           << L"  \"width\": " << config.width << L",\n"
           << L"  \"height\": " << config.height << L",\n"
           << L"  \"maxIterations\": " << config.maxIterations << L",\n"
           << L"  \"smooth\": " << (config.smooth ? L"true" : L"false") << L",\n"
           << L"  \"frequency\": " << frequency << L",\n"
           << L"  \"phaseOffset\": " << phaseOffset << L",\n"
           << L"  \"invert\": " << (invert ? L"true" : L"false") << L",\n"
           << L"  \"interiorColor\": " << interiorColor << L",\n"
           << L"  \"theme\": \"" << GetPresetName(theme.CurrentPreset()) << L"\",\n"
           << L"  \"gradientStops\": [";
        bool first = true;
        for (const auto& stop : theme.Stops())
        {
            if (!first) ss << L",";
            first = false;
            ss << L"\n    {\"position\": " << stop.position
               << L", \"r\": " << static_cast<unsigned>(stop.r)
               << L", \"g\": " << static_cast<unsigned>(stop.g)
               << L", \"b\": " << static_cast<unsigned>(stop.b) << L"}";
        }
        ss << L"\n  ]\n}";
        return ss.str();
    }
}
