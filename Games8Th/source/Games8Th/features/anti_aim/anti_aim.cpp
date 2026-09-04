#include "anti_aim.h"
#include "../../config/config.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../interfaces/interfaces.h"
#include "../../hooks/hooks.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../offsets/offsets.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <random>

namespace AntiAim {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.0f / kPi;
constexpr float kKnifeThreatRangeSq = 350.f * 350.f; // velocity: avoid backstab radius

float NormalizeYaw(float value) {
    while (value > 180.f) value -= 360.f;
    while (value < -180.f) value += 360.f;
    return value;
}
float BinaryJitter(float lo, float hi) {
    static bool flip = false;
    flip = !flip;
    return flip ? lo : hi;
}
float RandomJitter(float lo, float hi) {
    static std::mt19937 rng{ std::random_device{}() };
    if (lo > hi) std::swap(lo, hi);
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}
float SwitchJitter(float lo, float hi) {
    static float value = 0.f;
    static bool initialized = false;
    static bool rising = true;
    if (lo > hi) std::swap(lo, hi);
    if (!initialized || value < lo || value > hi) { value = lo; initialized = true; rising = true; }
    if (rising) { value += 4.5f; if (value >= hi) { value = hi; rising = false; } }
    else { value -= 4.5f; if (value <= lo) { value = lo; rising = true; } }
    return value;
}
float ThirdWayJitter(float lo, float hi) {
    static int state = 0;
    if (lo > hi) std::swap(lo, hi);
    const float mid = (lo + hi) * 0.5f;
    const float result = state == 0 ? lo : (state == 1 ? mid : hi);
    state = (state + 1) % 3;
    return result;
}
float AngleTo(float dx, float dy) {
    return std::atan2f(dy, dx) * kRad2Deg;
}

// velocity-style avoid backstab: find nearest alive enemy holding a knife
// within 350 units; returns yaw pointing directly away from their position
// when a threat exists.
struct KnifeThreatResult {
    bool found = false;
    float awayYaw = 0.f;
};
KnifeThreatResult FindKnifeThreat(C_CSPlayerPawn* local, const Vector_t& localPos, uint8_t localTeam) {
    KnifeThreatResult out;
    if (!local || !I::GameEntity || !I::GameEntity->Instance)
        return out;

    int nMaxRaw = 0;
    __try {
        nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return out;
    }
    if (nMaxRaw <= 0)
        return out;
    const int nMax = (nMaxRaw > 128) ? 128 : nMaxRaw;

    float bestDistSq = kKnifeThreatRangeSq;
    Vector_t bestPos{};
    bool bestFound = false;

    for (int i = 1; i <= nMax; ++i) {
        void* entRaw = nullptr;
        __try {
            entRaw = I::GameEntity->Instance->Get(i);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        auto* ent = reinterpret_cast<C_BaseEntity*>(entRaw);
        if (!ent || !Mem::ValidEntity(ent))
            continue;

        char clsName[128]{};
        if (!Mem::SchemaClassName(ent, clsName, sizeof(clsName)))
            continue;
        if (HASH(clsName) != HASH("CCSPlayerPawn"))
            continue;

        auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(ent);
        if (pawn == local)
            continue;
        if (pawn->getTeam() == 0 || pawn->getTeam() == localTeam)
            continue;
        if (pawn->getHealth() < 1)
            continue;

        C_CSWeaponBase* wpn = nullptr;
        __try { wpn = pawn->GetActiveWeapon(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!wpn || !Mem::ValidEntity(wpn))
            continue;

        CCSWeaponBaseVData* vd = nullptr;
        __try { vd = wpn->Data(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!vd || !Mem::Valid(vd, 0x20))
            continue;
        int wtype = 0;
        __try { wtype = vd->m_WeaponType(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (wtype != 0) // CSWeaponType knife = 0
            continue;

        const Vector_t pos = pawn->getPosition();
        const float dx = pos.x - localPos.x;
        const float dy = pos.y - localPos.y;
        const float distSq = dx * dx + dy * dy;
        if (distSq > bestDistSq)
            continue;

        bestDistSq = distSq;
        bestPos = pos;
        bestFound = true;
    }

    if (bestFound) {
        out.found = true;
        out.awayYaw = AngleTo(bestPos.x - localPos.x, bestPos.y - localPos.y) - 180.f;
    }
    return out;
}
} // namespace

bool g_aa_active = false;
float g_realPitch = 0.f;
float g_realYaw = 0.f;

void OnCreateMove(CUserCmd* cmd) {
    static float previousRealPitch = 0.f, previousFakePitch = 0.f;
    static float previousRealYaw = 0.f, previousFakeYaw = 0.f;
    static bool hadFake = false;
    if (!cmd || !Config::anti_aim) { g_aa_active = false; hadFake = false; return; }
    CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
    if (!base || !base->pViewAngles) { g_aa_active = false; return; }
    if (Config::anti_aim_pitch_mode == Config::AA_PITCH_OFF && Config::anti_aim_yaw_mode == Config::AA_YAW_OFF) {
        g_aa_active = false; hadFake = false; return;
    }
    void* rules = SdkPrioA::GameRules();
    if (rules && Mem::IsReadable(rules, 0x50)) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(rules);
        if (bytes[0x40] || bytes[0x41]) { g_aa_active = false; return; }
    }
    if (cmd->nButtons.nValue & IN_USE) { g_aa_active = false; return; }
    const float rawPitch = base->pViewAngles->angValue.x;
    const float rawYaw = base->pViewAngles->angValue.y;
    float realPitch = std::clamp(rawPitch, -89.f, 89.f);
    float realYaw = rawYaw;
    if (hadFake && std::fabs(rawPitch - previousFakePitch) < 20.f)
        realPitch = std::clamp(previousRealPitch + rawPitch - previousFakePitch, -89.f, 89.f);
    if (hadFake && std::fabs(NormalizeYaw(rawYaw - previousFakeYaw)) < 45.f)
        realYaw = NormalizeYaw(previousRealYaw + NormalizeYaw(rawYaw - previousFakeYaw));
    g_realPitch = realPitch; g_realYaw = realYaw;
    if (cmd->nButtons.nValue & IN_ATTACK) { g_aa_active = true; return; }

    // velocity-style avoid backstab: when a knife enemy is close, point yaw
    // exactly away from them before applying manual / mode overrides.
    bool knifeOverride = false;
    float knifeYaw = 0.f;
    if (Config::anti_aim_avoid_backstab) {
        C_CSPlayerPawn* lp = H::SafeLocalPlayer();
        if (lp && Mem::IsUserPtr(lp)) {
            __try {
                const uint8_t localTeam = lp->getTeam();
                const Vector_t localPos = lp->getPosition();
                const KnifeThreatResult r = FindKnifeThreat(lp, localPos, localTeam);
                if (r.found && Config::anti_aim_yaw_mode != Config::AA_YAW_OFF) {
                    knifeOverride = true;
                    knifeYaw = NormalizeYaw(r.awayYaw);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                knifeOverride = false;
            }
        }
    }

    float targetPitch = realPitch;
    switch (Config::anti_aim_pitch_mode) {
    case Config::AA_PITCH_UP: targetPitch = -88.5f; break;
    case Config::AA_PITCH_DOWN: targetPitch = 88.5f; break;
    case Config::AA_PITCH_CUSTOM: targetPitch = Config::anti_aim_pitch_angle; break;
    case Config::AA_PITCH_JITTER: targetPitch = BinaryJitter(Config::anti_aim_pitch_jitter_min, Config::anti_aim_pitch_jitter_max); break;
    case Config::AA_PITCH_RANDOM_JITTER: targetPitch = RandomJitter(Config::anti_aim_pitch_jitter_min, Config::anti_aim_pitch_jitter_max); break;
    case Config::AA_PITCH_SWITCH_JITTER: targetPitch = SwitchJitter(Config::anti_aim_pitch_jitter_min, Config::anti_aim_pitch_jitter_max); break;
    case Config::AA_PITCH_THIRD_WAY_JITTER: targetPitch = ThirdWayJitter(Config::anti_aim_pitch_jitter_min, Config::anti_aim_pitch_jitter_max); break;
    default: break;
    }
    float targetYaw = realYaw;
    if (knifeOverride) {
        targetYaw = knifeYaw;
    } else {
        const bool left = Config::anti_aim_manual_key_left > 0 && (GetAsyncKeyState(Config::anti_aim_manual_key_left) & 0x8000);
        const bool right = Config::anti_aim_manual_key_right > 0 && (GetAsyncKeyState(Config::anti_aim_manual_key_right) & 0x8000);
        const bool back = Config::anti_aim_manual_key_back > 0 && (GetAsyncKeyState(Config::anti_aim_manual_key_back) & 0x8000);
        if (left) targetYaw = realYaw + 90.f;
        else if (right) targetYaw = realYaw - 90.f;
        else if (back) targetYaw = realYaw - 180.f;
        else if (Config::anti_aim_yaw_mode == Config::AA_YAW_STATIC)
            targetYaw = realYaw + (Config::anti_aim_yaw_at_target ? -180.f : Config::anti_aim_yaw_angle);
    }
    // velocity-style auto yaw adjust: compensate the model's inherited
    // sideways roll so the fake "desync-ish" offset reads straight.
    if (!knifeOverride && Config::anti_aim_yaw_adjust && Config::anti_aim_yaw_mode == Config::AA_YAW_STATIC)
        targetYaw = NormalizeYaw(targetYaw + 33.f);

    float& pitch = base->pViewAngles->angValue.x;
    float& yaw = base->pViewAngles->angValue.y;
    pitch = std::clamp(targetPitch, -89.f, 89.f);
    yaw = NormalizeYaw(targetYaw);
    base->pViewAngles->angValue.z = 0.f;
    base->SetBits(BASE_BITS_VIEWANGLES);
    base->nMousedX = 0; base->nMousedY = 0;
    previousRealPitch = realPitch; previousFakePitch = pitch;
    previousRealYaw = realYaw; previousFakeYaw = yaw; hadFake = true;
    g_aa_active = true;
    const float delta = NormalizeYaw(yaw - realYaw) * (kPi / 180.f);
    const float forward = base->flForwardMove, side = base->flSideMove;
    base->flForwardMove = forward * std::cos(delta) + side * std::sin(delta);
    base->flSideMove = side * std::cos(delta) - forward * std::sin(delta);
}
} // namespace AntiAim
