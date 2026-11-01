#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "MovieExport.h"
#include "MovieWriter.h"
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

    std::vector<uint32_t> RenderMovieFrame(const MoviePlan& plan, double frameIndex,
        const std::atomic<bool>& cancel, const std::function<void(float)>& progress, int bandRows)
    {
        plan.Validate();
        if (bandRows < 1) throw std::invalid_argument("Invalid movie render band size.");
        if (cancel.load(std::memory_order_relaxed)) return {};
        static const ColorSpace colors;
        MandelbrotEngine engine;
        std::vector<uint32_t> output(static_cast<size_t>(plan.width) * plan.height), painted;
        std::vector<float> iterations;
        std::vector<std::array<float, 3>> accum;
        const int temporal = plan.motionSmoothing ? 2 : 1;
        for (int row = 0; row < plan.height; row += bandRows)
        {
            const int rows = std::min(bandRows, plan.height - row);
            const size_t count = static_cast<size_t>(plan.width) * rows;
            accum.assign(count, {}); painted.resize(count);
            for (int time = 0; time < temporal; ++time)
                for (int spatial = 0; spatial < 4; ++spatial)
                {
                    if (cancel.load(std::memory_order_relaxed)) return {};
                    // Same quarter-pixel grid and half-frame shutter on every frame.
                    // No adaptive sample selection, changing stride, or temporal jitter.
                    const auto camera = plan.CameraAt(frameIndex + (temporal == 2 ? (time == 0 ? -.25 : .25) : 0));
                    auto band = camera;
                    band.height = rows;
                    const BigFloat step(3.0 / camera.zoom / plan.width);
                    const double sx = spatial % 2 ? .25 : -.25;
                    const double sy = spatial / 2 ? .25 : -.25;
                    band.SetCenter(camera.hpCenterX + step.Mul(BigFloat(sx)),
                        camera.hpCenterY + step.Mul(BigFloat((plan.height - rows) * .5 - row - sy)));
                    engine.Compute(band, iterations, &cancel);
                    if (cancel.load(std::memory_order_relaxed)) return {};
                    // Explicit supersampling replaces neighbor-dependent palette filtering,
                    // making colors independent of render bands and their boundaries.
                    plan.theme.Recolor(iterations, painted.data(), count, plan.frequency, plan.phase,
                        plan.invert, plan.interior, band.fractalType, band.width, nullptr, false);
                    #pragma omp parallel for schedule(static)
                    for (intptr_t i = 0; i < static_cast<intptr_t>(count); ++i)
                        for (int channel = 0; channel < 3; ++channel)
                            accum[i][channel] += colors.decode[(painted[i] >> (8 * channel)) & 255] / (4 * temporal);
                    if (progress) progress(static_cast<float>(row + rows * ((time * 4 + spatial + 1.0) / (4 * temporal))) / plan.height);
                }
            #pragma omp parallel for schedule(static)
            for (intptr_t i = 0; i < static_cast<intptr_t>(count); ++i)
            {
                uint32_t color = 0xFF000000;
                for (int channel = 0; channel < 3; ++channel)
                    color |= static_cast<uint32_t>(colors.encode[static_cast<size_t>(std::clamp(accum[i][channel] * 4096 + .5f, 0.0f, 4096.0f))]) << (channel * 8);
                output[static_cast<size_t>(row) * plan.width + i] = color;
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
        m_worker = std::thread([this, plan = std::move(plan), destination = std::move(destination)]() mutable
        { Run(std::move(plan), std::move(destination)); });
    }
    MovieExporter::~MovieExporter() { Stop(); }
    void MovieExporter::Stop() { Cancel(); if (m_worker.joinable()) m_worker.join(); }
    MovieProgress MovieExporter::Progress() const
    {
        std::lock_guard lock(m_mutex);
        return {m_completed.load(), m_total, m_fraction.load(), m_finished, m_canceled, m_error};
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
                auto lastPreview = std::chrono::steady_clock::now();
                for (uint32_t frame = 0; frame < m_total; ++frame)
                {
                    ThrowIfCanceled(m_cancel);
                    auto pixels = RenderMovieFrame(plan, frame, m_cancel, [this](float fraction) { m_fraction.store(fraction); });
                    ThrowIfCanceled(m_cancel);
                    writer.WriteFrame(pixels);
                    m_fraction.store(0); m_completed.store(frame + 1);
                    const auto now = std::chrono::steady_clock::now();
                    if (frame == 0 || frame + 1 == m_total || now - lastPreview >= std::chrono::seconds(15))
                    {
                        auto preview = MakeMoviePreview(pixels, plan.width, plan.height, frame + 1);
                        { std::lock_guard lock(m_mutex); m_preview = std::move(preview); }
                        lastPreview = now;
                    }
                }
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
        std::lock_guard lock(m_mutex); m_finished = true;
    }
}
