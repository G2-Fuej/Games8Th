#include "debug/debug.h"

#include "includes.h"
#include "Games8Th/Games8Th.h"
#include "Games8Th/config/config.h"
#include "Games8Th/features/movement/movement.h"
#include "Games8Th/features/notify/notify.h"
#include "Games8Th/features/sound_esp/sound_esp.h"
#include "Games8Th/features/vote/vote.h"
#include "Games8Th/features/world/world.h"
#include "Games8Th/features/world/weather.h"
#include "Games8Th/features/visuals/visuals.h"
#include "Games8Th/renderer/icons.h"
#include "Games8Th/utils/console/console.h"
#include "Games8Th/utils/memory/patternscan/patternscan.h"
#include "Games8Th/utils/security/secure_allow.h"
#include "Games8Th/utils/security/sehsupport.h"
#include "Games8Th/utils/security/crashdump.h"
#include "Games8Th/hooks/hooks.h"
#include "Games8Th/features/sdk_prio_a/sdk_prio_a.h"
#include "cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "Games8Th/features/engine2/engine2.h"
#include "Games8Th/features/panorama/panorama.h"
#include "Games8Th/keybinds/keybinds.h"

#include <intrin.h>   // __readgsqword / __readfsdword
#include <atomic>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HANDLE g_hConsole = nullptr;
static FILE* g_logFile = nullptr;

// --- Manual map support ---
// When injected via manual mapper, hModule passed to DllMain is NOT in PEB
// loader data structures. GetModuleHandle on our own DLL will fail, and
// FreeLibraryAndExitThread will crash. We detect this and adapt.
HMODULE g_OurModule = nullptr;
bool    g_ManualMapped = false;

static bool IsModuleInPEB(HMODULE hMod) {
 // Walk PEB -> Ldr -> InLoadOrderModuleList using raw offsets.
 // This avoids any SDK struct definition issues and works on all x64 Windows.
 // PEB layout (x64): offset 0x18 = Ldr (PEB_LDR_DATA*)
 // PEB_LDR_DATA: offset 0x10 = InLoadOrderModuleList (LIST_ENTRY)
 // LDR_DATA_TABLE_ENTRY: offset 0x30 = DllBase (void*)

    const auto peb = reinterpret_cast<uintptr_t>( 
        reinterpret_cast<void*>(__readgsqword(0x60)) );
    if (!peb) return false;

    const auto ldr = *reinterpret_cast<uintptr_t*>(peb + 0x18);
    if (!ldr) return false;

 // InLoadOrderModuleList head
    const auto headFlink = reinterpret_cast<LIST_ENTRY*>(ldr + 0x10);
    auto entry = headFlink->Flink;

    int hops = 0;
    while (entry && entry != headFlink && hops++ < 512) {
        const auto dllBase = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(entry) + 0x30);
        if (dllBase == static_cast<void*>(hMod))
            return true;
        entry = entry->Flink;
    }
    return false;
}

Games8Th games8Th;

Present oPresent;
static SafetyHookInline g_presentHook{}; // gameoverlayrenderer64 PresentOverlay
HWND window = NULL;
WNDPROC oWndProc;
ID3D11Device* pDevice = NULL;
ID3D11DeviceContext* pContext = NULL;
ID3D11RenderTargetView* mainRenderTargetView;

std::atomic<bool> g_bMenuOpen{ false }; // synced with menu for input blocking in cmd/hooks (written render/message, read game thread)

// ImGui input queue is pushed by WndProc (game message thread) and drained by
// ImGui::NewFrame (render thread) - unsynchronized = "Unknown event!" + frame
// asserts (imgui.cpp:10599 / 10942). SRW lock serializes both sides.
static SRWLOCK g_imguiInputLock = SRWLOCK_INIT;

// WndProc runs on the game's message thread - it must participate in the
// unload quiescence barrier like every detour, or the image can unmap while
// a message dispatch is still inside this code (post-unload AV).
namespace {
struct WndProcDepthGuard {
	~WndProcDepthGuard() { H::Hooks::LeaveHook(); }
};
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	H::Hooks::EnterHook();
	WndProcDepthGuard depthGuard;
 // Keep CreateMove strip in sync even between Present frames
    const bool menuOpen = games8Th.renderer.menu.isOpen();
    g_bMenuOpen.store(menuOpen, std::memory_order_relaxed);

    if (menuOpen) {
 // Feed ImGui - TRY lock only. Present holds the lock inside
 // ImGui_ImplWin32_NewFrame, which calls ClipCursor() -> synchronous
 // WM_WINDOWPOSCHANGING etc. re-enters this WndProc. A blocking acquire
 // there deadlocks (SRW not re-entrant, cross-thread ABBA). Dropping the
 // message for the few-us frame window is harmless.
        if (TryAcquireSRWLockExclusive(&g_imguiInputLock)) {
            ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
            ReleaseSRWLockExclusive(&g_imguiInputLock);
        }

        ClipCursor(nullptr);
 // Don't call ShowCursor every message - Present handles refcount on toggle

 // Eat game-bound input while menu is open (raw + legacy mouse wheel)
        switch (uMsg) {
        case WM_INPUT:
            return 1;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
 // INSERT toggled via GetAsyncKeyState in Present - still block game
            return 1;
        case WM_CHAR:
        case WM_DEADCHAR:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return 1;
        case WM_SETCURSOR: {
            static HCURSOR s_arrow = LoadCursor(nullptr, IDC_ARROW);
            SetCursor(s_arrow);
            return 1;
        }
        default:
            break;
        }
    }

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

bool init = false;
static std::atomic<int> g_initPhase{ 0 }; // 0 none, 1 busy, 2 done
static std::atomic<bool> g_imguiInPresent{ false }; // re-entry guard: exchange-claim, cleared on every frame exit

// GAMES8TH_TRACE=1 - per-block ms logging for join-lag / map-switch crash
// forensics (renders + FSN). Cheap: two QueryPerformanceCounter calls.
static const bool g_traceEnabled = []() {
	char v[2]{};
	return ::GetEnvironmentVariableA("GAMES8TH_TRACE", v, 2) > 0 && v[0] == '1';
}();
static long long g_traceFreq = [] {
	LARGE_INTEGER f{};
	QueryPerformanceFrequency(&f);
	return f.QuadPart ? f.QuadPart : 1;
}();
static void TraceSpan(const char* tag, const LARGE_INTEGER& t0, double budgetMs) {
	if (!g_traceEnabled)
		return;
	LARGE_INTEGER t1{};
	QueryPerformanceCounter(&t1);
	const double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)g_traceFreq;
	if (ms > budgetMs)
		Con::Warn("TRACE %s %.1f ms", tag, ms);
}

static void AbortPresentInit()
{
	// Tear down anything Games8Th::init may have partially installed before the SEH throw.
	// Without this, a failed init leaves game hooks live and the retry installs duplicates.
    __try { games8Th.shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (oWndProc && window) {
		SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
		oWndProc = nullptr;
	}
	if (mainRenderTargetView) {
		mainRenderTargetView->Release();
		mainRenderTargetView = nullptr;
	}
	if (pContext) {
		pContext->Release();
		pContext = nullptr;
	}
	if (pDevice) {
		pDevice->Release();
		pDevice = nullptr;
	}
	window = NULL;
	init = false;
	g_initPhase.store(0, std::memory_order_release);
}

// RAII depth guard so WaitForQuiescence actually sees an in-flight Present -
// without this, unload's 150ms drain cannot count the render thread and can
// free g_presentHook's trampoline mid-frame.
struct PresentScopeGuard {
    bool m_armed = false;
    ~PresentScopeGuard() { if (m_armed) H::Hooks::LeaveHook(); }
};

// Inner body - no RAII objects at this scope (C2712): the guard lives in the
// thin hkPresent wrapper below.
static HRESULT __stdcall hkPresentInner(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	// Unload signal -> straight to the original address, never past this line.
	// Lets UninstallPresent (loader-lock detach / F4) restore bytes without a
	// render thread wandering deeper into trampoline-owned code.
	if (H::Hooks::s_unloading.load(std::memory_order_acquire))
		return oPresent(pSwapChain, SyncInterval, Flags);

 // Bump the frame counter used by H::SessionLive / EntityOk / FeaturesOk
 // to short-circuit repeated heavy pawn probes within the same frame.
 // Callers on other threads still get fresh probes (thread-local caches).
    H::g_presentFrame.fetch_add(1, std::memory_order_relaxed);

 // Latch with a 3-state phase so nested Present during init cannot run
 // ImGui / entity walks on a half-built renderer, and a second thread
 // cannot start init twice. Busy -> original Present only.
    if (g_initPhase.load(std::memory_order_acquire) == 1)
        return oPresent(pSwapChain, SyncInterval, Flags);

    if (g_initPhase.load(std::memory_order_acquire) == 0)
    {
        int expected = 0;
        if (!g_initPhase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            return oPresent(pSwapChain, SyncInterval, Flags);

        ID3D11Device* dev = nullptr;
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) {
            g_initPhase.store(0, std::memory_order_release);
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        pDevice = dev;
        pDevice->GetImmediateContext(&pContext);
        if (!pContext) {
            pDevice->Release();
            pDevice = nullptr;
            g_initPhase.store(0, std::memory_order_release);
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        DXGI_SWAP_CHAIN_DESC sd{};
        if (FAILED(pSwapChain->GetDesc(&sd)) || !sd.OutputWindow) {
            pContext->Release();
            pContext = nullptr;
            pDevice->Release();
            pDevice = nullptr;
            g_initPhase.store(0, std::memory_order_release);
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
        window = sd.OutputWindow;

        ID3D11Texture2D* pBackBuffer = nullptr;
        if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))
            || !pBackBuffer) {
            pContext->Release();
            pContext = nullptr;
            pDevice->Release();
            pDevice = nullptr;
            window = NULL;
            g_initPhase.store(0, std::memory_order_release);
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
        pBackBuffer->Release();
        if (!mainRenderTargetView) {
            pContext->Release();
            pContext = nullptr;
            pDevice->Release();
            pDevice = nullptr;
            window = NULL;
            g_initPhase.store(0, std::memory_order_release);
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        if (!oWndProc) {
            const LONG_PTR prev = SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
            if (prev) {
                oWndProc = (WNDPROC)prev;
            } else {
                // Subclass failed - CallWindowProc(nullptr) would AV on every
                // message. Skip WndProc install and retry next frame.
                Con::Error("SetWindowLongPtr(WndProc) failed - retrying next frame");
                pContext->Release(); pContext = nullptr;
                pDevice->Release(); pDevice = nullptr;
                if (mainRenderTargetView) { mainRenderTargetView->Release(); mainRenderTargetView = nullptr; }
                window = NULL;
                g_initPhase.store(0, std::memory_order_release);
                return oPresent(pSwapChain, SyncInterval, Flags);
            }
        }
#ifdef GAMES8THDEBUG
        initCall();
#endif
        __try {
            const bool ok = games8Th.init(window, pDevice, pContext, mainRenderTargetView);
            if (!ok) {
                Con::Error("Games8Th::init reported failure - aborting");
                AbortPresentInit();
                return oPresent(pSwapChain, SyncInterval, Flags);
            }
            init = true;
            g_initPhase.store(2, std::memory_order_release);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Con::Seh("Games8Th::init", GetExceptionCode());
            AbortPresentInit();
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
    }

	// Map-leave: watch first so engine-disconnect this frame sets the wipe
	// flag, then flush BEFORE any entity walk. Recaching ESP after
	// LevelShutdown (signon still 6) is the post-match leave crash.
	if (init && !g_imguiInPresent.load(std::memory_order_acquire)) {
		__try { H::SessionWatchLocalLife(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.lifeWatch"); }
		SdkPrioA::FlushRenderWipe();
		__try { H::Hooks::InstallSceneWorldHooks(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

 // CS2 can re-enter Present; never nest ImGui frames. Atomic claim - two
 // threads racing here would both run ImGui::NewFrame on one context. The
 // ISO pass-through below must stay BEFORE the claim (it exits unclaimed).
    if (g_imguiInPresent.load(std::memory_order_acquire)) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

 // GAMES8TH_ISO=4: pure Present pass-through (no ImGui, no entity, no menu).
 // Proves whether overlay/hook body (not config) trips insecure.
    if (H::IsolationLevel() >= 4) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    if (g_imguiInPresent.exchange(true, std::memory_order_acq_rel)) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

 // Map resize / device recycle: recreate RTV when backbuffer changes
    if (init && pDevice && pSwapChain) {
        static UINT s_bbW = 0, s_bbH = 0;
        static uint32_t s_descFrame = 0;
        if ((++s_descFrame & 7u) == 1u || s_bbW == 0 || !mainRenderTargetView) {
        DXGI_SWAP_CHAIN_DESC scd{};
        if (SUCCEEDED(pSwapChain->GetDesc(&scd))) {
            if (s_bbW == 0) {
                s_bbW = scd.BufferDesc.Width;
                s_bbH = scd.BufferDesc.Height;
            } else if (scd.BufferDesc.Width != s_bbW || scd.BufferDesc.Height != s_bbH
                || !mainRenderTargetView) {
                s_bbW = scd.BufferDesc.Width;
                s_bbH = scd.BufferDesc.Height;
                if (mainRenderTargetView) {
                    if (pContext)
                        pContext->OMSetRenderTargets(0, nullptr, nullptr);
                    mainRenderTargetView->Release();
                    mainRenderTargetView = nullptr;
                }
                ID3D11Texture2D* bb = nullptr;
                if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
                    pDevice->CreateRenderTargetView(bb, nullptr, &mainRenderTargetView);
                    bb->Release();
                }
            }
        }
        }
    }

    bool menuOpen = games8Th.renderer.menu.isOpen();
    static bool s_wasMenuOpen = false;
    if (GetAsyncKeyState(VK_INSERT) & 1) {
        games8Th.renderer.menu.toggleMenu();
        menuOpen = games8Th.renderer.menu.isOpen();
    }
    g_bMenuOpen.store(menuOpen, std::memory_order_relaxed);

 // Keep weapon_group_active live for menu Jump Active button
 // (CreateMove/Aimbot don't run when menu open, so group goes stale)
    const bool leaving = H::SessionMapLeaving();

    if (menuOpen && !leaving) {
        __try {
            if (C_CSPlayerPawn* mlp = H::SafeLocalAlive())
                Config::weapon_group_active = Config::ClassifyWeaponGroup(mlp->GetActiveWeapon());
            else
                Config::weapon_group_active = Config::WG_GENERAL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            TW_SEH_CATCH("Present.weaponGroupRefresh");
        }
    }

    if (Config::vote_auto && !leaving) {
        __try { Vote::OnFrame(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.vote"); }
    }

    if (Config::auto_accept) {
        __try { Panorama::OnFrame(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.panorama"); }
    }

 // Toggle keybinds must poll every Present - menu.render() only runs when
 // needOverlay is true, so thirdperson/etc never flipped with HUD off.
    __try { keybind.pollInputs(menuOpen); }
    __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.keybind"); }

 // World/weather tick does NOT need ImGui - run light path when only Night/weather on.
    const bool needWorldTick =
        Config::Night || Config::skybox || Config::lighting
        || Config::map_color || Config::custom_fog
|| Config::smoke_color || Config::fire_color
        || Config::inferno_color
        || (Config::weather && Config::weather_mode >= 1 && Config::weather_mode <= 4);

    const bool needOverlay =
        menuOpen
        || Config::watermark
        || Config::backtrack   // ghost skeleton draw lives in esp()
        || (Config::fov_circle && Config::aimbot)
        || (Config::fov_circle_autofire && Config::autofire)
        || (Config::fov_circle_magnet && Config::triggerbot && Config::trigger_magnet)
        || Config::widget_keybinds || Config::widget_bomb
        || Config::widget_spectators || Config::widget_radar
        || Config::sound_esp
        || Config::esp || Config::espFill || Config::showHealth || Config::showArmor
        || Config::showNameTags || Config::esp_skeleton || Config::showWeapon
        || Config::showWeaponIcon || Config::showDistance
        || Config::flag_flashed || Config::flag_scoped
        || Config::flag_defusing || Config::flag_bomb || Config::flag_reloading
        || Config::grenade_helper
        || Config::world_esp_weapons || Config::world_esp_bomb
        || Config::world_esp_smoke || Config::world_esp_molotov
        || Config::world_esp_he || Config::world_esp_flash || Config::world_esp_decoy
        || Config::hitmarker
        || Config::hitlog
        || Config::bullet_impact_effect || Config::bullet_tracers
        || Config::autowall_xhair
        || Notify::HasPending();

 // Night/weather without ESP/menu: tick world, skip ImGui NewFrame cost.
 // Weather uses SessionLive (in-game), not alive-only EntityOk.
    if (!needOverlay && needWorldTick) {
        const int iso = H::IsolationLevel();
        if (iso < 3) {
            if (H::SessionEntityReady() && !H::SessionMapLeaving() && !H::SessionPostMatch()) {
                __try {
                    if (Config::Night || Config::skybox || Config::lighting
                        || Config::map_color || Config::custom_fog
                        || Config::smoke_color || Config::fire_color)
                        World::Update();
                } __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.WorldLite", GetExceptionCode()); }
            }
        }
        if (s_wasMenuOpen) {
            while (ShowCursor(FALSE) >= 0) {}
            if (H::g_pInputSystem) {
                if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                    orig(H::g_pInputSystem, H::g_wantRelativeMouse);
            }
            s_wasMenuOpen = false;
        }
        g_imguiInPresent.store(false, std::memory_order_release);
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    if (!needOverlay) {
        if (s_wasMenuOpen) {
            while (ShowCursor(FALSE) >= 0) {}
            if (H::g_pInputSystem) {
                if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                    orig(H::g_pInputSystem, H::g_wantRelativeMouse);
            }
            s_wasMenuOpen = false;
        }
        g_imguiInPresent.store(false, std::memory_order_release);
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    bool imguiFrameOpen = false;
    bool imguiLocked = false;
    __try {
        AcquireSRWLockExclusive(&g_imguiInputLock);
        imguiLocked = true;
        MenuTryRebuildFonts();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ReleaseSRWLockExclusive(&g_imguiInputLock);
        imguiLocked = false;
        imguiFrameOpen = true;

        ImGui::GetIO().MouseDrawCursor = menuOpen;

 // Cursor show/hide only on edge - ShowCursor has a refcount; per-frame spam breaks it
        if (menuOpen != s_wasMenuOpen) {
            if (menuOpen) {
                ClipCursor(nullptr);
                while (ShowCursor(TRUE) < 0) {}
 // Force absolute via original (bypass hook latch) for free ImGui cursor
                if (H::g_pInputSystem) {
                    if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                        orig(H::g_pInputSystem, false);
                }
            } else {
                while (ShowCursor(FALSE) >= 0) {}
 // Restore game preference (do NOT force true - breaks CS2 main menu cursor)
                if (H::g_pInputSystem) {
                    if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                        orig(H::g_pInputSystem, H::g_wantRelativeMouse);
                }
            }
            s_wasMenuOpen = menuOpen;
        } else if (menuOpen) {
 // Throttle ClipCursor - per-frame calls add input jitter
            static ULONGLONG s_lastClipMs = 0;
            const ULONGLONG nowClip = GetTickCount64();
            if (nowClip - s_lastClipMs >= 50ull) {
                s_lastClipMs = nowClip;
                ClipCursor(nullptr);
            }
        } else {
 // Hide OS cursor once after inject if something left it visible
            static bool s_hidCursor = false;
            if (!s_hidCursor) {
                while (ShowCursor(FALSE) >= 0) {}
                s_hidCursor = true;
            }
        }

 // ESP/world/weather on Present - NOT FSN.
 // World env: signon>=6 (Ready). EntityOk waited on a gun + team intro.
 // Isolation: GAMES8TH_ISO=1 no world walk, =2 no player cache, =3 no entity.
 // Only clear caches once on leave edge - NOT every menu frame (thrash).
 // SessionWatchLocalLife already ran above (before needOverlay early-out).
        static bool s_hadSession = false;
        static std::uint8_t s_espBurst = 0;
        const int iso = H::IsolationLevel();
        const bool sessionOk = H::SessionEntityReady() && !leaving && !H::SessionPostMatch();
        const bool espOk = sessionOk;
        if (leaving)
            s_hadSession = false;
        if (sessionOk) {
            if (!s_hadSession)
                s_espBurst = 16;
            s_hadSession = true;
            if (iso < 3) {
                __try {
                    if (needWorldTick
                        && (Config::Night || Config::skybox || Config::lighting
                            || Config::map_color || Config::custom_fog
                            || Config::smoke_color || Config::fire_color))
                        World::Update();
                } __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.World", GetExceptionCode()); }
            }
        }
        if (espOk) {
            s_hadSession = true;
            if (iso < 2) {
				__try {
					const bool needEspCache =
						Esp::NeedPlayerCache() || Esp::NeedWorldCache()
						|| Config::grenade_helper
						|| Config::sound_esp || Config::hitmarker || Config::hitlog
						|| Config::widget_radar || Config::widget_bomb || Config::widget_spectators
						|| Config::glow_world_weapons || Config::glow_world_grenades
						|| (Config::glow && Config::glow_only_visible);
                    if (needEspCache) {
                        const bool burst = s_espBurst > 0;
                        if (burst)
                            --s_espBurst;
                        if (burst || (H::g_presentFrame.load(std::memory_order_relaxed) & 1) == 0) {
                            LARGE_INTEGER ts; if (g_traceEnabled) QueryPerformanceCounter(&ts);
                            Esp::cache();
                            if (g_traceEnabled) TraceSpan("present.espCache", ts, 3.0);
                        }
                    }
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					Con::Seh("hkPresent.Esp.cache", GetExceptionCode());
					Esp::InvalidateCaches();
				}
            }
        } else if (s_hadSession && H::SessionMapLeaving()) {
            // Keep last ESP through TDM death / pawn recycle. Wipe only on leave.
            s_hadSession = false;
            __try { Esp::InvalidateCaches(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.InvalidateCaches"); }
        }

 // Isolate each draw path - SEH mid-menu can leave BeginChild open; EndFrame recovers.
        {
            LARGE_INTEGER ts; if (g_traceEnabled) QueryPerformanceCounter(&ts);
            __try { games8Th.renderer.hud.render(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.hud"); }
            if (g_traceEnabled) TraceSpan("present.hud", ts, 3.0);
        }
        {
            LARGE_INTEGER ts; if (g_traceEnabled) QueryPerformanceCounter(&ts);
            __try { games8Th.renderer.menu.render(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.menu"); }
            if (g_traceEnabled) TraceSpan("present.menu", ts, 3.0);
        }
        {
            LARGE_INTEGER ts; if (g_traceEnabled) QueryPerformanceCounter(&ts);
            __try { games8Th.renderer.visuals.esp(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.esp"); }
            if (g_traceEnabled) TraceSpan("present.esp", ts, 3.0);
        }
        if (!leaving && Config::sound_esp && !H::SessionPostMatch()) {
            LARGE_INTEGER ts; if (g_traceEnabled) QueryPerformanceCounter(&ts);
            __try { SoundEsp::Draw(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.sound_esp"); }
            if (g_traceEnabled) TraceSpan("present.sound_esp", ts, 3.0);
        }
        if (!leaving && Config::weather && Config::weather_mode >= 1 && Config::weather_mode <= 4) {
            LARGE_INTEGER ts; if (g_traceEnabled) QueryPerformanceCounter(&ts);
            __try { World::Weather::Draw(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("Present.weather"); }
            if (g_traceEnabled) TraceSpan("present.weather", ts, 3.0);
        }

        __try {
            ImGui::Render();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Con::Seh("hkPresent.ImGuiRender", GetExceptionCode());
            __try { ImGui::EndFrame(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        imguiFrameOpen = false;

        if (pContext && mainRenderTargetView) {
            pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("hkPresent", GetExceptionCode());
 // Never leave the lock held (deadlock next WndProc)
        if (imguiLocked) {
            ReleaseSRWLockExclusive(&g_imguiInputLock);
            imguiLocked = false;
        }
 // NewFrame without EndFrame/Render -> next frame asserts imgui.cpp:10942
        if (imguiFrameOpen) {
            __try {
                ImGui::EndFrame();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            imguiFrameOpen = false;
        }
    }

    g_imguiInPresent.store(false, std::memory_order_release);
    return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
 // Never call null trampoline (MH re-inject / partial install) + unload
 // passthrough BEFORE arming: teardown path must not bump hook depth.
    if (!oPresent)
        return S_OK;
    if (H::Hooks::s_unloading.load(std::memory_order_acquire))
        return oPresent(pSwapChain, SyncInterval, Flags);
    // RAII depth guard so WaitForQuiescence sees in-flight Present (unload drain).
    // Owns Enter/Leave so the inner body stays SEH-only (no unwinding objects).
    PresentScopeGuard s_presentGuard;
    s_presentGuard.m_armed = true;
    H::Hooks::EnterHook();
    return hkPresentInner(pSwapChain, SyncInterval, Flags);
}


#ifdef _DEBUG
void init_console() {
    static bool s_inited = false;
    if (s_inited)
        return;
    s_inited = true;

 // Console alloc - skip heavy buffer resize (was slow on inject)
    (void)::AllocConsole();

    FILE* conOut = nullptr;
    freopen_s(&conOut, "CONOUT$", "w", stdout);
    freopen_s(&conOut, "CONOUT$", "w", stderr);

 // Light scrollback bump only (no Get/SetConsoleScreenBufferInfoEx palette dance)
    if (HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); hOut && hOut != INVALID_HANDLE_VALUE) {
        COORD sz{ 120, 2000 };
        SetConsoleScreenBufferSize(hOut, sz);
    }

    char logDir[MAX_PATH]{};
    char logPath[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableA("USERPROFILE", logDir, sizeof(logDir));
    if (envLen > 0 && envLen < sizeof(logDir)) {
        strcat_s(logDir, "\\Documents\\Games8Th");
        CreateDirectoryA(logDir, nullptr);

 // One rolling file only - no dated copy + alias (faster open, less clutter)
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE, "%s\\Games8Th.log", logDir);
        fopen_s(&g_logFile, logPath, "w");
    }

    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hConsole == INVALID_HANDLE_VALUE)
        g_hConsole = nullptr;

    SetConsoleTitleW(L"Games8Th DEBUG");
    SetConsoleOutputCP(CP_UTF8);

    Con::Init(g_hConsole, g_logFile);
    initDebug();

    Con::BootBanner(logPath[0] ? logPath : nullptr,
        H::IsolationLevel(), GetCurrentProcessId());
}
#else
void init_console() {}
#endif

// Present path: gameoverlayrenderer64 PresentOverlay via SafetyHook.
static void UninstallPresent()
{
	g_presentHook.reset();
	oPresent = nullptr;
}

static bool TryInstallPresent()
{
	if (g_presentHook && oPresent)
		return true;

	if (!GetModuleHandleA("gameoverlayrenderer64.dll"))
		return false;

	// PresentOverlay - gameoverlayrenderer64 hook
	constexpr const char* kOverlayPresentPat =
		"48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 54 41 56 41 57 48 83 EC 20 41 8B F0";

	auto* fn = M::FindPattern("gameoverlayrenderer64.dll", kOverlayPresentPat);
	if (!fn) {
		Con::Warn("PresentOverlay pattern miss in gameoverlayrenderer64");
		return false;
	}

	g_presentHook.reset();
	auto result = safetyhook::InlineHook::create(fn, reinterpret_cast<void*>(&hkPresent));
	if (!result) {
		Con::Error("PresentOverlay SafetyHook create failed type=%d @ %p",
			static_cast<int>(result.error().type), fn);
		return false;
	}

	g_presentHook = std::move(*result);
	oPresent = g_presentHook.original<Present>();
	if (!oPresent) {
		g_presentHook.reset();
		Con::Error("PresentOverlay hook ok but original null");
		return false;
	}

	Con::Ok("Present via gameoverlayrenderer64 SafetyHook @ %p", fn);
	return true;
}

DWORD WINAPI MainThread(LPVOID lpReserved);

static bool WaitAllModules(const char* const* names, int count, DWORD timeoutMs)
{
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	for (;;) {
		bool all = true;
		for (int i = 0; i < count; ++i) {
			if (!GetModuleHandleA(names[i])) {
				all = false;
				break;
			}
		}
		if (all)
			return true;
		if (GetAsyncKeyState(VK_F4) & 0x8000)
			return false;
		if (GetTickCount64() >= deadline)
			return false;
		Sleep(50);
	}
}

static bool StartMainThread(HMODULE hMod)
{
	HANDLE th = CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
	if (!th)
		th = CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
	if (!th)
		return false;
	CloseHandle(th);
	return true;
}

static void MainThreadBody(LPVOID lpReserved)
{
	const char* kNeed[] = {
		"d3d11.dll",
		"client.dll",
		"engine2.dll",
		"scenesystem.dll",
		"particles.dll",
		"schemasystem.dll",
		"tier0.dll",
		"inputsystem.dll",
		"materialsystem2.dll",
	};
	if (!WaitAllModules(kNeed, (int)(sizeof(kNeed) / sizeof(kNeed[0])), 45000)) {
		if (!GetModuleHandleA("d3d11.dll") || !GetModuleHandleA("client.dll")) {
			if (g_ManualMapped)
				ExitThread(0);
			FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), 0);
			return;
		}
	}

	init_console();
	CrashCapture::Install();

	// AntiTamper installs in Hooks::init (Andromeda 2nd-queue VAC check).

	// Wait for Steam overlay only - no kiero fallback
	for (int w = 0; w < 300 && !GetModuleHandleA("gameoverlayrenderer64.dll"); ++w) {
		if (GetAsyncKeyState(VK_F4) & 0x8000)
			break;
		Sleep(100);
	}

	bool init_hook = false;
	int hookFails = 0;
	do {
		if (TryInstallPresent()) {
			init_hook = true;
			break;
		}
		++hookFails;
		Sleep(200);
	} while (hookFails < 150 && !(GetAsyncKeyState(VK_F4) & 0x8000));

	if (!init_hook) {
		Con::Error("Present install failed - gameoverlayrenderer64 only (no kiero)");
		if (g_ManualMapped)
			ExitThread(0);
		else
			FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), 0);
		return;
	}

	while (!GetAsyncKeyState(VK_F4))
		Sleep(50);

	// Full unload: all game hooks BEFORE FreeLibrary (was Present-only -> post-unload AV).
	if (oWndProc != nullptr) {
		SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
		oWndProc = nullptr;
		// WndProc now tracks depth - drain message-thread frames before unmap.
		H::Hooks::WaitForQuiescence(100);
	}

	SecureAllow::Uninstall();
	// Graceful teardown: feature shutdowns + hook quiescence before trampoline free
	__try { games8Th.shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	// Let in-flight hook calls finish before trampoline free (shutdown already waits 250+100ms)
	H::Hooks::WaitForQuiescence(150);
	UninstallPresent();
	__try { CrashCapture::Uninstall(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (g_ManualMapped) {
		__try { SehSupport::UnregisterExceptionTable(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

#ifdef _DEBUG
	Con::Stats();
	Con::Shutdown();
	if (g_logFile) {
		fclose(g_logFile);
		g_logFile = nullptr;
	}
	g_hConsole = nullptr;
	::FreeConsole();
#endif
}

DWORD WINAPI MainThread(LPVOID lpReserved)
{
	__try {
		MainThreadBody(lpReserved);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("MainThread top-level", GetExceptionCode());
	}

	if (g_ManualMapped)
		ExitThread(EXIT_SUCCESS);
	else
		FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), EXIT_SUCCESS);

	return TRUE;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)DllMain, &mbi, sizeof(mbi)))
            hMod = static_cast<HMODULE>(mbi.AllocationBase);

        g_OurModule = hMod;
        g_ManualMapped = !IsModuleInPEB(hMod);

 // *** CRITICAL: Manual map support setup ***
 // Under manual map, the loader doesn't:
 // 1. Register our .pdata (exception directory) - any __try crashes
 // 2. Initialize the /GS security cookie - any /GS function crashes
 // 3. Run the CRT entry (static initializers) when the mapper calls
 // DllMain / the ManualMapEntry export directly instead of the PE
 // entry point - global objects (games8Th, Config, ImGui fonts)
 // stay zero-init -> vtable-NULL crash on first use
 // 4. Set up the TLS directory - thread_local reads garbage (manual-map has no TLS;
 // codebase uses static/TID-guard instead of thread_local - see hooks.cpp)
 // 5. Handle DisableThreadLibraryCalls (crashes walking PEB)
 // Order: .pdata FIRST (every __try below needs it), then static init,
 // then the GS cookie (initializers see a consistent sentinel cookie).
        if (g_ManualMapped) {
            SehSupport::RegisterExceptionTable(hMod);
            SehSupport::RunStaticInitializersIfNeeded();
            SehSupport::InitializeSecurityCookie();
        } else {
            DisableThreadLibraryCalls(hMod);
        }

        StartMainThread(hMod);
        break;
    }
    case DLL_PROCESS_DETACH:
 // Loader lock held here - NO blocking teardown (shutdown's Sleep loops +
 // SafetyHook thread-freeze can deadlock against hook threads that need the
 // loader lock). Just signal unload + free the Present trampoline; full
// ordered teardown stays on MainThread/F4. shutdown() is idempotent.
        {
            H::Hooks::s_unloading.store(true, std::memory_order_release);
            if (!g_ManualMapped) {
                __try { CrashCapture::Uninstall(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        UninstallPresent();
        break;
    }
    return TRUE;
}

// --- Manual map alternative entry point ---
// Many manual mappers call a exported function or shellcode stub that
// jumps directly here instead of going through DllMain. This handles that case.
// The mapper passes the base address of the mapped image as the parameter.
extern "C" __declspec(dllexport) void ManualMapEntry(HMODULE hBase)
{
    if (g_OurModule != nullptr)
        return; // Already initialized via DllMain

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)ManualMapEntry, &mbi, sizeof(mbi)))
        hBase = static_cast<HMODULE>(mbi.AllocationBase);

    g_OurModule = hBase;
    g_ManualMapped = true;

 // Set up manual map support BEFORE anything using SEH runs. Order: .pdata
 // first (__try needs it), then static init (direct DllMain/export entry
 // skips the CRT - `games8Th` would be vtable-NULL), then the GS cookie.
    SehSupport::RegisterExceptionTable(hBase);
    SehSupport::RunStaticInitializersIfNeeded();
    SehSupport::InitializeSecurityCookie();

    if (!StartMainThread(hBase))
        return;
}



