#pragma once
#include "ColorTheme.h"
#include "FractalConfig.h"
#include "MandelbrotEngine.h"
#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Mandelbrot
{
    inline constexpr int DefaultConsoleCols = 100;
    inline constexpr int DefaultConsoleRows = 32;
    inline constexpr int MinConsoleCols = 10;
    inline constexpr int MaxConsoleCols = 512;
    inline constexpr int MinConsoleRows = 5;
    inline constexpr int MaxConsoleRows = 256;
    inline constexpr double DefaultConsoleCharAspect = 2.0;
    inline constexpr double MinConsoleCharAspect = 0.5;
    inline constexpr double MaxConsoleCharAspect = 4.0;
    inline constexpr int DefaultConsoleFps = 15;
    inline constexpr int MinConsoleFps = 1;
    inline constexpr int MaxConsoleFps = 60;
    inline constexpr double MinConsoleZoomSpeed = 0.2;
    inline constexpr double MaxConsoleZoomSpeed = 5.0;

    enum class AsciiColorMode
    {
        Auto = 0,
        None,
        Ansi256,
        TrueColor
    };

    enum class AsciiCharsetPreset
    {
        Standard = 0, // " .:-=+*#%@"
        Dense,        // " .,:;i1tfLCG08@"
        Detailed,     // 70-level ASCII ramp
        Simple,       // " .oO@#"
        Custom
    };

    enum class ConsoleKeyAction
    {
        None = 0,
        Quit,
        ToggleAutoZoom,
        ToggleAutoTarget,
        ToggleDirection,
        ZoomInStep,
        ZoomOutStep,
        SpeedUp,
        SpeedDown,
        PanLeft,
        PanRight,
        PanUp,
        PanDown,
        NextFractal,
        NextTheme,
        CycleColorMode,
        CycleCharset,
        ToggleHud,
        ResetView
    };

    struct ConsoleAsciiOptions
    {
        FractalConfig start;
        ColorTheme theme{ PresetTheme::ElectricBlue };
        float frequency{ 1.0f };
        float phase{ 0.0f };
        bool invert{ false };
        uint32_t interior{ 0xFF000000u };
        DirectionalLightingParams lighting{};

        // Terminal grid dimensions (0 = auto-detect from console window, fallback to defaults)
        int cols{ 0 };
        int rows{ 0 };
        double charAspect{ DefaultConsoleCharAspect };

        // ASCII rendering style
        AsciiCharsetPreset charsetPreset{ AsciiCharsetPreset::Standard };
        std::string customCharset;
        AsciiColorMode colorMode{ AsciiColorMode::Auto };
        bool showHud{ true };

        // Auto-zoom & target configuration (matching GUI)
        bool autoZoom{ false };
        bool zoomIn{ true };
        double zoomSpeed{ 1.0 }; // octaves/sec
        std::optional<double> endZoom;
        BigFloat targetX{ -0.75 };
        BigFloat targetY{ 0.0 };
        bool autoTarget{ false };
        uint64_t targetSeed{ 0 };

        // Iteration & compute settings
        bool autoScaleIterations{ true };
        int baseIterations{ 500 };
        bool useGpu{ false };

        // Live playback settings
        int fps{ DefaultConsoleFps };
        int maxDurationSeconds{ 0 }; // 0 = unlimited until quit or endZoom/maxFrames reached
        int maxFrames{ 0 };          // 0 = unlimited
        bool once{ false };
        bool quiet{ false };
    };

    struct ConsoleFrameStats
    {
        int cols{ 0 };
        int rows{ 0 };
        int pixelWidth{ 0 };
        int pixelHeight{ 0 };
        double computeMs{ 0.0 };
        double colorMs{ 0.0 };
        double actualFps{ 0.0 };
        double elapsedSeconds{ 0.0 };
        uint64_t frameIndex{ 0 };
        bool isZooming{ false };
        bool zoomIn{ true };
        bool autoTarget{ false };
        double zoomSpeed{ 1.0 };
        AsciiColorMode activeColorMode{ AsciiColorMode::None };
    };

    // Returns the built-in printable ASCII ramp (ordered dark-to-bright) for a preset.
    std::string GetAsciiCharset(AsciiCharsetPreset preset);
    const char* GetAsciiCharsetPresetName(AsciiCharsetPreset preset) noexcept;
    const char* GetAsciiColorModeName(AsciiColorMode mode) noexcept;

    // Validates that a custom charset has at least 2 printable 7-bit ASCII characters (0x20..0x7E).
    bool IsValidAsciiCharset(std::string_view chars) noexcept;

    // Maps an 8-bit RGB color to the closest 8-bit ANSI 256-color palette index.
    uint8_t RgbToAnsi256(uint8_t r, uint8_t g, uint8_t b) noexcept;

    struct AsciiCellHistory
    {
        float rampPos{ 0.0f };
        float linR{ 0.0f };
        float linG{ 0.0f };
        float linB{ 0.0f };
        uint8_t rampIdx{ 0 };
    };

    struct AsciiTemporalState
    {
        bool valid{ false };
        int cols{ 0 };
        int rows{ 0 };
        std::string charset;
        FractalConfig config{};
        std::vector<AsciiCellHistory> cells;

        void Reset() noexcept
        {
            valid = false;
            cells.clear();
        }
    };

    // Computes the vertical fractal pixel resolution for `rows` terminal rows and character aspect ratio.
    int ComputeAsciiPixelHeight(int rows, double charAspect) noexcept;

    // Computes supersampled fractal buffer dimensions for an ASCII canvas of `cols` x `rows`.
    void ComputeAsciiSupersampleDimensions(
        int cols,
        int rows,
        double charAspect,
        int& outPixelWidth,
        int& outPixelHeight) noexcept;

    // Converts a rendered iteration buffer and BGRA color buffer into an ASCII frame of `cols` x `rows`.
    // Each row ends with '\n'. Emits ANSI foreground escape sequences according to `resolvedColorMode`.
    std::string RenderAsciiFrame(
        const std::vector<float>& iterations,
        const std::vector<uint32_t>& bgraPixels,
        int pixelWidth,
        int pixelHeight,
        int cols,
        int rows,
        std::string_view charset,
        AsciiColorMode resolvedColorMode);

    // Renders an ASCII frame with camera-reprojected temporal filtering and character-index hysteresis
    // to suppress glyph jitter during zoom and pan animations while preserving sharp detail.
    std::string RenderAsciiFrameStabilized(
        const std::vector<float>& iterations,
        const std::vector<uint32_t>& bgraPixels,
        const FractalConfig& currentConfig,
        int cols,
        int rows,
        std::string_view charset,
        AsciiColorMode resolvedColorMode,
        AsciiTemporalState* temporalState);

    // Formats the live telemetry status line (and optional interactive controls hint line) clipped to `cols`.
    std::string FormatConsoleHudPrimaryLine(
        const FractalConfig& config,
        const ColorTheme& theme,
        const ConsoleFrameStats& stats,
        int cols);

    std::string FormatConsoleHudControlsLine(int cols);

    // Stateful controller for console ASCII rendering, live auto-zoom, auto-target exploration,
    // and interactive keyboard navigation. Decoupled from OS terminal I/O for full unit testing.
    class ConsoleAsciiSession
    {
    public:
        explicit ConsoleAsciiSession(const ConsoleAsciiOptions& options, int initialCols, int initialRows);

        void Resize(int cols, int rows);

        // Computes and renders the current view to an ASCII frame string.
        std::string RenderFrame(
            double elapsedSeconds,
            double actualFps,
            AsciiColorMode resolvedColorMode,
            const std::atomic<bool>* cancelToken = nullptr);

        // Advances auto-zoom by `dtSeconds`. Returns true if auto-zoom remains active after the step.
        bool StepAutoZoom(double dtSeconds);

        // Applies an interactive keyboard command. Returns false if the session should quit.
        bool ApplyKeyAction(ConsoleKeyAction action);

        const FractalConfig& Config() const noexcept { return m_config; }
        const FractalConfig& DisplayedConfig() const noexcept { return m_displayedConfig; }
        const ColorTheme& Theme() const noexcept { return m_theme; }
        const ConsoleFrameStats& LastStats() const noexcept { return m_lastStats; }
        const BigFloat& TargetX() const noexcept { return m_targetX; }
        const BigFloat& TargetY() const noexcept { return m_targetY; }

        int Cols() const noexcept { return m_cols; }
        int Rows() const noexcept { return m_rows; }
        bool IsZooming() const noexcept { return m_isZooming; }
        bool IsZoomIn() const noexcept { return m_zoomIn; }
        bool IsAutoTarget() const noexcept { return m_autoTarget; }
        bool ShowHud() const noexcept { return m_showHud; }
        bool ReachedZoomBound() const noexcept { return m_reachedZoomBound; }
        double ZoomSpeed() const noexcept { return m_zoomSpeed; }
        AsciiColorMode PreferredColorMode() const noexcept { return m_colorMode; }
        std::string_view ActiveCharset() const noexcept { return m_charset; }

    private:
        void PickAndApplyAutoTarget();
        uint64_t NextRetargetSeed() noexcept;
        void SyncRecommendedIterations();
        void RefreshCharset();

        ConsoleAsciiOptions m_options;
        FractalConfig m_config;
        FractalConfig m_displayedConfig;
        ColorTheme m_theme;
        MandelbrotEngine m_engine;
        std::vector<float> m_iterations;
        std::vector<SubpixelSamples> m_subpixelSamples;
        std::vector<uint32_t> m_pixels;
        AsciiTemporalState m_temporalState;

        int m_cols{ DefaultConsoleCols };
        int m_rows{ DefaultConsoleRows };
        bool m_isZooming{ false };
        bool m_zoomIn{ true };
        bool m_autoTarget{ false };
        bool m_showHud{ true };
        bool m_reachedZoomBound{ false };
        double m_zoomSpeed{ 1.0 };

        BigFloat m_targetX;
        BigFloat m_targetY;
        bool m_hasActiveRandomTarget{ false };
        double m_retargetPanProgress{ 1.0 };
        int m_framesSinceRetarget{ 0 };
        uint64_t m_retargetCount{ 0 };
        uint64_t m_frameIndex{ 0 };
        bool m_hasRenderedFrame{ false };

        AsciiColorMode m_colorMode{ AsciiColorMode::Auto };
        AsciiCharsetPreset m_charsetPreset{ AsciiCharsetPreset::Standard };
        std::string m_charset;
        ConsoleFrameStats m_lastStats{};
    };

    // Runs Console ASCII mode (static snapshot or live auto-zoom/interactive terminal loop).
    // Returns 0 on normal completion, 1 on failure, or 130 if canceled via `externalCancel`.
    int RunConsoleAsciiCli(
        const ConsoleAsciiOptions& options,
        std::ostream& out,
        std::wostream& err,
        const std::atomic<bool>* externalCancel = nullptr,
        bool interactiveTerminal = false);
}
