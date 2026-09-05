#include "menu_ui.h"
#include "../config/language.h"

#include <cstring>
#include <vector>

namespace MenuUI {
namespace {

struct StyleStack {
    int colors = 0;
    int vars   = 0;
    void pushColor(ImGuiCol idx, ImVec4 c) { ImGui::PushStyleColor(idx, c); ++colors; }
    void pushVar(ImGuiStyleVar idx, float v) { ImGui::PushStyleVar(idx, v); ++vars; }
    void pushVar(ImGuiStyleVar idx, ImVec2 v) { ImGui::PushStyleVar(idx, v); ++vars; }
    void pop() {
        if (vars)   { ImGui::PopStyleVar(vars); vars = 0; }
        if (colors) { ImGui::PopStyleColor(colors); colors = 0; }
    }
};

StyleStack g_cardStack;
bool       g_cardOpen = false;
bool       g_cardHasItemWidth = false;
StyleStack g_stripStack;
bool       g_stripOpen = false;
StyleStack g_sideStack;
bool       g_sideOpen = false;
StyleStack g_contentStack;
bool       g_contentOpen = false;

ImDrawList* g_contentDl = nullptr;
struct PairTrack {
    int    frame = -1;
    bool   has   = false;
    ImVec2 min, max;
};
PairTrack g_pair;

struct Anim {
    float open = 0.f;
    int   tab  = 0;
};
Anim g_anim;

inline float Saturate(float x) { return (std::clamp)(x, 0.f, 1.f); }

static ImVec4 Darker(ImVec4 c, float amt) {
    return ImVec4(
        (std::max)(0.f, c.x - amt),
        (std::max)(0.f, c.y - amt),
        (std::max)(0.f, c.z - amt),
        c.w);
}
static ImVec4 Lighter(ImVec4 c, float amt) {
    return ImVec4(
        (std::min)(1.f, c.x + amt),
        (std::min)(1.f, c.y + amt),
        (std::min)(1.f, c.z + amt),
        c.w);
}
static ImVec4 Mix(ImVec4 a, ImVec4 b, float t) {
    t = (std::clamp)(t, 0.f, 1.f);
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

struct ShellPalette {
    ImVec4 bg, side, card, frame, border, text, muted, track;
};

ShellPalette Palette() {
    ShellPalette p;
    p.bg     = Config::menu_bg;
    p.side   = Config::menu_sidebar_bg;
    p.card   = Config::menu_child_bg;
    p.border = Config::menu_border;
    // Shell chrome stays neutral even when an older config stored a tinted rim.
    const float borderAvg = (p.border.x + p.border.y + p.border.z) / 3.f;
    if (std::fabs(p.border.x - borderAvg) + std::fabs(p.border.y - borderAvg) +
        std::fabs(p.border.z - borderAvg) > 0.12f)
        p.border = Mix(p.border, ImVec4(1.f, 1.f, 1.f, p.border.w), 0.55f);
    p.text   = Config::menu_text;
    p.muted  = Config::menu_text_muted;
    // Keep the shell opaque like the old workbench. Transparency belongs to
    // the outer window only; cards and input wells must remain readable.
    p.frame  = Darker(p.card, 0.035f);
    p.track  = Darker(p.bg, 0.030f);
    p.side.w = 1.f;
    p.card.w = 1.f;
    p.frame.w = 1.f;
    p.track.w = 1.f;
    if (p.border.w < 0.06f) p.border.w = 0.08f;
    if (p.border.w > 0.16f) p.border.w = 0.14f;
    p.text.w  = 1.f;
    p.muted.w = 1.f;

    p.bg.w = (std::clamp)(Config::menu_opacity, 0.70f, 1.f);
    return p;
}

void PaintGloss(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float strength = 1.f) {
    if (!dl) return;
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    if (w < 6.f || h < 6.f) return;
    // Old UI used a quiet chrome edge instead of a glass reflection.
    strength = Saturate(strength);
    dl->AddRect(min, max, BorderU32(0.72f + 0.08f * strength), rounding, 0, 1.f);
    dl->AddLine(ImVec2(min.x + rounding + 4.f, min.y + 1.f),
        ImVec2(max.x - rounding - 4.f, min.y + 1.f),
        AccentU32(0.16f * strength), 1.f);
}

bool DrawValueSlider(const char* label, const char* id, float* v, float vmin, float vmax,
                     const char* fmt, bool asInt) {
    if (!v || !(vmax > vmin))
        return false;

    const float avail = ImGui::GetContentRegionAvail().x;
    const float trackH = 14.f;
    const float barH = 3.f;
    const float grabR = 5.f;

    char val[48];
    if (asInt)
        std::snprintf(val, sizeof(val), fmt ? fmt : "%d", (int)*v);
    else
        std::snprintf(val, sizeof(val), fmt ? fmt : "%.3f", *v);

    const ImVec2 valSz = ImGui::CalcTextSize(val);

    ImGui::PushID(id ? id : label);

    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, WithA(TextMuted(), 0.95f));
    ImGui::TextUnformatted(Lang::T(label ? label : ""));
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorPosX(x0 + avail - valSz.x);
    ImGui::PushStyleColor(ImGuiCol_Text, TextBright());
    ImGui::TextUnformatted(val);
    ImGui::PopStyleColor();

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##track", ImVec2((std::max)(8.f, avail), trackH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    bool changed = false;
    if (held) {
        const float t = Saturate((ImGui::GetIO().MousePos.x - p0.x) / (std::max)(1.f, avail));
        float nv = vmin + t * (vmax - vmin);
        if (asInt)
            nv = (float)(int)(nv + (nv >= 0.f ? 0.5f : -0.5f));
        nv = (std::clamp)(nv, vmin, vmax);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }

    const float frac = Saturate((*v - vmin) / (vmax - vmin));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p0.y + trackH * 0.5f;
    const float y0 = cy - barH * 0.5f;
    const float y1 = cy + barH * 0.5f;
    const float rr = barH * 0.5f;
    const ImU32 trackCol = ToU32(WithA(TextMuted(), hovered || held ? 0.34f : 0.22f));
    dl->AddRectFilled(ImVec2(p0.x, y0), ImVec2(p0.x + avail, y1), trackCol, rr);
    const float fillW = avail * frac;
    if (fillW > 0.5f) {
        const ImVec2 fa(p0.x, y0);
        const ImVec2 fb(p0.x + fillW, y1);
        dl->AddRectFilled(fa, fb, AccentU32(held ? 1.f : (hovered ? 0.92f : 0.80f)), rr);
        dl->AddRectFilledMultiColor(fa, ImVec2(fb.x, y0 + barH * 0.45f),
            IM_COL32(255, 255, 255, 40), IM_COL32(255, 255, 255, 40),
            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    }

    const float gx = (std::clamp)(p0.x + fillW, p0.x + grabR, p0.x + avail - grabR);
    dl->AddCircleFilled(ImVec2(gx, cy), grabR + 1.f,
        AccentU32(held || hovered ? 0.75f : 0.42f));
    dl->AddCircleFilled(ImVec2(gx, cy), grabR - 1.f, ToU32(WithA(TextBright(), 0.98f)));

    ImGui::PopID();
    return changed;
}

} // namespace

void AnimTick(bool openTarget) {
    if (!openTarget) {
        g_anim.open = 0.f;
        return;
    }
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f || dt > 0.05f)
        dt = 1.f / 60.f;
    // Frame-rate independent exponential ease-out: no speed jump on
    // stutter frames, and the tail never snaps (old linear ramp did).
    //
    // DeltaTime floor: when DeltaTime is tiny (very high FPS or a glitchy
    // time source) k -> 0 and the menu would stay semi-transparent forever:
    // chrome (drawn via AddRectFilled) stays visible while every ImGui
    // control fades to ~invisible, which looked like "sidebar flickers,
    // right-side UI empty". Flooring dt keeps the open animation bounded.
    const float k = 1.0f - std::expf(-(std::max)(dt, 1.f / 240.f) / 0.06f);
    g_anim.open = Saturate(g_anim.open + k * (1.0f - g_anim.open));
    // Safety net: if the animation ever stalls low (e.g. repeated toggles),
    // snap it to full opacity after 2 seconds instead of staying invisible.
    static int s_openStallFrames = 0;
    if (g_anim.open < 0.35f) {
        if (++s_openStallFrames > 150)
            g_anim.open = 1.f;
    } else {
        s_openStallFrames = 0;
    }
}

bool AnimVisible() { return g_anim.open > 0.001f; }
float OpenAlpha()  { return g_anim.open; }

void NotifyTab(int tab) {
    g_anim.tab = tab;
}

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    const ShellPalette p = Palette();
    const ImVec4 accent = Config::menu_accent;
    const float r = (std::clamp)((std::max)(Config::menu_rounding, 2.f), 2.f, 6.f);

    auto tint = [&](float a) { return ImVec4(accent.x, accent.y, accent.z, a); };

    c[ImGuiCol_WindowBg]             = p.bg;
    c[ImGuiCol_ChildBg]              = p.card;
    c[ImGuiCol_PopupBg]              = p.card;
    c[ImGuiCol_Border]               = p.border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text]                 = p.text;
    c[ImGuiCol_TextDisabled]         = p.muted;

    c[ImGuiCol_FrameBg]              = p.frame;
    c[ImGuiCol_FrameBgHovered]       = Lighter(p.frame, 0.018f);
    c[ImGuiCol_FrameBgActive]        = Lighter(p.frame, 0.035f);

    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = p.bg;
    c[ImGuiCol_MenuBarBg]            = p.side;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.f, 0.f, 0.f, 0.12f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(1.f, 1.f, 1.f, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.f, 1.f, 1.f, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;

    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_CheckboxSelectedBg]   = tint(0.55f);
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = Lighter(accent, 0.12f);

    c[ImGuiCol_Button]               = p.frame;
    c[ImGuiCol_ButtonHovered]        = Lighter(p.frame, 0.018f);
    c[ImGuiCol_ButtonActive]         = Mix(p.frame, tint(1.f), 0.10f);

    c[ImGuiCol_Header]               = WithA(TextBright(), 0.045f);
    c[ImGuiCol_HeaderHovered]        = WithA(TextBright(), 0.07f);
    c[ImGuiCol_HeaderActive]         = tint(0.12f);

    c[ImGuiCol_Separator]            = WithA(Config::menu_border, 0.55f);
    c[ImGuiCol_SeparatorHovered]     = WithA(TextBright(), 0.12f);
    c[ImGuiCol_SeparatorActive]      = accent;

    c[ImGuiCol_ResizeGrip]           = ImVec4(1.f, 1.f, 1.f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]    = tint(0.22f);
    c[ImGuiCol_ResizeGripActive]     = tint(0.36f);

    c[ImGuiCol_Tab]                  = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]           = tint(0.14f);
    c[ImGuiCol_TabSelected]          = tint(0.12f);
    c[ImGuiCol_TabSelectedOverline]  = accent;
    c[ImGuiCol_TabDimmed]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected]    = tint(0.08f);

    c[ImGuiCol_TextSelectedBg]       = tint(0.28f);
    c[ImGuiCol_NavCursor]            = accent;
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.02f, 0.02f, 0.03f, 0.50f);

    c[ImGuiCol_TableHeaderBg]        = ImVec4(1.f, 1.f, 1.f, 0.04f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(1.f, 1.f, 1.f, 0.08f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(1.f, 1.f, 1.f, 0.04f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.f, 1.f, 1.f, 0.015f);

    style.WindowRounding    = r;
    style.ChildRounding     = (std::max)(2.f, r - 1.f);
    style.FrameRounding     = 3.f;
    style.PopupRounding     = (std::max)(3.f, r - 1.f);
    style.ScrollbarRounding = 4.f;
    style.GrabRounding      = 4.f;
    style.TabRounding       = 4.f;

    const bool compact = Config::menu_compact;
    const float dpi = Layout::DpiMul();
    style.WindowPadding     = ImVec2(0, 0);
    style.FramePadding      = compact ? ImVec2(8, 4) : ImVec2(10, 5);
    style.ItemSpacing       = compact ? ImVec2(10, 6) : ImVec2(12, 7);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.CellPadding       = ImVec2(5, 3);
    style.IndentSpacing     = 12.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;

    style.ScrollbarSize     = 6.0f;
    style.GrabMinSize       = 10.0f;
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);

    if (dpi > 1.001f) {
        style.FramePadding.x     = floorf(style.FramePadding.x * dpi);
        style.FramePadding.y     = floorf(style.FramePadding.y * dpi);
        style.ItemSpacing.x      = floorf(style.ItemSpacing.x * dpi);
        style.ItemSpacing.y      = floorf(style.ItemSpacing.y * dpi);
        style.ItemInnerSpacing.x = floorf(style.ItemInnerSpacing.x * dpi);
        style.ItemInnerSpacing.y = floorf(style.ItemInnerSpacing.y * dpi);
        style.CellPadding.x      = floorf(style.CellPadding.x * dpi);
        style.CellPadding.y      = floorf(style.CellPadding.y * dpi);
        style.IndentSpacing      = floorf(style.IndentSpacing * dpi);
        style.ScrollbarSize      = floorf(style.ScrollbarSize * dpi);
        style.GrabMinSize        = floorf(style.GrabMinSize * dpi);
    }
    ImGui::GetIO().FontGlobalScale = 1.f;

    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;
    style.Alpha             = 1.0f;
    style.DisabledAlpha     = 0.40f;
}

void ApplyPreset(int idx) {
    Config::menu_preset         = idx;
    Config::menu_compact        = true;
    Config::menu_widgets_follow = true;

    switch (idx) {
    case 0: // Midnight OLED (Pure deep black + electric cyan)
        Config::menu_bg         = ImVec4(0.060f, 0.066f, 0.082f, 0.97f);
        Config::menu_child_bg   = ImVec4(0.095f, 0.102f, 0.122f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.038f, 0.042f, 0.055f, 1.00f);
        Config::menu_border     = ImVec4(0.85f, 0.90f, 1.00f, 0.10f);
        Config::menu_text       = ImVec4(0.93f, 0.94f, 0.96f, 1.f);
        Config::menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.f);
        Config::menu_accent     = ImVec4(0.45f, 0.72f, 0.98f, 1.f);
        Config::menu_rounding   = 5.0f;
        Config::menu_opacity    = 0.98f;
        Config::menu_glass      = 0.12f;
        break;
    case 1: // Steel Slate (Modern dark slate + ice sky blue)
    default:
        Config::menu_bg         = ImVec4(0.068f, 0.070f, 0.078f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.102f, 0.105f, 0.118f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
        Config::menu_border     = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
        Config::menu_text       = ImVec4(0.93f, 0.94f, 0.96f, 1.f);
        Config::menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.f);
        Config::menu_accent     = ImVec4(0.42f, 0.68f, 0.92f, 1.f);
        Config::menu_rounding   = 4.0f;
        Config::menu_opacity    = 0.98f;
        Config::menu_glass      = 0.12f;
        break;
    case 2: // Nordic Frost (Arctic charcoal + frost ice blue)
        Config::menu_bg         = ImVec4(0.110f, 0.125f, 0.160f, 0.90f);
        Config::menu_child_bg   = ImVec4(0.160f, 0.185f, 0.230f, 0.65f);
        Config::menu_sidebar_bg = ImVec4(0.080f, 0.095f, 0.125f, 0.85f);
        Config::menu_border     = ImVec4(0.55f, 0.70f, 0.85f, 0.18f);
        Config::menu_text       = ImVec4(0.92f, 0.95f, 0.98f, 1.f);
        Config::menu_text_muted = ImVec4(0.55f, 0.62f, 0.70f, 1.f);
        Config::menu_accent     = ImVec4(0.53f, 0.75f, 0.92f, 1.f);
        Config::menu_rounding   = 7.0f;
        Config::menu_opacity    = 0.90f;
        Config::menu_glass      = 0.12f;
        break;
    case 3: // Cyberpunk Neon (Deep obsidian violet + electric magenta purple)
        Config::menu_bg         = ImVec4(0.055f, 0.040f, 0.075f, 0.92f);
        Config::menu_child_bg   = ImVec4(0.110f, 0.080f, 0.150f, 0.62f);
        Config::menu_sidebar_bg = ImVec4(0.035f, 0.025f, 0.050f, 0.86f);
        Config::menu_border     = ImVec4(0.75f, 0.40f, 0.95f, 0.22f);
        Config::menu_text       = ImVec4(0.96f, 0.93f, 0.98f, 1.f);
        Config::menu_text_muted = ImVec4(0.62f, 0.52f, 0.68f, 1.f);
        Config::menu_accent     = ImVec4(0.78f, 0.35f, 0.98f, 1.f);
        Config::menu_rounding   = 6.0f;
        Config::menu_opacity    = 0.92f;
        Config::menu_glass      = 0.12f;
        break;
    case 4: // Emerald Matrix (Deep dark moss + vivid emerald green)
        Config::menu_bg         = ImVec4(0.045f, 0.065f, 0.050f, 0.90f);
        Config::menu_child_bg   = ImVec4(0.090f, 0.130f, 0.100f, 0.60f);
        Config::menu_sidebar_bg = ImVec4(0.025f, 0.040f, 0.030f, 0.85f);
        Config::menu_border     = ImVec4(0.30f, 0.85f, 0.50f, 0.18f);
        Config::menu_text       = ImVec4(0.93f, 0.97f, 0.94f, 1.f);
        Config::menu_text_muted = ImVec4(0.50f, 0.64f, 0.54f, 1.f);
        Config::menu_accent     = ImVec4(0.24f, 0.88f, 0.54f, 1.f);
        Config::menu_rounding   = 6.0f;
        Config::menu_opacity    = 0.90f;
        Config::menu_glass      = 0.12f;
        break;
    case 5: // Sunset Crimson (Volcanic dark slate + warm coral crimson)
        Config::menu_bg         = ImVec4(0.075f, 0.045f, 0.045f, 0.90f);
        Config::menu_child_bg   = ImVec4(0.145f, 0.090f, 0.090f, 0.60f);
        Config::menu_sidebar_bg = ImVec4(0.050f, 0.028f, 0.028f, 0.85f);
        Config::menu_border     = ImVec4(0.95f, 0.40f, 0.35f, 0.18f);
        Config::menu_text       = ImVec4(0.98f, 0.94f, 0.94f, 1.f);
        Config::menu_text_muted = ImVec4(0.68f, 0.54f, 0.54f, 1.f);
        Config::menu_accent     = ImVec4(0.96f, 0.38f, 0.32f, 1.f);
        Config::menu_rounding   = 6.0f;
        Config::menu_opacity    = 0.90f;
        Config::menu_glass      = 0.12f;
        break;
    case 6: // Dracula Velvet (Dracula slate + iconic velvet rose pink)
        Config::menu_bg         = ImVec4(0.105f, 0.100f, 0.135f, 0.90f);
        Config::menu_child_bg   = ImVec4(0.160f, 0.150f, 0.205f, 0.65f);
        Config::menu_sidebar_bg = ImVec4(0.075f, 0.070f, 0.095f, 0.85f);
        Config::menu_border     = ImVec4(0.85f, 0.50f, 0.75f, 0.18f);
        Config::menu_text       = ImVec4(0.96f, 0.95f, 0.98f, 1.f);
        Config::menu_text_muted = ImVec4(0.62f, 0.58f, 0.68f, 1.f);
        Config::menu_accent     = ImVec4(0.96f, 0.45f, 0.72f, 1.f);
        Config::menu_rounding   = 6.0f;
        Config::menu_opacity    = 0.90f;
        Config::menu_glass      = 0.12f;
        break;
    case 7: // Solar Luxury Gold (Warm obsidian charcoal + luxury amber gold)
        Config::menu_bg         = ImVec4(0.065f, 0.060f, 0.050f, 0.90f);
        Config::menu_child_bg   = ImVec4(0.135f, 0.125f, 0.105f, 0.60f);
        Config::menu_sidebar_bg = ImVec4(0.040f, 0.035f, 0.030f, 0.85f);
        Config::menu_border     = ImVec4(0.90f, 0.75f, 0.30f, 0.18f);
        Config::menu_text       = ImVec4(0.98f, 0.96f, 0.92f, 1.f);
        Config::menu_text_muted = ImVec4(0.66f, 0.62f, 0.52f, 1.f);
        Config::menu_accent     = ImVec4(0.98f, 0.78f, 0.22f, 1.f);
        Config::menu_rounding   = 6.0f;
        Config::menu_opacity    = 0.90f;
        Config::menu_glass      = 0.12f;
        break;
    }
    // Preserve the old workbench shell for every accent preset.
    Config::menu_bg         = ImVec4(0.068f, 0.070f, 0.078f, 0.98f);
    Config::menu_child_bg   = ImVec4(0.102f, 0.105f, 0.118f, 1.00f);
    Config::menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
    Config::menu_border     = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
    Config::menu_text       = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    Config::menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
    Config::menu_opacity    = 0.98f;
    Config::menu_glass      = 0.12f;
    ApplyTheme();
}

void DrawWindowFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding) {
    if (!dl) return;
    dl->AddRect(ImVec2(min.x + 1.f, min.y + 2.f),
        ImVec2(max.x + 1.f, max.y + 3.f), IM_COL32(0, 0, 0, 56), rounding, 0, 1.f);
    dl->AddRect(min, max, BorderU32(0.95f), rounding, 0, 1.f);
    const float inset = (std::max)(rounding, 4.f);
    dl->AddRectFilled(ImVec2(min.x + inset, min.y + 1.f),
        ImVec2(max.x - inset, min.y + 2.5f), AccentU32(0.78f), 0.f);
}

void Section(const char* label) {
    if (!label) label = "";
    const bool compact = Config::menu_compact;
    ImGui::Dummy(ImVec2(0, compact ? 5.f : 7.f));
    const ImVec2 marker = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        marker, ImVec2(marker.x + 3.f, marker.y + ImGui::GetFontSize()),
        AccentU32(0.90f), 1.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithA(TextBright(), 0.92f));
    ImGui::TextUnformatted(Lang::T(label));
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, compact ? 2.f : 3.f));
}

void Gap(float mult) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * mult));
}

void SoftSeparator() {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
        BorderU32(0.55f), 1.f);
    ImGui::Dummy(ImVec2(0, Config::menu_compact ? 5.f : 6.f));
}

void BeginCard(const char* id, float width, bool autoY) {
    const Layout L = Layout::Current();
    const ShellPalette p = Palette();
    float w = width;
    if (w > 0.f) {
        const float avail = ImGui::GetContentRegionAvail().x;
        if (w > avail) w = (std::max)(1.f, avail);
    }

    g_cardStack = {};
    g_cardStack.pushColor(ImGuiCol_ChildBg, p.card);
    g_cardStack.pushColor(ImGuiCol_Border, WithA(ImVec4(1.f, 1.f, 1.f, 1.f), 0.09f));
    g_cardStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_cardStack.pushVar(ImGuiStyleVar_WindowPadding, ImVec2(L.cardPad, L.cardPad));
    g_cardStack.pushVar(ImGuiStyleVar_ItemSpacing,
        Config::menu_compact ? ImVec2(10.f, 6.f) : ImVec2(12.f, 7.f));

    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (autoY) flags |= ImGuiChildFlags_AutoResizeY;

    ImGui::BeginChild(id, ImVec2(w, 0), flags, ImGuiWindowFlags_None);
    g_cardOpen = true;
    ImGui::PushItemWidth(-1.f);
    g_cardHasItemWidth = true;
}

void EndCard() {
    if (g_cardHasItemWidth) { ImGui::PopItemWidth(); g_cardHasItemWidth = false; }
    if (g_cardOpen) {
        ImVec2 min = ImGui::GetWindowPos();
        ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
        ImGui::EndChild();
        g_cardOpen = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (dl && (max.x - min.x) > 4.f && (max.y - min.y) > 4.f)
            PaintGloss(dl, min, max, Layout::Current().childRound, 0.35f);

        const int frm = ImGui::GetFrameCount();
        if (g_pair.frame != frm) { g_pair = PairTrack{}; g_pair.frame = frm; }
        if (!g_pair.has) {
            g_pair.has = true; g_pair.min = min; g_pair.max = max;
        } else if (std::fabsf(min.y - g_pair.min.y) < 3.f) {
            ImDrawList* pdl = g_contentDl ? g_contentDl : dl;
            // Quantize to whole pixels and only fill gaps larger than 2px.
            // 1-2px AutoResizeY jitter (hover states, subpixel wrap) used to
            // make this patch block bounce up/down every frame.
            const float hA = floorf(g_pair.max.y - g_pair.min.y + 0.5f);
            const float hB = floorf(max.y - min.y + 0.5f);
            ImVec2 eMin, eMax;
            if (hB > hA + 2.f) {
                eMin = ImVec2(g_pair.min.x, g_pair.max.y - 1.f);
                eMax = ImVec2(g_pair.max.x, g_pair.max.y + hB - hA);
            } else if (hA > hB + 2.f) {
                eMin = ImVec2(min.x, max.y - 1.f);
                eMax = ImVec2(max.x, max.y + hA - hB);
            } else {
                eMin = eMax = ImVec2(0, 0);
            }
            if (pdl && eMax.y > eMin.y + 1.f && eMax.x > eMin.x + 1.f) {
                const ShellPalette p = Palette();
                const float cr = Layout::Current().childRound;
                pdl->AddRectFilled(eMin, eMax, ToU32(p.card), cr, ImDrawFlags_RoundCornersBottom);
                const ImU32 bc = BorderU32(0.72f);
                pdl->AddLine(ImVec2(eMin.x, eMin.y), ImVec2(eMin.x, eMax.y), bc, 1.f);
                pdl->AddLine(ImVec2(eMax.x - 1.f, eMin.y), ImVec2(eMax.x - 1.f, eMax.y), bc, 1.f);
                pdl->AddLine(ImVec2(eMin.x, eMax.y - 1.f), ImVec2(eMax.x, eMax.y - 1.f), bc, 1.f);
            }
            g_pair.has = false;
        } else {
            g_pair.has = true; g_pair.min = min; g_pair.max = max;
        }
    }
    g_cardStack.pop();
}

void BeginStrip(const char* id) {
    const Layout L = Layout::Current();
    const ShellPalette p = Palette();
    g_stripStack = {};
    g_stripStack.pushColor(ImGuiCol_ChildBg, p.card);
    g_stripStack.pushColor(ImGuiCol_Border, WithA(ImVec4(1.f, 1.f, 1.f, 1.f), 0.09f));
    g_stripStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_stripStack.pushVar(ImGuiStyleVar_WindowPadding,
        ImVec2(L.compact ? 8.f : 10.f, L.compact ? 6.f : 8.f));
    ImGui::BeginChild(id, ImVec2(-1.f, 0.f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_None);
    g_stripOpen = true;
}

void EndStrip() {
    if (g_stripOpen) {
        ImVec2 min = ImGui::GetWindowPos();
        ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
        ImGui::EndChild();
        g_stripOpen = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (dl && (max.x - min.x) > 4.f && (max.y - min.y) > 4.f)
            PaintGloss(dl, min, max, Layout::Current().childRound, 0.35f);
    }
    g_stripStack.pop();
}

bool Slider(const char* label, const char* id, float* v, float vmin, float vmax, const char* fmt) {
    return DrawValueSlider(label, id, v, vmin, vmax, fmt, false);
}

bool SliderInt(const char* label, const char* id, int* v, int vmin, int vmax, const char* fmt) {
    if (!v)
        return false;
    float fv = (float)*v;
    const bool changed = DrawValueSlider(label, id, &fv, (float)vmin, (float)vmax, fmt, true);
    if (changed)
        *v = (int)fv;
    return changed;
}

bool Combo(const char* label, const char* id, int* cur, const char* const items[], int count) {
    std::vector<std::string> translatedItems;
    std::vector<const char*> visibleItems;
    translatedItems.reserve(count > 0 ? static_cast<std::size_t>(count) : 0u);
    visibleItems.reserve(count > 0 ? static_cast<std::size_t>(count) : 0u);
    for (int i = 0; i < count; ++i)
        translatedItems.emplace_back(Lang::T(items && items[i] ? items[i] : ""));
    for (const std::string& item : translatedItems)
        visibleItems.push_back(item.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, WithA(TextMuted(), 0.95f));
    // 遵循 ImGui 约定: 以 "##" 开头的 label 只做 ID, 不渲染文本。
    if (label && !(label[0] == '#' && label[1] == '#'))
        ImGui::TextUnformatted(Lang::T(label));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1.f);
    return ImGui::Combo(id, cur, visibleItems.data(), count);
}

void Tip(const char* text) {
    if (text && text[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

bool FeatureToggle(const char* label, bool* v, const char* tip) {
    if (!v)
        return false;
    const bool compact = Config::menu_compact;
    const float rowH = compact ? 22.f : 24.f;
    const float swW = compact ? 29.f : 31.f;
    const float swH = compact ? 18.f : 19.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    ImGui::PushID(label ? label : "ft");
    ImGui::InvisibleButton("##hit", ImVec2((std::max)(8.f, avail), rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (pressed)
        *v = !*v;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float midY = p0.y + rowH * 0.5f;
    const float swX = p0.x + avail - swW;
    const float swY = midY - swH * 0.5f;
    const float knob = swH - 4.f;
    const float rr = swH * 0.5f;
    const ImU32 track = *v
        ? AccentU32(hovered ? 0.95f : 0.84f)
        : ToU32(WithA(TextMuted(), hovered ? 0.36f : 0.24f));
    dl->AddRectFilled(ImVec2(swX, swY), ImVec2(swX + swW, swY + swH), track, rr);
    if (*v)
        dl->AddRectFilledMultiColor(
            ImVec2(swX, swY), ImVec2(swX + swW, swY + swH * 0.45f),
            IM_COL32(255, 255, 255, 36), IM_COL32(255, 255, 255, 36),
            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    const float kx = swX + 2.f + (*v ? (swW - knob - 4.f) : 0.f);
    const ImVec2 kc(kx + knob * 0.5f, midY);
    dl->AddCircleFilled(kc, knob * 0.5f + 1.f, AccentU32(*v ? 0.78f : 0.32f));
    dl->AddCircleFilled(kc, knob * 0.5f, ToU32(WithA(TextBright(), 0.98f)));

    if (label && label[0]) {
        const ImU32 tc = ToU32(*v ? TextBright() : WithA(TextMuted(), 0.95f));
        const char* visibleLabel = Lang::T(label);
        const ImVec2 ts = ImGui::CalcTextSize(visibleLabel);
        dl->AddText(ImVec2(p0.x, midY - ts.y * 0.5f), tc, visibleLabel);
    }

    if (hovered) {
        if (tip && tip[0])
            ImGui::SetTooltip("%s\n\xE5\x8F\xB3\xE9\x94\xAE\xE6\x89\x93\xE5\xBC\x80\xE8\xAE\xBE\xE7\xBD\xAE\xE3\x80\x82", Lang::T(tip));
        else
            ImGui::SetTooltip("\xE5\x8F\xB3\xE9\x94\xAE\xE6\x89\x93\xE5\xBC\x80\xE8\xAE\xBE\xE7\xBD\xAE\xE3\x80\x82");
    }
    ImGui::PopID();
    return pressed;
}

void PopupTitle(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, TextBright());
    ImGui::TextUnformatted(Lang::T(title));
    ImGui::PopStyleColor();
    SoftSeparator();
}

bool NavIcon(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index) {
    (void)index;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));

    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "##nav_%s", label ? label : "x");
    const bool pressed = ImGui::Button(idBuf, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a(min.x + 2.f, min.y + 2.f);
    const ImVec2 b(max.x - 2.f, max.y - 2.f);
    const float rr = 6.f;

    if (hovered && !selected)
        dl->AddRectFilled(a, b, ToU32(WithA(TextBright(), 0.045f)), rr);
    if (selected) {
        dl->AddRectFilled(a, b, ToU32(WithA(TextBright(), 0.055f)), rr);
        dl->AddRectFilled(ImVec2(a.x, a.y + 5.f), ImVec2(a.x + 3.f, b.y - 5.f),
            AccentU32(1.f), 1.5f);
    }

    const ImU32 iconCol = selected ? AccentU32(1.f)
        : (hovered ? ToU32(TextBright()) : ToU32(WithA(TextMuted(), 0.78f)));

    if (iconUtf8 && iconUtf8[0] && g_MenuIconFont) {
        const float iconSz = 16.f;
        const ImVec2 isz = g_MenuIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, iconUtf8);
        const float cx = floorf((min.x + max.x) * 0.5f - isz.x * 0.5f);
        float cy = floorf((min.y + max.y) * 0.5f - isz.y * 0.5f);
        if (Config::menu_sidebar_labels && label && label[0]) {
            ImFont* font = ImGui::GetFont();
            // Draw at native atlas size - downscaling to a fixed 12px blurs glyphs.
            // Cap at 18 so labels never collide with the icon row (navBtnH 48).
            const float ls = (std::min)(ImGui::GetFontSize(), 18.f);
            const char* visibleLabel = Lang::T(label);
            const ImVec2 lsz = font->CalcTextSizeA(ls, FLT_MAX, 0.f, visibleLabel);
            const float lx = floorf((min.x + max.x) * 0.5f - lsz.x * 0.5f);
            const float ly = floorf(max.y - lsz.y - 4.f);
            dl->AddText(font, ls, ImVec2(lx, ly), iconCol, visibleLabel);
            cy = floorf(min.y + (max.y - min.y) * 0.42f - isz.y * 0.5f);
        }
        dl->AddText(g_MenuIconFont, iconSz, ImVec2(cx, cy), iconCol, iconUtf8);
    }

    if (label && label[0] && !Config::menu_sidebar_labels
        && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", Lang::T(label));

    return pressed;
}

bool NavButton(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index) {
    if (size.x > 0.f && size.x <= 80.f && iconUtf8 && iconUtf8[0])
        return NavIcon(label, iconUtf8, selected, size, index);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));

    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "##nav_%s", label ? label : "x");
    const bool pressed = ImGui::Button(idBuf, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !selected)
        dl->AddRectFilled(ImVec2(min.x + 2.f, min.y + 3.f),
            ImVec2(max.x - 2.f, max.y - 3.f), ToU32(WithA(TextBright(), 0.045f)), 3.f);
    if (selected) {
        dl->AddRectFilled(ImVec2(min.x + 2.f, min.y + 5.f),
            ImVec2(min.x + 4.f, max.y - 5.f), AccentU32(1.f), 1.f);
        dl->AddRectFilled(ImVec2(min.x + 7.f, min.y + 3.f),
            ImVec2(max.x - 2.f, max.y - 3.f), ToU32(WithA(TextBright(), 0.055f)), 3.f);
    }
    const ImU32 col = selected || hovered ? ToU32(TextBright()) : ToU32(TextMuted());
    const ImU32 iconCol = selected ? AccentU32(1.f) : col;

    float x = min.x + 10.f;
    const float midY = (min.y + max.y) * 0.5f;
    if (iconUtf8 && iconUtf8[0] && g_MenuIconFont) {
        const float iconSz = 16.f;
        const ImVec2 isz = g_MenuIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, iconUtf8);
        dl->AddText(g_MenuIconFont, iconSz, ImVec2(x, midY - isz.y * 0.5f), iconCol, iconUtf8);
        x += isz.x + 8.f;
    }
    if (label && label[0]) {
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const char* visibleLabel = Lang::T(label);
        const ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, visibleLabel);
        dl->AddText(font, fs, ImVec2(x, midY - tsz.y * 0.5f), col, visibleLabel);
    }
    return pressed;
}

bool NavButton(const char* label, bool selected, const ImVec2& size, int index) {
    return NavButton(label, nullptr, selected, size, index);
}

void SubNav(const char* const* labels, int count, int* selected) {
    if (!labels || count <= 0 || !selected) return;

    static const char* s_selKey = nullptr;
    static int s_selVal = -1;
    if (s_selKey != labels[0] || s_selVal != *selected) {
        s_selKey = labels[0];
        s_selVal = *selected;
        ImGui::SetScrollY(0.f);
    }

    const Layout L = Layout::Current();
    const float h = L.subNavH;
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 trackMin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddLine(
        ImVec2(trackMin.x, trackMin.y + h - 0.5f),
        ImVec2(trackMin.x + avail, trackMin.y + h - 0.5f),
        IM_COL32(255, 255, 255, 16), 1.f);

    const float baseW = floorf(avail / (float)count);
    const float lastW = (std::max)(1.f, avail - baseW * (count - 1));

    ImGui::SetCursorScreenPos(trackMin);
    ImGui::BeginGroup();
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0, 0.f);
        const float w = (i == count - 1) ? lastW : baseW;
        const bool sel = (*selected == i);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithA(TextBright(), 0.05f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithA(Accent(), 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? TextBright() : TextMuted());

        char idBuf[64];
        std::snprintf(idBuf, sizeof(idBuf), "%s##sub_%d", labels[i], i);
        if (ImGui::Button(idBuf, ImVec2(w, h)))
            *selected = i;

        if (sel) {
            const ImVec2 bmin = ImGui::GetItemRectMin();
            const ImVec2 bmax = ImGui::GetItemRectMax();
            dl->AddRectFilled(
                ImVec2(bmin.x + 8.f, bmax.y - 2.f),
                ImVec2(bmax.x - 8.f, bmax.y),
                AccentU32(0.95f), 1.5f);
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(trackMin.x, trackMin.y + h + (Config::menu_compact ? 5.f : 7.f)));
    ImGui::Dummy(ImVec2(0, 0));
}

void BeginSidebar(const Layout& L, float height) {
    const ShellPalette p = Palette();
    g_sideStack = {};
    g_sideStack.pushColor(ImGuiCol_ChildBg, p.side);
    g_sideStack.pushColor(ImGuiCol_Border, WithA(Config::menu_border, 0.78f));
    g_sideStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_sideStack.pushVar(ImGuiStyleVar_WindowPadding,
        L.compact ? ImVec2(4.f, 6.f) : ImVec2(6.f, 8.f));
    g_sideStack.pushVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(0.f, L.compact ? 3.f : 4.f));

    ImGui::BeginChild("##sidebar", ImVec2(L.sidebar, height),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar);
    g_sideOpen = true;
}

void EndSidebar() {
    if (g_sideOpen) { ImGui::EndChild(); g_sideOpen = false; }
    g_sideStack.pop();
}

void BeginContent(const Layout& L, float contentW, float contentH) {
    const ShellPalette p = Palette();
    g_contentStack = {};
    g_contentStack.pushVar(ImGuiStyleVar_WindowPadding, ImVec2(L.contentPad, L.contentPad));
    g_contentStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_contentStack.pushColor(ImGuiCol_ChildBg, WithA(p.bg, 0.0f));
    g_contentStack.pushColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    ImGui::BeginChild("##content", ImVec2(contentW, contentH),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_None);
    g_contentOpen = true;
    g_contentDl = ImGui::GetWindowDrawList();
}

void EndContent() {
    if (g_contentOpen) { ImGui::EndChild(); g_contentOpen = false; }
    g_contentStack.pop();
}

namespace {
bool s_headerDrag = false;
}

void BeginHeader(const Layout& L, float windowW) {
    const ImVec2 wp = ImGui::GetWindowPos();
    const float h = L.headerH;
    ImGui::SetCursorPos(ImVec2(0.f, 0.f));
    ImGui::InvisibleButton("##win_drag", ImVec2(windowW, h));
    const bool hover = ImGui::IsItemHovered();
    const bool held  = ImGui::IsItemActive();

    if (hover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 ws = ImGui::GetWindowSize();
        ImGui::SetWindowPos(ImVec2((io.DisplaySize.x - ws.x) * 0.5f, (io.DisplaySize.y - ws.y) * 0.5f));
        s_headerDrag = false;
    } else if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_headerDrag = true;
    }
    if (s_headerDrag && held) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 w0 = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(w0.x + io.MouseDelta.x, w0.y + io.MouseDelta.y));
    } else if (!held) {
        s_headerDrag = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ShellPalette p = Palette();
    dl->AddRectFilled(wp, ImVec2(wp.x + windowW, wp.y + h),
        ToU32(p.side), 0.f);
    dl->AddRectFilled(ImVec2(wp.x + L.shellPad, wp.y),
        ImVec2(wp.x + windowW - L.shellPad, wp.y + 2.f),
        AccentU32(0.78f), 1.f);
    dl->AddLine(
        ImVec2(wp.x + L.shellPad, wp.y + h),
        ImVec2(wp.x + windowW - L.shellPad, wp.y + h),
        BorderU32(0.72f), 1.f);

    ImGui::SetCursorPos(ImVec2(L.contentPad, (h - ImGui::GetFontSize()) * 0.5f));
}

void HeaderBrand(const char* name, const char* accentSuffix, const char* badge) {
    if (name && name[0]) {
        const ImVec2 dot = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(dot.x + 3.f, dot.y + ImGui::GetFontSize() * 0.52f),
            3.f, AccentU32(0.95f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 11.f);
        ImGui::PushStyleColor(ImGuiCol_Text, TextBright());
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
    }
    if (accentSuffix && accentSuffix[0]) {
        ImGui::SameLine(0, 6.f);
        ImGui::PushStyleColor(ImGuiCol_Text, WithA(Accent(), 0.82f));
        ImGui::TextUnformatted(accentSuffix);
        ImGui::PopStyleColor();
    }
    if (badge && badge[0]) {
        ImGui::SameLine(0, 8.f);
        const ImVec2 badgePos = ImGui::GetCursorScreenPos();
        const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(badgePos.x - 6.f, badgePos.y - 2.f),
            ImVec2(badgePos.x + badgeSize.x + 6.f, badgePos.y + badgeSize.y + 2.f),
            AccentU32(0.16f), 3.f);
        ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
        ImGui::TextUnformatted(badge);
        ImGui::PopStyleColor();
    }
}

void HeaderRightHint(const char* text) {
    if (!text || !text[0]) return;
    const Layout L = Layout::Current();
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float dotGap = 10.f;
    const float textX = ws.x - ts.x - L.contentPad;
    const float textY = (L.headerH - ts.y) * 0.5f;
    ImGui::SetCursorPos(ImVec2(textX - dotGap, textY));
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(ImGui::GetCursorScreenPos().x + 3.f,
            ImGui::GetCursorScreenPos().y + ts.y * 0.52f),
        3.f, AccentU32(0.90f));
    ImGui::SetCursorPos(ImVec2(textX, textY));
    ImGui::PushStyleColor(ImGuiCol_Text, WithA(TextMuted(), 0.85f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

} // namespace MenuUI

