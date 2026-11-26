#include "pch.h"
#include "MainWindow.xaml.h"
#include "ViewState.h"
#include "FrameTiming.h"
#include "FramePresentation.h"
#include "JuliaView.h"
#include "resource.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif

#include <dwmapi.h>
#include <iomanip>
#include <sstream>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Microsoft::UI::Composition;

namespace winrt::MandelbrotViewer::implementation
{
    winrt::hstring MainWindow::FormatFloat(double val, int precision)
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(precision) << val;
        return winrt::hstring(ss.str());
    }

    static winrt::hstring FormatCoordinate(double val)
    {
        std::wostringstream ss;
        ss << (val >= 0.0 ? L"+" : L"") << std::fixed << std::setprecision(8) << val;
        return winrt::hstring(ss.str());
    }

    winrt::hstring MainWindow::FormatZoom(double zoom)
    {
        std::wostringstream ss;
        if (zoom < 1e4)
        {
            ss << std::fixed << std::setprecision(1) << zoom << L"x";
        }
        else
        {
            ss << std::scientific << std::setprecision(2) << zoom << L"x";
        }
        return winrt::hstring(ss.str());
    }

    void MainWindow::UpdatePlayStopVisual(
        FontIcon const& icon,
        UIElement const& button,
        bool running,
        wchar_t const* startTooltip,
        wchar_t const* stopTooltip,
        wchar_t const* startAutoName,
        wchar_t const* stopAutoName) const
    {
        if (icon)
        {
            const bool dark = IsEffectiveDarkTheme();
            icon.Glyph(running ? L"\uE71A" : L"\uE768");
            const winrt::Windows::UI::Color color = running
                ? (dark ? winrt::Windows::UI::Color{ 255, 255, 107, 107 } : winrt::Windows::UI::Color{ 255, 197, 34, 31 })
                : (dark ? winrt::Windows::UI::Color{ 255, 81, 207, 102 } : winrt::Windows::UI::Color{ 255, 24, 128, 56 });
            icon.Foreground(Media::SolidColorBrush(color));
        }
        if (button)
        {
            ToolTipService::SetToolTip(button, box_value(running ? stopTooltip : startTooltip));
            if (startAutoName && stopAutoName)
            {
                Automation::AutomationProperties::SetName(button, running ? stopAutoName : startAutoName);
            }
        }
    }

    bool MainWindow::QuerySystemDarkTheme() const
    {
        if (m_uiSettings)
        {
            const auto bg = m_uiSettings.GetColorValue(
                winrt::Windows::UI::ViewManagement::UIColorType::Background);
            return Mandelbrot::IsSystemBackgroundDark(bg.R, bg.G, bg.B);
        }
        return Application::Current().RequestedTheme() == ApplicationTheme::Dark;
    }

    bool MainWindow::IsEffectiveDarkTheme() const
    {
        return Mandelbrot::IsDarkAppTheme(m_appTheme, QuerySystemDarkTheme());
    }

    void MainWindow::ApplyAppTheme(Mandelbrot::AppTheme theme)
    {
        m_appTheme = theme;
        if (WindowRoot())
        {
            const bool highContrast = m_accessibilitySettings && m_accessibilitySettings.HighContrast();
            ElementTheme targetTheme = ElementTheme::Default;
            if (theme == Mandelbrot::AppTheme::Light)
            {
                targetTheme = ElementTheme::Light;
            }
            else if (theme == Mandelbrot::AppTheme::Dark)
            {
                targetTheme = ElementTheme::Dark;
            }
            else if (!highContrast)
            {
                const bool systemDark = QuerySystemDarkTheme();
                const bool appStartupDark = Application::Current().RequestedTheme() == ApplicationTheme::Dark;
                targetTheme = (systemDark == appStartupDark)
                    ? ElementTheme::Default
                    : (systemDark ? ElementTheme::Dark : ElementTheme::Light);
            }
            if (WindowRoot().RequestedTheme() != targetTheme)
            {
                WindowRoot().RequestedTheme(targetTheme);
            }
        }

        if (AppThemeComboBox())
        {
            const int desiredIndex = static_cast<int>(theme);
            if (AppThemeComboBox().SelectedIndex() != desiredIndex)
            {
                ScopedEventSuppress suppress(m_suppressEventDepth);
                AppThemeComboBox().SelectedIndex(desiredIndex);
            }
        }

        SyncWindowThemeVisuals();
    }

    void MainWindow::SyncWindowThemeVisuals()
    {
        if (m_isClosed) return;
        const bool dark = IsEffectiveDarkTheme();

        if (auto appWindow = AppWindow())
        {
            if (winrt::Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported())
            {
                appWindow.TitleBar().PreferredTheme(
                    dark ? winrt::Microsoft::UI::Windowing::TitleBarTheme::Dark
                         : winrt::Microsoft::UI::Windowing::TitleBarTheme::Light);
            }
        }

        if (m_hwnd)
        {
            const BOOL useDark = dark ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        }

        if (ThemeCycleIcon() && ThemeCycleButton())
        {
            wchar_t const* glyph = L"\uE770";
            std::wstring tooltip = L"Appearance: ";
            tooltip += Mandelbrot::GetAppThemeName(m_appTheme);
            if (m_appTheme == Mandelbrot::AppTheme::System)
            {
                glyph = L"\uE770";
                tooltip += dark ? L" (Dark)" : L" (Light)";
            }
            else if (m_appTheme == Mandelbrot::AppTheme::Light)
            {
                glyph = L"\uE706";
            }
            else
            {
                glyph = L"\uE708";
            }
            tooltip += L" \u2014 Click to switch";
            ThemeCycleIcon().Glyph(glyph);
            ToolTipService::SetToolTip(ThemeCycleButton(), box_value(tooltip));
            Automation::AutomationProperties::SetName(ThemeCycleButton(), tooltip);
        }

        UpdatePlayStopVisual(
            OverlayPlayZoomIcon(), OverlayPlayZoomButton(), m_isZoomAnimating,
            L"Start Animated Zoom", L"Stop Animated Zoom");
        UpdatePlayStopVisual(
            JuliaTravelIcon(), JuliaTravelOverlayButton(), m_juliaTraveling,
            L"Start Random Julia Tour", L"Stop Random Julia Tour",
            L"Start random Julia tour", L"Stop random Julia tour");
    }

    std::unique_ptr<Mandelbrot::BackgroundRenderer> MainWindow::CreateBackgroundRenderer(void (MainWindow::*onCompleted)())
    {
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        return std::make_unique<Mandelbrot::BackgroundRenderer>([weak, dispatcher, onCompleted]
        {
            dispatcher.TryEnqueue([weak, onCompleted]
            {
                if (auto window = weak.get()) (window.get()->*onCompleted)();
            });
        }, Mandelbrot::BackgroundRenderer::Compute{}, Mandelbrot::ExecutionMode::Gpu);
    }

    void MainWindow::SyncMaxIterationsUi(int value, int sliderThreshold)
    {
        if (!IterationsSlider() || !IterationsValueText()) return;
        if (sliderThreshold > 0 && std::abs(static_cast<int>(IterationsSlider().Value()) - value) < sliderThreshold)
            return;
        ScopedEventSuppress suppress(m_suppressEventDepth);
        IterationsSlider().Value(value);
        IterationsValueText().Text(winrt::to_hstring(value));
    }

    void MainWindow::ApplyBookmark(const Mandelbrot::BookmarkLocation& bookmark, bool selectComboIndex, int comboIndex)
    {
        m_config.SetCenter(bookmark.centerX, bookmark.centerY);
        m_config.zoom = bookmark.zoom;
        m_baseIterations = bookmark.maxIterations;
        m_config.maxIterations = bookmark.maxIterations;

        ScopedEventSuppress suppress(m_suppressEventDepth);
        SyncMaxIterationsUi(bookmark.maxIterations);
        if (selectComboIndex) BookmarksComboBox().SelectedIndex(comboIndex);
    }

    void MainWindow::CopyTextToClipboard(
        std::wstring const& text,
        TextBlock const& statusTarget,
        wchar_t const* successMessage)
    {
        try
        {
            auto package = winrt::Windows::ApplicationModel::DataTransfer::DataPackage();
            package.SetText(text);
            winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            statusTarget.Text(successMessage);
        }
        catch (const winrt::hresult_error&)
        {
            statusTarget.Text(L"Could not access clipboard. Please try again.");
        }
    }

    MainWindow::MainWindow()
    {
        {
            ScopedEventSuppress suppress(m_suppressEventDepth);
            InitializeComponent();
        }
        Title(L"Fractal Studio - Mandelbrot Set");

        if (auto native = try_as<IWindowNative>())
        {
            HWND hwnd = nullptr;
            if (SUCCEEDED(native->get_WindowHandle(&hwnd)) && hwnd)
            {
                m_hwnd = hwnd;
                const UINT dpi = GetDpiForWindow(hwnd);
                const HINSTANCE instance = GetModuleHandleW(nullptr);
                const int bigW = dpi ? GetSystemMetricsForDpi(SM_CXICON, dpi) : GetSystemMetrics(SM_CXICON);
                const int bigH = dpi ? GetSystemMetricsForDpi(SM_CYICON, dpi) : GetSystemMetrics(SM_CYICON);
                const int smallW = dpi ? GetSystemMetricsForDpi(SM_CXSMICON, dpi) : GetSystemMetrics(SM_CXSMICON);
                const int smallH = dpi ? GetSystemMetricsForDpi(SM_CYSMICON, dpi) : GetSystemMetrics(SM_CYSMICON);
                if (auto hBig = static_cast<HICON>(LoadImageW(
                    instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, bigW, bigH, LR_SHARED)))
                {
                    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBig));
                }
                if (auto hSmall = static_cast<HICON>(LoadImageW(
                    instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, smallW, smallH, LR_SHARED)))
                {
                    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmall));
                }
            }
        }

        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        try
        {
            m_uiSettings = winrt::Windows::UI::ViewManagement::UISettings();
            m_accessibilitySettings = winrt::Windows::UI::ViewManagement::AccessibilitySettings();
            auto refreshSystemTheme = [weak, dispatcher]
            {
                dispatcher.TryEnqueue([weak]
                {
                    if (auto self = weak.get(); self && !self->m_isClosed)
                    {
                        if (self->m_appTheme == Mandelbrot::AppTheme::System)
                        {
                            self->ApplyAppTheme(Mandelbrot::AppTheme::System);
                        }
                        else
                        {
                            self->SyncWindowThemeVisuals();
                        }
                    }
                });
            };
            m_colorValuesChangedRevoker = m_uiSettings.ColorValuesChanged(
                winrt::auto_revoke, [refreshSystemTheme](auto&&, auto&&) { refreshSystemTheme(); });
            m_highContrastChangedRevoker = m_accessibilitySettings.HighContrastChanged(
                winrt::auto_revoke, [refreshSystemTheme](auto&&, auto&&) { refreshSystemTheme(); });
        }
        catch (const winrt::hresult_error&)
        {
        }

        WindowRoot().ActualThemeChanged([weak](auto&&, auto&&)
        {
            if (auto self = weak.get(); self && !self->m_isClosed)
            {
                self->SyncWindowThemeVisuals();
            }
        });
        WindowRoot().Loaded([weak](auto&&, auto&&)
        {
            if (auto self = weak.get(); self && !self->m_isClosed)
            {
                self->SyncWindowThemeVisuals();
            }
        });

        m_renderer = CreateBackgroundRenderer(&MainWindow::OnRenderCompleted);
        InitializeControls();
        Closed([weak](auto&&, auto&&)
        {
            auto self = weak.get();
            if (!self) return;
            self->m_isClosed = true;
            self->m_colorValuesChangedRevoker.revoke();
            self->m_highContrastChangedRevoker.revoke();
            if (self->m_movieProgressTimer) self->m_movieProgressTimer.Stop();
            if (self->m_movieExporter) self->m_movieExporter->Stop();
            self->StopJuliaTravel(false);
            self->m_isZoomAnimating = false;
            self->m_isDragging = false;
            self->FreezePresentation();
            if (self->m_animateTimer) self->m_animateTimer.Stop();
            if (self->m_zoomTimer) self->m_zoomTimer.Stop();
            if (self->m_renderer) self->m_renderer->Stop();
            if (self->m_juliaRenderer) self->m_juliaRenderer->Stop();
        });
    }

    void MainWindow::InitializeControls()
    {
        {
            ScopedEventSuppress suppress(m_suppressEventDepth);

            // Initialize preview bitmap (256 x 1)
            m_previewBitmap = WriteableBitmap(256, 1);
            PalettePreviewImage().Source(m_previewBitmap);

            // Populate App Appearance Themes (System Default selected on launch)
            for (const auto theme : Mandelbrot::GetAllAppThemes())
            {
                AppThemeComboBox().Items().Append(box_value(Mandelbrot::GetAppThemeName(theme)));
            }
            AppThemeComboBox().SelectedIndex(static_cast<int>(m_appTheme));

            // Populate Fractal Formulas
            for (const auto& type : Mandelbrot::GetAllFractalTypes())
            {
                FractalTypeComboBox().Items().Append(box_value(Mandelbrot::GetFractalTypeName(type)));
            }
            FractalTypeComboBox().SelectedIndex(0);

            // Populate Landmark Bookmarks
            PopulateBookmarks();

            // Populate Theme Presets
            const auto& presets = Mandelbrot::GetAllPresets();
            for (const auto& preset : presets)
            {
                ThemePresetComboBox().Items().Append(box_value(Mandelbrot::GetPresetName(preset)));
            }
            ThemePresetComboBox().SelectedIndex(0);

            // Setup live animation timer
            auto weak = get_weak();
            m_animateTimer = DispatcherTimer();
            m_animateTimer.Interval(std::chrono::milliseconds(25));
            m_animateTimer.Tick([weak](auto&&, auto&&)
            {
                auto self = weak.get();
                if (!self || self->m_isClosed) return;
                self->m_phaseOffset = std::fmod(self->m_phaseOffset + 0.005f, 1.0f);

                {
                    ScopedEventSuppress innerSuppress(self->m_suppressEventDepth);
                    self->PhaseSlider().Value(self->m_phaseOffset);
                    self->PhaseValueText().Text(FormatFloat(self->m_phaseOffset, 2));
                }

                self->RecolorBuffer();
            });

            // Setup smooth continuous zoom timer
            m_zoomTimer = DispatcherTimer();
            m_zoomTimer.Interval(Mandelbrot::ZoomFrameDelay(std::chrono::steady_clock::duration::zero()));
            m_zoomTimer.Tick([weak](auto&&, auto&&)
            {
                auto self = weak.get();
                if (!self || self->m_isClosed) return;
                if (!self->m_isZoomAnimating)
                {
                    self->m_zoomTimer.Stop();
                    return;
                }

                // Calculation runs on the worker; this callback only advances the requested camera.
                self->m_zoomTimer.Stop();

                const auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - self->m_lastZoomFrameTime).count();
                self->m_lastZoomFrameTime = now;
                dt = std::clamp(dt, 0.008, 0.10);

                // A resize or manual jump needs its first valid frame before motion can resume.
                if (!self->m_hasCurrentFrame || !self->m_hasPresentation)
                {
                    self->m_zoomTimer.Interval(std::chrono::milliseconds(16));
                    self->m_zoomTimer.Start();
                    return;
                }

                const double speed = Mandelbrot::PresentationZoomSpeed(self->m_zoomSpeed, self->m_lastFractalTimeMs);
                const double exponent = (self->m_zoomInDirection ? 1.0 : -1.0) * speed * dt;
                const double factor = std::pow(2.0, exponent);

                self->m_retargetPanProgress = std::min(1.0, self->m_retargetPanProgress + dt / 0.40);
                const double s = self->m_retargetPanProgress;
                const double panBlend = std::clamp(s * s * (3.0 - 2.0 * s), 0.08, 1.0);

                auto nextCamera = self->m_config;
                Mandelbrot::MandelbrotEngine::StepZoomAnimation(
                    nextCamera, self->m_zoomTargetX, self->m_zoomTargetY, factor, panBlend);
                const double span = 3.0 / self->m_config.zoom;
                const bool jump = std::abs((nextCamera.hpCenterX - self->m_config.hpCenterX).ToDouble()) > span * 2.0 ||
                    std::abs((nextCamera.hpCenterY - self->m_config.hpCenterY).ToDouble()) > span * 2.0;
                self->m_config = jump ? nextCamera : Mandelbrot::LimitCameraToFrame(self->m_displayedConfig, self->m_config, nextCamera);
                const bool canContinue = self->m_zoomInDirection
                    ? self->m_config.zoom < Mandelbrot::MandelbrotEngine::MaxZoom
                    : self->m_config.zoom > Mandelbrot::MandelbrotEngine::MinZoom;

                if (self->m_autoScaleIterations)
                {
                    const int rawRecommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                        self->m_config.zoom, self->m_config.fractalType, self->m_baseIterations);
                    if (rawRecommended != self->m_config.maxIterations)
                    {
                        self->m_config.maxIterations = rawRecommended;
                        // Throttle XAML Slider layout updates to steps of >= 10 while keeping maxIterations smooth
                        self->SyncMaxIterationsUi(rawRecommended, 10);
                    }
                }

                // Keep spatial sampling stable; PresentationZoomSpeed adapts the camera pace.
                self->m_config.edgeRefine = true;
                self->m_config.renderStride = 2;

                if (jump) self->RenderFractal();
                else
                {
                    self->AnimateCamera(self->m_config, std::chrono::milliseconds(32));
                    self->RenderFractal(true);
                }

                if (!canContinue)
                {
                    self->StopZoomAnimation();
                }
                else if (self->m_isZoomAnimating)
                {
                    self->m_zoomTimer.Interval(Mandelbrot::ZoomFrameDelay(std::chrono::steady_clock::now() - now));
                    self->m_zoomTimer.Start();
                }
            });

            // Populate custom gradient stops UI
            PopulateStopSelector();
            SyncTargetInputsFromCenter();
        }

        ApplyAppTheme(m_appTheme);
        UpdateGradientPreview();
    }

    void MainWindow::PopulateBookmarks()
    {
        ScopedEventSuppress suppress(m_suppressEventDepth);
        BookmarksComboBox().Items().Clear();
        const auto& bookmarks = Mandelbrot::GetBookmarks(m_config.fractalType);
        for (const auto& bm : bookmarks)
        {
            BookmarksComboBox().Items().Append(box_value(bm.name));
        }
        if (!bookmarks.empty())
        {
            BookmarksComboBox().SelectedIndex(0);
        }
    }

    void MainWindow::OnViewportSizeChanged(IInspectable const&, SizeChangedEventArgs const& e)
    {
        if (e.NewSize().Width <= 0 || e.NewSize().Height <= 0) return;
        const int w = std::max(64, static_cast<int>(std::ceil(e.NewSize().Width)));
        const int h = std::max(64, static_cast<int>(std::ceil(e.NewSize().Height)));
        auto clip = Media::RectangleGeometry();
        clip.Rect(winrt::Windows::Foundation::Rect{ 0.0f, 0.0f, e.NewSize().Width, e.NewSize().Height });
        FractalSurface().Clip(clip);

        if (w == m_currentPixelWidth && h == m_currentPixelHeight && m_bitmap)
        {
            return;
        }

        FreezePresentation();
        m_hasPresentation = false;
        JuliaPointMarker().Visibility(Visibility::Collapsed);
        if (m_scaleExpression) FractalImage().StopAnimation(m_scaleExpression);
        if (m_offsetExpression) FractalImage().StopAnimation(m_offsetExpression);
        FractalImage().Scale(float3{ 1.0f, 1.0f, 1.0f });
        FractalImage().Translation(float3{ 0.0f, 0.0f, 0.0f });
        FractalImage().Width(w);
        FractalImage().Height(h);
        m_currentPixelWidth = w;
        m_currentPixelHeight = h;
        m_config.width = w;
        m_config.height = h;

        RenderFractal();
    }

    void MainWindow::RenderFractal(bool continueAnimation)
    {
        if (m_isClosed || m_movieExporter || !m_renderer || m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;
        if (!continueAnimation)
        {
            FreezePresentation();
            m_hasCurrentFrame = false;
            HudTimingText().Text(L"Rendering...");
            RefreshJuliaSettings();
        }
        const auto renderConfig = m_isZoomAnimating ? Mandelbrot::AnimationRenderConfig(m_config) : m_config;
        m_renderer->Submit(renderConfig, !continueAnimation);
    }

    void MainWindow::OnRenderCompleted()
    {
        if (m_isClosed || !m_renderer) return;
        const bool preserveMotion = m_isZoomAnimating && m_hasCurrentFrame && m_hasPresentation;
        bool rejected = false;
        auto result = m_renderer->TakeCompleted(m_iterations, [&](const Mandelbrot::RenderResult& ready)
        {
            // Check the entire in-flight motion segment before swapping images, so even a
            // delayed compositor frame remains covered by the new guard-band bitmap.
            rejected = preserveMotion && (!Mandelbrot::FrameCoversViewport(ready.config, m_motionFrom) ||
                !Mandelbrot::FrameCoversViewport(ready.config, m_motionTo) ||
                !Mandelbrot::FrameCoversViewport(ready.config, m_config));
            return !rejected;
        }, &m_subpixelSamples);
        if (rejected)
        {
            RenderFractal(true);
            return;
        }
        if (!result) return;
        if (result->error)
        {
            if (m_isZoomAnimating) StopZoomAnimation(false);
            m_renderer->Cancel();
            HudTimingText().Text(L"Fractal rendering failed. Try another view or a lower iteration count.");
            return;
        }

        m_displayedConfig = result->config;
        m_hasCurrentFrame = true;
        m_lastFractalTimeMs = result->elapsedMs;
        if (!m_bitmap || m_bitmap.PixelWidth() != result->config.width || m_bitmap.PixelHeight() != result->config.height)
        {
            m_bitmap = WriteableBitmap(result->config.width, result->config.height);
            FractalImage().Source(m_bitmap);
        }

        const auto colorStart = std::chrono::high_resolution_clock::now();
        RecolorBuffer(false);
        SetPresentationFrame(preserveMotion);
        const auto colorEnd = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> colorElapsed = colorEnd - colorStart;

        UpdateHud(result->elapsedMs, colorElapsed.count());
        if (m_isZoomAnimating)
        {
            // Retarget hysteresis counts completed frames, not timer ticks.
            ++m_framesSinceRetarget;
            if (m_autoPickInterestingTarget && m_zoomInDirection &&
                (!m_hasActiveRandomTarget ||
                 (m_framesSinceRetarget >= 15 &&
                  Mandelbrot::MandelbrotEngine::ShouldRetargetZoom(m_displayedConfig, m_iterations, m_zoomTargetX, m_zoomTargetY))))
            {
                PickAndApplyRandomTarget();
            }
        }
    }

    Mandelbrot::MandelbrotConfig MainWindow::PresentationCamera() const
    {
        if (!m_hasPresentation) return m_config;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_motionStart).count();
        const double duration = std::chrono::duration<double>(m_motionDuration).count();
        return Mandelbrot::InterpolateCamera(m_motionFrom, m_motionTo, duration > 0.0 ? elapsed / duration : 1.0);
    }

    void MainWindow::FreezePresentation()
    {
        if (!m_hasPresentation) return;
        const auto current = PresentationCamera();
        m_cameraProperties.StopAnimation(L"Scale");
        m_cameraProperties.StopAnimation(L"Offset");
        m_motionFrom = m_motionTo = current;
        m_motionStart = std::chrono::steady_clock::now();
    }

    void MainWindow::AnimateCamera(const Mandelbrot::MandelbrotConfig& view, std::chrono::milliseconds duration)
    {
        if (!m_hasPresentation) return;
        m_motionFrom = PresentationCamera();
        m_motionTo = view;
        m_motionStart = std::chrono::steady_clock::now();
        m_motionDuration = duration;
        const auto transform = Mandelbrot::FrameToViewport(m_cameraAnchor, view);
        auto scale = m_compositor.CreateScalarKeyFrameAnimation();
        scale.Target(L"Scale");
        scale.Duration(duration);
        scale.StopBehavior(AnimationStopBehavior::LeaveCurrentValue);
        scale.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
        scale.InsertKeyFrame(1.0f, static_cast<float>(transform.scale), m_compositor.CreateLinearEasingFunction());
        auto offset = m_compositor.CreateVector3KeyFrameAnimation();
        offset.Target(L"Offset");
        offset.Duration(duration);
        offset.StopBehavior(AnimationStopBehavior::LeaveCurrentValue);
        offset.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
        offset.InsertKeyFrame(1.0f, float3{ static_cast<float>(transform.x), static_cast<float>(transform.y), 0.0f },
            m_compositor.CreateLinearEasingFunction());
        auto group = m_compositor.CreateAnimationGroup();
        group.Add(scale);
        group.Add(offset);
        m_cameraProperties.StartAnimationGroup(group);
    }

    void MainWindow::BindPresentationExpressions()
    {
        // A single compositor camera survives bitmap swaps. Fresh images change only their
        // source-to-anchor mapping, so frame delivery never restarts the visible motion.
        m_scaleExpression = m_compositor.CreateExpressionAnimation(
            L"Vector3(frame.Scale * camera.Scale, frame.Scale * camera.Scale, 1.0)");
        m_scaleExpression.Target(L"Scale");
        m_scaleExpression.SetReferenceParameter(L"frame", m_frameProperties);
        m_scaleExpression.SetReferenceParameter(L"camera", m_cameraProperties);
        m_offsetExpression = m_compositor.CreateExpressionAnimation(L"frame.Offset * camera.Scale + camera.Offset");
        m_offsetExpression.Target(L"Translation");
        m_offsetExpression.SetReferenceParameter(L"frame", m_frameProperties);
        m_offsetExpression.SetReferenceParameter(L"camera", m_cameraProperties);
        FractalImage().StartAnimation(m_scaleExpression);
        FractalImage().StartAnimation(m_offsetExpression);
        m_juliaMarkerAnimation = nullptr;
    }

    void MainWindow::SetPresentationFrame(bool preserveMotion)
    {
        if (!m_compositor)
        {
            // GetElementVisual opts this image out of the UIElement transform APIs used below.
            m_compositor = Media::CompositionTarget::GetCompositorForCurrentThread();
            m_frameProperties = m_compositor.CreatePropertySet();
            m_frameProperties.InsertScalar(L"Scale", 1.0f);
            m_frameProperties.InsertVector3(L"Offset", float3{ 0.0f, 0.0f, 0.0f });
            FractalImage().CenterPoint(float3{ 0.0f, 0.0f, 0.0f });
        }
        const auto view = Mandelbrot::FrameViewport(m_displayedConfig, m_currentPixelWidth, m_currentPixelHeight);
        bool bind = !preserveMotion;
        if (!preserveMotion)
        {
            if (m_cameraProperties)
            {
                m_cameraProperties.StopAnimation(L"Scale");
                m_cameraProperties.StopAnimation(L"Offset");
            }
            m_cameraAnchor = view;
            m_cameraProperties = m_compositor.CreatePropertySet();
            m_cameraProperties.InsertScalar(L"Scale", 1.0f);
            m_cameraProperties.InsertVector3(L"Offset", float3{ 0.0f, 0.0f, 0.0f });
            m_motionFrom = m_motionTo = view;
            m_motionStart = std::chrono::steady_clock::now();
        }
        else
        {
            const auto bridge = Mandelbrot::FrameToViewport(m_cameraAnchor, view);
            if (bridge.scale > 1.5 || bridge.scale < 1.0 / 1.5 ||
                std::abs(bridge.x) > view.width || std::abs(bridge.y) > view.height)
            {
                // Rebase before float compositor coordinates grow large. Sample the old
                // camera on the compositor itself to keep the handoff continuous at deep zoom.
                const auto current = PresentationCamera();
                auto previous = m_cameraProperties;
                previous.StopAnimation(L"Scale");
                previous.StopAnimation(L"Offset");
                m_cameraAnchor = view;
                m_cameraProperties = m_compositor.CreatePropertySet();
                const auto initial = Mandelbrot::FrameToViewport(view, current);
                const auto target = Mandelbrot::FrameToViewport(view, m_motionTo);
                m_cameraProperties.InsertScalar(L"Scale", static_cast<float>(initial.scale));
                m_cameraProperties.InsertVector3(L"Offset", float3{ static_cast<float>(initial.x), static_cast<float>(initial.y), 0.0f });
                auto scale = m_compositor.CreateScalarKeyFrameAnimation();
                scale.Target(L"Scale");
                scale.Duration(std::chrono::milliseconds(32));
                scale.StopBehavior(AnimationStopBehavior::LeaveCurrentValue);
                scale.SetReferenceParameter(L"previous", previous);
                scale.SetScalarParameter(L"basisScale", static_cast<float>(bridge.scale));
                scale.InsertExpressionKeyFrame(0.0f, L"previous.Scale / basisScale");
                scale.InsertKeyFrame(1.0f, static_cast<float>(target.scale), m_compositor.CreateLinearEasingFunction());
                auto offset = m_compositor.CreateVector3KeyFrameAnimation();
                offset.Target(L"Offset");
                offset.Duration(std::chrono::milliseconds(32));
                offset.StopBehavior(AnimationStopBehavior::LeaveCurrentValue);
                offset.SetReferenceParameter(L"previous", previous);
                offset.SetScalarParameter(L"basisScale", static_cast<float>(bridge.scale));
                offset.SetVector3Parameter(L"basisOffset", float3{ static_cast<float>(bridge.x), static_cast<float>(bridge.y), 0.0f });
                offset.InsertExpressionKeyFrame(0.0f, L"previous.Offset - basisOffset * (previous.Scale / basisScale)");
                offset.InsertKeyFrame(1.0f, float3{ static_cast<float>(target.x), static_cast<float>(target.y), 0.0f },
                    m_compositor.CreateLinearEasingFunction());
                auto group = m_compositor.CreateAnimationGroup();
                group.Add(scale);
                group.Add(offset);
                m_cameraProperties.StartAnimationGroup(group);
                m_motionFrom = current;
                m_motionStart = std::chrono::steady_clock::now();
                m_motionDuration = std::chrono::milliseconds(32);
                bind = true;
            }
        }
        FractalImage().Width(m_displayedConfig.width);
        FractalImage().Height(m_displayedConfig.height);
        const auto source = Mandelbrot::FrameToViewport(m_displayedConfig, m_cameraAnchor);
        m_frameProperties.InsertScalar(L"Scale", static_cast<float>(source.scale));
        m_frameProperties.InsertVector3(L"Offset", float3{ static_cast<float>(source.x), static_cast<float>(source.y), 0.0f });
        if (bind) BindPresentationExpressions();
        m_hasPresentation = true;
        UpdateJuliaMarker();
    }

    void MainWindow::RecolorBuffer(bool includeJulia)
    {
        RecolorBitmap(m_bitmap, m_displayedConfig, m_iterations, m_subpixelSamples);
        if (includeJulia && m_juliaVisible)
            RecolorBitmap(m_juliaBitmap, m_juliaDisplayedConfig, m_juliaIterations, m_juliaSamples);
    }

    void MainWindow::RecolorBitmap(WriteableBitmap const& bitmap, const Mandelbrot::MandelbrotConfig& config,
        const std::vector<float>& iterations, const std::vector<Mandelbrot::SubpixelSamples>& samples)
    {
        if (!bitmap || iterations.empty()) return;

        const size_t totalPixels = static_cast<size_t>(bitmap.PixelWidth()) * static_cast<size_t>(bitmap.PixelHeight());
        if (iterations.size() < totalPixels) return;

        auto buffer = bitmap.PixelBuffer();
        auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();

        uint8_t* pRawBytes = nullptr;
        if (FAILED(byteAccess->Buffer(&pRawBytes)) || !pRawBytes) return;

        uint32_t* pPixels = reinterpret_cast<uint32_t*>(pRawBytes);

        m_theme.Recolor(
            iterations,
            pPixels,
            totalPixels,
            m_frequency,
            m_phaseOffset,
            m_invertColors,
            m_interiorColor,
            config.fractalType,
            config.width,
            &samples,
            true,
            &m_lighting);

        bitmap.Invalidate();
    }

    void MainWindow::UpdateHud(double fractalTimeMs, double colorTimeMs)
    {
        const auto config = PresentationCamera();
        std::wostringstream coords;
        if (config.zoom >= 1e6)
        {
            const std::wstring xStr = config.hpCenterX.ToWString(16);
            const std::wstring yStr = config.hpCenterY.ToWString(16);
            coords << L"Center: (" << xStr << L", " << yStr << L")";
        }
        else
        {
            coords << L"Center: (" << FormatCoordinate(config.centerX).c_str()
                   << L", " << FormatCoordinate(config.centerY).c_str() << L")";
        }
        HudCoordsText().Text(coords.str());

        std::wstring zoomLabel = L"Zoom: " + std::wstring(FormatZoom(config.zoom).c_str());
        if (config.zoom >= Mandelbrot::MandelbrotEngine::DeepZoomThreshold)
        {
            zoomLabel += L" [1024-bit Perturbation]";
        }
        if (m_isZoomAnimating)
        {
            if (m_zoomInDirection && m_autoPickInterestingTarget)
            {
                zoomLabel += L" [Auto-Exploring]";
            }
            else
            {
                zoomLabel += m_zoomInDirection ? L" [Zooming In]" : L" [Zooming Out]";
            }
        }
        HudZoomText().Text(zoomLabel);

        std::wostringstream timing;
        timing << L"Fractal: " << std::fixed << std::setprecision(1) << fractalTimeMs
               << L" ms | Color: " << std::fixed << std::setprecision(1) << colorTimeMs
               << L" ms | Iter: " << m_displayedConfig.maxIterations;
        HudTimingText().Text(timing.str());
    }

    void MainWindow::OnFractalTypeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;

        if (m_isZoomAnimating)
        {
            StopZoomAnimation(false);
        }

        const int index = FractalTypeComboBox().SelectedIndex();
        const auto& types = Mandelbrot::GetAllFractalTypes();
        if (index >= 0 && index < static_cast<int>(types.size()))
        {
            const auto type = types[index];
            m_config.fractalType = type;
            const bool supportsJulia = Mandelbrot::SupportsJuliaCompanion(type);
            JuliaOptionsPanel().Visibility(supportsJulia ? Visibility::Visible : Visibility::Collapsed);
            if (!supportsJulia) ShowJuliaToggle().IsOn(false);

            const auto typeName = Mandelbrot::GetFractalTypeName(type);
            HudTitleText().Text(winrt::hstring(typeName));
            Title(winrt::hstring(L"Fractal Studio - " + typeName));

            PopulateBookmarks();

            const auto& bookmarks = Mandelbrot::GetBookmarks(type);
            if (!bookmarks.empty())
            {
                ApplyBookmark(bookmarks[0]);
            }

            if (supportsJulia)
            {
                UpdateJuliaLayout();
                if (m_juliaVisible)
                {
                    SelectJuliaParameter(m_config.hpCenterX, m_config.hpCenterY);
                }
            }

            SyncTargetInputsFromCenter();
            RenderFractal();
        }
    }

    void MainWindow::OnBookmarkSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;

        if (m_isZoomAnimating)
        {
            StopZoomAnimation(false);
        }

        const int index = BookmarksComboBox().SelectedIndex();
        const auto& bookmarks = Mandelbrot::GetBookmarks(m_config.fractalType);
        if (index >= 0 && index < static_cast<int>(bookmarks.size()))
        {
            ApplyBookmark(bookmarks[index]);
            SyncTargetInputsFromCenter();
            RenderFractal();
        }
    }

    void MainWindow::OnIterationsSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        const int iter = static_cast<int>(e.NewValue());
        if (IterationsValueText())
        {
            IterationsValueText().Text(winrt::to_hstring(iter));
        }

        if (IsSuppressingEvents()) return;
        m_baseIterations = iter;
        m_config.maxIterations = iter;
        RenderFractal();
    }

    void MainWindow::OnAutoIterationsToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents() || !AutoIterationsToggle() || !IterationsSlider() || !IterationsValueText()) return;
        m_autoScaleIterations = AutoIterationsToggle().IsOn();
        if (m_autoScaleIterations)
        {
            const int recommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_baseIterations);
            m_config.maxIterations = recommended;
            SyncMaxIterationsUi(recommended);
            RenderFractal();
        }
        RefreshJuliaSettings();
    }

    void MainWindow::OnSmoothToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents() || !SmoothToggle()) return;
        m_config.smooth = SmoothToggle().IsOn();
        RenderFractal();
    }
}
