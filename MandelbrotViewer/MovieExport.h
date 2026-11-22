#pragma once
#include "MoviePlan.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace Mandelbrot
{
    struct KeyframeInnerRect;
    struct MoviePreview { int width{}, height{}; uint32_t frame{}; std::vector<uint32_t> pixels; };
    // Automatic plans must call PrepareAutoTargets after their settings are finalized.
    // MovieExporter prepares them on its worker before rendering the first frame.
    std::vector<uint32_t> RenderMovieFrame(const MoviePlan& plan, double frameIndex,
        const std::atomic<bool>& cancel, const std::function<void(float)>& progress = {},
        int bandRows = 64, MandelbrotEngine* sharedEngine = nullptr);
    std::vector<uint32_t> RenderMovieKeyframe(const MoviePlan& plan, const FractalConfig& keyframeCamera,
        const KeyframeInnerRect* skipRect, const std::atomic<bool>& cancel,
        const std::function<void(float)>& progress = {}, int bandRows = 64,
        MandelbrotEngine* sharedEngine = nullptr);
    MoviePreview MakeMoviePreview(const std::vector<uint32_t>& pixels, int width, int height, uint32_t frame);

    struct MovieProgress
    {
        uint32_t completed{}, total{};
        float frameFraction{};
        bool finished{}, canceled{}, planning{};
        std::wstring error;
        double elapsedSeconds{};
        double currentZoom{ 1.0 };
    };

    class MovieExporter
    {
    public:
        MovieExporter(MoviePlan plan, std::wstring destination);
        ~MovieExporter();
        void Cancel();
        void Stop();
        MovieProgress Progress() const;
        std::optional<MoviePreview> TakePreview();
    private:
        void Run(MoviePlan plan, std::wstring destination);
        std::atomic<bool> m_cancel{};
        std::atomic<uint32_t> m_completed{};
        std::atomic<float> m_fraction{};
        std::atomic<bool> m_planning{};
        std::atomic<double> m_currentZoom{ 1.0 };
        uint32_t m_total;
        std::chrono::steady_clock::time_point m_startTime{};
        std::chrono::steady_clock::time_point m_endTime{};
        mutable std::mutex m_mutex;
        std::mutex m_queueMutex;
        std::condition_variable m_queueCv;
        std::optional<MoviePreview> m_preview;
        bool m_finished{}, m_canceled{};
        std::wstring m_error;
        std::thread m_worker;
    };
}
