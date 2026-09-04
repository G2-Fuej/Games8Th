#pragma once
#include <cstdint>
#include <vector>
#include "../../interfaces/CUserCmd/CUserCmd.h"
// CL_Bypass - buttons / subticks / move_crc recompute + SerializePartialToArray hook.// CRC path: recompute CBaseUserCmdPB::strMoveCrc after feature mutations so// subtick / button rewrites pass client serialize ( surface).
namespace CL_Bypass {
void PreClientCreateMove(CUserCmd* cmd);
	void PostClientCreateMove(void* pCSGOInput, CUserCmd* cmd);
	// Called from SerializePartialToArray when msg looks like CBaseUserCmdPB	
void OnCBaseUserCmdPB(void* pMsg);
	// Recompute move_crc on live base cmd (Post + serialize hook)

void RecomputeMoveCrc(CBaseUserCmdPB* base);
	// Button helpers	
void SetAttack(CUserCmd* cmd, bool addSubtick = false);
	void SetDontAttack(CUserCmd* cmd, bool addSubtick = false);
	void SetJump(CUserCmd* cmd, bool addSubtick = false);
	void SetDontJump(CUserCmd* cmd, bool addSubtick = false);
	void AddProcessSubTick(std::
uint64_t button, bool pressed);
	void AddProcessSubTick(std::
uint64_t button, bool pressed, float when);
	bool Init();
	bool __fastcall hkSerializePartialToArray(void* msg, void* out, int size);
	void SetInOriginalCreateMove(bool v);
	bool InOriginalCreateMove();
	// true after Init installed SerializePartial hook	
bool CrcHookActive();
}
 // namespace CL_Bypass

