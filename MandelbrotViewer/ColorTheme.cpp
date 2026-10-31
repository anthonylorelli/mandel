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
            m_avgR = 0;
            m_avgG = 0;
            m_avgB = 0;
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
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;

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
                sumR += s.r;
                sumG += s.g;
                sumB += s.b;
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

            sumR += r;
            sumG += g;
            sumB += b;

            // BGRA layout in little-endian uint32_t
            m_lut[i] = 0xFF000000 | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }

        m_avgR = static_cast<uint8_t>(sumR / LUT_SIZE);
        m_avgG = static_cast<uint8_t>(sumG / LUT_SIZE);
        m_avgB = static_cast<uint8_t>(sumB / LUT_SIZE);
    }

    void ColorTheme::GeneratePreview(uint32_t* pDestPixels, size_t width) const
    {
        if (!pDestPixels || width == 0 || m_lut.empty()) return;

        for (size_t x = 0; x < width; ++x)
        {
            const float t = (width > 1) ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0f;
            const size_t lutIdx = static_cast<size_t>(t * static_cast<float>(LUT_SIZE - 1));
            pDestPixels[x] = m_lut[std::min(lutIdx, LUT_SIZE - 1)];
        }
    }

    float ColorTheme::MapIterationToPhase(float iter, float frequency) noexcept
    {
        if (iter <= 0.0f) return 0.0f;
        // Logarithmic iteration compression: equal multiplicative steps in iteration count
        // produce equal palette phase intervals across all iteration depths (10 to 50,000+),
        // preventing high-iteration areas from bunching up into dense visual noise.
        constexpr float kLogScale = 1.6f;
        constexpr float kInvKnee = 0.1f; // Soft knee at iter = 10
        return kLogScale * std::log(1.0f + iter * kInvKnee) * frequency;
    }

    uint32_t ColorTheme::ColorIteration(float value, float frequency, float phaseOffset, bool invert,
        uint32_t interiorColor, FractalType fractalType, float deltaU) const
    {
        if (value < 0.0f) return interiorColor;
        const float lutMax = static_cast<float>(LUT_SIZE - 1);
        if (fractalType == FractalType::Newton)
        {
            const int root = static_cast<int>(value / 1000000.0f);
            const float iter = value - root * 1000000.0f;
            const float rootBase = static_cast<float>(root % 3) / 3.0f;
            const float compressedIter = 12.0f * std::log(1.0f + iter * (1.0f / 12.0f));
            float ring = std::fmod(compressedIter * 0.06f * frequency + phaseOffset, 1.0f);
            if (ring < 0.0f) ring += 1.0f;
            float u = rootBase + ring * (1.0f / 3.0f);
            if (u >= 1.0f) u -= 1.0f;
            if (invert) u = 1.0f - u;
            const size_t index = static_cast<size_t>(u * lutMax);
            uint32_t color = m_lut[index & (LUT_SIZE - 1)];
            const float shading = std::max(0.35f, 1.0f / (1.0f + 0.025f * compressedIter));
            if (shading < 0.99f)
            {
                const uint32_t a = (color >> 24) & 0xFF;
                const uint32_t r = static_cast<uint32_t>(((color >> 16) & 0xFF) * shading);
                const uint32_t g = static_cast<uint32_t>(((color >> 8) & 0xFF) * shading);
                const uint32_t b = static_cast<uint32_t>((color & 0xFF) * shading);
                color = (a << 24) | (r << 16) | (g << 8) | b;
            }
            return color;
        }

        return ColorAtPhase(MapIterationToPhase(value, frequency), phaseOffset, invert, deltaU);
    }

    uint32_t ColorTheme::ColorAtPhase(float basePhase, float phaseOffset, bool invert, float deltaU) const
    {
        const float lutMax = static_cast<float>(LUT_SIZE - 1);
        const float uCenter = basePhase + phaseOffset;
        auto sampleLut = [&](float phase) noexcept -> uint32_t
        {
            float u = std::fmod(phase, 1.0f);
            if (u < 0.0f) u += 1.0f;
            if (invert) u = 1.0f - u;
            return m_lut[static_cast<size_t>(u * lutMax) & (LUT_SIZE - 1)];
        };
        if (deltaU <= 0.04f) return sampleLut(uCenter);
        const float span = std::min(deltaU, 1.0f);
        const uint32_t colors[] = { sampleLut(uCenter - 0.375f * span), sampleLut(uCenter - 0.125f * span),
            sampleLut(uCenter + 0.125f * span), sampleLut(uCenter + 0.375f * span) };
        float r = 0.0f, g = 0.0f, b = 0.0f;
        for (auto color : colors)
        {
            r += static_cast<float>((color >> 16) & 0xFF);
            g += static_cast<float>((color >> 8) & 0xFF);
            b += static_cast<float>(color & 0xFF);
        }
        r *= 0.25f; g *= 0.25f; b *= 0.25f;
        if (deltaU > 0.20f)
        {
            const float damp = std::clamp((deltaU - 0.20f) / 1.0f, 0.0f, 0.72f);
            r = r * (1.0f - damp) + m_avgR * damp;
            g = g * (1.0f - damp) + m_avgG * damp;
            b = b * (1.0f - damp) + m_avgB * damp;
        }
        return 0xFF000000u | (static_cast<uint32_t>(std::clamp(r + 0.5f, 0.0f, 255.0f)) << 16) |
            (static_cast<uint32_t>(std::clamp(g + 0.5f, 0.0f, 255.0f)) << 8) |
            static_cast<uint32_t>(std::clamp(b + 0.5f, 0.0f, 255.0f));
    }

    namespace
    {
        struct LinearColorTables
        {
            std::array<float, 256> decode{};
            std::array<uint8_t, 4097> encode{};
            LinearColorTables()
            {
                for (size_t i = 0; i < decode.size(); ++i)
                {
                    const float s = static_cast<float>(i) / 255.0f;
                    decode[i] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
                }
                for (size_t i = 0; i < encode.size(); ++i)
                {
                    const float linear = static_cast<float>(i) / (encode.size() - 1);
                    const float s = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                    encode[i] = static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }
        };
    }

    void ColorTheme::Recolor(
        const std::vector<float>& iterationBuffer,
        uint32_t* pDestPixels,
        size_t count,
        float frequency,
        float phaseOffset,
        bool invert,
        uint32_t interiorColor,
        FractalType fractalType,
        int width,
        const std::vector<SubpixelSamples>* subpixelSamples) const
    {
        if (!pDestPixels || iterationBuffer.size() < count || m_lut.empty()) return;

        const float* iterData = iterationBuffer.data();
        const intptr_t totalCount = static_cast<intptr_t>(count);
        const intptr_t rowStride = (width > 0)
            ? static_cast<intptr_t>(width)
            : ((totalCount >= 64) ? static_cast<intptr_t>(std::sqrt(static_cast<double>(totalCount))) : 0);

        if (fractalType == FractalType::Newton)
        {
            #pragma omp parallel for schedule(static, 4096)
            for (intptr_t idx = 0; idx < totalCount; ++idx)
            {
                pDestPixels[idx] = ColorIteration(iterData[idx], frequency, phaseOffset, invert,
                    interiorColor, fractalType);
            }
        }
        else
        {
            #pragma omp parallel for schedule(static, 4096)
            for (intptr_t idx = 0; idx < totalCount; ++idx)
            {
                const float iter = iterData[idx];

                if (iter < 0.0f)
                {
                    pDestPixels[idx] = interiorColor;
                    continue;
                }

                const float basePhase = MapIterationToPhase(iter, frequency);

                // Estimate 2D screen-space phase gradient max(|dU/dx|, |dU/dy|) for band-limited anti-aliasing
                float deltaU = 0.0f;
                if (totalCount >= 64)
                {
                    // Horizontal neighbors must stay in the same scanline, including partial final rows.
                    const bool hasLeft = idx > 0 && (rowStride <= 0 || idx % rowStride != 0);
                    const bool hasRight = idx + 1 < totalCount &&
                        (rowStride <= 0 || (idx + 1) % rowStride != 0);
                    if (hasRight && iterData[idx + 1] >= 0.0f)
                    {
                        deltaU = std::max(deltaU, std::abs(MapIterationToPhase(iterData[idx + 1], frequency) - basePhase));
                    }
                    else if (hasLeft && iterData[idx - 1] >= 0.0f)
                    {
                        deltaU = std::max(deltaU, std::abs(basePhase - MapIterationToPhase(iterData[idx - 1], frequency)));
                    }

                    if (rowStride > 0)
                    {
                        const intptr_t ny = (idx + rowStride < totalCount) ? (idx + rowStride) : (idx - rowStride);
                        if (ny >= 0 && ny < totalCount)
                        {
                            const float iterY = iterData[ny];
                            if (iterY >= 0.0f)
                            {
                                deltaU = std::max(deltaU, std::abs(MapIterationToPhase(iterY, frequency) - basePhase));
                            }
                        }
                    }
                }

                pDestPixels[idx] = ColorAtPhase(basePhase, phaseOffset, invert, deltaU);
            }
        }

        if (!subpixelSamples || subpixelSamples->empty()) return;
        static const LinearColorTables colorSpace;
        const intptr_t sampleCount = static_cast<intptr_t>(subpixelSamples->size());
        #pragma omp parallel for schedule(static, 1024)
        for (intptr_t i = 0; i < sampleCount; ++i)
        {
            const auto& sample = (*subpixelSamples)[i];
            if (sample.pixelIndex >= count || !(sample.weight > 0.0f)) continue;
            const float weight = std::min(sample.weight, 1.0f);
            uint32_t colors[4];
            float phases[4]{};
            if (fractalType != FractalType::Newton)
                for (int s = 0; s < 4; ++s)
                    if (sample.iterations[s] >= 0.0f)
                        phases[s] = MapIterationToPhase(sample.iterations[s], frequency);
            for (int s = 0; s < 4; ++s)
            {
                // Each quarter-pixel sample has a half-pixel footprint. Estimate its
                // palette filter from adjacent subpixels, excluding interior/basin boundaries.
                float deltaU = 0.0f;
                if (fractalType != FractalType::Newton && sample.iterations[s] >= 0.0f)
                    for (int neighbor : { s ^ 1, s ^ 2 })
                        if (sample.iterations[neighbor] >= 0.0f)
                            deltaU = std::max(deltaU, std::abs(phases[s] - phases[neighbor]));
                colors[s] = fractalType != FractalType::Newton && sample.iterations[s] >= 0.0f
                    ? ColorAtPhase(phases[s], phaseOffset, invert, deltaU)
                    : ColorIteration(sample.iterations[s], frequency, phaseOffset, invert, interiorColor, fractalType);
            }
            const uint32_t center = pDestPixels[sample.pixelIndex];
            uint32_t output = 0;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int shift = channel * 8;
                float average = 0.0f;
                for (auto color : colors) average += colorSpace.decode[(color >> shift) & 0xFF] * 0.25f;
                const float linear = average * weight + colorSpace.decode[(center >> shift) & 0xFF] * (1.0f - weight);
                const size_t index = static_cast<size_t>(std::clamp(linear * 4096.0f + 0.5f, 0.0f, 4096.0f));
                output |= static_cast<uint32_t>(colorSpace.encode[index]) << shift;
            }
            float alpha = 0.0f;
            for (auto color : colors) alpha += static_cast<float>(color >> 24) * 0.25f;
            alpha = alpha * weight + static_cast<float>(center >> 24) * (1.0f - weight);
            pDestPixels[sample.pixelIndex] = output | (static_cast<uint32_t>(alpha + 0.5f) << 24);
        }
    }
}
