#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "MovieWriter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <omp.h>
#include <sstream>
#include <stdexcept>
#include <utility>
#if defined(_M_ARM64)
#include <arm64_neon.h>
#endif

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace Mandelbrot
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        void Check(HRESULT hr, const char* context = "Movie video encoding failed")
        {
            if (FAILED(hr))
            {
                std::ostringstream text;
                text << context << " (0x" << std::hex << static_cast<uint32_t>(hr) << ").";
                throw std::runtime_error(text.str());
            }
        }

        struct Apartment
        {
            HRESULT status{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
            Apartment() { if (status != RPC_E_CHANGED_MODE) Check(status, "COM initialization failed"); }
            ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
        };

        struct MediaFoundationSession
        {
            HRESULT status{MFStartup(MF_VERSION)};
            MediaFoundationSession() { Check(status, "Media Foundation initialization failed"); }
            ~MediaFoundationSession() { if (SUCCEEDED(status)) MFShutdown(); }
        };

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

        // Rewrite H.264 SPS frame cropping to match exact visual dimensions (supporting
        // small frames padded to hardware MFT minimums) and tag full-range BT.709/sRGB VUI.
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

        // Rewrite HEVC SPS conformance window to match exact visual dimensions and tag
        // full-range BT.709/sRGB VUI.
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

        void SplitAnnexB(const uint8_t* data, size_t size, std::vector<std::pair<const uint8_t*, size_t>>& nalus)
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

        // Parallel SIMD full-range BT.709 BGRA -> NV12 conversion directly into the locked MFT sample buffer.
        void ConvertBgraToNv12FullRangeBT709(
            const uint32_t* bgra, int srcW, int srcH,
            uint8_t* nv12, int codedW, int codedH) noexcept
        {
            constexpr double kr = 0.2126;
            constexpr double kb = 0.0722;
            constexpr int32_t kOne = 1 << 20;
            constexpr int32_t kHalf = 1 << 19;
            constexpr int32_t kChromaPos = 1 << 17; // 0.5 / 4 in Q20
            const int32_t yR = static_cast<int32_t>(std::lround(kr * kOne));
            const int32_t yB = static_cast<int32_t>(std::lround(kb * kOne));
            const int32_t yG = kOne - yR - yB;
            const int32_t cbR = static_cast<int32_t>(std::lround((0.5 * kr / (4.0 * (1.0 - kb))) * kOne));
            const int32_t cbG = kChromaPos - cbR;
            const int32_t crB = static_cast<int32_t>(std::lround((0.5 * kb / (4.0 * (1.0 - kr))) * kOne));
            const int32_t crG = kChromaPos - crB;
            constexpr int32_t kChromaBias = (128 << 20) + kHalf;

            uint8_t* yPlane = nv12;
            uint8_t* uvPlane = nv12 + static_cast<size_t>(codedW) * codedH;
            const int chromaRows = codedH / 2;

            #pragma omp parallel for schedule(static) if (codedW * codedH >= 65536)
            for (int cy = 0; cy < chromaRows; ++cy)
            {
                const int y0 = cy * 2;
                const int y1 = y0 + 1;
                const uint32_t* r0 = bgra + static_cast<size_t>(std::min(y0, srcH - 1)) * srcW;
                const uint32_t* r1 = bgra + static_cast<size_t>(std::min(y1, srcH - 1)) * srcW;
                uint8_t* yOut0 = yPlane + static_cast<size_t>(y0) * codedW;
                uint8_t* yOut1 = yPlane + static_cast<size_t>(y1) * codedW;
                uint8_t* uvOut = uvPlane + static_cast<size_t>(cy) * codedW;

                int x = 0;
#if defined(_M_ARM64)
                auto computeY4 = [&](uint16x4_t rv, uint16x4_t gv, uint16x4_t bv) noexcept -> uint16x4_t
                {
                    uint32x4_t acc = vdupq_n_u32(static_cast<uint32_t>(kHalf));
                    acc = vmlaq_n_u32(acc, vmovl_u16(rv), static_cast<uint32_t>(yR));
                    acc = vmlaq_n_u32(acc, vmovl_u16(gv), static_cast<uint32_t>(yG));
                    acc = vmlaq_n_u32(acc, vmovl_u16(bv), static_cast<uint32_t>(yB));
                    return vqmovn_u32(vshrq_n_u32(acc, 20));
                };
                auto computeY16 = [&](const uint8x16x4_t& px) noexcept -> uint8x16_t
                {
                    const uint16x8_t bLo = vmovl_u8(vget_low_u8(px.val[0]));
                    const uint16x8_t bHi = vmovl_u8(vget_high_u8(px.val[0]));
                    const uint16x8_t gLo = vmovl_u8(vget_low_u8(px.val[1]));
                    const uint16x8_t gHi = vmovl_u8(vget_high_u8(px.val[1]));
                    const uint16x8_t rLo = vmovl_u8(vget_low_u8(px.val[2]));
                    const uint16x8_t rHi = vmovl_u8(vget_high_u8(px.val[2]));
                    const uint16x8_t yLo = vcombine_u16(
                        computeY4(vget_low_u16(rLo), vget_low_u16(gLo), vget_low_u16(bLo)),
                        computeY4(vget_high_u16(rLo), vget_high_u16(gLo), vget_high_u16(bLo)));
                    const uint16x8_t yHi = vcombine_u16(
                        computeY4(vget_low_u16(rHi), vget_low_u16(gHi), vget_low_u16(bHi)),
                        computeY4(vget_high_u16(rHi), vget_high_u16(gHi), vget_high_u16(bHi)));
                    return vcombine_u8(vqmovn_u16(yLo), vqmovn_u16(yHi));
                };
                auto computeCb4 = [&](uint16x4_t rs4, uint16x4_t gs4, uint16x4_t bs4) noexcept -> uint16x4_t
                {
                    int32x4_t acc = vdupq_n_s32(kChromaBias);
                    acc = vmlaq_n_s32(acc, vreinterpretq_s32_u32(vmovl_u16(bs4)), kChromaPos);
                    acc = vmlsq_n_s32(acc, vreinterpretq_s32_u32(vmovl_u16(rs4)), cbR);
                    acc = vmlsq_n_s32(acc, vreinterpretq_s32_u32(vmovl_u16(gs4)), cbG);
                    return vqmovun_s32(vshrq_n_s32(acc, 20));
                };
                auto computeCr4 = [&](uint16x4_t rs4, uint16x4_t gs4, uint16x4_t bs4) noexcept -> uint16x4_t
                {
                    int32x4_t acc = vdupq_n_s32(kChromaBias);
                    acc = vmlaq_n_s32(acc, vreinterpretq_s32_u32(vmovl_u16(rs4)), kChromaPos);
                    acc = vmlsq_n_s32(acc, vreinterpretq_s32_u32(vmovl_u16(gs4)), crG);
                    acc = vmlsq_n_s32(acc, vreinterpretq_s32_u32(vmovl_u16(bs4)), crB);
                    return vqmovun_s32(vshrq_n_s32(acc, 20));
                };

                for (; x + 16 <= srcW && x + 16 <= codedW; x += 16)
                {
                    const uint8x16x4_t bgra0 = vld4q_u8(reinterpret_cast<const uint8_t*>(r0 + x));
                    const uint8x16x4_t bgra1 = vld4q_u8(reinterpret_cast<const uint8_t*>(r1 + x));
                    vst1q_u8(yOut0 + x, computeY16(bgra0));
                    vst1q_u8(yOut1 + x, computeY16(bgra1));

                    const uint16x8_t bSum = vpadalq_u8(vpaddlq_u8(bgra0.val[0]), bgra1.val[0]);
                    const uint16x8_t gSum = vpadalq_u8(vpaddlq_u8(bgra0.val[1]), bgra1.val[1]);
                    const uint16x8_t rSum = vpadalq_u8(vpaddlq_u8(bgra0.val[2]), bgra1.val[2]);
                    const uint16x8_t cb16 = vcombine_u16(
                        computeCb4(vget_low_u16(rSum), vget_low_u16(gSum), vget_low_u16(bSum)),
                        computeCb4(vget_high_u16(rSum), vget_high_u16(gSum), vget_high_u16(bSum)));
                    const uint16x8_t cr16 = vcombine_u16(
                        computeCr4(vget_low_u16(rSum), vget_low_u16(gSum), vget_low_u16(bSum)),
                        computeCr4(vget_high_u16(rSum), vget_high_u16(gSum), vget_high_u16(bSum)));

                    uint8x8x2_t uv;
                    uv.val[0] = vqmovn_u16(cb16);
                    uv.val[1] = vqmovn_u16(cr16);
                    vst2_u8(uvOut + x, uv);
                }
#endif
                for (; x < codedW; x += 2)
                {
                    const int x0 = std::min(x, srcW - 1);
                    const int x1 = std::min(x + 1, srcW - 1);
                    const uint32_t c00 = r0[x0];
                    const uint32_t c01 = r0[x1];
                    const uint32_t c10 = r1[x0];
                    const uint32_t c11 = r1[x1];

                    const int32_t b00 = static_cast<int32_t>(c00 & 255);
                    const int32_t g00 = static_cast<int32_t>((c00 >> 8) & 255);
                    const int32_t r00 = static_cast<int32_t>((c00 >> 16) & 255);
                    const int32_t b01 = static_cast<int32_t>(c01 & 255);
                    const int32_t g01 = static_cast<int32_t>((c01 >> 8) & 255);
                    const int32_t r01 = static_cast<int32_t>((c01 >> 16) & 255);
                    const int32_t b10 = static_cast<int32_t>(c10 & 255);
                    const int32_t g10 = static_cast<int32_t>((c10 >> 8) & 255);
                    const int32_t r10 = static_cast<int32_t>((c10 >> 16) & 255);
                    const int32_t b11 = static_cast<int32_t>(c11 & 255);
                    const int32_t g11 = static_cast<int32_t>((c11 >> 8) & 255);
                    const int32_t r11 = static_cast<int32_t>((c11 >> 16) & 255);

                    yOut0[x]     = static_cast<uint8_t>((yR * r00 + yG * g00 + yB * b00 + kHalf) >> 20);
                    yOut0[x + 1] = static_cast<uint8_t>((yR * r01 + yG * g01 + yB * b01 + kHalf) >> 20);
                    yOut1[x]     = static_cast<uint8_t>((yR * r10 + yG * g10 + yB * b10 + kHalf) >> 20);
                    yOut1[x + 1] = static_cast<uint8_t>((yR * r11 + yG * g11 + yB * b11 + kHalf) >> 20);

                    const int32_t bs = b00 + b01 + b10 + b11;
                    const int32_t gs = g00 + g01 + g10 + g11;
                    const int32_t rs = r00 + r01 + r10 + r11;
                    const int32_t cbVal = (kChromaBias + kChromaPos * bs - cbR * rs - cbG * gs) >> 20;
                    const int32_t crVal = (kChromaBias + kChromaPos * rs - crG * gs - crB * bs) >> 20;
                    uvOut[x]     = static_cast<uint8_t>(std::clamp(cbVal, 0, 255));
                    uvOut[x + 1] = static_cast<uint8_t>(std::clamp(crVal, 0, 255));
                }
            }
        }
    }

    struct MovieWriter::Impl
    {
        Apartment apartment;
        MediaFoundationSession mfSession;
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        ComPtr<IMFDXGIDeviceManager> dxgiManager;
        ComPtr<IMFTransform> mft;
        ComPtr<IMFMediaEventGenerator> eventGen;
        IMFActivate* chosenActivate{};

        std::fstream file;
        int width{}, height{}, codedW{}, codedH{}, fps{};
        MovieCodec activeCodec{MovieCodec::H264};
        bool isHardware{};
        bool isAsync{};
        bool providesSamples{};
        bool streamingStarted{};
        bool finished{};
        DWORD inId{}, outId{};
        DWORD outBufSize{4 * 1024 * 1024};
        uint32_t needInputCount{};
        uint64_t submittedFrames{};
        uint64_t mediaPos{};

        struct Entry
        {
            uint64_t offset{};
            uint32_t bytes{};
            int32_t ctsOffset{};
            bool keyframe{};
        };
        std::vector<Entry> entries;
        std::vector<uint8_t> vps, sps, pps;
        std::vector<std::pair<const uint8_t*, size_t>> scratchNalus;

        uint64_t Pos() { return static_cast<uint64_t>(file.tellp()); }
        void Bytes(const void* data, size_t n) { file.write(static_cast<const char*>(data), static_cast<std::streamsize>(n)); }
        void Four(const char* four) { Bytes(four, 4); }
        void U8(uint8_t n) { Bytes(&n, 1); }
        void U16(uint16_t n) { const uint8_t b[]{static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)}; Bytes(b, 2); }
        void U32(uint32_t n) { const uint8_t b[]{static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)}; Bytes(b, 4); }
        void U64(uint64_t n) { U32(static_cast<uint32_t>(n >> 32)); U32(static_cast<uint32_t>(n)); }
        void Zero(size_t n) { std::vector<char> zeros(n, 0); Bytes(zeros.data(), n); }
        void Patch(uint64_t pos, uint32_t n) { const auto end = Pos(); file.seekp(pos); U32(n); file.seekp(end); }
        uint64_t Box(const char* kind) { const auto p = Pos(); U32(0); Four(kind); return p; }
        void End(uint64_t p) { Patch(p, static_cast<uint32_t>(Pos() - p)); }
        void Matrix() { U32(1u << 16); Zero(12); U32(1u << 16); Zero(12); U32(1u << 30); }

        bool ConfigureMftTypes(IMFTransform* candidate, DWORD candIn, DWORD candOut, MovieCodec codec, int tryW, int tryH)
        {
            const GUID subtype = (codec == MovieCodec::Hevc) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
            const uint64_t pixelsPerSec = static_cast<uint64_t>(tryW) * tryH * fps;
            const UINT32 idealBitrate = static_cast<UINT32>(
                std::clamp<uint64_t>(pixelsPerSec * 3 / 5, 6'000'000ULL, 150'000'000ULL));
            const std::array<UINT32, 3> bitrateCandidates{{
                idealBitrate,
                std::min<UINT32>(idealBitrate, 100'000'000u),
                std::min<UINT32>(idealBitrate, 50'000'000u)}};

            auto buildOutput = [&](UINT32 bitrate, bool withProfile, bool withColor) -> ComPtr<IMFMediaType>
            {
                ComPtr<IMFMediaType> outType;
                Check(MFCreateMediaType(&outType));
                outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outType->SetGUID(MF_MT_SUBTYPE, subtype);
                outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
                outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(std::max(1, fps * 2)));
                MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(tryW), static_cast<UINT32>(tryH));
                MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1u);
                MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1u, 1u);
                if (withProfile && codec == MovieCodec::H264)
                    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
                if (withColor)
                {
                    outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
                    outType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
                    outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
                    outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
                }
                return outType;
            };

            bool outputConfigured = false;
            UINT32 prevBitrate = 0;
            for (UINT32 bitrate : bitrateCandidates)
            {
                if (bitrate == prevBitrate) continue;
                prevBitrate = bitrate;
                for (auto [withProf, withColor] : {
                    std::pair{true, true}, std::pair{false, true}, std::pair{true, false}, std::pair{false, false}})
                {
                    auto outType = buildOutput(bitrate, withProf, withColor);
                    if (SUCCEEDED(candidate->SetOutputType(candOut, outType.Get(), 0)))
                    {
                        outputConfigured = true;
                        break;
                    }
                }
                if (outputConfigured) break;
            }
            if (!outputConfigured) return false;

            ComPtr<IMFMediaType> inType;
            Check(MFCreateMediaType(&inType));
            inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(tryW), static_cast<UINT32>(tryH));
            MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1u);
            MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1u, 1u);
            inType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            inType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
            inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            inType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);

            if (FAILED(candidate->SetInputType(candIn, inType.Get(), 0)))
            {
                inType->DeleteItem(MF_MT_VIDEO_NOMINAL_RANGE);
                if (FAILED(candidate->SetInputType(candIn, inType.Get(), 0)))
                    return false;
            }
            return true;
        }

        bool TryInitEncoder(MovieCodec codec, bool hw)
        {
            const GUID subtype = (codec == MovieCodec::Hevc) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
            const int minDim = hw ? 96 : 64;
            const int evenW = std::max(minDim, (width + 1) & ~1);
            const int evenH = std::max(minDim, (height + 1) & ~1);
            const int mbW = (evenW + 15) & ~15;
            const int mbH = (evenH + 15) & ~15;

            MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, subtype};
            const UINT32 flags = hw
                ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
                : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &activates, &count)) || count == 0)
                return false;

            const std::array<std::pair<int, int>, 2> dimCandidates{{{evenW, evenH}, {mbW, mbH}}};
            const size_t numDimCandidates = (evenW == mbW && evenH == mbH) ? 1u : 2u;

            for (UINT32 i = 0; i < count; ++i)
            {
                for (size_t d = 0; d < numDimCandidates; ++d)
                {
                    const auto [tryW, tryH] = dimCandidates[d];
                    ComPtr<IMFTransform> candidate;
                    if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate))))
                        break;

                    ComPtr<IMFAttributes> attrs;
                    UINT32 asyncFlag = 0;
                    if (SUCCEEDED(candidate->GetAttributes(&attrs)) && attrs)
                    {
                        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
                        if (asyncFlag) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                        UINT32 d3dAware = 0;
                        attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
                        if (hw && d3dAware && dxgiManager)
                        {
                            candidate->ProcessMessage(
                                MFT_MESSAGE_SET_D3D_MANAGER,
                                reinterpret_cast<ULONG_PTR>(dxgiManager.Get()));
                        }
                    }

                    DWORD candIn = 0, candOut = 0;
                    if (FAILED(candidate->GetStreamIDs(1, &candIn, 1, &candOut)))
                    {
                        candIn = 0;
                        candOut = 0;
                    }

                    ComPtr<ICodecAPI> codecApi;
                    if (SUCCEEDED(candidate.As(&codecApi)))
                    {
                        VARIANT bFrames{};
                        bFrames.vt = VT_UI4;
                        bFrames.ulVal = 0;
                        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &bFrames);

                        VARIANT lowLat{};
                        lowLat.vt = VT_BOOL;
                        lowLat.boolVal = VARIANT_TRUE;
                        codecApi->SetValue(&CODECAPI_AVEncCommonLowLatency, &lowLat);
                    }

                    if (!ConfigureMftTypes(candidate.Get(), candIn, candOut, codec, tryW, tryH))
                    {
                        candidate.Reset();
                        activates[i]->ShutdownObject();
                        continue;
                    }

                    codedW = tryW;
                    codedH = tryH;
                    activeCodec = codec;
                    isHardware = hw;
                    isAsync = (asyncFlag != 0);
                    inId = candIn;
                    outId = candOut;
                    mft = candidate;
                    if (isAsync) Check(mft.As(&eventGen));

                    UpdateOutputStreamInfo();
                    CaptureSequenceHeader();
                    Check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
                    Check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
                    streamingStarted = true;

                    chosenActivate = activates[i];
                    chosenActivate->AddRef();
                    for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
                    CoTaskMemFree(activates);
                    return true;
                }
            }

            for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
            CoTaskMemFree(activates);
            return false;
        }

        void UpdateOutputStreamInfo()
        {
            MFT_OUTPUT_STREAM_INFO osi{};
            if (SUCCEEDED(mft->GetOutputStreamInfo(outId, &osi)))
            {
                providesSamples = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                const DWORD minFallback = static_cast<DWORD>(std::max<size_t>(4 * 1024 * 1024, static_cast<size_t>(codedW) * codedH));
                outBufSize = std::max<DWORD>(osi.cbSize, minFallback);
            }
        }

        void AbsorbParamNal(const uint8_t* p, size_t n)
        {
            if (n == 0) return;
            if (activeCodec == MovieCodec::H264)
            {
                const uint8_t nalType = p[0] & 0x1Fu;
                if (nalType == 7) sps = PatchH264Sps(p, n, width, height);
                else if (nalType == 8) pps.assign(p, p + n);
            }
            else
            {
                const uint8_t nalType = (p[0] >> 1) & 0x3Fu;
                if (nalType == 32) vps.assign(p, p + n);
                else if (nalType == 33) sps = PatchHevcSps(p, n, width, height);
                else if (nalType == 34) pps.assign(p, p + n);
            }
        }

        void CaptureSequenceHeader()
        {
            ComPtr<IMFMediaType> curOut;
            if (FAILED(mft->GetOutputCurrentType(outId, &curOut))) return;
            UINT32 blobLen = 0;
            if (FAILED(curOut->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobLen)) || blobLen == 0) return;
            std::vector<uint8_t> blob(blobLen);
            if (FAILED(curOut->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), blobLen, &blobLen))) return;
            SplitAnnexB(blob.data(), blobLen, scratchNalus);
            for (const auto& [ptr, len] : scratchNalus)
                AbsorbParamNal(ptr, len);
        }

        void HandleEncodedSample(IMFSample* sample)
        {
            if (!sample) return;
            ComPtr<IMFMediaBuffer> buf;
            Check(sample->ConvertToContiguousBuffer(&buf));
            BYTE* raw = nullptr;
            DWORD len = 0;
            Check(buf->Lock(&raw, nullptr, &len));
            struct UnlockGuard { IMFMediaBuffer* b; ~UnlockGuard() { b->Unlock(); } } unlock{buf.Get()};
            if (len == 0) return;

            SplitAnnexB(raw, len, scratchNalus);

            std::vector<uint8_t> mp4Sample;
            mp4Sample.reserve(static_cast<size_t>(len) + 16);
            bool isKey = false;
            UINT32 cleanPoint = 0;
            if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0)
                isKey = true;

            for (const auto& [ptr, n] : scratchNalus)
            {
                if (activeCodec == MovieCodec::H264)
                {
                    const uint8_t nalType = ptr[0] & 0x1Fu;
                    if (nalType == 7 || nalType == 8) { AbsorbParamNal(ptr, n); continue; }
                    if (nalType == 9) continue;
                    if (nalType == 5) isKey = true;
                }
                else
                {
                    const uint8_t nalType = (ptr[0] >> 1) & 0x3Fu;
                    if (nalType == 32 || nalType == 33 || nalType == 34) { AbsorbParamNal(ptr, n); continue; }
                    if (nalType == 35) continue;
                    if (nalType >= 16 && nalType <= 21) isKey = true;
                }

                const uint32_t sz = static_cast<uint32_t>(n);
                mp4Sample.push_back(static_cast<uint8_t>(sz >> 24));
                mp4Sample.push_back(static_cast<uint8_t>(sz >> 16));
                mp4Sample.push_back(static_cast<uint8_t>(sz >> 8));
                mp4Sample.push_back(static_cast<uint8_t>(sz));
                mp4Sample.insert(mp4Sample.end(), ptr, ptr + n);
            }

            if (mp4Sample.empty()) return;
            if (entries.empty()) isKey = true;

            const int64_t dtsIndex = static_cast<int64_t>(entries.size());
            int64_t ptsIndex = dtsIndex;
            LONGLONG sampleTime = 0;
            if (SUCCEEDED(sample->GetSampleTime(&sampleTime)))
                ptsIndex = std::llround(static_cast<double>(sampleTime) * fps / 10'000'000.0);

            entries.push_back({
                Pos(),
                static_cast<uint32_t>(mp4Sample.size()),
                static_cast<int32_t>(ptsIndex - dtsIndex),
                isKey});
            Bytes(mp4Sample.data(), mp4Sample.size());
        }

        void PullOneAsyncOutput()
        {
            MFT_OUTPUT_DATA_BUFFER odb{};
            ComPtr<IMFSample> allocSample;
            if (!providesSamples)
            {
                ComPtr<IMFMediaBuffer> obuf;
                Check(MFCreateMemoryBuffer(outBufSize, &obuf));
                Check(MFCreateSample(&allocSample));
                Check(allocSample->AddBuffer(obuf.Get()));
                odb.pSample = allocSample.Get();
            }

            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) odb.pEvents->Release();
            struct SampleReleaseGuard
            {
                bool release;
                IMFSample* s;
                ~SampleReleaseGuard() { if (release && s) s->Release(); }
            } relGuard{providesSamples, odb.pSample};

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                ComPtr<IMFMediaType> newOut;
                Check(mft->GetOutputAvailableType(outId, 0, &newOut));
                Check(mft->SetOutputType(outId, newOut.Get(), 0));
                UpdateOutputStreamInfo();
                CaptureSequenceHeader();
                return;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            Check(hr, "Hardware video encoder output failed");
            HandleEncodedSample(odb.pSample);
        }

        void WaitAsyncInputReady()
        {
            while (needInputCount == 0)
            {
                ComPtr<IMFMediaEvent> ev;
                Check(eventGen->GetEvent(0, &ev), "Hardware video encoder event wait failed");
                MediaEventType met = MEUnknown;
                Check(ev->GetType(&met));
                HRESULT evStatus = S_OK;
                ev->GetStatus(&evStatus);
                Check(evStatus, "Hardware video encoder reported error");
                if (met == METransformNeedInput) ++needInputCount;
                else if (met == METransformHaveOutput) PullOneAsyncOutput();
                else if (met == MEError) Check(E_FAIL, "Hardware video encoder stream error");
            }

            for (;;)
            {
                ComPtr<IMFMediaEvent> ev;
                const HRESULT hr = eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
                if (hr == MF_E_NO_EVENTS_AVAILABLE || FAILED(hr) || !ev) break;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformNeedInput) ++needInputCount;
                else if (met == METransformHaveOutput) PullOneAsyncOutput();
            }
        }

        void DrainSyncOutputs()
        {
            for (;;)
            {
                MFT_OUTPUT_DATA_BUFFER odb{};
                ComPtr<IMFSample> allocSample;
                if (!providesSamples)
                {
                    ComPtr<IMFMediaBuffer> obuf;
                    Check(MFCreateMemoryBuffer(outBufSize, &obuf));
                    Check(MFCreateSample(&allocSample));
                    Check(allocSample->AddBuffer(obuf.Get()));
                    odb.pSample = allocSample.Get();
                }

                DWORD status = 0;
                const HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
                if (odb.pEvents) odb.pEvents->Release();
                struct SampleReleaseGuard
                {
                    bool release;
                    IMFSample* s;
                    ~SampleReleaseGuard() { if (release && s) s->Release(); }
                } relGuard{providesSamples, odb.pSample};

                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
                {
                    ComPtr<IMFMediaType> newOut;
                    Check(mft->GetOutputAvailableType(outId, 0, &newOut));
                    Check(mft->SetOutputType(outId, newOut.Get(), 0));
                    UpdateOutputStreamInfo();
                    CaptureSequenceHeader();
                    continue;
                }
                Check(hr, "Video encoder ProcessOutput failed");
                HandleEncodedSample(odb.pSample);
            }
        }

        Impl(const std::wstring& path, int w, int h, int rate, MovieCodec requestedCodec)
            : width(w), height(h), fps(rate), activeCodec(requestedCodec)
        {
            if (w < 1 || h < 1 || w > 7680 || h > 4320 || rate < 1 || rate > 120)
                throw std::invalid_argument("Invalid movie writer dimensions or frame rate.");

            if (SUCCEEDED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext)))
            {
                ComPtr<ID3D10Multithread> multithread;
                if (SUCCEEDED(d3dDevice.As(&multithread)))
                    multithread->SetMultithreadProtected(TRUE);
                UINT resetToken = 0;
                if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, &dxgiManager)))
                    dxgiManager->ResetDevice(d3dDevice.Get(), resetToken);
            }

            const MovieCodec alternateCodec = (requestedCodec == MovieCodec::Hevc)
                ? MovieCodec::H264
                : MovieCodec::Hevc;
            if (!TryInitEncoder(requestedCodec, true) &&
                !TryInitEncoder(alternateCodec, true) &&
                !TryInitEncoder(requestedCodec, false) &&
                !TryInitEncoder(alternateCodec, false))
            {
                throw std::runtime_error("No compatible H.264 or HEVC encoder is available for the requested movie dimensions.");
            }

            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
            const auto ftyp = Box("ftyp");
            Four("isom"); U32(512); Four("isom"); Four("mp42");
            Four(activeCodec == MovieCodec::Hevc ? "hvc1" : "avc1");
            End(ftyp);
            mediaPos = Pos(); U32(1); Four("mdat"); U64(0); // 64-bit mdat supports movies above 4 GB.
        }

        ~Impl()
        {
            Apartment threadApartment;
            if (mft && streamingStarted && !finished)
            {
                mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            }
            eventGen.Reset();
            mft.Reset();
            if (chosenActivate)
            {
                chosenActivate->ShutdownObject();
                chosenActivate->Release();
                chosenActivate = nullptr;
            }
            dxgiManager.Reset();
            d3dContext.Reset();
            d3dDevice.Reset();
        }

        void Write(const std::vector<uint32_t>& pixels)
        {
            if (finished || pixels.size() != static_cast<size_t>(width) * height)
                throw std::invalid_argument("Movie frame dimensions do not match the stream.");
            if (submittedFrames == 3600ULL * 120ULL)
                throw std::runtime_error("Movie exceeds the supported duration.");

            Apartment threadApartment;
            const DWORD nv12Bytes = static_cast<DWORD>(static_cast<size_t>(codedW) * codedH * 3 / 2);
            ComPtr<IMFMediaBuffer> buffer;
            Check(MFCreateAlignedMemoryBuffer(nv12Bytes, MF_16_BYTE_ALIGNMENT, &buffer));

            BYTE* dst = nullptr;
            Check(buffer->Lock(&dst, nullptr, nullptr));
            ConvertBgraToNv12FullRangeBT709(pixels.data(), width, height, dst, codedW, codedH);
            Check(buffer->Unlock());
            Check(buffer->SetCurrentLength(nv12Bytes));

            ComPtr<IMFSample> sample;
            Check(MFCreateSample(&sample));
            Check(sample->AddBuffer(buffer.Get()));
            const LONGLONG startTicks = static_cast<LONGLONG>(submittedFrames * 10'000'000ULL / static_cast<uint64_t>(fps));
            const LONGLONG nextTicks = static_cast<LONGLONG>((submittedFrames + 1) * 10'000'000ULL / static_cast<uint64_t>(fps));
            Check(sample->SetSampleTime(startTicks));
            Check(sample->SetSampleDuration(nextTicks - startTicks));
            ++submittedFrames;

            if (isAsync)
            {
                WaitAsyncInputReady();
                --needInputCount;
                Check(mft->ProcessInput(inId, sample.Get(), 0), "Hardware video encoder input failed");
            }
            else
            {
                HRESULT hr = mft->ProcessInput(inId, sample.Get(), 0);
                if (hr == MF_E_NOTACCEPTING)
                {
                    DrainSyncOutputs();
                    hr = mft->ProcessInput(inId, sample.Get(), 0);
                }
                Check(hr, "Video encoder ProcessInput failed");
                DrainSyncOutputs();
            }
        }

        void Finish()
        {
            if (finished) return;
            if (submittedFrames == 0) throw std::runtime_error("Cannot save a movie with no frames.");

            Apartment threadApartment;
            Check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, inId));
            Check(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
            if (isAsync)
            {
                for (;;)
                {
                    ComPtr<IMFMediaEvent> ev;
                    Check(eventGen->GetEvent(0, &ev), "Hardware video encoder drain failed");
                    MediaEventType met = MEUnknown;
                    Check(ev->GetType(&met));
                    HRESULT evStatus = S_OK;
                    ev->GetStatus(&evStatus);
                    Check(evStatus, "Hardware video encoder drain error");
                    if (met == METransformHaveOutput) PullOneAsyncOutput();
                    else if (met == METransformDrainComplete) break;
                    else if (met == MEError) Check(E_FAIL, "Hardware video encoder failed during drain");
                }
            }
            else
            {
                DrainSyncOutputs();
            }
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            streamingStarted = false;

            if (entries.empty() || sps.empty() || pps.empty())
                throw std::runtime_error("Video encoder did not produce valid MP4 stream headers.");

            const auto end = Pos();
            file.seekp(mediaPos + 8); U64(end - mediaPos); file.seekp(end);

            const auto count = static_cast<uint32_t>(entries.size());
            const auto moov = Box("moov");
            const auto mvhd = Box("mvhd"); U32(0); Zero(8); U32(static_cast<uint32_t>(fps)); U32(count); U32(1u << 16); U16(0x100); Zero(10); Matrix(); Zero(24); U32(2); End(mvhd);
            const auto trak = Box("trak");
            const auto tkhd = Box("tkhd"); U32(3); Zero(8); U32(1); U32(0); U32(count); Zero(16); Matrix(); U32(static_cast<uint32_t>(width) << 16); U32(static_cast<uint32_t>(height) << 16); End(tkhd);
            const auto mdia = Box("mdia");
            const auto mdhd = Box("mdhd"); U32(0); Zero(8); U32(static_cast<uint32_t>(fps)); U32(count); U16(0x55c4); U16(0); End(mdhd);
            const auto hdlr = Box("hdlr"); U32(0); U32(0); Four("vide"); Zero(12); Bytes("Fractal\0", 8); End(hdlr);
            const auto minf = Box("minf");
            const auto vmhd = Box("vmhd"); U32(1); Zero(8); End(vmhd);
            const auto dinf = Box("dinf"); const auto dref = Box("dref"); U32(0); U32(1);
            const auto url = Box("url "); U32(1); End(url); End(dref); End(dinf);
            const auto stbl = Box("stbl"); const auto stsd = Box("stsd"); U32(0); U32(1);

            const auto sampleEntry = Box(activeCodec == MovieCodec::Hevc ? "hvc1" : "avc1");
            Zero(6); U16(1); Zero(16);
            U16(static_cast<uint16_t>(width)); U16(static_cast<uint16_t>(height));
            U32(72u << 16); U32(72u << 16); U32(0); U16(1); Zero(32); U16(24); U16(0xffff);

            if (activeCodec == MovieCodec::H264)
            {
                const auto avcC = Box("avcC");
                const uint8_t profileIdc = sps.size() > 1 ? sps[1] : 100;
                U8(1);
                U8(profileIdc);
                U8(sps.size() > 2 ? sps[2] : 0);
                U8(sps.size() > 3 ? sps[3] : 40);
                U8(0xFF); // 4-byte NAL length prefix
                U8(0xE1); // 1 SPS
                U16(static_cast<uint16_t>(sps.size()));
                Bytes(sps.data(), sps.size());
                U8(1); // 1 PPS
                U16(static_cast<uint16_t>(pps.size()));
                Bytes(pps.data(), pps.size());
                if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 || profileIdc == 244)
                {
                    U8(0xFD); // chroma_format = 1 (4:2:0)
                    U8(0xF8); // bit_depth_luma_minus8 = 0
                    U8(0xF8); // bit_depth_chroma_minus8 = 0
                    U8(0x00); // numOfSequenceParameterSetExt = 0
                }
                End(avcC);
            }
            else
            {
                const auto spsRbsp = UnescapeRbsp(sps.data(), sps.size());
                const auto hvcC = Box("hvcC");
                U8(1);
                if (spsRbsp.size() >= 15) Bytes(spsRbsp.data() + 3, 12);
                else { U8(0x01); U32(0x60000000u); U32(0xB0000000u); U16(0); U8(150); }
                U16(0xF000); // min_spatial_segmentation_idc = 0
                U8(0xFC);    // parallelismType = 0
                U8(0xFD);    // chromaFormat = 1 (4:2:0)
                U8(0xF8);    // bitDepthLumaMinus8 = 0
                U8(0xF8);    // bitDepthChromaMinus8 = 0
                U16(0);      // avgFrameRate = 0
                U8(0x0F);    // numTemporalLayers = 1, temporalIdNested = 1, lengthSizeMinusOne = 3
                const uint8_t numArrays =
                    static_cast<uint8_t>((vps.empty() ? 0 : 1) + (sps.empty() ? 0 : 1) + (pps.empty() ? 0 : 1));
                U8(numArrays);
                auto writeParamArray = [&](uint8_t nalType, const std::vector<uint8_t>& nal)
                {
                    if (nal.empty()) return;
                    U8(static_cast<uint8_t>(0x80u | nalType));
                    U16(1);
                    U16(static_cast<uint16_t>(nal.size()));
                    Bytes(nal.data(), nal.size());
                };
                writeParamArray(32, vps);
                writeParamArray(33, sps);
                writeParamArray(34, pps);
                End(hvcC);
            }

            // Explicit BT.709 primaries, sRGB transfer, BT.709 matrix, and full-range flag.
            const auto colr = Box("colr"); Four("nclx"); U16(1); U16(13); U16(1); U8(0x80); End(colr);
            End(sampleEntry); End(stsd);

            const auto stts = Box("stts"); U32(0); U32(1); U32(count); U32(1); End(stts);

            const bool hasCompositionOffsets = std::any_of(entries.begin(), entries.end(),
                [](const Entry& e) { return e.ctsOffset != 0; });
            if (hasCompositionOffsets)
            {
                const auto ctts = Box("ctts");
                U32(0x01000000u); // version 1 signed offsets
                U32(count);
                for (const auto& e : entries)
                {
                    U32(1);
                    U32(static_cast<uint32_t>(e.ctsOffset));
                }
                End(ctts);
            }

            std::vector<uint32_t> syncSamples;
            syncSamples.reserve(entries.size());
            for (uint32_t i = 0; i < count; ++i)
                if (entries[i].keyframe) syncSamples.push_back(i + 1);
            const auto stss = Box("stss");
            U32(0);
            U32(static_cast<uint32_t>(syncSamples.size()));
            for (uint32_t syncIdx : syncSamples) U32(syncIdx);
            End(stss);

            const auto stsc = Box("stsc"); U32(0); U32(1); U32(1); U32(1); U32(1); End(stsc);
            const auto stsz = Box("stsz"); U32(0); U32(0); U32(count);
            for (const auto& e : entries) U32(e.bytes);
            End(stsz);
            const auto co64 = Box("co64"); U32(0); U32(count);
            for (const auto& e : entries) U64(e.offset);
            End(co64);

            End(stbl); End(minf); End(mdia); End(trak); End(moov);
            file.flush();
            file.close();
            finished = true;
        }
    };

    MovieWriter::MovieWriter(const std::wstring& path, int width, int height, int fps, MovieCodec codec)
        : m_impl(std::make_unique<Impl>(path, width, height, fps, codec)) {}
    MovieWriter::~MovieWriter() = default;
    void MovieWriter::WriteFrame(const std::vector<uint32_t>& bgra) { m_impl->Write(bgra); }
    void MovieWriter::Finish() { m_impl->Finish(); }
    MovieCodec MovieWriter::ActiveCodec() const noexcept { return m_impl->activeCodec; }
    bool MovieWriter::IsHardwareAccelerated() const noexcept { return m_impl->isHardware; }
}
