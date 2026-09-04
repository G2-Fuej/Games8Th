#define NOMINMAX
#include "jumpbug.h"

#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../prediction/prediction.h"
#include "../input_inject/input_inject.h"
#include "../trace/trace.h"
#include "../../utils/cvar/cvar.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace JumpBug {
namespace {

constexpr std::uintptr_t kFlagsOff = 0x3F4;
constexpr std::uintptr_t kAbsVelOff = 0x3F8;
constexpr std::uintptr_t kMoveSvcOff = 0x1248;
constexpr std::uintptr_t kMoveTraceCtxOff = 0x638;
constexpr std::uint64_t kJump = IN_JUMP;
constexpr std::uint64_t kDuck = IN_DUCK;
constexpr std::uint64_t kMaskPlayerSolid = 0x201400Bull;
constexpr float kTick = 1.f / 64.f;

bool g_jbFired = false;
bool g_ejFired = false;
bool g_wasAir = false;
bool g_claimedJump = false;
bool g_jbActive = false;
float g_landingFrac = 1.f;

std::uintptr_t DuckAmountOff()
{
	static std::uintptr_t s_off = 0;
	if (!s_off) {
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("CCSPlayer_MovementServices->m_flDuckAmount"));
		if (!s_off) s_off = 0x40C;
	}
	return s_off;
}

std::uintptr_t DuckSpeedOff()
{
	static std::uintptr_t s_off = 0;
	if (!s_off) {
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("CCSPlayer_MovementServices->m_flDuckSpeed"));
		if (!s_off) s_off = 0x410;
	}
	return s_off;
}

std::uintptr_t GravityScaleOff()
{
	static std::uintptr_t s_off = 0;
	if (!s_off) {
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_flGravityScale"));
		if (!s_off) s_off = 0x540;
	}
	return s_off;
}

std::uintptr_t StaminaOff()
{
	static std::uintptr_t s_off = 0;
	if (!s_off) {
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("CCSPlayer_MovementServices->m_flStamina"));
		if (!s_off) s_off = 0x694;
	}
	return s_off;
}

std::uintptr_t ModernJumpOff()
{
	static std::uintptr_t s_off = 0;
	if (!s_off) {
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("CCSPlayer_MovementServices->m_ModernJump"));
		if (!s_off) s_off = 0x6C8;
	}
	return s_off;
}

float ReadFloatSeh(void* base, std::uintptr_t off, float fallback)
{
	if (!base)
		return fallback;
	__try {
		const float v = *reinterpret_cast<float*>(
			reinterpret_cast<std::uintptr_t>(base) + off);
		return std::isfinite(v) ? v : fallback;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return fallback;
	}
}

void* GetMoveSvc(C_CSPlayerPawn* pawn)
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
			reinterpret_cast<std::uintptr_t>(pawn) + kMoveSvcOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return svc;
}

bool BadMoveType(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return false;
	__try {
		uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_nActualMoveType"));
		if (!mtOff)
			mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
		if (!mtOff)
			mtOff = 0x526;
		const uint8_t moveType = *reinterpret_cast<uint8_t*>(
			reinterpret_cast<uintptr_t>(pawn) + mtOff);
		return moveType == MOVETYPE_LADDER || moveType == MOVETYPE_NOCLIP;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

std::uint64_t CmdButtons(CUserCmd* cmd)
{
	if (!cmd)
		return 0;
	std::uint64_t buttons = cmd->nButtons.nValue;
	__try {
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pInButtonState)
				buttons |= base->pInButtonState->nValue;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return buttons;
}

struct EngineTraceResolvers {
	void* moveHull = nullptr;
	void* setCollision = nullptr;
	bool ready = false;
};

const EngineTraceResolvers& EngineTrace()
{
	static EngineTraceResolvers r;
	if (r.ready)
		return r;
	r.moveHull = M::FindPattern("client.dll",
		"48 89 74 24 ? 55 57 41 54 41 55 41 56 48 8D AC 24 ? ? ? ? 48 81 EC A0 01 00 00");
	r.setCollision = M::FindPattern("client.dll",
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 0F B6 41 ? 33 FF C7 41");
	r.ready = r.moveHull && r.setCollision;
	return r;
}

bool EngineLandingTrace(
	void* moveSvc,
	C_CSPlayerPawn* pawn,
	std::uint64_t mask,
	const Vector_t& start,
	const Vector_t& end,
	const Vector_t& mins,
	const Vector_t& maxs,
	float* outFrac,
	Vector_t* outNormal)
{
	*outFrac = 0.f;
	*outNormal = Vector_t{};
	const EngineTraceResolvers& r = EngineTrace();
	if (!r.ready || !moveSvc || !pawn)
		return false;

	const auto ctx = reinterpret_cast<std::uint8_t*>(moveSvc) + kMoveTraceCtxOff;
	if (!Mem::IsReadable(ctx, 0x650))
		return false;

	__try {
		alignas(16) std::uint8_t filter[0x80]{};
		reinterpret_cast<void(__fastcall*)(void*, void*, std::uint64_t, int)>(
			r.setCollision)(filter, pawn, mask, 11);
		float bbox[6] = { mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z };
		Trace::CGameTrace tr{};
		reinterpret_cast<void(__fastcall*)(void*, Trace::CGameTrace*,
			const Vector_t*, const Vector_t*, const float*, const void*)>(
			r.moveHull)(ctx, &tr, &start, &end, bbox, filter);
		const float frac = tr.fraction();
		const Vector_t n = tr.normal();
		if (!std::isfinite(frac) || !std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z))
			return false;
		*outFrac = frac;
		*outNormal = n;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ReadLive(C_CSPlayerPawn* pawn, Vector_t& vel, std::uint32_t& flags)
{
	vel = Vector_t{ 0.f, 0.f, 0.f };
	flags = 0;
	if (!pawn)
		return false;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(pawn);
		flags = *reinterpret_cast<std::uint32_t*>(base + kFlagsOff);
		const std::uint32_t schemaF = pawn->m_fFlags();
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

void SetBtn(CUserCmd* cmd, std::uint64_t button, bool down, bool edge)
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
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
		}
		base->SetBits(BASE_BITS_BUTTONPB);
	}
}

void InjectJumpEdge(CUserCmd* cmd, float when)
{
	SetBtn(cmd, kJump, true, true);
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		InputInject::ClearJumpSubticks(base);
		const float w = std::clamp(when, 0.f, 0.999f);
		InputInject::SubtickButton(base, kJump, true, w);
		InputInject::SanitizeSubticks(base);
	}
	g_claimedJump = true;
}

bool FeatureOn(bool& feature)
{
	if (!feature)
		return false;
	if (keybind.isActive(feature))
		return true;
	const int k = keybind.getKey(feature);
	const int m = keybind.getMode(feature);
	if (k <= 0 && m == static_cast<int>(KeyMode::Hold))
		return true;
	return false;
}

bool AboutToLeaveEdge(C_CSPlayerPawn* pawn, const Vector_t& vel, float /*viewYawDeg*/)
{
	if (!pawn)
		return false;

	const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
	if (speed2d < 1.f)
		return false;

	if (!Trace::Ready())
		Trace::Init();
	if (!Trace::Ready())
		return false;

	Vector_t origin{};
	Vector_t mins{ -16.f, -16.f, 0.f };
	Vector_t maxs{ 16.f, 16.f, 72.f };
	__try {
		origin = pawn->m_vOldOrigin();
		if (!std::isfinite(origin.x) || (origin.x == 0.f && origin.y == 0.f && origin.z == 0.f))
			origin = pawn->getPosition();

		if (CCollisionProperty* col = pawn->m_pCollision()) {
			const Vector_t cm = col->m_vecMins();
			const Vector_t cM = col->m_vecMaxs();
			if (std::isfinite(cm.x) && std::isfinite(cM.z) && cM.z > cm.z) {
				mins = cm;
				maxs = cM;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}

	const float standable = Cvar::Float("sv_standable_normal", 0.7f);

	auto checkEdge = [&](float fractionAhead) -> bool {
		const float bt = kTick * fractionAhead;
		const Vector_t predicted{ origin.x + vel.x * bt, origin.y + vel.y * bt, origin.z };
		const Vector_t start{ predicted.x, predicted.y, predicted.z + 4.f };
		const Vector_t end{ predicted.x, predicted.y, predicted.z - 36.f };
		Trace::CGameTrace tr{};
		if (!Trace::TraceHull(start, end, mins, maxs, pawn, tr, kMaskPlayerSolid))
			return true;
		if (tr.startsolid())
			return false;
		if (!Trace::DidHit(tr))
			return true;
		const float nz = tr.normal().z;
		return !std::isfinite(nz) || nz < standable;
	};

	// Check if leaving ground within next tick or half-tick
	const bool leavesEdgeSoon = checkEdge(1.0f) || checkEdge(1.5f);
	if (!leavesEdgeSoon)
		return false;

	// Make sure we are not just walking down a small slope/stair
	const Vector_t probeStart{ origin.x, origin.y, origin.z + 4.f };
	const Vector_t probeEnd{ origin.x + vel.x * kTick, origin.y + vel.y * kTick, origin.z - 48.f };
	Trace::CGameTrace groundTr{};
	if (Trace::TraceHull(probeStart, probeEnd, mins, maxs, pawn, groundTr, kMaskPlayerSolid)) {
		if (Trace::DidHit(groundTr) && groundTr.normal().z >= standable) {
			const float drop = origin.z - groundTr.endpos().z;
			if (drop < 18.f)
				return false; // Shallow stair / step down
		}
	}

	return true;
}

bool ReadYaw(CUserCmd* cmd, float& yaw)
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
	return false;
}

// mercey jumpbug::get_impulse_mul - 1:1
float GetImpulseMul(C_CSPlayerPawn* pawn)
{
	void* movement_services = GetMoveSvc(pawn);
	if (!movement_services)
		return 0.0f;

	const float stamina = ReadFloatSeh(movement_services, StaminaOff(), 0.f);

	if (Cvar::Float("sv_legacy_jump", 0.f) > 0.5f) {
		if (stamina <= 0.0f)
			return 1.0f;
		return std::clamp(1.0f - (stamina / 100.0f), 0.0f, 1.0f);
	}

	int current_tick = 0;
	if (void* gv = Pred::Engines().globalVars) {
		__try {
			current_tick = *reinterpret_cast<int*>(
				reinterpret_cast<std::uintptr_t>(gv) + 0x44);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			current_tick = 0;
		}
	}

	const auto modern_jump = reinterpret_cast<std::uintptr_t>(movement_services) + ModernJumpOff();
	const float landing_vel_z = ReadFloatSeh(
		reinterpret_cast<void*>(modern_jump), 0x30, 0.f);
	std::uint32_t landed_tick = 0;
	__try {
		landed_tick = *reinterpret_cast<std::uint32_t*>(modern_jump + 0x20);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		landed_tick = 0;
	}

	const auto base = std::clamp((landing_vel_z * 0.0005f) + 1.0f, 0.02f, 1.0f);
	const auto ticks_since_landing = static_cast<float>(current_tick - static_cast<int>(landed_tick));

	auto result = std::clamp(base + (ticks_since_landing * 0.6f), 0.0f, 1.0f);

	if (stamina > 0.0f)
		result *= std::clamp(1.0f - (stamina / 100.0f), 0.0f, 1.0f);

	return result;
}

// mercey / maycry jumpbug::on_create_move - 1:1
void RunMerceyJumpbug(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	g_jbActive = false;

	const bool jbActive = FeatureOn(Config::jumpbug);
	if (!jbActive && !Config::jumpbug)
		return;

	const std::uint64_t buttons = CmdButtons(cmd);
	if (!jbActive && !(buttons & kJump))
		return;

	if (BadMoveType(pawn))
		return;

	// Use predicted prestate or networked pawn state - NOT interpolated render origin/velocity
	const auto& prestate = Pred::Last();
	const std::uint32_t netFlags = prestate.valid ? prestate.flags : pawn->m_fFlags();
	if (netFlags & FL_ONGROUND)
		return;

	Vector_t netVel{};
	if (prestate.valid && std::isfinite(prestate.velocity.x) && (prestate.velocity.x != 0.f || prestate.velocity.y != 0.f || prestate.velocity.z != 0.f))
		netVel = prestate.velocity;
	else {
		__try { netVel = pawn->m_vecVelocity(); } __except (EXCEPTION_EXECUTE_HANDLER) { netVel = Vector_t{}; }
		if (!std::isfinite(netVel.x) || (netVel.x == 0.f && netVel.y == 0.f && netVel.z == 0.f)) {
			__try { netVel = pawn->m_vecAbsVelocity(); } __except (EXCEPTION_EXECUTE_HANDLER) { netVel = Vector_t{}; }
		}
	}
	if (!std::isfinite(netVel.x) || !std::isfinite(netVel.y) || !std::isfinite(netVel.z))
		return;
	if (netVel.z > 0.0f)
		return;

	void* movement_services = GetMoveSvc(pawn);
	if (!movement_services)
		return;

	const float duck_amount = ReadFloatSeh(movement_services, DuckAmountOff(), 0.f);
	(void)ReadFloatSeh(movement_services, DuckSpeedOff(), 0.f);
	(void)GetImpulseMul(pawn);
	const bool holding_duck = (buttons & kDuck) != 0;

	Vector_t mins{ -16.f, -16.f, 0.f };
	Vector_t maxs{ 16.f, 16.f, 72.f };
	__try {
		if (CCollisionProperty* col = pawn->m_pCollision()) {
			const Vector_t cm = col->m_vecMins();
			const Vector_t cM = col->m_vecMaxs();
			if (std::isfinite(cm.x) && std::isfinite(cM.z) && cM.z > cm.z) {
				mins = cm;
				maxs = cM;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}

	// Origin: prefer prestate.origin (networked) -> m_vOldOrigin -> getPosition fallback
	Vector_t trace_origin{};
	if (prestate.valid && std::isfinite(prestate.origin.x) && (prestate.origin.x != 0.f || prestate.origin.y != 0.f || prestate.origin.z != 0.f))
		trace_origin = prestate.origin;
	else {
		__try { trace_origin = pawn->m_vOldOrigin(); } __except (EXCEPTION_EXECUTE_HANDLER) { trace_origin = Vector_t{}; }
		if (!std::isfinite(trace_origin.x) || (trace_origin.x == 0.f && trace_origin.y == 0.f && trace_origin.z == 0.f))
			trace_origin = pawn->getPosition();
	}

	if (holding_duck && duck_amount > 0.0f) {
		const float standing_height = 72.0f;
		const float duck_hull_diff = standing_height - maxs.z;
		trace_origin.z -= duck_hull_diff * 0.5f;
		maxs.z = standing_height;
	}

	std::uint64_t trace_mask = 0;
	__try {
		// maycry order: null-check before deref, then the duck-clip bit OR.
		void* pawn_ptr = *reinterpret_cast<void**>(
			reinterpret_cast<std::uintptr_t>(movement_services) + 56);
		if (pawn_ptr)
			trace_mask = *reinterpret_cast<std::uint64_t*>(
				reinterpret_cast<std::uint8_t*>(pawn_ptr) + 0xd48);
		if (!pawn_ptr || (*reinterpret_cast<std::uint32_t*>(
			reinterpret_cast<std::uint8_t*>(pawn_ptr) + 0x3f8) & 0x10))
			trace_mask |= 0x20ull;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		trace_mask = kMaskPlayerSolid;
	}
	if (!trace_mask)
		trace_mask = kMaskPlayerSolid;

	const float sv_gravity = Cvar::Float("sv_gravity", 800.f);
	const float sv_standable_normal = Cvar::Float("sv_standable_normal", 0.7f);
	const float gravity_scale = ReadFloatSeh(pawn, GravityScaleOff(), 1.f);

	Vector_t velocity = netVel;
	velocity.z -= (gravity_scale * sv_gravity * kTick) * 0.5f;

	const Vector_t trace_start = trace_origin;
	Vector_t trace_end{};
	trace_end.x = trace_origin.x + velocity.x * kTick;
	trace_end.y = trace_origin.y + velocity.y * kTick;
	trace_end.z = trace_origin.z + velocity.z * kTick;
	trace_end.z -= 2.0f;

	float frac = 0.f;
	Vector_t normal{};
	bool traced = EngineLandingTrace(movement_services, pawn, trace_mask,
		trace_start, trace_end, mins, maxs, &frac, &normal);
	if (!traced) {
		if (!Trace::Ready())
			Trace::Init();
		Trace::CGameTrace tr{};
		if (!Trace::TraceHull(trace_start, trace_end, mins, maxs, pawn, tr, trace_mask))
			return;
		frac = tr.fraction();
		normal = tr.normal();
		if (!std::isfinite(frac) || !std::isfinite(normal.z))
			return;
	}

	const bool valid_trace = (frac > 0.0f && frac < 1.0f);
	if (!valid_trace || normal.z < sv_standable_normal)
		return;

	const float dot = velocity.x * normal.x + velocity.y * normal.y;
	g_jbActive = normal.z >= 0.98f || dot >= 0.0f;

	const float when = std::clamp(frac, 0.001f, 0.99f);
	g_landingFrac = when;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base)
		return;

	if (normal.z < 0.985f)
		g_jbActive = false;

	// Jumpbug 4-step subtick injection: duck_down@0, duck_up@when, jump_up@when, jump_down@when
	SetBtn(cmd, kJump, true, true);
	const int oldSize = base->subtickMovesField.nCurrentSize;
	const bool wroteDuckDown = InputInject::SubtickButton(base, kDuck, true, 0.0f);
	const bool wroteDuckUp = InputInject::SubtickButton(base, kDuck, false, when);
	const bool wroteJumpUp = InputInject::SubtickButton(base, kJump, false, when);
	const bool wroteJumpDown = InputInject::SubtickButton(base, kJump, true, when);
	if (!wroteDuckDown || !wroteDuckUp || !wroteJumpUp || !wroteJumpDown) {
		if (base->subtickMovesField.nCurrentSize >= oldSize)
			base->subtickMovesField.nCurrentSize = oldSize;
		g_jbActive = false;
		return;
	}
	InputInject::SanitizeSubticks(base);

	g_claimedJump = true;
}

} // namespace

void Reset()
{
	g_jbFired = false;
	g_ejFired = false;
	g_wasAir = false;
	g_claimedJump = false;
	g_jbActive = false;
	g_landingFrac = 1.f;
}

bool ClaimedJumpThisTick()
{
	return g_claimedJump;
}

bool ActiveThisTick()
{
	return g_jbActive;
}

float LandingFraction()
{
	return g_landingFrac;
}

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	g_claimedJump = false;
	g_jbActive = false;
	g_landingFrac = 1.f;
	if (!cmd || !pawn)
		return;

	const bool wantEj = FeatureOn(Config::edgejump);
	const bool wantJb = FeatureOn(Config::jumpbug) || Config::jumpbug;
	if (!wantEj && !wantJb)
		return;

	int hp = 0;
	__try { hp = pawn->m_iHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (hp <= 0 || hp > 200)
		return;

	if (wantEj) {
		Vector_t vel{};
		std::uint32_t flags = 0;
		if (ReadLive(pawn, vel, flags)) {
			const bool onGround = (flags & FL_ONGROUND) != 0U;
			if (onGround) {
				if (g_wasAir) {
					g_ejFired = false;
				}
				g_wasAir = false;
			} else {
				g_wasAir = true;
			}

			if (onGround && !g_ejFired) {
				float yaw = 0.f;
				ReadYaw(cmd, yaw);
				if (AboutToLeaveEdge(pawn, vel, yaw)) {
					g_ejFired = true;
					InjectJumpEdge(cmd, 0.f);
					return;
				}
			}
		}
	}

	if (wantJb)
		RunMerceyJumpbug(cmd, pawn);
}

} // namespace JumpBug

