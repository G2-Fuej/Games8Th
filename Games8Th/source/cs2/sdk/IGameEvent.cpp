#include "IGameEvent.h"
#include "../../Games8Th/utils/memory/patternscan/patternscan.h"
#include "../../Games8Th/utils/memory/gaa/gaa.h"
#include "../../Games8Th/utils/console/console.h"
#include "../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../cs2/entity/handle.h"
#include "../../Games8Th/interfaces/interfaces.h"
#include "../../Games8Th/utils/memory/memsafe/memsafe.h"
#include <Windows.h>
namespace {    using FnEvtGetName = const char* (__fastcall*)(void*);
    using FnEvtGetInt64 = int64_t (__fastcall*)(void*, const char*, int64_t);
    using FnEvtGetController = CCSPlayerController* (__fastcall*)(void*, void*);
    using FnEvtGetString = const char* (__fastcall*)(void*, void*, const char*);
    // The client wrapper takes (event, key, defaultValue).
    using FnEvtGetFloat = float (__fastcall*)(void*, const char*, float);
    using FnEvtSetString = void (__fastcall*)(void*, void*, const char*, unsigned char);
    FnEvtGetName g_fnGetName = nullptr;
    FnEvtGetInt64 g_fnGetInt64 = nullptr;
    FnEvtGetController g_fnGetController = nullptr;
    FnEvtGetString g_fnGetString = nullptr;
    FnEvtGetFloat g_fnGetFloat = nullptr;
    FnEvtSetString g_fnSetString = nullptr;
    bool g_inited = false;
}
void IGameEvent::
InitPatterns() {    if (g_inited) return;
 // patterns for IGameEvent accessors
    uintptr_t pName = M::
patternScan("client", "8B 41 14 0F BA E0 1E 73 05 48 8D 41 18 C3");
    if (!pName) pName = M::
patternScan("client", "8B 41 ? 0F BA E0 ? 73 ? 48 8D 41");
    if (pName) g_fnGetName = reinterpret_cast<FnEvtGetName>(pName);
    uintptr_t pInt = M::
patternScan("client", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 01 41 8B F0");
    if (!pInt) pInt = M::
patternScan("client", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 01 41 8B F0");
    if (pInt) g_fnGetInt64 = reinterpret_cast<FnEvtGetInt64>(pInt);
    uintptr_t pCtrl = M::
patternScan("client", "48 83 EC 38 8B 02 4C 8D 44 24 20");
    if (!pCtrl) pCtrl = M::
patternScan("client", "48 83 EC ? 8B 02 4C 8D 44 24 ?");
    if (pCtrl) g_fnGetController = reinterpret_cast<FnEvtGetController>(pCtrl);
    // Unique dump GetString (patterns.json) - short prefix hits 7 thunks, first is NOT GetString.
    uintptr_t pStr = M::
patternScan("client", "48 83 EC 38 8B 02 48 83 C1 58 89 44 24 20 8B 42 04 89 44 24 24 48 8B 42 08 48 8D 54 24 20 48 89 44 24 28 E8 ? ? ? ? 48 83 C4 38 C3 CC CC CC 33 C9");
    if (pStr) g_fnGetString = reinterpret_cast<FnEvtGetString>(pStr);
    uintptr_t pSetStr = M::
patternScan("client", "48 83 EC 38 8B 02 48 83 C1 58 89 44 24 20 41 B1 1A");
    if (pSetStr) g_fnSetString = reinterpret_cast<FnEvtSetString>(pSetStr);
 // game_event_get_float: E8 rel32 ; movaps xmm3, xmm0 ; mov [rsp+20], ebx
 // target = sub_180B49C30 - hashes name, calls vt[+72] (slot 9) with token only.
    uintptr_t pFloatCall = M::patternScan("client", "E8 ?? ?? ?? ?? 0F 28 D8 89 5C 24 20");
    if (pFloatCall)
        g_fnGetFloat = reinterpret_cast<FnEvtGetFloat>(M::getAbsoluteAddress(pFloatCall, 1));
    g_inited = true;
    Con::
Ok("IGameEvent SDK: GetName=%s GetInt64=%s GetCtrl=%s GetString=%s SetString=%s GetFloat=%s",        g_fnGetName ? "ok" : "miss",        g_fnGetInt64 ? "ok" : "miss",        g_fnGetController ? "ok" : "miss",        g_fnGetString ? "ok" : "miss",        g_fnSetString ? "ok" : "miss",        g_fnGetFloat ? "ok" : "miss");
}
const char* IGameEvent::
GetName() {
    if (!this || !Mem::IsUserPtr(this)) return nullptr;

    // Source 2 exposes GetName as vtable slot 1. The old CBufferString
    // pattern returns an internal buffer for some event types and can make
    // bullet_impact silently disappear.
    void** vtable = nullptr;
    FnEvtGetName fn = nullptr;
    if (Mem::ReadField(this, 0, vtable)
        && vtable
        && Mem::ReadField(vtable, sizeof(void*), fn)
        && fn) {
        const char* name = nullptr;
        __try { name = fn(this); }
        __except (EXCEPTION_EXECUTE_HANDLER) { name = nullptr; }
        if (name && Mem::IsReadable(name, 1) && name[0])
            return name;
    }

    if (g_fnGetName) {
        __try { return g_fnGetName(this); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return nullptr;
}
int64_t IGameEvent::
GetInt64(const std::
string_view name, int64_t defVal) {    if (!this || !Mem::IsUserPtr(this) || name.empty()) return defVal;
    if (g_fnGetInt64) {        __try { return g_fnGetInt64(this, name.data(), defVal);
 }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return defVal;
}
CCSPlayerController* IGameEvent::
GetPlayerController(const std::
string_view name) {    if (!this || !Mem::IsUserPtr(this) || name.empty()) return nullptr;
    if (g_fnGetController) {

            CUtlStringToken token(name.data());
        __try { return g_fnGetController(this, &token);
 }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    int64_t id = GetInt64(name, -1);
    if (id >= 0 && I::
GameEntity && I::
GameEntity->Instance) {        __try {            return I::
GameEntity->Instance->Get<CCSPlayerController>(static_cast<int>(id & 0x7FFF));
        }
 __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return nullptr;
}
const char* IGameEvent::
GetString(const std::
string_view name, const char* defVal) {    if (!this || !Mem::IsUserPtr(this) || name.empty()) return defVal;
    if (g_fnGetString) {        CUtlStringToken token(name.data());
        __try {            const char* res = g_fnGetString(this, &token, defVal);
            if (res) return res;
        }
 __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return defVal;
}
float IGameEvent::
GetFloat(const std::
string_view name, float defVal) {    if (!this || !Mem::IsUserPtr(this) || name.empty()) return defVal;
    if (g_fnGetFloat) {
        __try { return g_fnGetFloat(this, name.data(), defVal); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
 // Fallback: CGameEvent::GetFloat is vt[+72] = slot 9, (this, CUtlStringToken*) - no defVal.
    __try {
        void** vt = *reinterpret_cast<void***>(this);
        if (vt && vt[9]) {
            CUtlStringToken token(name.data());
            using FnGetFloatVt = float(__fastcall*)(void*, void*);
            return reinterpret_cast<FnGetFloatVt>(vt[9])(this, &token);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return defVal;
}
void IGameEvent::
SetString(const std::
string_view name, const std::
string_view value) {    if (!this || !Mem::IsUserPtr(this) || name.empty() || !g_fnSetString) return;
    CUtlStringToken token(name.data());
    char buf[256];
    const size_t n = value.size() < 255 ? value.size() : 255;
    memcpy(buf, value.data(), n);
    buf[n] = '\0';
    __try { g_fnSetString(this, &token, buf, 0x1A); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

