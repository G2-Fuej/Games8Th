#include "skin_items.h"
#include "skin_sdk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <Windows.h>

#include "../../../../external/json/json.hpp"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"

static SkinItems g_items;
// Guards `items` (and skinsReady/skins/simple members) against concurrent
// access: the menu/Present thread mutates via Scan/EnsureSkins while the
// game thread reads via IsLegacySkin/FirstAgentModel on every FSN frame.
// Zero-init SRWLOCK is manual-map safe.
static SRWLOCK g_itemsLock = SRWLOCK_INIT;

const char* SkinItems::SimpleName(uint16_t def, const char* schemaWeapon)
{
	switch (def) {
	case 1: return "weapon_deagle";
	case 2: return "weapon_elite";
	case 3: return "weapon_fiveseven";
	case 4: return "weapon_glock";
	case 7: return "weapon_ak47";
	case 8: return "weapon_aug";
	case 9: return "weapon_awp";
	case 10: return "weapon_famas";
	case 11: return "weapon_g3sg1";
	case 13: return "weapon_galilar";
	case 14: return "weapon_m249";
	case 16: return "weapon_m4a1";
	case 17: return "weapon_mac10";
	case 19: return "weapon_p90";
	case 23: return "weapon_mp5sd";
	case 24: return "weapon_ump45";
	case 25: return "weapon_xm1014";
	case 26: return "weapon_bizon";
	case 27: return "weapon_mag7";
	case 28: return "weapon_negev";
	case 29: return "weapon_sawedoff";
	case 30: return "weapon_tec9";
	case 32: return "weapon_hkp2000";
	case 33: return "weapon_mp7";
	case 34: return "weapon_mp9";
	case 35: return "weapon_nova";
	case 36: return "weapon_p250";
	case 38: return "weapon_scar20";
	case 39: return "weapon_sg556";
	case 40: return "weapon_ssg08";
	case 60: return "weapon_m4a1_silencer";
	case 61: return "weapon_usp_silencer";
	case 63: return "weapon_cz75a";
	case 64: return "weapon_revolver";
	case 500: return "weapon_bayonet";
	case 503: return "weapon_knife_css";
	case 505: return "weapon_knife_flip";
	case 506: return "weapon_knife_gut";
	case 507: return "weapon_knife_karambit";
	case 508: return "weapon_knife_m9_bayonet";
	case 509: return "weapon_knife_tactical";
	case 512: return "weapon_knife_falchion";
	case 514: return "weapon_knife_survival_bowie";
	case 515: return "weapon_knife_butterfly";
	case 516: return "weapon_knife_push";
	case 517: return "weapon_knife_cord";
	case 518: return "weapon_knife_canis";
	case 519: return "weapon_knife_ursus";
	case 520: return "weapon_knife_gypsy_jackknife";
	case 521: return "weapon_knife_outdoor";
	case 522: return "weapon_knife_stiletto";
	case 523: return "weapon_knife_widowmaker";
	case 525: return "weapon_knife_skeleton";
	case 526: return "weapon_knife_kukri";
	case SkinSdk::kGloveBloodhound: return "studded_bloodhound_gloves";
	case SkinSdk::kGloveBrokenFang: return "studded_brokenfang_gloves";
	case SkinSdk::kGloveSporty: return "sporty_gloves";
	case SkinSdk::kGloveSlick: return "slick_gloves";
	case SkinSdk::kGloveHandwraps: return "leather_handwraps";
	case SkinSdk::kGloveMotorcycle: return "motorcycle_gloves";
	case SkinSdk::kGloveSpecialist: return "specialist_gloves";
	case SkinSdk::kGloveHydra: return "studded_hydra_gloves";
	default: break;
	}
	if (schemaWeapon && schemaWeapon[0] && schemaWeapon[0] != '#') {
		if (strstr(schemaWeapon, "grenade") || strstr(schemaWeapon, "flashbang") || strstr(schemaWeapon, "molotov")
			|| strstr(schemaWeapon, "decoy") || strstr(schemaWeapon, "c4") || strstr(schemaWeapon, "taser")
			|| strstr(schemaWeapon, "healthshot") || strstr(schemaWeapon, "shield") || strstr(schemaWeapon, "bumpmine")
			|| strstr(schemaWeapon, "breachcharge") || strstr(schemaWeapon, "tablet") || strstr(schemaWeapon, "fists")
			|| strstr(schemaWeapon, "axe") || strstr(schemaWeapon, "hammer") || strstr(schemaWeapon, "spanner"))
			return nullptr;

		if (!strncmp(schemaWeapon, "weapon_", 7) || !strncmp(schemaWeapon, "glove_", 6)
			|| !strncmp(schemaWeapon, "studded_", 8) || !strncmp(schemaWeapon, "sporty_", 7)
			|| !strncmp(schemaWeapon, "motorcycle_", 11) || !strncmp(schemaWeapon, "specialist_", 11)
			|| !strncmp(schemaWeapon, "handwrap_", 9) || !strncmp(schemaWeapon, "slick_", 6)
			|| !strncmp(schemaWeapon, "leather_", 8) || !strncmp(schemaWeapon, "bloodhound_", 11)
			|| !strncmp(schemaWeapon, "hydra_", 6) || !strncmp(schemaWeapon, "brokenfang_", 11))
			return schemaWeapon;
	}
	return nullptr;
}

static int8_t AgentTeam(const char* model, const char* baseName, const char* iconName)
{
	auto score = [](const char* s) -> int8_t {
		if (!s || !s[0]) return 0;
		const char* base = strrchr(s, '/');
		if (!base) base = strrchr(s, '\\');
		base = base ? base + 1 : s;
		if (!_strnicmp(base, "ctm_", 4)) return 3;
		if (!_strnicmp(base, "tm_", 3)) return 2;
		if (strstr(s, "/ctm_") || strstr(s, "\\ctm_") || strstr(s, "/ct/") || strstr(s, "\\ct\\"))
			return 3;
		if (strstr(s, "/tm_") || strstr(s, "\\tm_") || strstr(s, "/legacy/")
			|| strstr(s, "\\legacy\\") || strstr(s, "/t/") || strstr(s, "\\t\\"))
			return 2;
		if (strstr(s, "ctm_") || strstr(s, "customplayer_ct")) return 3;
		if (strstr(s, "tm_") || strstr(s, "customplayer_tm") || strstr(s, "terror")) return 2;
		return 0;
	};
	if (int8_t t = score(model)) return t;
	if (int8_t t = score(iconName)) return t;
	if (int8_t t = score(baseName)) return t;
	return 0;
}

static SRWLOCK g_fsCacheLock = SRWLOCK_INIT;

static bool KitFits(const char* simple, const char* kit, std::unordered_map<std::string, bool>& cache)
{
	if (!simple || !simple[0] || !kit || !kit[0])
		return false;
	const std::string key = std::string(simple) + "|" + kit;
	{
		AcquireSRWLockShared(&g_fsCacheLock);
		auto it = cache.find(key);
		if (it != cache.end()) {
			bool r = it->second;
			ReleaseSRWLockShared(&g_fsCacheLock);
			return r;
		}
		ReleaseSRWLockShared(&g_fsCacheLock);
	}
	char path[384];
	if (sprintf_s(path, "panorama/images/econ/default_generated/%s_%s_light_png.vtex_c", simple, kit) <= 0) {
		AcquireSRWLockExclusive(&g_fsCacheLock);
		cache[key] = false;
		ReleaseSRWLockExclusive(&g_fsCacheLock);
		return false;
	}
	const bool hit = SkinSdk::FileExistsGame(path);
	AcquireSRWLockExclusive(&g_fsCacheLock);
	cache[key] = hit;
	ReleaseSRWLockExclusive(&g_fsCacheLock);
	return hit;
}

static void CollectAliases(const std::string& simple, std::vector<std::string>& out)
{
	out.clear();
	if (simple.empty()) return;
	out.push_back(simple);
	const char* s = simple.c_str();
	if (!strcmp(s, "bloodhound_gloves")) out.push_back("studded_bloodhound_gloves");
	else if (!strcmp(s, "studded_bloodhound_gloves")) out.push_back("bloodhound_gloves");
	else if (!strcmp(s, "hydra_gloves")) out.push_back("studded_hydra_gloves");
	else if (!strcmp(s, "brokenfang_gloves")) out.push_back("studded_brokenfang_gloves");
	else if (!strcmp(s, "handwraps") || !strcmp(s, "leather_handwraps")) {
		out.push_back("leather_handwraps");
		out.push_back("handwraps");
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool SkinItems::EnsurePaintList()
{
	if (paintReady && !paint.empty())
		return true;
	CEconItemSchema* schema = SkinSdk::EconSchema();
	if (!schema)
		return false;
	auto& kits = schema->PaintKits();
	if (kits.m_size <= 0 || kits.m_size > 20000 || !kits.m_data)
		return false;
	std::vector<CPaintKit*> local;
	local.reserve(static_cast<size_t>(kits.m_size));
	std::unordered_set<int> seen;
	for (int i = 0; i < kits.m_size; ++i) {
		CPaintKit* k = kits.m_data[i].m_value;
		if (!k || !Mem::IsUserPtr(k) || k->nID <= 0 || k->nID == 9001)
			continue;
		if (!k->sName || !k->sName[0])
			continue;
		if (!seen.insert(k->nID).second)
			continue;
		local.push_back(k);
	}
	// paint is menu-thread-only data; commit under the items lock anyway so
	// Scan/EnsureSkins callers that already hold the lock stay deadlock-free
	// (SRWLOCK is not recursive - they call this BEFORE acquiring).
	AcquireSRWLockExclusive(&g_itemsLock);
	paint.swap(local);
	paintReady = !paint.empty();
	const bool ready = paintReady;
	ReleaseSRWLockExclusive(&g_itemsLock);
	return ready;
}

void SkinItems::Scan()
{
	if (modelsReady)
		return;
	CEconItemSchema* schema = SkinSdk::EconSchema();
	// The catalog belongs to the shared econ schema. LocalInventory is created
	// later and is not required to enumerate weapon, knife, glove, or agent defs;
	// gating on it left the menu stuck on "Loading game items schema...".
	if (!schema)
		return;
	auto& map = schema->SortedItemDefinitionMap();
	if (map.m_size <= 0 || map.m_size > 20000 || !map.m_data)
		return;

	// Build into a local list first; commit under the lock so a concurrent
	// game-thread IsLegacySkin/FirstAgentModel never sees a half-built or
	// reallocating vector.
	std::vector<Item> local;
	local.reserve(128);
	std::unordered_set<uint16_t> seenDef;
	std::unordered_set<std::string> seenSimple;
	seenDef.reserve(256);

	for (int i = 0; i < map.m_size; ++i) {
		CEconItemDefinition* p = map.m_data[i].m_value;
		if (!p || !Mem::IsUserPtr(p))
			continue;
		const bool isKnife = p->IsKnife(true);
		const bool isGlove = p->IsGlove(true) && SkinSdk::IsSkinnableGloveDef(p->DefIndex());
		const bool isAgent = p->IsAgent(true);
		const bool isWeapon = !isKnife && !isGlove && !isAgent && p->IsWeapon();
		if (!isWeapon && !isKnife && !isGlove && !isAgent)
			continue;
		const char* baseName = p->ItemBaseName();
		if (!baseName || !baseName[0])
			continue;
		const uint16_t def = p->DefIndex();
		if (def == 31 || (def >= 43 && def <= 49) || def == 57 || def == 68 || def == 69 || def == 70 || def == 72 || def == 75 || def == 76 || def == 78 || (def >= 80 && def <= 85))
			continue;
		if (!seenDef.insert(def).second)
			continue;

		Item row;
		row.def = def;
		row.rarity = p->Rarity();
		row.name = SkinSdk::LocalizeSafe(baseName, baseName);
		if (isWeapon) row.type = Weapon;
		else if (isKnife) row.type = Knife;
		else if (isGlove) row.type = Glove;
		else row.type = Agent;

		if (isAgent) {
			row.skinsReady = true;
			const char* model = p->ModelName();
			const char* icon = p->IconName();
			row.team = AgentTeam(model, baseName, icon);
			if (row.team != 2 && row.team != 3)
				continue;
			if (model && model[0]) row.icon = model;
			else if (icon && icon[0]) row.icon = icon;
		} else {
			const char* simple = SimpleName(def, p->WeaponName());
			if (!simple || !simple[0])
				continue;
			if (!isGlove) {
				std::string key = std::to_string(static_cast<int>(row.type)) + "|" + simple;
				if (!seenSimple.insert(key).second)
					continue;
			}
			row.simple = simple;
			row.skinsReady = false;
			// kitCache is mutated under g_itemsLock by EnsureSkins/LoadDisk -
			// read it under the same lock or this races the map.
			{
				AcquireSRWLockShared(&g_itemsLock);
				auto cacheIt = kitCache.find(simple);
				if (cacheIt != kitCache.end()) {
					row.skins = cacheIt->second;
					row.skinsReady = true;
				}
				ReleaseSRWLockShared(&g_itemsLock);
			}
		}
		local.emplace_back(std::move(row));
	}

	std::sort(local.begin(), local.end(), [](const Item& a, const Item& b) {
		if (a.type != b.type) return a.type < b.type;
		if (a.type == Agent && a.team != b.team) return a.team < b.team;
		return a.name < b.name;
	});
	local.erase(std::unique(local.begin(), local.end(), [](const Item& a, const Item& b) {
		if (a.type != b.type) return false;
		if (a.def == b.def) return true;
		if (a.type == Glove) return false;
		return a.name == b.name;
	}), local.end());

	AcquireSRWLockExclusive(&g_itemsLock);
	items.swap(local);
	modelsReady = true;
	const size_t nItems = items.size();
	ReleaseSRWLockExclusive(&g_itemsLock);

	// No lock held here - each takes its own section (SRWLOCK is not recursive).
	EnsurePaintList();
	LoadDisk();
	ApplyCache();
	AcquireSRWLockShared(&g_itemsLock);
	const size_t nKits = kitCache.size();
	ReleaseSRWLockShared(&g_itemsLock);
	Con::Ok("SkinItems models=%zu kits=%zu", nItems, nKits);
}

std::string SkinItems::CachePath()
{
	char dir[MAX_PATH]{};
	const DWORD n = GetEnvironmentVariableA("USERPROFILE", dir, sizeof(dir));
	if (n == 0 || n >= sizeof(dir))
		return "inv_skin_cache.json";
	std::string p = std::string(dir) + "\\Documents\\Games8Th";
	CreateDirectoryA(p.c_str(), nullptr);
	return p + "\\inv_skin_cache.json";
}

void SkinItems::LoadDisk()
{
	std::ifstream f(CachePath());
	if (!f.is_open())
		return;
	nlohmann::json doc;
	try { f >> doc; }
	catch (...) { return; }
	if (!doc.is_object() || doc.value("v", 0) != 3 || !doc.contains("kits") || !doc["kits"].is_object())
		return;
	std::unordered_map<std::string, std::vector<Skin>> pendingKits;
	for (auto it = doc["kits"].begin(); it != doc["kits"].end(); ++it) {
		if (!it.value().is_array())
			continue;
		std::vector<Skin> skins;
		for (const auto& e : it.value()) {
			if (!e.is_object()) continue;
			Skin s;
			s.id = e.value("id", 0);
			s.rarity = e.value("r", 0);
			s.legacy = e.value("leg", false);
			s.token = e.value("tok", std::string());
			s.name = e.value("n", std::string());
			if (s.id <= 0) continue;
			if (s.name.empty()) s.name = s.token.empty() ? "Unknown" : s.token;
			skins.emplace_back(std::move(s));
		}
		if (!skins.empty())
			pendingKits.emplace(it.key(), std::move(skins));
	}
	// kitCache is read under g_itemsLock elsewhere (Scan/EnsureSkins) - commit
	// under the exclusive lock instead of mutating it bare.
	AcquireSRWLockExclusive(&g_itemsLock);
	for (auto& kv : pendingKits)
		kitCache[kv.first] = std::move(kv.second);
	ReleaseSRWLockExclusive(&g_itemsLock);
}

void SkinItems::SaveDisk()
{
	// Exclusive: serializes concurrent EnsureSkins writers (file trunc race)
	// and keeps the iteration in sync with kitCache mutation elsewhere.
	AcquireSRWLockExclusive(&g_itemsLock);
	if (kitCache.empty()) {
		ReleaseSRWLockExclusive(&g_itemsLock);
		return;
	}
	nlohmann::json kits = nlohmann::json::object();
	for (const auto& kv : kitCache) {
		if (kv.second.empty()) continue;
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& s : kv.second)
			arr.push_back({ {"id", s.id}, {"r", s.rarity}, {"leg", s.legacy}, {"tok", s.token}, {"n", s.name} });
		kits[kv.first] = std::move(arr);
	}
	ReleaseSRWLockExclusive(&g_itemsLock);
	nlohmann::json doc = { {"v", 3}, {"kits", kits} };
	std::ofstream f(CachePath(), std::ios::trunc);
	if (f.is_open())
		f << doc.dump(1, '\t');
}

void SkinItems::ApplyCache()
{
	if (kitCache.empty())
		return;
	AcquireSRWLockExclusive(&g_itemsLock);
	for (auto& item : items) {
		if (item.skinsReady || item.simple.empty())
			continue;
		auto it = kitCache.find(item.simple);
		if (it == kitCache.end())
			continue;
		item.skins = it->second;
		item.skinsReady = true;
	}
	ReleaseSRWLockExclusive(&g_itemsLock);
}

SkinItems::Item* SkinItems::Find(uint16_t def)
{
	for (auto& it : items)
		if (it.def == def)
			return &it;
	return nullptr;
}

std::vector<SkinItems::Skin> SkinItems::BuildKits(const std::string& simple)
{
	std::vector<Skin> out;
	if (simple.empty() || !EnsurePaintList())
		return out;
	std::vector<std::string> aliases;
	CollectAliases(simple, aliases);
	out.reserve(128);
	std::unordered_set<int> seen;
	for (CPaintKit* k : paint) {
		if (!k || !k->sName || !k->sName[0] || k->nID <= 0)
			continue;
		bool fit = false;
		for (const auto& a : aliases) {
			if (KitFits(a.c_str(), k->sName, fsCache)) {
				fit = true;
				break;
			}
		}
		if (!fit || !seen.insert(k->nID).second)
			continue;
		Skin s;
		s.id = k->nID;
		s.rarity = k->nRarity;
		s.legacy = k->IsUseLegacyModel() != 0;
		s.token = k->sName;
		s.name = SkinSdk::LocalizeSafe(k->sDescriptionTag, k->sName);
		out.emplace_back(std::move(s));
	}
	std::sort(out.begin(), out.end(), [](const Skin& a, const Skin& b) {
		if (a.rarity != b.rarity) return a.rarity > b.rarity;
		if (a.name != b.name) return a.name < b.name;
		return a.id < b.id;
	});
	return out;
}

bool SkinItems::EnsureSkins(uint16_t def)
{
	if (!modelsReady)
		Scan();

	std::string simple;
	{
		AcquireSRWLockExclusive(&g_itemsLock);
		Item* item = Find(def);
		if (!item) {
			ReleaseSRWLockExclusive(&g_itemsLock);
			return false;
		}
		if (item->type == Agent) {
			ReleaseSRWLockExclusive(&g_itemsLock);
			return true;
		}
		if (item->skinsReady && !item->skins.empty()) {
			ReleaseSRWLockExclusive(&g_itemsLock);
			return true;
		}
		if (item->skinsReady && item->skins.empty()) {
			item->skinsReady = false;
			kitCache.erase(item->simple);
		}
		if (!item->simple.empty()) {
			auto cacheIt = kitCache.find(item->simple);
			if (cacheIt != kitCache.end() && !cacheIt->second.empty()) {
				item->skins = cacheIt->second;
				item->skinsReady = true;
				ReleaseSRWLockExclusive(&g_itemsLock);
				return true;
			}
		}
		simple = item->simple;
		ReleaseSRWLockExclusive(&g_itemsLock);
	}

	if (simple.empty())
		return false;

	// SLOW: FileExistsGame probes per paint kit. Must stay OUTSIDE the lock
	// or the game-thread readers stall the FSN frame.
	std::vector<Skin> kits = BuildKits(simple);

	// Section 2: commit.
	AcquireSRWLockExclusive(&g_itemsLock);
	if (!kits.empty())
		kitCache[simple] = kits;
	else
		kitCache.erase(simple);
	for (auto& it : items) {
		if (it.simple == simple || it.def == def) {
			it.skins = kits;
			it.skinsReady = !kits.empty();
			if (const char* tok = SimpleName(it.def, nullptr))
				it.simple = tok;
		}
	}
	ReleaseSRWLockExclusive(&g_itemsLock);

	if (!kits.empty())
		SaveDisk();
	return true;
}

bool SkinItems::IsLegacySkin(uint16_t def, int paintId)
{
	if (paintId <= 0)
		return false;

	// Direct engine truth: query CPaintKit from EconSchema (works immediately without waiting for menu scans)
	if (CPaintKit* k = SkinSdk::FindPaintKit(paintId)) {
		return k->IsUseLegacyModel() != 0;
	}

	AcquireSRWLockShared(&g_itemsLock);
	bool legacy = false;
	for (const auto& it : items) {
		if (it.def != def)
			continue;
		for (const auto& s : it.skins) {
			if (s.id == paintId) {
				legacy = s.legacy;
				break;
			}
		}
		break;
	}
	ReleaseSRWLockShared(&g_itemsLock);
	return legacy;
}

bool SkinItems::FirstAgentModel(int team, char* out, size_t outN)
{
	if (!out || outN == 0)
		return false;
	out[0] = '\0';
	AcquireSRWLockShared(&g_itemsLock);
	for (const auto& it : items) {
		if (it.type == Agent && it.team == team && !it.icon.empty()) {
			snprintf(out, outN, "%s", it.icon.c_str());
			ReleaseSRWLockShared(&g_itemsLock);
			return true;
		}
	}
	ReleaseSRWLockShared(&g_itemsLock);
	return false;
}

SkinItems& GetSkinItems() { return g_items; }


