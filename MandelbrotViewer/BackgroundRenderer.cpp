#include "pch.h"
#include "BackgroundRenderer.h"
#include <utility>

namespace Mandelbrot
{
    BackgroundRenderer::BackgroundRenderer(NotifyReady notify, Compute compute)
        : m_notify(std::move(notify)), m_compute(std::move(compute)), m_worker([this] { Run(); })
    {
    }

    BackgroundRenderer::~BackgroundRenderer()
    {
        Stop();
    }

    uint64_t BackgroundRenderer::Submit(const MandelbrotConfig& config, bool supersede)
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) return 0;
        if (supersede)
        {
            ++m_generation;
            m_cancel.store(true, std::memory_order_relaxed);
            if (m_completed)
            {
                // A completed frame means the worker is idle and the back buffer is available.
                m_workBuffer = std::move(m_completed->iterations);
                m_completed.reset();
            }
        }
        const uint64_t id = ++m_nextId;
        m_pending = Request{ config, id, m_generation };
        m_wake.notify_one();
        return id;
    }

    std::optional<RenderResult> BackgroundRenderer::TakeCompleted(std::vector<float>& displayedIterations,
        const std::function<bool(const RenderResult&)>& accept)
    {
        std::lock_guard lock(m_mutex);
        if (!m_completed) return std::nullopt;
        if (!m_completed->result.error && accept && !accept(m_completed->result))
        {
            m_workBuffer.swap(m_completed->iterations);
            m_completed.reset();
            m_wake.notify_one();
            return std::nullopt;
        }
        if (!m_completed->result.error) displayedIterations.swap(m_completed->iterations);
        m_workBuffer.swap(m_completed->iterations);
        auto result = std::move(m_completed->result);
        m_completed.reset();
        m_wake.notify_one();
        return result;
    }

    void BackgroundRenderer::Stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_cancel.store(true, std::memory_order_relaxed);
            m_pending.reset();
            m_completed.reset();
        }
        m_wake.notify_one();
        if (m_worker.joinable()) m_worker.join();
    }

    void BackgroundRenderer::Cancel()
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_cancel.store(true, std::memory_order_relaxed);
        m_pending.reset();
        if (m_completed)
        {
            m_workBuffer = std::move(m_completed->iterations);
            m_completed.reset();
        }
    }

    void BackgroundRenderer::Run()
    {
        std::unique_lock lock(m_mutex);
        for (;;)
        {
            // Wait for presentation before reusing the back buffer. Neither frames nor UI
            // notifications accumulate when rendering or the UI is slower than the camera.
            m_wake.wait(lock, [this] { return m_stopping || (m_pending && !m_completed); });
            if (m_stopping) return;
            const Request request = std::move(*m_pending);
            m_pending.reset();
            m_cancel.store(false, std::memory_order_relaxed);
            lock.unlock();

            RenderResult result{ request.config, request.id };
            try
            {
                result.elapsedMs = m_compute
                    ? m_compute(request.config, m_workBuffer, &m_cancel)
                    : m_engine.Compute(request.config, m_workBuffer, &m_cancel);
            }
            catch (...)
            {
                result.error = std::current_exception();
            }

            lock.lock();
            if (m_stopping) return;
            if (m_cancel.load(std::memory_order_relaxed) || request.generation != m_generation || result.error)
            {
                // A canceled deep render may have partially modified the reference cache.
                m_engine = MandelbrotEngine{};
                if (!result.error || request.generation != m_generation) continue;
            }
            m_completed = Completed{ std::move(result), std::move(m_workBuffer) };
            lock.unlock();
            try
            {
                if (m_notify) m_notify();
            }
            catch (...)
            {
                // A dispatcher may already have shut down. Never let a notification failure
                // escape a native worker thread; the owner can still stop and join it.
            }
            lock.lock();
        }
    }
}
