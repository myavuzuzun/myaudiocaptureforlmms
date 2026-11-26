#pragma once

#if defined(_WIN32)
#include <commdlg.h>
#include <shellapi.h>
#include "../common/Direct2DTheme.hpp"
#include "../dsp/CapturePluginEngine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace LMAC {

class CaptureUI {
public:
    static constexpr int UI_WIDTH = 640;
    static constexpr int UI_HEIGHT = 360;

    using ParamChangedCallback = void (*)(void* userData, int paramId, float value);

    CaptureUI(CapturePluginEngine* eng, ParamChangedCallback cb = nullptr, void* cbUser = nullptr)
        : engine(eng), onParamCb(cb), onParamUser(cbUser) {
        refreshDevices(false);
        initDirect2D();
    }

    ~CaptureUI() {
        closeWindow();
        cleanupDirect2D();
        if (engine) engine->reclaimRetiredSnapshots();
    }

    void setParamCallback(ParamChangedCallback cb, void* cbUser) {
        onParamCb = cb;
        onParamUser = cbUser;
    }

    bool openWindow(void* parentWin) {
        closeWindow();
        parentHWnd = static_cast<HWND>(parentWin);
        HINSTANCE hInst = getModuleInstance();
        registerWindowClass(hInst);

        hWnd = CreateWindowExA(
            0,
            "LMACCaptureWindowClass",
            "",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, UI_WIDTH, UI_HEIGHT,
            parentHWnd,
            nullptr,
            hInst,
            this
        );
        if (!hWnd) return false;

        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        DragAcceptFiles(hWnd, TRUE);
        createDeviceResources();
        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
        SetTimer(hWnd, TIMER_ID, 33, nullptr);
        return true;
    }

    void closeWindow() {
        if (hWnd) {
            KillTimer(hWnd, TIMER_ID);
            DestroyWindow(hWnd);
            hWnd = nullptr;
        }
        discardDeviceResources();
    }

    HWND getHWnd() const { return hWnd; }

private:
    static constexpr UINT_PTR TIMER_ID = 7001;

    struct RectF {
        float x, y, w, h;
        bool contains(float px, float py) const {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
        D2D1_RECT_F toD2D() const { return D2D1::RectF(x, y, x + w, y + h); }
    };

    enum class DragMode {
        NONE,
        SELECT_WAVE,
        TRIM_START,
        TRIM_END,
        PAN_WAVE,
        SCROLLBAR,
        GAIN_SLIDER
    };

    // Layout constants
    static constexpr float PAD_X = 12.0f;

    // Row 1: Top Capture Bar (y: 10..38)
    static constexpr RectF R_REC       = { 12.0f, 10.0f, 52.0f, 28.0f };
    static constexpr RectF R_TIME      = { 68.0f, 10.0f, 58.0f, 28.0f };
    static constexpr RectF R_METER     = { 129.0f, 14.0f, 20.0f, 20.0f };
    static constexpr RectF R_BACKEND   = { 155.0f, 10.0f, 78.0f, 28.0f };
    static constexpr RectF R_DEV_PREV  = { 237.0f, 10.0f, 20.0f, 28.0f };
    static constexpr RectF R_DEV_NAME  = { 257.0f, 10.0f, 208.0f, 28.0f };
    static constexpr RectF R_DEV_NEXT  = { 465.0f, 10.0f, 20.0f, 28.0f };
    static constexpr RectF R_CHAN      = { 491.0f, 10.0f, 66.0f, 28.0f };
    static constexpr RectF R_CAP       = { 563.0f, 10.0f, 65.0f, 28.0f };

    // Row 2: Waveform Card (y: 48..252)
    static constexpr RectF R_WAVE_CARD = { 12.0f, 48.0f, 616.0f, 204.0f };
    static constexpr RectF R_WAVE_AREA = { 16.0f, 54.0f, 608.0f, 174.0f };
    static constexpr RectF R_ZOOM_OUT  = { 16.0f, 232.0f, 22.0f, 16.0f };
    static constexpr RectF R_ZOOM_IN   = { 41.0f, 232.0f, 22.0f, 16.0f };
    static constexpr RectF R_ZOOM_FIT  = { 66.0f, 232.0f, 30.0f, 16.0f };
    static constexpr RectF R_SCROLLBAR = { 102.0f, 235.0f, 522.0f, 10.0f };

    // Row 3: Edit Toolbar (y: 262..290)
    static constexpr int NUM_EDIT_BTNS = 13;

    // Row 4: Bottom Playback & MIDI Bar (y: 304..346)
    static constexpr RectF R_PLAY      = { 12.0f, 308.0f, 54.0f, 28.0f };
    static constexpr RectF R_RST       = { 70.0f, 308.0f, 36.0f, 28.0f };
    static constexpr RectF R_MODE_RST  = { 114.0f, 308.0f, 62.0f, 28.0f };
    static constexpr RectF R_MODE_CONT = { 176.0f, 308.0f, 66.0f, 28.0f };
    static constexpr RectF R_GATE      = { 250.0f, 308.0f, 44.0f, 28.0f };
    static constexpr RectF R_ONESHOT   = { 294.0f, 308.0f, 54.0f, 28.0f };
    static constexpr RectF R_LOOP      = { 356.0f, 308.0f, 46.0f, 28.0f };
    static constexpr RectF R_GAIN_LBL  = { 410.0f, 300.0f, 218.0f, 16.0f };
    static constexpr RectF R_GAIN_SLD  = { 410.0f, 318.0f, 218.0f, 18.0f };
    static constexpr RectF R_MIDI_HINT = { 12.0f, 340.0f, 616.0f, 16.0f };

    static HINSTANCE getModuleInstance() {
        HMODULE hMod = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&CaptureUI::getModuleInstance),
            &hMod
        );
        return hMod ? hMod : GetModuleHandleA(nullptr);
    }

    static void registerWindowClass(HINSTANCE hInst) {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "LMACCaptureWindowClass";
        RegisterClassExA(&wc);
        registered = true;
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        CaptureUI* self = nullptr;
        if (msg == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
            self = static_cast<CaptureUI*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<CaptureUI*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }
        if (self) {
            return self->handleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    void initDirect2D() {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pD2DFactory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&pDWriteFactory));
        if (pDWriteFactory) {
            Fraudize::UITheme::createTextFormat(pDWriteFactory, L"Segoe UI", 10.5f, DWRITE_FONT_WEIGHT_BOLD,
                                                DWRITE_TEXT_ALIGNMENT_CENTER, true, &fmtBold);
            Fraudize::UITheme::createTextFormat(pDWriteFactory, L"Segoe UI", 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                DWRITE_TEXT_ALIGNMENT_CENTER, true, &fmtSmall);
            Fraudize::UITheme::createTextFormat(pDWriteFactory, L"Consolas", 10.5f, DWRITE_FONT_WEIGHT_BOLD,
                                                DWRITE_TEXT_ALIGNMENT_CENTER, false, &fmtMono);
            Fraudize::UITheme::createTextFormat(pDWriteFactory, L"Consolas", 9.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_TEXT_ALIGNMENT_TRAILING, false, &fmtStatus);
            Fraudize::UITheme::createTextFormat(pDWriteFactory, L"Segoe UI", 9.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_TEXT_ALIGNMENT_LEADING, false, &fmtHint);
        }
    }

    void cleanupDirect2D() {
        discardDeviceResources();
        if (fmtBold) { fmtBold->Release(); fmtBold = nullptr; }
        if (fmtSmall) { fmtSmall->Release(); fmtSmall = nullptr; }
        if (fmtMono) { fmtMono->Release(); fmtMono = nullptr; }
        if (fmtStatus) { fmtStatus->Release(); fmtStatus = nullptr; }
        if (fmtHint) { fmtHint->Release(); fmtHint = nullptr; }
        if (pDWriteFactory) { pDWriteFactory->Release(); pDWriteFactory = nullptr; }
        if (pD2DFactory) { pD2DFactory->Release(); pD2DFactory = nullptr; }
    }

    void createDeviceResources() {
        if (!hWnd || !pD2DFactory || pRenderTarget) return;
        RECT rc{};
        GetClientRect(hWnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
            static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top))
        );
        HRESULT hr = pD2DFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hWnd, size),
            &pRenderTarget
        );
        if (SUCCEEDED(hr) && pRenderTarget) {
            pRenderTarget->CreateSolidColorBrush(Fraudize::Colors::TextActive.toD2D(), &pBrush);
        }
    }

    void discardDeviceResources() {
        if (pBrush) { pBrush->Release(); pBrush = nullptr; }
        if (pRenderTarget) { pRenderTarget->Release(); pRenderTarget = nullptr; }
    }

    void refreshDevices(bool preserveId = true) {
        std::wstring prevId;
        if (preserveId && deviceIndex < devices.size()) {
            prevId = devices[deviceIndex].id;
        }
        devices = AudioCaptureEngine::enumerateDevices(engine->getCaptureBackend());
        deviceIndex = 0;
        if (!prevId.empty()) {
            for (size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].id == prevId) {
                    deviceIndex = i;
                    break;
                }
            }
        }
    }

    void notifyParam(int paramId, float value) {
        engine->setParameter(paramId, value);
        if (onParamCb) {
            onParamCb(onParamUser, paramId, value);
        }
    }

    RectF getEditBtnRect(int idx) const {
        float totalW = UI_WIDTH - 2.0f * PAD_X;
        float gap = 4.0f;
        float btnW = (totalW - gap * (NUM_EDIT_BTNS - 1)) / static_cast<float>(NUM_EDIT_BTNS);
        return { PAD_X + idx * (btnW + gap), 262.0f, btnW, 28.0f };
    }

    // Coordinate helpers for waveform
    void clampView(size_t totalFrames) {
        if (totalFrames == 0) {
            viewStart = 0.0;
            viewLen = 1.0;
            return;
        }
        double total = static_cast<double>(totalFrames);
        double minLen = std::min(total, 64.0);
        viewLen = std::clamp(viewLen, minLen, total);
        viewStart = std::clamp(viewStart, 0.0, std::max(0.0, total - viewLen));
    }

    double xToSample(float x, size_t totalFrames) const {
        if (totalFrames == 0) return 0.0;
        double norm = std::clamp(static_cast<double>(x - R_WAVE_AREA.x) / R_WAVE_AREA.w, 0.0, 1.0);
        return std::clamp(viewStart + norm * viewLen, 0.0, static_cast<double>(totalFrames));
    }

    float sampleToX(double sample) const {
        if (viewLen <= 0.0) return R_WAVE_AREA.x;
        double norm = (sample - viewStart) / viewLen;
        return R_WAVE_AREA.x + static_cast<float>(norm * R_WAVE_AREA.w);
    }

    void resetViewToClip() {
        auto snap = engine->getClip().getSnapshot();
        size_t n = snap ? snap->numFrames() : 0;
        viewStart = 0.0;
        viewLen = (n > 0) ? static_cast<double>(n) : 1.0;
        selStart = 0;
        selEnd = 0;
        lastClipVersion = snap ? snap->version : 0;
    }

    void syncViewIfClipReplaced() {
        auto snap = engine->getClip().getSnapshot();
        if (!snap) return;
        if (snap->version != lastClipVersion) {
            size_t n = snap->numFrames();
            if (n == 0) {
                viewStart = 0.0;
                viewLen = 1.0;
                selStart = selEnd = 0;
            } else {
                if (viewLen <= 1.0 || viewStart >= static_cast<double>(n)) {
                    viewStart = 0.0;
                    viewLen = static_cast<double>(n);
                } else {
                    clampView(n);
                }
                selStart = std::min(selStart, n);
                selEnd = std::min(selEnd, n);
            }
            lastClipVersion = snap->version;
        }
    }

    void getActiveRange(size_t& s, size_t& e) const {
        if (selEnd > selStart) {
            s = selStart;
            e = selEnd;
        } else {
            s = 0;
            e = 0;
        }
    }

    void executeEditButton(int idx) {
        auto& clip = engine->getClip();
        size_t s = 0, e = 0;
        getActiveRange(s, e);

        switch (idx) {
            case 0: // TRIM (crop to selection or trim handles)
                clip.cropToRange(s, e);
                selStart = selEnd = 0;
                resetViewToClip();
                engine->syncTrimParamsFromClip();
                break;
            case 1: // DEL
                if (e > s) {
                    clip.deleteRange(s, e);
                    selStart = selEnd = 0;
                    resetViewToClip();
                    engine->syncTrimParamsFromClip();
                }
                break;
            case 2: // SILENCE
                clip.silenceRange(s, e);
                break;
            case 3: // FADE IN
                clip.fadeIn(s, e);
                break;
            case 4: // FADE OUT
                clip.fadeOut(s, e);
                break;
            case 5: // NORM
                clip.normalize(s, e, -0.5f);
                break;
            case 6: // REV
                clip.reverse(s, e);
                break;
            case 7: // DC
                clip.removeDC(s, e);
                break;
            case 8: // UNDO
                clip.undo();
                selStart = selEnd = 0;
                resetViewToClip();
                engine->syncTrimParamsFromClip();
                break;
            case 9: // REDO
                clip.redo();
                selStart = selEnd = 0;
                resetViewToClip();
                engine->syncTrimParamsFromClip();
                break;
            case 10: // CLR
                clip.clear();
                selStart = selEnd = 0;
                resetViewToClip();
                engine->syncTrimParamsFromClip();
                break;
            case 11: // LOAD
                openWavDialog();
                break;
            case 12: // SAVE
                saveWavDialog();
                break;
            default:
                break;
        }
    }

    void openWavDialog() {
        wchar_t fileBuf[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hWnd;
        ofn.lpstrFilter = L"WAV Audio (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            loadWavPath(fileBuf);
        }
    }

    void saveWavDialog() {
        auto snap = engine->getClip().getSnapshot();
        if (!snap || snap->empty()) return;

        wchar_t fileBuf[MAX_PATH] = L"capture.wav";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hWnd;
        ofn.lpstrFilter = L"WAV Audio (*.wav)\0*.wav\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"wav";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&ofn)) {
            const auto& srcL = snap->L();
            const auto& srcR = snap->R();
            std::vector<float> outL(srcL.begin() + snap->trimStart, srcL.begin() + snap->trimEnd);
            std::vector<float> outR(srcR.begin() + snap->trimStart, srcR.begin() + snap->trimEnd);
            WavIO::saveWav16(fileBuf, outL, outR, snap->sampleRate);
        }
    }

    void loadWavPath(const std::wstring& path) {
        std::vector<float> L, R;
        uint32_t sr = 48000;
        if (WavIO::loadWav(path, L, R, sr)) {
            engine->getClip().setAudio(std::move(L), std::move(R), sr, true);
            engine->syncTrimParamsFromClip();
            engine->resetCursor();
            resetViewToClip();
        }
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_TIMER: {
                if (engine) engine->reclaimRetiredSnapshots();
                bool rec = engine->getCapture().isRecording();
                bool play = engine->isPlaying();
                auto snap = engine->getClip().getSnapshot();
                bool verChanged = snap && (snap->version != lastClipVersion);
                if (verChanged) {
                    syncViewIfClipReplaced();
                }
                if (rec || play || wasRecording || wasPlaying || verChanged) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                wasRecording = rec;
                wasPlaying = play;
                return 0;
            }

            case WM_PAINT:
                onPaint();
                ValidateRect(hwnd, nullptr);
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_DROPFILES: {
                HDROP hDrop = reinterpret_cast<HDROP>(wParam);
                wchar_t path[MAX_PATH] = L"";
                if (DragQueryFileW(hDrop, 0, path, MAX_PATH) > 0) {
                    loadWavPath(path);
                }
                DragFinish(hDrop);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONDBLCLK: {
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float my = static_cast<float>(GET_Y_LPARAM(lParam));
                if (R_WAVE_AREA.contains(mx, my)) {
                    auto snap = engine->getClip().getSnapshot();
                    if (snap && snap->numFrames() > 0) {
                        selStart = 0;
                        selEnd = snap->numFrames();
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (R_GAIN_SLD.contains(mx, my)) {
                    notifyParam(PARAM_GAIN, 0.5f);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONDOWN:
                SetFocus(hwnd);
                onMouseDown(static_cast<float>(GET_X_LPARAM(lParam)),
                            static_cast<float>(GET_Y_LPARAM(lParam)), false);
                return 0;

            case WM_MBUTTONDOWN:
                SetFocus(hwnd);
                onMouseDown(static_cast<float>(GET_X_LPARAM(lParam)),
                            static_cast<float>(GET_Y_LPARAM(lParam)), true);
                return 0;

            case WM_MOUSEMOVE:
                onMouseMove(static_cast<float>(GET_X_LPARAM(lParam)),
                            static_cast<float>(GET_Y_LPARAM(lParam)));
                return 0;

            case WM_LBUTTONUP:
            case WM_MBUTTONUP:
                if (dragMode != DragMode::NONE) {
                    dragMode = DragMode::NONE;
                    ReleaseCapture();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;

            case WM_MOUSEWHEEL: {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                float mx = static_cast<float>(pt.x);
                float my = static_cast<float>(pt.y);
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (R_WAVE_AREA.contains(mx, my)) {
                    auto snap = engine->getClip().getSnapshot();
                    size_t n = snap ? snap->numFrames() : 0;
                    if (n > 0) {
                        if (GetKeyState(VK_SHIFT) & 0x8000) {
                            double shift = viewLen * (delta > 0 ? -0.15 : 0.15);
                            viewStart += shift;
                            clampView(n);
                        } else {
                            double anchor = xToSample(mx, n);
                            double norm = (anchor - viewStart) / std::max(1.0, viewLen);
                            double factor = (delta > 0) ? 0.75 : 1.333333;
                            viewLen *= factor;
                            clampView(n);
                            viewStart = anchor - norm * viewLen;
                            clampView(n);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }

            case WM_KEYDOWN:
                onKeyDown(wParam);
                return 0;

            case WM_SIZE:
                if (pRenderTarget) {
                    D2D1_SIZE_U sz = D2D1::SizeU(LOWORD(lParam), HIWORD(lParam));
                    pRenderTarget->Resize(sz);
                }
                return 0;
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    void onKeyDown(WPARAM key) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (key == 'Z' || key == 'z')) {
            executeEditButton(8); // UNDO
        } else if (ctrl && (key == 'Y' || key == 'y')) {
            executeEditButton(9); // REDO
        } else if (key == VK_DELETE) {
            executeEditButton(1); // DEL
        } else if (key == VK_SPACE) {
            if (engine->isPlaying()) engine->stopPreview();
            else engine->startPreview();
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }

    void onMouseDown(float mx, float my, bool middleBtn) {
        if (middleBtn && R_WAVE_AREA.contains(mx, my)) {
            dragMode = DragMode::PAN_WAVE;
            dragStartX = mx;
            dragStartView = viewStart;
            SetCapture(hWnd);
            return;
        }

        // Top Capture Bar
        if (R_REC.contains(mx, my)) {
            auto& cap = engine->getCapture();
            if (cap.isRecording()) {
                cap.stopRecording();
            } else {
                std::wstring devId = (deviceIndex < devices.size()) ? devices[deviceIndex].id : L"";
                cap.startRecording(engine->getCaptureBackend(), devId,
                                   engine->getChannelMode(), engine->getDurationCap());
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_BACKEND.contains(mx, my)) {
            if (!engine->getCapture().isRecording()) {
                int next = (static_cast<int>(engine->getCaptureBackend()) + 1) % 3;
                engine->setCaptureBackend(static_cast<CaptureBackend>(next));
                refreshDevices(false);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return;
        }
        if (R_DEV_PREV.contains(mx, my)) {
            if (!devices.empty()) {
                deviceIndex = (deviceIndex == 0) ? (devices.size() - 1) : (deviceIndex - 1);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return;
        }
        if (R_DEV_NEXT.contains(mx, my)) {
            if (!devices.empty()) {
                deviceIndex = (deviceIndex + 1) % devices.size();
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return;
        }
        if (R_DEV_NAME.contains(mx, my)) {
            refreshDevices(true);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_CHAN.contains(mx, my)) {
            int next = (static_cast<int>(engine->getChannelMode()) + 1) % 4;
            engine->setChannelMode(static_cast<ChannelMode>(next));
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_CAP.contains(mx, my)) {
            int next = (static_cast<int>(engine->getDurationCap()) + 1) % 5;
            engine->setDurationCap(static_cast<DurationCap>(next));
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        // Zoom controls & scrollbar
        auto snap = engine->getClip().getSnapshot();
        size_t n = snap ? snap->numFrames() : 0;
        if (R_ZOOM_OUT.contains(mx, my) && n > 0) {
            double center = viewStart + 0.5 * viewLen;
            viewLen *= 1.5;
            clampView(n);
            viewStart = center - 0.5 * viewLen;
            clampView(n);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_ZOOM_IN.contains(mx, my) && n > 0) {
            double center = viewStart + 0.5 * viewLen;
            viewLen *= 0.65;
            clampView(n);
            viewStart = center - 0.5 * viewLen;
            clampView(n);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_ZOOM_FIT.contains(mx, my) && n > 0) {
            viewStart = 0.0;
            viewLen = static_cast<double>(n);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_SCROLLBAR.contains(mx, my) && n > 0) {
            dragMode = DragMode::SCROLLBAR;
            SetCapture(hWnd);
            updateScrollFromMouse(mx, n);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        // Waveform area (trim handles or selection)
        if (R_WAVE_AREA.contains(mx, my) && n > 0 && !engine->getCapture().isRecording()) {
            float xTrimS = sampleToX(static_cast<double>(snap->trimStart));
            float xTrimE = sampleToX(static_cast<double>(snap->trimEnd));
            if (std::abs(mx - xTrimS) <= 7.0f) {
                dragMode = DragMode::TRIM_START;
                SetCapture(hWnd);
                return;
            }
            if (std::abs(mx - xTrimE) <= 7.0f) {
                dragMode = DragMode::TRIM_END;
                SetCapture(hWnd);
                return;
            }
            dragMode = DragMode::SELECT_WAVE;
            dragAnchorSample = static_cast<size_t>(std::round(xToSample(mx, n)));
            selStart = dragAnchorSample;
            selEnd = dragAnchorSample;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        // Edit toolbar
        for (int i = 0; i < NUM_EDIT_BTNS; ++i) {
            if (getEditBtnRect(i).contains(mx, my)) {
                executeEditButton(i);
                InvalidateRect(hWnd, nullptr, FALSE);
                return;
            }
        }

        // Bottom playback & MIDI bar
        if (R_PLAY.contains(mx, my)) {
            if (engine->isPlaying()) engine->stopPreview();
            else engine->startPreview();
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_RST.contains(mx, my)) {
            engine->resetCursor();
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_MODE_RST.contains(mx, my)) {
            notifyParam(PARAM_PLAY_MODE, 0.0f);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_MODE_CONT.contains(mx, my)) {
            notifyParam(PARAM_PLAY_MODE, 1.0f);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_GATE.contains(mx, my)) {
            notifyParam(PARAM_GATE_MODE, 0.0f);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_ONESHOT.contains(mx, my)) {
            notifyParam(PARAM_GATE_MODE, 1.0f);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_LOOP.contains(mx, my)) {
            notifyParam(PARAM_LOOP, engine->getLoop() ? 0.0f : 1.0f);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        if (R_GAIN_SLD.contains(mx, my)) {
            dragMode = DragMode::GAIN_SLIDER;
            SetCapture(hWnd);
            float norm = std::clamp((mx - R_GAIN_SLD.x) / R_GAIN_SLD.w, 0.0f, 1.0f);
            notifyParam(PARAM_GAIN, norm);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
    }

    void updateScrollFromMouse(float mx, size_t totalFrames) {
        double total = static_cast<double>(totalFrames);
        if (total <= viewLen) {
            viewStart = 0.0;
            return;
        }
        double norm = std::clamp(static_cast<double>(mx - R_SCROLLBAR.x) / R_SCROLLBAR.w, 0.0, 1.0);
        viewStart = norm * (total - viewLen);
        clampView(totalFrames);
    }

    void onMouseMove(float mx, float my) {
        hoverX = mx;
        hoverY = my;
        if (dragMode == DragMode::NONE) return;

        auto snap = engine->getClip().getSnapshot();
        size_t n = snap ? snap->numFrames() : 0;

        switch (dragMode) {
            case DragMode::GAIN_SLIDER: {
                float norm = std::clamp((mx - R_GAIN_SLD.x) / R_GAIN_SLD.w, 0.0f, 1.0f);
                notifyParam(PARAM_GAIN, norm);
                break;
            }
            case DragMode::SELECT_WAVE:
                if (n > 0) {
                    size_t cur = static_cast<size_t>(std::round(xToSample(mx, n)));
                    selStart = std::min(dragAnchorSample, cur);
                    selEnd = std::max(dragAnchorSample, cur);
                }
                break;
            case DragMode::TRIM_START:
                if (n > 1 && snap) {
                    size_t s = static_cast<size_t>(std::round(xToSample(mx, n)));
                    s = std::min(s, (snap->trimEnd > 0) ? (snap->trimEnd - 1) : 0);
                    engine->getClip().setTrim(s, snap->trimEnd);
                    engine->syncTrimParamsFromClip();
                }
                break;
            case DragMode::TRIM_END:
                if (n > 1 && snap) {
                    size_t e = static_cast<size_t>(std::round(xToSample(mx, n)));
                    e = std::clamp(e, snap->trimStart + 1, n);
                    engine->getClip().setTrim(snap->trimStart, e);
                    engine->syncTrimParamsFromClip();
                }
                break;
            case DragMode::PAN_WAVE:
                if (n > 0) {
                    double dx = static_cast<double>(dragStartX - mx);
                    viewStart = dragStartView + (dx / R_WAVE_AREA.w) * viewLen;
                    clampView(n);
                }
                break;
            case DragMode::SCROLLBAR:
                if (n > 0) {
                    updateScrollFromMouse(mx, n);
                }
                break;
            default:
                break;
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }

    void drawButton(const RectF& r, const wchar_t* label, bool active, bool enabled = true,
                    Fraudize::D2DColor activeBg = Fraudize::Colors::ButtonActiveBg,
                    Fraudize::D2DColor activeFg = Fraudize::Colors::ButtonActiveText) {
        if (!pRenderTarget || !pBrush) return;
        bool hover = enabled && r.contains(hoverX, hoverY);

        Fraudize::D2DColor bg = active ? activeBg : Fraudize::Colors::ButtonInactiveBg;
        if (!active && hover) {
            bg = Fraudize::D2DColor(bg.r + 0.04f, bg.g + 0.04f, bg.b + 0.05f, 1.0f);
        }
        pBrush->SetColor(bg.toD2D());
        pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(r.toD2D(), 3.0f, 3.0f), pBrush);

        pBrush->SetColor((active ? activeBg : Fraudize::Colors::ButtonInactiveBorder).toD2D());
        pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r.toD2D(), 3.0f, 3.0f), pBrush, 1.0f);

        Fraudize::D2DColor fg = !enabled ? Fraudize::Colors::TextDim
                              : (active ? activeFg : Fraudize::Colors::TextActive);
        pBrush->SetColor(fg.toD2D());
        pRenderTarget->DrawText(label, static_cast<UINT32>(wcslen(label)),
                                 fmtSmall, r.toD2D(), pBrush);
    }

    void onPaint() {
        createDeviceResources();
        if (!pRenderTarget || !pBrush) return;

        pRenderTarget->BeginDraw();
        pRenderTarget->Clear(Fraudize::Colors::BgChassis.toD2D());

        // Outer border
        pBrush->SetColor(Fraudize::Colors::BorderChassis.toD2D());
        pRenderTarget->DrawRectangle(D2D1::RectF(0.5f, 0.5f, UI_WIDTH - 0.5f, UI_HEIGHT - 0.5f), pBrush, 1.0f);

        drawTopCaptureBar();
        drawWaveformCard();
        drawEditToolbar();
        drawBottomBar();

        HRESULT hr = pRenderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardDeviceResources();
        }
    }

    void drawTopCaptureBar() {
        auto& cap = engine->getCapture();
        bool rec = cap.isRecording();

        drawButton(R_REC, rec ? L"STOP" : L"REC", rec, true,
                   Fraudize::Colors::RecordRed, Fraudize::Colors::TextBright);

        // Recording time readout
        double sec = rec ? cap.getRecordedSeconds() : 0.0;
        if (!rec) {
            auto snap = engine->getClip().getSnapshot();
            if (snap) sec = snap->trimmedDurationSeconds();
        }
        int mins = static_cast<int>(sec) / 60;
        double rem = sec - mins * 60.0;
        wchar_t timeBuf[32];
        swprintf(timeBuf, 32, L"%02d:%04.1f", mins, rem);

        pBrush->SetColor(Fraudize::Colors::BgCard.toD2D());
        pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(R_TIME.toD2D(), 3.0f, 3.0f), pBrush);
        pBrush->SetColor(Fraudize::Colors::BorderCard.toD2D());
        pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(R_TIME.toD2D(), 3.0f, 3.0f), pBrush, 1.0f);
        pBrush->SetColor(rec ? Fraudize::Colors::RecordRed.toD2D() : Fraudize::Colors::TextActive.toD2D());
        pRenderTarget->DrawText(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), fmtMono, R_TIME.toD2D(), pBrush);

        // Stereo peak meter
        float pL = std::clamp(cap.getPeakL(), 0.0f, 1.0f);
        float pR = std::clamp(cap.getPeakR(), 0.0f, 1.0f);
        pBrush->SetColor(Fraudize::Colors::BgHeader.toD2D());
        pRenderTarget->FillRectangle(R_METER.toD2D(), pBrush);
        auto drawBar = [&](float x, float w, float level) {
            float h = level * R_METER.h;
            if (h > 0.5f) {
                D2D1_RECT_F b = D2D1::RectF(x, R_METER.y + R_METER.h - h, x + w, R_METER.y + R_METER.h);
                pBrush->SetColor((level > 0.9f ? Fraudize::Colors::RecordRed :
                                 (level > 0.7f ? Fraudize::Colors::MeterAmber : Fraudize::Colors::MeterGreen)).toD2D());
                pRenderTarget->FillRectangle(b, pBrush);
            }
        };
        drawBar(R_METER.x + 2.0f, 7.0f, pL);
        drawBar(R_METER.x + 11.0f, 7.0f, pR);

        // Backend button
        const wchar_t* bLbl = L"WASAPI IN";
        if (engine->getCaptureBackend() == CaptureBackend::WASAPI_LOOPBACK) bLbl = L"LOOPBACK";
        else if (engine->getCaptureBackend() == CaptureBackend::MME_WAVEIN) bLbl = L"WAVEIN";
        drawButton(R_BACKEND, bLbl, false, !rec);

        // Device selector
        drawButton(R_DEV_PREV, L"<", false, !rec);
        pBrush->SetColor(Fraudize::Colors::BgCard.toD2D());
        pRenderTarget->FillRectangle(R_DEV_NAME.toD2D(), pBrush);
        pBrush->SetColor(Fraudize::Colors::BorderCard.toD2D());
        pRenderTarget->DrawRectangle(R_DEV_NAME.toD2D(), pBrush, 1.0f);
        const wchar_t* devName = (deviceIndex < devices.size()) ? devices[deviceIndex].name.c_str() : L"Default";
        pBrush->SetColor(Fraudize::Colors::TextActive.toD2D());
        D2D1_RECT_F devTxtRect = D2D1::RectF(R_DEV_NAME.x + 6.0f, R_DEV_NAME.y, R_DEV_NAME.x + R_DEV_NAME.w - 6.0f, R_DEV_NAME.y + R_DEV_NAME.h);
        pRenderTarget->DrawText(devName, static_cast<UINT32>(wcslen(devName)), fmtSmall, devTxtRect, pBrush);
        drawButton(R_DEV_NEXT, L">", false, !rec);

        // Channel mode
        const wchar_t* chLbl = L"ST";
        switch (engine->getChannelMode()) {
            case ChannelMode::STEREO:   chLbl = L"CH: ST"; break;
            case ChannelMode::MONO_MIX: chLbl = L"CH: MO"; break;
            case ChannelMode::MONO_L:   chLbl = L"CH: L";  break;
            case ChannelMode::MONO_R:   chLbl = L"CH: R";  break;
        }
        drawButton(R_CHAN, chLbl, false);

        // Duration cap
        const wchar_t* capLbl = L"CAP: 5M";
        switch (engine->getDurationCap()) {
            case DurationCap::CAP_1M:   capLbl = L"CAP: 1M";  break;
            case DurationCap::CAP_3M:   capLbl = L"CAP: 3M";  break;
            case DurationCap::CAP_5M:   capLbl = L"CAP: 5M";  break;
            case DurationCap::CAP_10M:  capLbl = L"CAP: 10M"; break;
            case DurationCap::CAP_NONE: capLbl = L"CAP: INF"; break;
        }
        drawButton(R_CAP, capLbl, false);
    }

    void drawWaveformCard() {
        pBrush->SetColor(Fraudize::Colors::BgCard.toD2D());
        pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(R_WAVE_CARD.toD2D(), 4.0f, 4.0f), pBrush);
        pBrush->SetColor(Fraudize::Colors::BorderCard.toD2D());
        pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(R_WAVE_CARD.toD2D(), 4.0f, 4.0f), pBrush, 1.0f);

        // Center zero line
        float midY = R_WAVE_AREA.y + R_WAVE_AREA.h * 0.5f;
        pBrush->SetColor(Fraudize::Colors::BorderChassis.toD2D());
        pRenderTarget->DrawLine(D2D1::Point2F(R_WAVE_AREA.x, midY),
                                D2D1::Point2F(R_WAVE_AREA.x + R_WAVE_AREA.w, midY), pBrush, 1.0f);

        auto& cap = engine->getCapture();
        if (cap.isRecording()) {
            std::vector<LiveWaveBucket> live;
            cap.copyLivePreview(live);
            if (!live.empty()) {
                pBrush->SetColor(Fraudize::Colors::RecordRed.toD2D());
                float halfH = R_WAVE_AREA.h * 0.45f;
                float dx = R_WAVE_AREA.w / static_cast<float>(live.size());
                for (size_t i = 0; i < live.size(); ++i) {
                    float x = R_WAVE_AREA.x + i * dx;
                    float y1 = midY - live[i].maxVal * halfH;
                    float y2 = midY - live[i].minVal * halfH;
                    if (y2 - y1 < 1.0f) y2 = y1 + 1.0f;
                    pRenderTarget->DrawLine(D2D1::Point2F(x, y1), D2D1::Point2F(x, y2), pBrush, std::max(1.0f, dx * 0.85f));
                }
            }
            return;
        }

        auto snap = engine->getClip().getSnapshot();
        size_t n = snap ? snap->numFrames() : 0;

        if (n == 0) {
            std::wstring err = cap.getLastError();
            const wchar_t* msg = err.empty() ? L"NO AUDIO  -  CLICK REC OR DROP WAV" : err.c_str();
            pBrush->SetColor(err.empty() ? Fraudize::Colors::TextMuted.toD2D() : Fraudize::Colors::RecordRed.toD2D());
            pRenderTarget->DrawText(msg, static_cast<UINT32>(wcslen(msg)), fmtSmall, R_WAVE_AREA.toD2D(), pBrush);
        } else {
            clampView(n);
            pRenderTarget->PushAxisAlignedClip(R_WAVE_AREA.toD2D(), D2D1_ANTIALIAS_MODE_ALIASED);

            // Draw cached waveform envelope
            int cols = static_cast<int>(R_WAVE_AREA.w);
            if (static_cast<int>(waveCache.size()) != cols ||
                snap->version != cacheClipVer ||
                viewStart != cacheViewStart ||
                viewLen != cacheViewLen) {
                waveCache.resize(cols);
                const auto& bufL = snap->L();
                const auto& bufR = snap->R();
                const auto& peaks = snap->audio->peaks;
                for (int px = 0; px < cols; ++px) {
                    double s0 = viewStart + (static_cast<double>(px) / cols) * viewLen;
                    double s1 = viewStart + (static_cast<double>(px + 1) / cols) * viewLen;
                    size_t i0 = std::min(n - 1, static_cast<size_t>(std::max(0.0, s0)));
                    size_t i1 = std::min(n, std::max(i0 + 1, static_cast<size_t>(std::ceil(s1))));

                    float mn = 1.0f, mx = -1.0f;
                    if ((i1 - i0) >= AudioBuffer::PEAK_BLOCK * 2 && !peaks.empty()) {
                        size_t b0 = i0 / AudioBuffer::PEAK_BLOCK;
                        size_t b1 = std::min(peaks.size(), (i1 + AudioBuffer::PEAK_BLOCK - 1) / AudioBuffer::PEAK_BLOCK);
                        for (size_t b = b0; b < b1; ++b) {
                            mn = std::min(mn, peaks[b].mn);
                            mx = std::max(mx, peaks[b].mx);
                        }
                    } else {
                        for (size_t i = i0; i < i1; ++i) {
                            float v = 0.5f * (bufL[i] + bufR[i]);
                            mn = std::min(mn, v);
                            mx = std::max(mx, v);
                        }
                    }
                    waveCache[px] = { mn, mx };
                }
                cacheClipVer = snap->version;
                cacheViewStart = viewStart;
                cacheViewLen = viewLen;
            }

            float halfH = R_WAVE_AREA.h * 0.46f;
            pBrush->SetColor(Fraudize::Colors::WaveFill.toD2D());
            for (int px = 0; px < cols; ++px) {
                float x = R_WAVE_AREA.x + static_cast<float>(px);
                float yTop = midY - waveCache[px].mx * halfH;
                float yBot = midY - waveCache[px].mn * halfH;
                if (yBot - yTop < 1.0f) {
                    yTop = midY - 0.5f;
                    yBot = midY + 0.5f;
                }
                pRenderTarget->DrawLine(D2D1::Point2F(x, yTop), D2D1::Point2F(x, yBot), pBrush, 1.0f);
            }

            // Shaded out-of-trim regions
            float xTrimS = sampleToX(static_cast<double>(snap->trimStart));
            float xTrimE = sampleToX(static_cast<double>(snap->trimEnd));
            pBrush->SetColor(Fraudize::Colors::TrimShade.toD2D());
            if (xTrimS > R_WAVE_AREA.x) {
                pRenderTarget->FillRectangle(
                    D2D1::RectF(R_WAVE_AREA.x, R_WAVE_AREA.y, std::min(xTrimS, R_WAVE_AREA.x + R_WAVE_AREA.w), R_WAVE_AREA.y + R_WAVE_AREA.h),
                    pBrush
                );
            }
            if (xTrimE < R_WAVE_AREA.x + R_WAVE_AREA.w) {
                pRenderTarget->FillRectangle(
                    D2D1::RectF(std::max(xTrimE, R_WAVE_AREA.x), R_WAVE_AREA.y, R_WAVE_AREA.x + R_WAVE_AREA.w, R_WAVE_AREA.y + R_WAVE_AREA.h),
                    pBrush
                );
            }

            // Selection overlay
            if (selEnd > selStart) {
                float xs = std::clamp(sampleToX(static_cast<double>(selStart)), R_WAVE_AREA.x, R_WAVE_AREA.x + R_WAVE_AREA.w);
                float xe = std::clamp(sampleToX(static_cast<double>(selEnd)), R_WAVE_AREA.x, R_WAVE_AREA.x + R_WAVE_AREA.w);
                if (xe > xs) {
                    pBrush->SetColor(Fraudize::Colors::SelectionTint.toD2D());
                    pRenderTarget->FillRectangle(D2D1::RectF(xs, R_WAVE_AREA.y, xe, R_WAVE_AREA.y + R_WAVE_AREA.h), pBrush);
                    pBrush->SetColor(Fraudize::Colors::SelectionBorder.toD2D());
                    pRenderTarget->DrawLine(D2D1::Point2F(xs, R_WAVE_AREA.y), D2D1::Point2F(xs, R_WAVE_AREA.y + R_WAVE_AREA.h), pBrush, 1.0f);
                    pRenderTarget->DrawLine(D2D1::Point2F(xe, R_WAVE_AREA.y), D2D1::Point2F(xe, R_WAVE_AREA.y + R_WAVE_AREA.h), pBrush, 1.0f);
                }
            }

            // Trim handles
            pBrush->SetColor(Fraudize::Colors::TextBright.toD2D());
            if (xTrimS >= R_WAVE_AREA.x - 1.0f && xTrimS <= R_WAVE_AREA.x + R_WAVE_AREA.w + 1.0f) {
                pRenderTarget->DrawLine(D2D1::Point2F(xTrimS, R_WAVE_AREA.y), D2D1::Point2F(xTrimS, R_WAVE_AREA.y + R_WAVE_AREA.h), pBrush, 1.5f);
                pRenderTarget->FillRectangle(D2D1::RectF(xTrimS, R_WAVE_AREA.y, xTrimS + 7.0f, R_WAVE_AREA.y + 10.0f), pBrush);
            }
            if (xTrimE >= R_WAVE_AREA.x - 1.0f && xTrimE <= R_WAVE_AREA.x + R_WAVE_AREA.w + 1.0f) {
                pRenderTarget->DrawLine(D2D1::Point2F(xTrimE, R_WAVE_AREA.y), D2D1::Point2F(xTrimE, R_WAVE_AREA.y + R_WAVE_AREA.h), pBrush, 1.5f);
                pRenderTarget->FillRectangle(D2D1::RectF(xTrimE - 7.0f, R_WAVE_AREA.y, xTrimE, R_WAVE_AREA.y + 10.0f), pBrush);
            }

            // Playhead cursor
            double phNorm = engine->getPlayheadNorm();
            float xPlay = sampleToX(phNorm * static_cast<double>(n));
            if (xPlay >= R_WAVE_AREA.x && xPlay <= R_WAVE_AREA.x + R_WAVE_AREA.w) {
                pBrush->SetColor(Fraudize::Colors::Playhead.toD2D());
                pRenderTarget->DrawLine(D2D1::Point2F(xPlay, R_WAVE_AREA.y), D2D1::Point2F(xPlay, R_WAVE_AREA.y + R_WAVE_AREA.h), pBrush, 1.5f);
            }

            pRenderTarget->PopAxisAlignedClip();

            // Top-right compact status readout
            wchar_t info[96];
            if (selEnd > selStart) {
                double selSec = static_cast<double>(selEnd - selStart) / snap->sampleRate;
                swprintf(info, 96, L"%.2fs  |  SEL %.2fs  |  %u Hz",
                         snap->trimmedDurationSeconds(), selSec, snap->sampleRate);
            } else {
                swprintf(info, 96, L"%.2fs  |  %u Hz",
                         snap->trimmedDurationSeconds(), snap->sampleRate);
            }
            D2D1_RECT_F infoRect = D2D1::RectF(R_WAVE_AREA.x + 200.0f, R_WAVE_AREA.y + 2.0f,
                                               R_WAVE_AREA.x + R_WAVE_AREA.w - 6.0f, R_WAVE_AREA.y + 18.0f);
            pBrush->SetColor(Fraudize::Colors::TextMuted.toD2D());
            pRenderTarget->DrawText(info, static_cast<UINT32>(wcslen(info)), fmtStatus, infoRect, pBrush);
        }

        // Zoom controls & scrollbar
        drawButton(R_ZOOM_OUT, L"-", false, n > 0);
        drawButton(R_ZOOM_IN, L"+", false, n > 0);
        drawButton(R_ZOOM_FIT, L"FIT", false, n > 0);

        pBrush->SetColor(Fraudize::Colors::BgHeader.toD2D());
        pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(R_SCROLLBAR.toD2D(), 3.0f, 3.0f), pBrush);
        if (n > 0) {
            double total = static_cast<double>(n);
            float thumbX = R_SCROLLBAR.x + static_cast<float>((viewStart / total) * R_SCROLLBAR.w);
            float thumbW = std::max(14.0f, static_cast<float>((viewLen / total) * R_SCROLLBAR.w));
            pBrush->SetColor(Fraudize::Colors::BorderChassis.toD2D());
            pRenderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX, R_SCROLLBAR.y + 1.0f, std::min(R_SCROLLBAR.x + R_SCROLLBAR.w, thumbX + thumbW), R_SCROLLBAR.y + R_SCROLLBAR.h - 1.0f), 2.0f, 2.0f),
                pBrush
            );
        }
    }

    void drawEditToolbar() {
        auto snap = engine->getClip().getSnapshot();
        bool hasAudio = snap && !snap->empty() && !engine->getCapture().isRecording();
        bool hasSel = hasAudio && (selEnd > selStart);
        bool canUndo = engine->getClip().canUndo();
        bool canRedo = engine->getClip().canRedo();

        const wchar_t* labels[NUM_EDIT_BTNS] = {
            L"TRIM", L"DEL", L"SIL", L"FADE IN", L"FADE OUT", L"NORM",
            L"REV", L"DC", L"UNDO", L"REDO", L"CLR", L"LOAD", L"SAVE"
        };
        bool enabled[NUM_EDIT_BTNS] = {
            hasAudio, hasSel, hasAudio, hasAudio, hasAudio, hasAudio,
            hasAudio, hasAudio, canUndo, canRedo, hasAudio, !engine->getCapture().isRecording(), hasAudio
        };

        for (int i = 0; i < NUM_EDIT_BTNS; ++i) {
            drawButton(getEditBtnRect(i), labels[i], false, enabled[i]);
        }
    }

    void drawBottomBar() {
        bool playing = engine->isPlaying();
        drawButton(R_PLAY, playing ? L"STOP" : L"PLAY", playing);
        drawButton(R_RST, L"RST", false);

        bool cont = (engine->getPlayMode() == PlayMode::CONTINUE);
        drawButton(R_MODE_RST, L"RESTART", !cont);
        drawButton(R_MODE_CONT, L"CONTINUE", cont);

        bool oneShot = (engine->getGateMode() == GateMode::ONE_SHOT);
        drawButton(R_GATE, L"GATE", !oneShot);
        drawButton(R_ONESHOT, L"1-SHOT", oneShot);

        drawButton(R_LOOP, L"LOOP", engine->getLoop());

        // Gain slider
        float db = engine->getGainDb();
        wchar_t gainTxt[48];
        swprintf(gainTxt, 48, L"GAIN  %+.1f dB", db);
        pBrush->SetColor(Fraudize::Colors::TextMuted.toD2D());
        pRenderTarget->DrawText(gainTxt, static_cast<UINT32>(wcslen(gainTxt)), fmtSmall, R_GAIN_LBL.toD2D(), pBrush);

        float trackY = R_GAIN_SLD.y + R_GAIN_SLD.h * 0.5f;
        pBrush->SetColor(Fraudize::Colors::KnobTrack.toD2D());
        pRenderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(R_GAIN_SLD.x, trackY - 2.5f, R_GAIN_SLD.x + R_GAIN_SLD.w, trackY + 2.5f), 2.5f, 2.5f),
            pBrush
        );

        float norm = engine->getParameter(PARAM_GAIN);
        float zeroX = R_GAIN_SLD.x + R_GAIN_SLD.w * 0.5f;
        float knobX = R_GAIN_SLD.x + norm * R_GAIN_SLD.w;
        pBrush->SetColor(Fraudize::Colors::KnobArc.toD2D());
        pRenderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(std::min(zeroX, knobX), trackY - 2.5f, std::max(zeroX, knobX), trackY + 2.5f), 2.5f, 2.5f),
            pBrush
        );
        pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, trackY), 6.0f, 6.0f), pBrush);

        // Functional MIDI hint
        const wchar_t* hint = L"MIDI:  A4 = PLAY   |   A#4 = RESET CURSOR";
        pBrush->SetColor(Fraudize::Colors::TextMuted.toD2D());
        pRenderTarget->DrawText(hint, static_cast<UINT32>(wcslen(hint)), fmtHint, R_MIDI_HINT.toD2D(), pBrush);
    }

    CapturePluginEngine* engine = nullptr;
    ParamChangedCallback onParamCb = nullptr;
    void* onParamUser = nullptr;

    HWND parentHWnd = nullptr;
    HWND hWnd = nullptr;

    ID2D1Factory* pD2DFactory = nullptr;
    IDWriteFactory* pDWriteFactory = nullptr;
    ID2D1HwndRenderTarget* pRenderTarget = nullptr;
    ID2D1SolidColorBrush* pBrush = nullptr;
    IDWriteTextFormat* fmtBold = nullptr;
    IDWriteTextFormat* fmtSmall = nullptr;
    IDWriteTextFormat* fmtMono = nullptr;
    IDWriteTextFormat* fmtStatus = nullptr;
    IDWriteTextFormat* fmtHint = nullptr;

    std::vector<CaptureDeviceInfo> devices;
    size_t deviceIndex = 0;

    double viewStart = 0.0;
    double viewLen = 1.0;
    uint64_t lastClipVersion = 0;

    struct PeakCol { float mn, mx; };
    std::vector<PeakCol> waveCache;
    uint64_t cacheClipVer = 0;
    double cacheViewStart = -1.0;
    double cacheViewLen = -1.0;

    bool wasRecording = false;
    bool wasPlaying = false;

    size_t selStart = 0;
    size_t selEnd = 0;
    size_t dragAnchorSample = 0;

    DragMode dragMode = DragMode::NONE;
    float dragStartX = 0.0f;
    double dragStartView = 0.0;
    float hoverX = -1.0f;
    float hoverY = -1.0f;
};

} // namespace LMAC
#endif
