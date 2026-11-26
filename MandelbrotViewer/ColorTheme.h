#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>
#include "FractalCatalog.h"
#include "PixelSampling.h"

namespace Mandelbrot
{
    namespace detail
    {
        struct LinearSrgbTables
        {
            std::array<float, 256> decode{};
            std::array<uint8_t, 4097> encode{};

            LinearSrgbTables()
            {
                for (size_t i = 0; i < decode.size(); ++i)
                {
                    const float s = static_cast<float>(i) / 255.0f;
                    decode[i] = (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
                }
                for (size_t i = 0; i < encode.size(); ++i)
                {
                    const float linear = static_cast<float>(i) / static_cast<float>(encode.size() - 1);
                    const float s = (linear <= 0.0031308f) ? (12.92f * linear) : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
                    encode[i] = static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }

            static const LinearSrgbTables& Instance()
            {
                static const LinearSrgbTables tables;
                return tables;
            }
        };
    }

    struct DirectionalLightingParams
    {
        static constexpr float MinElevationDeg = 5.0f;
        static constexpr float MaxElevationDeg = 85.0f;
        static constexpr float MinStrength = 0.0f;
        static constexpr float MaxStrength = 2.5f;

        bool enabled{ false };
        float azimuthDeg{ 135.0f };  // 0=Right, 90=Top, 135=Top-Left, 180=Left, 270=Bottom
        float elevationDeg{ 45.0f }; // Light altitude above plane [5, 85] degrees
        float strength{ 1.0f };      // Relief / bump depth [0.0, 2.5]

        constexpr bool IsActive() const noexcept
        {
            return enabled && strength > 0.0f;
        }

        void ComputeLightDirection(float& lx, float& ly, float& lz) const noexcept
        {
            const float azRad = azimuthDeg * (std::numbers::pi_v<float> / 180.0f);
            const float elRad = std::clamp(elevationDeg, MinElevationDeg, MaxElevationDeg) * (std::numbers::pi_v<float> / 180.0f);
            const float cosEl = std::cos(elRad);
            lx = std::cos(azRad) * cosEl;
            ly = std::sin(azRad) * cosEl;
            lz = std::sin(elRad);
        }
    };

    struct GradientStop
    {
        float position{ 0.0f }; // 0.0 to 1.0
        uint8_t r{ 0 };
        uint8_t g{ 0 };
        uint8_t b{ 0 };
    };

    enum class PresetTheme
    {
        ElectricBlue = 0,
        Inferno,
        Psychedelic,
        Cyberpunk,
        Viridis,
        TwilightOcean,
        SunsetGlow,
        Monochrome,
        Custom
    };

    std::vector<GradientStop> GetPresetStops(PresetTheme theme);
    std::wstring GetPresetName(PresetTheme theme);
    const std::vector<PresetTheme>& GetAllPresets();

    class ColorTheme
    {
    public:
        static constexpr size_t LUT_SIZE = 4096;

        ColorTheme();
        explicit ColorTheme(PresetTheme preset);

        void SetPreset(PresetTheme preset);
        PresetTheme CurrentPreset() const { return m_currentPreset; }

        const std::vector<GradientStop>& Stops() const { return m_stops; }
        void SetStops(const std::vector<GradientStop>& stops);
        void AddStop(float position, uint8_t r, uint8_t g, uint8_t b);
        void RemoveStop(size_t index);
        void UpdateStop(size_t index, float position, uint8_t r, uint8_t g, uint8_t b);

        void RebuildLut();
        const std::vector<uint32_t>& Lut() const { return m_lut; }
        uint32_t AverageRgb() const noexcept
        {
            return (static_cast<uint32_t>(m_avgR) << 16) |
                   (static_cast<uint32_t>(m_avgG) << 8) |
                   static_cast<uint32_t>(m_avgB);
        }

        void Recolor(
            const std::vector<float>& iterationBuffer,
            uint32_t* pDestPixels,
            size_t count,
            float frequency,
            float phaseOffset,
            bool invert,
            uint32_t interiorColor = 0xFF000000,
            FractalType fractalType = FractalType::Mandelbrot,
            int width = 0,
            const std::vector<SubpixelSamples>* subpixelSamples = nullptr,
            bool filterPalette = true,
            const DirectionalLightingParams* lighting = nullptr) const;

        // Maps raw iteration count to logarithmic palette phase (before phaseOffset and modulo)
        static float MapIterationToPhase(float iter, float frequency) noexcept;

        // Generates preview pixel line of size width
        void GeneratePreview(uint32_t* pDestPixels, size_t width) const;

    private:
        uint32_t ColorIteration(float iter, float frequency, float phaseOffset, bool invert,
            uint32_t interiorColor, FractalType fractalType) const;
        uint32_t ColorAtPhase(float basePhase, float phaseOffset, bool invert, float deltaU) const;
        PresetTheme m_currentPreset{ PresetTheme::ElectricBlue };
        std::vector<GradientStop> m_stops;
        std::vector<uint32_t> m_lut; // BGRA format
        uint8_t m_avgR{ 0 };
        uint8_t m_avgG{ 0 };
        uint8_t m_avgB{ 0 };
    };
}
