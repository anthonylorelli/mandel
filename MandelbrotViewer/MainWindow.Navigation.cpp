#include "pch.h"
#include "MainWindow.xaml.h"
#include "FramePresentation.h"
#include "FrameTiming.h"
#include "JuliaView.h"
#include "ViewState.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::MandelbrotViewer::implementation
{
    void MainWindow::ApplyZoom(double factor, double pivotX, double pivotY)
    {
        if (m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;
        if (m_isZoomAnimating) StopZoomAnimation(false);

        Mandelbrot::MandelbrotEngine::ZoomViewport(m_config, factor, pivotX, pivotY);
        SyncTargetInputsFromCenter();

        if (m_autoScaleIterations)
        {
            const int recommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_baseIterations);
            m_config.maxIterations = recommended;
            SyncMaxIterationsUi(recommended);
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

        const size_t decimals = Mandelbrot::CoordinateDecimalPlaces(m_config.zoom);
        ScopedEventSuppress suppress(m_suppressEventDepth);
        m_targetXText = winrt::hstring(m_zoomTargetX.ToWString(decimals));
        m_targetYText = winrt::hstring(m_zoomTargetY.ToWString(decimals));
        TargetXTextBox().Text(m_targetXText);
        TargetYTextBox().Text(m_targetYText);
    }

    void MainWindow::PickAndApplyRandomTarget()
    {
        if (!m_hasCurrentFrame || m_iterations.empty())
        {
            m_zoomTargetX = m_config.hpCenterX;
            m_zoomTargetY = m_config.hpCenterY;
            m_hasActiveRandomTarget = false;
            return;
        }

        m_hasActiveRandomTarget = Mandelbrot::MandelbrotEngine::PickInterestingTarget(
            m_displayedConfig, m_iterations, m_zoomTargetX, m_zoomTargetY);
        m_retargetPanProgress = 0.0;
        m_framesSinceRetarget = 0;
        UpdateTargetTextBoxes();
    }

    void MainWindow::SetZoomDirection(bool zoomIn)
    {
        m_zoomInDirection = zoomIn;
        if (!ZoomDirectionInToggle() || !ZoomDirectionOutToggle()) return;

        ScopedEventSuppress suppress(m_suppressEventDepth);
        ZoomDirectionInToggle().IsChecked(zoomIn);
        ZoomDirectionOutToggle().IsChecked(!zoomIn);
    }

    void MainWindow::StartZoomAnimation()
    {
        if (m_movieExporter || m_movieSetup) return;
        StopJuliaTravel(false);
        if (m_autoPickInterestingTarget && m_zoomInDirection)
        {
            if (!m_hasCurrentFrame || !m_hasActiveRandomTarget ||
                Mandelbrot::MandelbrotEngine::ShouldRetargetZoom(m_displayedConfig, m_iterations, m_zoomTargetX, m_zoomTargetY))
            {
                PickAndApplyRandomTarget();
            }
        }
        else if (TargetXTextBox() && TargetYTextBox())
        {
            // Preserve the exact target when the user has not edited its formatted text.
            Mandelbrot::BigFloat parsedX = m_zoomTargetX;
            Mandelbrot::BigFloat parsedY = m_zoomTargetY;
            const bool validX = TargetXTextBox().Text() == m_targetXText ||
                Mandelbrot::BigFloat::TryParse(TargetXTextBox().Text(), parsedX);
            const bool validY = TargetYTextBox().Text() == m_targetYText ||
                Mandelbrot::BigFloat::TryParse(TargetYTextBox().Text(), parsedY);

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

        // If already at the zoom bound in the chosen direction, return.
        if ((m_zoomInDirection && m_config.zoom >= Mandelbrot::MandelbrotEngine::MaxZoom) ||
            (!m_zoomInDirection && m_config.zoom <= Mandelbrot::MandelbrotEngine::MinZoom))
        {
            return;
        }

        m_isZoomAnimating = true;
        m_lastZoomFrameTime = std::chrono::steady_clock::now();
        RenderFractal(true);

        if (StartZoomButton()) StartZoomButton().IsEnabled(false);
        if (StopZoomButton()) StopZoomButton().IsEnabled(true);
        UpdatePlayStopVisual(
            OverlayPlayZoomIcon(), OverlayPlayZoomButton(), true,
            L"Start Animated Zoom", L"Stop Animated Zoom");

        if (m_zoomTimer)
        {
            m_zoomTimer.Interval(Mandelbrot::ZoomFrameDelay(std::chrono::steady_clock::duration::zero()));
            m_zoomTimer.Start();
        }
    }

    void MainWindow::StopZoomAnimation(bool renderFinalFrame)
    {
        if (m_isZoomAnimating && m_hasPresentation)
        {
            const auto view = PresentationCamera();
            m_config.SetCenter(view.hpCenterX, view.hpCenterY);
            m_config.zoom = view.zoom;
        }
        FreezePresentation();
        m_isZoomAnimating = false;
        m_config.renderStride = 1;
        m_config.edgeRefine = true;
        if (m_zoomTimer)
        {
            m_zoomTimer.Stop();
        }

        if (StartZoomButton()) StartZoomButton().IsEnabled(true);
        if (StopZoomButton()) StopZoomButton().IsEnabled(false);
        UpdatePlayStopVisual(
            OverlayPlayZoomIcon(), OverlayPlayZoomButton(), false,
            L"Start Animated Zoom", L"Stop Animated Zoom");

        if (m_autoScaleIterations)
        {
            const int recommended = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                m_config.zoom, m_config.fractalType, m_baseIterations);
            m_config.maxIterations = recommended;
            SyncMaxIterationsUi(recommended);
        }

        if (!m_autoPickInterestingTarget || !m_hasActiveRandomTarget)
        {
            SyncTargetInputsFromCenter();
        }
        else
        {
            UpdateTargetTextBoxes();
        }
        if (renderFinalFrame) RenderFractal();
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
        if (IsSuppressingEvents() || !RandomTargetToggle()) return;
        m_autoPickInterestingTarget = RandomTargetToggle().IsOn();
        m_hasActiveRandomTarget = false;

        if (m_autoPickInterestingTarget && m_isZoomAnimating && m_zoomInDirection)
        {
            PickAndApplyRandomTarget();
        }
    }

    void MainWindow::OnZoomDirectionInClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        SetZoomDirection(true);
    }

    void MainWindow::OnZoomDirectionOutClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        SetZoomDirection(false);
    }

    void MainWindow::OnZoomSpeedSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        m_zoomSpeed = e.NewValue();
        if (ZoomSpeedValueText())
        {
            ZoomSpeedValueText().Text(FormatFloat(m_zoomSpeed, 1) + L"x");
        }
    }

    void MainWindow::OnStartZoomClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        StartZoomAnimation();
    }

    void MainWindow::OnStopZoomClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        StopZoomAnimation();
    }

    void MainWindow::OnViewportPointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        const auto point = e.GetCurrentPoint(FractalSurface());
        if (m_isDragging || !point.Properties().IsLeftButtonPressed()) return;
        if (m_isZoomAnimating) StopZoomAnimation();
        if (!FractalSurface().CapturePointer(e.Pointer())) return;
        m_isDragging = true;
        m_dragHasMoved = false;
        m_dragPointerId = e.Pointer().PointerId();
        m_dragStartPoint = point.Position();
        m_dragStartHpCenterX = m_config.hpCenterX;
        m_dragStartHpCenterY = m_config.hpCenterY;
        e.Handled(true);
    }

    void MainWindow::OnViewportPointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_isDragging || e.Pointer().PointerId() != m_dragPointerId ||
            m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;

        const auto currentPoint = e.GetCurrentPoint(FractalSurface()).Position();
        const double dx = currentPoint.X - m_dragStartPoint.X;
        const double dy = currentPoint.Y - m_dragStartPoint.Y;
        if (!m_dragHasMoved && dx * dx + dy * dy < 9.0) return;
        m_dragHasMoved = true;

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
        if (m_isDragging && e.Pointer().PointerId() == m_dragPointerId)
        {
            const auto point = e.GetCurrentPoint(FractalSurface());
            const bool selectJulia = m_juliaVisible && !m_dragHasMoved && m_hasPresentation &&
                point.Properties().PointerUpdateKind() == Microsoft::UI::Input::PointerUpdateKind::LeftButtonReleased;
            m_isDragging = false;
            FractalSurface().ReleasePointerCapture(e.Pointer());
            if (selectJulia)
            {
                const auto [x, y] = Mandelbrot::ViewportPoint(PresentationCamera(), point.Position().X, point.Position().Y);
                SelectJuliaParameter(x, y);
            }

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
        const int delta = e.GetCurrentPoint(FractalSurface()).Properties().MouseWheelDelta();
        if (delta == 0) return;
        const auto pos = e.GetCurrentPoint(FractalSurface()).Position();
        const double factor = (delta > 0) ? 1.25 : 0.8;
        ApplyZoom(factor, pos.X, pos.Y);
        e.Handled(true);
    }

    void MainWindow::OnViewportDoubleTapped(IInspectable const&, Input::DoubleTappedRoutedEventArgs const& e)
    {
        const auto pos = e.GetPosition(FractalSurface());
        ApplyZoom(2.0, pos.X, pos.Y);
    }

    void MainWindow::OnZoomInClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyZoom(1.25, (m_currentPixelWidth - 1) * 0.5, (m_currentPixelHeight - 1) * 0.5);
    }

    void MainWindow::OnZoomOutClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyZoom(0.8, (m_currentPixelWidth - 1) * 0.5, (m_currentPixelHeight - 1) * 0.5);
    }

    void MainWindow::OnResetViewClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isZoomAnimating)
        {
            StopZoomAnimation(false);
        }

        const auto& bookmarks = Mandelbrot::GetBookmarks(m_config.fractalType);
        if (!bookmarks.empty())
        {
            ApplyBookmark(bookmarks[0], true, 0);
        }
        else
        {
            m_config.SetCenter(-0.75, 0.0);
            m_config.zoom = 1.0;
            m_baseIterations = 500;
            m_config.maxIterations = 500;
            SyncMaxIterationsUi(500);
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

    void MainWindow::OnAppThemeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (IsSuppressingEvents() || !AppThemeComboBox()) return;
        const int index = AppThemeComboBox().SelectedIndex();
        const auto themes = Mandelbrot::GetAllAppThemes();
        if (index >= 0 && index < static_cast<int>(themes.size()))
        {
            ApplyAppTheme(themes[index]);
        }
    }

    void MainWindow::OnThemeCycleClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyAppTheme(Mandelbrot::CycleAppTheme(m_appTheme));
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
            SyncWindowThemeVisuals();
        }
    }

    void MainWindow::OnToggleFullScreenClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetFullScreen(!IsFullScreen());
    }

    void MainWindow::OnFullScreenAcceleratorInvoked(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        if (m_movieSetup || m_movieExporter) return;
        SetFullScreen(!IsFullScreen());
        e.Handled(true);
    }

    void MainWindow::OnEscapeAcceleratorInvoked(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        if (m_movieSetup) return;
        if (m_movieExporter) { OnMovieCancelClick(nullptr, nullptr); e.Handled(true); return; }
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
}
