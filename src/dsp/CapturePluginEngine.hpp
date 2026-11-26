#pragma once

#include "AudioClip.hpp"
#include "WavIO.hpp"
#include "../capture/AudioCaptureEngine.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace LMAC {

enum PluginParam : int {
    PARAM_GAIN = 0,       // 0..1 -> -24 dB .. +24 dB (0.5 = 0 dB)
    PARAM_PLAY_MODE = 1,  // <0.5 = RESTART, >=0.5 = CONTINUE
    PARAM_GATE_MODE = 2,  // <0.5 = GATE, >=0.5 = 1-SHOT
    PARAM_LOOP = 3,       // <0.5 = OFF, >=0.5 = ON
    PARAM_TRIM_START = 4, // 0..1 normalized
    PARAM_TRIM_END = 5,   // 0..1 normalized
    NUM_PARAMS = 6
};

enum class PlayMode : int {
    RESTART = 0,
    CONTINUE = 1
};

enum class GateMode : int {
    GATE = 0,
    ONE_SHOT = 1
};

struct MidiEvent {
    int deltaFrames = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

class CapturePluginEngine {
public:
    static constexpr uint32_t CHUNK_MAGIC = 0x43414D4Cu; // "LMAC"
    static constexpr uint32_t CHUNK_VERSION = 1;
    static constexpr size_t MAX_CHUNK_PCM_BYTES = 24u * 1024u * 1024u;
    static constexpr uint32_t MIDI_RING_SIZE = 256;
    static constexpr uint32_t MIDI_RING_MASK = MIDI_RING_SIZE - 1;

    CapturePluginEngine() {
        capture.setOnComplete([this](std::vector<float>&& L, std::vector<float>&& R, uint32_t sr) {
            clip.setAudio(std::move(L), std::move(R), sr, true);
            syncTrimParamsFromClip();
            resetCursorRequested.store(true, std::memory_order_release);
        });
        rtMidiScratch.reserve(64);
    }

    AudioClip& getClip() { return clip; }
    const AudioClip& getClip() const { return clip; }
    AudioCaptureEngine& getCapture() { return capture; }
    const AudioCaptureEngine& getCapture() const { return capture; }

    void setHostSampleRate(float sr) {
        if (sr > 1000.0f) {
            hostSampleRate.store(sr, std::memory_order_relaxed);
        }
    }

    float getHostSampleRate() const {
        return hostSampleRate.load(std::memory_order_relaxed);
    }

    void setCaptureBackend(CaptureBackend b) { captureBackend.store(static_cast<int>(b), std::memory_order_relaxed); }
    CaptureBackend getCaptureBackend() const { return static_cast<CaptureBackend>(captureBackend.load(std::memory_order_relaxed)); }

    void setChannelMode(ChannelMode m) { channelMode.store(static_cast<int>(m), std::memory_order_relaxed); }
    ChannelMode getChannelMode() const { return static_cast<ChannelMode>(channelMode.load(std::memory_order_relaxed)); }

    void setDurationCap(DurationCap c) { durationCap.store(static_cast<int>(c), std::memory_order_relaxed); }
    DurationCap getDurationCap() const { return static_cast<DurationCap>(durationCap.load(std::memory_order_relaxed)); }

    void setParameter(int index, float value) {
        value = std::clamp(value, 0.0f, 1.0f);
        switch (index) {
            case PARAM_GAIN:
                paramGain.store(value, std::memory_order_relaxed);
                updateCachedGain(value);
                break;
            case PARAM_PLAY_MODE:
                paramPlayMode.store(value, std::memory_order_relaxed);
                break;
            case PARAM_GATE_MODE:
                paramGateMode.store(value, std::memory_order_relaxed);
                break;
            case PARAM_LOOP:
                paramLoop.store(value, std::memory_order_relaxed);
                break;
            case PARAM_TRIM_START: {
                paramTrimStart.store(value, std::memory_order_relaxed);
                auto snap = clip.getSnapshot();
                if (snap && snap->numFrames() > 1) {
                    size_t s = static_cast<size_t>(std::round(value * static_cast<double>(snap->numFrames())));
                    size_t e = snap->trimEnd;
                    if (s >= e) s = (e > 0) ? (e - 1) : 0;
                    clip.setTrim(s, e);
                }
                break;
            }
            case PARAM_TRIM_END: {
                paramTrimEnd.store(value, std::memory_order_relaxed);
                auto snap = clip.getSnapshot();
                if (snap && snap->numFrames() > 1) {
                    size_t e = static_cast<size_t>(std::round(value * static_cast<double>(snap->numFrames())));
                    size_t s = snap->trimStart;
                    if (e <= s) e = std::min(snap->numFrames(), s + 1);
                    clip.setTrim(s, e);
                }
                break;
            }
            default:
                break;
        }
    }

    float getParameter(int index) const {
        switch (index) {
            case PARAM_GAIN:       return paramGain.load(std::memory_order_relaxed);
            case PARAM_PLAY_MODE:  return paramPlayMode.load(std::memory_order_relaxed);
            case PARAM_GATE_MODE:  return paramGateMode.load(std::memory_order_relaxed);
            case PARAM_LOOP:       return paramLoop.load(std::memory_order_relaxed);
            case PARAM_TRIM_START: return paramTrimStart.load(std::memory_order_relaxed);
            case PARAM_TRIM_END:   return paramTrimEnd.load(std::memory_order_relaxed);
            default:               return 0.0f;
        }
    }

    void syncTrimParamsFromClip() {
        auto snap = clip.getSnapshot();
        if (!snap || snap->numFrames() == 0) {
            paramTrimStart.store(0.0f, std::memory_order_relaxed);
            paramTrimEnd.store(1.0f, std::memory_order_relaxed);
            return;
        }
        double inv = 1.0 / static_cast<double>(snap->numFrames());
        paramTrimStart.store(static_cast<float>(snap->trimStart * inv), std::memory_order_relaxed);
        paramTrimEnd.store(static_cast<float>(snap->trimEnd * inv), std::memory_order_relaxed);
    }

    float getGainDb() const {
        return (paramGain.load(std::memory_order_relaxed) - 0.5f) * 48.0f;
    }

    PlayMode getPlayMode() const {
        return (paramPlayMode.load(std::memory_order_relaxed) >= 0.5f) ? PlayMode::CONTINUE : PlayMode::RESTART;
    }

    GateMode getGateMode() const {
        return (paramGateMode.load(std::memory_order_relaxed) >= 0.5f) ? GateMode::ONE_SHOT : GateMode::GATE;
    }

    bool getLoop() const {
        return paramLoop.load(std::memory_order_relaxed) >= 0.5f;
    }

    void startPreview() { previewCmd.store(1, std::memory_order_release); }
    void stopPreview()  { previewCmd.store(2, std::memory_order_release); }
    void resetCursor()  { resetCursorRequested.store(true, std::memory_order_release); }

    bool isPlaying() const { return playingAtomic.load(std::memory_order_relaxed); }
    double getPlayheadNorm() const { return playheadNormAtomic.load(std::memory_order_relaxed); }

    // Wait-free SPSC ring buffer push (called from host/MIDI thread)
    void queueMidiEvent(int deltaFrames, uint8_t status, uint8_t data1, uint8_t data2) {
        uint32_t w = midiWrite.load(std::memory_order_relaxed);
        uint32_t r = midiRead.load(std::memory_order_acquire);
        if ((w - r) >= MIDI_RING_SIZE) return; // full
        midiRing[w & MIDI_RING_MASK] = { std::max(0, deltaFrames), status, data1, data2 };
        midiWrite.store(w + 1, std::memory_order_release);
    }

    void clearMidiQueue() {
        midiRead.store(midiWrite.load(std::memory_order_acquire), std::memory_order_release);
    }

    void reclaimRetiredSnapshots() {
        std::lock_guard<std::mutex> lock(retiredMtx);
        for (auto& slot : retiredPool) {
            slot.reset();
        }
    }

    void process(float* outL, float* outR, int numFrames) {
        if (!outL || !outR || numFrames <= 0) return;
        std::fill(outL, outL + numFrames, 0.0f);
        std::fill(outR, outR + numFrames, 0.0f);

        // Non-blocking snapshot refresh on audio thread (retire old snapshot off-thread)
        uint64_t latestVer = clip.getVersion();
        if (!rtSnap || latestVer != rtVersion) {
            if (auto s = clip.tryGetSnapshot()) {
                if (rtSnap) {
                    std::unique_lock<std::mutex> lock(retiredMtx, std::try_to_lock);
                    if (lock.owns_lock()) {
                        for (auto& slot : retiredPool) {
                            if (!slot) {
                                slot = std::move(rtSnap);
                                break;
                            }
                        }
                    }
                }
                rtSnap = std::move(s);
                rtVersion = rtSnap->version;
                clampPlayheadToSnap();
            }
        }

        int cmd = previewCmd.exchange(0, std::memory_order_acq_rel);
        if (cmd == 1) {
            triggerNoteA4(127, true);
        } else if (cmd == 2) {
            stopPlayback(true);
        }
        if (resetCursorRequested.exchange(false, std::memory_order_acq_rel)) {
            resetPlayheadToTrimStart();
        }

        // Wait-free drain of MIDI ring buffer
        rtMidiScratch.clear();
        uint32_t r = midiRead.load(std::memory_order_relaxed);
        uint32_t w = midiWrite.load(std::memory_order_acquire);
        while (r != w) {
            rtMidiScratch.push_back(midiRing[r & MIDI_RING_MASK]);
            ++r;
        }
        if (!rtMidiScratch.empty()) {
            midiRead.store(r, std::memory_order_release);
            if (rtMidiScratch.size() > 1) {
                std::stable_sort(rtMidiScratch.begin(), rtMidiScratch.end(),
                                 [](const MidiEvent& a, const MidiEvent& b) { return a.deltaFrames < b.deltaFrames; });
            }
        }

        const float sr = hostSampleRate.load(std::memory_order_relaxed);
        const float userGain = cachedGainLin.load(std::memory_order_relaxed);
        const float attackStep = 1.0f / std::max(1.0f, sr * 0.0015f); // 1.5ms snappier attack
        const float releaseStep = 1.0f / std::max(1.0f, sr * 0.003f); // 3.0ms anti-click release
        const bool loop = getLoop();

        const bool hasClip = rtSnap && !rtSnap->empty();
        const float* bufL = hasClip ? rtSnap->L().data() : nullptr;
        const float* bufR = hasClip ? rtSnap->R().data() : nullptr;
        const size_t tStart = hasClip ? rtSnap->trimStart : 0;
        const size_t tEnd = hasClip ? rtSnap->trimEnd : 0;
        const double dStart = static_cast<double>(tStart);
        const double dEnd = static_cast<double>(tEnd);
        const double step = hasClip ? (static_cast<double>(rtSnap->sampleRate) / static_cast<double>(sr)) : 1.0;
        const bool fast1to1 = hasClip && (rtSnap->sampleRate == static_cast<uint32_t>(sr)) && (playhead == std::floor(playhead));

        size_t evIdx = 0;
        for (int i = 0; i < numFrames; ++i) {
            while (evIdx < rtMidiScratch.size() && rtMidiScratch[evIdx].deltaFrames <= i) {
                handleMidiEvent(rtMidiScratch[evIdx]);
                ++evIdx;
            }

            if (fadingOut) {
                rampEnv -= releaseStep;
                if (rampEnv <= 0.0f) {
                    rampEnv = 0.0f;
                    fadingOut = false;
                    playing = false;
                    if (resetOnFadeEnd) {
                        resetPlayheadToTrimStart();
                        resetOnFadeEnd = false;
                    }
                }
            } else if (playing && rampEnv < 1.0f) {
                rampEnv = std::min(1.0f, rampEnv + attackStep);
            }

            if (!playing || !hasClip) {
                continue;
            }

            if (playhead < dStart || playhead >= dEnd) {
                if (loop) {
                    playhead = dStart;
                } else {
                    playing = false;
                    fadingOut = false;
                    rampEnv = 0.0f;
                    playhead = dStart;
                    continue;
                }
            }

            float sL, sR;
            if (fast1to1) {
                size_t idx = static_cast<size_t>(playhead);
                sL = bufL[idx];
                sR = bufR[idx];
            } else {
                sL = sampleCubic(bufL, playhead, tStart, tEnd);
                sR = sampleCubic(bufR, playhead, tStart, tEnd);
            }

            const float totalGain = userGain * velocityGain * rampEnv;
            outL[i] = sL * totalGain;
            outR[i] = sR * totalGain;

            playhead += step;

            if (playhead >= dEnd) {
                if (loop) {
                    const double span = dEnd - dStart;
                    if (span > 0.0) {
                        playhead = dStart + std::fmod(playhead - dStart, span);
                    } else {
                        playhead = dStart;
                    }
                } else {
                    playing = false;
                    fadingOut = false;
                    rampEnv = 0.0f;
                    playhead = dStart;
                }
            }
        }

        while (evIdx < rtMidiScratch.size()) {
            handleMidiEvent(rtMidiScratch[evIdx]);
            ++evIdx;
        }

        playingAtomic.store(playing, std::memory_order_relaxed);
        if (rtSnap && rtSnap->numFrames() > 0) {
            playheadNormAtomic.store(
                std::clamp(playhead / static_cast<double>(rtSnap->numFrames()), 0.0, 1.0),
                std::memory_order_relaxed
            );
        } else {
            playheadNormAtomic.store(0.0, std::memory_order_relaxed);
        }
    }

    std::vector<uint8_t> serializeChunk() const {
        auto snap = clip.getSnapshot();
        std::vector<uint8_t> out;

        uint32_t magic = CHUNK_MAGIC;
        uint32_t version = CHUNK_VERSION;
        float params[NUM_PARAMS] = {
            getParameter(PARAM_GAIN),
            getParameter(PARAM_PLAY_MODE),
            getParameter(PARAM_GATE_MODE),
            getParameter(PARAM_LOOP),
            getParameter(PARAM_TRIM_START),
            getParameter(PARAM_TRIM_END)
        };
        int32_t backend = captureBackend.load(std::memory_order_relaxed);
        int32_t chMode = channelMode.load(std::memory_order_relaxed);
        int32_t durCap = durationCap.load(std::memory_order_relaxed);

        uint32_t sr = snap ? snap->sampleRate : 48000;
        uint64_t trimS = snap ? static_cast<uint64_t>(snap->trimStart) : 0;
        uint64_t trimE = snap ? static_cast<uint64_t>(snap->trimEnd) : 0;
        uint64_t numFrames = snap ? static_cast<uint64_t>(snap->numFrames()) : 0;

        uint64_t maxFrames = MAX_CHUNK_PCM_BYTES / 4;
        if (numFrames > maxFrames) {
            numFrames = maxFrames;
            trimS = std::min(trimS, numFrames);
            trimE = std::min(trimE, numFrames);
        }

        const size_t headerSize = 4 + 4 + sizeof(params) + 12 + 4 + 8 + 8 + 8;
        out.resize(headerSize + static_cast<size_t>(numFrames) * 4);

        uint8_t* p = out.data();
        auto writeBytes = [&](const void* src, size_t sz) {
            std::memcpy(p, src, sz);
            p += sz;
        };

        writeBytes(&magic, 4);
        writeBytes(&version, 4);
        writeBytes(params, sizeof(params));
        writeBytes(&backend, 4);
        writeBytes(&chMode, 4);
        writeBytes(&durCap, 4);
        writeBytes(&sr, 4);
        writeBytes(&trimS, 8);
        writeBytes(&trimE, 8);
        writeBytes(&numFrames, 8);

        if (snap && numFrames > 0) {
            const auto& srcL = snap->L();
            const auto& srcR = snap->R();
            int16_t* pcm = reinterpret_cast<int16_t*>(p);
            for (uint64_t i = 0; i < numFrames; ++i) {
                pcm[i * 2] = WavIO::floatToInt16(srcL[i]);
                pcm[i * 2 + 1] = WavIO::floatToInt16(srcR[i]);
            }
        }
        return out;
    }

    bool deserializeChunk(const void* data, size_t size) {
        const size_t headerSize = 4 + 4 + sizeof(float) * NUM_PARAMS + 12 + 4 + 8 + 8 + 8;
        if (!data || size < headerSize) return false;

        const uint8_t* p = static_cast<const uint8_t*>(data);
        auto readBytes = [&](void* dst, size_t sz) {
            std::memcpy(dst, p, sz);
            p += sz;
        };

        uint32_t magic = 0, version = 0;
        readBytes(&magic, 4);
        readBytes(&version, 4);
        if (magic != CHUNK_MAGIC || version != CHUNK_VERSION) return false;

        float params[NUM_PARAMS]{};
        readBytes(params, sizeof(params));

        int32_t backend = 0, chMode = 0, durCap = 2;
        readBytes(&backend, 4);
        readBytes(&chMode, 4);
        readBytes(&durCap, 4);

        uint32_t sr = 48000;
        uint64_t trimS = 0, trimE = 0, numFrames = 0;
        readBytes(&sr, 4);
        readBytes(&trimS, 8);
        readBytes(&trimE, 8);
        readBytes(&numFrames, 8);

        if (numFrames > MAX_CHUNK_PCM_BYTES / 4) return false;
        if (size < headerSize + static_cast<size_t>(numFrames) * 4) return false;

        captureBackend.store(std::clamp(backend, 0, 2), std::memory_order_relaxed);
        channelMode.store(std::clamp(chMode, 0, 3), std::memory_order_relaxed);
        durationCap.store(std::clamp(durCap, 0, 4), std::memory_order_relaxed);

        float g = std::clamp(params[PARAM_GAIN], 0.0f, 1.0f);
        paramGain.store(g, std::memory_order_relaxed);
        updateCachedGain(g);
        paramPlayMode.store(std::clamp(params[PARAM_PLAY_MODE], 0.0f, 1.0f), std::memory_order_relaxed);
        paramGateMode.store(std::clamp(params[PARAM_GATE_MODE], 0.0f, 1.0f), std::memory_order_relaxed);
        paramLoop.store(std::clamp(params[PARAM_LOOP], 0.0f, 1.0f), std::memory_order_relaxed);
        paramTrimStart.store(std::clamp(params[PARAM_TRIM_START], 0.0f, 1.0f), std::memory_order_relaxed);
        paramTrimEnd.store(std::clamp(params[PARAM_TRIM_END], 0.0f, 1.0f), std::memory_order_relaxed);

        std::vector<float> L(numFrames);
        std::vector<float> R(numFrames);
        const int16_t* pcm = reinterpret_cast<const int16_t*>(p);
        for (uint64_t i = 0; i < numFrames; ++i) {
            L[i] = WavIO::int16ToFloat(pcm[i * 2]);
            R[i] = WavIO::int16ToFloat(pcm[i * 2 + 1]);
        }

        clip.setAudioWithTrim(std::move(L), std::move(R), sr,
                            static_cast<size_t>(trimS), static_cast<size_t>(trimE));
        syncTrimParamsFromClip();
        resetCursorRequested.store(true, std::memory_order_release);
        return true;
    }

private:
    void updateCachedGain(float norm) {
        float db = (norm - 0.5f) * 48.0f;
        cachedGainLin.store(std::pow(10.0f, db / 20.0f), std::memory_order_relaxed);
    }

    void clampPlayheadToSnap() {
        if (!rtSnap || rtSnap->empty()) {
            playhead = 0.0;
            playing = false;
            fadingOut = false;
            return;
        }
        double dStart = static_cast<double>(rtSnap->trimStart);
        double dEnd = static_cast<double>(rtSnap->trimEnd);
        if (playhead < dStart || playhead >= dEnd) {
            playhead = dStart;
        }
    }

    void resetPlayheadToTrimStart() {
        if (rtSnap && !rtSnap->empty()) {
            playhead = static_cast<double>(rtSnap->trimStart);
        } else {
            playhead = 0.0;
        }
    }

    void triggerNoteA4(uint8_t velocity, bool forceRestartIfStopped = false) {
        if (!rtSnap || rtSnap->empty()) return;
        (void)forceRestartIfStopped;

        velocityGain = static_cast<float>(std::clamp<int>(velocity, 1, 127)) / 127.0f;
        double dStart = static_cast<double>(rtSnap->trimStart);
        double dEnd = static_cast<double>(rtSnap->trimEnd);

        if (getPlayMode() == PlayMode::RESTART) {
            playhead = dStart;
        } else if (playhead < dStart || playhead >= dEnd) {
            playhead = dStart;
        }

        playing = true;
        fadingOut = false;
        resetOnFadeEnd = false;
        rampEnv = 0.0f;
    }

    void stopPlayback(bool resetCursorAfter) {
        if (playing) {
            fadingOut = true;
            resetOnFadeEnd = resetCursorAfter;
        } else if (resetCursorAfter) {
            resetPlayheadToTrimStart();
        }
    }

    void handleMidiEvent(const MidiEvent& ev) {
        uint8_t msg = ev.status & 0xF0;
        if (msg == 0x90 && ev.data2 > 0) {
            if (ev.data1 == 69) {
                a4Held = true;
                triggerNoteA4(ev.data2);
            } else if (ev.data1 == 70) {
                resetPlayheadToTrimStart();
                if (playing) {
                    rampEnv = 0.0f;
                    fadingOut = false;
                }
            }
        } else if (msg == 0x80 || (msg == 0x90 && ev.data2 == 0)) {
            if (ev.data1 == 69) {
                a4Held = false;
                if (getGateMode() == GateMode::GATE && playing) {
                    fadingOut = true;
                    resetOnFadeEnd = (getPlayMode() == PlayMode::RESTART);
                }
            }
        } else if (msg == 0xB0 && (ev.data1 == 120 || ev.data1 == 123)) {
            a4Held = false;
            stopPlayback(false);
        }
    }

    static inline float sampleCubic(const float* buf, double pos, size_t minIdx, size_t maxIdx) {
        if (!buf || maxIdx <= minIdx) return 0.0f;
        size_t i1 = static_cast<size_t>(pos);
        if (i1 < minIdx) i1 = minIdx;
        if (i1 >= maxIdx) i1 = maxIdx - 1;

        float t = static_cast<float>(pos - static_cast<double>(i1));
        size_t i0 = (i1 > minIdx) ? (i1 - 1) : i1;
        size_t i2 = (i1 + 1 < maxIdx) ? (i1 + 1) : i1;
        size_t i3 = (i2 + 1 < maxIdx) ? (i2 + 1) : i2;

        float y0 = buf[i0];
        float y1 = buf[i1];
        float y2 = buf[i2];
        float y3 = buf[i3];

        float c0 = y1;
        float c1 = 0.5f * (y2 - y0);
        float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    AudioClip clip;
    AudioCaptureEngine capture;

    std::atomic<float> hostSampleRate{44100.0f};
    std::atomic<float> paramGain{0.5f};
    std::atomic<float> cachedGainLin{1.0f};
    std::atomic<float> paramPlayMode{0.0f};
    std::atomic<float> paramGateMode{0.0f};
    std::atomic<float> paramLoop{0.0f};
    std::atomic<float> paramTrimStart{0.0f};
    std::atomic<float> paramTrimEnd{1.0f};

    std::atomic<int> captureBackend{static_cast<int>(CaptureBackend::WASAPI_INPUT)};
    std::atomic<int> channelMode{static_cast<int>(ChannelMode::STEREO)};
    std::atomic<int> durationCap{static_cast<int>(DurationCap::CAP_5M)};

    std::atomic<int> previewCmd{0};
    std::atomic<bool> resetCursorRequested{false};
    std::atomic<bool> playingAtomic{false};
    std::atomic<double> playheadNormAtomic{0.0};

    MidiEvent midiRing[MIDI_RING_SIZE]{};
    std::atomic<uint32_t> midiWrite{0};
    std::atomic<uint32_t> midiRead{0};
    std::vector<MidiEvent> rtMidiScratch;

    std::shared_ptr<const ClipSnapshot> rtSnap;
    static constexpr size_t RETIRED_SLOTS = 8;
    std::shared_ptr<const ClipSnapshot> retiredPool[RETIRED_SLOTS]{};
    std::mutex retiredMtx;
    uint64_t rtVersion = 0;
    double playhead = 0.0;
    float velocityGain = 1.0f;
    float rampEnv = 0.0f;
    bool playing = false;
    bool fadingOut = false;
    bool resetOnFadeEnd = false;
    bool a4Held = false;
};

} // namespace LMAC
