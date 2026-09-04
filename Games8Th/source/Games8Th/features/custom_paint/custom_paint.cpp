#include "custom_paint.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"

#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <array>

#include "safetyhook.hpp"

namespace CustomPaint {

using AppendFn = void(__fastcall*)(void* vec, const void* looseVar);

// Per-skin tint storage - every skin (def+paint) has its own 4 colors
static std::unordered_map<uint32_t, std::array<ImVec4,4>> g_weaponTints;
static std::unordered_map<uint32_t, ImVec4> g_gloveTints;
static SRWLOCK g_tintLock = SRWLOCK_INIT;
static uint32_t SkinKey(int def, int paint) { return (static_cast<uint32_t>(def) << 16) | (static_cast<uint32_t>(paint) & 0xFFFF); }

namespace {

AppendFn g_append = nullptr;
void* g_buildMaterialTarget = nullptr; // midhook target (lea rdx / lea rcx site)
SafetyHookMid g_midHook{};

bool g_ready = false;

using SetUtlStringFn = void(__fastcall*)(void* pThis, const char* str);
static SetUtlStringFn g_setUtlString = nullptr;

static void SetLooseVarName(void* pVar, const char* name) {
    if (!pVar || !name) return;
    if (!g_setUtlString) {
        HMODULE tier0 = GetModuleHandleA("tier0.dll");
        if (tier0) {
            g_setUtlString = reinterpret_cast<SetUtlStringFn>(GetProcAddress(tier0, "?Set@CUtlString@@QEAAXPEBD@Z"));
        }
    }
    if (g_setUtlString) {
        g_setUtlString(pVar, name);
    }
}

static char* Tier0Dup(const char* str) {
    if (!str) return nullptr;
    return _strdup(str);
}

// Pack ImVec4 (0..1) to uint32_t rgba / abgr
uint32_t PackColor4(const ImVec4& col) {
    auto clamp01 = [](float v) -> float { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    const uint8_t r = static_cast<uint8_t>(clamp01(col.x) * 255.f);
    const uint8_t g = static_cast<uint8_t>(clamp01(col.y) * 255.f);
    const uint8_t b = static_cast<uint8_t>(clamp01(col.z) * 255.f);
    const uint8_t a = static_cast<uint8_t>(clamp01(col.w) * 255.f);
    return (static_cast<uint32_t>(r)) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

// Midhook: ctx.r8 = vector, ctx.rbx = weapon (UC silvhook) - but also try rcx/rdx for robustness
} // namespace

// Per-skin tint storage - every skin (def+paint) has its own 4 colors
void CustomPaint::SetWeaponTint(int def, int paint, const ImVec4 cols[4]) {
    if (!cols) return;
    AcquireSRWLockExclusive(&g_tintLock);
    g_weaponTints[SkinKey(def, paint)] = { cols[0], cols[1], cols[2], cols[3] };
    ReleaseSRWLockExclusive(&g_tintLock);
}
bool CustomPaint::GetWeaponTint(int def, int paint, ImVec4 out[4]) {
    AcquireSRWLockShared(&g_tintLock);
    auto it = g_weaponTints.find(SkinKey(def, paint));
    bool ok = false;
    if (it != g_weaponTints.end()) {
        out[0] = it->second[0]; out[1] = it->second[1]; out[2] = it->second[2]; out[3] = it->second[3];
        ok = true;
    }
    ReleaseSRWLockShared(&g_tintLock);
    return ok;
}
void CustomPaint::SetGloveTint(int def, int paint, const ImVec4& col) {
    AcquireSRWLockExclusive(&g_tintLock);
    g_gloveTints[SkinKey(def, paint)] = col;
    ReleaseSRWLockExclusive(&g_tintLock);
}
bool CustomPaint::GetGloveTint(int def, int paint, ImVec4& out) {
    AcquireSRWLockShared(&g_tintLock);
    auto it = g_gloveTints.find(SkinKey(def, paint));
    bool ok = false;
    if (it != g_gloveTints.end()) { out = it->second; ok = true; }
    ReleaseSRWLockShared(&g_tintLock);
    return ok;
}
void CustomPaint::ClearAllTints() {
    AcquireSRWLockExclusive(&g_tintLock);
    g_weaponTints.clear();
    g_gloveTints.clear();
    ReleaseSRWLockExclusive(&g_tintLock);
}

std::vector<WeaponTintEntry> CustomPaint::GetWeaponTintSnapshot() {
    std::vector<WeaponTintEntry> res;
    AcquireSRWLockShared(&g_tintLock);
    res.reserve(g_weaponTints.size());
    for (const auto& kv : g_weaponTints) {
        WeaponTintEntry e;
        e.def = static_cast<int>(kv.first >> 16);
        e.paint = static_cast<int>(kv.first & 0xFFFF);
        e.cols[0] = kv.second[0];
        e.cols[1] = kv.second[1];
        e.cols[2] = kv.second[2];
        e.cols[3] = kv.second[3];
        res.push_back(e);
    }
    ReleaseSRWLockShared(&g_tintLock);
    return res;
}

std::vector<GloveTintEntry> CustomPaint::GetGloveTintSnapshot() {
    std::vector<GloveTintEntry> res;
    AcquireSRWLockShared(&g_tintLock);
    res.reserve(g_gloveTints.size());
    for (const auto& kv : g_gloveTints) {
        GloveTintEntry e;
        e.def = static_cast<int>(kv.first >> 16);
        e.paint = static_cast<int>(kv.first & 0xFFFF);
        e.col = kv.second;
        res.push_back(e);
    }
    ReleaseSRWLockShared(&g_tintLock);
    return res;
}

namespace {
static bool GetWeaponPaintInfo(void* weapon, int& outDef, int& outPaint) {
    outDef = 0; outPaint = 0;
    if (!weapon || !Mem::IsReadable(weapon, 0x10)) return false;
    __try {
        const uint32_t paintOff = SchemaFinder::Get(hash_32_fnv1a_const("C_EconEntity->m_nFallbackPaintKit"));
        const uint32_t usePaintOff = paintOff ? paintOff : 0x1680;
        const int* paintPtr = reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(weapon) + usePaintOff);
        if (Mem::IsReadable(paintPtr, sizeof(int))) {
            int p = *paintPtr;
            if (p >= 0 && p < 10000) outPaint = p;
        }
        // Read item definition index via schema offset for C_EconItemView->m_iItemDefinitionIndex
        const uint32_t attrOff = SchemaFinder::Get(hash_32_fnv1a_const("C_EconEntity->m_AttributeManager"));
        const uint32_t itemOff = SchemaFinder::Get(hash_32_fnv1a_const("C_AttributeContainer->m_Item"));
        const uint32_t defOff  = SchemaFinder::Get(hash_32_fnv1a_const("C_EconItemView->m_iItemDefinitionIndex"));
        if (attrOff && itemOff && defOff) {
            auto* pDef = reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(weapon) + attrOff + itemOff + defOff);
            if (Mem::IsReadable(pDef, sizeof(uint16_t))) {
                outDef = static_cast<int>(*pDef);
            }
        }
        return outPaint != 0 || outDef != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Verified IDA: vec is at RCX at call site, but R8 also holds it beforehand. Try both.
void MidBuildMaterial(safetyhook::Context& ctx) {
    __try {
        if (!Config::custom_paint_enabled && !Config::custom_paint_glove_enabled) return;
        if (!g_append) return;

        // Try multiple vec sources - UC says r8, but disasm shows rcx = vector
        void* vec = nullptr;
        void* cand[] = { reinterpret_cast<void*>(ctx.r8), reinterpret_cast<void*>(ctx.rcx), reinterpret_cast<void*>(ctx.rdx), reinterpret_cast<void*>(ctx.r9) };
        for (void* c : cand) {
            if (c && Mem::IsReadable(c, 16) && Mem::IsReadable(*reinterpret_cast<void**>(c), 8)) { vec = c; break; }
            if (c && Mem::IsReadable(c, 32)) { vec = c; break; }
        }
        if (!vec) return;

        void* weapon = reinterpret_cast<void*>(ctx.rbx);
        int wDef = 0, wPaint = 0;
        bool hasWeaponInfo = false;
        if (weapon) {
            hasWeaponInfo = GetWeaponPaintInfo(weapon, wDef, wPaint);
        }

        const bool isGloveItem = (wDef == 5027 || wDef == 5028 || (wDef >= 5030 && wDef <= 5035) || (wDef >= 5247 && wDef <= 5249));

        if (!isGloveItem) {
            // Weapon tint branch (g_vColor0..3)
            bool hasWeaponSkin = false;
            ImVec4 skinCols[4];
            if (hasWeaponInfo) {
                hasWeaponSkin = GetWeaponTint(wDef, wPaint, skinCols);
            }
            if (!hasWeaponSkin && wPaint != 0) {
                hasWeaponSkin = GetWeaponTint(0, wPaint, skinCols);
            }

            static const char* const kColorNames[4] = { "g_vColor0", "g_vColor1", "g_vColor2", "g_vColor3" };
            if (hasWeaponSkin) {
                for (int i = 0; i < 4; ++i) {
                    const ImVec4& c = skinCols[i];
                    bool isWhite = c.x >= 0.99f && c.y >= 0.99f && c.z >= 0.99f && c.w >= 0.99f;
                    if (isWhite) continue;
                    alignas(16) uint8_t var[kLooseVarSize]{}; memset(var, 0, kLooseVarSize);
                    SetLooseVarName(var + kOffName, kColorNames[i]);
                    *reinterpret_cast<int32_t*>(var + kOffType) = kLooseVarColor4;
                    *reinterpret_cast<uint32_t*>(var + kOffColor4) = PackColor4(c);
                    g_append(vec, var);
                }
            } else if (Config::custom_paint_enabled) {
                const ImVec4 cols[4] = { Config::custom_paint_color0, Config::custom_paint_color1, Config::custom_paint_color2, Config::custom_paint_color3 };
                for (int i = 0; i < 4; ++i) {
                    const ImVec4& c = cols[i];
                    bool isWhite = c.x >= 0.99f && c.y >= 0.99f && c.z >= 0.99f && c.w >= 0.99f;
                    if (isWhite) continue;
                    alignas(16) uint8_t var[kLooseVarSize]{}; memset(var, 0, kLooseVarSize);
                    SetLooseVarName(var + kOffName, kColorNames[i]);
                    *reinterpret_cast<int32_t*>(var + kOffType) = kLooseVarColor4;
                    *reinterpret_cast<uint32_t*>(var + kOffColor4) = PackColor4(c);
                    g_append(vec, var);
                }
            }
        } else {
            // Glove tint branch (g_vColorTint)
            ImVec4 gCol;
            bool hasGlove = false;
            if (hasWeaponInfo) {
                hasGlove = GetGloveTint(wDef, wPaint, gCol);
                if (!hasGlove && wPaint != 0)
                    hasGlove = GetGloveTint(0, wPaint, gCol);
            }
            if (!hasGlove && Config::custom_paint_glove_enabled) {
                gCol = Config::custom_paint_glove_color;
                hasGlove = true;
            }
            if (hasGlove) {
                bool isWhite = gCol.x >= 0.99f && gCol.y >= 0.99f && gCol.z >= 0.99f;
                if (!isWhite) {
                    alignas(16) uint8_t var[kLooseVarSize]{}; memset(var, 0, kLooseVarSize);
                    SetLooseVarName(var + kOffName, "g_vColorTint");
                    *reinterpret_cast<int32_t*>(var + kOffType) = kLooseVarFloat3;
                    float* f = reinterpret_cast<float*>(var + kOffFloat3);
                    f[0] = std::clamp(gCol.x, 0.f, 1.f);
                    f[1] = std::clamp(gCol.y, 0.f, 1.f);
                    f[2] = std::clamp(gCol.z, 0.f, 1.f);
                    f[3] = std::clamp(gCol.w, 0.f, 1.f);
                    g_append(vec, var);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

bool Init() {
    if (g_ready) return true;

    // Resolve g_append via E8 pattern (IDA 0x1807C7363 : E8 ? ? ? ? 0F 28 B4 24)
    // UC: E8 ? ? ? ? 0F 28 B4 24 ? ? ? ? 4C 39 A5 @ client.dll
    uint8_t* hit = M::FindPattern("client.dll", "E8 ? ? ? ? 0F 28 B4 24 ? ? ? ? 4C 39 A5");
    if (!hit) hit = M::FindPattern("client", "E8 ? ? ? ? 0F 28 B4 24 ? ? ? ? 4C 39 A5");
    if (hit) {
        g_append = reinterpret_cast<AppendFn>(M::GetAbsoluteAddress(hit, 1, 0));
        Con::Ok("CustomPaint g_append @ %p", g_append);
    } else {
        Con::PatternMiss("client", "CustomPaint g_append");
        return false;
    }

    // Resolve build_material midhook target: 48 8D 15 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 48 8B D0 48 8D 8B
    // This is inside sub_1807C7010 (RegenerateWeaponSkin) at 0x1807C7636
    g_buildMaterialTarget = M::FindPattern("client.dll", "48 8D 15 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 48 8B D0 48 8D 8B");
    if (!g_buildMaterialTarget) g_buildMaterialTarget = M::FindPattern("client", "48 8D 15 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 48 8B D0 48 8D 8B");
    if (!g_buildMaterialTarget) {
        Con::PatternMiss("client", "CustomPaint build_material");
        return false;
    }
    Con::Ok("CustomPaint build_material @ %p", g_buildMaterialTarget);

    // Create midhook - SafetyHook supports MidHook
    auto res = safetyhook::MidHook::create(g_buildMaterialTarget, MidBuildMaterial);
    if (!res) {
        Con::Error("CustomPaint MidHook create failed type=%d @ %p", (int)res.error().type, g_buildMaterialTarget);
        return false;
    }
    g_midHook = std::move(*res);
    g_ready = true;
    Con::Ok("CustomPaint MidHook installed (weapons g_vColor0-3 + gloves g_vColorTint)");
    return true;
}

void Shutdown() {
    g_midHook = {};
    g_append = nullptr;
    g_buildMaterialTarget = nullptr;
    g_ready = false;
}

bool IsReady() { return g_ready; }

} // namespace CustomPaint

