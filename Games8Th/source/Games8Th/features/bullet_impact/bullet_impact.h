#pragma once

#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "../../../../external/imgui/imgui.h"
#include <cstdint>

// Bullet tracers + impact cubes / sparks. Owned separately from hitmarker.
namespace BulletFx {
	void Install();
	// Clear map-owned impact state without unregistering the permanent listener.
	void Reset();
	void Shutdown();
	// mercey-style listener; FireEventClientSide is only a fallback.
	bool ListenerActive();
	// bullet_impact game event -> buffer world impact + eye origin for tracer.
	void OnGameEvent(void* gameEvent);
	void Draw(const ViewMatrix& vm);

	// Call on AF/TR/aim fire so a tracer can originate from the actual shot eye.
	void NoteLastFire(const Vector_t& eye, const QAngle_t& fireAngles);

	// Nearest buffered bullet impact (used by hitmarker to refine world hit pos).
	bool FindNearestImpact(const Vector_t& anchor, Vector_t& out, float maxAge, float maxDist);

	// Last shot fire ray (eye + normalized dir). Used by hitmarker for raycast.
	bool GetLastFire(Vector_t& eye, Vector_t& dir);
}

