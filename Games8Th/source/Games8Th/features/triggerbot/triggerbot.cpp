#include "triggerbot.h"
#include "../aim/aim_common.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../nospread/nospread.h"
#include "../autowall/autowall.h"
#include "../prediction/prediction.h"
#include "../sdk_prio_a/sdk_prio_a.h"

#include "../gamemode/gamemode.h"
#include "../../utils/memory/memsafe/memsafe.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Triggerbot {

bool g_triggerWantShoot = false;
bool g_triggerWantStop = false;
uint64_t g_triggerOnSinceMs = 0;
bool g_triggerWasOn = false;
int g_triggerOnMissFrames = 0;
// Delay is ARMED once the first on-target window satisfied it - brief target
// drops afterwards must not restart the countdown (the old full reset turned
// any delay>0 into a dead trigger while standing on a strafing target).
bool g_triggerDelayArmed = false;
QAngle_t g_triggerFireAngle{};
Vector_t g_triggerFireEye{};
bool g_triggerFireValid = false;
int g_triggerFireHist = -1;
float g_triggerAngSpeed = 0.f;
QAngle_t g_triggerPrevCam{};
bool g_triggerPrevCamOk = false;

// Target-loss tolerance in TIME, not frames - 3 frames @64fps is 47 ms but
// only 7.5 ms @400fps, which reset the contact clock constantly.
constexpr std::uint64_t kMissToleranceMs = 140;

// Magnet sticky lock (pawn handle + hitbox) - kills head/chest thrash
struct MagnetLock {
	uint32_t handle = 0;
	int hb = 0; // Config::HB_HEAD, avoid static init order with Config
	Vector_t point{};
	QAngle_t viewAng{}; // punch-compensated view to write
	QAngle_t fireAng{}; // bullet direction (view + punch)
	bool valid = false;
	int missFrames = 0;
};
MagnetLock g_magnet{};
uint64_t g_magnetLastMs = 0;

void ClearMagnetLock() {
	g_magnet = MagnetLock{};
	g_magnetLastMs = 0;
}

void ResetInternal() {
	g_triggerWantShoot = false;
	g_triggerWantStop = false;
	g_triggerOnSinceMs = 0;
	g_triggerWasOn = false;
	g_triggerOnMissFrames = 0;
	g_triggerDelayArmed = false;
	g_triggerFireValid = false;
	g_triggerFireHist = -1;
	g_triggerFireEye = Vector_t{ 0.f, 0.f, 0.f };
	g_triggerAngSpeed = 0.f;
	g_triggerPrevCamOk = false;
	ClearMagnetLock();
}

void ResetTargetPresence() {
	g_triggerOnSinceMs = 0;
	g_triggerWasOn = false;
	g_triggerOnMissFrames = 0;
	g_triggerDelayArmed = false;
	ClearMagnetLock();
}

// Hitbox priority for magnet (lower = better). Used when FOV within ?.
int MagnetHbPriority(int hb) {
	switch (hb) {
	case Config::
HB_HEAD: return 0;
	case Config::
HB_NECK: return 1;
	case Config::
HB_CHEST: return 2;
	case Config::
HB_STOMACH: return 3;
	case Config::
HB_PELVIS: return 4;
	case Config::
HB_ARMS: return 5;
	case Config::
HB_LEGS: return 6;
	case Config::
HB_FEET: return 7;
	default: return 9;
	}
}

bool MagnetUsesHb(int hb) {
	if (hb < 0 || hb >= Config::
HB_COUNT)
		return false;
	// Dedicated magnet list if any box enabled; else trigger fire hitboxes
	bool anyMag = false;
	for (int i = 0; i < Config::
HB_COUNT; ++i) {
		if (Config::
trigger_magnet_hitboxes[i]) {
			anyMag = true;
			break;
		}
	}
	if (anyMag)
		return Config::
trigger_magnet_hitboxes[hb];
	return Config::
trigger_hitboxes[hb];
}

// Pure exponential smooth - NO aimbot humanize (that caused magnet wobble).
QAngle_t MagnetSmoothToward(const QAngle_t& cur, const QAngle_t& target, float smooth) {
	QAngle_t goal = target;
	goal.z = 0.f;
	goal.Normalize();
	goal.x = std::
clamp(goal.x, -89.f, 89.f);
	if (!goal.IsValid() || !cur.IsValid())
		return cur;
	if (smooth <= 0.01f)
		return goal;

	float dx = std::
remainderf(goal.x - cur.x, 360.f);
	float dy = std::
remainderf(goal.y - cur.y, 360.f);
	const float err = std::
sqrt(dx * dx + dy * dy);
	const float dead = std::
clamp(Config::
trigger_magnet_deadzone, 0.f, 1.f);
	if (err <= dead)
		return cur;

	const float st = std::
clamp(smooth, 1.f, 50.f);
	// ~64-tick reference so feel matches aim smooth scale without humanize
	const float factor = std::
clamp(1.f / st, 0.02f, 1.f);
	// Stronger when far (edge of FOV), softer near center
	const float maxFov = std::
clamp(Config::
trigger_magnet_fov, 0.5f, 30.f);
	const float edge = std::
clamp(err / maxFov, 0.f, 1.f);
	const float shaped = factor * (0.55f + 0.45f * edge);

	QAngle_t out = cur;
	out.x += dx * shaped;
	out.y += dy * shaped;
	out.z = 0.f;
	out.Normalize();
	out.x = std::
clamp(out.x, -89.f, 89.f);
	return out;
}

// Aim point on hitbox: prefer ray?capsule toward cam, else multipoint nearest FOV, else center.
bool MagnetPickPoint(
	C_CSPlayerPawn* pawn,
	int hb,
	const Vector_t& eye,
	const Vector_t& camDir,
	Vector_t& outPt)
{
	if (!pawn || hb < 0 || hb >= Config::
HB_COUNT)
		return false;

	// 1) Ray from eye along cam ? configured hitbox (surface under reticle path)
	{
		float t = 0.f;
		Vector_t pt{};
		if (Bones::
RayHitsConfiguredHitbox(pawn, hb, eye, camDir, 1.02f, t, pt, 1.f)
			&& Bones::
IsValidPos(pt)) {
			outPt = pt;
			return true;
		}
	}

	// 2) Multipoint cloud - closest angular to cam
	Vector_t mpts[12]{};
	const int nMp = Bones::
CollectHitboxMultipoints(
		pawn, hb, 0.55f, mpts, 12, &eye, -1.f, false);
	if (nMp > 0) {
		float bestFov = 1.0e9f;
		Vector_t best{};
		bool any = false;
		QAngle_t camAng{};
		const float camLen2D = std::sqrt(camDir.x * camDir.x + camDir.y * camDir.y);
		camAng.x = std::atan2(-camDir.z, (std::max)(1e-4f, camLen2D)) * AimCommon::kRad2Deg;
		camAng.y = std::atan2(camDir.y, camDir.x) * AimCommon::kRad2Deg;
		camAng.z = 0.f;
		for (int i = 0; i < nMp; ++i) {
			if (!Bones::
IsValidPos(mpts[i]))
				continue;
			QAngle_t a{};
			if (!AimCommon::
CalcAngles(eye, mpts[i], a))
				continue;
			const float fov = AimCommon::
GetFov(camAng, a);
			if (fov >= bestFov)
				continue;
			bestFov = fov;
			best = mpts[i];
			any = true;
		}
		if (any) {
			outPt = best;
			return true;
		}
	}

	// 3) Capsule / hitbox center
	return Bones::
GetHitboxPoint(pawn, hb, outPt) && Bones::
IsValidPos(outPt);
}

bool MagnetPawnOk(C_CSPlayerPawn* pawn, C_CSPlayerPawn* lp, uint8_t localTeam) {
	if (!pawn || !lp || pawn == lp || !Mem::
ValidEntity(pawn))
		return false;
	__try {
		const int hp = Mem::
ClampHealth(pawn->m_iHealth());
		if (hp < 1 || pawn->m_lifeState() != 0)
			return false;
		if (AimCommon::
IsTargetImmune(pawn))
			return false;
		const uint8_t team = pawn->m_iTeamNum();
		if (!Mem::
ValidTeam(static_cast<int>(team)))
			return false;
		if (GameMode::
WantTeamCheck(Config::
team_check) && team == localTeam)
			return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

// Returns true if magnet locked a bone this tick (g_magnet filled; angles applied).
bool RunMagnet(
	C_CSPlayerPawn* lp,
	C_CSWeaponBase* pWpn,
	const Vector_t& eye,
	const QAngle_t& camIn,
	uint8_t localTeam,
	bool trAwOn,
	QAngle_t& camOut)
{
	camOut = camIn;
	if (!Config::
trigger_magnet || !lp || !Bones::
IsValidPos(eye) || !camIn.IsValid()) {
		ClearMagnetLock();
		return false;
	}
	// Always on (no menu) - no pull during reload / fire cooldown
	if (!pWpn || !AimCommon::
CanWeaponFire(pWpn, lp)) {
		// Keep sticky identity but don't pull while can't shoot
		return g_magnet.valid;
	}

	const float maxFov = std::
clamp(Config::
trigger_magnet_fov, 0.5f, 30.f);
	const float stickyFov = maxFov * 1.35f;
	const float dead = std::
clamp(Config::
trigger_magnet_deadzone, 0.f, 1.f);

	Vector_t camDir{};
	{
		QAngle_t c = camIn;
		c.z = 0.f;
		c.ToDirections(&camDir, nullptr, nullptr);
		const float fl = camDir.Length();
		if (fl < 1e-4f)
			return false;
		camDir.x /= fl; camDir.y /= fl; camDir.z /= fl;
	}

	// Sticky: revalidate locked pawn+HB first
	if (g_magnet.valid && g_magnet.handle != 0) {
		C_CSPlayerPawn* sticky = nullptr;
		const int n = AimCommon::
AimTargetCount();
		const AimCommon::
AimTarget* tlist = AimCommon::
AimTargets();
		for (int i = 0; i < n; ++i) {
			if (tlist[i].handle == g_magnet.handle && tlist[i].pawn) {
				sticky = tlist[i].pawn;
				break;
			}
		}
		if (sticky && Mem::
ValidEntity(sticky)
			&& MagnetPawnOk(sticky, lp, localTeam) && MagnetUsesHb(g_magnet.hb)) {
			Vector_t pt{};
			if (MagnetPickPoint(sticky, g_magnet.hb, eye, camDir, pt)) {
				if (!(Config::
trigger_smoke_check
						&& AimCommon::
LineBlockedBySmoke(eye, pt))) {
					const bool wall = AimCommon::
IsBehindWall(
						eye, pt, lp, sticky, Trace::
kMaskShot);
					bool ok = false;
					const bool overrideMinDmg = keybind.isActive(Config::mindamage_override);
					const float curMinDmg = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage;
					const float curMinDmgAw = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage_aw;
					if (!wall) {
						ok = Trace::IsVisible(eye, pt, lp, sticky, Trace::kMaskVis);
						if (ok && curMinDmg > 0.f && pWpn) {
							ok = AutoWall::PassesMinDamage(
								eye, pt, g_magnet.hb, pWpn, lp, sticky, false,
								curMinDmg);
						}
					} else if (trAwOn && pWpn) {
						ok = AutoWall::PassesMinDamage(
							eye, pt, g_magnet.hb, pWpn, lp, sticky, true,
							curMinDmgAw);
					}
					if (ok) {
						QAngle_t fireAng{};
						if (AimCommon::
CalcAngles(eye, pt, fireAng)) {
							fireAng.z = 0.f;
							fireAng.Normalize();
							QAngle_t viewAng = fireAng;
							QAngle_t punch{};
							if (AimCommon::
GetFirePunch(lp, punch) && punch.IsValid())
								AimCommon::
ApplyPunchSubtract(viewAng, punch);
							viewAng.z = 0.f;
							viewAng.Normalize();
							const float fov = AimCommon::
GetFov(camIn, viewAng);
							if (Mem::
Finite(fov) && fov <= stickyFov) {
								g_magnet.point = pt;
								g_magnet.fireAng = fireAng;
								g_magnet.viewAng = viewAng;
								g_magnet.missFrames = 0;
								g_magnet.valid = true;

								const float sm = std::
clamp(Config::
trigger_magnet_smooth, 0.f, 50.f);
								QAngle_t pulled = MagnetSmoothToward(camIn, viewAng, sm);
								if (!Config::
trigger_magnet_silent)
									AimCommon::
SetViewAngles(pulled);
								camOut = pulled;
								g_magnetLastMs = AimCommon::
NowMs();
								return true;
							}
						}
					}
				}
			}
		}
		if (++g_magnet.missFrames >= 6)
			ClearMagnetLock();
	}

	// Fresh scan - one pen budget per pawn, prefer vis, head prio
	struct Cand {
		C_CSPlayerPawn* pawn = nullptr;
		std::
uint32_t handle = 0;
		int hb = Config::
HB_HEAD;
		Vector_t pt{};
		QAngle_t fireAng{};
		QAngle_t viewAng{};
		float fov = 0.f;
		int rank = 0; // 0 vis, 1 pen
		int prio = 9;
	};
	Cand best{};
	bool found = false;

	const int n = AimCommon::
AimTargetCount();
	const AimCommon::
AimTarget* tlist = AimCommon::
AimTargets();
	if (n <= 0 || !tlist) {
		ClearMagnetLock();
		return false;
	}

	// Hitbox order: head first when prio on
	static constexpr int kHbOrder[] = {
		Config::
HB_HEAD, Config::
HB_NECK, Config::
HB_CHEST,
		Config::
HB_STOMACH, Config::
HB_PELVIS,
		Config::
HB_ARMS, Config::
HB_LEGS, Config::
HB_FEET
	};

	for (int ti = 0; ti < n; ++ti) {
		C_CSPlayerPawn* pawn = tlist[ti].pawn;
		if (!pawn || !Mem::
ValidEntity(pawn) || !MagnetPawnOk(pawn, lp, localTeam))
			continue;

		bool pawnPenUsed = false; // max 1 AW pen eval per enemy

		for (int hi = 0; hi < 8; ++hi) {
			const int hb = kHbOrder[hi];
			if (!MagnetUsesHb(hb))
				continue;

			Vector_t pt{};
			if (!MagnetPickPoint(pawn, hb, eye, camDir, pt))
				continue;
			if (Config::
trigger_smoke_check
				&& AimCommon::
LineBlockedBySmoke(eye, pt))
				continue;

			QAngle_t fireAng{};
			if (!AimCommon::
CalcAngles(eye, pt, fireAng))
				continue;
			fireAng.z = 0.f;
			fireAng.Normalize();
			QAngle_t viewAng = fireAng;
			QAngle_t punch{};
			if (AimCommon::
GetFirePunch(lp, punch) && punch.IsValid())
				AimCommon::
ApplyPunchSubtract(viewAng, punch);
			viewAng.z = 0.f;
			viewAng.Normalize();

			const float fov = AimCommon::
GetFov(camIn, viewAng);
			if (!Mem::
Finite(fov) || fov > maxFov)
				continue;

			const bool wall = AimCommon::
IsBehindWall(
				eye, pt, lp, pawn, Trace::
kMaskShot);
			int rank = 0;
			const bool overrideMinDmg = keybind.isActive(Config::mindamage_override);
			const float curMinDmg = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage;
			const float curMinDmgAw = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage_aw;
			if (wall) {
				if (!trAwOn || !pWpn || pawnPenUsed)
					continue;
				pawnPenUsed = true;
				if (!AutoWall::PassesMinDamage(
						eye, pt, hb, pWpn, lp, pawn, true,
						curMinDmgAw))
					continue;
				rank = 1;
			} else {
				if (!Trace::IsVisible(eye, pt, lp, pawn, Trace::kMaskVis))
					continue;
				if (curMinDmg > 0.f && pWpn) {
					if (!AutoWall::PassesMinDamage(
							eye, pt, hb, pWpn, lp, pawn, false,
							curMinDmg))
						continue;
				}
			}

			const int prio = MagnetHbPriority(hb);
			// Score: vis first, then FOV, then hitbox prio (when head_prio)
			bool better = false;
			if (!found) {
				better = true;
			} else if (rank < best.rank) {
				better = true;
			} else if (rank == best.rank) {
				const float fovEps = Config::
trigger_magnet_head_prio ? 0.55f : 0.12f;
				if (fov + 1e-4f < best.fov - fovEps)
					better = true;
				else if (std::
fabs(fov - best.fov) <= fovEps
					&& Config::
trigger_magnet_head_prio
					&& prio < best.prio)
					better = true;
				else if (std::
fabs(fov - best.fov) < 0.08f && prio < best.prio)
					better = true;
			}
			if (!better)
				continue;

			best.pawn = pawn;
			best.handle = tlist[ti].handle;
			best.hb = hb;
			best.pt = pt;
			best.fireAng = fireAng;
			best.viewAng = viewAng;
			best.fov = fov;
			best.rank = rank;
			best.prio = prio;
			found = true;
		}
	}

	if (!found || !best.pawn) {
		if (g_magnet.valid && ++g_magnet.missFrames >= 8)
			ClearMagnetLock();
		return false;
	}

	g_magnet.handle = best.handle;
	g_magnet.hb = best.hb;
	g_magnet.point = best.pt;
	g_magnet.fireAng = best.fireAng;
	g_magnet.viewAng = best.viewAng;
	g_magnet.missFrames = 0;
	g_magnet.valid = true;
	g_magnetLastMs = AimCommon::
NowMs();

	const float sm = std::
clamp(Config::
trigger_magnet_smooth, 0.f, 50.f);
	// Deadzone: already near goal - hold
	if (AimCommon::
GetFov(camIn, best.viewAng) <= dead) {
		camOut = camIn;
		return true;
	}

	QAngle_t pulled = MagnetSmoothToward(camIn, best.viewAng, sm);
	if (!Config::
trigger_magnet_silent)
		AimCommon::
SetViewAngles(pulled);
	camOut = pulled;
	return true;
}

// Scoped sniper: slightly wider capsule so peeks register first contact frame.
// Unscoped / rifles stay tighter (was ghost-firing hairline).
float TriggerCapsuleScale(bool sniperScoped)
{
	return sniperScoped ? 0.97f : 0.92f;
}

// Multipoint floor for RayHitsConfiguredHitbox (1.0 = full capsule edge).
// Scoped sniper 0.82 mid-outer; rifle 0.75 - first real clip, not bone-center wait.
float TriggerSteadyBias(bool sniperScoped)
{
	return sniperScoped ? 0.82f : 0.75f;
}

// Min TriggerScoreHit for accept. 3=center 2=mid 1=edge.
// Edge (1) for all - peeks register first contact frame (seed Exact still verifies pellet).
int TriggerMinScore(bool /*sniperScoped*/)
{
	return 1;
}

// How centered is the ray on allowed hitboxes? Higher = better (bone-center).
// 3 = tight multipoint, 2 = mid, 1 = full capsule edge, 0 = miss.
// Always walk full HB list - seed mode used to pass coreOnly=true which dropped
// legs/feet/arms even when those boxes were enabled in the menu.
int TriggerScoreHit(
	const Vector_t& eye,
	const Vector_t& dir,
	C_CSPlayerPawn* pawn,
	float radiusScale,
	int& outHb,
	Vector_t& outPoint,
	bool /*coreOnlyUnused*/)
{
	outHb = Config::
HB_HEAD;
	outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!pawn)
		return 0;

	static constexpr int kAllHb[] = {
		Config::
HB_HEAD, Config::
HB_NECK, Config::
HB_CHEST,
		Config::
HB_STOMACH, Config::
HB_PELVIS,
		Config::
HB_ARMS, Config::
HB_LEGS, Config::
HB_FEET
	};
	static constexpr float kBiasTier[] = { 0.55f, 0.75f, 1.0f };
	static constexpr int nList = 8;

	for (int tier = 0; tier < 3; ++tier) {
		const float bias = kBiasTier[tier];
		float bestT = 1.0e12f;
		int bestHb = Config::
HB_HEAD;
		Vector_t bestPt{};
		bool found = false;
		for (int i = 0; i < nList; ++i) {
			const int hb = kAllHb[i];
			if (hb < 0 || hb >= Config::
HB_COUNT || !Config::
trigger_hitboxes[hb])
				continue;
			float t = 0.f;
			Vector_t pt{};
			if (!Bones::
RayHitsConfiguredHitbox(
					pawn, hb, eye, dir, radiusScale, t, pt, bias))
				continue;
			if (t >= bestT)
				continue;
			bestT = t;
			bestHb = hb;
			bestPt = pt;
			found = true;
		}
		if (found) {
			outHb = bestHb;
			outPoint = bestPt;
			return 3 - tier;
		}
	}
	return 0;
}

QAngle_t LerpTriggerAng(const QAngle_t& a, const QAngle_t& b, float t)
{
	QAngle_t o{};
	o.x = a.x + (b.x - a.x) * t;
	float dy = b.y - a.y;
	if (dy > 180.f) dy -= 360.f;
	if (dy < -180.f) dy += 360.f;
	o.y = a.y + dy * t;
	o.z = 0.f;
	o.Normalize();
	return o;
}

float TriggerAngDelta(const QAngle_t& a, const QAngle_t& b)
{
	float dy = std::
fabs(a.y - b.y);
	if (dy > 180.f)
		dy = 360.f - dy;
	const float dx = std::
fabs(a.x - b.x);
	return (std::
max)(dx, dy);
}

// Fast path: hit entity is already C_CSPlayerPawn.
C_CSPlayerPawn* ResolveEnemyPawn(
	void* hitEnt,
	C_CSPlayerPawn* lp,
	uint8_t localTeam)
{
	if (!hitEnt || !lp || !Mem::
ValidEntity(hitEnt))
		return nullptr;

	C_CSPlayerPawn* pawn = nullptr;
	__try {
		auto* asBase = reinterpret_cast<C_BaseEntity*>(hitEnt);
		if (asBase->IsBasePlayer()) {
			pawn = reinterpret_cast<C_CSPlayerPawn*>(hitEnt);
		} else {
			void* cur = hitEnt;
			for (int depth = 0; depth < 6 && cur && Mem::
ValidEntity(cur); ++depth) {
				if (cur == lp)
					return nullptr;
				auto* base = reinterpret_cast<C_BaseEntity*>(cur);
				if (base->IsBasePlayer()) {
					pawn = reinterpret_cast<C_CSPlayerPawn*>(cur);
					break;
				}
				CBaseHandle owner{};
				__try {
					uint32_t schOff = SchemaFinder::
Get(
						hash_32_fnv1a_const("C_BaseEntity->m_hOwnerEntity"));
					uint32_t off = schOff ? schOff : 0x520u;
					owner = *reinterpret_cast<CBaseHandle*>(
						reinterpret_cast<std::
uint8_t*>(cur) + off);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					break;
				}
				if (!owner.valid() || !I::
GameEntity || !I::
GameEntity->Instance)
					break;
				void* next = nullptr;
				__try {
					next = I::
GameEntity->Instance->Get(owner);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					break;
				}
				if (!next || next == cur)
					break;
				cur = next;
			}
			if (!pawn)
				return nullptr;
		}

		if (!Mem::
ValidEntity(pawn))
			return nullptr;

		const int hp = Mem::
ClampHealth(pawn->m_iHealth());
		if (hp < 1 || pawn->m_lifeState() != 0)
			return nullptr;

		// DM spawn protect / invuln (invisible) - never trigger
		if (AimCommon::
IsTargetImmune(pawn))
			return nullptr;

		const uint8_t team = pawn->m_iTeamNum();
		if (!Mem::
ValidTeam(static_cast<int>(team)))
			return nullptr;
		if (GameMode::
WantTeamCheck(Config::
team_check) && team == localTeam)
			return nullptr;
		if (pawn == lp)
			return nullptr;

		return pawn;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool ReadCmdViewAngles(CUserCmd* cmd, QAngle_t& out) {
	if (!cmd)
		return false;
	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base || !base->pViewAngles)
		return false;
	out = base->pViewAngles->angValue;
	out.z = 0.f;
	return out.IsValid();
}

// IDA CreateMove (0x180C97330): attack uses attack1_start_history_index into
// CSGOInputHistoryEntryPB. During flicks base angles lag; history holds the tip.
struct TriggerAngleSample {
	QAngle_t ang{};
	int histIndex = -1; // >=0 -> inputHistory slot
};

int CollectTriggerAngles(CUserCmd* cmd, TriggerAngleSample* out, int maxOut, bool pixelPerfect)
{
	if (!out || maxOut < 1)
		return 0;
	int n = 0;

	auto push = [&](const QAngle_t& ang, int histIdx) {
		if (n >= maxOut || !ang.IsValid())
			return;
		QAngle_t a = ang;
		a.z = 0.f;
		a.Normalize();
		// Tighter dedupe so dense flick history isn't collapsed to 1-2 samples
		for (int i = 0; i < n; ++i) {
			if (std::
fabs(out[i].ang.x - a.x) < 0.008f
				&& std::
fabs(out[i].ang.y - a.y) < 0.008f)
				return;
		}
		out[n].ang = a;
		out[n].histIndex = histIdx;
		++n;
	};

	// Always collect full history (newest first). AWP used to take only cam+tip
	// (pixelPerfect) which starved flicks of path samples -> never fired mid-sweep.
	// Early-fire from leading history tip is handled at score/select time, not here.
	(void)
pixelPerfect;

	if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
		const int count = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
		const int take = (std::
min)(count, maxOut - 2);
		const int start = count - take;
		for (int i = count - 1; i >= start && i >= 0; --i) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(i);
			if (!e || !e->pViewAngles)
				continue;
			push(e->pViewAngles->angValue, i);
		}
	}

	// Camera / base last so they stay in the set (reticle truth for AWP)
	QAngle_t cam{};
	if (AimCommon::
GetViewAngles(cam))
		push(cam, -1);

	QAngle_t base{};
	if (ReadCmdViewAngles(cmd, base))
		push(base, -1);

	return n;
}

bool TriggerIsFlicking(const TriggerAngleSample* samples, int n)
{
	float maxSpan = 0.f;
	if (samples && n >= 2) {
		for (int i = 0; i < n; ++i) {
			for (int j = i + 1; j < n; ++j) {
				const float d = TriggerAngDelta(samples[i].ang, samples[j].ang);
				if (d > maxSpan)
					maxSpan = d;
			}
		}
	}
	// ~1.1? within one cmd OR sustained high mouse speed
	return maxSpan >= 1.1f || g_triggerAngSpeed >= 0.85f;
}

// Limb groups: engine TraceLine often hits world/floor before legs/feet capsules.
// Capsule multipoint still fires when crosshair sits on limb geometry.
bool TriggerWantsLimbs()
{
	return (Config::
HB_ARMS < Config::
HB_COUNT && Config::
trigger_hitboxes[Config::HB_ARMS])
		|| (Config::
HB_LEGS < Config::
HB_COUNT && Config::
trigger_hitboxes[Config::HB_LEGS])
		|| (Config::
HB_FEET < Config::
HB_COUNT && Config::
trigger_hitboxes[Config::HB_FEET]);
}

// Looser multipoint bias for thin limbs (feet especially).
float TriggerBiasForHb(int hb, float baseBias)
{
	if (hb == Config::
HB_FEET)
		return (std::
min)(1.f, baseBias + 0.28f);
	if (hb == Config::
HB_LEGS || hb == Config::
HB_ARMS)
		return (std::
min)(1.f, baseBias + 0.15f);
	return baseBias;
}

// Capsule scan all enemies (used when engine ray hits floor/world before limb).
bool TriggerCapsuleScan(
	const Vector_t& lep,
	const Vector_t& fwd,
	C_CSPlayerPawn* lp,
	uint8_t localTeam,
	bool sniperScoped,
	float minBias,
	int minScore,
	C_CSPlayerPawn*& outTarget,
	Vector_t& outPoint,
	int& outHb,
	int& outScore)
{
	outTarget = nullptr;
	outHb = Config::
HB_HEAD;
	outScore = 0;
	if (!lp)
		return false;

	const float radiusScale = TriggerCapsuleScale(sniperScoped);
	const int n = AimCommon::
AimTargetCount();
	const AimCommon::
AimTarget* targets = AimCommon::
AimTargets();
	if (n <= 0 || !targets)
		return false;

	float bestT = 1.0e12f;
	int bestScore = 0;
	C_CSPlayerPawn* bestPawn = nullptr;
	Vector_t bestPt{};
	int bestHb = Config::
HB_HEAD;

	for (int i = 0; i < n; ++i) {
		C_CSPlayerPawn* pawn = targets[i].pawn;
		if (!pawn || pawn == lp || !Mem::
ValidEntity(pawn))
			continue;
		if (AimCommon::
IsTargetImmune(pawn))
			continue;
		// Team already filtered by CollectAimTargets; re-check cheaply.
		__try {
			const uint8_t team = pawn->m_iTeamNum();
			if (GameMode::
WantTeamCheck(Config::
team_check) && team == localTeam)
				continue;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}

		int hb = Config::
HB_HEAD;
		Vector_t pt{};
		const int score = TriggerScoreHit(
			lep, fwd, pawn, radiusScale, hb, pt,
			Config::
trigger_mode == Config::
TR_MODE_SEED_NOSPREAD);
		bool pass = (score >= minScore);
		if (!pass && score > 0) {
			const float needBias = TriggerBiasForHb(hb, minBias);
			if (needBias > minBias) {
				float t = 0.f;
				Vector_t p2{};
				if (Bones::
RayHitsConfiguredHitbox(pawn, hb, lep, fwd, radiusScale, t, p2, needBias)) {
					pass = true;
					pt = p2;
				}
			}
		}
		if (!pass)
			continue;

		// Block if solid wall clearly in front of multipoint (not floor under feet).
		// Floor hits below the limb still allow fire - only reject if something
		// between eye and hit along ray is non-player closer than hit.
		{
			const float hitDist = (pt - lep).Length();
			if (hitDist > 2.f) {
				Trace::
CGameTrace trW{};
				const Vector_t almost{
					lep.x + fwd.x * (hitDist - 1.5f),
					lep.y + fwd.y * (hitDist - 1.5f),
					lep.z + fwd.z * (hitDist - 1.5f)
				};
				if (Trace::
TraceLine(lep, almost, lp, trW, Trace::
kMaskShot)
					&& Trace::
DidHit(trW)) {
					// Absolute-distance reject - old fraction<0.97 test left a
					// ~3%-of-range blind zone (?60u at 2000u) firing into walls.
					const float wallDist = trW.fraction() * (hitDist - 1.5f);
					if (wallDist < hitDist - 4.f) {
						C_CSPlayerPawn* mid = ResolveEnemyPawn(trW.hit_entity(), lp, localTeam);
						if (!mid || mid != pawn)
							continue; // wall / other solid in front
					}
				}
			}
		}

		const float t = (pt - lep).Length();
		if (score < bestScore)
			continue;
		if (score == bestScore && t >= bestT)
			continue;
		bestScore = score;
		bestT = t;
		bestPawn = pawn;
		bestPt = pt;
		bestHb = hb;
	}

	if (!bestPawn || bestScore < minScore)
		return false;
	outTarget = bestPawn;
	outPoint = bestPt;
	outHb = bestHb;
	outScore = bestScore;
	return true;
}

// Engine finds which pawn; capsule multipoint decides WHERE (stops hairline early-fire).
// outScore: 3=center, 2=mid, 1=edge, 0=miss
// Legs/feet: engine ray often hits world first -> capsule fallback when limbs enabled.
bool TriggerRayHitsDir(
	const Vector_t& lep,
	Vector_t fwd,
	C_CSPlayerPawn* lp,
	uint8_t localTeam,
	bool sniperScoped,
	float minBias,
	int minScore,
	C_CSPlayerPawn*& outTarget,
	Vector_t& outPoint,
	int& outHb,
	int& outScore)
{
	outTarget = nullptr;
	outHb = Config::
HB_HEAD;
	outScore = 0;

	const float fl = fwd.Length();
	if (fl < 1e-4f || !Mem::
Finite(fl))
		return false;
	fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;

	constexpr float kTraceDist = 8192.f;
	const Vector_t end{
		lep.x + fwd.x * kTraceDist,
		lep.y + fwd.y * kTraceDist,
		lep.z + fwd.z * kTraceDist
	};

	const float radiusScale = TriggerCapsuleScale(sniperScoped);
	C_CSPlayerPawn* target = nullptr;
	bool usedEnginePawn = false;

	Trace::
CGameTrace tr{};
	Vector_t engineHit{};
	int engineHg = -1;
	if (Trace::
TraceLine(lep, end, lp, tr, Trace::
kMaskShot) && Trace::
DidHit(tr)) {
		target = ResolveEnemyPawn(tr.hit_entity(), lp, localTeam);
		usedEnginePawn = (target != nullptr);
		if (usedEnginePawn) {
			engineHit = tr.endpos();
			engineHg = tr.hitgroup();
		}
	}

	// Engine hull hit a player -> score capsules on that pawn.
	// Strafe: studio capsules lag 1 tick behind the hull CSBaseGunFire uses.
	// If engine already hit an allowed hitgroup, trust that even when capsules miss.
	if (target) {
		int hitHb = Config::
HB_HEAD;
		Vector_t hitPoint{};
		const int score = TriggerScoreHit(
			lep, fwd, target, radiusScale, hitHb, hitPoint,
			Config::
trigger_mode == Config::
TR_MODE_SEED_NOSPREAD);
		
		bool pass = (score >= minScore);
		if (!pass && score > 0) {
			const float needBias = TriggerBiasForHb(hitHb, minBias);
			if (needBias > minBias) {
				float t = 0.f;
				Vector_t pt{};
				if (Bones::
RayHitsConfiguredHitbox(target, hitHb, lep, fwd, radiusScale, t, pt, needBias)) {
					pass = true;
					hitPoint = pt;
				}
			}
		}
		int engineScore = score;
		if (!pass && Bones::IsValidPos(engineHit)) {
			const int mapped = Bones::HitgroupToHitbox(engineHg);
			if (mapped >= 0 && mapped < Config::HB_COUNT && Config::trigger_hitboxes[mapped]) {
				pass = true;
				hitHb = mapped;
				hitPoint = engineHit;
				engineScore = (std::max)(score, minScore);
			}
		}

		if (pass) {

			if (sniperScoped && score >= 2) {
				Trace::
CGameTrace tr2{};
				const Vector_t past{
					hitPoint.x + fwd.x * 2.f,
					hitPoint.y + fwd.y * 2.f,
					hitPoint.z + fwd.z * 2.f
				};
				if (Trace::
TraceLine(lep, past, lp, tr2, Trace::
kMaskShot) && Trace::
DidHit(tr2)
					&& tr2.fraction() < 0.98f) {
					C_CSPlayerPawn* hitPawn = ResolveEnemyPawn(tr2.hit_entity(), lp, localTeam);
					if (!hitPawn)
						goto limb_fallback;
				}
			}

			outTarget = target;
			outPoint = hitPoint;
			outHb = hitHb;
			outScore = engineScore;
			return true;
		}
		// Engine pawn but no allowed hitbox on ray - try limb scan (crosshair on feet,
		// engine hit torso/world sibling) only when limbs are enabled.
	}

limb_fallback:
	// Floor / world / miss: still fire if legs/feet/arms capsules under crosshair.
	if (TriggerWantsLimbs() || !usedEnginePawn) {
		return TriggerCapsuleScan(
			lep, fwd, lp, localTeam, sniperScoped, minBias, minScore,
			outTarget, outPoint, outHb, outScore);
	}
	return false;
}

bool TriggerRayHits(
	const Vector_t& lep,
	const QAngle_t& aimView,
	C_CSPlayerPawn* lp,
	uint8_t localTeam,
	bool sniperScoped,
	float minBias,
	int minScore,
	C_CSPlayerPawn*& outTarget,
	Vector_t& outPoint,
	int& outHb,
	int& outScore)
{
	Vector_t fwd{};
	aimView.ToDirections(&fwd, nullptr, nullptr);
	return TriggerRayHitsDir(
		lep, fwd, lp, localTeam, sniperScoped, minBias, minScore,
		outTarget, outPoint, outHb, outScore);
}

// Expand discrete history samples with lerped angles along the flick path so
// a sub-frame head cross is still detected (main cause of "miss on fast flick").
int ExpandTriggerAngles(
	const TriggerAngleSample* in, int nIn,
	TriggerAngleSample* out, int maxOut,
	bool flicking)
{
	if (!in || !out || maxOut < 1 || nIn <= 0)
		return 0;

	int n = 0;
	auto push = [&](const QAngle_t& ang, int histIdx) {
		if (n >= maxOut || !ang.IsValid())
			return;
		QAngle_t a = ang;
		a.z = 0.f;
		a.Normalize();
		for (int i = 0; i < n; ++i) {
			if (std::
fabs(out[i].ang.x - a.x) < 0.004f
				&& std::
fabs(out[i].ang.y - a.y) < 0.004f)
				return;
		}
		out[n].ang = a;
		out[n].histIndex = histIdx;
		++n;
	};

	// denser subdivision while flicking only (steady = raw history, no lerp spam)
	const int steps = flicking ? 6 : 0;

	for (int i = 0; i < nIn; ++i) {
		push(in[i].ang, in[i].histIndex);
		if (i + 1 >= nIn)
			continue;
		const float span = TriggerAngDelta(in[i].ang, in[i + 1].ang);
		if (span < 0.10f)
			continue;
		// More steps when span is large
		const int sub = (std::
min)(steps + static_cast<int>(span * 3.f), 16);
		for (int s = 1; s < sub; ++s) {
			const float t = static_cast<float>(s) / static_cast<float>(sub);
			// Bind lerp to nearest real history - fire stamps angles into that slot
			const int hist = (t < 0.5f) ? in[i].histIndex : in[i + 1].histIndex;
			push(LerpTriggerAng(in[i].ang, in[i + 1].ang, t), hist);
		}
	}
	return n;
}

// IDA CSBaseGunFire (0x1807C7040): SPREADSEEDGEN(player, &ang, tick)
// tick = history entry +0x68 (nPlayerTickCount). NEVER nRenderTickCount (+0x60).

bool HistoryShootPos(CCSGOInputHistoryEntryPB* e, Vector_t& out)
{
	if (!e || !e->pShootPosition)
		return false;
	const Vector4D_t& v = e->pShootPosition->vecValue;
	if (!std::
isfinite(v.x) || !std::
isfinite(v.y) || !std::
isfinite(v.z))
		return false;
	if (v.x == 0.f && v.y == 0.f && v.z == 0.f)
		return false;
	out = { v.x, v.y, v.z };
	return Bones::
IsValidPos(out);
}

// Triggerbot: fire when any this-tick angle sample (history tip / camera / base)
// clips a hitbox. Stamps winning angle + attack1 history index for flick sync.
bool RunTriggerbotImpl(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	g_triggerWantShoot = false;
	g_triggerWantStop = false;
	g_triggerFireValid = false;
	g_triggerFireHist = -1;

	if (!keybind.isActive(Config::
triggerbot)) {
		ResetInternal();
		return false;
	}
	// Hard early-outs must ResetInternal - else g_triggerWasOn keeps counting
	// delay through flash/scope/knife and fires the instant the gate lifts.
	if (!I::
GameEntity || !Mem::
Valid(I::
GameEntity->Instance, 0x2100)) {
		ResetInternal();
		return false;
	}
	if (!Mem::
ValidEntity(lp)) {
		ResetInternal();
		return false;
	}
	// Freeze time / buy time - no combat (default on)
	if (AimCommon::
IsFreezePeriod()) {
		ResetInternal();
		return false;
	}
	// NOTE: local spawn protection does NOT block the trigger - only enemies in
	// spawn protection are skipped (IsTargetImmune on the target scan). The old
	// local-immune gate here disabled the trigger while WE were spawn-protected
	// (TDM), which is exactly when a triggerbot is wanted.
	if (Config::
trigger_flash_check && AimCommon::
IsBlinded(lp)) {
		ResetInternal();
		return false;
	}
	if (!Trace::
Ready()) {
		ResetTargetPresence();
		// Trace not ready: don't burn delay, but don't reset mouse-speed EMA hard
		g_triggerWantShoot = false;
		g_triggerWantStop = false;
		g_triggerFireValid = false;
		return false;
	}

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::
ValidEntity(pWpn) || pWpn->IsNonGunWeapon()) {
		ResetInternal();
		return false;
	}
	Config::
ApplyWeaponGroup(pWpn);

	const bool sniper = AimCommon::
IsSniperWeapon(pWpn);
	const bool scopeWpn = AimCommon::
IsScopeWeapon(pWpn);
	const bool scoped = AimCommon::
IsLocalScoped(lp, pWpn);
	if (scopeWpn && !scoped && Config::trigger_scoped_only) {
		ResetInternal();
		return false;
	}

	// Default eye = engine ShootPosition; per-sample history pShootPosition overrides below
	const Vector_t lepDefault = Bones::
GetShootPos(lp);
	if (!Bones::
IsValidPos(lepDefault)) {
		ResetInternal();
		return false;
	}

	const uint8_t localTeam = lp->getTeam();
	const bool sniperScoped = sniper && scoped;

	// Mouse speed EMA (deg/frame) - catches flicks even when history is thin
	{
		QAngle_t cam{};
		if (AimCommon::
GetViewAngles(cam)) {
			cam.z = 0.f;
			if (g_triggerPrevCamOk) {
				const float d = TriggerAngDelta(g_triggerPrevCam, cam);
				g_triggerAngSpeed = g_triggerAngSpeed * 0.55f + d * 0.45f;
			}
			g_triggerPrevCam = cam;
			g_triggerPrevCamOk = true;
		}
	}

	// Full history always (not AWP-only tip) so flick path has enough samples
	TriggerAngleSample raw[24]{};
	const int nRaw = CollectTriggerAngles(cmd, raw, 24, false);
	if (nRaw <= 0) {
		ResetTargetPresence();
		return false;
	}

	const bool flicking = TriggerIsFlicking(raw, nRaw)
		|| g_triggerAngSpeed >= 0.65f; // AWP flicks: history span can stay small

	// Path-subdivide only while flicking (was always-on for snipers -> FPS + traces)
	TriggerAngleSample samples[48]{};
	const int nSamples = ExpandTriggerAngles(raw, nRaw, samples, 48, flicking);
	if (nSamples <= 0) {
		ResetTargetPresence();
		return false;
	}

	// Mode: Seed needs NoSpread ready. If seed selected but SPREADSEEDGEN miss,
	// fall through to hitchance so switching modes never deadlocks.
	// Pred flags match CreateMove sim (raw m_fFlags can lag mid-air / land).
	const bool onGround = (Pred::
Flags(lp) & FL_ONGROUND) != 0;
	int triggerMode = Config::
trigger_mode;
	if (triggerMode < 0 || triggerMode >= Config::
TR_MODE_COUNT)
		triggerMode = Config::
TR_MODE_HITCHANCE;
	const bool seedWanted = triggerMode == Config::
TR_MODE_SEED_NOSPREAD;
	const bool hitchanceWanted = triggerMode == Config::
TR_MODE_HITCHANCE;
	if (seedWanted && !NoSpread::
Ready())
		NoSpread::
Init();
	if (hitchanceWanted && !HitChance::
Ready())
		HitChance::
Init();
	const bool seedReady = NoSpread::
Ready();
	const bool seedMode = seedWanted && seedReady;
	const bool hitchanceMode = hitchanceWanted || (seedWanted && !seedReady);
	// Seed always when ready (no wildFlick skip - that late/no-shot rifles).
	const bool useSeedNow = seedMode;
	// MODE log: once per mode change only (was 1Hz every hold -> I/O + FPS).
#ifdef _DEBUG
	{
		static int s_lastCfg = -1;
		static int s_lastUse = -1;
		const int useI = useSeedNow ? 1 : 0;
		if (triggerMode != s_lastCfg || useI != s_lastUse) {
			s_lastCfg = triggerMode;
			s_lastUse = useI;
			Con::
Info(
				"trigger MODE cfg=%d seedReady=%d useSeed=%d hc=%d scoped=%d",
				triggerMode, seedReady ? 1 : 0, useI,
				hitchanceMode ? 1 : 0, scoped ? 1 : 0);
		}
	}
#endif
	QAngle_t camAng{};
	const bool haveCam = AimCommon::
GetViewAngles(camAng);
	if (haveCam) camAng.z = 0.f;

	C_CSPlayerPawn* target = nullptr;
	Vector_t hitPoint{};
	int hitHb = Config::
HB_HEAD;
	QAngle_t aimView{};
	Vector_t winEye = lepDefault;
	int winHist = -1;
	bool onTarget = false;
	int bestScore = 0;
	int bestIdx = -1;
	// True when target/hitPoint were validated by the live-cam ray above -
	// lets the final recheck skip an identical duplicate trace pass.
	bool fromCamRay = false;

	// Magnet: sticky soft-aim (no humanize) BEFORE fire ray.
	// Vis / AW / smoke / team / punch-aware. Optional silent + lead + head prio.
	const bool trAwOn = Config::
trigger_autowall && keybind.isActive(Config::
autowall);
	bool magnetLocked = false;
	if (Config::
trigger_magnet && haveCam) {
		QAngle_t camAfter = camAng;
		magnetLocked = RunMagnet(
			lp, pWpn, lepDefault, camAng, localTeam, trAwOn, camAfter);
		if (camAfter.IsValid())
			camAng = camAfter;
		// Seed magnet point for fire assist (still need ray or magnet-lock path)
		if (magnetLocked && g_magnet.valid) {
			const int n = AimCommon::
AimTargetCount();
			const AimCommon::
AimTarget* tlist = AimCommon::
AimTargets();
			for (int i = 0; i < n; ++i) {
				if (tlist[i].handle == g_magnet.handle && tlist[i].pawn) {
					target = tlist[i].pawn;
					hitPoint = g_magnet.point;
					hitHb = g_magnet.hb;
					break;
				}
			}
		}
	} else {
		ClearMagnetLock();
	}

	// Fire truth = LIVE camera only. History tips lead the reticle mid-sweep and
	// caused ghost shots before the crosshair actually sat on a hitbox.
	// Edge score (1) + bias - first capsule contact; seed Exact verifies pellet.
	const int minScore = TriggerMinScore(sniperScoped);
	// Magnet locked: looser multipoint so pull + fire same tick (was never-fire high smooth)
	const float steadyBias = magnetLocked
		? (std::
min)(1.f, TriggerSteadyBias(sniperScoped) + 0.12f)
		: TriggerSteadyBias(sniperScoped);
	if (haveCam) {
		Vector_t eye = lepDefault;
		C_CSPlayerPawn* t = nullptr;
		Vector_t pt{};
		int hb = Config::
HB_HEAD;
		int score = 0;
		if (TriggerRayHits(
				eye, camAng, lp, localTeam,
				sniperScoped, steadyBias, minScore,
				t, pt, hb, score)) {
			target = t;
			hitPoint = pt;
			hitHb = hb;
			aimView = camAng;
			winEye = eye;
			winHist = -1;
			onTarget = true;
			bestScore = score;
			fromCamRay = true;
		}
	}

	// Magnet-assist fire: crosshair not on capsule yet but lock is valid + near goal.
	// Seed: never fire off-reticle. Magnet may still pull the camera; fire waits for TriggerRayHits.
	if (!onTarget && !useSeedNow && magnetLocked && g_magnet.valid && target && Bones::
IsValidPos(hitPoint)) {
		const float lockFov = AimCommon::
GetFov(camAng, g_magnet.viewAng);
		const float fireWindow = std::
clamp(
			Config::
trigger_magnet_deadzone * 2.5f + 0.35f, 0.25f, 1.8f);
		if (Mem::
Finite(lockFov) && lockFov <= fireWindow
			&& MagnetPawnOk(target, lp, localTeam)) {
			// Re-check smoke / wall on magnet point
			bool allow = true;
			if (Config::
trigger_smoke_check
				&& AimCommon::
LineBlockedBySmoke(lepDefault, hitPoint))
				allow = false;
			if (allow) {
				const bool wall = AimCommon::
IsBehindWall(
					lepDefault, hitPoint, lp, target, Trace::
kMaskShot);
				const bool overrideMinDmg = keybind.isActive(Config::mindamage_override);
				const float curMinDmg = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage;
				const float curMinDmgAw = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage_aw;
				if (wall && !trAwOn)
					allow = false;
				else if (wall && trAwOn) {
					allow = AutoWall::PassesMinDamage(
						lepDefault, hitPoint, hitHb, pWpn, lp, target, true,
						curMinDmgAw);
				} else if (!wall) {
					allow = Trace::IsVisible(
						lepDefault, hitPoint, lp, target, Trace::kMaskVis);
					if (allow && curMinDmg > 0.f && pWpn) {
						allow = AutoWall::PassesMinDamage(
							lepDefault, hitPoint, hitHb, pWpn, lp, target, false,
							curMinDmg);
					}
				}
			}
			if (allow) {
				// Prefer fire angle (bullet) for silent magnet; cam for non-silent
				aimView = (Config::
trigger_magnet_silent && g_magnet.fireAng.IsValid())
					? g_magnet.fireAng : camAng;
				winEye = lepDefault;
				winHist = -1;
				onTarget = true;
				bestScore = 2;
			}
		}
	}

	// History bind for attack-hist stamp (seed tick + shoot pos).
	// Never set onTarget from history alone (ghost fire).
	// Seed parked: skip full TriggerRayHits per hist - angle match + player tick enough.
	// Heavy ray loop was delaying first seed Solve while reticle already on bone.
	if (onTarget && cmd && (!flicking || sniperScoped || useSeedNow)) {
		const float maxHistDelta = (useSeedNow || sniperScoped) ? 0.55f : 0.12f;
		for (int i = 0; i < nSamples; ++i) {
			if (samples[i].histIndex < 0)
				continue;
			if (haveCam && TriggerAngDelta(samples[i].ang, camAng) > maxHistDelta)
				continue;
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(samples[i].histIndex);
			if (!e)
				continue;

			if (useSeedNow) {
				// Prefer entry with nPlayerTickCount (SPREADSEEDGEN input)
				if (e->nPlayerTickCount <= 0)
					continue;
				Vector_t eye = lepDefault;
				Vector_t sp{};
				if (HistoryShootPos(e, sp) && Bones::
IsValidPos(sp))
					eye = sp;
				if (samples[i].histIndex > winHist) {
					winHist = samples[i].histIndex;
					winEye = eye;
					bestIdx = i;
				}
				break; // newest-first
			}

			// Non-seed: still verify hist ray hits same pawn
			Vector_t eye = lepDefault;
			Vector_t sp{};
			if (HistoryShootPos(e, sp) && Bones::
IsValidPos(sp))
				eye = sp;
			C_CSPlayerPawn* t = nullptr;
			Vector_t pt{};
			int hb = Config::
HB_HEAD;
			int score = 0;
			if (!TriggerRayHits(
					eye, samples[i].ang, lp, localTeam,
					sniperScoped, steadyBias, minScore,
					t, pt, hb, score)
				|| t != target)
				continue;
			if (samples[i].histIndex > winHist) {
				winHist = samples[i].histIndex;
				winEye = eye;
				bestIdx = i;
			}
			break;
		}
		// Seed: if no hist had player tick, fall back to newest angle-matched slot
		if (useSeedNow && winHist < 0) {
			for (int i = 0; i < nSamples; ++i) {
				if (samples[i].histIndex < 0)
					continue;
				if (haveCam && TriggerAngDelta(samples[i].ang, camAng) > maxHistDelta)
					continue;
				winHist = samples[i].histIndex;
				bestIdx = i;
				break;
			}
		}
	}

	// Autowall fallback only if no clear ray hit - use LIVE camera (never history tip)
	// trAwOn set above (magnet + fire share same AW gate)
	if (!onTarget && trAwOn && haveCam) {
		aimView = camAng;
		winHist = -1;

		Vector_t eye = lepDefault;
		winEye = eye;

		Vector_t fwd{};
		aimView.ToDirections(&fwd, nullptr, nullptr);
		const float fl = fwd.Length();
		if (fl < 1e-4f || !Mem::
Finite(fl))
			return false;
		fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;
		const float radiusScale = TriggerCapsuleScale(sniperScoped);

		float bestT = 1.0e12f;
		C_CSPlayerPawn* best = nullptr;
		Vector_t bestPt{};
		int bestHb = Config::
HB_HEAD;

		// Core HBs only for AW scan - limbs explode pen cost for little gain
		static constexpr int kAwHb[] = {
			Config::
HB_HEAD, Config::
HB_NECK, Config::
HB_CHEST,
			Config::
HB_STOMACH, Config::
HB_PELVIS
		};
		// Cap game pens this fallback (AutoWall also budgets internally)
		int awPensLeft = 8;

		for (int ti = 0; ti < AimCommon::
AimTargetCount() && awPensLeft > 0; ++ti) {
			auto* pawn = AimCommon::
AimTargets()[ti].pawn;
			if (!Mem::
ValidEntity(pawn) || AimCommon::
IsTargetImmune(pawn))
				continue;

			for (int hb : kAwHb) {
				if (awPensLeft <= 0)
					break;
				if (hb < 0 || hb >= Config::
HB_COUNT || !Config::
trigger_hitboxes[hb])
					continue;
				float t = 0.f;
				Vector_t pt{};
				// AW uses steady multipoint (never full edge)
				if (!Bones::
RayHitsConfiguredHitbox(
						pawn, hb, eye, fwd, radiusScale, t, pt,
						TriggerSteadyBias(sniperScoped)))
					continue;
				if (t >= bestT)
					continue; // worse than known hit - skip pen
				// 0 = any pen hit (do not fall back to visible mindmg)
				--awPensLeft;
				if (!AutoWall::
PassesMinDamage(
						eye, pt, hb, pWpn, lp, pawn, true,
						Config::
trigger_mindamage_aw))
					continue;
				bestT = t;
				best = pawn;
				bestPt = pt;
				bestHb = hb;
			}
		}
		if (best) {
			target = best;
			hitPoint = bestPt;
			hitHb = bestHb;
			onTarget = true;
		}
	}

	static std::
uint64_t s_triggerSeenMs = 0;
	static C_CSPlayerPawn* s_triggerLastTarget = nullptr;
	static std::uint32_t s_triggerLastGen = 0;
	// Entity slots recycle across map change / round resets - a new pawn at
	// the recycled address must not read as "same target" (delay clock would
	// never restart for the new enemy). Invalidate on entity-generation bump.
	const std::uint32_t entGen = SdkPrioA::EntityGen();
	if (s_triggerLastTarget && entGen != s_triggerLastGen) {
		s_triggerLastTarget = nullptr;
		s_triggerLastGen = entGen;
	}
	const std::
uint64_t now = AimCommon::
NowMs();
	if (!onTarget || !target || !Mem::
ValidEntity(target)) {
		// Strafe flicker: studio capsules miss 1-2 ticks while hull still hits.
		// Tolerance is TIME-based - the old 3-frame window collapsed to ~8 ms at
		// high fps and reset the contact clock constantly (dead trigger while
		// standing on a strafing target, alive only while walking = flick bypass).
		if (g_triggerWasOn && now - s_triggerSeenMs <= kMissToleranceMs)
			return true;
		g_triggerWasOn = false;
		g_triggerOnSinceMs = 0;
		g_triggerOnMissFrames = 0;
		g_triggerDelayArmed = false; // real re-approach -> delay restarts
		s_triggerSeenMs = 0;
		return false;
	}
	g_triggerOnMissFrames = 0;
	s_triggerSeenMs = now;
	// Target switch -> fresh presence: delay restarts for the new enemy.
	if (target != s_triggerLastTarget) {
		s_triggerLastTarget = target;
		g_triggerOnSinceMs = now;
		g_triggerDelayArmed = false;
	}

	if (Config::
trigger_smoke_check
		&& AimCommon::
LineBlockedBySmoke(lepDefault, hitPoint)) {
		g_triggerWasOn = false;
		g_triggerOnSinceMs = 0;
		g_triggerDelayArmed = false;
		return false;
	}

	if (!g_triggerWasOn) {
		g_triggerWasOn = true;
		g_triggerOnSinceMs = now;
	}

	// Delay holds the FIRST on-target window only - once satisfied it stays
	// armed. The old per-frame re-evaluation made any delay>0 + brief capsule
	// misses restart the countdown forever while parked crosshair.
	const float delay = std::
clamp(Config::
trigger_delay_ms, 0.f, 500.f);
	if (!g_triggerDelayArmed && delay > 0.01f
		&& (now - g_triggerOnSinceMs) >= static_cast<std::
uint64_t>(delay + 0.5f))
		g_triggerDelayArmed = true;
	// Seed nospread always respects the initial delay (no instant-fire
	// mid-flick); flick bypass only in hitchance mode, as before.
	const bool skipDelay = !useSeedNow && (flicking || sniperScoped);
	if (!skipDelay && !g_triggerDelayArmed && delay > 0.01f
		&& (now - g_triggerOnSinceMs) < static_cast<std::
uint64_t>(delay + 0.5f))
		return true;

	if (!AimCommon::
CanWeaponFire(pWpn, lp))
		return true; // on target but can't shoot (reload / fire-rate) - no autostop

	const Vector_t vel = Pred::
Velocity(lp);
	const float speed2d = std::
sqrt(vel.x * vel.x + vel.y * vel.y);

	if (scopeWpn && !scoped && Config::trigger_scoped_only)
		return true;

	// Eye for damage/HC: prefer history shoot pos that won the ray
	Vector_t dmgEye = lepDefault;
		if (winHist >= 0 && cmd) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(winHist);
			Vector_t sp{};
			if (HistoryShootPos(e, sp) && Bones::
IsValidPos(sp))
				dmgEye = sp;
		}

	bool dmgOk = false;
	bool allowPen = false;
	// Wall / mindmg - trigger-only Config::trigger_mindamage(_aw), not AF
	// Seed path: skip pre-Solve mindmg (feet/legs often fail PassesMinDamage on rim
	// -> dmgOk=false late reject while crosshair on target). Soft check after Solve.
	// Wall check: use live eye first. Online hist eye lag -> false behindWall
	// while crosshair already on enemy (log 01:24 REJECT then FIRE later).
	const bool behindLive = AimCommon::
IsBehindWall(
		lepDefault, hitPoint, lp, target, Trace::
kMaskShot);
	const bool behindHist = (Bones::
IsValidPos(dmgEye)
		&& (std::
fabs(dmgEye.x - lepDefault.x) > 1.f
			|| std::
fabs(dmgEye.y - lepDefault.y) > 1.f
			|| std::
fabs(dmgEye.z - lepDefault.z) > 1.f))
		? AimCommon::
IsBehindWall(dmgEye, hitPoint, lp, target, Trace::
kMaskShot)
		: behindLive;
	// Seed: only block if LIVE eye is walled (hist lag ignored)
	const bool behindWall = useSeedNow ? behindLive : behindHist;
	if (behindWall && !trAwOn) {
#ifdef _DEBUG
		static std::
uint64_t s_lastWallLog = 0;
		if (AimCommon::
NowMs() - s_lastWallLog >= 200ull) {
			s_lastWallLog = AimCommon::
NowMs();
			Con::
Info("trigger REJECT: behindWall (eye=(%.0f,%.0f,%.0f) pt=(%.0f,%.0f,%.0f) flick=%d)",
				dmgEye.x, dmgEye.y, dmgEye.z, hitPoint.x, hitPoint.y, hitPoint.z, flicking);
		}
#endif
		return true; // on geometry ray but can't pen - no shoot / no stop
	}

	allowPen = behindWall && trAwOn;

	if (useSeedNow) {
		// Seed: wall already gated; mindmg after Solve on pellet point
		dmgOk = true;
	} else {
		const bool overrideMinDmg = keybind.isActive(Config::mindamage_override);
		const float minDmg = overrideMinDmg
			? Config::mindamage_override_value
			: (allowPen ? Config::trigger_mindamage_aw : Config::trigger_mindamage);

		if (!allowPen) {
			dmgOk = AutoWall::
PassesMinDamage(
				dmgEye, hitPoint, hitHb, pWpn, lp, target, false, minDmg);
		} else {
			// Wallbang: must pen + hit target + dmg >= trigger_mindamage_aw
			const AutoWall::
Result aw = AutoWall::
Fire(
				dmgEye, hitPoint, hitHb, pWpn, lp, target, true);
			if (aw.hit && aw.penetrated && aw.damage >= 1.f) {
				float need = minDmg;
				if (need > 0.f) {
					const int hp = target->m_iHealth();
					if (hp > 0 && static_cast<float>(hp) < need)
						need = static_cast<float>(hp);
					dmgOk = aw.damage + 0.01f >= need;
				} else {
					dmgOk = true;
				}
			} else {
				dmgOk = false;
			}
		}
		if (!dmgOk) {
#ifdef _DEBUG
			static std::
uint64_t s_lastDmgLog = 0;
			if (AimCommon::
NowMs() - s_lastDmgLog >= 200ull) {
				s_lastDmgLog = AimCommon::
NowMs();
				Con::
Info("trigger REJECT: dmgOk=false");
			}
#endif
			return true;
		}
	}

	bool hcOk = true;
	float hcReq = Config::
trigger_hitchance;

	if (useSeedNow) {
		// Seed path below - no Monte Carlo
	} else if (hitchanceMode) {
		if (sniperScoped && Config::
trigger_hitchance <= 0.01f)
			hcReq = 0.f;
		if (hcReq > 0.01f && !allowPen)
			hcOk = HitChance::
PassesSafe(
				dmgEye, aimView, hitPoint, hitHb, pWpn, hcReq, lp, target);

		if (Config::
trigger_autostop && onGround
			&& std::
isfinite(speed2d) && speed2d > AimCommon::
kAfStopSpeed)
			g_triggerWantStop = true;
	}

	if (!flicking && Config::
trigger_autostop && onGround
		&& std::
isfinite(speed2d) && speed2d > AimCommon::
kAfStopSpeed)
		g_triggerWantStop = true;

	if (!hcOk) {
		g_triggerWantStop = false; // no shot this tick - never autostop without fire
		return true;
	}

	// Final cam recheck - HC path needs live reticle still on pawn.
	// Magnet lock: allow fire when still near magnet goal (assist window).
	// Seed path: skip - Solve verifies pellet this tick.
	if (!haveCam)
		return true;
	// Live-cam ray above already validated target/hitPoint this tick with the
	// exact same inputs (lepDefault / camAng / steadyBias / minScore) - re-running
	// TriggerRayHits is a guaranteed-identical duplicate trace pass on every
	// fired tick. Skip it; magnet-assist / AW-fallback hits still recheck below.
	if (!useSeedNow && !fromCamRay) {
		C_CSPlayerPawn* camT = nullptr;
		Vector_t camPt{};
		int camHb = Config::
HB_HEAD;
		int camScore = 0;
		Vector_t camEye = lepDefault;
		const int camMin = TriggerMinScore(sniperScoped);
		const float camBias = magnetLocked
			? (std::
min)(1.f, TriggerSteadyBias(sniperScoped) + 0.12f)
			: TriggerSteadyBias(sniperScoped);
		const bool camOk = TriggerRayHits(
			camEye, camAng, lp, localTeam,
			sniperScoped, camBias, camMin,
			camT, camPt, camHb, camScore)
			&& camT == target;
		if (!camOk) {
			// Magnet assist: still locked on same pawn + within fire window
			const bool magOk = magnetLocked && g_magnet.valid
				&& Bones::
IsValidPos(g_magnet.point)
				&& AimCommon::
GetFov(camAng, g_magnet.viewAng)
					<= std::
clamp(Config::
trigger_magnet_deadzone * 2.5f + 0.45f, 0.3f, 2.f);
			if (magOk) {
				hitPoint = g_magnet.point;
				hitHb = g_magnet.hb;
				aimView = (Config::
trigger_magnet_silent && g_magnet.fireAng.IsValid())
					? g_magnet.fireAng : camAng;
				winEye = camEye;
				bestScore = (std::
max)(bestScore, 2);
			} else if (allowPen) {
				int hb2 = Config::
HB_HEAD;
				Vector_t pt2{};
				Vector_t fwd{};
				camAng.ToDirections(&fwd, nullptr, nullptr);
				const float fl = fwd.Length();
				if (fl < 1e-4f || !Mem::
Finite(fl))
					return true;
				fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;
				const int sc = TriggerScoreHit(
					camEye, fwd, target, TriggerCapsuleScale(sniperScoped),
					hb2, pt2, false);
				if (sc < camMin)
					return true;
				hitPoint = pt2;
				hitHb = hb2;
				aimView = camAng;
				winEye = camEye;
				bestScore = sc;
			} else {
				return true;
			}
		} else {
			hitPoint = camPt;
			hitHb = camHb;
			aimView = camAng;
			winEye = camEye;
			if (winHist < 0)
				winEye = camEye;
			bestScore = camScore;
		}
	} else if (haveCam && camAng.IsValid()) {
		aimView = camAng;
		if (winHist < 0)
			winEye = lepDefault;
	}

	bool seedFired = false;

	if (useSeedNow) {
		// Soft ms gap only while engine also not ready (see SeedCycleAllowsFire).
		if (!NoSpread::
SeedCycleAllowsFire(pWpn, lp))
			return true;

		int atkIdx = winHist;
		// Parked on enemy: live eye matches FireBullet origin better than laggy hist.
		// Hist shoot-pos lag -> wrong wantDir / wall false / late Solve.
		Vector_t seedEye = lepDefault;
		int seedTick = 0;
		float seedFrac = 0.f;
		// Wish = cam / crosshair (what player aimed). Solve rewrites for pellet.
		QAngle_t wishAng = aimView;
		wishAng.z = 0.f;
		wishAng.Normalize();

		if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
			const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			// Prefer hist with real nPlayerTickCount near cam (not just last slot)
			if (atkIdx < 0 || atkIdx >= histCount) {
				atkIdx = -1;
				for (int hi = histCount - 1; hi >= 0; --hi) {
					CCSGOInputHistoryEntryPB* he = cmd->GetInputHistoryEntry(hi);
					if (!he || !Mem::ValidEntity(he) || he->nPlayerTickCount <= 0)
						continue;
					if (he->pViewAngles && haveCam) {
						QAngle_t ha = he->pViewAngles->angValue;
						ha.z = 0.f;
						if (TriggerAngDelta(ha, camAng) > 0.75f)
							continue;
					}
					atkIdx = hi;
					break;
				}
				if (atkIdx < 0)
					atkIdx = histCount > 0 ? histCount - 1 : -1;
			}
			if (atkIdx >= 0 && atkIdx < histCount) {
				CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(atkIdx);
				if (e && Mem::ValidEntity(e)) {
					seedTick = AimCommon::
GetSeedTickFromEntry(e);
					seedFrac = e->flPlayerTickFraction;
					if (!std::
isfinite(seedFrac))
						seedFrac = 0.f;
					Vector_t sp{};
					if (HistoryShootPos(e, sp) && Bones::
IsValidPos(sp)) {
						const float dx = sp.x - lepDefault.x;
						const float dy = sp.y - lepDefault.y;
						const float dz = sp.z - lepDefault.z;
						if (dx * dx + dy * dy + dz * dz < 9.f)
							seedEye = sp;
					}
				}
			}
		}
		if (seedTick <= 0)
			seedTick = AimCommon::
GetRenderTick(cmd, atkIdx, lp);
		Bones::ClampSeedTickToShootRing(lp, seedTick, seedFrac);
		if (seedTick <= 0) {
#ifdef _DEBUG
			static std::
uint64_t s_lastTick0 = 0;
			if (AimCommon::
NowMs() - s_lastTick0 >= 250ull) {
				s_lastTick0 = AimCommon::
NowMs();
				Con::
Info("trigger seed FAIL: seedTick=0 hist=%d", atkIdx);
			}
#endif
			return true;
		}

		if (!Bones::
IsValidPos(seedEye))
			seedEye = lepDefault;

		// Punch from fill - never steal hist nPlayerTickCount (SPREADSEEDGEN hash).
		// Keep live eye unless fill eye is near (fill lag online = late/miss).
		{
			const int histTick = seedTick;
			HitChance::
SeedFireContext sfc{};
			if (HitChance::
BuildSeedFireContext(
					lp, pWpn, wishAng, seedTick, seedFrac, seedEye, sfc) && sfc.ok) {
				if (histTick <= 0 && sfc.tick > 0)
					seedTick = sfc.tick;
				if (seedFrac <= 0.f && std::
isfinite(sfc.frac))
					seedFrac = sfc.frac;
				if (Bones::
IsValidPos(sfc.eye)) {
					const float dx = sfc.eye.x - lepDefault.x;
					const float dy = sfc.eye.y - lepDefault.y;
					const float dz = sfc.eye.z - lepDefault.z;
					if (dx * dx + dy * dy + dz * dz < 9.f)
						seedEye = sfc.eye;
				}
			}
		}

		wishAng = (haveCam && camAng.IsValid()) ? camAng : aimView;
		wishAng.z = 0.f;
		wishAng.x = std::clamp(wishAng.x, -89.f, 89.f);
		wishAng.Normalize();

		// Push wish off SPREADSEEDGEN half-deg bin edge BEFORE the fast-path
		// exact test. Server-side ULP drift on quant(pitch,yaw) flips the seed
		// hash -> different pellet -> miss. Applies to both fast + Solve paths
		// because Solve consumes wishAng too.
		{
			QAngle_t punchNudge{};
			const QAngle_t* pn = nullptr;
			if (HitChance::
ReadSeedFirePunch(lp, pWpn, seedTick, seedFrac, punchNudge)
				&& punchNudge.IsValid())
				pn = &punchNudge;
			NoSpread::
NudgeBinSafe(wishAng, pn);
		}

		// Fast path: natural pellet already hits enabled HB (parked crosshair).
		// Skip multi-HB Solve spiral -> same-frame fire online.
		NoSpread::Shot shot{};
		bool solved = false;
		// Solve tries natural wish first (game pellet). ExactShotHitsAny graze
		// during spray fired uncompensated - same overspray miss as autofire.
		__try {
			const int preferHb = (hitHb >= 0 && hitHb < Config::HB_COUNT)
				? hitHb : Config::HB_HEAD;
			bool onlyHb[Config::HB_COUNT]{};
			if (preferHb >= 0 && preferHb < Config::HB_COUNT
				&& Config::trigger_hitboxes[preferHb])
				onlyHb[preferHb] = true;
			else {
				for (int i = 0; i < Config::HB_COUNT; ++i)
					onlyHb[i] = Config::trigger_hitboxes[i];
			}
			if (NoSpread::Solve(
					seedEye, wishAng, seedTick, seedFrac, pWpn, lp, target,
					preferHb, onlyHb, shot)
				&& shot.ok && shot.fireAngles.IsValid()) {
				solved = true;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			solved = false;
		}
		// Reject wild rewrite vs cam. Budget matches Solve (cone-sized).
		if (solved && shot.ok && haveCam && camAng.IsValid()) {
			QAngle_t sa = shot.fireAngles;
			sa.z = 0.f;
			float dy = sa.y - camAng.y;
			while (dy > 180.f) dy -= 360.f;
			while (dy < -180.f) dy += 360.f;
			const float d = std::sqrt(
				(sa.x - camAng.x) * (sa.x - camAng.x) + dy * dy);
			const float maxCam = NoSpread::HistRewriteLimitDeg(pWpn, lp);
			if (d > maxCam + 0.25f)
				solved = false;
		}
		if (!solved || !shot.ok || !shot.fireAngles.IsValid()) {
#ifdef _DEBUG
			static std::
uint64_t s_lastSolve = 0;
			const std::
uint64_t tnow = AimCommon::
NowMs();
			if (tnow - s_lastSolve >= 2000ull) {
				s_lastSolve = tnow;
				Con::
Info("trigger seed FAIL: Solve tick=%d hb=%d", seedTick, hitHb);
			}
#endif
			return true;
		}

		// Keep roll - IDA FireBullet AngleVectors uses z; seed hash ignores it.
		QAngle_t fireAng = shot.fireAngles;
		if (!std::
isfinite(fireAng.z))
			fireAng.z = 0.f;
		fireAng.x = std::
clamp(fireAng.x, -89.f, 89.f);
		fireAng.Normalize();

		// Bin-safe on final fireAngles: if Solve rewrote pitch/yaw onto a
		// half-deg edge, server-side quant can flip -> different SPREADSEEDGEN
		// seed -> miss. Nudge is <0.15? so it does NOT invalidate the pellet
		// (capsule accept scale 0.94-0.96 has room). Roll (z) preserved.
		{
			QAngle_t punchNudge{};
			const QAngle_t* pn = nullptr;
			if (HitChance::
ReadSeedFirePunch(lp, pWpn, seedTick, seedFrac, punchNudge)
				&& punchNudge.IsValid())
				pn = &punchNudge;
			const float rollKeep = fireAng.z;
			NoSpread::
NudgeBinSafe(fireAng, pn);
			fireAng.z = rollKeep;
			fireAng.Normalize();
		}
		int hitHbSeed = shot.hitbox;
		Vector_t hitPtSeed = shot.hitPoint;
		if (shot.seedTick > 0)
			seedTick = shot.seedTick;
		if (std::
isfinite(shot.seedFrac))
			seedFrac = shot.seedFrac;

		// Hard mindmg on Solve pellet HB only (same-HB center). Fail closed.
		// Re-check wall on PELLET (not pre-Solve crosshair) - stale allowPen used
		// vis mindmg on wall pellets / AW mindmg on free LOS.
		if (!Bones::
IsValidPos(hitPtSeed) || hitHbSeed < 0
			|| hitHbSeed >= Config::
HB_COUNT
			|| !Config::
trigger_hitboxes[hitHbSeed])
			return true;
		if (!Trace::
Ready())
			return true; // can't validate wall/pen - fail closed
		// Live eye first (hist lag = false wall); seedEye for dmg sim.
		const bool pelletBehind = AimCommon::IsBehindWall(
			lepDefault, hitPtSeed, lp, target, Trace::kMaskShot);
		if (pelletBehind && !trAwOn)
			return true;
		const bool pelletAllowPen = pelletBehind && trAwOn;
		const bool overrideMinDmg = keybind.isActive(Config::mindamage_override);
		const float seedMinDmg = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage;
		const float seedMinDmgAw = overrideMinDmg ? Config::mindamage_override_value : Config::trigger_mindamage_aw;
		if (!NoSpread::SeedPassesDamage(
				seedEye, hitPtSeed, hitHbSeed, pWpn, lp, target, pelletAllowPen,
				seedMinDmg,
				seedMinDmgAw))
			return true;

		// Stamp solved angles + the ring-clamped player tick. Old path left
		// nPlayerTickCount at a stale hist tick (#8128) while the weapon
		// ring only had #8139..#8141 -> shoot-pos miss + seed hash mismatch.
			if (cmd && atkIdx >= 0) {
				const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
				if (atkIdx < histCount) {
					if (CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(atkIdx)) {
						if (seedTick > 0) {
							e->nPlayerTickCount = seedTick;
							e->flPlayerTickFraction = seedFrac;
							e->SetBits(
								EInputHistoryBits::INPUT_HISTORY_BITS_PLAYERTICKCOUNT
								| EInputHistoryBits::INPUT_HISTORY_BITS_PLAYERTICKFRACTION);
						}
					}
					cmd->SetAttackHistoryFire(atkIdx, fireAng, seedEye, true);
					Bones::StampShootPositionHistory(lp, seedTick, seedFrac, seedEye);
				}
			}

		g_triggerFireAngle = fireAng;
		g_triggerFireEye = seedEye;
		g_triggerFireValid = true;
		g_triggerWantShoot = true;
		g_triggerFireHist = atkIdx;
		NoSpread::
NoteSeedFired(pWpn, lp);
#ifdef _DEBUG
		{
			// FIRE log once per real shot (gap already prevents spam)
			static std::
uint64_t s_lastFireS = 0;
			const std::
uint64_t tnow = AimCommon::
NowMs();
			if (tnow - s_lastFireS >= 100ull) {
				s_lastFireS = tnow;
				Con::
Info("trigger seed FIRE tick=%d hb=%d", seedTick, hitHbSeed);
			}
		}
#endif
		hitHb = hitHbSeed;
		hitPoint = hitPtSeed;
		seedFired = true;
	}

	if (!seedFired) {
		// Hitchance / flick path only (never seed fallthrough)
		g_triggerFireAngle = aimView;
		g_triggerFireEye = winEye;
		g_triggerFireValid = true;
		g_triggerWantShoot = true;
#ifdef _DEBUG
		static std::
uint64_t s_lastFireLog = 0;
		if (AimCommon::
NowMs() - s_lastFireLog >= 200ull) {
			s_lastFireLog = AimCommon::
NowMs();
			Con::
Info("trigger FIRE path=%s ang=(%.1f,%.1f) eye=(%.0f,%.0f,%.0f) hb=%d flick=%d score=%d seedWanted=%d seedReady=%d",
				hitchanceMode ? "hitchance" : (seedWanted ? "seed-fallback-BUG" : "other"),
				aimView.x, aimView.y, winEye.x, winEye.y, winEye.z, hitHb, flicking, bestScore,
				seedWanted ? 1 : 0, seedReady ? 1 : 0);
		}
#endif
		if (cmd) {
			const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			int idx = (winHist >= 0 && winHist < histCount) ? winHist : -1;
			if (idx < 0 && histCount > 0)
				idx = histCount - 1;
			g_triggerFireHist = idx;
			if (idx >= 0)
				cmd->SetAttackHistoryFire(idx, aimView, winEye);
		} else {
			g_triggerFireHist = winHist;
		}
	}

	return true;
}

// SEH shell - bone/trace/entity faults must not take down the process
bool RunSafe(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	bool ok = false;
	__try {
		ok = RunTriggerbotImpl(lp, cmd);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ResetInternal();
		ok = false;
	}
	return ok;
}

bool Run(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	return RunSafe(lp, cmd);
}

void Reset() {
	ResetInternal();
}

bool WantShoot() { return g_triggerWantShoot; }
bool WantStop() { return g_triggerWantStop; }
bool FireValid() { return g_triggerFireValid; }
const QAngle_t& FireAngle() { return g_triggerFireAngle; }
const Vector_t& FireEye() { return g_triggerFireEye; }
int FireHistIndex() { return g_triggerFireHist; }

void ClearShootFlags() {
	g_triggerWantShoot = false;
	g_triggerWantStop = false;
	g_triggerFireValid = false;
	g_triggerFireHist = -1;
}

} // namespace Triggerbot

