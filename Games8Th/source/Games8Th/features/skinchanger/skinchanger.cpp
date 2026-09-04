#include "skinchanger.h"
#include "skin_sdk.h"
#include "skin_items.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cmath>
#include <Windows.h>

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/sdk/IGameEvent.h"

namespace
{
	// SEH helpers: object-free so they can use __try (C2712 otherwise)
	static CBaseHandle SehReadHandle(void* p) {
		__try { return *reinterpret_cast<CBaseHandle*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return CBaseHandle{}; }
	}
	static int SehReadInt(void* p) {
		__try { return *reinterpret_cast<int*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	static CBaseHandle* SehReadPtr(void* p) {
		__try { return *reinterpret_cast<CBaseHandle**>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}
	static float SehReadFloat(void* p) {
		__try { return *reinterpret_cast<float*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
	}
	static void SehWriteI32At(void* base, size_t off, int32_t v) {
		__try { *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(base) + off) = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	static void SehWriteF32At(void* base, size_t off, float v) {
		__try { *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + off) = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	static void SehWriteU32At(void* base, size_t off, uint32_t v) {
		__try { *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(base) + off) = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	__declspec(noinline) static CCSPlayer_WeaponServices* SehWeaponServices(C_CSPlayerPawn* pawn)
	{
		if (!pawn) return nullptr;
		CCSPlayer_WeaponServices* ws = nullptr;
		__try { ws = pawn->GetWeaponServices(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		return ws;
	}

	static void SehWritePtrAt(void* base, size_t off, uintptr_t v) {
		__try { *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(base) + off) = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	static const char* SehReadSymbolString(void* pIdent, uint32_t off) {
		__try {
			auto* pSym = reinterpret_cast<CUtlSymbolLarge*>(reinterpret_cast<uint8_t*>(pIdent) + off);
			if (pSym && Mem::IsReadable(pSym, sizeof(CUtlSymbolLarge)))
				return pSym->String();
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		return nullptr;
	}
	struct SkinCfg {
		uint16_t def = 0;
		int paint = 0;
		float wear = 0.f;
		int seed = 0;
		bool enabled = false;
		bool legacy = false;
		bool stattrak = false;
		int stattrakCount = 0;
		char tag[64]{};
	};

	bool g_forceReapply = false;
	bool g_applyGloves = false;
	uint64_t g_agentHash = 0;
	float g_lastAgentSpawn = -1.f;
	int g_lastAgentTeam = 0;
	float g_lastSpawn = -1.f;
	C_BaseEntity* g_lastVm = nullptr;
	std::unordered_map<uint32_t, uint64_t> g_appliedSig;
	static SRWLOCK g_sigLock = SRWLOCK_INIT;
	static int s_pendingHudClear = 0; // retry HUD clear for a few frames after skin apply / map join (HUD not yet created)
	// Post-spawn knife settle window. nerv parity: NO forced full walks here -
	// the window only re-touches the knife viewmodel model/mesh (cheap, safe).
	// The old v1.3 form forced every weapon through the heavy regeneration
	// path 90 frames straight, which spammed HUD icon clears and crashed
	// round transitions.
	static int s_spawnReapply = 0;
	static int s_viewNotReady = 0; // consecutive not-ready retries - bounds the burst so a permanently-uninitialized view can't force fullRefresh forever

	// Manual-map safe helpers - SRWLOCK is zero-init safe unlike std::mutex (CRT).
	inline bool SigContains(uint32_t k) {
		AcquireSRWLockShared(&g_sigLock);
		bool r = g_appliedSig.find(k) != g_appliedSig.end();
		ReleaseSRWLockShared(&g_sigLock);
		return r;
	}
	inline bool SigFind(uint32_t k, uint64_t& out) {
		AcquireSRWLockShared(&g_sigLock);
		auto it = g_appliedSig.find(k);
		bool ok = it != g_appliedSig.end();
		if (ok) out = it->second;
		ReleaseSRWLockShared(&g_sigLock);
		return ok;
	}
	inline void SigInsert(uint32_t k, uint64_t v) {
		AcquireSRWLockExclusive(&g_sigLock);
		g_appliedSig[k] = v;
		ReleaseSRWLockExclusive(&g_sigLock);
	}
	inline void SigErase(uint32_t k) {
		AcquireSRWLockExclusive(&g_sigLock);
		g_appliedSig.erase(k);
		ReleaseSRWLockExclusive(&g_sigLock);
	}
	inline void SigClear() {
		AcquireSRWLockExclusive(&g_sigLock);
		g_appliedSig.clear();
		ReleaseSRWLockExclusive(&g_sigLock);
	}

	uint32_t Off(const char* field)
	{
		return SchemaFinder::Get(hash_32_fnv1a_const(field));
	}

	template <typename T>
	T& Field(void* base, uint32_t off)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
	}

	C_EconItemView* ItemView(C_CSWeaponBase* w)
	{
		const uint32_t attr = Off("C_EconEntity->m_AttributeManager");
		const uint32_t item = Off("C_AttributeContainer->m_Item");
		if (!attr || !item)
			return nullptr;
		return reinterpret_cast<C_EconItemView*>(reinterpret_cast<uint8_t*>(w) + attr + item);
	}

	C_EconItemView* GloveView(C_CSPlayerPawn* pawn)
	{
		const uint32_t off = Off("C_CSPlayerPawn->m_EconGloves");
		if (!off)
			return nullptr;
		return reinterpret_cast<C_EconItemView*>(reinterpret_cast<uint8_t*>(pawn) + off);
	}

	void SetViewU16(C_EconItemView* v, const char* f, uint16_t val)
	{
		if (!v || !Mem::IsUserPtr(v)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<uint16_t>(v, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	void SetViewU32(C_EconItemView* v, const char* f, uint32_t val)
	{
		if (!v || !Mem::IsUserPtr(v)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<uint32_t>(v, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	void SetViewU64(C_EconItemView* v, const char* f, uint64_t val)
	{
		if (!v || !Mem::IsUserPtr(v)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<uint64_t>(v, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	void SetViewBool(C_EconItemView* v, const char* f, bool val)
	{
		if (!v || !Mem::IsUserPtr(v)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<bool>(v, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	void SetPawnBool(C_CSPlayerPawn* pawn, const char* f, bool val)
	{
		if (!pawn || !Mem::IsUserPtr(pawn)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<bool>(pawn, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	uint16_t GetViewU16(C_EconItemView* v, const char* f)
	{
		if (!v || !Mem::IsUserPtr(v)) return 0;
		const uint32_t o = Off(f);
		if (!o) return 0;
		__try { return Field<uint16_t>(v, o); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	bool GetViewBool(C_EconItemView* v, const char* f)
	{
		if (!v || !Mem::IsUserPtr(v)) return false;
		const uint32_t o = Off(f);
		if (!o) return false;
		__try { return Field<bool>(v, o); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Spawn-frame gate: engine material/skin calls on a half-initialized
	// CEconItemView corrupt game state silently (ntdll fail-fast - no SEH
	// dispatch, no crashlog; same class as ClearHudIconSlots note). Ready =
	// entity valid + item view readable + m_bInitialized set by the game.
	bool EconViewReady(C_CSWeaponBase* w)
	{
		if (!w || !Mem::ValidEntity(w))
			return false;
		C_EconItemView* v = ItemView(w);
		if (!v || !Mem::IsUserPtr(v))
			return false;
		return GetViewBool(v, "C_EconItemView->m_bInitialized");
	}

	void SetWeaponI32(C_CSWeaponBase* w, const char* f, int32_t val)
	{
		if (!w || !Mem::IsUserPtr(w)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<int32_t>(w, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	void SetWeaponF32(C_CSWeaponBase* w, const char* f, float val)
	{
		if (!w || !Mem::IsUserPtr(w)) return;
		const uint32_t o = Off(f);
		if (!o) return;
		__try { Field<float>(w, o) = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	uint64_t OwnerXuid(C_CSWeaponBase* w)
	{
		const uint32_t lo = Off("C_EconEntity->m_OriginalOwnerXuidLow");
		const uint32_t hi = Off("C_EconEntity->m_OriginalOwnerXuidHigh");
		if (!lo || !hi || !w || !Mem::IsUserPtr(w)) return 0;
		uint32_t low = 0, high = 0;
		__try {
			low = Field<uint32_t>(w, lo);
			high = Field<uint32_t>(w, hi);
		} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		return (static_cast<uint64_t>(high) << 32) | low;
	}

	float ClampWear(float w)
	{
		if (!std::isfinite(w) || w < 0.0001f) return 0.0001f;
		if (w > 1.f) return 1.f;
		return w;
	}

	uint64_t MakeSig(const SkinCfg& cfg)
	{
		// FNV-1a mix. The old XOR packing aliased fields: wear bits shifted
		// into lanes 16..47 overlapped paint (16..31) and seed (32..47), so
		// different (paint, wear) combos could produce identical signatures
		// and the per-weapon diff skipped a real change.
		uint64_t h = 0xCBF29CE484222325ull;
		const auto mix = [&h](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
		mix(cfg.def);
		mix(static_cast<uint32_t>(cfg.paint));
		mix(static_cast<uint32_t>(cfg.seed));
		mix(cfg.legacy ? 1 : 0);
		mix(cfg.stattrak ? 1 : 0);
		uint32_t wearBits = 0;
		static_assert(sizeof(wearBits) == sizeof(cfg.wear), "float size mismatch");
		memcpy(&wearBits, &cfg.wear, sizeof(wearBits));
		mix(wearBits);
		return h;
	}

	bool IsGrenade(int id)
	{
		return id == 43 || id == 44 || id == 45 || id == 46 || id == 47 || id == 48 || id == 49;
	}
	bool IsDefaultKnife(int id) { return id == 42 || id == 59; }

	SkinCfg Resolve(uint16_t weaponDef, bool isKnife)
	{
		SkinCfg cfg;
		if (isKnife) {
			if (!Config::skin_knife || Config::skin_knife_def <= 0)
				return cfg;
			cfg.enabled = true;
			cfg.def = static_cast<uint16_t>(Config::skin_knife_def);
			cfg.paint = Config::skin_knife_paint;
			cfg.wear = Config::skin_knife_wear;
			cfg.seed = Config::skin_knife_seed;
			cfg.stattrak = Config::skin_knife_stattrak;
			cfg.stattrakCount = 1337;
			return cfg;
		}
		Config::WeaponSkin ws{};
		if (!Config::SkinWeapon_Find(weaponDef, ws))
			return cfg;
		// Vanilla (paint 0) = no skin at all - route through the revert path
		// so the weapon gets its stock item identity and mesh groups back
		// (a paint-0 entry that faked the item ID made the model vanish).
		if (ws.paint <= 0)
			return cfg;
		cfg.enabled = true;
		cfg.def = weaponDef;
		cfg.paint = ws.paint;
		cfg.wear = ws.wear;
		cfg.seed = ws.seed;
		cfg.stattrak = ws.stattrak;
		cfg.stattrakCount = 1337;
		return cfg;
	}

	bool LookupLegacy(uint16_t def, int paint)
	{
		// Thread-safe: menu thread mutates item.skins concurrently
		// (EnsureSkins). Raw iteration here was a heap UAF.
		return GetSkinItems().IsLegacySkin(def, paint);
	}

	bool IsSkinnedWeaponId(int weaponId)
	{
		if (weaponId == 42 || weaponId == 59 || (weaponId >= 500 && weaponId <= 526))
			return Config::skin_knife;
		return Config::SkinWeapon_Has(weaponId);
	}

	// Helper: get weapon definition index from handle (SEH-safe, for HUD switch check)
	static int GetWeaponDefFromHandleSafe(const CBaseHandle& h) {
		if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance) return 0;
		__try {
			auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(h.index());
			if (!w || !Mem::ValidEntity(w)) return 0;
			C_EconItemView* view = ItemView(w);
			if (!view) return 0;
			return GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
		} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	static bool ClearHudIconSlots()
	{
		bool ok = false;
		__try {
			auto ClearMatched = [](const char* hudName, uintptr_t backOff) -> bool {
				void* pHud = SkinSdk::FindHudElement(hudName);
				if (!pHud || !Mem::IsUserPtr(pHud)) return false;
				auto* pHudWeapons = reinterpret_cast<uint8_t*>(pHud) - backOff;
				// Full header span must be readable BEFORE deriving count/vector -
				// during round transitions the element can be mid-teardown and a
				// garbage "this" fed to the engine eraser corrupts the heap
				// silently (fail-fast, no SEH dispatch, no crashlog).
				if (!Mem::IsUserPtr(pHudWeapons)
					|| !Mem::IsReadable(pHudWeapons, backOff + 0x60)) return false;
				auto SlotCount = [](uint8_t* p) -> int {
					if (!p || !Mem::IsReadable(p, 0x68 + sizeof(int))) return 0;
					__try {
						int n = *reinterpret_cast<int*>(p + 0x50);
						if (n <= 0 || n > 64) n = *reinterpret_cast<int*>(p + 0x68);
						if (n <= 0 || n > 64) n = *reinterpret_cast<int*>(p + 0x38);
						return (n > 0 && n <= 64) ? n : 0;
					} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
				};
				auto SlotsPtr = [](uint8_t* p) -> uint8_t* {
					if (!p || !Mem::IsReadable(p, 0x58 + sizeof(void*))) return nullptr;
					__try {
						uint8_t* ptr = *reinterpret_cast<uint8_t**>(p + 0x58);
						if (!ptr || !Mem::IsUserPtr(ptr)) ptr = *reinterpret_cast<uint8_t**>(p + 0x40);
						return ptr;
					} 
					__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
				};
				int nCount = SlotCount(pHudWeapons);
				if (nCount <= 0) return false;
				auto* pSlots = SlotsPtr(pHudWeapons);
				// Vector must hold the WHOLE span we are about to walk.
				if (!pSlots || !Mem::IsUserPtr(pSlots)
					|| !Mem::IsReadable(pSlots, (size_t)nCount * 72)) return false;
				int i = 0, guard = 0, cleared = 0;
				while (i >= 0 && i < nCount && ++guard <= 128) {
					// Re-read both each iteration; abort on any inconsistency
					// instead of feeding the engine eraser a stale index.
					const int nNow = SlotCount(pHudWeapons);
					auto* pNow = SlotsPtr(pHudWeapons);
					if (nNow <= 0 || !pNow) break;
					pSlots = pNow;
					nCount = nNow;
					if (!Mem::IsReadable(pSlots, (size_t)nCount * 72)) break;
					if ((size_t)i * 72 + 60 + sizeof(int) > (size_t)nCount * 72) break;
					__try {
						const int weaponId = *reinterpret_cast<int*>(pSlots + (size_t)i * 72 + 60);
						// Plausible weapon-def range first - a garbage slot that
						// happens to pass IsSkinnedWeaponId is how the engine
						// eraser gets aimed at a non-slot offset.
						if (weaponId < 1 || weaponId > 800) { ++i; continue; }
						// IDA: clear any weapon slot pending HUD refresh - fixes ",s" stale icon
						// (IsSkinned gate removed for revert; compositeOwner update will refresh)
					} __except (EXCEPTION_EXECUTE_HANDLER) { ++i; continue; }
					const int next = SkinSdk::ClearHudWeaponIcon(pHudWeapons, i, 0);
					if (next < -1 || next > 64) break; // engine disagreed badly - stop
					i = next + 1;
					++cleared;
					if (cleared >= 8) break; // cosmetic refresh - never loop long
				}
				if (cleared)
					SkinSdk::UpdateWeaponRows(pHudWeapons);
				return true;
			};
			// IDA (fresh client.dll.i64): the icon-clear driver is a vtable method
			// whose `this` sits at container + 152 (0x98) - it calls the updater
			// with (this - 152). The registered CHudElement name carries the
			// CCSGO_ prefix, so the working pair is ("CCSGO_HudWeaponSelection",
			// 0x98). The old 0xA0 guess put the container 8 bytes low and the
			// slot-count probe always failed -> HUD icons never refreshed.
			if (ClearMatched("CCSGO_HudWeaponSelection", 0x98)) ok = true;
			else if (ClearMatched("HudWeaponSelection", 0x98)) ok = true;
			else if (ClearMatched("CCSGO_HudWeaponSelection", 0xA0)) ok = true;
			else if (ClearMatched("CCSGO_HudWeaponSelection", 0x90)) ok = true;
			else if (ClearMatched("HudWeaponSelection", 0xA0)) ok = true;
			else if (ClearMatched("CCSGO_HudWeaponSelection", 0x90)) ok = true;
			else if (ClearMatched("HudWeaponSelection", 0xA0)) ok = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
		return ok;
	}

	uint64_t MeshMask(bool isKnife, bool legacy)
	{
		if (isKnife) return legacy ? 1ull : 2ull;
		return legacy ? 2ull : 1ull;
	}

	void ApplyPaint(C_CSWeaponBase* w, C_EconItemView* view, const SkinCfg& cfg, uint32_t accountId, bool isKnife)
	{
		if (!w || !view || !cfg.enabled)
			return;
		const uint16_t applyDef = cfg.def ? cfg.def : GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
		const float wear = ClampWear(cfg.wear);
		const float seed = static_cast<float>(cfg.seed >= 0 ? cfg.seed : 0);

		// Vanilla skin (paint 0): clear all paint fallbacks and skip the fake
		// item identity entirely - the base model of the current def index is
		// what renders. Knife def swap stays: def index is kept so the chosen
		// knife MODEL persists while the skin is vanilla (never the default
		// ct/t knife), and m_bDisallowSOC keeps the local def from being
		// overwritten by the next SOC refresh.
		if (cfg.paint <= 0) {
			SetViewBool(view, "C_EconItemView->m_bDisallowSOC", true);
			SetViewBool(view, "C_EconItemView->m_bInitialized", true);
			if (isKnife && cfg.def)
				SetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex", applyDef);
			SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", 0);
			SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", 0);
			SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", 0.f);
			SetWeaponI32(w, "C_EconEntity->m_nFallbackStatTrak", -1);
			return;
		}

		// Andromeda: accountId from steamID, fallback 0 -> still set (SOC needs non-zero? use 1 if 0)
		if (!accountId) accountId = static_cast<uint32_t>(SkinSdk::InventorySteamId());
		if (!accountId) accountId = 1;

		// Keep the local fake item from being replaced by the next SOC refresh.
		SetViewBool(view, "C_EconItemView->m_bDisallowSOC", true);
		SetViewBool(view, "C_EconItemView->m_bRestoreCustomMaterialAfterPrecache", true);
		SetViewBool(view, "C_EconItemView->m_bInitialized", true);
		SetViewU32(view, "C_EconItemView->m_iAccountID", accountId);
		if (isKnife && cfg.def)
			SetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex", applyDef);
		// Andromeda sets both High and 64-bit ID (0xFFFFFFFF00000000). Also set Low for completeness (some builds read Low)
		SetViewU32(view, "C_EconItemView->m_iItemIDHigh", static_cast<uint32_t>(-1));
		SetViewU32(view, "C_EconItemView->m_iItemIDLow", 0);
		SetViewU64(view, "C_EconItemView->m_iItemID",
			(static_cast<uint64_t>(static_cast<uint32_t>(-1)) << 32));

		SetWeaponI32(w, "C_EconEntity->m_OriginalOwnerXuidLow", accountId);
		SetWeaponI32(w, "C_EconEntity->m_OriginalOwnerXuidHigh", 0);
		SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", cfg.paint);
		SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", cfg.seed);
		SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", wear);
		SetWeaponI32(w, "C_EconEntity->m_nFallbackStatTrak", cfg.stattrak ? cfg.stattrakCount : -1);
		// Fallback hard offsets if schema miss (Andromeda parity: direct 0x1680 etc never fails)
		if (!Off("C_EconEntity->m_OriginalOwnerXuidLow") && w && Mem::IsUserPtr(w)) {
			SehWriteI32At(w, 0x1678, accountId);
		}
		if (!Off("C_EconEntity->m_OriginalOwnerXuidHigh") && w && Mem::IsUserPtr(w)) {
			SehWriteI32At(w, 0x167C, 0);
		}
		if (!Off("C_EconEntity->m_nFallbackPaintKit") && w && Mem::IsUserPtr(w)) {
			SehWriteI32At(w, 0x1680, cfg.paint);
		}
		if (!Off("C_EconEntity->m_nFallbackSeed") && w && Mem::IsUserPtr(w)) {
			SehWriteI32At(w, 0x1684, cfg.seed);
		}
		if (!Off("C_EconEntity->m_flFallbackWear") && w && Mem::IsUserPtr(w)) {
			SehWriteF32At(w, 0x1688, wear);
		}
		if (!Off("C_EconEntity->m_nFallbackStatTrak") && w && Mem::IsUserPtr(w)) {
			SehWriteI32At(w, 0x168C, cfg.stattrak ? cfg.stattrakCount : -1);
		}

		// Always write these values. Leaving them untouched when paint==0 keeps
		// the previous skin cached in the composite material.
		SkinSdk::SetAttributeValueByName(view, "set item texture preference", static_cast<float>(cfg.paint));
		SkinSdk::SetAttributeValueByName(view, "set item texture prefab", static_cast<float>(cfg.paint));
		SkinSdk::SetAttributeValueByName(view, "set item texture wear", wear);
		SkinSdk::SetAttributeValueByName(view, "set item texture seed", seed);
		SkinSdk::SetAttributeValueByName(view, "kill eater",
			static_cast<float>(cfg.stattrak ? cfg.stattrakCount : 0));
		SkinSdk::SetAttributeValueByName(view, "kill eater score type", 0.f);
	}

	bool WalkWeapons(C_CSPlayerPawn* pawn, CCSPlayer_WeaponServices* ws, C_BaseEntity* vm,
		uint32_t accountId, uint64_t steamId, bool force, bool fullRefresh)
	{
		static uint32_t s_myWeapons = 0;
		static uint32_t s_activeWeapon = 0;
		if (!s_myWeapons) {
			s_myWeapons = Off("CPlayer_WeaponServices->m_hMyWeapons");
			if (!s_myWeapons) s_myWeapons = 0x48; // dump fallback (client_dll.hpp)
		}
		if (!s_activeWeapon) {
			s_activeWeapon = Off("CPlayer_WeaponServices->m_hActiveWeapon");
			if (!s_activeWeapon) s_activeWeapon = 0x60;
		}
		if (!s_myWeapons || !I::GameEntity || !I::GameEntity->Instance)
			return false;

		C_CSWeaponBase* active = nullptr;
		if (s_activeWeapon) {
			CBaseHandle curActiveHandle = SehReadHandle(reinterpret_cast<uint8_t*>(ws) + s_activeWeapon);
			if (curActiveHandle.valid() && I::GameEntity && I::GameEntity->Instance) {
				C_CSWeaponBase* tmp = I::GameEntity->Instance->Get<C_CSWeaponBase>(curActiveHandle.index());
				if (tmp && Mem::ValidEntity(tmp)) active = tmp;
			}
		}
		if (!active)
			active = pawn->GetActiveWeapon();

		// Andromeda uses C_NetworkUtlVectorBase: nSize at base, pElements at base+8 (with 4 pad)
		auto* base = reinterpret_cast<uint8_t*>(ws) + s_myWeapons;
		CBaseHandle* elems = nullptr;
		int sz = 0;
		{
			sz = SehReadInt(base + 0);
			elems = SehReadPtr(base + 8);
			if (sz <= 0 || sz > 64 || !elems || !Mem::IsUserPtr(elems)) {
				// fallback CUtlVector layout (some builds wrap differently)
				sz = SehReadInt(base + 0x10);
				elems = SehReadPtr(base + 0);
				if (sz <= 0 || sz > 64 || !elems || !Mem::IsUserPtr(elems)) {
					sz = 0; elems = nullptr;
				}
			}
		}

		if (!elems || sz <= 0 || sz > 64 || !Mem::IsUserPtr(elems)
			|| !Mem::IsReadable(elems, sizeof(CBaseHandle) * sz)) {
			if (fullRefresh) Con::Ok("SkinChanger: no weapons vector (sz=%d elems=%p off=0x%X)", sz, elems, s_myWeapons);
			return false;
		}

		int nWeapons = 0, nApplied = 0, nNoStatic = 0, nNoCfg = 0, nNoView = 0, nNoScene = 0, nNoWeapon = 0;

		for (int i = 0; i < sz; ++i) {
			if (!elems[i].valid() || elems[i].index() <= 0 || elems[i].index() > 0x7FFF)
				continue;
			auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(elems[i]);
			if (!w || !Mem::ValidEntity(w))
				continue;
			auto* wEnt = reinterpret_cast<C_BaseEntity*>(w);
			++nWeapons;
			static uint32_t s_designer = 0;
			if (!s_designer) {
				s_designer = SchemaFinder::Get(hash_32_fnv1a_const("CEntityIdentity->m_designerName"));
				if (!s_designer) s_designer = 0x20; // dump fallback
			}
			void* pIdent = wEnt->m_pEntityIdentity();
			if (!pIdent || !Mem::IsUserPtr(pIdent)) { ++nNoWeapon; continue; }
			if (s_designer) {
				const char* dn = SehReadSymbolString(pIdent, s_designer);
				if (dn && dn[0] && !strstr(dn, "weapon_")) { ++nNoWeapon; continue; }
			}
			C_EconItemView* view = ItemView(w);
			if (!view) { ++nNoView; continue; }
			CEconItemDefinition* pDef = SkinSdk::GetStaticData(view);
			int curDef = GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
			if (!pDef && curDef > 0) {
				pDef = SkinSdk::FindDefByIndex(static_cast<uint16_t>(curDef));
			}
			if (!pDef) { ++nNoStatic; continue; }
			if (curDef <= 0) curDef = pDef->DefIndex();
			if (IsGrenade(curDef))
				continue;
			const bool treatAsKnife = pDef->IsKnife(false) || IsDefaultKnife(curDef);
			SkinCfg cfg = Resolve(static_cast<uint16_t>(curDef), treatAsKnife);
				const bool wasSkinned = SigContains(elems[i].raw());
				if (!cfg.enabled) {
					if (wasSkinned) {
						// IDA verified: guard view ready before revert - fixes crash on deselect
						if (!EconViewReady(w)) { ++nNoView; continue; }
						void* comp = SkinSdk::CompositeOwner(w);
						if (comp && !Mem::IsUserPtr(comp)) comp = nullptr;
						if (treatAsKnife) {
							int team = pawn->m_iTeamNum();
							uint16_t defaultDef = (team == 3) ? 42 : 59;
							CEconItemDefinition* defDefault = SkinSdk::FindDefByIndex(defaultDef);
							const char* defaultModel = defDefault ? defDefault->ModelName() : nullptr;
							if (!defaultModel || !defaultModel[0]) defaultModel = (team == 3) ? "weapons/models/knife/knife_default_ct.vmdl" : "weapons/models/knife/knife_default_t.vmdl";
							SetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex", defaultDef);
							SetViewU32(view, "C_EconItemView->m_iItemIDHigh", 0);
							SetViewU32(view, "C_EconItemView->m_iItemIDLow", 0);
							SetViewU64(view, "C_EconItemView->m_iItemID", 0);
							SetViewBool(view, "C_EconItemView->m_bDisallowSOC", false);
							SetViewBool(view, "C_EconItemView->m_bInitialized", true);
							SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", 0);
							SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", 0);
							SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", 0.f);
							if (!Off("C_EconEntity->m_nFallbackPaintKit") && w && Mem::IsUserPtr(w)) {
								SehWriteI32At(w, 0x1680, 0);
							}
							if (defaultModel) {
								SkinSdk::SetModel(wEnt, defaultModel);
								if (C_BaseEntity* km = SkinSdk::GetKnifeModel(pawn)) {
									SkinSdk::SetModel(km, defaultModel);
									if (km->m_pGameSceneNode()) {
										SkinSdk::SetMeshGroupMask(km->m_pGameSceneNode(), 2);
										SkinSdk::PostDataUpdate(km);
									}
								}
								if (w == active && vm) SkinSdk::SetModel(vm, defaultModel);
							}
							// Subclass id = murmur2(decimal def index) - hashing a
							// weapon-name string produced an unregistered token
							// and left the knife undeployable.
							uint32_t subOff = Off("C_BaseEntity->m_nSubclassID");
							const std::uint32_t tok = SkinSdk::MakeSubclassToken(defaultDef);
							if (subOff) {
								SehWriteU32At(w, subOff, tok);
							} else if (w && Mem::IsUserPtr(w)) {
								SehWriteU32At(w, 0x380, tok);
							}
							SkinSdk::UpdateSubclass(w);
							SkinSdk::UpdateWeaponViewModel(w);
							const char* resolvedModel = SkinSdk::WeaponModelPath(view);
							if (!resolvedModel && defaultModel) {
								SkinSdk::SetModel(wEnt, defaultModel);
							}
							SkinSdk::SetMeshGroupMask(wEnt->m_pGameSceneNode(), 2);
							if (w == active && vm && vm->m_pGameSceneNode()) SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), 2);
							if (comp) SkinSdk::UpdateCompositeMaterial(comp);
							SkinSdk::UpdateCompositeMaterialSet(w);
							SkinSdk::UpdateSkin(w);
							SkinSdk::PostDataUpdate(wEnt);
							SigErase(elems[i].raw());
							++nApplied;
						} else {
							SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", 0);
							SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", 0);
							SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", 0.f);
							if (!Off("C_EconEntity->m_nFallbackPaintKit") && w && Mem::IsUserPtr(w)) {
								SehWriteI32At(w, 0x1680, 0);
							}
							SetViewU32(view, "C_EconItemView->m_iItemIDHigh", 0);
							SetViewU32(view, "C_EconItemView->m_iItemIDLow", 0);
							SetViewU64(view, "C_EconItemView->m_iItemID", 0);
							SetViewBool(view, "C_EconItemView->m_bDisallowSOC", false);
							SetViewBool(view, "C_EconItemView->m_bInitialized", true);
							SkinSdk::SetAttributeValueByName(view, "set item texture preference", 0.f);
							SkinSdk::SetAttributeValueByName(view, "set item texture prefab", 0.f);
							SkinSdk::SetAttributeValueByName(view, "set item texture wear", 0.f);
							SkinSdk::SetAttributeValueByName(view, "set item texture seed", 0.f);
							SkinSdk::SetAttributeValueByName(view, "kill eater", 0.f);
							SkinSdk::SetAttributeValueByName(view, "kill eater score type", 0.f);
							// Mesh group 1 = primary group. Mask 0 hid EVERY group -
							// the weapon model disappeared on vanilla revert.
							SkinSdk::SetMeshGroupMask(wEnt->m_pGameSceneNode(), 1);
							if (w == active && vm && vm->m_pGameSceneNode()) SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), 1);
							if (comp) SkinSdk::UpdateCompositeMaterial(comp);
							SkinSdk::UpdateCompositeMaterialSet(w);
							SkinSdk::UpdateSkin(w);
							SkinSdk::PostDataUpdate(wEnt);
							SigErase(elems[i].raw());
							++nApplied;
						}
					} else {
						++nNoCfg;
					}
					continue;
				}
				CEconItemDefinition* target = (treatAsKnife && cfg.def) ? SkinSdk::FindDefByIndex(cfg.def) : pDef;
				if (!target)
					continue;
				if (!treatAsKnife) {
					if (curDef < 1 || curDef > 70) continue;
					if (IsDefaultKnife(curDef) || (curDef >= 500 && curDef <= 526)) continue;
				}
				cfg.legacy = LookupLegacy(cfg.def ? cfg.def : static_cast<uint16_t>(curDef), cfg.paint);
				cfg.wear = ClampWear(cfg.wear);
				ApplyPaint(w, view, cfg, accountId, treatAsKnife);

				void* composite = SkinSdk::CompositeOwner(w);
				const uint64_t sig = MakeSig(cfg);
				uint64_t oldSig = 0;
				bool haveOld = SigFind(elems[i].raw(), oldSig);
				const bool isActive = (w == active);
				// nerv parity: validate against LIVE entity state, not only our
				// sig - a game-side reset (SOC refresh / round init) must
				// self-heal on the next walk with no force window involved.
				bool liveMatch = haveOld && oldSig == sig;
				if (liveMatch) {
					const uint32_t offPaint = Off("C_EconEntity->m_nFallbackPaintKit");
					int curPaint = offPaint
						? SehReadInt(reinterpret_cast<uint8_t*>(w) + offPaint)
						: cfg.paint;
					if (curPaint != cfg.paint) {
						liveMatch = false;
					} else if (treatAsKnife && cfg.def) {
						const int liveDef = GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
						if (liveDef != static_cast<int>(cfg.def))
							liveMatch = false;
					}
				}
				const bool needHeavy = force || !liveMatch;
				if (!needHeavy)
					continue;
				const uint64_t meshMask = MeshMask(treatAsKnife, cfg.legacy);

				if (treatAsKnife) {
					// ApplyPaint already wrote the new def index into the item
					// view - let the game resolve the canonical model path from
					// it. Legacy def-table name stays as fallback.
					const char* model = SkinSdk::WeaponModelPath(view);
					if (!model || !model[0])
						model = target->ModelName();
					if (model && model[0]) {
						SkinSdk::SetModel(wEnt, model);
						if (C_BaseEntity* km = SkinSdk::GetKnifeModel(pawn)) {
							SkinSdk::SetModel(km, model);
							if (km->m_pGameSceneNode()) {
								SkinSdk::SetMeshGroupMask(km->m_pGameSceneNode(), meshMask);
									SkinSdk::PostDataUpdate(km);
							}
						}
						if (isActive && vm)
							SkinSdk::SetModel(vm, model);
					}
					// Subclass id = murmur2(decimal def index). The old path
					// hashed target->WeaponName() - an unregistered token that
					// broke knife deploy entirely (could not switch to knife).
					uint32_t subOff = Off("C_BaseEntity->m_nSubclassID");
					const std::uint32_t tok = SkinSdk::MakeSubclassToken(
						static_cast<std::uint16_t>(cfg.def ? cfg.def : target->DefIndex()));
					if (subOff) {
						SehWriteU32At(w, subOff, tok);
					} else if (w && Mem::IsUserPtr(w)) {
						SehWriteU32At(w, 0x380, tok);
					}
					SkinSdk::UpdateSubclass(w);
					SkinSdk::UpdateWeaponViewModel(w);
				}

				if (wEnt->m_pGameSceneNode())
					SkinSdk::SetMeshGroupMask(wEnt->m_pGameSceneNode(), meshMask);
				if (isActive && vm && vm->m_pGameSceneNode()) {
					SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), meshMask);
						SkinSdk::PostDataUpdate(vm);
				}
				if (EconViewReady(w)) {
					if (composite)
						SkinSdk::UpdateCompositeMaterial(composite);
					SkinSdk::UpdateCompositeMaterialSet(w);
					SkinSdk::UpdateSkin(w);
					SkinSdk::RegenerateWeaponSkin(w, true);
				}
				if (wEnt->m_pGameSceneNode())
					SkinSdk::PostDataUpdate(wEnt);
				if (fullRefresh && view && Mem::IsUserPtr(view)) {
					// Andromeda clears CEconItemView description at 0x200
					// IDA: 0x200 description clear removed - stale offset corrupted HUD (showed ",s"); use schema m_szCustomName if needed
					// // IDA: 0x200 description clear removed - stale offset corrupted HUD (showed ",s"); use schema m_szCustomName if needed
					// SehWritePtrAt(view, 0x200, 0); // disabled // disabled
				}
				SigInsert(elems[i].raw(), sig);
				++nApplied;
			}
		if (fullRefresh)
			Con::Ok("SkinChanger: weapons=%d applied=%d noWeapon=%d noView=%d noStatic=%d noScene=%d noCfg=%d",
				nWeapons, nApplied, nNoWeapon, nNoView, nNoStatic, nNoScene, nNoCfg);
		// Any actual apply/revert this walk must refresh HUD icons - scheduled
		// once here; the HUD block below runs it at most once per frame.
		if (nApplied > 0)
			s_pendingHudClear = 5;
		return true;
	}

	void SetGlove(C_CSPlayerPawn* pawn)
	{
		if (!pawn || !Mem::ValidEntity(pawn))
			return;
		C_EconItemView* glove = GloveView(pawn);
		if (!glove)
			return;
		static uint8_t uUpdateFrames = 0;
		static uint16_t s_def = 0;
		static int s_paint = -1;
		static int s_seed = -1;
		static float s_wear = -1.f;

		if (!Config::skin_glove || !SkinSdk::IsSkinnableGloveDef(Config::skin_glove_def)) {
			if (s_def != 0) {
				// Revert glove to default (no glove) - Andromeda parity for config disable
				SetViewU16(glove, "C_EconItemView->m_iItemDefinitionIndex", 0);
				SetViewU32(glove, "C_EconItemView->m_iItemIDHigh", 0);
				SetViewU32(glove, "C_EconItemView->m_iItemIDLow", 0);
				SetViewU64(glove, "C_EconItemView->m_iItemID", 0);
				SetViewBool(glove, "C_EconItemView->m_bDisallowSOC", false);
				SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SkinSdk::SetAttributeValueByName(glove, "set item texture preference", 0.f);
				SkinSdk::SetAttributeValueByName(glove, "set item texture prefab", 0.f);
				SkinSdk::SetAttributeValueByName(glove, "set item texture wear", 0.f);
				SkinSdk::SetAttributeValueByName(glove, "set item texture seed", 0.f);
				uUpdateFrames = 5;
				SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SkinSdk::SetBodyGroup(pawn);
				SkinSdk::UpdateBodyGroupChoice(pawn);
				SkinSdk::PostDataUpdate(pawn);
				SetPawnBool(pawn, "C_CSPlayerPawn->m_bNeedToReApplyGloves", true);
				s_def = 0; s_paint = -1; s_seed=-1; s_wear=-1.f;
				g_lastSpawn = -1.f;
			} else {
				s_def = 0; s_paint = -1;
			}
			// Still need to keep body group updated for a few frames even when disabling
			if (uUpdateFrames > 0) {
				SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SkinSdk::SetBodyGroup(pawn);
				SkinSdk::UpdateBodyGroupChoice(pawn);
				SkinSdk::PostDataUpdate(pawn);
				SetPawnBool(pawn, "C_CSPlayerPawn->m_bNeedToReApplyGloves", true);
				--uUpdateFrames;
			}
			return;
		}
		const uint16_t def = static_cast<uint16_t>(Config::skin_glove_def);
		const int paint = Config::skin_glove_paint;
		const int seed = Config::skin_glove_seed;
		const float wear = ClampWear(Config::skin_glove_wear);
		float spawn = 0.f;
		const uint32_t spawnOff = Off("C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
		if (spawnOff && pawn && Mem::IsUserPtr(pawn))
			spawn = SehReadFloat(reinterpret_cast<uint8_t*>(pawn) + spawnOff);
		static C_CSPlayerPawn* s_lastGlovePawn = nullptr;
		const bool pawnChanged = (pawn != s_lastGlovePawn);
		if (pawnChanged)
			s_lastGlovePawn = pawn;

		const bool spawned = spawn != g_lastSpawn;
		const bool cfgChanged = s_def != def || s_paint != paint || s_seed != seed || s_wear != wear || pawnChanged;
		if (spawned || g_applyGloves || cfgChanged) {
			uUpdateFrames = 5;
				SetViewBool(glove, "C_EconItemView->m_bDisallowSOC", true);
				SetViewBool(glove, "C_EconItemView->m_bRestoreCustomMaterialAfterPrecache", true);
			SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SetViewU16(glove, "C_EconItemView->m_iItemDefinitionIndex", def);
				uint32_t accountId = static_cast<uint32_t>(SkinSdk::InventorySteamId());
				if (!accountId)
					accountId = 1;
				SetViewU32(glove, "C_EconItemView->m_iAccountID", accountId);
			const uint64_t fakeId = 0xF000000000000000ull
				| (static_cast<uint64_t>(def) << 32)
				| static_cast<uint32_t>(paint & 0xFFFF);
			SetViewU64(glove, "C_EconItemView->m_iItemID", fakeId);
			SetViewU32(glove, "C_EconItemView->m_iItemIDHigh", static_cast<uint32_t>(fakeId >> 32));
			SetViewU32(glove, "C_EconItemView->m_iItemIDLow", static_cast<uint32_t>(fakeId));
			SkinSdk::SetAttributeValueByName(glove, "set item texture preference", static_cast<float>(paint));
			SkinSdk::SetAttributeValueByName(glove, "set item texture prefab", static_cast<float>(paint));
			SkinSdk::SetAttributeValueByName(glove, "set item texture wear", wear);
			SkinSdk::SetAttributeValueByName(glove, "set item texture seed", static_cast<float>(seed));
			s_def = def; s_paint = paint; s_seed = seed; s_wear = wear;
			g_lastSpawn = spawn;
			g_applyGloves = false;
		}
		if (uUpdateFrames > 0) {
			SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
			SkinSdk::SetBodyGroup(pawn);
			SkinSdk::UpdateBodyGroupChoice(pawn);
			SkinSdk::PostDataUpdate(pawn);
			SetPawnBool(pawn, "C_CSPlayerPawn->m_bNeedToReApplyGloves", true);
			--uUpdateFrames;
		}
	}

	void SetAgent(C_CSPlayerPawn* pawn)
	{
		if (!pawn || !Mem::ValidEntity(pawn))
			return;
		static C_CSPlayerPawn* s_lastAgentPawn = nullptr;
		if (pawn != s_lastAgentPawn) {
			s_lastAgentPawn = pawn;
			g_agentHash = 0;
			g_lastAgentSpawn = -1.f;
			g_lastAgentTeam = 0;
		}
		const int team = pawn->m_iTeamNum();
		if (team != 2 && team != 3) {
			g_agentHash = 0;
			return;
		}
		float spawn = 0.f;
		const uint32_t spawnOff = Off("C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
		if (spawnOff)
			spawn = SehReadFloat(reinterpret_cast<uint8_t*>(pawn) + spawnOff);

		if (!Config::skin_agent) {
			if (g_agentHash != 0) {
				// Revert agent: reset hash so next enable reapplies; try to set default model
				// Default agents: 5036 (T) / 5037 (CT) or first found for team
				int team = pawn->m_iTeamNum();
				uint16_t tryDef = (team == 2) ? 5036 : 5037;
				if (CEconItemDefinition* d = SkinSdk::FindDefByIndex(tryDef); d && d->ModelName() && d->ModelName()[0]) {
					if (SkinSdk::SetModel(pawn, d->ModelName()))
						SkinSdk::PostDataUpdate(pawn);
				} else {
					// fallback: find any agent for team from items (locked copy-out)
					char modelBuf[512]{};
					if (GetSkinItems().FirstAgentModel(team == 2 ? 2 : 3, modelBuf, sizeof(modelBuf))
						&& modelBuf[0]) {
						if (SkinSdk::SetModel(pawn, modelBuf))
							SkinSdk::PostDataUpdate(pawn);
					}
				}
				g_agentHash = 0;
				g_lastAgentSpawn = -1.f;
				g_lastAgentTeam = 0;
			}
			return;
		}
		const int defIdx = (team == 2) ? Config::skin_agent_t : Config::skin_agent_ct;
		if (defIdx <= 0) {
			g_agentHash = 0;
			return;
		}
		CEconItemDefinition* pDef = SkinSdk::FindDefByIndex(static_cast<uint16_t>(defIdx));
		if (!pDef)
			return;
		const char* model = pDef->ModelName();
		if (!model || !model[0] || !pawn->m_pGameSceneNode())
			return;
		uint64_t h = hash_32_fnv1a_const(model);
		h ^= static_cast<uint64_t>(team) << 24;
		if (h == g_agentHash && spawn == g_lastAgentSpawn && team == g_lastAgentTeam)
			return;
		if (!SkinSdk::SetModel(pawn, model))
			return;
		SkinSdk::PostDataUpdate(pawn);
		g_agentHash = h;
		g_lastAgentSpawn = spawn;
		g_lastAgentTeam = team;
	}

	bool EventWeaponIsKnife(const char* name)
	{
		if (!name || !name[0]) return false;
		const char* n = name;
		if (!strncmp(n, "weapon_", 7)) n += 7;
		if (!_stricmp(n, "knife") || !_stricmp(n, "knife_t")
			|| !_stricmp(n, "knife_default_ct") || !_stricmp(n, "knife_default_t")
			|| !_stricmp(n, "bayonet"))
			return true;
		return !strncmp(n, "knife_", 6);
	}
}

void SkinChanger::Init()
{
	SkinSdk::Init();
	// Andromeda parity: pre-scan models early (instant, no FileExists) so weapon cfg resolve works even before menu open
	if (!GetSkinItems().Ready())
		GetSkinItems().Scan();
}

void SkinChanger::RefreshAll()
{
	// Do NOT clear g_appliedSig here: WalkWeapons needs old entries to REVERT
	// weapons/knife whose skin entry vanished (config switch / Disable). The
	// force flag alone makes every enabled skin re-run its heavy update.
	g_forceReapply = true;
	g_applyGloves = true;
	g_agentHash = 0;
	g_lastAgentSpawn = -1.f;
	g_lastAgentTeam = 0;
	s_pendingHudClear = 5; // Andromeda HUD retry: clear icon slots for a few frames after skin change / map join
}

void SkinChanger::NotifySkinsChanged()
{
	// Light path for single-item picks: the changed weapon's MakeSig no longer
	// matches its stored entry, so the next FSN heavy-updates ONLY that weapon
	// (model/paint/subclass/viewmodel). No force flag, no gloves/agent reset.
	// Just schedule the HUD slot refresh so the icon/name swap immediately.
	s_pendingHudClear = 5;
}

void SkinChanger::OnFrameStageNotify(int stage)
{
	if (stage != FRAME_RENDER_START)
		return;
	if (H::SessionMapLeaving() || !H::SessionEntityOk())
		return;
	if (!I::EngineClient || !I::EngineClient->in_game())
		return;
	// Config Load mutates skin maps on the menu thread (clear + re-insert).
	// Walking weapons against a half-loaded config applies garbage and races
	// the map - skip until settled. The RefreshAll() at the end of Load
	// triggers one clean apply pass afterwards.
	if (Config::loading.load(std::memory_order_acquire))
		return;
	if (H::SessionMapLeaving() || H::SessionPostMatch()) {
		SigClear();
		return;
	}
	// Andromeda parity: local inventory is the first gate, before pawn/vm.
	// If inventory null, still try once (first frame after inject inventory may be lazy). Don't block gloves/weapons forever.
	void* inv = SkinSdk::LocalInventory();
	if (!inv) {
		if (g_forceReapply)
			Con::Ok("SkinChanger: LocalInventory null (inventory mgr or +0x3F540) - trying fallback");
		// Fallback: Andromeda's ScanAllItems uses same mgr, but if still null retry next frame
		// Don't return immediately if we can get pawn anyway (weapon paint with accountId 0 still works for gloves)
		// For weapon/knife we need inventory for econ def lookup, but that uses EconSchema not inventory, so allow walk with 0 steamId
	}
	C_CSPlayerPawn* pawn = H::SafeLocalAlive();
	if (!pawn || !Mem::ValidEntity(pawn))
		return;
	CCSPlayer_WeaponServices* ws = SehWeaponServices(pawn);
	if (!ws || !Mem::IsUserPtr(ws))
		return;
	C_BaseEntity* vm = SkinSdk::GetViewModel(pawn);
	if (!vm || !Mem::ValidEntity(vm)) {
		if (g_forceReapply)
			Con::Ok("SkinChanger: GetViewModel failed (no hud weapon model)");
		return;
	}
	uint64_t steamId = SkinSdk::InventorySteamId();
	uint32_t accountId = static_cast<uint32_t>(steamId);
	if (!accountId) accountId = 1; // Andromeda fallback: ensure non-zero for SOC (0 would be rejected)
	if (!steamId) steamId = accountId; // keep owner check permissive
	const bool vmChanged = (vm != g_lastVm);
	g_lastVm = vm;
	// HUD refresh triggers: respawn / map-join spawn (m_flLastSpawnTimeIndex) and menu
	// skin selection via RefreshAll. The hud-arms viewmodel entity is recreated on every
	// weapon deploy (vm != g_lastVm) - that churn must never touch HUD icon slots.
	bool spawnChanged = false;
	CBaseHandle curActive{};
	{
		const uint32_t spawnOff = Off("C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
		float curSpawn = 0.f;
		if (spawnOff) {
			curSpawn = SehReadFloat(reinterpret_cast<uint8_t*>(pawn) + spawnOff);
		}
		static float s_lastHudSpawn = -1.f;
		spawnChanged = (curSpawn != s_lastHudSpawn && curSpawn >= 0.f);
		if (spawnChanged) s_lastHudSpawn = curSpawn;
		const uint32_t offActive = Off("CPlayer_WeaponServices->m_hActiveWeapon");
		const uint32_t useOff = offActive ? offActive : 0x60u;
		curActive = SehReadHandle(reinterpret_cast<uint8_t*>(ws) + useOff);
		if (spawnChanged) {
			if (s_pendingHudClear < 5) s_pendingHudClear = 5;
			s_spawnReapply = 90; // knife settle window - light touches only
		}
	}
	// One heavy pass per EVENT only (menu refresh / config load / spawn edge) -
	// nerv parity. Continuous force windows re-ran material regeneration and
	// HUD slot clears every frame: visible HUD churn + round-transition crash.
	const bool force = g_forceReapply;
	g_forceReapply = false;
	// Deploy swap: restore custom knife model on recreated viewmodel (also on spawn - old !spawnChanged guard left knife bugged 3s)
	if (vmChanged && Config::skin_knife && Config::skin_knife_def > 0) {
		const int activeDef = GetWeaponDefFromHandleSafe(curActive);
		if (activeDef == 42 || activeDef == 59 || (activeDef >= 500 && activeDef <= 526)) {
			if (CEconItemDefinition* kd = SkinSdk::FindDefByIndex(static_cast<uint16_t>(Config::skin_knife_def))) {
				const char* km = kd->ModelName();
				if (km && km[0]) {
					SkinCfg kcfg{};
					kcfg.enabled = true;
					kcfg.def = static_cast<uint16_t>(Config::skin_knife_def);
					kcfg.paint = Config::skin_knife_paint;
					kcfg.legacy = LookupLegacy(kcfg.def, kcfg.paint);
					SkinSdk::SetModel(vm, km);
					if (vm->m_pGameSceneNode())
						SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), MeshMask(true, kcfg.legacy));
				}
			}
		}
	}
	// Don't clear g_appliedSig here - WalkWeapons needs old entries to revert disabled skins
	// It will update/erase per weapon.

	WalkWeapons(pawn, ws, vm, accountId, steamId, force, force);
	if (force)
		Con::Ok("SkinChanger: apply frame steam=%I64u vm=%p inv=%p", steamId, (void*)vm, (void*)SkinSdk::LocalInventory());

	// Knife settle window: the game can recreate or override the knife
	// viewmodel in the seconds after spawn WITHOUT changing the vm pointer.
	// Re-touch model + mesh only (cheap, idempotent) - never the heavy walk.
	if (!force && s_spawnReapply > 0) {
		--s_spawnReapply;
		const int activeDef = GetWeaponDefFromHandleSafe(curActive);
		if ((activeDef == 42 || activeDef == 59 || (activeDef >= 500 && activeDef <= 526))
			&& Config::skin_knife && Config::skin_knife_def > 0 && !vmChanged) {
			if (CEconItemDefinition* kd = SkinSdk::FindDefByIndex(static_cast<uint16_t>(Config::skin_knife_def))) {
				const char* km = kd->ModelName();
				if (km && km[0]) {
					SkinCfg kcfg{};
					kcfg.enabled = true;
					kcfg.def = static_cast<uint16_t>(Config::skin_knife_def);
					kcfg.paint = Config::skin_knife_paint;
					kcfg.legacy = LookupLegacy(kcfg.def, kcfg.paint);
					SkinSdk::SetModel(vm, km);
					if (vm->m_pGameSceneNode())
						SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), MeshMask(true, kcfg.legacy));
				}
			}
		}
	}

	// HUD icon update - nerv parity: runs at most once per frame and ONLY when
	// a walk actually applied something (scheduled via s_pendingHudClear) or
	// during the short not-yet-created-HUD retry. The old fullRefresh-keyed
	// path re-cleared slots on every frame of the spawn window - that was the
	// visible "HUD updates multiple times".
	if (s_pendingHudClear > 0) {
		if (Config::skin_knife || !Config::SkinWeapon_Empty()) {
			if (ClearHudIconSlots()) {
				s_pendingHudClear = 0;
			} else {
				--s_pendingHudClear;
			}
		} else {
			s_pendingHudClear = 0;
		}
	}
	SetGlove(pawn);
	SetAgent(pawn);
}

void SkinChanger::OnFireEventClientSide(void* gameEvent)
{
	if (!gameEvent || !Config::skin_knife || Config::skin_knife_def <= 0)
		return;
	auto* ev = reinterpret_cast<IGameEvent*>(gameEvent);
	const char* name = ev->GetName();
	if (!name || _stricmp(name, "player_death"))
		return;
	CCSPlayerController* local = nullptr;
	C_CSPlayerPawn* pawn = H::SafeLocalAlive();
	if (pawn && I::GameEntity && I::GameEntity->Instance) {
		CBaseHandle h = pawn->m_hController();
		if (h.valid())
			local = I::GameEntity->Instance->Get<CCSPlayerController>(h);
	}
	if (!local)
		return;
	CCSPlayerController* attacker = ev->GetPlayerController("attacker");
	if (!attacker || attacker != local)
		return;
	const char* weapon = ev->GetString("weapon");
	if (!EventWeaponIsKnife(weapon))
		return;
	const char* icon = SkinSdk::KnifeIconName(Config::skin_knife_def);
	if (!icon || !icon[0])
		icon = SkinSdk::KnifeWeaponName(Config::skin_knife_def);
	if (icon && icon[0])
		ev->SetString("weapon", icon);
}

