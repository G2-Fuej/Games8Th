#include "crashdump.h"

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

extern HMODULE g_OurModule;

namespace {

constexpr DWORD kStatusStackBufferOverrun = 0xC0000409u;
constexpr DWORD kStatusHeapCorruption     = 0xC0000374u;
constexpr DWORD kStatusFailFastException  = 0xC0000602u;
constexpr DWORD kStatusInvalidCrtParam    = 0xC0000417u;
constexpr DWORD kStatusFatalUserCallback  = 0xC000041Du;
constexpr DWORD kStatusAssertionFailure   = 0xC0000420u;
constexpr DWORD kStatusFatalAppExit       = 0x40000015u;
constexpr DWORD kMsThreadName             = 0x406D1388u;

char g_dir[MAX_PATH]{};
bool g_dirReady = false;
PVOID g_veh = nullptr;
PVOID g_vch = nullptr;
volatile LONG g_busy = 0;
volatile LONG64 g_lastLogMs = 0;

bool ClaimLogSlot()
{
	const LONG64 now = static_cast<LONG64>(GetTickCount64());
	for (;;) {
		const LONG64 last = g_lastLogMs;
		if (last != 0 && now - last < 1000)
			return false;
		if (InterlockedCompareExchange64(&g_lastLogMs, now, last) == last)
			return true;
	}
}

void EnsureDir()
{
	if (g_dirReady)
		return;
	char user[MAX_PATH]{};
	DWORD n = GetEnvironmentVariableA("USERPROFILE", user, MAX_PATH);
	if (n > 0 && n < MAX_PATH - 32) {
		_snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s\\Documents\\Games8Th", user);
		char docs[MAX_PATH]{};
		_snprintf_s(docs, sizeof(docs), _TRUNCATE, "%s\\Documents", user);
		CreateDirectoryA(docs, nullptr);
		CreateDirectoryA(g_dir, nullptr);
		g_dirReady = true;
		return;
	}
	_snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s\\Games8Th",
		GetEnvironmentVariableA("LOCALAPPDATA", user, MAX_PATH) ? user : "C:");
	CreateDirectoryA(g_dir, nullptr);
	g_dirReady = true;
}

bool Noise(DWORD code)
{
	return code == 0x80000001u
		|| code == EXCEPTION_BREAKPOINT
		|| code == EXCEPTION_SINGLE_STEP
		|| code == kMsThreadName;
}

bool FailFast(DWORD code)
{
	return code == kStatusStackBufferOverrun
		|| code == kStatusHeapCorruption
		|| code == kStatusFailFastException
		|| code == kStatusInvalidCrtParam
		|| code == kStatusFatalUserCallback
		|| code == kStatusAssertionFailure
		|| code == kStatusFatalAppExit
		|| code == EXCEPTION_STACK_OVERFLOW;
}

const char* CodeName(DWORD code)
{
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:    return "AV";
	case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILL";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "DIV0";
	case EXCEPTION_STACK_OVERFLOW:      return "STACK";
	case kStatusStackBufferOverrun:     return "GS";
	case kStatusHeapCorruption:         return "HEAP";
	case kStatusFailFastException:      return "FAILFAST";
	default:                            return "EX";
	}
}

bool InOurModule(const void* addr)
{
	if (!addr || !g_OurModule)
		return false;
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(addr), &mod) && mod == g_OurModule)
		return true;
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(g_OurModule, &mbi, sizeof(mbi)) || !mbi.AllocationBase)
		return false;
	const auto base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
	std::size_t span = mbi.RegionSize;
	__try {
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.SizeOfImage)
				span = nt->OptionalHeader.SizeOfImage;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	const auto a = reinterpret_cast<uintptr_t>(addr);
	return a >= base && a < base + span;
}

void Append(const char* text)
{
	if (!text || !text[0])
		return;
	EnsureDir();
	char path[MAX_PATH]{};
	_snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\crash.log", g_dir);
	HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;
	DWORD w = 0;
	WriteFile(h, text, static_cast<DWORD>(strlen(text)), &w, nullptr);
	CloseHandle(h);
}

void ModName(const void* addr, char* out, size_t outSz)
{
	out[0] = 0;
	if (!addr || !out || outSz < 8)
		return;
	HMODULE mod = nullptr;
	if (!GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(addr), &mod) || !mod)
		return;
	char path[MAX_PATH]{};
	if (!GetModuleFileNameA(mod, path, MAX_PATH))
		return;
	const char* slash = strrchr(path, '\\');
	_snprintf_s(out, outSz, _TRUNCATE, "%s+0x%X",
		slash ? slash + 1 : path,
		static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
}

void LogFault(const char* tag, EXCEPTION_POINTERS* ep)
{
	if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
		return;
	if (!ClaimLogSlot()) {
		InterlockedExchange(&g_busy, 0);
		return;
	}

	const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
	const CONTEXT* ctx = ep ? ep->ContextRecord : nullptr;
	const DWORD code = er ? er->ExceptionCode : 0;
	const void* fault = er ? er->ExceptionAddress : nullptr;

	char mod[80]{};
	ModName(fault, mod, sizeof(mod));

	char buf[1536]{};
	int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"[%s] pid=%lu tid=%lu %s code=0x%08X addr=%p rip=%p %s\n",
		tag, GetCurrentProcessId(), GetCurrentThreadId(), CodeName(code), code, fault,
		ctx ? reinterpret_cast<void*>(ctx->Rip) : nullptr,
		mod[0] ? mod : "?");

	if (er && code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && n > 0) {
		char av[160]{};
		_snprintf_s(av, sizeof(av), _TRUNCATE, "  av=%s va=%p\n",
			er->ExceptionInformation[0] ? "write" : "read",
			reinterpret_cast<const void*>(er->ExceptionInformation[1]));
		n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "%s", av);
	}

	if (code != EXCEPTION_STACK_OVERFLOW && n > 0) {
		void* frames[8]{};
		const USHORT nf = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
		if (nf) {
			n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "  stack:");
			for (USHORT i = 0; i < nf && n > 0; ++i) {
				char fm[80]{};
				ModName(frames[i], fm, sizeof(fm));
				n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, " %s",
					fm[0] ? fm : "?");
			}
			_snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "\n");
		}
	}

	Append(buf);
	InterlockedExchange(&g_busy, 0);
}

LONG CALLBACK OnContinue(EXCEPTION_POINTERS* ep)
{
	const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
	if (Noise(code))
		return EXCEPTION_CONTINUE_SEARCH;
	// Unhandled: log even when RIP is in client.dll / scenesystem.
	LogFault("unhandled", ep);
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK OnFirstChance(EXCEPTION_POINTERS* ep)
{
	const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
	if (Noise(code))
		return EXCEPTION_CONTINUE_SEARCH;
	const void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	// Access violations are used as a recoverable probe by CS2 and by our
	// stale-entity guards. Do not synchronously log them from the render
	// thread; an unrecovered fault is still captured by OnContinue below.
	if (FailFast(code) || (code != EXCEPTION_ACCESS_VIOLATION && InOurModule(addr)))
		LogFault("veh", ep);
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace CrashCapture {

void Install()
{
	EnsureDir();
	if (!g_veh)
		g_veh = AddVectoredExceptionHandler(1, OnFirstChance);
	if (!g_vch)
		g_vch = AddVectoredContinueHandler(1, OnContinue);
}

void Uninstall()
{
	if (g_vch) {
		RemoveVectoredContinueHandler(g_vch);
		g_vch = nullptr;
	}
	if (g_veh) {
		RemoveVectoredExceptionHandler(g_veh);
		g_veh = nullptr;
	}
}

} // namespace CrashCapture


