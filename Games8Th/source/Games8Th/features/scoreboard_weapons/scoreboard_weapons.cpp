#include "scoreboard_weapons.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../panorama/panorama.h"

#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ScoreboardWeapons {
namespace {

static constexpr const char* kSetupScript = R"PANORAMA(
(function () {
	if (typeof SClient !== "undefined") { SClient = undefined; }
	SClient = (function () {
		var handlers = {};
		return {
			register_handler: function (type, callback) { handlers[type] = callback; },
			receive: function (msg) {
				if (msg && handlers[msg.type]) handlers[msg.type](msg);
			}
		};
	})();
	SWeaponManager = (function () {
		function isValid(panel) { return panel && panel.IsValid(); }
		function getScoreboard() {
			var root = $.GetContextPanel();
			if (!isValid(root)) return null;
			var scoreboard = root.id === "Scoreboard" ? root : root.FindChildTraverse("Scoreboard");
			return isValid(scoreboard) ? scoreboard : null;
		}
		function getRow(sb, xuid, account_id) {
			var keys = [xuid, account_id];
			var prefixes = ["player-", "id-", "id-player-", "player_"];
			for (var i = 0; i < keys.length; ++i) {
				if (!keys[i]) continue;
				for (var j = 0; j < prefixes.length; ++j) {
					var row = sb.FindChildTraverse(prefixes[j] + keys[i]);
					if (isValid(row)) return row;
				}
			}
			return null;
		}
		function getSize(w) {
			var t = w.type, p = w.path;
			if (t === 1) {
				if (p.indexOf("usp_silencer") !== -1) return "42px";
				if (p.indexOf("deagle") !== -1) return "36px";
				if (p.indexOf("revolver") !== -1) return "32px";
				if (p.indexOf("tec9") !== -1) return "36px";
				if (p.indexOf("elite") !== -1) return "32px";
				return "30px";
			}
			if (t === 2 || t === 3 || t === 4 || t === 5 || t === 6)
				return p.indexOf("mac10") !== -1 ? "30px" : "52px";
			if (t === 9) {
				if (p.indexOf("incgrenade") !== -1 || p.indexOf("smokegrenade") !== -1) return "14px";
				if (p.indexOf("molotov") !== -1 || p.indexOf("flashbang") !== -1) return "16px";
				return "14px";
			}
			if (t === 7) return "16px";
			if (t === 8) return "22px";
			if (t === 11) return p.indexOf("healthshot") !== -1 ? "18px" : "14px";
			if (t === 0) return "46px";
			return "16px";
		}
		function updateWeaponIcon(parent, w, active_path) {
			var id = "wep_" + w.path.replace(/[^a-zA-Z0-9]/g, "_");
			var img = parent.FindChildTraverse(id);
			if (!isValid(img)) img = $.CreatePanel("Image", parent, id);
			img.style.verticalAlign = "center";
			img.style.margin = "0px 1px";
			img.scaling = "stretch-aspect-preserve";
			var finalPath = w.path;
			if (finalPath.indexOf("file://") !== 0) {
				if (finalPath.indexOf("icons/equipment") === -1)
					finalPath = "icons/equipment/" + finalPath;
				if (finalPath.indexOf(".svg") === -1 && finalPath.indexOf(".vsvg") === -1)
					finalPath += ".svg";
				finalPath = "file://{images}/" + finalPath;
			}
			img.SetImage(finalPath);
			img.style.height = "16px";
			img.style.width = getSize(w);
			img.style.opacity = (w.path === active_path) ? "1.0" : "0.35";
			img.style.visibility = "visible";
		}
		function updateNow(xuid, account_id, weapons, active_path) {
			var sb = getScoreboard();
			if (!sb) return;
			var row = getRow(sb, xuid, account_id);
			if (!row) return;
			var nameIcons = row.FindChildTraverse("id-sb-name__nameicons");
			if (!isValid(nameIcons)) return;
			var containerId = "custom-weapons-container-" + xuid;
			var container = nameIcons.FindChildTraverse(containerId);
			if (!weapons || weapons.length === 0) {
				if (isValid(container)) container.style.visibility = "collapse";
				return;
			}
			if (!isValid(container)) {
				container = $.CreatePanel("Panel", nameIcons, containerId);
				container.AddClass("custom-weapons-container");
				container.style.flowChildren = "right";
				container.style.height = "20px";
				container.style.width = "fit-children";
				container.style.padding = "1px 4px";
				container.style.verticalAlign = "center";
				container.style.marginLeft = "3px";
				container.style.backgroundColor = "rgba(0,0,0,0.35)";
				container.style.borderRadius = "3px";
				container.style.border = "1px solid rgba(255,255,255,0.18)";
			}
			var children = container.Children();
			for (var i = 0; i < children.length; ++i) {
				if (isValid(children[i])) children[i].style.visibility = "collapse";
			}
			var primary = [], pistols = [], equip = [];
			weapons.forEach(function (w) {
				if (w.type === 2 || w.type === 3 || w.type === 4 || w.type === 5 || w.type === 6)
					primary.push(w);
				else if (w.type === 1) pistols.push(w);
				else equip.push(w);
			});
			primary.concat(pistols).concat(equip).forEach(function (w) {
				updateWeaponIcon(container, w, active_path);
			});
			container.style.visibility = "visible";
		}
		return {
			update: function (xuid, account_id, weapons, active_path) {
				updateNow(xuid, account_id, weapons, active_path);
			},
			clear: function () {
				var sb = getScoreboard();
				if (!sb) return;
				var containers = sb.FindChildrenWithClassTraverse("custom-weapons-container");
				for (var i = 0; i < containers.length; ++i) {
					if (isValid(containers[i])) containers[i].style.visibility = "collapse";
				}
			}
		};
	})();
	SClient.register_handler("updateWeapons", function (msg) {
		if (msg && msg.content)
			SWeaponManager.update(msg.content.xuid, msg.content.account_id, msg.content.weapons, msg.content.active_path);
	});
	SClient.register_handler("clearWeapons", function (msg) {
		if (msg && msg.content)
			SWeaponManager.update(msg.content.xuid, msg.content.account_id, [], "");
	});
})();
)PANORAMA";

constexpr std::uint64_t kSteamIdBase = 76561197960265728ull;

struct WeaponEntry {
	std::string name;
	int type = 0;
	bool operator==(const WeaponEntry& o) const {
		return type == o.type && name == o.name;
	}
};

struct PlayerWeaponState {
	std::vector<WeaponEntry> weapons;
	std::string activeName;
	bool operator==(const PlayerWeaponState& o) const {
		return activeName == o.activeName && weapons == o.weapons;
	}
};

bool g_scriptInjected = false;
bool g_scoreboardOpen = false;
int g_throttle = 0;
int g_initThrottle = 0;
std::unordered_map<std::uint64_t, PlayerWeaponState> g_cache;

// SEH helpers - POD only (C2712)
__declspec(noinline) static std::uint64_t SehSteamId(CCSPlayerController* ctrl)
{
	std::uint64_t sid = 0;
	if (!ctrl) return 0;
	__try { sid = ctrl->m_steamID(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { sid = 0; }
	return sid;
}

__declspec(noinline) static int SehHealth(C_CSPlayerPawn* pawn)
{
	int hp = 0;
	if (!pawn) return 0;
	__try { hp = pawn->m_iHealth(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hp = 0; }
	return hp;
}

__declspec(noinline) static C_CSPlayerPawn* SehPlayerPawn(CCSPlayerController* ctrl)
{
	if (!ctrl || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	C_CSPlayerPawn* pawn = nullptr;
	__try {
		const CBaseHandle h = ctrl->m_hPlayerPawn();
		if (h.valid())
			pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(h);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		pawn = nullptr;
	}
	return pawn;
}

__declspec(noinline) static C_CSWeaponBase* SehActiveWeapon(C_CSPlayerPawn* pawn)
{
	if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	C_CSWeaponBase* wep = nullptr;
	__try {
		if (auto* ws = pawn->m_pWeaponServices()) {
			const CBaseHandle ah = ws->m_hActiveWeapon();
			if (ah.valid())
				wep = I::GameEntity->Instance->Get<C_CSWeaponBase>(ah);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		wep = nullptr;
	}
	return wep;
}

__declspec(noinline) static bool SehOwnerIsPawn(C_CSWeaponBase* wep, C_CSPlayerPawn* pawn)
{
	if (!wep || !pawn || !I::GameEntity || !I::GameEntity->Instance)
		return false;
	bool ok = false;
	__try {
		const CBaseHandle owner = reinterpret_cast<C_BaseEntity*>(wep)->m_hOwnerEntity();
		if (!owner.valid())
			return false;
		void* ownerEnt = I::GameEntity->Instance->Get(owner.index());
		ok = (ownerEnt == pawn);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	return ok;
}

// POD weapon read - no std::string in SEH frame
struct WeaponRaw {
	char name[48];
	int type;
	bool ok;
};

__declspec(noinline) static WeaponRaw SehReadWeapon(C_CSWeaponBase* wep)
{
	WeaponRaw r{};
	r.ok = false;
	if (!wep || !Mem::ValidEntity(wep))
		return r;
	__try {
		CCSWeaponBaseVData* vd = wep->Data();
		if (!vd || !Mem::Valid(vd, 0x20)) {
			// mercey fallback: [wep + m_nSubclassID + 8] -> CCSWeaponBaseVData*.
			// GetWeaponData field can be unset on freshly-dropped weapons.
			vd = nullptr;
			const auto subOff = SchemaFinder::Get(
				hash_32_fnv1a_const("C_BaseEntity->m_nSubclassID"));
			if (subOff) {
				void* viaSub = *reinterpret_cast<void**>(
					reinterpret_cast<std::uintptr_t>(wep) + subOff + 8);
				if (viaSub && Mem::IsUserPtr(viaSub) && Mem::Valid(viaSub, 0x20))
					vd = reinterpret_cast<CCSWeaponBaseVData*>(viaSub);
			}
		}
		if (!vd || !Mem::Valid(vd, 0x20))
			return r;
		const char* raw = vd->m_szName();
		if (!raw || !raw[0] || !Mem::IsReadable(raw, 8))
			return r;
		if (std::strncmp(raw, "weapon_", 7) != 0)
			return r;
		for (int i = 0; i < 47; ++i) {
			const char c = raw[7 + i];
			if (!c) break;
			r.name[i] = c;
		}
		if (!r.name[0])
			return r;
		r.type = vd->m_WeaponType();
		r.ok = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		r.ok = false;
	}
	return r;
}

// mercey item classification - exact schema class names, one list, no
// substring heuristics (projectiles / inferno must NOT match).
constexpr const char* kItemClasses[] = {
	"C_AK47", "C_WeaponM4A1", "C_WeaponM4A1Silencer", "C_WeaponAWP",
	"C_WeaponAug", "C_WeaponFamas", "C_WeaponGalilAR", "C_WeaponSG556",
	"C_WeaponG3SG1", "C_WeaponSCAR20", "C_WeaponSSG08", "C_WeaponMAC10",
	"C_WeaponMP5SD", "C_WeaponMP7", "C_WeaponMP9", "C_WeaponBizon",
	"C_WeaponP90", "C_WeaponUMP45", "C_WeaponNOVA", "C_WeaponSawedoff",
	"C_WeaponXM1014", "C_WeaponMag7", "C_WeaponM249", "C_WeaponNegev",
	"C_DEagle", "C_WeaponElite", "C_WeaponFiveSeven", "C_WeaponGlock",
	"C_WeaponHKP2000", "C_WeaponUSPSilencer", "C_WeaponP250",
	"C_WeaponCZ75a", "C_WeaponTec9", "C_WeaponRevolver", "C_WeaponTaser",
	"C_Knife", "C_C4", "C_Item_Healthshot", "C_HEGrenade", "C_Flashbang",
	"C_SmokeGrenade", "C_MolotovGrenade", "C_IncendiaryGrenade",
	"C_DecoyGrenade",
};

__declspec(noinline) static bool SehIsItemEntity(CEntityInstance* ent)
{
	if (!ent || !Mem::ValidEntity(ent))
		return false;
	char n[96]{};
	bool ok = false;
	__try {
		if (!Mem::SchemaClassName(ent, n, sizeof(n)))
			return false;
		for (const char* cls : kItemClasses) {
			if (std::strcmp(n, cls) == 0) {
				ok = true;
				break;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	return ok;
}

bool RunHudScript(const char* js)
{
	if (!js || !js[0])
		return false;

	// Use the HUD global first, matching mercey's script-panel path exactly. The
	// FindPanel fallbacks cover the short interval before the HUD global exists.
	if (Panorama::RunScriptHud(js))
		return true;

	static constexpr const char* kPanels[] = {
		"CSGOHud", "CSGOMainMenu", "CSGOPopupManager", "PopupManager"
	};
	for (const char* id : kPanels) {
		if (void* panel = Panorama::FindPanel(id)) {
			if (Panorama::RunScript(panel, js))
				return true;
		}
	}
	return false;
}

// mercey invocation path: PanoramaUIEngine001 interface -> UIEngine (vt 13) ->
// CUIEngine::RunScript (vt 77): (panel, script, origin="", flags=1). The
// pattern-resolved RunScript above can drift across game updates; the vfunc
// slots are stable. Validated per call like mercey - fn within panorama
// image bounds, panel vtable mapped - before the engine call.
__declspec(noinline) static bool VfuncRunScript(void* panel, const char* js)
{
	if (!panel || !js || !js[0])
		return false;

	HMODULE panMod = GetModuleHandleA("panorama.dll");
	if (!panMod)
		return false;

	bool ok = false;
	__try {
		const auto panBegin = reinterpret_cast<std::uintptr_t>(panMod);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(panMod);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(panBegin + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		const auto panEnd = panBegin + nt->OptionalHeader.SizeOfImage;

		using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
		auto ci = reinterpret_cast<CreateInterfaceFn>(
			GetProcAddress(panMod, "CreateInterface"));
		if (!ci)
			return false;
		void* engIface = ci("PanoramaUIEngine001", nullptr);
		if (!engIface)
			return false;

		void** vtEngineIface = *reinterpret_cast<void***>(engIface);
		if (!vtEngineIface || !vtEngineIface[13])
			return false;
		auto getUiEngine = reinterpret_cast<void* (__fastcall*)(void*)>(vtEngineIface[13]);
		void* uiEngine = getUiEngine(engIface);
		if (!uiEngine)
			return false;

		void** vtUiEngine = *reinterpret_cast<void***>(uiEngine);
		if (!vtUiEngine)
			return false;
		auto runScript = reinterpret_cast<__int64 (__fastcall*)(
			void*, void*, const char*, const char*, __int64)>(vtUiEngine[77]);
		const auto fnAddr = reinterpret_cast<std::uintptr_t>(runScript);
		if (fnAddr < panBegin || fnAddr >= panEnd)
			return false;

		// Panel must still be a live object (vtable readable).
		if (!Mem::IsReadable(*reinterpret_cast<void**>(panel), sizeof(void*)))
			return false;

		runScript(uiEngine, panel, js, "", static_cast<__int64>(1));
		ok = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	return ok;
}

// Primary: mercey vfunc path on the HUD-global panel. Fallback: pattern-based
// RunScript + FindPanel list (covers panorama.dll interface hiccups).
bool SendScript(const char* js)
{
	if (!js || !js[0])
		return false;
	if (void* hud = Panorama::HudPanel()) {
		if (VfuncRunScript(hud, js))
			return true;
	}
	return RunHudScript(js);
}

void TryInitialize()
{
	if (g_scriptInjected)
		return;
	if (SendScript(kSetupScript)) {
		g_scriptInjected = true;
		Con::Ok("ScoreboardWeapons: HUD script injected");
	}
}

void ClearAll()
{
	if (!g_scriptInjected)
		return;
	(void)SendScript("if(typeof(SWeaponManager)!=='undefined'){SWeaponManager.clear();}");
	g_cache.clear();
}

bool SendClear(std::uint64_t steamId)
{
	const std::uint64_t accountId = steamId >= kSteamIdBase ? steamId - kSteamIdBase : steamId;
	char buf[384]{};
	std::snprintf(buf, sizeof(buf),
		R"(if(typeof(SClient)!=='undefined'){SClient.receive({type:"clearWeapons",content:{xuid:"%llu",account_id:"%llu"}});})",
		static_cast<unsigned long long>(steamId),
		static_cast<unsigned long long>(accountId));
	return SendScript(buf);
}

void SendPlayerWeapons(CCSPlayerController* ctrl, C_CSPlayerPawn* pawn,
	const std::vector<C_CSWeaponBase*>& weapons)
{
	if (!ctrl)
		return;
	const std::uint64_t steamId = SehSteamId(ctrl);
	if (steamId < kSteamIdBase)
		return;

	PlayerWeaponState state{};
	const int hp = SehHealth(pawn);

	if (pawn && hp > 0) {
		if (C_CSWeaponBase* active = SehActiveWeapon(pawn)) {
			const WeaponRaw ar = SehReadWeapon(active);
			if (ar.ok)
				state.activeName = ar.name;
		}
		for (C_CSWeaponBase* wep : weapons) {
			if (!wep || !SehOwnerIsPawn(wep, pawn))
				continue;
			const WeaponRaw wr = SehReadWeapon(wep);
			if (!wr.ok)
				continue;
			WeaponEntry e{};
			e.name = wr.name;
			e.type = wr.type;
			state.weapons.push_back(std::move(e));
		}
	}

	auto it = g_cache.find(steamId);
	if (it != g_cache.end() && it->second == state)
		return;

	if (state.weapons.empty()) {
		if (SendClear(steamId))
			g_cache[steamId] = state;
		return;
	}

	auto sortKey = [](int type) -> int {
		if (type >= 2 && type <= 6) return 0;
		if (type == 1) return 1;
		return 2;
	};
	std::stable_sort(state.weapons.begin(), state.weapons.end(),
		[&](const WeaponEntry& a, const WeaponEntry& b) {
			return sortKey(a.type) < sortKey(b.type);
		});

	std::string weaponsJson = "[";
	for (size_t i = 0; i < state.weapons.size(); ++i) {
		char item[96]{};
		std::snprintf(item, sizeof(item), R"({path:"%s",type:%d})",
			state.weapons[i].name.c_str(), state.weapons[i].type);
		weaponsJson += item;
		if (i + 1 < state.weapons.size())
			weaponsJson += ",";
	}
	weaponsJson += "]";

	const std::uint64_t accountId = steamId - kSteamIdBase;
	char script[2048]{};
	std::snprintf(script, sizeof(script),
		R"(if(typeof(SClient)!=='undefined'){SClient.receive({type:"updateWeapons",content:{xuid:"%llu",account_id:"%llu",weapons:%s,active_path:"%s"}});})",
		static_cast<unsigned long long>(steamId),
		static_cast<unsigned long long>(accountId),
		weaponsJson.c_str(),
		state.activeName.c_str());

	if (SendScript(script))
		g_cache[steamId] = std::move(state);
}} // namespace

void OnLevelChange()
{
	// HUD is torn down on leave - RunHudScript here is the "left the game" crash.
	g_scriptInjected = false;
	g_scoreboardOpen = false;
	g_throttle = 0;
	g_initThrottle = 0;
	g_cache.clear();
}

void OnFrameStageNotify()
{
	if (H::SessionMapLeaving() || !H::SessionLive())
		return;

	if (!Config::scoreboard_weapons) {
		if (g_scriptInjected) {
			ClearAll();
			g_scriptInjected = false;
		}
		g_scoreboardOpen = false;
		g_cache.clear();
		return;
	}

	if (!I::GameEntity || !I::GameEntity->Instance)
		return;
	if (!H::SafeLocalPlayer())
		return;

	const bool tab = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
	if (!tab) {
		if (g_scoreboardOpen && g_scriptInjected)
			ClearAll();
		g_scoreboardOpen = false;
		return;
	}

	if (!g_scoreboardOpen) {
		g_scoreboardOpen = true;
		g_cache.clear();
		g_throttle = 0;
		TryInitialize();
	}

	if (!g_scriptInjected) {
		++g_initThrottle;
		if (g_initThrottle % 30 == 0)
			TryInitialize();
		if (!g_scriptInjected)
			return;
	}

	++g_throttle;
	if (g_throttle % 8 != 0)
		return;
	if (g_throttle % 64 == 0)
		g_cache.clear();

	std::vector<C_CSWeaponBase*> weaponEnts;
	weaponEnts.reserve(128);

	const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	const int cap = (nMax > 0 && nMax < 8192) ? nMax : 2048;

	struct PlayerSlot {
		CCSPlayerController* ctrl = nullptr;
		C_CSPlayerPawn* pawn = nullptr;
	};
	std::vector<PlayerSlot> players;
	players.reserve(64);

	for (int i = 1; i <= cap; ++i) {
		void* raw = I::GameEntity->Instance->Get(i);
		if (!raw || !Mem::ValidEntity(raw))
			continue;
		auto* ent = reinterpret_cast<CEntityInstance*>(raw);

		if (i <= 64) {
			auto* ctrl = reinterpret_cast<CCSPlayerController*>(raw);
			const std::uint64_t sid = SehSteamId(ctrl);
			if (sid >= kSteamIdBase)
				players.push_back({ ctrl, SehPlayerPawn(ctrl) });
		}

		if (SehIsItemEntity(ent))
			weaponEnts.push_back(reinterpret_cast<C_CSWeaponBase*>(raw));
	}

	for (const auto& p : players)
		SendPlayerWeapons(p.ctrl, p.pawn, weaponEnts);
}

} // namespace ScoreboardWeapons

