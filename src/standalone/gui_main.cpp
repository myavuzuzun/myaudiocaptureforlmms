#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <thread>
#include <atomic>
#include "../dsp/CapturePluginEngine.hpp"
#include "../gui/CaptureUI.hpp"

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    LMAC::CapturePluginEngine engine;
    constexpr uint32_t SR = 48000;
    engine.setHostSampleRate(static_cast<float>(SR));

    // Lightweight waveOut playback thread for standalone audition
    std::atomic<bool> audioRunning{true};
    std::thread audioThread([&]() {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = SR;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = SR * 4;

        HWAVEOUT hWaveOut = nullptr;
        if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR || !hWaveOut) {
            return;
        }

        constexpr int NUM_BUFS = 4;
        constexpr int FRAMES = 512;
        std::vector<std::vector<int16_t>> bufs(NUM_BUFS, std::vector<int16_t>(FRAMES * 2, 0));
        WAVEHDR hdrs[NUM_BUFS]{};
        std::vector<float> outL(FRAMES), outR(FRAMES);

        for (int i = 0; i < NUM_BUFS; ++i) {
            hdrs[i].lpData = reinterpret_cast<LPSTR>(bufs[i].data());
            hdrs[i].dwBufferLength = static_cast<DWORD>(bufs[i].size() * sizeof(int16_t));
            waveOutPrepareHeader(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
            waveOutWrite(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
        }

        while (audioRunning.load(std::memory_order_acquire)) {
            bool anyQueued = false;
            for (int i = 0; i < NUM_BUFS; ++i) {
                if (hdrs[i].dwFlags & WHDR_DONE) {
                    engine.process(outL.data(), outR.data(), FRAMES);
                    for (int f = 0; f < FRAMES; ++f) {
                        bufs[i][f * 2] = LMAC::WavIO::floatToInt16(outL[f]);
                        bufs[i][f * 2 + 1] = LMAC::WavIO::floatToInt16(outR[f]);
                    }
                    hdrs[i].dwFlags &= ~WHDR_DONE;
                    waveOutWrite(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
                    anyQueued = true;
                }
            }
            if (!anyQueued) Sleep(5);
        }

        waveOutReset(hWaveOut);
        for (int i = 0; i < NUM_BUFS; ++i) {
            waveOutUnprepareHeader(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
        }
        waveOutClose(hWaveOut);
    });

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "LMACStandaloneHost";
    RegisterClassExA(&wc);

    RECT rc{ 0, 0, LMAC::CaptureUI::UI_WIDTH, LMAC::CaptureUI::UI_HEIGHT };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);

    HWND hMain = CreateWindowExA(
        0, "LMACStandaloneHost", "myaudiocaptureforlmms",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr
    );

    LMAC::CaptureUI ui(&engine);
    ui.openWindow(hMain);

    ShowWindow(hMain, nCmdShow);
    UpdateWindow(hMain);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    ui.closeWindow();
    audioRunning.store(false, std::memory_order_release);
    if (audioThread.joinable()) audioThread.join();
    return 0;
}
