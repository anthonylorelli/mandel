#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif

#include <iomanip>
#include <sstream>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Storage::Streams;

namespace winrt::MandelbrotViewer::implementation
{
    static winrt::hstring FormatFloat(double val, int precision = 2)
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

    static winrt::hstring FormatZoom(double zoom)
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

    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"Fractal Studio - Mandelbrot Set");

        InitializeControls();
    }

    void MainWindow::InitializeControls()
    {
        m_suppressEvents = true;

        // Initialize preview bitmap (256 x 1)
        m_previewBitmap = WriteableBitmap(256, 1);
        PalettePreviewImage().Source(m_previewBitmap);

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
        m_animateTimer = DispatcherTimer();
        m_animateTimer.Interval(std::chrono::milliseconds(25));
        m_animateTimer.Tick([this](auto&&, auto&&)
        {
            m_phaseOffset = std::fmod(m_phaseOffset + 0.005f, 1.0f);

            m_suppressEvents = true;
            PhaseSlider().Value(m_phaseOffset);
            PhaseValueText().Text(FormatFloat(m_phaseOffset, 2));
            m_suppressEvents = false;

            RecolorBuffer();
        });

        // Setup smooth continuous zoom timer
        m_zoomTimer = DispatcherTimer();
        m_zoomTimer.Interval(std::chrono::milliseconds(16));
        m_zoomTimer.Tick([this](auto&&, auto&&)
        {
            if (!m_isZoomAnimating)
            {
                m_zoomTimer.Stop();
                return;
            }

            // Pause timer while computing the frame so timer events never pile up behind a deep-zoom render
            m_zoomTimer.Stop();

            const auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - m_lastZoomFrameTime).count();
            m_lastZoomFrameTime = now;
            dt = std::clamp(dt, 0.008, 0.10);

            const double exponent = (m_zoomInDirection ? 1.0 : -1.0) * m_zoomSpeed * dt;
            const double factor = std::pow(2.0, exponent);

            if (m_autoPickInterestingTarget && m_zoomInDirection)
            {
                m_framesSinceRetarget++;
                if (m_framesSinceRetarget >= 15 &&
                    Mandelbrot::MandelbrotEngine::ShouldRetargetZoom(m_config, m_iterations, m_zoomTargetX, m_zoomTargetY))
                {
                    PickAndApplyRandomTarget();
                }
            }

            m_retargetPanProgress = std::min(1.0, m_retargetPanProgress + dt / 0.40);
            const double s = m_retargetPanProgress;
            const double panBlend = std::clamp(s * s * (3.0 - 2.0 * s), 0.08, 1.0);

            const bool canContinue = Mandelbrot::MandelbrotEngine::StepZoomAnimation(
                m_config, m_zoomTargetX, m_zoomTargetY, factor, panBlend);

            if (m_autoScaleIterations)
            {
                const int rawRecommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                    m_config.zoom, m_config.fractalType, m_baseIterations);
                if (rawRecommended != m_config.maxIterations)
                {
                    m_config.maxIterations = rawRecommended;
                    // Throttle XAML Slider layout updates to steps of >= 10 while keeping maxIterations smooth
                    if (std::abs(static_cast<int>(IterationsSlider().Value()) - rawRecommended) >= 10)
                    {
                        m_suppressEvents = true;
                        IterationsSlider().Value(rawRecommended);
                        IterationsValueText().Text(winrt::to_hstring(rawRecommended));
                        m_suppressEvents = false;
                    }
                }
            }

            // Apply hysteresis-stabilized adaptive motion quality level:
            // Level 0: stride = 2, edgeRefine = true (~3.8x speedup, 1x1 exact interior/exterior boundaries, C1 smooth exterior)
            // Level 1: stride = 3, edgeRefine = true (~7x speedup for heavy deep-zoom frames)
            // Level 2: stride = 4, edgeRefine = false (~10x speedup for extreme iteration budgets)
            switch (m_animQualityLevel)
            {
            case 0:
                m_config.renderStride = 2;
                m_config.edgeRefine = true;
                break;
            case 1:
                m_config.renderStride = 3;
                m_config.edgeRefine = true;
                break;
            default:
                m_config.renderStride = 4;
                m_config.edgeRefine = false;
                break;
            }

            RenderFractal();

            // Hysteresis-stabilized adaptive governor: require sustained slow/fast streaks before changing stride
            if (m_lastFractalTimeMs > 22.0)
            {
                m_slowFrameStreak++;
                m_fastFrameStreak = 0;
                if (m_slowFrameStreak >= 2 && m_animQualityLevel < 2)
                {
                    m_animQualityLevel++;
                    m_slowFrameStreak = 0;
                }
            }
            else if (m_lastFractalTimeMs < 6.0)
            {
                m_fastFrameStreak++;
                m_slowFrameStreak = 0;
                if (m_fastFrameStreak >= 8 && m_animQualityLevel > 0)
                {
                    m_animQualityLevel--;
                    m_fastFrameStreak = 0;
                }
            }
            else
            {
                m_slowFrameStreak = 0;
                m_fastFrameStreak = 0;
            }

            if (!canContinue)
            {
                StopZoomAnimation();
            }
            else if (m_isZoomAnimating)
            {
                m_zoomTimer.Start();
            }
        });

        // Populate custom gradient stops UI
        PopulateStopSelector();
        SyncTargetInputsFromCenter();

        m_suppressEvents = false;

        UpdateGradientPreview();
    }

    void MainWindow::PopulateBookmarks()
    {
        m_suppressEvents = true;
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
        m_suppressEvents = false;
    }

    void MainWindow::OnViewportSizeChanged(IInspectable const&, SizeChangedEventArgs const& e)
    {
        const int w = std::max(64, static_cast<int>(e.NewSize().Width));
        const int h = std::max(64, static_cast<int>(e.NewSize().Height));

        if (w == m_currentPixelWidth && h == m_currentPixelHeight && m_bitmap)
        {
            return;
        }

        m_currentPixelWidth = w;
        m_currentPixelHeight = h;
        m_config.width = w;
        m_config.height = h;

        m_bitmap = WriteableBitmap(w, h);
        FractalImage().Source(m_bitmap);

        RenderFractal();
    }

    void MainWindow::RenderFractal()
    {
        if (!m_bitmap || m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;

        const double fractalTime = m_engine.Compute(m_config, m_iterations);
        m_lastFractalTimeMs = fractalTime;

        const auto colorStart = std::chrono::high_resolution_clock::now();
        RecolorBuffer();
        const auto colorEnd = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> colorElapsed = colorEnd - colorStart;

        UpdateHud(fractalTime, colorElapsed.count());
    }

    void MainWindow::RecolorBuffer()
    {
        if (!m_bitmap || m_iterations.empty()) return;

        const size_t totalPixels = static_cast<size_t>(m_currentPixelWidth) * static_cast<size_t>(m_currentPixelHeight);
        if (m_iterations.size() < totalPixels) return;

        auto buffer = m_bitmap.PixelBuffer();
        auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();

        uint8_t* pRawBytes = nullptr;
        if (FAILED(byteAccess->Buffer(&pRawBytes)) || !pRawBytes) return;

        uint32_t* pPixels = reinterpret_cast<uint32_t*>(pRawBytes);

        m_theme.Recolor(
            m_iterations,
            pPixels,
            totalPixels,
            m_frequency,
            m_phaseOffset,
            m_invertColors,
            m_interiorColor,
            m_config.fractalType,
            m_currentPixelWidth);

        m_bitmap.Invalidate();
    }

    void MainWindow::UpdateHud(double fractalTimeMs, double colorTimeMs)
    {
        std::wostringstream coords;
        if (m_config.zoom >= 1e6)
        {
            const std::wstring xStr = m_config.hpCenterX.ToWString(16);
            const std::wstring yStr = m_config.hpCenterY.ToWString(16);
            coords << L"Center: (" << (xStr[0] == L'-' ? L"" : L"+") << xStr
                   << L", " << (yStr[0] == L'-' ? L"" : L"+") << yStr << L")";
        }
        else
        {
            coords << L"Center: (" << FormatCoordinate(m_config.centerX).c_str()
                   << L", " << FormatCoordinate(m_config.centerY).c_str() << L")";
        }
        HudCoordsText().Text(coords.str());

        std::wstring zoomLabel = L"Zoom: " + std::wstring(FormatZoom(m_config.zoom).c_str());
        if (m_config.zoom >= Mandelbrot::MandelbrotEngine::DeepZoomThreshold)
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
               << L" ms | Iter: " << m_config.maxIterations;
        HudTimingText().Text(timing.str());
    }

    void MainWindow::UpdateGradientPreview()
    {
        if (!m_previewBitmap) return;

        auto buffer = m_previewBitmap.PixelBuffer();
        auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();

        uint8_t* pRaw = nullptr;
        if (SUCCEEDED(byteAccess->Buffer(&pRaw)) && pRaw)
        {
            uint32_t* pPixels = reinterpret_cast<uint32_t*>(pRaw);
            m_theme.GeneratePreview(pPixels, 256);
            m_previewBitmap.Invalidate();
        }
    }

    void MainWindow::ApplyZoom(double factor, double pivotX, double pivotY)
    {
        if (m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;

        Mandelbrot::MandelbrotEngine::ZoomViewport(m_config, factor, pivotX, pivotY);
        SyncTargetInputsFromCenter();

        if (m_autoScaleIterations)
        {
            const int recommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_baseIterations);
            m_config.maxIterations = recommended;

            m_suppressEvents = true;
            IterationsSlider().Value(recommended);
            IterationsValueText().Text(winrt::to_hstring(recommended));
            m_suppressEvents = false;
        }

        RenderFractal();
    }

    void MainWindow::SyncTargetInputsFromCenter()
    {
        m_zoomTargetX = m_config.hpCenterX;
        m_zoomTargetY = m_config.hpCenterY;
        m_hasActiveRandomTarget = false;
        UpdateTargetTextBoxes();
    }

    void MainWindow::UpdateTargetTextBoxes()
    {
        if (!TargetXTextBox() || !TargetYTextBox()) return;

        const size_t decimals = (m_config.zoom >= 1e14) ? 28 : ((m_config.zoom >= 1e6) ? 16 : 10);
        m_suppressEvents = true;
        TargetXTextBox().Text(winrt::hstring(m_zoomTargetX.ToWString(decimals)));
        TargetYTextBox().Text(winrt::hstring(m_zoomTargetY.ToWString(decimals)));
        m_suppressEvents = false;
    }

    void MainWindow::PickAndApplyRandomTarget()
    {
        if (m_iterations.empty() && m_currentPixelWidth > 0 && m_currentPixelHeight > 0)
        {
            m_engine.Compute(m_config, m_iterations);
        }

        Mandelbrot::MandelbrotEngine::PickInterestingTarget(
            m_config, m_iterations, m_zoomTargetX, m_zoomTargetY);
        m_hasActiveRandomTarget = true;
        m_retargetPanProgress = 0.0;
        m_framesSinceRetarget = 0;
        UpdateTargetTextBoxes();
    }

    void MainWindow::SetZoomDirection(bool zoomIn)
    {
        m_zoomInDirection = zoomIn;
        if (!ZoomDirectionInToggle() || !ZoomDirectionOutToggle()) return;

        m_suppressEvents = true;
        ZoomDirectionInToggle().IsChecked(zoomIn);
        ZoomDirectionOutToggle().IsChecked(!zoomIn);
        m_suppressEvents = false;
    }

    void MainWindow::StartZoomAnimation()
    {
        if (m_autoPickInterestingTarget && m_zoomInDirection)
        {
            if (!m_hasActiveRandomTarget ||
                Mandelbrot::MandelbrotEngine::ShouldRetargetZoom(m_config, m_iterations, m_zoomTargetX, m_zoomTargetY))
            {
                PickAndApplyRandomTarget();
            }
        }
        else if (TargetXTextBox() && TargetYTextBox())
        {
            Mandelbrot::BigFloat parsedX;
            Mandelbrot::BigFloat parsedY;
            const bool validX = Mandelbrot::BigFloat::TryParse(TargetXTextBox().Text(), parsedX);
            const bool validY = Mandelbrot::BigFloat::TryParse(TargetYTextBox().Text(), parsedY);

            m_zoomTargetX = validX ? parsedX : m_config.hpCenterX;
            m_zoomTargetY = validY ? parsedY : m_config.hpCenterY;
            m_retargetPanProgress = 1.0;

            if (!validX || !validY)
            {
                SyncTargetInputsFromCenter();
            }
        }
        else
        {
            m_zoomTargetX = m_config.hpCenterX;
            m_zoomTargetY = m_config.hpCenterY;
            m_retargetPanProgress = 1.0;
        }

        // If already at zoom bound in the chosen direction, flip direction or return
        if ((m_zoomInDirection && m_config.zoom >= Mandelbrot::MandelbrotEngine::MaxZoom) ||
            (!m_zoomInDirection && m_config.zoom <= Mandelbrot::MandelbrotEngine::MinZoom))
        {
            return;
        }

        m_isZoomAnimating = true;
        m_animQualityLevel = 0;
        m_slowFrameStreak = 0;
        m_fastFrameStreak = 0;
        m_lastZoomFrameTime = std::chrono::steady_clock::now();

        if (StartZoomButton()) StartZoomButton().IsEnabled(false);
        if (StopZoomButton()) StopZoomButton().IsEnabled(true);
        if (OverlayPlayZoomIcon())
        {
            OverlayPlayZoomIcon().Glyph(L"\uE71A");
            OverlayPlayZoomIcon().Foreground(Media::SolidColorBrush(winrt::Windows::UI::Color{ 255, 255, 107, 107 }));
        }
        if (OverlayPlayZoomButton())
        {
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                OverlayPlayZoomButton(), box_value(L"Stop Animated Zoom"));
        }

        if (m_zoomTimer)
        {
            m_zoomTimer.Start();
        }
    }

    void MainWindow::StopZoomAnimation()
    {
        m_isZoomAnimating = false;
        m_animQualityLevel = 0;
        m_slowFrameStreak = 0;
        m_fastFrameStreak = 0;
        m_config.renderStride = 1;
        m_config.edgeRefine = true;
        if (m_zoomTimer)
        {
            m_zoomTimer.Stop();
        }

        if (StartZoomButton()) StartZoomButton().IsEnabled(true);
        if (StopZoomButton()) StopZoomButton().IsEnabled(false);
        if (OverlayPlayZoomIcon())
        {
            OverlayPlayZoomIcon().Glyph(L"\uE768");
            OverlayPlayZoomIcon().Foreground(Media::SolidColorBrush(winrt::Windows::UI::Color{ 255, 81, 207, 102 }));
        }
        if (OverlayPlayZoomButton())
        {
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                OverlayPlayZoomButton(), box_value(L"Start Animated Zoom"));
        }

        if (m_autoScaleIterations)
        {
            const int recommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_baseIterations);
            m_config.maxIterations = recommended;
            m_suppressEvents = true;
            IterationsSlider().Value(recommended);
            IterationsValueText().Text(winrt::to_hstring(recommended));
            m_suppressEvents = false;
        }

        if (!m_autoPickInterestingTarget || !m_hasActiveRandomTarget)
        {
            SyncTargetInputsFromCenter();
        }
        else
        {
            UpdateTargetTextBoxes();
        }
        RenderFractal();
    }

    void MainWindow::OnOverlayZoomToggleClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isZoomAnimating)
        {
            StopZoomAnimation();
        }
        else
        {
            StartZoomAnimation();
        }
    }

    void MainWindow::OnUseCurrentCenterClick(IInspectable const&, RoutedEventArgs const&)
    {
        SyncTargetInputsFromCenter();
    }

    void MainWindow::OnRandomTargetToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents || !RandomTargetToggle()) return;
        m_autoPickInterestingTarget = RandomTargetToggle().IsOn();
        m_hasActiveRandomTarget = false;

        if (m_autoPickInterestingTarget && m_isZoomAnimating && m_zoomInDirection)
        {
            PickAndApplyRandomTarget();
        }
    }

    void MainWindow::OnZoomDirectionInClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents) return;
        SetZoomDirection(true);
    }

    void MainWindow::OnZoomDirectionOutClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents) return;
        SetZoomDirection(false);
    }

    void MainWindow::OnZoomSpeedSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        m_zoomSpeed = e.NewValue();
        if (ZoomSpeedValueText())
        {
            ZoomSpeedValueText().Text(FormatFloat(m_zoomSpeed, 1) + L"x");
        }
    }

    void MainWindow::OnStartZoomClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents) return;
        StartZoomAnimation();
    }

    void MainWindow::OnStopZoomClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents) return;
        StopZoomAnimation();
    }

    void MainWindow::OnViewportPointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        m_isDragging = true;
        m_dragStartPoint = e.GetCurrentPoint(FractalImage()).Position();
        m_dragStartHpCenterX = m_config.hpCenterX;
        m_dragStartHpCenterY = m_config.hpCenterY;
        FractalImage().CapturePointer(e.Pointer());
    }

    void MainWindow::OnViewportPointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_isDragging || m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;

        const auto currentPoint = e.GetCurrentPoint(FractalImage()).Position();
        const double dx = currentPoint.X - m_dragStartPoint.X;
        const double dy = currentPoint.Y - m_dragStartPoint.Y;

        Mandelbrot::MandelbrotEngine::PanViewport(
            m_config, m_dragStartHpCenterX, m_dragStartHpCenterY, dx, dy);
        SyncTargetInputsFromCenter();

        if (!m_isZoomAnimating)
        {
            m_config.renderStride = (m_config.zoom >= 1e8 || m_lastFractalTimeMs > 20.0) ? 2 : 1;
            m_config.edgeRefine = true;
        }

        RenderFractal();
    }

    void MainWindow::OnViewportPointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (m_isDragging)
        {
            m_isDragging = false;
            FractalImage().ReleasePointerCapture(e.Pointer());

            if (!m_isZoomAnimating && m_config.renderStride > 1)
            {
                m_config.renderStride = 1;
                m_config.edgeRefine = true;
                RenderFractal();
            }
        }
    }

    void MainWindow::OnViewportPointerWheelChanged(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        const int delta = e.GetCurrentPoint(FractalImage()).Properties().MouseWheelDelta();
        const auto pos = e.GetCurrentPoint(FractalImage()).Position();
        const double factor = (delta > 0) ? 1.25 : 0.8;
        ApplyZoom(factor, pos.X, pos.Y);
    }

    void MainWindow::OnViewportDoubleTapped(IInspectable const&, Input::DoubleTappedRoutedEventArgs const& e)
    {
        const auto pos = e.GetPosition(FractalImage());
        ApplyZoom(2.0, pos.X, pos.Y);
    }

    void MainWindow::OnZoomInClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyZoom(1.25, m_currentPixelWidth * 0.5, m_currentPixelHeight * 0.5);
    }

    void MainWindow::OnZoomOutClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyZoom(0.8, m_currentPixelWidth * 0.5, m_currentPixelHeight * 0.5);
    }

    void MainWindow::OnResetViewClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isZoomAnimating)
        {
            StopZoomAnimation();
        }

        const auto& bookmarks = Mandelbrot::GetBookmarks(m_config.fractalType);
        if (!bookmarks.empty())
        {
            const auto& bm = bookmarks[0];
            m_config.SetCenter(bm.centerX, bm.centerY);
            m_config.zoom = bm.zoom;
            m_baseIterations = bm.maxIterations;
            m_config.maxIterations = bm.maxIterations;

            m_suppressEvents = true;
            IterationsSlider().Value(bm.maxIterations);
            IterationsValueText().Text(winrt::to_hstring(bm.maxIterations));
            BookmarksComboBox().SelectedIndex(0);
            m_suppressEvents = false;
        }
        else
        {
            m_config.SetCenter(-0.75, 0.0);
            m_config.zoom = 1.0;
            m_baseIterations = 500;
            m_config.maxIterations = 500;

            m_suppressEvents = true;
            IterationsSlider().Value(500);
            IterationsValueText().Text(L"500");
            m_suppressEvents = false;
        }

        SyncTargetInputsFromCenter();
        RenderFractal();
    }

    void MainWindow::SetSidebarVisible(bool visible)
    {
        m_isSidebarVisible = visible;
        if (visible)
        {
            SidebarColumn().Width(winrt::Microsoft::UI::Xaml::GridLength{ 380, winrt::Microsoft::UI::Xaml::GridUnitType::Pixel });
            SidebarBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            OpenSidebarButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ToggleSidebarIcon().Glyph(L"\uE8A0");
        }
        else
        {
            SidebarBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SidebarColumn().Width(winrt::Microsoft::UI::Xaml::GridLength{ 0, winrt::Microsoft::UI::Xaml::GridUnitType::Pixel });
            OpenSidebarButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            ToggleSidebarIcon().Glyph(L"\uE89F");
        }
    }

    void MainWindow::OnCloseSidebarClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetSidebarVisible(false);
    }

    void MainWindow::OnOpenSidebarClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetSidebarVisible(true);
    }

    void MainWindow::OnToggleSidebarClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetSidebarVisible(!m_isSidebarVisible);
    }

    bool MainWindow::IsFullScreen()
    {
        return AppWindow().Presenter().Kind() == winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen;
    }

    void MainWindow::SetFullScreen(bool fullScreen)
    {
        if (fullScreen == IsFullScreen()) return;

        if (fullScreen)
        {
            // Remember the panel state so it can be restored, then give the fractal the whole screen
            m_sidebarVisibleBeforeFullScreen = m_isSidebarVisible;
            SetSidebarVisible(false);

            // FullScreen presenter removes title bar, borders and covers the taskbar
            AppWindow().SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen);
            ToggleFullScreenIcon().Glyph(L"\uE73F");
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(ToggleFullScreenButton(), box_value(L"Exit Full Screen (F11 or Esc)"));
        }
        else
        {
            AppWindow().SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::Overlapped);
            SetSidebarVisible(m_sidebarVisibleBeforeFullScreen);
            ToggleFullScreenIcon().Glyph(L"\uE740");
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(ToggleFullScreenButton(), box_value(L"Enter Full Screen (F11)"));
        }
    }

    void MainWindow::OnToggleFullScreenClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetFullScreen(!IsFullScreen());
    }

    void MainWindow::OnFullScreenAcceleratorInvoked(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        SetFullScreen(!IsFullScreen());
        e.Handled(true);
    }

    void MainWindow::OnEscapeAcceleratorInvoked(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        // Esc leaves full screen first; otherwise it toggles the controls panel
        if (IsFullScreen())
        {
            SetFullScreen(false);
        }
        else
        {
            SetSidebarVisible(!m_isSidebarVisible);
        }
        e.Handled(true);
    }

    void MainWindow::OnFractalTypeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressEvents) return;

        if (m_isZoomAnimating)
        {
            StopZoomAnimation();
        }

        const int index = FractalTypeComboBox().SelectedIndex();
        const auto& types = Mandelbrot::GetAllFractalTypes();
        if (index >= 0 && index < static_cast<int>(types.size()))
        {
            const auto type = types[index];
            m_config.fractalType = type;

            const auto typeName = Mandelbrot::GetFractalTypeName(type);
            HudTitleText().Text(winrt::hstring(typeName));
            Title(winrt::hstring(L"Fractal Studio - " + typeName));

            PopulateBookmarks();

            const auto& bookmarks = Mandelbrot::GetBookmarks(type);
            if (!bookmarks.empty())
            {
                const auto& bm = bookmarks[0];
                m_config.SetCenter(bm.centerX, bm.centerY);
                m_config.zoom = bm.zoom;
                m_baseIterations = bm.maxIterations;
                m_config.maxIterations = bm.maxIterations;

                m_suppressEvents = true;
                IterationsSlider().Value(bm.maxIterations);
                IterationsValueText().Text(winrt::to_hstring(bm.maxIterations));
                m_suppressEvents = false;
            }

            SyncTargetInputsFromCenter();
            RenderFractal();
        }
    }

    void MainWindow::OnBookmarkSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressEvents) return;

        if (m_isZoomAnimating)
        {
            StopZoomAnimation();
        }

        const int index = BookmarksComboBox().SelectedIndex();
        const auto& bookmarks = Mandelbrot::GetBookmarks(m_config.fractalType);
        if (index >= 0 && index < static_cast<int>(bookmarks.size()))
        {
            const auto& bm = bookmarks[index];
            m_config.SetCenter(bm.centerX, bm.centerY);
            m_config.zoom = bm.zoom;
            m_baseIterations = bm.maxIterations;
            m_config.maxIterations = bm.maxIterations;

            m_suppressEvents = true;
            IterationsSlider().Value(bm.maxIterations);
            IterationsValueText().Text(winrt::to_hstring(bm.maxIterations));
            m_suppressEvents = false;

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

        if (m_suppressEvents) return;
        m_baseIterations = iter;
        m_config.maxIterations = iter;
        RenderFractal();
    }

    void MainWindow::OnAutoIterationsToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents || !AutoIterationsToggle() || !IterationsSlider() || !IterationsValueText()) return;
        m_autoScaleIterations = AutoIterationsToggle().IsOn();
        if (m_autoScaleIterations)
        {
            const int recommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_baseIterations);
            m_config.maxIterations = recommended;

            m_suppressEvents = true;
            IterationsSlider().Value(recommended);
            IterationsValueText().Text(winrt::to_hstring(recommended));
            m_suppressEvents = false;
            RenderFractal();
        }
    }

    void MainWindow::OnSmoothToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents || !SmoothToggle()) return;
        m_config.smooth = SmoothToggle().IsOn();
        RenderFractal();
    }

    void MainWindow::OnThemePresetSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressEvents) return;

        const int index = ThemePresetComboBox().SelectedIndex();
        const auto& presets = Mandelbrot::GetAllPresets();
        if (index >= 0 && index < static_cast<int>(presets.size()))
        {
            m_theme.SetPreset(presets[index]);
            UpdateGradientPreview();
            PopulateStopSelector();
            RecolorBuffer();
        }
    }

    void MainWindow::OnFrequencySliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        m_frequency = static_cast<float>(e.NewValue());
        if (FrequencyValueText())
        {
            const int decimals = (m_frequency < 0.01f) ? 4 : ((m_frequency < 0.1f) ? 3 : 2);
            FrequencyValueText().Text(FormatFloat(m_frequency, decimals) + L"x");
        }

        if (m_suppressEvents) return;
        RecolorBuffer();
    }

    void MainWindow::OnPhaseSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        m_phaseOffset = static_cast<float>(e.NewValue());
        if (PhaseValueText())
        {
            PhaseValueText().Text(FormatFloat(m_phaseOffset, 2));
        }

        if (m_suppressEvents) return;
        RecolorBuffer();
    }

    void MainWindow::OnAnimateToggleClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (AnimateToggle().IsChecked().GetBoolean())
        {
            m_animateTimer.Start();
        }
        else
        {
            m_animateTimer.Stop();
        }
    }

    void MainWindow::OnInvertChecked(IInspectable const&, RoutedEventArgs const&)
    {
        m_invertColors = true;
        RecolorBuffer();
    }

    void MainWindow::OnInvertUnchecked(IInspectable const&, RoutedEventArgs const&)
    {
        m_invertColors = false;
        RecolorBuffer();
    }

    void MainWindow::PopulateStopSelector()
    {
        m_suppressEvents = true;

        StopSelectorComboBox().Items().Clear();
        const auto& stops = m_theme.Stops();
        for (size_t i = 0; i < stops.size(); ++i)
        {
            std::wostringstream ss;
            ss << L"Stop " << (i + 1) << L" (" << static_cast<int>(stops[i].position * 100.0f) << L"%)";
            StopSelectorComboBox().Items().Append(box_value(ss.str()));
        }

        if (!stops.empty())
        {
            StopSelectorComboBox().SelectedIndex(0);
        }

        m_suppressEvents = false;

        UpdateStopControlsFromSelected();
    }

    void MainWindow::UpdateStopControlsFromSelected()
    {
        const int index = StopSelectorComboBox().SelectedIndex();
        const auto& stops = m_theme.Stops();
        if (index < 0 || index >= static_cast<int>(stops.size())) return;

        const auto& stop = stops[index];

        m_suppressEvents = true;
        StopPositionSlider().Value(stop.position);
        StopPositionText().Text(FormatFloat(stop.position, 2));

        StopRedSlider().Value(stop.r);
        StopRedText().Text(winrt::to_hstring(stop.r));

        StopGreenSlider().Value(stop.g);
        StopGreenText().Text(winrt::to_hstring(stop.g));

        StopBlueSlider().Value(stop.b);
        StopBlueText().Text(winrt::to_hstring(stop.b));

        // Update swatch preview rectangle
        winrt::Windows::UI::Color color;
        color.A = 255;
        color.R = stop.r;
        color.G = stop.g;
        color.B = stop.b;
        StopColorSwatch().Fill(Media::SolidColorBrush(color));

        m_suppressEvents = false;
    }

    void MainWindow::OnStopSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressEvents) return;
        UpdateStopControlsFromSelected();
    }

    void MainWindow::OnAddStopClick(IInspectable const&, RoutedEventArgs const&)
    {
        // Add stop at 0.5 or between stops
        m_theme.AddStop(0.5f, 255, 180, 0); // Vibrant warm gold accent
        UpdateGradientPreview();
        PopulateStopSelector();
        StopSelectorComboBox().SelectedIndex(static_cast<int>(m_theme.Stops().size() - 1));
        RecolorBuffer();
    }

    void MainWindow::OnRemoveStopClick(IInspectable const&, RoutedEventArgs const&)
    {
        const int index = StopSelectorComboBox().SelectedIndex();
        if (index >= 0 && m_theme.Stops().size() > 2)
        {
            m_theme.RemoveStop(index);
            UpdateGradientPreview();
            PopulateStopSelector();
            RecolorBuffer();
        }
    }

    void MainWindow::OnStopPositionSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        const float pos = static_cast<float>(e.NewValue());
        if (StopPositionText())
        {
            StopPositionText().Text(FormatFloat(pos, 2));
        }

        if (m_suppressEvents) return;
        const int index = StopSelectorComboBox().SelectedIndex();
        if (index >= 0 && index < static_cast<int>(m_theme.Stops().size()))
        {
            auto stop = m_theme.Stops()[index];
            m_theme.UpdateStop(index, pos, stop.r, stop.g, stop.b);
            UpdateGradientPreview();
            RecolorBuffer();
        }
    }

    void MainWindow::OnStopColorChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const&)
    {
        const uint8_t r = static_cast<uint8_t>(StopRedSlider().Value());
        const uint8_t g = static_cast<uint8_t>(StopGreenSlider().Value());
        const uint8_t b = static_cast<uint8_t>(StopBlueSlider().Value());

        StopRedText().Text(winrt::to_hstring(r));
        StopGreenText().Text(winrt::to_hstring(g));
        StopBlueText().Text(winrt::to_hstring(b));

        winrt::Windows::UI::Color color;
        color.A = 255;
        color.R = r;
        color.G = g;
        color.B = b;
        StopColorSwatch().Fill(Media::SolidColorBrush(color));

        if (m_suppressEvents) return;
        const int index = StopSelectorComboBox().SelectedIndex();
        if (index >= 0 && index < static_cast<int>(m_theme.Stops().size()))
        {
            const float pos = m_theme.Stops()[index].position;
            m_theme.UpdateStop(index, pos, r, g, b);
            UpdateGradientPreview();
            RecolorBuffer();
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::OnExportImageClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_bitmap || m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) co_return;

        try
        {
            auto picker = winrt::Windows::Storage::Pickers::FileSavePicker();
            picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
            picker.FileTypeChoices().Insert(L"PNG Image (*.png)", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));
            picker.SuggestedFileName(winrt::hstring(Mandelbrot::GetFractalTypeName(m_config.fractalType) + L"_Capture.png"));

            // WinUI 3 Desktop requires HWND initialization for FileSavePicker
            HWND hwnd = nullptr;
            winrt::check_hresult(this->try_as<IWindowNative>()->get_WindowHandle(&hwnd));
            auto initializeWithWindow = picker.as<IInitializeWithWindow>();
            initializeWithWindow->Initialize(hwnd);

            auto file = co_await picker.PickSaveFileAsync();
            if (file)
            {
                auto stream = co_await file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite);
                auto encoder = co_await winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
                    winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);

                auto buffer = m_bitmap.PixelBuffer();
                auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
                uint8_t* pRaw = nullptr;
                if (SUCCEEDED(byteAccess->Buffer(&pRaw)) && pRaw)
                {
                    encoder.SetPixelData(
                        winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                        winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Straight,
                        static_cast<uint32_t>(m_currentPixelWidth),
                        static_cast<uint32_t>(m_currentPixelHeight),
                        96.0, 96.0,
                        winrt::array_view<const uint8_t>(pRaw, pRaw + (static_cast<size_t>(m_currentPixelWidth) * m_currentPixelHeight * 4)));

                    co_await encoder.FlushAsync();
                }
            }
        }
        catch (...)
        {
            // Silently handle picker cancellation
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::OnCopyCoordsClick(IInspectable const&, RoutedEventArgs const&)
    {
        std::wostringstream ss;
        ss << L"{\n"
           << L"  \"fractalType\": \"" << Mandelbrot::GetFractalTypeName(m_config.fractalType) << L"\",\n"
           << L"  \"centerX\": \"" << m_config.hpCenterX.ToWString(32) << L"\",\n"
           << L"  \"centerY\": \"" << m_config.hpCenterY.ToWString(32) << L"\",\n"
           << L"  \"zoom\": " << std::scientific << std::setprecision(6) << m_config.zoom << std::fixed << L",\n"
           << L"  \"maxIterations\": " << m_config.maxIterations << L",\n"
           << L"  \"smooth\": " << (m_config.smooth ? L"true" : L"false") << L",\n"
           << L"  \"frequency\": " << m_frequency << L",\n"
           << L"  \"phaseOffset\": " << m_phaseOffset << L",\n"
           << L"  \"theme\": \"" << Mandelbrot::GetPresetName(m_theme.CurrentPreset()) << L"\"\n"
           << L"}";

        auto package = winrt::Windows::ApplicationModel::DataTransfer::DataPackage();
        package.SetText(ss.str());
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);

        // Visual confirmation by updating Hud text briefly
        HudTimingText().Text(L"Coordinates copied to clipboard!");
        co_return;
    }
}
