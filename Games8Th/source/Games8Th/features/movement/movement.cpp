#define NOMINMAX
#include "movement.h"
#include "../prediction/prediction.h"
#include "../input_inject/input_inject.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/console/console.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include <Windows.h>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>


namespace {
	// Dump build 14169 (client_dll.hpp) - hard fallbacks when schema miss returns 0.
	constexpr std::uintptr_t kFlagsOff = 0x3F4;
	constexpr std::uintptr_t kAbsVelOff = 0x3F8;
	constexpr std::uintptr_t kGroundEntityOff = 0x530;
	constexpr std::uintptr_t kMoveServicesOff = 0x1248; // C_BasePlayerPawn->m_pMovementServices
	constexpr std::uintptr_t kButtonsOff = 0x50;        // CPlayer_MovementServices->m_nButtons
	constexpr std::uintptr_t kStaminaOffFallback = 0x694;
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDeg2Rad = kPi / 180.f;
	constexpr std::uint64_t kJumpMask = IN_JUMP;
	constexpr std::uint64_t kMoveMask =
		IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;

	// IDA ProcessSubTickInput: flWhen must be < 1.0; max 32 steps/tick.
	// Subtick bhop: press @ landFrac (or 0), release @ end-of-tick.
	constexpr float kSubtickWhenRelease = 0.999f;



	void* GetMoveServices(C_CSPlayerPawn* pawn)
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
		// Schema miss -> hard dump offset
		__try {
			svc = *reinterpret_cast<void**>(
				reinterpret_cast<std::uintptr_t>(pawn) + kMoveServicesOff);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
		return svc;
	}

	void ZeroStamina(C_CSPlayerPawn* pawn)
	{
		void* moveSvc = GetMoveServices(pawn);
		if (!moveSvc)
			return;

		std::uintptr_t off = SchemaFinder::Get(
			hash_32_fnv1a_const("CCSPlayer_MovementServices->m_flStamina"));
		if (!off)
			off = kStaminaOffFallback;

		__try {
			*reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(moveSvc) + off) = 0.f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	float NormalizeYaw(float yaw)
	{
		while (yaw > 180.f) yaw -= 360.f;
		while (yaw < -180.f) yaw += 360.f;
		return yaw;
	}

	std::uint64_t CmdButtons(CUserCmd* cmd)
	{
		if (!cmd)
			return 0ULL;
		std::uint64_t buttons = cmd->nButtons.nValue;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pInButtonState)
				buttons |= base->pInButtonState->nValue;
		}
		return buttons;
	}

	bool ReadCmdYaw(CUserCmd* cmd, float& yaw)
	{
		yaw = 0.f;
		if (!cmd)
			return false;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pViewAngles) {
				yaw = base->pViewAngles->angValue.y;
				return true;
			}
		}
		if (Input::GetViewAngles && Input::viewAngleContext) {
			const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
			if (viewPtr) {
				yaw = reinterpret_cast<Vector_t*>(viewPtr)->y;
				return true;
			}
		}
		return false;
	}

	bool BadMoveType(C_CSPlayerPawn* pawn)
	{
		__try {
			const uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
			if (!mtOff)
				return false;
			const uint8_t moveType = *reinterpret_cast<uint8_t*>(
				reinterpret_cast<uintptr_t>(pawn) + mtOff);
			return moveType == MOVETYPE_LADDER
				|| moveType == MOVETYPE_NOCLIP
				|| moveType == MOVETYPE_OBSERVER;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;
		}
	}

	bool InWater(std::uint32_t flags)
	{
		return (flags & (FL_INWATER | FL_WATERJUMP)) != 0U;
	}

	// Live pawn flags/vel. Prefer dump 0x3F4; OR schema only for FL_ONGROUND bit.
	bool ReadLiveMoveState(C_CSPlayerPawn* pawn, Vector_t& vel, std::uint32_t& flags)
	{
		vel = Vector_t{ 0.f, 0.f, 0.f };
		flags = 0;
		if (!pawn)
			return false;
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(pawn);
			const std::uintptr_t schemaOff = SchemaFinder::Get(
				hash_32_fnv1a_const("C_BaseEntity->m_fFlags"));
			const std::uint32_t dumpF = *reinterpret_cast<std::uint32_t*>(base + kFlagsOff);
			std::uint32_t schemaF = 0;
			if (schemaOff && schemaOff != kFlagsOff)
				schemaF = *reinterpret_cast<std::uint32_t*>(base + schemaOff);
			flags = dumpF;
			// Only promote real ONGROUND - never PARTIAL (false ground mid-air).
			if (schemaF & FL_ONGROUND)
				flags |= FL_ONGROUND;

			vel = pawn->m_vecAbsVelocity();
			if (vel.x == 0.f && vel.y == 0.f && vel.z == 0.f)
				vel = *reinterpret_cast<Vector_t*>(base + kAbsVelOff);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

}


// set_button(DOWN/UP): hold bit only - NOT value|changed every tick
// (that is press-edge spam and breaks continuous A/D).
void SetMoveButtonHeld(CUserCmd* cmd, std::uint64_t button, bool down)
{
	if (!cmd)
		return;
	if (down) {
		cmd->nButtons.nValue |= button;
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
				base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			} else {
				base->pInButtonState->nValue &= ~button;
				base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			}
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
		}
		base->SetBits(BASE_BITS_BUTTONPB);
	}
}

// Mouse autostrafe.
// CS2 leftmove is unit ?1 - NOT Source1 ?450.
void AutoStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn, const Vector_t& /*vel*/, bool onGround, std::uint32_t flags)
{
	if (!cmd || !pawn || !Config::autostrafe)
		return;
	if (onGround)
		return;
	if (BadMoveType(pawn) || InWater(flags))
		return;

	// GetAsyncKeyState(VK_SPACE) only - not cmd jump bits
	if (!(GetAsyncKeyState(VK_SPACE) & 0x8000))
		return;

	auto* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base)
		return;

	// i_csgo_input layout (pad 0x228 + thirdperson/buttons):
	// forward_move 0x260, left_move 0x264, mouse_delta_x 0x26C
	constexpr std::uintptr_t kCsgoInputForward = 0x260;
	constexpr std::uintptr_t kCsgoInputLeft = 0x264;
	constexpr std::uintptr_t kCsgoInputMouseDx = 0x26C;

	int mouseDelta = base->nMousedX;
	void* csgoInput = Input::GetCSGOInput();
	if (mouseDelta == 0 && csgoInput) {
		__try {
			mouseDelta = *reinterpret_cast<int*>(
				reinterpret_cast<std::uint8_t*>(csgoInput) + kCsgoInputMouseDx);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			mouseDelta = 0;
		}
	}

	// look right (+mdx) -> left_move=-1 (RIGHT); look left -> +1 (LEFT)
	// no mouse -> alternate A/D each command (legit). Silent mode owns vectorial.
	float leftMove = 0.f;
	if (mouseDelta > 1)
		leftMove = -1.f;
	else if (mouseDelta < -1)
		leftMove = 1.f;
	else
		leftMove = (cmd->nCommandNumber % 2 == 0) ? 1.f : -1.f;

	// Unit scale (?1) - set_forwardmove(0) / set_leftmove(left_move)
	base->flForwardMove = 0.f;
	base->flSideMove = leftMove;
	base->flUpMove = 0.f;
	base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);

	if (csgoInput) {
		__try {
			auto* p = reinterpret_cast<std::uint8_t*>(csgoInput);
			*reinterpret_cast<float*>(p + kCsgoInputForward) = 0.f;
			*reinterpret_cast<float*>(p + kCsgoInputLeft) = leftMove;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// Clear both, then hold the active side
	SetMoveButtonHeld(cmd, IN_MOVELEFT, false);
	SetMoveButtonHeld(cmd, IN_MOVERIGHT, false);
	if (leftMove > 0.f)
		SetMoveButtonHeld(cmd, IN_MOVELEFT, true);
	else if (leftMove < 0.f)
		SetMoveButtonHeld(cmd, IN_MOVERIGHT, true);
}

void Movement::OnCreateMove(CUserCmd* user_cmd)
{
	if (!user_cmd || g_bMenuOpen)
		return;

	if (I::EngineClient) {
		__try {
			if (!I::EngineClient->in_game() || !I::EngineClient->connected())
				return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
	}

	C_CSPlayerPawn* pawn = H::SafeLocalAlive();
	if (!pawn)
		return;

	// Jumpbug / bhop / silent WASD strafe run in hooks.cpp.
	// Here: mouse autostrafe only (mode 0).
	Vector_t vel{};
	std::uint32_t flags = 0;
	if (!ReadLiveMoveState(pawn, vel, flags))
		return;
	const bool onGround = (flags & FL_ONGROUND) != 0U;

	if (Config::autostrafe && Config::autostrafe_mode == 0
		&& user_cmd->csgoUserCmd.pBaseCmd) {
		__try {
			AutoStrafe(user_cmd, pawn, vel, onGround, flags);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("AutoStrafe", GetExceptionCode());
		}
	}
}

std::unique_ptr<Movement> g_movement = std::make_unique<Movement>();

