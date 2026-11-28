#pragma once
#include "MandelbrotEngine.h"
#include "ColorTheme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace Mandelbrot
{
    enum class AppTheme
    {
        System = 0,
        Light = 1,
        Dark = 2
    };

    inline constexpr std::array<AppTheme, 3> GetAllAppThemes() noexcept
    {
        return { AppTheme::System, AppTheme::Light, AppTheme::Dark };
    }

    inline constexpr wchar_t const* GetAppThemeName(AppTheme theme) noexcept
    {
        switch (theme)
        {
        case AppTheme::Light: return L"Light";
        case AppTheme::Dark: return L"Dark";
        case AppTheme::System:
        default: return L"System Default";
        }
    }

    inline constexpr AppTheme CycleAppTheme(AppTheme current) noexcept
    {
        switch (current)
        {
        case AppTheme::System: return AppTheme::Light;
        case AppTheme::Light: return AppTheme::Dark;
        case AppTheme::Dark:
        default: return AppTheme::System;
        }
    }

    inline constexpr bool IsSystemBackgroundDark(uint8_t r, uint8_t g, uint8_t b) noexcept
    {
        // Perceptual luma check matching Windows App SDK dark-mode guidance (5*G + 2*R + B <= 8*128).
        return (5 * static_cast<int>(g) + 2 * static_cast<int>(r) + static_cast<int>(b)) <= 1024;
    }

    inline constexpr bool IsDarkAppTheme(AppTheme theme, bool systemIsDark) noexcept
    {
        switch (theme)
        {
        case AppTheme::Light: return false;
        case AppTheme::Dark: return true;
        case AppTheme::System:
        default: return systemIsDark;
        }
    }

    inline size_t CoordinateDecimalPlaces(double zoom)
    {
        // Keep several guard digits beyond the viewport's pixel scale, up to BigFloat precision.
        const double depth = std::ceil(std::log10(std::clamp(zoom, 1.0, MandelbrotEngine::MaxZoom)));
        return static_cast<size_t>(std::clamp(depth + 8.0, 16.0, 308.0));
    }

    inline std::wstring SerializeViewState(
        const MandelbrotConfig& config, const ColorTheme& theme,
        float frequency, float phaseOffset, bool invert, uint32_t interiorColor,
        const DirectionalLightingParams& lighting = {},
        bool histogramEqualize = false)
    {
        std::wostringstream ss;
        ss.imbue(std::locale::classic());
        ss << std::setprecision(std::numeric_limits<double>::max_digits10);
        const size_t decimals = CoordinateDecimalPlaces(config.zoom);
        const std::wstring fractalName = config.juliaSet
            ? (config.fractalType == FractalType::BurningShip ? L"Burning Ship Julia Set" : L"Julia Set")
            : GetFractalTypeName(config.fractalType);
        ss << L"{\n"
           << L"  \"fractalType\": \"" << fractalName << L"\",\n"
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
           << L"  \"smooth\": " << (config.smooth ? L"true" : L"false") << L",\n";
        if (config.coloringMode != ColoringMode::EscapeTime)
            ss << L"  \"coloringMode\": \"" << GetColoringModeName(config.coloringMode) << L"\",\n";
        ss << L"  \"frequency\": " << frequency << L",\n"
           << L"  \"phaseOffset\": " << phaseOffset << L",\n"
           << L"  \"invert\": " << (invert ? L"true" : L"false") << L",\n"
           << L"  \"interiorColor\": " << interiorColor << L",\n";
        if (histogramEqualize)
            ss << L"  \"histogramEqualize\": true,\n";
        if (lighting.enabled)
            ss << L"  \"lighting3d\": true,\n"
               << L"  \"lightAzimuth\": " << lighting.azimuthDeg << L",\n"
               << L"  \"lightElevation\": " << lighting.elevationDeg << L",\n"
               << L"  \"lightStrength\": " << lighting.strength << L",\n";
        ss << L"  \"theme\": \"" << GetPresetName(theme.CurrentPreset()) << L"\",\n"
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
