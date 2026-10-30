#pragma once
#include "MandelbrotEngine.h"
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace Mandelbrot
{
    struct RenderResult
    {
        MandelbrotConfig config;
        uint64_t requestId{};
        double elapsedMs{};
        std::exception_ptr error;
    };

    // One worker owns the engine and its reference-orbit cache. The caller owns the displayed
    // iteration buffer; TakeCompleted exchanges it with the worker's completed back buffer.
    class BackgroundRenderer
    {
    public:
        using NotifyReady = std::function<void()>;
        using Compute = std::function<double(const MandelbrotConfig&, std::vector<float>&, const std::atomic<bool>*)>;

        explicit BackgroundRenderer(NotifyReady notify, Compute compute = {});
        ~BackgroundRenderer();
        BackgroundRenderer(const BackgroundRenderer&) = delete;
        BackgroundRenderer& operator=(const BackgroundRenderer&) = delete;

        // Animation replaces the single pending request without aborting useful work.
        // A view change also cancels the active request and invalidates unpublished results.
        uint64_t Submit(const MandelbrotConfig& config, bool supersede = true);
        // An optional presentation predicate can discard frames that no longer cover the
        // visible camera. It runs under the worker lock and must not call back into this object.
        std::optional<RenderResult> TakeCompleted(std::vector<float>& displayedIterations,
            const std::function<bool(const RenderResult&)>& accept = {});
        void Cancel();
        void Stop();

    private:
        struct Request
        {
            MandelbrotConfig config;
            uint64_t id{};
            uint64_t generation{};
        };
        struct Completed
        {
            RenderResult result;
            std::vector<float> iterations;
        };
        void Run();

        NotifyReady m_notify;
        Compute m_compute;
        MandelbrotEngine m_engine;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::optional<Request> m_pending;
        std::optional<Completed> m_completed;
        std::vector<float> m_workBuffer;
        std::atomic<bool> m_cancel{ false };
        uint64_t m_generation{};
        uint64_t m_nextId{};
        bool m_stopping{ false };
        std::thread m_worker;
    };
}
