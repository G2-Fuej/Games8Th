#pragma once

#include "../../utils/math/vector/vector.h"

class C_CSWeaponBase;
class C_CSPlayerPawn;

namespace AutoWall {

struct Result {
	float damage = 0.f;
	bool penetrated = false;
	bool hit = false;
};

bool Init();
bool Ready();

// Post-armor damage at aimPoint.
// allowPen=false -> visible estimate (caller owns LOS).
// allowPen=true -> game CreateTrace+DamageToPoint,
// TraceLine pen fallback.
Result Fire(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen);

// true if damage >= minDamage. minDamage<=0 -> any hit.
// If target HP < minDamage, requires lethal (dmg >= HP).
bool PassesMinDamage(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamage);

// Target-less crosshair penetration probe (for the autowall crosshair overlay).
// Traces eye -> dir and classifies the surface under the reticle:
// Clear - nothing solid in front (unobstructed shot)
// Penetrable - a bullet exits the wall(s) with >= 1 dmg (can wallbang)
// Blocked - wall too thick / weapon can't penetrate
// NoData - not ready / bad weapon
enum class XhairPen { NoData = 0, Clear = 1, Penetrable = 2, Blocked = 3 };

XhairPen CheckCrosshairPenetration(
	const Vector_t& eye,
	const Vector_t& dir,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local);

// Game-thread cache for the crosshair overlay. Present must NOT call
// engine traces directly (multi-queue insecure surface - every Present
// trace trips detection). Tick runs on the game thread (CreateMove/FSN);
// Present only reads the cached pen value.
void TickXhairCache();
XhairPen GetCachedXhairPen();
void InvalidateXhairCache();

} // namespace AutoWall

