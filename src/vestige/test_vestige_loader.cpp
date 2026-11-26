#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../include/vestige/vestige.h"

#ifndef effGetChunk
#define effGetChunk 23
#endif
#ifndef effSetChunk
#define effSetChunk 24
#endif

static intptr_t __cdecl hostCallback(AEffect* effect, int32_t opcode, int32_t index,
                                     intptr_t value, void* ptr, float opt) {
    (void)effect; (void)index; (void)value; (void)ptr; (void)opt;
    if (opcode == audioMasterVersion) return 2400;
    return 0;
}

int main(int argc, char** argv) {
    const char* dllPath = (argc > 1) ? argv[1] : "dist/myaudiocaptureforlmms.dll";
    HMODULE hMod = LoadLibraryA(dllPath);
    if (!hMod) {
        printf("FAIL: LoadLibraryA(%s) failed (err=%lu)\n", dllPath, GetLastError());
        return 1;
    }

    using EntryFn = AEffect* (__cdecl *)(audioMasterCallback);
    auto entry = reinterpret_cast<EntryFn>(GetProcAddress(hMod, "VSTPluginMain"));
    if (!entry) {
        printf("FAIL: VSTPluginMain not exported\n");
        FreeLibrary(hMod);
        return 1;
    }

    AEffect* fx = entry(hostCallback);
    if (!fx || fx->magic != kEffectMagic) {
        printf("FAIL: Invalid AEffect or magic\n");
        FreeLibrary(hMod);
        return 1;
    }

    if (!(fx->flags & effFlagsIsSynth) || fx->numOutputs != 2) {
        printf("FAIL: Expected synth flags and 2 outputs\n");
        FreeLibrary(hMod);
        return 1;
    }

    fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.0f);
    fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    fx->dispatcher(fx, effSetBlockSize, 0, 256, nullptr, 0.0f);
    fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0.0f);

    char name[64]{};
    fx->dispatcher(fx, effGetEffectName, 0, 0, name, 0.0f);
    if (std::strcmp(name, "myaudiocaptureforlmms") != 0) {
        printf("FAIL: Unexpected effect name: %s\n", name);
        return 1;
    }

    // Send A4 Note On via effProcessEvents
    VstMidiEvent midi{};
    midi.type = kVstMidiType;
    midi.byteSize = sizeof(VstMidiEvent);
    midi.midiData[0] = static_cast<char>(0x90);
    midi.midiData[1] = 69;
    midi.midiData[2] = 100;

    VstEvents evs{};
    evs.numEvents = 1;
    evs.events[0] = reinterpret_cast<VstEvent*>(&midi);
    fx->dispatcher(fx, effProcessEvents, 0, 0, &evs, 0.0f);

    std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
    float* outs[2] = { outL.data(), outR.data() };
    fx->processReplacing(fx, nullptr, outs, 256);

    void* chunkPtr = nullptr;
    intptr_t chunkSize = fx->dispatcher(fx, effGetChunk, 0, 0, &chunkPtr, 0.0f);
    if (chunkSize <= 0 || !chunkPtr) {
        printf("FAIL: effGetChunk returned empty\n");
        return 1;
    }
    if (fx->dispatcher(fx, effSetChunk, 0, chunkSize, chunkPtr, 0.0f) != 1) {
        printf("FAIL: effSetChunk failed\n");
        return 1;
    }

    fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0.0f);
    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0.0f);
    FreeLibrary(hMod);

    printf("[PASS] VeSTige loader verified %s (chunk=%lld bytes)\n", dllPath, static_cast<long long>(chunkSize));
    return 0;
}
