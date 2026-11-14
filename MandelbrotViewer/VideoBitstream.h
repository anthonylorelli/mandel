#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Mandelbrot::detail
{
    std::vector<uint8_t> UnescapeRbsp(const uint8_t* nal, size_t len);
    std::vector<uint8_t> EscapeRbsp(const std::vector<uint8_t>& rbsp, size_t headerBytes);

    // Rewrite H.264 SPS frame cropping to match exact visual dimensions (supporting
    // small frames padded to hardware MFT minimums) and tag full-range BT.709/sRGB VUI.
    std::vector<uint8_t> PatchH264Sps(const uint8_t* nal, size_t len, int targetW, int targetH);

    // Rewrite HEVC SPS conformance window to match exact visual dimensions and tag
    // full-range BT.709/sRGB VUI.
    std::vector<uint8_t> PatchHevcSps(const uint8_t* nal, size_t len, int targetW, int targetH);

    void SplitAnnexB(
        const uint8_t* data,
        size_t size,
        std::vector<std::pair<const uint8_t*, size_t>>& nalus);
}
