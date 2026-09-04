#define NOMINMAX
#include "fastladder.h"
#include "move_helpers.h"

#include "../../config/config.h"
#include "../../features/aim/aim_common.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../utils/schema/schema.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>

namespace FastLadder {
namespace {

bool g_wrote = false;
bool g_holdCam = false;
QAngle_t g_cam{};

constexpr float kMaxPitch = 89.f;

void NormalizeAngles(QAngle_t& ang)
{
	while (ang.y > 180.f)
		ang.y -= 360.f;
	while (ang.y < -180.f)
		ang.y += 360.f;
	while (ang.x > 89.f)
		ang.x -= 180.f;
	while (ang.x < -89.f)
		ang.x += 180.f;
	ang.z = 0.f;
}

void CameraSet(const QAngle_t& ang)
{
	if (!Input::SetViewAngle)
		return;
	const uintptr_t ctx = Input::viewAngleContext
		? Input::viewAngleContext
		: reinterpret_cast<uintptr_t>(Input::pCSGOInput);
	if (!ctx)
		return;
	Vector_t v{ ang.x, ang.y, 0.f };
	__try {
		Input::SetViewAngle(ctx, 0, &v);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

bool OnLadder(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return false;
	std::uint32_t off = SchemaFinder::Get(
		hash_32_fnv1a_const("C_BaseEntity->m_nActualMoveType"));
	if (!off)
		off = 0x526;
	std::uint8_t moveType = MOVETYPE_WALK;
	__try {
		moveType = *reinterpret_cast<std::uint8_t*>(
			reinterpret_cast<std::uintptr_t>(pawn) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return moveType == MOVETYPE_LADDER;
}

void ClearHold()
{
	g_wrote = false;
	g_holdCam = false;
}

} // namespace

bool WroteThisTick()
{
	return g_wrote;
}

bool PeekHold(QAngle_t& out)
{
	if (!g_holdCam || !g_cam.IsValid())
		return false;
	out = g_cam;
	return true;
}

void RestoreCamera()
{
	if (!g_holdCam || !g_cam.IsValid())
		return;
	CameraSet(g_cam);
}

void RestoreFrom(const QAngle_t& ang)
{
	if (!ang.IsValid())
		return;
	CameraSet(ang);
}

void Reset()
{
	ClearHold();
}

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	g_wrote = false;
	if (!cmd || !pawn || !Config::fastladder || !OnLadder(pawn)) {
		ClearHold();
		return;
	}

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base) {
		ClearHold();
		return;
	}

	const float forwardMove = base->flForwardMove;
	const float sideMove = base->flSideMove;
	if (forwardMove == 0.f && sideMove == 0.f) {
		ClearHold();
		return;
	}

	QAngle_t view{};
	if (!AimCommon::GetViewAngles(view) || !view.IsValid()) {
		ClearHold();
		return;
	}

	bool goingUp = false;
	if (std::fabsf(forwardMove) > 0.01f)
		goingUp = forwardMove > 0.f;
	else
		goingUp = view.x < 0.f;

	QAngle_t modified = view;
	modified.x = kMaxPitch;
	modified.y += goingUp ? -90.f : 90.f;
	NormalizeAngles(modified);

	MoveWrite::SetMove(cmd, base, -1.f, goingUp ? 1.f : -1.f);

	if (base->pViewAngles
		&& reinterpret_cast<std::uintptr_t>(base->pViewAngles) > 0x10000ull) {
		base->pViewAngles->angValue = modified;
		base->pViewAngles->SetBits(0x1u | 0x2u | 0x4u);
		base->SetBits(BASE_BITS_VIEWANGLES);
	}

	g_cam = view;
	g_holdCam = true;
	g_wrote = true;
}

} // namespace FastLadder

