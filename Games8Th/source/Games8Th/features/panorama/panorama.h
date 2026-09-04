#pragma once

// Panorama UI bridge (panorama.dll + client matchmaking).
// - Auto-accept: Lobby ReadyUp on match-found
// - Custom HUD API: RunFrame ticks + RunScript / DispatchEvent for later panels
namespace Panorama {

	void Install();
	void Uninstall();

	// Captured from CUIEngine::RunFrame hook (null until first UI tick).
	void* UIEngine();

	// client FindPanel by id (e.g. "CSGOHud", "CSGOPopupManager", "PopupManager").
	void* FindPanel(const char* id);
	// HUD script panel from the client HUD global, matching mercey's path.
	void* HudPanel();

	// Compile+run JS in a panel context. panel=null -> CSGOHud.
	bool RunScript(const char* js);
	bool RunScript(void* panel, const char* js);
	// Run through the HUD global's IUIPanel directly (no Panel2D conversion).
	bool RunScriptHud(const char* js);

	// $.DispatchEvent("<name>") via RunScript on CSGOHud.
	bool DispatchEvent(const char* eventName);

// Queue auto-accept (MatchFound hook). Retries on FSN RENDER_START (main menu).
	void RequestAutoAccept();

	// Game-thread retry ticker (FrameStageNotify RENDER_START + NET_UPDATE_START).
	void OnFrame();

	// Map transition: UI tree rebuilt - drop cached panels/engine (dangling ptrs).
	void OnLevelChange();
}

