#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Mandelbrot
{
    enum class MovieCodec
    {
        H264,
        Hevc
    };

    // Color-tagged H.264 / HEVC MP4 writer: hardware-accelerated Media Foundation
    // encoding with software MFT fallback, full-range BT.709/sRGB color metadata,
    // and 64-bit media sizes and sample offsets for large movies through 8K.
    class MovieWriter
    {
    public:
        MovieWriter(const std::wstring& path, int width, int height, int fps, MovieCodec codec = MovieCodec::H264);
        ~MovieWriter();
        MovieWriter(const MovieWriter&) = delete;
        MovieWriter& operator=(const MovieWriter&) = delete;
        void WriteFrame(const std::vector<uint32_t>& bgra);
        void Finish();
        MovieCodec ActiveCodec() const noexcept;
        bool IsHardwareAccelerated() const noexcept;
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
