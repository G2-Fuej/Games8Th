#pragma once

#include "../../utils/math/vector/vector.h"
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include <cstdint>

// Self-recorded position history for backtracking.
//
// CS2 keeps NO readable multi-tick history client-side (only current + previous
// networked state, blended by GetPlayerInterp). So we keep our own ring buffer:
// every game tick (CreateMove), store each enemy's head/chest/pelvis/origin.
//
// WHY DEPTH MUST MATCH THE CLAIM:
// The server rewinds enemies to the player-tick stamped in the fired command
// (CCSGOInputHistoryEntryPB::nPlayerTickCount), NOT to an arbitrary age. A ghost
// aimed at depth X only connects if the command claims X too. Therefore:
//   - ghost selection uses TargetDepthMs() = interp floor (31.25ms) + claimed ticks
//   - ExtraTicks() is subtracted from the command's history player ticks
//     (hooks.cpp, mirrors nospread's proven history rewrite)
// Slider beyond one interp window therefore extends the actual rewind, capped
// at 12 ticks (~187ms) - deeper claims get clamped/rejected server-side.
class CUserCmd;

namespace Backtrack {

constexpr float kTickMs = 15.625f;          // 64 tick
constexpr float kInterpMs = 2.f * kTickMs;  // interpolation window floor
constexpr int   kMaxClaimTicks = 12;

// Extra ticks to claim beyond the interp window, derived from
// Config::backtrack_ms. 0 = interp-only (always connectable).
int ExtraTicks();

// Ghost age queries should use this (interp floor + claimed ticks).
float TargetDepthMs();

// Record enemies into per-pawn rings. Call once per CreateMove tick while
// in-game. No-op (and clears stale rings) when feature is off.
void Tick(void* localPawn);

// Present-thread ghost skeletons (pelvis->chest->head + head dot) at target
// depth. Internally gated by Config::backtrack && Config::backtrack_skeleton.
void DrawGhosts(const ViewMatrix& vm);

// Applies authoritative lag compensation to input history entries on attack ticks (manual shooting & triggerbot).
void ApplyCommand(CUserCmd* cmd);

// Drop all rings (map leave / round reset / unload).
void Reset();

} // namespace Backtrack

