#include "pch.h"
#include "TestHarness.h"
#include "BackgroundRenderer.h"
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace Mandelbrot;
using namespace std::chrono_literals;

namespace
{
    struct Notifications
    {
        std::mutex mutex;
        std::condition_variable changed;
        size_t count{};
        void Ready()
        {
            std::lock_guard lock(mutex);
            ++count;
            changed.notify_all();
        }
        bool Wait(size_t expected)
        {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, 5s, [&] { return count >= expected; });
        }
    };

    // The first computation stays in flight until released or canceled. Tests can replace
    // pending requests deterministically without depending on fractal render timings.
    struct ControlledCompute
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool released{};
        bool canceled{};
        std::vector<double> zooms;
        std::thread::id threadId;

        double Run(const MandelbrotConfig& config, std::vector<float>& out, const std::atomic<bool>* cancel)
        {
            std::unique_lock lock(mutex);
            threadId = std::this_thread::get_id();
            zooms.push_back(config.zoom);
            changed.notify_all();
            if (zooms.size() == 1)
            {
                while (!released && !cancel->load()) changed.wait_for(lock, 1ms);
                canceled = cancel->load();
            }
            out.assign(static_cast<size_t>(config.width) * config.height, static_cast<float>(config.zoom));
            return config.zoom;
        }
        bool WaitStarted()
        {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, 5s, [&] { return !zooms.empty(); });
        }
        void Release()
        {
            std::lock_guard lock(mutex);
            released = true;
            changed.notify_all();
        }
    };
}

TEST_CASE(BackgroundRenderer, CalculatesOffThreadAndKeepsOnlyLatestPendingView)
{
    Notifications notifications;
    ControlledCompute compute;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [&](const auto& config, auto& out, auto cancel) { return compute.Run(config, out, cancel); });
    MandelbrotConfig config;
    config.width = 16;
    config.height = 12;
    const auto firstId = renderer.Submit(config);
    ASSERT_TRUE(compute.WaitStarted());
    uint64_t lastId{};
    for (int i = 2; i <= 25; ++i)
    {
        config.zoom = i;
        lastId = renderer.Submit(config, false);
    }
    config.zoom = 999; // Requests own their snapshots.
    compute.Release();
    ASSERT_TRUE(notifications.Wait(1));
    std::vector<float> pixels;
    auto first = renderer.TakeCompleted(pixels);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(firstId, first->requestId);
    ASSERT_EQ(1.0f, pixels.front());
    ASSERT_TRUE(notifications.Wait(2));
    auto last = renderer.TakeCompleted(pixels);
    ASSERT_TRUE(last.has_value());
    ASSERT_EQ(lastId, last->requestId);
    ASSERT_EQ(25.0, last->config.zoom);
    ASSERT_EQ(25.0f, pixels.front());
    renderer.Stop();
    ASSERT_EQ(static_cast<size_t>(2), compute.zooms.size());
    ASSERT_FALSE(compute.canceled);
    ASSERT_TRUE(compute.threadId != std::this_thread::get_id());
}

TEST_CASE(BackgroundRenderer, ViewChangeCancelsActiveWorkAndRejectsStaleFrame)
{
    Notifications notifications;
    ControlledCompute compute;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [&](const auto& config, auto& out, auto cancel) { return compute.Run(config, out, cancel); });
    MandelbrotConfig config;
    renderer.Submit(config);
    ASSERT_TRUE(compute.WaitStarted());
    config.fractalType = FractalType::Newton;
    config.width = 31;
    config.height = 23;
    config.zoom = 2.0;
    const auto id = renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(1));
    std::vector<float> pixels;
    auto result = renderer.TakeCompleted(pixels);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(id, result->requestId);
    ASSERT_EQ(static_cast<int>(FractalType::Newton), static_cast<int>(result->config.fractalType));
    ASSERT_EQ(31, result->config.width);
    ASSERT_EQ(23, result->config.height);
    ASSERT_EQ(static_cast<size_t>(31 * 23), pixels.size());
    ASSERT_EQ(2.0f, pixels.front());
    renderer.Stop();
    ASSERT_TRUE(compute.canceled);
    ASSERT_EQ(static_cast<size_t>(1), notifications.count);
}

TEST_CASE(BackgroundRenderer, UnpublishedFrameIsInvalidatedByNewView)
{
    Notifications notifications;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [](const auto& config, auto& out, auto) { out.assign(1, static_cast<float>(config.zoom)); return 1.0; });
    MandelbrotConfig config;
    renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(1));
    config.zoom = 2.0;
    auto id = renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(2));
    std::vector<float> pixels;
    auto result = renderer.TakeCompleted(pixels);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(id, result->requestId);
    ASSERT_EQ(2.0f, pixels.front());
}

TEST_CASE(BackgroundRenderer, StopCancelsActiveWorkAndDropsPendingRequests)
{
    Notifications notifications;
    ControlledCompute compute;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [&](const auto& config, auto& out, auto cancel) { return compute.Run(config, out, cancel); });
    MandelbrotConfig config;
    renderer.Submit(config);
    ASSERT_TRUE(compute.WaitStarted());
    config.zoom = 2.0;
    renderer.Submit(config, false);
    renderer.Stop();
    renderer.Stop();
    ASSERT_TRUE(compute.canceled);
    ASSERT_EQ(static_cast<size_t>(1), compute.zooms.size());
    ASSERT_EQ(static_cast<size_t>(0), notifications.count);
    ASSERT_EQ(uint64_t{ 0 }, renderer.Submit(config));
    std::vector<float> pixels{ 42.0f };
    ASSERT_FALSE(renderer.TakeCompleted(pixels).has_value());
    ASSERT_EQ(42.0f, pixels.front());
}

TEST_CASE(BackgroundRenderer, CancelWithoutReplacementKeepsDisplayedPixels)
{
    Notifications notifications;
    ControlledCompute compute;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [&](const auto& config, auto& out, auto cancel) { return compute.Run(config, out, cancel); });
    MandelbrotConfig config;
    renderer.Submit(config);
    ASSERT_TRUE(compute.WaitStarted());
    renderer.Cancel();
    config.zoom = 2.0;
    renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(1));
    std::vector<float> pixels;
    auto result = renderer.TakeCompleted(pixels);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(2.0, result->config.zoom);
    renderer.Stop();
    ASSERT_TRUE(compute.canceled);
}

TEST_CASE(BackgroundRenderer, ErrorsPreserveDisplayedPixelsAndWorkerCanRecover)
{
    Notifications notifications;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [](const auto& config, auto& out, auto)
        {
            out.assign(4, -99.0f);
            if (config.zoom == 1.0) throw std::runtime_error("test render failure");
            out.assign(4, 2.0f);
            return 3.0;
        });
    MandelbrotConfig config;
    std::vector<float> pixels{ 42.0f };
    renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(1));
    std::vector<SubpixelSamples> samples{ {0, 1.0f, {42.0f,42.0f,42.0f,42.0f}} };
    auto failed = renderer.TakeCompleted(pixels, {}, &samples);
    ASSERT_TRUE(failed.has_value());
    ASSERT_TRUE(failed->error != nullptr);
    ASSERT_EQ(static_cast<size_t>(1), pixels.size());
    ASSERT_EQ(42.0f, pixels.front());
    ASSERT_EQ(static_cast<size_t>(1), samples.size());
    ASSERT_EQ(42.0f, samples.front().iterations[0]);
    config.zoom = 2.0;
    renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(2));
    auto recovered = renderer.TakeCompleted(pixels, {}, &samples);
    ASSERT_TRUE(recovered.has_value());
    ASSERT_FALSE(recovered->error != nullptr);
    ASSERT_EQ(3.0, recovered->elapsedMs);
    ASSERT_EQ(2.0f, pixels.front());
    ASSERT_TRUE(samples.empty());
}

TEST_CASE(BackgroundRenderer, ExchangesTwoReusableBuffers)
{
    Notifications notifications;
    std::vector<const float*> buffers;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [&](const auto&, auto& out, auto)
        {
            out.assign(64, 7.0f);
            buffers.push_back(out.data());
            return 1.0;
        });
    std::vector<float> pixels(64);
    const float* initial = pixels.data();
    MandelbrotConfig config;
    for (size_t i = 0; i < 4; ++i)
    {
        renderer.Submit(config);
        ASSERT_TRUE(notifications.Wait(i + 1));
        ASSERT_TRUE(renderer.TakeCompleted(pixels).has_value());
        ASSERT_EQ(7.0f, pixels.front());
    }
    renderer.Stop();
    ASSERT_TRUE(buffers[0] != initial);
    ASSERT_TRUE(buffers[1] == initial);
    ASSERT_TRUE(buffers[0] == buffers[2]);
    ASSERT_TRUE(buffers[1] == buffers[3]);
}

TEST_CASE(BackgroundRenderer, RejectedPresentationKeepsTheDisplayedBuffer)
{
    Notifications notifications;
    BackgroundRenderer renderer([&] { notifications.Ready(); },
        [](const auto& config, auto& out, auto) { out.assign(4, static_cast<float>(config.zoom)); return 1.0; });
    MandelbrotConfig config;
    std::vector<float> pixels{ 42.0f };
    std::vector<SubpixelSamples> samples{ {0, 1.0f, {42.0f,42.0f,42.0f,42.0f}} };
    renderer.Submit(config);
    ASSERT_TRUE(notifications.Wait(1));
    ASSERT_FALSE(renderer.TakeCompleted(pixels, [](const RenderResult&) { return false; }, &samples).has_value());
    ASSERT_EQ(static_cast<size_t>(1), pixels.size());
    ASSERT_EQ(42.0f, pixels.front());
    ASSERT_EQ(static_cast<size_t>(1), samples.size());
    ASSERT_EQ(42.0f, samples.front().iterations[0]);
    config.zoom = 2.0;
    renderer.Submit(config, false);
    ASSERT_TRUE(notifications.Wait(2));
    ASSERT_TRUE(renderer.TakeCompleted(pixels, {}, &samples).has_value());
    ASSERT_EQ(2.0f, pixels.front());
    ASSERT_TRUE(samples.empty());
}

TEST_CASE(BackgroundRenderer, PixelValuesMatchSynchronousEngineForEveryFractal)
{
    Notifications notifications;
    BackgroundRenderer renderer([&] { notifications.Ready(); });
    MandelbrotEngine synchronous;
    size_t requestCount = 0;
    std::vector<float> displayed;
    std::vector<SubpixelSamples> displayedSamples;
    for (auto type : GetAllFractalTypes())
        for (bool deep : { false, true })
        {
            MandelbrotConfig config;
            config.fractalType = type;
            config.width = 33;
            config.height = 25;
            config.maxIterations = 800;
            if (deep)
            {
                config.SetCenter(type == FractalType::Newton ? BigFloat() : BigFloat(-2.0) + BigFloat(1e-25), BigFloat(1e-25));
                config.zoom = 1e25;
            }
            for (int stride : { 1, 3 })
            {
                config.renderStride = stride;
                renderer.Submit(config);
                ASSERT_TRUE(notifications.Wait(++requestCount));
                auto result = renderer.TakeCompleted(displayed, {}, &displayedSamples);
                ASSERT_TRUE(result.has_value());
                ASSERT_FALSE(result->error != nullptr);
                std::vector<float> expected;
                std::vector<SubpixelSamples> expectedSamples;
                synchronous.Compute(config, expected, nullptr, &expectedSamples);
                ASSERT_TRUE(expected == displayed);
                ASSERT_EQ(expectedSamples.size(), displayedSamples.size());
                for (size_t i = 0; i < expectedSamples.size(); ++i)
                {
                    ASSERT_EQ(expectedSamples[i].pixelIndex, displayedSamples[i].pixelIndex);
                    ASSERT_EQ(expectedSamples[i].weight, displayedSamples[i].weight);
                    ASSERT_TRUE(expectedSamples[i].iterations == displayedSamples[i].iterations);
                }
            }
        }
}
