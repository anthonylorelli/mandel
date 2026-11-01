#include "pch.h"
#include "MainWindow.xaml.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#pragma comment(lib, "uuid.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;

namespace winrt::MandelbrotViewer::implementation
{
    namespace
    {
        hstring FootageTime(uint32_t frames, int fps)
        {
            const auto tenths = static_cast<uint64_t>(frames) * 10 / fps;
            std::wostringstream text;
            text << std::setfill(L'0') << std::setw(2) << tenths / 36000 << L":"
                << std::setw(2) << tenths / 600 % 60 << L":" << std::setw(2) << tenths / 10 % 60 << L"." << tenths % 10;
            return hstring(text.str());
        }
    }

    IAsyncAction MainWindow::OnExportMovieClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isClosed || m_movieExporter || m_movieSetup || !m_hasPresentation) co_return;
        auto lifetime = get_strong();
        m_movieSetup = true;
        ExportMovieButton().IsEnabled(false);
        try
        {
            Mandelbrot::MoviePlan plan;
            plan.start = PresentationCamera();
            plan.theme = m_theme; plan.frequency = m_frequency; plan.phase = m_phaseOffset;
            plan.invert = m_invertColors; plan.interior = m_interiorColor;
            const auto targetXText = TargetXTextBox().Text(), targetYText = TargetYTextBox().Text();
            const auto exactX = m_zoomTargetX, exactY = m_zoomTargetY;
            const bool exactXText = targetXText == m_targetXText, exactYText = targetYText == m_targetYText;
            if (m_isZoomAnimating) StopZoomAnimation(false);
            StopJuliaTravel(false);
            if (m_animateTimer) m_animateTimer.Stop();
            AnimateToggle().IsChecked(false);

            StackPanel settings; settings.Spacing(10);
            TextBlock help;
            help.Text(L"Starts from the current view and zooms to the target below. High-quality MP4 output supports 8K and can produce large files.");
            help.TextWrapping(TextWrapping::Wrap); settings.Children().Append(help);
            ComboBox resolution; resolution.Header(box_value(L"Resolution")); resolution.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (const auto& item : Mandelbrot::MovieResolutions) resolution.Items().Append(box_value(item.label));
            resolution.SelectedIndex(1); settings.Children().Append(resolution);
            ComboBox fps; fps.Header(box_value(L"Frames per second")); fps.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (auto value : Mandelbrot::MovieFrameRates) fps.Items().Append(box_value(to_hstring(value)));
            fps.SelectedIndex(2); settings.Children().Append(fps);
            NumberBox duration; duration.Header(box_value(L"Duration (seconds)")); duration.Minimum(1); duration.Maximum(3600);
            duration.Value(30); duration.SmallChange(1); duration.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
            settings.Children().Append(duration);
            TextBox endZoom; endZoom.Header(box_value(L"End zoom (x)"));
            std::wostringstream zoomText; zoomText << std::setprecision(12) << std::clamp(
                m_zoomInDirection ? plan.start.zoom * 1000.0 : plan.start.zoom / 1000.0,
                Mandelbrot::MandelbrotEngine::MinZoom, Mandelbrot::MandelbrotEngine::MaxZoom);
            endZoom.Text(zoomText.str()); settings.Children().Append(endZoom);
            TextBox targetX; targetX.Header(box_value(L"Target center X")); targetX.Text(targetXText); settings.Children().Append(targetX);
            TextBox targetY; targetY.Header(box_value(L"Target center Y")); targetY.Text(targetYText); settings.Children().Append(targetY);
            CheckBox motion; motion.Content(box_value(L"Smooth motion between frames")); motion.IsChecked(true); settings.Children().Append(motion);
            TextBlock errorText; errorText.TextWrapping(TextWrapping::Wrap); settings.Children().Append(errorText);
            ScrollViewer scroller; scroller.Content(settings); scroller.MaxHeight(510);
            ContentDialog dialog; dialog.XamlRoot(WindowRoot().XamlRoot()); dialog.Title(box_value(L"Export Zoom Movie"));
            dialog.Content(scroller); dialog.PrimaryButtonText(L"Choose File..."); dialog.CloseButtonText(L"Cancel");
            const bool autoIterations = m_autoScaleIterations;
            const int baseIterations = m_baseIterations;
            dialog.PrimaryButtonClick([&](auto&&, ContentDialogButtonClickEventArgs const& args)
            {
                try
                {
                    plan.width = Mandelbrot::MovieResolutions.at(resolution.SelectedIndex()).width;
                    plan.height = Mandelbrot::MovieResolutions.at(resolution.SelectedIndex()).height;
                    plan.fps = Mandelbrot::MovieFrameRates.at(fps.SelectedIndex());
                    if (!std::isfinite(duration.Value()) || duration.Value() < 1 || duration.Value() > 3600 ||
                        duration.Value() != std::floor(duration.Value()))
                        throw std::invalid_argument("Enter a whole number of seconds.");
                    plan.seconds = static_cast<int>(duration.Value());
                    size_t used{}; const std::wstring text(endZoom.Text()); plan.endZoom = std::stod(text, &used);
                    if (used != text.size()) throw std::invalid_argument("Enter a valid end zoom.");
                    plan.targetX = exactX; plan.targetY = exactY;
                    if (!(exactXText && targetX.Text() == targetXText) && !Mandelbrot::BigFloat::TryParse(targetX.Text(), plan.targetX))
                        throw std::invalid_argument("Enter a valid target X coordinate.");
                    if (!(exactYText && targetY.Text() == targetYText) && !Mandelbrot::BigFloat::TryParse(targetY.Text(), plan.targetY))
                        throw std::invalid_argument("Enter a valid target Y coordinate.");
                    if (autoIterations)
                        plan.start.maxIterations = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                            std::max(plan.start.zoom, plan.endZoom), plan.start.fractalType, baseIterations);
                    plan.motionSmoothing = motion.IsChecked().GetBoolean();
                    plan.Validate();
                }
                catch (const std::exception& error) { errorText.Text(to_hstring(error.what())); args.Cancel(true); }
            });
            if (co_await dialog.ShowAsync() == ContentDialogResult::Primary && !m_isClosed)
            {
                HWND hwnd{}; check_hresult(try_as<IWindowNative>()->get_WindowHandle(&hwnd));
                com_ptr<IFileSaveDialog> picker;
                check_hresult(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFileSaveDialog), picker.put_void()));
                const COMDLG_FILTERSPEC filter{L"High-quality MP4 movie", L"*.mp4"};
                check_hresult(picker->SetFileTypes(1, &filter)); check_hresult(picker->SetDefaultExtension(L"mp4"));
                check_hresult(picker->SetFileName(L"Fractal_Zoom.mp4")); check_hresult(picker->SetTitle(L"Save Zoom Movie"));
                FILEOPENDIALOGOPTIONS options{}; check_hresult(picker->GetOptions(&options));
                check_hresult(picker->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_STRICTFILETYPES | FOS_OVERWRITEPROMPT));
                const HRESULT choice = picker->Show(hwnd);
                if (choice != HRESULT_FROM_WIN32(ERROR_CANCELLED)) check_hresult(choice);
                if (SUCCEEDED(choice) && !m_isClosed)
                {
                    com_ptr<IShellItem> file; check_hresult(picker->GetResult(file.put()));
                    PWSTR rawPath{}; check_hresult(file->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
                    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, CoTaskMemFree);
                    m_renderer->Cancel(); if (m_juliaRenderer) m_juliaRenderer->Cancel();
                    m_movieFps = plan.fps;
                    m_movieExporter = std::make_unique<Mandelbrot::MovieExporter>(std::move(plan), std::wstring(path.get()));
                    m_movieExporting = true;
                    ViewportGrid().Visibility(Visibility::Collapsed); SidebarBorder().Visibility(Visibility::Collapsed);
                    MoviePreviewImage().Source(nullptr);
                    MoviePreviewText().Text(L"The first rendered frame will appear here. Preview refreshes about every 15 seconds.");
                    MovieCancelButton().Content(box_value(L"Cancel Export")); MovieCancelButton().IsEnabled(true);
                    MovieStatusText().Text(L"Rendering..."); MovieProgressBar().Value(0);
                    MovieProgressOverlay().Visibility(Visibility::Visible);
                    MovieCancelButton().Focus(FocusState::Programmatic);
                    if (!m_movieProgressTimer)
                    {
                        m_movieProgressTimer = DispatcherTimer(); m_movieProgressTimer.Interval(std::chrono::milliseconds(250));
                        auto weak = get_weak();
                        m_movieProgressTimer.Tick([weak](auto&&, auto&&) { if (auto window = weak.get()) window->UpdateMovieProgress(); });
                    }
                    m_movieProgressTimer.Start(); UpdateMovieProgress();
                }
            }
        }
        catch (const hresult_error& error) { if (!m_isClosed) HudTimingText().Text(L"Movie export failed: " + error.message()); }
        catch (const std::exception& error) { if (!m_isClosed) HudTimingText().Text(L"Movie export failed: " + to_hstring(error.what())); }
        m_movieSetup = false;
        if (!m_isClosed)
        {
            ExportMovieButton().IsEnabled(true);
            if (!m_movieExporter) { RenderFractal(); if (m_juliaVisible) RenderJulia(); }
        }
    }

    void MainWindow::UpdateMovieProgress()
    {
        if (m_isClosed || !m_movieExporter) return;
        const auto progress = m_movieExporter->Progress();
        MovieProgressBar().Value(100.0 * std::min(static_cast<double>(progress.total),
            static_cast<double>(progress.completed) + progress.frameFraction) / progress.total);
        MovieRenderedText().Text(L"Rendered " + FootageTime(progress.completed, m_movieFps) + L" of " +
            FootageTime(progress.total, m_movieFps) + L"  |  " + to_hstring(progress.completed) + L" / " + to_hstring(progress.total) + L" frames");
        if (auto preview = m_movieExporter->TakePreview())
        {
            Media::Imaging::WriteableBitmap bitmap(preview->width, preview->height);
            auto bytes = bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
            uint8_t* raw{}; check_hresult(bytes->Buffer(&raw));
            std::memcpy(raw, preview->pixels.data(), preview->pixels.size() * sizeof(uint32_t)); bitmap.Invalidate();
            MoviePreviewImage().Source(bitmap);
            MoviePreviewText().Text(L"Preview at " + FootageTime(preview->frame, m_movieFps) + L" of footage | updates about every 15 seconds");
        }
        if (progress.finished)
        {
            m_movieProgressTimer.Stop();
            MovieStatusText().Text(!progress.error.empty() ? L"Export failed: " + hstring(progress.error) :
                progress.canceled ? L"Export canceled. No partial movie was saved." : L"Movie saved.");
            MovieCancelButton().Content(box_value(L"Close")); MovieCancelButton().IsEnabled(true);
        }
        else if (progress.completed == progress.total) MovieStatusText().Text(L"Finalizing movie...");
    }

    void MainWindow::OnMovieCancelClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_movieExporter) return;
        if (!m_movieExporter->Progress().finished)
        {
            m_movieExporter->Cancel(); MovieCancelButton().IsEnabled(false); MovieStatusText().Text(L"Canceling...");
        }
        else
        {
            m_movieExporter.reset(); m_movieExporting = false;
            MovieProgressOverlay().Visibility(Visibility::Collapsed);
            ViewportGrid().Visibility(Visibility::Visible);
            SidebarBorder().Visibility(m_isSidebarVisible ? Visibility::Visible : Visibility::Collapsed);
            RenderFractal(); if (m_juliaVisible) RenderJulia();
        }
    }
}
