#include "pch.h"
#include "ColorTheme.h"
#include "MandelbrotEngine.h"
#include <cmath>
#include <algorithm>
#include <numbers>

namespace Mandelbrot
{
    const std::vector<PresetTheme>& GetAllPresets()
    {
        static const std::vector<PresetTheme> presets = {
            PresetTheme::ElectricBlue,
            PresetTheme::Inferno,
            PresetTheme::Psychedelic,
            PresetTheme::Cyberpunk,
            PresetTheme::Viridis,
            PresetTheme::TwilightOcean,
            PresetTheme::SunsetGlow,
            PresetTheme::Monochrome
        };
        return presets;
    }

    std::wstring GetPresetName(PresetTheme theme)
    {
        switch (theme)
        {
        case PresetTheme::ElectricBlue:  return L"Electric Blue";
        case PresetTheme::Inferno:       return L"Inferno Fire";
        case PresetTheme::Psychedelic:   return L"Psychedelic Rainbow";
        case PresetTheme::Cyberpunk:     return L"Cyberpunk Neon";
        case PresetTheme::Viridis:       return L"Viridis";
        case PresetTheme::TwilightOcean: return L"Twilight Ocean";
        case PresetTheme::SunsetGlow:    return L"Sunset Glow";
        case PresetTheme::Monochrome:    return L"Monochrome";
        case PresetTheme::Custom:        return L"Custom Palette";
        default:                         return L"Unknown";
        }
    }

    std::vector<GradientStop> GetPresetStops(PresetTheme theme)
    {
        switch (theme)
        {
        case PresetTheme::ElectricBlue:
            return {
                { 0.00f,   0,   7, 100 },
                { 0.16f,  32, 107, 203 },
                { 0.42f, 237, 255, 255 },
                { 0.64f, 255, 170,   0 },
                { 0.85f,   0,   2,   0 },
                { 1.00f,   0,   7, 100 }
            };
        case PresetTheme::Inferno:
            return {
                { 0.00f,   0,   0,   4 },
                { 0.20f,  87,  16, 110 },
                { 0.40f, 187,  55,  84 },
                { 0.65f, 249, 142,   9 },
                { 0.85f, 254, 211,  60 },
                { 1.00f, 252, 255, 164 }
            };
        case PresetTheme::Psychedelic:
            return {
                { 0.00f, 255,   0,   0 },
                { 0.17f, 255, 255,   0 },
                { 0.33f,   0, 255,   0 },
                { 0.50f,   0, 255, 255 },
                { 0.67f,   0,   0, 255 },
                { 0.83f, 255,   0, 255 },
                { 1.00f, 255,   0,   0 }
            };
        case PresetTheme::Cyberpunk:
            return {
                { 0.00f,  13,   2,  33 },
                { 0.25f, 114,   9, 183 },
                { 0.50f, 247,  37, 133 },
                { 0.75f,  76, 201, 240 },
                { 0.90f, 254, 228,  64 },
                { 1.00f,  13,   2,  33 }
            };
        case PresetTheme::Viridis:
            return {
                { 0.00f,  68,   1,  84 },
                { 0.25f,  59,  82, 139 },
                { 0.50f,  33, 145, 140 },
                { 0.75f,  94, 201,  98 },
                { 1.00f, 253, 231,  37 }
            };
        case PresetTheme::TwilightOcean:
            return {
                { 0.00f,   2,  12,  27 },
                { 0.25f,  10,  58,  86 },
                { 0.50f,  24, 154, 180 },
                { 0.75f, 117, 230, 218 },
                { 1.00f, 212, 241, 244 }
            };
        case PresetTheme::SunsetGlow:
            return {
                { 0.00f,  36,   0,  70 },
                { 0.30f, 123,  44, 191 },
                { 0.60f, 255, 112,  67 },
                { 0.85f, 255, 202,  40 },
                { 1.00f, 255, 248, 225 }
            };
        case PresetTheme::Monochrome:
            return {
                { 0.00f,   0,   0,   0 },
                { 0.50f, 240, 240, 240 },
                { 1.00f,   0,   0,   0 }
            };
        case PresetTheme::Custom:
        default:
            return {
                { 0.00f,   0,   7, 100 },
                { 0.50f, 237, 255, 255 },
                { 1.00f,   0,   7, 100 }
            };
        }
    }

    ColorTheme::ColorTheme()
        : ColorTheme(PresetTheme::ElectricBlue)
    {
    }

    ColorTheme::ColorTheme(PresetTheme preset)
    {
        SetPreset(preset);
    }

    void ColorTheme::SetPreset(PresetTheme preset)
    {
        m_currentPreset = preset;
        m_stops = GetPresetStops(preset);
        RebuildLut();
    }

    void ColorTheme::SetStops(const std::vector<GradientStop>& stops)
    {
        m_stops = stops;
        m_currentPreset = PresetTheme::Custom;
        RebuildLut();
    }

    void ColorTheme::AddStop(float position, uint8_t r, uint8_t g, uint8_t b)
    {
        m_stops.push_back({ std::clamp(position, 0.0f, 1.0f), r, g, b });
        m_currentPreset = PresetTheme::Custom;
        RebuildLut();
    }

    void ColorTheme::RemoveStop(size_t index)
    {
        if (m_stops.size() > 2 && index < m_stops.size())
        {
            m_stops.erase(m_stops.begin() + index);
            m_currentPreset = PresetTheme::Custom;
            RebuildLut();
        }
    }

    void ColorTheme::UpdateStop(size_t index, float position, uint8_t r, uint8_t g, uint8_t b)
    {
        if (index < m_stops.size())
        {
            m_stops[index] = { std::clamp(position, 0.0f, 1.0f), r, g, b };
            m_currentPreset = PresetTheme::Custom;
            RebuildLut();
        }
    }

    void ColorTheme::RebuildLut()
    {
        m_lut.resize(LUT_SIZE);

        if (m_stops.empty())
        {
            std::fill(m_lut.begin(), m_lut.end(), 0xFF000000);
            return;
        }

        // Sort a temporary copy of stops by position
        auto sortedStops = m_stops;
        std::sort(sortedStops.begin(), sortedStops.end(), [](const GradientStop& a, const GradientStop& b) {
            return a.position < b.position;
        });

        // Ensure 0.0 and 1.0 bounds
        if (sortedStops.front().position > 0.0f)
        {
            auto first = sortedStops.front();
            first.position = 0.0f;
            sortedStops.insert(sortedStops.begin(), first);
        }
        if (sortedStops.back().position < 1.0f)
        {
            auto last = sortedStops.back();
            last.position = 1.0f;
            sortedStops.push_back(last);
        }

        const size_t numStops = sortedStops.size();

        for (size_t i = 0; i < LUT_SIZE; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(LUT_SIZE - 1);

            // Find segment [s, s+1] containing t
            size_t seg = 0;
            while (seg + 1 < numStops && sortedStops[seg + 1].position < t)
            {
                seg++;
            }

            if (seg + 1 >= numStops)
            {
                const auto& s = sortedStops.back();
                m_lut[i] = 0xFF000000 | (static_cast<uint32_t>(s.r) << 16) | (static_cast<uint32_t>(s.g) << 8) | static_cast<uint32_t>(s.b);
                continue;
            }

            const auto& s0 = sortedStops[seg];
            const auto& s1 = sortedStops[seg + 1];
            const float span = s1.position - s0.position;
            const float factor = (span > 0.00001f) ? ((t - s0.position) / span) : 0.0f;

            // Smooth cosine interpolation to prevent harsh linear color bends
            const float mu = (1.0f - std::cos(factor * 3.14159265358979323846f)) * 0.5f;

            const uint8_t r = static_cast<uint8_t>(std::clamp(s0.r * (1.0f - mu) + s1.r * mu, 0.0f, 255.0f));
            const uint8_t g = static_cast<uint8_t>(std::clamp(s0.g * (1.0f - mu) + s1.g * mu, 0.0f, 255.0f));
            const uint8_t b = static_cast<uint8_t>(std::clamp(s0.b * (1.0f - mu) + s1.b * mu, 0.0f, 255.0f));

            // BGRA layout in little-endian uint32_t
            m_lut[i] = 0xFF000000 | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
    }

    void ColorTheme::GeneratePreview(uint32_t* pDestPixels, size_t width) const
    {
        if (!pDestPixels || width == 0 || m_lut.empty()) return;

        for (size_t x = 0; x < width; ++x)
        {
            const float t = static_cast<float>(x) / static_cast<float>(width - 1);
            const size_t lutIdx = static_cast<size_t>(t * static_cast<float>(LUT_SIZE - 1));
            pDestPixels[x] = m_lut[std::min(lutIdx, LUT_SIZE - 1)];
        }
    }

    void ColorTheme::Recolor(
        const std::vector<float>& iterationBuffer,
        uint32_t* pDestPixels,
        size_t count,
        float frequency,
        float phaseOffset,
        bool invert,
        uint32_t interiorColor,
        FractalType fractalType) const
    {
        if (!pDestPixels || iterationBuffer.size() < count || m_lut.empty()) return;

        const float lutMax = static_cast<float>(LUT_SIZE - 1);
        const uint32_t* lutData = m_lut.data();
        const float* iterData = iterationBuffer.data();

        if (fractalType == FractalType::Newton)
        {
            #pragma omp parallel for schedule(static, 4096)
            for (intptr_t idx = 0; idx < static_cast<intptr_t>(count); ++idx)
            {
                const float val = iterData[idx];
                if (val < 0.0f)
                {
                    pDestPixels[idx] = interiorColor;
                    continue;
                }

                const int root = static_cast<int>(val / 1000000.0f);
                const float iter = val - (root * 1000000.0f);

                // Map root index (0, 1, 2) to base partition of the palette [0, 1)
                const float rootBase = static_cast<float>(root % 3) / 3.0f;

                // Modulate with iteration count for fine concentric bands / ripple contours
                float ring = std::fmod((iter * 0.06f * frequency + phaseOffset), 1.0f);
                if (ring < 0.0f) ring += 1.0f;

                float u = rootBase + ring * (1.0f / 3.0f);
                if (u >= 1.0f) u -= 1.0f;
                if (invert) u = 1.0f - u;

                const size_t lutIdx = static_cast<size_t>(u * lutMax);
                uint32_t color = lutData[lutIdx & (LUT_SIZE - 1)];

                // Subtle smooth shading: points taking more iterations darken slightly
                const float shading = std::max(0.35f, 1.0f / (1.0f + 0.015f * iter));
                if (shading < 0.99f)
                {
                    const uint32_t a = (color >> 24) & 0xFF;
                    const uint32_t r = static_cast<uint32_t>(((color >> 16) & 0xFF) * shading);
                    const uint32_t g = static_cast<uint32_t>(((color >> 8) & 0xFF) * shading);
                    const uint32_t b = static_cast<uint32_t>((color & 0xFF) * shading);
                    color = (a << 24) | (r << 16) | (g << 8) | b;
                }

                pDestPixels[idx] = color;
            }
        }
        else
        {
            #pragma omp parallel for schedule(static, 4096)
            for (intptr_t idx = 0; idx < static_cast<intptr_t>(count); ++idx)
            {
                const float iter = iterData[idx];

                if (iter < 0.0f)
                {
                    // Point is inside Mandelbrot set
                    pDestPixels[idx] = interiorColor;
                    continue;
                }

                // Map iteration count into continuous [0, 1) cyclic space
                float u = std::fmod((iter * frequency + phaseOffset), 1.0f);
                if (u < 0.0f) u += 1.0f;
                if (invert) u = 1.0f - u;

                const size_t lutIdx = static_cast<size_t>(u * lutMax);
                pDestPixels[idx] = lutData[lutIdx & (LUT_SIZE - 1)];
            }
        }
    }
}
