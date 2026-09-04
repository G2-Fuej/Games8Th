#include "hooks.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <atomic>

#include "../../Games8Th/utils/memory/Interface/Interface.h"
#include "../utils/memory/patternscan/patternscan.h"
#include "../utils/console/console.h"
#include "../utils/crypto/xorstr.h"

#include "../features/visuals/visuals.h"
#include "../features/chams/chams.h"

#include "../../cs2/datatypes/cutlbuffer/cutlbuffer.h"
#include "../../cs2/datatypes/keyvalues/keyvalues.h"
#include "../../cs2/entity/C_Material/C_Material.h"
#include "../../cs2/sdk/IGameEvent.h"

#include "../config/config.h"
#include "../keybinds/keybinds.h"
#include "../interfaces/interfaces.h"
#include "../utils/schema/schema.h"
#include "../utils/fnv1a/fnv1a.h"
#include "../features/aim/aim.h"
#include "../features/aim/aim_common.h"
#include "../features/anti_aim/anti_aim.h"
#include "../features/backtrack/backtrack.h"
#include "../features/movement/movement.h"
#include "../features/movement/jumpbug.h"
#include "../features/movement/fastladder.h"
#include "../features/world/world.h"
#include "../features/world/weather.h"
#include "../features/hitmarker/hitmarker.h"
#include "../features/bullet_impact/bullet_impact.h"
#include "../features/hitsound/hitsound.h"
#include "../features/sound_esp/sound_esp.h"
#include "../features/vote/vote.h"
#include "../features/panorama/panorama.h"
#include "../features/scoreboard_weapons/scoreboard_weapons.h"
#include "../features/glow/glow.h"
#include "../features/gamemode/gamemode.h"
#include "../features/autowall/autowall.h"
#include "../features/grenade_helper/grenade_helper.h"
#include "../features/prediction/prediction.h"
#include "../features/engine2/engine2.h"
#include "../features/w2s/w2s.h"
#include "../features/bomb/bomb.h"
#include "../features/auto_pistol/auto_pistol.h"
#include "../features/enemy_spec/enemy_spec.h"
#include "../features/sdk_prio_a/sdk_prio_a.h"
#include "../features/cl_bypass/cl_bypass.h"
#include "../features/skinchanger/skinchanger.h"

#include "../features/input_inject/input_inject.h"
#include "../features/subtick_move/subtick_move.h"
#include "../../cs2/datatypes/viewmatrix/viewmatrix.h"
#include "../features/bones/bones.h"
#include "../utils/memory/memsafe/memsafe.h"

C_CSPlayerPawn* H::SafeLocalPlayer() noexcept
{
	if (!oGetLocalPlayer)
		return nullptr;
	C_CSPlayerPawn* p = nullptr;
	__try {
		p = oGetLocalPlayer(0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!p)
		return nullptr;
	// range+vtable only - no VirtualQuery (hot path)
	const auto a = reinterpret_cast<std::uintptr_t>(p);
	if (a < 0x10000ull || a > 0x00007FFFFFFFFFFFull)
		return nullptr;
	void* vt = nullptr;
	__try {
		vt = *reinterpret_cast<void**>(p);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	const auto va = reinterpret_cast<std::uintptr_t>(vt);
	if (va < 0x10000ull || va > 0x00007FFFFFFFFFFFull)
		return nullptr;

	// TDM death/respawn: probe health read - free'd pawn fails SEH / garbage hp.
	// Dead pawns still returned (EnemySpec); combat uses SafeLocalAlive.
	int hp = 0;
	__try {
		hp = p->m_iHealth();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (hp < 0 || hp > 200)
		return nullptr;
	return p;
}

// Alive local only - CreateMove / combat / skin apply. Spec uses SafeLocalPlayer.
C_CSPlayerPawn* H::SafeLocalAlive() noexcept
{
	C_CSPlayerPawn* p = SafeLocalPlayer();
	if (!p)
		return nullptr;
	int hp = 0;
	std::uint8_t life = 1;
	__try {
		hp = p->m_iHealth();
		life = p->m_lifeState();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (hp <= 0 || hp > 200 || life != 0)
		return nullptr;
	return p;
}

// Dump client_dll.hpp: CPlayer_MovementServices::m_nLastCommandNumberProcessed @ 0x188.
// CreateMove join gate: pawn exists but ProcessMovement hasn't run.
// Schema-first: the Aug-22 update proved frozen dump offsets rot silently.
static bool LocalPawnCmdReady(C_CSPlayerPawn* p) noexcept
{
	if (!p || !Mem::ValidEntity(p))
		return false;
	void* move = nullptr;
	__try { move = p->m_pMovementServices(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!move || !Mem::IsUserPtr(move))
		return false;
	static std::uint32_t s_off = 0;
	if (!s_off) {
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("CPlayer_MovementServices->m_nLastCommandNumberProcessed"));
		if (!s_off) s_off = 0x188u;
	}
	std::uint32_t last = 0;
	if (!Mem::ReadField(move, s_off, last))
		return false;
	return last != 0;
}

// Menu input block hooks - do NOT ShowCursor here (refcount spam); Present owns cursor.
void __fastcall H::hkIsRelativeMouseMode(void* pInputSystem, bool active)
{
	H::Hooks::EnterHook();
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		auto orig = IsRelativeMouseMode.GetOriginal();
		if (orig) { __try { orig(pInputSystem, active); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		H::Hooks::LeaveHook(); return;
	}
	if (pInputSystem)
		g_pInputSystem = pInputSystem;

	// Track preferred mode for restore-on-close:
	// - Menu closed: trust every game request
	// - Menu open: only latch true (ignore forced-false noise while UI is up)
	if (!g_bMenuOpen.load(std::memory_order_relaxed) || active)
		g_wantRelativeMouse = active;

	auto orig = IsRelativeMouseMode.GetOriginal();
	if (!orig) { H::Hooks::LeaveHook(); return; }

	// Menu open: force absolute mouse for ImGui cursor; closed: honor game
	const bool menuOpen = g_bMenuOpen.load(std::memory_order_relaxed);
	__try { orig(pInputSystem, menuOpen ? false : active); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("IsRelativeMouseMode", GetExceptionCode()); }
	H::Hooks::LeaveHook();
}

bool __fastcall H::hkMouseInputEnabled(void* rcx)
{
	H::Hooks::EnterHook();
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		bool ret = true;
		auto orig = MouseInputEnabled.GetOriginal();
		if (orig) { __try { ret = orig(rcx); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		H::Hooks::LeaveHook(); return ret;
	}
	// Block game mouse look/click while menu is up (ImGui uses WndProc path)
	if (g_bMenuOpen.load(std::memory_order_relaxed)) { H::Hooks::LeaveHook(); return false; }

	auto orig = MouseInputEnabled.GetOriginal();
	if (orig) {
		bool ret = true;
		__try { ret = orig(rcx); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("MouseInputEnabled", GetExceptionCode()); }
		H::Hooks::LeaveHook(); return ret;
	}
	H::Hooks::LeaveHook();
	return true;
}

static void sehFrameStage(void* a1, int stage) {
	(void)a1;
	__try {
		const bool leaving = H::SessionMapLeaving();
		// Park blocks entity/world work until next spawn. Auto-accept still
		// needs OnFrame in the main-menu queue (SessionLive is false there).
		if ((!leaving || !H::SessionLive())
			&& (stage == FRAME_NET_UPDATE_START || stage == FRAME_RENDER_START)) {
			__try { Panorama::OnFrame(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Panorama.OnFrame", GetExceptionCode()); }
		}

		// LevelShutdown can miss (pattern / listen-server). Drain game-thread
		// Aim/Pred wipe here so Present-only leave does not leave stale pawn*.
		if (stage == FRAME_NET_UPDATE_END) {
			if (leaving)
				H::SessionDrainGameLeave();
			H::SessionDrainDeath();
		}

		// Spec while dead - must run before EntityOk return.
		if (!leaving && H::SessionEntityReady()
			&& (stage == FRAME_RENDER_START || stage == FRAME_RENDER_END)) {
			if (stage == FRAME_RENDER_START) {
				__try { EnemySpec::OnFrame(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("EnemySpec", GetExceptionCode()); }
			}
		}

		// Weather: signon>=6. Don't wait for a gun or team intro.
		if (!leaving && H::SessionEntityReady() && !H::SessionPostMatch()
			&& stage == FRAME_NET_UPDATE_END) {
			__try { SdkPrioA::WarmWorldScan(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("SdkPrioA.WarmWorld", GetExceptionCode()); }
			__try { World::WarmScan(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("World.WarmScan", GetExceptionCode()); }
			World::Weather::WarmTick();
			if (H::SessionLive()) {
				__try { World::Weather::Update(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Weather.FSN_net", GetExceptionCode()); }
			}
		}

		// Panorama updates its scoreboard tree during render start. Running this
		// at network-update end races the row panels and silently loses the update.
		if (!leaving && H::SessionEntityReady() && !H::SessionPostMatch()
			&& stage == FRAME_RENDER_START) {
			__try { ScoreboardWeapons::OnFrameStageNotify(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("ScoreboardWeapons", GetExceptionCode()); }
		}

		__try { AutoWall::TickXhairCache(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("AutoWall.Tick", GetExceptionCode()); }

		// Combat bodies: alive pawn with scene. Intro/weapon wait was join lag.
		if (!H::SessionEntityOk() || leaving)
			return;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("FrameStageNotify", GetExceptionCode());
	}
}

// Full input strip while menu is open (attack, move, look, weapon scroll, etc.)
static void strip_menu_input(CUserCmd* cmd)
{
	if (!cmd)
		return;

	// Zero every button path - wheel weapon-switch often lands in nValueScroll
	cmd->nButtons.nValue = 0;
	cmd->nButtons.nValueChanged = 0;
	cmd->nButtons.nValueScroll = 0;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (base) {
		base->flForwardMove = 0.f;
		base->flSideMove = 0.f;
		base->flUpMove = 0.f;
		base->nMousedX = 0;
		base->nMousedY = 0;
		base->nImpulse = 0;
		// Mouse wheel -> weapon select; must clear or scroll still swaps guns
		base->nWeaponSelect = 0;

		// IDA: buttons_pb @ +0x38; skip if unset / obviously bad
		if (base->pInButtonState
			&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
			base->pInButtonState->nValue = 0;
			base->pInButtonState->nValueChanged = 0;
			base->pInButtonState->nValueScroll = 0;
		}

		auto& field = base->subtickMovesField;
		if (field.pRep && field.nCurrentSize > 0 && field.nCurrentSize <= 64
			&& field.pRep->nAllocatedSize > 0 && field.pRep->nAllocatedSize <= 128) {
			const int n = (field.nCurrentSize < field.pRep->nAllocatedSize)
				? field.nCurrentSize : field.pRep->nAllocatedSize;
			for (int i = 0; i < n; ++i) {
				CSubtickMoveStep* step = field.pRep->tElements[i];
				if (!step)
					continue;
				step->nButton = 0;
				step->bPressed = false;
			}
		}
	}

	cmd->csgoUserCmd.nAttack1StartHistoryIndex = -1;
	cmd->csgoUserCmd.nAttack2StartHistoryIndex = -1;
}

static void sehCreateMovePost(CUserCmd* user_cmd)
{
	__try {
		if (!user_cmd)
			return;

		if (!I::EngineClient && !Engine2::NetworkGameClient())
			return;

		if (!H::SessionEntityReady())
			return;

		// Alive only - dead/respawn pawn has free'd weapon services (TDM crash)
		C_CSPlayerPawn* pLocalPawn = H::SafeLocalAlive();
		if (!pLocalPawn)
			return;
		if (!LocalPawnCmdReady(pLocalPawn))
			return;

		// Clear subtick queue before feature mutations
		CL_Bypass::PreClientCreateMove(user_cmd);

		// Wipe ALL engine-packed subtick steps before the
		// feature pipeline. Held-space / key-event / engine analog garbage in the
		// list desyncs server-side land + move state; features re-add their own.
		__try {
			InputInject::ClearAllSubticks(user_cmd->csgoUserCmd.pBaseCmd);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("CM.desubtick", GetExceptionCode());
		}

		// Strip first so wheel/attack/move die while menu open
		if (g_bMenuOpen) {
			strip_menu_input(user_cmd);
			// Clear stale aim/trigger shoot flags (CreateMove used to skip Aimbot while open)
			Aimbot(user_cmd);
		}

		if (g_bMenuOpen) {
			CL_Bypass::PostClientCreateMove(Input::pCSGOInput, user_cmd);
			return;
		}

		// Order: jumpbug/edgejump -> bhop -> strafe. Bhop/strafe MUST run before
		// any Pred::Start this cmd - both read Pred::Last() as THIS cmd's
		// pre-sim state (the previous cmd's post-sim capture, per their
		// contract comments). Capturing first fed them the current post-sim
		// state, so the subtick landing jump was written one tick early and
		// bhop never hopped.
		if (g_movement)
			g_movement->OnCreateMove(user_cmd);

		if (Config::jumpbug || Config::edgejump) {
			__try { JumpBug::OnCreateMove(user_cmd, pLocalPawn); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("JumpBug", GetExceptionCode()); }
		}

		if (Config::bhop) {
			__try {
				SubtickMove::RewriteBhop(user_cmd, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("SubtickBhop", GetExceptionCode());
			}
		}

		// WASD strafe - same pre-sim contract as bhop. The yaw steps must be
		// in the cmd before the prediction/RunCommand pass reads them.
		if (Config::autostrafe && Config::autostrafe_mode == 1) {
			__try {
				SubtickMove::RewriteStrafe(user_cmd, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("SubtickStrafe", GetExceptionCode());
			}
		}

		// Refresh the shared prediction snapshot AFTER movement - bhop/strafe
		// read it next cmd as their pre-sim state. Combat re-captures inside
		// its own Start below when active.
		{
			const bool predOnMove = Pred::Start(user_cmd);
			if (predOnMove)
				Pred::End();
		}

		// Match-end: still walk. Aim/knife/bomb walk dying pawn lists.
		if (H::SessionPostMatch()) {
			if (user_cmd->csgoUserCmd.pBaseCmd)
				InputInject::SanitizeSubticks(user_cmd->csgoUserCmd.pBaseCmd);
			CL_Bypass::PostClientCreateMove(Input::pCSGOInput, user_cmd);
			return;
		}

		// Snapshot live post-CM state for aim/AF/TR. Movement already ran.
		// Do not pred for jumpbug/edge - those used to force a second RunCommand
		// every hop and tripped the slot-4 / full-repredict dsync.
        const bool needPred = AimCommon::CombatActive();
		const bool predOn = needPred ? Pred::Start(user_cmd) : false;

		AimCommon::CollectAimTargets(pLocalPawn);

		__try { Backtrack::Tick(pLocalPawn); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("CM.BacktrackTick"); }

		AimCommon::BindCmd(user_cmd);
		Aimbot(user_cmd);
		AimHumanize_OnCreateMove(user_cmd);
		AimCommon::UnbindCmd();

		// Backtrack claim - rewinds input history entries and sets attack start indices for server lagcomp
		if (Config::backtrack && user_cmd) {
			__try { Backtrack::ApplyCommand(user_cmd); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("CM.BacktrackApply"); }
		}

		if (Config::auto_pistol)
			AutoPistol::OnCreateMove(pLocalPawn, user_cmd);

		if (Config::auto_defuse) {
			__try {
				if (void* c4 = Bomb::PlantedC4Entity())
					Bomb::TryStartDefuse(c4, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				TW_SEH_CATCH("CM.AutoDefuse");
			}
		}

		if (predOn)
			Pred::End();

		// After combat so aim cannot overwrite the 89?/?90 remap. Before the
		// analog wipe so WASD is still in the cmd (wipe would early-out).
		__try { FastLadder::OnCreateMove(user_cmd, pLocalPawn); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("FastLadder", GetExceptionCode()); }

		// Apply fake view angles after movement helpers so the final command keeps
		// the old UI's Anti-Aim behavior and the movement correction is authoritative.
		__try { AntiAim::OnCreateMove(user_cmd); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("AntiAim", GetExceptionCode()); }

		// Sanitize: keep flWhen==1.0 (UC release); do not clamp to 0.999
		if (user_cmd->csgoUserCmd.pBaseCmd) {
			CBaseUserCmdPB* finalBase = user_cmd->csgoUserCmd.pBaseCmd;
			// Final subtick: steps exist but the subtick
			// strafer did not handle this tick -> drop base moves (server uses
			// the subtick path). Skip for mouse strafe (mode 0) which owns
			// flSideMove itself. Skip when fastladder just remapped analog.
			__try {
				if (finalBase->subtickMovesField.nCurrentSize > 0
					&& !SubtickMove::HandledThisTick()
					&& !FastLadder::WroteThisTick()
					&& !(Config::autostrafe && Config::autostrafe_mode == 0)) {
					finalBase->flForwardMove = 0.f;
					finalBase->flSideMove = 0.f;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("CM.finalSubtick", GetExceptionCode());
			}

			if (!FastLadder::WroteThisTick() && !JumpBug::ClaimedJumpThisTick()) {
				__try {
					InputInject::EnsureAnalogMoveStep(user_cmd, pLocalPawn);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					Con::Seh("CM.analogStep", GetExceptionCode());
				}
			}
			if (!JumpBug::ClaimedJumpThisTick())
				InputInject::SanitizeSubticks(finalBase);
		}

		// Buttons/subtick flush only - CRC rewrite stays disabled (serialize-safe)
		CL_Bypass::PostClientCreateMove(Input::pCSGOInput, user_cmd);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		AimCommon::UnbindCmd();
		Pred::End();
		Con::Seh("CreateMove post", GetExceptionCode());
	}
}

// RAII depth guard - exception-safe counterpart to Enter/LeaveHook. Detour
// bodies themselves stay plain __try (no C2712); the guard lives in a thin
// non-SEH wrapper so an unwind can never leak s_hookDepth.
namespace {
	struct HookScopeGuard {
		bool m_armed = false;
		void arm() noexcept { H::Hooks::EnterHook(); m_armed = true; }
		~HookScopeGuard() { if (m_armed) H::Hooks::LeaveHook(); }
	};
}

static bool CreateMoveBody(void* pInput, int slot, bool active)
{
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		bool _ret = true;
		if (auto orig = H::CreateMove.GetOriginal()) {
			__try { _ret = orig(pInput, slot, active); } __except (EXCEPTION_EXECUTE_HANDLER) { _ret = true; }
		}
		return _ret;
	}

	// Pre-CM: pack CCSGOInput + moveSvc for ProcessMovement (runs inside original).
	// LUT: only +0x258 = code 3. Held +0x250|+0x258 = code 1 (dead hop).
	if (pInput)
		Input::pCSGOInput = pInput;

	const bool sessionOk = H::SessionEntityOk() && !H::SessionMapLeaving();

	// Undo last tick's cmd pitch-89 before original copies it onto the camera.
	if (slot == 0) {
		__try { FastLadder::RestoreCamera(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("FastLadder.preCam", GetExceptionCode()); }
	}

	// MUST return original bool. Dropping RAX (always true) corrupts
	// CCSGOInput / secure join after inject.
	bool origRet = true;
	if (H::CreateMove.IsHooked()) {
		auto original = H::CreateMove.GetOriginal();
		if (original) {
			CL_Bypass::SetInOriginalCreateMove(true);
			origRet = original(pInput, slot, active);
			CL_Bypass::SetInOriginalCreateMove(false);
		}
	}

	if (slot != 0) return origRet;
	H::SessionDrainDeath();
	if (H::SessionMapLeaving()) {
		H::SessionDrainGameLeave();
		return origRet;
	}
	if (!sessionOk) return origRet;
	if (!LocalPawnCmdReady(H::SafeLocalAlive())) return origRet;

	CUserCmd* user_cmd = Input::get_user_cmd(0);
	if (!user_cmd) {
		static int s_nullCount = 0;
		if (s_nullCount < 5) {
			++s_nullCount;
			Con::Error(
				"get_user_cmd null (SetupCmd=%p GetCmd=%p Array=%p Tick=%p Table=%p)",
				(void*)Input::SetupCmd,
				(void*)Input::GetCUserCmdBySequenceNumber,
				(void*)Input::GetCUserCmdArray,
				(void*)Input::GetEntityCmdSlot,
				Input::ppUserCmdArrayTable);
		}
		return origRet;
	}

	sehCreateMovePost(user_cmd);
	return origRet;
}

bool __fastcall H::hkCreateMove(void* pInput, int slot, bool active)
{
	HookScopeGuard guard;
	guard.arm();
	return CreateMoveBody(pInput, slot, active);
}

void __fastcall H::hkHandleViewAngles(void* thisptr, int slot)
{
	H::Hooks::EnterHook();
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		auto original = HandleViewAngles.GetOriginal();
		if (original) { __try { original(thisptr, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		H::Hooks::LeaveHook(); return;
	}
	QAngle_t saved{};
	const bool have = AimCommon::GetViewAngles(saved) && saved.IsValid();
	if (HandleViewAngles.IsHooked()) {
		if (auto original = HandleViewAngles.GetOriginal()) {
			__try { original(thisptr, slot); }
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("HandleViewAngles original", GetExceptionCode());
			}
		}
	}
	if (have)
		FastLadder::RestoreFrom(saved);
	H::Hooks::LeaveHook();
}

void __fastcall H::hkSetViewAngle(void* thisptr, int slot, Vector_t* ang)
{
	H::Hooks::EnterHook();
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		if (SetViewAngle.IsHooked()) {
			auto o = SetViewAngle.GetOriginal();
			if (o) { __try { o(thisptr, slot, ang); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		}
		H::Hooks::LeaveHook(); return;
	}
	auto original = SetViewAngle.IsHooked() ? SetViewAngle.GetOriginal() : nullptr;
	if (!original) { H::Hooks::LeaveHook(); return; }

	// ponytail: manual-map has no TLS - use static + TID guard (game thread only, re-entry guard)
	static std::atomic<DWORD> s_restoringTid{0};
	const DWORD tid = ::GetCurrentThreadId();
	const bool restoring = s_restoringTid.load(std::memory_order_relaxed) == tid;
	if (restoring) {
		__try { original(thisptr, slot, ang); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("SetViewAngle restore", GetExceptionCode()); }
		H::Hooks::LeaveHook(); return;
	}

	__try { original(thisptr, slot, ang); }
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("SetViewAngle original", GetExceptionCode());
		H::Hooks::LeaveHook(); return;
	}

	if (slot != 0 || !ang) { H::Hooks::LeaveHook(); return; }
	QAngle_t hold{};
	if (!FastLadder::PeekHold(hold)) { H::Hooks::LeaveHook(); return; }
	if (std::fabs(ang->x - 89.f) > 0.05f) { H::Hooks::LeaveHook(); return; }

	Vector_t v{ hold.x, hold.y, 0.f };
	s_restoringTid.store(tid, std::memory_order_relaxed);
	__try { original(thisptr, slot, &v); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("SetViewAngle hold", GetExceptionCode()); }
	s_restoringTid.store(0, std::memory_order_relaxed);
	H::Hooks::LeaveHook();
}

static void FrameStageBody(void* a1, int stage)
{
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		if (auto orig = H::FrameStageNotify.GetOriginal()) { __try { orig(a1, stage); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		return;
	}
	// Null original = pattern miss / disable race - never call through nullptr
	if (H::FrameStageNotify.IsHooked()) {
		if (auto original = H::FrameStageNotify.GetOriginal()) {
			__try { original(a1, stage); }
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("FrameStageNotify original", GetExceptionCode());
			}
		}
	}
	// Apply after the engine's network/render update. Applying before the
	// original lets the game overwrite item views and models in the same stage.
	__try { SkinChanger::OnFrameStageNotify(stage); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("SkinChanger", GetExceptionCode()); }
	sehFrameStage(a1, stage);
}

void __fastcall H::hkFrameStageNotify(void* a1, int stage)
{
	HookScopeGuard guard;
	guard.arm();
	FrameStageBody(a1, stage);
}

/* REMOVED-FEATURE-HOOK (not in current hooks.h)
*/

// IDA CCSPlayer_WeaponServices::GetInterpolatedShootPosition @ 0x1808C2D10.
// CSBaseGunFire resolves the fire origin through the weaponServices shoot-position
// ring (32 x 20B @ +0xE8, head +0x368, count +0x36C) at time [ #tick + frac ].
// The engine's ring writer stores whole-tick entries only (frac 0.000); a
// seed-nospread fire at sub-tick frac (e.g. 0.510) has no matching entry and no
// later entry to interpolate with -> console spam:
// "[Shooting] cl: ... didn't find a shoot position history entry for
// [ #34938 + 0.510 ] -- history only has entries [ #34916 + 0.000 ] to
// [ #34938 + 0.000 ] ..."
// Fix: stamp the EXACT requested (tick, frac) with the current eye into the ring
// right before the original lookup. The engine's whole-tick entry for the tick
// already exists at that moment, so the pair ((tick, 0.000), (tick, frac)) is
// ordered and the lookup interpolates -> origin = our eye, zero spam. Only stamp
// when the request is ahead of the newest ring entry (the failing case) to keep
// the ring ordered for behind-the-ring (reprediction) lookups.
float* __fastcall H::hkGetInterpolatedShootPosition(void* weaponServices,
	float* out, int* tickFrac)
{
	H::Hooks::EnterHook();
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		float* ret = out;
		auto original = GetInterpolatedShootPosition.GetOriginal();
		if (original) { __try { ret = original(weaponServices, out, tickFrac); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		H::Hooks::LeaveHook(); return ret;
	}
	__try {
		// Session gate: never touch the ring during map load / join window.
		if (H::SessionEntityOk() && weaponServices && Mem::ValidEntity(weaponServices) && tickFrac) {
			int tick = tickFrac[0];
			float frac = reinterpret_cast<const float*>(tickFrac)[1];
			if (tick > 0 && std::isfinite(frac) && frac >= 0.f && frac < 1.f) {
				const int head = *reinterpret_cast<const std::int32_t*>(
					static_cast<const std::uint8_t*>(weaponServices) + 0x368);
				const int count = *reinterpret_cast<const std::int32_t*>(
					static_cast<const std::uint8_t*>(weaponServices) + 0x36C);
				if (count > 0 && count <= 32) {
					const auto* base = static_cast<const std::uint8_t*>(weaponServices) + 0xE8;
					const auto* oldest = base + 20ull * static_cast<size_t>(head & 31);
					const auto* newest = base
						+ 20ull * static_cast<size_t>((head + count - 1) & 31);
					int oTick = 0, nTick = 0;
					float oFrac = 0.f, nFrac = 0.f;
					std::memcpy(&oTick, oldest + 0, 4);
					std::memcpy(&oFrac, oldest + 4, 4);
					std::memcpy(&nTick, newest + 0, 4);
					std::memcpy(&nFrac, newest + 4, 4);
					const bool behind = oTick > 0 && ((tick < oTick)
						|| (tick == oTick && frac + 1e-4f < oFrac));
					const bool ahead = nTick > 0 && ((tick > nTick)
						|| (tick == nTick && frac > nFrac));
					Vector_t eye{};
					std::memcpy(&eye.x, newest + 8, 4);
					std::memcpy(&eye.y, newest + 12, 4);
					std::memcpy(&eye.z, newest + 16, 4);
					if (behind && nTick > 0) {
						// Stale hist tick (e.g. #8128 vs ring #8139..#8141).
						// Rewrite the lookup - do not append an old tick.
						tickFrac[0] = nTick;
						reinterpret_cast<float*>(tickFrac)[1] = nFrac;
					} else if (ahead && Bones::IsValidPos(eye)) {
						Bones::StampShootPositionHistorySvc(
							weaponServices, tick, frac, eye);
					}
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (auto original = GetInterpolatedShootPosition.GetOriginal()) {
		float* ret = out;
		__try { ret = original(weaponServices, out, tickFrac); }
		__except (EXCEPTION_EXECUTE_HANDLER) { ret = out; }
		H::Hooks::LeaveHook(); return ret;
	}
	H::Hooks::LeaveHook();
	return out;
}

static uintptr_t ReadVfuncSlot(uintptr_t vt, unsigned off)
{
	uintptr_t fn = 0;
	__try {
		fn = *reinterpret_cast<uintptr_t*>(vt + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		fn = 0;
	}
	return fn;
}

bool H::Hooks::init() {
	s_down.store(false, std::memory_order_release);
	s_unloading.store(false, std::memory_order_release);
	s_hookDepth.store(0, std::memory_order_release);
	int totalHooks = 0, hookedCount = 0;
	bool requiredOk = true;

	uintptr_t weapon_data_addr = M::patternScan(XS("client"), XS("48 8B 81 ? ? ? ? 85 D2 78 ? 48 83 FA ? 73 ? F3 0F 10 84 90 ? ? ? ? C3 F3 0F 10 80 ? ? ? ? C3 CC CC CC CC"));
	if (weapon_data_addr)
		oGetWeaponData = *reinterpret_cast<int*>(weapon_data_addr + 0x3);
	else
		Con::OffsetMiss("oGetWeaponData");

	uintptr_t base_entity_addr = M::patternScan(XS("client"), XS("4C 8D 49 ? 81 FA"));
	if (base_entity_addr)
		ogGetBaseEntity = reinterpret_cast<decltype(ogGetBaseEntity)>(base_entity_addr);
	else
		Con::PatternMiss("client", "GetBaseEntity");

	uintptr_t local_player_addr = M::patternScan(XS("client"), XS("48 83 EC 28 83 F9 FF 75 ? 48 8B 0D ? ? ? ? 48 8D 54 24 30 48 8B 01 FF 90 ? ? ? ? 8B 08 48 63 C1 4C 8D 05"));
	if (local_player_addr)
		oGetLocalPlayer = reinterpret_cast<decltype(oGetLocalPlayer)>(local_player_addr);
	else
		Con::PatternMiss("client", "GetLocalPlayer");

	// MessageLite::SerializePartialToArray (move_crc capture)
	CL_Bypass::Init();

	// UC / IDA CCSGOInput::CreateMove - builds usercmd + subticks (not the old CHLClient-style fn)
	// pattern: 85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40 (IDA 0x180B09520)
	uintptr_t create_move_addr = M::patternScan(XS("client"),
		XS("85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40"));
	if (create_move_addr)
	{
		++totalHooks;
		if (!CreateMove.Add(reinterpret_cast<void*>(create_move_addr),
			reinterpret_cast<void*>(&hkCreateMove))) {
			Con::Error("CreateMove hook.Add failed @ 0x%p", (void*)create_move_addr);
			requiredOk = false;
		} else {
			Con::Ok("CreateMove (CCSGOInput) @ 0x%p", (void*)create_move_addr);
			++hookedCount;
		}
	}
	else {
		Con::PatternMiss("client", "CreateMove");
		requiredOk = false;
	}

	auto addHook = [&](auto& hook, uintptr_t addr, auto detour, const char* name, const char* mod = "client") {
		if (!addr) {
			Con::PatternMiss(mod, name);
			if (std::string(name) == "FrameStageNotify") requiredOk = false;
			return;
		}
		++totalHooks;
		if (!hook.Add(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(detour))) {
			Con::Error("%s hook.Add failed @ 0x%p", name, (void*)addr);
			if (std::string(name) == "FrameStageNotify") requiredOk = false;
		} else {
			Con::Ok("%s @ 0x%p", name, (void*)addr);
			++hookedCount;
		}
	};
	auto addHookOptional = [&](auto& hook, uintptr_t addr, auto detour, const char* name, const char* mod = "client") {
		if (!addr) { Con::PatternMiss(mod, name); return; }
		++totalHooks;
		if (!hook.Add(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(detour)))
			Con::Error("%s hook.Add failed @ 0x%p", name, (void*)addr);
		else { Con::Ok("%s @ 0x%p", name, (void*)addr); ++hookedCount; }
	};

	{
		uintptr_t hit = M::patternScan(XS("client"),
			XS("FF FF FF FF 48 8D 05 ? ? ? ? 48 89 0D ? ? ? ?"));
		uintptr_t fn = 0;
		if (hit) {
			std::uint8_t* disp = reinterpret_cast<std::uint8_t*>(hit + 7);
			uintptr_t vt = reinterpret_cast<uintptr_t>(M::GetAbsoluteAddress(disp, 0));
			if (vt)
				fn = ReadVfuncSlot(vt, 0x40);
		}
		addHook(HandleViewAngles, fn, &hkHandleViewAngles, "HandleViewAngles");
	}
	addHook(SetViewAngle,
		M::patternScan(XS("client"), XS("85 D2 75 3D 48 63 81")),
		&hkSetViewAngle, "SetViewAngle");

	addHook(FrameStageNotify, M::patternScan(XS("client"), XS("48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED")), &hkFrameStageNotify, "FrameStageNotify");
	// GetInterpolatedShootPosition (IDA 0x1808C2D10) - nospread "[Shooting] cl:"
	// spam fix: stamp requested sub-tick time into the shoot-position ring.
	addHook(GetInterpolatedShootPosition,
		M::patternScan(XS("client"), XS("40 55 56 41 56 48 81 EC 20 01 00 00")),
		&hkGetInterpolatedShootPosition, "GetInterpolatedShootPosition");
	World::Weather::Install();
	Hitmarker::Install();
	BulletFx::Install();
	Hitsound::Install();
	SoundEsp::Install();
	Vote::Install();
	Panorama::Install();
	addHook(GetRenderFov, M::patternScan(XS("client"), XS("40 53 48 83 EC ? 48 8B D9 E8 ? ? ? ? 48 85 C0 74 ? 48 8B C8 48 83 C4")), &hkGetRenderFov, "GetRenderFov");
	// CALCVIEWMODEL - viewmodel XYZ + FOV (bypass engine clamps after original)
	addHook(GetViewModelOffsets,
		M::patternScan(XS("client"), XS("40 55 53 56 41 56 41 57 48 8B EC 48 83 EC 20 4D")),
		&hkGetViewModelOffsets, "GetViewModelOffsets");
	// GetScreenAspectRatio - engine2 IVEngineClient vtable slot 88
	// pattern dump: 48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8D 0D (rva 0x76050)
	addHook(GetScreenAspectRatio,
		M::patternScan(XS("engine2"),
			XS("48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8D 0D")),
		&hkGetScreenAspectRatio, "GetScreenAspectRatio", "engine2");
	// OVERRIDEVIEW - CViewSetup origin/angles (third person)
	// dump: pattern::client::OverrideView primary; loose fallback
	{
		uintptr_t ov = M::patternScan(XS("client"),
			XS("40 57 48 83 EC 60 48 8B FA E8 ? ? ? ? BA FF"));
		if (!ov)
			ov = M::patternScan(XS("client"),
				XS("40 57 48 83 EC ? 48 8B FA E8 ? ? ? ? BA"));
		addHook(OverrideView, ov, &hkOverrideView, "OverrideView");
	}
	// SetupFog - IDA 0x18027D8B0; writes gradient_fog shader params
	addHook(SetupFog,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 4C 24 ? 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 63 02")),
		&hkSetupFog, "SetupFog");
	// DRAWSCOPEOVERLAY - sniper scope HUD out-struct (bars / blur / texture)
	addHook(DrawScopeOverlay,
		M::patternScan(XS("client"), XS("48 8B C4 53 57 48 83 EC ? 48 8B FA")),
		&hkDrawScopeOverlay, "DrawScopeOverlay");

	W2S::Init();
	// GetMatrixForView dump (RVA 0x1666C0) is a 3-arg FOV helper - NOT matrix
	// capture. Hooking it as 6-arg poisoned W2S live matrix / stack. Use
	// ScreenTransform + pViewMatrix instead (see w2s.cpp).
	// DrawCrosshair - reticle allow-gate (rifles / knives). Unique dump pattern.
	addHook(DrawCrosshair,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 85")),
		&hkDrawCrosshair, "DrawCrosshair");

	// FlashOverlay - dump FlashOverlay; IDA 0x18113C960 "FlashbangOverlay"
	addHook(RenderFlashBangOverlay,
		M::patternScan(XS("client"), XS("85 D2 0F 88 ? ? ? ? 48 89 4C 24 08 55 56")),
		&hkRenderFlashbangOverlay, "FlashOverlay");
	// DRAWLEGS - Firstperson Legs
	addHook(DrawLegs,
		M::patternScan(XS("client"), XS("40 55 53 56 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? F2 0F 10 42")),
		&hkDrawLegs, "DrawLegs");
	// DRAWSMOKEVERTEX - smoke volume
	addHook(DrawSmokeVertex,
		M::patternScan(XS("client"), XS("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 48 8B 9C 24 ? ? ? ? 4D 8B F8")),
		&hkDrawSmokeVertex, "DrawSmokeVertex");
	// DRAWSMOKEARRAY - live client.dll 0xCB4AD0 (frame C8 07); IDA was D8 07 - wildcard both
	// Unique: mov [rsp+10],rdx; push rbp; push r12; lea rbp; sub rsp; mov r12,rdx; lea rcx; mov edx,-1
	{
		uintptr_t smokeArr = M::patternScan(XS("client"),
			XS("48 89 54 24 10 55 41 54 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 4C 8B E2 48 8D 0D ? ? ? ? BA FF FF FF FF"));
		if (!smokeArr)
			smokeArr = M::patternScan(XS("client"),
				XS("48 89 54 24 10 55 41 54 48 8D AC 24 ? ? ? ? 48 81 EC ? 07 00 00 4C 8B E2"));
		addHook(DrawSmokeArray, smokeArr, &hkDrawSmokeArray, "DrawSmokeArray");
	}
	// RENDERDECALS - bullet / blood / explosion decals
	addHook(RenderDecals,
		M::patternScan(XS("client"), XS("44 88 4C 24 ? 48 89 54 24 ? 55 53 57")),
		&hkRenderDecals, "RenderDecals");
	// CacheParticleEffect - real particle spawn (IDA 0x18078EE10). Old CreateParticleEffect = SetCP.
	addHook(CacheParticleEffect,
		M::patternScan(XS("client"),
			XS("4C 8B DC 53 48 81 EC 90 00 00 00 F2 0F 10 05")),
		&hkCacheParticleEffect, "CacheParticleEffect");
	// ParticleDrawArray - particles.dll fire/molly tint (UC Particle Modulation, IDA 0x1802826D0)
	// Write RGB floats at a2+0x50 before draw. Name filter in detour (inferno_fx / groundfire).
	// Try particles.dll first, then client.dll fallback; pattern shifts per build.
	{
		uintptr_t pda = M::patternScan(XS("particles"), XS("48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55"));
		const char* pdaMod = "particles";
		if (!pda) {
			pda = M::patternScan(XS("particles"), XS("48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 56"));
			if (pda) pdaMod = "particles#2";
		}
		if (!pda) {
			pda = M::patternScan(XS("client"), XS("48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55"));
			if (pda) pdaMod = "client";
		}
		if (!pda) {
			// wildcard looser: push rbp variant seen on some builds
			pda = M::patternScan(XS("particles"), XS("40 53 55 56 57 41 56 48 81 EC ? ? ? ?"));
			if (pda) pdaMod = "particles#3";
		}
		addHook(ParticleDrawArray, pda, &hkParticleDrawArray, "ParticleDrawArray", pdaMod);
		if (!pda)
			Con::PatternMiss(pdaMod, "ParticleDrawArray (fire/inferno tint will not work)");
	}

	addHook(MouseInputEnabled, M::patternScan(XS("client"), XS("40 53 48 83 EC 20 80 B9 ? ? ? ? ? 48 8B D9 75 78")), &hkMouseInputEnabled, "MouseInputEnabled");
	addHook(IsRelativeMouseMode, M::patternScan(XS("inputsystem"), XS("48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC ? 0F B6 F2")), &hkIsRelativeMouseMode, "IsRelativeMouseMode", "inputsystem");
	addHook(DrawGlow, M::patternScan(XS("client"), XS("40 53 48 83 EC 20 48 8B 54")), &hkDrawGlow, "DrawGlow");
	// GetGlowColor - last-mile float4 before ManageGlow; isolates glow from mesh tint
	addHook(GetGlowColor,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 54")),
		&hkGetGlowColor, "GetGlowColor");
	// ManageGlowSceneObject - force pure glow colour + backface mult = 1.0f
	addHook(ManageGlowSceneObject,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 48 8B F2 45 33 F6")),
		&hkManageGlowSceneObject, "ManageGlowSceneObject");
	// ApplyGlowScene (B04B30) - pre-stamp + post force scene float4
	addHook(ApplyGlowScene,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 ? 48 89 6C 24 ? 48 89 7C 24 ? 41 56 48 81 EC 80 00 00 00")),
		&hkApplyGlowScene, "ApplyGlowScene");
	addHook(FireEventClientSide,
		M::patternScan(XS("client"), XS("40 53 41 54 41 56 48 83 EC ? 4C 8B F2")),
		&hkFireEventClientSide, "FireEventClientSide");
	// Andromeda Hook_AntiTamper - client 40 53 41 57 48 83 EC ? 48 89 74 24 ? 48 8B F1
	// 2nd-queue VAC "launched insecure" check. Detour always returns false.
	addHook(AntiTamper,
		M::patternScan(XS("client"), XS("40 53 41 57 48 83 EC ? 48 89 74 24 ? 48 8B F1")),
		&hkAntiTamper, "AntiTamper");
	// UnlockInventory (CCSInventoryManager::IsLoadoutAllowed) and GetServerLoadoutItem
	// are NOT hooked to prevent Valve's mmqueue tamper check from flagging client code bytes.
	// yougey wetness/rain: CMapInfo env rain strength + wetness coverage
	// Dump SetupMapInfo - wildcard stack saves; hard E8 broke on some builds
	uintptr_t setupMap = M::patternScan(XS("client"),
		XS("48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 48 81 EC ? ? ? ? 0F 29 70 ? 48 8B EA 0F 29 78 ? 45 33 C0"));

	// Priority A: entity add/remove (gen bump) + LevelShutdown map unload
	if (void* a = SdkPrioA::OnAddAddr()) {
		addHook(OnAddEntity, reinterpret_cast<uintptr_t>(a), &hkOnAddEntity, "OnAddEntity");
		if (H::OnAddEntity.IsHooked())
			SdkPrioA::MarkHooked("OnAddEntity", "gen bump only");
	}
	if (void* a = SdkPrioA::OnRemoveAddr()) {
		addHook(OnRemoveEntity, reinterpret_cast<uintptr_t>(a), &hkOnRemoveEntity, "OnRemoveEntity");
		if (H::OnRemoveEntity.IsHooked())
			SdkPrioA::MarkHooked("OnRemoveEntity", "gen bump only");
	}
	if (void* a = SdkPrioA::LevelShutdownAddr()) {
		addHook(LevelShutdown, reinterpret_cast<uintptr_t>(a), &hkLevelShutdown, "LevelShutdown");
		if (H::LevelShutdown.IsHooked())
			SdkPrioA::MarkHooked("LevelShutdown", "map gen + feature cleanup");
	}
	Con::Ok("Hooks: %d/%d hooked, required=%s", hookedCount, totalHooks, requiredOk ? "ok" : "FAIL");
	return requiredOk;
}

void H::Hooks::InstallSceneWorldHooks()
{
	static std::atomic<bool> s_done{ false };
	static std::atomic<bool> s_installing{ false };
	static std::atomic<bool> s_sortAttempted{ false };
	static std::atomic<bool> s_skyAttempted{ false };
	static std::atomic<bool> s_worldAttempted{ false };
	static std::atomic<int>  s_chamsTries{ 0 };
	if (s_done.load(std::memory_order_acquire))
		return;
	if (!GetModuleHandleA("scenesystem.dll"))
		return;
	if (!H::SessionEntityReady() || H::SessionMapLeaving() || H::SessionPostMatch())
		return;
	bool expected = false;
	if (!s_installing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return;
	struct InstallGuard {
		std::atomic<bool>& flag;
		~InstallGuard() { flag.store(false, std::memory_order_release); }
	} guard{ s_installing };

	// mercey sort_primitives - overlay chams drawn last (scenesystem)
	if (!s_sortAttempted.exchange(true, std::memory_order_acq_rel)) {
		uintptr_t sp = M::patternScan(XS("scenesystem"),
			XS("45 85 C9 0F 84 ? ? ? ? 55 56 57 48 83 EC 30"));
		if (sp && SortPrimitives.Add(reinterpret_cast<void*>(sp), reinterpret_cast<void*>(&hkSortPrimitives)))
			Con::Ok("SortPrimitives @ 0x%p (deferred)", (void*)sp);
		else
			Con::PatternMiss("scenesystem", "SortPrimitives (chams overlay sorting)");
	}

	// Chams materials - CreateMaterial/SetTypeKV3 live once scenesystem ready.
	// Only retry while the one-shot sig attempts are still pending; once they
	// are done this is the sole remaining work, so bound the retries and park.
	if (!chams::Materials::ready())
		chams::Materials::init();

	if (!s_skyAttempted.exchange(true, std::memory_order_acq_rel)) {
		uintptr_t sky = M::patternScan(XS("scenesystem"),
			XS("45 85 C9 0F 8E ? ? ? ? 4C 8B DC 55 41 56 49 8D AB 58 FC FF FF 48 81 EC 98 04 00 00"));
		if (sky && DrawSkyboxArray.Add(reinterpret_cast<void*>(sky), reinterpret_cast<void*>(&hkDrawSkyboxArray)))
			Con::Ok("DrawSkyboxArray @ 0x%p (deferred)", (void*)sky);
		else
			Con::PatternMiss("scenesystem", "DrawSkyboxArray");
	}
	if (!s_worldAttempted.exchange(true, std::memory_order_acq_rel)) {
		World::InstallMapColorHook();
		World::InstallLightingHook();
	}

	if (s_sortAttempted.load(std::memory_order_acquire)
		&& s_skyAttempted.load(std::memory_order_acquire)
		&& s_worldAttempted.load(std::memory_order_acquire)) {
		if (chams::Materials::ready()) {
			s_done.store(true, std::memory_order_release);
		} else if (s_chamsTries.fetch_add(1, std::memory_order_relaxed) >= 300) {
			// ~300 frames of retries failed - park instead of paying the full
			// module lookup + session probes every frame for the whole session.
			Con::Warn("chams materials unresolved after retries - scene hooks parked");
			s_done.store(true, std::memory_order_release);
		}
	}
}

static void OnAddEntityBody(void* entitySystem, void* entity, int handle) {
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		if (auto orig = H::OnAddEntity.GetOriginal()) { __try { orig(entitySystem, entity, handle); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		return;
	}
	if (H::OnAddEntity.IsHooked()) {
		auto original = H::OnAddEntity.GetOriginal();
		if (original) {
			__try { original(entitySystem, entity, handle); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("OnAddEntity.orig"); }
		}
	}
	__try { SdkPrioA::OnEntityAdded(entitySystem, entity, handle); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("OnAddEntity.post"); }
}

void __fastcall H::hkOnAddEntity(void* entitySystem, void* entity, int handle) {
	HookScopeGuard guard;
	guard.arm();
	OnAddEntityBody(entitySystem, entity, handle);
}

static void OnRemoveEntityBody(void* entitySystem, void* entity, int handle) {
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		if (auto orig = H::OnRemoveEntity.GetOriginal()) { __try { orig(entitySystem, entity, handle); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
		return;
	}
	__try { SdkPrioA::OnEntityRemoved(entitySystem, entity, handle); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("OnRemoveEntity.pre"); }
	if (H::OnRemoveEntity.IsHooked()) {
		auto original = H::OnRemoveEntity.GetOriginal();
		if (original) {
			__try { original(entitySystem, entity, handle); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("OnRemoveEntity.orig"); }
		}
	}
}

void __fastcall H::hkOnRemoveEntity(void* entitySystem, void* entity, int handle) {
	HookScopeGuard guard;
	guard.arm();
	OnRemoveEntityBody(entitySystem, entity, handle);
}

static void* LevelShutdownBody(void* a1) {
	if (H::Hooks::s_unloading.load(std::memory_order_acquire)) {
		void* _ret = nullptr;
		if (auto orig = H::LevelShutdown.GetOriginal()) { __try { _ret = orig(a1); } __except (EXCEPTION_EXECUTE_HANDLER) { _ret = nullptr; } }
		return _ret;
	}
	__try { H::SessionOnMapLeave(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("LevelShutdown.pre"); }
	void* _ret = nullptr;
	if (H::LevelShutdown.IsHooked()) {
		auto original = H::LevelShutdown.GetOriginal();
		if (original) { __try { _ret = original(a1); } __except (EXCEPTION_EXECUTE_HANDLER) { _ret = nullptr; } }
	}
	return _ret;
}

void* __fastcall H::hkLevelShutdown(void* a1) {
	HookScopeGuard guard;
	guard.arm();
	return LevelShutdownBody(a1);
}

/* REMOVED-FEATURE-HOOK (not in current hooks.h)
*/

void* __fastcall H::hkDrawGlow(void* glowProp) {
	H::Hooks::EnterHook();
	void* ret = nullptr;
	const bool unloading = H::Hooks::s_unloading.load(std::memory_order_acquire);
	if (DrawGlow.IsHooked()) {
		auto original = DrawGlow.GetOriginal();
		if (original) { __try { ret = original(glowProp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
	}
	if (!unloading
		&& H::SessionEntityReady() && !H::SessionMapLeaving() && !H::SessionPostMatch()) {
		__try {
			Glow::OnDrawGlow(reinterpret_cast<Glow::CGlowProperty*>(glowProp));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("hkDrawGlow", GetExceptionCode());
		}
	}
	H::Hooks::LeaveHook();
	return ret;
}

void __fastcall H::hkGetGlowColor(void* glowProp, float* outRgba) {
	H::Hooks::EnterHook();
	const bool unloading = H::Hooks::s_unloading.load(std::memory_order_acquire);
	if (!unloading && Glow::AnyEnabled()
		&& H::SessionEntityReady() && !H::SessionMapLeaving() && !H::SessionPostMatch()) {
		__try {
			if (Glow::ForceSceneColor(reinterpret_cast<Glow::CGlowProperty*>(glowProp), outRgba)) {
				H::Hooks::LeaveHook(); return;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("hkGetGlowColor force", GetExceptionCode());
		}
	}
	if (unloading || GetGlowColor.IsHooked()) {
		auto original = GetGlowColor.GetOriginal();
		if (original) {
			__try {
				original(glowProp, outRgba);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				TW_SEH_CATCH("GetGlowColor.orig");
			}
		}
	}
	H::Hooks::LeaveHook();
}

void* __fastcall H::hkManageGlowSceneObject(void** glowSceneOut, void* a2, void* sceneNode,
	float* color4, int a5, int a6, int a7, int a8) {
	H::Hooks::EnterHook();
	// a8 is m_flGlowBackfaceMult - forced to 1.0f only while glow features are
	// active; otherwise pass the game's value through untouched.
	const bool glowGate = !H::Hooks::s_unloading.load(std::memory_order_acquire)
		&& Glow::AnyEnabled()
		&& H::SessionEntityReady() && !H::SessionMapLeaving() && !H::SessionPostMatch();
	int backface = glowGate ? 0x3F800000 /*1.0f*/ : a8;
	if (glowGate) {
		__try {
			Glow::ForceManageColor(color4);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			TW_SEH_CATCH("ManageGlow.force");
		}
	}
	if (ManageGlowSceneObject.IsHooked()) {
		auto original = ManageGlowSceneObject.GetOriginal();
		if (original) {
			void* ret = nullptr;
			__try { ret = original(glowSceneOut, a2, sceneNode, color4, a5, a6, a7, backface); }
			__except (EXCEPTION_EXECUTE_HANDLER) { ret = nullptr; }
			H::Hooks::LeaveHook(); return ret;
		}
	}
	H::Hooks::LeaveHook();
	return nullptr;
}

std::int64_t __fastcall H::hkApplyGlowScene(void* glowProp, void* sceneNode) {
	H::Hooks::EnterHook();
	using Fn = std::int64_t(__fastcall*)(Glow::CGlowProperty*, void*);
	Fn original = nullptr;
	if (ApplyGlowScene.IsHooked())
		original = reinterpret_cast<Fn>(ApplyGlowScene.GetOriginal());
	if (!H::Hooks::s_unloading.load(std::memory_order_acquire)
		&& Glow::AnyEnabled()
		&& H::SessionEntityReady() && !H::SessionMapLeaving() && !H::SessionPostMatch()) {
		std::int64_t ret = Glow::OnApplyGlowScene(
			reinterpret_cast<Glow::CGlowProperty*>(glowProp), sceneNode, original);
		H::Hooks::LeaveHook(); return ret;
	}
	if (original) {
		std::int64_t ret = 0;
		__try { ret = original(reinterpret_cast<Glow::CGlowProperty*>(glowProp), sceneNode); }
		__except (EXCEPTION_EXECUTE_HANDLER) { ret = 0; }
		H::Hooks::LeaveHook(); return ret;
	}
	H::Hooks::LeaveHook();
	return 0;
}

bool __fastcall H::hkFireEventClientSide(void* eventManager, void* gameEvent) {
	H::Hooks::EnterHook();
	const bool unloading = H::Hooks::s_unloading.load(std::memory_order_acquire);
	const bool leaving = H::SessionMapLeaving();
	const bool sessionOk = !unloading && H::SessionEntityOk() && !leaving;
	if (sessionOk) {
		__try { World::Weather::OnGameEvent(gameEvent); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("FECS.Weather"); }
		__try { Hitmarker::OnGameEvent(gameEvent); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("FECS.Hitmarker"); }
		if (!BulletFx::ListenerActive())
			__try { BulletFx::OnGameEvent(gameEvent); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("FECS.BulletFx"); }
		__try { SoundEsp::OnGameEvent(gameEvent); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("FECS.SoundEsp"); }
		__try { Vote::OnGameEvent(gameEvent); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("FECS.Vote"); }
		__try { SkinChanger::OnFireEventClientSide(gameEvent); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("FECS.SkinChanger"); }
	}

	if (FireEventClientSide.IsHooked()) {
		auto original = FireEventClientSide.GetOriginal();
		if (original) {
			bool ret = false;
			__try { ret = original(eventManager, gameEvent); }
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("FireEventClientSide original", GetExceptionCode());
			}
			H::Hooks::LeaveHook(); return ret;
		}
	}
	H::Hooks::LeaveHook();
	return false;
}

bool __fastcall H::hkAntiTamper(void* /*src*/, int /*a2*/, __int64 /*a3*/, int /*a4*/)
{
	// Installed detour - must track depth like every other hook (quiescence
	// contract, hooks.h). Body never calls the original or touches state.
	H::Hooks::EnterHook();
	H::Hooks::LeaveHook();
	return false;
}

// ============================================================================
// Session gates + teardown (declared in hooks.h). Contracts from hooks.h:
// SessionLive local pawn OR engine in-game (OR of signals)
// SessionFeaturesOk live (no extra timer)
// SessionEntityReady local pawn + engine in-game. NO write probes. ESP.
// SessionEntityOk EntityReady + alive write-ready
// (services/scene/origin/weapon) + not cinematic. Glow / world / combat.
// SessionMapLeaving true while leave wipe in flight (cross-thread)
// SessionOnMapLeave debounced wipe (LevelShutdown / FSN leave edge)
// SessionWatchLocalLife death/respawn edge: wipe caches holding free'd pawn*
// IsolationLevel env GAMES8TH_ISO 0..4
// Per-(thread, frame) caching via g_presentFrame - repeated probes per frame
// short-circuit; cross-thread callers get their own TLS slot.
// ============================================================================
namespace {
constexpr std::uint32_t kLeaveCoalesceMs = 400;

std::atomic<std::uint64_t> g_leaveStartedMs{0};
std::atomic<bool> g_sessionParked{false};
std::atomic<bool> g_hadLocalAlive{false};
std::atomic<bool> g_hadEngineInGame{false};
std::atomic<bool> g_leaveGameWipePending{false};
std::atomic<bool> g_deathWipePending{false};
std::atomic<int> g_isolation{-1};

inline void SessionParkNow() noexcept {
	g_sessionParked.store(true, std::memory_order_relaxed);
	g_leaveStartedMs.store(static_cast<std::uint64_t>(::GetTickCount64()),
		std::memory_order_relaxed);
}

inline void SessionUnpark() noexcept {
	g_sessionParked.store(false, std::memory_order_relaxed);
	g_leaveStartedMs.store(0, std::memory_order_relaxed);
}

struct SessionFrameCache {
	std::uint32_t frame = 0xFFFFFFFFu;
	bool live = false;
	bool featuresOk = false;
	bool entityOk = false;
};
// Packed atomic slot - (frame << 5) | postMatch<<4 | entityReady<<3 |
// live<<2 | featuresOk<<1 | entityOk. Shared (not TLS): values are
// thread-independent. Manual-map has no TLS directory.
static std::atomic<std::uint64_t> g_sessionCache{ 0 };

inline void SessionInvalidateCache() noexcept {
	g_sessionCache.store(0, std::memory_order_relaxed);
}

inline std::uint64_t SessionNowMs() noexcept {
	return static_cast<std::uint64_t>(::GetTickCount64());
}

inline bool SessionEngineInGame() noexcept {
	__try {
		// NGC+0x230: 2=CONNECTED (still loading), 6=FULL. Features at signon 2
		// walk half-init pawn/weapon/particles -> join hitch then crash.
		// No EngineClient fallback: after leave NGC is gone while in_game()
		// stays true -> Present recaches a dead entity list, then join AVs.
		if (!Engine2::NetworkGameClient())
			return false;
		return Engine2::SignonState() >= 6;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("SessionEngineInGame");
	}
	return false;
}

// Scene + origin only. Active-weapon wait delayed world/skins until first gun.
static bool LocalPawnSceneReady(C_CSPlayerPawn* p) noexcept {
	if (!p || !Mem::ValidEntity(p))
		return false;
	CCSPlayer_WeaponServices* ws = nullptr;
	void* move = nullptr;
	CGameSceneNode* node = nullptr;
	__try {
		ws = p->GetWeaponServices();
		move = p->m_pMovementServices();
		node = p->m_pGameSceneNode();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!ws || !Mem::IsUserPtr(ws))
		return false;
	if (!move || !Mem::IsUserPtr(move))
		return false;
	if (!node || !Mem::IsUserPtr(node))
		return false;
	Vector_t pos{};
	__try {
		pos = node->m_vecAbsOrigin();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
}

// Skip writes until pawn can take them: weapon_services +
// game_scene_node + finite origin + live active weapon. TDM first-alive
// ticks have services/scene but no gun - CreateMove AV there.
static bool LocalPawnReady(C_CSPlayerPawn* p) noexcept {
	if (!LocalPawnSceneReady(p))
		return false;
	C_CSWeaponBase* wpn = nullptr;
	__try { wpn = p->GetActiveWeapon(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!wpn || !Mem::ValidEntity(wpn))
		return false;
	CGameSceneNode* wnode = nullptr;
	__try { wnode = ((C_BaseEntity*)wpn)->m_pGameSceneNode(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return wnode && Mem::IsUserPtr(wnode);
}

// Team intro / post-match cinematic. Skip all writes here.
// Join+~10s crash = intro ends, pawn recycles, timer already expired.
static bool SessionInCinematic() noexcept {
	void* rules = SdkPrioA::GameRules();
	if (!rules || !Mem::IsReadable(rules, 0xF08))
		return false;
	bool intro = false;
	int phase = 0;
	__try {
		intro = *reinterpret_cast<bool*>(
			reinterpret_cast<std::uint8_t*>(rules) + 0xF04);
		phase = *reinterpret_cast<int*>(
			reinterpret_cast<std::uint8_t*>(rules) + 0x84);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return intro || phase >= 4;
}

// Returns the current packed cache value - recomputes + stores when the
// present frame advanced since the last probe.
static bool SessionProbePostMatch(bool engineInGame) noexcept {
	if (!engineInGame)
		return false;
	void* rules = SdkPrioA::GameRules();
	if (!rules || !Mem::IsReadable(rules, 0x88))
		return false;
	int phase = 0;
	__try {
		phase = *reinterpret_cast<int*>(
			reinterpret_cast<std::uint8_t*>(rules) + 0x84);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	// 5 = GAMEPHASE_MATCH_ENDED. 4 is HALFTIME - still a live round.
	return phase >= 5;
}

static std::uint64_t SessionRefreshCache() noexcept {
	const std::uint32_t frame = H::g_presentFrame.load(std::memory_order_relaxed);
	std::uint64_t v = g_sessionCache.load(std::memory_order_relaxed);
	if ((std::uint32_t)(v >> 5) == frame)
		return v;
	C_CSPlayerPawn* pawn = H::SafeLocalPlayer();
	const bool local = pawn != nullptr;
	const bool engine = SessionEngineInGame();
	const bool live = local || engine;
	const bool featuresOk = live;
	// Visuals gate: signon>=6 only. Local pawn dies/recycles in TDM -
	// requiring it wiped ESP. Writes stay on EntityOk.
	const bool entityReady = engine;
	// Write gate: alive + services/scene/origin. Team intro and "has a gun"
	// used to stall ESP/world/skins for seconds after join.
	C_CSPlayerPawn* alive = H::SafeLocalAlive();
	const bool writeReady = alive && LocalPawnSceneReady(alive);
	const bool entityOk = entityReady && writeReady;
	const bool postMatch = SessionProbePostMatch(engine);
	v = ((std::uint64_t)frame << 5)
		| (postMatch ? 16ull : 0ull)
		| (entityReady ? 8ull : 0ull)
		| (live ? 4ull : 0ull) | (featuresOk ? 2ull : 0ull) | (entityOk ? 1ull : 0ull);
	g_sessionCache.store(v, std::memory_order_relaxed);
	return v;
}
} // namespace

bool H::SessionLive() noexcept {
	__try {
		return (SessionRefreshCache() & 4ull) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool H::SessionFeaturesOk() noexcept {
	__try {
		return (SessionRefreshCache() & 2ull) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool H::SessionEntityOk() noexcept {
	__try {
		return (SessionRefreshCache() & 1ull) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool H::SessionEntityReady() noexcept {
	__try {
		return (SessionRefreshCache() & 8ull) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool H::SessionMapLeaving() noexcept {
	// Sticky until next spawned pawn - not a 2.5s timer. Timer expiry let
	// ESP/glow walk entities during the next-queue heartbeat.
	return g_sessionParked.load(std::memory_order_relaxed);
}

bool H::SessionPostMatch() noexcept {
	__try {
		return (SessionRefreshCache() & 16ull) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void H::SessionDrainDeath() noexcept {
	if (!g_deathWipePending.exchange(false, std::memory_order_acq_rel))
		return;
	__try { Aimbot_Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Death.aimbotReset"); }
	__try { JumpBug::Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Death.jumpbugReset"); }
	__try { FastLadder::Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Death.fastladderReset"); }
	__try { Pred::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Death.predInvalidate"); }
	__try { Bones::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Death.bonesInvalidate"); }
	__try { AutoWall::InvalidateXhairCache(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Death.awXhairInvalidate"); }
}

void H::SessionDrainGameLeave() noexcept {
	if (!g_leaveGameWipePending.exchange(false, std::memory_order_acq_rel))
		return;
	__try { Aimbot_Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Leave.aimbotReset"); }
	__try { JumpBug::Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Leave.jumpbugReset"); }
	__try { FastLadder::Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Leave.fastladderReset"); }
	__try { Pred::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Leave.predInvalidate"); }
	__try { Bones::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Leave.bonesInvalidate"); }
	__try { AutoWall::InvalidateXhairCache(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Leave.awXhairInvalidate"); }
	__try { ScoreboardWeapons::OnLevelChange(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("SessionMapLeave.Scoreboard"); }
	__try { Panorama::OnLevelChange(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("SessionMapLeave.Panorama"); }
}

void H::SessionOnMapLeave() noexcept {
	SessionParkNow();
	g_hadLocalAlive.store(false, std::memory_order_relaxed);
	g_leaveGameWipePending.store(true, std::memory_order_release);
	SessionInvalidateCache();
	__try { SdkPrioA::OnLevelShutdown(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("SessionOnMapLeave", GetExceptionCode()); }
	// Game-thread owned state (CreateMove/FSN): reset here, NOT on the render
	// thread. Render-owned caches are wiped by SdkPrioA::FlushRenderWipe in
	// hkPresent (level-shutdown/leave would race Present mid-draw otherwise).
	SessionDrainGameLeave();
}

void H::SessionWatchLocalLife() noexcept {
	__try {
		// Single probe per frame - the parked-branch re-probe below cost a
		// second SEH pawn chain every Present for a result one frame stale.
		C_CSPlayerPawn* pawn = SafeLocalAlive();
		const bool alive = pawn != nullptr;
		const bool wasAlive = g_hadLocalAlive.exchange(alive, std::memory_order_relaxed);
		const bool engine = SessionEngineInGame();
		const bool wasEngine = g_hadEngineInGame.exchange(engine, std::memory_order_relaxed);
		const std::uint64_t now = SessionNowMs();

		// Do NOT unpark on signon>=6. That is the 2nd-queue heartbeat window
		// (mmqueue registering/heartbeating). Wait for a real spawned pawn.
		if (g_sessionParked.load(std::memory_order_relaxed)
			&& engine && alive) {
			if (LocalPawnSceneReady(pawn)) {
				const std::uint64_t start = g_leaveStartedMs.load(std::memory_order_relaxed);
				if (start == 0 || (now - start) >= kLeaveCoalesceMs) {
					SessionUnpark();
					SessionInvalidateCache();
				}
				return;
			}
		}

		if (alive) {
			return;
		}
		if (!engine && wasEngine) {
			// Present thread - only timestamps + render wipe flag.
			// Full SessionOnMapLeave (Aim/Pred) is game-thread
			// (hkLevelShutdown / FSN drain). Calling it here races CreateMove.
			SessionParkNow();
			g_hadLocalAlive.store(false, std::memory_order_relaxed);
			g_leaveGameWipePending.store(true, std::memory_order_release);
			SessionInvalidateCache();
			__try { SdkPrioA::RequestRenderWipe(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Life.engineLeaveWipe"); }
			return;
		}
		if (wasAlive) {
			// Death: session flags only on Present. Aim/Pred/Bones live on
			// CreateMove - wiping them here UAF on the next hop/shot.
			SessionInvalidateCache();
			g_deathWipePending.store(true, std::memory_order_release);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("SessionWatchLife.outer");
	}
}

int H::IsolationLevel() noexcept {
	int v = g_isolation.load(std::memory_order_relaxed);
	if (v >= 0)
		return v;
	v = 0;
	char buf[8]{};
	if (::GetEnvironmentVariableA("GAMES8TH_ISO", buf, sizeof(buf)) > 0)
		v = ::atoi(buf);
	if (v < 0) v = 0;
	if (v > 4) v = 4;
	g_isolation.store(v, std::memory_order_relaxed);
	return v;
}

void H::Hooks::WaitForQuiescence(uint32_t ms) noexcept {
	const ULONGLONG deadline = GetTickCount64() + ms;
	while (s_hookDepth.load(std::memory_order_acquire) > 0) {
		if (GetTickCount64() >= deadline) break;
		::Sleep(1);
	}
}

void H::Hooks::shutdown() {
	// NOT a permanent latch: a failed-init retry calls shutdown() then init()
	// again - if this stuck at true, the later F4 unload would skip teardown
	// with every hook still patched. init() resets it so each install cycle
	// gets exactly one teardown.
	if (s_down.exchange(true, std::memory_order_acq_rel))
		return;
	s_unloading.store(true, std::memory_order_release);
	// Quiescence barrier: let in-flight hooks exit before we restore bytes.
	// Hooks check s_unloading at entry and skip feature work; depth tracks entry.
	WaitForQuiescence(250);
	// Restore first so original bytes are back while trampolines stay valid
	// for in-flight GetOriginal(); Remove after a short drain.
	auto forEachHook = [](auto&& fn) {
		fn(IsRelativeMouseMode);
		fn(MouseInputEnabled);
		fn(SortPrimitives);
		fn(DrawSkyboxArray);
		fn(DrawAggregateSceneObjectArray);
		fn(GeneratePrimitives);
		fn(GlobalLightUpdate);
		fn(UpdateLightObject);
		fn(ToneMapUpdate);
		fn(FrameStageNotify);
		fn(GetRenderFov);
		fn(GetViewModelOffsets);
		fn(OverrideView);
		fn(SetupFog);
		fn(GetScreenAspectRatio);
		fn(DrawScopeOverlay);
		fn(DrawCrosshair);
		fn(RenderFlashBangOverlay);
		fn(DrawLegs);
		fn(DrawSmokeVertex);
		fn(DrawSmokeArray);
		fn(RenderDecals);
		fn(CacheParticleEffect);
		fn(ParticleDrawArray);
		fn(CreateMove);
		fn(HandleViewAngles);
		fn(SetViewAngle);
		fn(GetInterpolatedShootPosition);
		fn(DrawGlow);
		fn(GetGlowColor);
		fn(ManageGlowSceneObject);
		fn(ApplyGlowScene);
		fn(FireEventClientSide);
		fn(AntiTamper);
		fn(OnAddEntity);
		fn(OnRemoveEntity);
		fn(LevelShutdown);
	};

	forEachHook([](auto& h) {
		__try { h.Restore(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	});
	WaitForQuiescence(100);
	forEachHook([](auto& h) {
		__try { h.Remove(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	});

	__try { Panorama::Uninstall(); } __except (EXCEPTION_EXECUTE_HANDLER) {}

	g_leaveStartedMs.store(0, std::memory_order_relaxed);
	Con::Tag("unload", Con::Level::Ok, "hooks down");
}

