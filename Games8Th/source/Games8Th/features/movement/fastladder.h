#pragma once

class CUserCmd;
class C_CSPlayerPawn;
struct QAngle_t;

// Cmd-only ladder boost: pitch 89 + yaw ?90, back+strafe analog.
// Camera is restored after the write - engine apply of cmd angles is undone.
namespace FastLadder {
	void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn);
	bool WroteThisTick();
	// True while the last ladder write is still live (incl. after CreateMove).
	bool PeekHold(QAngle_t& out);
	void RestoreCamera();
	void RestoreFrom(const QAngle_t& ang);
	void Reset();
}

