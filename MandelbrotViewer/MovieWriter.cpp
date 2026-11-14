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
#include "Mp4Muxer.h"
#include "VideoBitstream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace Mandelbrot
{
    using Microsoft::WRL::ComPtr;
    using namespace detail;

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

        Mp4FileWriter muxer;
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

        std::vector<uint8_t> vps, sps, pps;
        std::vector<std::pair<const uint8_t*, size_t>> scratchNalus;

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
            if (!muxer.HasSamples()) isKey = true;

            const int64_t dtsIndex = static_cast<int64_t>(muxer.SampleCount());
            int64_t ptsIndex = dtsIndex;
            LONGLONG sampleTime = 0;
            if (SUCCEEDED(sample->GetSampleTime(&sampleTime)))
                ptsIndex = std::llround(static_cast<double>(sampleTime) * fps / 10'000'000.0);

            muxer.WriteSample(
                mp4Sample.data(),
                mp4Sample.size(),
                static_cast<int32_t>(ptsIndex - dtsIndex),
                isKey);
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

            muxer.Open(path, activeCodec);
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

            muxer.Finalize(width, height, fps, activeCodec, vps, sps, pps);
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
