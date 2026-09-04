#pragma once
#include "../../utils/math/vector/vector.h"
#include "../../utils/math/viewmatrix/viewmatrix.h"

// Grenade trajectory predictor (held-weapon preview + live in-air arcs).
// Source 2 projectile physics in userland, matched to the engine tracer
// (IDA sub_18104AEF0): two substeps of dt = 1/128 per 1/64 tick,
// half-step Verlet midpoint gravity (sv_gravity * 0.4),
// engine hull-trace collision with MASK_GRENADE.
//
// Detonation rules per kind match Think_Detonate:
//   smoke/decoy  - speed-gated every 12 ticks (0.1875s)
//   he/flash     - (tick-8)*dt > 1.5s fuse
//   molotov/inc  - shatter on near-flat impact or rest
namespace NadePred {

enum class Kind : int {
	None = -1,
	Flash = 43,
	HE = 44,
	Smoke = 45,
	Molotov = 46,
	Decoy = 47,
	Incendiary = 48,
};

struct Result {
	static constexpr int kMaxPoints = 256;
	static constexpr int kMaxBounces = 24;

	Vector_t points[kMaxPoints];
	int pointCount = 0;
	Vector_t bounces[kMaxBounces];
	int bounceCount = 0;
	Vector_t endPos{};
	bool valid = false;
	// Seconds from sim start to impact/detonation/rest (the loop's exit tick).
	// For in-air sims this is the remaining time to landing - badge countdown.
	float fuse = 0.f;

	void Reset() {
		pointCount = 0;
		bounceCount = 0;
		endPos = Vector_t(0.0f, 0.0f, 0.0f);
		valid = false;
		fuse = 0.f;
	}
};

// Simulate one throw. start = eye pos, vel = initial velocity (units/s).
// skip = local pawn (sim ignores own hitbox). Returns false on invalid input.
bool Simulate(const Vector_t& start, const Vector_t& vel, Kind kind, void* skip, Result& out);

// Initial throw velocity from view angles + live weapon data.
Vector_t ComputeThrowVelocity(const QAngle_t& viewAngles, float throwStrength, float baseVelocity, const Vector_t& inheritVelocity);

// Per-frame: re-sim if inputs changed, then draw arc + bounces + landing mark.
void Draw(const ViewMatrix& vm);

} // namespace NadePred

