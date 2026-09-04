#include "nospread.h"
#include "../hitchance/hitchance.h"
#include "../bones/bones.h"
#include "../aim/aim_common.h"
#include "../autowall/autowall.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>

// ???????????????????????????????????????????????????????????????????????
// Seed nospread - online-stable rewrite (IDA live 2026-07-28)
//
// CSBaseGunFire:
// punched = view + ComputeAimPunchFire(tick,frac)
// seed = SPREADSEEDGEN(punched, nPlayerTickCount) @ 0x180CB8450
// (sx,sy) = CalcSpread(..., seed+1, ...) @ 0x180CB8D70
// dir = fwd - right*sx + up*sy @ FireBullet
//
// Online miss / model flick:
// - local ValveRng ULP ? server -> freeze GAME pellet once from wish
// - big pitch/yaw rewrite -> FillGunFireData hist interp -> model flick
// Prefer: roll-trick (seed ignores roll) -> closed-form same seed-bin ->
// limited rewrite only if needed.
// ???????????????????????????????????????????????????????????????????????

namespace NoSpread {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.f / kPi;
// Accept scale - limbs need looser rim (feet/legs thin capsules).
constexpr float kSearchScale = 0.96f;
constexpr float kSearchBias  = 0.92f;
constexpr float kAcceptScale = 0.96f;
constexpr float kAcceptBias  = 0.92f;
// IDA SPREADSEEDGEN 0x180CB6E80: SHA1(quant pitch, quant yaw, tick) - roll not hashed.
// FireBullet 0x1808474E0: dir = fwd - right*sx + up*sy (AngleVectors uses roll).

bool DirToAngles(const Vector_t& dir, QAngle_t& out)
{
	const float hyp = std::sqrt(dir.x * dir.x + dir.y * dir.y);
	if (!std::isfinite(hyp) || hyp < 1e-8f)
		return false;
	out.x = -std::atan2(dir.z, hyp) * kRad2Deg;
	out.y = std::atan2(dir.y, dir.x) * kRad2Deg;
	out.z = 0.f;
	out.Normalize();
	return out.IsValid();
}

bool CalcAngles(const Vector_t& from, const Vector_t& to, QAngle_t& out)
{
	Vector_t d{ to.x - from.x, to.y - from.y, to.z - from.z };
	const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (!std::isfinite(len) || len < 1e-4f)
		return false;
	d.x /= len; d.y /= len; d.z /= len;
	return DirToAngles(d, out);
}

float AngDelta(const QAngle_t& a, const QAngle_t& b)
{
	float dp = a.x - b.x;
	float dy = a.y - b.y;
	while (dy > 180.f) dy -= 360.f;
	while (dy < -180.f) dy += 360.f;
	return std::sqrt(dp * dp + dy * dy);
}

bool NormalizeDir(Vector_t& d)
{
	const float lenSqr = d.x * d.x + d.y * d.y + d.z * d.z;
	if (!std::isfinite(lenSqr) || lenSqr < 1e-12f)
		return false;
	const float inv = 1.f / std::sqrt(lenSqr);
	d.x *= inv; d.y *= inv; d.z *= inv;
	return std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z);
}

// IDA FireBullet inverse: wantDir = fwd - right*sx + up*sy
// -> fwd ? want + right*sx - up*sy, iterate orthonormal basis.
bool SolveViewForDir(
	const Vector_t& wantDir,
	float sx,
	float sy,
	const QAngle_t& punchedStart,
	QAngle_t& outPunched)
{
	QAngle_t punched = punchedStart;
	punched.z = 0.f;
	punched.Normalize();
	if (!punched.IsValid())
		return false;

	for (int it = 0; it < 12; ++it) {
		Vector_t fwd{}, right{}, up{};
		punched.ToDirections(&fwd, &right, &up);
		Vector_t ideal{
			wantDir.x + right.x * sx - up.x * sy,
			wantDir.y + right.y * sx - up.y * sy,
			wantDir.z + right.z * sx - up.z * sy
		};
		if (!NormalizeDir(ideal))
			return false;
		QAngle_t next{};
		if (!DirToAngles(ideal, next))
			return false;
		if (AngDelta(punched, next) < 0.008f) {
			outPunched = next;
			return true;
		}
		punched = next;
	}
	outPunched = punched;
	return outPunched.IsValid();
}

} // namespace (close anon so NudgeBinSafe is public in NoSpread)

// IDA CB1340: floor(AngleNormalize(a)*2)*0.5 - keep punch away from bin edges.
// Public: triggerbot / autofire call this on final fireAngles before stamping
// to guard against server-side ULP quant flip on SPREADSEEDGEN half-deg bins.
void NudgeBinSafe(QAngle_t& view, const QAngle_t* punch)
{
	QAngle_t punched = view;
	if (punch) {
		punched.x += punch->x;
		punched.y += punch->y;
	}
	punched.Normalize();

	auto edge = [](float a) {
		// fractional part of a*2 after normalize into half-deg bins
		float n = a;
		while (n > 180.f) n -= 360.f;
		while (n < -180.f) n += 360.f;
		const float q = std::floor(n * 2.f) * 0.5f;
		return n - q; // [0, 0.5)
	};
	const float fx = edge(punched.x);
	const float fy = edge(punched.y);
	if (fx < 0.10f)
		view.x += (0.10f - fx);
	else if (fx > 0.40f)
		view.x -= (fx - 0.40f);
	if (fy < 0.10f)
		view.y += (0.10f - fy);
	else if (fy > 0.40f)
		view.y -= (fy - 0.40f);
	// Keep roll - FireBullet AngleVectors uses z; seed hash ignores it.
	// Zeroing z here wiped roll-trick solutions -> overspray miss.
	if (!std::isfinite(view.x))
		view.x = 0.f;
	if (!std::isfinite(view.y))
		view.y = 0.f;
	if (!std::isfinite(view.z))
		view.z = 0.f;
	view.x = std::clamp(view.x, -89.f, 89.f);
	view.Normalize();
}

namespace { // reopen anon for remaining helpers

bool HbEnabled(int hb, const bool* enabled)
{
	if (hb < 0 || hb >= Config::HB_COUNT)
		return false;
	if (!enabled)
		return true;
	return enabled[hb];
}

bool IsCore(int hb)
{
	return hb == Config::HB_HEAD || hb == Config::HB_NECK
		|| hb == Config::HB_CHEST || hb == Config::HB_STOMACH
		|| hb == Config::HB_PELVIS;
}

// Pitch/yaw rewrite budget. Online: roll free (seed ignores z). Pitch/yaw
// rewrite must cover deagle/AWP cone - too tight = miss; too wide = flick.
// Roll-trick does not consume this budget.
float MaxDeltaDeg(float inac, float spr, C_CSWeaponBase* weapon, C_CSPlayerPawn* local)
{
	const float bloom = (std::isfinite(inac) && std::isfinite(spr))
		? (inac + spr) : 0.f;
	const float cone = bloom * kRad2Deg;

	bool air = false;
	if (local) {
		__try {
			const std::uint32_t f = local->m_fFlags();
			air = (f & FL_ONGROUND) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			air = false;
		}
	}

	const bool heavy = AimCommon::IsHeavyPistol(weapon);
	const bool sniper = AimCommon::IsSniperWeapon(weapon);

	float d = 0.f;
	if (heavy) {
		// Deagle jump bloom ~0.3-0.8 rad - need room for closed-form pitch, not 72?.
		d = (std::max)(air ? 14.0f : 8.0f, cone * (air ? 2.2f : 1.5f) + (air ? 5.0f : 2.0f));
		return std::clamp(d, air ? 14.0f : 8.0f, air ? 34.f : 24.f);
	}
	if (sniper) {
		// FillGunFireData mode-1 interp: keep near cam, but allow pellet cone.
		bool scoped = false;
		if (local) {
			__try { scoped = AimCommon::IsLocalScoped(local, weapon); }
			__except (EXCEPTION_EXECUTE_HANDLER) { scoped = false; }
		}
		if (!scoped && air)
			return std::clamp((std::max)(5.0f, cone * 1.0f + 1.0f), 5.0f, 10.f);
		if (!scoped)
			return std::clamp((std::max)(5.0f, cone * 1.1f + 1.5f), 5.0f, 12.f);
		d = (std::max)(air ? 5.0f : 4.5f, cone * 1.2f + 1.5f);
		return std::clamp(d, air ? 5.0f : 4.5f, air ? 14.f : 16.f);
	}
	// Rifles: cone-sized rewrite only. Old 22-28? closed-form during spray
	// stamped hist far from cam -> FillGunFireData interp miss (overspray).
	d = (std::max)(4.0f, cone * 1.15f + 1.0f);
	return std::clamp(d, 4.0f, air ? 14.f : 12.f);
}

// Half-deg quant (IDA CB1340) - roll-trick bin skip without SHA1.
float QuantHalf(float a)
{
	while (a > 180.f) a -= 360.f;
	while (a < -180.f) a += 360.f;
	return std::floor(a * 2.f) * 0.5f;
}

// UC / IDA roll-trick: same seed (pitch/yaw quant), roll cancels pellet.
// IDA SPREADSEEDGEN hashes quant(pitch), quant(yaw), tick - roll ignored.
// FireBullet: dir = fwd - right*sx + up*sy (AngleVectors USES roll).
//
// Classic cancel: pitch += deg(atan(|s|)), roll = -deg(atan2(sx,sy)).
// Big bloom (deagle air / unscoped AWP) -> pitchAdj flips half-deg bin -> seed
// miss. Fix: try pure-roll first (no pitch), then pitch+roll with half-deg
// walk that KEEPS seed0 (quant-skip SHA1). Wider ?4? for online heavies.
bool RollTrickView(
	const QAngle_t& wishUnpunched,
	const QAngle_t* punch,
	int seedTick,
	float sx,
	float sy,
	QAngle_t& outView)
{
	if (seedTick <= 0)
		return false;
	if (!std::isfinite(sx) || !std::isfinite(sy))
		return false;

	QAngle_t punchedWish = wishUnpunched;
	if (punch && punch->IsValid()) {
		punchedWish.x += punch->x;
		punchedWish.y += punch->y;
	}
	punchedWish.z = 0.f;
	punchedWish.Normalize();

	const float len = std::sqrt(sx * sx + sy * sy);
	if (!std::isfinite(len) || len < 1e-8f)
		return false;

	const std::uint32_t seed0 = HitChance::ComputeSeed(punchedWish, seedTick);
	const float pitchAdj = std::atan(len) * kRad2Deg;
	const float roll = -std::atan2(sx, sy) * kRad2Deg;
	const float qWishX = QuantHalf(punchedWish.x);
	const float qWishY = QuantHalf(punchedWish.y);

	auto toView = [&](const QAngle_t& punchedCand, QAngle_t& out) -> bool {
		QAngle_t view = punchedCand;
		if (punch && punch->IsValid()) {
			view.x -= punch->x;
			view.y -= punch->y;
		}
		view.x = std::clamp(view.x, -89.f, 89.f);
		view.Normalize();
		if (!view.IsValid())
			return false;
		out = view;
		return true;
	};

	// Classic pitch+roll FIRST (best cancel). Pure-roll alone is partial and
	// would always return early if tried first -> never reach pitch walk.
	// k=0..16 -> 0, ?0.5 ... ?4.0. Skip SHA1 when quant matches wish.
	const float basePitch = punchedWish.x + pitchAdj;
	float lastQx = 1e9f;
	for (int k = 0; k < 17; ++k) {
		float tryPitch = basePitch;
		if (k > 0) {
			const int step = (k + 1) / 2;
			const float sign = (k % 2 == 1) ? 1.f : -1.f;
			tryPitch += sign * static_cast<float>(step) * 0.5f;
		}
		tryPitch = std::clamp(tryPitch, -89.f, 89.f);
		const float qx = QuantHalf(tryPitch);
		if (k > 0 && std::fabs(qx - lastQx) < 1e-4f)
			continue;
		lastQx = qx;

		QAngle_t punchedCand{ tryPitch, punchedWish.y, roll };
		punchedCand.Normalize();
		const bool sameBin = std::fabs(qx - qWishX) < 1e-4f
			&& std::fabs(QuantHalf(punchedCand.y) - qWishY) < 1e-4f;
		if (!sameBin && HitChance::ComputeSeed(punchedCand, seedTick) != seed0)
			continue;

		if (toView(punchedCand, outView))
			return true;
	}

	// Fallback: pure roll on wish pitch - same quant -> same seed always.
	// Partial cancel only; closed-form / bins may still land after.
	{
		QAngle_t punchedCand{ punchedWish.x, punchedWish.y, roll };
		punchedCand.Normalize();
		if (toView(punchedCand, outView))
			return true;
	}
	return false;
}

// Local ran1 only - search path (stomps nothing). Prefer FreezeGamePellet for online.
bool BulletDir(
	const QAngle_t& view,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	const QAngle_t* punch,
	Vector_t& outDir,
	float* outSx,
	float* outSy)
{
	return HitChance::GetBulletDirectionCached(
		view, seedTick, weapon, inac, spr, outDir, outSx, outSy, 1u, punch,
		/*useLocalSpread=*/true);
}

// One game CalcSpread (matches server). Call sparingly - stomps tier0 RandomSeed.
bool BulletDirGame(
	const QAngle_t& view,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	const QAngle_t* punch,
	Vector_t& outDir,
	float* outSx,
	float* outSy)
{
	return HitChance::GetBulletDirectionCached(
		view, seedTick, weapon, inac, spr, outDir, outSx, outSy, 1u, punch,
		/*useLocalSpread=*/false);
}

// Build FireBullet dir from known (sx,sy) - no seedgen/CalcSpread.
bool DirFromPellet(
	const QAngle_t& view,
	const QAngle_t* punch,
	float sx,
	float sy,
	Vector_t& outDir)
{
	QAngle_t ang = view;
	if (punch && punch->IsValid()) {
		ang.x += punch->x;
		ang.y += punch->y;
	}
	if (!std::isfinite(ang.z))
		ang.z = 0.f;
	ang.Normalize();
	if (!ang.IsValid())
		return false;
	Vector_t fwd{}, right{}, up{};
	ang.ToDirections(&fwd, &right, &up);
	outDir = {
		fwd.x - right.x * sx + up.x * sy,
		fwd.y - right.y * sx + up.y * sy,
		fwd.z - right.z * sx + up.z * sy
	};
	return NormalizeDir(outDir);
}

// Freeze server pellet once from wish (game CalcSpread).
// ONLY reuse when candidate stays in same SPREADSEEDGEN bin (quant pitch/yaw).
// Pitch/yaw rewrite -> new seed -> new (sx,sy) - must recompute.
struct FrozenPellet {
	float sx = 0.f;
	float sy = 0.f;
	float qPitch = 0.f; // quant half-deg of punched pitch
	float qYaw = 0.f;
	std::uint32_t seed = 0;
	bool ok = false;
};

void PunchedQuant(const QAngle_t& viewUnpunched, const QAngle_t* punch,
	float& outQx, float& outQy)
{
	QAngle_t p = viewUnpunched;
	if (punch && punch->IsValid()) {
		p.x += punch->x;
		p.y += punch->y;
	}
	p.z = 0.f;
	p.Normalize();
	outQx = QuantHalf(p.x);
	outQy = QuantHalf(p.y);
}

bool SameSeedBin(const FrozenPellet& fr, const QAngle_t& viewUnpunched,
	const QAngle_t* punch)
{
	if (!fr.ok)
		return false;
	float qx = 0.f, qy = 0.f;
	PunchedQuant(viewUnpunched, punch, qx, qy);
	return std::fabs(qx - fr.qPitch) < 1e-4f && std::fabs(qy - fr.qYaw) < 1e-4f;
}

bool FreezeGamePellet(
	const QAngle_t& wishUnpunched,
	const QAngle_t* punch,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	FrozenPellet& out)
{
	out = {};
	if (seedTick <= 0 || !weapon)
		return false;
	Vector_t dir{};
	float sx = 0.f, sy = 0.f;
	// Game path first - bit-exact vs server. Fallback local if sig miss.
	if (!BulletDirGame(wishUnpunched, seedTick, weapon, inac, spr, punch, dir, &sx, &sy)) {
		if (!BulletDir(wishUnpunched, seedTick, weapon, inac, spr, punch, dir, &sx, &sy))
			return false;
	}
	if (!std::isfinite(sx) || !std::isfinite(sy))
		return false;
	out.sx = sx;
	out.sy = sy;
	PunchedQuant(wishUnpunched, punch, out.qPitch, out.qYaw);
	QAngle_t punched = wishUnpunched;
	if (punch && punch->IsValid()) {
		punched.x += punch->x;
		punched.y += punch->y;
	}
	punched.z = 0.f;
	punched.Normalize();
	out.seed = HitChance::ComputeSeed(punched, seedTick);
	out.ok = true;
	return true;
}

// All menu-enabled HBs. Prefer first, then core, then limbs.
bool RayHitsAny(
	C_CSPlayerPawn* target,
	const Vector_t& eye,
	const Vector_t& dir,
	int preferHb,
	const bool* enabled,
	int& outHb,
	Vector_t& outPt,
	float scale,
	float bias,
	bool allowLimbs)
{
	static constexpr int kCore[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS
	};
	static constexpr int kLimb[] = {
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};
	int tryList[12]{};
	int n = 0;
	auto push = [&](int hb) {
		if (hb < 0 || hb >= Config::HB_COUNT || !HbEnabled(hb, enabled))
			return;
		for (int i = 0; i < n; ++i)
			if (tryList[i] == hb) return;
		tryList[n++] = hb;
	};
	// Prefer whatever ray/crosshair hit (limb or core)
	if (preferHb >= 0)
		push(preferHb);
	for (int hb : kCore)
		push(hb);
	if (allowLimbs) {
		for (int hb : kLimb)
			push(hb);
	}
	for (int i = 0; i < n; ++i) {
		float t = 0.f;
		Vector_t pt{};
		if (!Bones::RayHitsConfiguredHitbox(
				target, tryList[i], eye, dir, scale, t, pt, bias))
			continue;
		outHb = tryList[i];
		outPt = pt;
		return true;
	}
	return false;
}

// localAir/heavy/sniper precomputed once per SolveForAim - avoid SEH spam.
struct TryCtx {
	bool localAir = false;
	bool heavy = false;
	bool sniper = false;
	float accScale = kAcceptScale;
	float accBias = kAcceptBias;
	float gScale = 0.98f;
	float gBias = 0.94f;
	bool corePreferGate = false; // grounded non-heavy high bloom
};

void BuildTryCtx(C_CSWeaponBase* weapon, C_CSPlayerPawn* local,
	float inac, float spr, TryCtx& c)
{
	c = {};
	c.heavy = AimCommon::IsHeavyPistol(weapon);
	c.sniper = AimCommon::IsSniperWeapon(weapon);
	if (local) {
		__try {
			c.localAir = (local->m_fFlags() & FL_ONGROUND) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			c.localAir = false;
		}
	}
	// Heavy: tighter accept - old 1.05 scale accepted rim hits on huge deagle cone
	// -> "solved" then server pellet misses (overspray feel).
	if (c.heavy) {
		const float bloom = (std::isfinite(inac) && std::isfinite(spr)) ? (inac + spr) : 0.f;
		if (c.localAir || bloom > 0.20f) {
			c.accScale = 0.88f;
			c.accBias = 0.85f;
			c.gScale = 0.88f;
			c.gBias = 0.85f;
		} else {
			c.accScale = 0.94f;
			c.accBias = 0.90f;
			c.gScale = 0.94f;
			c.gBias = 0.90f;
		}
		c.corePreferGate = true; // prefer head/chest over limbs when bloom high
	} else {
		const float bloom = (std::isfinite(inac) && std::isfinite(spr)) ? (inac + spr) : 0.f;
		// Spray bloom: 0.96 rim on a fat cone = graze "hit" then server miss.
		if (bloom > 0.16f || c.sniper) {
			c.accScale = 0.90f;
			c.accBias = 0.86f;
			c.gScale = 0.90f;
			c.gBias = 0.86f;
			c.corePreferGate = !c.localAir;
		} else if (bloom > 0.08f) {
			c.gScale = 0.94f;
			c.gBias = 0.90f;
			c.corePreferGate = !c.localAir && bloom > 0.12f;
		}
	}
}

// frozen: when set, use fixed game pellet (same seed-bin) - no re-CalcSpread.
// Online: same (sx,sy) as server for all roll/closed-form candidates.
bool TryView(
	const Vector_t& eye,
	const QAngle_t& view,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punch,
	int preferHb,
	const bool* enabled,
	float maxDelta,
	const QAngle_t& wish,
	const TryCtx& ctx,
	Shot& best,
	const FrozenPellet* frozen = nullptr)
{
	(void)local;
	// Pitch/yaw delta only - roll free (seed ignores roll).
	QAngle_t wishPy = wish; wishPy.z = 0.f;
	QAngle_t viewPy = view; viewPy.z = 0.f;
	if (!view.IsValid() || AngDelta(wishPy, viewPy) > maxDelta + 0.05f)
		return false;

	// Keep roll - IDA FireBullet basis uses it (seed hash does not).
	QAngle_t cand = view;
	if (!std::isfinite(cand.z))
		cand.z = 0.f;
	cand.Normalize();
	if (!cand.IsValid())
		return false;

	Vector_t dir{};
	float sx = 0.f, sy = 0.f;
	// Frozen pellet ONLY if same quant pitch/yaw as wish (same seed).
	// Pitch/yaw rewrite -> re-run local ran1 (game stomp only on final accept).
	if (frozen && SameSeedBin(*frozen, cand, punch)) {
		sx = frozen->sx;
		sy = frozen->sy;
		if (!DirFromPellet(cand, punch, sx, sy, dir))
			return false;
	} else {
		if (!BulletDir(cand, seedTick, weapon, inac, spr, punch, dir, &sx, &sy))
			return false;
	}

	int hb = -1;
	Vector_t pt{};
	const bool limbs = !ctx.corePreferGate || (preferHb >= 0 && !IsCore(preferHb));
	if (!RayHitsAny(target, eye, dir, preferHb, enabled, hb, pt,
			ctx.accScale, ctx.accBias, limbs))
		return false;

	// Core prefer only when grounded + high bloom.
	if (ctx.corePreferGate && !IsCore(hb) && preferHb >= 0 && IsCore(preferHb)) {
		bool anyCore = false;
		if (enabled) {
			for (int i = 0; i < Config::HB_COUNT; ++i)
				if (enabled[i] && IsCore(i)) { anyCore = true; break; }
		} else anyCore = true;
		if (anyCore)
			return false;
	}

	best.fireAngles = cand;
	best.hitPoint = pt;
	best.hitbox = hb;
	best.seedTick = seedTick;
	best.seedFrac = tickFrac;
	best.sx = sx;
	best.sy = sy;
	best.ok = true;
	return true;
}

// Accept with frozen game pellet (preferred) or one game re-verify.
bool AcceptView(
	const Vector_t& eye,
	const QAngle_t& view,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punch,
	int preferHb,
	const bool* enabled,
	float maxDelta,
	const QAngle_t& wish,
	const TryCtx& ctx,
	Shot& best,
	const FrozenPellet* frozen = nullptr)
{
	Shot tmp{};
	if (!TryView(eye, view, seedTick, tickFrac, weapon, local, target, inac, spr,
			punch, preferHb, enabled, maxDelta, wish, ctx, tmp, frozen))
		return false;

	// Same-bin freeze = already server pellet. Else game re-verify (pitch rewrite).
	if (!frozen || !SameSeedBin(*frozen, tmp.fireAngles, punch)) {
		Vector_t gDir{};
		float gsx = 0.f, gsy = 0.f;
		if (BulletDirGame(tmp.fireAngles, seedTick, weapon, inac, spr, punch, gDir, &gsx, &gsy)) {
			int ghb = -1;
			Vector_t gpt{};
			if (!RayHitsAny(target, eye, gDir, preferHb, enabled, ghb, gpt,
					ctx.gScale, ctx.gBias,
					!ctx.corePreferGate || (preferHb >= 0 && !IsCore(preferHb))))
				return false;
			tmp.hitbox = ghb;
			tmp.hitPoint = gpt;
			tmp.sx = gsx;
			tmp.sy = gsy;
		}
	}
	// Re-check core prefer on final HB
	if (ctx.corePreferGate && !IsCore(tmp.hitbox) && preferHb >= 0 && IsCore(preferHb)) {
		bool anyCore = false;
		if (enabled) {
			for (int i = 0; i < Config::HB_COUNT; ++i)
				if (enabled[i] && IsCore(i)) { anyCore = true; break; }
		} else anyCore = true;
		if (anyCore)
			return false;
	}
	best = tmp;
	return best.ok;
}

// Finish a local probe - prefer frozen game pellet (no second CalcSpread).
bool AcceptProbe(
	const Vector_t& eye,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punch,
	int preferHb,
	const bool* enabled,
	const TryCtx& ctx,
	Shot& probe,
	Shot& best,
	const FrozenPellet* frozen = nullptr)
{
	if (!probe.ok)
		return false;
	if (frozen && SameSeedBin(*frozen, probe.fireAngles, punch)) {
		Vector_t dir{};
		if (!DirFromPellet(probe.fireAngles, punch, frozen->sx, frozen->sy, dir))
			return false;
		int hb = -1;
		Vector_t pt{};
		if (!RayHitsAny(target, eye, dir, preferHb, enabled, hb, pt,
				ctx.gScale, ctx.gBias,
				!ctx.corePreferGate || (preferHb >= 0 && !IsCore(preferHb))))
			return false;
		probe.hitbox = hb;
		probe.hitPoint = pt;
		probe.sx = frozen->sx;
		probe.sy = frozen->sy;
	} else {
		Vector_t gDir{};
		float gsx = 0.f, gsy = 0.f;
		if (BulletDirGame(probe.fireAngles, seedTick, weapon, inac, spr, punch, gDir, &gsx, &gsy)) {
			int ghb = -1;
			Vector_t gpt{};
			if (!RayHitsAny(target, eye, gDir, preferHb, enabled, ghb, gpt,
					ctx.gScale, ctx.gBias,
					!ctx.corePreferGate || (preferHb >= 0 && !IsCore(preferHb))))
				return false;
			probe.hitbox = ghb;
			probe.hitPoint = gpt;
			probe.sx = gsx;
			probe.sy = gsy;
		}
	}
	if (ctx.corePreferGate && !IsCore(probe.hitbox) && preferHb >= 0 && IsCore(preferHb)) {
		bool anyCore = false;
		if (enabled) {
			for (int i = 0; i < Config::HB_COUNT; ++i)
				if (enabled[i] && IsCore(i)) { anyCore = true; break; }
		} else anyCore = true;
		if (anyCore)
			return false;
	}
	best = probe;
	return best.ok;
}

// Per-weapon fire gap. CanWeaponFire alone fails-open when nextTick=0 / tickbase lag
// -> deagle re-fires every ~160ms (log 003530) and overspray-misses.
struct Latch {
	std::uint16_t def = 0;
	std::uint64_t fireMs = 0;
};
Latch g_latch{};

std::uint16_t WeaponDef(C_CSWeaponBase* w)
{
	if (!w) return 0;
	std::uint16_t d = 0;
	__try { d = w->m_iItemDefinitionIndex(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { d = 0; }
	return d;
}

std::uint64_t NowMs()
{
	return static_cast<std::uint64_t>(GetTickCount64());
}

// -- Live VData accuracy (dump CCSWeaponBaseVData / IDA GetInaccuracy path) --
// No per-def hardcodes. Cycle + stand/fire/jump/spread from weapon VData.

struct VDataAcc {
	float cycle = 0.1f;
	float spread = 0.f;
	float stand = 0.f;
	float fire = 0.f;
	float jump = 0.f;
	float move = 0.f;
	float recoverStand = 0.f;
	bool ok = false;
};

bool ReadVDataAcc(C_CSWeaponBase* weapon, VDataAcc& out)
{
	out = {};
	if (!weapon || !Mem::ValidEntity(weapon))
		return false;
	CCSWeaponBaseVData* vd = nullptr;
	int mode = 0;
	__try {
		vd = weapon->Data();
		mode = weapon->m_weaponMode();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!vd)
		return false;
	if (mode != 0 && mode != 1)
		mode = 0;

	__try {
		out.cycle = (mode == 1) ? vd->m_flCycleTimeSecondary() : vd->m_flCycleTimePrimary();
		out.spread = (mode == 1) ? vd->m_flSpread1() : vd->m_flSpread0();
		out.stand = (mode == 1) ? vd->m_flInaccuracyStand1() : vd->m_flInaccuracyStand0();
		out.fire = (mode == 1) ? vd->m_flInaccuracyFire1() : vd->m_flInaccuracyFire0();
		out.jump = (mode == 1) ? vd->m_flInaccuracyJump1() : vd->m_flInaccuracyJump0();
		out.move = (mode == 1) ? vd->m_flInaccuracyMove1() : vd->m_flInaccuracyMove0();
		out.recoverStand = vd->m_flRecoveryTimeStand();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}

	auto fin = [](float& v, float fb) {
		if (!std::isfinite(v) || v < 0.f) v = fb;
	};
	fin(out.cycle, 0.1f);
	fin(out.spread, 0.f);
	fin(out.stand, 0.f);
	fin(out.fire, 0.f);
	fin(out.jump, 0.f);
	fin(out.move, 0.f);
	fin(out.recoverStand, 0.f);
	// Clamp cycle to sane range (AWP ~1.45, SMG ~0.07)
	out.cycle = std::clamp(out.cycle, 0.05f, 2.5f);
	out.ok = true;
	return true;
}

// Soft re-arm after NoteSeedFired - anti double-frame only.
// Engine nextTick (CanWeaponFire) is the real cycle gate. Old deagle 70% cycle
// + recovery bloom hold felt like multi-hundred-ms lag while already on target.
std::uint64_t SeedMinGapMs(C_CSWeaponBase* weapon)
{
	VDataAcc a{};
	const bool have = ReadVDataAcc(weapon, a) && a.ok;
	if (AimCommon::IsHeavyPistol(weapon)) {
		// Deagle cycle ~0.225s - ~1 tick soft only; CanWeaponFire re-arms rest
		float sec = have ? a.cycle * 0.08f : 0.016f;
		const auto ms = static_cast<std::uint64_t>(sec * 1000.f + 0.5f);
		return std::clamp(ms, 8ull, 32ull);
	}
	if (!have)
		return 4ull;
	float frac = 0.05f;
	if (AimCommon::IsSniperWeapon(weapon))
		frac = 0.10f;
	else if (AimCommon::IsSemiWeapon(weapon))
		frac = 0.08f;
	else if (AimCommon::IsSprayAutoWeapon(weapon))
		frac = 0.08f;
	float sec = a.cycle * frac;
	const auto ms = static_cast<std::uint64_t>(sec * 1000.f + 0.5f);
	if (AimCommon::IsSprayAutoWeapon(weapon))
		return std::clamp(ms, 12ull, 48ull);
	return std::clamp(ms, 2ull, 48ull);
}

} // namespace

float HistRewriteLimitDeg(C_CSWeaponBase* weapon, C_CSPlayerPawn* local)
{
	float inac = 0.f, spr = 0.f;
	if (!HitChance::ReadCurrentBloom(weapon, local, inac, spr))
		return 8.f;
	return MaxDeltaDeg(inac, spr, weapon, local);
}

// -- Public -----------------------------------------------------------

bool Init()
{
	if (!HitChance::Init())
		return false;
	return HitChance::SpreadSeedReady();
}

bool Ready()
{
	return HitChance::SpreadSeedReady();
}

// Solve pellet for one aim point / prefer HB.
// Online path: freeze GAME pellet once -> roll-only -> closed-form same bin ->
// limited pitch/yaw rewrite. No multi-CalcSpread (server desync + FPS).
bool SolveForAim(
	const Vector_t& eye,
	const QAngle_t& wishIn,
	const Vector_t& aimPt,
	int preferHitbox,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punchPtr,
	const bool* enabledHitboxes,
	const TryCtx& ctx,
	Shot& out)
{
	if (!Bones::IsValidPos(aimPt))
		return false;

	QAngle_t wish = wishIn;
	wish.z = 0.f;
	wish.Normalize();
	if (!wish.IsValid()) {
		if (!CalcAngles(eye, aimPt, wish))
			return false;
	}

	// Geometric wish toward this aim, punch-sub - only if near cam wish
	// (large bone-wish vs cam = model flick online).
	{
		QAngle_t toAim{};
		if (CalcAngles(eye, aimPt, toAim)) {
			if (punchPtr && punchPtr->IsValid()) {
				toAim.x -= punchPtr->x;
				toAim.y -= punchPtr->y;
			}
			toAim.z = 0.f;
			toAim.x = std::clamp(toAim.x, -89.f, 89.f);
			toAim.Normalize();
			if (toAim.IsValid()) {
				const float d = AngDelta(wish, toAim);
				// Blend toward aim only when already close - keeps hist near cam
				if (d <= 6.f)
					wish = toAim;
				else if (d <= 14.f) {
					// Soft blend 30% toward aim (not full snap)
					float dy = toAim.y - wish.y;
					while (dy > 180.f) dy -= 360.f;
					while (dy < -180.f) dy += 360.f;
					wish.x += (toAim.x - wish.x) * 0.30f;
					wish.y += dy * 0.30f;
					wish.z = 0.f;
					wish.x = std::clamp(wish.x, -89.f, 89.f);
					wish.Normalize();
				}
			}
		}
	}

	// Bin-edge nudge on wish - avoids quant flip at fire (online seed miss)
	NudgeBinSafe(wish, punchPtr);

	const float maxDelta = MaxDeltaDeg(inac, spr, weapon, local);

	Vector_t wantDir{};
	{
		QAngle_t toAim{};
		if (!CalcAngles(eye, aimPt, toAim))
			return false;
		toAim.ToDirections(&wantDir, nullptr, nullptr);
		if (!NormalizeDir(wantDir))
			return false;
	}

	out.seedFrac = tickFrac;

	// ONE game CalcSpread from wish - freeze for whole Solve (server pellet).
	FrozenPellet frozen{};
	const bool haveFrozen = FreezeGamePellet(
		wish, punchPtr, seedTick, weapon, inac, spr, frozen);
	const FrozenPellet* fp = haveFrozen ? &frozen : nullptr;
	const float wishSx = haveFrozen ? frozen.sx : 0.f;
	const float wishSy = haveFrozen ? frozen.sy : 0.f;

	// 0) Natural seed FIRST - zero rewrite when crosshair already good.
	if (AcceptView(eye, wish, seedTick, tickFrac, weapon, local, target, inac, spr,
			punchPtr, preferHitbox, enabledHitboxes, maxDelta, wish, ctx, out, fp))
		return true;

	// 1) Roll-trick - same seed bin, only roll changes (no model flick).
	// Prefer this over pitch/yaw rewrite online.
	if (haveFrozen) {
		QAngle_t rollView{};
		if (RollTrickView(wish, punchPtr, seedTick, wishSx, wishSy, rollView)) {
			if (AcceptView(eye, rollView, seedTick, tickFrac, weapon, local, target,
					inac, spr, punchPtr, preferHitbox, enabledHitboxes,
					maxDelta, wish, ctx, out, fp))
				return true;
		}
	}

	// 2) Closed-form invert with FROZEN pellet + roll (same seed bin if roll-only
	// enough; pitch/yaw change only within maxDelta).
	if (haveFrozen) {
		QAngle_t punchedWish = wish;
		if (punchPtr) {
			punchedWish.x += punchPtr->x;
			punchedWish.y += punchPtr->y;
		}
		punchedWish.z = 0.f;
		punchedWish.Normalize();

		QAngle_t punchedSolve{};
		if (SolveViewForDir(wantDir, wishSx, wishSy, punchedWish, punchedSolve)) {
			QAngle_t viewSolve = punchedSolve;
			if (punchPtr) {
				viewSolve.x -= punchPtr->x;
				viewSolve.y -= punchPtr->y;
			}
			// Keep roll 0 first; try roll cancel with frozen pellet
			viewSolve.z = 0.f;
			viewSolve.x = std::clamp(viewSolve.x, -89.f, 89.f);
			viewSolve.Normalize();

			// Prefer roll on solved pitch/yaw (same seed if quant holds)
			{
				QAngle_t rv{};
				if (RollTrickView(viewSolve, punchPtr, seedTick, wishSx, wishSy, rv)) {
					if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
							inac, spr, punchPtr, preferHitbox, enabledHitboxes,
							maxDelta, wish, ctx, out, fp))
						return true;
				}
			}

			// Only accept pitch/yaw rewrite if within budget
			QAngle_t wishPy = wish; wishPy.z = 0.f;
			QAngle_t solPy = viewSolve; solPy.z = 0.f;
			if (AngDelta(wishPy, solPy) <= maxDelta) {
				if (AcceptView(eye, viewSolve, seedTick, tickFrac, weapon, local, target,
						inac, spr, punchPtr, preferHitbox, enabledHitboxes,
						maxDelta, wish, ctx, out, fp))
					return true;
			}
		}
	}
	if (out.ok)
		return true;

	// Wide search: unscoped sniper air stays roll-only (FillGunFireData flick).
	// Unscoped grounded + deagle: allow tight bins - pure roll often not enough.
	bool skipWideSearch = false;
	bool scopedSniper = false;
	if (ctx.sniper && local) {
		__try {
			scopedSniper = AimCommon::IsLocalScoped(local, weapon);
		} __except (EXCEPTION_EXECUTE_HANDLER) { scopedSniper = false; }
		if (!scopedSniper && ctx.localAir)
			skipWideSearch = true; // air unscoped - roll/closed-form only
	}

	// 3) Half-deg bins. Frozen pellet; roll on each bin.
	// Unscoped sniper: 3x3 @ 0.5? (tight). Heavy/other: 3x3 @ 1.0?.
	if (!skipWideSearch)
	{
		const float step = (ctx.sniper && !scopedSniper) ? 0.5f : 1.0f;
		const float steps[3] = { -step, 0.f, step };
		for (float dx : steps) {
			for (float dy : steps) {
				if (dx == 0.f && dy == 0.f)
					continue; // natural already tried
				QAngle_t c = wish;
				c.x += dx;
				c.y += dy;
				c.z = 0.f;
				c.x = std::clamp(c.x, -89.f, 89.f);
				c.Normalize();
				QAngle_t wishPy = wish; wishPy.z = 0.f;
				if (AngDelta(wishPy, c) > maxDelta)
					continue;
				// Pitch rewrite -> new seed bin -> recompute pellet (not frozen)
				const FrozenPellet* binFp = (fp && SameSeedBin(*fp, c, punchPtr)) ? fp : nullptr;
				Shot probe{};
				if (TryView(eye, c, seedTick, tickFrac, weapon, local, target, inac, spr,
						punchPtr, preferHitbox, enabledHitboxes, maxDelta, wish, ctx,
						probe, binFp)) {
					if (AcceptProbe(eye, seedTick, weapon, target, inac, spr, punchPtr,
							preferHitbox, enabledHitboxes, ctx, probe, out, binFp))
						return true;
				}
				// Roll on bin with THIS bin's pellet if frozen miss
				float binSx = wishSx, binSy = wishSy;
				bool binHave = haveFrozen && binFp;
				if (!binHave && haveFrozen) {
					// re-freeze for new bin (one game call) - only when needed
					FrozenPellet reb{};
					if (FreezeGamePellet(c, punchPtr, seedTick, weapon, inac, spr, reb)) {
						binSx = reb.sx; binSy = reb.sy; binHave = true;
						// Accept with re-frozen
						QAngle_t rv{};
						if (RollTrickView(c, punchPtr, seedTick, binSx, binSy, rv)) {
							FrozenPellet localFr = reb;
							if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
									inac, spr, punchPtr, preferHitbox, enabledHitboxes,
									maxDelta, wish, ctx, out, &localFr))
								return true;
						}
					}
				} else if (haveFrozen) {
					QAngle_t rv{};
					if (RollTrickView(c, punchPtr, seedTick, binSx, binSy, rv)) {
						if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
								inac, spr, punchPtr, preferHitbox, enabledHitboxes,
								maxDelta, wish, ctx, out, fp))
							return true;
					}
				}
			}
		}
	}
	if (out.ok)
		return true;

	// 4) Cone spiral - frozen pellet only when same bin; re-freeze on rewrite.
	// Heavy air gets more samples (deagle jump). Unscoped air sniper skipped above.
	if (!skipWideSearch)
	{
		const float bloom = (std::isfinite(inac) && std::isfinite(spr)) ? (inac + spr) : 0.f;
		int budget = 14;
		if (bloom > 0.15f || ctx.localAir)
			budget = 18;
		if (ctx.heavy && ctx.localAir)
			budget = 24;
		if (ctx.sniper && !scopedSniper)
			budget = 10; // keep tight for unscoped
		constexpr float kGolden = 2.399963229728653f;
		for (int i = 0; i < budget; ++i) {
			const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(budget);
			const float r = maxDelta * std::sqrt(t);
			const float th = static_cast<float>(i) * kGolden;
			QAngle_t c = wish;
			c.x += r * std::cos(th);
			c.y += r * std::sin(th);
			c.z = 0.f;
			c.x = std::clamp(c.x, -89.f, 89.f);
			c.Normalize();
			QAngle_t wishPy = wish; wishPy.z = 0.f;
			if (AngDelta(wishPy, c) > maxDelta + 0.02f)
				continue;
			const FrozenPellet* spFp = (fp && SameSeedBin(*fp, c, punchPtr)) ? fp : nullptr;
			Shot probe{};
			if (TryView(eye, c, seedTick, tickFrac, weapon, local, target, inac, spr,
					punchPtr, preferHitbox, enabledHitboxes, maxDelta, wish, ctx,
					probe, spFp)) {
				if (AcceptProbe(eye, seedTick, weapon, target, inac, spr, punchPtr,
						preferHitbox, enabledHitboxes, ctx, probe, out, spFp))
					return true;
			}
			if (haveFrozen && spFp) {
				QAngle_t rv{};
				if (RollTrickView(c, punchPtr, seedTick, wishSx, wishSy, rv)) {
					if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
							inac, spr, punchPtr, preferHitbox, enabledHitboxes,
							maxDelta, wish, ctx, out, fp))
						return true;
				}
			}
		}
	}

	// 5) Pure geometric + roll (only if near wish - avoid wild snap)
	{
		QAngle_t pure{};
		if (CalcAngles(eye, aimPt, pure)) {
			if (punchPtr) {
				pure.x -= punchPtr->x;
				pure.y -= punchPtr->y;
			}
			pure.z = 0.f;
			pure.x = std::clamp(pure.x, -89.f, 89.f);
			pure.Normalize();
			if (pure.IsValid() && AngDelta(wish, pure) <= maxDelta + 2.f) {
				if (AcceptView(eye, pure, seedTick, tickFrac, weapon, local, target,
						inac, spr, punchPtr, preferHitbox, enabledHitboxes,
						maxDelta, pure, ctx, out, fp))
					return true;
				if (haveFrozen) {
					QAngle_t rv{};
					if (RollTrickView(pure, punchPtr, seedTick, wishSx, wishSy, rv)) {
						if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
								inac, spr, punchPtr, preferHitbox, enabledHitboxes,
								maxDelta, pure, ctx, out, fp))
							return true;
					}
				}
			}
		}
	}

	return out.ok;
}

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
	Shot& out)
{
	out = Shot{};
	if (!Ready() || seedTick <= 0)
		return false;
	if (!Mem::ValidEntity(weapon) || !Mem::ValidEntity(local) || !Mem::ValidEntity(target))
		return false;
	if (!Bones::IsValidPos(eye))
		return false;
	if (preferHitbox < 0 || preferHitbox >= Config::HB_COUNT)
		preferHitbox = Config::HB_HEAD;

	float inac = 0.f, spr = 0.f;
	if (!HitChance::ReadCurrentBloom(weapon, local, inac, spr))
		return false;

	// No bloom gate - let the solve try. BuildTryCtx tightens capsule acceptance
	// in air (accScale=0.88) to filter rim hits. If solve fails, caller falls back
	// to natural aim (aim at hitbox, normal spread).

	// One punch for whole Solve (all HBs share seed stamp)
	QAngle_t punch{};
	const QAngle_t* punchPtr = nullptr;
	bool punchOk = HitChance::ReadSeedFirePunch(local, weapon, seedTick, tickFrac, punch)
		&& punch.IsValid();
	if (!punchOk)
		punchOk = HitChance::ReadAimPunch(local, punch) && punch.IsValid();
	if (punchOk) {
		punch.z = 0.f;
		punchPtr = &punch;
	}

	// Air/heavy/sniper flags once - reused by every TryView.
	TryCtx ctx{};
	BuildTryCtx(weapon, local, inac, spr, ctx);

	// HB order: prefer first, then core, then limbs (menu filter).
	static constexpr int kOrder[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS,
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};
	int tryHb[Config::HB_COUNT]{};
	int nTry = 0;
	auto pushHb = [&](int hb) {
		if (hb < 0 || hb >= Config::HB_COUNT) return;
		if (enabledHitboxes && !enabledHitboxes[hb]) return;
		for (int i = 0; i < nTry; ++i)
			if (tryHb[i] == hb) return;
		tryHb[nTry++] = hb;
	};
	pushHb(preferHitbox);
	for (int hb : kOrder) {
		// High bloom: skip limb spiral. Thin capsules graze-accept then server miss.
		if (ctx.corePreferGate && !IsCore(hb))
			continue;
		pushHb(hb);
	}
	if (nTry == 0) {
		for (int hb : kOrder)
			pushHb(hb);
	}

	QAngle_t wishBase = wishView;
	wishBase.z = 0.f;
	wishBase.Normalize();

	// Prefer HB first (often hits) - full spiral only on that HB.
	// Other HBs: natural+roll+closed-form only (no bin/spiral) unless prefer failed.
	// Still covers multi-HB; avoids Nx56 spiral FPS death.
	for (int i = 0; i < nTry; ++i) {
		const int hb = tryHb[i];
		Vector_t aimPt{};
		if (!Bones::GetHitboxPoint(target, hb, aimPt) || !Bones::IsValidPos(aimPt))
			aimPt = SeedAimPoint(target, hb, eye);
		else
			aimPt = SeedAimPoint(target, hb, aimPt);
		if (!Bones::IsValidPos(aimPt))
			continue;

		Shot cand{};
		if (SolveForAim(eye, wishBase, aimPt, hb, seedTick, tickFrac, weapon, local,
				target, inac, spr, punchPtr, enabledHitboxes, ctx, cand)
			&& cand.ok && cand.fireAngles.IsValid()) {
			out = cand;
			return true;
		}
		// Prefer missed with full search - secondary HBs still get full path
		// (RayHitsAny inside Accept already multi-HB; aim point is the difference).
	}

	return out.ok;
}

bool SolveNoSpread(
	const Vector_t& eye,
	const QAngle_t& wishView,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Shot& out)
{
	out = Shot{};
	(void)eye;
	if (!Ready() || seedTick <= 0 || !weapon || !wishView.IsValid())
		return false;
	if (!Mem::ValidEntity(weapon))
		return false;

	float inac = 0.f, spr = 0.f;
	if (!HitChance::ReadCurrentBloom(weapon, local, inac, spr))
		return false;

	const float wishYaw = wishView.y;
	QAngle_t wish = wishView;
	wish.Normalize();
	if (!wish.IsValid())
		return false;

	for (int i = 0; i < 720; ++i) {
		QAngle_t test{ static_cast<float>(i) * 0.5f, wishYaw, 0.f };
		test.Normalize();
		if (!test.IsValid())
			continue;

		const std::uint32_t seed = HitChance::ComputeSeed(test, seedTick);

		Vector_t dir{};
		float sx = 1e9f, sy = 1e9f;
		(void)HitChance::GetBulletDirectionCached(
			test, seedTick, weapon, inac, spr, dir, &sx, &sy, 1u, nullptr, true);
		// Dir may fail on pitch bins outside ?89; spread is still valid.
		// Search only needs seed + CalcSpread, not AngleVectors of the test pitch.
		if (!std::isfinite(sx) || !std::isfinite(sy)
			|| std::fabs(sx) > 8.f || std::fabs(sy) > 8.f)
			continue;

		QAngle_t adj = wish;
		adj.x += std::atan(std::sqrt(sx * sx + sy * sy)) * kRad2Deg;
		adj.z = -std::atan2(sx, sy) * kRad2Deg;
		adj.Normalize();
		if (!adj.IsValid())
			continue;

		if (HitChance::ComputeSeed(adj, seedTick) != seed)
			continue;

		out.fireAngles = adj;
		out.seedTick = seedTick;
		out.seedFrac = tickFrac;
		out.sx = sx;
		out.sy = sy;
		out.ok = true;
		return true;
	}
	return false;
}

bool SeedCycleAllowsFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local)
{
	if (!weapon)
		return true;
	const std::uint16_t def = WeaponDef(weapon);
	const std::uint64_t now = NowMs();
	const std::uint64_t gap = SeedMinGapMs(weapon);

	// Soft anti double-frame only. Engine nextTick via CanWeaponFire is real
	// re-arm - old deagle recovery/bloom hold blocked 250-450ms after every shot
	// (and after Solve success) while crosshair already parked on enemy.
	if (def != 0 && g_latch.def == def && g_latch.fireMs != 0
		&& (now - g_latch.fireMs) < gap) {
		// Spray: hard gap. CanWeaponFire stays true until nextAttack writes -
		// same-tick 2nd CM would double-stamp hist (overspray miss).
		if (!AimCommon::IsSprayAutoWeapon(weapon)
			&& local && AimCommon::CanWeaponFire(weapon, local)) {
			g_latch.fireMs = 0;
			return true;
		}
		return false;
	}

	// Past soft gap - clear latch; Solve uses live inac+spr so bloom gate unnecessary
	if (def != 0 && g_latch.def == def && g_latch.fireMs != 0)
		g_latch.fireMs = 0;

	return true;
}

void NoteSeedFired(C_CSWeaponBase* weapon, C_CSPlayerPawn* /*local*/)
{
	if (!weapon)
		return;
	g_latch.def = WeaponDef(weapon);
	g_latch.fireMs = NowMs();
}

Vector_t SeedAimPoint(C_CSPlayerPawn* target, int hitbox, const Vector_t& fallback)
{
	Vector_t pt = fallback;
	if (target && hitbox >= 0 && hitbox < Config::HB_COUNT) {
		Bones::Capsule cap{};
		if (Bones::GetHitboxCapsule(target, hitbox, cap) && cap.ok
			&& Bones::IsValidPos(cap.center))
			pt = cap.center;
		else {
			Vector_t c{};
			if (Bones::GetHitboxPoint(target, hitbox, c) && Bones::IsValidPos(c))
				pt = c;
		}
	}
	if (!target || !Bones::IsValidPos(pt))
		return pt;

	Vector_t vel{};
	__try { vel = target->m_vecAbsVelocity(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return pt; }

	const float sp = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
	if (!std::isfinite(sp) || sp < 30.f)
		return pt;

	// Short lead only - long lead + seed rewrite = overshoot miss on strafe.
	const bool head = (hitbox == Config::HB_HEAD || hitbox == Config::HB_NECK);
	const float leadT = head ? 0.004f : 0.006f;
	Vector_t lead{
		pt.x + vel.x * leadT,
		pt.y + vel.y * leadT,
		pt.z + vel.z * leadT
	};
	const float dx = lead.x - pt.x;
	const float dy = lead.y - pt.y;
	const float h = std::sqrt(dx * dx + dy * dy);
	const float maxH = head ? 3.f : 5.f;
	if (h > maxH && h > 1e-4f) {
		const float s = maxH / h;
		lead.x = pt.x + dx * s;
		lead.y = pt.y + dy * s;
	}
	const float dz = lead.z - pt.z;
	const float maxZ = head ? 2.f : 3.f;
	if (std::fabs(dz) > maxZ)
		lead.z = pt.z + (dz > 0.f ? maxZ : -maxZ);
	return Bones::IsValidPos(lead) ? lead : pt;
}

// Hard mindmg on the REAL pellet hitbox only.
// allowPen must match PELLET wall state (caller re-checks after Solve).
// Never rewrite HB (old feet->head under mindmg 100).
bool SeedPassesDamage(
	const Vector_t& eye,
	Vector_t& inOutPoint,
	int& inOutHb,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamageVis,
	float minDamageAw)
{
	if (!target || !Mem::ValidEntity(target) || !Bones::IsValidPos(eye)
		|| !Bones::IsValidPos(inOutPoint) || inOutHb < 0)
		return false;

	auto needFor = [&](float cfgNeed) -> float {
		if (cfgNeed <= 0.f)
			return 0.f;
		float need = cfgNeed;
		const int hp = target->m_iHealth();
		if (hp > 0 && static_cast<float>(hp) < need)
			need = static_cast<float>(hp);
		return need;
	};

	auto dmgOk = [&](const Vector_t& pt, int hb) -> bool {
		if (!Bones::IsValidPos(pt) || hb < 0)
			return false;
		if (allowPen) {
			const AutoWall::Result aw = AutoWall::Fire(
				eye, pt, hb, weapon, local, target, true);
			// Wall path: must real-pen + hit. mindmg_aw=0 -> any pen dmg >= 1.
			// Free-LOS with allowPen=true (stale flag) -> !penetrated -> reject.
			if (!aw.hit || !aw.penetrated || aw.damage < 1.f)
				return false;
			const float need = needFor(minDamageAw);
			if (need > 0.f && aw.damage + 0.01f < need)
				return false;
			return true;
		}
		// Visible: allowPen=false -> wall returns !hit (fail closed).
		const AutoWall::Result vis = AutoWall::Fire(
			eye, pt, hb, weapon, local, target, false);
		if (!vis.hit || vis.damage < 1.f)
			return false;
		const float need = needFor(minDamageVis);
		if (need > 0.f && vis.damage + 0.01f < need)
			return false;
		return true;
	};

	// 1) Exact Solve pellet point + HB
	if (dmgOk(inOutPoint, inOutHb))
		return true;

	// 2) Same HB capsule center only (rim under-estimate) - NOT other HBs
	const Vector_t center = SeedAimPoint(target, inOutHb, inOutPoint);
	if (Bones::IsValidPos(center) && center.Distance(inOutPoint) > 0.5f
		&& dmgOk(center, inOutHb)) {
		inOutPoint = center;
		return true;
	}

	return false;
}

bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX,
	float* outSpreadY,
	unsigned seedAdd,
	float tickFrac)
{
	return HitChance::GetBulletDirection(
		fireAngles, seedTick, weapon, local, outDir, outSpreadX, outSpreadY,
		seedAdd, tickFrac);
}

bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint,
	float tickFrac)
{
	return HitChance::ExactShotHits(
		eye, fireAngles, seedTick, weapon, local, target, hitbox, outPoint, tickFrac);
}

bool ExactShotHitsAny(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	const bool* enabledHitboxes,
	int* outHitbox,
	Vector_t* outPoint,
	float tickFrac)
{
	return HitChance::ExactShotHitsAny(
		eye, fireAngles, seedTick, weapon, local, target, enabledHitboxes,
		outHitbox, outPoint, tickFrac);
}

std::uint32_t ComputeSeed(const QAngle_t& angles, int attackTick)
{
	return HitChance::ComputeSeed(angles, attackTick);
}

namespace SeedDbg {

const char* HbName(int hb)
{
	switch (hb) {
	case Config::HB_HEAD: return "head";
	case Config::HB_NECK: return "neck";
	case Config::HB_CHEST: return "chest";
	case Config::HB_STOMACH: return "stomach";
	case Config::HB_PELVIS: return "pelvis";
	case Config::HB_ARMS: return "arms";
	case Config::HB_LEGS: return "legs";
	case Config::HB_FEET: return "feet";
	default: return "?";
	}
}

const char* WpnTag(int def)
{
	switch (def) {
	case 1: return "deagle";
	case 7: return "ak47";
	case 9: return "awp";
	case 16: return "m4a4";
	case 40: return "ssg08";
	case 60: return "m4a1";
	case 61: return "usp";
	case 64: return "r8";
	default: return "gun";
	}
}

void Log(const Snap& s, unsigned intervalMs)
{
#ifdef _DEBUG
	char key[72]{};
	std::snprintf(key, sizeof(key), "seed.%s.%s.%s",
		s.who ? s.who : "?",
		s.event ? s.event : "?",
		s.reason && s.reason[0] ? s.reason : "-");

	static struct {
		char key[72];
		DWORD last;
	} s_rate[48]{};
	const DWORD now = GetTickCount();
	const DWORD iv = intervalMs ? intervalMs : 150u;
	bool allow = true;
	int freeSlot = -1;
	for (int i = 0; i < 48; ++i) {
		if (s_rate[i].key[0] == '\0') {
			if (freeSlot < 0) freeSlot = i;
			continue;
		}
		if (std::strncmp(s_rate[i].key, key, 71) == 0) {
			if (now - s_rate[i].last < iv)
				allow = false;
			else
				s_rate[i].last = now;
			freeSlot = -2;
			break;
		}
	}
	if (freeSlot >= 0 && allow) {
		std::snprintf(s_rate[freeSlot].key, 72, "%s", key);
		s_rate[freeSlot].last = now;
	}
	if (!allow)
		return;

	int def = s.def;
	if (!def && s.weapon) {
		__try { def = s.weapon->m_iItemDefinitionIndex(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { def = 0; }
	}

	Con::Info("[seed] %s %s %s  %s  path=%s",
		s.who ? s.who : "?",
		s.event ? s.event : "?",
		s.reason && s.reason[0] ? s.reason : "-",
		WpnTag(def),
		s.path && s.path[0] ? s.path : "-");
	if (s.event && (s.event[0] == 'F' || s.event[0] == 'T')) {
		Con::Detail("angles", "wish=(%.2f, %.2f) fire=(%.2f, %.2f) dAng=%.2f",
			s.wish.x, s.wish.y, s.fire.x, s.fire.y, s.dAng);
		Con::Detail("hit", "prefer=%s hit=%s dAim=%.1f sx=%.4f sy=%.4f",
			HbName(s.preferHb), HbName(s.hitHb), s.dAim, s.sx, s.sy);
	}
#else
	(void)s;
	(void)intervalMs;
#endif
}

} // namespace SeedDbg

} // namespace NoSpread

