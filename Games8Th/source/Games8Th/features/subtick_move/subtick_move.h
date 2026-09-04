#pragma once

// Bhop: air strip + land-frac subtick release/press.
// Strafe mode 1: WASD edge-latch + 16 yaw_delta steps.
// Hook order: jumpbug -> bhop -> combat -> RewriteStrafe -> final subtick.

class CUserCmd;
class C_CSPlayerPawn;
namespace SubtickMove {

bool Init();

// AFTER original CreateMove, BEFORE Pred (cmd-only, no engine re-sim).
void RewriteBhop(CUserCmd* cmd, C_CSPlayerPawn* pawn);
void RewriteStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn);

// Final subtick: true when the strafer injected yaw steps this tick -
// caller keeps base moves; otherwise base moves are zeroed.
bool HandledThisTick();

} // namespace SubtickMove

