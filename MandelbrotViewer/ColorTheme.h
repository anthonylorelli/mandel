#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace Mandelbrot
{
    enum class FractalType;

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

        void Recolor(
            const std::vector<float>& iterationBuffer,
            uint32_t* pDestPixels,
            size_t count,
            float frequency,
            float phaseOffset,
            bool invert,
            uint32_t interiorColor = 0xFF000000,
            FractalType fractalType = static_cast<FractalType>(0),
            int width = 0) const;

        // Maps raw iteration count to logarithmic palette phase (before phaseOffset and modulo)
        static float MapIterationToPhase(float iter, float frequency) noexcept;

        // Generates preview pixel line of size width
        void GeneratePreview(uint32_t* pDestPixels, size_t width) const;

    private:
        PresetTheme m_currentPreset{ PresetTheme::ElectricBlue };
        std::vector<GradientStop> m_stops;
        std::vector<uint32_t> m_lut; // BGRA format
        uint8_t m_avgR{ 0 };
        uint8_t m_avgG{ 0 };
        uint8_t m_avgB{ 0 };
    };
}
