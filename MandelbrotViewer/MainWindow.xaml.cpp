#include "pch.h"
#include "MainWindow.xaml.h"
#include "ViewState.h"
#include "FrameTiming.h"
#include "FramePresentation.h"
#include "JuliaView.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif

#include <iomanip>
#include <sstream>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Dispatching.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Microsoft::UI::Composition;

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

        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        m_renderer = std::make_unique<Mandelbrot::BackgroundRenderer>([weak, dispatcher]
        {
            dispatcher.TryEnqueue([weak]
            {
                if (auto window = weak.get()) window->OnRenderCompleted();
            });
        });
        InitializeControls();
        Closed([this](auto&&, auto&&)
        {
            m_isClosed = true;
            m_isZoomAnimating = false;
            m_isDragging = false;
            FreezePresentation();
            if (m_animateTimer) m_animateTimer.Stop();
            if (m_zoomTimer) m_zoomTimer.Stop();
            if (m_renderer) m_renderer->Stop();
            if (m_juliaRenderer) m_juliaRenderer->Stop();
        });
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
        m_zoomTimer.Interval(Mandelbrot::ZoomFrameDelay(std::chrono::steady_clock::duration::zero()));
        m_zoomTimer.Tick([this](auto&&, auto&&)
        {
            if (!m_isZoomAnimating)
            {
                m_zoomTimer.Stop();
                return;
            }

            // Calculation runs on the worker; this callback only advances the requested camera.
            m_zoomTimer.Stop();

            const auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - m_lastZoomFrameTime).count();
            m_lastZoomFrameTime = now;
            dt = std::clamp(dt, 0.008, 0.10);

            // A resize or manual jump needs its first valid frame before motion can resume.
            if (!m_hasCurrentFrame || !m_hasPresentation)
            {
                m_zoomTimer.Interval(std::chrono::milliseconds(16));
                m_zoomTimer.Start();
                return;
            }

            const double speed = Mandelbrot::PresentationZoomSpeed(m_zoomSpeed, m_lastFractalTimeMs);
            const double exponent = (m_zoomInDirection ? 1.0 : -1.0) * speed * dt;
            const double factor = std::pow(2.0, exponent);

            if (m_autoPickInterestingTarget && m_zoomInDirection && m_hasCurrentFrame)
            {
                if (m_framesSinceRetarget >= 15 &&
                    Mandelbrot::MandelbrotEngine::ShouldRetargetZoom(m_displayedConfig, m_iterations, m_zoomTargetX, m_zoomTargetY))
                {
                    PickAndApplyRandomTarget();
                }
            }

            m_retargetPanProgress = std::min(1.0, m_retargetPanProgress + dt / 0.40);
            const double s = m_retargetPanProgress;
            const double panBlend = std::clamp(s * s * (3.0 - 2.0 * s), 0.08, 1.0);

            auto nextCamera = m_config;
            Mandelbrot::MandelbrotEngine::StepZoomAnimation(
                nextCamera, m_zoomTargetX, m_zoomTargetY, factor, panBlend);
            const double span = 3.0 / m_config.zoom;
            const bool jump = std::abs((nextCamera.hpCenterX - m_config.hpCenterX).ToDouble()) > span * 2.0 ||
                std::abs((nextCamera.hpCenterY - m_config.hpCenterY).ToDouble()) > span * 2.0;
            m_config = jump ? nextCamera : Mandelbrot::LimitCameraToFrame(m_displayedConfig, m_config, nextCamera);
            const bool canContinue = m_zoomInDirection
                ? m_config.zoom < Mandelbrot::MandelbrotEngine::MaxZoom
                : m_config.zoom > Mandelbrot::MandelbrotEngine::MinZoom;

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

            // Keep spatial sampling stable; PresentationZoomSpeed adapts the camera pace.
            m_config.edgeRefine = true;
            m_config.renderStride = 2;

            if (jump) RenderFractal();
            else
            {
                AnimateCamera(m_config, std::chrono::milliseconds(32));
                RenderFractal(true);
            }

            if (!canContinue)
            {
                StopZoomAnimation();
            }
            else if (m_isZoomAnimating)
            {
                m_zoomTimer.Interval(Mandelbrot::ZoomFrameDelay(std::chrono::steady_clock::now() - now));
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
        if (m_isClosed || !m_renderer || m_currentPixelWidth <= 0 || m_currentPixelHeight <= 0) return;
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
            if (m_autoPickInterestingTarget && m_zoomInDirection && !m_hasActiveRandomTarget)
                PickAndApplyRandomTarget();
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
            &samples);

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
        if (m_isZoomAnimating) StopZoomAnimation(false);

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

        const size_t decimals = Mandelbrot::CoordinateDecimalPlaces(m_config.zoom);
        m_suppressEvents = true;
        m_targetXText = winrt::hstring(m_zoomTargetX.ToWString(decimals));
        m_targetYText = winrt::hstring(m_zoomTargetY.ToWString(decimals));
        TargetXTextBox().Text(m_targetXText);
        TargetYTextBox().Text(m_targetYText);
        m_suppressEvents = false;
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

        m_suppressEvents = true;
        ZoomDirectionInToggle().IsChecked(zoomIn);
        ZoomDirectionOutToggle().IsChecked(!zoomIn);
        m_suppressEvents = false;
    }

    void MainWindow::StartZoomAnimation()
    {
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

        // If already at zoom bound in the chosen direction, flip direction or return
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
            JuliaOptionsPanel().Visibility(type == Mandelbrot::FractalType::Mandelbrot ? Visibility::Visible : Visibility::Collapsed);
            if (type != Mandelbrot::FractalType::Mandelbrot) ShowJuliaToggle().IsOn(false);

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
        RefreshJuliaSettings();
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
        co_await ExportViewport(false);
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::ExportViewport(bool julia)
    {
        if (julia ? !m_juliaHasFrame : (!m_bitmap || !m_hasPresentation)) co_return;
        auto lifetime = get_strong();

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
            winrt::check_hresult(this->try_as<IWindowNative>()->get_WindowHandle(&hwnd));
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

    winrt::Windows::Foundation::IAsyncAction MainWindow::OnCopyCoordsClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto package = winrt::Windows::ApplicationModel::DataTransfer::DataPackage();
        package.SetText(Mandelbrot::SerializeViewState(
            PresentationCamera(), m_theme, m_frequency, m_phaseOffset, m_invertColors, m_interiorColor));
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);

        // Visual confirmation by updating Hud text briefly
        HudTimingText().Text(L"Coordinates copied to clipboard!");
        co_return;
    }
}
