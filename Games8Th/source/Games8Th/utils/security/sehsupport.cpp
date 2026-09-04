#include "sehsupport.h"
#include <cstdint>

// __security_cookie / __security_cookie_complement - MSVC CRT globals
// Normally initialized by __security_init_cookie() in _DllMainCRTStartup.
// Under manual map with WipeHeader + no CRT init, they remain at sentinel value
// (0x00002B992DDFA232 on x64) which /GS-checked functions will detect as
// "cookie changed" and abort via __report_gsfailure.
extern "C" {
extern uintptr_t __security_cookie;
    extern uintptr_t __security_cookie_complement;
}

// MSVC CRT per-module static initializer table (.CRT$XCU). __xc_a/__xc_z are
// LINKER-GENERATED per-module boundaries - the CRT entry stub walks them via
// initterm(__xc_a, __xc_z). When a manual mapper calls DllMain/ManualMapEntry
// directly the CRT entry never runs, so we walk them ourselves.
extern "C" void (__cdecl* __xc_a[])(void);
extern "C" void (__cdecl* __xc_z[])(void);
extern "C" void __cdecl _initterm(void (__cdecl* pfbegin[])(void), void (__cdecl* pfend[])(void));

// Default MSVC sentinel value on x64 (pre-randomization)
constexpr uintptr_t kDefaultCookie = 0x00002B992DDFA232ULL;

namespace SehSupport {

// Cached registration so unload can undo it - leaving stale RUNTIME_FUNCTION
// entries pointing into freed pages kills cs2 on the next unwind over our range
static PRUNTIME_FUNCTION g_pFuncTable = nullptr;
static DWORD g_funcCount = 0;
static DWORD64 g_imageBase = 0;
static bool g_registered = false;

bool RegisterExceptionTable(void* baseAddr) {
    if (!baseAddr) return false;

    auto imageBase = reinterpret_cast<uint8_t*>(baseAddr);

 // Parse PE headers
    if (IsBadReadPtr(imageBase, sizeof(IMAGE_DOS_HEADER)))
        return false;

    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
 // Header wiped by mapper - we can't find .pdata this way.
 // Fall back: this DLL won't have working SEH under such mappers.
        return false;
    }

    if (IsBadReadPtr(imageBase + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return false;

    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

 // Exception directory (IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3)
    IMAGE_DATA_DIRECTORY exDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exDir.VirtualAddress == 0 || exDir.Size == 0)
        return false;

    auto pFuncTable = reinterpret_cast<PRUNTIME_FUNCTION>(imageBase + exDir.VirtualAddress);
    DWORD count = exDir.Size / sizeof(RUNTIME_FUNCTION);

 // Register with the OS. From this point, __try/__except works.
    if (RtlAddFunctionTable(pFuncTable, count, reinterpret_cast<DWORD64>(imageBase)) == FALSE)
        return false;

    g_pFuncTable = pFuncTable;
    g_funcCount = count;
    g_imageBase = reinterpret_cast<DWORD64>(imageBase);
    g_registered = true;
    return true;
}

void UnregisterExceptionTable() {
    if (!g_registered)
        return;
    RtlDeleteFunctionTable(g_pFuncTable);
    g_pFuncTable = nullptr;
    g_funcCount = 0;
    g_imageBase = 0;
    g_registered = false;
}

void RunStaticInitializersIfNeeded() {
 // If the CRT entry point ran, __security_init_cookie already randomized
 // the cookie -> static init already happened. Never run it twice.
    if (__security_cookie != kDefaultCookie)
        return;
    __try {
        _initterm(__xc_a, __xc_z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
 // Partial init - every downstream call is SEH-guarded; keep going.
    }
}

void InitializeSecurityCookie() {
 // If cookie is at default (sentinel), CRT didn't run - initialize manually
    if (__security_cookie == kDefaultCookie) {
 // Generate a cookie from various entropy sources
        LARGE_INTEGER perf;
        QueryPerformanceCounter(&perf);

        uintptr_t cookie = static_cast<uintptr_t>(perf.QuadPart);
        cookie ^= reinterpret_cast<uintptr_t>(&cookie);         // Stack address entropy
        cookie ^= GetCurrentProcessId();
        cookie ^= GetCurrentThreadId();
        cookie ^= GetTickCount64();

 // Ensure top 16 bits are zero (MSVC requirement)
        cookie &= 0x0000FFFFFFFFFFFFULL;

 // Avoid default sentinel
        if (cookie == kDefaultCookie) cookie ^= 0xDEADBEEF;

        __security_cookie = cookie;
        __security_cookie_complement = ~cookie;
    }
}

} // namespace SehSupport

