#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Mandelbrot
{
    // Color-tagged Photo-JPEG MP4: independently encoded 4:2:2 full-range frames,
    // including 8K, with 64-bit media sizes and sample offsets for large movies.
    class MovieWriter
    {
    public:
        MovieWriter(const std::wstring& path, int width, int height, int fps);
        ~MovieWriter();
        MovieWriter(const MovieWriter&) = delete;
        MovieWriter& operator=(const MovieWriter&) = delete;
        void WriteFrame(const std::vector<uint32_t>& bgra);
        void Finish();
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
