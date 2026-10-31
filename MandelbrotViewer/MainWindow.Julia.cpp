#include "pch.h"
#include "MainWindow.xaml.h"
#include "JuliaView.h"
#include "ViewState.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Numerics;

namespace winrt::MandelbrotViewer::implementation
{
    void MainWindow::OnJuliaViewToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents || m_isClosed) return;
        m_juliaVisible = ShowJuliaToggle().IsOn() && m_config.fractalType == Mandelbrot::FractalType::Mandelbrot;
        JuliaPane().Visibility(m_juliaVisible ? Visibility::Visible : Visibility::Collapsed);
        JuliaSettingsPanel().Visibility(m_juliaVisible ? Visibility::Visible : Visibility::Collapsed);
        JuliaColumn().Width(GridLength{ m_juliaVisible ? 1.0 : 0.0, m_juliaVisible ? GridUnitType::Star : GridUnitType::Pixel });
        m_juliaDragging = false;
        JuliaSurface().ReleasePointerCaptures();
        if (!m_juliaVisible)
        {
            if (m_juliaRenderer) m_juliaRenderer->Cancel();
            m_juliaHasFrame = false;
            JuliaPointMarker().Visibility(Visibility::Collapsed);
            if (m_juliaMarkerAnimation) JuliaPointMarker().StopAnimation(m_juliaMarkerAnimation);
            m_juliaMarkerAnimation = nullptr;
            return;
        }
        if (!m_juliaRenderer)
        {
            auto weak = get_weak();
            auto dispatcher = DispatcherQueue();
            m_juliaRenderer = std::make_unique<Mandelbrot::BackgroundRenderer>([weak, dispatcher]
            {
                dispatcher.TryEnqueue([weak]
                {
                    if (auto window = weak.get()) window->OnJuliaRenderCompleted();
                });
            });
        }
        if (!m_juliaInitialized) OnJuliaUseCenterClick(nullptr, nullptr);
        else RenderJulia();
        UpdateJuliaMarker();
    }

    void MainWindow::OnJuliaCloseClick(IInspectable const&, RoutedEventArgs const&)
    {
        ShowJuliaToggle().IsOn(false);
    }

    void MainWindow::OnJuliaUseCenterClick(IInspectable const&, RoutedEventArgs const&)
    {
        const auto view = PresentationCamera();
        SelectJuliaParameter(view.hpCenterX, view.hpCenterY);
    }

    void MainWindow::SelectJuliaParameter(const Mandelbrot::BigFloat& x, const Mandelbrot::BigFloat& y)
    {
        m_juliaInitialized = true;
        m_juliaConfig.juliaX = x;
        m_juliaConfig.juliaY = y;
        Mandelbrot::ResetJuliaView(m_juliaConfig);
        JuliaParameterText().Text(L"c = " + hstring(x.ToWString(12)) + L" " + hstring(y.ToWString(12)) + L"i");
        UpdateJuliaMarker();
        RenderJulia();
    }

    void MainWindow::UpdateJuliaMarker()
    {
        if (!m_juliaVisible || !m_hasPresentation || !m_compositor) return;
        const auto [x, y] = Mandelbrot::ProjectPoint(m_cameraAnchor, m_juliaConfig.juliaX, m_juliaConfig.juliaY);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e6 || std::abs(y) > 1e6)
        {
            JuliaPointMarker().Visibility(Visibility::Collapsed);
            return;
        }
        if (!m_juliaMarkerProperties) m_juliaMarkerProperties = m_compositor.CreatePropertySet();
        m_juliaMarkerProperties.InsertVector3(L"Position", float3{ static_cast<float>(x), static_cast<float>(y), 0.0f });
        if (!m_juliaMarkerAnimation)
        {
            m_juliaMarkerAnimation = m_compositor.CreateExpressionAnimation(
                L"point.Position * camera.Scale + camera.Offset - Vector3(7.0, 7.0, 0.0)");
            m_juliaMarkerAnimation.Target(L"Translation");
            m_juliaMarkerAnimation.SetReferenceParameter(L"point", m_juliaMarkerProperties);
            m_juliaMarkerAnimation.SetReferenceParameter(L"camera", m_cameraProperties);
            JuliaPointMarker().StartAnimation(m_juliaMarkerAnimation);
        }
        JuliaPointMarker().Visibility(Visibility::Visible);
    }

    void MainWindow::OnJuliaViewportSizeChanged(IInspectable const&, SizeChangedEventArgs const& e)
    {
        if (!m_juliaVisible || e.NewSize().Width <= 0 || e.NewSize().Height <= 0) return;
        auto clip = Media::RectangleGeometry();
        clip.Rect(Rect{ 0, 0, e.NewSize().Width, e.NewSize().Height });
        JuliaSurface().Clip(clip);
        const int width = std::max(64, static_cast<int>(std::ceil(e.NewSize().Width)));
        const int height = std::max(64, static_cast<int>(std::ceil(e.NewSize().Height)));
        if (m_juliaSized && m_juliaConfig.width == width && m_juliaConfig.height == height) return;
        m_juliaConfig.width = width;
        m_juliaConfig.height = height;
        JuliaImage().Width(width);
        JuliaImage().Height(height);
        if (!m_juliaSized) Mandelbrot::ResetJuliaView(m_juliaConfig);
        m_juliaSized = true;
        RenderJulia();
    }

    void MainWindow::RefreshJuliaSettings()
    {
        if (!m_juliaVisible) return;
        const int iterations = m_autoScaleIterations
            ? Mandelbrot::MandelbrotEngine::RecommendMaxIterations(m_juliaConfig.zoom, Mandelbrot::FractalType::Mandelbrot, m_baseIterations)
            : m_config.maxIterations;
        if (m_juliaConfig.maxIterations != iterations || m_juliaConfig.smooth != m_config.smooth) RenderJulia();
    }

    void MainWindow::RenderJulia()
    {
        if (!m_juliaVisible || m_isClosed || !m_juliaRenderer || !m_juliaSized) return;
        m_juliaConfig.maxIterations = m_autoScaleIterations
            ? Mandelbrot::MandelbrotEngine::RecommendMaxIterations(m_juliaConfig.zoom, Mandelbrot::FractalType::Mandelbrot, m_baseIterations)
            : m_config.maxIterations;
        m_juliaConfig.smooth = m_config.smooth;
        m_juliaHasFrame = false;
        JuliaTimingText().Text(L"Rendering...");
        m_juliaRenderer->Submit(m_juliaConfig);
    }

    void MainWindow::OnJuliaRenderCompleted()
    {
        if (!m_juliaVisible || m_isClosed || !m_juliaRenderer) return;
        auto result = m_juliaRenderer->TakeCompleted(m_juliaIterations, {}, &m_juliaSamples);
        if (!result) return;
        if (result->error)
        {
            m_juliaRenderer->Cancel();
            JuliaTimingText().Text(L"Rendering failed. Try another point or fewer iterations.");
            return;
        }
        m_juliaDisplayedConfig = result->config;
        if (!m_juliaBitmap || m_juliaBitmap.PixelWidth() != result->config.width || m_juliaBitmap.PixelHeight() != result->config.height)
        {
            m_juliaBitmap = WriteableBitmap(result->config.width, result->config.height);
            JuliaImage().Source(m_juliaBitmap);
        }
        RecolorBitmap(m_juliaBitmap, m_juliaDisplayedConfig, m_juliaIterations, m_juliaSamples);
        m_juliaHasFrame = true;
        std::wostringstream zoom, timing;
        zoom << L"Zoom: " << std::setprecision(3) << result->config.zoom << L"x";
        zoom << L"\nCenter: (" << result->config.hpCenterX.ToWString(10) << L", " << result->config.hpCenterY.ToWString(10) << L")";
        timing << std::fixed << std::setprecision(1) << L"Fractal: " << result->elapsedMs << L" ms | Iter: " << result->config.maxIterations;
        JuliaZoomText().Text(zoom.str());
        JuliaTimingText().Text(timing.str());
    }

    void MainWindow::ApplyJuliaZoom(double factor, double x, double y)
    {
        if (!m_juliaVisible) return;
        Mandelbrot::MandelbrotEngine::ZoomViewport(m_juliaConfig, factor, x, y);
        m_juliaConfig.renderStride = 1;
        RenderJulia();
    }

    void MainWindow::OnJuliaPointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        const auto point = e.GetCurrentPoint(JuliaSurface());
        if (m_juliaDragging || !point.Properties().IsLeftButtonPressed() || !JuliaSurface().CapturePointer(e.Pointer())) return;
        m_juliaDragging = true;
        m_juliaDragPointerId = e.Pointer().PointerId();
        m_juliaDragStart = point.Position();
        m_juliaDragCenterX = m_juliaConfig.hpCenterX;
        m_juliaDragCenterY = m_juliaConfig.hpCenterY;
        e.Handled(true);
    }

    void MainWindow::OnJuliaPointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_juliaDragging || e.Pointer().PointerId() != m_juliaDragPointerId) return;
        const auto point = e.GetCurrentPoint(JuliaSurface()).Position();
        if (point.X == m_juliaDragStart.X && point.Y == m_juliaDragStart.Y &&
            m_juliaConfig.hpCenterX == m_juliaDragCenterX && m_juliaConfig.hpCenterY == m_juliaDragCenterY) return;
        Mandelbrot::MandelbrotEngine::PanViewport(m_juliaConfig, m_juliaDragCenterX, m_juliaDragCenterY,
            point.X - m_juliaDragStart.X, point.Y - m_juliaDragStart.Y);
        m_juliaConfig.renderStride = 2;
        RenderJulia();
        e.Handled(true);
    }

    void MainWindow::OnJuliaPointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_juliaDragging || e.Pointer().PointerId() != m_juliaDragPointerId) return;
        m_juliaDragging = false;
        JuliaSurface().ReleasePointerCapture(e.Pointer());
        if (m_juliaConfig.renderStride != 1)
        {
            m_juliaConfig.renderStride = 1;
            RenderJulia();
        }
    }

    void MainWindow::OnJuliaPointerWheelChanged(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        const auto point = e.GetCurrentPoint(JuliaSurface());
        const int delta = point.Properties().MouseWheelDelta();
        if (delta == 0) return;
        ApplyJuliaZoom(delta > 0 ? 1.25 : 0.8, point.Position().X, point.Position().Y);
        e.Handled(true);
    }

    void MainWindow::OnJuliaDoubleTapped(IInspectable const&, Input::DoubleTappedRoutedEventArgs const& e)
    {
        const auto point = e.GetPosition(JuliaSurface());
        ApplyJuliaZoom(2.0, point.X, point.Y);
    }

    void MainWindow::OnJuliaZoomInClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyJuliaZoom(1.25, (m_juliaConfig.width - 1) * 0.5, (m_juliaConfig.height - 1) * 0.5);
    }

    void MainWindow::OnJuliaZoomOutClick(IInspectable const&, RoutedEventArgs const&)
    {
        ApplyJuliaZoom(0.8, (m_juliaConfig.width - 1) * 0.5, (m_juliaConfig.height - 1) * 0.5);
    }

    void MainWindow::OnJuliaResetClick(IInspectable const&, RoutedEventArgs const&)
    {
        Mandelbrot::ResetJuliaView(m_juliaConfig);
        RenderJulia();
    }

    IAsyncAction MainWindow::OnJuliaExportClick(IInspectable const&, RoutedEventArgs const&)
    {
        co_await ExportViewport(true);
    }

    IAsyncAction MainWindow::OnJuliaCopyCoordsClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_juliaHasFrame) co_return;
        auto package = Windows::ApplicationModel::DataTransfer::DataPackage();
        package.SetText(Mandelbrot::SerializeViewState(m_juliaDisplayedConfig, m_theme,
            m_frequency, m_phaseOffset, m_invertColors, m_interiorColor));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        JuliaTimingText().Text(L"Julia coordinates and parameter copied.");
        co_return;
    }
}
