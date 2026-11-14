#include "pch.h"
#include "Mp4Muxer.h"
#include "VideoBitstream.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <omp.h>
#include <stdexcept>
#if defined(_M_ARM64)
#include <arm64_neon.h>
#endif

namespace Mandelbrot::detail
{
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

    uint64_t Mp4FileWriter::Pos()
    {
        return static_cast<uint64_t>(m_file.tellp());
    }

    void Mp4FileWriter::Bytes(const void* data, size_t n)
    {
        m_file.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    }

    void Mp4FileWriter::Four(const char* four)
    {
        Bytes(four, 4);
    }

    void Mp4FileWriter::U8(uint8_t n)
    {
        Bytes(&n, 1);
    }

    void Mp4FileWriter::U16(uint16_t n)
    {
        const uint8_t b[]{ static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n) };
        Bytes(b, 2);
    }

    void Mp4FileWriter::U32(uint32_t n)
    {
        const uint8_t b[]{
            static_cast<uint8_t>(n >> 24),
            static_cast<uint8_t>(n >> 16),
            static_cast<uint8_t>(n >> 8),
            static_cast<uint8_t>(n)
        };
        Bytes(b, 4);
    }

    void Mp4FileWriter::U64(uint64_t n)
    {
        U32(static_cast<uint32_t>(n >> 32));
        U32(static_cast<uint32_t>(n));
    }

    void Mp4FileWriter::Zero(size_t n)
    {
        std::vector<char> zeros(n, 0);
        Bytes(zeros.data(), n);
    }

    void Mp4FileWriter::Patch(uint64_t pos, uint32_t n)
    {
        const auto end = Pos();
        m_file.seekp(pos);
        U32(n);
        m_file.seekp(end);
    }

    uint64_t Mp4FileWriter::Box(const char* kind)
    {
        const auto p = Pos();
        U32(0);
        Four(kind);
        return p;
    }

    void Mp4FileWriter::End(uint64_t p)
    {
        Patch(p, static_cast<uint32_t>(Pos() - p));
    }

    void Mp4FileWriter::Matrix()
    {
        U32(1u << 16);
        Zero(12);
        U32(1u << 16);
        Zero(12);
        U32(1u << 30);
    }

    void Mp4FileWriter::Open(const std::wstring& path, MovieCodec codec)
    {
        m_entries.clear();
        m_file.exceptions(std::ios::failbit | std::ios::badbit);
        m_file.open(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        const auto ftyp = Box("ftyp");
        Four("isom"); U32(512); Four("isom"); Four("mp42");
        Four(codec == MovieCodec::Hevc ? "hvc1" : "avc1");
        End(ftyp);
        m_mediaPos = Pos();
        U32(1);
        Four("mdat");
        U64(0); // 64-bit mdat supports movies above 4 GB.
    }

    void Mp4FileWriter::WriteSample(const uint8_t* data, size_t size, int32_t ctsOffset, bool keyframe)
    {
        m_entries.push_back({
            Pos(),
            static_cast<uint32_t>(size),
            ctsOffset,
            keyframe
        });
        Bytes(data, size);
    }

    void Mp4FileWriter::Finalize(
        int width,
        int height,
        int fps,
        MovieCodec codec,
        const std::vector<uint8_t>& vps,
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps)
    {
        if (m_entries.empty() || sps.empty() || pps.empty())
            throw std::runtime_error("Video encoder did not produce valid MP4 stream headers.");

        const auto end = Pos();
        m_file.seekp(m_mediaPos + 8);
        U64(end - m_mediaPos);
        m_file.seekp(end);

        const auto count = static_cast<uint32_t>(m_entries.size());
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

        const auto sampleEntry = Box(codec == MovieCodec::Hevc ? "hvc1" : "avc1");
        Zero(6); U16(1); Zero(16);
        U16(static_cast<uint16_t>(width)); U16(static_cast<uint16_t>(height));
        U32(72u << 16); U32(72u << 16); U32(0); U16(1); Zero(32); U16(24); U16(0xffff);

        if (codec == MovieCodec::H264)
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

        const bool hasCompositionOffsets = std::any_of(m_entries.begin(), m_entries.end(),
            [](const Mp4SampleEntry& e) { return e.ctsOffset != 0; });
        if (hasCompositionOffsets)
        {
            const auto ctts = Box("ctts");
            U32(0x01000000u); // version 1 signed offsets
            U32(count);
            for (const auto& e : m_entries)
            {
                U32(1);
                U32(static_cast<uint32_t>(e.ctsOffset));
            }
            End(ctts);
        }

        std::vector<uint32_t> syncSamples;
        syncSamples.reserve(m_entries.size());
        for (uint32_t i = 0; i < count; ++i)
            if (m_entries[i].keyframe) syncSamples.push_back(i + 1);
        const auto stss = Box("stss");
        U32(0);
        U32(static_cast<uint32_t>(syncSamples.size()));
        for (uint32_t syncIdx : syncSamples) U32(syncIdx);
        End(stss);

        const auto stsc = Box("stsc"); U32(0); U32(1); U32(1); U32(1); U32(1); End(stsc);
        const auto stsz = Box("stsz"); U32(0); U32(0); U32(count);
        for (const auto& e : m_entries) U32(e.bytes);
        End(stsz);
        const auto co64 = Box("co64"); U32(0); U32(count);
        for (const auto& e : m_entries) U64(e.offset);
        End(co64);

        End(stbl); End(minf); End(mdia); End(trak); End(moov);
        m_file.flush();
        m_file.close();
    }
}
