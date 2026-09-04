#include "nadepred.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../trace/trace.h"
#include "../bones/bones.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/cvar/cvar.h"
#include "../aim/aim_common.h"
#include "../../../../external/imgui/imgui.h"

#include <cstring>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cmath>

// Source 2 grenade physics - initial conditions reverse-engineered from
// client.dll cl_sim_grenade_trajectory path:
//   CBaseCSGrenade::GetThrowParams (sub_1807A07B0):
//     speed   = (clamp01(strength) * 0.7 + 0.3) * min(750, throwVel * 0.9), floor 15
//     pitch'  = normPitch - (90 - |normPitch|) * 10 / 90        (lob bias)
//     origin.z += strength * 12 - 12                            (underhand drop)
//     vel     = dir(pitch', yaw) * speed + localAbsVel * 1.25
//   Preview spawner (sub_1810452E0 / sub_181044530): CGrenadeTracer ticked at
//   dt = 1/64 (off_1820B05F0[13] = 0.015625), spawn origin = clip(origin + fwd*16).
// Flight sim stays userland, matched to the engine tracer (IDA sub_18104AEF0):
// per 1/64 tick the engine runs TWO physics substeps at dt = 1/128
// (interval_per_tick forced to 0.0078125 inside the move). Simulating at 1/64
// quantized bounce contact points up to half a tick off - arcs diverged at
// every wall/roof edge. Gravity sv_gravity*0.4 = 320, grenade sphere r=2,
// hull-trace collision MASK_GRENADE.
namespace NadePred {
namespace {

constexpr float kSimDt = 1.0f / 128.0f;          // engine substep: 2 per 1/64 tick
constexpr int   kMaxTicks = 2048;                // 16 s of flight at 128 Hz
constexpr int   kTicksPerPoint = 4;              // one arc point per 1/32 s
constexpr float kGravity = 320.0f;               // sv_gravity 800 * gravityScale 0.4 (fallback, dynamic in Simulate)
constexpr float kDefaultElasticity = 0.45f;      // projectile bounce dampen
constexpr float kMolotovSlopeFallbackDeg = 90.0f; // weapon_molotov_maxdetonateslope default (IDA registration: xmm1=90.0, min 30.0 - ANGLE in degrees)
constexpr float kMolotovFuseFallback = 2.0f;     // molotov_throw_detonate_time default (IDA registration dword_18198A3DC)
constexpr float kSteepBounceNormalZ = 0.7f;
constexpr float kSteepBounceSpeedSq = 96000.0f;
constexpr float kHullRadius = 2.0f;              // CBaseCSGrenadeProjectile collision sphere
constexpr int   kBounceLimit = 20;
constexpr int   kSmokeCheckTicks = 12;           // engine deploy Think at 64 Hz, samples every ~0.2s
constexpr float kInheritVelocityScale = 1.25f;   // run/jump throw velocity inheritance
constexpr float kForceResimFrames = 20;
constexpr float kSpawnPushoutDist = 16.0f;       // origin pushed out along dir before sim

inline bool IsFiniteVec(const Vector_t& v) {
	return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

inline const Vector_t& ZeroVec() {
	static constexpr Vector_t kZero(0.0f, 0.0f, 0.0f);
	return kZero;
}

Vector_t ClipVelocity(const Vector_t& vel, const Vector_t& n, float overbounce) {
	const float backoff = vel.x * n.x + vel.y * n.y + vel.z * n.z;
	Vector_t out{
		vel.x - n.x * backoff * overbounce,
		vel.y - n.y * backoff * overbounce,
		vel.z - n.z * backoff * overbounce,
	};
	if (fabsf(out.x) < 0.1f) out.x = 0.0f;
	if (fabsf(out.y) < 0.1f) out.y = 0.0f;
	if (fabsf(out.z) < 0.1f) out.z = 0.0f;
	return out;
}

// Engine compares impact normal.z against cos(deg2rad(cvar)) - the cvar is an
// angle in degrees, NOT a 0..1 slope (IDA sub_18104BA00, current build).
float MolotovDetonateSlopeNormalZ() {
	float deg = 0.0f;
	__try {
		deg = Cvar::Float("weapon_molotov_maxdetonateslope", kMolotovSlopeFallbackDeg);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		deg = kMolotovSlopeFallbackDeg;
	}
	if (!isfinite(deg))
		deg = kMolotovSlopeFallbackDeg;
	deg = std::clamp(deg, 0.0f, 90.0f);
	return cosf(deg * 3.14159265358979323846f / 180.0f);
}

bool ShouldDetonate(Kind kind, const Vector_t& vel, int tick, float molotovFuse) {
	switch (kind) {
	case Kind::Smoke:
	case Kind::Decoy: {
		// Deploy Think runs per 1/64 tick, not per substep - sample only on
		// even substeps (64 Hz) with the same 12-sample spacing.
		if (tick & 1)
			return false;
		const int t64 = tick >> 1;
		const float threshold = (kind == Kind::Smoke) ? 0.1f : 0.2f;
		const float speed2d = sqrtf(vel.x * vel.x + vel.y * vel.y);
		return speed2d < threshold && (t64 % kSmokeCheckTicks) == 0;
	}
	case Kind::Molotov:
	case Kind::Incendiary:
		// IDA tick loop (sub_1810452E0): type-2 dwell = the convar
		// molotov_throw_detonate_time (default 2.0) - plus the loop's uniform
		// 0.125 slack. Impact/rest detonation is separate (ResolveCollision).
		return static_cast<float>(tick) * kSimDt > molotovFuse;
	case Kind::Flash:
	case Kind::HE:
		// 1.5 s fuse + 0.125 s slack (the loop's uniform slack).
		return static_cast<float>(tick - 16) * kSimDt > 1.5f;
	default:
		return false;
	}
}

// Hull trace first (real engine collision vs grenade sphere); line trace as
// fallback when the hull path fails to resolve.
bool GrenadeTrace(const Vector_t& start, const Vector_t& end, void* skip, Trace::CGameTrace& tr) {
	const Vector_t mins(-kHullRadius, -kHullRadius, -kHullRadius);
	const Vector_t maxs(kHullRadius, kHullRadius, kHullRadius);
	if (Trace::TraceHull(start, end, mins, maxs, skip, tr,
			Trace::kMaskGrenade, Trace::kFilterLayerGrenade, Trace::kFilterA5Grenade))
		return true;
	return Trace::TraceLine(start, end, skip, tr,
		Trace::kMaskGrenade, Trace::kFilterLayerGrenade, Trace::kFilterA5Grenade);
}

// Reflect + dampen. Returns true when the projectile detonates on this impact.
// Exact mirror of the engine tracer's shared ResolveFlyCollisionCustom
// (IDA sub_18104BA00, current build) for ALL grenade types:
//   1. clip(overbounce 2.0)
//   2. velocity *= projectile elasticity (proj+1340; NO surface-elasticity or
//      friction read anywhere in this path - GetSurfaceData is never called)
//   3. steep dampen: n.z >= 0.7 && post-clip speedSq > 96000 &&
//      dot(normalize(v), n) > 0.5 -> v *= 1.5 - dot
//   4. molotov detonate: n.z >= cos(deg2rad(weapon_molotov_maxdetonateslope))
//      OR speedSq < 400; any type: speedSq < 400 -> rest/stop
bool ResolveCollision(const Trace::CGameTrace& tr, Vector_t& pos, Vector_t& vel, Kind kind, void* skip) {
	const Vector_t n = tr.normal();
	const bool molotovFamily = (kind == Kind::Molotov || kind == Kind::Incendiary);

	// 1. Clip (engine passes overbounce 2.0).
	Vector_t newVel = ClipVelocity(vel, n, 2.0f);

	// 2. Projectile elasticity - uniform, no surface-properties read (engine
	//    multiplies the projectile's own elasticity member only).
	newVel = newVel * kDefaultElasticity;

	// 3. speedSq AFTER clip+elasticity - the engine's detonate/steep gates
	//    use this (|clip*elast + baseVel|?; baseVel is zero in flight).
	const float speedSq = newVel.x * newVel.x + newVel.y * newVel.y + newVel.z * newVel.z;

	// Steep-bounce dampen (engine gate on the same post-clip speedSq).
	if (n.z >= kSteepBounceNormalZ && speedSq > kSteepBounceSpeedSq) {
		const float len = sqrtf(speedSq);
		if (len > 0.0001f) {
			const float dot = (newVel.x * n.x + newVel.y * n.y + newVel.z * n.z) / len;
			if (dot > 0.5f)
				newVel = newVel * (1.5f - dot);
		}
	}

	// 4. Detonate / rest:
	//    - molotov family: surface slope gate (cos of the cvar, default 90? =
	//      any impact) OR slow (< 20 u/s post-clip)
	//    - every type: slow -> velocity zeroed (tracer rests, loop ends)
	if (molotovFamily && n.z >= MolotovDetonateSlopeNormalZ()) {
		vel = ZeroVec();
		return true;
	}
	if (speedSq < 400.0f) {
		vel = ZeroVec();
		return molotovFamily; // molotov: rest = detonate; others: just stop
	}

	vel = newVel;

	// Consume remainder of tick after bounce - re-clip if the post trace also
	// hits (multi-bounce same tick), same damp rule as above.
	const float remaining = 1.0f - tr.fraction();
	if (remaining > 0.0f) {
		Vector_t curPos = pos;
		Vector_t curVel = newVel;
		float curRem = remaining;
		for (int extra = 0; extra < 2 && curRem > 0.001f; ++extra) {
			const Vector_t postEnd = curPos + curVel * (curRem * kSimDt);
			Trace::CGameTrace post{};
			if (!GrenadeTrace(curPos, postEnd, skip, post)) { curPos = postEnd; break; }
			curPos = post.endpos();
			if (!Trace::DidHit(post)) break;
			Vector_t postVel = ClipVelocity(curVel, post.normal(), 2.0f) * kDefaultElasticity;
			const float postSpeedSq = postVel.x * postVel.x + postVel.y * postVel.y + postVel.z * postVel.z;
			if (postSpeedSq < 400.0f) { curVel = ZeroVec(); break; }
			curVel = postVel;
			curRem *= (1.0f - post.fraction());
			if (curRem <= 0.001f) break;
		}
		pos = curPos;
		vel = curVel;
	}
	return false;
}

} // namespace

// Reverse-engineered 1:1 from CBaseCSGrenade throw-param build (client.dll
// sub_1807A07B0 via cl_sim_grenade_trajectory callback chain).
Vector_t ComputeThrowVelocity(const QAngle_t& viewAngles, float throwStrength, float baseVelocity, const Vector_t& inheritVelocity) {
	// Strength: mid-band snaps to exactly 0.5 (underhand/overhand jump), then clamp.
	float s = throwStrength;
	if (fabsf(s - 0.5f) <= 0.1f)
		s = 0.5f;
	if (!isfinite(s))
		s = 1.0f;
	s = std::clamp(s, 0.0f, 1.0f);

	// Pitch: normalize into [-90, 90] by wrapping around, then apply the lob
	// bias used for all grenades: pitch' = p - (90 - |p|) * 10/90.
	float pitch = viewAngles.x;
	while (pitch > 90.0f) pitch -= 360.0f;
	while (pitch < -90.0f) pitch += 360.0f;
	pitch -= (90.0f - fabsf(pitch)) * 10.0f / 90.0f;

	const float yawRad = viewAngles.y * 3.14159265358979323846f / 180.0f;
	const float pitchRad = pitch * 3.14159265358979323846f / 180.0f;
	const float cp = cosf(pitchRad);
	Vector_t fwd{ cp * cosf(yawRad), cp * sinf(yawRad), -sinf(pitchRad) };

	// Speed: 15 floor, capped at 750, scaled off the weapon's own velocity.
	float base = 15.0f;
	const float scaled = baseVelocity * 0.9f;
	if (scaled >= 15.0f)
		base = (std::min)(750.0f, scaled);
	const float speed = (s * 0.7f + 0.3f) * base;

	return fwd * speed + inheritVelocity * kInheritVelocityScale;
}

// Underhand origin drop: thrown origin sits (strength * 12 - 12) below eye height.
Vector_t ApplyThrowOriginShift(const Vector_t& origin, float throwStrength) {
	float s = throwStrength;
	if (fabsf(s - 0.5f) <= 0.1f)
		s = 0.5f;
	s = std::clamp(isfinite(s) ? s : 1.0f, 0.0f, 1.0f);
	return Vector_t{ origin.x, origin.y, origin.z + (s * 12.0f - 12.0f) };
}

// Engine pushes the spawn point out along the throw direction (~16 units) and
// clips it against the world so the preview never starts inside geometry.
void PushOutThrowOrigin(Vector_t& origin, const Vector_t& dir, void* skip) {
	const Vector_t candidate{
		origin.x + dir.x * kSpawnPushoutDist,
		origin.y + dir.y * kSpawnPushoutDist,
		origin.z + dir.z * kSpawnPushoutDist,
	};
	Trace::CGameTrace tr{};
	if (GrenadeTrace(origin, candidate, skip, tr)) {
		if (Trace::DidHit(tr)) {
			const Vector_t clipped = tr.endpos();
			if (IsFiniteVec(clipped) && Bones::IsValidPos(clipped)) {
				origin = clipped;
				return;
			}
		}
	}
	if (Bones::IsValidPos(candidate))
		origin = candidate;
}

bool Simulate(const Vector_t& start, const Vector_t& vel, Kind kind, void* skip, Result& out) {
	out.Reset();
	if (kind == Kind::None || !IsFiniteVec(start) || !IsFiniteVec(vel))
		return false;

	Vector_t pos = start;
	Vector_t velCur = vel;
	int bounceCount = 0;
	int stride = 0;

	// Half-step Verlet - IDA: grenade gravity = sv_gravity * 0.4 (320 @ 800).
	// Resolved once: per-tick convar reads are pure overhead and the value
	// could flip mid-flight on a map transition.
	const float dynGravity = Cvar::Float("sv_gravity", 800.f) * 0.4f;
	// molotov_throw_detonate_time + the tick loop's uniform 0.125 slack.
	float molotovFuse = Cvar::Float("molotov_throw_detonate_time", kMolotovFuseFallback) + 0.125f;
	if (!isfinite(molotovFuse) || molotovFuse < 0.25f)
		molotovFuse = kMolotovFuseFallback + 0.125f;

	int tick = 0;
	for (; tick < kMaxTicks; ++tick) {
		if (stride == 0 && out.pointCount < Result::kMaxPoints)
			out.points[out.pointCount++] = pos;

		const float newVz = velCur.z - dynGravity * kSimDt;
		const Vector_t move(velCur.x * kSimDt, velCur.y * kSimDt, (velCur.z + newVz) * 0.5f * kSimDt);
		velCur.z = newVz;

		const Vector_t traceEnd = pos + move;
		Trace::CGameTrace tr{};
		if (!GrenadeTrace(pos, traceEnd, skip, tr)) {
			out.Reset();
			return false;
		}
		pos = tr.endpos();

		bool impactDetonate = false;
		const bool hit = Trace::DidHit(tr);
		if (hit) {
			++bounceCount;
			if (out.bounceCount < Result::kMaxBounces)
				out.bounces[out.bounceCount++] = pos;
			impactDetonate = ResolveCollision(tr, pos, velCur, kind, skip);
		}

		if (impactDetonate || ShouldDetonate(kind, velCur, tick, molotovFuse) || bounceCount > kBounceLimit)
			break;

		if (hit && (velCur.x == 0.0f && velCur.y == 0.0f && velCur.z == 0.0f))
			break;

		stride = (hit || ++stride >= kTicksPerPoint) ? 0 : stride;
	}

	out.endPos = pos;
	out.valid = (out.pointCount > 0);
	out.fuse = static_cast<float>(tick) * kSimDt;

	// Tail-snap so the polyline visibly ends at the impact site.
	if (out.pointCount > 0 && out.pointCount < Result::kMaxPoints) {
		const Vector_t& last = out.points[out.pointCount - 1];
		const Vector_t d = last - out.endPos;
		if (d.Length() > 1.0f)
			out.points[out.pointCount++] = out.endPos;
	}
	return true;
}

namespace {

Kind HeldKind(C_CSWeaponBase* wep) {
	if (!wep)
		return Kind::None;
	__try {
		const std::uint16_t def = wep->m_iItemDefinitionIndex();
		switch (def) {
		case 43: return Kind::Flash;
		case 44: return Kind::HE;
		case 45: return Kind::Smoke;
		case 46: return Kind::Molotov;
		case 47: return Kind::Decoy;
		case 48: return Kind::Incendiary;
		default: return Kind::None;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return Kind::None;
	}
}

struct SimCache {
	QAngle_t ang{};
	Vector_t eye{};
	Vector_t absVel{};
	float strength = -1.0f;
	float throwVel = -1.0f;
	Kind kind = Kind::None;
	Result res;
	int idleFrames = 0;
};
SimCache g_cache;

bool InputsChanged(const SimCache& c) {
	if (c.kind != g_cache.kind) return true;
	if (fabsf(c.strength - g_cache.strength) > 0.005f) return true;
	if (fabsf(c.throwVel - g_cache.throwVel) > 0.5f) return true;
	const QAngle_t da = c.ang - g_cache.ang;
	if (fabsf(da.x) > 0.05f || fabsf(da.y) > 0.05f) return true;
	const Vector_t de = c.eye - g_cache.eye;
	if (de.Length() > 0.5f) return true;
	const Vector_t dv = c.absVel - g_cache.absVel;
	if (dv.Length() > 5.0f) return true;
	return false;
}

Vector_t GetLocalAbsVelocitySeh(C_CSPlayerPawn* local) {
	Vector_t v(0.0f, 0.0f, 0.0f);
	__try {
		v = local->m_vecAbsVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		v = ZeroVec();
	}
	if (!IsFiniteVec(v))
		v = ZeroVec();
	return v;
}

float GetWeaponThrowStrengthSeh(C_CSWeaponBase* wep) {
	float s = 1.0f;
	__try {
		s = wep->m_flThrowStrength();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		s = 1.0f;
	}
	if (!isfinite(s))
		s = 1.0f; // std::clamp(NaN, 0, 1) returns NaN - sanitize before clamping
	s = std::clamp(s, 0.0f, 1.0f);
	return s;
}

float GetWeaponThrowVelocitySeh(C_CSWeaponBase* wep, Kind kind) {
	float tv = -1.0f;
	__try {
		if (CCSWeaponBaseVData* vd = wep->Data())
			tv = vd->m_flThrowVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		tv = -1.0f;
	}
	if (!isfinite(tv) || tv <= 0.0f)
		tv = (kind == Kind::Molotov || kind == Kind::Incendiary) ? 700.0f : 750.0f;
	return tv;
}

static uint32_t g_stashedPosOffset = 0;
static uint32_t g_stashedBoolOffset = 0;

bool ReadStashedThrowPosSeh(C_CSPlayerPawn* local, Vector_t& out) {
	if (!g_stashedPosOffset)
		g_stashedPosOffset = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_vecStashedGrenadeThrowPosition"));
	if (!g_stashedBoolOffset)
		g_stashedBoolOffset = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_bGrenadeParametersStashed"));

	if (!g_stashedPosOffset || !g_stashedBoolOffset)
		return false;

	__try {
		const bool isStashed = *reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(local) + g_stashedBoolOffset);
		if (isStashed) {
			out = *reinterpret_cast<const Vector_t*>(reinterpret_cast<const uint8_t*>(local) + g_stashedPosOffset);
			return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return false;
}

Vector_t GetThrowOriginSeh(C_CSPlayerPawn* local) {
	Vector_t eye{};
	if (ReadStashedThrowPosSeh(local, eye) && Bones::IsValidPos(eye)) {
		return eye;
	}
	return Bones::GetEyePos(local);
}

void UpdateCache(C_CSPlayerPawn* local, C_CSWeaponBase* wep) {
	SimCache next{};
	next.kind = HeldKind(wep);
	if (next.kind == Kind::None) {
		g_cache = SimCache{};
		return;
	}

	QAngle_t ang{};
	if (!AimCommon::GetViewAngles(ang) || !ang.IsValid())
		return;
	next.ang = { ang.x, ang.y, 0.0f };

	const Vector_t eye = GetThrowOriginSeh(local);
	if (!Bones::IsValidPos(eye))
		return;
	next.absVel = GetLocalAbsVelocitySeh(local);
	next.strength = GetWeaponThrowStrengthSeh(wep);
	next.throwVel = GetWeaponThrowVelocitySeh(wep, next.kind);
	next.eye = ApplyThrowOriginShift(eye, next.strength);

	// Direction mirrors the flight path, so derive it after the pitch warp
	// the same way ComputeThrowVelocity does internally.
	float pitch = next.ang.x;
	while (pitch > 90.0f) pitch -= 360.0f;
	while (pitch < -90.0f) pitch += 360.0f;
	pitch -= (90.0f - fabsf(pitch)) * 10.0f / 90.0f;
	const float yawRad = next.ang.y * 3.14159265358979323846f / 180.0f;
	const float pitchRad = pitch * 3.14159265358979323846f / 180.0f;
	const float cp = cosf(pitchRad);
	Vector_t dir{ cp * cosf(yawRad), cp * sinf(yawRad), -sinf(pitchRad) };

	// Push out BEFORE the change test - the cached eye is post-pushout, so the
	// comparison must see the same value or the cache misses every frame.
	PushOutThrowOrigin(next.eye, dir, local);

	if (InputsChanged(next) || g_cache.idleFrames >= static_cast<int>(kForceResimFrames)) {
		g_cache = next;
		g_cache.idleFrames = 0;

		const Vector_t v0 = ComputeThrowVelocity(g_cache.ang, g_cache.strength, g_cache.throwVel, g_cache.absVel);
		Simulate(g_cache.eye, v0, g_cache.kind, local, g_cache.res);
	} else {
		++g_cache.idleFrames;
	}
}

ImU32 ColorU32(float alphaMul) {
	const ImVec4& c = Config::nadepred_color;
	const auto ch = [&](float v) { return static_cast<ImU32>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
	return IM_COL32(ch(c.x), ch(c.y), ch(c.z), ch(c.w * alphaMul));
}

// minimal = flight line + landing mark only (thrown projectiles): no bounce
// dots. Labels are drawn separately by the in-air path for rested utility.
void DrawResult(ImDrawList* dl, const ViewMatrix& vm, const Result& res, float alphaMul, bool minimal = false) {
	if (!res.valid || res.pointCount < 2)
		return;

	const ImU32 colMain = ColorU32(alphaMul);
	const ImU32 colShadow = IM_COL32(0, 0, 0, static_cast<int>(110 * alphaMul));
	const ImU32 colDotFill = IM_COL32(12, 14, 18, static_cast<int>(200 * alphaMul));

	Vector_t sp{};
	ImVec2 prev{};
	bool havePrev = vm.WorldToScreen(res.points[0], sp);
	if (havePrev)
		prev = { sp.x, sp.y };

	for (int i = 1; i < res.pointCount; ++i) {
		Vector_t cur{};
		const bool ok = vm.WorldToScreen(res.points[i], cur);
		if (ok && havePrev) {
			const ImVec2 c0{ cur.x, cur.y };
			dl->AddLine(prev, c0, colShadow, 3.2f);
			dl->AddLine(prev, c0, colMain, 1.6f);
			prev = c0;
		} else {
			havePrev = ok;
			if (ok)
				prev = { cur.x, cur.y };
		}
	}

	if (Config::nadepred_show_bounces && !minimal) {
		const ImU32 colBounce = ColorU32(0.85f * alphaMul);
		for (int i = 0; i < res.bounceCount; ++i) {
			Vector_t s{};
			if (!vm.WorldToScreen(res.bounces[i], s))
				continue;
			const ImVec2 p{ s.x, s.y };
			dl->AddCircleFilled(p, 3.6f, colShadow, 12);
			dl->AddCircleFilled(p, 2.6f, colDotFill, 12);
			dl->AddCircle(p, 2.6f, colBounce, 12, 1.1f);
		}
	}

	Vector_t se{};
	if (vm.WorldToScreen(res.endPos, se)) {
		const ImVec2 p{ se.x, se.y };
		const ImU32 colLand = ColorU32(alphaMul);
		dl->AddCircleFilled(p, 7.5f, colShadow, 24);
		dl->AddCircleFilled(p, 6.5f, colDotFill, 24);
		dl->AddCircle(p, 6.5f, colLand, 24, 1.5f);
		dl->AddCircleFilled(p, 1.8f, colLand, 10);
		constexpr float t = 11.0f;
		constexpr float g = 3.5f;
		dl->AddLine(ImVec2(p.x - t, p.y), ImVec2(p.x - g, p.y), colLand, 1.3f);
		dl->AddLine(ImVec2(p.x + g, p.y), ImVec2(p.x + t, p.y), colLand, 1.3f);
		dl->AddLine(ImVec2(p.x, p.y - t), ImVec2(p.x, p.y - g), colLand, 1.3f);
		dl->AddLine(ImVec2(p.x, p.y + g), ImVec2(p.x, p.y + t), colLand, 1.3f);
	}
}

// -- In-air trajectory: live arc ahead of already-thrown projectiles ---------

// Tracked designer names (OnAddEntity-fed world list - no entity walking).
Kind DesignerKind(const char* d) {
	if (!d || !d[0])
		return Kind::None;
	if (!_stricmp(d, "hegrenade_projectile")) return Kind::HE;
	if (!_stricmp(d, "flashbang_projectile")) return Kind::Flash;
	if (!_stricmp(d, "smokegrenade_projectile")) return Kind::Smoke;
	if (!_stricmp(d, "molotov_projectile")) return Kind::Molotov;
	if (!_stricmp(d, "incgrenade_projectile")) return Kind::Incendiary;
	if (!_stricmp(d, "decoy_projectile")) return Kind::Decoy;
	return Kind::None;
}

bool SameVec(const Vector_t& a, const Vector_t& b) {
	constexpr float eps = 0.001f;
	return fabsf(a.x - b.x) < eps && fabsf(a.y - b.y) < eps && fabsf(a.z - b.z) < eps;
}

Vector_t ReadEntityVecSeh(C_BaseEntity* e, uint32_t off) {
	Vector_t v(0.0f, 0.0f, 0.0f);
	if (!e || !off || !Mem::IsUserPtr(e))
		return v;
	__try {
		v = *reinterpret_cast<const Vector_t*>(reinterpret_cast<const uint8_t*>(e) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		v = Vector_t(0.0f, 0.0f, 0.0f);
	}
	return IsFiniteVec(v) ? v : Vector_t(0.0f, 0.0f, 0.0f);
}

struct AirEntry {
	int idx = 0;
	Kind kind = Kind::None;
	Vector_t pos{};
	Vector_t vel{};
	Result res;
	std::uint64_t restMs = 0;      // first frame the entity stopped moving (0 = moving)
	std::uint64_t firstSeenMs = 0; // wallclock when first tracked (? throw time)
	// Position history - rest detection + velocity fallback. m_vecAbsVelocity
	// is not replicated for remote projectiles (reads zero), so "is it still"
	// must come from the networked position, not the velocity prop.
	Vector_t lastPos{};
	std::uint64_t posMs = 0;
	bool havePos = false;
};

// Effect duration (s) for utility that keeps existing after the projectile
// rests. 0 = pops on rest (HE/flash).
float KindEffectFull(Kind k) {
	switch (k) {
	case Kind::Smoke: return 18.f;
	case Kind::Decoy: return 15.f;
	case Kind::Molotov:
	case Kind::Incendiary: return 7.f;
	default: return 0.f;
	}
}

// Absolute fuses from the throw (Simulate's constants + engine slack). The
// in-flight sim restarts every tick, so its fuse output freezes at the full
// value for time-gated kinds - those must count from the tracked spawn.
constexpr float kHeFlashFuseTotal = 1.625f; // 1.5 s fuse + 0.125 slack
constexpr float kMolotovFuseTotal = 2.125f; // molotov_throw_detonate_time + slack

// Per-frame resim only when the projectile actually moved; entries vanish as
// projectiles detonate (world list remove) or stop moving (rest -> ~zero arc).
constexpr int kMaxAirEntries = 16;
AirEntry g_air[kMaxAirEntries];
int g_airN = 0;

// Ghost timers - the engine deletes the projectile entity exactly when the
// effect starts (smoke deploy / decoy deploy / molotov impact). A vanished
// burn entity keeps its countdown here, anchored at its last known position,
// so the timer survives the removal and runs to the actual effect end.
struct GhostTimer {
	bool active = false;
	Kind kind = Kind::None;
	Vector_t pos{};
	std::uint64_t stampMs = 0;
	std::uint32_t mapGen = 0;
};
constexpr int kMaxGhosts = 8;
GhostTimer g_ghosts[kMaxGhosts];
int g_ghostWrite = 0;

const char* KindLabel(Kind k) {
	switch (k) {
	case Kind::Flash: return "FLASHBANG";
	case Kind::HE: return "HE GRENADE";
	case Kind::Smoke: return "SMOKE";
	case Kind::Molotov: return "MOLOTOV";
	case Kind::Incendiary: return "INCENDIARY";
	case Kind::Decoy: return "DECOY";
	default: return "";
	}
}

// Shared utility label - fuseSec < 0 hides the timer (name + distance only).
void DrawUtilityLabel(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& worldPos,
	const Vector_t& localPos, Kind kind, float fuseSec) {
	if (!Config::nadepred_air_labels)
		return;
	Vector_t sp{};
	if (!vm.WorldToScreen(worldPos, sp))
		return;
	char buf[48]{};
	if (fuseSec >= 0.f)
		snprintf(buf, sizeof(buf), "%s  %.0fm  T-%.1fs", KindLabel(kind),
			(worldPos - localPos).Length() * 0.0254f, fuseSec);
	else
		snprintf(buf, sizeof(buf), "%s  %.0fm", KindLabel(kind),
			(worldPos - localPos).Length() * 0.0254f);
	const ImVec2 tsz = ImGui::CalcTextSize(buf);
	const ImVec2 tp{ sp.x - tsz.x * 0.5f, sp.y - tsz.y - 12.f };
	dl->AddRectFilled(ImVec2(tp.x - 4.f, tp.y - 2.f),
		ImVec2(tp.x + tsz.x + 4.f, tp.y + tsz.y + 2.f),
		IM_COL32(10, 12, 16, 170), 3.f);
	dl->AddText(tp, IM_COL32(232, 238, 246, 240), buf);
}

void DrawInAir(ImDrawList* dl, const ViewMatrix& vm, C_CSPlayerPawn* local) {
	if (!I::GameEntity || !I::GameEntity->Instance) {
		g_airN = 0;
		return;
	}

	static uint32_t s_absVelOff = 0;
	if (!s_absVelOff)
		s_absVelOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_vecAbsVelocity"));

	// Local thrower: accept the pawn itself or its controller (projectile
	// owner is thrower-dependent across builds).
	CCSPlayerController* localCtrl = nullptr;
	__try {
		const CBaseHandle hc = local->m_hController();
		if (hc.valid())
			localCtrl = I::GameEntity->Instance->Get<CCSPlayerController>(hc);
	} __except (EXCEPTION_EXECUTE_HANDLER) { localCtrl = nullptr; }

	Vector_t localPos(0.0f, 0.0f, 0.0f);
	__try {
		if (CGameSceneNode* nd = local->m_pGameSceneNode(); nd && Mem::IsUserPtr(nd))
			localPos = nd->m_vecAbsOrigin();
	} __except (EXCEPTION_EXECUTE_HANDLER) { localPos = Vector_t(0.0f, 0.0f, 0.0f); }

	int idxs[SdkPrioA::kMaxTrackedWorld];
	const int n = SdkPrioA::CopyWorldIndices(idxs, SdkPrioA::kMaxTrackedWorld);

	AirEntry next[kMaxAirEntries];
	int nextN = 0;

	for (int k = 0; k < n && nextN < kMaxAirEntries; ++k) {
		if (idxs[k] <= 0)
			continue;
		C_BaseEntity* e = I::GameEntity->Instance->Get<C_BaseEntity>(idxs[k]);
		if (!e || !Mem::ValidEntity(e))
			continue;
		char dn[64]{};
		if (!Mem::DesignerName(e, dn, sizeof(dn)) || !dn[0])
			continue;
		Kind kind = DesignerKind(dn);
		if (kind == Kind::None && !_stricmp(dn, "inferno"))
			kind = Kind::Molotov; // molotov burn phase = separate inferno entity
		if (kind == Kind::None) {
			// Designer name can miss (build variance / class-only entities).
			// Fall back to the schema class name so the label never dies.
			char cls[64]{};
			if (Mem::SchemaClassName(e, cls, sizeof(cls)) && cls[0]) {
				for (char* p = cls; *p; ++p)
					if (*p >= 'A' && *p <= 'Z') *p += 32;
				if (std::strstr(cls, "inferno"))
					kind = Kind::Molotov;
				else if (std::strstr(cls, "smokegrenadeprojectile"))
					kind = Kind::Smoke;
				else if (std::strstr(cls, "hegrenadeprojectile"))
					kind = Kind::HE;
				else if (std::strstr(cls, "flashbangprojectile"))
					kind = Kind::Flash;
				else if (std::strstr(cls, "decoyprojectile"))
					kind = Kind::Decoy;
				else if (std::strstr(cls, "molotovprojectile")
					|| std::strstr(cls, "incendiaryprojectile"))
					kind = Kind::Molotov;
			}
		}
		if (kind == Kind::None)
			continue;

		// Owner: pawn, controller, or the thrown weapon entity. Fail-open
		// classification - any failed read must still SHOW the label (unknown
		// = treated as enemy) or the feature silently dies.
		CBaseHandle ownerH{};
		__try { ownerH = e->m_hOwnerEntity(); } __except (EXCEPTION_EXECUTE_HANDLER) { ownerH = CBaseHandle{}; }
		C_BaseEntity* ownerEnt = nullptr;
		if (ownerH.valid())
			ownerEnt = I::GameEntity->Instance->Get<C_BaseEntity>(ownerH.index());
		bool localOwner = ownerEnt
			&& (ownerEnt == reinterpret_cast<C_BaseEntity*>(local)
				|| (localCtrl && ownerEnt == reinterpret_cast<C_BaseEntity*>(localCtrl)));
		if (!localOwner && ownerEnt && Mem::ValidEntity(ownerEnt)) {
			// Owner may be the thrown weapon - walk one level up to the pawn.
			CBaseHandle h2{};
			__try { h2 = ownerEnt->m_hOwnerEntity(); } __except (EXCEPTION_EXECUTE_HANDLER) { h2 = CBaseHandle{}; }
			if (h2.valid()) {
				C_BaseEntity* up = I::GameEntity->Instance->Get<C_BaseEntity>(h2.index());
				localOwner = up && Mem::ValidEntity(up)
					&& (up == reinterpret_cast<C_BaseEntity*>(local)
						|| (localCtrl && up == reinterpret_cast<C_BaseEntity*>(localCtrl)));
			}
		}

		Vector_t pos(0.0f, 0.0f, 0.0f);
		__try {
			if (CGameSceneNode* nd = e->m_pGameSceneNode(); nd && Mem::IsUserPtr(nd))
				pos = nd->m_vecAbsOrigin();
		} __except (EXCEPTION_EXECUTE_HANDLER) { pos = Vector_t(0.0f, 0.0f, 0.0f); }
		if (!IsFiniteVec(pos))
			continue;
		const std::uint64_t nowMs = GetTickCount64();
		const Vector_t netVel = ReadEntityVecSeh(e, s_absVelOff);
		Vector_t simVel = netVel;
		std::uint64_t restMs = 0;
		// Prior tracking entry. Entity slots are recycled by the engine: a new
		// projectile spawning 1000+ units from the stale anchor must not
		// inherit its timers (HE fuse showed T-0 on recycled slots).
		const AirEntry* prior = nullptr;
		for (int j = 0; j < g_airN; ++j) {
			if (g_air[j].idx != idxs[k])
				continue;
			if (g_air[j].havePos) {
				const Vector_t jump{ pos.x - g_air[j].lastPos.x,
					pos.y - g_air[j].lastPos.y, pos.z - g_air[j].lastPos.z };
				if (jump.Length() < 1000.f)
					prior = &g_air[j];
			}
			break;
		}
		// Rest detection + velocity fallback from the networked position over
		// a 150 ms window: an airborne projectile always moves, a deployed one
		// never does. A single-frame delta flagged every projectile moving
		// slower than ~45 u/s (late arc, decoy glide) as resting and burned
		// its timers early - the window measures average speed instead.
		// m_vecAbsVelocity reads zero for remote projectiles, hence the
		// position-derived velocity fallback.
		Vector_t winPos = pos;
		ULONGLONG winMs = nowMs;
		if (prior && prior->havePos) {
			winPos = prior->lastPos;
			winMs = prior->posMs;
			const ULONGLONG dtMs = (nowMs >= winMs) ? (nowMs - winMs) : 0;
			const Vector_t d{ pos.x - winPos.x, pos.y - winPos.y, pos.z - winPos.z };
			if (d.Length() >= 0.75f) {
				winPos = pos; // still moving - slide the window
				winMs = nowMs;
				if (netVel.x * netVel.x + netVel.y * netVel.y + netVel.z * netVel.z < 1.0f
					&& dtMs > 0 && dtMs <= 250) {
					const float invDt = 1000.f / static_cast<float>(dtMs);
					const Vector_t dv{ d.x * invDt, d.y * invDt, d.z * invDt };
					if (IsFiniteVec(dv))
						simVel = dv; // remote projectile: velocity from net pos
				}
			} else if (dtMs >= 150) {
				// No measurable movement over the window -> rest anchor sits at
				// the actual stop time (window start), not at confirm time.
				restMs = (prior->restMs != 0) ? prior->restMs : winMs;
			}
			// Still but unconfirmed - keep the window anchored.
		}
		const bool resting = restMs != 0 && (nowMs - restMs) >= 150;

		// -- Rested utility - the timer only counts DOWN to when it goes off --
		// smoke/decoy/molotov: countdown to effect END from first rest.
		// HE/flash: countdown to the POP from the absolute fuse (throw time).
		// The engine removes the entity when it expires -> label gone.
		const float effectFull = KindEffectFull(kind);
		if (resting) {
			std::uint64_t stampMs = nowMs;
			if (prior) {
				// burn kinds anchor at first rest, fuse kinds at the throw
				if (effectFull > 0.f && prior->restMs)
					stampMs = prior->restMs;
				else if (effectFull <= 0.f && prior->firstSeenMs)
					stampMs = prior->firstSeenMs;
			}
			const float ageSec = static_cast<float>(nowMs - stampMs) / 1000.f;
			float remaining = (effectFull > 0.f) ? (effectFull - ageSec)
				: (kHeFlashFuseTotal - ageSec);
			if (remaining < 0.f)
				remaining = 0.f; // stay at 0 until the engine removes the entity

			DrawUtilityLabel(dl, vm, pos, localPos, kind, remaining);

			next[nextN].idx = idxs[k];
			next[nextN].kind = kind;
			next[nextN].pos = pos;
			next[nextN].vel = simVel;
			next[nextN].lastPos = winPos;
			next[nextN].posMs = winMs;
			next[nextN].havePos = true;
			if (effectFull > 0.f)
				next[nextN].restMs = stampMs;
			else if (prior && prior->firstSeenMs)
				next[nextN].firstSeenMs = prior->firstSeenMs;
			else
				next[nextN].firstSeenMs = stampMs;
			++nextN;
			continue;
		}

		// -- In flight: flight line + landing mark + label --
		Result res;
		bool reused = false;
		if (prior && SameVec(prior->pos, pos) && SameVec(prior->vel, simVel)) {
			res = prior->res;
			reused = true;
		}
		if (!reused)
			Simulate(pos, simVel, kind, e, res);

		next[nextN].idx = idxs[k];
		next[nextN].kind = kind;
		next[nextN].pos = pos;
		next[nextN].vel = simVel;
		next[nextN].res = res;
		next[nextN].lastPos = winPos;
		next[nextN].posMs = winMs;
		next[nextN].havePos = true;
		if (prior && prior->firstSeenMs)
			next[nextN].firstSeenMs = prior->firstSeenMs;
		else
			next[nextN].firstSeenMs = nowMs; // rest-stamp anchor for HE/flash
		++nextN;

		// Countdown only for kinds that EXPLODE mid-air/impact (HE/flash) -
		// absolute fuse from the throw (the per-tick sim restart would freeze
		// the number). Smoke/decoy/molotov "go off" when they land -> fuse -1
		// hides the timer (name + distance only) until the resting path.
		float fuse = -1.f;
		if (kind == Kind::HE || kind == Kind::Flash) {
			const std::uint64_t firstSeenMs = next[nextN - 1].firstSeenMs;
			fuse = kHeFlashFuseTotal - static_cast<float>(nowMs - firstSeenMs) / 1000.f;
			if (fuse < 0.f)
				fuse = 0.f;
		}

		// Thrown utility: the line it flies, the landing mark, and the label.
		DrawResult(dl, vm, res, localOwner ? 0.65f : 0.45f, true);
		DrawUtilityLabel(dl, vm, pos, localPos, kind, fuse);
	}

	// -- Ghost promotion: a tracked burn entity that vanished from the world
	// list was deleted by the engine exactly when its effect started (smoke
	// deploy / decoy deploy / molotov impact). Keep the countdown running at
	// the last known position so the timer reaches the real effect end.
	const std::uint64_t nowMs = GetTickCount64();
	const std::uint32_t curMapGen = SdkPrioA::MapGen();
	for (int j = 0; j < g_airN; ++j) {
		const AirEntry& p = g_air[j];
		if (!p.havePos || p.kind == Kind::None || p.idx <= 0)
			continue;
		bool seenNow = false;
		for (int q = 0; q < nextN; ++q) {
			if (next[q].idx == p.idx) { seenNow = true; break; }
		}
		if (seenNow)
			continue;
		const float full = KindEffectFull(p.kind);
		if (full <= 0.f)
			continue; // HE/flash pop = gone for good
		if (p.restMs == 0 && p.kind != Kind::Molotov && p.kind != Kind::Incendiary)
			continue; // in-flight smoke/decoy vanished before landing - no anchor
		// An effect entity (e.g. the inferno) took over nearby -> its own
		// live label covers the countdown; skip the ghost.
		bool takenOver = false;
		for (int q = 0; q < nextN && !takenOver; ++q) {
			if (next[q].kind != p.kind)
				continue;
			const Vector_t d{ next[q].pos.x - p.lastPos.x,
				next[q].pos.y - p.lastPos.y, next[q].pos.z - p.lastPos.z };
			if (d.Length() < 100.f)
				takenOver = true;
		}
		if (takenOver)
			continue;
		GhostTimer& gh = g_ghosts[g_ghostWrite % kMaxGhosts];
		++g_ghostWrite;
		gh.active = true;
		gh.kind = p.kind;
		gh.pos = p.lastPos;
		gh.stampMs = (p.restMs != 0) ? p.restMs : nowMs;
		gh.mapGen = curMapGen;
	}

	// -- Ghost draw + expiry --
	for (auto& gh : g_ghosts) {
		if (!gh.active)
			continue;
		if (gh.mapGen != curMapGen || H::SessionMapLeaving()) {
			gh.active = false;
			continue;
		}
		const float remaining = KindEffectFull(gh.kind)
			- static_cast<float>(nowMs - gh.stampMs) / 1000.f;
		if (remaining <= 0.f) {
			gh.active = false; // effect over
			continue;
		}
		DrawUtilityLabel(dl, vm, gh.pos, localPos, gh.kind, remaining);
	}

	for (int j = 0; j < nextN; ++j)
		g_air[j] = next[j];
	g_airN = nextN;
}

} // namespace

void Draw(const ViewMatrix& vm) {
	if (!Config::nadepred_enable)
		return;
	if (H::SessionMapLeaving() || !H::SessionEntityReady())
		return;
	if (!vm.viewMatrix || !Trace::Ready())
		return;

	C_CSPlayerPawn* local = H::SafeLocalAlive();
	if (!local || !Mem::ValidEntity(local))
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	// Held-weapon preview - independent from the in-air arcs below so the
	// arc keeps rendering while a thrown nade flies (player is holding a gun).
	C_CSWeaponBase* wep = nullptr;
	__try {
		wep = local->GetActiveWeapon();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		wep = nullptr;
	}
	if (wep && Mem::ValidEntity(wep)) {
		UpdateCache(local, wep);
		if (g_cache.kind != Kind::None)
			DrawResult(dl, vm, g_cache.res, 1.0f);
		else
			g_cache.res.Reset();
	} else {
		g_cache.res.Reset();
	}

	// Live trajectory for utility already in flight.
	if (Config::nadepred_in_air)
		DrawInAir(dl, vm, local);
}

} // namespace NadePred

