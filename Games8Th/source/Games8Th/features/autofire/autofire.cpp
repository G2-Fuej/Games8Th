#include "autofire.h"
#include "../aim/aim_common.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../nospread/nospread.h"
#include "../autowall/autowall.h"
#include "../prediction/prediction.h"

#include "../../utils/memory/memsafe/memsafe.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Autofire {

std::uint32_t g_afLocked = 0;
std::uint32_t g_afPending = 0;
std::uint64_t g_afLockMs = 0;
std::uint64_t g_afSwitchReadyMs = 0;
std::uint64_t g_afFirstShotReadyMs = 0;
bool g_afFirstArmed = false;
bool g_afFirstDone = false;
bool g_afBlockFirst = false;
int g_afGrace = 0;
bool g_autofireWantShoot = false;
bool g_autofireWantStop = false;
bool g_autofireWantScope = false;
QAngle_t g_autofireSilentAngle{};
Vector_t g_autofireSilentEye{};
bool g_autofireSilentValid = false;
int g_autofireFireHist = -1;
QAngle_t g_afLastAimAngle{};
Vector_t g_afLastAimPoint{};
int g_afLastHb = Config::HB_HEAD;
bool g_afLastAimValid = false;
bool g_afSettled = false;
float g_afRemX = 0.f;
float g_afRemY = 0.f;

// Multipoint only for head / chest / stomach / pelvis. Else center.
static bool IsAfMultipointHitbox(int hb) {
	return hb == Config::HB_HEAD || hb == Config::HB_CHEST
		|| hb == Config::HB_STOMACH || hb == Config::HB_PELVIS;
}

// 0 = center only (not in MP list, or unsupported box)
float AutofireMultipointScale(int hb) {
	if (!IsAfMultipointHitbox(hb))
		return 0.f;
	if (!Config::autofire_multipoint[hb])
		return 0.f;
	return std::clamp(Config::autofire_multipoint_scale[hb], 0.f, 1.f);
}

// Soft LOS gate only. Wallbang + mindmg enforced in scan / shoot paths
// via AutoWall::Fire (must hit target entity + pen dmg).
// AF Autowall checkbox AND global AW keybind must both be active.
static bool AfAutowallActive() {
	return Config::autofire_autowall && keybind.isActive(Config::autowall);
}

static bool AfAimAllowed(const Vector_t& eye, const Vector_t& point,
	C_CSPlayerPawn* lp, C_CSPlayerPawn* pawn) {
	if (!Trace::Ready())
		return false;

	const bool behindWall = AimCommon::IsBehindWall(
		eye, point, lp, pawn, Trace::kMaskShot);
	const bool awOn = AfAutowallActive();

	if (!awOn) {
		// No pen: wall = reject
		if (behindWall)
			return false;
		if (Config::autofire_vis_check
			&& !Trace::IsVisible(eye, point, lp, pawn, Trace::kMaskVis))
			return false;
	} else if (behindWall) {
		// AW on + wall: allow scan - AutoWall mindmg gate kills non-pen
		// (do NOT treat as visible)
	} else if (Config::autofire_vis_check) {
		if (!Trace::IsVisible(eye, point, lp, pawn, Trace::kMaskVis))
			return false;
	}

	if (Config::autofire_smoke_check && AimCommon::LineBlockedBySmoke(eye, point))
		return false;
	return true;
}

int AimHbPriority(int hb) {
	if (hb < 0 || hb >= Config::HB_COUNT)
		return 99;
	return hb;
}

// Torso only (not head/neck/limbs) - body-if-lethal + prefer-body.
bool IsBodyHitbox(int hb) {
	return hb == Config::HB_CHEST
		|| hb == Config::HB_STOMACH
		|| hb == Config::HB_PELVIS;
}

bool IsHeadHitbox(int hb) {
	return hb == Config::HB_HEAD || hb == Config::HB_NECK;
}

void ResetAutofire() {
	g_afLocked = 0;
	g_afPending = 0;
	g_afLockMs = 0;
	g_afSwitchReadyMs = 0;
	g_afFirstShotReadyMs = 0;
	g_afFirstArmed = false;
	g_afFirstDone = false;
	g_afBlockFirst = false;
	g_afGrace = 0;
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireWantScope = false;
	g_autofireSilentValid = false;
	g_autofireSilentEye = Vector_t{ 0.f, 0.f, 0.f };
	g_autofireFireHist = -1;
	g_afLastAimValid = false;
	g_afSettled = false;
	g_afRemX = 0.f;
	g_afRemY = 0.f;
	AimCommon::ResetSmoothState();
}

void BeginAutofireEngagement(std::uint32_t handle, std::uint64_t now) {
	g_afLocked = handle;
	g_afPending = 0;
	g_afLockMs = now;
	g_afSwitchReadyMs = 0;
	g_afFirstShotReadyMs = 0;
	g_afFirstArmed = false;
	g_afFirstDone = false;
	g_afBlockFirst = false;
	g_afGrace = AimCommon::kLockGraceFrames;
	// New target: don't hold previous enemy view through reaction; clean smooth clock
	g_afLastAimValid = false;
	g_afSettled = false;
	g_afRemX = 0.f;
	g_afRemY = 0.f;
	AimCommon::ResetSmoothState();
}

// Refine multipoint on ONE hitbox (or all if preferHb < 0).
// Always respects AfAimAllowed + optional maxFov - never returns wall/smoke points.
bool FindClosestAutofireBone(
	const Vector_t& eye,
	const QAngle_t& crosshair,
	C_CSPlayerPawn* pawn,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outPoint,
	int& outHb,
	int preferHb = -1,
	float maxFov = 1.0e9f)
{
	if (!pawn || !local || !Bones::IsValidPos(eye) || !crosshair.IsValid())
		return false;
	if (!(maxFov > 0.f))
		return false;

	float bloom = -1.f;
	if (Config::autofire_multipoint_dynamic && weapon)
		bloom = AimCommon::LiveMultipointBloom(weapon, local);

	const bool targetAir = (pawn->m_fFlags() & FL_ONGROUND) == 0;

	// Estimate range once - skip dyn bloom crush point-blank (same as scan path)
	float estDist = -1.f;
	{
		Vector_t o{};
		if (Bones::GetOrigin(pawn, o) && Bones::IsValidPos(o))
			estDist = eye.Distance(o);
	}
	if (estDist > 0.f && estDist < 160.f)
		bloom = -1.f;

	float bestFov = maxFov + 1.f;
	bool found = false;

	for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
		if (!Config::autofire_hitboxes[hb])
			continue;
		if (preferHb >= 0 && hb != preferHb)
			continue;
		const float mpScale = AutofireMultipointScale(hb);

		Vector_t points[9]{};
		const int nPts = Bones::CollectHitboxMultipoints(
			pawn, hb, mpScale, points, 9, &eye, bloom, targetAir);
		if (nPts <= 0 || nPts > 9)
			continue;

		for (int p = 0; p < nPts; ++p) {
			if (!Bones::IsValidPos(points[p]))
				continue;
			Vector_t pt{};
			if (!AimCommon::PredictAimPoint(pawn, points[p], pt) || !Bones::IsValidPos(pt))
				continue;
			if (!AfAimAllowed(eye, pt, local, pawn))
				continue;
			QAngle_t ang{};
			if (!AimCommon::CalcAngles(eye, pt, ang))
				continue;
			const float fov = AimCommon::GetFov(crosshair, ang);
			if (!Mem::Finite(fov) || fov > maxFov || fov >= bestFov)
				continue;
			bestFov = fov;
			outPoint = pt;
			outHb = hb;
			found = true;
		}
	}
	return found;
}

bool FireRayHitsTarget(
	const Vector_t& eye,
	const QAngle_t& ang,
	C_CSPlayerPawn* target,
	int preferHb,
	bool sniper,
	float eyeDist = -1.f,
	bool seedLoose = false,
	C_CSPlayerPawn* local = nullptr)
{
	if (!target || !Mem::ValidEntity(target) || !ang.IsValid() || !Bones::IsValidPos(eye))
		return false;

	Vector_t fwd{};
	ang.ToDirections(&fwd, nullptr, nullptr);
	const float fl = fwd.Length();
	if (fl < 1e-4f || !Mem::Finite(fl))
		return false;
	fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;

	float rScale = sniper ? 0.88f : 0.90f;
	float bias = sniper ? 0.55f : 0.60f;
	if (seedLoose) {
		rScale = sniper ? 0.96f : 0.94f;
		bias = sniper ? 0.80f : 0.78f;
	}
	if (eyeDist > 0.f && eyeDist < 220.f) {
		const float tClose = 1.f - std::clamp(eyeDist / 220.f, 0.f, 1.f);
		rScale = (std::min)(1.05f, rScale + tClose * 0.12f);
		bias = (std::min)(0.95f, bias + tClose * 0.32f);
	}

	float t = 0.f;
	Vector_t pt{};
	if (preferHb >= 0 && preferHb < Config::HB_COUNT) {
		if (Bones::RayHitsConfiguredHitbox(
				target, preferHb, eye, fwd, rScale, t, pt, bias))
			return true;
	}
	for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
		if (!Config::autofire_hitboxes[hb])
			continue;
		if (hb == preferHb)
			continue;
		if (Bones::RayHitsConfiguredHitbox(
				target, hb, eye, fwd, rScale, t, pt, bias))
			return true;
	}

	if (local && Trace::Ready()) {
		constexpr float kTraceDist = 8192.f;
		const Vector_t end{
			eye.x + fwd.x * kTraceDist,
			eye.y + fwd.y * kTraceDist,
			eye.z + fwd.z * kTraceDist
		};
		Trace::CGameTrace tr{};
		if (Trace::TraceLine(eye, end, local, tr, Trace::kMaskShot) && Trace::DidHit(tr)
			&& Trace::HitsTarget(tr.hit_entity(), target)) {
			const int mapped = Bones::HitgroupToHitbox(tr.hitgroup());
			const int hb = (mapped >= 0) ? mapped : preferHb;
			if (hb >= 0 && hb < Config::HB_COUNT && Config::autofire_hitboxes[hb])
				return true;
		}
	}
	return false;
}

bool RunAutofireImpl(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireWantScope = false;
	g_autofireSilentValid = false;
	g_autofireSilentEye = Vector_t{ 0.f, 0.f, 0.f };
	g_autofireFireHist = -1;
	if (!keybind.isActive(Config::autofire)) {
		ResetAutofire();
		return false;
	}
	// Early-outs must ResetAutofire - else sticky g_afBlockFirst strips M1
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100)) {
		ResetAutofire();
		return false;
	}
	if (!Mem::ValidEntity(lp)) {
		ResetAutofire();
		return false;
	}
	// Freeze time / buy time - no combat (default on)
	if (AimCommon::IsFreezePeriod()) {
		ResetAutofire();
		return false;
	}
	// NOTE: local spawn protection (m_bGunGameImmunity) does NOT block autofire -
	// only enemies in spawn protection are skipped (IsTargetImmune on the target
	// scan). The old local-immune gate here disabled firing while WE were
	// spawn-protected (TDM), which is exactly when a triggerbot is wanted.
	if (Config::autofire_flash_check && AimCommon::IsBlinded(lp)) {
		g_autofireWantShoot = false;
		g_autofireWantStop = false;
		g_autofireWantScope = false;
		g_autofireSilentValid = false;
		g_afBlockFirst = true;
		return g_afLocked != 0;
	}

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(pWpn) || pWpn->IsNonGunWeapon()) {
		ResetAutofire();
		return false;
	}
	// Reload / empty / defuse / inspect: hold lock + reaction/first-shot.
	// Reset here re-armed delays after every reload / TDM respawn clip fill.
	if (!AimCommon::WeaponReadyForCombat(pWpn, lp)) {
		g_autofireWantShoot = false;
		g_autofireWantStop = false;
		g_autofireWantScope = false;
		g_autofireSilentValid = false;
		g_afBlockFirst = true;
		return g_afLocked != 0;
	}
	Config::ApplyWeaponGroup(pWpn);

	if (Config::autofire_fov <= 0.f) {
		ResetAutofire();
		return false;
	}

	bool anyHb = false;
	for (int h = 0; h < Config::HB_COUNT; ++h) {
		if (Config::autofire_hitboxes[h]) {
			anyHb = true;
			break;
		}
	}
	if (!anyHb) {
		ResetAutofire();
		return false;
	}

	if (AimCommon::AimTargetCount() <= 0 || !AimCommon::AimTargets()) {
		if (g_afLocked != 0 && g_afGrace > 0) {
			--g_afGrace;
			g_afBlockFirst = true;
			return true;
		}
		ResetAutofire();
		return false;
	}
	// Engine local shoot origin (NetClientInfo ShootPosition) - not m_vOldOrigin
	const Vector_t lep = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(lep)) {
		ResetAutofire();
		return false;
	}

	QAngle_t qView{};
	if (!AimCommon::GetViewAngles(qView)) {
		ResetAutofire();
		return false;
	}

	// FOV = bullet direction (view + FULL punch). Menu rcs_scale is view-only.
	QAngle_t qViewAim = qView;
	{
		QAngle_t fullPunch{};
		if (AimCommon::GetFirePunch(lp, fullPunch)) {
			qViewAim.x += fullPunch.x;
			qViewAim.y += fullPunch.y;
			qViewAim.Normalize();
		}
	}

	const float baseFov = Config::autofire_fov;
	const float stickyFov = baseFov * 1.15f;

	float bestFov = baseFov;
	float bestDmg = -1.f;
	float bestDist = 1.0e12f;
	QAngle_t bestAngle{};
	Vector_t bestPoint{};
	int bestHb = Config::HB_HEAD;
	bool found = false;
	std::uint32_t bestHandle = 0;
	C_CSPlayerPawn* bestPawn = nullptr;

	float lockedFov = stickyFov;
	float lockedDmg = -1.f;
	float lockedDist = 1.0e12f;
	QAngle_t lockedAngle{};
	Vector_t lockedPoint{};
	int lockedHb = Config::HB_HEAD;
	bool lockedFound = false;
	C_CSPlayerPawn* lockedPawn = nullptr;

	const int selectMode = Config::autofire_target_select;
	const bool overrideMinDmg = keybind.isActive(Config::mindamage_override);
	const float minDmgVis = overrideMinDmg ? Config::mindamage_override_value : Config::autofire_mindamage;
	const float minDmgAw = overrideMinDmg ? Config::mindamage_override_value : Config::autofire_mindamage_aw;
	// Damage scan for: damage-sort mode, mindmg filter (don't lock targets we
	// won't shoot), or body-if-lethal (needs real dmg for the lethal call -
	// without this it silently no-ops when mindmg=0 + crosshair/distance mode).
	const bool needDamageScan =
		(selectMode == Config::AF_TARGET_DAMAGE)
		|| (minDmgVis > 0.f)
		|| Config::autofire_body_if_lethal
		|| (AfAutowallActive() && minDmgAw > 0.f);

	// Bloom for dynamic multipoint: live GetInaccuracy+GetSpread vs hitbox radius
	float bloom = -1.f;
	if (Config::autofire_multipoint_dynamic && pWpn)
		bloom = AimCommon::LiveMultipointBloom(pWpn, lp);

	// Visible multipoint: up to 3 pens/HB. Wall: 1 center only (game pen is expensive).
	constexpr int kMaxFirePerHbVis = 3;
	constexpr int kMaxFirePerHbWall = 1;
	// Public-release pen budget - old 48-64 x multipoint nuked FPS on crowded servers.
	// Sticky lock still gets reserve below; FOV prefilter cuts most work.
	const int nAimTargets = AimCommon::AimTargetCount();
	const int scanPenBudget = AfAutowallActive()
		? (std::min)(16, 6 + nAimTargets * 3)
		: (std::min)(24, 8 + nAimTargets * 4);
	int scanPenLeft = scanPenBudget;
	// Per-pawn wall-pen allowance (AW). Visible cheap estimates don't burn this.
	const int kPenPerPawnWall = 2;

	// Scan locked target first so pen budget hits the sticky lock before FOV spam
	int scanOrder[64];
	int nScan = 0;
	int lockedSlot = -1;
	{
		const int nT = (std::min)(nAimTargets, 64);
		if (g_afLocked != 0) {
			for (int i = 0; i < nT; ++i) {
				if (AimCommon::AimTargets()[i].handle == g_afLocked) {
					lockedSlot = i;
					break;
				}
			}
		}
		// Lock missing from list (died / 1-frame collect miss): do NOT wipe
		// pending/lock here. Instant wipe re-armed reaction and skipped
		// target-switch. Switch path + !found grace handle it (aimbot parity).
		if (lockedSlot >= 0)
			scanOrder[nScan++] = lockedSlot;
		for (int i = 0; i < nT; ++i) {
			if (i == lockedSlot)
				continue;
			scanOrder[nScan++] = i;
		}
	}

	// Focus Target: only while locked pawn still in list + mid-spray
	const bool focusHold = Config::autofire_focus_target
		&& g_afLocked != 0
		&& lockedSlot >= 0
		&& AimCommon::ReadShotsFired(lp) >= 1;

	for (int si = 0; si < nScan; ++si) {
		const int ti = scanOrder[si];
		C_CSPlayerPawn* pawn = AimCommon::AimTargets()[ti].pawn;
		if (!Mem::ValidEntity(pawn))
			continue;

		const int hp = AimCommon::AimTargets()[ti].hp;
		const std::uint32_t pawnHandle = AimCommon::AimTargets()[ti].handle;
		if (focusHold && pawnHandle != g_afLocked)
			continue;

		const bool isLocked = (g_afLocked != 0 && pawnHandle == g_afLocked);
		const float maxFov = isLocked ? stickyFov : baseFov;

		Vector_t pawnOrigin{};
		float worldDist = 1.0e12f;
		if (Bones::GetOrigin(pawn, pawnOrigin))
			worldDist = lep.Distance(pawnOrigin);

		const bool targetAir = (pawn->m_fFlags() & FL_ONGROUND) == 0;
		// Close range: don't starve multipoint with bloom clamp (see CollectHitboxMultipoints)
		const float bloomForMp = (worldDist < 160.f) ? -1.f : bloom;

		// Fresh per-pawn wall pen budget (don't let pawn A burn all global pens)
		int pawnPenLeft = kPenPerPawnWall;
		// If global nearly empty but this is locked target, give a last-chance pens
		if (isLocked && scanPenLeft < 3)
			scanPenLeft = (std::max)(scanPenLeft, 3);

		for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
			if (!Config::autofire_hitboxes[hb])
				continue;

			const float mpScale = AutofireMultipointScale(hb);

			// Center gate is a cheap skip only - fire still uses maxFov per
			// point. Additive hitbox angular radius: the old multiplicative
			// slack (maxFov * 1.25) dropped near-rim multipoints that sit
			// INSIDE the FOV cone at close range / low FOV (hitbox center
			// just outside the ring, near rim well inside it).
			const float hbAngDeg = std::atan2(
				Bones::MultipointRadius(pawn, hb), (std::max)(worldDist, 1.f))
				* AimCommon::kRad2Deg;
			const float centerFovGate = maxFov + hbAngDeg + 0.5f;

			// Cheap gate: center FOV + selection filters before full multipoint
			Vector_t center{};
			if (!Bones::GetHitboxPoint(pawn, hb, center) || !Bones::IsValidPos(center))
				continue;
			Vector_t gateCenter = center;
			{
				Vector_t predCenter{};
				if (AimCommon::PredictAimPoint(pawn, center, predCenter)
					&& Bones::IsValidPos(predCenter))
					gateCenter = predCenter;
			}
			QAngle_t centerAng{};
			if (!AimCommon::CalcAngles(lep, gateCenter, centerAng))
				continue;
			const float centerFov = AimCommon::GetFov(qViewAim, centerAng);
			if (!Mem::Finite(centerFov) || centerFov > centerFovGate)
				continue;
			if (mpScale <= 0.02f && !AfAimAllowed(lep, gateCenter, lp, pawn))
				continue;

			const bool centerWall = Trace::Ready()
				&& AimCommon::IsBehindWall(lep, gateCenter, lp, pawn, Trace::kMaskShot);

			Vector_t points[9]{};
			int nPts = 0;
			bool wallCenterOnly = false;
			if (centerWall && AfAutowallActive()) {
				// Wall + AW: center first; open rim only after center pens
				points[0] = center;
				nPts = 1;
				wallCenterOnly = true;
			} else {
				// Visible (or wall without AW - AfAimAllowed already rejected wall)
				nPts = Bones::CollectHitboxMultipoints(
					pawn, hb, mpScale, points, 9,
					&lep, bloomForMp, targetAir);
			}
			if (nPts <= 0 || nPts > 9)
				continue;

			// Visible EstimateVisible is same per-HB (armor/range only) - safe reuse.
			// Wall pen is per-point - NEVER reuse wall dmg across multipoints.
			float cachedVisDmg = -1.f;
			int fireCount = 0;
			int maxFireHb = centerWall ? kMaxFirePerHbWall
				: (worldDist < 200.f ? 5 : kMaxFirePerHbVis);

			for (int p = 0; p < nPts; ++p) {
				if (!Bones::IsValidPos(points[p]))
					continue;
				Vector_t scanPt{};
				if (!AimCommon::PredictAimPoint(pawn, points[p], scanPt)
					|| !Bones::IsValidPos(scanPt))
					continue;
				QAngle_t angle{};
				if (!AimCommon::CalcAngles(lep, scanPt, angle))
					continue;

				const float fov = AimCommon::GetFov(qViewAim, angle);
				if (!Mem::Finite(fov) || fov > maxFov)
					continue;

				if (!AfAimAllowed(lep, scanPt, lp, pawn))
					continue;

				const bool behindWall = Trace::Ready()
					&& AimCommon::IsBehindWall(lep, scanPt, lp, pawn, Trace::kMaskShot);

				float dmg = 0.f;
				bool penetrated = false;
				bool dmgKnown = false;
				const bool forceAw = behindWall && AfAutowallActive();
				// Damage scan for sort/mindmg, or always when wallbang path
				const bool wantDmg = needDamageScan || forceAw;
				if (wantDmg) {
					const bool reuseVis = !behindWall && cachedVisDmg >= 0.f;
					if (reuseVis) {
						dmg = cachedVisDmg;
						dmgKnown = true;
					} else {
						const bool noBudget = fireCount >= maxFireHb
							|| scanPenLeft <= 0
							|| (behindWall && pawnPenLeft <= 0);
						if (noBudget) {
							// No pen budget left. Hard reject only where a
							// gate needs the number (wall pen check / visible
							// mindmg). Soft scans (body-if-lethal, damage
							// sort) degrade to unknown-dmg candidates instead
							// of being dropped (old path starved valid points
							// once the budget ran out).
							if (behindWall || minDmgVis > 0.f)
								continue;
						} else {
							const bool allowPen = behindWall && AfAutowallActive();
							const AutoWall::Result aw = AutoWall::Fire(
								lep, scanPt, hb, pWpn, lp, pawn, allowPen);
							++fireCount;
							--scanPenLeft;
							if (behindWall)
								--pawnPenLeft;

							dmg = aw.hit ? aw.damage : 0.f;
							penetrated = aw.penetrated;
							dmgKnown = true;
							// Wall: real pen + target hit only
							if (behindWall && (!aw.hit || !aw.penetrated || dmg < 1.f))
								continue;
							if (!behindWall)
								cachedVisDmg = dmg;
						}
					}
					if (dmgKnown && dmg < 1.f)
						continue;

					// Wallbang -> mindmg_aw; visible -> mindmg vis.
					// Unknown-dmg candidates only exist when need == 0.
					float need = (behindWall || penetrated) ? minDmgAw : minDmgVis;
					if (need > 0.f) {
						if (static_cast<float>(hp) < need)
							need = static_cast<float>(hp);
						if (dmg + 0.01f < need)
							continue;
					}

					// Wall center pens + mindmg OK -> open rim multipoint (each rim re-pens)
					if (wallCenterOnly && p == 0 && behindWall && penetrated
						&& mpScale > 0.02f && nPts == 1) {
						const int nMp = Bones::CollectHitboxMultipoints(
							pawn, hb, mpScale, points, 9,
							&lep, bloomForMp, targetAir);
						if (nMp > 1 && nMp <= 9) {
							nPts = nMp;
							maxFireHb = (std::min)(3, nMp);
							wallCenterOnly = false;
						}
					}
				} else if (behindWall) {
					// No damage path + wall = reject (AW off already blocked above)
					continue;
				}

				const bool lethal = dmg + 0.01f >= static_cast<float>(hp);
				const bool bestLethal = bestDmg + 0.01f >= static_cast<float>(
					bestPawn ? Mem::ClampHealth(bestPawn->m_iHealth()) : 0);
				const bool lockedLethal = lockedDmg + 0.01f >= static_cast<float>(
					lockedPawn ? Mem::ClampHealth(lockedPawn->m_iHealth()) : 0);

				// SortTargets + body policy overlays
				auto Prefer = [&](bool haveRef, float refFov, float refDist, float refDmg,
					bool refLethal, int refHb) -> bool {
					if (!haveRef)
						return true;

					const bool candBody = IsBodyHitbox(hb);
					const bool refBody = IsBodyHitbox(refHb);
					const bool candHead = IsHeadHitbox(hb);
					const bool refHead = IsHeadHitbox(refHb);

					// Body if lethal: oneshot torso beats head (same pawn / any).
					// Only when dmg already lethal on a body box.
					if (Config::autofire_body_if_lethal) {
						if (lethal && candBody && !(refLethal && refBody))
							return true;
						if (refLethal && refBody && !(lethal && candBody))
							return false;
					}

					// Prefer body: only within FOV slack of current best (was
					// hard body-over-head even at much worse FOV). Slack scales
					// with configured FOV - the fixed 2.5? was force-body at
					// low FOV (slack ? FOV -> head could never win back) and
					// a no-op at high FOV.
					const float kBodyFovSlack = std::clamp(baseFov * 0.25f, 0.6f, 2.5f);
					if (Config::autofire_prefer_body) {
						if (candBody && !refBody && fov <= refFov + kBodyFovSlack)
							return true;
						if (!candBody && refBody && refFov <= fov + kBodyFovSlack)
							return false;
					}

					switch (selectMode) {
					case Config::AF_TARGET_DISTANCE:
						// Closer pawn; FOV tie-break only when distances nearly equal
						if (worldDist + 8.f < refDist)
							return true;
						if (refDist + 8.f < worldDist)
							return false;
						return fov < refFov;
					case Config::AF_TARGET_DAMAGE: {
						// Lethal > non-lethal. Default: head > body when both lethal.
						if (lethal && !refLethal)
							return true;
						if (!lethal && refLethal)
							return false;

						const int pri = AimHbPriority(hb);
						const int refPri = AimHbPriority(refHb);

						if (lethal) {
							// Both lethal: head before body (unless body-if-lethal
							// already forced body above), then FOV
							if (pri < refPri)
								return true;
							if (pri > refPri)
								return false;
							return fov < refFov;
						}

						// Both non-lethal: default head > body (prefer-body may
						// already have flipped); then dmg / pri / FOV
						if (!Config::autofire_prefer_body) {
							if (candHead && !refHead)
								return true;
							if (!candHead && refHead)
								return false;
						}
						if (dmg > refDmg + 0.5f)
							return true;
						if (dmg + 0.5f < refDmg)
							return false;
						if (pri < refPri)
							return true;
						if (pri > refPri)
							return false;
						return fov < refFov;
					}
					case Config::AF_TARGET_CROSSHAIR:
					default:
						return fov < refFov;
					}
				};

				if (Prefer(found, bestFov, bestDist, bestDmg, bestLethal, bestHb)) {
					bestFov = fov;
					bestDmg = dmg;
					bestDist = worldDist;
					bestAngle = angle;
					bestPoint = scanPt;
					bestHb = hb;
					found = true;
					bestHandle = pawnHandle;
					bestPawn = pawn;
				}

				if (isLocked && Prefer(lockedFound, lockedFov, lockedDist, lockedDmg, lockedLethal, lockedHb)) {
					lockedFov = fov;
					lockedDmg = dmg;
					lockedDist = worldDist;
					lockedAngle = angle;
					lockedPoint = scanPt;
					lockedHb = hb;
					lockedFound = true;
					lockedPawn = pawn;
				}
			}
		}
	}

	// Silent Aim checkbox only - seed mode does NOT force silent.
	// Seed still rewrites fire hist angle when shooting; camera follows Silent Aim.
	const bool afSilentNow = Config::autofire_silent;

	if (!found) {
		if (g_afLocked != 0 && g_afGrace > 0) {
			--g_afGrace;
			g_afBlockFirst = true; // don't shoot on FOV miss
			// Keep lock + frame ownership so delays don't re-arm. Do NOT
			// rewrite last aim - that froze the mouse for 8 frames
			// (target juke = "mouse dead"). Same as aimbot grace.
			return true;
		}
		ResetAutofire();
		return false;
	}
	g_afGrace = AimCommon::kLockGraceFrames;

	const std::uint64_t now = AimCommon::NowMs();
	const float switchMs = std::clamp(Config::aim_target_switch_delay_ms, 0.f, 500.f);
	const float reactionMs = std::clamp(Config::aim_reaction_delay_ms, 0.f, 500.f);
	const float firstShotMs = std::clamp(Config::aim_first_shot_delay_ms, 0.f, 500.f);

	if (g_afLocked == 0) {
		BeginAutofireEngagement(bestHandle, now);
	}
	else if (bestHandle != g_afLocked) {
		// Focus Target: never switch mid-spray even if another pawn scores better
		const bool blockSwitch = Config::autofire_focus_target
			&& AimCommon::ReadShotsFired(lp) >= 1;
		if (!blockSwitch) {
			if (switchMs <= 0.01f) {
				BeginAutofireEngagement(bestHandle, now);
			}
			else {
				if (g_afPending != bestHandle) {
					g_afPending = bestHandle;
					g_afSwitchReadyMs = now + static_cast<std::uint64_t>(switchMs);
				}
				if (now >= g_afSwitchReadyMs)
					BeginAutofireEngagement(bestHandle, now);
			}
		}
	}
	else {
		g_afPending = 0;
		g_afSwitchReadyMs = 0;
	}

	QAngle_t aimAngle{};
	Vector_t aimPoint{};
	int aimHb = Config::HB_HEAD;
	C_CSPlayerPawn* aimTarget = nullptr;
	if (g_afLocked == bestHandle) {
		aimAngle = bestAngle;
		aimPoint = bestPoint;
		aimHb = bestHb;
		aimTarget = bestPawn;
	}
	else if (lockedFound) {
		aimAngle = lockedAngle;
		aimPoint = lockedPoint;
		aimHb = lockedHb;
		aimTarget = lockedPawn;
	}
	else {
		// Switch-wait + old lock lost: track best for view, do NOT re-engage.
		// Old path Begin'd instantly -> target-switch delay was dead (aimbot
		// already had this guard).
		if (!(g_afPending == bestHandle && g_afSwitchReadyMs > 0
			&& now < g_afSwitchReadyMs))
			BeginAutofireEngagement(bestHandle, now);
		aimAngle = bestAngle;
		aimPoint = bestPoint;
		aimHb = bestHb;
		aimTarget = bestPawn;
	}

	// Delays (menu ms, 0 = off). Stack order after lock:
	// 1) reaction - no FIRE. Still aim/smooth (low FOV needs settle). Do NOT arm
	// first-shot yet - old path started first-shot during reaction -> double wait
	// and g_afFirstDone cleared reaction block.
	// 2) first shot - arm when reaction ends; aim OK, fire blocked until ready
	// 3) switch delay - only when bestHandle changes (BeginAutofireEngagement)
	// Delays apply in EVERY mode, seed nospread included. The old path skipped
	// reaction/first-shot when already on bone or with a heavy pistol - that made
	// seed autofire fire instantly with zero humanization (felt like silent aim,
	// "delays not working"). Now the same reaction -> first-shot stack gates seed
	// fire too; the seed Solve still guarantees the hit when the shot fires.
	const bool inReaction = reactionMs > 0.01f
		&& (now - g_afLockMs) < static_cast<std::uint64_t>(reactionMs);
	if (inReaction) {
		g_afBlockFirst = true;
	} else {
		// Arm first-shot only after reaction so timers stack, not overlap.
		if (!g_afFirstArmed) {
			g_afFirstArmed = true;
			g_afFirstShotReadyMs = now + static_cast<std::uint64_t>(firstShotMs);
			g_afFirstDone = (firstShotMs <= 0.01f);
		}
		if (!g_afFirstDone) {
			if (now < g_afFirstShotReadyMs)
				g_afBlockFirst = true;
			else {
				g_afBlockFirst = false;
				g_afFirstDone = true;
			}
		} else {
			g_afBlockFirst = false;
		}
	}

	// Switch-wait with old lock lost: first-shot logic above may have cleared
	// the block (already spent on previous target). Keep fire stripped until
	// the switch timer commits - same as aimbot.
	if (g_afLocked != bestHandle && g_afPending == bestHandle
		&& g_afSwitchReadyMs > 0 && now < g_afSwitchReadyMs)
		g_afBlockFirst = true;

	// RCS + smooth - same model as aimbot:
	// smooth(cur + punch, bone) ? punch
	// Old AF: SmoothToward(cur, bone?punch) without SmoothRcsStep lag match.
	// Fire path still uses GetFirePunch (full) so spray doesn't climb.
	QAngle_t punchScaled{};
	const bool havePunch = Config::rcs && AimCommon::GetScaledPunch(lp, punchScaled);
	if (havePunch)
		(void)AimCommon::SmoothRcsStep(punchScaled);
	else
		AimCommon::ResetRcsSmooth();
	const QAngle_t rcsPunch = havePunch ? AimCommon::GetRcsApplied() : QAngle_t{};

	// Bone aim (no punch) - camera = smoothed aim-space ? punch
	QAngle_t boneAim = aimAngle;
	boneAim.x = std::clamp(boneAim.x, -89.f, 89.f);
	boneAim.z = 0.f;
	if (!boneAim.IsValid()) {
		ResetAutofire();
		return false;
	}

	// Hold last-aim (camera estimate) for FOV-grace
	{
		QAngle_t camHold = boneAim;
		if (havePunch)
			AimCommon::ApplyPunchSubtract(camHold, rcsPunch);
		g_afLastAimAngle = camHold;
	}
	g_afLastAimPoint = aimPoint;
	g_afLastHb = aimHb;
	g_afLastAimValid = true;

	// Seed = fire-hist only. Camera: smooth bone; silent: no camera.
	// ORDER: smooth FIRST, then the humanize gates - during reaction /
	// first-shot holds the camera keeps settling toward the target (old order
	// returned before the smooth block, freezing the view for the whole delay
	// window and then starting the sweep - a robotic "delay-then-snap" that
	// read as silent aim).
	const bool silent = afSilentNow;
	QAngle_t shotAngle = boneAim;

	if (!silent) {
		QAngle_t finalAngle = boneAim;
		const float smooth = std::clamp(Config::aimbot_smooth, 0.f, 100.f);
		QAngle_t liveCam{};
		const bool haveLiveCam = AimCommon::GetViewAngles(liveCam) && liveCam.IsValid();
		if (smooth > 0.01f && haveLiveCam) {
			QAngle_t ref = liveCam;
			if (havePunch) {
				ref.x += rcsPunch.x;
				ref.y += rcsPunch.y;
				ref.Normalize();
			}
			finalAngle = AimCommon::SmoothToward(ref, boneAim, smooth);
		}
		if (havePunch)
			AimCommon::ApplyPunchSubtract(finalAngle, rcsPunch);
		finalAngle.x = std::clamp(finalAngle.x, -89.f, 89.f);
		finalAngle.z = 0.f;
		if (!finalAngle.IsValid()) {
			ResetAutofire();
			return false;
		}
		// Quantize AFTER punch subtract - old order compared punch-space
		// desired vs live camera and added a punch-sized step every tick.
		if (smooth > 0.01f && haveLiveCam)
			AimCommon::QuantizeViewStep(finalAngle, liveCam, g_afRemX, g_afRemY);
		AimCommon::SetViewAngles(finalAngle);
		shotAngle = finalAngle;

		// First-acquisition settle. Latch once camera is close, then spray
		// tracks freely. Hard 2.5? + high smooth + humanize never arrived
		// -> fire stacked forever. Timeout after reaction+first-shot+280ms.
		if (!g_afSettled) {
			QAngle_t settleRef = boneAim;
			if (havePunch)
				AimCommon::ApplyPunchSubtract(settleRef, rcsPunch);
			const float settleFov = std::clamp(2.5f + smooth * 0.04f, 2.5f, 6.5f);
			const float settleNow = (std::isfinite(settleRef.x) && std::isfinite(settleRef.y))
				? AimCommon::GetFov(finalAngle, settleRef) : 0.f;
			const std::uint64_t settleDeadline = g_afLockMs
				+ static_cast<std::uint64_t>(reactionMs)
				+ static_cast<std::uint64_t>(firstShotMs)
				+ 280ull;
			if (settleNow <= settleFov || now >= settleDeadline)
				g_afSettled = true;
			else
				g_afBlockFirst = true;
		}
	} else {
		// Silent: no camera settle. Latch so toggling silent mid-lock
		// does not suddenly wait 2.5? after the first shot already fired.
		g_afSettled = true;
	}

	// Reaction / first-shot / switch humanize (and the settle gate above):
	// HOLD view + no FIRE. The camera already moved this frame - smoothing
	// runs during the hold so the view arrives on target while we wait.
	if (g_afBlockFirst) {
		if (aimTarget && Config::autofire_autostop) {
			const Vector_t vel = Pred::Velocity(lp);
			const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
			const bool onGround = (Pred::Flags(lp) & FL_ONGROUND) != 0;
			if (onGround && std::isfinite(speed2d)
				&& speed2d > AimCommon::kAfStopSpeed)
				g_autofireWantStop = true;
		}
		if (aimTarget && Config::autofire_autoscope
			&& AimCommon::IsScopeWeapon(pWpn)
			&& !AimCommon::IsLocalScoped(lp, pWpn))
			g_autofireWantScope = true;
		return true;
	}

	// Camera / silent hold angle for grace + last-aim
	g_afLastAimAngle = silent ? g_afLastAimAngle : shotAngle;

	if (AimCommon::CanWeaponFire(pWpn, lp)) {
		const Vector_t vel = Pred::Velocity(lp);
		const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
		const bool onGround = (Pred::Flags(lp) & FL_ONGROUND) != 0;
		const bool sniper = AimCommon::IsSniperWeapon(pWpn);
		const bool scopeWpn = AimCommon::IsScopeWeapon(pWpn);

		const bool scopedOk = !scopeWpn || !Config::autofire_scoped_only
			|| AimCommon::IsLocalScoped(lp, pWpn);

		// Autoscope: request zoom as soon as a hittable target is held - the
		// old path set this only when dmgOk, but dmgOk is gated behind
		// scopedOk, which requires ALREADY being scoped when scoped_only is
		// on -> scoped_only + autoscope together could never scope (deadlock).
		if (Config::autofire_autoscope && scopeWpn
			&& !AimCommon::IsLocalScoped(lp, pWpn))
			g_autofireWantScope = true;

		Vector_t shotPoint = aimPoint;
		int shotHb = aimHb;

		const bool seedModeGate =
			Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD;

		// Seed: center + short vel lead (rim/no lead misses movers).
		// HC: refine multipoint ONLY on scan-selected hitbox.
		// Old CROSSHAIR path preferredHb=-1 -> re-picked head rim after
		// body-if-lethal / mindmg chose chest -> wrong shots + thrash.
		if (aimTarget && !seedModeGate && shotHb >= 0 && shotHb < Config::HB_COUNT) {
				// Punch-aware FOV ref (bone + punch) for multipoint refine
				QAngle_t refineRef = boneAim;
				if (havePunch) {
					refineRef.x += rcsPunch.x;
					refineRef.y += rcsPunch.y;
					refineRef.Normalize();
				}
				Vector_t closest{};
				int closestHb = shotHb;
				const float refineFov = stickyFov;
				if (FindClosestAutofireBone(
						lep, refineRef, aimTarget, pWpn, lp,
						closest, closestHb, shotHb, refineFov)
					&& closestHb == shotHb
					&& Bones::IsValidPos(closest)
					&& AfAimAllowed(lep, closest, lp, aimTarget)) {
					// Verify min damage if active before adopting refined point
					const bool refineBehindWall = AimCommon::IsBehindWall(
						lep, closest, lp, aimTarget, Trace::kMaskShot);
					const bool refineAllowPen = refineBehindWall && AfAutowallActive();
					const float refineMinDamage = refineAllowPen ? minDmgAw : minDmgVis;
					bool refineDmgOk = true;
					if (refineMinDamage > 0.f) {
						refineDmgOk = AutoWall::PassesMinDamage(
							lep, closest, shotHb, pWpn, lp, aimTarget, refineAllowPen,
							refineMinDamage);
					}
					if (refineDmgOk) {
						// Prefer closer FOV than current scan pick (same HB only)
						QAngle_t aNew{}, aOld{};
						const bool okN = AimCommon::CalcAngles(lep, closest, aNew);
						const bool okO = AimCommon::CalcAngles(lep, shotPoint, aOld);
						if (okN && okO) {
							const float fN = AimCommon::GetFov(refineRef, aNew);
							const float fO = AimCommon::GetFov(refineRef, aOld);
							if (Mem::Finite(fN) && Mem::Finite(fO) && fN <= fO + 0.05f)
								shotPoint = closest;
						} else if (okN) {
							shotPoint = closest;
						}
					}
				}
		}

		// Hist stamp = unpunched wish. IDA CSBaseGunFire fill (0x1807D1180)
		// adds ComputeAimPunchFire then SPREADSEEDGEN(view+punch). Camera RCS
		// is view-only and must not starve / double the fire punch.
		QAngle_t fireAngle{};
		if (!AimCommon::CalcAngles(lep, shotPoint, fireAngle))
			fireAngle = boneAim;
		fireAngle.x = std::clamp(fireAngle.x, -89.f, 89.f);
		fireAngle.z = 0.f;
		if (!fireAngle.IsValid())
			fireAngle = boneAim;
		{
			QAngle_t fp{};
			if (AimCommon::GetFirePunch(lp, fp) && fp.IsValid())
				AimCommon::ApplyPunchSubtract(fireAngle, fp);
		}

		// onTarget: geometric wish dir (fireAngle + punch). Seed uses looser capsule -
		// ExactShotHits is real hit gate. HC keeps tight so low FOV doesn't spam miss.
		QAngle_t gateAng = fireAngle;
		{
			QAngle_t gatePunch{};
			if (AimCommon::GetFirePunch(lp, gatePunch)) {
				gateAng.x += gatePunch.x;
				gateAng.y += gatePunch.y;
				gateAng.z = 0.f;
				gateAng.Normalize();
				gateAng.x = std::clamp(gateAng.x, -89.f, 89.f);
			}
		}
		const float shotDist = Bones::IsValidPos(shotPoint)
			? lep.Distance(shotPoint) : -1.f;
		// Seed nospread: scan already picked the point - skip hitchance-style
		// capsule gate. Correction fail-closes at fire if seed cannot be solved.
		bool onTarget = seedModeGate
			? (aimTarget && Bones::IsValidPos(shotPoint))
			: FireRayHitsTarget(
				lep, gateAng, aimTarget, shotHb, sniper, shotDist, false, lp);

		// Low FOV + smooth lag: multipoint fail -> retry hitbox center once
		if (!onTarget && !seedModeGate && aimTarget && shotHb >= 0 && shotHb < Config::HB_COUNT) {
			Vector_t cpt{};
			if (Bones::GetHitboxPoint(aimTarget, shotHb, cpt) && Bones::IsValidPos(cpt)
				&& AfAimAllowed(lep, cpt, lp, aimTarget)) {
				QAngle_t ca{};
				if (AimCommon::CalcAngles(lep, cpt, ca)) {
					QAngle_t fp{};
					if (AimCommon::GetFirePunch(lp, fp))
						AimCommon::ApplyPunchSubtract(ca, fp);
					ca.x = std::clamp(ca.x, -89.f, 89.f);
					ca.z = 0.f;
					if (ca.IsValid()) {
						QAngle_t ga = ca;
						QAngle_t gp{};
						if (AimCommon::GetFirePunch(lp, gp)) {
							ga.x += gp.x; ga.y += gp.y; ga.z = 0.f;
							ga.Normalize();
							ga.x = std::clamp(ga.x, -89.f, 89.f);
						}
						const float cd = lep.Distance(cpt);
						if (FireRayHitsTarget(
								lep, ga, aimTarget, shotHb, sniper, cd, seedModeGate, lp)) {
							shotPoint = cpt;
							fireAngle = ca;
							gateAng = ga;
							onTarget = true;
						}
					}
				}
			}
		}

		// dmgOk = selection filters (smoke/vis) + real hit path + mindamage
		// hcOk = spread would land - only required to shoot, not to stop
		// BUG: HC/seed lived inside `else` of Trace::Ready - when Trace down + mindmg
		// off, dmgOk=true but afMode never ran -> hcOk stuck false -> never WantShoot.
		bool dmgOk = false;
		bool hcOk = false;
		if (scopedOk && onTarget && aimTarget) {
			if (!AfAimAllowed(lep, shotPoint, lp, aimTarget)) {
				dmgOk = false;
			} else if (!Trace::Ready()) {
				// AfAimAllowed() is fail-closed, so this is only a race-safe guard.
				dmgOk = false;
			} else {
				const bool behindWall = AimCommon::IsBehindWall(
					lep, shotPoint, lp, aimTarget, Trace::kMaskShot);

				if (behindWall && !AfAutowallActive()) {
					dmgOk = false;
				} else if (behindWall && AfAutowallActive()) {
					// Wallbang: must pen wall + hit target + dmg >= mindamage_aw
					const AutoWall::Result aw = AutoWall::Fire(
						lep, shotPoint, shotHb, pWpn, lp, aimTarget, true);
					if (aw.hit && aw.penetrated && aw.damage >= 1.f) {
						float need = minDmgAw;
						if (need > 0.f) {
							const int hp = aimTarget->m_iHealth();
							if (hp > 0 && static_cast<float>(hp) < need)
								need = static_cast<float>(hp);
							dmgOk = aw.damage + 0.01f >= need;
						} else {
							dmgOk = true;
						}
					} else {
						dmgOk = false;
					}
				} else {
					// Visible: LOS + Min Damage (no pen estimate).
					const bool losOk = Trace::IsVisible(
						lep, shotPoint, lp, aimTarget, Trace::kMaskVis);
					const bool closeOpen = shotDist > 0.f && shotDist < 96.f;
					if (!losOk && !closeOpen) {
						dmgOk = false;
					} else if (minDmgVis <= 0.f) {
						dmgOk = true;
					} else {
						dmgOk = AutoWall::PassesMinDamage(
							lep, shotPoint, shotHb, pWpn, lp, aimTarget, false,
							minDmgVis);
						if (!dmgOk && shotHb >= 0 && shotHb < Config::HB_COUNT) {
							Vector_t cpt{};
							if (Bones::GetHitboxPoint(aimTarget, shotHb, cpt)
								&& Bones::IsValidPos(cpt)
								&& AfAimAllowed(lep, cpt, lp, aimTarget)) {
								dmgOk = AutoWall::PassesMinDamage(
									lep, cpt, shotHb, pWpn, lp, aimTarget, false,
									minDmgVis);
								if (dmgOk) {
									shotPoint = cpt;
									QAngle_t ca{};
									if (AimCommon::CalcAngles(lep, cpt, ca)) {
										QAngle_t fp{};
										if (AimCommon::GetFirePunch(lp, fp))
											AimCommon::ApplyPunchSubtract(ca, fp);
										ca.x = std::clamp(ca.x, -89.f, 89.f);
										ca.z = 0.f;
										if (ca.IsValid())
											fireAngle = ca;
									}
								}
							}
						}
					}
				}
			}

			// HC / Seed always when dmgOk - independent of Trace::Ready branch.
			if (dmgOk) {
				int afMode = Config::autofire_mode;
				if (afMode < 0 || afMode >= Config::AF_MODE_COUNT)
					afMode = Config::AF_MODE_HITCHANCE;
				if (afMode == Config::AF_MODE_SEED_NOSPREAD) {
					if (!NoSpread::Ready())
						NoSpread::Init();
					if (!NoSpread::Ready())
						afMode = Config::AF_MODE_HITCHANCE;
				}

				if (afMode == Config::AF_MODE_SEED_NOSPREAD) {
					// Seed nospread: geometric eye->hit, hitchance skipped,
					// fail closed, stamp roll on every hist slot.
					hcOk = false;
					if (NoSpread::Ready() && NoSpread::SeedCycleAllowsFire(pWpn, lp)
						&& Bones::IsValidPos(shotPoint)) {
						const int histCount = (cmd && cmd->csgoUserCmd.inputHistoryField.pRep)
							? cmd->csgoUserCmd.inputHistoryField.nCurrentSize : 0;
						const int atkIdx = histCount > 0 ? histCount - 1 : -1;
						Vector_t seedEye = lep;
						float seedFrac = 0.f;
						if (cmd && atkIdx >= 0) {
							if (CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(atkIdx)) {
								seedFrac = e->flPlayerTickFraction;
								if (!std::isfinite(seedFrac))
									seedFrac = 0.f;
								if (e->pShootPosition) {
									const Vector4D_t& v = e->pShootPosition->vecValue;
									const Vector_t sp{ v.x, v.y, v.z };
									if (Bones::IsValidPos(sp)) {
										const float dx = sp.x - lep.x;
										const float dy = sp.y - lep.y;
										const float dz = sp.z - lep.z;
										if (dx * dx + dy * dy + dz * dz < 9.f)
											seedEye = sp;
									}
								}
							}
						}
						int seedTick = AimCommon::GetRenderTick(cmd, atkIdx, lp);
						Bones::ClampSeedTickToShootRing(lp, seedTick, seedFrac);
						if (seedTick > 0) {
							QAngle_t wish{};
							if (!AimCommon::CalcAngles(seedEye, shotPoint, wish))
								wish = fireAngle;
							wish.z = 0.f;
							wish.x = std::clamp(wish.x, -89.f, 89.f);
							wish.Normalize();

							// Bin-edge nudge (trigger parity): SPREADSEEDGEN hashes
							// quant(pitch, yaw); parking the wish on a half-degree
							// edge lets server-side ULP drift flip the seed hash ->
							// different pellet -> the solved shot misses.
							NoSpread::NudgeBinSafe(wish, nullptr);

							NoSpread::Shot shot{};
							bool solved = false;
							__try {
								solved = NoSpread::SolveNoSpread(
									seedEye, wish, seedTick, seedFrac, pWpn, lp, shot);
							} __except (EXCEPTION_EXECUTE_HANDLER) {
								solved = false;
							}
							if (solved && shot.ok && shot.fireAngles.IsValid()
								&& (std::fabs(shot.fireAngles.x) > 1e-6f
									|| std::fabs(shot.fireAngles.y) > 1e-6f
									|| std::fabs(shot.fireAngles.z) > 1e-6f)) {
								if (shot.seedTick > 0)
									seedTick = shot.seedTick;
								if (std::isfinite(shot.seedFrac))
									seedFrac = shot.seedFrac;

								QAngle_t punch{};
								if (!HitChance::ReadSeedFirePunch(
										lp, pWpn, seedTick, seedFrac, punch)
									|| !punch.IsValid()) {
									punch = QAngle_t{};
								}
								punch.z = 0.f;

								// Engine SPREADSEEDGEN(view + punch). Stamp unpunched
								// view so fire reconstitutes the solved WORLD angle.
								QAngle_t histAng = shot.fireAngles;
								histAng.x -= punch.x;
								histAng.y -= punch.y;
								if (!std::isfinite(histAng.z))
									histAng.z = 0.f;
								histAng.Normalize();

								// Post-Solve bin nudge (trigger parity): if Solve
								// landed the fire angles on a quant edge the same
								// server-side ULP drift applies. Nudge is <0.15?
								// and the capsule accept scale has room; roll kept.
								{
									const float rollKeep = histAng.z;
									NoSpread::NudgeBinSafe(
										histAng, punch.IsValid() ? &punch : nullptr);
									histAng.z = rollKeep;
									histAng.Normalize();
								}

								fireAngle = histAng;
								g_autofireFireHist = atkIdx;
								if (cmd && atkIdx >= 0) {
									cmd->StampNospreadHistory(
										histAng, seedEye, seedTick, seedFrac);
									Bones::StampShootPositionHistory(
										lp, seedTick, seedFrac, seedEye);
								}
								g_autofireSilentEye = seedEye;
								hcOk = true;
								NoSpread::NoteSeedFired(pWpn, lp);
							}
						}
					}
				} else {
					// Hitchance - PassesSafe uses live GetInaccuracy+GetSpread
					hcOk = false;
					float needHc = std::clamp(
						Config::autofire_hitchance, 0.f, 100.f);
					if (sniper && lp->m_bIsScoped() && needHc <= 0.01f)
						needHc = 0.f;
					if (needHc <= 0.01f) {
						hcOk = true;
					} else {
						if (!HitChance::Ready())
							HitChance::Init();
						hcOk = HitChance::PassesSafe(
							lep, fireAngle, shotPoint, shotHb, pWpn,
							needHc, lp, aimTarget);
					}
				}
			}
		}

		// Fire when HC/seed says ok. Movement is NEVER touched unless Autostop on.
		// No hard run/air speed kill - those felt like forced stops with Autostop off.
		// Scoped Only only gates dmgOk above; it never brakes movement.
		if (dmgOk) {
			if (Config::autofire_autostop && onGround
				&& std::isfinite(speed2d) && speed2d > AimCommon::kAfStopSpeed)
				g_autofireWantStop = true;

			if (hcOk) {
				g_autofireWantShoot = true;
				g_autofireSilentAngle = fireAngle;
				// Seed path sets g_autofireFireHist. Non-seed must capture from game's
				// nAttack1StartHistoryIndex so PressAttack overwrites the correct entry.
				const bool seedFire = Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD
					&& NoSpread::Ready();
				if (!seedFire && cmd) {
					// Bounds-clamp: a stale game index from an older larger
					// history must not reach GetInputHistoryEntry downstream.
					const int histCount = (cmd->csgoUserCmd.inputHistoryField.pRep)
						? cmd->csgoUserCmd.inputHistoryField.nCurrentSize : 0;
					const int gameIdx = cmd->csgoUserCmd.nAttack1StartHistoryIndex;
					g_autofireFireHist = (gameIdx >= 0 && gameIdx < histCount) ? gameIdx : -1;
				}
				// Seed FIRE already wrote FillGunFireData eye into g_autofireSilentEye.
				// Hitchance path: always live shootpos.
				if (!seedFire || !Bones::IsValidPos(g_autofireSilentEye))
					g_autofireSilentEye = lep;
				g_autofireSilentValid = true;
			}
		}
	}

	return true;
}

bool RunSafe(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	bool ok = false;
	__try {
		ok = RunAutofireImpl(lp, cmd);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ResetAutofire();
		ok = false;
	}
	return ok;
}

bool Run(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	return RunSafe(lp, cmd);
}

void Reset() {
	ResetAutofire();
}

bool WantShoot() { return g_autofireWantShoot; }
bool WantStop() { return g_autofireWantStop; }
bool WantScope() { return g_autofireWantScope; }
bool BlockFirstShot() { return g_afBlockFirst; }
bool SilentValid() { return g_autofireSilentValid; }
const QAngle_t& SilentAngle() { return g_autofireSilentAngle; }
const Vector_t& SilentEye() { return g_autofireSilentEye; }
int FireHistIndex() { return g_autofireFireHist; }

void ClearShootFlags() {
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireWantScope = false;
	g_afBlockFirst = false;
	g_autofireSilentValid = false;
	g_autofireFireHist = -1;
}

} // namespace Autofire

