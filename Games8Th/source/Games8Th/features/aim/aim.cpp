#include "aim.h"
#include "aim_common.h"
#include "../autofire/autofire.h"
#include "../triggerbot/triggerbot.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../Games8Th/interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../Games8Th/interfaces/interfaces.h"
#include "../../../Games8Th/interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../Games8Th/hooks/hooks.h"
#include "../../../Games8Th/interfaces/CUserCmd/CUserCmd.h"
#include "../../../Games8Th/config/config.h"
#include "../../../Games8Th/utils/schema/schema.h"
#include "../../../Games8Th/utils/fnv1a/fnv1a.h"
#include "../../../Games8Th/keybinds/keybinds.h"
#include "../../../Games8Th/offsets/offsets.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../nospread/nospread.h"
#include "../autowall/autowall.h"
#include "../gamemode/gamemode.h"
#include "../input_inject/input_inject.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../prediction/prediction.h"
#include "../hitmarker/hitmarker.h"
#include "../../../Games8Th/utils/memory/memsafe/memsafe.h"
#include "../../../Games8Th/utils/memory/patternscan/patternscan.h"
#include "../../../Games8Th/utils/console/console.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

using AimCommon::
CalcAngles;
using AimCommon::
GetFov;
using AimCommon::
CanWeaponFire;
using AimCommon::
IsSniperWeapon;
using AimCommon::
IsScopeWeapon;
using AimCommon::
IsLocalScoped;
using AimCommon::
IsBlinded;
using AimCommon::
SmoothToward;
using AimCommon::
ResetSmoothState;
using AimCommon::
IsBehindWall;
using AimCommon::
GetScaledPunch;
using AimCommon::
GetFirePunch;
using AimCommon::
ApplyPunchSubtract;
using AimCommon::
GetViewAngles;
using AimCommon::
SetViewAngles;
using AimCommon::
NowMs;
using AimCommon::
LiveMultipointBloom;
using AimCommon::
GetSeedTickFromEntry;
using AimCommon::
GetRenderTick;
using AimCommon::
PredictAimPoint;
using AimCommon::
kLockGraceFrames;
using AimCommon::
g_oldPunch;
using AimCommon::
g_hadPunch;
using AimCommon::
StandaloneRcs;
using AimCommon::
ApplyDeltaRcs;

std::uint32_t g_lockedTarget = 0;
std::uint32_t g_pendingTarget = 0;
std::uint64_t g_lockAcquiredMs = 0;
std::uint64_t g_switchReadyMs = 0;
std::uint64_t g_firstShotReadyMs = 0;
bool g_firstShotArmed = false;
bool g_firstShotDone = false;
bool g_blockFirstShot = false;
int g_lockGrace = 0;
// Sticky hitbox while locked - stops head/chest thrash (magnetic shake)
int g_lockedHitbox = -1;
// Semi cooldown: prevents AF/trigger from re-firing on the frame immediately
// after a semi release. Without this, s_semiNeedRelease strips attack, then
// afFire triggers PressAttack next frame -> double-tap on semi weapons.
bool g_semiCooldown = false;

// -- Player-input yield --------------------------------------------------
// Surplus view delta vs our last written angle (minus our own previous step)
// means the player is actively dragging the mouse. The aimbot then yields:
// it stops writing the view so the player's own aim always wins (no fight),
// re-anchors the baseline each frame, and resumes when input stops.
static QAngle_t g_prevAimWrite{};
static QAngle_t g_lastAimWrite{};
static bool g_havePrevWrite = false;
static bool g_haveLastWrite = false;
static std::uint64_t g_yieldUntilMs = 0;
static bool g_yieldActive = false;
// Mouse-count remainder - the smoothed step is quantized to
// whole counts (sensitivity x 0.022?/count); the fractional part carries to
// the next tick so movement lands on the real mouse-count lattice.
static float g_aimRemX = 0.f;
static float g_aimRemY = 0.f;
constexpr float kYieldInputDeg = 1.20f;          // surplus motion = explicit player flick
constexpr std::uint64_t kYieldWindowMs = 60;     // back off briefly after manual flick

static void ResetHumanize() {
	g_lockedTarget = 0;
	g_pendingTarget = 0;
	g_lockAcquiredMs = 0;
	g_switchReadyMs = 0;
	g_firstShotReadyMs = 0;
	g_firstShotArmed = false;
	g_firstShotDone = false;
	g_blockFirstShot = false;
	g_lockGrace = 0;
	g_lockedHitbox = -1;
	g_semiCooldown = false;
	g_havePrevWrite = false;
	g_haveLastWrite = false;
	g_yieldActive = false;
	g_yieldUntilMs = 0;
	g_aimRemX = 0.f;
	g_aimRemY = 0.f;
	ResetSmoothState();
}

static void BeginEngagement(std::uint32_t handle, std::uint64_t now) {
	g_lockedTarget = handle;
	g_pendingTarget = 0;
	g_lockAcquiredMs = now;
	g_switchReadyMs = 0;
	g_firstShotReadyMs = 0;
	g_firstShotArmed = false;
	g_firstShotDone = false;
	g_blockFirstShot = false;
	g_lockGrace = kLockGraceFrames;
	g_lockedHitbox = -1;
	g_aimRemX = 0.f;
	g_aimRemY = 0.f;
	// Stale last-write from the previous target looks like a huge player
	// drag on the first post-lock frame -> 140 ms yield every new fight.
	g_havePrevWrite = false;
	g_haveLastWrite = false;
	g_yieldActive = false;
	g_yieldUntilMs = 0;
	// Clean path noise + smooth clock on every new engagement
	ResetSmoothState();
}

// Soft fail: transient one-frame failures (vis probe / entity blip / angle
// read) while locked keep the engagement + delay state. A hard reset here
// re-arms reaction + first-shot on every blip - eats clicks and restarts
// the smooth curve (visible aim stutter). Hard reset only when truly idle.
static bool AimbotSoftFail() {
	if (g_lockedTarget != 0 && g_lockGrace > 0) {
		--g_lockGrace;
		g_blockFirstShot = !g_firstShotDone;
		return true;
	}
	ResetHumanize();
	return false;
}

struct AimCandidate {
	bool valid = false;
	C_CSPlayerPawn* pawn = nullptr;
	std::uint32_t handle = 0;
	int hitbox = -1;
	Vector_t point{};
	Vector_t rawPoint{};
	QAngle_t angle{};
	float fov = (std::numeric_limits<float>::max)();
	bool visible = false;
};

static bool IsFinitePoint(const Vector_t& point) {
	return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)
		&& Bones::IsValidPos(point);
}

static bool IsTargetAirborne(C_CSPlayerPawn* pawn) {
	if (!pawn)
		return false;
	__try {
		return (pawn->m_fFlags() & FL_ONGROUND) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static bool BetterPoint(const AimCandidate& candidate, const AimCandidate& current) {
	if (!current.valid)
		return true;
	if (candidate.visible != current.visible)
		return candidate.visible;
	if (candidate.fov + 0.001f < current.fov)
		return true;
	if (std::fabs(candidate.fov - current.fov) <= 0.001f)
		return candidate.hitbox < current.hitbox;
	return false;
}

static bool BetterTarget(const AimCandidate& candidate, const AimCandidate& current) {
	if (!current.valid)
		return true;
	return BetterPoint(candidate, current);
}

static float AimMultipointScale(const int hitbox) {
	switch (hitbox) {
	case Config::HB_HEAD: return 0.55f;
	case Config::HB_NECK: return 0.50f;
	case Config::HB_CHEST: return 0.60f;
	case Config::HB_STOMACH: return 0.55f;
	case Config::HB_PELVIS: return 0.50f;
	case Config::HB_ARMS: return 0.45f;
	case Config::HB_LEGS: return 0.45f;
	case Config::HB_FEET: return 0.40f;
	default: return 0.50f;
	}
}

bool RunAimbot(C_CSPlayerPawn* lp) {
	if (!keybind.isActive(Config::
aimbot)) {
		ResetHumanize();
		return false;
	}
	if (!I::
GameEntity || !Mem::
Valid(I::
GameEntity->Instance, 0x2100))
		return AimbotSoftFail();
	if (!Mem::
ValidEntity(lp))
		return AimbotSoftFail();

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::
ValidEntity(pWpn) || pWpn->IsNonGunWeapon())
		return AimbotSoftFail();
	Config::
ApplyWeaponGroup(pWpn);

	// Flash / scoped-only: hold lock + block fire. SoftFail here re-armed
	// reaction + first-shot after every flash tick / unscoped peek.
	if (Config::aim_flash_check && IsBlinded(lp)) {
		g_blockFirstShot = true;
		return g_lockedTarget != 0 || AimbotSoftFail();
	}
	if (Config::aim_scoped_only && IsScopeWeapon(pWpn) && !IsLocalScoped(lp, pWpn)) {
		g_blockFirstShot = true;
		return g_lockedTarget != 0 || AimbotSoftFail();
	}

	if (Config::
aimbot_fov <= 0.f)
		return AimbotSoftFail();

	bool anyHb = false;
	for (int h = 0; h < Config::
HB_COUNT; ++h) {
		if (Config::
aim_hitboxes[h]) {
			anyHb = true;
			break;
		}
	}
	if (!anyHb)
		return AimbotSoftFail();

	if (AimCommon::AimTargetCount() <= 0 || !AimCommon::AimTargets())
		return AimbotSoftFail();

	// Engine local shoot origin (NetClientInfo ShootPosition) - not m_vOldOrigin
	const Vector_t lep = Bones::
GetShootPos(lp);
	if (!Bones::
IsValidPos(lep))
		return AimbotSoftFail();

	QAngle_t qView{};
	if (!GetViewAngles(qView))
		return AimbotSoftFail();

	// Player-input yield: surplus view delta vs our last write (minus our own
	// previous step) means the player is actively aiming - back off so the
	// aimbot never fights the mouse. Window re-arms on continued input.
	{
		const std::uint64_t now0 = NowMs();
		if (g_yieldActive && now0 >= g_yieldUntilMs) {
			g_yieldActive = false;
			g_aimRemX = 0.f;
			g_aimRemY = 0.f;
			ResetSmoothState();
		}
		if (g_haveLastWrite && g_havePrevWrite) {
			const float pdx = std::remainderf(qView.x - g_lastAimWrite.x, 360.f);
			const float pdy = std::remainderf(qView.y - g_lastAimWrite.y, 360.f);
			const float sdx = std::remainderf(g_lastAimWrite.x - g_prevAimWrite.x, 360.f);
			const float sdy = std::remainderf(g_lastAimWrite.y - g_prevAimWrite.y, 360.f);
			const float playerDeg = std::sqrt(pdx * pdx + pdy * pdy);
			const float ownStep = std::sqrt(sdx * sdx + sdy * sdy);
			if (playerDeg > ownStep + kYieldInputDeg) {
				if (!g_yieldActive) {
					g_yieldActive = true;
					g_lockedHitbox = -1; // re-pick best bone on resume
					ResetSmoothState();
				}
				g_yieldUntilMs = now0 + kYieldWindowMs;
			}
		}
	}

	// FOV = bullet direction (view + FULL punch). Menu rcs_scale is view-only;
	// half-scale FOV ranked wrong targets during spray (crosshair ? impact).
	QAngle_t qViewAim = qView;
	{
		QAngle_t fullPunch{};
		if (GetFirePunch(lp, fullPunch)) {
			qViewAim.x += fullPunch.x;
			qViewAim.y += fullPunch.y;
			qViewAim.Normalize();
		}
	}

	// Keep target acquisition exactly inside the configured FOV. A widened
	// sticky window made the aimbot continue tracking outside the HUD circle.
	const float baseFov = Config::
aimbot_fov;
	const float stickyFov = baseFov;

	// Candidate bridge for the existing lock/smoothing/fire state machine.

	QAngle_t bestAngle{};
	bool found = false;
	std::uint32_t bestHandle = 0;
	int bestHb = -1;
	QAngle_t lockedAngle{};
	bool lockedFound = false;
	int lockedHb = -1;

	// Prefer sticky hitbox if still valid; only switch if clearly better (0.45?)
	constexpr float kHbSwitchSlack = 0.45f;

	// Multipoint scan: the center is always a candidate, while enabled hitboxes
	// add live capsule edge points and keep the most exposed valid point.
	// Bloom is weapon+local state - identical for every hitboxxtarget in this
	// pass; the per-iteration call burned Nx8 vfunc pairs per CreateMove.
	const float scanBloom = LiveMultipointBloom(pWpn, lp);
	AimCandidate scanBest{};
	AimCandidate scanLocked{};
	for (int ti = 0; ti < AimCommon::AimTargetCount(); ++ti) {
		const AimCommon::AimTarget& target = AimCommon::AimTargets()[ti];
		C_CSPlayerPawn* pawn = target.pawn;
		if (!Mem::ValidEntity(pawn))
			continue;
		__try {
			if (pawn->m_iHealth() <= 0 || pawn->m_lifeState() != 0)
				continue;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}

		const std::uint32_t pawnHandle = target.handle;
		const bool isLocked = g_lockedTarget != 0 && pawnHandle == g_lockedTarget;
		const float maxFov = isLocked ? stickyFov : baseFov;
		AimCandidate targetBest{};
		AimCandidate targetSticky{};
		for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
			if (!Config::aim_hitboxes[hb])
				continue;
			Vector_t center{};
			if (!Bones::GetHitboxPoint(pawn, hb, center) || !IsFinitePoint(center))
				continue;

			Vector_t points[9]{};
			const float scale = AimMultipointScale(hb);
			const float bloom = scanBloom;
			const int pointCount = Bones::CollectHitboxMultipoints(
				pawn, hb, scale, points, 9, &lep, bloom, IsTargetAirborne(pawn));
			if (pointCount <= 0)
				continue;

			for (int pi = 0; pi < pointCount; ++pi) {
				const Vector_t rawPoint = points[pi];
				if (!IsFinitePoint(rawPoint))
					continue;
				Vector_t point{};
				if (!PredictAimPoint(pawn, rawPoint, point) || !IsFinitePoint(point))
					continue;
				QAngle_t angle{};
				if (!CalcAngles(lep, point, angle))
					continue;
				const float fov = GetFov(qViewAim, angle);
				if (!Mem::Finite(fov) || fov > maxFov)
					continue;

				const bool traceReady = Trace::Ready();
				if (Config::aim_vis_check && !traceReady)
					continue;
				const bool visible = traceReady
					&& Trace::IsVisible(lep, point, lp, pawn, Trace::kMaskVis);
				if (Config::aim_vis_check && !visible)
					continue;
				if (Config::aim_smoke_check && AimCommon::LineBlockedBySmoke(lep, point))
					continue;

				AimCandidate candidate{};
				candidate.valid = true;
				candidate.pawn = pawn;
				candidate.handle = pawnHandle;
				candidate.hitbox = hb;
				candidate.point = point;
				candidate.rawPoint = rawPoint;
				candidate.angle = angle;
				candidate.fov = fov;
				candidate.visible = Config::aim_vis_check && visible;
				if (BetterTarget(candidate, targetBest))
					targetBest = candidate;
				if (isLocked && g_lockedHitbox >= 0 && hb == g_lockedHitbox
					&& BetterPoint(candidate, targetSticky))
					targetSticky = candidate;
			}
		}

		if (!targetBest.valid)
			continue;
		AimCandidate pick = targetBest;
		if (isLocked && targetSticky.valid
			&& targetBest.fov + kHbSwitchSlack >= targetSticky.fov)
			pick = targetSticky;
		if (isLocked)
			scanLocked = pick;
		if (BetterTarget(pick, scanBest))
			scanBest = pick;
	}

	// Feed the existing lock, smoothing, RCS, and fire timing path.
	if (scanBest.valid) {
		bestAngle = scanBest.angle;
		bestHandle = scanBest.handle;
		bestHb = scanBest.hitbox;
		found = true;
	}
	if (scanLocked.valid) {
		lockedAngle = scanLocked.angle;
		lockedHb = scanLocked.hitbox;
		lockedFound = true;
	}


	if (!found) {
		// Brief miss while locked: keep delay state, don't re-trigger reaction/first-shot.
		// No view hold - freezing the camera for 8 frames kills the player's own
		// mouse input (target juked = "mouse dead" feel). Lock state survives; the
		// player's input works during the grace window.
		if (g_lockedTarget != 0 && g_lockGrace > 0) {
			--g_lockGrace;
			// firstShotDone stays false through reaction + first-shot window
			g_blockFirstShot = !g_firstShotDone;
			return true;
		}
		ResetHumanize();
		return false;
	}
	g_lockGrace = kLockGraceFrames;

	const std::
uint64_t now = NowMs();
	const float switchMs = std::
clamp(Config::
aim_target_switch_delay_ms, 0.f, 500.f);
	const float reactionMs = std::
clamp(Config::
aim_reaction_delay_ms, 0.f, 500.f);
	const float firstShotMs = std::
clamp(Config::
aim_first_shot_delay_ms, 0.f, 500.f);

	// Target switch delay
	if (g_lockedTarget == 0) {
		BeginEngagement(bestHandle, now);
	}
	else if (bestHandle != g_lockedTarget) {
		if (switchMs <= 0.01f) {
			BeginEngagement(bestHandle, now);
		}
		else {
			if (g_pendingTarget != bestHandle) {
				g_pendingTarget = bestHandle;
				g_switchReadyMs = now + static_cast<std::uint64_t>(switchMs);
			}
			if (now >= g_switchReadyMs)
				BeginEngagement(bestHandle, now);
		}
	}
	else {
		g_pendingTarget = 0;
		g_switchReadyMs = 0;
	}

	QAngle_t aimAngle{};
	int aimHb = bestHb;
	if (g_lockedTarget == bestHandle) {
		aimAngle = bestAngle;
		aimHb = bestHb;
	}
	else if (lockedFound) {
		aimAngle = lockedAngle;
		aimHb = lockedHb;
	}
	else {
		// Switch-wait with old lock lost: track best for view but do NOT re-engage
		// yet - the switch timer is still running (fire blocked before return).
		// Old code re-engaged instantly -> target-switch delay was dead in this case.
		if (!(g_pendingTarget == bestHandle && g_switchReadyMs > 0
			&& now < g_switchReadyMs))
			BeginEngagement(bestHandle, now);
		aimAngle = bestAngle;
		aimHb = bestHb;
	}

	// Remember hitbox for sticky tracking
	if (aimHb >= 0)
		g_lockedHitbox = aimHb;

	// RCS: smooth in aim-space (camera + punch) toward bone, then subtract
	// THE SAME punch. Bug was: SmoothToward used full punchScaled but subtract
	// used lagged GetRcsApplied -> under-comp during spray when rcs_smooth > 0.
	QAngle_t punchScaled{};
	const bool havePunch = Config::rcs && GetScaledPunch(lp, punchScaled);
	if (havePunch)
		(void)AimCommon::SmoothRcsStep(punchScaled);
	else
		AimCommon::ResetRcsSmooth();
	// One source of truth for add + subtract
	const QAngle_t rcsPunch = havePunch ? AimCommon::GetRcsApplied() : QAngle_t{};

	aimAngle.x = std::clamp(aimAngle.x, -89.f, 89.f);
	aimAngle.z = 0.f;

	if (!aimAngle.IsValid())
		return AimbotSoftFail();

	const float smooth = std::clamp(Config::aimbot_smooth, 0.f, 100.f);
	QAngle_t finalAngle = aimAngle;
	if (smooth > 0.01f) {
		QAngle_t cur{};
		if (GetViewAngles(cur) && cur.IsValid()) {
			QAngle_t ref = cur;
			if (havePunch) {
				// Match subtract: smooth from camera+applied toward bone
				ref.x += rcsPunch.x;
				ref.y += rcsPunch.y;
				ref.Normalize();
			}
			finalAngle = SmoothToward(ref, aimAngle, smooth);
		}
	}
	if (havePunch) {
		finalAngle.x -= rcsPunch.x;
		finalAngle.y -= rcsPunch.y;
		finalAngle.Normalize();
	}
	finalAngle.x = std::clamp(finalAngle.x, -89.f, 89.f);
	finalAngle.z = 0.f;

	if (!finalAngle.IsValid())
		return AimbotSoftFail();

	// Reaction / first-shot: fire blocked, camera still eases toward bone.
	// Old path froze the view for the whole reaction window then snapped -
	// delay-then-snap, looked like silent + "reaction not working".
	const bool inReaction = reactionMs > 0.01f
		&& (now - g_lockAcquiredMs) < static_cast<std::uint64_t>(reactionMs);

	// Preferred view update: non-silent moves camera; silent stamps usercmd angles directly.
	// Silent applies ONLY when the fire path is silent THIS frame (triggerbot magnet
	// actually firing - its WantShoot lands from last frame). Config alone must not
	// turn the plain aimbot into silent cmd stamping - camera would never move with
	// autofire_silent/trigger_magnet_silent enabled while just holding the aim key.
	const bool isSilentAim =
		(Autofire::WantShoot() && Config::autofire_silent)
		|| (Triggerbot::WantShoot() && Config::trigger_magnet_silent);
	if (!isSilentAim) {
		const bool yielding = g_yieldActive && now < g_yieldUntilMs;
		if (!yielding) {
			if (smooth > 0.01f) {
				QAngle_t liveView{};
				if (GetViewAngles(liveView) && liveView.IsValid())
					AimCommon::QuantizeViewStep(finalAngle, liveView, g_aimRemX, g_aimRemY);
			}
			SetViewAngles(finalAngle);
			g_prevAimWrite = g_lastAimWrite;
			g_lastAimWrite = finalAngle;
			g_havePrevWrite = g_haveLastWrite;
			g_haveLastWrite = true;
		} else {
			// Player owns the camera - re-anchor the baseline so only NEW input
			// keeps the yield alive, and the aimbot resumes from the player's view.
			g_prevAimWrite = qView;
			g_lastAimWrite = qView;
			g_havePrevWrite = true;
			g_haveLastWrite = true;
		}
	} else {
		AimCommon::StampCmdAngles(finalAngle);
	}

	if (inReaction) {
		g_blockFirstShot = true;
		return true;
	}

	// First shot delay (strip LMB only - smooth already running above)
	if (!g_firstShotArmed) {
		g_firstShotArmed = true;
		g_firstShotReadyMs = now + static_cast<std::
uint64_t>(firstShotMs);
		g_firstShotDone = (firstShotMs <= 0.01f);
	}

	if (!g_firstShotDone) {
		if (now < g_firstShotReadyMs)
			g_blockFirstShot = true;
		else {
			g_blockFirstShot = false;
			g_firstShotDone = true;
		}
	} else {
		g_blockFirstShot = false;
	}

	// Switch-wait with old lock lost: never fire before the switch timer
	// completes (first-shot logic above may have cleared the block).
	if (g_lockedTarget != bestHandle && g_pendingTarget == bestHandle
		&& g_switchReadyMs > 0 && now < g_switchReadyMs)
		g_blockFirstShot = true;

	// If we killed/acquried new target mid-delay, reaction re-arms via
	// BeginEngagement (g_lockAcquiredMs reset). First-shot re-arms below
	// only on the frame reaction expires (g_firstShotArmed = false after
	// ResetHumanize/BeginEngagement). But if reactionMs=0, first-shot
	// logic fires on the same frame as BeginEngagement. That frame's
	// g_firstShotDone goes true immediately (firstShotMs <= 0.01f).
	// All good - just ensure g_blockFirstShot is clear for subsequent frames.
	// Do NOT ApplyDeltaRcs while aimbot-RCS is active - absolute punch already applied
	return true;
}



// IDA CCSGOInput button words - auto_pistol: only +0x258 = IsButtonActive code 3
// std::
uintptr_t kCsgoBtn0 = 0x250;
constexpr std::
uintptr_t kCsgoBtn1 = 0x258;
constexpr std::
uintptr_t kCsgoBtn2 = 0x260;
constexpr std::
uintptr_t kCsgoBtn3 = 0x268;

static void ClearCsgoAttackInput()
{
	void* pInput = Input::
GetCSGOInput();
	if (!pInput)
		return;
	constexpr std::
uint64_t kAttack = IN_ATTACK;
	__try {
		auto* b = reinterpret_cast<std::
uint8_t*>(pInput);
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn0) &= ~kAttack;
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn1) &= ~kAttack;
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn2) &= ~kAttack;
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn3) &= ~kAttack;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Rising edge for semi: clear other slots, set active word (+0x258)
// 
// 
static void EdgeCsgoAttackInput()
{
	void* pInput = Input::
GetCSGOInput();
	if (!pInput)
		return;
	constexpr std::
uint64_t kAttack = IN_ATTACK;
	__try {
		auto* b = reinterpret_cast<std::
uint8_t*>(pInput);
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn0) &= ~kAttack;
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn2) &= ~kAttack;
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn3) &= ~kAttack;
		*reinterpret_cast<std::
uint64_t*>(b + kCsgoBtn1) |= kAttack;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Semi/bolt (AWP, deagle, pistols...): rising edge required. Sticky WantShoot holds
// IN_ATTACK -> FIRE log, zero bullets. Full-auto holds bit only.
// After semi press, release next CreateMove so next arm can edge again.
static bool s_semiNeedRelease = false;
// Full-auto: track OUR hold state across frames. cmd is fresh each CreateMove so
// cmd->nButtons.nValue only reflects player input, not our previous AF press.
// Without this, nValueChanged is set every frame -> server sees repeated "press"
// events -> spray desync (bullets climb / random spread reset).
static bool s_autoHeld = false;
// Semi post-fire debounce: player held M1 through our release frame -> engine
// re-reads physical hold on fresh cmd next frame -> stale rising edge -> double
// fire. Strip once to kill the ghost click.
static bool g_stripHeldM1 = false;

static void StripAttack(CUserCmd* cmd) {
		if (!cmd)
			return;
		constexpr std::
uint64_t kAttackMask = IN_ATTACK | IN_SECOND_ATTACK;
		cmd->nButtons.nValue &= ~kAttackMask;
		cmd->nButtons.nValueChanged &= ~kAttackMask;
		cmd->nButtons.nValueScroll &= ~kAttackMask;

		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (base && base->pInButtonState) {
			base->pInButtonState->nValue &= ~kAttackMask;
			base->pInButtonState->nValueChanged &= ~kAttackMask;
			base->pInButtonState->nValueScroll &= ~kAttackMask;
		}
		cmd->csgoUserCmd.nAttack1StartHistoryIndex = -1;
		cmd->csgoUserCmd.nAttack2StartHistoryIndex = -1;
		// Kill engine-packed protobuf subtick steps for attack so server
		// does not see our release AND a stale engine press in the same cmd.
		if (base)
			InputInject::
ClearAttackSubticks(base);
		s_autoHeld = false;
	}

static void PressScope(CUserCmd* cmd) {
	if (!cmd)
		return;
	__try {
		constexpr std::
uint64_t kScope = IN_SECOND_ATTACK;
		cmd->nButtons.nValue |= kScope;
		cmd->nButtons.nValueChanged |= kScope;
		// Alive only - dead/respawn moveSvc free'd (TDM crash)
		if (C_CSPlayerPawn* lp = H::
SafeLocalAlive())
			InputInject::
ForceDown(lp, kScope);
		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (base && base->pInButtonState
			&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
			base->pInButtonState->nValue |= kScope;
			base->pInButtonState->nValueChanged |= kScope;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static bool ActiveWeaponIsSemi()
{
	C_CSPlayerPawn* lp = H::
SafeLocalAlive();
	if (!lp)
		return false;
	C_CSWeaponBase* w = nullptr;
	__try { w = lp->GetActiveWeapon(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return Mem::
ValidEntity(w) && AimCommon::
IsSemiWeapon(w);
}

static void PressAttack(CUserCmd* cmd) {
	if (!cmd)
		return;

	__try {
		constexpr std::
uint64_t kAttack = IN_ATTACK;
		// Full-auto: hold bit, edge only on first down (re-edge every CM = spray desync).
		// Semi/bolt: CS2 needs rising edge on cmd AND CCSGOInput+0x258.
		// Holding M1 / sticky WantShoot keeps IN_ATTACK -> no edge -> seed FIRE, no bullet.
		// Match auto_pistol: clear -> value|changed -> inject +0x258 -> release next CM.
		const bool semi = ActiveWeaponIsSemi();
		if (semi) {
			// Force release state first so this frame is a true press edge
			cmd->nButtons.nValue &= ~kAttack;
			cmd->nButtons.nValueChanged &= ~kAttack;
			cmd->nButtons.nValueScroll &= ~kAttack;
			if (CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd) {
				if (base->pInButtonState
					&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
					base->pInButtonState->nValue &= ~kAttack;
					base->pInButtonState->nValueChanged &= ~kAttack;
					base->pInButtonState->nValueScroll &= ~kAttack;
				}
			}
			ClearCsgoAttackInput();
		}

		// Capture hold state BEFORE latching s_autoHeld - old order set latch
		// first so PB path always saw pbDown=true and skipped nValueChanged/SetBits
		// on the first full-auto shot (no rising edge on protobuf buttons).
		const bool alreadyDown = s_autoHeld || (cmd->nButtons.nValue & kAttack) != 0;
		cmd->nButtons.nValue |= kAttack;
		if (semi || !alreadyDown)
			cmd->nButtons.nValueChanged |= kAttack;
		cmd->nButtons.nValueScroll &= ~kAttack;

		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (base && base->pInButtonState
			&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
			const bool pbDown = s_autoHeld || (base->pInButtonState->nValue & kAttack) != 0;
			base->pInButtonState->nValue |= kAttack;
			if (semi || !pbDown)
				base->pInButtonState->nValueChanged |= kAttack;
			base->pInButtonState->nValueScroll &= ~kAttack;
			// Only signal protobuf state change on first press (semi always edges)
			if (semi || !pbDown) {
				base->pInButtonState->SetBits(
					BUTTON_STATE_PB_BITS_BUTTONSTATE1
					| BUTTON_STATE_PB_BITS_BUTTONSTATE2
					| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
				base->SetBits(BASE_BITS_BUTTONPB);
			}
		}

		if (semi)
			s_semiNeedRelease = true;
		else
			s_autoHeld = true;

		if (semi)
			EdgeCsgoAttackInput();

		// Bind attack-hist. Seed path already wrote tick/angles/eye in AF/trigger
		// FIRE - do NOT re-StampInputHistory (WriteSubtickFromEntry can skew
		// nPlayerTickCount / frac -> different SPREADSEEDGEN hash -> miss).
		if (Triggerbot::
WantShoot() || Autofire::
WantShoot()) {
			const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			int idx = cmd->csgoUserCmd.nAttack1StartHistoryIndex;
			if (Triggerbot::
FireValid() && Triggerbot::
FireHistIndex() >= 0)
				idx = Triggerbot::
FireHistIndex();
			else if (Autofire::
SilentValid() && Autofire::
FireHistIndex() >= 0)
				idx = Autofire::
FireHistIndex();
			if (idx < 0 && histCount > 0)
				idx = histCount - 1;
			// Upper clamp too: a stale game index from an older larger history
			// would reach GetInputHistoryEntry out of range (same class as the
			// autofire fix - the lower clamp alone was not enough).
			if (idx >= histCount)
				idx = histCount > 0 ? histCount - 1 : -1;
			if (idx >= 0 && histCount > 0) {
				cmd->csgoUserCmd.SetAttack1StartHistoryIndex(idx);

				CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(idx);
				const bool trigFire = Triggerbot::
WantShoot() && Triggerbot::
FireValid();
				const bool afFire = Autofire::
WantShoot() && Autofire::
SilentValid();

				QAngle_t ang{};
				Vector_t eye{};
				bool haveAng = false;

				if (trigFire) {
					ang = Triggerbot::
FireAngle();
					eye = Triggerbot::
FireEye();
					haveAng = true;
				} else if (afFire) {
					ang = Autofire::
SilentAngle();
					eye = Autofire::
SilentEye();
					haveAng = true;
				}

							const bool seedMode = trigFire
					? (Config::
trigger_mode == Config::
TR_MODE_SEED_NOSPREAD)
					: (Config::
autofire_mode == Config::
AF_MODE_SEED_NOSPREAD);

				if (haveAng && ang.IsValid()) {
					const bool trigSilent = trigFire && (Config::
trigger_magnet && Config::
trigger_magnet_silent);
					const bool afSilent = afFire && Config::
autofire_silent;
					const bool isSilent = trigSilent || afSilent;

					if (!seedMode) {
						// Silent: only write to attack hist entry, NOT base cmd
						// (base->pViewAngles is GOTV/spectator stream - leave intact).
						const bool narrow = isSilent;
						cmd->SetAttackHistoryFire(idx, ang, eye, narrow);
					}
					if (Bones::
IsValidPos(eye)) {
						QAngle_t rayAng = ang;
						QAngle_t punch{};
						if (AimCommon::
GetFirePunch(H::
SafeLocalPlayer(), punch)
							&& punch.IsValid()) {
							rayAng.x += punch.x;
							rayAng.y += punch.y;
						}
						rayAng.z = 0.f;
						rayAng.Normalize();
						Hitmarker::
NoteLastFire(eye, rayAng);
					}
				}

				// seed: hist already written by AF/trigger Solve - WriteSubtickFromEntry
				// can skew nPlayerTickCount / frac -> SPREADSEEDGEN miss online.
				if (e && !seedMode && haveAng && ang.IsValid()) {
					int ptick = e->nPlayerTickCount > 0 ? e->nPlayerTickCount : 0;
					int rtick = e->nRenderTickCount > 0 ? e->nRenderTickCount : ptick;
					if (ptick > 0 && rtick > 0) {
						const int skew = rtick > ptick ? rtick - ptick : ptick - rtick;
						if (skew > 1)
							rtick = ptick;
					}
					float pfrac = e->flPlayerTickFraction;
					float rfrac = e->flRenderTickFraction;
					if (!std::
isfinite(pfrac)) pfrac = 0.f;
					if (!std::
isfinite(rfrac)) rfrac = pfrac;
					if (pfrac < 0.f || pfrac >= 1.f) pfrac = 0.f;
					if (rfrac < 0.f || rfrac >= 1.f) rfrac = pfrac;
					if (ptick > 0)
						Pred::
StampInputHistory(e, rtick, rfrac, ptick, pfrac, ang, eye);
				}
			} else if (cmd->csgoUserCmd.nAttack1StartHistoryIndex >= 0) {
				cmd->csgoUserCmd.CheckAndSetBits(CCSGOUserCmdPB::
BITS_ATTACK1START);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::
Seh("PressAttack", GetExceptionCode());
	}
}

// Counter-strafe: cancel horizontal velocity for standing accuracy
static void ApplyAutoStop(CUserCmd* cmd, C_CSPlayerPawn* lp) {
	if (!cmd || !lp)
		return;

	const Vector_t vel = Pred::
Velocity(lp);
	const float speed2d = std::
sqrt(vel.x * vel.x + vel.y * vel.y);
	if (!std::
isfinite(speed2d) || speed2d < 1.f)
		return;

	QAngle_t view{};
	if (!GetViewAngles(view))
		return;

	Vector_t fwd{}, right{};
	view.ToDirections(&fwd, &right, nullptr);
	fwd.z = 0.f;
	right.z = 0.f;
	const float fl = fwd.Length();
	const float rl = right.Length();
	if (fl > 1e-4f) { fwd.x /= fl; fwd.y /= fl; }
	if (rl > 1e-4f) { right.x /= rl; right.y /= rl; }

	// Opposite of velocity projected onto move axes
	float fmove = -(vel.x * fwd.x + vel.y * fwd.y);
	float smove = -(vel.x * right.x + vel.y * right.y);
	const float mlen = std::
sqrt(fmove * fmove + smove * smove);
	if (mlen < 1e-4f)
		return;

	constexpr float kMaxMove = 450.f;
	fmove = (fmove / mlen) * kMaxMove;
	smove = (smove / mlen) * kMaxMove;

	constexpr std::
uint64_t kMoveKeys =
		IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;
	cmd->nButtons.nValue &= ~kMoveKeys;
	cmd->nButtons.nValueChanged &= ~kMoveKeys;
	cmd->nButtons.nValueScroll &= ~kMoveKeys;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (base) {
		base->flForwardMove = fmove;
		base->flSideMove = smove;
		base->flUpMove = 0.f;
		if (base->pInButtonState) {
			base->pInButtonState->nValue &= ~kMoveKeys;
			base->pInButtonState->nValueChanged &= ~kMoveKeys;
			base->pInButtonState->nValueScroll &= ~kMoveKeys;
		}
	}
}


} // namespace

	void Aimbot_Reset() {
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		AimCommon::g_hadPunch = false;
		AimCommon::ResetRcsSmooth();
		AimCommon::ClearTargets();
		AimCommon::UnbindCmd();
		s_semiNeedRelease = false;
		s_autoHeld = false;
		g_semiCooldown = false;
	}

void AimHumanize_OnCreateMove(CUserCmd* cmd) {
	if (!cmd)
		return;

	// Belt-and-suspenders: never counter-strafe unless the matching Autostop toggle is on.
	const bool wantStop =
		(Config::
autofire_autostop && Autofire::
WantStop())
		|| (Config::
trigger_autostop && Triggerbot::
WantStop());
	if (wantStop) {
		// Alive only - dead/respawn moveSvc free'd (TDM crash)
		if (C_CSPlayerPawn* lp = H::
SafeLocalAlive()) {
			// Autostop is ground-only - never rewrite move while airborne
			if ((Pred::
Flags(lp) & FL_ONGROUND) != 0)
				ApplyAutoStop(cmd, lp);
		}
	}

	// Fire only when AF/trigger armed WantShoot.
	// Strip manual M1 during aimbot OR autofire reaction/first-shot windows -
	// old path only used g_blockFirstShot (aimbot). AF BlockFirstShot was ignored
	// so holding M1 during AF delays bypassed humanize + desynced seed hist.
	//
	// Semi/bolt (AWP/deagle/pistols): press (cmd edge + CCSGOInput+0x258) one CM,
	// full release next. Without release, held M1 / sticky +0x258 never rises
	// -> seed FIRE log, no bullet (log 00:48 flood).
	const bool afFire = Autofire::
WantShoot() || Triggerbot::
WantShoot();
	if (s_semiNeedRelease) {
		// Release unconditionally - don't gate on ActiveWeaponIsSemi().
		// Weapon may have switched (quickswitch) between press and release frame;
		// gating on current weapon leaves old edge stuck -> next semi has no rising edge.
		// Track if player held M1 through release -> next frame needs debounce strip
		// to prevent stale re-read as fresh rising edge (double-fire).
		g_stripHeldM1 = (cmd->nButtons.nValue & IN_ATTACK) != 0;
		StripAttack(cmd);
		ClearCsgoAttackInput();
		s_semiNeedRelease = false;
		s_autoHeld = false;
		g_semiCooldown = true; // block re-fire next frame
	} else if (g_semiCooldown) {
		// Semi cooldown frame: afFire still true but we skip PressAttack to let
		// the release edge settle. Allows rising edge on the frame after.
		g_semiCooldown = false;
		// Debounce: if player injected M1 while release was stripping, clear it
		if (cmd->nButtons.nValue & IN_ATTACK)
			StripAttack(cmd);
	} else if (afFire) {
		PressAttack(cmd);
	} else if (g_stripHeldM1) {
		// Post-release debounce: player held M1 through our release -> stripped M1
		// dropped from cmd for one frame. Engine re-reads physical hold on fresh cmd
		// -> IN_ATTACK rises again without intentional click -> double-fire on semi.
		// Strip once to kill the stale edge.
		if (cmd->nButtons.nValue & IN_ATTACK)
			StripAttack(cmd);
		g_stripHeldM1 = false;
	} else if ((g_blockFirstShot && !(cmd->nButtons.nValue & IN_ATTACK))
		|| Autofire::
BlockFirstShot()) {
		// Aimbot reaction/first-shot/switch delays must never eat the PLAYER's
		// manual clicks (pistol spam felt broken) - strip only when the player
		// isn't firing this frame. Autofire's own block still gates bot fire.
		StripAttack(cmd);
		if (ActiveWeaponIsSemi())
			ClearCsgoAttackInput();
		s_semiNeedRelease = false;
	} else {
		// Clear stale s_autoHeld when player releases M1 without AF controlling it.
		// Without this, s_autoHeld stays true from last AF burst -> nValueChanged not
		// set on next burst first frame -> missed first shot.
		if (s_autoHeld && !(cmd->nButtons.nValue & IN_ATTACK))
			s_autoHeld = false;
		if (s_semiNeedRelease) {
			ClearCsgoAttackInput();
			s_semiNeedRelease = false;
		}
	}

	// Manual fire ray for hitmarker world marks - guns only
	// (knife M1 must not spawn beams). AF/TR already call NoteLastFire in
	// PressAttack. Without this stamp, manual shots had no fire ray and the
	// hitmarker fell back to hitbox/skeleton guesses.
	if (((Config::hitmarker && Config::hitmarker_world)
		|| Config::float_damage) && !afFire
		&& (cmd->nButtons.nValue & IN_ATTACK)
		&& !g_blockFirstShot
		&& !Autofire::
BlockFirstShot()) {
		__try {
			C_CSPlayerPawn* lp = H::
SafeLocalPlayer();
			if (lp) {
				C_CSWeaponBase* w = nullptr;
				__try { w = lp->GetActiveWeapon(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
				if (w && Mem::
ValidEntity(w) && !w->IsNonGunWeapon()) {
					Vector_t eye = Bones::
GetShootPos(lp);
					QAngle_t view{};
					if (Bones::
IsValidPos(eye) && AimCommon::
GetViewAngles(view) && view.IsValid()) {
						QAngle_t punch{};
						if (AimCommon::
GetFirePunch(lp, punch) && punch.IsValid()) {
							view.x += punch.x;
							view.y += punch.y;
						}
						view.z = 0.f;
						view.Normalize();
						Hitmarker::
NoteLastFire(eye, view);
					}
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("aim.hitmarkerFire"); }
	}

	// Autoscope after strip/fire so attack2 is not wiped by StripAttack.
	if (Autofire::
WantScope())
		PressScope(cmd);
}

void Aimbot(CUserCmd* cmd) {
	// Cleared every CreateMove; RunAimbot re-arms only while its delays are active.
	// Prevents sticky strip when AF owns the frame (RunAimbot not called).
	// BindCmd: sehCreateMovePost (spans AimHumanize PressAttack stamp).
	g_blockFirstShot = false;
	(void)
cmd;

	if (g_bMenuOpen) {
		// Still track held weapon group for menu "Live:" / Jump Active / PushLive.
		// Do not run combat; do not ApplyProfile (menu PushLive owns live when editing active).
		if (C_CSPlayerPawn* mlp = H::SafeLocalAlive()) {
			C_CSWeaponBase* wpn = mlp->GetActiveWeapon();
			if (Mem::ValidEntity(wpn) && !wpn->IsNonGunWeapon())
				Config::weapon_group_active = Config::ClassifyWeaponGroup(wpn);
			else
				Config::weapon_group_active = Config::WG_GENERAL;
		}
		// Drop stale pawn* list - entity slots recycle while menu sits open
		AimCommon::ClearTargets();
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		g_hadPunch = false;
		return;
	}
	// Priority A: soft-disable combat on Overwatch / demo / HLTV
	if (SdkPrioA::ShouldSoftDisableCombat()) {
		AimCommon::ClearTargets();
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		g_hadPunch = false;
		return;
	}
	if (!Input::GetViewAngles || !Input::SetViewAngle || !Input::viewAngleContext) {
		AimCommon::ClearTargets();
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		g_hadPunch = false;
		return;
	}

	// Alive only - dead pawn weapon services free mid-TDM
	C_CSPlayerPawn* lp = H::SafeLocalAlive();
	if (!lp) {
		AimCommon::ClearTargets();
		g_hadPunch = false;
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		return;
	}

	// Keep live aim settings synced to held weapon group (FOV circle, etc.)
	{
		C_CSWeaponBase* wpn = nullptr;
		__try { wpn = lp->GetActiveWeapon(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { wpn = nullptr; }
		if (Mem::
ValidEntity(wpn) && !wpn->IsNonGunWeapon())
			Config::
ApplyWeaponGroup(wpn);
		else
			Config::
ApplyWeaponGroup(nullptr);
	}

	// Target list built once in sehCreateMovePost.
	// Rebuild only if empty (menu path / early return skipped collect).
	// CombatActive gate: nothing consumes targets this tick - do not walk.
	if (AimCommon::AimTargetCount() <= 0 && AimCommon::CombatActive())
		AimCommon::
CollectAimTargets(lp);

	// Autofire owns aim only while Run() returns true (tracking / grace / delays).
	// Silent AF key held with no target must NOT block aimbot - old silentAfHold
	// killed visible aim whenever the AF key was down without a lock.
	const bool af = Autofire::Run(lp, cmd);
	if (af) {
		// AF owns the camera this tick - drop aimbot yield so a leftover
		// 140 ms window does not freeze the mouse on AF->aimbot handoff.
		g_yieldActive = false;
		g_yieldUntilMs = 0;
		g_havePrevWrite = false;
		g_haveLastWrite = false;
		g_aimRemX = 0.f;
		g_aimRemY = 0.f;
	}
	const bool aimed = af ? true : RunAimbot(lp);
	if (!af)
		Autofire::ClearShootFlags();

	// Triggerbot: skip entirely while autofire owns aim (avoid dual HC/seed)
	if (!af)
		Triggerbot::
Run(lp, cmd);
	else
		Triggerbot::
ClearShootFlags();

	// Standalone RCS only when aimbot/autofire not locking (avoids double RCS).
	// While absolute aimbot RCS owns the frame: seed delta baseline from live punch
	// so standalone doesn't spike view when lock drops mid-spray.
	// On aimbot->standalone transition: reset baseline so first standalone frame
	// seeds fresh instead of applying stale delta (punch recovery spike).
	static bool s_wasAimed = false;
	if (Config::rcs_standalone && !aimed) {
		if (s_wasAimed) {
			// Transition frame: seed baseline, don't apply delta
			g_hadPunch = false;
			AimCommon::ResetRcsSmooth();
		}
		StandaloneRcs(lp);
	} else if (aimed && Config::rcs) {
		// Aimbot already ran SmoothRcsStep on scaled punch - seed standalone
		// baseline in same units (not raw punch) so first free-spray frame
		// doesn't delta against wrong scale.
		QAngle_t p{};
		if (AimCommon::GetScaledPunch(lp, p)) {
			g_oldPunch = p;
			g_hadPunch = true;
		}
	} else if (!Config::rcs && !Config::rcs_standalone) {
		g_hadPunch = false;
		AimCommon::ResetRcsSmooth();
	}
	s_wasAimed = aimed;
}

