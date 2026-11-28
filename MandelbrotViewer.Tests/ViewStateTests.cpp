#include "pch.h"
#include "TestHarness.h"
#include "ViewState.h"

using namespace Mandelbrot;

namespace
{
    std::wstring JsonValue(const std::wstring& json, const std::wstring& name)
    {
        const std::wstring key = L"\"" + name + L"\": ";
        const size_t start = json.find(key);
        ASSERT_TRUE(start != std::wstring::npos);
        const size_t valueStart = start + key.size();
        if (json[valueStart] == L'"')
        {
            const size_t end = json.find(L'"', valueStart + 1);
            return json.substr(valueStart + 1, end - valueStart - 1);
        }
        return json.substr(valueStart, json.find_first_of(L",\n}", valueStart) - valueStart);
    }
}

TEST_CASE(ViewState, CoordinatesRetainSubpixelPrecisionAtDeepZoom)
{
    MandelbrotConfig config;
    config.width = 1920;
    config.height = 1080;
    ColorTheme theme;
    for (double zoom : { 1.0, 1e14, 1e40, 1e100, 1e200 })
    {
        config.zoom = zoom;
        const double pixelSpan = 3.0 / zoom / config.width;
        config.SetCenter(BigFloat(-0.75) + BigFloat(pixelSpan * 0.123456789),
            BigFloat(0.125) - BigFloat(pixelSpan * 0.987654321));
        const auto json = SerializeViewState(config, theme, 1.0f, 0.0f, false, 0xFF000000);
        BigFloat restoredX;
        BigFloat restoredY;
        ASSERT_TRUE(BigFloat::TryParse(JsonValue(json, L"centerX"), restoredX));
        ASSERT_TRUE(BigFloat::TryParse(JsonValue(json, L"centerY"), restoredY));
        ASSERT_LT(std::abs((restoredX - config.hpCenterX).ToDouble()) / pixelSpan, 0.0001);
        ASSERT_LT(std::abs((restoredY - config.hpCenterY).ToDouble()) / pixelSpan, 0.0001);
    }
    ASSERT_EQ(static_cast<size_t>(308), CoordinateDecimalPlaces(MandelbrotEngine::MaxZoom));
}

TEST_CASE(ViewState, CopyIncludesCustomPaletteAndRenderSettings)
{
    MandelbrotConfig config;
    config.zoom = 123456789.12345679;
    config.smooth = false;
    ColorTheme theme;
    theme.SetStops({ { 0.0f, 1, 2, 3 }, { 0.5f, 4, 5, 6 }, { 1.0f, 7, 8, 9 } });
    const auto json = SerializeViewState(config, theme, 0.125f, 0.25f, true, 0xFF123456);

    ASSERT_TRUE(JsonValue(json, L"theme") == L"Custom Palette");
    ASSERT_TRUE(JsonValue(json, L"invert") == L"true");
    ASSERT_TRUE(JsonValue(json, L"smooth") == L"false");
    ASSERT_TRUE(JsonValue(json, L"interiorColor") == std::to_wstring(0xFF123456u));
    ASSERT_TRUE(JsonValue(json, L"width") == std::to_wstring(config.width));
    ASSERT_TRUE(JsonValue(json, L"height") == std::to_wstring(config.height));
    ASSERT_EQ(config.zoom, std::stod(JsonValue(json, L"zoom")));
    ASSERT_TRUE(json.find(L"{\"position\": 0.5, \"r\": 4, \"g\": 5, \"b\": 6}") != std::wstring::npos);
}

TEST_CASE(ViewState, JsonUsesInvariantNumbersRegardlessOfUserLocale)
{
    struct CommaDecimal : std::numpunct<wchar_t>
    {
        wchar_t do_decimal_point() const override { return L','; }
        wchar_t do_thousands_sep() const override { return L'.'; }
        std::string do_grouping() const override { return "\3"; }
    };
    struct RestoreLocale
    {
        std::locale previous = std::locale();
        ~RestoreLocale() { std::locale::global(previous); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    MandelbrotConfig config;
    config.zoom = 1234.5;
    const auto json = SerializeViewState(config, ColorTheme(), 0.125f, 0.25f, false, 0xFF000000);
    ASSERT_TRUE(JsonValue(json, L"zoom") == L"1234.5");
    ASSERT_TRUE(JsonValue(json, L"frequency") == L"0.125");
    ASSERT_TRUE(JsonValue(json, L"phaseOffset") == L"0.25");
    ASSERT_TRUE(JsonValue(json, L"width") == L"1024");
}

TEST_CASE(ViewState, AppThemeDefaultsToSystemAndResolvesLightAndDarkModes)
{
    constexpr auto themes = GetAllAppThemes();
    ASSERT_EQ(static_cast<size_t>(3), themes.size());
    ASSERT_TRUE(themes[0] == AppTheme::System);
    ASSERT_TRUE(themes[1] == AppTheme::Light);
    ASSERT_TRUE(themes[2] == AppTheme::Dark);

    ASSERT_TRUE(std::wstring(GetAppThemeName(AppTheme::System)) == L"System Default");
    ASSERT_TRUE(std::wstring(GetAppThemeName(AppTheme::Light)) == L"Light");
    ASSERT_TRUE(std::wstring(GetAppThemeName(AppTheme::Dark)) == L"Dark");

    // System Default adopts the OS dark/light theme dynamically.
    ASSERT_TRUE(IsDarkAppTheme(AppTheme::System, true));
    ASSERT_TRUE(!IsDarkAppTheme(AppTheme::System, false));

    // Explicit Light / Dark overrides ignore the OS theme.
    ASSERT_TRUE(!IsDarkAppTheme(AppTheme::Light, true));
    ASSERT_TRUE(!IsDarkAppTheme(AppTheme::Light, false));
    ASSERT_TRUE(IsDarkAppTheme(AppTheme::Dark, true));
    ASSERT_TRUE(IsDarkAppTheme(AppTheme::Dark, false));

    // Quick-cycle order covers System -> Light -> Dark -> System.
    ASSERT_TRUE(CycleAppTheme(AppTheme::System) == AppTheme::Light);
    ASSERT_TRUE(CycleAppTheme(AppTheme::Light) == AppTheme::Dark);
    ASSERT_TRUE(CycleAppTheme(AppTheme::Dark) == AppTheme::System);

    // UISettings perceptual background luma check.
    ASSERT_TRUE(IsSystemBackgroundDark(0, 0, 0));
    ASSERT_TRUE(IsSystemBackgroundDark(32, 32, 32));
    ASSERT_TRUE(!IsSystemBackgroundDark(255, 255, 255));
    ASSERT_TRUE(!IsSystemBackgroundDark(243, 243, 243));
}

TEST_CASE(ViewState, DirectionalLightingSerializesOnlyWhenEnabled)
{
    MandelbrotConfig config;
    ColorTheme theme;

    const auto offJson = SerializeViewState(config, theme, 1.0f, 0.0f, false, 0xFF000000);
    ASSERT_TRUE(offJson.find(L"lighting3d") == std::wstring::npos);

    DirectionalLightingParams lit{};
    lit.enabled = true;
    lit.azimuthDeg = 210.0f;
    lit.elevationDeg = 35.0f;
    lit.strength = 1.75f;
    const auto onJson = SerializeViewState(config, theme, 1.0f, 0.0f, false, 0xFF000000, lit);
    ASSERT_TRUE(JsonValue(onJson, L"lighting3d") == L"true");
    ASSERT_NEAR(210.0, std::stod(JsonValue(onJson, L"lightAzimuth")), 1e-4);
    ASSERT_NEAR(35.0, std::stod(JsonValue(onJson, L"lightElevation")), 1e-4);
    ASSERT_NEAR(1.75, std::stod(JsonValue(onJson, L"lightStrength")), 1e-4);
}

TEST_CASE(ViewState, HistogramEqualizeSerializesOnlyWhenEnabled)
{
    MandelbrotConfig config;
    ColorTheme theme;

    const auto offJson = SerializeViewState(config, theme, 1.0f, 0.0f, false, 0xFF000000);
    ASSERT_TRUE(offJson.find(L"histogramEqualize") == std::wstring::npos);

    const auto onJson = SerializeViewState(config, theme, 1.0f, 0.0f, false, 0xFF000000, {}, true);
    ASSERT_TRUE(JsonValue(onJson, L"histogramEqualize") == L"true");
}


