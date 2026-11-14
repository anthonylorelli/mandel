#include "pch.h"
#include "VideoBitstream.h"

#include <algorithm>
#include <array>

namespace Mandelbrot::detail
{
    namespace
    {
        struct BitReader
        {
            const uint8_t* data{};
            size_t bits{};
            size_t pos{};

            uint32_t ReadBit() noexcept
            {
                if (pos >= bits) return 0;
                const uint32_t b = (data[pos >> 3] >> (7 - (pos & 7))) & 1u;
                ++pos;
                return b;
            }

            uint32_t ReadBits(int n) noexcept
            {
                uint32_t v = 0;
                for (int i = 0; i < n; ++i) v = (v << 1) | ReadBit();
                return v;
            }

            uint32_t ReadUE() noexcept
            {
                int zeros = 0;
                while (pos < bits && ReadBit() == 0 && zeros < 31) ++zeros;
                return zeros == 0 ? 0 : ((1u << zeros) - 1u + ReadBits(zeros));
            }

            int32_t ReadSE() noexcept
            {
                const uint32_t code = ReadUE();
                return (code & 1u)
                    ? static_cast<int32_t>((code + 1u) >> 1)
                    : -static_cast<int32_t>(code >> 1);
            }
        };

        struct BitWriter
        {
            std::vector<uint8_t> bytes;
            int bitPos{};

            void WriteBit(uint32_t b)
            {
                if (bitPos == 0) bytes.push_back(0);
                if (b & 1u) bytes.back() |= static_cast<uint8_t>(1u << (7 - bitPos));
                bitPos = (bitPos + 1) & 7;
            }

            void WriteBits(uint32_t v, int n)
            {
                for (int i = n - 1; i >= 0; --i) WriteBit((v >> i) & 1u);
            }

            void WriteUE(uint32_t v)
            {
                const uint32_t code = v + 1u;
                int len = 0;
                for (uint32_t t = code; t > 1; t >>= 1) ++len;
                for (int i = 0; i < len; ++i) WriteBit(0);
                WriteBits(code, len + 1);
            }

            void WriteSE(int32_t v)
            {
                const uint32_t code = v <= 0
                    ? static_cast<uint32_t>(-2 * static_cast<int64_t>(v))
                    : static_cast<uint32_t>(2 * static_cast<int64_t>(v) - 1);
                WriteUE(code);
            }

            void WriteTrailingBits()
            {
                WriteBit(1);
                while (bitPos != 0) WriteBit(0);
            }
        };
    }

    std::vector<uint8_t> UnescapeRbsp(const uint8_t* nal, size_t len)
    {
        std::vector<uint8_t> out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i)
        {
            if (i >= 2 && nal[i] == 0x03 && nal[i - 1] == 0x00 && nal[i - 2] == 0x00)
                continue;
            out.push_back(nal[i]);
        }
        return out;
    }

    std::vector<uint8_t> EscapeRbsp(const std::vector<uint8_t>& rbsp, size_t headerBytes)
    {
        std::vector<uint8_t> out;
        out.reserve(rbsp.size() + 8);
        for (size_t i = 0; i < headerBytes && i < rbsp.size(); ++i)
            out.push_back(rbsp[i]);
        int zeros = 0;
        for (size_t i = headerBytes; i < rbsp.size(); ++i)
        {
            const uint8_t b = rbsp[i];
            if (zeros >= 2 && b <= 0x03)
            {
                out.push_back(0x03);
                zeros = 0;
            }
            out.push_back(b);
            zeros = (b == 0x00) ? (zeros + 1) : 0;
        }
        return out;
    }

    std::vector<uint8_t> PatchH264Sps(const uint8_t* nal, size_t len, int targetW, int targetH)
    {
        const auto rbsp = UnescapeRbsp(nal, len);
        if (rbsp.size() < 4) return std::vector<uint8_t>(nal, nal + len);

        BitReader r{rbsp.data() + 1, (rbsp.size() - 1) * 8, 0};
        BitWriter w;
        w.WriteBits(rbsp[0], 8);

        const uint32_t profileIdc = r.ReadBits(8);
        const uint32_t constraints = r.ReadBits(8);
        const uint32_t levelIdc = r.ReadBits(8);
        const uint32_t spsId = r.ReadUE();
        w.WriteBits(profileIdc, 8);
        w.WriteBits(constraints, 8);
        w.WriteBits(levelIdc, 8);
        w.WriteUE(spsId);

        uint32_t chromaFormatIdc = 1;
        if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 || profileIdc == 244 ||
            profileIdc == 44  || profileIdc == 83  || profileIdc == 86  || profileIdc == 118 ||
            profileIdc == 128 || profileIdc == 138 || profileIdc == 139 || profileIdc == 134)
        {
            chromaFormatIdc = r.ReadUE();
            w.WriteUE(chromaFormatIdc);
            if (chromaFormatIdc == 3) w.WriteBit(r.ReadBit());
            w.WriteUE(r.ReadUE()); // bit_depth_luma_minus8
            w.WriteUE(r.ReadUE()); // bit_depth_chroma_minus8
            w.WriteBit(r.ReadBit()); // qpprime_y_zero_transform_bypass_flag
            const uint32_t scalingPresent = r.ReadBit();
            w.WriteBit(scalingPresent);
            if (scalingPresent)
            {
                const int count = (chromaFormatIdc != 3) ? 8 : 12;
                for (int i = 0; i < count; ++i)
                {
                    const uint32_t listPresent = r.ReadBit();
                    w.WriteBit(listPresent);
                    if (listPresent)
                    {
                        const int listSize = (i < 6) ? 16 : 64;
                        int lastScale = 8, nextScale = 8;
                        for (int j = 0; j < listSize; ++j)
                        {
                            if (nextScale != 0)
                            {
                                const int32_t delta = r.ReadSE();
                                w.WriteSE(delta);
                                nextScale = (lastScale + delta + 256) % 256;
                            }
                            lastScale = (nextScale == 0) ? lastScale : nextScale;
                        }
                    }
                }
            }
        }

        w.WriteUE(r.ReadUE()); // log2_max_frame_num_minus4
        const uint32_t pocType = r.ReadUE();
        w.WriteUE(pocType);
        if (pocType == 0)
        {
            w.WriteUE(r.ReadUE());
        }
        else if (pocType == 1)
        {
            w.WriteBit(r.ReadBit());
            w.WriteSE(r.ReadSE());
            w.WriteSE(r.ReadSE());
            const uint32_t n = r.ReadUE();
            w.WriteUE(n);
            for (uint32_t i = 0; i < n; ++i) w.WriteSE(r.ReadSE());
        }

        w.WriteUE(r.ReadUE()); // max_num_ref_frames
        w.WriteBit(r.ReadBit()); // gaps_in_frame_num_value_allowed_flag
        const uint32_t picWidthMbsMinus1 = r.ReadUE();
        const uint32_t picHeightMapUnitsMinus1 = r.ReadUE();
        const uint32_t frameMbsOnly = r.ReadBit();
        w.WriteUE(picWidthMbsMinus1);
        w.WriteUE(picHeightMapUnitsMinus1);
        w.WriteBit(frameMbsOnly);
        if (!frameMbsOnly) w.WriteBit(r.ReadBit());
        w.WriteBit(r.ReadBit()); // direct_8x8_inference_flag

        if (r.ReadBit()) { r.ReadUE(); r.ReadUE(); r.ReadUE(); r.ReadUE(); }

        const int mbW = static_cast<int>(picWidthMbsMinus1 + 1) * 16;
        const int mbH = static_cast<int>(picHeightMapUnitsMinus1 + 1) * (frameMbsOnly ? 16 : 32);
        const int subWidthC = (chromaFormatIdc == 1 || chromaFormatIdc == 2) ? 2 : 1;
        const int subHeightC = (chromaFormatIdc == 1) ? 2 : 1;
        const int cropUnitX = subWidthC;
        const int cropUnitY = subHeightC * static_cast<int>(2u - frameMbsOnly);
        const int cropRightUnits = std::max(0, mbW - targetW) / cropUnitX;
        const int cropBottomUnits = std::max(0, mbH - targetH) / cropUnitY;

        if (cropRightUnits > 0 || cropBottomUnits > 0)
        {
            w.WriteBit(1);
            w.WriteUE(0);
            w.WriteUE(static_cast<uint32_t>(cropRightUnits));
            w.WriteUE(0);
            w.WriteUE(static_cast<uint32_t>(cropBottomUnits));
        }
        else
        {
            w.WriteBit(0);
        }

        // Emit full-range BT.709 / sRGB VUI matching our NV12 conversion and colr nclx box.
        w.WriteBit(1);      // vui_parameters_present_flag
        w.WriteBit(1);      // aspect_ratio_info_present_flag
        w.WriteBits(1, 8);  // aspect_ratio_idc = 1:1
        w.WriteBit(0);      // overscan_info_present_flag
        w.WriteBit(1);      // video_signal_type_present_flag
        w.WriteBits(5, 3);  // video_format = unspecified
        w.WriteBit(1);      // video_full_range_flag = 1 (0..255)
        w.WriteBit(1);      // colour_description_present_flag
        w.WriteBits(1, 8);  // colour_primaries = BT.709
        w.WriteBits(13, 8); // transfer_characteristics = sRGB
        w.WriteBits(1, 8);  // matrix_coefficients = BT.709
        w.WriteBit(0);      // chroma_loc_info_present_flag
        w.WriteBit(0);      // timing_info_present_flag
        w.WriteBit(0);      // nal_hrd_parameters_present_flag
        w.WriteBit(0);      // vcl_hrd_parameters_present_flag
        w.WriteBit(0);      // pic_struct_present_flag
        w.WriteBit(0);      // bitstream_restriction_flag

        w.WriteTrailingBits();
        return EscapeRbsp(w.bytes, 1);
    }

    std::vector<uint8_t> PatchHevcSps(const uint8_t* nal, size_t len, int targetW, int targetH)
    {
        const auto rbsp = UnescapeRbsp(nal, len);
        if (rbsp.size() < 16) return std::vector<uint8_t>(nal, nal + len);

        BitReader r{rbsp.data() + 2, (rbsp.size() - 2) * 8, 0};
        BitWriter w;
        w.WriteBits(rbsp[0], 8);
        w.WriteBits(rbsp[1], 8);

        const uint32_t vpsId = r.ReadBits(4);
        const uint32_t maxSubLayersMinus1 = r.ReadBits(3);
        const uint32_t temporalIdNesting = r.ReadBit();
        w.WriteBits(vpsId, 4);
        w.WriteBits(maxSubLayersMinus1, 3);
        w.WriteBit(temporalIdNesting);

        for (int i = 0; i < 12; ++i) w.WriteBits(r.ReadBits(8), 8);
        std::array<uint32_t, 8> subProf{}, subLvl{};
        for (uint32_t i = 0; i < maxSubLayersMinus1; ++i)
        {
            subProf[i] = r.ReadBit();
            subLvl[i] = r.ReadBit();
            w.WriteBit(subProf[i]);
            w.WriteBit(subLvl[i]);
        }
        if (maxSubLayersMinus1 > 0)
        {
            for (uint32_t i = maxSubLayersMinus1; i < 8; ++i) w.WriteBits(r.ReadBits(2), 2);
        }
        for (uint32_t i = 0; i < maxSubLayersMinus1; ++i)
        {
            if (subProf[i]) for (int b = 0; b < 11; ++b) w.WriteBits(r.ReadBits(8), 8);
            if (subLvl[i]) w.WriteBits(r.ReadBits(8), 8);
        }

        w.WriteUE(r.ReadUE()); // sps_seq_parameter_set_id
        const uint32_t chromaFormatIdc = r.ReadUE();
        w.WriteUE(chromaFormatIdc);
        if (chromaFormatIdc == 3) w.WriteBit(r.ReadBit());
        const uint32_t picW = r.ReadUE();
        const uint32_t picH = r.ReadUE();
        w.WriteUE(picW);
        w.WriteUE(picH);

        if (r.ReadBit()) { r.ReadUE(); r.ReadUE(); r.ReadUE(); r.ReadUE(); }

        const int subWidthC = (chromaFormatIdc == 1 || chromaFormatIdc == 2) ? 2 : 1;
        const int subHeightC = (chromaFormatIdc == 1) ? 2 : 1;
        const int cropRightUnits = std::max(0, static_cast<int>(picW) - targetW) / subWidthC;
        const int cropBottomUnits = std::max(0, static_cast<int>(picH) - targetH) / subHeightC;
        if (cropRightUnits > 0 || cropBottomUnits > 0)
        {
            w.WriteBit(1);
            w.WriteUE(0);
            w.WriteUE(static_cast<uint32_t>(cropRightUnits));
            w.WriteUE(0);
            w.WriteUE(static_cast<uint32_t>(cropBottomUnits));
        }
        else
        {
            w.WriteBit(0);
        }

        w.WriteUE(r.ReadUE()); // bit_depth_luma_minus8
        w.WriteUE(r.ReadUE()); // bit_depth_chroma_minus8
        const uint32_t log2MaxPocMinus4 = r.ReadUE();
        w.WriteUE(log2MaxPocMinus4);
        const uint32_t subLayerOrdering = r.ReadBit();
        w.WriteBit(subLayerOrdering);
        for (uint32_t i = (subLayerOrdering ? 0u : maxSubLayersMinus1); i <= maxSubLayersMinus1; ++i)
        {
            w.WriteUE(r.ReadUE());
            w.WriteUE(r.ReadUE());
            w.WriteUE(r.ReadUE());
        }
        w.WriteUE(r.ReadUE()); // log2_min_luma_coding_block_size_minus3
        w.WriteUE(r.ReadUE()); // log2_diff_max_min_luma_coding_block_size
        w.WriteUE(r.ReadUE()); // log2_min_luma_transform_block_size_minus2
        w.WriteUE(r.ReadUE()); // log2_diff_max_min_luma_transform_block_size
        w.WriteUE(r.ReadUE()); // max_transform_hierarchy_depth_inter
        w.WriteUE(r.ReadUE()); // max_transform_hierarchy_depth_intra
        const uint32_t scalingListEnabled = r.ReadBit();
        w.WriteBit(scalingListEnabled);
        if (scalingListEnabled)
        {
            const uint32_t scalingData = r.ReadBit();
            w.WriteBit(scalingData);
            if (scalingData) return std::vector<uint8_t>(nal, nal + len);
        }
        w.WriteBit(r.ReadBit()); // amp_enabled_flag
        w.WriteBit(r.ReadBit()); // sample_adaptive_offset_enabled_flag
        const uint32_t pcmEnabled = r.ReadBit();
        w.WriteBit(pcmEnabled);
        if (pcmEnabled)
        {
            w.WriteBits(r.ReadBits(4), 4);
            w.WriteBits(r.ReadBits(4), 4);
            w.WriteUE(r.ReadUE());
            w.WriteUE(r.ReadUE());
            w.WriteBit(r.ReadBit());
        }
        const uint32_t numStRps = r.ReadUE();
        w.WriteUE(numStRps);
        std::array<uint32_t, 65> numDeltaPocs{};
        for (uint32_t stIdx = 0; stIdx < numStRps && stIdx < 64; ++stIdx)
        {
            const uint32_t interRps = (stIdx != 0) ? r.ReadBit() : 0;
            if (stIdx != 0) w.WriteBit(interRps);
            if (interRps)
            {
                const uint32_t deltaIdxMinus1 = (stIdx == numStRps) ? r.ReadUE() : 0;
                if (stIdx == numStRps) w.WriteUE(deltaIdxMinus1);
                w.WriteBit(r.ReadBit());
                w.WriteUE(r.ReadUE());
                const uint32_t refIdx = stIdx - 1 - std::min(stIdx - 1, deltaIdxMinus1);
                uint32_t count = 0;
                for (uint32_t j = 0; j <= numDeltaPocs[refIdx]; ++j)
                {
                    const uint32_t used = r.ReadBit();
                    w.WriteBit(used);
                    const uint32_t useDelta = used ? 1u : r.ReadBit();
                    if (!used) w.WriteBit(useDelta);
                    if (used || useDelta) ++count;
                }
                numDeltaPocs[stIdx] = count;
            }
            else
            {
                const uint32_t neg = r.ReadUE();
                const uint32_t pos = r.ReadUE();
                w.WriteUE(neg);
                w.WriteUE(pos);
                numDeltaPocs[stIdx] = neg + pos;
                for (uint32_t j = 0; j < neg; ++j) { w.WriteUE(r.ReadUE()); w.WriteBit(r.ReadBit()); }
                for (uint32_t j = 0; j < pos; ++j) { w.WriteUE(r.ReadUE()); w.WriteBit(r.ReadBit()); }
            }
        }
        const uint32_t ltRpsPresent = r.ReadBit();
        w.WriteBit(ltRpsPresent);
        if (ltRpsPresent)
        {
            const uint32_t numLt = r.ReadUE();
            w.WriteUE(numLt);
            const int pocBits = static_cast<int>(log2MaxPocMinus4 + 4);
            for (uint32_t j = 0; j < numLt; ++j)
            {
                w.WriteBits(r.ReadBits(pocBits), pocBits);
                w.WriteBit(r.ReadBit());
            }
        }
        w.WriteBit(r.ReadBit()); // sps_temporal_mvp_enabled_flag
        w.WriteBit(r.ReadBit()); // strong_intra_smoothing_enabled_flag

        w.WriteBit(1);      // vui_parameters_present_flag
        w.WriteBit(1);      // aspect_ratio_info_present_flag
        w.WriteBits(1, 8);  // aspect_ratio_idc = 1:1
        w.WriteBit(0);      // overscan_info_present_flag
        w.WriteBit(1);      // video_signal_type_present_flag
        w.WriteBits(5, 3);  // video_format = unspecified
        w.WriteBit(1);      // video_full_range_flag = 1 (0..255)
        w.WriteBit(1);      // colour_description_present_flag
        w.WriteBits(1, 8);  // colour_primaries = BT.709
        w.WriteBits(13, 8); // transfer_characteristics = sRGB
        w.WriteBits(1, 8);  // matrix_coefficients = BT.709
        w.WriteBit(0);      // chroma_loc_info_present_flag
        w.WriteBit(0);      // neutral_chroma_indication_flag
        w.WriteBit(0);      // field_seq_flag
        w.WriteBit(0);      // frame_field_info_present_flag
        w.WriteBit(0);      // default_display_window_flag
        w.WriteBit(0);      // vui_timing_info_present_flag
        w.WriteBit(0);      // bitstream_restriction_flag
        w.WriteBit(0);      // sps_extension_present_flag

        w.WriteTrailingBits();
        return EscapeRbsp(w.bytes, 2);
    }

    void SplitAnnexB(
        const uint8_t* data,
        size_t size,
        std::vector<std::pair<const uint8_t*, size_t>>& nalus)
    {
        nalus.clear();
        auto findStart = [&](size_t from, size_t& scLen) noexcept -> size_t
        {
            for (size_t p = from; p + 3 <= size; ++p)
            {
                if (data[p] == 0 && data[p + 1] == 0)
                {
                    if (data[p + 2] == 1) { scLen = 3; return p; }
                    if (p + 4 <= size && data[p + 2] == 0 && data[p + 3] == 1) { scLen = 4; return p; }
                }
            }
            return size;
        };
        size_t scLen = 0;
        size_t pos = findStart(0, scLen);
        while (pos < size)
        {
            const size_t nalStart = pos + scLen;
            size_t nextScLen = 0;
            const size_t nextPos = findStart(nalStart, nextScLen);
            size_t nalEnd = nextPos;
            while (nalEnd > nalStart && data[nalEnd - 1] == 0) --nalEnd;
            if (nalEnd > nalStart) nalus.emplace_back(data + nalStart, nalEnd - nalStart);
            pos = nextPos;
            scLen = nextScLen;
        }
    }
}
