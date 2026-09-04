#pragma once
#include "includeHooks.h"
#include <atomic>
#include "../../cs2/entity/C_AggregateSceneObject/C_AggregateSceneObject.h"
#include "../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../cs2/datatypes/cutlbuffer/cutlbuffer.h"
#include "../../cs2/datatypes/keyvalues/keyvalues.h"
#include "../../cs2/entity/C_Material/C_Material.h"
#include "../interfaces/CCSGOInput/CCSGOInput.h"
// Forward declaration
class CEntityIdentity;
namespace H {
// Bumped once per Present frame by hkPresent. Session gates cache per	// (thread, frame) so features that call SessionLive / SessionEntityOk /	// SessionFeaturesOk many times per frame (glow / ESP / features) don't	// re-run heavy pawn probes. Cross-thread callers use their own cache	// keyed by the same counter.	
inline std::
atomic<std::
uint32_t> g_presentFrame{ 0 }
;
	void __fastcall hkFrameStageNotify(void* a1, int stage);
	// scenesystem SortPrimitives - mercey chams overlay ordering (4 args, IDA pattern 45 85 C9)
	void __fastcall hkSortPrimitives(void* a1, void* a2, void* entries, std::uint32_t count);
	void __fastcall hkDrawSkyboxArray(__int64 a1, __int64 a2, __int64 a3, int a4, int a5, __int64 a6, __int64 a7);
	std::
int64_t __fastcall hkDrawAggregateSceneObjectArray(void* a1, void* a2, void* pAggregateArr);
	// GeneratePrimitives - builds mesh draw list; used to t
// all world props	
std::int64_t __fastcall hkGeneratePrimitives(void* a1, void* sceneObj, void* a3, void* drawList);
	void* __fastcall hkGlobalLightUpdate(void* pThis);
	// IDA FlashOverlay @ 0x18113C960 - "FlashbangOverlay" material path	
void __fastcall hkRenderFlashbangOverlay(void* a1, int split, void** matSys, void* a4, void* a5);
	void __fastcall hkDrawLegs(void* a1, void* a2, void* a3, void* a4, void* a5);
	std::
int64_t __fastcall hkDrawSmokeVertex(void* a1, void* a2, int a3, int a4, void* a5, void* a6);
	// IDA DrawSmokeArray @ 0x180CB4190 - SmokeConstantBuffer batch (remove + color path sibling)

std::
int64_t __fastcall hkDrawSmokeArray(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6);
	void* __fastcall hkRenderDecals(void* a1, void* a2, char a3, char a4);
	// IDA CacheParticleEffect @ 0x18078EE10 - real spawn gate (old CreateParticleEffect was SetCP)

void* __fastcall hkCacheParticleEffect(void* mgr, unsigned int* outIndex, const char* name,
int attach, void* entity, void* a6, void* a7, int a8);
	// particles.dll ParticleDrawArray ( Particle Modulation) - RGB floats @ a2+0x50	// IDA sub_1802826D0 - pattern 48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55	
void* __fastcall hkParticleDrawArray(void* a1, void* a2, void* a3, void* a4, void* a5);
	// scenesystem ToneMapUpdate @ 0x1801874F0 - GPU exposure outputs @ +136/+140	
float* __fastcall hkToneMapUpdate(void* tonemapState);
	// scenesystem UpdateLightObject @ 0x180199590 - light RGB @ +0xE4	// (old LightSceneObject short sig was this same fn - 
	// do not double-hook)
void __fastcall hkUpdateLightObject(void* sceneSys, void* lightObj, void* a3);
	// IDA CCSGOInput::CreateMove 0x180B09520 - returns 
// bool (ABI).
// void detour left RAX garbage -> map-join / secure path corruption.
	bool __fastcall hkCreateMove(void* pInput, int slot, bool active);
	// CCSGOInput apply-cmd-angles (vtable+0x40). Save camera, original, restore.
	void __fastcall hkHandleViewAngles(void* thisptr, int slot);
	void __fastcall hkSetViewAngle(void* thisptr, int slot, Vector_t* ang);
	// IDA CCSPlayer_WeaponServices::GetInterpolatedShootPosition @ 0x1808C2D10 -
	// shoot-position ring lookup for the fire origin. Seed-nospread fires at a
	// sub-tick (tick, frac) the ring has no entry for -> "[Shooting] cl:" spam.
	// Hook stamps the exact requested time before the original lookup runs.
	float* __fastcall hkGetInterpolatedShootPosition(void* weaponServices,
		float* out, int* tickFrac);
	void* __fastcall hkDrawGlow(void* glowProp);
	// IDA sub_180B499F0 - colour_override -> float4 consumed by ManageGlowSceneObject	
void __fastcall hkGetGlowColor(void* glowProp, float* outRgba);
	// IDA sub_180B1B2B0 - force pure glow colour4 + backface mult = 1 (no mesh tint)

void* __fastcall hkManageGlowSceneObject(void** glowSceneOut, void* a2, void* sceneNode,
float* color4, int a5, int a6, int a7, int a8);
	// IDA sub_180B04B30 - full glow apply (pre-fill -> GetGlowColor -> ManageGlow)

std::
int64_t __fastcall hkApplyGlowScene(void* glowProp, void* sceneNode);
	bool __fastcall hkFireEventClientSide(void* eventManager, void* gameEvent);
	// Andromeda Hook_AntiTamper - 2nd-queue VAC secure check. Always false.
	bool __fastcall hkAntiTamper(void* src, int a2, __int64 a3, int a4);
	inline float g_flActiveFov = 90.f;
	float hkGetRenderFov(void* rcx);
	void __fastcall hkGetViewModelOffsets(void* viewmodel, float* offsets, float* fov);
	void __fastcall hkOverrideView(void* rcx, void* setup);
	std::uintptr_t __fastcall hkSetupFog(void* output, int* mode);
	// engine2 IVEngineClient::GetScreenAspectRatio - 
// float(this, width, height)
float __fastcall hkGetScreenAspectRatio(void* thisptr, int width, int height);
	std::
int64_t __fastcall hkDrawScopeOverlay(void* a1, void* a2);
	bool __fastcall hkDrawCrosshair(void* a1);
	// Menu input blocking hooks - declare early	
void __fastcall hkIsRelativeMouseMode(void* pInputSystem, bool active);
	bool __fastcall hkMouseInputEnabled(void* rcx);
	using IsRelativeMouseModeFn = void(__fastcall*)(void*, bool);
	using MouseInputEnabledFn = bool(__fastcall*)(void*);
	inline CInlineHookObj<IsRelativeMouseModeFn> IsRelativeMouseMode = { }
;
	inline CInlineHookObj<MouseInputEnabledFn> MouseInputEnabled = { }
;
	inline void* g_pInputSystem = nullptr;
	inline bool g_wantRelativeMouse = true;
	inline CInlineHookObj<decltype(&hkSortPrimitives)> SortPrimitives = { }
;
	inline CInlineHookObj<decltype(&hkDrawSkyboxArray)> DrawSkyboxArray = { }
;
	inline CInlineHookObj<decltype(&hkDrawAggregateSceneObjectArray)> DrawAggregateSceneObjectArray = { }
;
	inline CInlineHookObj<decltype(&hkGeneratePrimitives)> GeneratePrimitives = { }
;
	inline CInlineHookObj<decltype(&hkGlobalLightUpdate)> GlobalLightUpdate = { }
;
	inline CInlineHookObj<decltype(&hkUpdateLightObject)> UpdateLightObject = { }
;
	inline CInlineHookObj<decltype(&hkToneMapUpdate)> ToneMapUpdate = { }
;
	inline CInlineHookObj<decltype(&hkFrameStageNotify)> FrameStageNotify = { }
;
	inline CInlineHookObj<decltype(&hkGetRenderFov)> GetRenderFov = { }
;
	inline CInlineHookObj<decltype(&hkGetViewModelOffsets)> GetViewModelOffsets = { }
;
	inline CInlineHookObj<decltype(&hkOverrideView)> OverrideView = { }
;
	inline CInlineHookObj<decltype(&hkSetupFog)> SetupFog = { }
;
	inline CInlineHookObj<decltype(&hkGetScreenAspectRatio)> GetScreenAspectRatio = { }
;
	inline CInlineHookObj<decltype(&hkDrawScopeOverlay)> DrawScopeOverlay = { }
;
	inline CInlineHookObj<decltype(&hkDrawCrosshair)> DrawCrosshair = { }
;
	inline CInlineHookObj<decltype(&hkRenderFlashbangOverlay)> RenderFlashBangOverlay = { }
;
	inline CInlineHookObj<decltype(&hkDrawLegs)> DrawLegs = { }
;
	inline CInlineHookObj<decltype(&hkDrawSmokeVertex)> DrawSmokeVertex = { }
;
	inline CInlineHookObj<decltype(&hkDrawSmokeArray)> DrawSmokeArray = { }
;
	inline CInlineHookObj<decltype(&hkRenderDecals)> RenderDecals = { }
;
	inline CInlineHookObj<decltype(&hkCacheParticleEffect)> CacheParticleEffect = { }
;
	inline CInlineHookObj<decltype(&hkParticleDrawArray)> ParticleDrawArray = { }
;
	inline CInlineHookObj<decltype(&hkCreateMove)> CreateMove = { }
;
	inline CInlineHookObj<decltype(&hkHandleViewAngles)> HandleViewAngles = { }
;
	inline CInlineHookObj<decltype(&hkSetViewAngle)> SetViewAngle = { }
;
	inline CInlineHookObj<decltype(&hkGetInterpolatedShootPosition)> GetInterpolatedShootPosition = { }
;
	inline CInlineHookObj<decltype(&hkDrawGlow)> DrawGlow = { }
;
	inline CInlineHookObj<decltype(&hkGetGlowColor)> GetGlowColor = { }
;
	inline CInlineHookObj<decltype(&hkManageGlowSceneObject)> ManageGlowSceneObject = { }
;
	inline CInlineHookObj<decltype(&hkApplyGlowScene)> ApplyGlowScene = { }
;
	inline CInlineHookObj<decltype(&hkFireEventClientSide)> FireEventClientSide = { }
;
	inline CInlineHookObj<decltype(&hkAntiTamper)> AntiTamper = { }
;
	// Priority A - entity list + map unload	
void __fastcall hkOnAddEntity(void* entitySystem, void* entity, int handle);
	void __fastcall hkOnRemoveEntity(void* entitySystem, void* entity, int handle);
	void* __fastcall hkLevelShutdown(void* a1);
	inline CInlineHookObj<decltype(&hkOnAddEntity)> OnAddEntity = { }
;
	inline CInlineHookObj<decltype(&hkOnRemoveEntity)> OnRemoveEntity = { }
;
	inline CInlineHookObj<decltype(&hkLevelShutdown)> LevelShutdown = { }
;
	// inline hooks / resolved funcs	
inline int  oGetWeaponData;
	inline void* (__fastcall* ogGetBaseEntity)(void*, int);
	// Defined here; assigned in Hooks::init (also referenced by Input::get_user_cmd)

inline C_CSPlayerPawn* (__fastcall* oGetLocalPlayer)(int) = nullptr;
	// SEH + range/vtable - use instead of bare oGetLocalPlayer on unload-sensitive paths.	// Impl in hooks.cpp (header stays SEH-free for C2712).
[[nodiscard]] C_CSPlayerPawn* SafeLocalPlayer() noexcept;
// Alive + lifeState==0 only (CreateMove / combat). Spec uses SafeLocalPlayer.
[[nodiscard]] C_CSPlayerPawn* SafeLocalAlive() noexcept;
// Match-session gates.
// SessionLive: local pawn OR engine in-game (OR of signals).
// SessionFeaturesOk: live (no extra timer).
// SessionEntityReady: engine in-game (signon>=6). No pawn probe -
// ESP stays up through TDM death / pawn recycle.
// SessionEntityOk: Ready + alive + scene/origin (not cinematic, not waiting for a gun).
[[nodiscard]]
bool SessionLive() noexcept;
[[nodiscard]] bool SessionFeaturesOk() noexcept;
[[nodiscard]] bool SessionEntityOk() noexcept;
// See SessionEntityReady contract above.
[[nodiscard]] bool SessionEntityReady() noexcept;
// True from LevelShutdown until the next spawned pawn (not a timer).
// Present/glow/ESP bail - 2nd-queue heartbeat is signon>=6 with no pawn.
[[nodiscard]]
bool SessionMapLeaving() noexcept;
// CCSGameRules::m_gamePhase >= 5 (GAMEPHASE_MATCH_ENDED). Present / glow /
// world writes only - CreateMove stays live so you can still walk.
[[nodiscard]] bool SessionPostMatch() noexcept;
// Debounced wipe (LevelShutdown / FSN leave). Coalesced, no feature freeze.
void SessionOnMapLeave() noexcept;
// Game-thread Aim/Pred drain. LevelShutdown calls it; FSN NET_UPDATE_END
// drains if Present only saw the leave (pattern miss / listen server).
void SessionDrainGameLeave() noexcept;
// Game-thread death/respawn cache drop (Aim/Pred/Bones). Present only flags it.
void SessionDrainDeath() noexcept;
// Death/respawn edge: session flags only. ESP stays (TDM recycle flicker).
void SessionWatchLocalLife() noexcept;
// Isolation (env GAMES8TH_ISO=0..4). 0=all (default). Higher = strip surface.	// 1=no world entity walk 2=no player ESP cache 3=hooks only 4=Present pass-through
[[nodiscard]] 
int IsolationLevel() noexcept;
class Hooks {
public:
bool init();
// Andromeda: scene GPU hooks after IsInGame, not at inject/lobby.
static void InstallSceneWorldHooks();
// F4 / process detach - disable every game hook before FreeLibrary.		// Present-only teardown left CreateMove/FSN live -> post-unload AV.
void shutdown();
// Quiescence: wait for in-flight hooks to drain (hook threads vs unload thread)
static void WaitForQuiescence(uint32_t ms = 250) noexcept;
	inline static std::atomic<int> s_hookDepth{0};
	inline static std::atomic<bool> s_unloading{false};
	inline static std::atomic<bool> s_down{false}; // shutdown-in-progress latch; reset by init() for install-retry cycles
inline static void EnterHook() noexcept { s_hookDepth.fetch_add(1, std::memory_order_relaxed); }
inline static void LeaveHook() noexcept { s_hookDepth.fetch_sub(1, std::memory_order_relaxed); }
// Quiescence contract: EVERY detour must EnterHook() before any work (incl.
// GetOriginal()/trampoline calls) and LeaveHook() on all exits, and must check
// s_unloading at entry (orig-only passthrough when set). shutdown() Restore/
// Remove depends on this: Remove frees trampolines - untracked detour mid-
// trampoline = UAF. Pattern (C2712-safe: no RAII local inside __try fns):
//   EnterHook(); if (s_unloading) {orig; LeaveHook; ret;}
//   __try { body } __except {} LeaveHook(); return;
};

}
extern std::atomic<bool> g_bMenuOpen; // blocking game input when menu open; written render/message threads, read game thread

