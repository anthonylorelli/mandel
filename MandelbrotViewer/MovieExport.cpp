#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MovieExport.h"
#include "MovieWriter.h"
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <system_error>

namespace Mandelbrot
{
    namespace
    {
        struct ColorSpace
        {
            std::array<float, 256> decode{};
            std::array<uint8_t, 4097> encode{};
            ColorSpace()
            {
                for (size_t i = 0; i < decode.size(); ++i)
                {
                    const float v = static_cast<float>(i) / 255;
                    decode[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
                }
                for (size_t i = 0; i < encode.size(); ++i)
                {
                    const float v = static_cast<float>(i) / 4096;
                    const float s = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - .055f;
                    encode[i] = static_cast<uint8_t>(std::clamp(s * 255 + .5f, 0.0f, 255.0f));
                }
            }
        };
        struct Canceled {};
        void ThrowIfCanceled(const std::atomic<bool>& cancel) { if (cancel.load(std::memory_order_relaxed)) throw Canceled{}; }
    }

    bool MoviePlan::PrepareAutoTargets(const std::atomic<bool>& cancel)
    {
        Validate();
        m_autoTargets.clear();
        if (!autoRetarget) return true;
        MandelbrotEngine engine;
        auto probe = start;
        // Analyze the same small viewport at every output resolution and frame rate.
        // Target decisions depend on footage time and magnification, not render speed.
        const double scale = std::min(192.0 / width, 108.0 / height);
        probe.width = std::max(8, static_cast<int>(std::lround(width * scale)));
        probe.height = std::max(8, static_cast<int>(std::lround(height * scale)));
        probe.renderStride = 1; probe.edgeRefine = false;
        std::vector<float> iterations;
        const double range = std::log(endZoom) - std::log(start.zoom);
        const int checks = static_cast<int>(std::ceil(std::max(seconds / .5, range / std::log(1.2))));
        AutoTarget state;
        state.x = start.hpCenterX; state.y = start.hpCenterY;
        state.targetX = state.x; state.targetY = state.y;
        uint64_t seed = targetSeed;
        for (int check = 0; check < checks; ++check)
        {
            if (cancel.load(std::memory_order_relaxed)) { m_autoTargets.clear(); return false; }
            const double t = static_cast<double>(check) / checks;
            if (check != 0) state = AdvanceAutoTarget(m_autoTargets.back(), t);
            // Retarget hysteresis follows elapsed footage or a doubling in zoom,
            // including very fast movies that cross several zoom decades per second.
            if (check != 0 && (t - m_autoTargets.back().progress) * seconds < .5 &&
                (t - m_autoTargets.back().progress) * range < std::log(2.0)) continue;
            probe.zoom = check == 0 ? start.zoom : std::exp(std::log(start.zoom) + range * t);
            probe.SetCenter(state.x, state.y);
            engine.Compute(probe, iterations, &cancel);
            if (cancel.load(std::memory_order_relaxed)) { m_autoTargets.clear(); return false; }
            if (check == 0 || MandelbrotEngine::ShouldRetargetZoom(probe, iterations, state.targetX, state.targetY))
            {
                // A deterministic stream of nonzero seeds leaves subframe lookups
                // independent of randomness and of the order in which they are rendered.
                seed += 0x9e3779b97f4a7c15ull;
                uint64_t sample = seed;
                sample = (sample ^ (sample >> 30)) * 0xbf58476d1ce4e5b9ull;
                sample = (sample ^ (sample >> 27)) * 0x94d049bb133111ebull;
                sample ^= sample >> 31;
                BigFloat x = state.targetX, y = state.targetY;
                const bool found = MandelbrotEngine::PickInterestingTarget(probe, iterations, x, y, sample == 0 ? 1 : sample);
                // The live picker can fall back to a distant bookmark when everything
                // is uniform. A movie must not fly across millions of viewport widths;
                // keep following the current target until visible detail is available.
                const double span = 3.0 / probe.zoom;
                if (found && std::abs((x - state.x).ToDouble()) <= span * .5 &&
                    std::abs((y - state.y).ToDouble()) <= span * probe.height / probe.width * .5)
                {
                    state.targetX = x; state.targetY = y;
                    m_autoTargets.push_back(state);
                }
                else if (check == 0) m_autoTargets.push_back(state);
            }
        }
        if (cancel.load(std::memory_order_relaxed)) { m_autoTargets.clear(); return false; }
        return true;
    }

    std::vector<uint32_t> RenderMovieFrame(const MoviePlan& plan, double frameIndex,
        const std::atomic<bool>& cancel, const std::function<void(float)>& progress,
        int bandRows, MandelbrotEngine* sharedEngine)
    {
        plan.Validate();
        if (bandRows < 1) throw std::invalid_argument("Invalid movie render band size.");
        if (cancel.load(std::memory_order_relaxed)) return {};
        static const ColorSpace colors;
        const ExecutionMode requestedMode = plan.useGpu ? ExecutionMode::Gpu : ExecutionMode::Automatic;
        std::optional<MandelbrotEngine> localEngine;
        if (!sharedEngine)
        {
            localEngine.emplace(requestedMode);
        }
        MandelbrotEngine& engine = sharedEngine ? *sharedEngine : *localEngine;
        const ExecutionMode previousMode = engine.GetExecutionMode();
        const ExecutionMode effectiveMode = (previousMode == ExecutionMode::Scalar) ? ExecutionMode::Scalar : requestedMode;
        struct ModeGuard
        {
            MandelbrotEngine& eng;
            ExecutionMode prev;
            ~ModeGuard() { eng.SetExecutionMode(prev); }
        } modeGuard{ engine, previousMode };
        engine.SetExecutionMode(effectiveMode);

        std::vector<uint32_t> output(static_cast<size_t>(plan.width) * plan.height), painted;
        std::vector<float> iterations;
        std::vector<SubpixelSamples> subpixels;
        const int temporal = plan.motionSmoothing ? 2 : 1;
        std::array<MandelbrotConfig, 2> cameras{};
        std::array<BigFloat, 2> steps{};
        for (int time = 0; time < temporal; ++time)
        {
            cameras[time] = plan.CameraAt(frameIndex + (temporal == 2 ? (time == 0 ? -.25 : .25) : 0));
            steps[time] = BigFloat(3.0 / cameras[time].zoom / plan.width);
        }

        if (effectiveMode == ExecutionMode::Gpu &&
            engine.CanUseGpu(cameras[0]) &&
            (temporal == 1 || engine.CanUseGpu(cameras[1])) &&
            engine.BeginGpuMovieFrame(plan.width, plan.height))
        {
            const int maxStripByMem = std::max(
                1,
                static_cast<int>((4ULL * 1024 * 1024) / static_cast<size_t>(plan.width)) - 2);
            const int maxStripByTdr = std::max(
                16,
                static_cast<int>(12'000'000'000ULL /
                    (static_cast<uint64_t>(plan.width) * static_cast<uint64_t>(std::max(1, plan.start.maxIterations)))));
            const int gpuBandRows = std::min({ bandRows, maxStripByMem, maxStripByTdr });

            bool gpuOk = true;
            for (int time = 0; time < temporal && gpuOk; ++time)
            {
                if (cancel.load(std::memory_order_relaxed)) return {};
                const auto& camera = cameras[time];
                engine.PrepareDeepReference(camera, &cancel);
                if (cancel.load(std::memory_order_relaxed)) return {};

                for (int row = 0; row < plan.height && gpuOk; row += gpuBandRows)
                {
                    if (cancel.load(std::memory_order_relaxed)) return {};
                    const int rows = std::min(gpuBandRows, plan.height - row);
                    const int topHalo = row > 0 ? 1 : 0;
                    const int bottomHalo = (row + rows < plan.height) ? 1 : 0;
                    const int firstRow = row - topHalo;
                    const int bandHeight = rows + topHalo + bottomHalo;
                    const BigFloat rowOffset((plan.height - bandHeight) * .5 - firstRow);

                    auto band = camera;
                    band.height = bandHeight;
                    band.SetCenter(camera.hpCenterX,
                        camera.hpCenterY + steps[time].Mul(rowOffset));

                    uint32_t* outBand = output.data() + static_cast<size_t>(row) * plan.width;
                    gpuOk = engine.RenderGpuMovieBand(
                        band, topHalo, rows, row, time,
                        plan.theme, plan.frequency, plan.phase, plan.invert, plan.interior,
                        outBand, &cancel);
                    if (cancel.load(std::memory_order_relaxed)) return {};

                    if (gpuOk && progress)
                    {
                        const double completedRows = static_cast<double>(time) * plan.height + (row + rows);
                        progress(static_cast<float>(completedRows / (static_cast<double>(temporal) * plan.height)));
                    }
                }
            }

            if (cancel.load(std::memory_order_relaxed)) return {};
            if (gpuOk && engine.FinishGpuMovieFrame(plan.width, plan.height, output.data()))
            {
                return output;
            }
        }

        for (int time = 0; time < temporal; ++time)
        {
            if (cancel.load(std::memory_order_relaxed)) return {};
            const auto& camera = cameras[time];
            engine.PrepareDeepReference(camera, &cancel);
            if (cancel.load(std::memory_order_relaxed)) return {};

            for (int row = 0; row < plan.height; row += bandRows)
            {
                if (cancel.load(std::memory_order_relaxed)) return {};
                const int rows = std::min(bandRows, plan.height - row);
                const int topHalo = row > 0 ? 1 : 0;
                const int bottomHalo = (row + rows < plan.height) ? 1 : 0;
                const int firstRow = row - topHalo;
                const int bandHeight = rows + topHalo + bottomHalo;
                const size_t count = static_cast<size_t>(plan.width) * rows;
                const size_t bandCount = static_cast<size_t>(plan.width) * bandHeight;
                painted.resize(bandCount);
                const BigFloat rowOffset((plan.height - bandHeight) * .5 - firstRow);

                auto band = camera;
                band.height = bandHeight;
                band.SetCenter(camera.hpCenterX,
                    camera.hpCenterY + steps[time].Mul(rowOffset));
                engine.Compute(band, iterations, &cancel, &subpixels);
                if (cancel.load(std::memory_order_relaxed)) return {};
                // Halo rows provide complete 3x3 neighborhoods across band boundaries, making
                // adaptive subpixel refinement and colors independent of render bands.
                plan.theme.Recolor(iterations, painted.data(), bandCount, plan.frequency, plan.phase,
                    plan.invert, plan.interior, band.fractalType, band.width, &subpixels, false);
                const uint32_t* bandPixels = painted.data() + static_cast<size_t>(topHalo) * plan.width;
                uint32_t* outBand = output.data() + static_cast<size_t>(row) * plan.width;
                if (time == 0)
                {
                    std::memcpy(outBand, bandPixels, count * sizeof(uint32_t));
                }
                else
                {
                    #pragma omp parallel for schedule(static)
                    for (intptr_t i = 0; i < static_cast<intptr_t>(count); ++i)
                    {
                        const uint32_t c0 = outBand[i];
                        const uint32_t c1 = bandPixels[i];
                        if (c0 == c1) continue;
                        uint32_t color = 0xFF000000;
                        for (int channel = 0; channel < 3; ++channel)
                        {
                            const float avg = (colors.decode[(c0 >> (8 * channel)) & 255] +
                                               colors.decode[(c1 >> (8 * channel)) & 255]) * 0.5f;
                            color |= static_cast<uint32_t>(colors.encode[static_cast<size_t>(std::clamp(avg * 4096 + .5f, 0.0f, 4096.0f))]) << (channel * 8);
                        }
                        outBand[i] = color;
                    }
                }
                if (progress)
                {
                    const double completedRows = static_cast<double>(time) * plan.height + (row + rows);
                    progress(static_cast<float>(completedRows / (static_cast<double>(temporal) * plan.height)));
                }
            }
        }
        return output;
    }

    MoviePreview MakeMoviePreview(const std::vector<uint32_t>& pixels, int width, int height, uint32_t frame)
    {
        if (width < 1 || height < 1 || width > 7680 || height > 4320 || pixels.size() != static_cast<size_t>(width) * height)
            throw std::invalid_argument("Invalid movie preview dimensions.");
        MoviePreview preview;
        preview.width = std::min(960, width);
        preview.height = std::max(1, height * preview.width / width);
        preview.frame = frame;
        preview.pixels.resize(static_cast<size_t>(preview.width) * preview.height);
        #pragma omp parallel for schedule(static)
        for (int y = 0; y < preview.height; ++y)
            for (int x = 0; x < preview.width; ++x)
            {
                uint32_t sums[3]{}; uint32_t count = 0;
                for (int sy = y * height / preview.height; sy < (y + 1) * height / preview.height; ++sy)
                    for (int sx = x * width / preview.width; sx < (x + 1) * width / preview.width; ++sx)
                    {
                        const auto c = pixels[static_cast<size_t>(sy) * width + sx];
                        for (int ch = 0; ch < 3; ++ch) sums[ch] += (c >> (8 * ch)) & 255;
                        ++count;
                    }
                uint32_t color = 0xFF000000;
                for (int ch = 0; ch < 3; ++ch) color |= ((sums[ch] + count / 2) / count) << (8 * ch);
                preview.pixels[static_cast<size_t>(y) * preview.width + x] = color;
            }
        return preview;
    }

    MovieExporter::MovieExporter(MoviePlan plan, std::wstring destination) : m_total(plan.FrameCount())
    {
        plan.Validate();
        m_planning.store(plan.autoRetarget);
        m_worker = std::thread([this, plan = std::move(plan), destination = std::move(destination)]() mutable
        { Run(std::move(plan), std::move(destination)); });
    }
    MovieExporter::~MovieExporter() { Stop(); }
    void MovieExporter::Stop() { Cancel(); if (m_worker.joinable()) m_worker.join(); }
    MovieProgress MovieExporter::Progress() const
    {
        std::lock_guard lock(m_mutex);
        return {m_completed.load(), m_total, m_fraction.load(), m_finished, m_canceled, m_planning.load(), m_error};
    }
    std::optional<MoviePreview> MovieExporter::TakePreview()
    {
        std::lock_guard lock(m_mutex);
        auto result = std::move(m_preview); m_preview.reset(); return result;
    }
    void MovieExporter::Run(MoviePlan plan, std::wstring destination)
    {
        std::filesystem::path staging;
        bool ownsStaging = false;
        try
        {
            if (!plan.PrepareAutoTargets(m_cancel)) throw Canceled{};
            m_planning.store(false);
            static std::atomic<uint64_t> sequence{};
            for (;;)
            {
                staging = destination + L".rendering-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence) + L".tmp";
                HANDLE handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); ownsStaging = true; break; }
                if (GetLastError() != ERROR_FILE_EXISTS) throw std::system_error(GetLastError(), std::system_category(), "Cannot create movie output");
                staging.clear();
            }
            {
                MovieWriter writer(staging.wstring(), plan.width, plan.height, plan.fps);
                MandelbrotEngine engine(plan.useGpu ? ExecutionMode::Gpu : ExecutionMode::Automatic);
                struct PendingFrame { uint32_t frame{}; std::vector<uint32_t> pixels; };
                std::mutex queueMutex;
                std::condition_variable queueCv;
                std::optional<PendingFrame> pending;
                bool renderDone = false;
                std::exception_ptr writerException;

                std::thread encoderThread([&]()
                {
                    auto lastPreview = std::chrono::steady_clock::now();
                    try
                    {
                        for (;;)
                        {
                            PendingFrame item;
                            {
                                std::unique_lock lock(queueMutex);
                                queueCv.wait(lock, [&]
                                {
                                    return pending.has_value() || renderDone || m_cancel.load(std::memory_order_relaxed);
                                });
                                if (m_cancel.load(std::memory_order_relaxed)) break;
                                if (!pending.has_value())
                                {
                                    if (renderDone) break;
                                    continue;
                                }
                                item = std::move(*pending);
                                pending.reset();
                            }
                            queueCv.notify_one();

                            ThrowIfCanceled(m_cancel);
                            writer.WriteFrame(item.pixels);
                            ThrowIfCanceled(m_cancel);
                            m_completed.store(item.frame + 1);
                            const auto now = std::chrono::steady_clock::now();
                            if (item.frame == 0 || item.frame + 1 == m_total || now - lastPreview >= std::chrono::seconds(15))
                            {
                                auto preview = MakeMoviePreview(item.pixels, plan.width, plan.height, item.frame + 1);
                                { std::lock_guard lock(m_mutex); m_preview = std::move(preview); }
                                lastPreview = now;
                            }
                        }
                    }
                    catch (...)
                    {
                        std::lock_guard lock(queueMutex);
                        writerException = std::current_exception();
                    }
                    queueCv.notify_all();
                });

                struct EncoderGuard
                {
                    std::mutex& mutex;
                    std::condition_variable& cv;
                    bool& done;
                    std::thread& thread;
                    ~EncoderGuard()
                    {
                        { std::lock_guard lock(mutex); done = true; }
                        cv.notify_all();
                        if (thread.joinable()) thread.join();
                    }
                } guard{queueMutex, queueCv, renderDone, encoderThread};

                const int bandBudgetPixels = (plan.useGpu && engine.IsGpuAvailable()) ? (3840 * 1080) : (1920 * 270);
                const int exportBandRows = std::clamp(bandBudgetPixels / std::max(1, plan.width), 64, plan.height);
                for (uint32_t frame = 0; frame < m_total; ++frame)
                {
                    ThrowIfCanceled(m_cancel);
                    {
                        std::lock_guard lock(queueMutex);
                        if (writerException) std::rethrow_exception(writerException);
                    }
                    auto pixels = RenderMovieFrame(plan, frame, m_cancel,
                        [this](float fraction) { m_fraction.store(fraction); }, exportBandRows, &engine);
                    ThrowIfCanceled(m_cancel);
                    {
                        std::unique_lock lock(queueMutex);
                        queueCv.wait(lock, [&]
                        {
                            return !pending.has_value() || writerException != nullptr || m_cancel.load(std::memory_order_relaxed);
                        });
                        if (writerException) std::rethrow_exception(writerException);
                        ThrowIfCanceled(m_cancel);
                        pending = PendingFrame{frame, std::move(pixels)};
                        m_fraction.store(0);
                    }
                    queueCv.notify_one();
                }

                {
                    std::lock_guard lock(queueMutex);
                    renderDone = true;
                }
                queueCv.notify_all();
                if (encoderThread.joinable()) encoderThread.join();
                if (writerException) std::rethrow_exception(writerException);
                ThrowIfCanceled(m_cancel);
                writer.Finish();
            }
            ThrowIfCanceled(m_cancel);
            if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::system_error(GetLastError(), std::system_category(), "Cannot save completed movie");
            staging.clear();
        }
        catch (const Canceled&) { std::lock_guard lock(m_mutex); m_canceled = true; }
        catch (const std::exception& error)
        {
            std::lock_guard lock(m_mutex);
            const std::string message(error.what()); m_error.assign(message.begin(), message.end());
        }
        catch (...) { std::lock_guard lock(m_mutex); m_error = L"Unexpected movie export error."; }
        if (ownsStaging && !staging.empty()) { std::error_code ignored; std::filesystem::remove(staging, ignored); }
        m_planning.store(false);
        std::lock_guard lock(m_mutex); m_finished = true;
    }
}
