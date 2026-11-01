#pragma once
#include "MoviePlan.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace Mandelbrot
{
    struct MoviePreview { int width{}, height{}; uint32_t frame{}; std::vector<uint32_t> pixels; };
    std::vector<uint32_t> RenderMovieFrame(const MoviePlan& plan, double frameIndex,
        const std::atomic<bool>& cancel, const std::function<void(float)>& progress = {}, int bandRows = 64);
    MoviePreview MakeMoviePreview(const std::vector<uint32_t>& pixels, int width, int height, uint32_t frame);

    struct MovieProgress
    {
        uint32_t completed{}, total{};
        float frameFraction{};
        bool finished{}, canceled{};
        std::wstring error;
    };

    class MovieExporter
    {
    public:
        MovieExporter(MoviePlan plan, std::wstring destination);
        ~MovieExporter();
        void Cancel() { m_cancel.store(true, std::memory_order_relaxed); }
        void Stop();
        MovieProgress Progress() const;
        std::optional<MoviePreview> TakePreview();
    private:
        void Run(MoviePlan plan, std::wstring destination);
        std::atomic<bool> m_cancel{};
        std::atomic<uint32_t> m_completed{};
        std::atomic<float> m_fraction{};
        uint32_t m_total;
        mutable std::mutex m_mutex;
        std::optional<MoviePreview> m_preview;
        bool m_finished{}, m_canceled{};
        std::wstring m_error;
        std::thread m_worker;
    };
}
