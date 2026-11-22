#include "pch.h"
#include "MainWindow.xaml.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <random>
#pragma comment(lib, "uuid.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;

namespace winrt::MandelbrotViewer::implementation
{
    namespace
    {
        hstring FormatTenthsTimecode(uint64_t tenths)
        {
            std::wostringstream text;
            text << std::setfill(L'0') << std::setw(2) << tenths / 36000 << L":"
                << std::setw(2) << tenths / 600 % 60 << L":" << std::setw(2) << tenths / 10 % 60 << L"." << tenths % 10;
            return hstring(text.str());
        }

        hstring FootageTime(uint32_t frames, int fps)
        {
            return FormatTenthsTimecode(static_cast<uint64_t>(frames) * 10 / fps);
        }

        hstring ElapsedTime(double seconds)
        {
            return FormatTenthsTimecode(static_cast<uint64_t>(std::max(0.0, std::floor(seconds * 10.0 + 0.5))));
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
            help.Text(L"Starts from the current view and zooms to a fixed target or automatically follows detailed regions. High-quality MP4 output supports 8K and can produce large files.");
            help.TextWrapping(TextWrapping::Wrap); settings.Children().Append(help);
            ComboBox resolution; resolution.Header(box_value(L"Resolution")); resolution.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (const auto& item : Mandelbrot::MovieResolutions) resolution.Items().Append(box_value(item.label));
            resolution.SelectedIndex(1); settings.Children().Append(resolution);
            ComboBox fps; fps.Header(box_value(L"Frames per second")); fps.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (auto value : Mandelbrot::MovieFrameRates) fps.Items().Append(box_value(to_hstring(value)));
            fps.SelectedIndex(2); settings.Children().Append(fps);
            ComboBox codec; codec.Header(box_value(L"Video codec")); codec.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (const auto& item : Mandelbrot::MovieCodecs) codec.Items().Append(box_value(item.label));
            codec.SelectedIndex(0); settings.Children().Append(codec);
            NumberBox duration; duration.Header(box_value(L"Duration (seconds)")); duration.Minimum(1); duration.Maximum(3600);
            duration.Value(30); duration.SmallChange(1); duration.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
            settings.Children().Append(duration);
            ComboBox direction; direction.Header(box_value(L"Zoom direction")); direction.HorizontalAlignment(HorizontalAlignment::Stretch);
            direction.Items().Append(box_value(L"Zoom in")); direction.Items().Append(box_value(L"Zoom out"));
            direction.SelectedIndex(m_zoomInDirection ? 0 : 1); settings.Children().Append(direction);
            Slider zoomSpeed;
            zoomSpeed.Minimum(Mandelbrot::MinMovieZoomSpeed); zoomSpeed.Maximum(Mandelbrot::MaxMovieZoomSpeed); zoomSpeed.StepFrequency(0.1);
            zoomSpeed.Value(std::clamp(m_zoomSpeed, Mandelbrot::MinMovieZoomSpeed, Mandelbrot::MaxMovieZoomSpeed));
            settings.Children().Append(zoomSpeed);
            TextBlock zoomSummary; zoomSummary.TextWrapping(TextWrapping::Wrap); settings.Children().Append(zoomSummary);
            CheckBox autoTarget; autoTarget.Content(box_value(L"Auto-Pick Target (zoom in)"));
            const bool automatic = m_autoPickInterestingTarget && m_zoomInDirection && !plan.start.juliaSet;
            autoTarget.IsChecked(automatic); settings.Children().Append(autoTarget);
            TextBlock autoHelp; autoHelp.Text(L"Choose new detailed targets automatically as the zoom reaches uniform areas.");
            autoHelp.TextWrapping(TextWrapping::Wrap); settings.Children().Append(autoHelp);
            TextBox targetX; targetX.Header(box_value(L"Target center X")); targetX.Text(targetXText); settings.Children().Append(targetX);
            TextBox targetY; targetY.Header(box_value(L"Target center Y")); targetY.Text(targetYText); settings.Children().Append(targetY);
            const double startZoom = plan.start.zoom;
            const bool isJulia = plan.start.juliaSet;
            // Capture dialog controls weakly so event subscriptions never create COM reference cycles.
            const auto updateZoomAndTargets = [
                weakDuration = make_weak(duration),
                weakDirection = make_weak(direction),
                weakZoomSpeed = make_weak(zoomSpeed),
                weakZoomSummary = make_weak(zoomSummary),
                weakAutoTarget = make_weak(autoTarget),
                weakTargetX = make_weak(targetX),
                weakTargetY = make_weak(targetY),
                startZoom, isJulia]()
            {
                auto durationCtrl = weakDuration.get();
                auto directionCtrl = weakDirection.get();
                auto zoomSpeedCtrl = weakZoomSpeed.get();
                auto zoomSummaryCtrl = weakZoomSummary.get();
                auto autoTargetCtrl = weakAutoTarget.get();
                auto targetXCtrl = weakTargetX.get();
                auto targetYCtrl = weakTargetY.get();
                if (!durationCtrl || !directionCtrl || !zoomSpeedCtrl || !zoomSummaryCtrl ||
                    !autoTargetCtrl || !targetXCtrl || !targetYCtrl)
                    return;

                const bool zoomIn = directionCtrl.SelectedIndex() == 0;
                const double speed = std::clamp(std::round(zoomSpeedCtrl.Value() * 10.0) / 10.0,
                    Mandelbrot::MinMovieZoomSpeed, Mandelbrot::MaxMovieZoomSpeed);
                std::wostringstream speedHeader;
                speedHeader << L"Zoom speed (" << std::fixed << std::setprecision(1) << speed << L"x)";
                zoomSpeedCtrl.Header(box_value(speedHeader.str()));
                const int secs = std::isfinite(durationCtrl.Value())
                    ? std::clamp(static_cast<int>(std::floor(durationCtrl.Value())), 1, 3600) : 30;
                const double previewEndZoom = Mandelbrot::MoviePlan::EndZoomForSpeed(startZoom, zoomIn, speed, secs);
                zoomSummaryCtrl.Text(hstring(L"Zoom range: ") + FormatZoom(startZoom) + L" \u2192 " + FormatZoom(previewEndZoom));
                const bool canAutoTarget = zoomIn && !isJulia;
                if (!canAutoTarget && autoTargetCtrl.IsChecked().GetBoolean()) autoTargetCtrl.IsChecked(false);
                autoTargetCtrl.IsEnabled(canAutoTarget);
                const bool manual = !canAutoTarget || !autoTargetCtrl.IsChecked().GetBoolean();
                targetXCtrl.IsEnabled(manual); targetYCtrl.IsEnabled(manual);
            };
            updateZoomAndTargets();
            duration.ValueChanged([updateZoomAndTargets](auto&&, auto&&) { updateZoomAndTargets(); });
            direction.SelectionChanged([updateZoomAndTargets](auto&&, auto&&) { updateZoomAndTargets(); });
            zoomSpeed.ValueChanged([updateZoomAndTargets](auto&&, auto&&) { updateZoomAndTargets(); });
            autoTarget.Checked([updateZoomAndTargets](auto&&, auto&&) { updateZoomAndTargets(); });
            autoTarget.Unchecked([updateZoomAndTargets](auto&&, auto&&) { updateZoomAndTargets(); });
            CheckBox motion; motion.Content(box_value(L"Smooth motion between frames")); motion.IsChecked(true); settings.Children().Append(motion);
            TextBlock errorText; errorText.TextWrapping(TextWrapping::Wrap); settings.Children().Append(errorText);
            ScrollViewer scroller; scroller.Content(settings); scroller.MaxHeight(510);
            ContentDialog dialog;
            dialog.XamlRoot(WindowRoot().XamlRoot());
            dialog.RequestedTheme(WindowRoot().ActualTheme());
            dialog.Title(box_value(L"Export Zoom Movie"));
            dialog.Content(scroller); dialog.PrimaryButtonText(L"Choose File..."); dialog.CloseButtonText(L"Cancel");
            auto dialogThemeRevoker = WindowRoot().ActualThemeChanged(
                winrt::auto_revoke,
                [weakDialog = make_weak(dialog), weakRoot = make_weak(WindowRoot())](auto&&, auto&&)
                {
                    if (auto dlg = weakDialog.get())
                    {
                        if (auto root = weakRoot.get()) dlg.RequestedTheme(root.ActualTheme());
                    }
                });
            const bool autoIterations = m_autoScaleIterations;
            const int baseIterations = m_baseIterations;
            const auto clickToken = dialog.PrimaryButtonClick([&](auto&&, ContentDialogButtonClickEventArgs const& args)
            {
                try
                {
                    const int resIdx = resolution.SelectedIndex();
                    const int fpsIdx = fps.SelectedIndex();
                    const int codecIdx = codec.SelectedIndex();
                    if (resIdx < 0 || static_cast<size_t>(resIdx) >= Mandelbrot::MovieResolutions.size() ||
                        fpsIdx < 0 || static_cast<size_t>(fpsIdx) >= Mandelbrot::MovieFrameRates.size() ||
                        codecIdx < 0 || static_cast<size_t>(codecIdx) >= Mandelbrot::MovieCodecs.size())
                        throw std::invalid_argument("Select a valid resolution, frame rate, and codec.");
                    plan.width = Mandelbrot::MovieResolutions[static_cast<size_t>(resIdx)].width;
                    plan.height = Mandelbrot::MovieResolutions[static_cast<size_t>(resIdx)].height;
                    plan.fps = Mandelbrot::MovieFrameRates[static_cast<size_t>(fpsIdx)];
                    plan.codec = Mandelbrot::MovieCodecs[static_cast<size_t>(codecIdx)].codec;
                    if (!std::isfinite(duration.Value()) || duration.Value() < 1 || duration.Value() > 3600 ||
                        duration.Value() != std::floor(duration.Value()))
                        throw std::invalid_argument("Enter a whole number of seconds.");
                    plan.seconds = static_cast<int>(duration.Value());
                    const bool zoomIn = direction.SelectedIndex() == 0;
                    const double speed = std::clamp(std::round(zoomSpeed.Value() * 10.0) / 10.0,
                        Mandelbrot::MinMovieZoomSpeed, Mandelbrot::MaxMovieZoomSpeed);
                    plan.SetZoomMotion(zoomIn, speed);
                    plan.autoRetarget = zoomIn && !plan.start.juliaSet && autoTarget.IsChecked().GetBoolean();
                    plan.targetX = exactX; plan.targetY = exactY;
                    if (!plan.autoRetarget && !(exactXText && targetX.Text() == targetXText) && !Mandelbrot::BigFloat::TryParse(targetX.Text(), plan.targetX))
                        throw std::invalid_argument("Enter a valid target X coordinate.");
                    if (!plan.autoRetarget && !(exactYText && targetY.Text() == targetYText) && !Mandelbrot::BigFloat::TryParse(targetY.Text(), plan.targetY))
                        throw std::invalid_argument("Enter a valid target Y coordinate.");
                    plan.autoScaleIterations = autoIterations;
                    plan.baseIterations = baseIterations;
                    if (autoIterations)
                        plan.start.maxIterations = Mandelbrot::MandelbrotEngine::RecommendMaxIterations(
                            plan.start.zoom, plan.start.fractalType, baseIterations);
                    plan.motionSmoothing = motion.IsChecked().GetBoolean();
                    plan.Validate();
                    if (plan.autoRetarget)
                    {
                        std::random_device random;
                        plan.targetSeed = (static_cast<uint64_t>(random()) << 32) ^ random();
                    }
                }
                catch (const std::exception& error) { errorText.Text(to_hstring(error.what())); args.Cancel(true); }
            });
            const auto dialogResult = co_await dialog.ShowAsync();
            dialog.PrimaryButtonClick(clickToken);
            if (dialogResult == ContentDialogResult::Primary && !m_isClosed)
            {
                HWND hwnd{};
                auto windowNative = try_as<IWindowNative>();
                if (!windowNative) throw winrt::hresult_no_interface();
                check_hresult(windowNative->get_WindowHandle(&hwnd));
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
                    ViewportLayout().Visibility(Visibility::Collapsed); SidebarBorder().Visibility(Visibility::Collapsed);
                    MoviePreviewImage().Source(nullptr);
                    MoviePreviewText().Text(L"The first rendered frame will appear here. Preview refreshes about every 15 seconds.");
                    MovieCancelButton().Content(box_value(L"Cancel Export")); MovieCancelButton().IsEnabled(true);
                    MovieElapsedText().Text(L"Elapsed time: 00:00:00.0");
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
            ExportMovieButton().IsEnabled(m_movieExporter == nullptr);
            if (!m_movieExporter) { RenderFractal(); if (m_juliaVisible) RenderJulia(); }
        }
    }

    void MainWindow::UpdateMovieProgress()
    {
        if (m_isClosed || !m_movieExporter) return;
        const auto progress = m_movieExporter->Progress();
        if (progress.total > 0)
        {
            MovieProgressBar().Value(100.0 * std::min(static_cast<double>(progress.total),
                static_cast<double>(progress.completed) + progress.frameFraction) / progress.total);
        }
        MovieRenderedText().Text(L"Rendered " + FootageTime(progress.completed, m_movieFps) + L" of " +
            FootageTime(progress.total, m_movieFps) + L"  |  " + to_hstring(progress.completed) + L" / " + to_hstring(progress.total) + L" frames");
        MovieElapsedText().Text(L"Elapsed time: " + ElapsedTime(progress.elapsedSeconds));
        if (auto preview = m_movieExporter->TakePreview())
        {
            if (preview->width > 0 && preview->height > 0 &&
                preview->pixels.size() == static_cast<size_t>(preview->width) * static_cast<size_t>(preview->height))
            {
                try
                {
                    Media::Imaging::WriteableBitmap bitmap(preview->width, preview->height);
                    auto bytes = bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
                    uint8_t* raw{}; check_hresult(bytes->Buffer(&raw));
                    if (raw)
                    {
                        std::memcpy(raw, preview->pixels.data(), preview->pixels.size() * sizeof(uint32_t));
                        bitmap.Invalidate();
                        MoviePreviewImage().Source(bitmap);
                        MoviePreviewText().Text(L"Preview at " + FootageTime(preview->frame, m_movieFps) + L" of footage | updates about every 15 seconds");
                    }
                }
                catch (...) {}
            }
        }
        if (progress.finished)
        {
            m_movieProgressTimer.Stop();
            MovieStatusText().Text(!progress.error.empty() ? L"Export failed: " + hstring(progress.error) :
                progress.canceled ? L"Export canceled. No partial movie was saved." : L"Movie saved.");
            MovieCancelButton().Content(box_value(L"Close")); MovieCancelButton().IsEnabled(true);
        }
        else if (progress.planning) MovieStatusText().Text(L"Finding interesting zoom targets...");
        else if (progress.completed == progress.total) MovieStatusText().Text(L"Finalizing movie...");
        else MovieStatusText().Text(L"Rendering...");
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
            m_movieExporter.reset();
            MovieProgressOverlay().Visibility(Visibility::Collapsed);
            ViewportLayout().Visibility(Visibility::Visible);
            SidebarBorder().Visibility(m_isSidebarVisible ? Visibility::Visible : Visibility::Collapsed);
            ExportMovieButton().IsEnabled(true);
            RenderFractal(); if (m_juliaVisible) RenderJulia();
        }
    }
}
