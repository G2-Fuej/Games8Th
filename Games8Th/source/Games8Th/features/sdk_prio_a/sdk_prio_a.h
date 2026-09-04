#pragma once
// Priority A dump patterns - resolve + small APIs for features that still need them.

#include "../../utils/math/vector/vector.h"
#include <cstdint>

class C_CSWeaponBase;

namespace SdkPrioA {

bool Init();
bool Ready();

// Generation bumped on OnAdd/OnRemove/LevelShutdown - bones/ESP can invalidate caches.
std::uint32_t EntityGen();
std::uint32_t MapGen();

// Safety: Overwatch / demo / HLTV - aim/trigger/autofire should soft-disable.
bool IsOverwatch();
bool IsDemoOrHltv();
bool ShouldSoftDisableCombat(); // true if either

// Game rules global (may be null).
void* GameRules();

// Weapon VData via C_CSWeaponBase::GetEconWpnData (function path).
void* GetEconWpnData(C_CSWeaponBase* weapon);

// Engine abs origin (fn). Returns false -> caller uses schema fallback.
bool GetAbsOrigin(void* entity, Vector_t& out);

// Surrounding AABB from engine ComputeHitboxSurroundingBox.
bool ComputeHitboxSurroundingBox(void* entity, Vector_t& minsOut, Vector_t& maxsOut);

// Addresses for Hooks::init (null if scan miss).
void* OnAddAddr();
void* OnRemoveAddr();
void* LevelShutdownAddr();
void MarkHooked(const char* name, const char* note = nullptr);

// Hook bodies (H:: wrappers call these).
void OnLevelShutdown();
void OnEntityAdded(void* entitySystem, void* entity, int handle);
void OnEntityRemoved(void* entitySystem, void* entity, int handle);

// Map leave cache wipe - split by thread owner:
// OnLevelShutdown (game thread) bumps gens + marks render-cache wipe pending.
// FlushRenderWipe (hkPresent, render thread) clears render-owned caches
// (Esp / SoundEsp / Hitmarker / Hitsound / HitLog / Notify /
// World env). Never clear those from the game thread while
// Present is mid-build/mid-draw - vector clear vs push/render is a
// heap UAF that reads as a random instant process exit on map change.
void RequestRenderWipe();
void FlushRenderWipe();

// Player ESP: controller entity indices tracked via OnAdd/OnRemove (designer only).
// Avoids Present full-slot Get() walks that trip multi-queue insecure.
constexpr int kMaxTrackedControllers = 64;
int CopyControllerIndices(int* out, int maxOut);

// World ESP: dropped weapons / nades / planted C4 via OnAdd designer.
// Present iterates this list instead of 1..HighestEntityIndex.
constexpr int kMaxTrackedWorld = 256;
int CopyWorldIndices(int* out, int maxOut);
// Game-thread sliced seed (FSN). Never walk 1..Highest on Present.
void WarmWorldScan();

} // namespace SdkPrioA

