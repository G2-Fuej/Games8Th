#include "sdk_prio_a.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"

#include "../world/world.h"
#include "../hitmarker/hitmarker.h"
#include "../bullet_impact/bullet_impact.h"
#include "../hitsound/hitsound.h"
#include "../grenade_helper/grenade_helper.h"
#include "../hitlog/hitlog.h"
#include "../notify/notify.h"
#include "../sound_esp/sound_esp.h"
#include "../visuals/visuals.h"
#include "../w2s/w2s.h"
#include "../engine2/engine2.h"
#include "../world/weather.h"
#include "../widgets/steam_avatar.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace SdkPrioA {

enum class Status : std::uint8_t {
	NotStarted = 0,
	Resolved,
	Hooked,
	AlreadyHad,
	Blocked,
	Failed,
};

struct Entry {
	const char* name = "";
	Status status = Status::NotStarted;
	void* addr = nullptr;
	const char* note = "";
};

namespace {

constexpr const char* kPatOnAdd =
	"48 89 74 24 10 57 48 83 EC 20 41 B9 FF 7F 00 00 41 8B C0 41 23 C1 48 8B F2 41 83 F8 FF 48 8B F9 44 0F 45 C8 41 81 F9 00 40 00 00 73 0D FF 81 90";
constexpr const char* kPatOnRemove =
	"48 89 74 24 10 57 48 83 EC 20 41 B9 FF 7F 00 00 41 8B C0 41 23 C1 48 8B F2 41 83 F8 FF 48 8B F9 44 0F 45 C8 41 81 F9 00 40 00 00 73 08 FF 89 90";
// IDA CSource2Client LevelShutdown @ 0x180B1D530 (cdll_client_int.cpp ~2736).
constexpr const char* kPatLevelShutdown =
	"48 89 5C 24 ? 55 56 41 56 48 83 EC 20 48 8B 0D ? ? ? ? 48 8D 54 24";
// IDA 0x1807CDDB0 C_CSWeaponBase::GetEconWpnData - unique through second call + null check
constexpr const char* kPatGetEconWpnData =
	"40 53 48 83 EC 40 48 8B D9 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 85 C0 75";
constexpr const char* kPatGetEconWpnDataLoose =
	"40 53 48 83 EC 40 48 8B D9 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 85 C0";
constexpr const char* kPatPGameRules =
	"48 8B 1D ? ? ? ? 48 8D 54 24 ? 0F 28 D0 48 8D 4C 24";
constexpr const char* kPatIsOverwatch =
	"48 83 EC 28 E8 ? ? ? ? 0F B6 40 72 48 83 C4";
constexpr const char* kPatIsDemoOrHltv =
	"48 83 EC 28 48 8B 0D ? ? ? ? 48 8B 01 FF 90 50 01 00 00 84 C0 75 0D";
constexpr const char* kPatComputeHitboxBox =
	"48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 20 80 FF FF";
constexpr const char* kPatGetAbsOrigin =
	"40 53 48 83 EC 20 48 8B 99 30 03 00 00 90 80 BB 10 01 00 00 00 74 08 48 8B CB E8";

Entry g_entries[] = {
	{ "TraceToExit", Status::NotStarted, nullptr, "" },
	{ "OnAddEntity", Status::NotStarted, nullptr, "" },
	{ "OnRemoveEntity", Status::NotStarted, nullptr, "" },
	{ "LevelShutdown", Status::NotStarted, nullptr, "" },
	{ "C_CSWeaponBase_GetEconWpnData", Status::NotStarted, nullptr, "" },
	{ "GetMapName", Status::NotStarted, nullptr, "" },
	{ "GetMapBspName", Status::NotStarted, nullptr, "" },
	{ "pGameRules", Status::NotStarted, nullptr, "" },
	{ "IsOverwatch", Status::NotStarted, nullptr, "" },
	{ "IsDemoOrHltv", Status::NotStarted, nullptr, "" },
	{ "C_BaseEntity_ComputeHitboxSurroundingBox", Status::NotStarted, nullptr, "" },
	{ "GetAbsOrigin", Status::NotStarted, nullptr, "" },
};

constexpr int kEntryCount = static_cast<int>(sizeof(g_entries) / sizeof(g_entries[0]));

Entry* FindEntry(const char* name) {
	for (int i = 0; i < kEntryCount; ++i) {
		if (std::strcmp(g_entries[i].name, name) == 0)
			return &g_entries[i];
	}
	return nullptr;
}

void SetEntry(const char* name, Status st, void* addr, const char* note) {
	if (Entry* e = FindEntry(name)) {
		e->status = st;
		e->addr = addr;
		e->note = note ? note : "";
	}
}

std::uint8_t* ScanClient(const char* pat) {
	auto* p = M::FindPattern("client.dll", pat);
	if (!p)
		p = M::FindPattern("client", pat);
	return p;
}

std::uint8_t* ScanClientEither(const char* strict, const char* loose) {
	auto* p = ScanClient(strict);
	if (!p && loose)
		p = ScanClient(loose);
	return p;
}

using FnGetEconWpn = void*(__fastcall*)(void* weapon);
using FnGetAbs = Vector_t*(__fastcall*)(void* entity);
using FnComputeBox = char(__fastcall*)(void* entity, Vector_t* mins, Vector_t* maxs);
using FnBool = bool(__fastcall*)();

FnGetEconWpn g_getEconWpn = nullptr;
FnGetAbs g_getAbsOrigin = nullptr;
FnComputeBox g_computeBox = nullptr;
FnBool g_isOverwatch = nullptr;
FnBool g_isDemoOrHltv = nullptr;
void** g_ppGameRules = nullptr;
void* g_onAddAddr = nullptr;
void* g_onRemoveAddr = nullptr;
void* g_levelShutdownAddr = nullptr;

std::uint32_t g_entityGen = 1;
std::uint32_t g_mapGen = 1;
bool g_ready = false;
std::atomic<bool> g_renderWipePending{ false };

std::mutex g_ctrlMutex;
int g_ctrlIdx[kMaxTrackedControllers]{};
int g_ctrlCount = 0;

std::mutex g_worldMutex;
int g_worldIdx[kMaxTrackedWorld]{};
int g_worldCount = 0;

static int HandleToIndex(int handleBits) {
	const int idx = handleBits & 0x7FFF;
	if (idx <= 0 || idx > 0x7FFE)
		return -1;
	return idx;
}

static bool NameLooksController(const char* name)
{
	if (!name || !name[0])
		return false;
	if (std::strcmp(name, "cs_player_controller") == 0)
		return true;
	if (std::strstr(name, "player_controller") != nullptr)
		return true;
	return std::strcmp(name, "CCSPlayerController") == 0;
}

static bool DesignerIsController(void* entity) {
	if (!entity || !Mem::ValidEntity(entity))
		return false;
	char buf[64]{};
	if (Mem::DesignerName(entity, buf, sizeof(buf)) && NameLooksController(buf))
		return true;
	return Mem::SchemaClassName(entity, buf, sizeof(buf)) && NameLooksController(buf);
}

static void CtrlAdd(int idx) {
	if (idx <= 0)
		return;
	std::lock_guard<std::mutex> lock(g_ctrlMutex);
	for (int i = 0; i < g_ctrlCount; ++i) {
		if (g_ctrlIdx[i] == idx)
			return;
	}
	if (g_ctrlCount >= kMaxTrackedControllers)
		return;
	g_ctrlIdx[g_ctrlCount++] = idx;
}

static void CtrlRemove(int idx) {
	if (idx <= 0)
		return;
	std::lock_guard<std::mutex> lock(g_ctrlMutex);
	for (int i = 0; i < g_ctrlCount; ++i) {
		if (g_ctrlIdx[i] != idx)
			continue;
		g_ctrlIdx[i] = g_ctrlIdx[g_ctrlCount - 1];
		--g_ctrlCount;
		return;
	}
}

static void CtrlClear() {
	std::lock_guard<std::mutex> lock(g_ctrlMutex);
	g_ctrlCount = 0;
}

static bool NameLooksWorldEsp(const char* name)
{
	if (!name || !name[0])
		return false;
	if (std::strncmp(name, "weapon_", 7) == 0)
		return true;
	if (std::strstr(name, "projectile") || std::strstr(name, "grenade")
		|| std::strstr(name, "molotov") || std::strstr(name, "inferno")
		|| std::strstr(name, "incendiary") || std::strstr(name, "incgrenade")
		|| std::strstr(name, "flash") || std::strstr(name, "decoy")
		|| std::strstr(name, "smoke"))
		return true;
	if (std::strstr(name, "planted") || std::strstr(name, "c4")
		|| std::strstr(name, "C4"))
		return true;
	return false;
}

static bool DesignerIsWorldEsp(void* entity)
{
	if (!entity || !Mem::ValidEntity(entity))
		return false;
	char buf[64]{};
	if (Mem::DesignerName(entity, buf, sizeof(buf)) && NameLooksWorldEsp(buf))
		return true;
	if (buf[0])
		return false;
	return Mem::SchemaClassName(entity, buf, sizeof(buf)) && NameLooksWorldEsp(buf);
}

static void WorldAdd(int idx) {
	if (idx <= 0)
		return;
	std::lock_guard<std::mutex> lock(g_worldMutex);
	for (int i = 0; i < g_worldCount; ++i) {
		if (g_worldIdx[i] == idx)
			return;
	}
	if (g_worldCount >= kMaxTrackedWorld)
		return;
	g_worldIdx[g_worldCount++] = idx;
}

static void WorldRemove(int idx) {
	if (idx <= 0)
		return;
	std::lock_guard<std::mutex> lock(g_worldMutex);
	for (int i = 0; i < g_worldCount; ++i) {
		if (g_worldIdx[i] != idx)
			continue;
		g_worldIdx[i] = g_worldIdx[g_worldCount - 1];
		--g_worldCount;
		return;
	}
}

static void WorldClear() {
	std::lock_guard<std::mutex> lock(g_worldMutex);
	g_worldCount = 0;
}

void LevelShutdownCleanup() {
	CtrlClear();
	WorldClear();
	// RENDER-THREAD ONLY (hkPresent FlushRenderWipe). Clearing these from the
	// game thread while Present builds/draws them = heap UAF = instant exit.
	__try { GrenadeHelper::OnLevelInit(nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.nadeLineupLevel"); }
	__try { World::InvalidateEnvCache(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.worldEnv"); }
	__try { World::Fog::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.fogDrop"); }
	__try { World::Weather::OnLevelChange(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.weatherLevel"); }
	__try { Hitmarker::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.hitmarkerShutdown"); }
	// Map reset must keep the permanent GameEventManager listener registered.
	__try { BulletFx::Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.bulletFxReset"); }
	__try { Hitsound::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.hitsoundShutdown"); }
	__try { HitLog::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.hitLogClear"); }
	__try { Notify::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.notifyClear"); }
	__try { SoundEsp::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.soundEspClear"); }
	__try { Esp::InvalidateCaches(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.espInvalidate"); }
	// Avatar SRVs belong to the (possibly recreated) D3D11 device - dangling
	// views would be drawn after a device reset. Render-thread owned cache.
	__try { SteamAvatar::ClearCache(); } __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("sdka.steamAvatarClear"); }
}

} // namespace

std::uint32_t EntityGen() { return g_entityGen; }
std::uint32_t MapGen() { return g_mapGen; }

bool IsOverwatch() {
	if (!g_isOverwatch)
		return false;
	bool r = false;
	__try { r = g_isOverwatch(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return r;
}

bool IsDemoOrHltv() {
	if (!g_isDemoOrHltv)
		return false;
	bool r = false;
	__try { r = g_isDemoOrHltv(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return r;
}

bool ShouldSoftDisableCombat() {
	return IsOverwatch() || IsDemoOrHltv();
}

void* GameRules() {
	if (!g_ppGameRules || !Mem::IsReadable(g_ppGameRules, sizeof(void*)))
		return nullptr;
	void* p = nullptr;
	__try { p = *g_ppGameRules; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return p;
}

void* GetEconWpnData(C_CSWeaponBase* weapon) {
	if (!weapon || !g_getEconWpn || !Mem::ValidEntity(weapon))
		return nullptr;
	void* r = nullptr;
	__try { r = g_getEconWpn(weapon); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return r;
}

bool GetAbsOrigin(void* entity, Vector_t& out) {
	if (!entity || !g_getAbsOrigin || !Mem::ValidEntity(entity))
		return false;
	Vector_t* p = nullptr;
	__try { p = g_getAbsOrigin(entity); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!p || !Mem::IsReadable(p, sizeof(Vector_t)))
		return false;
	__try { out = *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool ComputeHitboxSurroundingBox(void* entity, Vector_t& minsOut, Vector_t& maxsOut) {
	if (!entity || !g_computeBox)
		return false;
	Vector_t mins{}, maxs{};
	char ok = 0;
	__try { ok = g_computeBox(entity, &mins, &maxs); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!ok)
		return false;
	minsOut = mins;
	maxsOut = maxs;
	return true;
}

void OnLevelShutdown() {
	++g_mapGen;
	++g_entityGen;
	__try { W2S::Invalidate(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	g_renderWipePending.store(true, std::memory_order_release);
}

void RequestRenderWipe() {
	g_renderWipePending.store(true, std::memory_order_release);
}

void FlushRenderWipe() {
	if (!g_renderWipePending.exchange(false, std::memory_order_acquire))
		return;
	LevelShutdownCleanup();
}

void OnEntityAdded(void* /*entitySystem*/, void* entity, int handle) {
	++g_entityGen;
	if (!entity)
		return;
	if (H::SessionMapLeaving() || H::SessionPostMatch())
		return;
	{
		const int signon = Engine2::SignonState();
		if (signon < 6)
			return;
	}
	const int idx = HandleToIndex(handle);
	if (idx <= 0)
		return;
	if (DesignerIsController(entity))
		CtrlAdd(idx);
	else if (DesignerIsWorldEsp(entity))
		WorldAdd(idx);
}

void OnEntityRemoved(void* /*entitySystem*/, void* /*entity*/, int handle) {
	++g_entityGen;
	const int idx = HandleToIndex(handle);
	if (idx > 0) {
		CtrlRemove(idx);
		WorldRemove(idx);
	}
}

int CopyControllerIndices(int* out, int maxOut) {
	if (!out || maxOut <= 0)
		return 0;
	std::lock_guard<std::mutex> lock(g_ctrlMutex);
	const int n = (g_ctrlCount < maxOut) ? g_ctrlCount : maxOut;
	for (int i = 0; i < n; ++i)
		out[i] = g_ctrlIdx[i];
	return n;
}

int CopyWorldIndices(int* out, int maxOut) {
	if (!out || maxOut <= 0)
		return 0;
	std::lock_guard<std::mutex> lock(g_worldMutex);
	const int n = (g_worldCount < maxOut) ? g_worldCount : maxOut;
	for (int i = 0; i < n; ++i)
		out[i] = g_worldIdx[i];
	return n;
}

void WarmWorldScan()
{
	if (Engine2::SignonState() < 6)
		return;
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return;

	static int s_cursor = 0;
	static std::uint32_t s_map = 0;
	const std::uint32_t mapGen = g_mapGen;
	if (mapGen != s_map) {
		s_map = mapGen;
		s_cursor = 1;
	}

	{
		std::lock_guard<std::mutex> lock(g_worldMutex);
		if (g_worldCount > 0 && s_cursor == 0)
			return;
	}

	if (s_cursor <= 0) {
		static unsigned long long s_emptyMs = 0;
		const unsigned long long now = GetTickCount64();
		if (s_emptyMs != 0 && now < s_emptyMs + 4000ull)
			return;
		s_emptyMs = now;
		s_cursor = 1;
	}

	const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0) {
		s_cursor = 0;
		return;
	}

	constexpr int kSlice = 96;
	const int end = (s_cursor + kSlice - 1 < nMax) ? (s_cursor + kSlice - 1) : nMax;
	for (int i = s_cursor; i <= end; ++i) {
		void* e = I::GameEntity->Instance->Get(i);
		if (!e)
			continue;
		if (DesignerIsController(e))
			CtrlAdd(i);
		else if (DesignerIsWorldEsp(e))
			WorldAdd(i);
	}
	s_cursor = end + 1;
	if (s_cursor > nMax)
		s_cursor = 0;
}

void* OnAddAddr() { return g_onAddAddr; }
void* OnRemoveAddr() { return g_onRemoveAddr; }
void* LevelShutdownAddr() { return g_levelShutdownAddr; }

void MarkHooked(const char* name, const char* note) {
	if (Entry* e = FindEntry(name)) {
		e->status = Status::Hooked;
		if (note)
			e->note = note;
	}
}

bool Init() {
	Con::Section("SdkPrioA");

	SetEntry("GetMapName", Status::AlreadyHad, nullptr,
		"GameMode::TryClientMapName / engine2 LevelName");
	SetEntry("GetMapBspName", Status::AlreadyHad, nullptr,
		"GameMode::TryClientMapBspName");
	SetEntry("TraceToExit", Status::Blocked, nullptr,
		"==CreateTrace; exit is HandleBulletPen segment idx; TraceToExitSimple");

	auto resolve = [](const char* name, const char* pat, const char* loose = nullptr) -> void* {
		void* p = ScanClientEither(pat, loose);
		if (p) {
			SetEntry(name, Status::Resolved, p, "ok");
			Con::Ok("PrioA %s @ 0x%p", name, p);
		} else {
			SetEntry(name, Status::Failed, nullptr, "scan miss");
			Con::PatternMiss("client", name);
		}
		return p;
	};

	g_getEconWpn = reinterpret_cast<FnGetEconWpn>(
		resolve("C_CSWeaponBase_GetEconWpnData", kPatGetEconWpnData, kPatGetEconWpnDataLoose));
	g_isOverwatch = reinterpret_cast<FnBool>(
		resolve("IsOverwatch", kPatIsOverwatch));
	g_isDemoOrHltv = reinterpret_cast<FnBool>(
		resolve("IsDemoOrHltv", kPatIsDemoOrHltv));
	g_computeBox = reinterpret_cast<FnComputeBox>(
		resolve("C_BaseEntity_ComputeHitboxSurroundingBox", kPatComputeHitboxBox));
	g_getAbsOrigin = reinterpret_cast<FnGetAbs>(
		resolve("GetAbsOrigin", kPatGetAbsOrigin));

	if (auto* site = ScanClient(kPatPGameRules)) {
		const uintptr_t abs = M::getAbsoluteAddress(reinterpret_cast<uintptr_t>(site), 3, 0);
		if (abs) {
			g_ppGameRules = reinterpret_cast<void**>(abs);
			SetEntry("pGameRules", Status::Resolved, g_ppGameRules, "global ptr");
			Con::Ok("PrioA pGameRules @ 0x%p", (void*)abs);
		} else {
			SetEntry("pGameRules", Status::Failed, nullptr, "rip resolve fail");
		}
	} else {
		SetEntry("pGameRules", Status::Failed, nullptr, "scan miss");
		Con::PatternMiss("client", "pGameRules");
	}

	g_onAddAddr = resolve("OnAddEntity", kPatOnAdd);
	if (g_onAddAddr)
		SetEntry("OnAddEntity", Status::Resolved, g_onAddAddr, "awaiting hook");
	g_onRemoveAddr = resolve("OnRemoveEntity", kPatOnRemove);
	if (g_onRemoveAddr)
		SetEntry("OnRemoveEntity", Status::Resolved, g_onRemoveAddr, "awaiting hook");
	g_levelShutdownAddr = resolve("LevelShutdown", kPatLevelShutdown);
	if (g_levelShutdownAddr)
		SetEntry("LevelShutdown", Status::Resolved, g_levelShutdownAddr, "awaiting hook");

	g_ready = true;

	int ok = 0, fail = 0, had = 0, blocked = 0;
	for (int i = 0; i < kEntryCount; ++i) {
		switch (g_entries[i].status) {
		case Status::Resolved:
		case Status::Hooked: ++ok; break;
		case Status::AlreadyHad: ++had; break;
		case Status::Blocked: ++blocked; break;
		case Status::Failed: ++fail; break;
		default: break;
		}
	}
	Con::Info("PrioA summary: resolved=%d already=%d blocked=%d failed=%d (hooks pending)",
		ok, had, blocked, fail);
	return true;
}

bool Ready() { return g_ready; }

} // namespace SdkPrioA

