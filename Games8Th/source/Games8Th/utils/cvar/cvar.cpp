#include "cvar.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace Cvar {
namespace {

constexpr std::uintptr_t kCvarDataOffset = 0x08;
constexpr std::uintptr_t kCvarValueOffset = 0x58;

struct Resolved {
	const char* name;
	uintptr_t  convar;   // ConVar object (RCX-side of the paired lea)
};

// Convar resolution cache. Names are static strings in the callers
// ("sv_gravity", ...), so storing the pointer (not a copy) is safe.
Resolved g_cache[16]{};
int g_cacheN = 0;

std::uintptr_t ResolveRipRel3(std::uintptr_t insn) {
	const std::int32_t disp = *reinterpret_cast<const std::int32_t*>(insn + 3);
	return insn + 7 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp));
}

std::uintptr_t FindCStringInRange(const char* sz, std::uintptr_t start, std::uintptr_t end) {
	const size_t len = std::strlen(sz) + 1;
	if (end <= start || end - start < len)
		return 0;
	for (std::uintptr_t p = start; p + len <= end; ++p) {
		if (std::memcmp(reinterpret_cast<const void*>(p), sz, len) == 0)
			return p;
	}
	return 0;
}

std::uintptr_t ResolveConVarInModule(HMODULE hMod, const char* name) {
	if (!hMod)
		return 0;
	const auto* pDos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	const auto* pNt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
		reinterpret_cast<std::uintptr_t>(pDos) + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE)
		return 0;

	const std::uintptr_t imageBase = reinterpret_cast<std::uintptr_t>(hMod);
	const std::uintptr_t imageEnd = imageBase + pNt->OptionalHeader.SizeOfImage;
	const std::uintptr_t codeStart = imageBase + pNt->OptionalHeader.BaseOfCode;
	const std::uintptr_t codeEnd = codeStart + pNt->OptionalHeader.SizeOfCode;

	const std::uintptr_t strAddr = FindCStringInRange(name, imageBase, imageEnd);
	if (!strAddr)
		return 0;

	// lea rdx, <name> ; lea rcx, <ConVar>
	for (std::uintptr_t p = codeStart; p + 14 <= codeEnd; ++p) {
		const auto* b = reinterpret_cast<const std::uint8_t*>(p);
		if (b[0] != 0x48 || b[1] != 0x8D || b[2] != 0x15)
			continue;
		if (ResolveRipRel3(p) != strAddr)
			continue;
		const std::uintptr_t leaRcx = p + 7;
		const auto* c = reinterpret_cast<const std::uint8_t*>(leaRcx);
		if (c[0] != 0x48 || c[1] != 0x8D || c[2] != 0x0D)
			continue;
		return ResolveRipRel3(leaRcx);
	}
	return 0;
}

std::uintptr_t ResolveConVar(const char* name) {
	for (int i = 0; i < g_cacheN; ++i) {
		if (g_cache[i].name == name)
			return g_cache[i].convar;
	}

	std::uintptr_t addr = 0;
	HMODULE h = GetModuleHandleA("client.dll");
	if (!h)
		h = GetModuleHandleA("client");
	if (!h)
		h = GetModuleHandleA("engine2.dll");
	if (!h)
		h = GetModuleHandleA("server.dll");
	if (h) {
		addr = ResolveConVarInModule(h, name);
		if (!addr) {
			// Fallback sweep across the same module set.
			const char* mods[] = { "client.dll", "engine2.dll", "server.dll" };
			for (const char* m : mods) {
				if (HMODULE hm = GetModuleHandleA(m)) {
					addr = ResolveConVarInModule(hm, name);
					if (addr)
						break;
				}
			}
		}
	}

	if (g_cacheN < static_cast<int>(sizeof(g_cache) / sizeof(g_cache[0]))) {
		g_cache[g_cacheN].name = name;
		g_cache[g_cacheN].convar = addr;
		++g_cacheN;
	}
	return addr;
}

} // namespace

float Float(const char* name, float fallback) {
	if (!name || !name[0])
		return fallback;

	const std::uintptr_t convar = ResolveConVar(name);
	if (!convar)
		return fallback;

	__try {
		const std::uintptr_t data = *reinterpret_cast<std::uintptr_t*>(convar + kCvarDataOffset);
		if (!data || data < 0x10000ull)
			return fallback;
		const float v = *reinterpret_cast<const float*>(data + kCvarValueOffset);
		return (v == v) ? v : fallback; // NaN guard
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return fallback;
	}
}

void Warmup() {
	// Every convar read on a hot path (aimbot smoothing, movement, subtick).
	// First resolution scans the whole client.dll image - eager here means
	// the first in-game use never hitches.
	Float("sensitivity", 2.5f);
	Float("sv_gravity", 800.f);
	Float("sv_standable_normal", 0.7f);
	Float("sv_autobunnyhopping", 0.f);
	Float("sv_legacy_jump", 0.f);
	Float("sv_airaccelerate", 12.f);
	Float("sv_maxspeed", 250.f);
	Float("sv_air_max_wishspeed", 30.f);
	Float("sv_quantize_movement_input", 1.f);
}

} // namespace Cvar

