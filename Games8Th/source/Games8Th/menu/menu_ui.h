#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../../../external/imgui/imgui.h"
#include "../config/config.h"

extern ImFont* g_MenuIconFont;

namespace MenuUI {

struct Layout {
    float windowW    = 520.f;
    float windowH    = 500.f;
    float sidebar    = 48.f;
    float headerH    = 26.f;
    float shellPad   = 5.f;
    float contentPad = 8.f;
    float gap        = 5.f;
    float cardPad    = 7.f;
    float navBtnH    = 34.f;
    float subNavH    = 22.f;
    float rounding   = 4.f;
    float childRound = 3.f;
    bool  compact    = true;
    float dpi        = 1.f;

    static float DpiMul() {
        int p = Config::menu_dpi_scale;
        if (p < 100) p = 100;
        if (p > 200) p = 200;
        p = ((p + 12) / 25) * 25;
        if (p < 100) p = 100;
        if (p > 200) p = 200;
        return static_cast<float>(p) * 0.01f;
    }

    static Layout Current() {
        Layout L;
        L.compact = Config::menu_compact;
        L.dpi     = DpiMul();
        L.rounding   = (std::clamp)(Config::menu_rounding, 0.f, 14.f);
        L.childRound = (std::max)(0.f, L.rounding - 1.f);
        if (L.compact) {
            L.windowW    = 520.f;
            L.windowH    = 500.f;
            L.gap        = 5.f;
            L.cardPad    = 7.f;
            L.contentPad = 8.f;
            L.shellPad   = 5.f;
            L.navBtnH    = 34.f;
            L.subNavH    = 22.f;
            L.sidebar    = 48.f;
            L.headerH    = 26.f;
        } else {
            L.windowW    = 520.f;
            L.windowH    = 500.f;
            L.gap        = 8.f;
            L.cardPad    = 10.f;
            L.contentPad = 12.f;
            L.shellPad   = 8.f;
            L.navBtnH    = 38.f;
            L.subNavH    = 26.f;
            L.sidebar    = 48.f;
            L.headerH    = 26.f;
        }
        if (Config::menu_sidebar_labels) {
            L.sidebar = L.compact ? 92.f : 92.f;
            L.navBtnH = L.compact ? 40.f : 40.f;
        }
        if (L.dpi > 1.001f) {
            const float s = L.dpi;
            L.windowW    = floorf(L.windowW * s);
            L.windowH    = floorf(L.windowH * s);
            L.gap        = floorf(L.gap * s);
            L.cardPad    = floorf(L.cardPad * s);
            L.contentPad = floorf(L.contentPad * s);
            L.shellPad   = floorf(L.shellPad * s);
            L.navBtnH    = floorf(L.navBtnH * s);
            L.subNavH    = floorf(L.subNavH * s);
            L.sidebar    = floorf(L.sidebar * s);
            L.headerH    = floorf(L.headerH * s);
        }
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (ds.x > 64.f && ds.y > 64.f) {
            L.windowW = (std::min)(L.windowW, floorf(ds.x - 24.f));
            L.windowH = (std::min)(L.windowH, floorf(ds.y - 24.f));
        }
        if (L.windowW < 360.f) L.windowW = 360.f;
        if (L.windowH < 300.f) L.windowH = 300.f;
        return L;
    }

    float colLeft(float avail) const {
        return floorf((std::max)(0.f, avail - gap) * 0.5f);
    }
};

void AnimTick(bool openTarget);
bool AnimVisible();
float OpenAlpha();
void NotifyTab(int tab);

inline ImVec4 Accent()     { return Config::menu_accent; }
inline ImVec4 TextMuted()  { return Config::menu_text_muted; }
inline ImVec4 TextBright() { return Config::menu_text; }
inline ImVec4 WithA(ImVec4 c, float a) { c.w = a; return c; }
inline ImU32  ToU32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32  AccentU32(float a = 1.f) {
    ImVec4 c = Config::menu_accent;
    c.w *= a;
    return ToU32(c);
}
inline ImU32  BorderU32(float a = 1.f) {
    ImVec4 c = Config::menu_border;
    c.w *= a;
    return ToU32(c);
}

void ApplyTheme();
void ApplyPreset(int idx);

void DrawWindowFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding);

void Section(const char* label);
void Gap(float mult = 1.f);
void SoftSeparator();

void BeginCard(const char* id, float width, bool autoY = false);
void EndCard();
void BeginStrip(const char* id);
void EndStrip();

bool Slider(const char* label, const char* id, float* v, float vmin, float vmax, const char* fmt = "%.3f");
bool SliderInt(const char* label, const char* id, int* v, int vmin, int vmax, const char* fmt = "%d");
bool Combo(const char* label, const char* id, int* cur, const char* const items[], int count);
void Tip(const char* text);
bool FeatureToggle(const char* label, bool* v, const char* tip = nullptr);
void PopupTitle(const char* title);

bool NavIcon(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index = 0);
bool NavButton(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index = 0);
bool NavButton(const char* label, bool selected, const ImVec2& size, int index = 0);
void SubNav(const char* const* labels, int count, int* selected);

void BeginSidebar(const Layout& L, float height);
void EndSidebar();

void BeginContent(const Layout& L, float contentW, float contentH);
void EndContent();

void BeginHeader(const Layout& L, float windowW);
void HeaderBrand(const char* name, const char* accentSuffix, const char* badge);
void HeaderRightHint(const char* text);

} // namespace MenuUI

