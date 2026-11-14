#pragma once
#include "MovieWriter.h"
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace Mandelbrot::detail
{
    // Parallel SIMD full-range BT.709 BGRA -> NV12 conversion.
    void ConvertBgraToNv12FullRangeBT709(
        const uint32_t* bgra, int srcW, int srcH,
        uint8_t* nv12, int codedW, int codedH) noexcept;

    struct Mp4SampleEntry
    {
        uint64_t offset{};
        uint32_t bytes{};
        int32_t ctsOffset{};
        bool keyframe{};
    };

    // ISO BMFF / MP4 container muxer with 64-bit mdat & co64 offsets,
    // avcC / hvcC decoder configs, and explicit nclx full-range BT.709/sRGB color tags.
    class Mp4FileWriter
    {
    public:
        void Open(const std::wstring& path, MovieCodec codec);
        void WriteSample(const uint8_t* data, size_t size, int32_t ctsOffset, bool keyframe);

        size_t SampleCount() const noexcept { return m_entries.size(); }
        bool HasSamples() const noexcept { return !m_entries.empty(); }

        void Finalize(
            int width,
            int height,
            int fps,
            MovieCodec codec,
            const std::vector<uint8_t>& vps,
            const std::vector<uint8_t>& sps,
            const std::vector<uint8_t>& pps);

    private:
        uint64_t Pos();
        void Bytes(const void* data, size_t n);
        void Four(const char* four);
        void U8(uint8_t n);
        void U16(uint16_t n);
        void U32(uint32_t n);
        void U64(uint64_t n);
        void Zero(size_t n);
        void Patch(uint64_t pos, uint32_t n);
        uint64_t Box(const char* kind);
        void End(uint64_t p);
        void Matrix();

        std::fstream m_file;
        uint64_t m_mediaPos{};
        std::vector<Mp4SampleEntry> m_entries;
    };
}
