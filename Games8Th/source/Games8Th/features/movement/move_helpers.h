#pragma once

// Shared cmd move-write helpers for movement features (edgestop / fastladder /
// slowwalk). Mirrors movement.cpp's private helpers - unit-scale (?1) proto
// moves + WASD button mirroring on cmd + pInButtonState.

#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../keybinds/keybinds.h"
namespace MoveWrite {

// Same gate as jumpbug: checkbox alone works when unbound + Hold mode.
inline bool FeatureOn(bool& feature)
{
	if (!feature)
		return false;
	if (keybind.isActive(feature))
		return true;
	const int k = keybind.getKey(feature);
	const int m = keybind.getMode(feature);
	return k <= 0 && m == static_cast<int>(KeyMode::
Hold);
}

inline void SyncButtons(CUserCmd* cmd, CBaseUserCmdPB* base, float fmove, float smove)
{
	constexpr std::uint64_t kWasd = IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;

	std::
uint64_t b = cmd->nButtons.nValue & ~kWasd;
	if (fmove > 0.f)
		b |= IN_FORWARD;
	else if (fmove < 0.f)
		b |= IN_BACK;
	// +smove = LEFT (movement.cpp convention: right = (-sy, cy))
	if (smove > 0.f)
		b |= IN_MOVELEFT;
	else if (smove < 0.f)
		b |= IN_MOVERIGHT;
	cmd->nButtons.nValue = b;
	cmd->nButtons.nValueChanged &= ~kWasd;
	cmd->nButtons.nValueScroll &= ~kWasd;

	if (base && base->pInButtonState) {
		std::
uint64_t pb = base->pInButtonState->nValue & ~kWasd;
		if (fmove > 0.f)
			pb |= IN_FORWARD;
		else if (fmove < 0.f)
			pb |= IN_BACK;
		if (smove > 0.f)
			pb |= IN_MOVELEFT;
		else if (smove < 0.f)
			pb |= IN_MOVERIGHT;
		base->pInButtonState->nValue = pb;
		base->pInButtonState->nValueChanged &= ~kWasd;
		base->pInButtonState->nValueScroll &= ~kWasd;
		base->pInButtonState->SetBits(
			BUTTON_STATE_PB_BITS_BUTTONSTATE1
			| BUTTON_STATE_PB_BITS_BUTTONSTATE2
			| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
	}
}

inline void SetMove(CUserCmd* cmd, CBaseUserCmdPB* base, float fmove, float smove)
{
	base->flForwardMove = fmove;
	base->flSideMove = smove;
	base->flUpMove = 0.f;
	base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);
	SyncButtons(cmd, base, fmove, smove);
}

} // namespace MoveWrite

