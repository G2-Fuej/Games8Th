#include "menu.h"
#include "../config/config.h"
#include "../config/language.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include "../config/configmanager.h"

#include "../keybinds/keybinds.h"
#include "../features/grenade_helper/grenade_helper.h"
#include "../features/notify/notify.h"
#include "../features/hitsound/hitsound.h"
#include "../features/hitmarker/hitmarker.h"
#include "../features/skinchanger/skin_menu.h"
#include <string>

#include "../utils/logging/log.h"
#include "../utils/console/console.h"
#include "../features/visuals/assets/undefeated_font.hpp"
#include "icons/fa_solid_font.hpp"

ImFont* g_WeaponIconFont = nullptr;
ImFont* g_MenuIconFont = nullptr;

// Font Awesome 6 Free solid codepoints (UTF-8) for sidebar tabs
namespace MenuTabIcon {
 // subset glyphs: crosshairs f05b, eye f06e, folder-open f07c, sliders f1de
    static const char* kAim     = "\xef\x81\x9b"; // U+F05B
    static const char* kVisuals = "\xef\x81\xae"; // U+F06E
    static const char* kSkins   = "\xef\x81\xbc"; // U+F07C folder-open (subset has no palette glyph)
    static const char* kMisc    = "\xef\x87\x9e"; // U+F1DE
    static const char* kConfig  = "\xef\x81\xbc"; // U+F07C
    static const char* const kAll[5] = { kAim, kVisuals, kSkins, kMisc, kConfig };
}

#include "menu_ui.h"

// Thin layout facade - chrome lives in menu_ui.*.
namespace ui {
    using MenuUI::Accent;
    using MenuUI::TextMuted;
    using MenuUI::TextBright;
    using MenuUI::WithA;
    using MenuUI::ToU32;
    using MenuUI::AccentU32;
    using MenuUI::BorderU32;
    using MenuUI::DrawWindowFrame;

    static void SectionLabel(const char* label) { MenuUI::Section(label); }
    static void BeginCard(const char* id, float width, bool autoY = false) {
        MenuUI::BeginCard(id, width, autoY);
    }
    static void EndCard() { MenuUI::EndCard(); }
    static void BeginStrip(const char* id) { MenuUI::BeginStrip(id); }
    static void EndStrip() { MenuUI::EndStrip(); }

    static bool SliderFull(const char* label, const char* id, float* v, float vmin, float vmax,
                           const char* fmt = "%.3f") {
        return MenuUI::Slider(label, id, v, vmin, vmax, fmt);
    }
    static bool SliderInt(const char* label, const char* id, int* v, int vmin, int vmax,
                          const char* fmt = "%d") {
        return MenuUI::SliderInt(label, id, v, vmin, vmax, fmt);
    }
    static bool ComboFull(const char* label, const char* id, int* cur, const char* const items[],
                          int count) {
        return MenuUI::Combo(label, id, cur, items, count);
    }
    static bool FeatureToggle(const char* label, bool* v, const char* tip = nullptr) {
        return MenuUI::FeatureToggle(label, v, tip);
    }
    static bool Checkbox(const char* label, bool* v) {
        return ImGui::Checkbox(Lang::T(label), v);
    }
    static void PopupTitle(const char* title) { MenuUI::PopupTitle(title); }

    static bool NavButton(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index = 0) {
        return MenuUI::NavButton(label, iconUtf8, selected, size, index);
    }
    static bool NavButton(const char* label, bool selected, const ImVec2& size, int index = 0) {
        return MenuUI::NavButton(label, selected, size, index);
    }
    static void SubNav(const char* const* labels, int count, int* selected) {
        MenuUI::SubNav(labels, count, selected);
    }
    static void ApplyMenuPreset(int idx) { MenuUI::ApplyPreset(idx); }

} // namespace ui

void ApplyImGuiTheme() {
    MenuUI::ApplyTheme();
}

namespace {
float g_builtFontSize = -1.f;

void AddMenuFonts(float uiFontSize) {
    ImGuiIO& io = ImGui::GetIO();
    uiFontSize = std::clamp(uiFontSize, 12.f, 24.f);

    static const ImWchar kChineseRanges[] = {
        0x0020, 0x00FF, 0x2000, 0x206F, 0x3000, 0x30FF,
        0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0
    };
    ImFontConfig uiCfg{};
    uiCfg.OversampleH = 1;
    uiCfg.OversampleV = 1;
    uiCfg.PixelSnapH = true;

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
        uiFontSize, &uiCfg, kChineseRanges);
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf",
            uiFontSize, &uiCfg, kChineseRanges);
    if (!font)
        io.Fonts->AddFontDefault();

    {
        ImFontConfig iconCfg{};
        iconCfg.FontDataOwnedByAtlas = false;
        iconCfg.OversampleH = 3;
        iconCfg.OversampleV = 3;
        iconCfg.PixelSnapH = false;
        static const ImWchar iconRanges[] = { 0x20, 0xFF, 0 };
        g_WeaponIconFont = io.Fonts->AddFontFromMemoryTTF(
            undefeated_font::font_data,
            undefeated_font::font_size,
            28.0f,
            &iconCfg,
            iconRanges);
    }

    {
        ImFontConfig faCfg{};
        faCfg.FontDataOwnedByAtlas = false;
        faCfg.OversampleH = 3;
        faCfg.OversampleV = 3;
        faCfg.PixelSnapH = false;
        static const ImWchar faRanges[] = {
            0xF05B, 0xF05B,
            0xF06E, 0xF06E,
            0xF1DE, 0xF1DE,
            0xF07C, 0xF07C,
            0
        };
        g_MenuIconFont = io.Fonts->AddFontFromMemoryTTF(
            MenuIconsFont::font_data,
            MenuIconsFont::font_size,
            16.0f,
            &faCfg,
            faRanges);
    }
}

} // namespace

void MenuTryRebuildFonts() {
    if (!ImGui::GetCurrentContext())
        return;
    const float want = std::clamp(Config::ui_font_size, 12.f, 24.f);
    if (g_builtFontSize < 0.f) {
        g_builtFontSize = want;
        return;
    }
    if (std::fabs(want - g_builtFontSize) < 0.5f)
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->ClearFonts();
    g_WeaponIconFont = nullptr;
    g_MenuIconFont = nullptr;
    AddMenuFonts(want);
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
    g_builtFontSize = want;
}


Menu::Menu() {
    activeTab = 0;
    showMenu = false; // must start closed ??" open menu disables relative mouse and drifts the cursor
}


// Multi-select Smoke / Flash / Scope checks (aimbot / autofire / trigger)
static void ChecksDropdown(const char* label, const char* id,
    bool* smoke, bool* flash, bool* scope)
{
    char preview[96];
    preview[0] = '\0';
    int n = 0;
    auto append = [&](const char* s) {
        if (n > 0) strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
        strncat_s(preview, sizeof(preview), s, _TRUNCATE);
        ++n;
    };
    if (smoke && *smoke) append(Lang::T("Smoke"));
    if (flash && *flash) append(Lang::T("Flash"));
    if (scope && *scope) append(Lang::T("Scope"));
    if (n == 0) strcpy_s(preview, Lang::T("None"));

    ImGui::TextUnformatted(Lang::T(label));
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo(id, preview)) {
        if (smoke) {
            ImGui::Checkbox(Lang::T("Smoke Check"), smoke);
        }
        if (flash) {
            ImGui::Checkbox(Lang::T("Flash Check"), flash);
        }
        if (scope) {
            ImGui::Checkbox(Lang::T("Scope Check"), scope);
        }
        ImGui::EndCombo();
    }
}

// Scope checks are independent per mode (aim / autofire / trigger).
// Old version forced autofire+trigger to mirror aim - fixed so trigger's own
// smoke/flash/scope settings work without being overridden by aimbot.

// Push full edited profile -> live only when UI group == held weapon group.
static void PushLiveIfEditingActive() {
    if (Config::weapon_group_ui == Config::weapon_group_active)
        Config::ApplyProfileToLive(Config::weapon_group_ui);
}

// Mode + bind: side-by-side when wide enough, else stacked.
// Explicit widths ??" never let Button(w=0) auto-size off the card edge.
static void KeybindRow(bool& feature) {
    const float gap = 8.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail < 220.f) {
        ImGui::TextDisabled(Lang::T("Mode"));
        keybind.menuMode(feature, -1.f);
        ImGui::TextDisabled(Lang::T("Bind"));
        keybind.menuButton(feature, -1.f);
        return;
    }

    const float colW = floorf((avail - gap) * 0.5f);

    ImGui::BeginGroup();
    ImGui::TextDisabled(Lang::T("Mode"));
    keybind.menuMode(feature, colW);
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);

    ImGui::BeginGroup();
    ImGui::TextDisabled(Lang::T("Bind"));
 // Use remaining line width (not colW) so rounding never overflows
    keybind.menuButton(feature, -1.f);
    ImGui::EndGroup();
}

// Stacked full-width keybind (thirdperson / lineup)
static void KeybindStack(bool& feature) {
    ImGui::TextDisabled(Lang::T("Mode"));
    keybind.menuMode(feature, -1.f);
    ImGui::TextDisabled(Lang::T("Bind"));
    keybind.menuButton(feature, -1.f);
}

// Shared multi-select hitbox combo
static void HitboxMultiSelect(const char* title, const char* id,
    bool* boxes, int count, const char* const* names,
    int idBase, const char* tip)
{
    char preview[96];
    preview[0] = '\0';
    int nSel = 0;
    for (int i = 0; i < count; ++i) {
        if (!boxes[i]) continue;
        if (nSel > 0)
            strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
        strncat_s(preview, sizeof(preview), Lang::T(names[i]), _TRUNCATE);
        ++nSel;
    }
    if (nSel == 0)
        strcpy_s(preview, Lang::T("None"));

    ImGui::TextUnformatted(Lang::T(title));
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(idBase + i);
            if (ImGui::Checkbox(Lang::T(names[i]), &boxes[i])) {
                bool any = false;
                for (int j = 0; j < count; ++j)
                    if (boxes[j]) { any = true; break; }
                if (!any)
                    boxes[i] = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (tip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
}

static bool IsEditingActiveGroup() {
    return Config::weapon_group_ui == Config::weapon_group_active;
}

void Menu::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
    internal_config::ConfigManager::LoadMenuSize();
    {
        static char s_iniPath[MAX_PATH]{};
        const auto ini = internal_config::ConfigManager::MenuSizePath().parent_path() / "imgui.ini";
        strncpy_s(s_iniPath, ini.string().c_str(), _TRUNCATE);
        io.IniFilename = s_iniPath[0] ? s_iniPath : nullptr;
    }
 // SEH mid-frame (GPU paint-kit seed) can leave BeginChild open.
 // Recover stack without CRT assert popup (Missing EndChild).
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = true;
    io.ConfigErrorRecoveryEnableTooltip = false;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(pDevice, pContext);

    MenuUI::ApplyTheme();
    ImGui::GetIO().FontGlobalScale = 1.f;

    const float uiFontSize = std::clamp(Config::ui_font_size, 12.f, 24.f);
    AddMenuFonts(uiFontSize);
    g_builtFontSize = uiFontSize;
    if (!g_MenuIconFont)
        Con::Error("Menu icon font failed to load");
    else
        Con::Ok("Menu tab icons (Font Awesome solid)");

    std::cout << "initialized menu\n";
}

void Menu::shutdown() noexcept {
    __try {
        if (Config::menu_w >= 640.f && Config::menu_h >= 420.f)
            internal_config::ConfigManager::SaveMenuSize();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // best effort - may already be partially torn down
        __try { ImGui::DestroyContext(nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void Menu::render() {
 // keybind.pollInputs runs in hkPresent every frame (not only when overlay draws)

 // Instant close: AnimTick(false) zeros state; no draw after toggle off
    MenuUI::AnimTick(showMenu);
    if (!showMenu || !MenuUI::AnimVisible()) {
 // DPI only while menu open ??" keep ESP/HUD at 1x
        if (ImGui::GetIO().FontGlobalScale != 1.f)
            ImGui::GetIO().FontGlobalScale = 1.f;
        return;
    }

    MenuUI::ApplyTheme();
    if (activeTab < 0 || activeTab > 4)
        activeTab = 0;

    MenuUI::Layout L = MenuUI::Layout::Current();
    const float openA = MenuUI::OpenAlpha();

    const float dpi = L.dpi;
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kFixedMenuW = 520.f;
    constexpr float kFixedMenuH = 500.f;
    L.windowW = (std::min)(kFixedMenuW, (std::max)(360.f, io.DisplaySize.x - 24.f));
    L.windowH = (std::min)(kFixedMenuH, (std::max)(300.f, io.DisplaySize.y - 24.f));
    ImGui::SetNextWindowSize(ImVec2(L.windowW, L.windowH), ImGuiCond_Always);
    {
        ImVec2 startPos(80.f * dpi, 80.f * dpi);
        if (Config::menu_x >= 0.f && Config::menu_y >= 0.f) {
            startPos.x = Config::menu_x;
            startPos.y = Config::menu_y;
        }
        startPos.x = (std::min)(startPos.x, (std::max)(0.f, io.DisplaySize.x - L.windowW - 12.f));
        startPos.y = (std::min)(startPos.y, (std::max)(0.f, io.DisplaySize.y - L.windowH - 12.f));
        ImGui::SetNextWindowPos(startPos, ImGuiCond_Once);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(L.windowW, L.windowH), ImVec2(L.windowW, L.windowH));
    ImGui::SetNextWindowBgAlpha((std::clamp)(Config::menu_opacity, 0.55f, 1.f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, openA);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
        (std::clamp)((std::max)(Config::menu_rounding, 2.f), 2.f, 8.f));

    ImGui::Begin("Games8Th", nullptr, flags);

    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    const bool dragging = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!dragging && (fabsf(wpos.x - Config::menu_x) > 0.5f
        || fabsf(wpos.y - Config::menu_y) > 0.5f)) {
        Config::menu_w = kFixedMenuW;
        Config::menu_h = kFixedMenuH;
        Config::menu_x = wpos.x;
        Config::menu_y = wpos.y;
        internal_config::ConfigManager::SaveMenuSize();
    }
    const float round = ImGui::GetStyle().WindowRounding;
    const float pad = L.shellPad;

    ImGui::SetCursorPos(ImVec2(pad, pad));
    const float bodyH = (std::max)(80.f, wsize.y - pad * 2.f - L.headerH);

    MenuUI::BeginHeader(L, wsize.x);
    {
        char fpsBuf[48];
        const float fps = ImGui::GetIO().Framerate;
        if (fps > 1.f)
            std::snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", fps);
        else
            std::snprintf(fpsBuf, sizeof(fpsBuf), "--");
        MenuUI::HeaderRightHint(fpsBuf);
    }
    ImGui::SetCursorPos(ImVec2(pad, pad + L.headerH));

    MenuUI::BeginSidebar(L, bodyH);
    {
        static const char* tabs[] = { "Aim", "Visuals", "Skins", "Misc", "Config" };
        const float btnW = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < 5; ++i) {
            if (MenuUI::NavButton(tabs[i], MenuTabIcon::kAll[i], activeTab == i,
                    ImVec2(btnW, L.navBtnH), i)) {
                activeTab = i;
                MenuUI::NotifyTab(i);
            }
        }
    }
    MenuUI::EndSidebar();

    ImGui::SameLine(0, L.gap);

    const float contentW = (std::max)(1.f,
        wsize.x - pad * 2.f - L.sidebar - L.gap);

    MenuUI::BeginContent(L, contentW, bodyH);
    {
        static int s_renderTab = -1;
        if (s_renderTab != activeTab) {
            s_renderTab = activeTab;
            ImGui::SetScrollY(0.f);
        }

        const float gap = L.gap;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail < gap + 2.f)
            avail = gap + 2.f;
        const float half = L.colLeft(avail);

        switch (activeTab) {
        case 0: // Aim
        {
            static int aimSub = 0; // 0 Aimbot | 1 Autofire | 2 Trigger | 3 Extra | 4 Anti-Aim
            static const char* kHitboxNames[] = {
                "Head", "Neck", "Chest", "Stomach", "Pelvis", "Arms", "Legs", "Feet"
            };
            static const char* kGroups[] = {
                "General", "Pistols", "SMGs", "Rifles", "Shotguns", "Snipers", "LMGs"
            };

 // Sticky weapon group (full width, all aim pages)
            {
                ui::BeginStrip("##aim_wg");

                ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                ImGui::TextUnformatted(Lang::T("Weapon Group"));
                ImGui::PopStyleColor();

                if (Config::weapon_group_ui < 0 || Config::weapon_group_ui >= Config::WG_COUNT)
                    Config::weapon_group_ui = Config::WG_GENERAL;

                const float rowGap = 6.f;
                const float stripAvail = ImGui::GetContentRegionAvail().x;
                const float minBtn = 100.f;
                const bool rowOk = stripAvail >= (minBtn * 2.f + 160.f + rowGap * 2.f);

                if (rowOk) {
                    const float btnW = minBtn + 18.f;
                    const float comboW = stripAvail - (btnW * 2.f) - rowGap * 2.f;
                    ImGui::SetNextItemWidth((std::max)(80.f, comboW));
                    ui::ComboFull("##weapon_group", "##weapon_group_sel", &Config::weapon_group_ui, kGroups, IM_ARRAYSIZE(kGroups));

                    ImGui::SameLine(0, rowGap);
                    if (ImGui::Button(Lang::T("Jump Active"), ImVec2(btnW, 0)))
                        Config::weapon_group_ui = Config::weapon_group_active;

                    ImGui::SameLine(0, rowGap);
                    if (ImGui::Button(Lang::T("Copy -> All"), ImVec2(btnW, 0))) {
                        const Config::AimWeaponProfile src = Config::MenuAimProfile();
                        for (int g = 0; g < Config::WG_COUNT; ++g)
                            Config::weapon_profiles[g] = src;
                        Config::ApplyProfileToLive(Config::weapon_group_active);
                    }
                } else {
                    ImGui::SetNextItemWidth(-1.f);
                    ui::ComboFull("##weapon_group", "##weapon_group_sel", &Config::weapon_group_ui, kGroups, IM_ARRAYSIZE(kGroups));
                    const float stackAvail = ImGui::GetContentRegionAvail().x;
                    const float half0 = floorf((std::max)(0.f, stackAvail - rowGap) * 0.5f);
                    const float half1 = (std::max)(1.f, stackAvail - half0 - rowGap);
                    if (ImGui::Button(Lang::T("Jump Active"), ImVec2(half0, 0)))
                        Config::weapon_group_ui = Config::weapon_group_active;
                    ImGui::SameLine(0, rowGap);
                    if (ImGui::Button(Lang::T("Copy -> All"), ImVec2(half1, 0))) {
                        const Config::AimWeaponProfile src = Config::MenuAimProfile();
                        for (int g = 0; g < Config::WG_COUNT; ++g)
                            Config::weapon_profiles[g] = src;
                        Config::ApplyProfileToLive(Config::weapon_group_active);
                    }
                }

                const bool editingActive = IsEditingActiveGroup();
                ImGui::TextDisabled(Lang::T("Edit: %s  |  Live: %s%s"),
                    Config::WeaponGroupName(Config::weapon_group_ui),
                    Config::WeaponGroupName(Config::weapon_group_active),
                    editingActive ? "  (synced)" : "");

                ui::EndStrip();
            }

            MenuUI::Gap(0.75f);

            {
                static const char* kAimTabs[] = { "Aimbot", "Autofire", "Trigger", "Extra", "Anti-Aim" };
                ui::SubNav(kAimTabs, 5, &aimSub);
            }

            Config::AimWeaponProfile& aimP = Config::MenuAimProfile();
            const bool live = IsEditingActiveGroup();
            (void)live; // live push is PushLiveIfEditingActive at end of aim tab

 // ?-??-??-??-??-??-??-??-??-??-??-??-? AIMBOT ?-??-??-??-??-??-??-??-??-??-??-??-?
            if (aimSub == 0) {
                ui::BeginCard("##aim_ab_l", half, true);
                ui::SectionLabel("Aimbot");
                ImGui::Checkbox(Lang::T("Enable"), &Config::aimbot);
                KeybindRow(Config::aimbot);

                ui::SliderFull("FOV", "##ab_fov", &aimP.aimbot_fov, 0.f, 90.f, "%.1f");

                ui::SliderFull("Smooth", "##ab_smooth", &aimP.aimbot_smooth, 0.f, 100.f, "%.1f");


                {
                    static const char* kSmoothModes[] = { "Constant", "Linear", "Sine" };
                    if (aimP.aimbot_smooth_mode < 0 || aimP.aimbot_smooth_mode >= Config::SMOOTH_MODE_COUNT)
                        aimP.aimbot_smooth_mode = Config::SMOOTH_LINEAR;
                    ui::ComboFull("Smooth Mode", "##sm_mode", &aimP.aimbot_smooth_mode, kSmoothModes, Config::SMOOTH_MODE_COUNT);
                }

                ImGui::BeginDisabled(aimP.aimbot_smooth <= 0.01f);
                ui::SliderFull("Humanize", "##ab_human", &aimP.aimbot_humanize, 0.f, 100.f, "%.0f");
                ImGui::EndDisabled();

                HitboxMultiSelect("Hitboxes", "##aim_hitboxes",
                    aimP.aim_hitboxes, Config::HB_COUNT, kHitboxNames, 0,
                    "Which body parts aimbot targets.");

                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_ab_r", 0, true);
                ui::SectionLabel("Filters");
                if (ImGui::Checkbox(Lang::T("Team Check##aim"), &Config::team_check))
                    Config::teamCheck = Config::team_check;
                ImGui::Checkbox(Lang::T("Visibility Check"), &aimP.aim_vis_check);
                ChecksDropdown("Checks", "##aim_checks",
                    &aimP.aim_smoke_check, &aimP.aim_flash_check, &aimP.aim_scoped_only);

                ImGui::Spacing();
                ui::SectionLabel("Overlay");
                ImGui::Checkbox(Lang::T("Aim FOV Circle"), &Config::fov_circle);
                if (Config::fov_circle)
                    ImGui::ColorEdit4("Aim FOV Color", (float*)&Config::fovCircleColor,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

                ImGui::Checkbox(Lang::T("Autofire FOV Circle"), &Config::fov_circle_autofire);
                if (Config::fov_circle_autofire)
                    ImGui::ColorEdit4("AF FOV Color", (float*)&Config::fovCircleColorAf,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

                ImGui::Spacing();
                ui::SectionLabel("Timing");
                ui::SliderFull("Reaction", "##ab_react", &aimP.aim_reaction_delay_ms, 0.f, 500.f, "%.0f ms");
                ui::SliderFull("Target Switch", "##ab_tswitch", &aimP.aim_target_switch_delay_ms, 0.f, 500.f, "%.0f ms");
                ui::SliderFull("First Shot", "##ab_first", &aimP.aim_first_shot_delay_ms, 0.f, 500.f, "%.0f ms");
                ui::EndCard();
            }
 // ?-??-??-??-??-??-??-??-??-??-??-??-? AUTOFIRE ?-??-??-??-??-??-??-??-??-??-??-??-?
            else if (aimSub == 1) {
                ui::BeginCard("##aim_af_l", half, true);
                ui::SectionLabel("Autofire");
                ImGui::Checkbox(Lang::T("Enable"), &Config::autofire);
                KeybindRow(Config::autofire);

                ui::SliderFull("FOV", "##af_fov", &aimP.autofire_fov, 0.f, 90.f, "%.1f");

                ImGui::Checkbox(Lang::T("Silent Aim"), &Config::autofire_silent);

                {
                    static const char* kAfModes[] = { "Hitchance", "Seed Nospread" };
                    if (aimP.autofire_mode < 0 || aimP.autofire_mode >= Config::AF_MODE_COUNT)
                        aimP.autofire_mode = Config::AF_MODE_HITCHANCE;
                    ui::ComboFull("Mode", "##af_mode", &aimP.autofire_mode, kAfModes, Config::AF_MODE_COUNT);
                }

                if (aimP.autofire_mode == Config::AF_MODE_HITCHANCE) {
                    ui::SliderFull("Hitchance", "##af_hc", &aimP.autofire_hitchance, 0.f, 100.f, "%.0f%%");
                } else {
                    ImGui::TextDisabled(Lang::T("Seed mode: HC %% unused"));
                }

                ImGui::Checkbox(Lang::T("Autostop"), &aimP.autofire_autostop);

                ImGui::Checkbox(Lang::T("Autoscope"), &aimP.autofire_autoscope);

                {
                    static const char* kTargetSel[] = {
                        "Crosshair", "Distance", "Best Damage"
                    };
                    if (aimP.autofire_target_select < 0
                        || aimP.autofire_target_select >= Config::AF_TARGET_COUNT)
                        aimP.autofire_target_select = Config::AF_TARGET_CROSSHAIR;
                    ui::ComboFull("Target", "##af_target_sel", &aimP.autofire_target_select,
                        kTargetSel, IM_ARRAYSIZE(kTargetSel));
                }

                ImGui::Checkbox(Lang::T("Focus Target"), &aimP.autofire_focus_target);

                ImGui::Checkbox(Lang::T("Body if Lethal"), &aimP.autofire_body_if_lethal);

                ImGui::Checkbox(Lang::T("Prefer Body"), &aimP.autofire_prefer_body);

                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_af_r", 0, true);
                ui::SectionLabel("Targeting");
                ImGui::Checkbox(Lang::T("Visibility"), &aimP.autofire_vis_check);
                ChecksDropdown("Checks", "##af_checks",
                    &aimP.autofire_smoke_check, &aimP.autofire_flash_check, &aimP.autofire_scoped_only);

 // Hitboxes + multipoint
                {
                    static constexpr int kAfMpHb[] = {
                        Config::HB_HEAD, Config::HB_CHEST, Config::HB_STOMACH, Config::HB_PELVIS
                    };
                    static constexpr int kAfMpCount = 4;
                    static const char* kAfMpNames[] = {
                        "Head", "Chest", "Stomach", "Pelvis"
                    };
                    aimP.autofire_multipoint[Config::HB_NECK] = false;
                    aimP.autofire_multipoint[Config::HB_ARMS] = false;
                    aimP.autofire_multipoint[Config::HB_LEGS] = false;
                    aimP.autofire_multipoint[Config::HB_FEET] = false;

                    HitboxMultiSelect("Hitboxes", "##af_hitboxes",
                        aimP.autofire_hitboxes, Config::HB_COUNT, kHitboxNames, 200,
                        "Which body parts autofire targets.");
 // Clear MP when parent hitbox disabled
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        if (!aimP.autofire_hitboxes[i])
                            aimP.autofire_multipoint[i] = false;

                    {
                        char preview[96];
                        preview[0] = '\0';
                        int nSel = 0;
                        for (int mi = 0; mi < kAfMpCount; ++mi) {
                            const int hb = kAfMpHb[mi];
                            if (!aimP.autofire_multipoint[hb]) continue;
                            if (nSel > 0)
                                strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
                            strncat_s(preview, sizeof(preview), Lang::T(kAfMpNames[mi]), _TRUNCATE);
                            ++nSel;
                        }
                        if (nSel == 0)
                            strcpy_s(preview, Lang::T("None"));
                        ImGui::TextUnformatted(Lang::T("Multipoint"));
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::BeginCombo("##af_multipoint", preview)) {
                            for (int mi = 0; mi < kAfMpCount; ++mi) {
                                const int hb = kAfMpHb[mi];
                                ImGui::PushID(220 + mi);
                                // Disabled while the hitbox itself is off - a
                                // disabled Checkbox can never return true, so
                                // no un-set fallback is needed here.
                                ImGui::BeginDisabled(!aimP.autofire_hitboxes[hb]);
                                ImGui::Checkbox(Lang::T(kAfMpNames[mi]), &aimP.autofire_multipoint[hb]);
                                ImGui::EndDisabled();
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                    }

                    ui::FeatureToggle("Dynamic Multipoint", &aimP.autofire_multipoint_dynamic, "Auto-adjust multipoint by spread and distance.");
                    ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Appearing);
                    if (ImGui::BeginPopupContextItem("##af_mp_pop")) {
                        ui::PopupTitle("Multipoint Scale");
                        ImGui::TextDisabled(Lang::T("0%% = center  |  100%% = full edge"));
                        ImGui::Spacing();
                        int shown = 0;
                        for (int mi = 0; mi < 4; ++mi) {
                            const int hb = kAfMpHb[mi];
                            if (!aimP.autofire_multipoint[hb] || !aimP.autofire_hitboxes[hb])
                                continue;
                            float pct = aimP.autofire_multipoint_scale[hb] * 100.f;
                            ImGui::PushID(300 + mi);
                            if (ui::SliderFull(kAfMpNames[mi], "##mp", &pct, 0.f, 100.f, "%.0f%%"))
                                aimP.autofire_multipoint_scale[hb] = pct / 100.f;
                            ImGui::PopID();
                            ++shown;
                        }
                        if (shown == 0)
                            ImGui::TextDisabled(Lang::T("Enable Multipoint boxes first"));
                        ImGui::EndPopup();
                    }
                }

                ImGui::Spacing();
                ui::SectionLabel("Damage");
                ui::SliderFull("Min Damage", "##af_md", &aimP.autofire_mindamage, 0.f, 120.f, "%.0f");

                ui::FeatureToggle("Autowall", &aimP.autofire_autowall, "Shoot through walls.");
                ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Appearing);
                if (ImGui::BeginPopupContextItem("##af_aw_pop")) {
                    ui::PopupTitle("Autowall Damage");
                    ui::SliderFull("Min Damage (AW)", "##af_md_aw", &aimP.autofire_mindamage_aw, 0.f, 120.f, "%.0f");
                    ImGui::TextDisabled(Lang::T("Shared with Trigger when AW on"));
                    ImGui::EndPopup();
                }
                if (aimP.autofire_autowall) {
                    ImGui::TextDisabled(Lang::T("Autowall bind (global)"));
                    KeybindRow(Config::autowall);
                }

                ImGui::Spacing();
                ui::FeatureToggle("Min Damage Override", &aimP.mindamage_override, "Override min damage when key is active.");
                ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Appearing);
                if (ImGui::BeginPopupContextItem("##af_mdo_pop")) {
                    ui::PopupTitle("Min Damage Override");
                    ui::SliderFull("Override Damage", "##pop_mdo_val", &aimP.mindamage_override_value, 1.f, 120.f, "%.0f");
                    ImGui::EndPopup();
                }
                if (aimP.mindamage_override) {
                    ui::SliderFull("Override Damage", "##af_mdo_val", &aimP.mindamage_override_value, 1.f, 120.f, "%.0f");
                    KeybindRow(Config::mindamage_override);
                }
                ui::EndCard();
            }
 // ?-??-??-??-??-??-??-??-??-??-??-??-? TRIGGER ?-??-??-??-??-??-??-??-??-??-??-??-?
            else if (aimSub == 2) {
                ui::BeginCard("##aim_tr_l", half, true);
                ui::SectionLabel("Triggerbot");
                ImGui::Checkbox(Lang::T("Enable"), &Config::triggerbot);
                KeybindRow(Config::triggerbot);

                ImGui::Checkbox(Lang::T("Magnet"), &aimP.trigger_magnet);
                if (aimP.trigger_magnet) {
                    ui::SliderFull("Magnet FOV", "##tr_mag_fov",
                        &aimP.trigger_magnet_fov, 0.5f, 15.f, "%.1f");
                    ui::SliderFull("Magnet Smooth", "##tr_mag_sm",
                        &aimP.trigger_magnet_smooth, 0.f, 50.f, "%.0f");
                    ui::SliderFull("Deadzone", "##tr_mag_dz",
                        &aimP.trigger_magnet_deadzone, 0.f, 0.8f, "%.2f");
                    ImGui::Checkbox(Lang::T("Silent Magnet"), &aimP.trigger_magnet_silent);
                    ImGui::Checkbox(Lang::T("Head Priority"), &aimP.trigger_magnet_head_prio);
                    ImGui::Checkbox(Lang::T("Magnet FOV Circle"), &Config::fov_circle_magnet);
                    if (Config::fov_circle_magnet)
                        ImGui::ColorEdit4("Magnet FOV Color", (float*)&Config::fovCircleColorMagnet,
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::TextDisabled(Lang::T("Vis always | AW = Trigger Autowall + bind"));
                    ImGui::TextDisabled(Lang::T("Sticky lock | punch RCS | multipoint aim"));
                }

                ui::SliderFull("Delay", "##tr_delay", &aimP.trigger_delay_ms, 0.f, 500.f, "%.0f ms");

                {
                    static const char* kTrModes[] = { "Hitchance", "Seed Nospread" };
                    if (aimP.trigger_mode < 0 || aimP.trigger_mode >= Config::TR_MODE_COUNT)
                        aimP.trigger_mode = Config::TR_MODE_HITCHANCE;
                    ui::ComboFull("Mode", "##tr_mode", &aimP.trigger_mode, kTrModes, Config::TR_MODE_COUNT);
                }

                if (aimP.trigger_mode == Config::TR_MODE_HITCHANCE) {
                    ui::SliderFull("Hitchance", "##tr_hc", &aimP.trigger_hitchance, 0.f, 100.f, "%.0f%%");
                } else {
                    ImGui::TextDisabled(Lang::T("Seed: HC %% unused"));
                }

                ImGui::Checkbox(Lang::T("Autostop"), &aimP.trigger_autostop);

                ImGui::Checkbox(Lang::T("Autowall"), &aimP.trigger_autowall);
                if (aimP.trigger_autowall) {
                    ImGui::TextDisabled(Lang::T("Autowall bind (global)"));
                    KeybindRow(Config::autowall);
                }

                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_tr_r", 0, true);
                ui::SectionLabel("Filters");
                ChecksDropdown("Checks", "##tr_checks",
                    &aimP.trigger_smoke_check, &aimP.trigger_flash_check, &aimP.trigger_scoped_only);

                HitboxMultiSelect("Hitboxes", "##tr_hitboxes",
                    aimP.trigger_hitboxes, Config::HB_COUNT, kHitboxNames, 100,
                    "Which body parts trigger can fire on.");

                if (aimP.trigger_magnet) {
                    ImGui::Spacing();
                    HitboxMultiSelect("Magnet Hitboxes", "##tr_mag_hb",
                        aimP.trigger_magnet_hitboxes, Config::HB_COUNT, kHitboxNames, 100,
                        "Empty = same as fire hitboxes. Else magnet only these.");
                }

                ImGui::Spacing();
                ui::SectionLabel("Damage");
                ui::SliderFull("Min Damage", "##tr_md", &aimP.trigger_mindamage, 0.f, 120.f, "%.0f");
                if (aimP.trigger_autowall) {
                    ui::SliderFull("Min Damage (AW)", "##tr_md_aw", &aimP.trigger_mindamage_aw, 0.f, 120.f, "%.0f");
                }
                ImGui::Spacing();
                ui::FeatureToggle("Min Damage Override##tr", &aimP.mindamage_override, "Override min damage when key is active.");
                ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Appearing);
                if (ImGui::BeginPopupContextItem("##tr_mdo_pop")) {
                    ui::PopupTitle("Min Damage Override");
                    ui::SliderFull("Override Damage", "##tr_pop_mdo_val", &aimP.mindamage_override_value, 1.f, 120.f, "%.0f");
                    ImGui::EndPopup();
                }
                if (aimP.mindamage_override) {
                    ui::SliderFull("Override Damage", "##tr_mdo_val", &aimP.mindamage_override_value, 1.f, 120.f, "%.0f");
                    KeybindRow(Config::mindamage_override);
                }
                ui::EndCard();
            }
 // ?-??-??-??-??-??-??-??-??-??-??-??-? EXTRA (RCS / team) ?-??-??-??-??-??-??-??-??-??-??-??-?
            else if (aimSub == 3) {
                ui::BeginCard("##aim_ex_l", 0, true);
                ui::SectionLabel("Recoil");
                ImGui::Checkbox(Lang::T("Aimbot RCS"), &aimP.rcs);
                ImGui::Checkbox(Lang::T("Standalone RCS"), &aimP.rcs_standalone);
                ui::SliderFull("Scale X (Yaw)", "##rcs_x", &aimP.rcs_scale_x, 0.f, 1.f, "%.2f");
                ui::SliderFull("Scale Y (Pitch)", "##rcs_y", &aimP.rcs_scale_y, 0.f, 1.f, "%.2f");
                ui::SliderFull("Smooth", "##rcs_smooth", &aimP.rcs_smooth, 0.f, 20.f, "%.0f");
                ui::EndCard();
            }
            else {
                ui::BeginCard("##aim_aa_l", half, true);
                ui::SectionLabel("Anti-Aim");
                ui::FeatureToggle("Enable", &Config::anti_aim,
                    "\xE6\x8E\xA7\xE5\x88\xB6\xE8\xA7\x92\xE8\x89\xB2\xE5\x8F\x91\xE9\x80\x81\xE4\xBC\xAA\xE9\x80\xA0\xE7\x9A\x84\xE4\xBF\xAF\x4E\xE5\x92\x8C\xE5\x81\x8F\xE8\x88\xAA\xE8\xA7\x92\xE5\xBA\xA6\xE3\x80\x82");

                static const char* kPitchModes[] = {
                    "Off", "Up", "Down", "Custom", "Jitter",
                    "Random Jitter", "Switch Jitter", "Third Way Jitter"
                };
                if (Config::anti_aim_pitch_mode < 0 ||
                    Config::anti_aim_pitch_mode >= Config::AA_PITCH_COUNT)
                    Config::anti_aim_pitch_mode = Config::AA_PITCH_OFF;
                ui::ComboFull("Pitch Mode", "##aa_pitch_mode",
                    &Config::anti_aim_pitch_mode, kPitchModes, Config::AA_PITCH_COUNT);

                if (Config::anti_aim_pitch_mode == Config::AA_PITCH_CUSTOM)
                    ui::SliderFull("Pitch Angle", "##aa_pitch_angle",
                        &Config::anti_aim_pitch_angle, -89.f, 89.f, "%.0f");
                if (Config::anti_aim_pitch_mode >= Config::AA_PITCH_JITTER) {
                    ui::SliderFull("Pitch Jitter Min", "##aa_pitch_jitter_min",
                        &Config::anti_aim_pitch_jitter_min, -89.f, 89.f, "%.0f");
                    ui::SliderFull("Pitch Jitter Max", "##aa_pitch_jitter_max",
                        &Config::anti_aim_pitch_jitter_max, -89.f, 89.f, "%.0f");
                    if (Config::anti_aim_pitch_jitter_min > Config::anti_aim_pitch_jitter_max)
                        std::swap(Config::anti_aim_pitch_jitter_min,
                            Config::anti_aim_pitch_jitter_max);
                }
                ui::Checkbox("Hide Shots", &Config::anti_aim_hideshots);
                ui::Checkbox("Avoid Backstab", &Config::anti_aim_avoid_backstab);
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_aa_r", 0, true);
                ui::SectionLabel("Yaw");
                static const char* kYawModes[] = { "Off", "Static" };
                if (Config::anti_aim_yaw_mode < 0 ||
                    Config::anti_aim_yaw_mode >= Config::AA_YAW_COUNT)
                    Config::anti_aim_yaw_mode = Config::AA_YAW_OFF;
                ui::ComboFull("Yaw Mode", "##aa_yaw_mode",
                    &Config::anti_aim_yaw_mode, kYawModes, Config::AA_YAW_COUNT);
                if (Config::anti_aim_yaw_mode == Config::AA_YAW_STATIC) {
                    ui::SliderFull("Yaw Angle", "##aa_yaw_angle",
                        &Config::anti_aim_yaw_angle, -180.f, 180.f, "%.0f");
                    ui::Checkbox("At Target", &Config::anti_aim_yaw_at_target);
                    ui::Checkbox("Yaw Adjust", &Config::anti_aim_yaw_adjust);
                }
                ui::SectionLabel("Manual Direction");
                ImGui::TextDisabled(Lang::T("\xE6\x96\xB9\xE5\x90\x91\xE9\x94\xAE\xE5\x8F\xAF\xE5\x9C\xA8\xE9\x85\x8D\xE7\xBD\xAE\xE6\x96\x87\xE4\xBB\xB6\xE4\xB8\xAD\xE8\xAE\xBE\xE7\xBD\xAE\xE8\x99\x9A\xE6\x8B\x9F\xE9\x94\xAE\xE5\x80\xBC\xE3\x80\x82"));
                ui::SliderInt("Manual Left Key", "##aa_left_key",
                    &Config::anti_aim_manual_key_left, 0, 255, "%d");
                ui::SliderInt("Manual Right Key", "##aa_right_key",
                    &Config::anti_aim_manual_key_right, 0, 255, "%d");
                ui::SliderInt("Manual Back Key", "##aa_back_key",
                    &Config::anti_aim_manual_key_back, 0, 255, "%d");
                ui::EndCard();
            }

 // Profile edits ??' live only when UI group == held weapon group.
 // Editing another group never stomps live Config (FOV circles, etc.).
            PushLiveIfEditingActive();
        }
        break;

        case 1: // Visuals
        {
 // 0 Players | 1 World (atmosphere + item/nade ESP) | 2 Nade Pred | 3 View | 4 Removals
            static int visSub = 0;
            {
                static const char* kVisTabs[] = {
                    "Players", "World", "Nade Pred", "View", "Removals"
                };
                ui::SubNav(kVisTabs, 5, &visSub);
            }

            const ImGuiColorEditFlags colFlags =
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

            const ImGuiColorEditFlags chamCol =
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;
            // chams::ChamIds order - visible + ignorez lists
            static const int visIds[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
            static const char* visMats[] = {
                "Liquid", "Metallic", "Matte", "Flat", "Bloom", "Outlines", "Glow",
                "Electric", "Distortion", "Hologram", "Pearl"
            };
            static const int xqzIds[] = { 11, 12, 13, 14, 15, 16, 17, 18 };
            static const char* xqzMats[] = {
                "Liquid IZ", "Matte IZ", "Flat IZ", "Bloom IZ", "Outlines IZ",
                "Glow IZ", "Distortion IZ", "Hologram IZ"
            };
            auto chamCombo = [](const char* label, const char* id, int* matId,
                const int* ids, const char* const* names, int n) {
                int sel = 0;
                for (int i = 0; i < n; ++i)
                    if (ids[i] == *matId) { sel = i; break; }
                if (ui::ComboFull(label, id, &sel, names, n))
                    *matId = ids[sel];
            };

            if (visSub == 0) {
                ui::BeginCard("##vis_left", half, true);
                ui::SectionLabel("Player ESP");
                ImGui::TextDisabled(Lang::T("Right-click a feature for settings"));

                ui::FeatureToggle("Box", &Config::esp, "Draw boxes around players.");
                if (ImGui::BeginPopupContextItem("##esp_box_pop")) {
                    ui::PopupTitle("Box Settings");
                    ImGui::ColorEdit4("Visible##esp_box_vis", (float*)&Config::espColor, colFlags);
                    ImGui::ColorEdit4("Invisible##esp_box_invis", (float*)&Config::espColorInvisible, colFlags);
                    ui::SliderFull("Thickness", "##esp_th", &Config::espThickness, 1.0f, 5.0f, "%.1f");
                    ui::SliderFull("Width Scale", "##esp_bw", &Config::esp_box_width, 0.28f, 0.70f, "%.2f");
                    const char* styles[] = { "Full", "Corner" };
                    ui::ComboFull("Style", "##esp_style", &Config::esp_box_style, styles, IM_ARRAYSIZE(styles));
                    ImGui::Checkbox(Lang::T("Fill"), &Config::espFill);
                    if (Config::espFill)
                        ui::SliderFull("Fill Opacity", "##esp_fo", &Config::espFillOpacity, 0.0f, 1.0f, "%.2f");
                    ImGui::EndPopup();
                }

                static const char* espPosItems[] = { "Top", "Bottom", "Left", "Right" };

                ui::FeatureToggle("Health Bar", &Config::showHealth, "Show health bar next to players.");
                if (ImGui::BeginPopupContextItem("##esp_hp_pop")) {
                    ui::PopupTitle("Health Bar Settings");
                    ImGui::Checkbox(Lang::T("Auto Color (HP)"), &Config::esp_health_auto);
                    if (!Config::esp_health_auto)
                        ImGui::ColorEdit4("Color##esp_hp", (float*)&Config::esp_health_color, colFlags);
                    ui::SliderFull("Bar Width", "##esp_hp_bw", &Config::esp_bar_width, 2.0f, 8.0f, "%.0f");
                    ui::ComboFull("Position", "##esp_pos_health", &Config::esp_pos_health, espPosItems, IM_ARRAYSIZE(espPosItems));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Armor Bar", &Config::showArmor, "Show armor bar next to players.");
                if (ImGui::BeginPopupContextItem("##esp_armor_pop")) {
                    ui::PopupTitle("Armor Bar Settings");
                    ImGui::ColorEdit4("Color##esp_armor", (float*)&Config::esp_armor_color, colFlags);
                    ui::SliderFull("Bar Width", "##esp_ar_bw", &Config::esp_bar_width, 2.0f, 8.0f, "%.0f");
                    ui::ComboFull("Position", "##esp_pos_armor", &Config::esp_pos_armor, espPosItems, IM_ARRAYSIZE(espPosItems));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Name Tags", &Config::showNameTags, "Show player names.");
                if (ImGui::BeginPopupContextItem("##esp_name_pop")) {
                    ui::PopupTitle("Name Settings");
                    ImGui::ColorEdit4("Color##esp_name", (float*)&Config::esp_name_color, colFlags);
                    ImGui::Checkbox(Lang::T("Avatar"), &Config::esp_name_avatar);
                    ui::ComboFull("Position", "##esp_pos_name", &Config::esp_pos_name, espPosItems, IM_ARRAYSIZE(espPosItems));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Weapon ESP", &Config::showWeapon, "Show weapon name on players.");
                if (ImGui::BeginPopupContextItem("##esp_wep_pop")) {
                    ui::PopupTitle("Weapon Settings");
                    ImGui::ColorEdit4("Color##esp_wep", (float*)&Config::esp_weapon_color, colFlags);
                    ui::ComboFull("Position", "##esp_pos_weapon", &Config::esp_pos_weapon, espPosItems, IM_ARRAYSIZE(espPosItems));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Weapon Icons", &Config::showWeaponIcon, "Show weapon icon on players.");
                if (ImGui::BeginPopupContextItem("##esp_wep_icon_pop")) {
                    ui::PopupTitle("Weapon Icon Settings");
                    ImGui::ColorEdit4("Color##esp_wep_icon", (float*)&Config::esp_weapon_icon_color, colFlags);
                    ui::ComboFull("Position", "##esp_pos_weapon_icon", &Config::esp_pos_weapon_icon, espPosItems, IM_ARRAYSIZE(espPosItems));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Distance ESP", &Config::showDistance, "Show distance to players.");
                if (ImGui::BeginPopupContextItem("##esp_dist_pop")) {
                    ui::PopupTitle("Distance Settings");
                    ImGui::ColorEdit4("Color##esp_dist", (float*)&Config::esp_distance_color, colFlags);
                    ui::ComboFull("Position", "##esp_pos_distance", &Config::esp_pos_distance, espPosItems, IM_ARRAYSIZE(espPosItems));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Skeleton", &Config::esp_skeleton, "Draw player bones.");
                if (ImGui::BeginPopupContextItem("##esp_skel_pop")) {
                    ui::PopupTitle("Skeleton Settings");
                    ImGui::Checkbox(Lang::T("Head Circle"), &Config::esp_skeleton_head);
                    ImGui::ColorEdit4("Skel Visible##skel_vis", (float*)&Config::esp_skeleton_color, colFlags);
                    ImGui::ColorEdit4("Skel Invisible##skel_invis", (float*)&Config::esp_skeleton_color_invisible, colFlags);
                    ui::SliderFull("Thickness", "##esp_sk_th", &Config::esp_skeleton_thickness, 1.0f, 4.0f, "%.1f");
                    ImGui::EndPopup();
                }


                ui::FeatureToggle("Glow", &Config::glow, "Outline glow on players.");
                if (ImGui::BeginPopupContextItem("##esp_glow_pop")) {
                    ui::PopupTitle("Player Glow Settings");
                    ImGui::Checkbox(Lang::T("Team"), &Config::glow_team);
                    ImGui::Checkbox(Lang::T("Enemy"), &Config::glow_enemy);
                    ImGui::Checkbox(Lang::T("Only Visible"), &Config::glow_only_visible);
                    ImGui::ColorEdit4("Glow Visible##glow_vis", (float*)&Config::glow_color, colFlags);
                    ImGui::ColorEdit4("Glow Invisible##glow_invis", (float*)&Config::glow_color_invis, colFlags);
                    ImGui::TextDisabled(Lang::T("RGB + Alpha own glow only"));
                    ImGui::EndPopup();
                }

                ImGui::Checkbox(Lang::T("Visibility Check##esp"), &Config::esp_vis_check);
                if (ImGui::Checkbox(Lang::T("Team Check##esp"), &Config::teamCheck))
                    Config::team_check = Config::teamCheck;

                ui::SectionLabel("Flags");
                ImGui::TextDisabled(Lang::T("Active state labels (right of box)"));
                ImGui::Checkbox(Lang::T("Flashed"), &Config::flag_flashed);
                ImGui::SameLine(0, 12.f);
                ImGui::Checkbox(Lang::T("Bomb"), &Config::flag_bomb);
                ImGui::Checkbox(Lang::T("Scoped"), &Config::flag_scoped);
                ImGui::SameLine(0, 12.f);
                ImGui::Checkbox(Lang::T("Reloading"), &Config::flag_reloading);
                ImGui::Checkbox(Lang::T("Defusing"), &Config::flag_defusing);
                ImGui::SameLine(0, 12.f);
                ImGui::Checkbox(Lang::T("Money"), &Config::flag_money);
                ImGui::Checkbox(Lang::T("Kit"), &Config::flag_kit);
                ImGui::SameLine(0, 12.f);
                ImGui::Checkbox(Lang::T("Helmet"), &Config::flag_helmet);
                ImGui::Checkbox(Lang::T("Nades (H/F/S/M/D)"), &Config::flag_nades);

                ui::SectionLabel("Extra ESP");
                ui::FeatureToggle("Rank ESP", &Config::esp_rank, "Show competitive rank.");
                if (ImGui::BeginPopupContextItem("##esp_rank_pop")) {
                    ui::PopupTitle("Rank");
                    ImGui::ColorEdit4("Color##esp_rank", (float*)&Config::esp_rank_color, colFlags);
                    ImGui::TextDisabled(Lang::T("Competitive rank label"));
                    ImGui::EndPopup();
                }
                ui::FeatureToggle("3D Box", &Config::esp_3d_box, "3D box around players.");
                if (ImGui::BeginPopupContextItem("##esp_3d_pop")) {
                    ui::PopupTitle("3D Box");
                    ImGui::ColorEdit4("Color##esp_3d", (float*)&Config::esp_3d_box_color, colFlags);
                    ImGui::TextDisabled(Lang::T("3D box around player"));
                    ImGui::EndPopup();
                }
                ui::FeatureToggle("Offscreen Arrows", &Config::esp_oof, "Arrows for enemies off screen.");
                if (ImGui::BeginPopupContextItem("##esp_oof_pop")) {
                    ui::PopupTitle("OOF Arrows");
                    ImGui::ColorEdit4("Color##esp_oof", (float*)&Config::esp_oof_color, colFlags);
                    ui::SliderFull("Radius", "##esp_oof_r", &Config::esp_oof_radius, 80.f, 420.f, "%.0f");
                    ui::SliderFull("Size", "##esp_oof_s", &Config::esp_oof_size, 8.f, 28.f, "%.0f");
                    ImGui::TextDisabled(Lang::T("Distance label | pulse | low-HP pip"));
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Sound ESP", &Config::sound_esp, "Ring when enemies move nearby.");
                if (ImGui::BeginPopupContextItem("##esp_sound_pop")) {
                    ui::PopupTitle("Sound ESP");
                    ui::SliderFull("Ring Size", "##snd_rs", &Config::sound_esp_ring_size, 0.5f, 3.f, "%.2fx");
                    ui::SliderFull("Life", "##snd_life", &Config::sound_esp_duration, 0.4f, 4.f, "%.1fs");
                    ImGui::ColorEdit4("Color##sound_esp", (float*)&Config::sound_esp_color, colFlags);
                    ImGui::TextDisabled(Lang::T("Shows when ESP would show that enemy"));
                    ImGui::EndPopup();
                }
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##vis_right", 0, true);
                ui::SectionLabel("Chams");

                ImGui::Checkbox(Lang::T("Enemy Chams"), &Config::enemyChams);
                if (Config::enemyChams) {
                    chamCombo("Visible Material", "##chams_vis_mat", &Config::chamsMaterial, visIds, visMats, IM_ARRAYSIZE(visMats));
                    ImGui::ColorEdit4("Chams Visible##chams_vis", (float*)&Config::colVisualChams, chamCol);
                }

                ImGui::Checkbox(Lang::T("Chams XQZ"), &Config::enemyChamsInvisible);
                if (Config::enemyChamsInvisible) {
                    chamCombo("XQZ Material", "##chams_xqz_mat", &Config::chamsMaterialXQZ, xqzIds, xqzMats, IM_ARRAYSIZE(xqzMats));
                    ImGui::ColorEdit4("Chams XQZ##chams_xqz", (float*)&Config::colVisualChamsIgnoreZ, chamCol);
                }

                ui::SectionLabel("Local");
                ImGui::Checkbox(Lang::T("Local Body Chams"), &Config::localChams);
                if (Config::localChams) {
                    chamCombo("Local Material", "##chams_local_mat", &Config::localChamsMaterial, visIds, visMats, IM_ARRAYSIZE(visMats));
                    ImGui::ColorEdit4("Local Color", (float*)&Config::colLocalChams, chamCol);
                }
                ImGui::Checkbox(Lang::T("Ragdoll Chams"), &Config::ragdollChams);
                if (Config::ragdollChams) {
                    chamCombo("Ragdoll Material", "##chams_rag_mat", &Config::ragdollChamsMaterial, visIds, visMats, IM_ARRAYSIZE(visMats));
                    ImGui::ColorEdit4("Ragdoll Color", (float*)&Config::colRagdollChams, chamCol);
                }
                ImGui::Checkbox(Lang::T("Hand Chams"), &Config::armChams);
                if (Config::armChams) {
                    chamCombo("Hand Material", "##chams_hand_mat", &Config::armChamsMaterial, visIds, visMats, IM_ARRAYSIZE(visMats));
                    ImGui::ColorEdit4("Hand Color", (float*)&Config::colArmChams, chamCol);
                }
                ImGui::Checkbox(Lang::T("Weapon Chams"), &Config::viewmodelChams);
                if (Config::viewmodelChams) {
                    chamCombo("Weapon Material", "##chams_wep_mat", &Config::viewmodelChamsMaterial, visIds, visMats, IM_ARRAYSIZE(visMats));
                    ImGui::ColorEdit4("Weapon Color", (float*)&Config::colViewmodelChams, chamCol);
                }

                ui::EndCard();
            }
            else if (visSub == 1) {
 // ?"??"? World: 2 columns, all cards AutoResizeY (no bottom strip scroll) ?"??"?
                const ImGuiColorEditFlags worldCol =
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;

 // LEFT column
                ui::BeginCard("##world_left", half, true);
                ui::SectionLabel("Atmosphere");
                ImGui::TextDisabled(Lang::T("Map lighting & environment"));

                ImGui::Checkbox(Lang::T("Night Mode"), &Config::Night);
                if (Config::Night) {
                    ui::SliderFull("Darkness", "##night_dark", &Config::night_exposure, 0.f, 1.f, "%.2f");
                }

                ImGui::Checkbox(Lang::T("Skybox Color"), &Config::skybox);
                if (Config::skybox)
                    ImGui::ColorEdit4("Sky Color", (float*)&Config::skybox_color, worldCol);

                ImGui::Checkbox(Lang::T("Lighting Color"), &Config::lighting);
                if (Config::lighting)
                    ImGui::ColorEdit4("Light Color", (float*)&Config::lighting_color, worldCol);

                ImGui::Checkbox(Lang::T("Map Color"), &Config::map_color);
                if (Config::map_color) {
                    ImGui::ColorEdit4("World Color", (float*)&Config::map_color_value, worldCol);
                }

ImGui::Spacing();
                ui::SectionLabel("Items");
                ImGui::TextDisabled(Lang::T("Right-click for color"));

                ui::FeatureToggle("Dropped Weapons", &Config::world_esp_weapons, "Show guns on the ground.");
                if (ImGui::BeginPopupContextItem("##wesp_wep_pop")) {
                    ui::PopupTitle("Dropped Weapons");
                    ImGui::ColorEdit4("ESP Color##wesp_wep", (float*)&Config::world_esp_weapon_color, colFlags);
                    ImGui::Checkbox(Lang::T("Weapon Icon"), &Config::world_esp_weapon_icon);
                    ImGui::Checkbox(Lang::T("Weapon Distance"), &Config::world_esp_weapon_distance);
                    if (Config::world_esp_weapon_distance)
                        ImGui::ColorEdit4("Distance Color##wesp_wep_dist", (float*)&Config::world_esp_weapon_distance_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Planted Bomb", &Config::world_esp_bomb, "Bomb site, timer, and defuse info.");
                if (ImGui::BeginPopupContextItem("##wesp_bomb_pop")) {
                    ui::PopupTitle("Bomb ESP");
                    ImGui::ColorEdit4("Color##wesp_bomb", (float*)&Config::world_esp_bomb_color, colFlags);
                    ImGui::Checkbox(Lang::T("Bomb Time"), &Config::world_esp_bomb_timer);
                    ImGui::EndPopup();
                }

                ImGui::Spacing();
                ui::SectionLabel("Item Chams");
                ImGui::TextDisabled(Lang::T("Dropped weapons and utility"));
                ImGui::Checkbox(Lang::T("Item Chams"), &Config::itemChams);
                if (Config::itemChams || Config::itemChamsInvisible) {
                    ImGui::Checkbox(Lang::T("Pistol##chams_item"), &Config::itemChamsPistol);
                    ImGui::SameLine(0, 12.f);
                    ImGui::Checkbox(Lang::T("SMG##chams_item"), &Config::itemChamsSmg);
                    ImGui::Checkbox(Lang::T("Rifle##chams_item"), &Config::itemChamsRifle);
                    ImGui::SameLine(0, 12.f);
                    ImGui::Checkbox(Lang::T("Shotgun##chams_item"), &Config::itemChamsShotgun);
                    ImGui::Checkbox(Lang::T("Sniper##chams_item"), &Config::itemChamsSniper);
                    ImGui::SameLine(0, 12.f);
                    ImGui::Checkbox(Lang::T("Utility##chams_item"), &Config::itemChamsUtility);
                    chamCombo("Item Material", "##chams_item_mat", &Config::itemChamsMaterial, visIds, visMats, IM_ARRAYSIZE(visMats));
                    ImGui::ColorEdit4("Item Color##chams_item", (float*)&Config::colItemChams, chamCol);
                    ImGui::Checkbox(Lang::T("Item XQZ"), &Config::itemChamsInvisible);
                    if (Config::itemChamsInvisible) {
                        chamCombo("Item XQZ Material", "##chams_item_xqz_mat", &Config::itemChamsMaterialXQZ, xqzIds, xqzMats, IM_ARRAYSIZE(xqzMats));
                        ImGui::ColorEdit4("Item XQZ Color##chams_item_xqz", (float*)&Config::colItemChamsIgnoreZ, chamCol);
                    }
                }

                ui::EndCard();

                ImGui::SameLine(0, gap);

 // RIGHT column
                ui::BeginCard("##world_right", 0, true);
                ui::SectionLabel("Weather & Fog");
                ImGui::TextDisabled(Lang::T("Particles + gradient fog"));

                ImGui::Checkbox(Lang::T("Weather"), &Config::weather);
                if (Config::weather) {
                    const char* weatherModes[] = {
                        "Snow", "Stars", "Ash", "Rain"
                    };
                    int modeIdx = Config::weather_mode - 1;
                    if (modeIdx < 0) modeIdx = 0;
                    if (modeIdx > 3) modeIdx = 3;
                    if (ui::ComboFull("Mode", "##weather_mode", &modeIdx, weatherModes, IM_ARRAYSIZE(weatherModes)))
                        Config::weather_mode = modeIdx + 1;
                    ui::SliderFull("Intensity", "##weather_int", &Config::weather_intensity, 0.f, 1.f, "%.2f");
                }

                ImGui::Spacing();
                ImGui::Checkbox(Lang::T("Custom Fog"), &Config::custom_fog);
                if (Config::custom_fog) {
                    ImGui::ColorEdit4("Fog Color", (float*)&Config::custom_fog_color, worldCol);
                    ui::SliderFull("Fog Start", "##fog_start", &Config::custom_fog_start, 0.f, 4096.f, "%.0f");
                    ui::SliderFull("Fog End", "##fog_end", &Config::custom_fog_end, 0.f, 4096.f, "%.0f");
                    ui::SliderFull("Fog Falloff", "##fog_fall", &Config::custom_fog_falloff, 0.1f, 8.f, "%.2f");
                }

                ImGui::Spacing();
                ui::SectionLabel("FX Tint");
                ImGui::TextDisabled(Lang::T("Particle / volume colors"));

                ImGui::Checkbox(Lang::T("Smoke Color"), &Config::smoke_color);
                if (Config::smoke_color && !Config::remove_smoke) {
                    ImGui::ColorEdit4("Smoke Tint", (float*)&Config::smoke_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);
                }

                ImGui::Checkbox(Lang::T("Fire Color"), &Config::fire_color);
                if (Config::fire_color) {
                    ImGui::ColorEdit4("Fire Tint", (float*)&Config::fire_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }

                ImGui::Checkbox(Lang::T("Inferno Color"), &Config::inferno_color);
                if (Config::inferno_color) {
                    ImGui::ColorEdit4("Inferno Tint", (float*)&Config::inferno_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }

                ImGui::Checkbox(Lang::T("Explosion Color"), &Config::explosion_color);
                if (Config::explosion_color) {
                    ImGui::ColorEdit4("Explosion Tint", (float*)&Config::explosion_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);
                }

                ui::EndCard();
            }
            else if (visSub == 2) {
                // Grenade Lineup Helper
                ui::BeginCard("##nade_left", half, true);
                ui::SectionLabel("Grenade Prediction");
                ui::FeatureToggle("Enable Prediction", &Config::nadepred_enable, "Live trajectory preview while holding a grenade.");
                ImGui::BeginDisabled(!Config::nadepred_enable);
                ImGui::Checkbox(Lang::T("Show Bounce Dots"), &Config::nadepred_show_bounces);
                ImGui::Checkbox(Lang::T("Show In-Air Trajectory"), &Config::nadepred_in_air);
                ImGui::BeginDisabled(!Config::nadepred_in_air);
                ImGui::Checkbox(Lang::T("Utility Name & Distance"), &Config::nadepred_air_labels);
                ImGui::EndDisabled();
                ImGui::ColorEdit4("Trajectory Color", (float*)&Config::nadepred_color, colFlags);
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ui::SectionLabel("Grenade Helper");
                ui::FeatureToggle("Enable Grenade Helper", &Config::grenade_helper, "Save and show grenade lineup spots.");
                ImGui::BeginDisabled(!Config::grenade_helper);
                ImGui::Checkbox(Lang::T("Only Matching Nade"), &Config::grenade_helper_only_held);
                ui::SliderFull("Stand Draw Dist", "##nl_stand", &Config::grenade_helper_stand_dist, 50.f, 1500.f, "%.0f");
                ui::SliderFull("Select Dist", "##nl_select", &Config::grenade_helper_select_dist, 50.f, 2000.f, "%.0f");
                ui::SliderFull("Aim Marker Dist", "##nl_aim", &Config::grenade_helper_aim_dist, 5.f, 80.f, "%.0f");
                ImGui::ColorEdit4("Stand Color", (float*)&Config::grenade_helper_color, colFlags);
                ImGui::ColorEdit4("Aim Color", (float*)&Config::grenade_helper_aim_color, colFlags);
                ImGui::EndDisabled();
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##nade_right", 0, true);
                ui::SectionLabel("Capture & Management");
                ImGui::BeginDisabled(!Config::grenade_helper);
                ImGui::TextUnformatted(Lang::T("Name"));
                ImGui::SetNextItemWidth(-1.f);
                ImGui::InputText("##lineup_name", Config::grenade_helper_capture_name, sizeof(Config::grenade_helper_capture_name));
                // Throw style is auto-detected at capture - show the LIVE style
                // while armed (bind jumpthrows included), "auto" otherwise.
                ImGui::TextUnformatted(Lang::T("Throw"));
                if (GrenadeHelper::IsCapturing()) {
                    const char* live = GrenadeHelper::ThrowName(GrenadeHelper::CurrentDetected());
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent());
                    ImGui::TextUnformatted(live ? live : Lang::T("Stand"));
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                    ImGui::TextUnformatted(Lang::T("auto (detected at throw)"));
                    ImGui::PopStyleColor();
                }
                const char* kindItems[] = { "Any / Held", "HE", "Flash", "Smoke", "Molly", "Decoy" };
                ui::ComboFull("Kind", "##lineup_kind", &Config::grenade_helper_capture_kind, kindItems, 6);
                KeybindStack(Config::grenade_helper_capture);
                if (ImGui::Button(GrenadeHelper::IsCapturing() ? Lang::T("Cancel Capture") : Lang::T("Arm Capture"), ImVec2(-1, 0))) {
                    if (GrenadeHelper::IsCapturing())
                        GrenadeHelper::CancelCapture();
                    else {
                        GrenadeHelper::ArmCapture(
                            Config::grenade_helper_capture_name,
                            static_cast<GrenadeHelper::NadeKind>(std::clamp(Config::grenade_helper_capture_kind, 0, 5)));
                    }
                }
                ImGui::TextDisabled(Lang::T("Map: %s  (%d on this map / %d total)"),
                    GrenadeHelper::CurrentMap()[0] ? GrenadeHelper::CurrentMap() : "(none)",
                    GrenadeHelper::CountCurrentMap(),
                    static_cast<int>(GrenadeHelper::All().size()));
                ImGui::TextDisabled(Lang::T("Save: Documents\\Games8Th\\GrenadeHelpers\\"));
                if (!GrenadeHelper::CurrentMap()[0]) {
                    ImGui::TextDisabled(Lang::T("Join a map to list its lineups."));
                } else {
                    static int s_renameIdx = -1;
                    static char s_renameBuf[64] = {};
                    static int s_editThrow = 0;

                    if (ImGui::BeginChild("##lineup_list", ImVec2(0, 160), true)) {
                        int shown = 0;
                        auto& lineups = GrenadeHelper::AllMut();
                        for (int i = static_cast<int>(lineups.size()) - 1; i >= 0; --i) {
                            auto& L = lineups[i];
                            if (!GrenadeHelper::IsCurrentMap(L))
                                continue;
                            ImGui::PushID(i);

                            bool en = L.enabled;
                            if (ImGui::Checkbox(Lang::T("##en"), &en)) {
                                GrenadeHelper::SetEnabledAt(i, en);
                            }
                            ImGui::SameLine();

                            if (s_renameIdx == i) {
                                // Full edit: name + throw type (auto-detect can
                                // mislabel - override by typing/selecting here)
                                ImGui::SetNextItemWidth(-1.f);
                                ImGui::InputText("##ren", s_renameBuf, sizeof(s_renameBuf),
                                    ImGuiInputTextFlags_EnterReturnsTrue);
                                const int throwCount = static_cast<int>(GrenadeHelper::ThrowType::Count);
                                const char* throwItems[15] = {};
                                for (int t = 0; t < throwCount; ++t)
                                    throwItems[t] = GrenadeHelper::ThrowName(static_cast<GrenadeHelper::ThrowType>(t));
                                ImGui::SetNextItemWidth(-1.f);
                                ui::ComboFull("##ren_throw", "##ren_throw_sel", &s_editThrow, throwItems, throwCount);
                                if (ImGui::SmallButton("OK")) {
                                    if (s_renameBuf[0])
                                        GrenadeHelper::RenameAt(i, s_renameBuf);
                                    GrenadeHelper::SetThrowAt(i, static_cast<GrenadeHelper::ThrowType>(s_editThrow));
                                    s_renameIdx = -1;
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Cancel")) {
                                    s_renameIdx = -1;
                                }
                            } else {
                                if (!L.enabled)
                                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());

                                ImGui::Text(Lang::T("%s | %s | %s"),
                                    L.name.c_str(),
                                    GrenadeHelper::KindName(L.kind), GrenadeHelper::ThrowName(L.throwType));

                                if (!L.enabled)
                                    ImGui::PopStyleColor();

                                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 48.f);
                                if (ImGui::SmallButton("Edit")) {
                                    s_renameIdx = i;
                                    strncpy_s(s_renameBuf, sizeof(s_renameBuf), L.name.c_str(), _TRUNCATE);
                                    s_editThrow = static_cast<int>(L.throwType);
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("X")) {
                                    if (s_renameIdx == i)
                                        s_renameIdx = -1;
                                    GrenadeHelper::RemoveAt(i);
                                    ImGui::PopID();
                                    break;
                                }
                            }

                            ImGui::PopID();
                            ++shown;
                        }
                        if (shown == 0)
                            ImGui::TextDisabled(Lang::T("No lineups for this map."));
                    }
                    ImGui::EndChild();
                }
                if (ImGui::Button(Lang::T("Clear This Map")))
                    GrenadeHelper::ClearCurrentMap();
                ImGui::SameLine();
                if (ImGui::Button(Lang::T("Reload File")))
                    GrenadeHelper::Load();
                ImGui::EndDisabled();
                ui::EndCard();
            }
            else if (visSub == 3) {
                ui::BeginCard("##view_left", half, true);
                ui::SectionLabel("Viewmodel");
                ImGui::TextDisabled(Lang::T("Move and size your gun on screen"));

                ImGui::Checkbox(Lang::T("Enable##viewmodel_changer"), &Config::viewmodel_changer);

                ImGui::BeginDisabled(!Config::viewmodel_changer);
                ui::SliderFull("Offset X", "##vm_x", &Config::viewmodel_x, -20.f, 20.f, "%.2f");
                ui::SliderFull("Offset Y", "##vm_y", &Config::viewmodel_y, -20.f, 20.f, "%.2f");
                ui::SliderFull("Offset Z", "##vm_z", &Config::viewmodel_z, -20.f, 20.f, "%.2f");
                ui::SliderFull("Viewmodel FOV", "##vm_fov", &Config::viewmodel_fov, 40.f, 120.f, "%.0f");
                ImGui::EndDisabled();
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##view_right", 0, true);
                ui::SectionLabel("Third Person");
                ImGui::Checkbox(Lang::T("Enable##thirdperson"), &Config::thirdperson);

                ImGui::BeginDisabled(!Config::thirdperson);
                {
                    KeybindStack(Config::thirdperson);
                }
                ui::SliderFull("Distance", "##tp_dist", &Config::thirdperson_distance, 50.f, 300.f, "%.0f");
                ImGui::EndDisabled();

                ImGui::Spacing();
                ui::SectionLabel("World FOV");
                ImGui::Checkbox(Lang::T("Custom FOV"), &Config::fovEnabled);
                ImGui::BeginDisabled(!Config::fovEnabled);
                ui::SliderFull("FOV Value", "##world_fov", &Config::fov, 20.0f, 160.0f, "%.0f");
                ImGui::EndDisabled();

                ImGui::Spacing();
                ui::SectionLabel("Aspect Ratio");
                ImGui::Checkbox(Lang::T("Custom Aspect"), &Config::aspect_ratio_enabled);
                ImGui::BeginDisabled(!Config::aspect_ratio_enabled);
                ui::SliderFull("Ratio", "##aspect_ratio", &Config::aspect_ratio, 0.5f, 3.5f, "%.3f");
                if (ImGui::Button(Lang::T("4:3")))  Config::aspect_ratio = 4.f / 3.f;
                ImGui::SameLine();
                if (ImGui::Button(Lang::T("16:10"))) Config::aspect_ratio = 16.f / 10.f;
                ImGui::SameLine();
                if (ImGui::Button(Lang::T("16:9"))) Config::aspect_ratio = 16.f / 9.f;
                ImGui::SameLine();
                if (ImGui::Button(Lang::T("21:9"))) Config::aspect_ratio = 21.f / 9.f;
                ImGui::EndDisabled();

                ui::EndCard();
            }
            else if (visSub == 4) {
                ui::BeginCard("##removals_left", half, true);
                ui::SectionLabel("World / FX");
                ImGui::TextDisabled(Lang::T("Turn off visual clutter"));

                ui::SliderFull("Flash Reduce", "##flash_reduce", &Config::antiflash_amount, 0.f, 100.f, "%.0f%%");

                ImGui::Checkbox(Lang::T("Smoke"), &Config::remove_smoke);

                ImGui::Checkbox(Lang::T("Decals"), &Config::remove_decals);

                if (ImGui::Checkbox(Lang::T("Crosshair"), &Config::remove_crosshair)) {
                    if (Config::remove_crosshair)
                        Config::force_crosshair = false;
                }

                if (ImGui::Checkbox(Lang::T("Force Crosshair"), &Config::force_crosshair)) {
                    if (Config::force_crosshair)
                        Config::remove_crosshair = false;
                }

                ImGui::Checkbox(Lang::T("Visual Recoil"), &Config::remove_recoil);

                ImGui::Spacing();
                ui::SectionLabel("Autowall Crosshair");
                ImGui::Checkbox(Lang::T("Autowall Crosshair"), &Config::autowall_xhair);
                if (Config::autowall_xhair) {
                    static const char* kAwXhairStyle[] = { "Dot", "Box" };
                    ui::ComboFull("Style", "##aw_xhair_style", &Config::autowall_xhair_style, kAwXhairStyle, 2);
                    ui::SliderFull("Size", "##aw_xhair_size", &Config::autowall_xhair_size, 2.f, 100.f, "%.0f");
                    ImGui::ColorEdit4("Can Penetrate", (float*)&Config::autowall_xhair_can,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Blocked", (float*)&Config::autowall_xhair_cant,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##removals_right", 0, true);
                ui::SectionLabel("View / Scope");
                ImGui::Checkbox(Lang::T("Firstperson Legs"), &Config::remove_legs);

                ImGui::Checkbox(Lang::T("Hide Viewmodel When Scoped"), &Config::scope_hide_viewmodel);

                ImGui::Spacing();
                ui::SectionLabel("Scope Lines");
                ImGui::Checkbox(Lang::T("Custom Scope Lines"), &Config::scope_custom_lines);
                if (Config::scope_custom_lines) {
                    ui::SliderFull("Size", "##scope_size", &Config::scope_line_size, 0.f, 1.f, "%.2f");
                    ui::SliderFull("Gap", "##scope_gap", &Config::scope_line_gap, 0.f, 40.f, "%.0f");
                    ui::SliderFull("Thickness", "##scope_th", &Config::scope_line_thickness, 0.1f, 6.f, "%.2f");
                    ImGui::TextUnformatted(Lang::T("Color"));
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::ColorEdit4("##scope_col", (float*)&Config::scope_line_color,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
                }

                ImGui::Spacing();
                ui::SectionLabel("Scope Zoom FOV");
                ImGui::Checkbox(Lang::T("Custom Zoom FOV"), &Config::scope_zoom_fov);
                if (Config::scope_zoom_fov) {
                    ui::SliderFull("Zoom 1", "##scope_fov1", &Config::scope_fov_1, 1.f, 90.f, "%.0f");
                    ui::SliderFull("Zoom 2", "##scope_fov2", &Config::scope_fov_2, 1.f, 90.f, "%.0f");
                }
                ui::EndCard();
            }
        }
        break;

        case 2: // Skins
        {
            SkinMenu::Draw();
        }
        break;

        case 3: // Misc
        {
	            // Assist - auto pistol
	            // Match - auto accept / vote reveal
	            ui::BeginCard("##misc_left", half, true);
	            ui::SectionLabel("Movement");
	            ImGui::Checkbox(Lang::T("Bunny Hop"), &Config::bhop);
	            ImGui::Checkbox(Lang::T("Auto Strafe"), &Config::autostrafe);
	            if (Config::autostrafe) {
	                static const char* kStrafeMode[] = { "Mouse", "WASD Subtick" };
	                ui::ComboFull("Strafe Mode", "##strafe_mode", &Config::autostrafe_mode, kStrafeMode, 2);
	                if (ImGui::IsItemHovered())
	                    ImGui::SetTooltip(
	                        "Mouse: A/D from mouse delta.\n"
	                        "WASD Subtick: hold space + WASD in air; "
	                        "injects 16 yaw_delta subticks (needs sv_quantize_movement_input 1).");
	            }
	            ImGui::Checkbox(Lang::T("Jumpbug"), &Config::jumpbug);
	            if (ImGui::IsItemHovered())
	                ImGui::SetTooltip("Hold space in air (or bind a key). Duck/jump subticks at land fraction.");
	            if (Config::jumpbug)
	                KeybindRow(Config::jumpbug);
	            ImGui::Checkbox(Lang::T("Edgejump"), &Config::edgejump);
	            if (Config::edgejump)
	                KeybindRow(Config::edgejump);
	            ImGui::Checkbox(Lang::T("Fastladder"), &Config::fastladder);

	            ImGui::Spacing();
	            ui::SectionLabel("Backtrack");
	            ImGui::Checkbox(Lang::T("Backtrack"), &Config::backtrack);
	            if (ImGui::IsItemHovered())
	                ImGui::SetTooltip(
	                    "Aim at where enemies were. The command's fire tick is shifted\n"
	                    "to match, so the server rewinds to the same spot you shoot.");
	            if (Config::backtrack) {
	                ui::SliderFull("Window", "##bt_ms", &Config::backtrack_ms, 0.f, 200.f, "%.0f ms");
	                if (ImGui::IsItemHovered())
	                    ImGui::SetTooltip(
	                        "Rewind depth. First ~31ms is free (interp window);\n"
	                        "beyond that the fire tick is backdated. Deep claims may\n"
	                        "be clamped by the server - if shots stop registering,\n"
	                        "lower this.");
	                ImGui::Checkbox(Lang::T("Show Ghost Skeletons"), &Config::backtrack_skeleton);
	                if (ImGui::IsItemHovered())
	                    ImGui::SetTooltip("Draw recorded enemy skeletons at the rewind depth.");
	            }

	            ImGui::Spacing();
	            ui::SectionLabel("Assist");
	            ImGui::Checkbox(Lang::T("Auto Pistol"), &Config::auto_pistol);

	            ImGui::Spacing();
	            ui::SectionLabel("Match");
            ImGui::Checkbox(Lang::T("Auto Accept"), &Config::auto_accept);
            ImGui::Checkbox(Lang::T("Unlock Inventory"), &Config::unlock_inventory);
            ImGui::Checkbox(Lang::T("Vote Reveal"), &Config::vote_reveal);
            ImGui::Checkbox(Lang::T("Scoreboard Weapons"), &Config::scoreboard_weapons);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reveal enemy weapons and active weapon icons while holding TAB.");
            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##misc_right", 0, true);
            ui::SectionLabel("Hitsounds");
            ImGui::Checkbox(Lang::T("Enable"), &Config::hitsound);
            ImGui::PushItemWidth(-1.f);
            static bool hsListReady = false;
            if (!hsListReady) {
                Hitsound::RefreshList();
                hsListReady = true;
            }
            const int n = Hitsound::Count();
            // Built once per list refresh - rebuilding vector<string> per frame
            // for three combos was pure churn while the Misc tab is open.
            static std::vector<std::string> hsNames;     // [0] = "" placeholder
            static std::vector<std::string> hsLabelsDef; // Lang::T("(none)") + names
            static std::vector<std::string> hsLabelsOpt; // Lang::T("(same as hit)") + names
            static std::vector<const char*> hsPtrsDef;   // stable c_str() into hsLabelsDef
            static std::vector<const char*> hsPtrsOpt;   // stable c_str() into hsLabelsOpt
            auto buildHsLists = [&]() {
                hsNames.clear();
                hsLabelsDef.clear();
                hsLabelsOpt.clear();
                hsNames.emplace_back("");
                hsLabelsDef.emplace_back(Lang::T("(none)"));
                hsLabelsOpt.emplace_back(Lang::T("(same as hit)"));
                for (int i = 0; i < n; ++i) {
                    const char* nm = Hitsound::NameAt(i);
                    hsNames.emplace_back(nm ? nm : "");
                    hsLabelsDef.emplace_back(nm ? nm : "");
                    hsLabelsOpt.emplace_back(nm ? nm : "");
                }
                hsPtrsDef.clear();
                hsPtrsOpt.clear();
                for (size_t i = 0; i < hsLabelsDef.size(); ++i) {
                    hsPtrsDef.push_back(hsLabelsDef[i].c_str());
                    hsPtrsOpt.push_back(hsLabelsOpt[i].c_str());
                }
            };
            static int hsBuiltCount = -1;
            if (hsBuiltCount != n) {
                buildHsLists();
                hsBuiltCount = n;
            }
            {
                ImGui::TextUnformatted(Lang::T("Default"));
                auto drawDefault = [&](const char* id, char* buf, int bufSz) {
                    int cur = Hitsound::IndexOf(buf);
                    int sel = (cur < 0) ? 0 : cur + 1;
                    if (sel >= static_cast<int>(hsPtrsDef.size())) sel = 0;
                    if (ImGui::Combo(id, &sel, hsPtrsDef.data(), static_cast<int>(hsPtrsDef.size()))) {
                        if (sel == 0) buf[0] = 0;
                        else std::snprintf(buf, bufSz, "%s", hsNames[static_cast<size_t>(sel)].c_str());
                    }
                };
                drawDefault("##hs_default", Config::hitsound_file, sizeof(Config::hitsound_file));

                auto drawOptional = [&](const char* id, char* buf, int bufSz) {
                    int cur = Hitsound::IndexOf(buf);
                    int sel = (cur < 0) ? 0 : cur + 1;
                    if (sel >= static_cast<int>(hsPtrsOpt.size())) sel = 0;
                    if (ImGui::Combo(id, &sel, hsPtrsOpt.data(), static_cast<int>(hsPtrsOpt.size()))) {
                        if (sel == 0) buf[0] = 0;
                        else std::snprintf(buf, bufSz, "%s", hsNames[static_cast<size_t>(sel)].c_str());
                    }
                };
                ImGui::TextUnformatted(Lang::T("Headshot (optional)"));
                drawOptional("##hs_head", Config::hitsound_head, sizeof(Config::hitsound_head));
                ImGui::TextUnformatted(Lang::T("Kill (optional)"));
                drawOptional("##hs_kill", Config::hitsound_kill, sizeof(Config::hitsound_kill));

                ImGui::PopItemWidth();
                if (ImGui::Button(Lang::T("Refresh list"), ImVec2(-1.f, 0))) {
                    Hitsound::RefreshList();
                    hsListReady = true;
                    hsBuiltCount = -1; // force list rebuild even if count unchanged
                }
                if (ImGui::Button(Lang::T("Test sound"), ImVec2(-1.f, 0)))
                    Hitsound::PreviewSelected();
                ImGui::TextDisabled(Lang::T("Drop .wav into Documents\\Games8Th\\Hitsounds"));
            }
 // Hitmarker + Floating Damage + Hit Log share the Hit Feedback
 // header ??" no per-feature SectionLabel to keep the tab tight.
            ImGui::Spacing();
            ui::SectionLabel("Bullet Feedback");
            ImGui::Checkbox(Lang::T("Bullet Impacts"), &Config::bullet_impact_effect);
            if (Config::bullet_impact_effect) {
                static const char* impactTypes[] = { "Overlay", "Sparks", "Both" };
                ui::ComboFull("Impact Type", "##bullet_impact_type", &Config::bullet_impact_effect_type,
                    impactTypes, IM_ARRAYSIZE(impactTypes));
                ui::SliderFull("Impact Duration", "##bullet_impact_duration",
                    &Config::bullet_impact_effect_duration, 0.1f, 5.f, "%.1fs");
                const ImGuiColorEditFlags bulletColors = ImGuiColorEditFlags_NoInputs
                    | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoTooltip;
                ImGui::ColorEdit4("Impact Fill", (float*)&Config::bullet_impact_effect_fill_color, bulletColors);
                ImGui::ColorEdit4("Impact Edge", (float*)&Config::bullet_impact_effect_edge_color, bulletColors);
                ImGui::ColorEdit4("Spark Color", (float*)&Config::bullet_impact_effect_color_spark, bulletColors);
                ImGui::Checkbox(Lang::T("Impact Glow"), &Config::bullet_impact_effect_glow);
                if (Config::bullet_impact_effect_glow)
                    ui::SliderFull("Glow Strength", "##bullet_glow_strength",
                        &Config::bullet_impact_effect_glow_strength, 0.f, 2.f, "%.1f");
            }
            ImGui::Checkbox(Lang::T("Bullet Tracers"), &Config::bullet_tracers);
            if (Config::bullet_tracers) {
                ui::SliderFull("Tracer Duration", "##bullet_tracer_duration",
                    &Config::bullet_tracer_duration, 0.1f, 5.f, "%.1fs");
                const ImGuiColorEditFlags tracerColors = ImGuiColorEditFlags_NoInputs
                    | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoTooltip;
                ImGui::ColorEdit4("Tracer Color", (float*)&Config::bullet_tracer_color, tracerColors);
            }

            ImGui::Spacing();
            ImGui::Checkbox(Lang::T("Hitmarker"), &Config::hitmarker);
            if (Config::hitmarker) {
                ImGui::Checkbox(Lang::T("Screen"), &Config::hitmarker_screen);
                ImGui::Checkbox(Lang::T("World 3D"), &Config::hitmarker_world);
                ImGui::Checkbox(Lang::T("Show Damage"), &Config::hitmarker_show_damage);

 // Sliders/colors first so preview reads same-frame values
                ui::SliderFull("Size", "##hm_size", &Config::hitmarker_size, 6.f, 32.f, "%.0f");
                ui::SliderFull("Thickness", "##hm_thick", &Config::hitmarker_thickness, 1.f, 5.f, "%.1f");
                ui::SliderFull("World Size", "##hm_wsize", &Config::hitmarker_world_size, 4.f, 28.f, "%.0f");
                ui::SliderFull("Duration", "##hm_dur", &Config::hitmarker_duration, 0.25f, 2.5f, "%.2f");

 // Stack when narrow ??" three labeled pickers on one line clip half-cards
                {
                    const ImGuiColorEditFlags colHm = ImGuiColorEditFlags_NoInputs
                        | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoTooltip;
                    const float need = ImGui::CalcTextSize("Color").x
                        + ImGui::CalcTextSize("Head").x
                        + ImGui::CalcTextSize("Kill").x
                        + 3.f * 28.f + 24.f;
                    if (ImGui::GetContentRegionAvail().x >= need) {
                        ImGui::ColorEdit4("Color##hm", (float*)&Config::hitmarker_color, colHm);
                        ImGui::SameLine(0, 8.f);
                        ImGui::ColorEdit4("Head##hm", (float*)&Config::hitmarker_head_color, colHm);
                        ImGui::SameLine(0, 8.f);
                        ImGui::ColorEdit4("Kill##hm", (float*)&Config::hitmarker_kill_color, colHm);
                    } else {
                        ImGui::ColorEdit4("Color##hm", (float*)&Config::hitmarker_color, colHm);
                        ImGui::ColorEdit4("Head##hm", (float*)&Config::hitmarker_head_color, colHm);
                        ImGui::ColorEdit4("Kill##hm", (float*)&Config::hitmarker_kill_color, colHm);
                    }
                }

 // Live COD-X ??" after widgets so drag updates this frame; pulse + world sample
                {
                    static int s_hmPrevMode = 0; // 0 normal / 1 head / 2 kill
                    ImGui::Spacing();
                    ImGui::TextUnformatted(Lang::T("Preview"));
                    const float boxH = 88.f;
                    const float boxW = ImGui::GetContentRegionAvail().x;
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    const ImVec2 p1(p0.x + boxW, p0.y + boxH);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 20, 210), 6.f);
                    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 6.f, 0, 1.f);
                    Hitmarker::DrawPreview(dl, p0, p1, s_hmPrevMode);
                    ImGui::Dummy(ImVec2(boxW, boxH));
                    ImGui::Spacing();
                    if (ImGui::RadioButton("Normal##hmp", s_hmPrevMode == 0)) s_hmPrevMode = 0;
                    ImGui::SameLine(0, 10.f);
                    if (ImGui::RadioButton("Head##hmp", s_hmPrevMode == 1)) s_hmPrevMode = 1;
                    ImGui::SameLine(0, 10.f);
                    if (ImGui::RadioButton("Kill##hmp", s_hmPrevMode == 2)) s_hmPrevMode = 2;
                }
            }
            ImGui::Checkbox(Lang::T("Floating Damage"), &Config::float_damage);
            if (Config::float_damage) {
                ui::SliderFull("Float Life", "##fd_life", &Config::float_damage_duration, 0.3f, 2.5f, "%.2f");
                ui::SliderFull("Float Speed", "##fd_speed", &Config::float_damage_speed, 20.f, 120.f, "%.0f");
                {
                    const ImGuiColorEditFlags colFd = ImGuiColorEditFlags_NoInputs
                        | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoTooltip;
                    ImGui::ColorEdit4("Dmg##fd", (float*)&Config::float_damage_color, colFd);
                    ImGui::SameLine(0, 8.f);
                    ImGui::ColorEdit4("Head##fd", (float*)&Config::float_damage_head_color, colFd);
                    ImGui::SameLine(0, 8.f);
                    ImGui::ColorEdit4("Kill##fd", (float*)&Config::float_damage_kill_color, colFd);
                }
            }
 // Hit Log ??" under the Hit Feedback header (right-click for extras)
            ImGui::Checkbox(Lang::T("Hit Log"), &Config::hitlog);
            ImGui::SetNextWindowSize(ImVec2(280.f, 0.f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupContextItem("##hitlog_pop")) {
                ui::PopupTitle("Hit Log");
                ImGui::Checkbox(Lang::T("Game Console"), &Config::hitlog_console);
                ImGui::Checkbox(Lang::T("Show HP left"), &Config::hitlog_show_hp);
                ImGui::Checkbox(Lang::T("Session stats"), &Config::hitlog_show_stats);
                ImGui::EndPopup();
            }
            if (Config::hitlog) {
                ui::SliderFull("Duration", "##hitlog_life", &Config::hitlog_duration, 1.f, 12.f, "%.1f s");
                ui::SliderFull("Width", "##hitlog_w", &Config::hitlog_width, 200.f, 380.f, "%.0f");
                ui::SliderInt("Rows", "##hitlog_rows", &Config::hitlog_max_rows, 4, 16);
                ImGui::Checkbox(Lang::T("HP left##hl"), &Config::hitlog_show_hp);
                ImGui::SameLine();
                ImGui::Checkbox(Lang::T("Stats##hl"), &Config::hitlog_show_stats);
                ImGui::ColorEdit4("Log", (float*)&Config::hitlog_color, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit4("Head##hl", (float*)&Config::hitlog_head_color, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit4("Kill##hl", (float*)&Config::hitlog_kill_color, ImGuiColorEditFlags_NoInputs);
            }

            ImGui::Spacing();
            ui::SectionLabel("HUD & Widgets");
            ImGui::Checkbox(Lang::T("Watermark"), &Config::watermark);
            ImGui::Checkbox(Lang::T("Keybind List"), &Config::widget_keybinds);
            ImGui::Checkbox(Lang::T("Bomb Info"), &Config::widget_bomb);
            if (Config::widget_bomb) {
                ImGui::Checkbox(Lang::T("Auto Defuse"), &Config::auto_defuse);
            }
            ImGui::Checkbox(Lang::T("Spectator List"), &Config::widget_spectators);
            ImGui::Checkbox(Lang::T("Free Spectate"), &Config::enemy_spectate);
            if (Config::enemy_spectate) {
                ImGui::Checkbox(Lang::T("3rd Person Cam"), &Config::enemy_spectate_thirdperson);
            }
            ImGui::Checkbox(Lang::T("Radar"), &Config::widget_radar);
            if (Config::widget_radar) {
                const char* shapes[] = { "Circle", "Square" };
                ui::ComboFull("Shape", "##radar_shape", &Config::widget_radar_shape, shapes, IM_ARRAYSIZE(shapes));
                ui::SliderFull("Size", "##radar_size", &Config::widget_radar_size, 90.f, 280.f, "%.0f");
            }
            ui::EndCard();
        }
        break;

        case 4: // Config
        {
            static char configName[128] = "";
            static std::vector<std::string> configList = internal_config::ConfigManager::ListConfigs();
            static int selectedConfigIndex = -1;
            static std::string statusMsg;
            static float statusUntil = 0.f;

            auto setStatus = [&](const char* msg, bool ok = true, bool warn = false) {
                statusMsg = msg;
                statusUntil = static_cast<float>(ImGui::GetTime()) + 2.5f;
                if (warn)
                    Notify::Warn("Config", msg);
                else if (ok)
                    Notify::Success("Config", msg);
                else
                    Notify::Error("Config", msg);
            };

 // Fixed-height cards (not AutoResizeY): Manage + Design content is
 // ~750px tall vs ~455px viewport. AutoResizeY cards grow unbounded
 // (child auto-fit has no height clamp) and the bottom of the card -
 // the font manager / design section - gets cut off at the content
 // edge. Filling the region gives the card an internal scrollbar so
 // nothing clips.
            ui::BeginCard("##cfg_left", half);
            ui::SectionLabel("Manage");

            ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
            ImGui::TextWrapped("Documents\\Games8Th\\Configs");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::Checkbox(Lang::T("Compress on save"), &Config::config_compress);

 // UI font size - fixed built-in segoeui/arial chain
            {
                ui::SliderFull("Font Size", "##ui_font_size", &Config::ui_font_size,
                    12.f, 24.f, "%.0f px");
            }

            ImGui::Spacing();

            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##cfgname", "Config name...", configName, IM_ARRAYSIZE(configName));

            ImGui::Spacing();
            const float gapBtn = 6.f;
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float base = floorf((std::max)(0.f, avail - gapBtn * 2.f) / 3.f);
                const float last = (std::max)(1.f, avail - base * 2.f - gapBtn * 2.f);

                if (ImGui::Button(Lang::T("Save"), ImVec2((std::max)(1.f, base), 0))) {
                    if (configName[0] == '\0') {
                        setStatus("Enter a config name", false, true);
                    } else if (internal_config::ConfigManager::Save(configName)) {
                        configList = internal_config::ConfigManager::ListConfigs();
                        setStatus("Saved");
                    } else {
                        setStatus("Save failed", false);
                    }
                }
                ImGui::SameLine(0, gapBtn);
                if (ImGui::Button(Lang::T("Load"), ImVec2((std::max)(1.f, base), 0))) {
                    if (configName[0] == '\0') {
                        setStatus("Select or type a name", false, true);
                    } else if (internal_config::ConfigManager::Load(configName)) {
                        setStatus("Loaded");
                    } else {
                        setStatus("Load failed", false);
                    }
                }
                ImGui::SameLine(0, gapBtn);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.28f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.28f, 0.35f, 0.70f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.32f, 0.38f, 0.85f));
                if (ImGui::Button(Lang::T("Delete"), ImVec2(last, 0))) {
                    if (configName[0] == '\0') {
                        setStatus("Select a config first", false, true);
                    } else if (internal_config::ConfigManager::Remove(configName)) {
                        configName[0] = '\0';
                        selectedConfigIndex = -1;
                        configList = internal_config::ConfigManager::ListConfigs();
                        setStatus("Deleted");
                    } else {
                        setStatus("Delete failed", false);
                    }
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float half0 = floorf((std::max)(0.f, avail - gapBtn) * 0.5f);
                const float half1 = (std::max)(1.f, avail - half0 - gapBtn);
                if (ImGui::Button(Lang::T("Open Folder"), ImVec2(half0, 0))) {
                    internal_config::ConfigManager::OpenFolder();
                    Notify::Info("Config", "Opened configs folder");
                }
                ImGui::SameLine(0, gapBtn);
                if (ImGui::Button(Lang::T("Refresh"), ImVec2(half1, 0))) {
                    configList = internal_config::ConfigManager::ListConfigs();
                    setStatus("Refreshed");
                }
            }

            if (!statusMsg.empty() && ImGui::GetTime() < statusUntil) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent());
                ImGui::TextUnformatted(statusMsg.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ui::SectionLabel("Design");

            const ImGuiColorEditFlags designCol =
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

            const char* presets[] = {
                "Midnight OLED", "Steel Slate", "Nordic Frost", "Cyberpunk Neon",
                "Emerald Matrix", "Sunset Crimson", "Dracula Velvet", "Solar Gold"
            };
            if (ui::ComboFull("Preset", "##menu_preset", &Config::menu_preset, presets, IM_ARRAYSIZE(presets)))
                ui::ApplyMenuPreset(Config::menu_preset);
            if (ImGui::Button(Lang::T("Reset Design"), ImVec2(-1.f, 0))) {
                ui::ApplyMenuPreset(1); // Steel Slate default
            }

            ImGui::Spacing();
            ImGui::ColorEdit4("Accent", (float*)&Config::menu_accent, designCol);
            ImGui::ColorEdit4("Background", (float*)&Config::menu_bg, designCol);
            ImGui::ColorEdit4("Cards", (float*)&Config::menu_child_bg, designCol);
            ImGui::ColorEdit4("Sidebar", (float*)&Config::menu_sidebar_bg, designCol);
            ImGui::ColorEdit4("Border", (float*)&Config::menu_border, designCol);
            ImGui::ColorEdit4("Text", (float*)&Config::menu_text, designCol);
            ImGui::ColorEdit4("Muted", (float*)&Config::menu_text_muted, designCol);

            ui::SliderFull("Rounding", "##menu_rounding", &Config::menu_rounding, 0.f, 14.f, "%.0f");
            ui::SliderFull("Opacity", "##menu_opacity", &Config::menu_opacity, 0.55f, 1.f, "%.2f");
            ui::SliderFull("Glass", "##menu_glass", &Config::menu_glass, 0.f, 1.f, "%.2f");
            {
                static const char* kDpi[] = {
                    "100%", "125%", "150%", "175%", "200%"
                };
                int dpiIdx = 0;
                const int p = Config::menu_dpi_scale;
                if (p >= 200) dpiIdx = 4;
                else if (p >= 175) dpiIdx = 3;
                else if (p >= 150) dpiIdx = 2;
                else if (p >= 125) dpiIdx = 1;
                else dpiIdx = 0;
                if (ui::ComboFull("DPI Scale", "##menu_dpi", &dpiIdx, kDpi, IM_ARRAYSIZE(kDpi))) {
                    static const int kPct[] = { 100, 125, 150, 175, 200 };
                    if (dpiIdx >= 0 && dpiIdx < 5)
                        Config::menu_dpi_scale = kPct[dpiIdx];
                }
            }
            ImGui::Checkbox(Lang::T("Compact spacing"), &Config::menu_compact);
            ImGui::Checkbox(Lang::T("Sidebar labels"), &Config::menu_sidebar_labels);
            ImGui::Checkbox(Lang::T("Widgets follow menu"), &Config::menu_widgets_follow);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Keybinds, bomb, spectators, and radar use menu colors and rounding.");

            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##cfg_right", 0);
            ui::SectionLabel("Saved Configs");

            if (configList.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                ImGui::TextUnformatted(Lang::T("No configs yet."));
                ImGui::TextUnformatted(Lang::T("Save one to get started."));
                ImGui::PopStyleColor();
            } else {
                const float listH = (std::max)(80.f, ImGui::GetContentRegionAvail().y);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.15f));
                ImGui::BeginChild("##cfg_list", ImVec2(-1.f, listH),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_None);
                for (int i = 0; i < static_cast<int>(configList.size()); ++i) {
                    const bool sel = (selectedConfigIndex == i);
                    if (ImGui::Selectable(configList[i].c_str(), sel)) {
                        selectedConfigIndex = i;
                        strncpy_s(configName, sizeof(configName), configList[i].c_str(), _TRUNCATE);
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (internal_config::ConfigManager::Load(configList[i])) {
                            setStatus("Loaded");
                        } else {
                            setStatus("Load failed", false);
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ui::EndCard();
        }
        break;
        }
    }
    MenuUI::EndContent();

    MenuUI::DrawWindowFrame(ImGui::GetWindowDrawList(), wpos,
        ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), round);

    ImGui::End();
    ImGui::PopStyleVar(2); // window alpha + rounding
}

void Menu::toggleMenu() {
    showMenu = !showMenu;
    if (showMenu) {
        MenuUI::NotifyTab(activeTab);
    } else {
        MenuUI::AnimTick(false);
        if (Config::menu_w >= 640.f && Config::menu_h >= 420.f)
            internal_config::ConfigManager::SaveMenuSize();
    }
}


