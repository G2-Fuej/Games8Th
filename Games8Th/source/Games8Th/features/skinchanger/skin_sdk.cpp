#include "skin_sdk.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/vfunc/vfunc.h"
#include "../../utils/console/console.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/sdk/CUtlSymbolLarge.h"

namespace
{
	using FnVoid = void(__fastcall*)();
	using FnCall = void*(__fastcall*)();
    using FnEconSys = void*(__fastcall*)();
	using FnInvGet = void*(__fastcall*)();
	using FnStaticData = CEconItemDefinition*(__fastcall*)(C_EconItemView*);
	using FnSetAttr = void(__fastcall*)(C_EconItemView*, const char*, float);
	using FnSetModel = void(__fastcall*)(C_BaseEntity*, const char*);
	using FnSetMask = void(__fastcall*)(CGameSceneNode*, uint64_t);
	using FnUpdateSubclass = void(__fastcall*)(C_CSWeaponBase*);
	using FnVoidOne = void(__fastcall*)(C_CSWeaponBase*);
	using FnGetModelPath = const char* (__fastcall*)(C_EconItemView*);
	using FnUpdateSkin = void(__fastcall*)(C_CSWeaponBase*, bool);
	using FnUpdateComp = void(__fastcall*)(void*, bool);
	using FnUpdateCompSet = void(__fastcall*)(C_CSWeaponBase*, bool);
	using FnSetBody = void(__fastcall*)(C_CSPlayerPawn*, const char*, int);
	using FnUpdateBody = void(__fastcall*)(C_CSPlayerPawn*);
	using FnFindHud = void*(__fastcall*)(const char*);
	using FnClearHud = int64_t(__fastcall*)(void*, int, int64_t);
	using FnUpdateRows = void(__fastcall*)(void*);
	using FnLocalize = const char*(__fastcall*)(void*, const char*);

	void* g_client = nullptr;
	void* g_inventory = nullptr;
	void* g_fs = nullptr;
	void* g_localize = nullptr;
	FnEconSys g_getEcon = nullptr;
	FnInvGet g_invGet = nullptr;
	FnStaticData g_getStatic = nullptr;
	FnSetAttr g_setAttr = nullptr;
	FnSetModel g_setModel = nullptr;
	FnSetMask g_setMask = nullptr;
	FnUpdateSubclass g_updateSubclass = nullptr;
	FnVoidOne g_updateWeaponVm = nullptr;
	FnGetModelPath g_getModelPath = nullptr;
	FnUpdateSkin g_updateSkin = nullptr;
	FnUpdateComp g_updateComp = nullptr;
	FnUpdateCompSet g_updateCompSet = nullptr;
	FnSetBody g_setBody = nullptr;
	FnUpdateBody g_updateBody = nullptr;
	FnFindHud g_findHud = nullptr;
	FnClearHud g_clearHud = nullptr;
	FnUpdateRows g_updateRows = nullptr;
	FnLocalize g_findSafe = nullptr;
	bool g_inited = false;

	void* Rel32(void* insn)
	{
		if (!insn)
			return nullptr;
		return M::GetAbsoluteAddress(reinterpret_cast<uint8_t*>(insn), 1);
	}

	void* ScanClient(const char* pat)
	{
		return M::FindPattern("client.dll", pat);
	}

	void* ScanCall(const char* pat)
	{
		void* hit = ScanClient(pat);
		return Rel32(hit);
	}

	void* ScanMod(const char* mod, const char* pat)
	{
		return M::FindPattern(mod, pat);
	}

	struct KnifeRow { int def; const char* weapon; };
	static const KnifeRow kKnives[] = {
		{ 500, "weapon_bayonet" },
		{ 503, "weapon_knife_css" },
		{ 505, "weapon_knife_flip" },
		{ 506, "weapon_knife_gut" },
		{ 507, "weapon_knife_karambit" },
		{ 508, "weapon_knife_m9_bayonet" },
		{ 509, "weapon_knife_tactical" },
		{ 512, "weapon_knife_falchion" },
		{ 514, "weapon_knife_survival_bowie" },
		{ 515, "weapon_knife_butterfly" },
		{ 516, "weapon_knife_push" },
		{ 517, "weapon_knife_cord" },
		{ 518, "weapon_knife_canis" },
		{ 519, "weapon_knife_ursus" },
		{ 520, "weapon_knife_gypsy_jackknife" },
		{ 521, "weapon_knife_outdoor" },
		{ 522, "weapon_knife_stiletto" },
		{ 523, "weapon_knife_widowmaker" },
		{ 525, "weapon_knife_skeleton" },
		{ 526, "weapon_knife_kukri" },
	};
}

uint16_t CEconItemDefinition::DefIndex() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(this) + 0x10);
}
uint8_t CEconItemDefinition::Rarity() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(this) + 0x42);
}
const char* CEconItemDefinition::ItemBaseName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x70);
}
const char* CEconItemDefinition::ItemTypeName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x80);
}
const char* CEconItemDefinition::ModelName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x148);
}
int32_t CEconItemDefinition::StickerSupportCount() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + 0x168);
}
const char* CEconItemDefinition::IconName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x230);
}
const char* CEconItemDefinition::WeaponName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x260);
}

bool CEconItemDefinition::IsKnife(bool excludeDefault) const
{
	const char* t = ItemTypeName();
	if (!t || hash_32_fnv1a_const(t) != hash_32_fnv1a_const("#CSGO_Type_Knife"))
		return false;
	return excludeDefault ? DefIndex() >= 500 : true;
}
bool CEconItemDefinition::IsGlove(bool excludeDefault) const
{
	const char* t = ItemTypeName();
	if (!t || hash_32_fnv1a_const(t) != hash_32_fnv1a_const("#Type_Hands"))
		return false;
	const bool def = DefIndex() == 5028;
	return excludeDefault ? !def : true;
}
bool CEconItemDefinition::IsAgent(bool excludeDefault) const
{
	const char* t = ItemTypeName();
	if (!t || hash_32_fnv1a_const(t) != hash_32_fnv1a_const("#Type_CustomPlayer"))
		return false;
	const bool def = DefIndex() == 5036 || DefIndex() == 5037;
	return excludeDefault ? !def : true;
}
bool CEconItemDefinition::IsWeapon() const
{
	if (IsKnife(false) || IsGlove(false) || IsAgent(false))
		return false;
	const uint16_t def = DefIndex();
	if (def >= 1 && def <= 70)
		return true;
	return StickerSupportCount() >= 4;
}

uint8_t CPaintKit::IsUseLegacyModel() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(this) + 0xAE);
}

SkinUtlMap<int, CEconItemDefinition*>& CEconItemSchema::SortedItemDefinitionMap()
{
	return *reinterpret_cast<SkinUtlMap<int, CEconItemDefinition*>*>(
		reinterpret_cast<uint8_t*>(this) + 0x128);
}
SkinUtlMap<int, CPaintKit*>& CEconItemSchema::PaintKits()
{
	return *reinterpret_cast<SkinUtlMap<int, CPaintKit*>*>(
		reinterpret_cast<uint8_t*>(this) + 0x2F0);
}
CEconItemSchema* CEconItemSystem::Schema()
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<CEconItemSchema**>(reinterpret_cast<uint8_t*>(this) + 0x8);
}

void SkinSdk::Init()
{
	if (g_inited)
		return;
	g_inited = true;

	g_client = I::Get<void>("client.dll", "Source2Client00");
	// Andromeda's GetEconItemSystem helper. Current build rewrote the body
	// after the jne (old alloc-size constant B9 50.. gone), so the original
	// long pattern misses -> menu stuck on "Loading game items schema...".
	// Primary: runtime-proven short prefix (jne disp byte 81 pinned, same hit
	// mercey's changer resolves live on this build). Then unpinned-disp, then
	// the legacy full form for older builds.
	g_getEcon = reinterpret_cast<FnEconSys>(ScanClient(
	    "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81"));
	if (!g_getEcon)
		g_getEcon = reinterpret_cast<FnEconSys>(ScanClient(
			"48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85"));
	if (!g_getEcon)
		g_getEcon = reinterpret_cast<FnEconSys>(ScanClient(
			"48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 ? ? ? ? 48 89 5C 24 30 B9 50 00 00 00 48 89 74 24 40 4C 89 74 24 20 E8 ? ? ? ? 33 F6 48"));
	// Fallback via interface vtable if pattern miss (never happens but safe)
	if (!g_getEcon) {
		Con::PatternMiss("client", "GetEconItemSystem");
	}
	g_invGet = reinterpret_cast<FnInvGet>(ScanCall(
		"E8 ? ? ? ? 48 8B D8 E8 ? ? ? ? 8B 70"));
	// Andromeda's full C_EconItemView_GetStaticData (prefix + full tail) - use full for stability
	g_getStatic = reinterpret_cast<FnStaticData>(ScanClient(
		"40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? 48 85 DB 75 ? B9 ? ? ? ? 48 89 7C 24 ? E8 ? ? ? ? 33 FF 48 8B D8 48 85 C0 74 ? 48 8D 05 ? ? ? ? 48 89 7B ? B9 ? ? ? ? 48 89 03 E8 ? ? ? ? 48 85 C0 74 ? 48 8B C8 E8 ? ? ? ? 48 8B F8 48 8D 05 ? ? ? ? 48 89 7B ? 48 89 03 EB ? 48 8B DF 48 8B 7C 24 ? 48 89 1D ? ? ? ? 48 8B 4B ? 48 8B 5C 24 ? 48 85 C9 75"));
	if (!g_getStatic) {
		g_getStatic = reinterpret_cast<FnStaticData>(ScanClient(
			"40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? 48 85 DB 75 ? B9"));
		if (!g_getStatic) Con::PatternMiss("client", "GetStaticData");
	}
	g_setAttr = reinterpret_cast<FnSetAttr>(ScanCall(
		"E8 ? ? ? ? 66 41 0F 6E D4"));
	if (!g_setAttr) Con::PatternMiss("client", "SetAttributeValueByName");
	g_setModel = reinterpret_cast<FnSetModel>(ScanClient(
		"40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 40"));
	if (!g_setModel) Con::PatternMiss("client", "SetModel");
	g_setMask = reinterpret_cast<FnSetMask>(ScanClient(
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71"));
	if (!g_setMask) Con::PatternMiss("client", "SetMeshGroupMask");
	g_updateSubclass = reinterpret_cast<FnUpdateSubclass>(ScanClient(
		"4C 8B DC 53 48 81 EC ? ? ? ? 48 8B 41"));
	if (!g_updateSubclass) Con::PatternMiss("client", "UpdateSubclass");
	// Knife-swap support (verified unique on current build): post-subclass
	// viewmodel refresh + game-side model path resolver.
	g_updateWeaponVm = reinterpret_cast<FnVoidOne>(ScanClient(
		"40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 83 BB 88 03 00 00 00"));
	if (!g_updateWeaponVm) Con::PatternMiss("client", "WeaponGetViewModel");
	g_getModelPath = reinterpret_cast<FnGetModelPath>(ScanClient(
		"48 89 5C 24 10 56 48 83 EC 20 48 8B 1D ? ? ? ?"));
	if (!g_getModelPath) Con::PatternMiss("client", "WeaponGetModelPath");
	// Andromeda's UpdateSkin full pattern + short fallback
	g_updateSkin = reinterpret_cast<FnUpdateSkin>(ScanClient(
		"48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 0A 33 D2 48 8B CF E8 ? ? ? ? 48 8D 8F 90 19 00 00"));
	if (!g_updateSkin)
		g_updateSkin = reinterpret_cast<FnUpdateSkin>(ScanClient(
			"48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 0A"));
	if (!g_updateSkin) Con::PatternMiss("client", "UpdateSkin");
	// Composite: prefer Andromeda's CALL pattern (more stable across builds) then direct
	g_updateComp = reinterpret_cast<FnUpdateComp>(ScanCall(
		"E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24"));
	if (!g_updateComp)
		g_updateComp = reinterpret_cast<FnUpdateComp>(ScanClient(
			"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 44 0F B6 F2"));
	if (!g_updateComp) Con::PatternMiss("client", "UpdateCompositeMaterial");
	g_updateCompSet = reinterpret_cast<FnUpdateCompSet>(ScanClient(
		"40 55 53 41 57 48 8D AC 24 00 FE ? ?"));
	if (!g_updateCompSet) Con::PatternMiss("client", "UpdateCompositeMaterialSet");
	g_setBody = reinterpret_cast<FnSetBody>(ScanCall("E8 ? ? ? ? EB 0C 48 8B CF"));
	if (!g_setBody) Con::PatternMiss("client", "SetBodyGroup");
    // C_BaseEntity_UpdateBodyGroupChoice is a direct function in this build;
    // the old caller-relative pattern no longer exists.
    g_updateBody = reinterpret_cast<FnUpdateBody>(ScanClient(
        "48 8B C4 55 48 8B EC 48 83 EC 70 48 89 58 10 48 89 70 18 48"));
	if (!g_updateBody) Con::PatternMiss("client", "UpdateBodyGroupChoice");
	g_findHud = reinterpret_cast<FnFindHud>(ScanClient(
		"40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24 ? 48 8D 88 58 02 00 00"));
	if (!g_findHud) Con::PatternMiss("client", "FindHudElement");
	g_clearHud = reinterpret_cast<FnClearHud>(ScanCall("E8 ? ? ? ? 8B F8 C6 84 24"));
	if (!g_clearHud) Con::PatternMiss("client", "ClearHudWeaponIcon");
	g_updateRows = reinterpret_cast<FnUpdateRows>(ScanClient(
		"48 89 5C 24 10 57 48 83 EC 30 33 FF 48 8B D9 39 79 38"));
	if (!g_updateRows) Con::PatternMiss("client", "UpdateWeaponRows");
	g_findSafe = reinterpret_cast<FnLocalize>(ScanMod("localize.dll",
		"40 56 57 48 83 EC ? 48 8B F2 48 8B F9 48 85 D2 0F 84"));
	if (!g_findSafe) Con::Warn("SkinSdk localize FindSafe miss (non-fatal)");

	g_fs = I::Get<void>("filesystem_stdio.dll", "VFileSystem017");
	if (!g_fs)
		g_fs = I::Get<void>("filesystem_stdio", "VFileSystem017");
	g_localize = I::Get<void>("localize.dll", "Localize_001");
	if (!g_localize)
		g_localize = I::Get<void>("localize", "Localize_001");

	// Detailed ok/warn per critical weapon path
	if (!g_getEcon || !g_getStatic || !g_setAttr || !g_setModel || !g_updateSkin)
		Con::Error("SkinSdk critical miss: econ=%p static=%p attr=%p setModel=%p skin=%p (weapon knife will fail)", (void*)g_getEcon, (void*)g_getStatic, (void*)g_setAttr, (void*)g_setModel, (void*)g_updateSkin);
	else
		Con::Ok("SkinSdk client=%p econ=%p invGet=%p static=%p attr=%p setModel=%p mask=%p skin=%p comp=%p compSet=%p hud=%p fs=%p loc=%p",
			g_client, (void*)g_getEcon, (void*)g_invGet, (void*)g_getStatic, (void*)g_setAttr,
			(void*)g_setModel, (void*)g_setMask, (void*)g_updateSkin, (void*)g_updateComp, (void*)g_updateCompSet, (void*)g_findHud, g_fs, g_localize);
}

void* SkinSdk::Source2Client()
{
	if (!g_client)
		g_client = I::Get<void>("client.dll", "Source2Client00");
	return g_client;
}

CEconItemSystem* SkinSdk::EconItemSystem()
{
    if (!g_getEcon)
        return nullptr;
    void* sys = nullptr;
    __try { sys = g_getEcon(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { sys = nullptr; }
	return reinterpret_cast<CEconItemSystem*>(sys);
}

CEconItemSchema* SkinSdk::EconSchema()
{
	CEconItemSystem* sys = EconItemSystem();
	if (!sys || !Mem::IsUserPtr(sys))
		return nullptr;
	CEconItemSchema* s = nullptr;
	__try { s = sys->Schema(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { s = nullptr; }
	return (s && Mem::IsUserPtr(s)) ? s : nullptr;
}

void* SkinSdk::LocalInventory()
{
	if (!g_invGet)
		return nullptr;
	void* mgr = nullptr;
	__try { mgr = g_invGet(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { mgr = nullptr; }
	if (!mgr || !Mem::IsUserPtr(mgr))
		return nullptr;
	void* inv = nullptr;
	__try { inv = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mgr) + 0x3F540); }
	__except (EXCEPTION_EXECUTE_HANDLER) { inv = nullptr; }
	return (inv && Mem::IsUserPtr(inv)) ? inv : nullptr;
}

uint64_t SkinSdk::InventorySteamId()
{
	void* inv = LocalInventory();
	if (!inv)
		return 0;
	uint64_t id = 0;
	__try { id = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(inv) + 0x10); }
	__except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
	return id;
}

void* SkinSdk::FileSystem() { return g_fs; }

bool SkinSdk::FileExistsGame(const char* path)
{
	if (!g_fs || !path || !path[0])
		return false;
	bool ok = false;
	__try { ok = M::vfunc<bool, 21U>(g_fs, path, "GAME"); }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	return ok;
}

const char* SkinSdk::LocalizeSafe(const char* token, const char* fallback)
{
	if (token && token[0] && g_localize && g_findSafe) {
		const char* loc = nullptr;
		__try { loc = g_findSafe(g_localize, token); }
		__except (EXCEPTION_EXECUTE_HANDLER) { loc = nullptr; }
		if (loc && loc[0] && loc[0] != '#')
			return loc;
	}
	return (fallback && fallback[0]) ? fallback : "Unknown";
}

CEconItemDefinition* SkinSdk::FindDefByIndex(uint16_t defIdx)
{
	CEconItemSchema* schema = EconSchema();
	if (!schema)
		return nullptr;
	auto& map = schema->SortedItemDefinitionMap();
	if (!map.m_data || map.m_size <= 0 || map.m_size > 20000)
		return nullptr;
	for (int i = 0; i < map.m_size; ++i) {
		CEconItemDefinition* d = map.m_data[i].m_value;
		if (d && Mem::IsUserPtr(d) && d->DefIndex() == defIdx)
			return d;
	}
	return nullptr;
}

CPaintKit* SkinSdk::FindPaintKit(int paintId)
{
	if (paintId <= 0)
		return nullptr;
	CEconItemSchema* schema = EconSchema();
	if (!schema)
		return nullptr;
	auto& kits = schema->PaintKits();
	if (!kits.m_data || kits.m_size <= 0 || kits.m_size > 20000)
		return nullptr;
	for (int i = 0; i < kits.m_size; ++i) {
		CPaintKit* k = kits.m_data[i].m_value;
		if (k && Mem::IsUserPtr(k) && k->nID == paintId)
			return k;
	}
	return nullptr;
}

CEconItemDefinition* SkinSdk::GetStaticData(C_EconItemView* view)
{
	if (!view || !g_getStatic)
		return nullptr;
	CEconItemDefinition* d = nullptr;
	__try { d = g_getStatic(view); }
	__except (EXCEPTION_EXECUTE_HANDLER) { d = nullptr; }
	return (d && Mem::IsUserPtr(d)) ? d : nullptr;
}

void SkinSdk::SetAttributeValueByName(C_EconItemView* view, const char* name, float value)
{
	if (!view || !name || !g_setAttr)
		return;
	__try { g_setAttr(view, name, value); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool SkinSdk::SetModel(C_BaseEntity* ent, const char* model)
{
	if (!ent || !model || !g_setModel)
		return false;
	bool ok = false;
	__try { g_setModel(ent, model); ok = true; }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return ok;
}

void SkinSdk::SetMeshGroupMask(CGameSceneNode* node, uint64_t mask)
{
	if (!node || !g_setMask)
		return;
	__try { g_setMask(node, mask); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::PostDataUpdate(C_BaseEntity* entity, int /*updateType*/)
{
	if (!entity || !Mem::ValidEntity(entity))
		return;
	// v1.3 behavior: refresh via the scene node (slot 25). The interim
	// CEntityInstance slot-10 rewrite corrupted weapon deploy state
	// (knife switch broke, wrong models stuck on other weapons).
	CGameSceneNode* node = entity->m_pGameSceneNode();
	if (!node)
		return;
	__try { M::vfunc<void, 25U>(node, 0, 0); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

	void SkinSdk::UpdateSubclass(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateSubclass)
		return;
	__try { g_updateSubclass(weapon); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// MurmurHash2 lowercase variant - the exact hash CS2 uses for subclass id
// registration. Subclass token = murmur2(decimal string of def index).
namespace {
	std::uint32_t Murmur2Lower(const char* str, int len, std::uint32_t seed)
	{
		constexpr auto m{ 0x5bd1e995 };
		constexpr auto r{ 24 };
		auto h = seed ^ len;
		int i = 0;
		auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
		while (len >= 4) {
			std::uint32_t k =
				static_cast<std::uint32_t>(static_cast<unsigned char>(lower(str[i]))) |
				(static_cast<std::uint32_t>(static_cast<unsigned char>(lower(str[i + 1]))) << 8) |
				(static_cast<std::uint32_t>(static_cast<unsigned char>(lower(str[i + 2]))) << 16) |
				(static_cast<std::uint32_t>(static_cast<unsigned char>(lower(str[i + 3]))) << 24);
			k *= m; k ^= k >> r; k *= m;
			h *= m; h ^= k;
			i += 4; len -= 4;
		}
		switch (len) {
		case 3: h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(lower(str[i + 2]))) << 16; [[fallthrough]];
		case 2: h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(lower(str[i + 1]))) << 8; [[fallthrough]];
		case 1: h ^= static_cast<unsigned char>(lower(str[i])); h *= m;
		}
		h ^= h >> 13; h *= m; h ^= h >> 15;
		return h;
	}
}

std::uint32_t SkinSdk::MakeSubclassToken(std::uint16_t defIndex)
{
	char buf[8]{};
	const int n = std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(defIndex));
	if (n <= 0 || n >= static_cast<int>(sizeof(buf)))
		return 0;
	return Murmur2Lower(buf, n, 0x31415926);
}

void SkinSdk::UpdateWeaponViewModel(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateWeaponVm)
		return;
	__try { g_updateWeaponVm(weapon); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

const char* SkinSdk::WeaponModelPath(C_EconItemView* view)
{
	if (!view || !g_getModelPath)
		return nullptr;
	const char* path = nullptr;
	__try { path = g_getModelPath(view); }
	__except (EXCEPTION_EXECUTE_HANDLER) { path = nullptr; }
	return (path && path[0] && Mem::IsReadable(path, 8)) ? path : nullptr;
}

void SkinSdk::UpdateSkin(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateSkin)
		return;
	__try { g_updateSkin(weapon, true); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateCompositeMaterial(void* compositeOwner)
{
	if (!compositeOwner || !g_updateComp)
		return;
	__try { g_updateComp(compositeOwner, true); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateCompositeMaterialSet(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateCompSet)
		return;
	__try { g_updateCompSet(weapon, false); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::SetBodyGroup(C_CSPlayerPawn* pawn)
{
	if (!pawn || !g_setBody)
		return;
	__try { g_setBody(pawn, "first_or_third_person", 1); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateBodyGroupChoice(C_CSPlayerPawn* pawn)
{
	if (!pawn || !g_updateBody)
		return;
	__try { g_updateBody(pawn); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::RegenerateWeaponSkin(C_CSWeaponBase* weapon, bool force)
{
	// Disabled: the interim build fired g_updateCompSet(weapon, force) on
	// spawn/deploy bursts with an unverified arg layout - contributed to
	// knife/pistol model cross-contamination. Kept as no-op so call sites
	// and the header stay stable.
	(void)weapon; (void)force;
}

void* SkinSdk::FindHudElement(const char* name)
{
	if (!name || !g_findHud)
		return nullptr;
	void* h = nullptr;
	__try { h = g_findHud(name); }
	__except (EXCEPTION_EXECUTE_HANDLER) { h = nullptr; }
	return h;
}

int SkinSdk::ClearHudWeaponIcon(void* hudWeapons, int slot, int64_t unk)
{
	if (!hudWeapons || !g_clearHud)
		return -1;
	int64_t r = -1;
	__try { r = g_clearHud(hudWeapons, slot, unk); }
	__except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
	return static_cast<int>(r);
}

void SkinSdk::UpdateWeaponRows(void* hudWeapons)
{
	if (!hudWeapons || !g_updateRows)
		return;
	__try { g_updateRows(hudWeapons); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* SkinSdk::CompositeOwner(C_CSWeaponBase* weapon)
{
	if (!weapon)
		return nullptr;
	return reinterpret_cast<uint8_t*>(weapon) + 0x608;
}

static C_BaseEntity* HudChildByName(C_CSPlayerPawn* pawn, const char* needle)
{
	if (!pawn)
		return nullptr;
	static uint32_t s_hud = 0;
	if (!s_hud)
		s_hud = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_hHudModelArms"));
	if (!s_hud || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	CBaseHandle h{};
	__try { h = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uint8_t*>(pawn) + s_hud); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!h.valid())
		return nullptr;
	// Index-only resolve: serial-checked Get(CBaseHandle) can reject hud-model
	// handles whose stored serial carries the flags bit. Matches Andromeda.
	C_BaseEntity* arms = (I::GameEntity && I::GameEntity->Instance)
		? I::GameEntity->Instance->Get<C_BaseEntity>(h.index()) : nullptr;
	if (!arms)
		return nullptr;
	CGameSceneNode* node = arms->m_pGameSceneNode();
	if (!node || !Mem::IsUserPtr(node))
		return nullptr;
	__try {
		for (CGameSceneNode* child = node->m_pChild(); child && Mem::IsUserPtr(child); child = child->m_pNextSibling()) {
			CEntityInstance* owner = child->m_pOwner();
			if (!owner || !Mem::IsUserPtr(owner))
				continue;
			auto* ent = reinterpret_cast<C_BaseEntity*>(owner);
			if (!ent->IsViewmodel())
				continue;
			if (!needle)
				return ent;
			CGameSceneNode* n = ent->m_pGameSceneNode();
			if (!n || !Mem::IsUserPtr(n))
				continue;
			const char* mn = n->m_modelState().m_ModelName().String();
			if (mn && strstr(mn, needle))
				return ent;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return nullptr;
}

// Andromeda semantics: viewmodel's m_hOwnerEntity resolves to the owning
// weapon entity. Engine stores hud-model handles with serial that may carry
// the flags bit (CEntityInstance::handle() strips it) - resolve by INDEX
// only, like Andromeda's CHandle::Get (GetBaseEntity(GetEntryIndex())).
static C_BaseEntity* VmOwnerEntity(C_BaseEntity* ent)
{
	if (!ent || !ent->m_hOwnerEntity().valid() || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	return I::GameEntity->Instance->Get<C_BaseEntity>(ent->m_hOwnerEntity().index());
}

C_BaseEntity* SkinSdk::GetViewModel(C_CSPlayerPawn* pawn)
{
	if (!pawn || !Mem::IsUserPtr(pawn))
		return nullptr;
	// Andromeda GetLocalActiveWeapon: m_hActiveWeapon INDEX-only resolve -
	// serial-checked Get can reject live weapon handles (flags bit in serial).
	C_CSWeaponBase* wpn = nullptr;
	if (CCSPlayer_WeaponServices* sws = pawn->GetWeaponServices(); sws && Mem::IsUserPtr(sws)) {
		const CBaseHandle ha = sws->m_hActiveWeapon();
		if (ha.valid() && I::GameEntity && I::GameEntity->Instance)
			wpn = I::GameEntity->Instance->Get<C_CSWeaponBase>(ha.index());
	}
	if (!wpn)
		wpn = pawn->GetActiveWeapon();

	if (!wpn)
		return HudChildByName(pawn, nullptr);
	static uint32_t s_hud = 0;
	if (!s_hud)
		s_hud = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_hHudModelArms"));
	if (!s_hud || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	CBaseHandle h{};
	__try { h = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uint8_t*>(pawn) + s_hud); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	C_BaseEntity* arms = I::GameEntity->Instance->Get<C_BaseEntity>(h.index());
	if (!arms || !Mem::ValidEntity(arms))
		return nullptr;
	CGameSceneNode* node = arms->m_pGameSceneNode();
	if (!node || !Mem::IsUserPtr(node))
		return nullptr;
	__try {
		for (CGameSceneNode* child = node->m_pChild(); child && Mem::IsUserPtr(child); child = child->m_pNextSibling()) {
			CEntityInstance* owner = child->m_pOwner();
			if (!owner || !Mem::IsUserPtr(owner))
				continue;
			auto* ent = reinterpret_cast<C_BaseEntity*>(owner);
			if (!ent->IsViewmodel())
				continue;
			if (VmOwnerEntity(ent) == reinterpret_cast<C_BaseEntity*>(wpn))
				return ent;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return nullptr;
}

C_BaseEntity* SkinSdk::GetKnifeModel(C_CSPlayerPawn* pawn)
{
	return HudChildByName(pawn, "knife");
}

const char* SkinSdk::KnifeWeaponName(int defIdx)
{
	for (const auto& k : kKnives)
		if (k.def == defIdx)
			return k.weapon;
	return "";
}

const char* SkinSdk::KnifeIconName(int defIdx)
{
	const char* w = KnifeWeaponName(defIdx);
	if (w && strncmp(w, "weapon_", 7) == 0)
		return w + 7;
	return w ? w : "";
}

bool SkinSdk::IsSkinnableGloveDef(int defIdx)
{
	switch (defIdx) {
	case kGloveBloodhound:
	case kGloveBrokenFang:
	case kGloveSporty:
	case kGloveSlick:
	case kGloveHandwraps:
	case kGloveMotorcycle:
	case kGloveSpecialist:
	case kGloveHydra:
		return true;
	default:
		return false;
	}
}

