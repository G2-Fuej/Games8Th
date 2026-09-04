#include "patternscan.h"

#include "../../module/module.h"
#include "../../console/console.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Module resolve
//
// Call sites mix short registry names ("client") and PE names ("client.dll").
// GetModuleHandleA("client") fails - game loads as "client.dll".
// patternScan used Modules registry (short OK); FindPattern/scan did not -> miss.
// ---------------------------------------------------------------------------
static HMODULE ResolveModuleHandle(
const char* name)
{
	if (!name || !name[0])
		return nullptr;

	// 1) Modules registry (short keys: "client", "engine2", ...)
	if (const uintptr_t reg = modules.getModule(name))
		return reinterpret_cast<HMODULE>(reg);

	// 2) Exact PE name
	if (HMODULE h = GetModuleHandleA(name))
		return h;

	char buf[160]{};
	const size_t len = strnlen(name, 140);
	if (len == 0 || len >= 140)
		return nullptr;

	const bool hasDot = (strchr(name, '.') != nullptr);

	// 3) short -> short.dll
	if (!hasDot) {
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s.dll", name);
		if (HMODULE h = GetModuleHandleA(buf))
			return h;
		// registry already tried exact short; also try after dll form via getModule base name
		if (const uintptr_t reg = modules.getModule(name))
			return reinterpret_cast<HMODULE>(reg);
		return nullptr;
	}

	// 4) "client.dll" -> strip .dll -> registry short name
	if (len > 4) {
		const char* ext = name + (len - 4);
		if (_stricmp(ext, ".dll") == 0) {
			memcpy(buf, name, len - 4);
			buf[len - 4] = '\0';
			if (const uintptr_t reg = modules.getModule(buf))
				return reinterpret_cast<HMODULE>(reg);
			if (HMODULE h = GetModuleHandleA(buf))
				return h;
		}
	}

	return nullptr;
}

static bool GetModuleBounds(HMODULE mod, uintptr_t& outBase, size_t& outSize)
{
	outBase = 0;
	outSize = 0;
	if (!mod)
		return false;

	outBase = reinterpret_cast<uintptr_t>(mod);

	MODULEINFO mi{};
	if (GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))
		&& mi.SizeOfImage > 0) {
		outBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll ? mi.lpBaseOfDll : mod);
		outSize = static_cast<size_t>(mi.SizeOfImage);
		return true;
	}

	// Fallback: PE headers (works when Psapi fails)
	__try {
		const auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
			reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		outSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
		return outSize > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// pair: (byte, must_match) - must_match=false -> wildcard
std::
vector<std::
pair<uint8_t, bool>> PatternToBytes(const std::
string& pattern)
{
	std::
vector<std::
pair<uint8_t, bool>> patternBytes;
	const char* start = pattern.c_str();
	const char* end = start + pattern.size();

	for (const char* current = start; current < end; ++current) {
		if (*current == ' ')
			continue;
		if (*current == '?') {
			patternBytes.emplace_back(0, false);
			if (current + 1 < end && *(current + 1) == '?')
				++current;
		} else {
			if (!std::
isxdigit(static_cast<unsigned char>(*current)))
				continue;
			char* next = nullptr;
			const auto val = static_cast<uint8_t>(strtoul(current, &next, 16));
			patternBytes.emplace_back(val, true);
			// strtoul advances past hex digits; loop will ++current so step back one
			if (next && next > current)
				current = next - 1;
		}
	}

	return patternBytes;
}

// pair: (byte, is_wildcard) - opposite flag of PatternToBytes (legacy FindPattern)
std::
vector<std::
pair<uint8_t, bool>> PatternToBytesWildcard(const std::
string& pattern)
{
	std::
vector<std::
pair<uint8_t, bool>> out;
	std::
stringstream stream(pattern);
	std::
string token;
	while (stream >> token) {
		if (token.empty())
			continue;
		if (token[0] == '?') {
			out.emplace_back(0u, true);
			continue;
		}
		if (token.size() < 2
			|| !std::
isxdigit(static_cast<unsigned char>(token[0]))
			|| !std::
isxdigit(static_cast<unsigned char>(token[1])))
			continue;
		out.emplace_back(static_cast<uint8_t>(std::
strtoul(token.data(), nullptr, 16)), false);
	}
	return out;
}

static uintptr_t patternScanRaw(uintptr_t base, size_t size, const std::
pair<uint8_t, bool>* pattern, size_t len)
{
	if (!base || !pattern || len == 0 || size < len)
		return 0;

	const size_t last = size - len;
	__try {
		for (size_t i = 0; i <= last; ++i) {
			bool found = true;
			for (size_t j = 0; j < len; ++j) {
				// second=true -> must match byte
				if (pattern[j].second
					&& pattern[j].first != *reinterpret_cast<uint8_t*>(base + i + j)) {
					found = false;
					break;
				}
			}
			if (found)
				return base + i;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::
Seh("patternScan scan", GetExceptionCode());
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Pattern-scan result cache + async warmup.
//
// Bulk scans dominate Hooks::init (~30 patterns * ~50ms on client.dll ~= 1.5s
// sequential). WarmupPatternsAsync fires them all through std::async, which the
// runtime distributes across worker threads. Cache is keyed by (module|pattern)
// so subsequent calls to patternScan are constant-time lookups. Miss re-scans
// and stores (still ~50ms once, then cached).
// ---------------------------------------------------------------------------
namespace {
	std::
mutex g_patCacheMtx;
	std::
unordered_map<std::
string, uintptr_t> g_patCache;

	std::
mutex g_patFutMtx;
	std::
vector<std::
future<void>> g_patFuts;

	// Cheap composite key. Normalize module so "client" and "client.dll" share cache.
	std::string NormMod(std::string_view name)
	{
		size_t slash = name.find_last_of("\\/");
		if (slash != std::string_view::npos)
			name = name.substr(slash + 1);
		std::string s(name);
		if (s.size() > 4 && _stricmp(s.c_str() + (s.size() - 4), ".dll") == 0)
			s.resize(s.size() - 4);
		for (char& c : s) {
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c + 32);
		}
		return s;
	}

	std::string PatKey(const std::string& mod, const std::string& pat)
	{
		std::string k = NormMod(mod);
		k.push_back('|');
		k.append(pat);
		return k;
	}

	uintptr_t ScanUncached(const std::
string& module, const std::
string& pattern)
	{
		HMODULE hModule = ResolveModuleHandle(module.c_str());
		if (!hModule)
			return 0;
		uintptr_t baseAddress = 0;
		size_t moduleSize = 0;
		if (!GetModuleBounds(hModule, baseAddress, moduleSize))
			return 0;
		const auto patternBytes = PatternToBytes(pattern);
		if (patternBytes.empty())
			return 0;
		return patternScanRaw(
			baseAddress, moduleSize, patternBytes.data(), patternBytes.size());
	}
}

uintptr_t M::
patternScan(const std::
string& module, const std::
string& pattern)
{
	const std::
string key = PatKey(module, pattern);
	{
		std::
lock_guard<std::
mutex> lk(g_patCacheMtx);
		if (const auto it = g_patCache.find(key); it != g_patCache.end()) {
			// Never sticky-cache a miss - warmup race / partial map can store 0
			// and permanently kill later real hits (create_effect false miss).
			if (it->second)
				return it->second;
		}
	}
	const uintptr_t result = ScanUncached(module, pattern);
	if (result) {
		std::lock_guard<std::mutex> lk(g_patCacheMtx);
		g_patCache[key] = result;
	}
	return result;
}

void M::
WarmupPatternsAsync(
	std::
initializer_list<std::
pair<const char*, const char*>> patterns)
{
	std::
lock_guard<std::
mutex> lk(g_patFutMtx);
	g_patFuts.reserve(g_patFuts.size() + patterns.size());
	for (const auto& [mod, pat] : patterns) {
		if (!mod || !pat)
			continue;
		std::
string mods = mod, pats = pat;
		g_patFuts.emplace_back(std::
async(std::
launch::
async,
			[mods = std::
move(mods), pats = std::
move(pats)]() {
				const std::
string key = PatKey(mods, pats);
				// Skip if already scanned with a HIT (dedupe). Misses are not sticky.
				{
					std::
lock_guard<std::
mutex> lk2(g_patCacheMtx);
					if (const auto it = g_patCache.find(key); it != g_patCache.end() && it->second)
						return;
				}
				const uintptr_t r = ScanUncached(mods, pats);
				if (r) {
					std::lock_guard<std::mutex> lk2(g_patCacheMtx);
					g_patCache[key] = r;
				}
			}));
	}
}

void M::
WaitForWarmup()
{
	std::
vector<std::
future<void>> drain;
	{
		std::
lock_guard<std::
mutex> lk(g_patFutMtx);
		drain.swap(g_patFuts);
	}
	for (auto& f : drain) {
		if (f.valid())
			f.wait();
	}
}

uint8_t* M::scan(const char* module_name, const char* pattern)
{
	if (!module_name || !pattern)
		return nullptr;
	const uintptr_t r = patternScan(module_name, pattern);
	return r ? reinterpret_cast<uint8_t*>(r) : nullptr;
}

std::uint8_t* M::FindPattern(const char* module_name, const std::string& byte_sequence)
{
	const uintptr_t r = patternScan(module_name ? module_name : "", byte_sequence);
	return r ? reinterpret_cast<std::uint8_t*>(r) : nullptr;
}


