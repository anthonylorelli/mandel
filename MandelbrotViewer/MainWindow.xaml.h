#pragma once

#include "MainWindow.g.h"
#include "MandelbrotEngine.h"
#include "ColorTheme.h"
#include <chrono>

namespace winrt::MandelbrotViewer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        // Viewport interaction
        void OnViewportSizeChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e);
        void OnViewportPointerPressed(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnViewportPointerMoved(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnViewportPointerReleased(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnViewportPointerWheelChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnViewportDoubleTapped(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& e);

        // Overlay & HUD buttons
        void OnOverlayZoomToggleClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnZoomInClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnZoomOutClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnResetViewClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Animated Zoom controls
        void OnUseCurrentCenterClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnZoomDirectionInClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnZoomDirectionOutClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnZoomSpeedSliderChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
        void OnStartZoomClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnStopZoomClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Sidebar panel visibility
        void OnCloseSidebarClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnOpenSidebarClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnToggleSidebarClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Full screen mode & keyboard shortcuts
        void OnToggleFullScreenClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnFullScreenAcceleratorInvoked(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender, winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e);
        void OnEscapeAcceleratorInvoked(winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender, winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e);

        // Sidebar parameters
        void OnFractalTypeSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnBookmarkSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnIterationsSliderChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
        void OnAutoIterationsToggleChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSmoothToggleChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Color theme & live tuning
        void OnThemePresetSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnFrequencySliderChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
        void OnPhaseSliderChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
        void OnAnimateToggleClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnInvertChecked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnInvertUnchecked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        // Custom Gradient Editor
        void OnStopSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnAddStopClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnRemoveStopClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnStopPositionSliderChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
        void OnStopColorChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);

        // Export & Share
        winrt::Windows::Foundation::IAsyncAction OnExportImageClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::Windows::Foundation::IAsyncAction OnCopyCoordsClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void InitializeControls();
        void PopulateBookmarks();
        void RenderFractal();
        void RecolorBuffer();
        void UpdateHud(double fractalTimeMs, double colorTimeMs);
        void UpdateGradientPreview();
        void PopulateStopSelector();
        void UpdateStopControlsFromSelected();
        void ApplyZoom(double factor, double centerPivotX, double centerPivotY);
        void SyncTargetInputsFromCenter();
        void StartZoomAnimation();
        void StopZoomAnimation();
        void SetZoomDirection(bool zoomIn);
        void SetSidebarVisible(bool visible);
        void SetFullScreen(bool fullScreen);
        bool IsFullScreen();

        Mandelbrot::MandelbrotEngine m_engine;
        Mandelbrot::ColorTheme m_theme;
        Mandelbrot::MandelbrotConfig m_config;
        std::vector<float> m_iterations;

        winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap m_bitmap{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap m_previewBitmap{ nullptr };
        winrt::Microsoft::UI::Xaml::DispatcherTimer m_animateTimer{ nullptr };
        winrt::Microsoft::UI::Xaml::DispatcherTimer m_zoomTimer{ nullptr };

        bool m_isZoomAnimating{ false };
        bool m_zoomInDirection{ true };
        double m_zoomSpeed{ 1.0 };
        int m_animQualityLevel{ 0 };
        int m_slowFrameStreak{ 0 };
        int m_fastFrameStreak{ 0 };
        double m_lastFractalTimeMs{ 0.0 };
        Mandelbrot::BigFloat m_zoomTargetX{ -0.75 };
        Mandelbrot::BigFloat m_zoomTargetY{ 0.0 };
        std::chrono::steady_clock::time_point m_lastZoomFrameTime{};

        bool m_isDragging{ false };
        winrt::Windows::Foundation::Point m_dragStartPoint{ 0, 0 };
        Mandelbrot::BigFloat m_dragStartHpCenterX{ -0.75 };
        Mandelbrot::BigFloat m_dragStartHpCenterY{ 0.0 };

        bool m_autoScaleIterations{ true };
        int m_baseIterations{ 500 };

        bool m_isSidebarVisible{ true };
        bool m_sidebarVisibleBeforeFullScreen{ true };
        bool m_suppressEvents{ true };
        float m_frequency{ 1.0f };
        float m_phaseOffset{ 0.0f };
        bool m_invertColors{ false };
        uint32_t m_interiorColor{ 0xFF000000 };
        int m_currentPixelWidth{ 800 };
        int m_currentPixelHeight{ 600 };
    };
}

namespace winrt::MandelbrotViewer::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
