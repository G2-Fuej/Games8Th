#include "memsafe.h"
#include "../patternscan/patternscan.h"

namespace {

using ParticlesFindKeyFn = std::uint64_t(__fastcall*)(const char*, unsigned int, int);

ParticlesFindKeyFn GetParticlesFindKeyFn()
{
	static ParticlesFindKeyFn find = nullptr;
	if (!find)
		find = reinterpret_cast<ParticlesFindKeyFn>(
			M::patternScan("particles", "48 89 5C 24 ? 57 48 81 EC ? ? ? ? 33 C0"));
	return find;
}

// POD-only. patternScan's std::string temps cannot live in the same frame as __try (C2712).
std::uint64_t CallParticlesFindKey(ParticlesFindKeyFn find, const char* szName)
{
	std::uint64_t k = 0;
	__try { k = find(szName, 0x12, 0x31415926); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	return k;
}

} // namespace

std::uint64_t SehParticlesFindKey(const char* szName) noexcept
{
	if (!szName || !szName[0])
		return 0;
	const ParticlesFindKeyFn find = GetParticlesFindKeyFn();
	if (!find)
		return 0;
	return CallParticlesFindKey(find, szName);
}

namespace Mem {

bool IsReadableHeavy(const void* p, std::
size_t size) noexcept {
	if (!IsReadable(p, size))
		return false;

	const auto start = reinterpret_cast<std::
uintptr_t>(p);
	const auto end = start + size;
	std::
uintptr_t addr = start;
	while (addr < end) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0)
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		const DWORD prot = mbi.Protect;
		if ((prot & PAGE_NOACCESS) || (prot & PAGE_GUARD))
			return false;
		if (!(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			return false;
		const auto regionEnd = reinterpret_cast<std::
uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (regionEnd <= addr)
			return false;
		addr = regionEnd;
	}
	return true;
}

} // namespace Mem

