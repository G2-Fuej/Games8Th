#include "fog_handler.h"
#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"
#include "../../utils/crypto/xorstr.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <emmintrin.h>
#include <xmmintrin.h>

namespace World {
namespace Fog {
namespace {

constexpr std::uint32_t kHashGradientFog  = 0x4B01FF63u;
constexpr std::uint32_t kHashGradientFog2 = 0x0AA49C2Au;
constexpr std::uint32_t kHashGradientFog3 = 0xFBF6448Du;
constexpr std::uint32_t kHashEnableFog    = 0x6E0FAD7Eu;

using FnSetParamF = std::uint64_t(__fastcall*)(__m128i* map, std::uint32_t hash, const __m128i* value);
using FnSetParamI = std::uint64_t(__fastcall*)(__m128i* map, std::uint32_t hash, int value);

FnSetParamF g_setParamF = nullptr;
FnSetParamI g_setParamI = nullptr;
bool g_resolved = false;

void ResolveFns()
{
	if (g_resolved)
		return;
	g_resolved = true;

	// Unique CALL of set_shader_param (IDA 0x1801702C0) then movzx eax,[rbx+25h]
	const auto callF = reinterpret_cast<std::uint8_t*>(
		M::patternScan("client", "E8 ? ? ? ? 0F B6 43 25"));
	if (callF)
		g_setParamF = reinterpret_cast<FnSetParamF>(M::GetAbsoluteAddress(callF, 1, 0));

	// set_shader_param_i - IDA 0x18016FE90, unique `mov esi, r8d`
	const uintptr_t addrI = M::patternScan("client",
		"48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 20 66 0F 6E CA 41 8B F0");
	if (addrI)
		g_setParamI = reinterpret_cast<FnSetParamI>(addrI);

	if (!g_setParamF)
		Con::OffsetMiss("Fog set_shader_param");
	else
		Con::Ok("Fog set_shader_param @ 0x%p", (void*)g_setParamF);
	if (!g_setParamI)
		Con::OffsetMiss("Fog set_shader_param_i");
	else
		Con::Ok("Fog set_shader_param_i @ 0x%p", (void*)g_setParamI);
}

} // namespace

bool ApplyToScene(void* outputRaw)
{
	if (!outputRaw || !Config::custom_fog)
		return false;
	auto* output = reinterpret_cast<__m128i*>(outputRaw);

	ResolveFns();
	if (!g_setParamF || !g_setParamI)
		return false;

	float start = Config::custom_fog_start;
	float end = Config::custom_fog_end;
	if (end <= start)
		end = start + 256.f;
	const float fall = std::clamp(Config::custom_fog_falloff, 0.1f, 16.f);
	const float r = std::clamp(Config::custom_fog_color.x, 0.f, 1.f);
	const float g = std::clamp(Config::custom_fog_color.y, 0.f, 1.f);
	const float b = std::clamp(Config::custom_fog_color.z, 0.f, 1.f);
	const float opac = std::clamp(Config::custom_fog_color.w, 0.f, 1.f);

	// IDA sub_18027D400 packs entity+0x66C / +0x67C / +0x688 into these hashes.
	alignas(16) __m128i p1{};
	alignas(16) __m128i p2{};
	alignas(16) __m128i p3{};
	_mm_store_ps(reinterpret_cast<float*>(&p1), _mm_set_ps(0.f, 0.f, end, start));
	_mm_store_ps(reinterpret_cast<float*>(&p2), _mm_set_ps(0.f, 0.f, fall, opac));
	_mm_store_ps(reinterpret_cast<float*>(&p3), _mm_set_ps(0.f, b, g, r));

	__try {
		g_setParamF(output, kHashGradientFog, &p1);
		g_setParamF(output, kHashGradientFog2, &p2);
		g_setParamF(output, kHashGradientFog3, &p3);
		g_setParamI(output + 17, kHashEnableFog, 1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

void Shutdown() {}

} // namespace Fog
} // namespace World

std::uintptr_t __fastcall H::hkSetupFog(void* output, int* mode)
{
	std::uintptr_t result = 0;
	auto original = H::SetupFog.GetOriginal();
	if (original) {
		__try {
			result = original(output, mode);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			result = 0;
		}
	}

	if (output && Config::custom_fog) {
		__try {
			World::Fog::ApplyToScene(output);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	return result;
}

