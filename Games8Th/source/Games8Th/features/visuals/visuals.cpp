#include "visuals.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include "../../hooks/hooks.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../../../external/imgui/imgui.h"
#include "../../interfaces/interfaces.h"
#include "../../config/config.h"
#include "../../menu/menu.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../glow/glow.h"
#include "../autowall/autowall.h"
#include "../aim/aim_common.h"
#include "../gamemode/gamemode.h"
#include "../backtrack/backtrack.h"
#include "../grenade_helper/grenade_helper.h"
#include "../nadepred/nadepred.h"
#include "../world/weather.h"
#include "../hitmarker/hitmarker.h"
#include "../bullet_impact/bullet_impact.h"
#include "../hitlog/hitlog.h"
#include "../w2s/w2s.h"
#include "../bomb/bomb.h"
#include "../widgets/steam_avatar.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../../offsets/offsets.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "assets/weapon_icons.hpp"
#include "weapon_icon_draw.h"

extern ID3D11Device* pDevice;

using namespace Esp;

LocalPlayerCached cached_local;
// Double-buffer players: Present fills write buffer, atomic-publishes for DrawGlow/CM.
// Fixed-size arrays (NOT vector): vector copy-assign on publish can realloc the
// buffer another thread is still iterating (EspPlayersSnapshot / EspLookupVisible)
// -> UAF. POD slots never realloc; worst case is a one-frame-stale slot.
static PlayerCache s_playersBuf[2][Mem::kMaxPlayers]{};
static std::atomic<int> s_playersCount[2]{ 0, 0 };
static std::atomic<int> s_playersPub{ 0 }; // index of published buffer
std::vector<PlayerCache> cached_players; // alias of published for ESP draw (same-thread Present)
std::vector<WorldCache> cached_world;
PlantedBombInfo g_plantedBomb;
struct VisSticky {
	std::uint32_t h = 0;
	bool vis = true;
	float x = 0.f, y = 0.f, z = 0.f;
	bool ok = false;
};
static VisSticky s_visSticky[64]{};
static int s_visStickyN = 0;
// Guards the publish no-op (see EspPublishPlayersFromCache). File-scope so
// EspClearPlayersPublished can invalidate - a clear rewrites the published
// buffer to empty, so the next non-clear publish must NOT be skipped even
// if the freshly built list happens to hash the same as the pre-clear list.
static std::uint64_t s_lastPublishHash = 0;

int EspPlayersSnapshot(PlayerCache* out, int maxOut) {
	if (!out || maxOut <= 0)
		return 0;
	const int pub = s_playersPub.load(std::memory_order_acquire);
	const int n = s_playersCount[pub].load(std::memory_order_acquire);
	const int c = (n < maxOut) ? n : maxOut;
	for (int i = 0; i < c; ++i)
		out[i] = s_playersBuf[pub][static_cast<size_t>(i)];
	return c;
}

bool EspTryLookupVisible(std::uint32_t handleRaw, bool& outVisible) {
	outVisible = true;
	const int pub = s_playersPub.load(std::memory_order_acquire);
	const int n = s_playersCount[pub].load(std::memory_order_acquire);
	for (int i = 0; i < n; ++i) {
		const PlayerCache& p = s_playersBuf[pub][i];
		if (p.handle.valid() && p.handle.raw() == handleRaw) {
			outVisible = p.visible;
			return true;
		}
	}
	return false;
}

bool EspLookupVisible(std::uint32_t handleRaw) {
	bool vis = true;
	if (EspTryLookupVisible(handleRaw, vis))
		return vis;
	return true; // fail-open
}

// No SEH in callers with locals - copy from globals only (C2712 safe).
// Skip the vector copy when the published state is identical to what we just
// built. Cheap FNV1a over (handle, visibility, health) covers the fields other
// threads read (Glow). Missed changes are impossible because those
// fields are always written every frame - the hash sees any difference.
static std::uint64_t EspHashPlayersForPublish() {
	std::uint64_t h = 14695981039346656037ull;
	auto mix = [&](std::uint64_t v) {
		h ^= v;
		h *= 1099511628211ull;
	};
	mix(static_cast<std::uint64_t>(cached_players.size()));
	for (const auto& p : cached_players) {
		mix(static_cast<std::uint64_t>(p.handle.raw()));
		mix(p.visible ? 1ull : 0ull);
		mix(static_cast<std::uint64_t>(p.health & 0xFF));
	}
	return h;
}

static void EspPublishPlayersFromCache() {
	const std::uint64_t hash = EspHashPlayersForPublish();
	if (hash == s_lastPublishHash)
		return; // published buffer already matches - skip the copy
	s_lastPublishHash = hash;

	const int cur = s_playersPub.load(std::memory_order_relaxed);
	const int write = (cur ^ 1) & 1;
	const int c = static_cast<int>(cached_players.size());
	const int n = (c < Mem::kMaxPlayers) ? c : Mem::kMaxPlayers;
	for (int i = 0; i < n; ++i)
		s_playersBuf[write][static_cast<size_t>(i)] = cached_players[static_cast<size_t>(i)];
	// Publish count BEFORE index: reader acquires count, then slots.
	s_playersCount[write].store(n, std::memory_order_release);
	s_playersPub.store(write, std::memory_order_release);
}

static void EspClearPlayersPublished() {
	const int cur = s_playersPub.load(std::memory_order_relaxed);
	const int write = (cur ^ 1) & 1;
	s_playersCount[write].store(0, std::memory_order_release);
	s_playersPub.store(write, std::memory_order_release);
	// Invalidate the publish hash - otherwise a subsequent build that
	// coincidentally hashes to the pre-clear value would skip publishing
	// and leave readers looking at an empty buffer.
	s_lastPublishHash = 0;
}

static Vector_t GetAbsOrigin(C_CSPlayerPawn* pawn) {
	if (!pawn)
		return {};
	Vector_t out{};
	__try {
		CGameSceneNode* node = pawn->m_pGameSceneNode();
		if (node)
			out = node->m_vecAbsOrigin();
		else
			out = pawn->m_vOldOrigin();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return {};
	}
	return out;
}

static bool GetLocalOrigin(Vector_t& out) {
	out = Vector_t{ 0.f, 0.f, 0.f };
	if (cached_local.active && Bones::IsValidPos(cached_local.position)) {
		out = cached_local.position;
		return true;
	}
	if (C_CSPlayerPawn* live = H::SafeLocalAlive()) {
		out = GetAbsOrigin(live);
		if (Bones::IsValidPos(out))
			return true;
	}
	if (C_CSPlayerPawn* any = H::SafeLocalPlayer()) {
		out = GetAbsOrigin(any);
		if (Bones::IsValidPos(out))
			return true;
	}
	return false;
}

static bool IsDormant(C_CSPlayerPawn* pawn) {
	if (!pawn)
		return true;
	bool d = true;
	__try {
		CGameSceneNode* node = pawn->m_pGameSceneNode();
		d = node && node->m_bDormant();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return true;
	}
	return d;
}

static bool ResolvePawn(const CBaseHandle& h, C_CSPlayerPawn** out) {
	*out = nullptr;
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return false;

	// TDM death/respawn recycles the handle slot. Get() on a stale
	// serial can AV inside the entity system - wrap the whole resolve.
	C_CSPlayerPawn* pawn = nullptr;
	CBaseHandle actual{};
	__try {
		pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(h);
		if (!pawn)
			return false;
		actual = pawn->handle();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!actual.valid()
		|| actual.index() != h.index()
		|| actual.serial_number() != h.serial_number())
		return false;

	*out = pawn;
	return true;
}

static const char* StripWeaponPrefix(const char* nm) {
	if (!nm || !nm[0])
		return nm;
	const char* p = nm;
	if (strncmp(p, "weapon_", 7) == 0)
		p += 7;
	else if (strncmp(p, "C_Weapon", 8) == 0)
		p += 8;
	else if (strncmp(p, "CWeapon", 7) == 0)
		p += 7;
	else if (p[0] == 'C' && p[1] == '_')
		p += 2;
	else if (p[0] == 'C' && p[1] >= 'A' && p[1] <= 'Z')
		p += 1;
	return p;
}

static void CopyCleanWeaponName(const char* nm, char* buf, size_t n) {
	if (!buf || n == 0)
		return;
	buf[0] = '\0';
	if (!nm || !nm[0])
		return;

	// "weapon_ak47" / "C_WeaponAWP" / "CAK47" -> short label
	const char* p = StripWeaponPrefix(nm);
	if (!p)
		return;

	size_t i = 0;
	for (; i + 1 < n && p[i]; ++i) {
		char c = p[i];
		if (c == '_') c = ' ';
		buf[i] = c;
	}
	buf[i] = '\0';
	// IDA: HUD/world ESP showed ",s " / "'s " <model> after skin apply due to stale econ description at view+0x200; trim artifact
	{
		char* s = buf;
		while (*s == ',' || *s == '\'' || *s == '"' || *s == '`' || *s == ' ' || *s == '\t' || *s == '\xE2' || *s == '\x98' || *s == '\x85') ++s;
		if (s != buf) { size_t len = strlen(s); memmove(buf, s, len + 1); }
		// also handle "'s " prefix seen on Bowie Doppler world ESP
		if ((buf[0] == '\'' && buf[1] == 's' && (buf[2] == ' ' || buf[2] == ',')) || ((buf[0] == 's' || buf[0] == 'S') && (buf[1] == ',' || buf[1] == '\'' || buf[1] == ' '))) {
			s = buf + 1; while (*s == ',' || *s == ' ' || *s == '\t') ++s;
			size_t len = strlen(s); memmove(buf, s, len + 1);
		}
		if (!strncmp(buf, ",s ", 3) || !strncmp(buf, ",s,", 3) || !strncmp(buf, "'s ", 3) || !strncmp(buf, "'s,", 3) || !strncmp(buf, "s' ", 3)) {
			size_t len = strlen(buf + 3); memmove(buf, buf + 3, len + 1);
			while (buf[0] == ' ' || buf[0] == ',') { memmove(buf, buf + 1, strlen(buf)); }
		}
	}
}

// Icon lookup key: lowercase, underscores (ak47 / m4a1_silencer / knife_karambit)
static void MakeWeaponIconKey(const char* nm, char* buf, size_t n) {
	if (!buf || n == 0)
		return;
	buf[0] = '\0';
	if (!nm || !nm[0])
		return;
	// IDA: trim leading ",s" / "'s" HUD artifact before icon lookup
	while (nm[0] == ',' || nm[0] == '\'' || nm[0] == '"' || nm[0] == ' ' || nm[0] == '\t') ++nm;
	if ((nm[0] == 's' || nm[0] == 'S') && (nm[1] == ',' || nm[1] == '\'' || nm[1] == ' ')) { ++nm; while (nm[0] == ',' || nm[0] == '\'' || nm[0] == ' ' || nm[0] == '\t') ++nm; }
	if (nm[0] == '\'' && nm[1] == 's' && (nm[2] == ' ' || nm[2] == ',')) { nm += 2; while (nm[0] == ',' || nm[0] == '\'' || nm[0] == ' ' || nm[0] == '\t') ++nm; }
	if (!strncmp(nm, ",s ", 3) || !strncmp(nm, "'s ", 3) || !strncmp(nm, "s' ", 3)) nm += 3;
	const char* p = StripWeaponPrefix(nm);
	if (!p)
		return;

	size_t i = 0;
	for (; i + 1 < n && p[i]; ++i) {
		unsigned char c = static_cast<unsigned char>(p[i]);
		if (c == ' ' || c == '-')
			c = '_';
		else
			c = static_cast<unsigned char>(std::tolower(c));
		buf[i] = static_cast<char>(c);
	}
	buf[i] = '\0';
}

static const char* ResolveWeaponIconGlyph(const char* key) {
	if (!key || !key[0])
		return nullptr;
	auto it = weapon_icons::icon_table.find(key);
	if (it != weapon_icons::icon_table.end() && !it->second.empty())
		return it->second.c_str();

	// knife_* fallback
	if (strncmp(key, "knife", 5) == 0) {
		it = weapon_icons::icon_table.find("knife");
		if (it != weapon_icons::icon_table.end() && !it->second.empty())
			return it->second.c_str();
	}
	return nullptr;
}

// Centered weapon icon - classic undefeated font first, atlas fallback.
static float DrawWeaponIconCentered(ImDrawList* dl, float cx, float y, ImU32 col, const char* key, const char* cachedGlyph = nullptr) {
	if (!dl || (!key && !cachedGlyph))
		return 0.f;

	// Undefeated ASCII glyph - primary design
	const char* glyph = cachedGlyph ? cachedGlyph : ResolveWeaponIconGlyph(key);
	if (glyph && glyph[0] && g_WeaponIconFont) {
		const float textSz = ImGui::GetFontSize();
		const bool isKnife = key ? (strncmp(key, "knife", 5) == 0 || strstr(key, "bayonet") != nullptr
			|| strstr(key, "karambit") != nullptr) : false;
		// Balance with ESP text - 1.15x rendered wider than name/distance lines
		float iconSz = isKnife ? (textSz * 0.85f) : (textSz * 0.95f);

		ImVec2 sz = g_WeaponIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, glyph);
		if (sz.x <= 1.f)
			return 0.f;

		const float x = floorf(cx - sz.x * 0.5f);
		const float drawY = floorf(y);

		const ImU32 shadow = IM_COL32(0, 0, 0, 200);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x - 1.f, drawY), shadow, glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x + 1.f, drawY), shadow, glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x, drawY - 1.f), shadow, glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x, drawY + 1.f), shadow, glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x, drawY), col, glyph);

		return floorf(sz.y + 2.f);
	}

	// Atlas fallback
	if (key && key[0]) {
		WeaponIconDraw::EnsureReady(pDevice);
		const float textSz = ImGui::GetFontSize();
		const float atlasH = (std::max)(14.f, textSz * 0.95f);
		if (WeaponIconDraw::Has(key)) {
			const float adv = WeaponIconDraw::DrawCentered(dl, cx, y, col, key, atlasH);
			if (adv > 0.f)
				return adv;
		}
	}
	return 0.f;
}

static void ReadWeaponEntityName(C_CSWeaponBase* wep, char* label, size_t labelN, char* key, size_t keyN, int* clip = nullptr, int* maxClip = nullptr) {
	if (label && labelN)
		label[0] = '\0';
	if (key && keyN)
		key[0] = '\0';
	if (clip)
		*clip = -1;
	if (maxClip)
		*maxClip = -1;
	if (!wep)
		return;
	__try {
		if (clip) {
			const int c = wep->m_iClip1();
			*clip = (c >= 0 && c <= 250) ? c : -1;
		}
		const char* raw = nullptr;
		char clsBuf[128]{};
		char vdataName[128]{};
		if (H::oGetWeaponData > 0) {
			CCSWeaponBaseVData* data = wep->Data();
			if (data) {
				if (maxClip) {
					const int mc = data->m_iMaxClip1();
					*maxClip = (mc > 0 && mc <= 250) ? mc : -1;
				}
				const char* nm = data->m_szName();
				if (nm && Mem::PeekCString(nm, vdataName, sizeof(vdataName)) && vdataName[0])
					raw = vdataName;
			}
		}
		if (!raw) {
			if (Mem::SchemaClassName(wep, clsBuf, sizeof(clsBuf)))
				raw = clsBuf;
		}
		if (!raw || !raw[0])
			return;
		if (label && labelN)
			CopyCleanWeaponName(raw, label, labelN);
		if (key && keyN)
			MakeWeaponIconKey(raw, key, keyN);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.readWeaponEntityName");
		if (label && labelN)
			label[0] = '\0';
		if (key && keyN)
			key[0] = '\0';
	}
}

static void ReadWeaponName(C_CSPlayerPawn* pawn, char* label, size_t labelN, char* key, size_t keyN, int* clip = nullptr, int* maxClip = nullptr) {
	if (label && labelN)
		label[0] = '\0';
	if (key && keyN)
		key[0] = '\0';
	if (clip)
		*clip = -1;
	if (maxClip)
		*maxClip = -1;
	if (!pawn)
		return;
	__try {
		ReadWeaponEntityName(pawn->GetActiveWeapon(), label, labelN, key, keyN, clip, maxClip);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.readWeaponName");
		if (label && labelN)
			label[0] = '\0';
		if (key && keyN)
			key[0] = '\0';
	}
}

static bool AnyWorldEspEnabled() {
	return Config::widget_bomb
		|| Config::world_esp_weapons || Config::world_esp_bomb
		|| Config::world_esp_smoke || Config::world_esp_molotov
		|| Config::world_esp_he || Config::world_esp_flash
		|| Config::world_esp_decoy
		|| Config::glow_world_weapons
		|| Config::glow_world_grenades;
}

bool Esp::NeedWorldCache() {
	return AnyWorldEspEnabled();
}

// Player controller pass - skip when nothing consumes cached_players / local
static bool AnyPlayerCacheNeeded() {
	return Config::esp || Config::espFill || Config::showHealth || Config::showArmor
		|| Config::showNameTags || Config::esp_skeleton || Config::showWeapon
		|| Config::showWeaponIcon || Config::showDistance
		|| Config::flag_flashed || Config::flag_scoped
		|| Config::flag_defusing || Config::flag_bomb || Config::flag_reloading
		|| Config::flag_money || Config::flag_kit || Config::flag_helmet || Config::flag_nades
		|| Config::esp_rank || Config::esp_3d_box || Config::esp_oof
		|| Config::widget_radar
		|| Config::widget_bomb
		// glow_only_visible needs cache vis; plain glow does not
		|| (Config::glow && Config::glow_only_visible)
		|| Config::world_esp_weapon_distance;
}

bool Esp::NeedPlayerCache() {
	// Backtrack records from the published player list - keep it fresh.
	return AnyPlayerCacheNeeded() || Config::backtrack;
}

// Heavy per-pawn fills - only when UI actually draws that field.
// Old path always ReadWeapon + flags + money + rank + nades -> multi-queue insecure.
static bool WantPlayerWeaponInfo() {
	return Config::showWeapon || Config::showWeaponIcon || Config::flag_bomb;
}
static bool WantPlayerFlags() {
	return Config::flag_flashed || Config::flag_scoped || Config::flag_defusing
		|| Config::flag_bomb || Config::flag_reloading
		|| Config::flag_kit || Config::flag_helmet;
}
static bool WantPlayerEquip() {
	return Config::flag_money || Config::esp_rank;
}
static bool WantPlayerNades() {
	return Config::flag_nades;
}
static bool WantPlayerNames() {
	return Config::showNameTags || Config::esp_name_avatar || Config::widget_spectators;
}
static bool WantPlayerVis() {
	// Traces from Present are the other big insecure surface - only when color needs it.
	return Config::esp_vis_check
		|| (Config::glow && Config::glow_only_visible);
}

// CS2 competitive ranks (0 = unranked). Display short labels for ESP.
static const char* CompetitiveRankName(int rank) {
	static const char* kNames[] = {
		"Unranked",
		"S1", "S2", "S3", "S4", "SE", "SEM",
		"GN1", "GN2", "GN3", "GNM",
		"MG1", "MG2", "MGE", "DMG",
		"LE", "LEM", "SMFC", "GE"
	};
	if (rank < 0 || rank >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0])))
		return nullptr;
	return kNames[rank];
}

// Nade def indices (CS2 item defs)
static bool IsNadeDef(std::uint16_t def, bool& he, bool& flash, bool& smoke, bool& molly, bool& decoy) {
	// 43 HE, 44 flash, 45 smoke, 46 molly, 47 decoy, 48 incendiary
	switch (def) {
	case 43: he = true; return true;
	case 44: flash = true; return true;
	case 45: smoke = true; return true;
	case 46: molly = true; return true;
	case 47: decoy = true; return true;
	case 48: molly = true; return true;
	default: return false;
	}
}

static void FillEquipFromController(CCSPlayerController* ctrl, PlayerCache& entry) {
	entry.money = -1;
	entry.rank = 0;
	if (!ctrl)
		return;
	__try {
		entry.rank = ctrl->m_iCompetitiveRanking();
		void* moneySvc = ctrl->m_pInGameMoneyServices();
		if (moneySvc && Mem::IsUserPtr(moneySvc)) {
			auto* ms = reinterpret_cast<CCSPlayerController_InGameMoneyServices*>(moneySvc);
			const int acc = ms->m_iAccount();
			if (acc >= 0 && acc <= 16000)
				entry.money = acc;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.fillEquip");
		entry.money = -1;
		entry.rank = 0;
	}
}

// Walk m_hMyWeapons for nade inventory.
static void FillNadeInventory(C_CSPlayerPawn* pawn, PlayerCache& entry) {
	entry.nade_he = entry.nade_flash = entry.nade_smoke = entry.nade_molly = entry.nade_decoy = false;
	if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
		return;
	CCSPlayer_WeaponServices* ws = pawn->GetWeaponServices();
	if (!ws || !Mem::IsUserPtr(ws))
		return;

	static std::uint32_t s_myWeapons = 0;
	if (!s_myWeapons)
		s_myWeapons = SchemaFinder::Get(hash_32_fnv1a_const("CPlayer_WeaponServices->m_hMyWeapons"));
	if (!s_myWeapons)
		return;

	__try {
		auto* base = reinterpret_cast<std::uint8_t*>(ws) + s_myWeapons;
		struct Try { CBaseHandle* elems; int sz; };
		const Try tries[2] = {
			{ *reinterpret_cast<CBaseHandle**>(base + 8), *reinterpret_cast<int*>(base + 0) },
			{ *reinterpret_cast<CBaseHandle**>(base + 0), *reinterpret_cast<int*>(base + 8) },
		};
		for (const auto& t : tries) {
			if (t.sz <= 0 || t.sz > 64 || !t.elems || !Mem::IsUserPtr(t.elems))
				continue;
			for (int i = 0; i < t.sz; ++i) {
				if (!t.elems[i].valid())
					continue;
				auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(t.elems[i]);
				if (!w)
					continue;
				const std::uint16_t def = w->m_iItemDefinitionIndex();
				IsNadeDef(def, entry.nade_he, entry.nade_flash, entry.nade_smoke,
					entry.nade_molly, entry.nade_decoy);
			}
			break;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.fillNades");
	}
}

static Vector_t GetEntityAbsOrigin(C_BaseEntity* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return {};
	// Schema-first (Offset::) with dump FB - no fixed 0x330/0xC8
	const uint32_t kSceneNode = Offset::m_pGameSceneNode();
	const uint32_t kAbsOrigin = Offset::m_vecAbsOrigin();
	CGameSceneNode* node = nullptr;
	if (!kSceneNode || !kAbsOrigin
		|| !Mem::ReadField(ent, kSceneNode, node)
		|| !node || !Mem::Valid(node, kAbsOrigin + 12)) {
		__try { node = ent->m_pGameSceneNode(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
		if (!node || !Mem::Valid(node, (kAbsOrigin ? kAbsOrigin : 0xC8) + 12))
			return {};
	}
	Vector_t o{};
	const uint32_t absOff = kAbsOrigin ? kAbsOrigin : 0xC8;
	if (Mem::ReadField(node, absOff, o)
		&& std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z))
		return o;
	__try { return node->m_vecAbsOrigin(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}

static bool IsEntityDormant(C_BaseEntity* ent) {
	if (!ent)
		return true;
	__try {
		CGameSceneNode* node = ent->m_pGameSceneNode();
		return node && node->m_bDormant();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// Effect timers: full handle XOR throw-origin sig. Slot reuse + lingering dead
// HE shells caused every-other-round expired timers for warn + world ESP.
struct WorldFxSlot {
	uint32_t key = 0;
	int kind = -1;
	float start = 0.f;
	bool seen = false;
	bool expired = false;
	uint32_t throwSig = 0;
};
static WorldFxSlot s_worldFx[64];
static int s_worldFxN = 0;

static uint32_t WorldHandleKey(CEntityInstance* ent, int fallbackIdx) {
	if (ent && Mem::ValidEntity(ent)) {
		CEntityIdentity* id = nullptr;
		if (Mem::ReadField(ent, Offset::m_pEntity(), id) && id && Mem::Valid(id, 0x14)) {
			uint32_t raw = 0;
			if (Mem::ReadField(id, 0x10, raw) && (raw & 0x7FFF) != 0)
				return raw;
		}
		const CBaseHandle h = ent->handle();
		if (h.valid())
			return static_cast<uint32_t>(h.index())
				| (static_cast<uint32_t>(h.serial_number()) << 15);
	}
	return static_cast<uint32_t>(fallbackIdx);
}

static uint32_t WorldThrowSig(const Vector_t& p) {
	const int x = static_cast<int>(p.x * 0.5f);
	const int y = static_cast<int>(p.y * 0.5f);
	const int z = static_cast<int>(p.z * 0.5f);
	uint32_t h = 2166136261u;
	h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
	h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
	h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
	return h ? h : 1u;
}

// Handle + round epoch + kind + throw origin. Live origin was mixed in before ->
// key churn while airborne. Kind isolates smoke/molly/HE on recycled handles.
static uint32_t s_worldRoundEpoch = 1;
static uint32_t WorldFxKey(CEntityInstance* ent, int fallbackIdx, int nadeKind, uint32_t throwSig = 0) {
	return WorldHandleKey(ent, fallbackIdx)
		^ (s_worldRoundEpoch * 0x85EBCA6Bu)
		^ (static_cast<uint32_t>(nadeKind + 1) * 0xC2B2AE3Du)
		^ (throwSig * 0x9E3779B9u);
}

static void WorldFxBeginFrame() {
	for (int j = 0; j < s_worldFxN; ++j)
		s_worldFx[j].seen = false;
}

static void WorldFxEndFrame() {
	for (int j = 0; j < s_worldFxN; ) {
		if (!s_worldFx[j].seen) {
			s_worldFx[j] = s_worldFx[s_worldFxN - 1];
			--s_worldFxN;
		} else {
			++j;
		}
	}
}

// Wall-clock fallback only - live clear uses explode/fire flags when available.
static float WorldFxLimit(int nadeKind, bool effectActive) {
	if (nadeKind == WORLD_SMOKE) return 18.f;
	// Fire: 7s hard cap; live clear uses lit flags / postFx. Projectile: air only.
	if (nadeKind == WORLD_MOLOTOV) return effectActive ? 7.f : 2.5f;
	if (nadeKind == WORLD_DECOY) return 15.f;
	if (nadeKind == WORLD_HE || nadeKind == WORLD_FLASH) return 1.6f; // match nade_pred kDurHeFlashFuse
	return 0.f;
}

// Wall clock for FX timers - must NOT use ImGui::GetTime (freezes when Present skips ImGui)
static float WallTimeSec() {
	return static_cast<float>(GetTickCount64()) * 0.001f;
}

// Remaining effect time; starts track on first see. <0 = no timer.
// kindTag encodes effectActive so fire vs projectile don't share clocks.
static float WorldEffectRemaining(uint32_t handleKey, int nadeKind, bool effectActive = false,
	uint32_t throwSig = 0) {
	const float limit = WorldFxLimit(nadeKind, effectActive);
	if (limit <= 0.f || handleKey == 0)
		return -1.f;

	// Separate slots for fire vs projectile of same handle
	const int kindTag = nadeKind + (effectActive ? 100 : 0);
	const float now = WallTimeSec();
	for (int j = 0; j < s_worldFxN; ++j) {
		if (s_worldFx[j].key != handleKey)
			continue;

		const bool newThrow = (throwSig != 0 && s_worldFx[j].throwSig != 0
			&& s_worldFx[j].throwSig != throwSig);
		if (s_worldFx[j].kind != kindTag || newThrow) {
			s_worldFx[j].kind = kindTag;
			s_worldFx[j].start = now;
			s_worldFx[j].expired = false;
			s_worldFx[j].throwSig = throwSig;
			s_worldFx[j].seen = true;
			return limit;
		}

		if (s_worldFx[j].expired) {
			s_worldFx[j].seen = true;
			return 0.f;
		}

		if (throwSig != 0)
			s_worldFx[j].throwSig = throwSig;

		const float age = now - s_worldFx[j].start;
		// Pause/hitch only - do NOT re-anchor old tracks (round-2 "too fast"/miss)
		if (age < -0.05f) {
			s_worldFx[j].start = now;
			s_worldFx[j].seen = true;
			return limit;
		}
		if (age >= limit) {
			s_worldFx[j].expired = true;
			s_worldFx[j].seen = true;
			return 0.f;
		}
		s_worldFx[j].seen = true;
		return limit - age;
	}
	if (s_worldFxN < 64) {
		s_worldFx[s_worldFxN] = WorldFxSlot{ handleKey, kindTag, now, true, false, throwSig };
		++s_worldFxN;
	}
	return limit;
}

static float ReadFloatSafe(const float* p) {
	float t = 0.f;
	if (!p)
		return 0.f;
	__try { t = *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.readFloatSafe"); t = 0.f; }
	return t;
}

// SEH isolated - no C++ objects with dtors
static void* SafeReadVoidPtr(uintptr_t addr) {
	void* v = nullptr;
	__try { v = *reinterpret_cast<void**>(addr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.safeReadPtr"); return nullptr; }
	return v;
}

// CS2 GlobalVars curtime - IDA: sub that does
// mov rax, [rip+gv]; movss xmm0, [rax+30h]; ret
// (old pattern hit a different user of same gv that reads +0x44 as int).
static float GetCurTime() {
	static uintptr_t s_gvAbs = 0;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		// Preferred: dedicated curtime getter
		uintptr_t insn = M::patternScan("client", "48 8B 05 ? ? ? ? F3 0F 10 40 30 C3");
		if (!insn)
			insn = M::patternScan("client", "48 8B 05 ? ? ? ? 0F 57 C0 8B 48");
		if (insn)
			s_gvAbs = M::getAbsoluteAddress(insn, 3, 0);
	}
	if (!s_gvAbs)
		return 0.f;
	void* gv = SafeReadVoidPtr(s_gvAbs);
	if (!gv)
		return 0.f;
	const uintptr_t base = reinterpret_cast<uintptr_t>(gv);
	// IDA-confirmed curtime @ +0x30; keep fallbacks if build shifts
	const int offs[5] = { 0x30, 0x2C, 0x34, 0x38, 0x24 };
	for (int i = 0; i < 5; ++i) {
		const float t = ReadFloatSafe(reinterpret_cast<float*>(base + offs[i]));
		if (t > 1.f && t < 1.0e7f && std::isfinite(t))
			return t;
	}
	return 0.f;
}

// Wall-clock bomb fuse - immune to bad curtime after round transitions
static uint32_t s_bombTrackKey = 0;
static float s_bombEndWall = 0.f;
static uint32_t s_bombFrozenKey = 0;
static float s_bombFrozenLeft = -1.f;

static void ResetBombWallClock() {
	s_bombTrackKey = 0;
	s_bombEndWall = 0.f;
	s_bombFrozenKey = 0;
	s_bombFrozenLeft = -1.f;
}

static float FreezeBombTimer(uint32_t key, float liveLeft) {
	if (s_bombFrozenKey == key && s_bombFrozenLeft >= 0.f)
		return s_bombFrozenLeft;
	float snap = liveLeft;
	if (!(snap >= 0.f && snap <= 45.f) && s_bombEndWall > 0.f && key == s_bombTrackKey)
		snap = (std::max)(0.f, s_bombEndWall - WallTimeSec());
	if (snap >= 0.f && snap <= 45.f) {
		s_bombFrozenKey = key;
		s_bombFrozenLeft = snap;
		return snap;
	}
	return liveLeft;
}

// Knife-like class/designer tag (C_Knife / knife_* / bayonet / karambit). Used
// to skip knife-classed world entities from ground-weapon ESP in TDM/FFA, where
// a dead pawn's holstered knife must NOT render as a dropped weapon icon.
static bool KnifeLookalikeTag(const char* n) {
	if (!n || !n[0])
		return false;
	if (strncmp(n, "knife", 5) == 0 || strstr(n, "bayonet") != nullptr
		|| strstr(n, "karambit") != nullptr || strstr(n, "butterfly") != nullptr)
		return true;
	if (strstr(n, "Knife") != nullptr)
		return true;
	return false;
}

static bool ClassLooksLikeWeapon(const char* n) {
	if (!n || !n[0])
		return false;
	if (strstr(n, "Projectile") || strstr(n, "Planted") || strstr(n, "Inferno")
		|| strstr(n, "Player") || strstr(n, "Controller") || strstr(n, "Viewmodel")
		|| strstr(n, "HudModel") || strstr(n, "Wearable") || strstr(n, "Item")
		|| strstr(n, "Ragdoll") || strstr(n, "Chicken"))
		return false;
	// designer: weapon_ak47 / class: C_WeaponAK47 / C_AK47 / C_DEagle / C_Knife
	if (strncmp(n, "weapon_", 7) == 0)
		return true;
	if (strstr(n, "Weapon") || strstr(n, "DEagle") || strstr(n, "AK47") || strstr(n, "Knife")
		|| strstr(n, "M4A") || strstr(n, "SSG") || strstr(n, "AWP") || strstr(n, "Glock")
		|| strstr(n, "USP") || strstr(n, "P250") || strstr(n, "FiveSeven") || strstr(n, "Tec9")
		|| strstr(n, "Elite") || strstr(n, "Revolver") || strstr(n, "Negev") || strstr(n, "M249")
		|| strstr(n, "Nova") || strstr(n, "XM1014") || strstr(n, "MAG7") || strstr(n, "Sawed")
		|| strstr(n, "MAC10") || strstr(n, "MP5") || strstr(n, "MP7") || strstr(n, "MP9")
		|| strstr(n, "P90") || strstr(n, "Bizon") || strstr(n, "UMP") || strstr(n, "Galil")
		|| strstr(n, "Famas") || strstr(n, "SG556") || strstr(n, "AUG") || strstr(n, "SCAR")
		|| strstr(n, "G3SG") || strstr(n, "Taser") || strstr(n, "C4") || strstr(n, "Molotov")
		|| strstr(n, "Flashbang") || strstr(n, "HEGrenade") || strstr(n, "SmokeGrenade")
		|| strstr(n, "Decoy") || strstr(n, "Incendiary") || strstr(n, "BaseCSGrenade")
		|| strstr(n, "BasePlayerWeapon") || strstr(n, "CSWeaponBase"))
		return true;
	return false;
}

// IDA: C_C4 (0x181A771B4) vs C_PlantedC4 (0x181B34E68) - dropped bomb is C_C4 / weapon_c4
static bool IsWorldDroppedC4(const char* cls, const char* designer) {
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";
	if (std::strstr(a, "Planted") || std::strstr(b, "planted"))
		return false;
	if (std::strcmp(a, "C_C4") == 0 || (std::strstr(a, "C_C4") && !std::strstr(a, "Planted")))
		return true;
	if (std::strstr(b, "weapon_c4") || std::strcmp(b, "c4") == 0)
		return true;
	// class "C4" without Planted (ClassLooksLikeWeapon path)
	if (std::strstr(a, "C4") && !std::strstr(a, "Projectile"))
		return true;
	return false;
}

// owner handle: schema preferred; IDA C_BaseEntity schema = 1312 (0x520)
static bool ReadOwnerHandle(C_BaseEntity* ent, CBaseHandle* out) {
	if (!ent || !out)
		return false;
	static uint32_t s_ownerOff = 0;
	if (!s_ownerOff) {
		const uint32_t sch = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_hOwnerEntity"));
		s_ownerOff = sch ? sch : 0x520u; // IDA: 1312
	}
	__try {
		*out = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uintptr_t>(ent) + s_ownerOff);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.readOwnerHandle");
		return false;
	}
}

// Resolve owner handle with serial check (Get(index) alone can hit recycled slots)
static C_BaseEntity* ResolveOwnerEntity(const CBaseHandle& owner) {
	if (!owner.valid() || owner.index() == 0)
		return nullptr;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	auto* ent = I::GameEntity->Instance->Get(owner);
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	const CBaseHandle actual = ent->handle();
	if (!actual.valid()
		|| actual.index() != owner.index()
		|| actual.serial_number() != owner.serial_number())
		return nullptr;
	return reinterpret_cast<C_BaseEntity*>(ent);
}

// Cheap drop test for the weapons ESP path: no active-weapon vfuncs and no
// owner-origin scene reads (the expensive chains that tanked FPS & could AV).
// Owner invalid / not a living pawn = dropped - except in FFA/DM where dead-owner
// guns are just corpse clutter (weapon deleted on death) and spam ESP where no
// real drops exist. Keep only true no-owner map weapons in FFA.
static bool IsDroppedWeaponCheap(C_BaseEntity* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return false;
	__try {

	// 1) Viewmodels / HUD model attachments are never dropped world weapons
	if (ent->IsViewmodel() || ent->IsViewmodelAttachment())
		return false;

	// 2) UI shop / inventory preview models (C_CSWeaponBase->m_bUIWeapon @ 0x18EA)
	static uint32_t s_uiWepOff = 0;
	if (!s_uiWepOff) {
		s_uiWepOff = SchemaFinder::Get(hash_32_fnv1a_const("C_CSWeaponBase->m_bUIWeapon"));
		if (!s_uiWepOff) s_uiWepOff = 0x18EA;
	}
	bool isUI = false;
	if (Mem::ReadField(ent, s_uiWepOff, isUI) && isUI)
		return false;

	// 3) Attached weapon check: any weapon equipped or holstered on a player pawn has m_pParent != nullptr
	CGameSceneNode* node = ent->m_pGameSceneNode();
	if (node && Mem::Valid(node, 0x100)) {
		CGameSceneNode* parent = nullptr;
		if (Mem::ReadField(node, Offset::m_pParent(), parent) && parent && Mem::IsUserPtr(parent))
			return false; // Attached to a pawn / viewmodel hierarchy -> NOT dropped!
	}

	// 4) Living owner check
	CBaseHandle owner{};
	if (ReadOwnerHandle(ent, &owner) && owner.valid() && owner.index() != 0) {
		auto* ownerEnt = ResolveOwnerEntity(owner);
		if (ownerEnt && Mem::ValidEntity(ownerEnt)) {
			if (ownerEnt->IsBasePlayer()) {
				if (ownerEnt->m_iHealth() > 0 && ownerEnt->m_lifeState() == 0)
					return false;
			}
		}
		if (GameMode::IsFfa())
			return false;
	}

	return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// CEntityInstance::m_pEntity @+0x10, designerName @+0x20 (dump-stable)
static const char* GetDesignerName(CEntityInstance* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	__try {
		CEntityIdentity* id = nullptr;
		if (!Mem::ReadField(ent, Offset::m_pEntity(), id) || !id || !Mem::Valid(id, 0x28))
			id = ent->m_pEntityIdentity();
		if (!id || !Mem::Valid(id, 0x28))
			return nullptr;
		const char* p = nullptr;
		if (!Mem::ReadField(id, Offset::m_designerName(), p) || !p)
			p = id->m_designerName();
		if (!p || !Mem::IsReadable(p, 2) || !p[0])
			return nullptr;
		return p;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.getDesignerName");
		return nullptr;
	}
}

// Flying nade / inferno - aligned with NadePred::ClassifyProjectile (IDA class names)
static int ClassifyWorldNade(const char* cls, const char* designer) {
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";

	auto has = [](const char* s, const char* k) -> bool {
		return s && k && k[0] && std::strstr(s, k) != nullptr;
	};

	// Designer-first (cheap, no class dump required for most projectiles)
	if (b[0]) {
		if (std::strcmp(b, "inferno") == 0 || has(b, "inferno"))
			return WORLD_MOLOTOV;
		if (has(b, "smokegrenade") && !has(b, "weapon_"))
			return WORLD_SMOKE;
		if ((has(b, "molotov") || has(b, "incgrenade") || has(b, "incendiary")) && !has(b, "weapon_"))
			return WORLD_MOLOTOV;
		if (has(b, "hegrenade") && !has(b, "weapon_"))
			return WORLD_HE;
		if (has(b, "flashbang") && !has(b, "weapon_"))
			return WORLD_FLASH;
		if (has(b, "decoy") && !has(b, "weapon_"))
			return WORLD_DECOY;
		if (has(b, "projectile")) {
			if (has(b, "smoke")) return WORLD_SMOKE;
			if (has(b, "molotov") || has(b, "incendiary") || has(b, "incgrenade")) return WORLD_MOLOTOV;
			if (has(b, "hegrenade") || has(b, "he_grenade")) return WORLD_HE;
			if (has(b, "flash")) return WORLD_FLASH;
			if (has(b, "decoy")) return WORLD_DECOY;
		}
	}

	if (has(a, "Inferno") || has(a, "FireCrackerBlast"))
		return WORLD_MOLOTOV;
	if (has(a, "SmokeGrenadeProjectile") || std::strcmp(a, "C_SmokeGrenadeProjectile") == 0)
		return WORLD_SMOKE;
	if (has(a, "MolotovProjectile") || has(a, "IncendiaryGrenadeProjectile")
		|| has(a, "IncGrenadeProjectile") || std::strcmp(a, "C_MolotovProjectile") == 0)
		return WORLD_MOLOTOV;
	if (has(a, "HEGrenadeProjectile") || has(a, "FragGrenadeProjectile")
		|| std::strcmp(a, "C_HEGrenadeProjectile") == 0)
		return WORLD_HE;
	if (has(a, "FlashbangProjectile") || std::strcmp(a, "C_FlashbangProjectile") == 0)
		return WORLD_FLASH;
	if (has(a, "DecoyProjectile") || std::strcmp(a, "C_DecoyProjectile") == 0)
		return WORLD_DECOY;

	if (has(a, "Projectile") || has(a, "GrenadeProjectile")) {
		if (has(a, "Smoke")) return WORLD_SMOKE;
		if (has(a, "Molotov") || has(a, "Incendiary")) return WORLD_MOLOTOV;
		if (has(a, "HEGrenade") || has(a, "Frag")) return WORLD_HE;
		if (has(a, "Flash")) return WORLD_FLASH;
		if (has(a, "Decoy")) return WORLD_DECOY;
		if (has(a, "BaseCSGrenadeProjectile") || has(a, "GrenadeProjectile"))
			return WORLD_HE;
	}

	return -1;
}

// Cheap gate: skip dump_class_info + origin for the vast majority of entity slots.
static bool DesignerMayMatter(const char* d, bool wantPlayers, bool wantWeapons,
	bool wantNades, bool wantBomb)
{
	if (!d || !d[0])
		return true; // unknown - may need class dump
	// Controllers
	if (wantPlayers) {
		if (d[0] == 'c' && (std::strcmp(d, "cs_player_controller") == 0
			|| std::strstr(d, "controller")))
			return true;
	}
	// Bomb
	if (wantBomb) {
		if (std::strstr(d, "c4") || std::strstr(d, "planted"))
			return true;
	}
	// Projectiles / fire
	if (wantNades) {
		if (std::strstr(d, "projectile") || std::strstr(d, "inferno")
			|| std::strstr(d, "grenade") || std::strstr(d, "molotov")
			|| std::strstr(d, "flash") || std::strstr(d, "decoy")
			|| std::strstr(d, "smoke") || std::strstr(d, "incendiary")
			|| std::strstr(d, "incgrenade"))
			return true;
	}
	// Dropped guns
	if (wantWeapons && std::strncmp(d, "weapon_", 7) == 0)
		return true;
	return false;
}

struct PlantedBombState {
	bool ok = false;
	bool ticking = false;
	bool exploded = false;
	bool defused = false;
	bool defusing = false;
	int site = -1;
	float blow = 0.f;
	float defuseEnd = 0.f;
	float defuseLength = 10.f;
	float timerLength = 40.f; // IDA: C_PlantedC4->m_flTimerLength @ 0x11D8
};

static PlantedBombState ReadPlantedBomb(C_PlantedC4* bomb) {
	PlantedBombState s{};
	if (!bomb)
		return s;
	__try {
		s.ticking = bomb->m_bBombTicking();
		s.exploded = bomb->m_bHasExploded();
		s.defused = bomb->m_bBombDefused();
		s.defusing = bomb->m_bBeingDefused();
		s.site = bomb->m_nBombSite();
		s.blow = bomb->m_flC4Blow();
		s.defuseEnd = bomb->m_flDefuseCountDown();
		s.defuseLength = bomb->m_flDefuseLength();
		if (!(s.defuseLength >= 4.f && s.defuseLength <= 12.f))
			s.defuseLength = 0.f;
		s.timerLength = bomb->m_flTimerLength();
		if (!(s.timerLength >= 10.f && s.timerLength <= 60.f))
			s.timerLength = 40.f;
		s.ok = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.readPlantedBomb");
		s.ok = false;
	}
	return s;
}

// Defuse remain: game countdown first, wall seed if curtime miss (widget row
// stayed empty when GlobalVars +0x30 was stale vs m_flDefuseCountDown).
static float ResolveDefuseLeft(const PlantedBombState& bs, float curtime) {
	if (!bs.ok || !bs.defusing || bs.defused)
		return -1.f;
	if (curtime > 0.f && bs.defuseEnd > curtime) {
		const float left = bs.defuseEnd - curtime;
		if (left >= 0.f && left <= 12.f)
			return left;
	}
	static uint32_t s_defKey = 0;
	static float s_defEndWall = 0.f;
	const uint32_t key = (uint32_t)(bs.defuseEnd * 1000.f)
		^ ((uint32_t)bs.site * 0x9E3779B9u);
	const float now = WallTimeSec();
	const float len = (bs.defuseLength >= 4.f && bs.defuseLength <= 12.f)
		? bs.defuseLength : 10.f;
	if (key != s_defKey || s_defEndWall <= now) {
		s_defKey = key;
		s_defEndWall = now + len;
	}
	const float left = s_defEndWall - now;
	return (left >= 0.f && left <= 12.f) ? left : len;
}

static void FillBombTimers(WorldCache& w, const PlantedBombState& bs, float blowLeft, float defLeft) {
	w.blow_left = blowLeft;
	w.blow_full = (bs.ok && bs.timerLength >= 10.f && bs.timerLength <= 60.f) ? bs.timerLength : 40.f;
	w.defuse_left = defLeft;
	w.defuse_full = (bs.defuseLength >= 4.f && bs.defuseLength <= 12.f) ? bs.defuseLength : 10.f;
}

static void DrawTextOutlined(ImDrawList* dl, float x, float y, ImU32 col, const char* text) {
	if (!dl || !text || !text[0])
		return;
	const float fx = std::floor(x);
	const float fy = std::floor(y);
	const ImU32 shadow = IM_COL32(0, 0, 0, 220);
	// Crisp Skeet-style 1px drop shadow + clean integer alignment
	dl->AddText(ImVec2(fx + 1.f, fy + 1.f), shadow, text);
	dl->AddText(ImVec2(fx, fy), col, text);
}

// Clean box: crisp 1px borders with subtle inner/outer drop shadow (Skeet / Neverlose style)
static void DrawBoxOutlined(ImDrawList* dl, float x, float y, float w, float h, ImU32 col, float thickness, int style) {
	const float fx = std::floor(x);
	const float fy = std::floor(y);
	const float fw = std::floor(w);
	const float fh = std::floor(h);
	const float t = std::clamp(thickness, 1.f, 3.f);

	if (style == Config::ESP_BOX_CORNER) {
		const float cl = std::floor((std::min)(fw, fh) * 0.25f);
		const ImU32 black = IM_COL32(0, 0, 0, 200);
		// Shadow TL
		dl->AddLine(ImVec2(fx - 1.f, fy - 1.f), ImVec2(fx + cl + 1.f, fy - 1.f), black, t + 1.2f);
		dl->AddLine(ImVec2(fx - 1.f, fy - 1.f), ImVec2(fx - 1.f, fy + cl + 1.f), black, t + 1.2f);
		// Shadow TR
		dl->AddLine(ImVec2(fx + fw - cl - 1.f, fy - 1.f), ImVec2(fx + fw + 1.f, fy - 1.f), black, t + 1.2f);
		dl->AddLine(ImVec2(fx + fw + 1.f, fy - 1.f), ImVec2(fx + fw + 1.f, fy + cl + 1.f), black, t + 1.2f);
		// Shadow BL
		dl->AddLine(ImVec2(fx - 1.f, fy + fh - cl - 1.f), ImVec2(fx - 1.f, fy + fh + 1.f), black, t + 1.2f);
		dl->AddLine(ImVec2(fx - 1.f, fy + fh + 1.f), ImVec2(fx + cl + 1.f, fy + fh + 1.f), black, t + 1.2f);
		// Shadow BR
		dl->AddLine(ImVec2(fx + fw + 1.f, fy + fh - cl - 1.f), ImVec2(fx + fw + 1.f, fy + fh + 1.f), black, t + 1.2f);
		dl->AddLine(ImVec2(fx + fw - cl - 1.f, fy + fh + 1.f), ImVec2(fx + fw + 1.f, fy + fh + 1.f), black, t + 1.2f);

		// Main TL
		dl->AddLine(ImVec2(fx, fy), ImVec2(fx + cl, fy), col, t);
		dl->AddLine(ImVec2(fx, fy), ImVec2(fx, fy + cl), col, t);
		// Main TR
		dl->AddLine(ImVec2(fx + fw - cl, fy), ImVec2(fx + fw, fy), col, t);
		dl->AddLine(ImVec2(fx + fw, fy), ImVec2(fx + fw, fy + cl), col, t);
		// Main BL
		dl->AddLine(ImVec2(fx, fy + fh - cl), ImVec2(fx, fy + fh), col, t);
		dl->AddLine(ImVec2(fx, fy + fh), ImVec2(fx + cl, fy + fh), col, t);
		// Main BR
		dl->AddLine(ImVec2(fx + fw, fy + fh - cl), ImVec2(fx + fw, fy + fh), col, t);
		dl->AddLine(ImVec2(fx + fw - cl, fy + fh), ImVec2(fx + fw, fy + fh), col, t);
		return;
	}

	// 1) Outer dark shadow
	dl->AddRect(ImVec2(fx - 1.f, fy - 1.f), ImVec2(fx + fw + 1.f, fy + fh + 1.f), IM_COL32(0, 0, 0, 180), 0.f, 0, 1.0f);
	// 2) Main box stroke
	dl->AddRect(ImVec2(fx, fy), ImVec2(fx + fw, fy + fh), col, 0.f, 0, t);
	// 3) Inner subtle shadow for clean contrast against bright map backgrounds
	dl->AddRect(ImVec2(fx + 1.f, fy + 1.f), ImVec2(fx + fw - 1.f, fy + fh - 1.f), IM_COL32(0, 0, 0, 140), 0.f, 0, 1.0f);
}

// Slim modern side bar (HP/armor). trailRatio = old (pre-damage) value; when it
// sits above `ratio` it renders as a fading damage trail between the two edges.
// Slim modern side bar (HP/armor). Clean dark background, crisp border, smooth spring animation.
static void DrawSideBar(ImDrawList* dl, float x, float y, float w, float h, float ratio, ImU32 fill,
	float trailRatio = -1.f, ImU32 trailCol = 0) {
	ratio = std::clamp(ratio, 0.f, 1.f);
	const float fillH = h * ratio;
	const float fillY = y + (h - fillH);
	const ImU32 bg = IM_COL32(14, 16, 20, 190);
	const ImU32 shadow = IM_COL32(0, 0, 0, 160);

	// Thin soft drop shadow
	dl->AddRect(ImVec2(x - 1.f, y - 1.f), ImVec2(x + w + 1.f, y + h + 1.f), shadow, 0.f, 0, 1.f);
	// Dark background
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, 0.f);

	if (trailCol && trailRatio > ratio + 1e-3f) {
		const float tH = h * std::clamp(trailRatio, 0.f, 1.f);
		const float tY = y + (h - tH);
		dl->AddRectFilled(ImVec2(x, tY), ImVec2(x + w, fillY), trailCol, 0.f);
	}
	if (fillH > 0.5f) {
		dl->AddRectFilled(ImVec2(x, fillY), ImVec2(x + w, y + h), fill, 0.f);
	}
}

static void DrawBottomBar(ImDrawList* dl, float x, float y, float w, float h, float ratio, ImU32 fill,
	float trailRatio = -1.f, ImU32 trailCol = 0) {
	ratio = std::clamp(ratio, 0.f, 1.f);
	const float fillW = w * ratio;
	const ImU32 bg = IM_COL32(14, 16, 20, 190);
	const ImU32 shadow = IM_COL32(0, 0, 0, 160);

	dl->AddRect(ImVec2(x - 1.f, y - 1.f), ImVec2(x + w + 1.f, y + h + 1.f), shadow, 0.f, 0, 1.f);
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, 0.f);

	if (trailCol && trailRatio > ratio + 1e-3f) {
		const float tW = w * std::clamp(trailRatio, 0.f, 1.f);
		dl->AddRectFilled(ImVec2(x + fillW, y), ImVec2(x + tW, y + h), trailCol, 0.f);
	}
	if (fillW > 0.5f) {
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + fillW, y + h), fill, 0.f);
	}
}

// Per-player animated bar state (index map with full-handle generation check).
// animation::spring port - stiffness 200 / damping 20, dt in
// seconds. Damage AND heal both animate through the spring; snap only on
// first frame or a heal jump > 0.5 fraction (velocity snap rule) so a big
// restore never lags behind. trail* = pre-damage ghost value (-1 = none),
// flash* = ghost alpha.
struct BarAnim {
	std::uint32_t handle = 0;
	float curHp = -1.f;  // animated fill ratios; -1 = uninitialized (snap on first frame)
	float curArm = -1.f;
	float curAmmo = -1.f;
	float velHp = 0.f;   // spring velocity
	float velArm = 0.f;
	float velAmmo = 0.f;
	float trailHp = -1.f;
	float trailArm = -1.f;
	float trailAmmo = -1.f;
	float flashHp = 0.f; // damage-edge trail alpha 1 -> 0
	float flashArm = 0.f;
	float flashAmmo = 0.f;
};
static std::unordered_map<int, BarAnim> g_barAnim;

static void TickSpring(float& cur, float& vel, float target, float dt) {
	const float diff = target - cur;
	const float accel = diff * 200.f - vel * 20.f;
	vel += accel * dt;
	cur += vel * dt;
}

static void TickBar(BarAnim& st, float target, float& cur, float& vel, float& trail, float& flash, float dt) {
	if (cur < 0.f || target - cur > 0.5f) {
		// First frame or big heal (>50 HP in one update) - land instantly
		cur = target;
		vel = 0.f;
	} else {
		if (target < cur && trail < 0.f)
			trail = cur; // damage edge - capture ghost before the spring falls
		TickSpring(cur, vel, target, std::clamp(dt, 0.f, 0.05f));
		if (trail >= 0.f && trail > cur + 1e-4f)
			trail = (std::max)(cur, trail - (trail - cur) * std::clamp(dt * 3.5f, 0.f, 1.f));
		else
			trail = -1.f;
	}
	if (target < cur)
		flash = (std::max)(flash, 1.f);
	flash = (std::max)(0.f, flash - dt * 4.5f);
}

static void DrawCenteredText(ImDrawList* dl, float cx, float y, ImU32 col, const char* text) {
	if (!text || !text[0])
		return;
	const ImVec2 ts = ImGui::CalcTextSize(text);
	DrawTextOutlined(dl, floorf(cx - ts.x * 0.5f), floorf(y), col, text);
}

static void FillPlayerFlags(C_CSPlayerPawn* pawn, PlayerCache& entry) {
	entry.flashed = false;
	entry.bomb = false;
	entry.scoped = false;
	entry.reloading = false;
	entry.defusing = false;
	entry.has_helmet = false;
	entry.has_defuser = false;
	if (!pawn)
		return;

	__try {
		entry.scoped = pawn->m_bIsScoped();
		entry.defusing = pawn->m_bIsDefusing();

		const float flashDur = pawn->m_flFlashDuration();
		const float flashAlpha = pawn->m_flFlashOverlayAlpha();
		entry.flashed = (flashDur > 0.05f) || (flashAlpha > 20.f);

		// ItemServices: dump CCSPlayer_ItemServices m_bHasDefuser@+0x48 m_bHasHelmet@+0x49
		void* itemSvc = pawn->m_pItemServices();
		if (itemSvc && Mem::IsUserPtr(itemSvc)) {
			const auto* p = reinterpret_cast<const std::uint8_t*>(itemSvc);
			entry.has_defuser = p[0x48] != 0;
			entry.has_helmet = p[0x49] != 0;
		}

		C_CSWeaponBase* wep = pawn->GetActiveWeapon();
		if (wep) {
			entry.reloading = wep->m_bInReload();

			// Bomb: prefer already-resolved weapon name/key (avoid dump_class_info)
			bool isC4 = false;
			if (entry.weapon_key[0] && (strstr(entry.weapon_key, "c4") || strstr(entry.weapon_key, "C4")))
				isC4 = true;
			else if (entry.weapon_name[0]
				&& (strstr(entry.weapon_name, "c4") || strstr(entry.weapon_name, "C4")))
				isC4 = true;
			if (isC4) {
				entry.bomb = true;
				// Planting still counts as bomb flag
				if (wep->m_bStartedArming())
					entry.bomb = true;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("esp.fillFlags");
	}
}

bool Visuals::ensureViewMatrix() {
	W2S::Init();
	if (const viewmatrix_t* live = W2S::Matrix()) {
		viewMatrix.viewMatrix = const_cast<viewmatrix_t*>(live);
		return W2S::HasCamera();
	}
	if (viewMatrix.viewMatrix)
		return W2S::HasCamera();

	// Retry a missed fallback after a short cooldown; module initialization can lag
	// behind the first Present call.
	static viewmatrix_t* s_fallback = nullptr;
	static std::uint64_t s_nextFallbackAttemptMs = 0;
	const std::uint64_t nowMs = static_cast<std::uint64_t>(GetTickCount64());
	if (!s_fallback && nowMs >= s_nextFallbackAttemptMs) {
		s_nextFallbackAttemptMs = nowMs + 1000;
		uintptr_t site = M::patternScan("client", "48 8D 0D ? ? ? ? 48 C1 E0 06");
		if (!site)
			site = M::patternScan("client.dll", "48 8D 0D ? ? ? ? 48 C1 E0 06");
		const uintptr_t abs = site ? M::getAbsoluteAddress(site, 3, 0) : 0;
		if (abs)
			s_fallback = reinterpret_cast<viewmatrix_t*>(abs);
	}
	if (!s_fallback)
		return false;
	viewMatrix.viewMatrix = s_fallback;
	return W2S::HasCamera();
}

void Visuals::init() {
	viewMatrix.viewMatrix = nullptr;
	ensureViewMatrix();
}

void Esp::cache() {
	if (H::SessionMapLeaving() || H::SessionPostMatch() || !H::SessionEntityReady()) {
		s_visStickyN = 0;
		return;
	}
	if (!W2S::HasCamera()) {
		s_visStickyN = 0;
		return;
	}
	// GAMES8TH_ISO=1: player controllers only (no world pad walk).
	// GAMES8TH_ISO=2+: Present already skips cache().
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100)) {
		cached_players.clear();
		cached_world.clear();
		cached_local.reset();
		g_plantedBomb = {};
		EspClearPlayersPublished();
		s_visStickyN = 0;
		return;
	}

	// Keep capacity - clear+push without reserve reallocs every Present
	if (cached_players.capacity() < 64)
		cached_players.reserve(64);
	if (cached_world.capacity() < 128)
		cached_world.reserve(128);
	cached_players.clear();
	cached_world.clear();
	g_plantedBomb = {};
	// Reset per-frame local state while preserving lastTeam. If the local
	// controller disappears during respawn/round teardown, stale active/team/
	// position data otherwise leaks into the next round's ESP filtering and
	// distance labels.
	cached_local.reset();

	const int iso = H::IsolationLevel();
	// iso>=1: never pad-walk entity list (world ESP / bomb / nade scan off)
	const bool wantWorld = (iso < 1) && AnyWorldEspEnabled();
	const bool wantPlayers = (iso < 2) && AnyPlayerCacheNeeded();
	if (!wantWorld && !wantPlayers) {
		EspClearPlayersPublished();
		s_visStickyN = 0;
		return;
	}

	const float curtime = wantWorld ? GetCurTime() : 0.f;
	if (wantWorld)
		WorldFxBeginFrame();

	int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0 && !wantPlayers) {
		if (wantWorld)
			WorldFxEndFrame();
		return;
	}

	int playerCount = 0;
	bool sawBomb = false;

	const bool wantWeapons = Config::world_esp_weapons || Config::glow_world_weapons;
	const bool wantBombScan = Config::world_esp_bomb || Config::widget_bomb;
	// Combat smoke checks no longer force world walk (see AnyWorldEspEnabled).
	// Still accept smoke entities when world_esp_smoke / warn / glow nades on.
	const bool wantNades = Config::world_esp_smoke || Config::world_esp_molotov
		|| Config::world_esp_he || Config::world_esp_flash || Config::world_esp_decoy
		|| Config::glow_world_grenades;

	// Sticky vis from last frame (full handle -> last sample) - avoid re-trace every RENDER_END
	const int stickyInN = s_visStickyN;
	VisSticky stickyIn[64]{};
	const int stickyCopy = (stickyInN < 64) ? stickyInN : 64;
	for (int si = 0; si < stickyCopy; ++si)
		stickyIn[si] = s_visSticky[si];

	// -- PLAYERS: only indices from OnAddEntity controller list ----------
	// No slot walk. No GetDesignerName on props. No dump_class_info.
	// Full entity Get(1..N) every Present is what flags multi-queue insecure.
	if (wantPlayers) {
		int ctrlIdx[SdkPrioA::kMaxTrackedControllers]{};
		int nCtrl = SdkPrioA::CopyControllerIndices(ctrlIdx, SdkPrioA::kMaxTrackedControllers);

		// Cold start / inject mid-match: one-time seed if list empty (rare).
		// Cap hard - never a continuous Present walk.
		static bool s_seededOnce = false;
		static std::uint32_t s_seedMapGen = 0;
		const std::uint32_t mapGen = SdkPrioA::MapGen();
		if (mapGen != s_seedMapGen) {
			s_seedMapGen = mapGen;
			s_seededOnce = false;
		}
		if (nCtrl == 0 && !s_seededOnce && nMax > 0) {
			s_seededOnce = true;
			const int seedMax = (nMax < 72) ? nMax : 72;
			for (int i = 1; i <= seedMax && nCtrl < SdkPrioA::kMaxTrackedControllers; ++i) {
				auto* e = I::GameEntity->Instance->Get(i);
				if (!Mem::ValidEntity(e))
					continue;
				const char* d = GetDesignerName(e);
				if (!d || !d[0])
					continue;
				if (std::strcmp(d, "cs_player_controller") != 0
					&& std::strstr(d, "player_controller") == nullptr)
					continue;
				ctrlIdx[nCtrl++] = i;
			}
		}

		for (int ci = 0; ci < nCtrl; ++ci) {
			const int i = ctrlIdx[ci];
			if (i <= 0)
				continue;
			CEntityInstance* Entity = nullptr;
			CCSPlayerController* Controller = nullptr;
			CBaseHandle hPawn{};
			bool isLocal = false;
			__try {
				Entity = I::GameEntity->Instance->Get(i);
				if (!Entity || !Mem::ValidEntity(Entity) || !Entity->handle().valid())
					continue;
				const char* designer = GetDesignerName(Entity);
				if (!designer || (std::strcmp(designer, "cs_player_controller") != 0
					&& std::strstr(designer, "player_controller") == nullptr))
					continue;
				if (playerCount >= Mem::kMaxPlayers)
					break;
				Controller = reinterpret_cast<CCSPlayerController*>(Entity);
				hPawn = Controller->m_hPlayerPawn();
				if (!hPawn.valid())
					hPawn = Controller->m_hPawn();
				isLocal = Controller->IsLocalPlayer();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				continue;
			}
			if (!Controller)
				continue;
			if (!hPawn.valid())
				continue;

			// Dead local pawn is mid-recycle in TDM. Never Get() it.
			if (isLocal && !H::SafeLocalAlive()) {
				cached_local.active = true;
				cached_local.alive = false;
				cached_local.health = 0;
				cached_local.armor = 0;
				cached_local.handle = 0;
				if (C_CSPlayerPawn* dead = H::SafeLocalPlayer()) {
					int deadTeam = 0;
					__try { deadTeam = static_cast<int>(dead->m_iTeamNum()); }
					__except (EXCEPTION_EXECUTE_HANDLER) { deadTeam = 0; }
					if (deadTeam == 2 || deadTeam == 3) {
						cached_local.team = deadTeam;
						cached_local.lastTeam = deadTeam;
					} else {
						cached_local.team = cached_local.lastTeam;
					}
					cached_local.position = GetAbsOrigin(dead);
					if (!Bones::IsValidPos(cached_local.position))
						cached_local.position = Vector_t{ 0.f, 0.f, 0.f };
				} else {
					cached_local.team = cached_local.lastTeam;
				}
				continue;
			}

			C_CSPlayerPawn* Player = nullptr;
			if (!ResolvePawn(hPawn, &Player) || !Mem::ValidEntity(Player))
				continue;

			int health = 0;
			uint8_t life = 1;
			int teamNum = 0;
			__try {
				health = Mem::ClampHealth(Player->m_iHealth());
				life = Player->m_lifeState();
				// Same path as aimbot (Offset::m_iTeamNum + dump fallback).
				// schema m_iTeamNum() can stick at 0 -> ESP team filter never fires.
				teamNum = static_cast<int>(Player->getTeam());
				if (teamNum != 2 && teamNum != 3)
					teamNum = static_cast<int>(Player->m_iTeamNum());
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				continue;
			}
			if (teamNum != 2 && teamNum != 3) {
				__try {
					const uint32_t off = Offset::m_iTeamNum();
					if (off && Controller)
						teamNum = *reinterpret_cast<uint8_t*>(
							reinterpret_cast<uintptr_t>(Controller) + off);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					teamNum = 0;
				}
			}
			if (teamNum != 2 && teamNum != 3)
				continue;

			if (isLocal) {
				cached_local.active = true;
				cached_local.alive = (health > 0 && life == 0);
				int localTeam = teamNum;
				if (localTeam != 2 && localTeam != 3)
					localTeam = cached_local.lastTeam;
				if (localTeam == 2 || localTeam == 3) {
					cached_local.team = localTeam;
					cached_local.lastTeam = localTeam;
				} else {
					cached_local.team = cached_local.lastTeam;
				}
				if (cached_local.alive) {
					cached_local.position = GetAbsOrigin(Player);
					if (!Bones::IsValidPos(cached_local.position))
						cached_local.position = Vector_t{ 0.f, 0.f, 0.f };
					cached_local.health = health;
					cached_local.armor = Mem::ClampArmor(Player->m_ArmorValue());
					cached_local.handle = Player->handle().index();
				} else {
					cached_local.health = 0;
					cached_local.armor = 0;
					cached_local.handle = 0;
					cached_local.alive = false;
				}
				continue;
			}

			if (health <= 0 || life != 0)
				continue;
			if (IsDormant(Player))
				continue;

			PlayerCache entry{};
			entry.handle = Player->handle();
			entry.health = health;
			entry.maxHealth = Mem::ClampHealth(Player->m_iMaxHealth());
			if (entry.maxHealth <= 0)
				entry.maxHealth = 100;
			entry.armor = Mem::ClampArmor(Player->m_ArmorValue());
			entry.team_num = teamNum;
			entry.position = GetAbsOrigin(Player);
			if (!Bones::IsValidPos(entry.position))
				continue;
			entry.viewOffset = Player->m_vecViewOffset();
			if (!Mem::Finite(entry.viewOffset.x) || !Mem::Finite(entry.viewOffset.y)
				|| !Mem::Finite(entry.viewOffset.z))
				entry.viewOffset = Vector_t{ 0.f, 0.f, 0.f };

			const int filterTeam = (cached_local.team == 2 || cached_local.team == 3)
				? cached_local.team
				: ((cached_local.lastTeam == 2 || cached_local.lastTeam == 3) ? cached_local.lastTeam : 0);
			if (filterTeam != 0 && teamNum == filterTeam)
				entry.type = team;
			else
				entry.type = enemy;

			entry.name[0] = '\0';
			entry.steamId = 0;
			entry.weapon_name[0] = '\0';
			entry.weapon_key[0] = '\0';
			if (WantPlayerNames()) {
				if (!Controller->ReadSanitizedName(entry.name, sizeof(entry.name)))
					entry.name[0] = '\0';
				if (Config::esp_name_avatar || Config::widget_spectators) {
					__try { entry.steamId = Controller->m_steamID(); }
					__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.readSteamId"); entry.steamId = 0; }
				}
			}
			if (WantPlayerWeaponInfo()) {
				ReadWeaponName(Player, entry.weapon_name, sizeof(entry.weapon_name),
					entry.weapon_key, sizeof(entry.weapon_key), &entry.clip, &entry.maxClip);
				entry.icon_glyph = ResolveWeaponIconGlyph(entry.weapon_key);
			}
			if (WantPlayerFlags())
				FillPlayerFlags(Player, entry);
			if (WantPlayerEquip())
				FillEquipFromController(Controller, entry);
			if (WantPlayerNades())
				FillNadeInventory(Player, entry);
			entry.visible = true;
			// Seed sticky vis if handle+pos match last frame (~same spot)
			{
				const std::uint32_t hk = entry.handle.raw();
				for (int si = 0; si < stickyCopy; ++si) {
					if (!stickyIn[si].ok || stickyIn[si].h != hk)
						continue;
					const float dx = entry.position.x - stickyIn[si].x;
					const float dy = entry.position.y - stickyIn[si].y;
					const float dz = entry.position.z - stickyIn[si].z;
					if (dx * dx + dy * dy + dz * dz < 12100.f) { // ~110u - strafe-scale reuse, kills per-frame re-traces in fights
					entry.visible = stickyIn[si].vis;
						entry.visCached = true;
						entry.visSampleX = stickyIn[si].x;
						entry.visSampleY = stickyIn[si].y;
						entry.visSampleZ = stickyIn[si].z;
					}
					break;
				}
			}

			cached_players.push_back(entry);
			++playerCount;
		}

		// Local controller can miss the OnAdd list (mid-inject / spec).
		// Aim reads live pawn; ESP must too or filterTeam stays 0.
		if (cached_local.team != 2 && cached_local.team != 3) {
			if (C_CSPlayerPawn* lp = H::SafeLocalPlayer()) {
				int t = 0;
				__try { t = static_cast<int>(lp->getTeam()); }
				__except (EXCEPTION_EXECUTE_HANDLER) { t = 0; }
				if (t != 2 && t != 3) {
					__try { t = static_cast<int>(lp->m_iTeamNum()); }
					__except (EXCEPTION_EXECUTE_HANDLER) { t = 0; }
				}
				if (t == 2 || t == 3) {
					cached_local.team = t;
					cached_local.lastTeam = t;
					cached_local.active = true;
				}
			}
		}
	}

	// -- WORLD: OnAdd-tracked indices (weapons/nades/C4). No 1..Highest walk.
	int worldIdx[SdkPrioA::kMaxTrackedWorld]{};
	int nWorld = 0;
	if (wantWorld)
		nWorld = SdkPrioA::CopyWorldIndices(worldIdx, SdkPrioA::kMaxTrackedWorld);

	if (!wantWorld) {
		// Vis post-pass still runs below for players
	} else
	for (int wi = 0; wi < nWorld; ++wi) {
		const int i = worldIdx[wi];
		if (i <= 0)
			continue;
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity))
			continue;

		const char* designerEarly = GetDesignerName(Entity);
		const bool designerIsCtrl = designerEarly && designerEarly[0]
			&& (std::strcmp(designerEarly, "cs_player_controller") == 0
				|| std::strstr(designerEarly, "player_controller") != nullptr);
		if (designerIsCtrl)
			continue; // players handled above

		const bool designerMatters = designerEarly && designerEarly[0]
			&& DesignerMayMatter(designerEarly, false, wantWeapons, wantNades, wantBombScan);
		if (designerEarly && designerEarly[0] && !designerMatters)
			continue;

		char clsBuf[128]{};
		const char* clsName = "";
		bool needClass = !designerEarly || !designerEarly[0];
		if (!needClass) {
			const bool knownWeapon = wantWeapons && std::strncmp(designerEarly, "weapon_", 7) == 0;
			const bool knownNade = ClassifyWorldNade(nullptr, designerEarly) >= 0;
			const bool knownBomb = IsWorldDroppedC4("", designerEarly)
				|| std::strstr(designerEarly, "planted") != nullptr;
			if (designerMatters && !knownWeapon && !knownNade && !knownBomb)
				needClass = true;
		}
		if (needClass) {
			if (Mem::SchemaClassName(Entity, clsBuf, sizeof(clsBuf)))
				clsName = clsBuf;
		}

		auto* base = reinterpret_cast<C_BaseEntity*>(Entity);
		const char* designer = designerEarly ? designerEarly : GetDesignerName(Entity);
		const char* tag = clsName;
		const char* tag2 = designer ? designer : "";
		if (!tag[0] && !tag2[0])
			continue;

		// Planted bomb only (weapon C_C4 / weapon_c4 is NOT planted)
		const bool isBomb = (Config::world_esp_bomb || Config::widget_bomb) &&
			(strstr(tag, "PlantedC4") || strstr(tag, "planted_c4")
				|| strstr(tag2, "planted_c4") || strstr(tag2, "PlantedC4")
				|| strstr(tag2, "planted c4"));
		const bool nadeGlow = Config::glow_world_grenades;
		const int nadeKind = ClassifyWorldNade(tag, tag2);
		const bool isSmoke = nadeKind == WORLD_SMOKE && (Config::world_esp_smoke || nadeGlow);
		const bool isMolly = nadeKind == WORLD_MOLOTOV && (Config::world_esp_molotov || nadeGlow);
		const bool isHE = nadeKind == WORLD_HE && (Config::world_esp_he || nadeGlow);
		const bool isFlash = nadeKind == WORLD_FLASH && (Config::world_esp_flash || nadeGlow);
		const bool isDecoy = nadeKind == WORLD_DECOY && (Config::world_esp_decoy || nadeGlow);

		// Lazy origin - only entities that can be a bomb / weapon / nade pay
		// for the engine scene-node read. Reading it for every entity each
		// frame was the hard FPS drop with dropped-weapon ESP enabled.
		Vector_t origin{ 0.f, 0.f, 0.f };
		bool originOk = false;
		{
			const bool mayMatter =
				isBomb || isSmoke || isMolly || isHE || isFlash || isDecoy
				|| (wantWeapons && (
					std::strncmp(tag2, "weapon_", 7) == 0
					|| std::strstr(tag, "Weapon") != nullptr
					|| std::strstr(tag2, "Weapon") != nullptr
					|| std::strstr(tag, "C4") != nullptr));
			if (mayMatter) {
				origin = GetEntityAbsOrigin(base);
				// reject only truly invalid origins (origin can be near zero on some maps)
				if (std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z)
					&& !(std::fabs(origin.x) < 0.01f && std::fabs(origin.y) < 0.01f
						&& std::fabs(origin.z) < 0.01f))
					originOk = true;
			}
		}

		// -- Planted C4 --------------------------------------
		if (isBomb) {
			if (!originOk)
				continue;
			const PlantedBombState bs = ReadPlantedBomb(reinterpret_cast<C_PlantedC4*>(Entity));
			// Hide after real explode. Defused stays on the bomb widget with timer.
			const bool liveBlow = bs.ok && curtime > 0.f && bs.blow > curtime && (bs.blow - curtime) <= 45.f;
			const bool explodedDead = bs.ok && bs.exploded && !bs.ticking && !liveBlow;
			if (explodedDead) {
				ResetBombWallClock();
				continue;
			}

			sawBomb = true;
			WorldCache w{};
			w.kind = WORLD_BOMB;
			w.position = origin;
			w.bomb_site = bs.ok ? bs.site : -1;
			if (w.bomb_site < 0 || w.bomb_site > 1) {
				const int classified = Bomb::ClassifySite(origin);
				if (classified >= 0)
					w.bomb_site = classified;
			}
			w.defusing = bs.ok && bs.defusing && !bs.defused;
			w.defused = bs.ok && bs.defused;
			w.timer = -1.f;

			float gameLeft = -1.f;
			if (liveBlow)
				gameLeft = bs.blow - curtime;

			// Epoch isolates recycled C4 handles across rounds
			const uint32_t bKey = WorldHandleKey(Entity, i)
				^ (s_worldRoundEpoch * 0x9E3779B9u)
				^ WorldThrowSig(origin);
			const float wallNow = WallTimeSec();
			float wallLeft = (s_bombEndWall > 0.f && bKey == s_bombTrackKey)
				? (s_bombEndWall - wallNow) : -1.f;
			const float seedLen = (bs.ok && bs.timerLength >= 10.f && bs.timerLength <= 60.f)
				? bs.timerLength : 40.f;
			const bool alreadyFrozen = (s_bombFrozenKey == bKey && s_bombFrozenLeft >= 0.f);
			const bool needSeed = !alreadyFrozen && !(bs.ok && bs.defused) && (bKey != 0) && (
				bKey != s_bombTrackKey
				|| wallLeft < 0.f
				|| (gameLeft >= 0.f && std::fabs(gameLeft - wallLeft) > 2.0f));
			if (needSeed) {
				s_bombTrackKey = bKey;
				const float seed = (gameLeft >= 0.f && gameLeft <= 45.f) ? gameLeft : seedLen;
				s_bombEndWall = wallNow + seed;
				wallLeft = seed;
			}

			if (bs.ok && bs.defused) {
				w.timer = FreezeBombTimer(bKey, (gameLeft >= 0.f) ? gameLeft : wallLeft);
			} else if (gameLeft >= 0.f && wallLeft >= 0.f && std::fabs(gameLeft - wallLeft) <= 1.5f)
				w.timer = gameLeft;
			else if (gameLeft >= 0.f)
				w.timer = gameLeft;
			else if (wallLeft >= 0.f && wallLeft <= 45.f)
				w.timer = wallLeft;
			else if (bs.ok && bs.ticking)
				w.timer = seedLen;

			const bool isDefused = bs.ok && bs.defused;

			if (isDefused) {
				snprintf(w.label, sizeof(w.label), "Defused");
				FillBombTimers(w, bs, w.timer, -1.f);
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = false;
				g_plantedBomb.defused = true;
				g_plantedBomb.blowLeft = w.timer;
				g_plantedBomb.defuseLeft = -1.f;
				g_plantedBomb.defuseLength = (bs.defuseLength >= 4.f) ? bs.defuseLength : 10.f;
				snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
				if (Config::world_esp_bomb)
					cached_world.push_back(w);
				continue;
			}

			if (bs.ok && bs.defusing) {
				const float defLeft = ResolveDefuseLeft(bs, curtime);
				const float blowLeft = w.timer;
				FillBombTimers(w, bs, blowLeft, defLeft);
				snprintf(w.label, sizeof(w.label), "Defusing");
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = true;
				g_plantedBomb.defused = false;
				g_plantedBomb.blowLeft = blowLeft;
				g_plantedBomb.defuseLeft = defLeft;
				g_plantedBomb.defuseLength = (bs.defuseLength >= 4.f) ? bs.defuseLength : 10.f;
			}
			else if (w.timer >= 0.f) {
				snprintf(w.label, sizeof(w.label), "Bomb");
				FillBombTimers(w, bs, w.timer, -1.f);
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = false;
				g_plantedBomb.defused = false;
				g_plantedBomb.blowLeft = w.timer;
				g_plantedBomb.defuseLeft = -1.f;
				g_plantedBomb.defuseLength = (bs.defuseLength >= 4.f) ? bs.defuseLength : 10.f;
			}
			else
				snprintf(w.label, sizeof(w.label), "Bomb");
			snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");

			if (!g_plantedBomb.active && bs.ok) {
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = w.defusing;
				g_plantedBomb.defused = false;
				g_plantedBomb.blowLeft = w.timer;
				g_plantedBomb.defuseLeft = ResolveDefuseLeft(bs, curtime);
				g_plantedBomb.defuseLength = (bs.defuseLength >= 4.f) ? bs.defuseLength : 10.f;
			}

			if (Config::world_esp_bomb)
				cached_world.push_back(w);
			continue;
		}

		// -- Projectiles / inferno ---------------------------
		// Never drop for dormant - nades often flagged dormant while airborne
		if (isSmoke || isMolly || isHE || isFlash || isDecoy) {
			const bool isInfernoFire = isMolly && (std::strstr(tag, "Inferno") || std::strstr(tag2, "inferno"));
			if (!originOk && !isInfernoFire)
				continue;
			WorldCache w{};
			w.position = origin;
			w.land_position = origin;
			ImVec4 gcol = Config::world_esp_smoke_color;
			if (isSmoke) {
				w.kind = WORLD_SMOKE;
				snprintf(w.weapon_key, sizeof(w.weapon_key), "smokegrenade");
				gcol = Config::world_esp_smoke_color;
				w.use_badge = true;
				bool didSmoke = false;
				static uint32_t s_smokeDidOff = 0;
				static uint32_t s_smokeDetOff = 0;
				if (!s_smokeDidOff) {
					s_smokeDidOff = SchemaFinder::Get(
						hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_bDidSmokeEffect"));
					if (!s_smokeDidOff) s_smokeDidOff = 0x127C;
				}
				if (!s_smokeDetOff) {
					s_smokeDetOff = SchemaFinder::Get(
						hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_vSmokeDetonationPos"));
					if (s_smokeDetOff < 0x100) s_smokeDetOff = 0x1290;
				}
				const uint32_t smokeOff = s_smokeDidOff;
				{
					auto* pb = reinterpret_cast<uint8_t*>(base) + smokeOff;
					if (Mem::IsReadable(pb, 1))
						didSmoke = (*pb != 0);
				}
				w.effect_active = didSmoke;
				// Smoke grenade volume in CS2 is exactly 144 units radius
				w.radius = 144.f;
				if (didSmoke) {
					const uint32_t detOff = s_smokeDetOff;
					auto* pd = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + detOff);
					if (Mem::IsReadable(pd, 12) && std::isfinite(pd[0]) && std::isfinite(pd[1]) && std::isfinite(pd[2])) {
						const float dx = pd[0] - origin.x;
						const float dy = pd[1] - origin.y;
						const float dz = pd[2] - origin.z;
						if (dx * dx + dy * dy + dz * dz > 1.f)
							w.position = Vector_t{ pd[0], pd[1], pd[2] };
					}
				}
				snprintf(w.label, sizeof(w.label), "SMOKE");
			}
			else if (isMolly) {
				w.kind = WORLD_MOLOTOV;
				const bool fire = strstr(tag, "Inferno") || strstr(tag2, "inferno");
				gcol = Config::world_esp_molotov_color;
				w.use_badge = true;
				if (fire) {
					// C_Inferno: lit fire positions -> convex hull with live half-width
					snprintf(w.label, sizeof(w.label), "FIRE");
					snprintf(w.weapon_key, sizeof(w.weapon_key), "molotov");
					w.effect_active = true;
					w.radius = 50.f;
					// Cache once - SchemaFinder map lookup was per-inferno per frame
					static int s_fcOff = 0, s_posOff = 0, s_burnOff = 0, s_halfOff = 0;
					if (!s_fcOff) {
						const uint32_t offFc = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_fireCount"));
						const uint32_t offPos = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_firePositions"));
						const uint32_t offBurn = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_bFireIsBurning"));
						const uint32_t offHalf = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_maxFireHalfWidth"));
						s_fcOff = offFc ? (int)offFc : 0x1960;
						s_posOff = offPos ? (int)offPos : 0x1020;
						s_burnOff = offBurn ? (int)offBurn : 0x1620;
						// IDA schema: HalfWidth @ 0x858C (0x8588 = m_nlosperiod - wrong)
						s_halfOff = (offHalf >= 0x100) ? (int)offHalf : 0x858C;
					}
					const int fcOff = s_fcOff;
					const int posOff = s_posOff;
					const int burnOff = s_burnOff;
					const int halfOff = s_halfOff;

					int fc = 0;
					{
						auto* pi = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + fcOff);
						if (Mem::IsReadable(pi, sizeof(int)))
							fc = *pi;
					}
					// fc==0 can happen for one frame after C_Inferno spawns before m_fireCount is networked.
					// Old code skipped entirely -> no hull for ~0.5s and "still no inferno radius" reports.
					// Fall through to fallback radius below instead of dropping the entity.
					// Post-effect = fire dying - clear with game
					bool postFx = false;
					{
						auto* pb = reinterpret_cast<uint8_t*>(base) + 0x196C;
						if (Mem::IsReadable(pb, 1))
							postFx = (*pb != 0);
					}
					if (postFx)
						continue;

					// IDA drawable min 60. Store raw halfW (draw path clamps) - no 1.35 pad
					// here (old code double-padded and over-drew).
					float flameR = 60.f;
					{
						auto tryHalf = [&](int off) -> float {
							auto* pf = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + off);
							if (!Mem::IsReadable(pf, sizeof(float)))
								return 0.f;
							const float hw = *pf;
							return (hw > 12.f && hw < 120.f) ? hw : 0.f;
						};
						float hw = tryHalf(halfOff);
						if (hw < 1.f && halfOff != 0x858C)
							hw = tryHalf(0x858C);
						if (hw > 1.f)
							flameR = hw;
						if (flameR < 50.f)
							flameR = 50.f;
					if (flameR > 90.f)
						flameR = 90.f;
					}
					const int nFire = (std::clamp)(fc, 0, 64);
					w.fire_half_width = flameR;
					w.fire_count = 0;
					float maxFireRadius = flameR;
					const auto* firePositions = reinterpret_cast<const Vector_t*>(
						reinterpret_cast<const uint8_t*>(base) + posOff);
					const auto* fireBurning = reinterpret_cast<const uint8_t*>(base) + burnOff;
					for (int fi = 0; fi < nFire; ++fi) {
						if (!Mem::IsReadable(fireBurning + fi, 1)
							|| fireBurning[fi] == 0
							|| !Mem::IsReadable(firePositions + fi, sizeof(Vector_t)))
							continue;
						const Vector_t p = firePositions[fi];
						if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
							continue;
						if (std::fabs(p.x) < 0.01f && std::fabs(p.y) < 0.01f
							&& std::fabs(p.z) < 0.01f)
							continue;
						w.fire_pos[w.fire_count++] = p;
						const float dx = p.x - origin.x;
						const float dy = p.y - origin.y;
						maxFireRadius = (std::max)(maxFireRadius,
							std::sqrt(dx * dx + dy * dy) + flameR);
					}
					w.radius = (std::clamp)(maxFireRadius, flameR, 250.f);
				} else {
					snprintf(w.label, sizeof(w.label), "MOLLY");
					snprintf(w.weapon_key, sizeof(w.weapon_key), "molotov");
					w.effect_active = false;
					w.radius = 150.f; // inferno_max_range - predicted full spread
					// Keep landed shell until fuse expires - Inferno handoff can lag
				}
			}
			else if (isHE) {
				w.kind = WORLD_HE;
				snprintf(w.label, sizeof(w.label), "HE");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "hegrenade");
				gcol = Config::world_esp_he_color;
				w.use_badge = true;
				// Only hide once explode began AND body is stopped (avoid mid-air wipe)
				float vx = 0.f, vy = 0.f, vz = 0.f;
				{
					auto* pv = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x3F8);
					if (Mem::IsReadable(pv, 12)) { vx = pv[0]; vy = pv[1]; vz = pv[2]; }
				}
				const bool flying = (vx * vx + vy * vy + vz * vz) >= 4.f;
				if (!flying) {
					bool explodeBegan = false;
					auto* pb = reinterpret_cast<uint8_t*>(base) + 0x1214;
					if (Mem::IsReadable(pb, 1)) explodeBegan = (*pb != 0);
					if (explodeBegan)
						continue;
				}
			}
			else if (isFlash) {
				w.kind = WORLD_FLASH;
				snprintf(w.label, sizeof(w.label), "FLASH");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "flashbang");
				gcol = Config::world_esp_flash_color;
				w.use_badge = true;
				float vx = 0.f, vy = 0.f, vz = 0.f;
				{
					auto* pv = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x3F8);
					if (Mem::IsReadable(pv, 12)) { vx = pv[0]; vy = pv[1]; vz = pv[2]; }
				}
				const bool flying = (vx * vx + vy * vy + vz * vz) >= 4.f;
				if (!flying) {
					bool explodeBegan = false;
					auto* pb = reinterpret_cast<uint8_t*>(base) + 0x1214;
					if (Mem::IsReadable(pb, 1)) explodeBegan = (*pb != 0);
					if (explodeBegan)
						continue;
				}
			}
			else {
				w.kind = WORLD_DECOY;
				snprintf(w.label, sizeof(w.label), "DECOY");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "decoy");
				gcol = Config::world_esp_decoy_color;
				w.use_badge = true;
			}
			// Expiry + timer: handle+epoch+kind+throwSig so recycled slots don't bleed
			const uint32_t throwSig = WorldThrowSig(origin);
			const uint32_t fxKey = WorldFxKey(Entity, i, nadeKind, throwSig);
			float left = WorldEffectRemaining(fxKey, nadeKind, w.effect_active, throwSig);

			// HE/Flash: engine m_flDetonateTime beats late wall discovery ("too fast")
			if ((isHE || isFlash) && left > 0.05f) {
				const uint32_t kOffDet = Offset::m_flDetonateTime(); // C_BaseGrenade->m_flDetonateTime
				float det = 0.f;
				auto* pf = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + kOffDet);
				if (Mem::IsReadable(pf, sizeof(float)))
					det = *pf;
				if (det > 1.f && det < 1.0e7f && curtime > 1.f && det > curtime && (det - curtime) <= 3.f) {
					const float eng = det - curtime;
					if (eng < left - 0.15f)
						left = eng;
				}
			}

			w.timer = left;
			// Drop the frame effect/fuse ends (instant clear on explode / smoke end)
			if (left >= 0.f && left <= 0.05f)
				continue;

			if (Config::glow_world_grenades)
				Glow::ApplyWorld(Entity, gcol, true);

			w.land_position = w.position;

			const bool wantNadeEsp =
				(isSmoke && Config::world_esp_smoke)
				|| (isMolly && Config::world_esp_molotov)
				|| (isHE && Config::world_esp_he) || (isFlash && Config::world_esp_flash)
				|| (isDecoy && Config::world_esp_decoy);
			// Pred/warn own flying + land badges via MirrorNadePathsToWorld.
			// Entity-scan cache is 1 frame stale (Update runs after cache) and
			// double-draws with Mirror when nade is far from predicted land.
			if (wantNadeEsp)
				cached_world.push_back(w);
			continue;
		}

		// -- Dropped C4 (C_C4 / weapon_c4) - weapons ESP or bomb ESP --
		// Dropped C4 keeps living m_hOwnerEntity; near-owner 96u hid it.
		const bool droppedC4 = IsWorldDroppedC4(tag, tag2);
		if (droppedC4) {
			if (!originOk)
				continue;
			if (!Config::world_esp_bomb && !Config::world_esp_weapons)
				continue;
			if (!IsDroppedWeaponCheap(base))
				continue;
			WorldCache w{};
			w.kind = Config::world_esp_weapons ? WORLD_WEAPON : WORLD_BOMB;
			w.position = origin;
			w.timer = -1.f;
			w.bomb_site = -1;
			w.use_badge = false;
			snprintf(w.label, sizeof(w.label), "C4");
			snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
			cached_world.push_back(w);
			continue;
		}

		// -- Dropped weapons (clean: name + icon + distance only) ---------
		if (!Config::world_esp_weapons)
			continue;
		if (!ClassLooksLikeWeapon(tag) && !ClassLooksLikeWeapon(tag2)
			&& std::strncmp(tag2, "weapon_", 7) != 0)
			continue;
		// Knife-classed world entities are NOT real drops in TDM/FFA - enemies
		// respawn holding a knife that stays near their corpse, and the cheap
		// owner test sees a dead pawn -> false "dropped knife" on the ground.
		// Spoofing these as drops made the ground show a knife icon mid-gun battle.
		if (KnifeLookalikeTag(tag) || KnifeLookalikeTag(tag2)
			|| std::strncmp(tag2, "knife_", 6) == 0
			|| std::strstr(tag2, "knife") != nullptr)
			continue;
		if (!originOk)
			continue;
		if (IsEntityDormant(base))
			continue;
		// Cheap drop test - dead owner / no owner = dropped; living pawn = held.
		// No active-weapon lookups, no owner-origin reads (was the FPS + crash
		// source: per-frame owner chains on every weapon entity).
		if (!IsDroppedWeaponCheap(base))
			continue;
		// Distance cap: post-fight piles of 30+ guns must not grow the walk /
		// draw cost - only track weapons within ~200 m of the local player.
		if (cached_local.active && Mem::Finite(cached_local.position.x)) {
			const float dx = origin.x - cached_local.position.x;
			const float dy = origin.y - cached_local.position.y;
			const float dz = origin.z - cached_local.position.z;
			if ((dx * dx + dy * dy + dz * dz) > (8000.f * 8000.f))
				continue;
		}

		WorldCache w{};
		w.kind = WORLD_WEAPON;
		w.position = origin;
		w.timer = -1.f;
		ReadWeaponEntityName(reinterpret_cast<C_CSWeaponBase*>(Entity),
			w.label, sizeof(w.label), w.weapon_key, sizeof(w.weapon_key));
		if (!w.label[0] && designer && designer[0]) {
			CopyCleanWeaponName(designer, w.label, sizeof(w.label));
			if (!w.weapon_key[0])
				MakeWeaponIconKey(designer, w.weapon_key, sizeof(w.weapon_key));
		}
		if (!w.label[0]) {
			CopyCleanWeaponName(clsName, w.label, sizeof(w.label));
			if (!w.weapon_key[0])
				MakeWeaponIconKey(clsName, w.weapon_key, sizeof(w.weapon_key));
		}
		if (!w.label[0])
			continue;
		w.icon_glyph = ResolveWeaponIconGlyph(w.weapon_key);
		cached_world.push_back(w);
	}

	// Fallback: pPlantedC4s pattern list, then CUtlAutoList RVA
	if (wantWorld && !sawBomb && (Config::world_esp_bomb || Config::widget_bomb)) {
		if (void* planted = Bomb::PlantedC4Entity()) {
			if (Mem::ValidEntity(planted)) {
				auto* ent = reinterpret_cast<CEntityInstance*>(planted);
				auto* base = reinterpret_cast<C_BaseEntity*>(planted);
				const Vector_t origin = GetEntityAbsOrigin(base);
				if (std::isfinite(origin.x) && !(std::fabs(origin.x) < 0.01f
					&& std::fabs(origin.y) < 0.01f && std::fabs(origin.z) < 0.01f)) {
					const PlantedBombState bs = ReadPlantedBomb(reinterpret_cast<C_PlantedC4*>(planted));
					const bool liveBlow = bs.ok && curtime > 0.f && bs.blow > curtime
						&& (bs.blow - curtime) <= 45.f;
					const bool explodedDead = bs.ok && bs.exploded && !bs.ticking && !liveBlow;
					if (!explodedDead) {
						sawBomb = true;
						WorldCache w{};
						w.kind = WORLD_BOMB;
						w.position = origin;
						w.bomb_site = bs.ok ? bs.site : -1;
						if (w.bomb_site < 0 || w.bomb_site > 1) {
							const int classified = Bomb::ClassifySite(origin);
							if (classified >= 0)
								w.bomb_site = classified;
						}
						const bool isDefused = bs.ok && bs.defused;
						w.defusing = bs.ok && bs.defusing && !isDefused;
						w.defused = isDefused;
						float gameLeft = liveBlow ? (bs.blow - curtime) : -1.f;
						const uint32_t fbKey = WorldHandleKey(ent, 0)
							^ (s_worldRoundEpoch * 0x9E3779B9u)
							^ WorldThrowSig(origin);
						if (isDefused)
							w.timer = FreezeBombTimer(fbKey, gameLeft);
						else
							w.timer = (gameLeft >= 0.f) ? gameLeft : ((bs.ok && bs.timerLength >= 10.f) ? bs.timerLength : 40.f);
						const float defLeftFb = ResolveDefuseLeft(bs, curtime);
						FillBombTimers(w, bs, w.timer, w.defusing ? defLeftFb : -1.f);
						if (isDefused)
							snprintf(w.label, sizeof(w.label), "Defused");
						else if (w.defusing)
							snprintf(w.label, sizeof(w.label), "Defusing");
						else
							snprintf(w.label, sizeof(w.label), "Bomb");
						snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
						g_plantedBomb.active = true;
						g_plantedBomb.site = w.bomb_site;
						g_plantedBomb.position = origin;
						g_plantedBomb.defusing = w.defusing;
						g_plantedBomb.defused = isDefused;
						g_plantedBomb.blowLeft = w.timer;
						g_plantedBomb.defuseLeft = isDefused ? -1.f : defLeftFb;
						g_plantedBomb.defuseLength = (bs.defuseLength >= 4.f) ? bs.defuseLength : 10.f;
						if (Config::world_esp_bomb)
							cached_world.push_back(w);
					}
				}
			}
		}
	}
	// Stale Autolist RVA fallback removed - was 0x236D678 (wrong; IDA is 0x236E678)
	// and double-deref'd the slot. Bomb::PlantedC4Entity() above is pattern-resolved.

	// Drop timers for entities gone this frame (new serial = new nade next round)
	if (wantWorld) {
		WorldFxEndFrame();
		// Bomb entity recycled / gone between rounds - don't keep a dead wall clock
		static int s_noBombFrames = 0;
		if (sawBomb)
			s_noBombFrames = 0;
		else if (++s_noBombFrames >= 45) {
			ResetBombWallClock();
			s_noBombFrames = 0;
		}
	}

	// NadePred::Update + path->world mirror run on Present (Visuals::esp).

	// Vis traces (bones + engine trace) from Present - only when UI needs occluded color.
	// Old: Config::glow alone forced traces even with plain glow (DrawGlow path) -> insecure.
	// Sticky: if pawn moved <~6u and we traced last frame, reuse.
	const bool needVis = WantPlayerVis()
		&& Trace::Ready() && H::oGetLocalPlayer && !cached_players.empty();
	if (needVis) {
		C_CSPlayerPawn* localPawn = H::SafeLocalAlive();
		if (localPawn) {
			const Vector_t eye = Bones::GetEyePos(localPawn);
			if (Bones::IsValidPos(eye)) {
				const int pc = (int)cached_players.size();
				const Vector_t localPos = cached_local.active ? cached_local.position : eye;
				// Alternating recheck: even frames recheck half, odd the other - always fresh-ish
				static int s_visFrame = 0;
				++s_visFrame;
				for (int i = 0; i < pc; ++i) {
					auto& entry = cached_players[i];
					// Reuse sticky if position barely moved
					if (entry.visCached) {
						const float mdx = entry.position.x - entry.visSampleX;
						const float mdy = entry.position.y - entry.visSampleY;
						const float mdz = entry.position.z - entry.visSampleZ;
						const float moved2 = mdx * mdx + mdy * mdy + mdz * mdz;
						// Stagger full re-trace: only every other frame per slot when still
						if (moved2 < 36.f && ((i + s_visFrame) & 1) == 0)
							continue; // keep entry.visible from sticky seed
					}

					C_CSPlayerPawn* tgt = nullptr;
					if (!ResolvePawn(entry.handle, &tgt) || !tgt) {
						entry.visible = true;
						entry.visCached = false;
						continue;
					}

					const float dx = entry.position.x - localPos.x;
					const float dy = entry.position.y - localPos.y;
					const float dz = entry.position.z - localPos.z;
					const float dist2 = dx * dx + dy * dy + dz * dz;
					// ~80m - far LOD still samples head+spine (head-only caused false occluded)
					const bool farLod = dist2 > (3150.f * 3150.f);

					Vector_t samples[3]{};
					int n = 0;
					uintptr_t ba = 0;
					Vector_t origin{};
					float height = 0.f;
					Bones::Map map{};
					if (Bones::GetBoneArrayReadonly(tgt, ba, origin, height)
						&& Bones::ResolveMapCached(tgt, ba, origin, height, map)) {
						Vector_t head{};
						if (Bones::GetSlotPos(ba, map, Bones::S_HEAD, head) && Bones::IsValidPos(head))
							samples[n++] = head;
						Vector_t p{};
						if (n < 3 && Bones::GetSlotPos(ba, map, Bones::S_SPINE2, p) && Bones::IsValidPos(p))
							samples[n++] = p;
						if (!farLod && n < 3
							&& Bones::GetSlotPos(ba, map, Bones::S_PELVIS, p) && Bones::IsValidPos(p))
							samples[n++] = p;
					}
					if (n < 1) {
						samples[n++] = entry.position + entry.viewOffset;
						if (!farLod)
							samples[n++] = entry.position;
					}

					// Head-first: stop on first hit (IsBodyVisible already early-outs)
					entry.visible = Trace::IsBodyVisible(eye, samples, n, localPawn, tgt);
					entry.visCached = true;
					entry.visSampleX = entry.position.x;
					entry.visSampleY = entry.position.y;
					entry.visSampleZ = entry.position.z;
				}
			}
		}
	}

	// Rebuild sticky table for next frame
	s_visStickyN = 0;
	for (const auto& e : cached_players) {
		if (s_visStickyN >= 64)
			break;
		if (!e.handle.valid())
			continue;
		VisSticky& s = s_visSticky[s_visStickyN++];
		s.h = e.handle.raw();
		s.vis = e.visible;
		s.x = e.position.x;
		s.y = e.position.y;
		s.z = e.position.z;
		s.ok = true;
	}

	// Publish player list for DrawGlow (other threads) - after vis sticky done.
	EspPublishPlayersFromCache();

	// Player glow: DrawGlow hook stamps properties - do NOT ApplyPlayer
	// from FSN cache walk (full entity scan + property spam tripped 2nd-queue insecure).
	// World glow still uses ApplyWorld below when world_esp/glow_world_* and cache ran.
}

// -- Skeleton ESP - -----------------------------------------
// Five fixed bone chains (spine / arm L / arm R / leg L / leg R) drawn as
// Catmull-Rom splines: smooth, connected, single color, no outline clutter.
// Chain breaks flush partial segments (missing bone / off-screen / teleport).
// Slot order matches head->neck->spine_4..1->pelvis, hand->elbow->
// shoulder->clavicle->spine_4, foot->knee->hip->pelvis.

static void DrawSkeleton(ImDrawList* drawList, C_CSPlayerPawn* pawn, const ViewMatrix& vm, ImU32 col) {
	if (!drawList || !pawn)
		return;

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	if (!Bones::GetBoneArrayReadonly(pawn, boneArray, origin, height))
		return;

	Bones::Map map{};
	if (!Bones::ResolveMapCached(pawn, boneArray, origin, height, map))
		return;

	// Raw screen positions for every slot this frame
	ImVec2 raw[Bones::S_COUNT];
	bool   okRaw[Bones::S_COUNT]{};
	for (int s = 0; s < Bones::S_COUNT; ++s) {
		Vector_t p{};
		if (Bones::GetSlotPos(boneArray, map, s, p) && Bones::IsValidPos(p)) {
			Vector_t sp{};
			if (vm.WorldToScreen(p, sp)) {
				raw[s] = ImVec2(sp.x, sp.y);
				okRaw[s] = true;
			}
		}
	}

	const float th = std::clamp(Config::esp_skeleton_thickness, 1.f, 4.f);

	constexpr int kNone = -1;
	// velocity chains (Bones::Slot ids)
	const int chains[5][7] = {
		{ Bones::S_HEAD,   Bones::S_NECK,   Bones::S_SPINE3, Bones::S_SPINE2, Bones::S_SPINE1, Bones::S_SPINE0, Bones::S_PELVIS },
		{ Bones::S_HAND_L, Bones::S_ARM_L_L, Bones::S_ARM_U_L, Bones::S_CLAV_L, Bones::S_SPINE3, kNone, kNone },
		{ Bones::S_HAND_R, Bones::S_ARM_L_R, Bones::S_ARM_U_R, Bones::S_CLAV_R, Bones::S_SPINE3, kNone, kNone },
		{ Bones::S_ANKLE_L, Bones::S_LEG_L_L, Bones::S_LEG_U_L, Bones::S_PELVIS, kNone, kNone, kNone },
		{ Bones::S_ANKLE_R, Bones::S_LEG_L_R, Bones::S_LEG_U_R, Bones::S_PELVIS, kNone, kNone, kNone },
	};

	constexpr float kStep = 0.08f;

	// Catmull-Rom spline over the chain's screen points with stack buffers (zero heap allocations).
	const auto flushSegment = [&](ImVec2* pts, int& nPts) {
		if (nPts < 2) {
			nPts = 0;
			return;
		}
		// Endpoints padding
		ImVec2 padded[12];
		padded[0] = pts[0];
		for (int i = 0; i < nPts; ++i)
			padded[i + 1] = pts[i];
		padded[nPts + 1] = pts[nPts - 1];
		const int totalPadded = nPts + 2;

		ImVec2 spline[128];
		int nSpline = 0;

		for (int i = 0; i + 3 < totalPadded; ++i) {
			const ImVec2& p0 = padded[i];
			const ImVec2& p1 = padded[i + 1];
			const ImVec2& p2 = padded[i + 2];
			const ImVec2& p3 = padded[i + 3];
			for (float t = 0.f; t <= 1.f && nSpline < 127; t += kStep) {
				const float t2 = t * t;
				const float t3 = t2 * t;
				spline[nSpline++] = ImVec2(
					0.5f * ((2.f * p1.x) + (-p0.x + p2.x) * t
						+ (2.f * p0.x - 5.f * p1.x + 4.f * p2.x - p3.x) * t2
						+ (-p0.x + 3.f * p1.x - 3.f * p2.x + p3.x) * t3),
					0.5f * ((2.f * p1.y) + (-p0.y + p2.y) * t
						+ (2.f * p0.y - 5.f * p1.y + 4.f * p2.y - p3.y) * t2
						+ (-p0.y + 3.f * p1.y - 3.f * p2.y + p3.y) * t3));
			}
		}
		if (nSpline >= 2)
			drawList->AddPolyline(spline, nSpline, col, th, 0);
		nPts = 0;
	};

	for (int c = 0; c < 5; ++c) {
		ImVec2 pts[8];
		int nPts = 0;
		for (int i = 0; i < 7; ++i) {
			const int b = chains[c][i];
			if (b == kNone)
				break;
			if (!okRaw[b]) {
				flushSegment(pts, nPts);
				continue;
			}
			const ImVec2 pt = raw[b];
			if (nPts > 0) {
				const float dx = pt.x - pts[nPts - 1].x;
				const float dy = pt.y - pts[nPts - 1].y;
				// teleport / projection jump - start a new segment
				if (dx * dx + dy * dy > 250000.f) {
					flushSegment(pts, nPts);
				}
			}
			if (nPts < 8)
				pts[nPts++] = pt;
		}
		flushSegment(pts, nPts);
	}

	if (Config::esp_skeleton_head && okRaw[Bones::S_HEAD]) {
		float headR = 3.2f;
		if (okRaw[Bones::S_NECK]) {
			const float dx = raw[Bones::S_HEAD].x - raw[Bones::S_NECK].x;
			const float dy = raw[Bones::S_HEAD].y - raw[Bones::S_NECK].y;
			headR = std::sqrt(dx * dx + dy * dy) * 0.72f;
		} else {
			Vector_t headW{};
			if (Bones::GetSlotPos(boneArray, map, Bones::S_HEAD, headW) && Bones::IsValidPos(headW)) {
				Vector_t a{}, b{};
				const Vector_t side{ headW.x + 4.2f, headW.y, headW.z };
				if (vm.WorldToScreen(headW, a) && vm.WorldToScreen(side, b))
					headR = std::hypot(a.x - b.x, a.y - b.y);
			}
		}
		headR = std::clamp(headR, 1.4f, 7.5f);
		const float outline = headR < 2.4f ? 0.8f : 1.05f;
		drawList->AddCircle(raw[Bones::S_HEAD], headR + 0.7f, IM_COL32(0, 0, 0, 120), 20, outline);
		drawList->AddCircle(raw[Bones::S_HEAD], headR, col, 20, outline);
	}
}

// CS2 ESP box (/ UC standard):
// 1) Primary: collision AABB (mins+origin, maxs+origin) -> project 8 corners
// Perspective-correct at all ranges (fixes close-up head-only + fat mid-range).
// 2) Fallback: feet origin + viewOffset head, width = height * scale.
static bool ComputeEspBox(C_CSPlayerPawn* pawn, const PlayerCache& player,
	const ViewMatrix& vm, float& outX, float& outY, float& outW, float& outH)
{
	const float widthScale = std::clamp(Config::esp_box_width, 0.28f, 0.70f);

	// --- Priority 1: engine hitbox surrounding box (dump 0x8F3B60) - exact pose/weapon-aware box ---
	// This replaces manual hull calc and fixes cut-off boxes when crouched/carry knife
	if (pawn && Mem::ValidEntity(pawn)) {
		Vector_t mins{}, maxs{};
		if (Bones::ComputeSurroundingBox(pawn, mins, maxs)) {
			const bool minsOk = Mem::Finite(mins.x) && Mem::Finite(mins.y) && Mem::Finite(mins.z);
			const bool maxsOk = Mem::Finite(maxs.x) && Mem::Finite(maxs.y) && Mem::Finite(maxs.z);
			const float hx = maxs.x - mins.x;
			const float hy = maxs.y - mins.y;
			const float hz = maxs.z - mins.z;
			if (minsOk && maxsOk && hx > 1.f && hy > 1.f && hz > 8.f) {
				const Vector_t corners[8] = {
					{ mins.x, mins.y, mins.z }, { mins.x, maxs.y, mins.z },
					{ maxs.x, maxs.y, mins.z }, { maxs.x, mins.y, mins.z },
					{ maxs.x, maxs.y, maxs.z }, { mins.x, maxs.y, maxs.z },
					{ mins.x, mins.y, maxs.z }, { maxs.x, mins.y, maxs.z },
				};
				float minSX = 1e9f, maxSX = -1e9f, minSY = 1e9f, maxSY = -1e9f;
				int n = 0;
				for (const Vector_t& c : corners) {
					Vector_t s{};
					if (!vm.WorldToScreen(c, s)) continue;
					minSX = (std::min)(minSX, s.x); maxSX = (std::max)(maxSX, s.x);
					minSY = (std::min)(minSY, s.y); maxSY = (std::max)(maxSY, s.y);
					++n;
				}
				const float boxW0 = maxSX - minSX; const float boxH0 = maxSY - minSY;
				if (n >= 2 && boxW0 >= 2.f && boxH0 >= 4.f && std::isfinite(boxW0) && std::isfinite(boxH0)) {
					const float padY = (std::max)(1.5f, boxH0 * 0.025f);
					const float padX = (std::max)(1.0f, boxW0 * 0.04f);
					const float wMul = 0.70f + (widthScale - 0.28f) * (0.55f / 0.42f);
					const float cx = (minSX + maxSX) * 0.5f;
					float boxW = boxW0 * std::clamp(wMul, 0.65f, 1.30f) + padX * 2.f;
					float boxH = boxH0 + padY * 1.5f;
					outX = cx - boxW * 0.5f; outY = minSY - padY; outW = boxW; outH = boxH;
					return true;
				}
			}
		}
	}

	// --- Priority 2: collision hull 8-corner projection (fallback) ---
	if (pawn && Mem::ValidEntity(pawn)) {
		CCollisionProperty* col = pawn->m_pCollision();
		if (col && Mem::Valid(col, 0x50)) {
			const Vector_t origin = GetAbsOrigin(pawn);
			Vector_t mins = col->m_vecMins();
			Vector_t maxs = col->m_vecMaxs();

			const bool minsOk = Mem::Finite(mins.x) && Mem::Finite(mins.y) && Mem::Finite(mins.z);
			const bool maxsOk = Mem::Finite(maxs.x) && Mem::Finite(maxs.y) && Mem::Finite(maxs.z);
			const bool originOk = Bones::IsValidPos(origin);
			// Reject degenerate / zero hulls
			const float hx = maxs.x - mins.x;
			const float hy = maxs.y - mins.y;
			const float hz = maxs.z - mins.z;

			if (minsOk && maxsOk && originOk && hx > 1.f && hy > 1.f && hz > 8.f) {
				mins.x += origin.x; mins.y += origin.y; mins.z += origin.z;
				maxs.x += origin.x; maxs.y += origin.y; maxs.z += origin.z;

				const Vector_t corners[8] = {
					{ mins.x, mins.y, mins.z }, { mins.x, maxs.y, mins.z },
					{ maxs.x, maxs.y, mins.z }, { maxs.x, mins.y, mins.z },
					{ maxs.x, maxs.y, maxs.z }, { mins.x, maxs.y, maxs.z },
					{ mins.x, mins.y, maxs.z }, { maxs.x, mins.y, maxs.z },
				};

				float minSX = 1e9f, maxSX = -1e9f, minSY = 1e9f, maxSY = -1e9f;
				int n = 0;
				for (const Vector_t& c : corners) {
					Vector_t s{};
					if (!vm.WorldToScreen(c, s))
						continue; // skip behind-camera corner, keep rest
					minSX = (std::min)(minSX, s.x);
					maxSX = (std::max)(maxSX, s.x);
					minSY = (std::min)(minSY, s.y);
					maxSY = (std::max)(maxSY, s.y);
					++n;
				}

				const float boxW0 = maxSX - minSX;
				const float boxH0 = maxSY - minSY;
				if (n >= 2 && boxW0 >= 2.f && boxH0 >= 4.f
					&& std::isfinite(boxW0) && std::isfinite(boxH0))
				{
					// Small pad so box is not skin-tight on collision hull
					const float padY = (std::max)(1.5f, boxH0 * 0.025f);
					const float padX = (std::max)(1.0f, boxW0 * 0.04f);
					// Width scale nudges collision width (1.0 = hull as-is; slider ~0.42 default is narrow)
					// Map slider so 0.42 ? slight tighten, 0.55 ? hull, 0.70 ? loose
					const float wMul = 0.70f + (widthScale - 0.28f) * (0.55f / 0.42f); // ~0.70..1.25
					const float cx = (minSX + maxSX) * 0.5f;
					float boxW = boxW0 * std::clamp(wMul, 0.65f, 1.30f) + padX * 2.f;
					float boxH = boxH0 + padY * 1.5f;

					outX = cx - boxW * 0.5f;
					outY = minSY - padY;
					outW = boxW;
					outH = boxH;
					return true;
				}
			}
		}
	}

	// --- Fallback: feet + eye height (no max height clamp) ---
	const float eyeZ = player.viewOffset.z;
	const float headPad = (eyeZ > 1.f) ? eyeZ * 0.18f : 12.f;
	const float footPad = (eyeZ > 1.f) ? eyeZ * 0.08f : 5.f;

	Vector_t feetWorld = player.position;
	feetWorld.z -= footPad;
	Vector_t headWorld = player.position + player.viewOffset;
	headWorld.z += headPad;

	Vector_t feetScreen{}, headScreen{};
	if (!vm.WorldToScreen(feetWorld, feetScreen) || !vm.WorldToScreen(headWorld, headScreen))
		return false;

	float top = headScreen.y;
	float bottom = feetScreen.y;
	if (bottom < top)
		std::swap(top, bottom);

	float boxH = bottom - top;
	if (boxH < 4.f)
		return false;

	const float padY = (std::max)(2.f, boxH * 0.03f);
	top -= padY;
	bottom += padY;
	boxH = bottom - top;

	float boxW = boxH * widthScale;
	const float padX = (std::max)(1.5f, boxW * 0.04f);
	boxW += padX * 2.f;

	const float centerX = (feetScreen.x + headScreen.x) * 0.5f;
	outX = centerX - boxW * 0.5f;
	outY = top;
	outW = boxW;
	outH = boxH;
	return true;
}

// 3D collision AABB wireframe (oriented via abs origin + hull mins/maxs).
static void Draw3DBox(ImDrawList* dl, C_CSPlayerPawn* pawn, const ViewMatrix& vm, ImU32 col, float thickness) {
	if (!dl || !pawn || !Mem::ValidEntity(pawn))
		return;
	CCollisionProperty* colp = pawn->m_pCollision();
	if (!colp || !Mem::Valid(colp, 0x50))
		return;
	const Vector_t origin = GetAbsOrigin(pawn);
	Vector_t mins = colp->m_vecMins();
	Vector_t maxs = colp->m_vecMaxs();
	if (!Bones::IsValidPos(origin))
		return;
	const float hx = maxs.x - mins.x, hy = maxs.y - mins.y, hz = maxs.z - mins.z;
	if (hx < 1.f || hy < 1.f || hz < 8.f)
		return;
	mins.x += origin.x; mins.y += origin.y; mins.z += origin.z;
	maxs.x += origin.x; maxs.y += origin.y; maxs.z += origin.z;

	const Vector_t corners[8] = {
		{ mins.x, mins.y, mins.z }, { maxs.x, mins.y, mins.z },
		{ maxs.x, maxs.y, mins.z }, { mins.x, maxs.y, mins.z },
		{ mins.x, mins.y, maxs.z }, { maxs.x, mins.y, maxs.z },
		{ maxs.x, maxs.y, maxs.z }, { mins.x, maxs.y, maxs.z },
	};
	Vector_t scr[8]{};
	bool ok[8]{};
	int nOk = 0;
	for (int i = 0; i < 8; ++i) {
		ok[i] = vm.WorldToScreen(corners[i], scr[i]);
		if (ok[i]) ++nOk;
	}
	if (nOk < 2)
		return;

	static const int edges[12][2] = {
		{0,1},{1,2},{2,3},{3,0},
		{4,5},{5,6},{6,7},{7,4},
		{0,4},{1,5},{2,6},{3,7}
	};
	const float t = std::clamp(thickness, 1.f, 4.f);
	const ImU32 outline = IM_COL32(0, 0, 0, 180);
	for (const auto& e : edges) {
		if (!ok[e[0]] || !ok[e[1]])
			continue;
		const ImVec2 a(scr[e[0]].x, scr[e[0]].y);
		const ImVec2 b(scr[e[1]].x, scr[e[1]].y);
		dl->AddLine(a, b, outline, t + 1.2f);
		dl->AddLine(a, b, col, t);
	}
}

// OOF: W2S::ProjectOrEdge (front = NDC edge, behind = yaw/pitch compass).
// Old path used WorldToScreen + world-XY atan2 -> wrong side when behind, and
// radius floated mid-screen without clamping to the visible edge rect.
static bool GetLocalEyeAndView(Vector_t& eye, QAngle_t& ang) {
	eye.x = eye.y = eye.z = 0.f;
	ang.x = ang.y = ang.z = 0.f;
	if (cached_local.active && Bones::IsValidPos(cached_local.position)) {
		eye = cached_local.position;
		// Approximate eye height if we have no live pawn this frame
		eye.z += 64.f;
	}
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (local) {
		const Vector_t e = Bones::GetEyePos(local);
		if (Bones::IsValidPos(e))
			eye = e;
	}
	if (!Bones::IsValidPos(eye))
		return false;

	if (Input::GetViewAngles && Input::viewAngleContext) {
		const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
		if (viewPtr && Mem::IsReadable(reinterpret_cast<void*>(viewPtr), sizeof(Vector_t))) {
			Vector_t v{};
			v.x = v.y = v.z = 0.f;
			__try { v = *reinterpret_cast<const Vector_t*>(viewPtr); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.getLocalEye"); v.x = v.y = v.z = 0.f; }
			if (std::isfinite(v.x) && std::isfinite(v.y)) {
				ang.x = v.x;
				ang.y = v.y;
				ang.z = 0.f;
			}
		}
	}
	return true;
}

static void DrawOofArrow(ImDrawList* dl, const PlayerCache& player,
	const Vector_t& eye, const QAngle_t& viewAng,
	ImU32 baseCol, float radius, float size, bool occluded)
{
	if (!dl)
		return;
	const ImGuiIO& io = ImGui::GetIO();
	const float sw = io.DisplaySize.x;
	const float sh = io.DisplaySize.y;
	if (sw < 32.f || sh < 32.f)
		return;
	const float cx = sw * 0.5f;
	const float cy = sh * 0.5f;

	// Chest aim point (stable vs head bob)
	Vector_t world = player.position;
	const float eyeLift = (player.viewOffset.z > 1.f) ? player.viewOffset.z * 0.55f : 36.f;
	world.z += eyeLift;
	if (!Bones::IsValidPos(world))
		return;

	// Edge margin keeps tip + label inside HUD bounds
	const float edgeMargin = 18.f + std::clamp(size, 8.f, 28.f) * 0.40f;
	float edgeX = 0.f, edgeY = 0.f;
	bool onScreen = false;
	if (!W2S::ProjectOrEdge(world, edgeX, edgeY, onScreen, edgeMargin, eye, viewAng))
		return;
	if (onScreen)
		return; // In FOV - standard ESP handles target

	float dx = edgeX - cx;
	float dy = edgeY - cy;
	float edgeLen = std::sqrt(dx * dx + dy * dy);
	if (edgeLen < 1e-3f) {
		dx = 0.f; dy = -1.f; edgeLen = 1.f;
	} else {
		dx /= edgeLen; dy /= edgeLen;
	}

	// Clamp tip inside visible frame
	const float maxR = edgeLen - 2.f;
	if (maxR < 20.f)
		return;
	const float r = std::clamp(radius, 40.f, maxR);
	const ImVec2 tip(cx + dx * r, cy + dy * r);

	// Distance (meters)
	float meters = 0.f;
	if (cached_local.active) {
		const float wx = player.position.x - cached_local.position.x;
		const float wy = player.position.y - cached_local.position.y;
		const float wz = player.position.z - cached_local.position.z;
		meters = std::sqrt(wx * wx + wy * wy + wz * wz) * 0.0254f;
	}

	// Threat scaling & distance fade (declutter peripheral noise)
	float s = std::clamp(size, 8.f, 32.f);
	float alpha = occluded ? 0.65f : 1.f;
	if (meters > 0.f && meters < 16.f) {
		s *= 1.f + (16.f - meters) * 0.012f; // Slight prominence on close threats
	} else if (meters > 45.f) {
		alpha *= std::clamp(1.f - (meters - 45.f) / 75.f, 0.35f, 1.f);
	}

	const bool lowHp = player.health > 0 && player.health <= 30;
	const ImU32 mainCol = lowHp ? IM_COL32(255, 70, 70, 255) : baseCol;

	const float cr = static_cast<float>((mainCol >> 0) & 0xFF) / 255.f;
	const float cg = static_cast<float>((mainCol >> 8) & 0xFF) / 255.f;
	const float cb = static_cast<float>((mainCol >> 16) & 0xFF) / 255.f;
	const float ca = static_cast<float>((mainCol >> 24) & 0xFF) / 255.f * alpha;

	// Balanced faceted lighting
	const ImU32 colLeft = IM_COL32(
		static_cast<int>(std::clamp(cr * 1.08f, 0.f, 1.f) * 255.f + 0.5f),
		static_cast<int>(std::clamp(cg * 1.08f, 0.f, 1.f) * 255.f + 0.5f),
		static_cast<int>(std::clamp(cb * 1.08f, 0.f, 1.f) * 255.f + 0.5f),
		static_cast<int>(ca * 255.f + 0.5f));
	const ImU32 colRight = IM_COL32(
		static_cast<int>(std::clamp(cr * 0.88f, 0.f, 1.f) * 255.f + 0.5f),
		static_cast<int>(std::clamp(cg * 0.88f, 0.f, 1.f) * 255.f + 0.5f),
		static_cast<int>(std::clamp(cb * 0.88f, 0.f, 1.f) * 255.f + 0.5f),
		static_cast<int>(ca * 255.f + 0.5f));
	const ImU32 colGlow = IM_COL32(
		static_cast<int>(cr * 255.f + 0.5f),
		static_cast<int>(cg * 255.f + 0.5f),
		static_cast<int>(cb * 255.f + 0.5f),
		static_cast<int>(ca * 0.28f * 255.f + 0.5f));
	const ImU32 colSpine = IM_COL32(255, 255, 255, static_cast<int>(ca * 0.55f * 255.f + 0.5f));
	const ImU32 colBorder = IM_COL32(8, 10, 14, static_cast<int>(ca * 0.85f * 255.f + 0.5f));

	// Sleek delta chevron geometry
	const float px = -dy, py = dx; // Perpendicular unit vector
	const float len = s * 1.35f;
	const float halfW = s * 0.44f;
	const float notchLen = len * 0.32f;

	const ImVec2 leftWing(tip.x - dx * len + px * halfW, tip.y - dy * len + py * halfW);
	const ImVec2 rightWing(tip.x - dx * len - px * halfW, tip.y - dy * len - py * halfW);
	const ImVec2 centerNotch(tip.x - dx * (len - notchLen), tip.y - dy * (len - notchLen));

	// 1) Soft ambient glow aura (anti-aliasing)
	const ImVec2 glowPoly[4] = { tip, leftWing, centerNotch, rightWing };
	dl->AddPolyline(glowPoly, 4, colGlow, 2.5f, ImDrawFlags_Closed);

	// 2) Smooth dual-facet delta fill
	const ImVec2 polyLeft[3] = { tip, leftWing, centerNotch };
	const ImVec2 polyRight[3] = { tip, centerNotch, rightWing };
	dl->AddConvexPolyFilled(polyLeft, 3, colLeft);
	dl->AddConvexPolyFilled(polyRight, 3, colRight);

	// 3) Sleek spine highlight line
	dl->AddLine(tip, centerNotch, colSpine, 1.0f);

	// 4) Ultra-crisp 1px hairline border
	dl->AddPolyline(glowPoly, 4, colBorder, 1.0f, ImDrawFlags_Closed);

	// 5) Clean, minimalist floating distance text (no heavy clunky card box)
	if (meters > 0.5f) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%dm", static_cast<int>(meters + 0.5f));
		const ImVec2 ts = ImGui::CalcTextSize(buf);

		const float textDist = len + 4.f + ts.y * 0.5f;
		const ImVec2 textCenter(tip.x - dx * textDist, tip.y - dy * textDist);
		const float tx = std::floor(textCenter.x - ts.x * 0.5f);
		const float ty = std::floor(textCenter.y - ts.y * 0.5f);

		// Subtle health color on text if wounded, otherwise clean light grey
		ImU32 textCol = IM_COL32(230, 234, 240, static_cast<int>(alpha * 240.f));
		if (player.health > 0 && player.health <= 30)
			textCol = IM_COL32(255, 80, 80, static_cast<int>(alpha * 255.f));
		else if (player.health > 0 && player.health <= 60)
			textCol = IM_COL32(255, 200, 70, static_cast<int>(alpha * 255.f));

		DrawTextOutlined(dl, tx, ty, textCol, buf);
	}
}

// Per-player W2S fail budget. Behind-cam / NaN-transform / just-spawned pawns
// can fail ComputeEspBox for many frames in a row. Retrying every frame wastes
// bone reads + trace calls. After 3 consecutive fails, skip that handle for 60
// frames (~1s at 60fps) then retry. Same-thread Present access; no lock needed.
struct W2SBudget {
	std::uint32_t handle = 0;
	int streak = 0;
	std::uint32_t retryAtFrame = 0;
};
static std::unordered_map<int, W2SBudget> g_w2sBudget;
static constexpr int kW2SMaxStreak = 3;
static constexpr std::uint32_t kW2SCooldownFrames = 60;

void Visuals::drawPlayers() {
	const bool anyFlag = Config::flag_flashed || Config::flag_bomb || Config::flag_scoped
		|| Config::flag_reloading || Config::flag_defusing
		|| Config::flag_money || Config::flag_kit || Config::flag_helmet || Config::flag_nades;
	if (!Config::esp && !Config::showHealth && !Config::espFill && !Config::showNameTags
		&& !Config::showArmor && !Config::showDistance && !Config::showWeapon
		&& !Config::showWeaponIcon
		&& !Config::esp_skeleton && !anyFlag
		&& !Config::esp_rank && !Config::esp_3d_box && !Config::esp_oof)
		return;

	if (cached_players.empty())
		return;

	int filterTeam = (cached_local.team == 2 || cached_local.team == 3)
		? cached_local.team
		: ((cached_local.lastTeam == 2 || cached_local.lastTeam == 3) ? cached_local.lastTeam : 0);
	if (filterTeam != 2 && filterTeam != 3) {
		if (C_CSPlayerPawn* lp = H::SafeLocalPlayer()) {
			__try { filterTeam = static_cast<int>(lp->getTeam()); }
			__except (EXCEPTION_EXECUTE_HANDLER) { filterTeam = 0; }
		}
	}

	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	if (!drawList)
		return;

	// Precompute colors once per frame (not per-player)
	const ImU32 colBoxVis = ImGui::ColorConvertFloat4ToU32(Config::espColor);
	const ImU32 colBoxOcc = ImGui::ColorConvertFloat4ToU32(Config::espColorInvisible);
	const ImU32 colSkelVis = ImGui::ColorConvertFloat4ToU32(Config::esp_skeleton_color);
	const ImU32 colSkelOcc = ImGui::ColorConvertFloat4ToU32(Config::esp_skeleton_color_invisible);
	const ImU32 colName = ImGui::ColorConvertFloat4ToU32(Config::esp_name_color);
	const ImU32 colWep = ImGui::ColorConvertFloat4ToU32(Config::esp_weapon_color);
	const ImU32 colWepIcon = ImGui::ColorConvertFloat4ToU32(Config::esp_weapon_icon_color);
	const ImU32 colDist = ImGui::ColorConvertFloat4ToU32(Config::esp_distance_color);
	const ImU32 colArmor = ImGui::ColorConvertFloat4ToU32(Config::esp_armor_color);
	const ImU32 colHpFixed = ImGui::ColorConvertFloat4ToU32(Config::esp_health_color);
	const ImU32 colRank = ImGui::ColorConvertFloat4ToU32(Config::esp_rank_color);
	const ImU32 col3d = ImGui::ColorConvertFloat4ToU32(Config::esp_3d_box_color);
	const ImU32 colOof = ImGui::ColorConvertFloat4ToU32(Config::esp_oof_color);
	ImVec4 fillVis = Config::espColor;
	fillVis.w = Config::espFillOpacity;
	ImVec4 fillOcc = Config::espColorInvisible;
	fillOcc.w = Config::espFillOpacity;
	const ImU32 colFillVis = ImGui::ColorConvertFloat4ToU32(fillVis);
	const ImU32 colFillOcc = ImGui::ColorConvertFloat4ToU32(fillOcc);
	const float lineH = ImGui::GetFontSize();
	const bool doTeamCheck = GameMode::WantTeamCheck(Config::teamCheck);
	const bool wantSkel = Config::esp_skeleton;
	const bool wantBox = Config::esp;
	const bool wantFill = Config::espFill && Config::esp;
	const bool want3d = Config::esp_3d_box;
	const bool wantOof = Config::esp_oof;

	// Resolve eye/view once per frame for OOF (ProjectOrEdge behind-cam path)
	Vector_t oofEye{};
	QAngle_t oofAng{};
	const bool oofReady = wantOof && GetLocalEyeAndView(oofEye, oofAng);

	const std::uint32_t curFrame = H::g_presentFrame.load(std::memory_order_relaxed);
	const float dt = ImGui::GetIO().DeltaTime;

	for (const auto& Player : cached_players) {
		if (!Player.handle.valid() || Player.health <= 0)
			continue;

		if (doTeamCheck && ((filterTeam == 2 || filterTeam == 3) ? Player.team_num == filterTeam : Player.type == team))
			continue;

		// W2S retry budget - skip cooldowning players before any bone/trace work
		const int hIdx = Player.handle.index();
		const std::uint32_t handleRaw = Player.handle.raw();
		auto& budget = g_w2sBudget[hIdx];
		if (budget.handle != handleRaw) {
			budget = {};
			budget.handle = handleRaw;
		}

		const bool useOccluded = Config::esp_vis_check && !Player.visible;
		const ImU32 boxColor = useOccluded ? colBoxOcc : colBoxVis;
		const ImU32 skelColor = useOccluded ? colSkelOcc : colSkelVis;
		const ImU32 fillColor = useOccluded ? colFillOcc : colFillVis;

		// Cheap in-front probe (cached origin, matrix math only - no bones/traces).
		// Fast flicks / 180? turns put enemies behind the camera for a few frames;
		// behind-cam must NOT feed the W2S streak, or ESP would stay hidden for the
		// whole ~1 s cooldown after every turn. Streak now only comes from real
		// in-front failures (NaN spawn transforms), which is what the budget is for.
		Vector_t probe{};
		if (!viewMatrix.WorldToScreen(Player.position, probe)) {
			budget.streak = 0;
			budget.retryAtFrame = 0;
			if (oofReady)
				DrawOofArrow(drawList, Player, oofEye, oofAng, colOof,
					Config::esp_oof_radius, Config::esp_oof_size, useOccluded);
			continue;
		}
		if (budget.streak >= kW2SMaxStreak && curFrame < budget.retryAtFrame)
			continue;

		C_CSPlayerPawn* drawPawn = nullptr;
		// Skeleton + collision box need live pawn; text-only path can skip resolve
		if (wantSkel || wantBox || want3d || Config::espFill || Config::showHealth || Config::showArmor
			|| Config::showNameTags || Config::showDistance || Config::showWeapon
			|| Config::showWeaponIcon || anyFlag || Config::esp_rank)
			ResolvePawn(Player.handle, &drawPawn);

		if (oofReady)
			DrawOofArrow(drawList, Player, oofEye, oofAng, colOof,
				Config::esp_oof_radius, Config::esp_oof_size, useOccluded);

		if (wantSkel && drawPawn)
			DrawSkeleton(drawList, drawPawn, viewMatrix, skelColor);

		if (want3d && drawPawn)
			Draw3DBox(drawList, drawPawn, viewMatrix, col3d, Config::espThickness);

		float boxX = 0.f, boxY = 0.f, boxW = 0.f, boxH = 0.f;
		if (!ComputeEspBox(drawPawn, Player, viewMatrix, boxX, boxY, boxW, boxH)) {
			if (++budget.streak >= kW2SMaxStreak)
				budget.retryAtFrame = curFrame + kW2SCooldownFrames;
			continue;
		}
		if (boxW < 2.f || boxH < 4.f) {
			if (++budget.streak >= kW2SMaxStreak)
				budget.retryAtFrame = curFrame + kW2SCooldownFrames;
			continue;
		}
		// Pixel-align box bounds for sharp geometry
		boxX = std::floor(boxX);
		boxY = std::floor(boxY);
		boxW = std::floor(boxW);
		boxH = std::floor(boxH);

		// Success - reset budget entry
		budget.streak = 0;
		budget.retryAtFrame = 0;

		if (wantFill) {
			drawList->AddRectFilled(
				ImVec2(boxX, boxY),
				ImVec2(boxX + boxW, boxY + boxH),
				fillColor);
		}

		if (wantBox)
			DrawBoxOutlined(drawList, boxX, boxY, boxW, boxH, boxColor, Config::espThickness, Config::esp_box_style);

		const float barW = std::clamp(Config::esp_bar_width, 2.f, 6.f);
		const float barGap = 2.f;
		const float cx = std::floor(boxX + boxW * 0.5f);

		// Positionable element layout: every element draws into its configured
		// slot (top/bottom/left/right) and advances that side's cursor, so
		// stacked elements never overlap. Bars consume outward px, text stacks
		// vertical lines per side.
		float topOut = 0.f, botOut = 0.f, leftOut = 0.f, rightOut = 0.f;
		float leftTextY = boxY;
		// Flags own the top of the right edge; right-side text stacks below them.
		float flagLines = 0.f;
		if (anyFlag) {
			if (Config::flag_money && Player.money >= 0) ++flagLines;
			if (Config::flag_flashed && Player.flashed) ++flagLines;
			if (Config::flag_bomb && Player.bomb) ++flagLines;
			if (Config::flag_scoped && Player.scoped) ++flagLines;
			if (Config::flag_reloading && Player.reloading) ++flagLines;
			if (Config::flag_defusing && Player.defusing) ++flagLines;
			if (Config::flag_kit && Player.has_defuser) ++flagLines;
			if (Config::flag_helmet && Player.has_helmet) ++flagLines;
			if (Config::flag_nades
				&& (Player.nade_he || Player.nade_flash || Player.nade_smoke
					|| Player.nade_molly || Player.nade_decoy)) ++flagLines;
		}
		float rightTextY = boxY + (flagLines > 0.f ? flagLines * (lineH - 1.f) + 3.f : 0.f);

		if (Config::showHealth) {
			const float maxHp = static_cast<float>((std::max)(Player.maxHealth, 1));
			const float hpTarget = std::clamp(Player.health / maxHp, 0.f, 1.f);
			BarAnim& st = g_barAnim[hIdx];
			if (st.handle != handleRaw) {
				st = {};
				st.handle = handleRaw;
			}
			TickBar(st, hpTarget, st.curHp, st.velHp, st.trailHp, st.flashHp, dt);
			ImU32 hpCol;
			if (Config::esp_health_auto) {
				// Smooth green -> yellow -> red
				const float r = std::clamp(2.f * (1.f - hpTarget), 0.f, 1.f);
				const float g = std::clamp(2.f * hpTarget, 0.f, 1.f);
				hpCol = IM_COL32((int)(r * 255.f), (int)(g * 220.f + 20.f), 45, 255);
			} else {
				hpCol = colHpFixed;
			}
			const ImU32 trailCol = IM_COL32(255, 64, 64, (int)(150.f * st.flashHp));

			float barX = 0.f, barY = 0.f, barWd = barW, barHd = boxH;
			bool horiz = false;
			switch (Config::esp_pos_health) {
			case Config::ESP_POS_RIGHT:
				barX = boxX + boxW + barGap + rightOut; barY = boxY;
				rightOut += barW + barGap;
				break;
			case Config::ESP_POS_TOP:
				barX = boxX; barY = boxY - 2.f - topOut; barWd = boxW; barHd = 2.f;
				topOut += 4.5f; horiz = true;
				break;
			case Config::ESP_POS_BOTTOM:
				barX = boxX; barY = boxY + boxH + 2.f + botOut; barWd = boxW; barHd = 2.f;
				botOut += 4.5f; horiz = true;
				break;
			default:
				barX = boxX - barGap - barW - leftOut; barY = boxY;
				leftOut += barW + barGap;
				break;
			}
			if (horiz)
				DrawBottomBar(drawList, barX, barY, barWd, barHd, st.curHp, hpCol, st.trailHp, trailCol);
			else
				DrawSideBar(drawList, barX, barY, barWd, barHd, st.curHp, hpCol, st.trailHp, trailCol);

			// Skeet-style dynamic thumb tracking: HP number sits flush on the health fill level
			const bool bigBar = horiz ? (barWd >= 18.f) : (barHd >= 18.f);
			if (Player.health < Player.maxHealth && bigBar) {
				char hpTxt[8];
				snprintf(hpTxt, sizeof(hpTxt), "%d", Player.health);
				const ImVec2 hs = ImGui::CalcTextSize(hpTxt);
				float hx = 0.f, hy = 0.f;
				if (horiz) {
					const float fillW = barWd * std::clamp(st.curHp, 0.f, 1.f);
					hx = std::clamp(barX + fillW - hs.x * 0.5f, barX, barX + barWd - hs.x);
					hy = (Config::esp_pos_health == Config::ESP_POS_TOP)
						? std::floor(barY - hs.y - 1.f)
						: std::floor(barY + barHd + 1.f);
				} else {
					const float fillH = barHd * std::clamp(st.curHp, 0.f, 1.f);
					const float thumbY = barY + (barHd - fillH);
					hy = std::clamp(thumbY - hs.y * 0.5f, barY - 2.f, barY + barHd - hs.y);
					hx = (Config::esp_pos_health == Config::ESP_POS_RIGHT)
						? std::floor(barX + barWd + 2.f)
						: std::floor(barX - hs.x - 2.f);
				}
				DrawTextOutlined(drawList, hx, hy, IM_COL32(255, 255, 255, 240), hpTxt);
			}
		}

		if (Config::showArmor && Player.armor > 0) {
			const float armTarget = std::clamp(Player.armor / 100.f, 0.f, 1.f);
			BarAnim& st = g_barAnim[hIdx];
			if (st.handle != handleRaw) {
				st = {};
				st.handle = handleRaw;
			}
			TickBar(st, armTarget, st.curArm, st.velArm, st.trailArm, st.flashArm, dt);
			const ImU32 trailCol = IM_COL32(210, 214, 230, (int)(120.f * st.flashArm));
			switch (Config::esp_pos_armor) {
			case Config::ESP_POS_RIGHT:
				DrawSideBar(drawList, boxX + boxW + barGap + rightOut, boxY, barW, boxH, st.curArm, colArmor, st.trailArm, trailCol);
				rightOut += barW + barGap;
				break;
			case Config::ESP_POS_TOP:
				DrawBottomBar(drawList, boxX, boxY - 2.f - topOut, boxW, 2.f, st.curArm, colArmor, st.trailArm, trailCol);
				topOut += 4.5f;
				break;
			case Config::ESP_POS_BOTTOM:
				DrawBottomBar(drawList, boxX, boxY + boxH + 2.f + botOut, boxW, 2.f, st.curArm, colArmor, st.trailArm, trailCol);
				botOut += 4.5f;
				break;
			default:
				DrawSideBar(drawList, boxX - barGap - barW - leftOut, boxY, barW, boxH, st.curArm, colArmor, st.trailArm, trailCol);
				leftOut += barW + barGap;
				break;
			}
		}

		// Bottom Ammo Bar (Neverlose / Skeet style) - hugs the box, other
		// bottom elements stack below it.
		if (Player.clip >= 0 && Player.maxClip > 0) {
			const float ammoTarget = std::clamp(static_cast<float>(Player.clip) / static_cast<float>(Player.maxClip), 0.f, 1.f);
			BarAnim& st = g_barAnim[hIdx];
			if (st.handle != handleRaw) {
				st = {};
				st.handle = handleRaw;
			}
			TickBar(st, ammoTarget, st.curAmmo, st.velAmmo, st.trailAmmo, st.flashAmmo, dt);
			const ImU32 ammoCol = IM_COL32(80, 150, 235, 255);
			const ImU32 trailAmmoCol = IM_COL32(255, 180, 50, (int)(140.f * st.flashAmmo));
			DrawBottomBar(drawList, boxX, boxY + boxH + 2.f + botOut, boxW, 2.0f, st.curAmmo, ammoCol, st.trailAmmo, trailAmmoCol);
			botOut += 4.5f;
		}

		float nameBlockTop = boxY - lineH - 3.f; // fallback anchor for rank
		float nameBlockH = lineH;
		float nameBlockCx = cx;
		if (Config::showNameTags && Player.name[0] != '\0') {
			const ImVec2 ns = ImGui::CalcTextSize(Player.name);
			const float avatarSz = floorf(lineH + 1.f);
			const float avatarGap = 3.f;
			const bool drawAvatar = Config::esp_name_avatar;

			float totalW = ns.x;
			if (drawAvatar)
				totalW += avatarSz + avatarGap;
			const float blockH = drawAvatar ? avatarSz : ns.y;

			float bx = 0.f, by = 0.f;
			switch (Config::esp_pos_name) {
			case Config::ESP_POS_BOTTOM:
				bx = floorf(cx - totalW * 0.5f); by = boxY + boxH + botOut;
				botOut += blockH + 2.f;
				break;
			case Config::ESP_POS_LEFT:
				bx = floorf(boxX - 4.f - leftOut - totalW); by = leftTextY;
				leftTextY += blockH + 2.f;
				break;
			case Config::ESP_POS_RIGHT:
				bx = floorf(boxX + boxW + 4.f + rightOut); by = rightTextY;
				rightTextY += blockH + 2.f;
				break;
			default:
				bx = floorf(cx - totalW * 0.5f); by = boxY - topOut - blockH;
				topOut += blockH + 2.f;
				break;
			}
			by = floorf(by);
			nameBlockTop = by;
			nameBlockH = blockH;
			nameBlockCx = bx + totalW * 0.5f;

			float x = bx;
			if (drawAvatar) {
				const ImVec2 avMin(x, floorf(by + (blockH - avatarSz) * 0.5f));
				const ImVec2 avMax(avMin.x + avatarSz, avMin.y + avatarSz);
				const float round = avatarSz * 0.5f;
				const ImTextureID tex = SteamAvatar::Get(Player.steamId, pDevice);
				if (tex != ImTextureID_Invalid) {
					drawList->AddImageRounded(ImTextureRef(tex), avMin, avMax,
						ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 230), round);
				} else {
					drawList->AddCircleFilled(
						ImVec2(avMin.x + round, avMin.y + round), round,
						IM_COL32(36, 38, 46, 200), 12);
					char letter[2] = { '?', '\0' };
					if (Player.name[0]) {
						char c = Player.name[0];
						if (c >= 'a' && c <= 'z')
							c = static_cast<char>(c - 'a' + 'A');
						letter[0] = c;
					}
					const ImVec2 ls = ImGui::CalcTextSize(letter);
					drawList->AddText(
						ImVec2(avMin.x + round - ls.x * 0.5f, avMin.y + round - ls.y * 0.5f),
						IM_COL32(200, 205, 215, 220), letter);
				}
				x = floorf(x + avatarSz + avatarGap);
			}

			DrawTextOutlined(drawList, x, floorf(by + (blockH - ns.y) * 0.5f), colName, Player.name);
		}

		if (Config::esp_rank) {
			const char* rn = CompetitiveRankName(Player.rank);
			if (rn && rn[0]) {
				const ImVec2 rs = ImGui::CalcTextSize(rn);
				const float ry = (Config::esp_pos_name == Config::ESP_POS_BOTTOM)
					? floorf(nameBlockTop + nameBlockH + 1.f)
					: floorf(nameBlockTop - rs.y - 1.f);
				DrawTextOutlined(drawList, floorf(nameBlockCx - rs.x * 0.5f), ry, colRank, rn);
			}
		}

		// Weapon icon / weapon text / distance - each in its own slot.
		if (Config::showWeaponIcon) {
			float ix = 0.f, iy = 0.f;
			switch (Config::esp_pos_weapon_icon) {
			case Config::ESP_POS_BOTTOM: ix = cx; iy = boxY + boxH + botOut; break;
			case Config::ESP_POS_LEFT:   ix = boxX - 4.f - leftOut - lineH * 0.6f; iy = leftTextY; break;
			case Config::ESP_POS_RIGHT:  ix = boxX + boxW + 4.f + rightOut + lineH * 0.6f; iy = rightTextY; break;
			default:             ix = cx; iy = boxY - topOut - lineH; break;
			}
			const float ih = DrawWeaponIconCentered(drawList, ix, iy, colWepIcon, Player.weapon_key, Player.icon_glyph);
			if (ih > 0.f) {
				const float adv = ih + 1.f;
				switch (Config::esp_pos_weapon_icon) {
				case Config::ESP_POS_BOTTOM: botOut += adv; break;
				case Config::ESP_POS_LEFT:   leftTextY += adv; break;
				case Config::ESP_POS_RIGHT:  rightTextY += adv; break;
				default:             topOut += adv; break;
				}
			}
		}

		if (Config::showWeapon && Player.weapon_name[0] != '\0') {
			switch (Config::esp_pos_weapon) {
			case Config::ESP_POS_BOTTOM:
				DrawCenteredText(drawList, cx, boxY + boxH + botOut, colWep, Player.weapon_name);
				botOut += lineH + 1.f;
				break;
			case Config::ESP_POS_LEFT: {
				const ImVec2 ws = ImGui::CalcTextSize(Player.weapon_name);
				DrawTextOutlined(drawList, floorf(boxX - 4.f - leftOut - ws.x), floorf(leftTextY), colWep, Player.weapon_name);
				leftTextY += lineH + 1.f;
				break;
			}
			case Config::ESP_POS_RIGHT:
				DrawTextOutlined(drawList, floorf(boxX + boxW + 4.f + rightOut), floorf(rightTextY), colWep, Player.weapon_name);
				rightTextY += lineH + 1.f;
				break;
			default:
				DrawCenteredText(drawList, cx, boxY - topOut - lineH, colWep, Player.weapon_name);
				topOut += lineH + 1.f;
				break;
			}
		}

		if (Config::showDistance && cached_local.active) {
			const float dx = Player.position.x - cached_local.position.x;
			const float dy = Player.position.y - cached_local.position.y;
			const float dz = Player.position.z - cached_local.position.z;
			const float units = std::sqrt(dx * dx + dy * dy + dz * dz);
			const int meters = static_cast<int>(units * 0.0254f + 0.5f);
			char distText[24];
			snprintf(distText, sizeof(distText), "%dm", meters);
			switch (Config::esp_pos_distance) {
			case Config::ESP_POS_BOTTOM:
				DrawCenteredText(drawList, cx, boxY + boxH + botOut, colDist, distText);
				botOut += lineH + 1.f;
				break;
			case Config::ESP_POS_LEFT:
				DrawTextOutlined(drawList, floorf(boxX - 4.f - leftOut - ImGui::CalcTextSize(distText).x), floorf(leftTextY), colDist, distText);
				leftTextY += lineH + 1.f;
				break;
			case Config::ESP_POS_RIGHT:
				DrawTextOutlined(drawList, floorf(boxX + boxW + 4.f + rightOut), floorf(rightTextY), colDist, distText);
				rightTextY += lineH + 1.f;
				break;
			default:
				DrawCenteredText(drawList, cx, boxY - topOut - lineH, colDist, distText);
				topOut += lineH + 1.f;
				break;
			}
		}

		if (anyFlag) {
			float flagY = floorf(boxY);
			const float flagX = floorf(boxX + boxW + 4.f);
			const float flagLine = lineH - 1.f;

			if (Config::flag_money && Player.money >= 0) {
				char mon[16];
				snprintf(mon, sizeof(mon), "$%d", Player.money);
				DrawTextOutlined(drawList, flagX, flagY, IM_COL32(115, 215, 125, 255), mon);
				flagY += flagLine;
			}

			struct FlagItem { bool on; const char* text; ImU32 col; };
			const FlagItem flags[] = {
				{ Config::flag_flashed   && Player.flashed,   "FLASH",  IM_COL32(255, 235, 90, 255)  },
				{ Config::flag_bomb      && Player.bomb,      "C4",     IM_COL32(255, 80, 80, 255)   },
				{ Config::flag_scoped    && Player.scoped,    "ZOOM",   IM_COL32(80, 200, 255, 255) },
				{ Config::flag_reloading && Player.reloading, "RELOAD", IM_COL32(255, 175, 60, 255)  },
				{ Config::flag_defusing  && Player.defusing,  "DEF",    IM_COL32(80, 225, 135, 255)  },
				{ Config::flag_kit       && Player.has_defuser, "KIT",  IM_COL32(90, 200, 255, 255)  },
				{ Config::flag_helmet    && Player.has_helmet,  "HK",   IM_COL32(175, 195, 255, 255) },
			};
			for (const auto& f : flags) {
				if (!f.on)
					continue;
				DrawTextOutlined(drawList, flagX, flagY, f.col, f.text);
				flagY += flagLine;
			}

			if (Config::flag_nades) {
				char nbuf[12]{};
				int ni = 0;
				if (Player.nade_he && ni < 10) nbuf[ni++] = 'H';
				if (Player.nade_flash && ni < 10) nbuf[ni++] = 'F';
				if (Player.nade_smoke && ni < 10) nbuf[ni++] = 'S';
				if (Player.nade_molly && ni < 10) nbuf[ni++] = 'M';
				if (Player.nade_decoy && ni < 10) nbuf[ni++] = 'D';
				nbuf[ni] = '\0';
				if (ni > 0) {
					DrawTextOutlined(drawList, flagX, flagY, IM_COL32(255, 160, 70, 255), nbuf);
				}
			}
		}
	}
}

// Forward - used by DrawInfernoRadius (defined below)
// Precalculated trigonometric circle tables for high-performance radius rings
struct RadiusCircleTables {
	float c16[17], s16[17];
	float c24[25], s24[25];
	float c32[33], s32[33];
	float c36[37], s36[37];
	RadiusCircleTables() {
		auto fill = [](float* c, float* s, int n) {
			for (int i = 0; i <= n; ++i) {
				const float a = (static_cast<float>(i) / static_cast<float>(n)) * 6.28318530718f;
				c[i] = std::cos(a);
				s[i] = std::sin(a);
			}
		};
		fill(c16, s16, 16);
		fill(c24, s24, 24);
		fill(c32, s32, 32);
		fill(c36, s36, 36);
	}
};
static const RadiusCircleTables s_radiusTables{};

static void DrawWorldRadiusRing(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
                                float radius, ImU32 col, int segs = 32) {
	if (!dl || radius <= 1.f || !vm.viewMatrix)
		return;
	if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)
		|| !std::isfinite(radius) || radius > 500.f)
		return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x <= 1.f || ds.y <= 1.f)
		return;
	const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;

	const float* cTab = s_radiusTables.c32;
	const float* sTab = s_radiusTables.s32;
	if (segs <= 16) {
		segs = 16;
		cTab = s_radiusTables.c16;
		sTab = s_radiusTables.s16;
	} else if (segs <= 24) {
		segs = 24;
		cTab = s_radiusTables.c24;
		sTab = s_radiusTables.s24;
	} else if (segs <= 32) {
		segs = 32;
		cTab = s_radiusTables.c32;
		sTab = s_radiusTables.s32;
	} else {
		segs = 36;
		cTab = s_radiusTables.c36;
		sTab = s_radiusTables.s36;
	}

	// Near-plane distance: below this a vertex is at/behind the camera.
	// Segments crossing it are clipped to the plane instead of dropped whole,
	// so the ring stays continuous when the local player stands on the effect.
	constexpr float kClipNear = 0.12f;

	struct RV { float numX = 0.f, numY = 0.f, w = 0.f; bool ok = false; } prev{};
	auto toScreen = [cx, cy](const RV& v) {
		const float invW = 1.f / v.w;
		return ImVec2(cx + v.numX * invW * cx, cy - v.numY * invW * cy);
	};

	for (int i = 0; i <= segs; ++i) {
		const Vector_t w{
			center.x + cTab[i] * radius,
			center.y + sTab[i] * radius,
			center.z + 2.f
		};
		RV cur{};
		cur.ok = vm.WorldToClip(w, cur.numX, cur.numY, cur.w);

		if (i > 0 && prev.ok && cur.ok) {
			const bool aFront = prev.w >= kClipNear;
			const bool bFront = cur.w >= kClipNear;
			if (aFront || bFront) {
				RV ca = prev, cb = cur;
				if (aFront != bFront) {
					const float den = cur.w - prev.w;
					if (std::fabs(den) < 1e-6f) {
						prev = cur;
						continue;
					}
					const float t = (kClipNear - prev.w) / den;
					if (t < 0.f || t > 1.f) {
						prev = cur;
						continue;
					}
					RV mid;
					mid.numX = prev.numX + (cur.numX - prev.numX) * t;
					mid.numY = prev.numY + (cur.numY - prev.numY) * t;
					mid.w = kClipNear;
					mid.ok = true;
					if (aFront) cb = mid; else ca = mid;
				}
				dl->AddLine(toScreen(ca), toScreen(cb), col, 1.6f);
			}
		}
		prev = cur;
	}
}

// 2D Cross product of OA and OB vectors for convex hull
static inline float InfernoHullCross2D(const Vector_t& o, const Vector_t& a, const Vector_t& b) {
	return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static void DrawInfernoRadius(ImDrawList* dl, const ViewMatrix& vm, const WorldCache& item,
	const ImVec4& col4, float alphaScale) {
	if (!dl || !vm.viewMatrix)
		return;

	const int cr = std::clamp((int)(col4.x * 255.f), 0, 255);
	const int cg = std::clamp((int)(col4.y * 255.f), 0, 255);
	const int cb = std::clamp((int)(col4.z * 255.f), 0, 255);

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x <= 1.f || ds.y <= 1.f)
		return;
	const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;

	// 1. Collect candidate boundary points from flame nodes
	Vector_t pts[160];
	int nPts = 0;

	if (item.fire_count > 0) {
		const float flameR = (item.fire_half_width > 15.f) ? item.fire_half_width : 60.f;
		const int maxCheck = (std::min)(item.fire_count, 16);
		// 8 radial perimeter samples per fire position
		for (int fi = 0; fi < maxCheck && nPts < 140; ++fi) {
			const Vector_t& fp = item.fire_pos[fi];
			if (!std::isfinite(fp.x) || !std::isfinite(fp.y) || !std::isfinite(fp.z))
				continue;
			for (int k = 0; k < 8; ++k) {
				const float a = (static_cast<float>(k) / 8.f) * 6.2831853f;
				pts[nPts++] = {
					fp.x + std::cos(a) * flameR,
					fp.y + std::sin(a) * flameR,
					fp.z + 2.f
				};
			}
		}
	}

	if (nPts < 3) {
		const float r = (item.radius > 20.f) ? item.radius : 150.f;
		for (int k = 0; k < 24 && nPts < 140; ++k) {
			const float a = (static_cast<float>(k) / 24.f) * 6.2831853f;
			pts[nPts++] = {
				item.position.x + std::cos(a) * r,
				item.position.y + std::sin(a) * r,
				item.position.z + 2.f
			};
		}
	}

	if (nPts < 3)
		return;

	// 2. Compute 2D Convex Hull (Monotone Chain algorithm)
	std::sort(pts, pts + nPts, [](const Vector_t& a, const Vector_t& b) {
		if (std::fabs(a.x - b.x) > 1e-4f)
			return a.x < b.x;
		return a.y < b.y;
	});

	// Monotone chain can temporarily hold both halves; nPts is capped at 140.
	Vector_t hull[280];
	int k = 0;
	// Lower hull
	for (int i = 0; i < nPts; ++i) {
		while (k >= 2 && InfernoHullCross2D(hull[k - 2], hull[k - 1], pts[i]) <= 0.f)
			k--;
		hull[k++] = pts[i];
	}
	// Upper hull
	for (int i = nPts - 2, t = k + 1; i >= 0; i--) {
		while (k >= t && InfernoHullCross2D(hull[k - 2], hull[k - 1], pts[i]) <= 0.f)
			k--;
		hull[k++] = pts[i];
	}
	if (k > 1)
		k--; // Remove duplicate wrap-around point

	if (k < 3)
		return;

	// 3. Project polygon vertices to screen space with near-plane clipping
	constexpr float kClipNear = 0.12f;
	struct RV { float numX = 0.f, numY = 0.f, w = 0.f; bool ok = false; };
	auto toScreen = [cx, cy](const RV& v) {
		const float invW = 1.f / v.w;
		return ImVec2(cx + v.numX * invW * cx, cy - v.numY * invW * cy);
	};

	ImVec2 scr[128];
	int nScr = 0;

	for (int i = 0; i < k; ++i) {
		const Vector_t& pA = hull[i];
		const Vector_t& pB = hull[(i + 1) % k];

		RV curA{}, curB{};
		curA.ok = vm.WorldToClip(pA, curA.numX, curA.numY, curA.w);
		curB.ok = vm.WorldToClip(pB, curB.numX, curB.numY, curB.w);

		if (!curA.ok || !curB.ok)
			continue;

		const bool aFront = curA.w >= kClipNear;
		const bool bFront = curB.w >= kClipNear;

		if (aFront && bFront) {
			if (nScr < 127) scr[nScr++] = toScreen(curA);
		} else if (aFront || bFront) {
			const float den = curB.w - curA.w;
			if (std::fabs(den) > 1e-6f) {
				const float tRatio = (kClipNear - curA.w) / den;
				if (tRatio >= 0.f && tRatio <= 1.f) {
					RV mid;
					mid.numX = curA.numX + (curB.numX - curA.numX) * tRatio;
					mid.numY = curA.numY + (curB.numY - curA.numY) * tRatio;
					mid.w = kClipNear;
					if (aFront) {
						if (nScr < 127) scr[nScr++] = toScreen(curA);
						if (nScr < 127) scr[nScr++] = toScreen(mid);
					} else {
						if (nScr < 127) scr[nScr++] = toScreen(mid);
					}
				}
			}
		}
	}

	if (nScr < 3)
		return;

	// 4. Render Polygram Colors & Styling
	const ImU32 fillCol = IM_COL32(cr, cg, cb, static_cast<int>(34.f * alphaScale));
	const ImU32 glowCol = IM_COL32(cr, cg, cb, static_cast<int>(75.f * alphaScale));
	const ImU32 borderCol = IM_COL32(cr, cg, cb, static_cast<int>(225.f * alphaScale));
	const ImU32 coreCol = IM_COL32(255, 235, 190, static_cast<int>(170.f * alphaScale));
	const ImU32 latticeCol = IM_COL32(cr, cg, cb, static_cast<int>(24.f * alphaScale));

	// A. Translucent polygram fill
	dl->AddConvexPolyFilled(scr, nScr, fillCol);

	// B. Internal polygram lattice (connecting spokes between fire center and perimeter)
	Vector_t centerScr{};
	if (vm.WorldToScreen(item.position, centerScr)) {
		const ImVec2 cPt(centerScr.x, centerScr.y);
		for (int i = 0; i < nScr; i += 2) {
			dl->AddLine(cPt, scr[i], latticeCol, 1.0f);
		}
		// Soft central core glow
		dl->AddCircleFilled(cPt, 7.5f, IM_COL32(255, 195, 85, static_cast<int>(45.f * alphaScale)));
	}

	// C. Glowing embers at active flame nodes
	const ImU32 emberInner = IM_COL32(255, 240, 200, static_cast<int>(220.f * alphaScale));
	const ImU32 emberOuter = IM_COL32(cr, cg, cb, static_cast<int>(140.f * alphaScale));
	const int emberCount = (std::min)(item.fire_count, 14);
	for (int fi = 0; fi < emberCount; ++fi) {
		Vector_t esp{};
		if (vm.WorldToScreen(item.fire_pos[fi], esp)) {
			const ImVec2 ep(esp.x, esp.y);
			dl->AddCircleFilled(ep, 3.5f, emberOuter);
			dl->AddCircleFilled(ep, 1.5f, emberInner);
		}
	}

	// D. Triple-pass boundary glow & sharp high-contrast border
	dl->AddPolyline(scr, nScr, glowCol, 3.6f, ImDrawFlags_Closed);
	dl->AddPolyline(scr, nScr, borderCol, 1.7f, ImDrawFlags_Closed);
	dl->AddPolyline(scr, nScr, coreCol, 0.85f, ImDrawFlags_Closed);
}

static void DrawBombTimerFx(ImDrawList* dl, const ViewMatrix& vm, const WorldCache& item,
	const ImVec4& col4, float sx, float sy, float pulse)
{
	if (!dl || !std::isfinite(sx) || !std::isfinite(sy))
		return;

	const int cr = std::clamp((int)(col4.x * 255.f), 0, 255);
	const int cg = std::clamp((int)(col4.y * 255.f), 0, 255);
	const int cb = std::clamp((int)(col4.z * 255.f), 0, 255);

	const float blow = item.blow_left;
	const float blowFull = (item.blow_full >= 10.f && item.blow_full <= 60.f) ? item.blow_full : 40.f;
	const bool defused = item.defused;
	const bool hasBlow = blow >= 0.f && blow <= 45.f && std::isfinite(blow);
	const float blowFrac = hasBlow ? std::clamp(blow / blowFull, 0.f, 1.f) : 0.f;
	const bool urgent = !defused && hasBlow && blow <= 10.f;
	const bool showTime = Config::world_esp_bomb_timer;
	const bool canMake = !hasBlow || item.defuse_left <= blow + 0.05f;
	const ImU32 accent = defused
		? IM_COL32(90, 210, 160, 210)
		: (urgent ? IM_COL32(230, 78, 70, 230) : IM_COL32(cr, cg, cb, 210));
	const ImU32 defCol = canMake ? IM_COL32(90, 210, 160, 230) : IM_COL32(230, 78, 70, 230);

	float r = 20.f;
	if (vm.viewMatrix) {
		Vector_t a{}, b{};
		const Vector_t side{ item.position.x + 16.f, item.position.y, item.position.z };
		if (vm.WorldToScreen(item.position, a) && vm.WorldToScreen(side, b)) {
			const float px = std::hypot(a.x - b.x, a.y - b.y);
			r = std::clamp(px * 1.25f, 16.f, 28.f);
		}
	}

	const ImVec2 c(sx, sy);
	constexpr float kPi = 3.14159265f;
	const float a0 = -kPi * 0.5f;
	const float stroke = r < 18.f ? 1.35f : 1.7f;

	dl->AddCircleFilled(c, r + 2.2f, IM_COL32(0, 0, 0, 100), 40);
	dl->AddCircleFilled(c, r, IM_COL32(16, 18, 24, 188), 40);
	dl->AddCircleFilled(ImVec2(sx, sy - r * 0.38f), r * 0.72f, IM_COL32(255, 255, 255, 28), 28);
	dl->AddCircle(c, r - 0.6f, IM_COL32(255, 255, 255, 38), 40, 1.05f);
	dl->AddCircle(c, r, IM_COL32(cr, cg, cb, 95), 40, 1.15f);

	if (showTime && hasBlow && blowFrac > 0.01f) {
		const float a1 = a0 + blowFrac * kPi * 2.f;
		dl->PathArcTo(c, r + 2.6f, a0, a1, 28);
		dl->PathStroke(IM_COL32(0, 0, 0, 90), stroke + 1.1f, 0);
		dl->PathArcTo(c, r + 2.6f, a0, a1, 28);
		dl->PathStroke(accent, stroke, 0);
	}
	if (showTime && !defused && item.defusing && item.defuse_left >= 0.f) {
		const float defFull = (item.defuse_full >= 4.f && item.defuse_full <= 12.f)
			? item.defuse_full : 10.f;
		const float defFrac = std::clamp(item.defuse_left / defFull, 0.f, 1.f);
		if (defFrac > 0.01f) {
			const float a1 = a0 + defFrac * kPi * 2.f;
			dl->PathArcTo(c, (std::max)(r - 3.2f, 6.f), a0, a1, 22);
			dl->PathStroke(defCol, stroke, 0);
		}
	}

	WeaponIconDraw::EnsureReady(pDevice);
	const float iconH = r * 0.78f;
	const ImU32 iconCol = IM_COL32(248, 249, 252, 240);
	const float iconY = sy - iconH * 0.62f;
	if (WeaponIconDraw::DrawCentered(dl, sx, iconY, iconCol, "c4", iconH) <= 0.f) {
		const float gr = r * 0.34f;
		const ImVec2 gc(sx, sy - r * 0.10f);
		const ImU32 gCol = IM_COL32(248, 249, 252, 230);
		dl->AddCircleFilled(gc, gr, IM_COL32(cr, cg, cb, 48), 18);
		dl->AddCircle(gc, gr, gCol, 18, 1.35f);
		dl->AddLine(
			ImVec2(gc.x + gr * 0.22f, gc.y - gr * 0.62f),
			ImVec2(gc.x + gr * 0.78f, gc.y - gr * 1.18f),
			gCol, 1.55f);
		dl->AddCircleFilled(
			ImVec2(gc.x + gr * 0.82f, gc.y - gr * 1.22f), gr * 0.18f, gCol, 8);
	}

	char siteLine[8]{};
	if (item.bomb_site == 0)
		snprintf(siteLine, sizeof(siteLine), "A");
	else if (item.bomb_site == 1)
		snprintf(siteLine, sizeof(siteLine), "B");
	else
		siteLine[0] = 0;

	ImFont* font = ImGui::GetFont();
	if (siteLine[0] && font) {
		const float sitePx = std::clamp(r * 0.72f, 15.f, 20.f);
		const ImVec2 ssz = font->CalcTextSizeA(sitePx, FLT_MAX, 0.f, siteLine);
		const float slx = floorf(sx - ssz.x * 0.5f);
		const float sly = floorf(sy + r * 0.18f);
		dl->AddText(font, sitePx, ImVec2(slx + 1.f, sly + 1.f), IM_COL32(0, 0, 0, 130), siteLine);
		dl->AddText(font, sitePx, ImVec2(slx, sly), IM_COL32(250, 251, 253, 250), siteLine);
	}

	const char* status = "Bomb";
	if (defused)
		status = "Defused";
	else if (item.defusing)
		status = "Defusing";
	if (font) {
		const float stPx = std::clamp(r * 0.48f, 11.f, 14.f);
		const ImVec2 stsz = font->CalcTextSizeA(stPx, FLT_MAX, 0.f, status);
		const float padX = 6.5f, padY = 2.0f;
		const float stx = floorf(sx - stsz.x * 0.5f);
		const float sty = floorf(sy - r - stsz.y - 8.f);
		const ImVec2 mn(stx - padX, sty - padY);
		const ImVec2 mx(stx + stsz.x + padX, sty + stsz.y + padY);
		const float rr = (mx.y - mn.y) * 0.5f;
		dl->AddRectFilled(mn, mx, IM_COL32(12, 14, 18, 178), rr);
		dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 28), rr, 0, 1.0f);
		const ImU32 stCol = defused
			? IM_COL32(110, 220, 170, 245)
			: (item.defusing ? IM_COL32(110, 220, 170, 245) : IM_COL32(244, 245, 248, 235));
		dl->AddText(font, stPx, ImVec2(stx + 1.f, sty + 1.f), IM_COL32(0, 0, 0, 100), status);
		dl->AddText(font, stPx, ImVec2(stx, sty), stCol, status);
	}

	if (showTime && font) {
		char timeLine[16]{};
		ImU32 timeCol = IM_COL32(236, 238, 242, 235);
		if (hasBlow) {
			snprintf(timeLine, sizeof(timeLine), "%.1f", blow);
			if (defused)
				timeCol = IM_COL32(110, 220, 170, 245);
			else if (urgent)
				timeCol = IM_COL32(235, 95, 85, 245);
		}
		if (timeLine[0]) {
			const float tszPx = std::clamp(r * 0.70f, 12.f, 16.f);
			const ImVec2 tsz = font->CalcTextSizeA(tszPx, FLT_MAX, 0.f, timeLine);
			const float padX = 6.5f, padY = 2.2f;
			const float tx = floorf(sx - tsz.x * 0.5f);
			const float ty = floorf(sy + r + 6.f);
			const ImVec2 mn(tx - padX, ty - padY);
			const ImVec2 mx(tx + tsz.x + padX, ty + tsz.y + padY);
			const float rr = (mx.y - mn.y) * 0.5f;
			dl->AddRectFilled(mn, mx, IM_COL32(12, 14, 18, 178), rr);
			dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 28), rr, 0, 1.0f);
			dl->AddText(font, tszPx, ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 100), timeLine);
			dl->AddText(font, tszPx, ImVec2(tx, ty), timeCol, timeLine);
		}
	}
	(void)pulse;
}

// Compact world nade badge - stone disc, hairline ring, timer arc. No outer glow.
static void DrawWorldNadeBadge(ImDrawList* dl, float cx, float cy, const ImVec4& col4,
                               const char* weaponKey, const char* label, bool active,
                               float timeLeft, int nadeKind = 0) {
	if (!dl)
		return;
	if (!std::isfinite(cx) || !std::isfinite(cy))
		return;
	if (cx < -4000.f || cx > 8000.f || cy < -4000.f || cy > 8000.f)
		return;

	const float iconBase = 28.f;
	const float r = iconBase * 0.58f;
	const int cr = std::clamp((int)(col4.x * 255.f), 0, 255);
	const int cg = std::clamp((int)(col4.y * 255.f), 0, 255);
	const int cb = std::clamp((int)(col4.z * 255.f), 0, 255);
	const ImU32 accent = IM_COL32(cr, cg, cb, active ? 210 : 150);
	const float stroke = r < 18.f ? 1.35f : 1.7f;
	constexpr float kPi = 3.14159265f;

	constexpr float kHeFlashFull = 1.6f;
	float full = 18.f;
	if (nadeKind == WORLD_MOLOTOV) full = 7.f;
	else if (nadeKind == WORLD_DECOY) full = 15.f;
	else if (nadeKind == WORLD_SMOKE) full = 18.f;
	else if (nadeKind == WORLD_HE || nadeKind == WORLD_FLASH) full = kHeFlashFull;
	else if (weaponKey) {
		if (strstr(weaponKey, "molotov") || strstr(weaponKey, "incendiary")) full = 7.f;
		else if (strstr(weaponKey, "decoy")) full = 15.f;
		else if (strstr(weaponKey, "smoke")) full = 18.f;
		else if (strstr(weaponKey, "hegrenade") || strstr(weaponKey, "flash")) full = kHeFlashFull;
	}
	const bool hasTimer = timeLeft >= 0.05f && timeLeft < 60.f && std::isfinite(timeLeft);
	const float frac = hasTimer ? std::clamp(timeLeft / full, 0.f, 1.f) : 0.f;
	const bool urgent = hasTimer && timeLeft < 1.0f
		&& nadeKind != WORLD_SMOKE && nadeKind != WORLD_DECOY;

	ImFont* font = ImGui::GetFont();
	if (label && label[0] && font) {
		const float lpx = 13.f;
		const ImVec2 lsz = font->CalcTextSizeA(lpx, FLT_MAX, 0.f, label);
		const float padX = 5.f, padY = 1.5f;
		const float lx = floorf(cx - lsz.x * 0.5f);
		const float ly = floorf(cy - r - lsz.y - 8.f);
		const ImVec2 mn(lx - padX, ly - padY);
		const ImVec2 mx(lx + lsz.x + padX, ly + lsz.y + padY);
		const float rr = (mx.y - mn.y) * 0.5f;
		dl->AddRectFilled(mn, mx, IM_COL32(12, 14, 18, 210), rr);
		dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 36), rr, 0, 1.0f);
		const ImU32 shadow = IM_COL32(0, 0, 0, 200);
		const ImU32 labCol = IM_COL32(250, 251, 253, 255);
		dl->AddText(font, lpx, ImVec2(lx - 1.f, ly), shadow, label);
		dl->AddText(font, lpx, ImVec2(lx + 1.f, ly), shadow, label);
		dl->AddText(font, lpx, ImVec2(lx, ly - 1.f), shadow, label);
		dl->AddText(font, lpx, ImVec2(lx, ly + 1.f), shadow, label);
		dl->AddText(font, lpx, ImVec2(lx, ly), labCol, label);
	}

	const ImVec2 c(cx, cy);
	dl->AddCircleFilled(c, r + 2.2f, IM_COL32(0, 0, 0, 100), 40);
	dl->AddCircleFilled(c, r, IM_COL32(16, 18, 24, 188), 40);
	dl->AddCircleFilled(ImVec2(cx, cy - r * 0.38f), r * 0.72f, IM_COL32(255, 255, 255, 28), 28);
	dl->AddCircle(c, r - 0.6f, IM_COL32(255, 255, 255, 38), 40, 1.05f);
	dl->AddCircle(c, r, IM_COL32(cr, cg, cb, 95), 40, 1.15f);

	if (hasTimer && frac > 0.01f) {
		const float a0 = -kPi * 0.5f;
		const float a1 = a0 + frac * kPi * 2.f;
		dl->PathArcTo(c, r + 2.6f, a0, a1, 28);
		dl->PathStroke(IM_COL32(0, 0, 0, 90), stroke + 1.1f, 0);
		dl->PathArcTo(c, r + 2.6f, a0, a1, 28);
		dl->PathStroke(urgent ? IM_COL32(230, 78, 70, 230) : accent, stroke, 0);
	}

	const char* glyph = nullptr;
	if (weaponKey && weaponKey[0]) {
		// Memoize - this runs per badge per frame; the underlying map values
		// are static so the resolved pointer is stable for the session.
		static std::unordered_map<std::string, const char*> s_glyphMemo;
		auto memo = s_glyphMemo.find(weaponKey);
		if (memo == s_glyphMemo.end()) {
			auto it = weapon_icons::icon_table.find(weaponKey);
			if (it != weapon_icons::icon_table.end() && !it->second.empty())
				glyph = it->second.c_str();
			s_glyphMemo.emplace(weaponKey, glyph);
		} else {
			glyph = memo->second;
		}
	}
	const ImU32 iconCol = IM_COL32(248, 249, 252, 240);
	if (glyph && g_WeaponIconFont) {
		const float iconSz = iconBase * 0.82f;
		const ImVec2 isz = g_WeaponIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, glyph);
		const float ix = floorf(cx - isz.x * 0.5f);
		const float iy = floorf(cy - isz.y * 0.55f);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(ix + 1.f, iy + 1.f), IM_COL32(0, 0, 0, 90), glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(ix, iy), iconCol, glyph);
	} else if (weaponKey && weaponKey[0] && WeaponIconDraw::Has(weaponKey)) {
		const float iconSz = iconBase * 0.78f;
		WeaponIconDraw::DrawCentered(dl, cx, cy - iconSz * 0.08f, iconCol, weaponKey, iconSz);
	}

	if (hasTimer && font) {
		char line[16];
		std::snprintf(line, sizeof(line), "%.1f", timeLeft);
		const float tszPx = 13.f;
		const ImVec2 tsz = font->CalcTextSizeA(tszPx, FLT_MAX, 0.f, line);
		const float padX = 5.f, padY = 1.5f;
		const float tx = floorf(cx - tsz.x * 0.5f);
		const float ty = floorf(cy + r + 6.f);
		const ImVec2 mn(tx - padX, ty - padY);
		const ImVec2 mx(tx + tsz.x + padX, ty + tsz.y + padY);
		const float rr = (mx.y - mn.y) * 0.5f;
		dl->AddRectFilled(mn, mx, IM_COL32(12, 14, 18, 210), rr);
		dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 36), rr, 0, 1.0f);
		const ImU32 tcol = urgent ? IM_COL32(235, 95, 85, 255) : IM_COL32(250, 251, 253, 255);
		const ImU32 shadow = IM_COL32(0, 0, 0, 200);
		dl->AddText(font, tszPx, ImVec2(tx - 1.f, ty), shadow, line);
		dl->AddText(font, tszPx, ImVec2(tx + 1.f, ty), shadow, line);
		dl->AddText(font, tszPx, ImVec2(tx, ty - 1.f), shadow, line);
		dl->AddText(font, tszPx, ImVec2(tx, ty + 1.f), shadow, line);
		dl->AddText(font, tszPx, ImVec2(tx, ty), tcol, line);
	}
}

void Visuals::drawWorld() {
	if (cached_world.empty())
		return;
	if (!AnyWorldEspEnabled())
		return;

	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	if (!drawList)
		return;

	const float lineH = ImGui::GetFontSize();
	const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.f);

	// Local origin once per frame - was re-resolved (pawn probe chain) per
	// dropped weapon per frame when weapon distance ESP is on.
	Vector_t woLocalOrigin{};
	const bool haveLocalOrigin = Config::world_esp_weapon_distance && GetLocalOrigin(woLocalOrigin);

	for (const auto& item : cached_world) {
		if (!item.label[0])
			continue;

		const bool isNadeKind =
			item.kind == WORLD_HE || item.kind == WORLD_FLASH || item.kind == WORLD_DECOY
			|| item.kind == WORLD_SMOKE || item.kind == WORLD_MOLOTOV;

		const bool worldWants =
			(item.kind == WORLD_BOMB && Config::world_esp_bomb)
			|| (item.kind == WORLD_WEAPON && Config::world_esp_weapons)
			|| (item.kind == WORLD_SMOKE && Config::world_esp_smoke)
			|| (item.kind == WORLD_MOLOTOV && Config::world_esp_molotov)
			|| (item.kind == WORLD_HE && Config::world_esp_he)
			|| (item.kind == WORLD_FLASH && Config::world_esp_flash)
			|| (item.kind == WORLD_DECOY && Config::world_esp_decoy);
		if (!worldWants)
			continue;

		Vector_t screen{};
		if (!viewMatrix.WorldToScreen(item.position, screen))
			continue;

		ImVec4 col4 = Config::world_esp_weapon_color;
		switch (item.kind) {
		case WORLD_BOMB:    col4 = Config::world_esp_bomb_color; break;
		case WORLD_SMOKE:   col4 = Config::world_esp_smoke_color; break;
		case WORLD_MOLOTOV: col4 = Config::world_esp_molotov_color; break;
		case WORLD_HE:      col4 = Config::world_esp_he_color; break;
		case WORLD_FLASH:   col4 = Config::world_esp_flash_color; break;
		case WORLD_DECOY:   col4 = Config::world_esp_decoy_color; break;
		default: break;
		}
		const ImU32 col = ImGui::ColorConvertFloat4ToU32(col4);

		// Nade projectiles / effects - single badge (name top / timer bottom)
		if (item.use_badge || isNadeKind) {
			if (item.timer >= 0.f && item.timer <= 0.05f)
				continue;
			if (!std::isfinite(item.position.x) || !std::isfinite(item.position.y)
				|| !std::isfinite(item.position.z))
				continue;

			DrawWorldNadeBadge(drawList, screen.x, screen.y - 18.f, col4,
				item.weapon_key, item.label, item.effect_active, item.timer, item.kind);
			if (item.effect_active && item.radius > 1.f) {
				float alphaScale = 0.85f + pulse * 0.15f;
				if (item.timer >= 0.f && item.timer < 0.3f)
					alphaScale *= item.timer / 0.3f;

				if (item.kind == WORLD_SMOKE) {
					const int cr = (int)(col4.x * 255), cg = (int)(col4.y * 255), cb = (int)(col4.z * 255);
					const ImU32 ringSoft = IM_COL32(cr, cg, cb,
						static_cast<int>((26 + pulse * 16) * alphaScale));
					const ImU32 ring = IM_COL32(cr, cg, cb,
						static_cast<int>((88 + pulse * 32) * alphaScale));
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 149.f, ringSoft, 32);
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 149.f, ring, 32);
				} else if (item.kind == WORLD_MOLOTOV) {
					DrawInfernoRadius(drawList, viewMatrix, item, col4, alphaScale);
				} else {
					const ImU32 ring = IM_COL32(
						(int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255),
						static_cast<int>((70 + pulse * 50) * alphaScale));
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, item.radius, ring, 24);
				}
			}
			continue;
		}

		if (item.kind == WORLD_BOMB) {
			DrawBombTimerFx(drawList, viewMatrix, item, col4, screen.x, screen.y - 8.f, pulse);
			continue;
		}

		const float r = 2.8f;
		drawList->AddCircleFilled(ImVec2(screen.x, screen.y), r + 1.f, IM_COL32(0, 0, 0, 200), 12);
		drawList->AddCircleFilled(ImVec2(screen.x, screen.y), r, col, 12);

		float textY = floorf(screen.y + r + 3.f);
		const bool isWeapon = (item.kind == WORLD_WEAPON);

		// Dropped weapons: icon + name + distance (clean stack, always readable)
		bool drewIcon = false;
		if (isWeapon) {
			if (Config::world_esp_weapon_icon && item.weapon_key[0]) {
				const float ih = DrawWeaponIconCentered(drawList, screen.x, textY, col, item.weapon_key, item.icon_glyph);
				if (ih > 0.f) {
					textY += ih;
					drewIcon = true;
				}
			}
			// Name always drawn under the icon (menu toggle only controls the icon)
			DrawCenteredText(drawList, screen.x, textY, col, item.label);
			textY += lineH;
		} else {
			if (item.weapon_key[0]) {
				const float ih = DrawWeaponIconCentered(drawList, screen.x, textY, col, item.weapon_key, item.icon_glyph);
				if (ih > 0.f) {
					textY += ih;
					drewIcon = true;
				}
			}
			if (!drewIcon) {
				DrawCenteredText(drawList, screen.x, textY, col, item.label);
				textY += lineH;
			}
		}

		if (isWeapon && Config::world_esp_weapon_distance && haveLocalOrigin) {
			const Vector_t& localOrigin = woLocalOrigin;
			{
				const float dx = item.position.x - localOrigin.x;
				const float dy = item.position.y - localOrigin.y;
				const float dz = item.position.z - localOrigin.z;
				const float units = std::sqrt(dx * dx + dy * dy + dz * dz);
				const int meters = static_cast<int>(units * 0.0254f + 0.5f);
				char distText[24];
				snprintf(distText, sizeof(distText), "%dm", meters);
				DrawCenteredText(drawList, screen.x, textY,
					ImGui::ColorConvertFloat4ToU32(Config::world_esp_weapon_distance_color), distText);
			}
		}
	}
}

void Esp::ResetWorldFxTimers() {
	s_worldFxN = 0;
	++s_worldRoundEpoch;
	ResetBombWallClock();
}

void Esp::InvalidateCaches() {
	cached_players.clear();
	cached_world.clear();
	cached_local.reset();
	cached_local.lastTeam = 0;
	g_plantedBomb = {};
	EspClearPlayersPublished();
	ResetWorldFxTimers();
	s_visStickyN = 0;
	// Drop stale W2S retry budgets (handles from prev map are meaningless)
	g_w2sBudget.clear();
	// Drop bar animations - stale cur ratios would lerp from last map's values
	g_barAnim.clear();
}

// Autowall crosshair - center overlay over the game reticle. Green when the
// surface under the crosshair is penetrable (or LOS clear), red when blocked.
// Two styles: dot (0) and box (1). Probe runs on the game thread
// (AutoWall::TickXhairCache via FSN) - Present only draws the cached result.
// Present-thread native traces are the multi-queue insecure surface.
static void DrawAutoWallCrosshair() {
	if (!Config::autowall_xhair)
		return;
	AutoWall::XhairPen st = AutoWall::GetCachedXhairPen();
	if (st == AutoWall::XhairPen::NoData)
		return;

	const bool canPen = (st == AutoWall::XhairPen::Penetrable
		|| st == AutoWall::XhairPen::Clear);
	ImVec4 cv = canPen ? Config::autowall_xhair_can : Config::autowall_xhair_cant;
	const ImU32 col = ImGui::ColorConvertFloat4ToU32(cv);
	const int oa = static_cast<int>(200.f * std::clamp(cv.w, 0.f, 1.f));
	const ImU32 outline = IM_COL32(0, 0, 0, oa);

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;
	const ImGuiIO& io = ImGui::GetIO();
	// Pixel-snapped center - ImGui rounds float centers to the nearest pixel,
	// smearing single-pixel primitives when DisplaySize is even (1920/2560/...).
	// Snap so the game crosshair (drawn on integer pixels) and our overlay
	// share the exact same pixel column/row.
	const ImVec2 c(std::floor(io.DisplaySize.x * 0.5f),
		std::floor(io.DisplaySize.y * 0.5f));
	const float sz = std::clamp(Config::autowall_xhair_size, 2.f, 100.f);

	if (Config::autowall_xhair_style == 1) {
		// Box: clean hollow square with a dark outline for contrast.
		// Uses half-pixel snap so the outline sits cleanly on a pixel grid.
		const ImVec2 p0(c.x - sz + 0.5f, c.y - sz + 0.5f);
		const ImVec2 p1(c.x + sz + 0.5f, c.y + sz + 0.5f);
		dl->AddRect(ImVec2(p0.x - 1.f, p0.y - 1.f), ImVec2(p1.x + 1.f, p1.y + 1.f),
			outline, 2.5f, 0, 3.f);
		dl->AddRect(p0, p1, col, 2.5f, 0, 2.f);
	} else {
		// Dot: hollow ring around the game crosshair so the real reticle
		// stays visible in the middle. Older filled-dot covered the game
		// crosshair completely and shifted the perceived aim point.
		const float rad = sz * 0.5f;
		const ImVec2 cc(c.x + 0.5f, c.y + 0.5f);
		dl->AddCircle(cc, rad + 1.f, outline, 24, 2.5f);
		dl->AddCircle(cc, rad, col, 24, 1.6f);
	}
}

void Visuals::esp() {
	if (H::SessionMapLeaving() || H::SessionPostMatch())
		return;
	if (!ensureViewMatrix())
		return;

	if (Config::grenade_helper)
		GrenadeHelper::Update();

	// Draw from last cache even if local pawn is mid-recycle (TDM death).
	__try { drawPlayers(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.drawPlayers"); }
	__try { drawWorld(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.drawWorld"); }
	// Weather::Update lives on hkPresent (main.cpp). Calling it again here
	// mid-frame raced particle create/destroy on round/map transitions.
	__try { Backtrack::DrawGhosts(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.backtrackGhosts"); }
	__try { GrenadeHelper::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.nadeLineup"); }
	__try { NadePred::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.nadePred"); }
	__try { Hitmarker::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.hitmarker"); }
	__try { BulletFx::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.bulletFx"); }
	__try { DrawAutoWallCrosshair(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.awXhair"); }
	__try { HitLog::Draw(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("esp.hitLog"); }
}

