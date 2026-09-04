#include "Games8Th.h"
#include "utils/module/module.h"
#include "utils/console/console.h"
#include "utils/security/crashdump.h"
#include "utils/cvar/cvar.h"
#include "features/prediction/prediction.h"
#include "features/skinchanger/skinchanger.h"
#include "features/skinchanger/skin_preview.h"
#include "features/hitmarker/hitmarker.h"
#include "features/bullet_impact/bullet_impact.h"
#include "features/hitsound/hitsound.h"
#include "features/grenade_helper/grenade_helper.h"
#include "features/world/weather.h"
#include "features/world/fog_handler.h"
#include "features/custom_paint/custom_paint.h"
#include <cstdio>
#include <future>
// Defined in main.cpp
extern HMODULE g_OurModule;
extern bool    g_ManualMapped;
bool Games8Th::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView) {
	CrashCapture::Install();
	Con::ScopedTimer initTimer("Games8Th::init", 0);
 // Schema parse from schemasystem.dll is independent of ImGui/D3D bring-up; // fire it on a worker thread while the menu builds its font atlas + IB/VB. // interfaces/hooks depend on schema offsets, so we join before those two. 
auto schemaFut = std::
async(std::
launch::
async, [this] {        return schema.init("client.dll", 0);    });
    Con::
Tag("init", Con::
Level::
Info, "menu");
    renderer.menu.init(window, pDevice, pContext, mainRenderTargetView);
    Con::
Tag("init", Con::
Level::
Info, "modules");
    modules.init();
 // Eager prediction resolution - the first bhop/jumpbug frame used to
 // pattern-scan client.dll (~15 scans) inside CreateMove on the game thread
 // (seconds-long hitch right when the user starts hopping).
    Con::
Tag("init", Con::
Level::
Info, "prediction");
    Pred::
Init();
 // Eager convar resolution - Cvar::Float scans the whole client.dll image
 // on first use; the aimbot's first frame (sensitivity for mouse-count
 // quantization) used to hitch for seconds right after inject.
    Con::
Tag("init", Con::
Level::
Info, "cvar");
    Cvar::
Warmup();
    Con::
Tag("init", Con::
Level::
Info, "schema");
    bool schemaOk = false;
    try {
        schemaOk = schemaFut.get();
    } catch (...) {
        Con::Tag("init", Con::Level::Error, "schema task raised an exception");
        shutdown();
        return false;
    }
    if (!schemaOk) {
        Con::Tag("init", Con::Level::Error, "schema FAILED - aborting init");
        shutdown();
        return false;
    }
    Con::
Tag("init", Con::
Level::
Info, "interfaces");
    const bool ifaceOk = interfaces.init();
    if (!ifaceOk) {
        Con::Tag("init", Con::Level::Error, "interfaces FAILED - aborting init (features stay disabled)");
        shutdown();
        return false;
    }
    Con::
Tag("init", Con::
Level::
Info, "visuals");
    renderer.visuals.init();
    Con::
Tag("init", Con::
Level::
Info, "skins");
    SkinChanger::Init();
    Con::Tag("init", Con::Level::Info, "custom_paint");
    CustomPaint::Init();
    Con::
Tag("init", Con::
Level::
Info, "hooks");
    const bool hooksOk = hooks.init();
    if (!hooksOk) {
        Con::Tag("init", Con::Level::Error, "hooks FAILED - rolling back");
        shutdown();
        return false;
    }
    Con::
Tag("init", Con::
Level::
Ok, "init ok");
    Con::
QuietBootEnd();
    Con::
Stats();
    return true;
}

void Games8Th::shutdown() noexcept {
    __try { hooks.shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { GetSkinPreview().Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { Hitmarker::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { BulletFx::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { Hitsound::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { GrenadeHelper::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { World::Weather::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { World::Fog::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { CustomPaint::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { renderer.menu.shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}


