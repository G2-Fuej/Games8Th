#pragma once

// Seed nospread - IDA CSBaseGunFire path (client.dll live 2026-07-28)
//
// Patterns (find_bytes confirmed):
// SPREADSEEDGEN 0x180CB8450 SHA1(quant pitch, quant yaw, tick) - roll ignored
// CalcSpread 0x180CB8D70 RandomSeed(seed) -> sx/sy (fire uses seed+1)
// GetSpread 0x1807CFBF0 mode@+0x17D8 -> VData +1880
// FillGunFireData 0x1807CFE20 tick/eye/punch for seed timebase
// quant 0x180CB2870 AngleNormalize -> *0.5 half-deg bins
//
// Online: freeze GAME pellet from wish; pure-roll first (same seed bin);
// classic pitch+roll walk holding seed0; closed-form; tight bins for deagle/AWP.

#include "../../utils/math/vector/vector.h"
#include <cstdint>

class C_CSWeaponBase;
class C_CSPlayerPawn;
namespace NoSpread {

bool Init();
bool Ready();

// -- One-shot solve (preferred API) -----------------------------------
struct Shot {
	QAngle_t fireAngles{};
	Vector_t hitPoint{};
	int hitbox = -1;
	int seedTick = 0;
	float seedFrac = 0.f;
	float sx = 0.f;
	float sy = 0.f;
	bool ok = false;
};

// Push view so `view + punch` lands with SPREADSEEDGEN half-degree bin
// fraction in [0.10, 0.40] - margin against server-side ULP quant flip.
// Caller must pass the same punch that will be applied at fire time.
// Cheap post-processing step; safe to call on fireAngles before stamping.
void NudgeBinSafe(QAngle_t& view, const QAngle_t* punch);

// Pitch/yaw hist rewrite cap (roll free). Same budget Solve uses.
float HistRewriteLimitDeg(C_CSWeaponBase* weapon, C_CSPlayerPawn* local);

// Compensate wishView so the deterministic pellet hits an enabled hitbox.
// wishView = unpunched camera / silent view. punch applied inside.
// enabledHitboxes = Config::autofire_hitboxes or trigger_hitboxes (null = all).
// Returns true only when ExactShotHits verifies the solution.
bool Solve(
	const Vector_t& eye,
	const QAngle_t& wishView,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int preferHitbox,
	const bool* enabledHitboxes,
	Shot& out);

// Seed nospread: brute half-deg pitch bins + classic pitch/roll cancel.
// wishView = geometric eye->hit (WORLD / punched-space). Return is the same space.
// 720 half-deg pitch bins at wish yaw -> SPREADSEEDGEN -> CalcSpread(seed+1) ->
// classic pitch+roll cancel on wish; keep only if adj hashes to the same seed.
// Fail closed. Caller stamps hist as (out ? punch) with roll kept.
bool SolveNoSpread(
	const Vector_t& eye,
	const QAngle_t& wishView,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Shot& out);

// Soft re-arm after fire (~1 tick). Engine CanWeaponFire is real cycle gate.
// Deagle: no multi-hundred-ms recovery/bloom hold (felt late while on target).
bool SeedCycleAllowsFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local);
void NoteSeedFired(C_CSWeaponBase* weapon, C_CSPlayerPawn* local = nullptr);

// Aim point = capsule center + short target-velocity lead.
Vector_t SeedAimPoint(C_CSPlayerPawn* target, int hitbox, const Vector_t& fallback);

// Hard mindmg on Solve pellet HB only (same-HB center retry). Never switches HB
// (old feet->head rewrite fired 20 dmg under mindmg 100).
// allowPen=true -> pen + minDamageAw (hp-aware). Vis -> minDamageVis.
bool SeedPassesDamage(
	const Vector_t& eye,
	Vector_t& inOutPoint,
	int& inOutHb,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamageVis,
	float minDamageAw);

// -- Thin wrappers (HitChance / legacy call sites) --------------------
bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX = nullptr,
	float* outSpreadY = nullptr,
	unsigned seedAdd = 1u,
	float tickFrac = 0.f);

bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint = nullptr,
	float tickFrac = 0.f);

bool ExactShotHitsAny(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	const bool* enabledHitboxes,
	int* outHitbox = nullptr,
	Vector_t* outPoint = nullptr,
	float tickFrac = 0.f);

std::
uint32_t ComputeSeed(const QAngle_t& angles, int attackTick);

// -- Legacy types kept so AF/trigger compile if any residual refs -----
struct SeedGate {
	float maxCompDelta = 6.f;
	float maxDeltaCap = 72.f;
	float maxRad = 0.50f;
	float maxLat = 900.f;
	float maxAimDist = 72.f;
	float maxBloom = 0.99f;
	float minScore = 1.05f;
	bool allowAnyHb = true;
	bool requireSameBin = false;
	bool exactNeedsAimNear = false;
	bool alwaysBloomGate = false;
	bool requireSettled = false;
};

inline float SeedMinAccept(const SeedGate&) { return 1.05f; }

// Debug diagnostics (Release = no-op body for heavy dump)
namespace SeedDbg {
struct Snap {
	const char* who = "seed";
	const char* event = "WAIT";
	const char* reason = "";
	const char* path = "";
	const char* angSrc = "";
	C_CSWeaponBase* weapon = nullptr;
	C_CSPlayerPawn* local = nullptr;
	bool sniperScoped = false;
	bool onGround = true;
	bool moving = false;
	bool punchOk = false;
	float speed2d = 0.f;
	float inac = 0.f;
	float spr = 0.f;
	float seedFrac = 0.f;
	float sx = 0.f;
	float sy = 0.f;
	float score = -1.f;
	float maxDelta = 0.f;
	float dAng = 0.f;
	float dAim = 0.f;
	float lat = 0.f;
	int seedTick = 0;
	int atkIdx = -1;
	int histCount = 0;
	int preferHb = -1;
	int hitHb = -1;
	int def = 0;
	QAngle_t wish{};
	QAngle_t fire{};
	QAngle_t punch{};
	Vector_t eye{};
	Vector_t aimPt{};
	Vector_t hitPt{};
	SeedGate gate{};
};
void Log(const Snap& s, unsigned intervalMs = 150u);
const char* HbName(int hb);
const char* WpnTag(int def);
} // namespace SeedDbg

} // namespace NoSpread

