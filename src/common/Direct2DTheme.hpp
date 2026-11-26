#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace Fraudize {

struct D2DColor {
    float r, g, b, a;
    constexpr D2DColor(float red = 0.0f, float green = 0.0f, float blue = 0.0f, float alpha = 1.0f)
        : r(red), g(green), b(blue), a(alpha) {}
    D2D1_COLOR_F toD2D() const { return D2D1::ColorF(r, g, b, a); }
};

// Dark-slate Fraudize Palette + Capture/Waveform accents
namespace Colors {
    static constexpr D2DColor BgChassis(0.075f, 0.080f, 0.090f);         // #131417
    static constexpr D2DColor BgHeader(0.055f, 0.058f, 0.065f);          // #0E0F11
    static constexpr D2DColor BgCard(0.090f, 0.095f, 0.105f);            // #17181D
    static constexpr D2DColor BorderChassis(0.18f, 0.19f, 0.22f);        // #2E3138
    static constexpr D2DColor BorderCard(0.16f, 0.17f, 0.19f);           // #292B30

    static constexpr D2DColor TextActive(0.90f, 0.92f, 0.96f);           // #E6EBF5
    static constexpr D2DColor TextBright(0.96f, 0.97f, 0.98f);
    static constexpr D2DColor TextMuted(0.50f, 0.53f, 0.58f);            // #808794
    static constexpr D2DColor TextDim(0.32f, 0.34f, 0.38f);

    static constexpr D2DColor KnobTrack(0.18f, 0.19f, 0.22f);
    static constexpr D2DColor KnobArc(0.90f, 0.92f, 0.96f);
    static constexpr D2DColor KnobInner(0.11f, 0.12f, 0.14f);

    static constexpr D2DColor ButtonActiveBg(0.92f, 0.94f, 0.97f);
    static constexpr D2DColor ButtonActiveText(0.055f, 0.058f, 0.065f);
    static constexpr D2DColor ButtonInactiveBg(0.12f, 0.13f, 0.15f);
    static constexpr D2DColor ButtonInactiveBorder(0.24f, 0.26f, 0.30f);

    static constexpr D2DColor RecordRed(0.85f, 0.24f, 0.24f);            // Muted crimson REC
    static constexpr D2DColor RecordRedDim(0.45f, 0.16f, 0.16f);
    static constexpr D2DColor SelectionTint(0.38f, 0.55f, 0.80f, 0.28f); // Translucent slate-blue
    static constexpr D2DColor SelectionBorder(0.55f, 0.72f, 0.95f, 0.65f);
    static constexpr D2DColor TrimShade(0.03f, 0.035f, 0.045f, 0.62f);
    static constexpr D2DColor WaveFill(0.68f, 0.73f, 0.82f, 0.85f);
    static constexpr D2DColor WaveLine(0.88f, 0.91f, 0.96f, 0.95f);
    static constexpr D2DColor Playhead(0.96f, 0.82f, 0.35f, 0.95f);      // Crisp amber cursor
    static constexpr D2DColor MeterGreen(0.35f, 0.75f, 0.48f);
    static constexpr D2DColor MeterAmber(0.86f, 0.68f, 0.26f);
}

class UITheme {
public:
    static bool createTextFormat(IDWriteFactory* factory, const wchar_t* font, float size,
                                 DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                                 bool ellipsis, IDWriteTextFormat** ppFmt) {
        if (!factory || !ppFmt) return false;
        HRESULT hr = factory->CreateTextFormat(
            font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"en-us", ppFmt
        );
        if (FAILED(hr) || !*ppFmt) return false;
        (*ppFmt)->SetTextAlignment(align);
        (*ppFmt)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (ellipsis) {
            (*ppFmt)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            IDWriteInlineObject* sign = nullptr;
            if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(*ppFmt, &sign)) && sign) {
                (*ppFmt)->SetTrimming(&trimming, sign);
                sign->Release();
            }
        }
        return true;
    }
};

} // namespace Fraudize
#endif
