#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MovieExport.h"
#include "MovieFrameRenderer.h"
#include "MovieZoomPyramid.h"
#include "GpuFractalSolver.h"
#include "MovieWriter.h"
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <system_error>

namespace Mandelbrot
{
    namespace
    {
        struct Canceled {};
        void ThrowIfCanceled(const std::atomic<bool>& cancel) { if (cancel.load(std::memory_order_relaxed)) throw Canceled{}; }
    }

    MovieExporter::MovieExporter(MoviePlan plan, std::wstring destination)
        : m_currentZoom(plan.start.zoom)
        , m_total(plan.FrameCount())
        , m_startTime(std::chrono::steady_clock::now())
    {
        if (destination.empty())
            throw std::invalid_argument("Movie export destination path is empty.");
        plan.Validate();
        m_planning.store(plan.autoRetarget);
        m_worker = std::thread([this, plan = std::move(plan), destination = std::move(destination)]() mutable
        { Run(std::move(plan), std::move(destination)); });
    }
    MovieExporter::~MovieExporter() { Stop(); }
    void MovieExporter::Cancel()
    {
        {
            std::lock_guard lock(m_queueMutex);
            m_cancel.store(true, std::memory_order_release);
        }
        m_queueCv.notify_all();
    }
    void MovieExporter::Stop() { Cancel(); if (m_worker.joinable()) m_worker.join(); }
    MovieProgress MovieExporter::Progress() const
    {
        std::lock_guard lock(m_mutex);
        const auto end = m_finished ? m_endTime : std::chrono::steady_clock::now();
        const double elapsed = std::max(0.0, std::chrono::duration<double>(end - m_startTime).count());
        return {m_completed.load(), m_total, m_fraction.load(), m_finished, m_canceled, m_planning.load(), m_error, elapsed, m_currentZoom.load(std::memory_order_relaxed)};
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
                MovieWriter writer(staging.wstring(), plan.width, plan.height, plan.fps, plan.codec);
                MandelbrotEngine engine(plan.useGpu ? ExecutionMode::Gpu : ExecutionMode::Automatic);
                const double peakZoom = std::max(plan.start.zoom, plan.endZoom);
                if (!plan.start.juliaSet)
                {
                    if (!plan.autoRetarget)
                    {
                        const bool zoomIn = (plan.endZoom >= plan.start.zoom);
                        engine.SetMovieReferenceHint(
                            peakZoom,
                            zoomIn ? &plan.targetX : &plan.start.hpCenterX,
                            zoomIn ? &plan.targetY : &plan.start.hpCenterY);
                    }
                    else
                    {
                        engine.SetMovieReferenceHint(peakZoom);
                    }
                }
                struct PendingFrame { uint32_t frame{}; std::vector<uint32_t> pixels; };
                std::optional<PendingFrame> pending;
                bool renderDone = false;
                bool encoderDone = false;
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
                                std::unique_lock lock(m_queueMutex);
                                m_queueCv.wait(lock, [&]
                                {
                                    return pending.has_value() || renderDone || m_cancel.load(std::memory_order_acquire);
                                });
                                if (m_cancel.load(std::memory_order_acquire)) break;
                                if (!pending.has_value())
                                {
                                    if (renderDone) break;
                                    continue;
                                }
                                item = std::move(*pending);
                                pending.reset();
                            }
                            m_queueCv.notify_one();

                            ThrowIfCanceled(m_cancel);
                            writer.WriteFrame(item.pixels);
                            ThrowIfCanceled(m_cancel);
                            m_currentZoom.store(plan.ZoomAt(static_cast<double>(item.frame)), std::memory_order_relaxed);
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
                        std::lock_guard lock(m_queueMutex);
                        writerException = std::current_exception();
                    }
                    {
                        std::lock_guard lock(m_queueMutex);
                        pending.reset();
                        encoderDone = true;
                    }
                    m_queueCv.notify_all();
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
                } guard{m_queueMutex, m_queueCv, renderDone, encoderThread};

                const int bandBudgetPixels = (plan.useGpu && engine.IsGpuAvailable()) ? (3840 * 1080) : (1920 * 270);
                const int exportBandRows = std::clamp(bandBudgetPixels / std::max(1, plan.width), 64, plan.height);

                auto checkWriterError = [&]()
                {
                    std::lock_guard lock(m_queueMutex);
                    if (writerException) std::rethrow_exception(writerException);
                };

                auto enqueueFrame = [&](uint32_t frameIdx, std::vector<uint32_t>&& framePixels)
                {
                    ThrowIfCanceled(m_cancel);
                    {
                        std::unique_lock lock(m_queueMutex);
                        m_queueCv.wait(lock, [&]
                        {
                            return !pending.has_value() || encoderDone || writerException != nullptr || m_cancel.load(std::memory_order_acquire);
                        });
                        if (writerException) std::rethrow_exception(writerException);
                        ThrowIfCanceled(m_cancel);
                        if (encoderDone) throw Canceled{};
                        pending = PendingFrame{ frameIdx, std::move(framePixels) };
                        m_fraction.store(0.0f);
                    }
                    m_queueCv.notify_one();
                };

                ZoomPyramidPlan pyramid = PlanZoomKeyframes(plan);
                if (pyramid.enabled)
                {
                    const int kfBandRows = std::clamp(
                        bandBudgetPixels / std::max(1, pyramid.keyframeWidth),
                        64,
                        pyramid.keyframeHeight);
                    uint32_t nextFrameToEmit = 0;

                    auto emitReadyFrames = [&](int maxReadyKf, auto&& lookupKfPixels)
                    {
                        while (nextFrameToEmit < m_total)
                        {
                            const auto& assign = pyramid.frames[nextFrameToEmit];
                            const int maxNeeded = std::max(assign.primaryKeyframe, assign.secondaryKeyframe);
                            if (maxNeeded > maxReadyKf) break;

                            ThrowIfCanceled(m_cancel);
                            checkWriterError();

                            const FractalConfig outCam = plan.CameraAt(static_cast<double>(nextFrameToEmit));
                            GpuMovieShutterParams shutter{};
                            const bool hasShutter = detail::BuildShutterParams(plan, static_cast<double>(nextFrameToEmit), outCam, shutter);

                            const auto& primaryPixels = lookupKfPixels(assign.primaryKeyframe);
                            const auto& primaryKf = pyramid.keyframes[static_cast<size_t>(assign.primaryKeyframe)].camera;
                            const std::vector<uint32_t>* secondaryPixels = (assign.secondaryKeyframe >= 0)
                                ? &lookupKfPixels(assign.secondaryKeyframe)
                                : nullptr;
                            const FractalConfig* secondaryKf = (assign.secondaryKeyframe >= 0)
                                ? &pyramid.keyframes[static_cast<size_t>(assign.secondaryKeyframe)].camera
                                : nullptr;

                            std::vector<uint32_t> framePixels;
                            ResampleKeyframeFrame(
                                primaryPixels,
                                primaryKf,
                                secondaryPixels,
                                secondaryKf,
                                assign.blendWeight,
                                outCam,
                                hasShutter ? &shutter : nullptr,
                                framePixels,
                                m_cancel);
                            ThrowIfCanceled(m_cancel);
                            enqueueFrame(nextFrameToEmit, std::move(framePixels));
                            ++nextFrameToEmit;
                        }
                    };

                    if (!pyramid.zoomIn)
                    {
                        // Zoom-out: keyframes are already ordered deepest -> shallowest, so every
                        // subsequent keyframe directly reuses the previous keyframe's interior.
                        // Equalize maxIterations within small chunks when autoScaleIterations is on
                        // so stitched interiors and exteriors never have mismatched iteration limits.
                        constexpr int kZoomOutEqualizeChunk = 4;
                        std::vector<uint32_t> prevKfPixels;
                        std::vector<uint32_t> currKfPixels;
                        const int numKf = static_cast<int>(pyramid.keyframes.size());
                        for (int k = 0; k < numKf; ++k)
                        {
                            ThrowIfCanceled(m_cancel);
                            checkWriterError();

                            const bool chunkBoundary = (k % kZoomOutEqualizeChunk) == 0;
                            if (plan.autoScaleIterations && !chunkBoundary)
                            {
                                pyramid.keyframes[static_cast<size_t>(k)].camera.maxIterations =
                                    pyramid.keyframes[static_cast<size_t>(k - 1)].camera.maxIterations;
                            }

                            const auto& kfCam = pyramid.keyframes[static_cast<size_t>(k)].camera;
                            KeyframeInnerRect innerRect{};
                            if (k > 0 && (!plan.autoScaleIterations || !chunkBoundary))
                            {
                                innerRect = ComputeInnerSkipRect(kfCam, pyramid.keyframes[static_cast<size_t>(k - 1)].camera);
                            }

                            currKfPixels = RenderMovieKeyframe(
                                plan,
                                kfCam,
                                innerRect.valid ? &innerRect : nullptr,
                                m_cancel,
                                [this](float f) { m_fraction.store(std::clamp(f, 0.0f, 1.0f)); },
                                kfBandRows,
                                &engine);
                            ThrowIfCanceled(m_cancel);

                            if (innerRect.valid)
                            {
                                CompositeInnerKeyframe(
                                    currKfPixels,
                                    kfCam,
                                    prevKfPixels,
                                    pyramid.keyframes[static_cast<size_t>(k - 1)].camera,
                                    innerRect,
                                    m_cancel);
                                ThrowIfCanceled(m_cancel);
                            }

                            emitReadyFrames(k, [&](int idx) -> const std::vector<uint32_t>&
                            {
                                return (idx == k) ? currKfPixels : prevKfPixels;
                            });

                            prevKfPixels = std::move(currKfPixels);
                        }
                    }
                    else
                    {
                        // Zoom-in: process small reverse chunks within RAM budget so each coarser
                        // keyframe reuses the deeper keyframe's interior, then stream frames forward.
                        const size_t maxChunk = ComputeZoomPyramidChunkSize(pyramid.keyframeWidth, pyramid.keyframeHeight);
                        const size_t numKf = pyramid.keyframes.size();
                        int carryOverIdx = -1;
                        std::vector<uint32_t> carryOverPixels;

                        for (size_t chunkStart = 0; chunkStart < numKf;)
                        {
                            const size_t chunkCap = (chunkStart == 0) ? std::min<size_t>(maxChunk, 3) : maxChunk;
                            const size_t chunkEnd = std::min(numKf, chunkStart + chunkCap);
                            const size_t chunkCount = chunkEnd - chunkStart;

                            if (plan.autoScaleIterations && chunkCount > 1)
                            {
                                const int chunkMaxIter = pyramid.keyframes[chunkEnd - 1].camera.maxIterations;
                                for (size_t k = chunkStart; k < chunkEnd; ++k)
                                {
                                    pyramid.keyframes[k].camera.maxIterations = chunkMaxIter;
                                }
                            }

                            std::vector<std::vector<uint32_t>> chunkPixels(chunkCount);
                            for (size_t step = 0; step < chunkCount; ++step)
                            {
                                const size_t k = chunkEnd - 1 - step;
                                ThrowIfCanceled(m_cancel);
                                checkWriterError();

                                const auto& kfCam = pyramid.keyframes[k].camera;
                                KeyframeInnerRect innerRect{};
                                if (k + 1 < chunkEnd)
                                {
                                    innerRect = ComputeInnerSkipRect(kfCam, pyramid.keyframes[k + 1].camera);
                                }

                                auto rendered = RenderMovieKeyframe(
                                    plan,
                                    kfCam,
                                    innerRect.valid ? &innerRect : nullptr,
                                    m_cancel,
                                    [this, step, chunkCount](float f)
                                    {
                                        const float combined = (static_cast<float>(step) + std::clamp(f, 0.0f, 1.0f)) / static_cast<float>(chunkCount);
                                        m_fraction.store(std::clamp(combined, 0.0f, 1.0f));
                                    },
                                    kfBandRows,
                                    &engine);
                                ThrowIfCanceled(m_cancel);

                                if (innerRect.valid)
                                {
                                    CompositeInnerKeyframe(
                                        rendered,
                                        kfCam,
                                        chunkPixels[k + 1 - chunkStart],
                                        pyramid.keyframes[k + 1].camera,
                                        innerRect,
                                        m_cancel);
                                    ThrowIfCanceled(m_cancel);
                                }
                                chunkPixels[k - chunkStart] = std::move(rendered);
                            }

                            emitReadyFrames(static_cast<int>(chunkEnd - 1), [&](int idx) -> const std::vector<uint32_t>&
                            {
                                if (idx == carryOverIdx) return carryOverPixels;
                                return chunkPixels[static_cast<size_t>(idx) - chunkStart];
                            });

                            carryOverPixels.clear();
                            carryOverPixels.shrink_to_fit();
                            carryOverIdx = -1;

                            if (nextFrameToEmit < m_total &&
                                pyramid.frames[nextFrameToEmit].primaryKeyframe == static_cast<int>(chunkEnd - 1))
                            {
                                carryOverIdx = static_cast<int>(chunkEnd - 1);
                                carryOverPixels = std::move(chunkPixels.back());
                            }

                            chunkStart = chunkEnd;
                        }
                    }
                }
                else
                {
                    for (uint32_t frame = 0; frame < m_total; ++frame)
                    {
                        ThrowIfCanceled(m_cancel);
                        checkWriterError();
                        auto pixels = RenderMovieFrame(plan, frame, m_cancel,
                            [this](float fraction) { m_fraction.store(fraction); }, exportBandRows, &engine);
                        enqueueFrame(frame, std::move(pixels));
                    }
                }

                {
                    std::lock_guard lock(m_queueMutex);
                    renderDone = true;
                }
                m_queueCv.notify_all();
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
            const std::string message(error.what());
#ifdef _WIN32
            const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.data(), static_cast<int>(message.size()), nullptr, 0);
            if (needed > 0)
            {
                m_error.resize(static_cast<size_t>(needed));
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.data(), static_cast<int>(message.size()), m_error.data(), needed);
            }
            else
            {
                const int acpNeeded = MultiByteToWideChar(CP_ACP, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
                if (acpNeeded > 0)
                {
                    m_error.resize(static_cast<size_t>(acpNeeded));
                    MultiByteToWideChar(CP_ACP, 0, message.data(), static_cast<int>(message.size()), m_error.data(), acpNeeded);
                }
                else
                {
                    m_error.clear();
                    m_error.reserve(message.size());
                    for (unsigned char c : message) m_error.push_back(static_cast<wchar_t>(c));
                }
            }
#else
            m_error.clear();
            m_error.reserve(message.size());
            for (unsigned char c : message) m_error.push_back(static_cast<wchar_t>(c));
#endif
        }
        catch (...) { std::lock_guard lock(m_mutex); m_error = L"Unexpected movie export error."; }
        if (ownsStaging && !staging.empty()) { std::error_code ignored; std::filesystem::remove(staging, ignored); }
        m_planning.store(false);
        std::lock_guard lock(m_mutex);
        m_endTime = std::chrono::steady_clock::now();
        m_finished = true;
    }
}
