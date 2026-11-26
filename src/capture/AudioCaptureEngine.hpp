#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <propidl.h>
#endif

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace LMAC {

enum class CaptureBackend : int {
    WASAPI_INPUT = 0,
    WASAPI_LOOPBACK = 1,
    MME_WAVEIN = 2
};

enum class ChannelMode : int {
    STEREO = 0,
    MONO_MIX = 1,
    MONO_L = 2,
    MONO_R = 3
};

enum class DurationCap : int {
    CAP_1M = 0,
    CAP_3M = 1,
    CAP_5M = 2,
    CAP_10M = 3,
    CAP_NONE = 4
};

inline double durationCapSeconds(DurationCap cap) {
    switch (cap) {
        case DurationCap::CAP_1M:   return 60.0;
        case DurationCap::CAP_3M:   return 180.0;
        case DurationCap::CAP_5M:   return 300.0;
        case DurationCap::CAP_10M:  return 600.0;
        case DurationCap::CAP_NONE: return 0.0;
    }
    return 300.0;
}

struct CaptureDeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct LiveWaveBucket {
    float minVal = 0.0f;
    float maxVal = 0.0f;
};

// Fixed-block chunked stereo buffer: avoids vector reallocation/memcpy spikes during long captures
class ChunkedStereoBuffer {
public:
    static constexpr size_t CHUNK_FRAMES = 65536;

    ChunkedStereoBuffer() {
        allocateChunk();
    }

    size_t size() const { return totalFrames; }
    bool empty() const { return totalFrames == 0; }

    void pushSample(float l, float r) {
        size_t cIdx = totalFrames / CHUNK_FRAMES;
        size_t offset = totalFrames % CHUNK_FRAMES;
        if (cIdx >= chunksL.size()) {
            allocateChunk();
        }
        chunksL[cIdx][offset] = l;
        chunksR[cIdx][offset] = r;
        ++totalFrames;
    }

    void appendSilence(size_t count) {
        while (count > 0) {
            size_t cIdx = totalFrames / CHUNK_FRAMES;
            size_t offset = totalFrames % CHUNK_FRAMES;
            if (cIdx >= chunksL.size()) {
                allocateChunk();
            }
            size_t avail = CHUNK_FRAMES - offset;
            size_t n = std::min(count, avail);
            std::fill_n(chunksL[cIdx].data() + offset, n, 0.0f);
            std::fill_n(chunksR[cIdx].data() + offset, n, 0.0f);
            totalFrames += n;
            count -= n;
        }
    }

    float sampleMidAt(size_t idx) const {
        size_t cIdx = idx / CHUNK_FRAMES;
        size_t offset = idx % CHUNK_FRAMES;
        return 0.5f * (chunksL[cIdx][offset] + chunksR[cIdx][offset]);
    }

    void flatten(std::vector<float>& outL, std::vector<float>& outR) const {
        outL.resize(totalFrames);
        outR.resize(totalFrames);
        size_t written = 0;
        for (size_t c = 0; c < chunksL.size() && written < totalFrames; ++c) {
            size_t n = std::min(CHUNK_FRAMES, totalFrames - written);
            std::memcpy(outL.data() + written, chunksL[c].data(), n * sizeof(float));
            std::memcpy(outR.data() + written, chunksR[c].data(), n * sizeof(float));
            written += n;
        }
    }

private:
    void allocateChunk() {
        chunksL.emplace_back(CHUNK_FRAMES);
        chunksR.emplace_back(CHUNK_FRAMES);
    }

    std::vector<std::vector<float>> chunksL;
    std::vector<std::vector<float>> chunksR;
    size_t totalFrames = 0;
};

class AudioCaptureEngine {
public:
    using CaptureCompleteCallback = std::function<void(std::vector<float>&& L, std::vector<float>&& R, uint32_t sampleRate)>;

    AudioCaptureEngine() = default;
    ~AudioCaptureEngine() {
        stopRecording();
    }

    void setOnComplete(CaptureCompleteCallback cb) {
        std::lock_guard<std::mutex> lock(cbMtx);
        onComplete = std::move(cb);
    }

    bool isRecording() const { return recording.load(std::memory_order_acquire); }
    double getRecordedSeconds() const { return recordedSeconds.load(std::memory_order_relaxed); }
    float getPeakL() const { return peakL.load(std::memory_order_relaxed); }
    float getPeakR() const { return peakR.load(std::memory_order_relaxed); }
    std::wstring getLastError() const {
        std::lock_guard<std::mutex> lock(errMtx);
        return lastError;
    }

    void copyLivePreview(std::vector<LiveWaveBucket>& out) const {
        std::lock_guard<std::mutex> lock(previewMtx);
        out = livePreview;
    }

    static std::vector<CaptureDeviceInfo> enumerateDevices(CaptureBackend backend) {
        std::vector<CaptureDeviceInfo> result;
        result.push_back({ L"", L"Default" });

#if defined(_WIN32)
        if (backend == CaptureBackend::MME_WAVEIN) {
            UINT count = waveInGetNumDevs();
            for (UINT i = 0; i < count; ++i) {
                WAVEINCAPSW caps{};
                if (waveInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                    result.push_back({ std::to_wstring(i), std::wstring(caps.szPname) });
                }
            }
            return result;
        }

        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool didComInit = (hrCom == S_OK || hrCom == S_FALSE);

        IMMDeviceEnumerator* pEnum = nullptr;
        static const CLSID clsidMMDeviceEnumerator = {
            0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e }
        };
        static const IID iidIMMDeviceEnumerator = {
            0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 }
        };
        static const PROPERTYKEY pkeyFriendlyName = {
            { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
        };

        HRESULT hr = CoCreateInstance(clsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                      iidIMMDeviceEnumerator, reinterpret_cast<void**>(&pEnum));
        if (SUCCEEDED(hr) && pEnum) {
            EDataFlow flow = (backend == CaptureBackend::WASAPI_LOOPBACK) ? eRender : eCapture;
            IMMDeviceCollection* pCol = nullptr;
            if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
                UINT count = 0;
                pCol->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* pDev = nullptr;
                    if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                        LPWSTR devId = nullptr;
                        std::wstring idStr;
                        if (SUCCEEDED(pDev->GetId(&devId)) && devId) {
                            idStr = devId;
                            CoTaskMemFree(devId);
                        }
                        std::wstring nameStr = L"Audio Device " + std::to_wstring(i + 1);
                        IPropertyStore* pProps = nullptr;
                        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            if (SUCCEEDED(pProps->GetValue(pkeyFriendlyName, &varName))) {
                                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                                    nameStr = varName.pwszVal;
                                }
                                PropVariantClear(&varName);
                            }
                            pProps->Release();
                        }
                        if (!idStr.empty()) {
                            result.push_back({ idStr, nameStr });
                        }
                        pDev->Release();
                    }
                }
                pCol->Release();
            }
            pEnum->Release();
        }

        if (didComInit) {
            CoUninitialize();
        }
#endif
        return result;
    }

    bool startRecording(CaptureBackend backend, const std::wstring& deviceId,
                        ChannelMode channelMode, DurationCap cap) {
        stopRecording();

        {
            std::lock_guard<std::mutex> lock(errMtx);
            lastError.clear();
        }
        {
            std::lock_guard<std::mutex> lock(previewMtx);
            livePreview.clear();
        }
        recordedSeconds.store(0.0, std::memory_order_relaxed);
        peakL.store(0.0f, std::memory_order_relaxed);
        peakR.store(0.0f, std::memory_order_relaxed);
        stopRequested.store(false, std::memory_order_release);
        recording.store(true, std::memory_order_release);

        worker = std::thread([this, backend, deviceId, channelMode, cap]() {
            runCaptureThread(backend, deviceId, channelMode, cap);
        });
        return true;
    }

    void stopRecording() {
        stopRequested.store(true, std::memory_order_release);
        if (worker.joinable()) {
            worker.join();
        }
        recording.store(false, std::memory_order_release);
        peakL.store(0.0f, std::memory_order_relaxed);
        peakR.store(0.0f, std::memory_order_relaxed);
    }

private:
    void setError(const std::wstring& msg) {
        std::lock_guard<std::mutex> lock(errMtx);
        lastError = msg;
    }

    static inline void applyChannelMode(float rawL, float rawR, ChannelMode mode, float& outL, float& outR) {
        switch (mode) {
            case ChannelMode::STEREO:
                outL = rawL;
                outR = rawR;
                break;
            case ChannelMode::MONO_MIX: {
                float m = 0.5f * (rawL + rawR);
                outL = m;
                outR = m;
                break;
            }
            case ChannelMode::MONO_L:
                outL = rawL;
                outR = rawL;
                break;
            case ChannelMode::MONO_R:
                outL = rawR;
                outR = rawR;
                break;
        }
    }

    void updateMetersAndPreview(const ChunkedStereoBuffer& buf, float pktPeakL, float pktPeakR, uint32_t sampleRate) {
        size_t n = buf.size();
        if (n == 0 || sampleRate == 0) return;

        float prevL = peakL.load(std::memory_order_relaxed);
        float prevR = peakR.load(std::memory_order_relaxed);
        peakL.store(std::max(pktPeakL, prevL * 0.85f), std::memory_order_relaxed);
        peakR.store(std::max(pktPeakR, prevR * 0.85f), std::memory_order_relaxed);
        recordedSeconds.store(static_cast<double>(n) / static_cast<double>(sampleRate), std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        if (now - lastPreviewTime < std::chrono::milliseconds(50)) {
            return;
        }
        lastPreviewTime = now;

        constexpr size_t BUCKETS = 256;
        std::vector<LiveWaveBucket> buckets(BUCKETS);
        for (size_t b = 0; b < BUCKETS; ++b) {
            size_t s = (b * n) / BUCKETS;
            size_t e = ((b + 1) * n) / BUCKETS;
            if (e <= s) e = std::min(s + 1, n);
            float mn = 1.0f, mx = -1.0f;
            size_t step = std::max<size_t>(1, (e - s) / 8);
            for (size_t i = s; i < e; i += step) {
                float v = buf.sampleMidAt(i);
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
            buckets[b] = { mn, mx };
        }
        std::lock_guard<std::mutex> lock(previewMtx);
        livePreview.swap(buckets);
    }

    void commitCapture(const ChunkedStereoBuffer& buf, uint32_t sampleRate) {
        recording.store(false, std::memory_order_release);
        if (!buf.empty()) {
            std::vector<float> outL, outR;
            buf.flatten(outL, outR);
            CaptureCompleteCallback cbCopy;
            {
                std::lock_guard<std::mutex> lock(cbMtx);
                cbCopy = onComplete;
            }
            if (cbCopy) {
                cbCopy(std::move(outL), std::move(outR), sampleRate);
            }
        }
    }

    void runCaptureThread(CaptureBackend backend, const std::wstring& deviceId,
                          ChannelMode channelMode, DurationCap cap) {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        if (backend == CaptureBackend::MME_WAVEIN) {
            runMmeCapture(deviceId, channelMode, cap);
        } else {
            runWasapiCapture(backend == CaptureBackend::WASAPI_LOOPBACK, deviceId, channelMode, cap);
        }
#else
        (void)backend; (void)deviceId; (void)channelMode; (void)cap;
        recording.store(false, std::memory_order_release);
#endif
    }

#if defined(_WIN32)
    void runMmeCapture(const std::wstring& deviceId, ChannelMode channelMode, DurationCap cap) {
        UINT devId = WAVE_MAPPER;
        if (!deviceId.empty()) {
            try {
                devId = static_cast<UINT>(std::stoul(deviceId));
            } catch (...) {
                devId = WAVE_MAPPER;
            }
        }

        constexpr uint32_t sampleRate = 48000;
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = sampleRate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = sampleRate * 4;

        HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HWAVEIN hWaveIn = nullptr;
        MMRESULT mm = waveInOpen(&hWaveIn, devId, &wfx, reinterpret_cast<DWORD_PTR>(hEvent), 0, CALLBACK_EVENT);
        if (mm != MMSYSERR_NOERROR || !hWaveIn) {
            if (hEvent) CloseHandle(hEvent);
            setError(L"WaveIn open failed");
            recording.store(false, std::memory_order_release);
            return;
        }

        constexpr int NUM_BUFS = 4;
        constexpr size_t FRAMES_PER_BUF = 480; // 10ms @ 48kHz
        std::vector<std::vector<int16_t>> buffers(NUM_BUFS, std::vector<int16_t>(FRAMES_PER_BUF * 2));
        WAVEHDR hdrs[NUM_BUFS]{};

        for (int i = 0; i < NUM_BUFS; ++i) {
            hdrs[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
            hdrs[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
            waveInPrepareHeader(hWaveIn, &hdrs[i], sizeof(WAVEHDR));
            waveInAddBuffer(hWaveIn, &hdrs[i], sizeof(WAVEHDR));
        }

        ChunkedStereoBuffer capBuf;
        double capSec = durationCapSeconds(cap);
        size_t maxFrames = (capSec > 0.0) ? static_cast<size_t>(capSec * sampleRate) : 0;

        waveInStart(hWaveIn);

        while (!stopRequested.load(std::memory_order_acquire)) {
            WaitForSingleObject(hEvent, 20);
            for (int i = 0; i < NUM_BUFS; ++i) {
                if (hdrs[i].dwFlags & WHDR_DONE) {
                    size_t frames = hdrs[i].dwBytesRecorded / 4;
                    if (frames > 0) {
                        size_t oldSize = capBuf.size();
                        if (maxFrames > 0 && oldSize + frames > maxFrames) {
                            frames = (maxFrames > oldSize) ? (maxFrames - oldSize) : 0;
                        }
                        const int16_t* s = buffers[i].data();
                        float pL = 0.0f, pR = 0.0f;
                        for (size_t f = 0; f < frames; ++f) {
                            float rawL = static_cast<float>(s[f * 2]) / 32768.0f;
                            float rawR = static_cast<float>(s[f * 2 + 1]) / 32768.0f;
                            float l, r;
                            applyChannelMode(rawL, rawR, channelMode, l, r);
                            pL = std::max(pL, std::abs(l));
                            pR = std::max(pR, std::abs(r));
                            capBuf.pushSample(l, r);
                        }
                        updateMetersAndPreview(capBuf, pL, pR, sampleRate);
                        if (maxFrames > 0 && capBuf.size() >= maxFrames) {
                            stopRequested.store(true, std::memory_order_release);
                            break;
                        }
                    }
                    hdrs[i].dwFlags &= ~WHDR_DONE;
                    if (!stopRequested.load(std::memory_order_acquire)) {
                        waveInAddBuffer(hWaveIn, &hdrs[i], sizeof(WAVEHDR));
                    }
                }
            }
        }

        waveInStop(hWaveIn);
        waveInReset(hWaveIn);
        for (int i = 0; i < NUM_BUFS; ++i) {
            waveInUnprepareHeader(hWaveIn, &hdrs[i], sizeof(WAVEHDR));
        }
        waveInClose(hWaveIn);
        if (hEvent) CloseHandle(hEvent);

        commitCapture(capBuf, sampleRate);
    }

    void runWasapiCapture(bool loopback, const std::wstring& deviceId,
                          ChannelMode channelMode, DurationCap cap) {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool didComInit = (hrCom == S_OK || hrCom == S_FALSE);

        static const CLSID clsidMMDeviceEnumerator = {
            0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e }
        };
        static const IID iidIMMDeviceEnumerator = {
            0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 }
        };
        static const IID iidIAudioClient = {
            0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 }
        };
        static const IID iidIAudioCaptureClient = {
            0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 }
        };
        static const GUID subformatFloat = {
            0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
        };
        static const GUID subformatPcm = {
            0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
        };

        IMMDeviceEnumerator* pEnum = nullptr;
        IMMDevice* pDevice = nullptr;
        IAudioClient* pClient = nullptr;
        IAudioCaptureClient* pCapture = nullptr;
        WAVEFORMATEX* pwfx = nullptr;
        HANDLE hEvent = nullptr;

        auto cleanup = [&]() {
            if (hEvent) { CloseHandle(hEvent); hEvent = nullptr; }
            if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
            if (pCapture) { pCapture->Release(); pCapture = nullptr; }
            if (pClient) { pClient->Release(); pClient = nullptr; }
            if (pDevice) { pDevice->Release(); pDevice = nullptr; }
            if (pEnum) { pEnum->Release(); pEnum = nullptr; }
            if (didComInit) { CoUninitialize(); }
        };

        HRESULT hr = CoCreateInstance(clsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                      iidIMMDeviceEnumerator, reinterpret_cast<void**>(&pEnum));
        if (FAILED(hr) || !pEnum) {
            setError(L"WASAPI enumerator failed");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        if (!deviceId.empty()) {
            hr = pEnum->GetDevice(deviceId.c_str(), &pDevice);
        } else {
            EDataFlow flow = loopback ? eRender : eCapture;
            hr = pEnum->GetDefaultAudioEndpoint(flow, eConsole, &pDevice);
        }
        if (FAILED(hr) || !pDevice) {
            setError(L"No active audio device");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        hr = pDevice->Activate(iidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pClient));
        if (FAILED(hr) || !pClient) {
            setError(L"AudioClient activate failed");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        hr = pClient->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            setError(L"GetMixFormat failed");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        REFERENCE_TIME defPeriod = 100000; // 10ms fallback
        REFERENCE_TIME minPeriod = 0;
        pClient->GetDevicePeriod(&defPeriod, &minPeriod);

        bool useEventCallback = !loopback;
        DWORD streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        REFERENCE_TIME hnsRequestedDuration = loopback ? 200000 : std::max<REFERENCE_TIME>(defPeriod, 100000);
        hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, hnsRequestedDuration, 0, pwfx, nullptr);
        if (FAILED(hr) && useEventCallback) {
            useEventCallback = false;
            hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, pwfx, nullptr);
        }
        if (FAILED(hr)) {
            setError(L"WASAPI Initialize failed");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        if (useEventCallback) {
            hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!hEvent || FAILED(pClient->SetEventHandle(hEvent))) {
                useEventCallback = false;
                if (hEvent) { CloseHandle(hEvent); hEvent = nullptr; }
            }
        }

        hr = pClient->GetService(iidIAudioCaptureClient, reinterpret_cast<void**>(&pCapture));
        if (FAILED(hr) || !pCapture) {
            setError(L"GetCaptureClient failed");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        bool isPcm = (pwfx->wFormatTag == WAVE_FORMAT_PCM);
        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
            auto* wfxe = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
            if (std::memcmp(&wfxe->SubFormat, &subformatFloat, sizeof(GUID)) == 0) isFloat = true;
            else if (std::memcmp(&wfxe->SubFormat, &subformatPcm, sizeof(GUID)) == 0) isPcm = true;
        }

        const uint32_t sampleRate = pwfx->nSamplesPerSec;
        const uint16_t channels = pwfx->nChannels;
        const uint16_t bits = pwfx->wBitsPerSample;

        if ((!isFloat && !isPcm) || channels == 0 || sampleRate == 0) {
            setError(L"Unsupported WASAPI mix format");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        ChunkedStereoBuffer capBuf;
        double capSec = durationCapSeconds(cap);
        size_t maxFrames = (capSec > 0.0) ? static_cast<size_t>(capSec * sampleRate) : 0;

        hr = pClient->Start();
        if (FAILED(hr)) {
            setError(L"WASAPI Start failed");
            cleanup();
            recording.store(false, std::memory_order_release);
            return;
        }

        while (!stopRequested.load(std::memory_order_acquire)) {
            if (useEventCallback && hEvent) {
                WaitForSingleObject(hEvent, 20);
            } else {
                Sleep(5);
            }
            UINT32 packetLength = 0;
            while (SUCCEEDED(pCapture->GetNextPacketSize(&packetLength)) && packetLength > 0) {
                BYTE* pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;
                hr = pCapture->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                size_t oldSize = capBuf.size();
                size_t framesToRead = numFramesAvailable;
                if (maxFrames > 0 && oldSize + framesToRead > maxFrames) {
                    framesToRead = (maxFrames > oldSize) ? (maxFrames - oldSize) : 0;
                }

                if (framesToRead > 0) {
                    float pL = 0.0f, pR = 0.0f;
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !pData) {
                        capBuf.appendSilence(framesToRead);
                    } else {
                        const size_t bytesPerSample = bits / 8;
                        const size_t frameBytes = bytesPerSample * channels;
                        auto readSample = [&](const BYTE* p) -> float {
                            if (isFloat && bits == 32) {
                                float v = 0.0f;
                                std::memcpy(&v, p, 4);
                                return std::clamp(v, -1.0f, 1.0f);
                            }
                            if (isPcm && bits == 16) {
                                int16_t s = 0;
                                std::memcpy(&s, p, 2);
                                return static_cast<float>(s) / 32768.0f;
                            }
                            if (isPcm && bits == 24) {
                                int32_t v = static_cast<int32_t>(p[0]) |
                                            (static_cast<int32_t>(p[1]) << 8) |
                                            (static_cast<int32_t>(p[2]) << 16);
                                if (v & 0x800000) v |= 0xFF000000;
                                return static_cast<float>(v) / 8388608.0f;
                            }
                            if (isPcm && bits == 32) {
                                int32_t v = 0;
                                std::memcpy(&v, p, 4);
                                return static_cast<float>(static_cast<double>(v) / 2147483648.0);
                            }
                            return 0.0f;
                        };

                        for (size_t f = 0; f < framesToRead; ++f) {
                            const BYTE* fp = pData + f * frameBytes;
                            float rawL = readSample(fp);
                            float rawR = (channels >= 2) ? readSample(fp + bytesPerSample) : rawL;
                            float l, r;
                            applyChannelMode(rawL, rawR, channelMode, l, r);
                            pL = std::max(pL, std::abs(l));
                            pR = std::max(pR, std::abs(r));
                            capBuf.pushSample(l, r);
                        }
                    }
                    updateMetersAndPreview(capBuf, pL, pR, sampleRate);
                }

                pCapture->ReleaseBuffer(numFramesAvailable);

                if (maxFrames > 0 && capBuf.size() >= maxFrames) {
                    stopRequested.store(true, std::memory_order_release);
                    break;
                }
            }
        }

        pClient->Stop();
        cleanup();
        commitCapture(capBuf, sampleRate);
    }
#endif

    std::atomic<bool> recording{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<double> recordedSeconds{0.0};
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};

    std::thread worker;
    mutable std::mutex cbMtx;
    CaptureCompleteCallback onComplete;

    mutable std::mutex errMtx;
    std::wstring lastError;

    mutable std::mutex previewMtx;
    std::vector<LiveWaveBucket> livePreview;
    std::chrono::steady_clock::time_point lastPreviewTime{};
};

} // namespace LMAC
