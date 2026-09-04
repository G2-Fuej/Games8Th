#include "widgets.h"
#include "steam_avatar.h"

#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/console/console.h"
#include "../../offsets/offsets.h"
#include "../visuals/visuals.h"
#include "../visuals/weapon_icon_draw.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../utils/math/vector/vector.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../../../../external/imgui/imgui.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <Windows.h>
#include <d3d11.h>

extern ID3D11Device* pDevice;

namespace Widgets {
namespace {

float PanelRounding() {
	return std::clamp(Config::menu_rounding, 2.f, 8.f);
}

ImVec4 WidgetAccent(const ImVec4& custom) {
	return Config::menu_widgets_follow ? Config::menu_accent : custom;
}

ImU32 ColU32(const ImVec4& c) {
	return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 ColU32A(const ImVec4& c, float aMul) {
	ImVec4 t = c;
	t.w = std::clamp(c.w * aMul, 0.f, 1.f);
	return ImGui::ColorConvertFloat4ToU32(t);
}

ImVec4 U32ToVec4(ImU32 c) {
	return ImVec4(
		((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f);
}

void DrawTextShadow(ImDrawList* dl, ImFont* font, float sz, ImVec2 p, ImU32 col, const char* text) {
	if (!dl || !font || !text)
		return;
	dl->AddText(font, sz, ImVec2(p.x + 1.f, p.y + 1.f), IM_COL32(0, 0, 0, 140), text);
	dl->AddText(font, sz, p, col, text);
}

void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 accent, float alpha = 1.f) {
	const float h = b.y - a.y;
	const float r = (std::min)(PanelRounding(), h * 0.5f);
	alpha = std::clamp(alpha, 0.f, 1.f);
	auto A = [alpha](int v) {
		return static_cast<int>(std::clamp(static_cast<float>(v) * alpha, 0.f, 255.f));
	};

	dl->AddRectFilled(ImVec2(a.x, a.y + 2.f), ImVec2(b.x, b.y + 3.f), IM_COL32(0, 0, 0, A(88)), r + 1.f);

	ImVec4 fill = Config::menu_bg;
	fill.w = std::clamp(Config::menu_opacity, 0.72f, 0.94f) * alpha;
	dl->AddRectFilled(a, b, ColU32(fill), r);

	const float glass = std::clamp(Config::menu_glass, 0.f, 1.f);
	const float sheenH = std::clamp(h * 0.22f, 6.f, r + 4.f);
	dl->PushClipRect(a, b, true);
	dl->AddRectFilled(
		a, ImVec2(b.x, a.y + sheenH),
		IM_COL32(255, 255, 255, A(static_cast<int>(22.f * glass))),
		r, ImDrawFlags_RoundCornersTop);
	dl->PopClipRect();

	ImVec4 border = Config::menu_border;
	border.w = std::clamp(border.w, 0.10f, 0.20f) * alpha;
	dl->AddRect(a, b, ColU32(border), r, 0, 1.f);
	dl->AddRect(a, b, IM_COL32(255, 255, 255, A(static_cast<int>(20.f * glass))), r, 0, 1.f);

	const float inset = (std::max)(r, 5.f);
	ImVec4 ac = U32ToVec4(accent);
	ac.w = 0.95f * alpha;
	dl->AddRectFilled(ImVec2(a.x + inset, a.y + 1.f), ImVec2(b.x - inset, a.y + 3.f), ColU32(ac), 1.f);
}

void DrawHeaderSep(ImDrawList* dl, ImVec2 a, ImVec2 b, float padX, float y, float alpha = 1.f) {
	const int a8 = static_cast<int>(18.f * std::clamp(alpha, 0.f, 1.f));
	dl->AddRectFilled(ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 1.f), IM_COL32(255, 255, 255, a8));
}

constexpr ImGuiColorEditFlags kWidgetColFlags =
	ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
	| ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

void WidgetColorEdit(const char* label, ImVec4* col) {
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	// ImGui 1.92: avoid obsolete GetWindowContentRegionMax
	const float btn = ImGui::GetFrameHeight();
	ImGui::SameLine();
	const float remain = ImGui::GetContentRegionAvail().x;
	if (remain > btn)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remain - btn);
	char id[64];
	std::snprintf(id, sizeof(id), "##wcol_%s", label);
	ImGui::ColorEdit4(id, (float*)col, kWidgetColFlags);
}

bool BeginWidgetPopup(const char* id, const char* title, ImU32 accent, float width = 196.f) {
	const float r = PanelRounding();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, r);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 5.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, r);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);

	const ImVec4 accentV = U32ToVec4(accent);
	ImVec4 popupBg = Config::menu_child_bg;
	popupBg.w = std::clamp(Config::menu_opacity + 0.06f, 0.80f, 0.96f);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
	ImGui::PushStyleColor(ImGuiCol_Border, Config::menu_border);
	ImGui::PushStyleColor(ImGuiCol_Text, Config::menu_text);
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 1.f, 0.07f));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.28f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.18f));
	ImGui::PushStyleColor(ImGuiCol_CheckMark, accentV);
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accentV.x, accentV.y, accentV.z, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.18f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.24f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.04f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.08f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, accentV);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(accentV.x, accentV.y, accentV.z, 1.f));

	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, 480.f));
	if (!ImGui::BeginPopup(id, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::PopStyleColor(16);
		ImGui::PopStyleVar(8);
		return false;
	}

	ImGui::TextUnformatted(title);
	ImGui::Dummy(ImVec2(0.f, 1.f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0.f, 3.f));
	return true;
}

void EndWidgetPopup() {
	ImGui::EndPopup();
	ImGui::PopStyleColor(16);
	ImGui::PopStyleVar(8);
}

bool WidgetResetButton(bool withSeparator = true) {
	if (withSeparator) {
		ImGui::Dummy(ImVec2(0.f, 2.f));
		ImGui::Separator();
		ImGui::Dummy(ImVec2(0.f, 2.f));
	}
	const float w = ImGui::GetContentRegionAvail().x;
	return ImGui::Button("Reset position", ImVec2(w, 0.f));
}

void DrawProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, ImU32 fill, ImU32 bg) {
	t = std::clamp(t, 0.f, 1.f);
	const float r = 1.5f;
	dl->AddRectFilled(a, b, bg, r);
	if (t > 0.01f) {
		const ImVec2 f(a.x + (b.x - a.x) * t, b.y);
		dl->AddRectFilled(a, f, fill, r);
		dl->AddRectFilledMultiColor(a, ImVec2(f.x, a.y + (b.y - a.y) * 0.45f),
			IM_COL32(255, 255, 255, 36), IM_COL32(255, 255, 255, 36),
			IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
	}
}

void DrawFuseBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, ImU32 fill) {
	t = std::clamp(t, 0.f, 1.f);
	const float r = 2.f;
	dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 16), r);
	if (t <= 0.01f)
		return;
	const ImVec2 f(a.x + (b.x - a.x) * t, b.y);
	dl->AddRectFilled(a, f, fill, r);
	dl->AddRectFilledMultiColor(a, ImVec2(f.x, a.y + (b.y - a.y) * 0.45f),
		IM_COL32(255, 255, 255, 40), IM_COL32(255, 255, 255, 40),
		IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
}

void DrawBombGlyph(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
	dl->AddCircleFilled(c, r * 0.70f, col, 14);
	dl->AddLine(
		ImVec2(c.x + r * 0.18f, c.y - r * 0.52f),
		ImVec2(c.x + r * 0.58f, c.y - r * 0.92f),
		col, 1.6f);
	dl->AddCircleFilled(ImVec2(c.x + r * 0.60f, c.y - r * 0.96f), r * 0.12f, col, 8);
}

void DrawSiteBadge(ImDrawList* dl, ImVec2 a, float w, float h, char site, ImU32 accent, bool idle) {
	const ImVec2 b(a.x + w, a.y + h);
	const float r = 4.f;
	ImVec4 av = U32ToVec4(accent);
	av.w = idle ? 0.16f : 0.28f;
	dl->AddRectFilled(a, b, ColU32(av), r);
	dl->AddRect(a, b, accent, r, 0, 1.f);

	WeaponIconDraw::EnsureReady(pDevice);
	const float iconH = h * 0.72f;
	const float drawn = WeaponIconDraw::DrawCentered(
		dl, a.x + w * 0.5f, a.y + (h - iconH) * 0.5f, accent, "c4", iconH);
	if (drawn <= 0.f)
		DrawBombGlyph(dl, ImVec2(a.x + w * 0.5f, a.y + h * 0.54f), h * 0.32f, accent);

	if (site != 'A' && site != 'B')
		return;
	char letter[2] = { site, '\0' };
	ImFont* font = ImGui::GetFont();
	const float px = 16.f;
	const ImVec2 ts = font->CalcTextSizeA(px, 9999.f, 0.f, letter);
	const ImVec2 lp(b.x - ts.x - 2.f, b.y - ts.y + 1.f);
	dl->AddRectFilled(ImVec2(lp.x - 3.f, lp.y + 1.f), ImVec2(b.x, b.y),
		IM_COL32(0, 0, 0, 170), 2.f);
	dl->AddText(font, px, ImVec2(lp.x + 1.f, lp.y + 1.f), IM_COL32(0, 0, 0, 140), letter);
	dl->AddText(font, px, lp, IM_COL32(250, 251, 253, 250), letter);
}

const char* ModeTagShort(int mode) {
	switch (mode) {
	case 0: return "A";
	case 1: return "H";
	case 2: return "T";
	default: return "?";
	}
}

float EstimateBombDamage(float distUnits, int armor) {
	constexpr float kDmg = 500.f;
	constexpr float kRadius = 1750.f;
	if (distUnits >= kRadius || distUnits < 0.f)
		return 0.f;
	float dmg = kDmg - distUnits * (kDmg / kRadius);
	if (dmg < 0.f)
		dmg = 0.f;
	if (armor > 0) {
		const float newDmg = dmg * 0.5f;
		float armorNeeded = (dmg - newDmg) * 0.5f;
		if (armorNeeded > static_cast<float>(armor)) {
			armorNeeded = static_cast<float>(armor) * 2.f;
			dmg = dmg - armorNeeded;
		} else {
			dmg = newDmg;
		}
		if (dmg < 0.f)
			dmg = 0.f;
	}
	return dmg;
}

ImVec2 ClampPanelPos(ImVec2 pos, float panelW, float panelH) {
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	pos.x = std::clamp(pos.x, 4.f, (std::max)(4.f, ds.x - panelW - 4.f));
	pos.y = std::clamp(pos.y, 4.f, (std::max)(4.f, ds.y - panelH - 4.f));
	return pos;
}

bool IsAutoPos(ImVec2 cfg) {
	// Sentinel from defaults / reset; both axes negative = auto place
	return cfg.x < 0.f && cfg.y < 0.f;
}

bool IsFinitePos(ImVec2 p) {
	return std::isfinite(p.x) && std::isfinite(p.y);
}

ImVec2 ResolvePos(ImVec2 cfg, float panelW, float panelH, ImVec2 fallback) {
	ImVec2 pos = cfg;
	if (!IsFinitePos(pos) || IsAutoPos(pos))
		pos = fallback;
	return ClampPanelPos(pos, panelW, panelH);
}

void HandleWidgetDrag(ImVec2& cfgPos, float panelW, float panelH, bool menuOpen) {
	if (!menuOpen)
		return;
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		cfgPos.x += ImGui::GetIO().MouseDelta.x;
		cfgPos.y += ImGui::GetIO().MouseDelta.y;
		cfgPos = ClampPanelPos(cfgPos, panelW, panelH);
		ImGui::SetWindowPos(cfgPos);
	} else {
		ImVec2 p = ImGui::GetWindowPos();
		if (!IsFinitePos(p))
			p = cfgPos;
		cfgPos = ClampPanelPos(p, panelW, panelH);
		if (cfgPos.x != p.x || cfgPos.y != p.y)
			ImGui::SetWindowPos(cfgPos);
	}
}

void ClipTextToWidth(char* buf, size_t bufSize, float maxW) {
	if (!buf || bufSize < 4 || maxW <= 0.f)
		return;
	if (ImGui::CalcTextSize(buf).x <= maxW)
		return;

	// Shrink until base text fits with room for "..."
	while (std::strlen(buf) > 0) {
		const size_t len = std::strlen(buf);
		char tmp[256];
		if (len + 3 >= sizeof(tmp) || len + 3 >= bufSize)
			break;
		std::memcpy(tmp, buf, len);
		tmp[len] = '.';
		tmp[len + 1] = '.';
		tmp[len + 2] = '.';
		tmp[len + 3] = '\0';
		if (ImGui::CalcTextSize(tmp).x <= maxW) {
			std::memcpy(buf, tmp, len + 4);
			return;
		}
		buf[len - 1] = '\0';
	}
	if (bufSize >= 4) {
		buf[0] = '.'; buf[1] = '.'; buf[2] = '.'; buf[3] = '\0';
	}
}

ImU32 MixU32(ImU32 a, ImU32 b, float t) {
	t = std::clamp(t, 0.f, 1.f);
	const ImVec4 va = U32ToVec4(a);
	const ImVec4 vb = U32ToVec4(b);
	return ColU32(ImVec4(
		va.x + (vb.x - va.x) * t,
		va.y + (vb.y - va.y) * t,
		va.z + (vb.z - va.z) * t,
		va.w + (vb.w - va.w) * t));
}

struct KbRowAnim {
	const char* name = "";
	int key = 0;
	int mode = 0;
	float value = -1.f;
	bool live = false;
	bool on = false;
	float vis = 0.f;
	float lit = 0.f;
};

int FindKbRow(const KbRowAnim* arr, int n, const char* name, int key) {
	for (int i = 0; i < n; ++i) {
		const char* an = arr[i].name ? arr[i].name : "";
		const char* bn = name ? name : "";
		if (arr[i].key == key && std::strcmp(an, bn) == 0)
			return i;
	}
	return -1;
}

void RenderKeybindList(bool menuOpen) {
	constexpr int kMaxBinds = 16;
	static KbRowAnim s_anim[kMaxBinds]{};
	static int s_animN = 0;
	static float s_panelVis = 0.f;

	if (!Config::widget_keybinds) {
		s_animN = 0;
		s_panelVis = 0.f;
		return;
	}

	KeybindSnapshot snaps[kMaxBinds]{};
	const int nAll = keybind.listSnapshots(snaps, kMaxBinds);

	KeybindSnapshot enabled[kMaxBinds]{};
	int nEnabled = 0;
	for (int i = 0; i < nAll; ++i) {
		if (!snaps[i].enabled)
			continue;
		enabled[nEnabled++] = snaps[i];
	}

	bool anyActive = false;
	for (int i = 0; i < nEnabled; ++i) {
		if (enabled[i].active) {
			anyActive = true;
			break;
		}
	}

	const bool hideIdle = !menuOpen && Config::widget_keybinds_only_when_active && !anyActive;
	const bool showAll = Config::widget_keybinds_show_all || menuOpen;

	KeybindSnapshot rows[kMaxBinds]{};
	int n = 0;
	if (!hideIdle) {
		for (int i = 0; i < nEnabled; ++i) {
			if (!showAll && !enabled[i].active)
				continue;
			rows[n++] = enabled[i];
		}
	}

	KbRowAnim next[kMaxBinds]{};
	int nNext = 0;
	for (int i = 0; i < n && nNext < kMaxBinds; ++i) {
		const auto& src = rows[i];
		KbRowAnim slot{};
		const int old = FindKbRow(s_anim, s_animN, src.name, src.key);
		if (old >= 0)
			slot = s_anim[old];
		slot.name = src.name ? src.name : "";
		slot.key = src.key;
		slot.mode = src.mode;
		slot.value = src.value;
		slot.live = true;
		slot.on = src.active;
		next[nNext++] = slot;
	}
	for (int i = 0; i < s_animN && nNext < kMaxBinds; ++i) {
		if (FindKbRow(next, nNext, s_anim[i].name, s_anim[i].key) >= 0)
			continue;
		if (s_anim[i].vis <= 0.01f)
			continue;
		KbRowAnim slot = s_anim[i];
		slot.live = false;
		slot.on = false;
		next[nNext++] = slot;
	}

	float dt = ImGui::GetIO().DeltaTime;
	if (dt <= 0.f || dt > 0.05f)
		dt = 1.f / 60.f;
	const float step = dt / 0.08f; // 80ms open/close

	for (int i = 0; i < nNext; ++i) {
		next[i].vis = std::clamp(next[i].vis + (next[i].live ? step : -step), 0.f, 1.f);
		next[i].lit = std::clamp(next[i].lit + (next[i].on ? step : -step), 0.f, 1.f);
	}

	s_animN = 0;
	for (int i = 0; i < nNext; ++i) {
		if (!next[i].live && next[i].vis <= 0.01f)
			continue;
		s_anim[s_animN++] = next[i];
	}

	bool anyLive = false;
	for (int i = 0; i < s_animN; ++i) {
		if (s_anim[i].live) {
			anyLive = true;
			break;
		}
	}
	s_panelVis = std::clamp(s_panelVis + (anyLive ? step : -step), 0.f, 1.f);
	if (s_panelVis <= 0.01f && s_animN <= 0)
		return;

	const float padX = 10.f;
	const float padY = 8.f;
	const float headerH = 16.f;
	const float rowH = 20.f;
	const float rowGap = 1.f;
	const float panelW = 188.f;

	float bodyH = 0.f;
	for (int i = 0; i < s_animN; ++i) {
		const float vis = s_anim[i].vis;
		if (vis <= 0.01f)
			continue;
		if (bodyH > 0.f)
			bodyH += rowGap * vis;
		bodyH += rowH * vis;
	}
	if (bodyH < 1.f)
		bodyH = 1.f;
	const float panelH = padY + headerH + 5.f + bodyH + padY;

	ImVec2 pos = ResolvePos(Config::widget_keybinds_pos, panelW, panelH, ImVec2(14.f, 58.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_keybinds", nullptr, flags);
	{
		ImGui::InvisibleButton("##kb_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_keybinds_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);
		const ImU32 accent = ColU32(WidgetAccent(Config::widget_keybinds_accent));
		const float pv = std::clamp(s_panelVis, 0.f, 1.f);
		DrawPanel(dl, a, b, accent, pv);

		const float titleX = a.x + padX;
		const float titleY = a.y + padY;
		dl->AddText(ImVec2(titleX, titleY), ColU32A(Config::menu_text_muted, 0.90f * pv), "Keybinds");

		int activeN = 0;
		for (int i = 0; i < s_animN; ++i)
			if (s_anim[i].on) ++activeN;
		if (activeN > 0) {
			char badge[16];
			std::snprintf(badge, sizeof(badge), "%d", activeN);
			const ImVec2 bsz = ImGui::CalcTextSize(badge);
			dl->AddText(ImVec2(b.x - padX - bsz.x, titleY),
				ColU32A(Config::menu_text, pv), badge);
		}

		DrawHeaderSep(dl, a, b, padX, a.y + padY + headerH, pv);

		float y = a.y + padY + headerH + 3.f;
		for (int i = 0; i < s_animN; ++i) {
			const KbRowAnim& s = s_anim[i];
			const float vis = s.vis;
			if (vis <= 0.01f)
				continue;

			const float hRow = rowH * vis;
			const float aMul = vis * pv;
			const float lit = std::clamp(s.lit, 0.f, 1.f);

			const ImVec2 rowA(a.x + padX, y);
			const ImVec2 rowB(b.x - padX, y + hRow);

			dl->PushClipRect(ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + hRow + 0.5f), true);

			if (lit > 0.02f) {
				dl->AddRectFilled(
					ImVec2(a.x + 3.f, y + 3.f),
					ImVec2(a.x + 5.f, y + hRow - 3.f),
					ColU32A(WidgetAccent(Config::widget_keybinds_accent), lit * aMul), 1.f);
			}

			const ImU32 nameOff = ColU32A(Config::menu_text_muted, 0.85f * aMul);
			const ImU32 nameOn = ColU32A(Config::menu_text, aMul);
			const ImU32 nameCol = MixU32(nameOff, nameOn, lit);

			char keyName[24];
			Keybinds::formatKeyName(s.key, keyName, sizeof(keyName));
			char keyBuf[32];
			if (s.mode == 0)
				std::snprintf(keyBuf, sizeof(keyBuf), "Always");
			else
				std::snprintf(keyBuf, sizeof(keyBuf), "%s", keyName);

			const ImVec2 ksz = ImGui::CalcTextSize(keyBuf);
			const float keyX = rowB.x - ksz.x;
			const float keyY = y + (hRow - ksz.y) * 0.5f;

			const char* mt = ModeTagShort(s.mode);
			const ImVec2 msz = ImGui::CalcTextSize(mt);
			const float modeX = keyX - 6.f - msz.x;
			const float modeY = y + (hRow - msz.y) * 0.5f;

			const float maxNameW = (std::max)(24.f, modeX - 6.f - rowA.x);
			char nameBuf[64];
			if (s.value > 0.f)
				std::snprintf(nameBuf, sizeof(nameBuf), "%s [%.0f]", s.name ? s.name : "", s.value);
			else
				std::snprintf(nameBuf, sizeof(nameBuf), "%s", s.name ? s.name : "");
			ClipTextToWidth(nameBuf, sizeof(nameBuf), maxNameW);

			const ImVec2 nsz = ImGui::CalcTextSize(nameBuf);
			dl->AddText(ImVec2(rowA.x, y + (hRow - nsz.y) * 0.5f), nameCol, nameBuf);

			const ImU32 modeOff = ColU32A(Config::menu_text_muted, 0.65f * aMul);
			const ImU32 modeOn = ColU32A(WidgetAccent(Config::widget_keybinds_accent), 0.85f * aMul);
			dl->AddText(ImVec2(modeX, modeY), MixU32(modeOff, modeOn, lit), mt);

			const ImU32 keyOff = ColU32A(Config::menu_text_muted, 0.90f * aMul);
			const ImU32 keyOn = ColU32A(Config::menu_text, aMul);
			dl->AddText(ImVec2(keyX, keyY), MixU32(keyOff, keyOn, lit), keyBuf);

			dl->PopClipRect();
			y += hRow + rowGap * vis;
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##kb_widget_settings");
		if (BeginWidgetPopup("##kb_widget_settings", "Keybinds", accent)) {
			ImGui::Checkbox("Only when active", &Config::widget_keybinds_only_when_active);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Hide list until a keybind is active.");
			ImGui::Checkbox("Show all binds", &Config::widget_keybinds_show_all);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Show every bind, not only active ones.");
			if (!Config::menu_widgets_follow)
				WidgetColorEdit("Accent", &Config::widget_keybinds_accent);
			if (WidgetResetButton())
				Config::widget_keybinds_pos = ImVec2(-1.f, -1.f);
			EndWidgetPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

void RenderBombWidget(bool menuOpen) {
	if (!Config::widget_bomb)
		return;

	const PlantedBombInfo& bomb = g_plantedBomb;
	const bool live = bomb.active;
	if (!live && !menuOpen)
		return;

	const float blow = live ? bomb.blowLeft : -1.f;
	const bool defused = live && bomb.defused;
	const bool urgent = !defused && blow >= 0.f && blow <= 10.f;
	// Always reserve rows when planted - hiding until cache/event fired
	// looked like "damage / defuse never show".
	const bool showDefuse = Config::widget_bomb_show_defuse && live && !defused;
	const bool showDmg = Config::widget_bomb_show_damage && live && !defused;

	const float padX = 12.f;
	const float padY = 10.f;
	const float badgeW = 52.f;
	const float badgeH = 28.f;
	const float panelW = 228.f;
	float panelH = padY + badgeH + 22.f + padY;
	if (showDefuse)
		panelH += 28.f;
	if (showDmg)
		panelH += 20.f;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImVec2 pos = ResolvePos(Config::widget_bomb_pos, panelW, panelH,
		ImVec2(ds.x * 0.5f - panelW * 0.5f, ds.y - panelH - 48.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_bomb", nullptr, flags);
	{
		ImGui::InvisibleButton("##bomb_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_bomb_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);

		// Menu UI accent (Config::menu_accent). Urgent / defused override only.
		const ImU32 menuAccent = ColU32(Config::menu_accent);
		const ImU32 accent = !live
			? ColU32A(Config::menu_accent, 0.55f)
			: (defused ? IM_COL32(80, 200, 120, 230)
				: (urgent ? ColU32(Config::widget_bomb_urgent) : menuAccent));
		DrawPanel(dl, a, b, accent);

		const char siteCh = live
			? ((bomb.site == 0) ? 'A' : (bomb.site == 1) ? 'B' : '?')
			: '?';
		const ImVec2 badgeA(a.x + padX + 2.f, a.y + padY);
		DrawSiteBadge(dl, badgeA, badgeW, badgeH, siteCh, accent, !live);

		ImFont* font = ImGui::GetFont();
		const float bodyX = badgeA.x + badgeW + 10.f;

		char status[24];
		if (!live)
			std::snprintf(status, sizeof(status), "NOT PLANTED");
		else if (defused)
			std::snprintf(status, sizeof(status), "Defused");
		else if (bomb.defusing)
			std::snprintf(status, sizeof(status), "Defusing");
		else if (urgent)
			std::snprintf(status, sizeof(status), "Detonating");
		else
			std::snprintf(status, sizeof(status), "Bomb");
		const ImU32 statusCol = live ? accent : ColU32(Config::menu_text);
		DrawTextShadow(dl, font, 13.f, ImVec2(bodyX, a.y + padY + 1.f), statusCol, status);

		char timeBuf[16];
		if (!live)
			std::snprintf(timeBuf, sizeof(timeBuf), "--.-");
		else if (blow >= 0.f)
			std::snprintf(timeBuf, sizeof(timeBuf), "%.1f", blow);
		else
			std::snprintf(timeBuf, sizeof(timeBuf), "--.-");

		const float timePx = 22.f;
		const ImVec2 tsz = font->CalcTextSizeA(timePx, 9999.f, 0.f, timeBuf);
		const ImU32 timeCol = !live
			? ColU32(Config::menu_text)
			: (defused ? IM_COL32(80, 200, 120, 255)
				: (urgent ? ColU32(Config::widget_bomb_urgent) : ColU32(Config::menu_text)));
		DrawTextShadow(dl, font, timePx, ImVec2(bodyX, a.y + padY + 13.f), timeCol, timeBuf);

		if (live) {
			const ImVec2 ssz = font->CalcTextSizeA(13.f, 9999.f, 0.f, "s");
			DrawTextShadow(dl, font, 13.f,
				ImVec2(bodyX + tsz.x + 3.f, a.y + padY + 13.f + tsz.y - ssz.y - 2.f),
				ColU32(Config::menu_text), "s");
		}

		auto bombSettingsPopup = [&]() {
			if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				ImGui::OpenPopup("##bomb_widget_settings");
			if (BeginWidgetPopup("##bomb_widget_settings", "Bomb", accent)) {
				ImGui::TextUnformatted("Uses menu accent");
				WidgetColorEdit("Urgent", &Config::widget_bomb_urgent);
				ImGui::Checkbox("Show damage", &Config::widget_bomb_show_damage);
				ImGui::Checkbox("Show defuse", &Config::widget_bomb_show_defuse);
				if (WidgetResetButton())
					Config::widget_bomb_pos = ImVec2(-1.f, -1.f);
				EndWidgetPopup();
			}
		};

		float y = a.y + padY + badgeH + 8.f;
		const float fuseFull = 40.f;
		const float fuseT = (!live || blow < 0.f) ? 0.f : std::clamp(blow / fuseFull, 0.f, 1.f);
		DrawFuseBar(dl, ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 5.f), fuseT, accent);
		y += 12.f;

		if (showDefuse) {
			const bool defusing = bomb.defusing;
			const float defLen = (bomb.defuseLength >= 4.f && bomb.defuseLength <= 12.f)
				? bomb.defuseLength : 10.f;
			const float defLeft = bomb.defuseLeft;
			const float defT = (!defusing || defLeft < 0.f)
				? 0.f : std::clamp(defLeft / defLen, 0.f, 1.f);
			const bool canMake = defusing && defLeft >= 0.f
				&& (blow < 0.f || defLeft <= blow + 0.05f);
			const ImU32 defCol = !defusing
				? ColU32A(Config::menu_accent, 0.55f)
				: (canMake ? menuAccent : ColU32(Config::widget_bomb_urgent));

			char defLine[24];
			if (!defusing)
				std::snprintf(defLine, sizeof(defLine), "--.-");
			else if (defLeft < 0.f)
				std::snprintf(defLine, sizeof(defLine), "...");
			else if (!canMake)
				std::snprintf(defLine, sizeof(defLine), "NO TIME");
			else
				std::snprintf(defLine, sizeof(defLine), "%.1fs", defLeft);
			const ImVec2 dsz = font->CalcTextSizeA(14.f, 9999.f, 0.f, defLine);
			DrawTextShadow(dl, font, 13.f, ImVec2(a.x + padX, y - 1.f),
				ColU32(Config::menu_text), "DEFUSE");
			DrawTextShadow(dl, font, 14.f, ImVec2(b.x - padX - dsz.x, y - 1.f), defCol, defLine);
			y += 16.f;
			DrawFuseBar(dl, ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 4.f), defT, defCol);
			y += 9.f;
		}

		if (showDmg) {
			Vector_t localPos{};
			int hp = 100;
			int armor = 0;
			bool havePos = false;
			if (cached_local.active && cached_local.alive && cached_local.health > 0
				&& std::isfinite(cached_local.position.x)) {
				localPos = cached_local.position;
				hp = cached_local.health;
				armor = cached_local.armor;
				havePos = true;
			} else if (H::SessionEntityReady()) {
				if (C_CSPlayerPawn* local = H::SafeLocalAlive()) {
					__try {
						hp = local->getHealth();
						if (hp > 0 && hp <= 200) {
							armor = Mem::ClampArmor(local->m_ArmorValue());
							localPos = local->getPosition();
							if (!std::isfinite(localPos.x))
								localPos = local->m_vOldOrigin();
							havePos = std::isfinite(localPos.x) && std::isfinite(localPos.y);
						}
					} __except (EXCEPTION_EXECUTE_HANDLER) {
						havePos = false;
					}
				}
			}

			float dmg = 0.f;
			bool haveDmg = false;
			if (havePos && std::isfinite(bomb.position.x)) {
				const float dx = bomb.position.x - localPos.x;
				const float dy = bomb.position.y - localPos.y;
				const float dz = bomb.position.z - localPos.z;
				dmg = EstimateBombDamage(std::sqrt(dx * dx + dy * dy + dz * dz), armor);
				haveDmg = true;
			}

			const bool lethal = haveDmg && dmg + 0.5f >= static_cast<float>(hp);
			char right[24];
			if (!haveDmg)
				std::snprintf(right, sizeof(right), "--");
			else if (dmg < 1.f)
				std::snprintf(right, sizeof(right), "SAFE");
			else if (lethal)
				std::snprintf(right, sizeof(right), "LETHAL");
			else
				std::snprintf(right, sizeof(right), "~%.0f", dmg);

			const ImU32 dmgCol = !haveDmg
				? ColU32(Config::menu_text)
				: (lethal ? ColU32(Config::widget_bomb_urgent)
					: (dmg < 1.f ? menuAccent : ColU32(Config::menu_text)));
			DrawTextShadow(dl, font, 13.f, ImVec2(a.x + padX, y),
				ColU32(Config::menu_text), "DAMAGE");
			const ImVec2 rsz = font->CalcTextSizeA(14.f, 9999.f, 0.f, right);
			DrawTextShadow(dl, font, 14.f, ImVec2(b.x - padX - rsz.x, y), dmgCol, right);
		}

		bombSettingsPopup();
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

struct SpecEntry {
	char name[64]{};
	std::uint64_t steamId = 0;
	std::uint8_t mode = 0; // 2 = 1st person (In-Eye), 3 = 3rd person (Chase)
};

// ObserverMode_t (cs2 dump macros.hpp): NONE=0 FIXED=1 IN_EYE=2 CHASE=3 ROAMING=4
// IDA cs_observer_observerservices: spec_mode 2 then spec_player slot - only 2/3 watch a player.
constexpr std::uint8_t kObsInEye = 2;
constexpr std::uint8_t kObsChase = 3;

C_BaseEntity* ResolveHandle(const CBaseHandle& h) {
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	C_BaseEntity* e = nullptr;
	__try { e = I::GameEntity->Instance->Get<C_BaseEntity>(h); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!e || !Mem::ValidEntity(e))
		return nullptr;
	return e;
}

CPlayer_ObserverServices* ReadObserverServices(C_CSPlayerPawn* pawn) {
	if (!pawn || !Mem::ValidEntity(pawn))
		return nullptr;
	CPlayer_ObserverServices* obs = nullptr;
	__try { obs = pawn->m_pObserverServices(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { obs = nullptr; }
	if (!obs || !Mem::Valid(obs, 0x50))
		return nullptr;
	return obs;
}

// Dump: m_hObserverTarget = CHandle<C_BaseEntity> (player pawn). Serial-checked Get, not index.
C_BaseEntity* ResolveWatchTarget(const CBaseHandle& h) {
	C_BaseEntity* e = ResolveHandle(h);
	if (!e)
		return nullptr;
	bool isCtrl = false;
	__try { isCtrl = e->IsPlayerController(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { isCtrl = false; }
	if (!isCtrl)
		return e;
	auto* ctrl = reinterpret_cast<CCSPlayerController*>(e);
	CBaseHandle hp{};
	__try { hp = ctrl->m_hPlayerPawn(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hp = CBaseHandle{}; }
	if (C_BaseEntity* pawn = ResolveHandle(hp))
		return pawn;
	return e;
}

bool WatchingLocalPlayer(CPlayer_ObserverServices* obs, C_BaseEntity** cands, int nCand, std::uint8_t* outMode = nullptr) {
	if (!obs || !cands || nCand <= 0)
		return false;
	std::uint8_t mode = 0;
	CBaseHandle target{};
	__try {
		mode = obs->m_iObserverMode();
		target = obs->m_hObserverTarget();
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (mode != kObsInEye && mode != kObsChase)
		return false;
	if (outMode)
		*outMode = mode;
	if (!target.valid())
		return false;
	C_BaseEntity* t = ResolveWatchTarget(target);
	for (int i = 0; i < nCand; ++i) {
		if (!cands[i])
			continue;
		if (t && t == cands[i])
			return true;
		CBaseHandle ch{};
		__try { ch = cands[i]->handle(); } __except (EXCEPTION_EXECUTE_HANDLER) { ch = CBaseHandle{}; }
		if (ch.valid() && ch.index() == target.index())
			return true;
	}
	return false;
}

// Living player pawns keep leftover observer services (IN_EYE + last target).
// Deathcam / spec: m_hPawn == m_hObserverPawn even when m_bPawnIsAlive still lags true.
bool ControllerIsPlaying(CCSPlayerController* ctrl) {
	if (!ctrl)
		return false;
	CBaseHandle hPlayer{};
	CBaseHandle hPawn{};
	CBaseHandle hObs{};
	__try {
		hPlayer = ctrl->m_hPlayerPawn();
		hPawn = ctrl->m_hPawn();
		hObs = ctrl->m_hObserverPawn();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (hObs.valid() && hPawn.valid() && hPawn == hObs)
		return false;

	const CBaseHandle hCheck = hPlayer.valid() ? hPlayer : hPawn;
	if (!hCheck.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return false;
	C_CSPlayerPawn* pp = nullptr;
	__try { pp = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hCheck); }
	__except (EXCEPTION_EXECUTE_HANDLER) { pp = nullptr; }
	if (!pp || !Mem::ValidEntity(pp))
		return false;
	int hp = 0;
	std::uint8_t life = 1;
	__try {
		hp = pp->m_iHealth();
		life = pp->m_lifeState();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return hp > 0 && life == 0;
}

CPlayer_ObserverServices* ObserverOfController(CCSPlayerController* ctrl) {
	if (!ctrl || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	if (ControllerIsPlaying(ctrl))
		return nullptr;
	CBaseHandle hObs{};
	CBaseHandle hPawn{};
	__try {
		hObs = ctrl->m_hObserverPawn();
		hPawn = ctrl->m_hPawn();
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	CBaseHandle hs[2]{};
	int n = 0;
	if (hObs.valid())
		hs[n++] = hObs;
	// Dead/spec current pawn (corpse deathcam). Living player pawns already bailed.
	if (hPawn.valid() && hPawn != hObs)
		hs[n++] = hPawn;
	for (int i = 0; i < n; ++i) {
		C_CSPlayerPawn* pawn = nullptr;
		__try { pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hs[i]); }
		__except (EXCEPTION_EXECUTE_HANDLER) { pawn = nullptr; }
		if (CPlayer_ObserverServices* obs = ReadObserverServices(pawn))
			return obs;
	}
	return nullptr;
}

// Spectators watch your player pawn (m_hPlayerPawn / corpse). Never your observer pawn -
// that would list people watching who YOU watch.
C_BaseEntity* LocalPlayerPawnToWatch() {
	if (!I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (!local || !Mem::ValidEntity(local))
		return nullptr;

	CBaseHandle hCtrl{};
	__try { hCtrl = local->m_hController(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hCtrl = CBaseHandle{}; }

	if (hCtrl.valid()) {
		CCSPlayerController* ctrl = nullptr;
		__try { ctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl); }
		__except (EXCEPTION_EXECUTE_HANDLER) { ctrl = nullptr; }
		if (ctrl && Mem::ValidEntity(ctrl)) {
			CBaseHandle hPlayer{};
			__try { hPlayer = ctrl->m_hPlayerPawn(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { hPlayer = CBaseHandle{}; }
			if (C_BaseEntity* pawn = ResolveHandle(hPlayer))
				return pawn;
		}
	}

	// Alive + handle miss: GetLocalPlayer(0) is the player pawn.
	if (H::SafeLocalAlive())
		return local;
	return nullptr;
}

int CollectControllerSlots(int* out, int maxOut) {
	if (!out || maxOut <= 0)
		return 0;
	int n = SdkPrioA::CopyControllerIndices(out, maxOut);
	if (n > 0)
		return n;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return 0;
	const int nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
	const int seedMax = (nMaxRaw < 72) ? nMaxRaw : 72;
	for (int i = 1; i <= seedMax && n < maxOut; ++i) {
		void* entRaw = nullptr;
		__try { entRaw = I::GameEntity->Instance->Get(i); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		auto* ent = reinterpret_cast<C_BaseEntity*>(entRaw);
		if (!Mem::ValidEntity(ent))
			continue;
		bool isCtrl = false;
		__try {
			CEntityIdentity* id = nullptr;
			if (!Mem::ReadField(ent, Offset::m_pEntity(), id) || !id || !Mem::Valid(id, 0x28))
				id = ent->m_pEntityIdentity();
			if (id && Mem::Valid(id, 0x28)) {
				const char* designer = nullptr;
				if (!Mem::ReadField(id, Offset::m_designerName(), designer) || !designer)
					designer = id->m_designerName();
				if (designer && Mem::IsReadable(designer, 2) && designer[0]
					&& (std::strcmp(designer, "cs_player_controller") == 0
						|| std::strstr(designer, "player_controller") != nullptr))
					isCtrl = true;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			isCtrl = false;
		}
		if (isCtrl)
			out[n++] = i;
	}
	return n;
}

int CollectSpectators(SpecEntry* out, int maxOut, int* totalOut) {
	if (totalOut)
		*totalOut = 0;
	if (!out || maxOut <= 0)
		return 0;
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return 0;

	C_BaseEntity* localPlayerPawn = LocalPlayerPawnToWatch();
	if (!localPlayerPawn)
		return 0;

	C_BaseEntity* watchCands[4]{};
	int nCand = 0;
	watchCands[nCand++] = localPlayerPawn;
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (nCand < 4 && local && Mem::ValidEntity(local)
		&& reinterpret_cast<C_BaseEntity*>(local) != localPlayerPawn)
		watchCands[nCand++] = reinterpret_cast<C_BaseEntity*>(local);

	CBaseHandle hCtrl{};
	if (local) {
		__try { hCtrl = local->m_hController(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { hCtrl = CBaseHandle{}; }
	}
	if (hCtrl.valid()) {
		CCSPlayerController* lctrl = nullptr;
		__try { lctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl); }
		__except (EXCEPTION_EXECUTE_HANDLER) { lctrl = nullptr; }
		if (lctrl && Mem::ValidEntity(lctrl) && !ControllerIsPlaying(lctrl)) {
			CBaseHandle hObs{};
			CBaseHandle hPawn{};
			__try {
				hObs = lctrl->m_hObserverPawn();
				hPawn = lctrl->m_hPawn();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				hObs = CBaseHandle{};
				hPawn = CBaseHandle{};
			}
			if (nCand < 4) {
				if (C_BaseEntity* op = ResolveHandle(hObs))
					watchCands[nCand++] = op;
			}
			if (nCand < 4 && hPawn != hObs) {
				if (C_BaseEntity* pp = ResolveHandle(hPawn))
					watchCands[nCand++] = pp;
			}
		}
	}

	int ctrlIdx[SdkPrioA::kMaxTrackedControllers]{};
	const int nCtrl = CollectControllerSlots(ctrlIdx, SdkPrioA::kMaxTrackedControllers);
	int count = 0;
	int total = 0;
	for (int ci = 0; ci < nCtrl; ++ci) {
		const int i = ctrlIdx[ci];
		if (i <= 0)
			continue;
		C_BaseEntity* Entity = nullptr;
		__try { Entity = I::GameEntity->Instance->Get(i); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!Mem::ValidEntity(Entity))
			continue;

		auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
		bool isLocal = false;
		__try { isLocal = Controller->IsLocalPlayer(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (isLocal)
			continue;
		if (ControllerIsPlaying(Controller))
			continue;

		CPlayer_ObserverServices* obs = ObserverOfController(Controller);
		std::uint8_t specMode = 0;
		if (!WatchingLocalPlayer(obs, watchCands, nCand, &specMode))
			continue;

		++total;
		if (count >= maxOut)
			continue;

		SpecEntry& e = out[count];
		e.steamId = 0;
		e.name[0] = '\0';
		e.mode = specMode;
		__try { e.steamId = Controller->m_steamID(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { e.steamId = 0; }
		if (!Controller->ReadSanitizedName(e.name, sizeof(e.name)) || !e.name[0])
			std::snprintf(e.name, sizeof(e.name), "Player");
		++count;
	}
	if (totalOut)
		*totalOut = total;
	return count;
}

void DrawLetterAvatar(ImDrawList* dl, ImVec2 center, float radius, const char* name, ImU32 /*accent*/) {
	dl->AddCircleFilled(center, radius, IM_COL32(36, 38, 44, 240), 12);
	char letter[2] = { '?', '\0' };
	if (name && name[0]) {
		char c = name[0];
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
		letter[0] = c;
	}
	const ImVec2 ts = ImGui::CalcTextSize(letter);
	dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
		ColU32A(Config::menu_text_muted, 0.95f), letter);
}

void RenderSpectatorList(bool menuOpen) {
	if (!Config::widget_spectators)
		return;

	static SpecEntry s_specs[16]{};
	static int s_n = 0;
	static int s_total = 0;
	static float s_nextRefresh = 0.f;

	int maxShow = Config::widget_spectators_max;
	if (maxShow < 1) maxShow = 1;
	if (maxShow > 16) maxShow = 16;

	const float now = static_cast<float>(ImGui::GetTime());
	// Throttle entity walk (~8 Hz); refresh immediately when menu opens for drag UX
	if (menuOpen || now >= s_nextRefresh) {
		s_nextRefresh = now + 0.12f;
		s_n = CollectSpectators(s_specs, maxShow, &s_total);
	}
	const int n = s_n;
	const int total = s_total;
	if (n <= 0 && !menuOpen)
		return;

	const float padX = 10.f;
	const float padY = 8.f;
	const float headerH = 16.f;
	const float rowH = 22.f;
	const float rowGap = 1.f;
	const float avatarR = 7.f;
	const bool showAvatars = Config::widget_spectators_show_avatars;
	const float panelW = 204.f;
	const int drawRows = (n > 0) ? n : 1;
	const float bodyH = (float)drawRows * rowH + (float)(drawRows - 1) * rowGap;
	const float panelH = padY + headerH + 5.f + bodyH + padY;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImVec2 pos = ResolvePos(Config::widget_spectators_pos, panelW, panelH,
		ImVec2(ds.x - panelW - 14.f, 58.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_spectators", nullptr, flags);
	{
		ImGui::InvisibleButton("##spec_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_spectators_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);
		const ImU32 accent = ColU32(WidgetAccent(Config::widget_spectators_accent));
		DrawPanel(dl, a, b, accent);

		const float titleX = a.x + padX;
		const float titleY = a.y + padY;
		dl->AddText(ImVec2(titleX, titleY),
			ColU32A(Config::menu_text_muted, 0.90f), "Spectators");

		{
			char badge[16];
			if (total > n)
				std::snprintf(badge, sizeof(badge), "%d+", total);
			else
				std::snprintf(badge, sizeof(badge), "%d", total);
			const ImVec2 bsz = ImGui::CalcTextSize(badge);
			dl->AddText(ImVec2(b.x - padX - bsz.x, titleY),
				total > 0
					? ColU32(Config::menu_text)
					: ColU32A(Config::menu_text_muted, 0.70f),
				badge);
		}

		DrawHeaderSep(dl, a, b, padX, a.y + padY + headerH);

		float y = a.y + padY + headerH + 3.f;
		if (n <= 0) {
			dl->AddText(ImVec2(a.x + padX, y + (rowH - ImGui::GetFontSize()) * 0.5f),
				ColU32A(Config::menu_text_muted, 0.70f), "None");
		} else {
			for (int i = 0; i < n; ++i) {
				const SpecEntry& s = s_specs[i];
				const ImVec2 rowA(a.x + padX, y);
				const ImVec2 rowB(b.x - padX, y + rowH);
				const float cy = y + rowH * 0.5f;

				float textX = rowA.x;

				if (showAvatars) {
					const ImVec2 av(rowA.x + avatarR, cy);
					const ImTextureID tex = SteamAvatar::Get(s.steamId, pDevice);
					if (tex != ImTextureID_Invalid) {
						dl->AddImageRounded(ImTextureRef(tex),
							ImVec2(av.x - avatarR, av.y - avatarR),
							ImVec2(av.x + avatarR, av.y + avatarR),
							ImVec2(0, 0), ImVec2(1, 1),
							IM_COL32(255, 255, 255, 230), avatarR);
					} else {
						DrawLetterAvatar(dl, av, avatarR, s.name, accent);
					}
					textX = av.x + avatarR + 6.f;
				}

				// Mode tag (1ST person POV vs 3RD person chase)
				const char* modeStr = (s.mode == kObsInEye) ? "1ST" : "3RD";
				const ImU32 modeCol = (s.mode == kObsInEye)
					? IM_COL32(160, 210, 240, 230)
					: IM_COL32(200, 190, 230, 230);
				const ImVec2 msz = ImGui::CalcTextSize(modeStr);
				const ImVec2 mMin(rowB.x - msz.x - 8.f, cy - msz.y * 0.5f - 1.f);
				const ImVec2 mMax(rowB.x, cy + msz.y * 0.5f + 1.f);
				dl->AddRectFilled(mMin, mMax, IM_COL32(255, 255, 255, 18), 3.f);
				dl->AddText(ImVec2(mMin.x + 4.f, cy - msz.y * 0.5f), modeCol, modeStr);

				const float maxNameW = (std::max)(24.f, mMin.x - 6.f - textX);
				char clipped[64];
				std::snprintf(clipped, sizeof(clipped), "%s", s.name);
				ClipTextToWidth(clipped, sizeof(clipped), maxNameW);
				const ImVec2 nsz = ImGui::CalcTextSize(clipped);
				dl->AddText(ImVec2(textX, cy - nsz.y * 0.5f),
					ColU32(Config::menu_text), clipped);

				y += rowH + rowGap;
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##spec_widget_settings");
		if (BeginWidgetPopup("##spec_widget_settings", "Spectators", accent)) {
			if (!Config::menu_widgets_follow)
				WidgetColorEdit("Accent", &Config::widget_spectators_accent);
			ImGui::Checkbox("Show avatars", &Config::widget_spectators_show_avatars);
			ImGui::TextUnformatted("Max shown");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::SliderInt("##spec_max", &Config::widget_spectators_max, 1, 16);
			if (WidgetResetButton())
				Config::widget_spectators_pos = ImVec2(-1.f, -1.f);
			EndWidgetPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

void RenderRadarWidget(bool menuOpen) {
	if (!Config::widget_radar)
		return;

	const bool square = Config::widget_radar_shape == 1;
	const float diameter = std::clamp(Config::widget_radar_size, 90.f, 280.f);
	const float half = diameter * 0.5f;
	const float pad = 6.f;
	const float panelW = diameter + pad * 2.f;
	const float panelH = diameter + pad * 2.f;
	ImVec2 pos = ResolvePos(Config::widget_radar_pos, panelW, panelH,
		ImVec2(ImGui::GetIO().DisplaySize.x - panelW - 18.f, 52.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_radar", nullptr, flags);
	{
		ImGui::InvisibleButton("##radar_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_radar_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 win = ImGui::GetWindowPos();
		const ImVec2 center(win.x + panelW * 0.5f, win.y + panelH * 0.5f);
		const ImU32 accent = ColU32(WidgetAccent(Config::widget_radar_accent));
		const float rOuter = half;
		const ImVec2 a(center.x - rOuter, center.y - rOuter);
		const ImVec2 b(center.x + rOuter, center.y + rOuter);

		const float glass = std::clamp(Config::menu_glass, 0.f, 1.f);
		ImVec4 plateC = Config::menu_bg;
		plateC.w = std::clamp(Config::menu_opacity, 0.72f, 0.94f);
		ImVec4 brd = Config::menu_border;
		brd.w = std::clamp(brd.w, 0.10f, 0.20f);
		const ImU32 plate = ColU32(plateC);
		const ImU32 border = ColU32(brd);
		const ImU32 grid = IM_COL32(255, 255, 255, 18);
		const ImU32 gridSoft = IM_COL32(255, 255, 255, 12);
		const int sheenA = static_cast<int>(22.f * glass);

		if (square) {
			DrawPanel(dl, a, b, accent);
		} else {
			dl->AddCircleFilled(ImVec2(center.x, center.y + 2.f), rOuter, IM_COL32(0, 0, 0, 88), 48);
			dl->AddCircleFilled(center, rOuter, plate, 48);
			dl->PushClipRect(a, b, true);
			dl->AddCircleFilled(ImVec2(center.x, center.y - rOuter * 0.45f), rOuter * 0.72f,
				IM_COL32(255, 255, 255, sheenA), 28);
			dl->PopClipRect();
			dl->AddCircle(center, rOuter, border, 48, 1.f);
			dl->AddCircle(center, rOuter, IM_COL32(255, 255, 255, sheenA), 48, 1.f);
		}

		const float inset = 5.f;
		const float usable = rOuter - inset;
		if (square) {
			dl->AddLine(ImVec2(center.x, a.y + inset), ImVec2(center.x, b.y - inset), grid, 1.f);
			dl->AddLine(ImVec2(a.x + inset, center.y), ImVec2(b.x - inset, center.y), grid, 1.f);
		} else {
			dl->AddCircle(center, usable * 0.5f, gridSoft, 32, 1.f);
			dl->AddLine(ImVec2(center.x, center.y - usable), ImVec2(center.x, center.y + usable), grid, 1.f);
			dl->AddLine(ImVec2(center.x - usable, center.y), ImVec2(center.x + usable, center.y), grid, 1.f);
		}

		{
			const float s = 4.5f;
			const ImVec2 tip(center.x, center.y - s);
			const ImVec2 bl(center.x - s * 0.7f, center.y + s * 0.5f);
			const ImVec2 br(center.x + s * 0.7f, center.y + s * 0.5f);
			dl->AddTriangleFilled(tip, bl, br, accent);
		}

		constexpr float kRange = 2200.f;
		const float scale = usable / kRange;
		const float maxR = usable - 1.f;
		const Vector_t localPos = cached_local.position;

		float yawRad = 0.f;
		if (Input::GetViewAngles && Input::viewAngleContext) {
			const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
			if (viewPtr) {
				__try {
					const Vector_t va = *reinterpret_cast<const Vector_t*>(viewPtr);
					constexpr float kDeg2Rad = 3.14159265358979323846f / 180.f;
					yawRad = va.y * kDeg2Rad;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					yawRad = 0.f;
				}
			}
		}
		const float cosY = std::cos(yawRad);
		const float sinY = std::sin(yawRad);

		for (const auto& p : cached_players) {
			if (p.health < 1)
				continue;
			const float wdx = p.position.x - localPos.x;
			const float wdy = p.position.y - localPos.y;
			const float fwd = wdx * cosY + wdy * sinY;
			const float right = wdx * sinY - wdy * cosY;
			float dx = right * scale;
			float dy = -fwd * scale;

			if (square) {
				// Clamp into square bounds
				dx = std::clamp(dx, -maxR, maxR);
				dy = std::clamp(dy, -maxR, maxR);
			} else {
				const float dist = std::sqrt(dx * dx + dy * dy);
				if (dist > maxR && dist > 0.001f) {
					const float t = maxR / dist;
					dx *= t;
					dy *= t;
				}
			}

			const ImVec2 pt(center.x + dx, center.y + dy);
			const bool isEnemy = (p.type == enemy);
			const ImU32 fill = isEnemy
				? IM_COL32(230, 80, 80, 245)
				: IM_COL32(100, 170, 240, 235);

			if (square) {
				const float hs = 2.4f;
				dl->AddRectFilled(ImVec2(pt.x - hs, pt.y - hs),
					ImVec2(pt.x + hs, pt.y + hs), fill, 1.f);
				if (!p.visible && isEnemy)
					dl->AddRect(ImVec2(pt.x - hs - 1.f, pt.y - hs - 1.f),
						ImVec2(pt.x + hs + 1.f, pt.y + hs + 1.f),
						IM_COL32(255, 255, 255, 40), 1.f, 0, 1.f);
			} else {
				dl->AddCircleFilled(pt, 2.4f, fill, 10);
				if (!p.visible && isEnemy)
					dl->AddCircle(pt, 3.6f, IM_COL32(255, 255, 255, 35), 10, 1.f);
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##radar_widget_settings");
		if (BeginWidgetPopup("##radar_widget_settings", "RADAR", accent, 210.f)) {
			ImGui::TextUnformatted("Shape");
			ImGui::SetNextItemWidth(-1.f);
			const char* shapes[] = { "Circle", "Square" };
			int shape = Config::widget_radar_shape;
			if (ImGui::Combo("##rshape", &shape, shapes, IM_ARRAYSIZE(shapes)))
				Config::widget_radar_shape = shape;

			ImGui::TextUnformatted("Size");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::SliderFloat("##rsz", &Config::widget_radar_size, 90.f, 280.f, "%.0f");
			if (!Config::menu_widgets_follow)
				WidgetColorEdit("Accent", &Config::widget_radar_accent);
			if (WidgetResetButton())
				Config::widget_radar_pos = ImVec2(-1.f, -1.f);
			EndWidgetPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

} // namespace

void Render() {
	if (H::SessionMapLeaving() || !H::SessionEntityReady() || H::SessionPostMatch())
		return;
	const bool menuOpen = g_bMenuOpen;
	__try { RenderKeybindList(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.keybinds", GetExceptionCode()); }
	__try { RenderBombWidget(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.bomb", GetExceptionCode()); }
	__try { RenderSpectatorList(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.specs", GetExceptionCode()); }
	__try { RenderRadarWidget(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.radar", GetExceptionCode()); }
}

} // namespace Widgets

