#include "pch.h"
#include "TestHarness.h"
#include "FramePresentation.h"

using namespace Mandelbrot;

TEST_CASE(FramePresentation, GuardBandPreservesPixelSpacingAndViewportCenter)
{
    for (int width : { 64, 767, 1024 })
    {
        MandelbrotConfig view;
        view.width = width;
        view.height = 537;
        view.zoom = 85000.0;
        view.SetCenter(-0.7436438870371587, 0.1318259042053119);
        const auto frame = AnimationRenderConfig(view);
        ASSERT_GT(frame.width, view.width);
        ASSERT_GT(frame.height, view.height);
        ASSERT_NEAR(3.0 / view.zoom / view.width, 3.0 / frame.zoom / frame.width, 1e-20);
        ASSERT_TRUE(frame.hpCenterX == view.hpCenterX);
        const auto t = FrameToViewport(frame, view);
        ASSERT_NEAR(1.0, t.scale, 1e-12);
        ASSERT_NEAR(-(frame.width - view.width) * 0.5, t.x, 1e-10);
        ASSERT_NEAR(-(frame.height - view.height) * 0.5, t.y, 1e-10);
        ASSERT_TRUE(FrameCoversViewport(frame, view));
        const auto restored = FrameViewport(frame, view.width, view.height);
        ASSERT_NEAR(view.zoom, restored.zoom, 1e-10);
    }
}

TEST_CASE(FramePresentation, TransformMatchesComplexCoordinateMapping)
{
    MandelbrotConfig view;
    view.width = 768;
    view.height = 576;
    view.SetCenter(-0.75, 0.125);
    view.zoom = 10.0;
    const auto frame = AnimationRenderConfig(view);
    view.zoom *= 1.05;
    view.SetCenter(-0.748, 0.127);
    const auto t = FrameToViewport(frame, view);
    for (int px : { 5, 300, frame.width - 5 })
        for (int py : { 3, 200, frame.height - 3 })
        {
            double cx, cy, expectedX, expectedY;
            MandelbrotEngine::ScreenToComplex(px, py, frame.width, frame.height,
                frame.centerX, frame.centerY, frame.zoom, cx, cy);
            MandelbrotEngine::ComplexToScreen(cx, cy, view.width, view.height,
                view.centerX, view.centerY, view.zoom, expectedX, expectedY);
            ASSERT_NEAR(expectedX + 0.5, t.scale * (px + 0.5) + t.x, 1e-9);
            ASSERT_NEAR(expectedY + 0.5, t.scale * (py + 0.5) + t.y, 1e-9);
        }
}

TEST_CASE(FramePresentation, DeepZoomTranslationDoesNotCancelCenterDifferences)
{
    for (double zoom : { 1e25, 1e100, 1e200 })
    {
        MandelbrotConfig frame;
        frame.width = 768;
        frame.height = 576;
        frame.zoom = zoom;
        frame.SetCenter(-0.75, 0.125);
        auto view = frame;
        const double step = 3.0 / zoom / frame.width;
        view.SetCenter(frame.hpCenterX + BigFloat(step * 20), frame.hpCenterY + BigFloat(step * 30));
        ASSERT_EQ(frame.centerX, view.centerX); // Both centers collapse to the same double.
        const auto t = FrameToViewport(frame, view);
        ASSERT_NEAR(1.0, t.scale, 1e-12);
        ASSERT_NEAR(-20.0, t.x, 1e-9);
        ASSERT_NEAR(30.0, t.y, 1e-9);
    }
}

TEST_CASE(FramePresentation, InterpolationIsLinearInScreenSpace)
{
    MandelbrotConfig a;
    a.width = 768;
    a.height = 576;
    a.zoom = 1e100;
    a.SetCenter(-0.75, 0.125);
    const auto frame = AnimationRenderConfig(a);
    auto b = a;
    b.zoom *= 1.06;
    b.SetCenter(a.hpCenterX + BigFloat(3e-100 / a.width * 10), a.hpCenterY - BigFloat(3e-100 / a.width * 8));
    const auto ta = FrameToViewport(frame, a), tb = FrameToViewport(frame, b);
    for (double progress : { 0.0, 0.1, 0.5, 0.9, 1.0 })
    {
        const auto current = InterpolateCamera(a, b, progress);
        const auto t = FrameToViewport(frame, current);
        ASSERT_NEAR(ta.scale + progress * (tb.scale - ta.scale), t.scale, 1e-12);
        ASSERT_NEAR(ta.x + progress * (tb.x - ta.x), t.x, 1e-9);
        ASSERT_NEAR(ta.y + progress * (tb.y - ta.y), t.y, 1e-9);
        ASSERT_TRUE(FrameCoversViewport(frame, current));
    }
}

TEST_CASE(FramePresentation, CameraLimitsPreventBlankEdgesAndExcessiveStretch)
{
    MandelbrotConfig start;
    start.width = 768;
    start.height = 576;
    start.zoom = 500000;
    start.SetCenter(-0.7436438870371587, 0.1318259042053119);
    const auto frame = AnimationRenderConfig(start);
    for (double zoomFactor : { 0.5, 1.0, 2.0 })
        for (double dx : { -1.0, 0.0, 1.0 })
            for (double dy : { -1.0, 0.0, 1.0 })
            {
                auto requested = start;
                requested.zoom *= zoomFactor;
                requested.SetCenter(start.hpCenterX + BigFloat(dx * 3.0 / start.zoom),
                    start.hpCenterY + BigFloat(dy * 3.0 / start.zoom));
                const auto limited = LimitCameraToFrame(frame, start, requested);
                ASSERT_TRUE(FrameCoversViewport(frame, limited));
                const auto t = FrameToViewport(frame, limited);
                ASSERT_LE(t.scale, MaxPresentationScale + 1e-9);
                for (double p : { 0.25, 0.5, 0.75 })
                    ASSERT_TRUE(FrameCoversViewport(frame, InterpolateCamera(start, limited, p)));
            }
    auto incompatible = start;
    incompatible.fractalType = FractalType::Newton;
    ASSERT_FALSE(FrameCoversViewport(frame, incompatible));
    ASSERT_EQ(start.zoom, LimitCameraToFrame(frame, start, incompatible).zoom);
}

TEST_CASE(FramePresentation, AnchorRebasePreservesTheScreenTransform)
{
    MandelbrotConfig oldAnchor;
    oldAnchor.width = 768;
    oldAnchor.height = 576;
    oldAnchor.zoom = 1e200;
    oldAnchor.SetCenter(-0.75, 0.125);
    auto newAnchor = oldAnchor;
    newAnchor.zoom *= 1.6;
    newAnchor.SetCenter(oldAnchor.hpCenterX + BigFloat(1e-202), oldAnchor.hpCenterY - BigFloat(2e-202));
    auto view = newAnchor;
    view.zoom *= 1.03;
    view.SetCenter(newAnchor.hpCenterX + BigFloat(2e-203), newAnchor.hpCenterY + BigFloat(3e-203));
    const auto previous = FrameToViewport(oldAnchor, view);
    const auto bridge = FrameToViewport(oldAnchor, newAnchor);
    const auto current = FrameToViewport(newAnchor, view);
    ASSERT_NEAR(current.scale, previous.scale / bridge.scale, 1e-12);
    ASSERT_NEAR(current.x, previous.x - bridge.x * previous.scale / bridge.scale, 1e-9);
    ASSERT_NEAR(current.y, previous.y - bridge.y * previous.scale / bridge.scale, 1e-9);
}

TEST_CASE(FramePresentation, ExportCropsGuardBandAndPreservesFinalPixels)
{
    MandelbrotConfig view;
    view.width = 8;
    view.height = 6;
    const auto frame = AnimationRenderConfig(view);
    std::vector<uint8_t> source(static_cast<size_t>(frame.width) * frame.height * 4);
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * frame.width + x) * 4;
            source[i] = static_cast<uint8_t>(x * 10);
            source[i + 1] = static_cast<uint8_t>(y * 10);
            source[i + 2] = 32;
            source[i + 3] = 255;
        }
    const auto snapshot = SnapshotViewport(source.data(), frame, view);
    ASSERT_EQ(static_cast<size_t>(view.width * view.height * 4), snapshot.size());
    const int padX = (frame.width - view.width) / 2, padY = (frame.height - view.height) / 2;
    for (int y = 0; y < view.height; ++y)
        for (int x = 0; x < view.width; ++x)
            for (int c = 0; c < 4; ++c)
                ASSERT_EQ(source[(static_cast<size_t>(y + padY) * frame.width + x + padX) * 4 + c],
                    snapshot[(static_cast<size_t>(y) * view.width + x) * 4 + c]);
    ASSERT_TRUE(source == SnapshotViewport(source.data(), frame, frame));
}

TEST_CASE(FramePresentation, HeavyFramesReduceCameraSpeedWithoutChangingQuality)
{
    ASSERT_EQ(1.0, PresentationZoomSpeed(1.0, 10.0));
    ASSERT_LT(PresentationZoomSpeed(1.0, 300.0), 1.0);
    ASSERT_GT(PresentationZoomSpeed(1.0, 300.0), 0.0);
    ASSERT_LE(PresentationZoomSpeed(10.0, 500.0), PresentationZoomSpeed(10.0, 100.0));
}

TEST_CASE(FramePresentation, DelayedFramesAndDirectionChangesKeepMotionCoveredAndAdvancing)
{
    for (double renderMs : { 32.0, 120.0, 400.0 })
    {
        MandelbrotConfig requested;
        requested.width = 768;
        requested.height = 576;
        requested.zoom = 1e100;
        requested.SetCenter(-0.75, 0.125);
        auto shown = AnimationRenderConfig(requested);
        auto rendering = shown;
        auto from = requested, to = requested;
        const BigFloat targetX = requested.hpCenterX + BigFloat(3e-100 * 0.12);
        const BigFloat targetY = requested.hpCenterY + BigFloat(3e-100 * 0.05);
        double budget = 0.0;
        double maxZoom = requested.zoom;
        int accepted = 0;
        for (int tick = 0; tick < 1200; ++tick)
        {
            from = InterpolateCamera(from, to, 0.5); // Half of a 32 ms compositor segment.
            ASSERT_TRUE(FrameCoversViewport(shown, from));
            const bool inward = (tick / 400) % 2 == 0;
            auto next = requested;
            const double factor = std::exp2((inward ? 1.0 : -1.0) * PresentationZoomSpeed(1.0, renderMs) * 0.016);
            MandelbrotEngine::StepZoomAnimation(next, targetX, targetY, factor, 0.5);
            requested = LimitCameraToFrame(shown, requested, next);
            to = requested;
            ASSERT_TRUE(FrameCoversViewport(shown, to));
            maxZoom = std::max(maxZoom, requested.zoom);
            budget += 16.0;
            if (budget >= renderMs)
            {
                budget = 0.0;
                if (FrameCoversViewport(rendering, from) && FrameCoversViewport(rendering, to))
                {
                    shown = rendering;
                    ++accepted;
                }
                rendering = AnimationRenderConfig(requested); // Latest queued request, including after a rejection.
            }
        }
        ASSERT_GT(accepted, 15);
        ASSERT_GT(maxZoom, 1.25e100);
    }
}

TEST_CASE(FramePresentation, ExportSamplesPixelCentersWithBilinearFiltering)
{
    MandelbrotConfig frame;
    frame.width = frame.height = 2;
    auto view = frame;
    view.zoom *= 2.0;
    const std::vector<uint8_t> source{ 0, 10, 20, 255, 80, 10, 20, 255,
        160, 10, 20, 255, 240, 10, 20, 255 };
    const auto pixels = SnapshotViewport(source.data(), frame, view);
    const std::vector<uint8_t> expected{ 60, 10, 20, 255, 100, 10, 20, 255,
        140, 10, 20, 255, 180, 10, 20, 255 };
    ASSERT_TRUE(expected == pixels);
}
