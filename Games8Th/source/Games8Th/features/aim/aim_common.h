#pragma once

#include <cstdint>
#include "../../utils/math/vector/vector.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"

class C_CSPlayerPawn;
class C_CSWeaponBase;

namespace AimCommon {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.0f / kPi;
constexpr float kPunchScale = 1.f;
constexpr int kLockGraceFrames = 8;
constexpr float kAfStopSpeed = 34.f;
constexpr float kNoHcRunSpeed = 75.f;

struct AimTarget {
	C_CSPlayerPawn* pawn = nullptr;
	std::
uint32_t handle = 0;
	int hp = 0;
};

void CollectAimTargets(C_CSPlayerPawn* lp);
// Map unload / death - drop raw pawn* list
void ClearTargets();
int AimTargetCount();
const AimTarget* AimTargets();
// True when any combat feature may consume the aim list this tick
// (aim key active / autofire / triggerbot). CollectAimTargets early-outs
// when false - the 128-slot entity walk must not run every CreateMove.
bool CombatActive() noexcept;

std::
uint64_t NowMs();
float LiveMultipointBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local);

bool CalcAngles(const Vector_t& viewPos, const Vector_t& aimPos, QAngle_t& out);
float GetFov(const QAngle_t& viewAngle, const QAngle_t& aimAngle);
// Hitscan interp lead (~1 tick + sim delta). Not projectile travel.
bool PredictAimPoint(C_CSPlayerPawn* pawn, const Vector_t& point, Vector_t& out);

bool CanWeaponFire(C_CSWeaponBase* wep, C_CSPlayerPawn* lp);
// Reload / empty / defuse / switch - NOT fire-rate. AF uses this to skip aim scan.
bool WeaponReadyForCombat(C_CSWeaponBase* wep, C_CSPlayerPawn* lp);
bool IsSniperWeapon(C_CSWeaponBase* weapon);
// AWP/Scout/Auto + AUG/SG - weapons that use scope zoom
bool IsScopeWeapon(C_CSWeaponBase* weapon);
// Local scoped: pawn flag + weapon zoomLevel (pass active weapon when known)
bool IsLocalScoped(C_CSPlayerPawn* lp, C_CSWeaponBase* weapon = nullptr);
// Deagle / R8 - high bloom after fire; seed uses ExactShotHits + engine cycle (no soft wait).
bool IsHeavyPistol(C_CSWeaponBase* weapon);
// Semi / bolt (VData !m_bIsFullAuto) - needs rising edge + release or hold never re-fires.
bool IsSemiWeapon(C_CSWeaponBase* weapon);
// SMG / rifle / LMG - spray seed path (hot bloom every cycle).
bool IsSprayAutoWeapon(C_CSWeaponBase* weapon);
bool IsBlinded(C_CSPlayerPawn* lp);
// True if eye->aim segment crosses an active smoke volume.
bool LineBlockedBySmoke(const Vector_t& eye, const Vector_t& aim);

// Round freeze (C_CSGameRules::m_bFreezePeriod) - no combat.
bool IsFreezePeriod();
// DM spawn protect / gun-game invuln (invisible + no damage). Always skip for AF/trigger.
bool IsTargetImmune(C_CSPlayerPawn* pawn);

// Visible aim - Constant / Linear / Sine. dt-normalized to ~64 Hz.
// Humanize: sticky bias/speed/drift intervals (not per-tick re-roll).
// Soft land + true-bone overshoot clamp (no residual deadzone).
QAngle_t SmoothToward(const QAngle_t& cur, const QAngle_t& target, float smooth);
// Clear humanize + smooth clock (key-up / new engagement).
void ResetSmoothState();
// Snap a desired view step onto the mouse-count lattice (sensitivity x 0.022).
// remX/remY carry the unused fraction so sub-count motion is not lost.
void QuantizeViewStep(QAngle_t& desired, const QAngle_t& liveView, float& remX, float& remY);

bool IsBehindWall(const Vector_t& eye, const Vector_t& aimPoint, void* skip, void* target,
	std::
uint64_t mask);

bool ReadAimPunch(C_CSPlayerPawn* lp, QAngle_t& out);
int ReadShotsFired(C_CSPlayerPawn* lp);
bool GetScaledPunch(C_CSPlayerPawn* lp, QAngle_t& out);
// rcs_smooth: ease applied toward absolute punch. Returns this frame's step
// (delta). sm<=0.01 -> instant catch-up. GetRcsApplied() = absolute lag state.
QAngle_t SmoothRcsStep(const QAngle_t& absolutePunch);
QAngle_t GetRcsApplied();
void ResetRcsSmooth();
bool GetFirePunch(C_CSPlayerPawn* lp, QAngle_t& out);
void ApplyPunchSubtract(QAngle_t& ang, const QAngle_t& punch);

bool GetViewAngles(QAngle_t& out);
// Camera + same-tick cmd (base pViewAngles + tip hist). BindCmd in CreateMove first.
void SetViewAngles(const QAngle_t& ang);
// Silent fire: stamp base + tip hist only (no camera). Server needs same-tick angles.
void StampCmdAngles(const QAngle_t& ang);
void BindCmd(CUserCmd* cmd);
void UnbindCmd();

int GetSeedTickFromEntry(CCSGOInputHistoryEntryPB* e);
int GetRenderTick(CUserCmd* cmd, int histIndex, C_CSPlayerPawn* lp);

extern QAngle_t g_oldPunch;
extern bool g_hadPunch;
bool ApplyDeltaRcs(C_CSPlayerPawn* lp);
bool StandaloneRcs(C_CSPlayerPawn* lp);

} // namespace AimCommon

