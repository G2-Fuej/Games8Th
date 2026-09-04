#pragma once

// Jumpbug / Edgejump - CreateMove, cmd-serialized only.
//
// Jumpbug: mercey 1:1 - fall + land-this-tick hull trace, duck-down@0,
// duck-up@frac, jump-up@frac, jump-down@frac. Space held is the trigger.
// Edgejump: ground + key + about to leave ledge -> jump edge before fall.

class CUserCmd;
class C_CSPlayerPawn;

namespace JumpBug {

bool ClaimedJumpThisTick();
bool ActiveThisTick();
float LandingFraction();

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn);
void Reset();

} // namespace JumpBug

