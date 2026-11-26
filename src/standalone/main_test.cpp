#include "../dsp/CapturePluginEngine.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace LMAC;

static void testAudioClipEdits() {
    AudioClip clip;
    std::vector<float> L(1000), R(1000);
    for (size_t i = 0; i < 1000; ++i) {
        L[i] = 0.25f + 0.2f * std::sin(2.0f * 3.14159265f * i / 100.0f);
        R[i] = -L[i];
    }
    clip.setAudio(L, R, 48000, false);

    // DC removal
    clip.removeDC(0, 1000);
    auto s1 = clip.getSnapshot();
    double mean = 0.0;
    for (float v : s1->L()) mean += v;
    mean /= s1->L().size();
    assert(std::abs(mean) < 1e-4);

    // Zero-copy trim check
    clip.setTrim(10, 900);
    auto s1Trim = clip.getSnapshot();
    assert(s1Trim->audio.get() == s1->audio.get());
    clip.setTrim(0, 1000);

    // Peak mipmap transient accuracy check
    {
        std::vector<float> impL(1024, 0.0f), impR(1024, 0.0f);
        impL[300] = 0.9f;
        impR[300] = 0.9f;
        AudioClip impClip;
        impClip.setAudio(impL, impR, 48000, false);
        auto impSnap = impClip.getSnapshot();
        assert(impSnap->audio->peaks.size() == 4);
        assert(std::abs(impSnap->audio->peaks[1].mx - 0.9f) < 1e-5f);
    }

    // Normalize
    clip.normalize(0, 1000, -0.5f);
    auto s2 = clip.getSnapshot();
    float peak = 0.0f;
    for (float v : s2->L()) peak = std::max(peak, std::abs(v));
    float expectedPeak = std::pow(10.0f, -0.5f / 20.0f);
    assert(std::abs(peak - expectedPeak) < 1e-3f);

    // Reverse + Undo
    float firstBefore = s2->L().front();
    float lastBefore = s2->L().back();
    clip.reverse(0, 1000);
    auto s3 = clip.getSnapshot();
    assert(std::abs(s3->L().front() - lastBefore) < 1e-6f);
    assert(std::abs(s3->L().back() - firstBefore) < 1e-6f);
    assert(clip.undo());
    auto s4 = clip.getSnapshot();
    assert(std::abs(s4->L().front() - firstBefore) < 1e-6f);
    assert(clip.redo());

    // Delete range
    clip.deleteRange(200, 400);
    assert(clip.getSnapshot()->numFrames() == 800);

    // Silence range
    clip.silenceRange(100, 200);
    auto s5 = clip.getSnapshot();
    assert(std::abs(s5->L()[150]) < 1e-6f);

    // Fade in / out
    clip.fadeIn(0, 100);
    assert(std::abs(clip.getSnapshot()->L()[0]) < 1e-6f);
    clip.fadeOut(700, 800);
    assert(std::abs(clip.getSnapshot()->L()[799]) < 1e-6f);

    // Crop
    clip.setTrim(100, 500);
    clip.cropToRange(0, 0);
    assert(clip.getSnapshot()->numFrames() == 400);

    // Clear + undo
    clip.clear();
    assert(clip.getSnapshot()->empty());
    assert(clip.undo());
    assert(clip.getSnapshot()->numFrames() == 400);

    printf("[PASS] testAudioClipEdits\n");
}

static void testWavRoundTrip() {
    std::vector<float> L(4800), R(4800);
    for (size_t i = 0; i < 4800; ++i) {
        L[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
        R[i] = 0.25f * std::cos(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
    }
    std::wstring tmpPath = L"bin\\test_roundtrip.wav";
    assert(WavIO::saveWav16(tmpPath, L, R, 48000));

    std::vector<float> inL, inR;
    uint32_t sr = 0;
    assert(WavIO::loadWav(tmpPath, inL, inR, sr));
    assert(sr == 48000);
    assert(inL.size() == 4800);
    for (size_t i = 0; i < 4800; ++i) {
        assert(std::abs(inL[i] - L[i]) < 1e-3f);
        assert(std::abs(inR[i] - R[i]) < 1e-3f);
    }
    _wremove(tmpPath.c_str());
    printf("[PASS] testWavRoundTrip\n");
}

static void testMidiA4AndModes() {
    CapturePluginEngine eng;
    eng.setHostSampleRate(48000.0f);

    // Create a linear ramp clip [0.001 .. 1.000] so every sample index is uniquely identifiable
    const size_t N = 2000;
    std::vector<float> L(N), R(N);
    for (size_t i = 0; i < N; ++i) {
        float v = static_cast<float>(i + 1) / static_cast<float>(N);
        L[i] = v;
        R[i] = v;
    }
    eng.getClip().setAudio(L, R, 48000, false);

    std::vector<float> outL(512), outR(512);

    // 1. Non-A4 notes must stay silent
    eng.queueMidiEvent(0, 0x90, 60, 127); // C4
    eng.queueMidiEvent(0, 0x90, 67, 127); // G4
    eng.queueMidiEvent(0, 0x90, 71, 127); // B4
    eng.process(outL.data(), outR.data(), 512);
    for (float v : outL) assert(std::abs(v) < 1e-6f);

    // 2. A4 Note On triggers playback and respects velocity
    eng.setParameter(PARAM_PLAY_MODE, 0.0f); // RESTART
    eng.setParameter(PARAM_GATE_MODE, 0.0f); // GATE
    eng.queueMidiEvent(0, 0x90, 69, 127);
    eng.process(outL.data(), outR.data(), 512);
    float fullAmp = outL[300]; // past 3ms (144-sample) ramp
    assert(fullAmp > 0.1f);

    // Note Off stops in GATE mode
    eng.queueMidiEvent(0, 0x80, 69, 0);
    eng.process(outL.data(), outR.data(), 512);
    assert(std::abs(outL[511]) < 1e-5f);

    // Half velocity (~64/127)
    eng.queueMidiEvent(0, 0x90, 69, 64);
    eng.process(outL.data(), outR.data(), 512);
    float halfAmp = outL[300];
    assert(std::abs(halfAmp - fullAmp * (64.0f / 127.0f)) < 0.02f);
    eng.queueMidiEvent(0, 0x80, 69, 0);
    eng.process(outL.data(), outR.data(), 512);

    // 3. CONTINUE mode + A#4 reset
    eng.setParameter(PARAM_PLAY_MODE, 1.0f); // CONTINUE
    eng.queueMidiEvent(0, 0x90, 69, 127);
    eng.process(outL.data(), outR.data(), 400);
    float firstEnd = outL[399];
    eng.queueMidiEvent(0, 0x80, 69, 0);
    eng.process(outL.data(), outR.data(), 300); // fade out & pause

    // Resume on A4: should continue past firstEnd, NOT restart near 0
    eng.queueMidiEvent(0, 0x90, 69, 127);
    eng.process(outL.data(), outR.data(), 300);
    float resumed = outL[200];
    assert(resumed > firstEnd);

    // Release and send A#4 (70) to reset cursor
    eng.queueMidiEvent(0, 0x80, 69, 0);
    eng.process(outL.data(), outR.data(), 300);
    eng.queueMidiEvent(0, 0x90, 70, 100); // A#4 reset
    eng.process(outL.data(), outR.data(), 64);
    eng.queueMidiEvent(0, 0x90, 69, 127);
    eng.process(outL.data(), outR.data(), 300);
    assert(std::abs(outL[200] - (201.0f / static_cast<float>(N))) < 0.02f);

    // 4. 1-SHOT ignores Note Off
    eng.queueMidiEvent(0, 0x80, 69, 0);
    eng.process(outL.data(), outR.data(), 300);
    eng.setParameter(PARAM_PLAY_MODE, 0.0f);
    eng.setParameter(PARAM_GATE_MODE, 1.0f); // 1-SHOT
    eng.queueMidiEvent(0, 0x90, 69, 127);
    eng.queueMidiEvent(10, 0x80, 69, 0);
    eng.process(outL.data(), outR.data(), 400);
    assert(outL[350] > 0.1f);

    // 5. Chunk serialize / deserialize
    eng.setParameter(PARAM_GAIN, 0.75f);
    eng.setParameter(PARAM_LOOP, 1.0f);
    auto chunk = eng.serializeChunk();
    assert(!chunk.empty());

    CapturePluginEngine eng2;
    assert(eng2.deserializeChunk(chunk.data(), chunk.size()));
    assert(std::abs(eng2.getParameter(PARAM_GAIN) - 0.75f) < 1e-5f);
    assert(eng2.getLoop() == true);
    assert(eng2.getClip().getSnapshot()->numFrames() == N);

    printf("[PASS] testMidiA4AndModes\n");
}

int main() {
    testAudioClipEdits();
    testWavRoundTrip();
    testMidiA4AndModes();
    printf("All lmmsaudiocapture unit tests passed!\n");
    return 0;
}
