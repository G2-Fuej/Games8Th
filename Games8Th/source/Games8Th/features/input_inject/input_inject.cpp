#define NOMINMAX
#include "input_inject.h"

#include "../prediction/prediction.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>

namespace InputInject {
namespace {

constexpr std::uintptr_t kMoveServicesOff = 0x1248;
constexpr std::uintptr_t kButtonsOff = 0x50;
constexpr std::uintptr_t kLegacyJumpOff = 0x6B0;
constexpr std::uintptr_t kModernJumpOff = 0x6C8;
constexpr std::int32_t kInvalidJumpTick = -1024;

struct MoveSvcButtons {
	std::uint64_t padVtable;
	std::uint64_t nValue;
	std::uint64_t nValueChanged;
	std::uint64_t nValueScroll;
};

void* MoveSvc(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return nullptr;
	void* svc = nullptr;
	__try {
		svc = pawn->m_pMovementServices();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		svc = nullptr;
	}
	if (svc)
		return svc;
	__try {
		svc = *reinterpret_cast<void**>(
			reinterpret_cast<std::uintptr_t>(pawn) + kMoveServicesOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return svc;
}

MoveSvcButtons* SvcButtons(void* moveSvc)
{
	if (!moveSvc)
		return nullptr;
	return reinterpret_cast<MoveSvcButtons*>(
		reinterpret_cast<std::uintptr_t>(moveSvc) + kButtonsOff);
}

void WriteSvc(void* moveSvc, std::uint64_t button, bool down, bool edge)
{
	MoveSvcButtons* b = SvcButtons(moveSvc);
	if (!b)
		return;
	__try {
		if (down) {
			b->nValue |= button;
			if (edge)
				b->nValueChanged |= button;
			else
				b->nValueChanged &= ~button;
			b->nValueScroll &= ~button;
		} else {
			b->nValue &= ~button;
			b->nValueChanged &= ~button;
			b->nValueScroll &= ~button;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void ClearJumpHistory(void* moveSvc)
{
	if (!moveSvc)
		return;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(moveSvc);
		*reinterpret_cast<std::uint8_t*>(base + kLegacyJumpOff + 0x10) = 0;
		*reinterpret_cast<std::int32_t*>(base + kModernJumpOff + 0x10) = kInvalidJumpTick;
		*reinterpret_cast<float*>(base + kModernJumpOff + 0x14) = 0.f;
		*reinterpret_cast<std::int32_t*>(base + kModernJumpOff + 0x18) = kInvalidJumpTick;
		*reinterpret_cast<float*>(base + kModernJumpOff + 0x1C) = 0.f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void SyncPbBits(CBaseUserCmdPB* base)
{
	if (!base)
		return;
	if (base->pInButtonState) {
		base->pInButtonState->SetBits(
			BUTTON_STATE_PB_BITS_BUTTONSTATE1
			| BUTTON_STATE_PB_BITS_BUTTONSTATE2
			| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
	}
	base->SetBits(BASE_BITS_BUTTONPB);
}

void WriteCmd(CUserCmd* cmd, std::uint64_t button, bool down, bool edge)
{
	if (!cmd)
		return;
	if (down) {
		cmd->nButtons.nValue |= button;
		if (edge)
			cmd->nButtons.nValueChanged |= button;
		else
			cmd->nButtons.nValueChanged &= ~button;
		cmd->nButtons.nValueScroll &= ~button;
	} else {
		cmd->nButtons.nValue &= ~button;
		cmd->nButtons.nValueChanged &= ~button;
		cmd->nButtons.nValueScroll &= ~button;
	}
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pInButtonState) {
			if (down) {
				base->pInButtonState->nValue |= button;
				if (edge)
					base->pInButtonState->nValueChanged |= button;
				else
					base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			} else {
				base->pInButtonState->nValue &= ~button;
				base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			}
		}
		SyncPbBits(base);
	}
}

CSubtickMoveStep* NewStep(CBaseUserCmdPB* base)
{
	if (!base)
		return nullptr;
	if (base->subtickMovesField.nCurrentSize >= 30)
		return nullptr;
	CSubtickMoveStep* step = base->add_subtick_move();
	if (!step || reinterpret_cast<std::uintptr_t>(step) < 0x10000)
		return nullptr;
	step->nHasBits = 0;
	step->nCachedBits = 0;
	step->nButton = 0;
	step->bPressed = false;
	step->flWhen = 0.f;
	step->flAnalogForwardDelta = 0.f;
	step->flAnalogLeftDelta = 0.f;
	return step;
}

bool g_inited = false;

} // namespace

bool Init()
{
	if (g_inited)
		return true;
	g_inited = true;
	// Patterns live in Pred::Init / Input::init - just ensure Pred scanned.
	Pred::Init();
	const auto& e = Pred::Engines();
	Con::Info(
		"InputInject: forceBtn=%p createStep=%p subtickIn=%p qForce=%p",
		e.forceButtonsDown, e.createNewSubtickMoveStep,
		e.processSubtickInput, e.queueForceSubtickMove);
	return true;
}

void SetHeld(CUserCmd* cmd, std::uint64_t button, bool down)
{
	WriteCmd(cmd, button, down, false);
}

void SetEdge(CUserCmd* cmd, std::uint64_t button, bool pressed)
{
	WriteCmd(cmd, button, pressed, true);
}

void Apply(
	CUserCmd* cmd,
	C_CSPlayerPawn* pawn,
	std::uint64_t button,
	bool down,
	bool edge)
{
	WriteCmd(cmd, button, down, edge);
	void* svc = MoveSvc(pawn);
	WriteSvc(svc, button, down, edge);
	if (down && edge && svc && button) {
		// ProcessMovement samples ForceButtonsDown mask for held-forced keys.
		const Pred::EngineAddrs& e = Pred::Engines();
		if (e.forceButtonsDown) {
			__try {
				using Fn = void(__fastcall*)(void*, std::uint64_t);
				reinterpret_cast<Fn>(e.forceButtonsDown)(svc, button);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		} else {
			Input::ForceButtons(pawn, button);
		}
	}
	if (button == IN_JUMP && !down)
		ClearJumpHistory(svc);
}

bool SubtickButton(
	CBaseUserCmdPB* base,
	std::uint64_t button,
	bool pressed,
	float when)
{
	CSubtickMoveStep* step = NewStep(base);
	if (!step)
		return false;
	step->nButton = button;
	step->bPressed = pressed;
	step->flWhen = std::clamp(when, 0.f, kWhenEnd);
	// mercey sets analog has-bits on EVERY button step (values 0) - the
	// engine's subtick processor branches on presence, and unset analog
	// bits let prior movement leak into the button window.
	step->flAnalogForwardDelta = 0.f;
	step->flAnalogLeftDelta = 0.f;
	step->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN
		| MOVESTEP_BITS_ANALOG_FORWARD_DELTA | MOVESTEP_BITS_ANALOG_LEFT_DELTA);
	return true;
}

bool InjectAttack(CUserCmd* cmd, C_CSPlayerPawn* pawn, bool secondary)
{
	const std::uint64_t btn = secondary ? IN_SECOND_ATTACK : IN_ATTACK;
	Apply(cmd, pawn, btn, true, true);
	if (cmd && cmd->csgoUserCmd.pBaseCmd)
		SubtickButton(cmd->csgoUserCmd.pBaseCmd, btn, true, kWhenStart);
	return true;
}

bool InjectJump(CUserCmd* cmd, C_CSPlayerPawn* pawn, float pressWhen)
{
	Apply(cmd, pawn, IN_JUMP, true, true);
	if (cmd && cmd->csgoUserCmd.pBaseCmd)
		SubtickButton(cmd->csgoUserCmd.pBaseCmd, IN_JUMP, true, pressWhen);
	return true;
}

bool InjectDuck(CUserCmd* cmd, C_CSPlayerPawn* pawn, bool down)
{
	Apply(cmd, pawn, IN_DUCK, down, down);
	if (down && cmd && cmd->csgoUserCmd.pBaseCmd)
		SubtickButton(cmd->csgoUserCmd.pBaseCmd, IN_DUCK, true, kWhenStart);
	return true;
}

void Clear(CUserCmd* cmd, C_CSPlayerPawn* pawn, std::uint64_t button)
{
	Apply(cmd, pawn, button, false, false);
	if (button == IN_JUMP && cmd && cmd->csgoUserCmd.pBaseCmd)
		ClearJumpSubticks(cmd->csgoUserCmd.pBaseCmd);
}

bool ForceDown(C_CSPlayerPawn* pawn, std::uint64_t buttons)
{
	if (!pawn || !buttons)
		return false;
	void* svc = MoveSvc(pawn);
	if (!svc)
		return false;
	const Pred::EngineAddrs& e = Pred::Engines();
	if (e.forceButtonsDown) {
		__try {
			using Fn = void(__fastcall*)(void*, std::uint64_t);
			reinterpret_cast<Fn>(e.forceButtonsDown)(svc, buttons);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	return Input::ForceButtons(pawn, buttons);
}

// Engine ProcessSubTickInput: ~32 steps. Torn pRep on leave can have
// nAllocatedSize = 0x7FFFFFFF - walk that and we OOB-write the protobuf arena.
bool SubtickRange(CBaseUserCmdPB* base, int& count)
{
	count = 0;
	if (!base || !base->subtickMovesField.pRep)
		return false;
	const int n = base->subtickMovesField.nCurrentSize;
	const int cap = base->subtickMovesField.pRep->nAllocatedSize;
	if (n <= 0 || n > 64 || cap <= 0 || cap > 128)
		return false;
	count = (n < cap) ? n : cap;
	return true;
}

void ClearJumpSubticks(CBaseUserCmdPB* base)
{
	int count = 0;
	if (!SubtickRange(base, count))
		return;
	for (int i = 0; i < count; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (!step || step->nButton != IN_JUMP)
			continue;
		step->nButton = 0;
		step->bPressed = false;
		step->flWhen = 0.f;
		step->nHasBits = 0;
		step->nCachedBits = 0;
	}
}

// Wipe ALL engine subtick steps
// before the feature pipeline. Held-space / key-event / engine-packed analog
// garbage left in the list desyncs server-side land / move state. Features
// re-add their own steps afterwards (bhop release->press, strafe analog, ...).
void ClearAllSubticks(CBaseUserCmdPB* base)
{
	if (!base)
		return;
	int count = 0;
	if (!SubtickRange(base, count)) {
		if (base->subtickMovesField.nCurrentSize < 0
			|| base->subtickMovesField.nCurrentSize > 64)
			base->subtickMovesField.nCurrentSize = 0;
		return;
	}
	for (int i = 0; i < count; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (!step)
			continue;
		step->nButton = 0;
		step->bPressed = false;
		step->flWhen = 0.f;
		step->flAnalogForwardDelta = 0.f;
		step->flAnalogLeftDelta = 0.f;
		step->nHasBits = 0;
		step->nCachedBits = 0;
	}
	// Fresh list - next add_subtick_move() reuses slot 0.
	// nTotalSize stays as-is (touch only m_current_size; zeroing it
	// too deviates from the engine's repeated-field bookkeeping).
	base->subtickMovesField.nCurrentSize = 0;
}

void ClearAnalogSubticks(CBaseUserCmdPB* base)
{
	int count = 0;
	if (!SubtickRange(base, count))
		return;
	for (int i = 0; i < count; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (!step)
			continue;
		step->flAnalogForwardDelta = 0.f;
		step->flAnalogLeftDelta = 0.f;
		const std::uint32_t keep = ~(MOVESTEP_BITS_ANALOG_FORWARD_DELTA
			| MOVESTEP_BITS_ANALOG_LEFT_DELTA);
		step->nHasBits &= keep;
		step->nCachedBits &= keep;
	}
}

void SortSubticks(CBaseUserCmdPB* base)
{
	int count = 0;
	if (!SubtickRange(base, count) || count <= 1)
		return;
	// Insertion sort - n <= 32
	for (int i = 1; i < count; ++i) {
		CSubtickMoveStep* key = base->subtickMovesField.pRep->tElements[i];
		if (!key)
			continue;
		const float kw = key->flWhen;
		int j = i - 1;
		while (j >= 0) {
			CSubtickMoveStep* cur = base->subtickMovesField.pRep->tElements[j];
			const float cw = cur ? cur->flWhen : 0.f;
			if (cw <= kw)
				break;
			base->subtickMovesField.pRep->tElements[j + 1] = cur;
			--j;
		}
		base->subtickMovesField.pRep->tElements[j + 1] = key;
	}
}

void SanitizeSubticks(CBaseUserCmdPB* base)
{
	int count = 0;
	if (!SubtickRange(base, count))
		return;
	for (int i = 0; i < count; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (!step)
			continue;
		if (!std::isfinite(step->flWhen))
			step->flWhen = 0.f;
		// IDA ProcessSubTickInput: flWhen >= 1.0 ignored (UC release uses 1.0).
		// Do NOT clamp 1.0 down to 0.999 - that turns no-op release into cancel.
		if (step->flWhen < 0.f)
			step->flWhen = 0.f;
		else if (step->flWhen > 1.f)
			step->flWhen = 1.f;
		// leave exactly 1.0 alone
		if (!std::isfinite(step->flAnalogForwardDelta))
			step->flAnalogForwardDelta = 0.f;
		if (!std::isfinite(step->flAnalogLeftDelta))
			step->flAnalogLeftDelta = 0.f;
	}
	SortSubticks(base);
}

bool SubtickAnalog(CBaseUserCmdPB* base, float when, float fwdDelta, float leftDelta)
{
	CSubtickMoveStep* step = NewStep(base);
	if (!step)
		return false;
	step->nButton = 0;
	step->bPressed = false;
	step->flWhen = std::clamp(when, 0.f, kWhenEnd);
	step->flAnalogForwardDelta = fwdDelta;
	step->flAnalogLeftDelta = leftDelta;
	step->SetBits(
		MOVESTEP_BITS_WHEN
		| MOVESTEP_BITS_ANALOG_FORWARD_DELTA
		| MOVESTEP_BITS_ANALOG_LEFT_DELTA);
	return true;
}

// WASD strafe apply_yaw_subtick - quantized-movement yaw step.
// All presence bits are set like the proto setters (button/
// pressed/analog present with value 0), so EnsureAnalogMoveStep skips ticks
// that already carry yaw steps.
bool SubtickYawDelta(CBaseUserCmdPB* base, float when, float yawDelta)
{
	if (!base)
		return false;
	while (yawDelta > 180.f) yawDelta -= 360.f;
	while (yawDelta < -180.f) yawDelta += 360.f;
	if (std::fabs(yawDelta) <= 0.01f)
		return false;
	CSubtickMoveStep* step = NewStep(base);
	if (!step)
		return false;
	step->nButton = 0;
	step->bPressed = false;
	step->flWhen = std::clamp(when, 0.f, kWhenEnd);
	step->flAnalogForwardDelta = 0.f;
	step->flAnalogLeftDelta = 0.f;
	step->flPitchDelta = 0.f;
	step->flYawDelta = yawDelta;
	step->SetBits(
		MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED
		| MOVESTEP_BITS_WHEN
		| MOVESTEP_BITS_ANALOG_FORWARD_DELTA
		| MOVESTEP_BITS_ANALOG_LEFT_DELTA
		| MOVESTEP_BITS_PITCH_DELTA
		| MOVESTEP_BITS_YAW_DELTA);
	return true;
}

// Analog fix: when NO feature wrote any move
// subtick step, append one whose analog deltas carry the difference between the
// cmd's base moves and the last moves the movement services processed. Without
// it the server has no analog sample for silent / keyless movement ticks.
bool EnsureAnalogMoveStep(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	if (!cmd || !pawn)
		return false;
	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base || !base->subtickMovesField.pRep)
		return false;

	// Already has a button/analog step (bhop, strafe, ...) - skip.
	int count = 0;
	if (!SubtickRange(base, count))
		return false;
	constexpr std::uint32_t kMoveBits =
		MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED
		| MOVESTEP_BITS_ANALOG_FORWARD_DELTA | MOVESTEP_BITS_ANALOG_LEFT_DELTA;
	for (int i = 0; i < count; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (step && (step->nHasBits & kMoveBits) != 0)
			return false;
	}

	void* svc = MoveSvc(pawn);
	if (!svc)
		return false;
	// CPlayer_MovementServices::m_flCmdForwardMove/LeftMove (client dump 0x1A0/0x1A4)
	float lastFwd = 0.f;
	float lastLeft = 0.f;
	__try {
		const auto m = reinterpret_cast<std::uintptr_t>(svc);
		lastFwd = *reinterpret_cast<float*>(m + 0x1A0);
		lastLeft = *reinterpret_cast<float*>(m + 0x1A4);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!std::isfinite(lastFwd) || !std::isfinite(lastLeft))
		return false;

	const float fwdDelta = base->flForwardMove - lastFwd;
	const float leftDelta = base->flSideMove - lastLeft;
	if (std::fabs(fwdDelta) < 1e-4f && std::fabs(leftDelta) < 1e-4f)
		return false;
	return SubtickAnalog(base, kWhenStart, fwdDelta, leftDelta);
}

} // namespace InputInject

void InputInject::ClearAttackSubticks(CBaseUserCmdPB* base)
{
	if (!base || !base->subtickMovesField.pRep)
		return;
	const int n = base->subtickMovesField.nCurrentSize;
	const int cap = base->subtickMovesField.pRep->nAllocatedSize;
	if (n <= 0 || n > 64 || cap <= 0 || cap > 128)
		return;
	const int count = (n < cap) ? n : cap;
	for (int i = 0; i < count; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (!step) continue;
		const bool isAtk = step->nButton == IN_ATTACK || step->nButton == IN_SECOND_ATTACK;
		if (!isAtk) continue;
		step->nButton = 0;
		step->bPressed = false;
		step->flWhen = 0.f;
		step->nHasBits = 0;
		step->nCachedBits = 0;
	}
}


