#include "panorama.h"

#include "../../config/config.h"
#include "../../hooks/includeHooks.h"
#include "../../hooks/hooks.h"
#include "../engine2/engine2.h"
#include "../../utils/console/console.h"
#include "../../utils/crypto/xorstr.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/patternscan/patternscan.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

	// -- Resolved -------------------------------------------------------------

	using RunFrameFn = __int64(__fastcall*)(void* uiEngine);
	// IDA: RunScript(engine, panel, scriptUtf8, originPath, flags)
	using RunScriptFn = __int64(__fastcall*)(void* uiEngine, void* panel,
const char* script,
		const char* originPath, __int64 flags);
	using MatchFoundFn = void(__fastcall*)(void* thisptr, void* kv);
	using GetLobbyFn = void*(__fastcall*)();
	// IDA sub_180F5D6E0 - PanoramaComponent_Lobby_ReadyUpForMatch (UI popup only)
	using ReadyUpFn = void(__fastcall*)(void* lobby, char ready, int a3, int a4, char confirm);
	// Dump SetLocalPlayerReady / SetPlayerReady - LobbyAPI accept button.
	// IDA 0x180F5C5C0: empty string -> C92150(reservation, 1) sends INETSUPPORT accept.
	using SetLocalPlayerReadyFn = void(__fastcall*)(void* thisptr, const char* reason);

	CInlineHookObj<RunFrameFn> g_hkRunFrame{};
	CInlineHookObj<MatchFoundFn> g_hkMatchFound{};

	RunScriptFn g_fnRunScript = nullptr;
	GetLobbyFn g_fnGetLobby = nullptr;
	ReadyUpFn g_fnReadyUp = nullptr;
	SetLocalPlayerReadyFn g_fnSetLocalPlayerReady = nullptr;
	void** g_ppPanelMgr = nullptr; // client qword (FindPanel @ +0x4D0) - NOT RunFrame CUIEngine*
	void** g_ppHudPanel = nullptr; // client HUD object; script panel is HUD + 0x8

	std::
atomic<void*> g_uiEngine{ nullptr };
	std::
atomic<void*> g_cachedScriptPanel{ nullptr }; // last good IUIPanel
	// RunFrame hook stays off (pattern risk on the UI thread); MatchFound hook
	// target verified unique in IDA (sub_180C977E0 - server reservation check)
	// and is what fires when the accept popup appears.
	constexpr bool kInstallPanoramaHooks = false;
	constexpr bool kInstallMatchFoundHook = true;

	// Auto-accept retry window
	std::
atomic<int> g_acceptAttempts{ 0 };
	std::
atomic<ULONGLONG> g_acceptUntil{ 0 };
	std::
atomic<ULONGLONG> g_lastAcceptTry{ 0 };

	// Client-stored UI ptr FindPanel @ +0x4D0 (cs_clientui CSGOHud path).
	// Do NOT call this on RunFrame's CUIEngine* - different vtable layout; wrong slot crashes.
	__declspec(noinline) static void* SehClientFindPanel(void* clientUi, const char* id) {
		if (!clientUi || !id || !id[0])
			return nullptr;
		void* out = nullptr;
		__try {
			void** vt = *reinterpret_cast<void***>(clientUi);
			if (!vt)
				return nullptr;
			using Fn = void*(__fastcall*)(void*, const char*);
			auto fn = reinterpret_cast<Fn>(vt[0x4D0 / 8]);
			if (!fn)
				return nullptr;
			out = fn(clientUi, id);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out = nullptr;
		}
		return out;
	}

	// Panel2D wrapper -> IUIPanel* (+0x40). Do not call IsValidPanel on CUIEngine here:
	// that vfunc offset is easy to get wrong and will crash the UI thread.
	__declspec(noinline) static void* SehToUIPanel(void* panelOrWrapper) {
		if (!panelOrWrapper)
			return nullptr;
		void* out = nullptr;
		__try {
			void** vt = *reinterpret_cast<void***>(panelOrWrapper);
			if (!vt)
				return nullptr;
			using Fn = void*(__fastcall*)(void*);
			auto getUi = reinterpret_cast<Fn>(vt[0x40 / 8]);
			if (getUi)
				out = getUi(panelOrWrapper);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out = nullptr;
		}
		return out;
	}

	__declspec(noinline) static void* TryGetPanoramaUiEngine() {
		using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
		HMODULE panMod = GetModuleHandleA("panorama.dll");
		if (!panMod)
			return nullptr;
		auto ci = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(panMod, "CreateInterface"));
		if (!ci)
			return nullptr;
		void* engIface = ci("PanoramaUIEngine001", nullptr);
		if (!engIface)
			return nullptr;
		void* eng = nullptr;
		__try {
			void** vt = *reinterpret_cast<void***>(engIface);
			if (vt) {
				using GetEng = void*(__fastcall*)(void*);
				auto getEng = reinterpret_cast<GetEng>(vt[13]);
				if (getEng)
					eng = getEng(engIface);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			eng = nullptr;
		}
		return eng;
	}

	void* ResolveUIEngine() {
		void* eng = g_uiEngine.load(std::memory_order_relaxed);
		if (!eng) {
			eng = TryGetPanoramaUiEngine();
			if (eng)
				g_uiEngine.store(eng, std::memory_order_relaxed);
		}
		return eng;
	}

	void* ClientUiPtr() {
		if (!g_ppPanelMgr)
			return nullptr;
		void* mgr = nullptr;
		__try { mgr = *g_ppPanelMgr; }
		__except (EXCEPTION_EXECUTE_HANDLER) { mgr = nullptr; }
		if (!mgr || reinterpret_cast<uintptr_t>(mgr) < 0x10000ull)
			return nullptr;
		return mgr;
	}

	void* ToScriptPanel(void* found) {
		if (!found)
			return nullptr;
		if (void* ui = SehToUIPanel(found))
			return ui;
		return found;
	}

	// Only client FindPanel (+0x4D0). Never call that slot on RunFrame's CUIEngine*.
	void* ResolveScriptPanel(void* /*engine*/) {
		void* cached = g_cachedScriptPanel.load(std::memory_order_relaxed);
		if (cached)
			return cached;

		static const char* kIds[] = {
			"CSGOHud", "CSGOMainMenu", "CSGOPopupManager", "PopupManager", "MainMenuRootPanel"
		};

		void* clientUi = ClientUiPtr();
		if (!clientUi)
			return nullptr;

		for (const char* id : kIds) {
			void* ui = ToScriptPanel(SehClientFindPanel(clientUi, id));
			if (ui) {
				g_cachedScriptPanel.store(ui, std::memory_order_relaxed);
				return ui;
			}
		}
		return nullptr;
	}

	// IDA RunScript(a1, panel, script, originPath, flags):
	// a4 = script-origin file path (const char*) - NOT an out ptr
	// a5 != 0 forces compile-from-string (skip code cache)
	__declspec(noinline) static bool SehRunScript(void* engine, void* panel, const char* js) {
		if (!g_fnRunScript || !engine || !panel || !js)
			return false;
		bool ok = false;
		__try {
			g_fnRunScript(engine, panel, js, "panorama/games8th.js", 1);
			ok = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		return ok;
	}


	__declspec(noinline) static void SehReadyUp(void* lobby, char confirm) {
		if (!g_fnReadyUp || !lobby)
			return;
		__try {
			g_fnReadyUp(lobby, 1, 0, 0, confirm);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	__declspec(noinline) static void SehSetLocalPlayerReady() {
		if (!g_fnSetLocalPlayerReady)
			return;
		__try {
			g_fnSetLocalPlayerReady(nullptr, "");
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	__declspec(noinline) static void* SehGetLobby() {
		if (!g_fnGetLobby)
			return nullptr;
		void* out = nullptr;
		__try { out = g_fnGetLobby(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
		return out;
	}

	bool TryRunAcceptScript() {
		void* eng = ResolveUIEngine();
		if (!eng)
			return false;

		static const char kJs[] =
			"(function(){"
			"try{if(typeof LobbyAPI!=='undefined'&&LobbyAPI.SetLocalPlayerReady){LobbyAPI.SetLocalPlayerReady('');LobbyAPI.SetLocalPlayerReady('1');}}"
			"catch(e){}"
			"try{if(typeof MatchmakingAPI!=='undefined'&&MatchmakingAPI.PartyMatchAccept){MatchmakingAPI.PartyMatchAccept();}}"
			"catch(e){}"
			"try{$.DispatchEvent('PanoramaComponent_Matchmaking_PartyMatchAccept');}"
			"catch(e){}"
			"try{$.DispatchEvent('PanoramaComponent_Lobby_ReadyUpForMatch',true,0,0);}"
			"catch(e){}"
			"try{$.DispatchEvent('CSGOCustomReadyUp');}"
			"catch(e){}"
			"try{"
			"  var root = $.GetContextPanel();"
			"  if(root){"
			"    var btn = root.FindChildInLayoutFile('AcceptMatchButton') || root.FindChildInLayoutFile('btnAccept') || root.FindChildTraverse('AcceptMatchButton') || root.FindChildTraverse('btnAccept');"
			"    if(btn){ $.DispatchEvent('Activated', btn, 'mouse'); }"
			"  }"
			"}catch(e){}"
			"})();";

		static const char* kIds[] = {
			"CSGOPopupManager", "PopupManager", "CSGOMainMenu", "CSGOHud", "MainMenuRootPanel"
		};
		void* clientUi = ClientUiPtr();
		if (clientUi) {
			for (const char* id : kIds) {
				void* ui = ToScriptPanel(SehClientFindPanel(clientUi, id));
				if (ui && SehRunScript(eng, ui, kJs))
					return true;
			}
		}
		void* panel = ResolveScriptPanel(eng);
		if (!panel)
			return false;
		return SehRunScript(eng, panel, kJs);
	}

	// Dump SetLocalPlayerReady is the accept button (LobbyAPI). ReadyUpForMatch
	// only drives the popup UI - MatchFound already calls it with confirm=0.
	__declspec(noinline) static void TryAcceptCpp() {
		SehSetLocalPlayerReady();
		void* lobby = SehGetLobby();
		if (lobby)
			SehReadyUp(lobby, 1);
	}

	void TryAutoAcceptOnce() {
		if (!Config::auto_accept)
			return;
		if (Config::loading.load(std::memory_order_acquire))
			return;
		// Already connecting/loading - LobbyAPI ready-up hits a dying lobby.
		if (Engine2::NetworkGameClient() && Engine2::SignonState() >= 2)
			return;
		const ULONGLONG now = GetTickCount64();
		// Burst path: MatchFound hook set g_acceptUntil/Attempts for 20s rapid retry (150ms)
		if (now <= g_acceptUntil.load(std::memory_order_relaxed)
			&& g_acceptAttempts.load(std::memory_order_relaxed) > 0) {
			const ULONGLONG last = g_lastAcceptTry.load(std::memory_order_relaxed);
			if (last && now - last < 150ull)
				return;
			g_lastAcceptTry.store(now, std::memory_order_relaxed);
			g_acceptAttempts.fetch_sub(1, std::memory_order_relaxed);
			__try { TryAcceptCpp(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			__try { TryRunAcceptScript(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return;
		}
		// Poll fallback: MatchFound hook missed (pattern drift) or menu queue without hook.
		// Try every 500ms when in lobby/main menu.
		static ULONGLONG s_pollLast = 0;
		if (now - s_pollLast < 500ull)
			return;
		s_pollLast = now;

		const ULONGLONG last = g_lastAcceptTry.load(std::memory_order_relaxed);
		if (last && now - last < 500ull)
			return;
		g_lastAcceptTry.store(now, std::memory_order_relaxed);
		__try { TryAcceptCpp(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		__try { TryRunAcceptScript(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	__int64 __fastcall hkRunFrame(void* uiEngine) {
		if (uiEngine)
			g_uiEngine.store(uiEngine, std::memory_order_relaxed);

		// Call original first - never risk skipping the UI frame on our failures.
		__int64 ret = 0;
		if (g_hkRunFrame.IsHooked()) {
			auto orig = g_hkRunFrame.GetOriginal();
			if (orig)
				ret = orig(uiEngine);
		}

		__try { TryAutoAcceptOnce(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("panorama.autoAccept"); }

		return ret;
	}

	void __fastcall hkMatchFound(void* thisptr, void* kv) {
		if (g_hkMatchFound.IsHooked()) {
			auto orig = g_hkMatchFound.GetOriginal();
			if (orig)
				orig(thisptr, kv);
		}
		if (Config::auto_accept && !Config::loading.load(std::memory_order_acquire)) {
			Panorama::RequestAutoAccept();
			__try { TryAcceptCpp(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			__try { TryRunAcceptScript(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

} // namespace

void Panorama::
Install() {
	// Always resolve RunScript + panel mgr (scoreboard weapons / HUD scripts).
	// Optional hooks stay gated for inject stability.
	// IDA panorama.dll 0x1800B6AA0 verified via idalib 53d94342/adb42df8 - strict + loose fallback for future builds.
	g_fnRunScript = reinterpret_cast<RunScriptFn>(M::
patternScan(XS("panorama"),
		XS("48 89 5C 24 10 4C 89 4C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48")));
	if (!g_fnRunScript)
		g_fnRunScript = reinterpret_cast<RunScriptFn>(M::patternScan(XS("panorama"),
			XS("48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48")));
	if (!g_fnRunScript)
		g_fnRunScript = reinterpret_cast<RunScriptFn>(M::patternScan(XS("panorama"),
			XS("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B D9 49 8B F8 48 8B F2 48 8B E9")));
	if (g_fnRunScript)
		Con::Ok("Panorama RunScript @ 0x%p", (void*)g_fnRunScript);
	else
		Con::OffsetMiss("Panorama RunScript");

	{
		uintptr_t addr = M::patternScan(XS("client"),
			XS("48 8B 0D ? ? ? ? 48 8B 11 4C 8B 82 D0 04 00 00 48 8D 15"));
		if (addr) {
			g_ppPanelMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(addr, 3));
			Con::Ok("PanelMgr @ 0x%p", (void*)g_ppPanelMgr);
		} else {
			Con::OffsetMiss("PanelMgr FindPanel");
		}
	}

	// mercey uses the client HUD global directly. FindPanel is retained as a
	// fallback because the global is populated only after the HUD is created.
	// Current build: mov rax,[rip+hud]; test rax,rax; je - rip-disp at +3
	// resolves straight to the global slot (same layout mercey runs live).
	// Old mov rsi,[rip]+call+test form kept as fallback for older builds.
	{
		uintptr_t addr = M::patternScan(XS("client"),
			XS("48 8B 05 ? ? ? ? 48 85 C0 74 71"));
		uintptr_t hudGlobal = 0;
		if (addr)
			hudGlobal = M::getAbsoluteAddress(addr, 3);
		if (!hudGlobal) {
			addr = M::patternScan(XS("client"),
				XS("48 89 35 ? ? ? ? E8 ? ? ? ? 48 85"));
			if (addr)
				hudGlobal = M::getAbsoluteAddress(addr, 3);
		}
		if (hudGlobal) {
			g_ppHudPanel = reinterpret_cast<void**>(hudGlobal);
			Con::Ok("HudPanel @ 0x%p", (void*)g_ppHudPanel);
		} else {
			Con::OffsetMiss("HudPanel global");
		}
	}

	// Fallback UI engine: CreateInterface PanoramaUIEngine001 -> vfunc 13
	if (!g_uiEngine.load(std::memory_order_relaxed)) {
		if (void* eng = TryGetPanoramaUiEngine())
			g_uiEngine.store(eng, std::memory_order_relaxed);
	}

	if constexpr (!kInstallMatchFoundHook) {
		Con::Info("Panorama MatchFound hook disabled");
		return;
	}

	{
		uintptr_t addr = M::patternScan(XS("client"),
			XS("48 85 D2 0F 84 ? ? ? ? 48 8B C4 55 53 56 57"));
		if (addr) {
			if (!g_hkMatchFound.Add(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&hkMatchFound)))
				Con::Error("MatchFoundHandler hook.Add failed");
			else
				Con::Ok("MatchFoundHandler @ 0x%p", (void*)addr);
		} else {
			Con::OffsetMiss("MatchFoundHandler");
		}
	}

	g_fnGetLobby = reinterpret_cast<GetLobbyFn>(M::patternScan(XS("client"),
		XS("40 53 48 83 EC 20 48 8B 0D ? ? ? ? 33 DB 48 85 C9")));
	if (g_fnGetLobby)
		Con::Ok("GetLobbyComponent @ 0x%p", (void*)g_fnGetLobby);
	else
		Con::OffsetMiss("GetLobbyComponent");

	g_fnReadyUp = reinterpret_cast<ReadyUpFn>(M::patternScan(XS("client"),
		XS("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 0F B6")));
	if (g_fnReadyUp)
		Con::Ok("Lobby ReadyUp @ 0x%p", (void*)g_fnReadyUp);
	else
		Con::OffsetMiss("Lobby ReadyUp");

	// Dump SetLocalPlayerReady / SetPlayerReady - unique in IDA (0x180F5C5C0).
	g_fnSetLocalPlayerReady = reinterpret_cast<SetLocalPlayerReadyFn>(M::patternScan(XS("client"),
		XS("40 53 48 83 EC 20 48 8B DA 48 8D 15 ? ? ? ? 48 8B CB FF")));
	if (g_fnSetLocalPlayerReady)
		Con::Ok("SetLocalPlayerReady @ 0x%p", (void*)g_fnSetLocalPlayerReady);
	else
		Con::OffsetMiss("SetLocalPlayerReady");
}

void Panorama::Uninstall()
{
	g_hkMatchFound.Remove();
	g_hkRunFrame.Remove();
}

void* Panorama::
UIEngine() {
	return g_uiEngine.load(std::
memory_order_relaxed);
}

void* Panorama::
FindPanel(const char* id) {
	if (!id || !id[0])
		return nullptr;
	void* clientUi = ClientUiPtr();
	if (!clientUi)
		return nullptr;
	return SehClientFindPanel(clientUi, id);
}

void* Panorama::HudPanel() {
	if (!g_ppHudPanel || !Mem::IsReadable(g_ppHudPanel, sizeof(void*)))
		return nullptr;
	void* hud = nullptr;
	__try { hud = *g_ppHudPanel; }
	__except (EXCEPTION_EXECUTE_HANDLER) { hud = nullptr; }
	if (!hud || !Mem::IsUserPtr(hud))
		return nullptr;

	void* panel = nullptr;
	__try { panel = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(hud) + 0x8); }
	__except (EXCEPTION_EXECUTE_HANDLER) { panel = nullptr; }
	return panel && Mem::IsUserPtr(panel) ? panel : nullptr;
}

bool Panorama::
RunScript(const char* js) {
	if (H::SessionMapLeaving())
		return false;
	void* eng = ResolveUIEngine();
	void* panel = ResolveScriptPanel(eng);
	if (!eng || !panel || !js)
		return false;
	return SehRunScript(eng, panel, js);
}

bool Panorama::
RunScript(void* panel, const char* js) {
	if (H::SessionMapLeaving())
		return false;
	if (!panel || !js)
		return false;
	void* engine = ResolveUIEngine();
	if (!engine)
		return false;
	void* ui = ToScriptPanel(panel);
	return SehRunScript(engine, ui ? ui : panel, js);
}

bool Panorama::RunScriptHud(const char* js) {
	if (H::SessionMapLeaving() || !js)
		return false;
	void* engine = ResolveUIEngine();
	void* panel = HudPanel();
	if (!engine || !panel)
		return false;
	// HudPanel() already returns the IUIPanel stored at HUD + 0x8. Do not run
	// it through ToScriptPanel, which is only for FindPanel's Panel2D wrapper.
	return SehRunScript(engine, panel, js);
}

bool Panorama::
DispatchEvent(const char* eventName) {
	if (!eventName || !eventName[0])
		return false;
	// Keep script tiny / stack-friendly
	char buf[384]{};
	const int n = std::
snprintf(buf, sizeof(buf),
		"(function(){try{$.DispatchEvent('%s');}catch(e){}})();", eventName);
	if (n <= 0 || n >= static_cast<int>(sizeof(buf)))
		return false;
	// Reject quotes in event name (injection / broken JS)
	for (const char* p = eventName; *p; ++p) {
		if (*p == '\'' || *p == '"' || *p == '\\')
			return false;
	}
	return RunScript(buf);
}

void Panorama::RequestAutoAccept() {
	g_acceptAttempts.store(40, std::memory_order_relaxed);
	const ULONGLONG now = GetTickCount64();
	g_acceptUntil.store(now + 18000ull, std::memory_order_relaxed);
	g_lastAcceptTry.store(now, std::memory_order_relaxed);
	Con::Ok("Auto-accept: match found, accepting");
}

void Panorama::
OnFrame() {
	// Menu queue still needs auto-accept. Connect/match park stays quiet.
	if (H::SessionMapLeaving() && H::SessionLive())
		return;
	// Game-thread retry ticker (wired into FrameStageNotify). Without this the
	// accept queue dies after the single MatchFound attempt (RunFrame hook off).
	__try { TryAutoAcceptOnce(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("panorama.OnFrame"); }
}

void Panorama::
OnLevelChange() {
	// UI tree is rebuilt on map transition - the cached script panel pointer
	// becomes dangling; RunScript on it crashes the panorama UI thread (the
	// "instant close when leaving a game" crash).
	g_cachedScriptPanel.store(nullptr, std::memory_order_relaxed);
	// UI engine instance may also be recreated - re-resolve on next use.
	g_uiEngine.store(nullptr, std::memory_order_relaxed);
}

