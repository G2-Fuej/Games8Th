#pragma once
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <vector>
#include <string>
#include "../../console/console.h"
#include "../memsafe/memsafe.h"

namespace M {
	template <typename T = std::
uint8_t>
	T* abs(T* relative_address, int pre_offset = 0x0, int post_offset = 0x0)
	{
		if (!relative_address) {
			Con::
Error("abs: relative_address is null");
			return nullptr;
		}
		relative_address += pre_offset;
		int32_t offset = 0;
		__try {
			offset = *reinterpret_cast<std::
int32_t*>(relative_address);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::
Seh("M::abs", GetExceptionCode());
			return nullptr;
		}
		relative_address += sizeof(std::
int32_t) + offset;
		relative_address += post_offset;
		return relative_address;
	}
	std::
uint8_t* FindPattern(const char* module_name, const std::
string& byte_sequence);

	template <typename T = std::
uint8_t>
	T* GetAbsoluteAddress(T* relative_address, int pre_offset = 0x0, int post_offset = 0x0)
	{
		if (!relative_address) {
			Con::
Error("GetAbsoluteAddress: relative_address is null");
			return nullptr;
		}
		relative_address += pre_offset;
		int32_t offset = 0;
		__try {
			offset = *reinterpret_cast<std::
int32_t*>(relative_address);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::
Seh("M::GetAbsoluteAddress", GetExceptionCode());
			return nullptr;
		}
		relative_address += sizeof(std::
int32_t) + offset;
		relative_address += post_offset;
		return relative_address;
	}

	template <typename T>
	inline T GetVFunc(void* thisptr, std::
size_t index)
	{
		if (!thisptr || !Mem::IsUserPtr(thisptr)) {
			Con::
Error("GetVFunc: thisptr is null");
			return T{};
		}
		__try {
			return (*reinterpret_cast<T**>(thisptr))[index];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::
Seh("M::GetVFunc", GetExceptionCode());
			return T{};
		}
	}

	uintptr_t patternScan(const std::
string& module, const std::
string& pattern);

	uint8_t* scan(const char* module_name, const char* pattern);

	// ---------------------------------------------------------------------------
	// Async warmup: fire N patternScan jobs on a thread pool up-front. Each result
	// lands in a shared cache keyed by (module, pattern). Subsequent patternScan
	// calls hit the cache -> instant. Call WaitForWarmup() before you rely on
	// results being ready (Hooks::init does this once).
	//
	// ponytail: single global cache; no eviction. Cache lives for the lifetime of
	// the DLL and never GC's. Upgrade path: TTL / bounded LRU if we ever load
	// >1k patterns.
	// ---------------------------------------------------------------------------
void WarmupPatternsAsync(
		std::
initializer_list<std::
pair<const char*, const char*>> patterns);
	// Blocks until every pending warmup future is done.
	void WaitForWarmup();
}

