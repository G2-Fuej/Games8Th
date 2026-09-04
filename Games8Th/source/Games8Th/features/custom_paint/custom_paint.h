#pragma once
#include <cstdint>
#include "../../../../external/imgui/imgui.h"
#include <array>
#include <vector>

namespace CustomPaint {

// IDA-verified 2026-08-23 via idalib-mcp (client.dll):
// build_material midhook @ 0x1807C7636 : 48 8D 15 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 48 8B D0 48 8D 8B
//   ctx.r8 = vector<CompositeMaterialInputLooseVariable_t>, ctx.rbx = weapon
// g_append @ 0x1807C7363 : E8 ? ? ? ? 0F 28 B4 24 ? ? ? ? 4C 39 A5
//   void append(vector*, looseVar*)
// Struct size 0x288 per UC + s2v.app, validated via heap corruption fix (4EBIK):
//   name @ +0x8 (CUtlString ptr), type @ +0x40, color @ +0x90
// UC silvhook: LOOSE_VAR_COLOR4 = 9, colors g_vColor0..3
// UC soar1337: gloves use g_vColorTint + LOOSE_VAR_FLOAT3
constexpr int kLooseVarColor4 = 9;
constexpr int kLooseVarFloat3 = 6; // for gloves g_vColorTint (LOOSE_VAR_FLOAT3/VECTOR3 is 6 per CS2 keyvalues parser)
constexpr size_t kLooseVarSize = 0x288;
constexpr size_t kOffName = 0x0;       // CUtlString name is at offset 0x0 in loose variable struct
constexpr size_t kOffType = 0x40;      // int32
constexpr size_t kOffColor4 = 0x90;    // uint32_t rgba / abgr color value
constexpr size_t kOffFloat3 = 0x48;    // float values for FLOAT3 / VECTOR3 (e.g. g_vSticker0Offset at +0x48)

struct WeaponTintEntry {
    int def = 0;
    int paint = 0;
    ImVec4 cols[4];
};

struct GloveTintEntry {
    int def = 0;
    int paint = 0;
    ImVec4 col;
};

bool Init();
void Shutdown();
bool IsReady();

// Per-skin tint storage - every skin (def+paint) has its own 4 colors. Picker shows/edits the selected skin only.
void SetWeaponTint(int def, int paint, const ImVec4 cols[4]);
bool GetWeaponTint(int def, int paint, ImVec4 out[4]);
void SetGloveTint(int def, int paint, const ImVec4& col);
bool GetGloveTint(int def, int paint, ImVec4& out);
void ClearAllTints();

std::vector<WeaponTintEntry> GetWeaponTintSnapshot();
std::vector<GloveTintEntry> GetGloveTintSnapshot();

} // namespace CustomPaint

