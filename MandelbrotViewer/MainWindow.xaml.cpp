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
        Title(L"Mandelbrot Set Explorer - WinUI 3");

        InitializeControls();
    }

    void MainWindow::InitializeControls()
    {
        m_suppressEvents = true;

        // Initialize preview bitmap (256 x 1)
        m_previewBitmap = WriteableBitmap(256, 1);
        PalettePreviewImage().Source(m_previewBitmap);

        // Populate Landmark Bookmarks
        const auto& bookmarks = Mandelbrot::GetBookmarks();
        for (const auto& bm : bookmarks)
        {
            BookmarksComboBox().Items().Append(box_value(bm.name));
        }
        BookmarksComboBox().SelectedIndex(0);

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

        // Populate custom gradient stops UI
        PopulateStopSelector();

        m_suppressEvents = false;

        UpdateGradientPreview();
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
            m_interiorColor);

        m_bitmap.Invalidate();
    }

    void MainWindow::UpdateHud(double fractalTimeMs, double colorTimeMs)
    {
        std::wostringstream coords;
        coords << L"Center: (" << FormatCoordinate(m_config.centerX).c_str()
               << L", " << FormatCoordinate(m_config.centerY).c_str() << L")";
        HudCoordsText().Text(coords.str());

        HudZoomText().Text(L"Zoom: " + FormatZoom(m_config.zoom));

        std::wostringstream timing;
        timing << L"Fractal: " << std::fixed << std::setprecision(1) << fractalTimeMs
               << L" ms | Color: " << std::fixed << std::setprecision(1) << colorTimeMs << L" ms";
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

        double cursorCx = 0.0, cursorCy = 0.0;
        Mandelbrot::MandelbrotEngine::ScreenToComplex(
            pivotX, pivotY,
            m_currentPixelWidth, m_currentPixelHeight,
            m_config.centerX, m_config.centerY, m_config.zoom,
            cursorCx, cursorCy);

        m_config.zoom *= factor;

        // Keep the complex coordinate at cursor fixed
        m_config.centerX = cursorCx + (m_config.centerX - cursorCx) / factor;
        m_config.centerY = cursorCy + (m_config.centerY - cursorCy) / factor;

        RenderFractal();
    }

    void MainWindow::OnViewportPointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        m_isDragging = true;
        m_dragStartPoint = e.GetCurrentPoint(FractalImage()).Position();
        m_dragStartCenterX = m_config.centerX;
        m_dragStartCenterY = m_config.centerY;
        FractalImage().CapturePointer(e.Pointer());
    }

    void MainWindow::OnViewportPointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_isDragging || m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;

        const auto currentPoint = e.GetCurrentPoint(FractalImage()).Position();
        const double dx = currentPoint.X - m_dragStartPoint.X;
        const double dy = currentPoint.Y - m_dragStartPoint.Y;

        const double aspect = static_cast<double>(m_currentPixelHeight) / static_cast<double>(m_currentPixelWidth);
        const double spanX = 3.0 / m_config.zoom;
        const double spanY = spanX * aspect;

        m_config.centerX = m_dragStartCenterX - (dx / static_cast<double>(m_currentPixelWidth)) * spanX;
        m_config.centerY = m_dragStartCenterY + (dy / static_cast<double>(m_currentPixelHeight)) * spanY;

        RenderFractal();
    }

    void MainWindow::OnViewportPointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (m_isDragging)
        {
            m_isDragging = false;
            FractalImage().ReleasePointerCapture(e.Pointer());
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
        m_config.centerX = -0.75;
        m_config.centerY = 0.0;
        m_config.zoom = 1.0;
        m_config.maxIterations = 500;

        m_suppressEvents = true;
        IterationsSlider().Value(500);
        IterationsValueText().Text(L"500");
        BookmarksComboBox().SelectedIndex(0);
        m_suppressEvents = false;

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

    void MainWindow::OnBookmarkSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressEvents) return;

        const int index = BookmarksComboBox().SelectedIndex();
        const auto& bookmarks = Mandelbrot::GetBookmarks();
        if (index >= 0 && index < static_cast<int>(bookmarks.size()))
        {
            const auto& bm = bookmarks[index];
            m_config.centerX = bm.centerX;
            m_config.centerY = bm.centerY;
            m_config.zoom = bm.zoom;
            m_config.maxIterations = bm.maxIterations;

            m_suppressEvents = true;
            IterationsSlider().Value(bm.maxIterations);
            IterationsValueText().Text(winrt::to_hstring(bm.maxIterations));
            m_suppressEvents = false;

            RenderFractal();
        }
    }

    void MainWindow::OnIterationsSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        const int iter = static_cast<int>(e.NewValue());
        IterationsValueText().Text(winrt::to_hstring(iter));

        if (m_suppressEvents) return;
        m_config.maxIterations = iter;
        RenderFractal();
    }

    void MainWindow::OnSmoothToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEvents) return;
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
        FrequencyValueText().Text(FormatFloat(m_frequency, 2) + L"x");

        if (m_suppressEvents) return;
        RecolorBuffer();
    }

    void MainWindow::OnPhaseSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        m_phaseOffset = static_cast<float>(e.NewValue());
        PhaseValueText().Text(FormatFloat(m_phaseOffset, 2));

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
        StopPositionText().Text(FormatFloat(pos, 2));

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
            picker.SuggestedFileName(L"Mandelbrot_Capture.png");

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
           << L"  \"centerX\": " << std::setprecision(16) << m_config.centerX << L",\n"
           << L"  \"centerY\": " << std::setprecision(16) << m_config.centerY << L",\n"
           << L"  \"zoom\": " << std::setprecision(6) << m_config.zoom << L",\n"
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
