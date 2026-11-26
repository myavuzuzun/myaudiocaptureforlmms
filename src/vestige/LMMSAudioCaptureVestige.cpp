#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Open-source VeSTige header (Javier Serrano Polo / LMMS)
#include "../../include/vestige/vestige.h"
#include "../dsp/CapturePluginEngine.hpp"
#include "../gui/CaptureUI.hpp"

#ifndef effSetProgramName
#define effSetProgramName 4
#endif
#ifndef effGetParamLabel
#define effGetParamLabel 6
#endif
#ifndef effGetParamDisplay
#define effGetParamDisplay 7
#endif
#ifndef effGetChunk
#define effGetChunk 23
#endif
#ifndef effSetChunk
#define effSetChunk 24
#endif
#ifndef effGetProgramNameIndexed
#define effGetProgramNameIndexed 29
#endif
#ifndef effGetTailSize
#define effGetTailSize 52
#endif
#ifndef effFlagsProgramChunks
#define effFlagsProgramChunks (1 << 5)
#endif

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>

struct ScopedFtzDaz {
    unsigned int oldMxcsr;
    ScopedFtzDaz() {
        oldMxcsr = _mm_getcsr();
        _mm_setcsr(oldMxcsr | 0x8040);
    }
    ~ScopedFtzDaz() {
        _mm_setcsr(oldMxcsr);
    }
};
#else
struct ScopedFtzDaz {};
#endif

struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};

struct LMACEffectBlock {
    AEffect fx;
    void (__cdecl *processDoubleReplacing)(AEffect*, double**, double**, int);
    char future[56];
};

class LMACCaptureVestigeWrapper {
public:
    explicit LMACCaptureVestigeWrapper(audioMasterCallback master) : audioMaster(master) {
        std::memset(&effectBlock, 0, sizeof(LMACEffectBlock));
        AEffect& fx = effectBlock.fx;
        fx.magic = kEffectMagic;
        fx.dispatcher = dispatcherCallback;
        fx.process = processCallback;
        fx.processReplacing = processReplacingCallback;
        fx.setParameter = setParameterCallback;
        fx.getParameter = getParameterCallback;
        fx.numPrograms = 1;
        fx.numParams = LMAC::NUM_PARAMS;
        fx.numInputs = 0;
        fx.numOutputs = 2;
        fx.flags = effFlagsHasEditor | effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
        fx.uniqueID = CCONST('L', 'M', 'A', 'C');
        fx.version = 1000;
        fx.object = this;
        fx.initialDelay = 0;
        fx.unkown_float = 1.0f;

        editorRect.top = 0;
        editorRect.left = 0;
        editorRect.bottom = LMAC::CaptureUI::UI_HEIGHT;
        editorRect.right = LMAC::CaptureUI::UI_WIDTH;

        engine.setHostSampleRate(44100.0f);
    }

    ~LMACCaptureVestigeWrapper() {
        closeEditor();
    }

    AEffect* getAEffect() { return &effectBlock.fx; }

    static intptr_t __cdecl dispatcherCallback(AEffect* effect, int opcode, int index,
                                                 intptr_t value, void* ptr, float opt) {
        if (!effect || !effect->object) return 0;
        auto* self = static_cast<LMACCaptureVestigeWrapper*>(effect->object);
        return self->dispatcher(opcode, index, value, ptr, opt);
    }

    static void __cdecl processCallback(AEffect* effect, float** inputs, float** outputs, int sampleFrames) {
        processReplacingCallback(effect, inputs, outputs, sampleFrames);
    }

    static void __cdecl processReplacingCallback(AEffect* effect, float** inputs, float** outputs, int sampleFrames) {
        (void)inputs;
        if (!effect || !effect->object || !outputs || sampleFrames <= 0) return;
        auto* self = static_cast<LMACCaptureVestigeWrapper*>(effect->object);
        ScopedFtzDaz ftzDaz;
        self->engine.process(outputs[0], outputs[1], sampleFrames);
    }

    static void __cdecl setParameterCallback(AEffect* effect, int index, float parameter) {
        if (!effect || !effect->object) return;
        auto* self = static_cast<LMACCaptureVestigeWrapper*>(effect->object);
        self->engine.setParameter(index, parameter);
    }

    static float __cdecl getParameterCallback(AEffect* effect, int index) {
        if (!effect || !effect->object) return 0.0f;
        auto* self = static_cast<LMACCaptureVestigeWrapper*>(effect->object);
        return self->engine.getParameter(index);
    }

    static void onUiParamChanged(void* userData, int paramId, float value) {
        auto* self = static_cast<LMACCaptureVestigeWrapper*>(userData);
        if (self && self->audioMaster) {
            self->audioMaster(&self->effectBlock.fx, audioMasterAutomate, paramId, 0, nullptr, value);
        }
    }

    intptr_t dispatcher(int opcode, int index, intptr_t value, void* ptr, float opt) {
        switch (opcode) {
            case effOpen:
                return 0;

            case effClose:
                closeEditor();
                delete this;
                return 0;

            case effSetProgram:
            case effGetProgram:
            case effSetProgramName:
                return 0;

            case effGetProgramName:
            case effGetProgramNameIndexed:
                if (ptr) {
                    std::snprintf(static_cast<char*>(ptr), 24, "Default");
                }
                return 1;

            case effSetSampleRate:
                if (opt > 0.0f) {
                    engine.setHostSampleRate(opt);
                }
                return 0;

            case effSetBlockSize:
                return 0;

            case effMainsChanged:
                if (value == 0) {
                    engine.clearMidiQueue();
                } else if (audioMaster) {
                    audioMaster(&effectBlock.fx, audioMasterWantMidi, 0, 1, nullptr, 0.0f);
                }
                return 0;

            case effProcessEvents: {
                if (!ptr) return 0;
                const auto* events = static_cast<const VstEvents*>(ptr);
                for (int i = 0; i < events->numEvents; ++i) {
                    const VstEvent* ev = events->events[i];
                    if (!ev) continue;
                    const auto* midi = reinterpret_cast<const VstMidiEvent*>(ev);
                    if (midi->type == kVstMidiType) {
                        engine.queueMidiEvent(
                            midi->deltaFrames,
                            static_cast<uint8_t>(midi->midiData[0]),
                            static_cast<uint8_t>(midi->midiData[1]),
                            static_cast<uint8_t>(midi->midiData[2])
                        );
                    }
                }
                return 1;
            }

            case effGetParamName:
                if (ptr) {
                    const char* names[LMAC::NUM_PARAMS] = {
                        "Gain", "PlayMode", "GateMode", "Loop", "TrimStart", "TrimEnd"
                    };
                    const char* nm = (index >= 0 && index < LMAC::NUM_PARAMS) ? names[index] : "";
                    std::snprintf(static_cast<char*>(ptr), 32, "%s", nm);
                }
                return 0;

            case effGetParamDisplay:
                if (ptr) {
                    char* dst = static_cast<char*>(ptr);
                    switch (index) {
                        case LMAC::PARAM_GAIN:
                            std::snprintf(dst, 32, "%+.1f", engine.getGainDb());
                            break;
                        case LMAC::PARAM_PLAY_MODE:
                            std::snprintf(dst, 32, "%s",
                                          engine.getPlayMode() == LMAC::PlayMode::CONTINUE ? "Continue" : "Restart");
                            break;
                        case LMAC::PARAM_GATE_MODE:
                            std::snprintf(dst, 32, "%s",
                                          engine.getGateMode() == LMAC::GateMode::ONE_SHOT ? "1-Shot" : "Gate");
                            break;
                        case LMAC::PARAM_LOOP:
                            std::snprintf(dst, 32, "%s", engine.getLoop() ? "On" : "Off");
                            break;
                        case LMAC::PARAM_TRIM_START:
                            std::snprintf(dst, 32, "%.1f%%", engine.getParameter(LMAC::PARAM_TRIM_START) * 100.0f);
                            break;
                        case LMAC::PARAM_TRIM_END:
                            std::snprintf(dst, 32, "%.1f%%", engine.getParameter(LMAC::PARAM_TRIM_END) * 100.0f);
                            break;
                        default:
                            dst[0] = '\0';
                            break;
                    }
                }
                return 0;

            case effGetParamLabel:
                if (ptr) {
                    std::snprintf(static_cast<char*>(ptr), 16, "%s",
                                  (index == LMAC::PARAM_GAIN) ? "dB" : "");
                }
                return 0;

            case effEditGetRect:
                if (ptr) {
                    *static_cast<ERect**>(ptr) = &editorRect;
                    return 1;
                }
                return 0;

            case effEditOpen:
                openEditor(ptr);
                return 1;

            case effEditClose:
                closeEditor();
                return 0;

            case effEditIdle:
                return 0;

            case effGetChunk:
                if (ptr) {
                    chunkCache = engine.serializeChunk();
                    *static_cast<void**>(ptr) = chunkCache.empty() ? nullptr : chunkCache.data();
                    return static_cast<intptr_t>(chunkCache.size());
                }
                return 0;

            case effSetChunk:
                if (ptr && value > 0) {
                    return engine.deserializeChunk(ptr, static_cast<size_t>(value)) ? 1 : 0;
                }
                return 0;

            case effGetPlugCategory:
                return kPlugCategSynth;

            case effGetEffectName:
                if (ptr) std::snprintf(static_cast<char*>(ptr), 64, "lmmsaudiocapture");
                return 1;

            case effGetVendorString:
                if (ptr) std::snprintf(static_cast<char*>(ptr), 64, "lmmsaudiocapture");
                return 1;

            case effGetProductString:
                if (ptr) std::snprintf(static_cast<char*>(ptr), 64, "lmmsaudiocapture");
                return 1;

            case effGetVendorVersion:
                return 1000;

            case effGetVstVersion:
                return 2400;

            case effCanDo:
                if (ptr) {
                    const char* s = static_cast<const char*>(ptr);
                    if (std::strcmp(s, "receiveVstEvents") == 0 ||
                        std::strcmp(s, "receiveVstMidiEvent") == 0) {
                        return 1;
                    }
                }
                return 0;

            case effGetTailSize:
                return 0;

            default:
                return 0;
        }
    }

private:
    void openEditor(void* parentWin) {
        closeEditor();
        ui = std::make_unique<LMAC::CaptureUI>(&engine, onUiParamChanged, this);
        ui->openWindow(parentWin);
    }

    void closeEditor() {
        if (ui) {
            ui->closeWindow();
            ui.reset();
        }
    }

    audioMasterCallback audioMaster = nullptr;
    LMACEffectBlock effectBlock{};
    ERect editorRect{};
    LMAC::CapturePluginEngine engine;
    std::unique_ptr<LMAC::CaptureUI> ui;
    std::vector<uint8_t> chunkCache;
};

extern "C" {
    AEffect* __cdecl VSTPluginMain(audioMasterCallback audioMaster) {
        if (audioMaster && audioMaster(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) {
            return nullptr;
        }
        auto* wrapper = new LMACCaptureVestigeWrapper(audioMaster);
        return wrapper->getAEffect();
    }
}
