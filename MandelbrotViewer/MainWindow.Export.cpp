#include "pch.h"
#include "MainWindow.xaml.h"
#include "FramePresentation.h"
#include "ViewState.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::MandelbrotViewer::implementation
{
    winrt::Windows::Foundation::IAsyncAction MainWindow::OnExportImageClick(IInspectable const&, RoutedEventArgs const&)
    {
        co_await ExportViewport(false);
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::ExportViewport(bool julia)
    {
        if (m_imageExporting || (julia ? !m_juliaHasFrame : (!m_bitmap || !m_hasPresentation))) co_return;
        auto lifetime = get_strong();
        m_imageExporting = true;
        struct ExportFlagGuard { bool& flag; ~ExportFlagGuard() { flag = false; } } exportGuard{ m_imageExporting };

        try
        {
            // Snapshot before the first suspension: animation and resizing can replace the live bitmap.
            const auto view = julia ? m_juliaDisplayedConfig : PresentationCamera();
            const auto frame = julia ? m_juliaDisplayedConfig : m_displayedConfig;
            const auto bitmap = julia ? m_juliaBitmap : m_bitmap;
            const uint32_t width = static_cast<uint32_t>(view.width);
            const uint32_t height = static_cast<uint32_t>(view.height);
            auto buffer = bitmap.PixelBuffer();
            auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
            uint8_t* pRaw = nullptr;
            winrt::check_hresult(byteAccess->Buffer(&pRaw));
            if (!pRaw) throw winrt::hresult_error(E_FAIL, L"Unable to read the viewport image.");
            const auto pixels = Mandelbrot::SnapshotViewport(pRaw, frame, view);

            auto picker = winrt::Windows::Storage::Pickers::FileSavePicker();
            picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
            picker.FileTypeChoices().Insert(L"PNG Image (*.png)", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));
            picker.SuggestedFileName(julia ? L"Julia_Set_Capture.png" : winrt::hstring(Mandelbrot::GetFractalTypeName(frame.fractalType) + L"_Capture.png"));

            // WinUI 3 Desktop requires HWND initialization for FileSavePicker
            HWND hwnd = nullptr;
            auto windowNative = try_as<IWindowNative>();
            if (!windowNative) throw winrt::hresult_no_interface();
            winrt::check_hresult(windowNative->get_WindowHandle(&hwnd));
            auto initializeWithWindow = picker.as<IInitializeWithWindow>();
            winrt::check_hresult(initializeWithWindow->Initialize(hwnd));

            auto file = co_await picker.PickSaveFileAsync();
            if (file)
            {
                auto stream = co_await file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite);
                stream.Size(0);
                auto encoder = co_await winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
                    winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);

                encoder.SetPixelData(
                    winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                    winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Straight,
                    width, height, 96.0, 96.0, pixels);

                co_await encoder.FlushAsync();
                if (!m_isClosed) (julia ? JuliaTimingText() : HudTimingText()).Text(L"Viewport image exported.");
            }
        }
        catch (const winrt::hresult_error& error)
        {
            // Picker cancellation returns a null file; actual export errors need visible feedback.
            if (!m_isClosed) (julia ? JuliaTimingText() : HudTimingText()).Text(L"Image export failed: " + error.message());
        }
    }

    void MainWindow::OnCopyCoordsClick(IInspectable const&, RoutedEventArgs const&)
    {
        CopyTextToClipboard(
            Mandelbrot::SerializeViewState(
                PresentationCamera(), m_theme, m_frequency, m_phaseOffset, m_invertColors, m_interiorColor, m_lighting),
            HudTimingText(),
            L"Coordinates copied to clipboard!");
    }
}
