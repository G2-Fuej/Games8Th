#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <Windows.h>

#include "chams.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../gamemode/gamemode.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/datatypes/keyvalues/keyvalues.h"

// ============================================================================
// Materials - mercey systems::materials ported. vmat sources verbatim.
// ============================================================================

namespace {

struct MaterialDef {
	chams::ChamIds id;
	const char* name;
	const char* vmat;
};

} // namespace

namespace chams {
namespace Materials {

static std::array<CMaterial2*, static_cast<std::size_t>(ChamIds::count)> s_loaded{};
static std::vector<CStrongHandle<CMaterial2>> s_handles{};
static std::atomic<bool> s_ready{ false };
static std::atomic<bool> s_tried{ false };
static std::atomic<ULONGLONG> s_retryAfterMs{ 0 };

// mercey systems::materials::load - stack KV3 (0x10), DestroyKV3 after CreateMaterial copies the tree.
struct Kv3Pod { std::uint64_t metadata; std::uint64_t payload; };
static_assert(sizeof(Kv3Pod) == 0x10);

using FnSetTypeKv3 = Kv3Pod*(__fastcall*)(Kv3Pod*, unsigned, unsigned);
using FnDestroyKv3 = void(__fastcall*)(Kv3Pod*, unsigned);

static FnSetTypeKv3 ResolveSetTypeKv3()
{
	// mercey kv3_alloc is tier0; dump also has the same bytes in client.
	if (uint8_t* p = M::FindPattern("tier0.dll",
		"40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16"))
		return reinterpret_cast<FnSetTypeKv3>(p);
	if (uint8_t* p = M::FindPattern("client.dll",
		"40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16"))
		return reinterpret_cast<FnSetTypeKv3>(p);
	return nullptr;
}

static FnDestroyKv3 ResolveDestroyKv3()
{
	if (uint8_t* p = M::FindPattern("tier0.dll",
		"40 57 41 57 48 83 EC 38 4C 8B 01 44 8B FA 49 8B C0 48 8B F9 48 C1 E8 02"))
		return reinterpret_cast<FnDestroyKv3>(p);
	if (uint8_t* p = M::FindPattern("client.dll",
		"40 57 41 57 48 83 EC 38 4C 8B 01 44 8B FA 49 8B C0 48 8B F9 48 C1 E8 02"))
		return reinterpret_cast<FnDestroyKv3>(p);
	return nullptr;
}

__declspec(noinline) static bool SehCreateMaterial(void* kv, const char* name, CStrongHandle<CMaterial2>* handle)
{
	if (!kv || !name || !handle || !I::CreateMaterial)
		return false;
	__try {
		I::CreateMaterial(nullptr, handle, name, kv, nullptr, 1);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

CMaterial2* load(const char* vmatData, const char* name)
{
	if (!vmatData || !name || !I::CreateMaterial)
		return nullptr;

	static FnSetTypeKv3 s_setType = nullptr;
	static FnDestroyKv3 s_destroy = nullptr;
	static bool s_resolved = false;
	if (!s_resolved) {
		s_resolved = true;
		s_setType = ResolveSetTypeKv3();
		s_destroy = ResolveDestroyKv3();
		if (!s_setType)
			Con::Error("chams kv3 SetType miss");
		if (!s_destroy)
			Con::Error("chams kv3 Destroy miss");
	}
	if (!s_setType || !I::LoadKeyValues)
		return nullptr;

	constexpr KV3ID_t kGenericKv3Id{
		"generic",
		0x41B818518343427Eull,
		0xB5F447C23C0CDF8Cull
	};

	Kv3Pod kv3{};
	Kv3Pod* typed = nullptr;
	__try { typed = s_setType(&kv3, 1u, 6u); }
	__except (EXCEPTION_EXECUTE_HANDLER) { typed = nullptr; }
	if (typed != &kv3) {
		Con::Error("chams::Materials::load(%s): SetTypeKV3 failed", name);
		return nullptr;
	}

	bool loaded = false;
	__try { loaded = I::LoadKeyValues(reinterpret_cast<CKeyValues3*>(&kv3), nullptr, vmatData, &kGenericKv3Id, nullptr, 0u); }
	__except (EXCEPTION_EXECUTE_HANDLER) { loaded = false; }

	CStrongHandle<CMaterial2> handle{};
	if (loaded && !SehCreateMaterial(&kv3, name, &handle)) {
		Con::Error("chams::Materials::load(%s): CreateMaterial SEH", name);
		loaded = false;
	}

	if (s_destroy) {
		__try { s_destroy(&kv3, 0u); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	if (!loaded)
		return nullptr;

	CMaterial2* mat = static_cast<CMaterial2*>(handle);
	if (!mat) {
		Con::Error("chams::Materials::load(%s): CreateMaterial returned empty handle", name);
		return nullptr;
	}

	s_handles.push_back(handle);
	return mat;
}

bool init()
{
	if (s_ready.load(std::memory_order_acquire))
		return true;
	if (GetTickCount64() < s_retryAfterMs.load(std::memory_order_acquire))
		return false;

	bool expected = false;
	if (!s_tried.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return s_ready.load(std::memory_order_acquire);

	if (!I::CreateMaterial) {
		s_retryAfterMs.store(GetTickCount64() + 1000, std::memory_order_release);
		s_tried.store(false, std::memory_order_release); // retry next call
		return false;
	}

	static const MaterialDef kDefs[] = {
		{ ChamIds::liquid_ignorez,  "materials/dev/liquid_ignorez.vmat",  R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_RENDER_BACKFACES = 1
	F_TRANSLUCENT = 1
	F_DISABLE_Z_BUFFERING = 1
	g_vColorTint = [1.0, 1.0, 1.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 0.8
	g_flSelfIllumBrightness = 3.0
	g_flSelfIllumScale = 1.5
	g_vSelfIllumTint = [0.4, 0.7, 1.0]
	g_flSelfIllumAlbedoFactor = 0.3
	g_vSelfIllumScrollSpeed = [0.05, 0.03]
	g_vTexCoordScrollSpeed = [0.01, 0.005]
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::matte_ignorez,   "materials/dev/matte_ignorez.vmat",   R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "generic.vfx"
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})" },
		{ ChamIds::flat_ignorez,    "materials/dev/flat_ignorez.vmat",    R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_unlitgeneric.vfx"
	F_RENDER_BACKFACES = 0
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_WRITE = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_BLEND_MODE = 1
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})" },
		{ ChamIds::bloom_ignorez,   "materials/dev/bloom_ignorez.vmat",   R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "solidcolor.vfx"
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	g_vColorTint = [5.0, 5.0, 5.0]
})" },
		{ ChamIds::outlines_ignorez, "materials/dev/outlines_ignorez.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 1
	F_TRANSLUCENT = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_WRITE = 1
	g_vColorTint = [1.0, 1.0, 1.0, 0.0]
	g_flOpacityScale = 0.45
	g_flFresnelExponent = 0.75
	g_flFresnelFalloff = 1.0
	g_flFresnelMax = 0.0
	g_flFresnelMin = 1.0
	g_flColorBoost = 2.25
	g_flToolsVisCubemapReflectionRoughness = 1.0
	g_flBeginMixingRoughness = 1.0
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::glow_ignorez,    "materials/dev/glow_ignorez.vmat",    R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 1
	F_TRANSLUCENT = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_WRITE = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_flFresnelExponent = 1.5
	g_flFresnelFalloff = 5.0
	g_flFresnelMax = 0.0
	g_flFresnelMin = 1.0
	g_flColorBoost = 20.0
	g_flOpacityScale = 0.6
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
})" },
		{ ChamIds::distortion_ignorez, "materials/dev/distortion_ignorez.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 1
	F_TRANSLUCENT = 1
	F_RENDER_BACKFACES = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_WRITE = 1
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_flOpacityScale = 0.85
	g_flFresnelExponent = 1.25
	g_flFresnelFalloff = 2.25
	g_flFresnelMax = 0.32
	g_flFresnelMin = 1.0
	g_flColorBoost = 14.0
	g_vTexCoordScrollSpeed = [0.24, 0.17]
	g_vTexCoordScale = [2.75, 2.75]
	g_flToolsVisCubemapReflectionRoughness = 1.0
	g_flBeginMixingRoughness = 1.0
	g_tColor = resource:"materials/dev/water_waves.vtex"
	g_tMask1 = resource:"materials/dev/water_waves.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::hologram_ignorez, "materials/dev/hologram_ignorez.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_RENDER_BACKFACES = 1
	F_TRANSLUCENT = 1
	F_DISABLE_Z_BUFFERING = 1
	g_vColorTint = [0.0, 0.0, 0.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 0.6
	g_flSelfIllumBrightness = 4.5
	g_flSelfIllumScale = 2.0
	g_vSelfIllumTint = [0.45, 0.85, 1.0]
	g_flSelfIllumAlbedoFactor = 0.55
	g_vSelfIllumScrollSpeed = [0.0, 0.35]
	g_vTexCoordScale = [0.75, 9.0]
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },

		{ ChamIds::liquid,       "materials/dev/liquid.vmat",        R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_RENDER_BACKFACES = 1
	F_TRANSLUCENT = 1
	g_vColorTint = [0.0, 0.0, 0.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 0.8
	g_flSelfIllumBrightness = 3.0
	g_flSelfIllumScale = 1.5
	g_vSelfIllumTint = [0.4, 0.7, 1.0]
	g_flSelfIllumAlbedoFactor = 0.3
	g_vSelfIllumScrollSpeed = [0.05, 0.03]
	g_vTexCoordScrollSpeed = [0.01, 0.005]
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::metallic,     "materials/dev/metallic.vmat",      R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_character.vfx"
	F_IRIDESCENCE = 1
	F_CLOTH_SHADING = 1
	F_RENDER_BACKFACES = 1
	F_DISABLE_Z_PREPASS = 1
	F_TRANSLUCENT = 1
	F_ADDITIVE_BLEND = 1
	g_vColorTint = [1.0, 1.0, 1.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 1.0
	g_vTexCoordScrollSpeed = [0.05, 0.02]
	g_vTexCoordScale = [1.2, 1.2]
	g_flIridescentStrength = 2.0
	g_flIridescentFresnelStrength = 15.0
	g_flIridescentHueShift = 0.5
	g_flSheenScale = 10.0
	g_flSheenTintColor = [1.0, 1.0, 1.0]
	g_fContrast = 0.5
	g_fBrightness = 1.5
	g_fSaturation = 1.5
	g_flAmbientOcclusionMasking = 0.0
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/dev/water_waves.vtex"
	g_tMetalness = resource:"materials/dev/water_waves.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tIridescentThickness_Mask = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
})" },
		{ ChamIds::matte,        "materials/dev/matte.vmat",         R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "generic.vfx"
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})" },
		{ ChamIds::flat,         "materials/dev/flat.vmat",          R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_unlitgeneric.vfx"
	F_RENDER_BACKFACES = 0
	F_DISABLE_Z_BUFFERING = 0
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_BLEND_MODE = 1
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})" },
		{ ChamIds::bloom,        "materials/dev/bloom.vmat",         R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "solidcolor.vfx"
	F_DISABLE_Z_WRITE = 0
	g_vColorTint = [8.0, 8.0, 8.0]
})" },
		{ ChamIds::outlines,     "materials/dev/outlines.vmat",      R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 1
	F_TRANSLUCENT = 1
	g_vColorTint = [1.0, 1.0, 1.0, 0.0]
	g_flOpacityScale = 0.45
	g_flFresnelExponent = 0.75
	g_flFresnelFalloff = 1.0
	g_flFresnelMax = 0.0
	g_flFresnelMin = 1.0
	g_flColorBoost = 2.25
	g_flToolsVisCubemapReflectionRoughness = 1.0
	g_flBeginMixingRoughness = 1.0
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::glow,         "materials/dev/glow.vmat",          R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 1
	F_TRANSLUCENT = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_BUFFERING = 0
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_flFresnelExponent = 1.5
	g_flFresnelFalloff = 5.0
	g_flFresnelMax = 0.0
	g_flFresnelMin = 1.0
	g_flColorBoost = 20.0
	g_flOpacityScale = 0.6
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
})" },
		{ ChamIds::electric,     "materials/dev/electric.vmat",      R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_RENDER_BACKFACES = 1
	F_TRANSLUCENT = 1
	g_vColorTint = [0.0, 0.0, 0.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 0.8
	g_flSelfIllumBrightness = 3.0
	g_flSelfIllumScale = 1.5
	g_vSelfIllumTint = [0.4, 0.7, 1.0]
	g_flSelfIllumAlbedoFactor = 0.3
	g_vSelfIllumScrollSpeed = [0.15, 0.1]
	g_vTexCoordScrollSpeed = [0.03, 0.02]
	g_vTexCoordScale = [2.0, 2.0]
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/particle/electrical/electrical_cracks.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::distortion,   "materials/dev/distortion.vmat",    R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 1
	F_TRANSLUCENT = 1
	F_RENDER_BACKFACES = 1
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_flOpacityScale = 0.85
	g_flFresnelExponent = 1.25
	g_flFresnelFalloff = 2.25
	g_flFresnelMax = 0.32
	g_flFresnelMin = 1.0
	g_flColorBoost = 14.0
	g_vTexCoordScrollSpeed = [0.24, 0.17]
	g_vTexCoordScale = [2.75, 2.75]
	g_flToolsVisCubemapReflectionRoughness = 1.0
	g_flBeginMixingRoughness = 1.0
	g_tColor = resource:"materials/dev/water_waves.vtex"
	g_tMask1 = resource:"materials/dev/water_waves.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::hologram,     "materials/dev/hologram.vmat",      R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_RENDER_BACKFACES = 1
	F_TRANSLUCENT = 1
	g_vColorTint = [0.0, 0.0, 0.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 0.6
	g_flSelfIllumBrightness = 4.5
	g_flSelfIllumScale = 2.0
	g_vSelfIllumTint = [0.45, 0.85, 1.0]
	g_flSelfIllumAlbedoFactor = 0.55
	g_vSelfIllumScrollSpeed = [0.0, 0.35]
	g_vTexCoordScale = [0.75, 9.0]
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})" },
		{ ChamIds::pearl,        "materials/dev/pearl.vmat",         R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_character.vfx"
	F_IRIDESCENCE = 1
	F_CLOTH_SHADING = 1
	F_RENDER_BACKFACES = 1
	F_DISABLE_Z_PREPASS = 1
	g_vColorTint = [1.0, 1.0, 1.0]
	g_flModelTintAmount = 1.0
	g_flOpacityScale = 1.0
	g_vTexCoordScale = [1.0, 1.0]
	g_flIridescentStrength = 3.5
	g_flIridescentFresnelStrength = 4.0
	g_flIridescentHueShift = 1.0
	g_flSheenScale = 6.0
	g_flSheenTintColor = [1.0, 1.0, 1.0]
	g_fContrast = 0.35
	g_fBrightness = 1.25
	g_fSaturation = 1.6
	g_flAmbientOcclusionMasking = 0.0
	g_bFogEnabled = 0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tMetalness = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tIridescentThickness_Mask = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
})" },
	};

	int okCount = 0;
	try {
		s_loaded.fill(nullptr);
		s_handles.clear();
		for (const auto& def : kDefs) {
			CMaterial2* mat = load(def.vmat, def.name);
			if (mat) {
				s_loaded[static_cast<std::size_t>(def.id)] = mat;
				++okCount;
			}
		}
	} catch (...) {
		s_loaded.fill(nullptr);
		s_handles.clear();
		okCount = 0;
	}

	const bool anyLoaded = okCount > 0;
	s_ready.store(anyLoaded,
		std::memory_order_release);
	if (okCount == static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0])))
		Con::Ok("Chams materials ready (%d/%d)", okCount, static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0])));
	else if (anyLoaded)
		Con::Warn("Chams materials partial (%d/%d)", okCount, static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0])));
	else {
		Con::Error("Chams materials: %d/%d loaded", okCount, static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0])));
		s_retryAfterMs.store(GetTickCount64() + 1000, std::memory_order_release);
		s_tried.store(false, std::memory_order_release);
	}
	return s_ready.load(std::memory_order_acquire);
}

bool ready() noexcept
{
	if (!s_tried.load(std::memory_order_acquire))
		init();
	return s_ready.load(std::memory_order_acquire);
}

CMaterial2* find(ChamIds id)
{
	const auto index = static_cast<std::size_t>(id);
	if (index >= s_loaded.size())
		return nullptr;
	if (!s_ready.load(std::memory_order_acquire))
		init();
	return s_loaded[index];
}

void set_material_vec3(CMaterial2* mat, const char* paramName, float x, float y, float z)
{
	if (!mat || !paramName || !Mem::IsUserPtr(mat))
		return;
	__try {
		const auto kvCount = *reinterpret_cast<const int*>(reinterpret_cast<const std::uint8_t*>(mat) + 0x18);
		const auto kvArray = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::uint8_t*>(mat) + 0x20);
		for (auto i = 0; i < kvCount; ++i) {
			const auto entry = kvArray + static_cast<std::uintptr_t>(i) * 0x40;
			const auto name = *reinterpret_cast<const char**>(entry + 0x28);
			if (!name || std::strcmp(name, paramName) != 0)
				continue;
			*reinterpret_cast<float*>(entry + 0x00) = x;
			*reinterpret_cast<float*>(entry + 0x04) = y;
			*reinterpret_cast<float*>(entry + 0x08) = z;
			return;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

} // namespace Materials
} // namespace chams

// ============================================================================
// Primitive buffer - mercey features::esp::detail
// ============================================================================

namespace chams {
namespace prim {

int output_buffer::count() const noexcept
{
	if (fixed_count < 0 || overflow_count < 0 ||
		fixed_capacity < 0 || overflow_capacity < 0 ||
		fixed_count > fixed_capacity ||
		overflow_count > overflow_capacity ||
		(fixed_count && !fixed_data) ||
		(overflow_count && !overflow_data)) {
		return -1;
	}

	constexpr auto sanePrimitiveLimit{ 1 << 20 };
	if (fixed_count > sanePrimitiveLimit - overflow_count)
		return -1;

	return fixed_count + overflow_count;
}

std::uintptr_t output_buffer::at(int index) const noexcept
{
	const auto total = this->count();
	if (index < 0 || total < 0 || index >= total)
		return 0;

	if (index < fixed_count)
		return fixed_data + static_cast<std::size_t>(index) * kStride;

	return overflow_data + static_cast<std::size_t>(index - fixed_count) * kStride;
}

bool read_buffer(void* addr, output_buffer& out)
{
	if (!addr || !Mem::IsReadable(addr, sizeof(output_buffer)))
		return false;
	bool ok = false;
	__try {
		out = *reinterpret_cast<output_buffer*>(addr);
		ok = out.count() >= 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return ok;
}

__declspec(noinline) static bool IsEngineGlowMaterial(CMaterial2* mat)
{
	if (!mat || !Mem::IsReadable(mat, sizeof(void*)))
		return false;

	const char* name = nullptr;
	__try { name = mat->GetName(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!name)
		return false;

	constexpr char needle[] = "glowproperty";
	constexpr std::size_t kMaxNameLength = 256;
	for (std::size_t start = 0; start < kMaxNameLength; ++start) {
		char first = 0;
		__try { first = name[start]; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		if (!first)
			break;
		if (first >= 'A' && first <= 'Z')
			first = static_cast<char>(first + ('a' - 'A'));
		if (first != needle[0])
			continue;

		bool match = true;
		for (std::size_t i = 1; needle[i]; ++i) {
			char current = 0;
			__try { current = name[start + i]; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			if (!current)
				return false;
			if (current >= 'A' && current <= 'Z')
				current = static_cast<char>(current + ('a' - 'A'));
			if (current != needle[i]) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

void replace_primitive(void* primitive, CMaterial2* mat, std::uint32_t packedColor)
{
	if (!primitive || !mat || !Mem::IsReadable(primitive, kStride))
		return;
	__try {
		auto* bytes = reinterpret_cast<std::uint8_t*>(primitive);
		const auto current = *reinterpret_cast<CMaterial2* const*>(bytes + kMatOff);
		const auto currentCopy = *reinterpret_cast<CMaterial2* const*>(bytes + kMatCopyOff);
		if (IsEngineGlowMaterial(current) || IsEngineGlowMaterial(currentCopy))
			return;
		*reinterpret_cast<CMaterial2**>(bytes + kMatOff) = mat;
		*reinterpret_cast<CMaterial2**>(bytes + kMatCopyOff) = mat;
		*reinterpret_cast<std::uint32_t*>(bytes + kColorOff) = packedColor;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

} // namespace prim
} // namespace chams

// ============================================================================
// Player chams - mercey features::esp::player::chams (backtrack/onshot skipped)
// ============================================================================

namespace {

std::uint32_t PackColor(const ImVec4& c)
{
	const auto r = static_cast<std::uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f + 0.5f);
	const auto g = static_cast<std::uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f + 0.5f);
	const auto b = static_cast<std::uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f + 0.5f);
	const auto a = static_cast<std::uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f + 0.5f);
	return static_cast<std::uint32_t>(r)
		| (static_cast<std::uint32_t>(g) << 8)
		| (static_cast<std::uint32_t>(b) << 16)
		| (static_cast<std::uint32_t>(a) << 24);
}

__declspec(noinline) CBaseHandle SehSceneOwnerHandle(void* sceneObject)
{
	CBaseHandle h{};
	if (!sceneObject)
		return h;
	__try {
		if (!Mem::IsReadable(sceneObject, 0xC8))
			return h;
		h = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<std::uint8_t*>(sceneObject) + 0xC0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return CBaseHandle{};
	}
	return h;
}

__declspec(noinline) void SehClearSceneSkip(void* sceneObject)
{
	if (!sceneObject)
		return;
	__try {
		auto* flags = reinterpret_cast<std::uint8_t*>(sceneObject) + 0x78;
		*flags = static_cast<std::uint8_t>(*flags & ~(1u << 3));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

__declspec(noinline) std::uint32_t SehSchemaHash(C_BaseEntity* e)
{
	if (!e)
		return 0;
	char name[128]{};
	if (!Mem::SchemaClassName(e, name, sizeof(name)) || !name[0])
		return 0;
	return hash_32_fnv1a_const(name);
}

__declspec(noinline) CGameSceneNode* SehSceneNode(C_BaseEntity* e)
{
	if (!e)
		return nullptr;
	CGameSceneNode* node = nullptr;
	__try { node = e->m_pGameSceneNode(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!node || !Mem::IsUserPtr(node))
		return nullptr;
	return node;
}

__declspec(noinline) int SehTeam(C_BaseEntity* e)
{
	if (!e)
		return -1;
	int team = -1;
	__try { team = e->m_iTeamNum(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	return team;
}

__declspec(noinline) int SehHealth(C_BaseEntity* e)
{
	if (!e)
		return 0;
	int hp = 0;
	__try { hp = e->m_iHealth(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	return hp;
}

__declspec(noinline) CBaseHandle SehOwnerEntityHandle(C_BaseEntity* e)
{
	CBaseHandle h{};
	if (!e)
		return h;
	__try { h = e->m_hOwnerEntity(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return CBaseHandle{}; }
	return h;
}

} // namespace

namespace chams {

static CMaterial2* FindMaterial(int materialId) noexcept
{
	if (materialId < 0 || materialId >= static_cast<int>(ChamIds::count))
		return nullptr;
	return Materials::find(static_cast<ChamIds>(materialId));
}

void PlayerChams::apply_layer(void* primitiveBuffer, OriginalFn original, void* a1,
	void* sceneObject, void* sceneView, const ImVec4& color, int materialId)
{
	prim::output_buffer before{};
	const auto prevCount = prim::read_buffer(primitiveBuffer, before) ? before.count() : -1;

	original(a1, sceneObject, sceneView, primitiveBuffer);

	prim::output_buffer after{};
	const auto newCount = prim::read_buffer(primitiveBuffer, after) ? after.count() : -1;
	if (newCount < 0 || prevCount < 0 || prevCount >= newCount)
		return;

	CMaterial2* material = FindMaterial(materialId);
	if (!material)
		return;

	const auto packed = PackColor(color);
	for (auto i = prevCount; i < newCount; ++i) {
		const auto p = after.at(i);
		if (!p)
			continue;
		prim::replace_primitive(reinterpret_cast<void*>(p), material, packed);
	}
}

void PlayerChams::apply_overlay(void* primitiveBuffer, OriginalFn original, void* a1,
	void* sceneObject, void* sceneView, const ImVec4& color, int materialId)
{
	CMaterial2* material = FindMaterial(materialId);
	if (!material)
		return;

	prim::output_buffer before{};
	const auto prevCount = prim::read_buffer(primitiveBuffer, before) ? before.count() : -1;

	original(a1, sceneObject, sceneView, primitiveBuffer);

	prim::output_buffer after{};
	const auto newCount = prim::read_buffer(primitiveBuffer, after) ? after.count() : -1;
	if (newCount < 0 || prevCount < 0 || prevCount >= newCount)
		return;

	const auto packed = PackColor(color);
	for (auto i = prevCount; i < newCount; ++i) {
		const auto p = after.at(i);
		if (!p)
			continue;
		prim::replace_primitive(reinterpret_cast<void*>(p), material, packed);
	}

	add_overlay_material(material);
}

bool PlayerChams::is_overlay_material(CMaterial2* mat) const
{
	std::lock_guard lock(m_overlayMaterialsMutex);
	const auto count = std::clamp(m_overlayMaterialCount.load(std::memory_order_acquire), 0, kMaxOverlayMaterials);
	for (auto i = 0; i < count; ++i) {
		if (m_overlayMaterials[i].load(std::memory_order_relaxed) == mat)
			return true;
	}
	return false;
}

void PlayerChams::add_overlay_material(CMaterial2* mat)
{
	if (!mat)
		return;
	std::lock_guard lock(m_overlayMaterialsMutex);
	const auto count = std::clamp(m_overlayMaterialCount.load(std::memory_order_acquire), 0, kMaxOverlayMaterials);
	for (auto i = 0; i < count; ++i) {
		if (m_overlayMaterials[i].load(std::memory_order_relaxed) == mat)
			return;
	}
	if (count >= kMaxOverlayMaterials)
		return;

	// Publish the slot before the count so SortPrimitives never observes an
	// advertised entry whose material pointer is still null.
	m_overlayMaterials[count].store(mat, std::memory_order_release);
	m_overlayMaterialCount.store(count + 1, std::memory_order_release);
}

bool PlayerChams::on_generate_primitives(C_BaseEntity* ownerEntity, std::uint32_t ownerHash,
	void* sceneObject, void* primitiveBuffer, OriginalFn original, void* a1, void* sceneView)
{
	const bool isPlayer = ownerHash == HASH("C_CSPlayerPawn");
	const bool isArms = ownerHash == HASH("C_CS2HudModelArms");
	const bool isWeapon = ownerHash == HASH("C_CS2HudModelWeapon");

	auto isLocalAttachment = [&](C_CSPlayerPawn* viewPawn) -> bool {
		CGameSceneNode* node = SehSceneNode(ownerEntity);
		if (!node)
			return false;
		__try {
			const auto offParent = SchemaFinder::Get(HASH("CGameSceneNode->m_pParent"));
			if (!offParent)
				return false;
			CGameSceneNode* parent = nullptr;
			if (!Mem::ReadField(node, offParent, parent) || !parent)
				return false;
			const auto offOwner = SchemaFinder::Get(HASH("CGameSceneNode->m_pOwner"));
			if (!offOwner)
				return false;
			void* parentOwner = nullptr;
			if (!Mem::ReadField(parent, offOwner, parentOwner))
				return false;
			return parentOwner == viewPawn;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	};

	auto applyConfig = [&](const ChamsConfig& cfg, void* targetSceneObj, bool forceOriginal = false) {
		if (cfg.secondary.enabled)
			apply_layer(primitiveBuffer, original, a1, targetSceneObj, sceneView, cfg.secondary.color, cfg.secondary.material);
		if (cfg.primary.enabled) {
			apply_layer(primitiveBuffer, original, a1, targetSceneObj, sceneView, cfg.primary.color, cfg.primary.material);
		} else if (cfg.secondary.enabled) {
			// XQZ-only: the ignore-z pass paints the whole silhouette (visible
			// parts included). A trailing depth-tested original pass re-covers
			// every visible pixel with normal rendering, so the cham colour
			// survives ONLY behind walls.
			original(a1, targetSceneObj, sceneView, primitiveBuffer);
		}
		if (!cfg.primary.enabled && !cfg.secondary.enabled && (cfg.overlay.enabled || forceOriginal))
			original(a1, targetSceneObj, sceneView, primitiveBuffer);
		if (cfg.overlay.enabled)
			apply_overlay(primitiveBuffer, original, a1, targetSceneObj, sceneView, cfg.overlay.color, cfg.overlay.material);
	};

	// Config -> mercey chams_config
	const ChamsConfig enemyCfg{
		Config::enemyChams || Config::enemyChamsInvisible,
		{ Config::enemyChams, Config::colVisualChams, Config::chamsMaterial },
		{ Config::enemyChamsInvisible, Config::colVisualChamsIgnoreZ, Config::chamsMaterialXQZ },
		{}
	};
	const ChamsConfig teamCfg{
		Config::teamChams || Config::teamChamsInvisible,
		{ Config::teamChams, Config::teamcolVisualChams, Config::teamChamsMaterial },
		{ Config::teamChamsInvisible, Config::teamcolVisualChamsIgnoreZ, Config::teamChamsMaterialXQZ },
		{}
	};
	const ChamsConfig localCfg{
		Config::localChams,
		{},
		{},
		{ Config::localChams, Config::colLocalChams, Config::localChamsMaterial }
	};
	const ChamsConfig localRagdollCfg{
		Config::ragdollChams,
		{ Config::ragdollChams, Config::colRagdollChams, Config::ragdollChamsMaterial },
		{},
		{}
	};
	const ChamsConfig enemyRagdollCfg = localRagdollCfg;
	const ChamsConfig teamRagdollCfg = localRagdollCfg;
	const ChamsConfig weaponCfg{
		Config::viewmodelChams,
		{},
		{},
		{ Config::viewmodelChams, Config::colViewmodelChams, Config::viewmodelChamsMaterial }
	};
	const ChamsConfig armsCfg{
		Config::armChams,
		{},
		{},
		{ Config::armChams, Config::colArmChams, Config::armChamsMaterial }
	};

	if (!isPlayer && !isArms && !isWeapon) {
		if (!Config::viewmodelChams)
			return false;
		if (!Config::thirdperson || !isLocalAttachment(H::SafeLocalAlive()))
			return false;
		applyConfig(weaponCfg, sceneObject);
		return true;
	}

	if (isArms || isWeapon) {
		const ChamsConfig& cfg = isArms ? armsCfg : weaponCfg;
		if (!cfg.enabled)
			return false;
		applyConfig(cfg, sceneObject);
		return true;
	}

	const auto local = H::SafeLocalPlayer();
	const auto team = SehTeam(ownerEntity);
	const auto health = SehHealth(ownerEntity);

	bool isOtherTeam = true;
	if (local) {
		const auto localTeam = SehTeam(reinterpret_cast<C_BaseEntity*>(local));
		if (GameMode::WantTeamCheck(Config::team_check)
			&& Mem::ValidTeam(team) && Mem::ValidTeam(localTeam))
			isOtherTeam = team != localTeam;
	}
	const auto isLocal = local != nullptr && ownerEntity == reinterpret_cast<C_BaseEntity*>(local);
	const auto isDead = health <= 0;

	const ChamsConfig* target = nullptr;
	if (isDead) {
		if (isLocal)
			target = &localRagdollCfg;
		else if (isOtherTeam)
			target = &enemyRagdollCfg;
		else
			target = &teamRagdollCfg;
	} else {
		if (isLocal)
			target = &localCfg;
		else if (isOtherTeam)
			target = &enemyCfg;
		else
			target = &teamCfg;
	}

	if (!target || !target->enabled)
		return false;
	if (!target->primary.enabled && !target->secondary.enabled && !target->overlay.enabled)
		return false;

	// mercey: clear the skip/occlude bit so pawn meshes take the swap
	SehClearSceneSkip(sceneObject);

	applyConfig(*target, sceneObject);
	return true;
}

__declspec(noinline) static bool SehReadPrimitive(void* p, prim::mesh_primitive& out)
{
	if (!p || !Mem::IsReadable(p, prim::kStride))
		return false;
	__try {
		out = *reinterpret_cast<prim::mesh_primitive*>(p);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

__declspec(noinline) static bool SehWritePrimitive(void* p, const prim::mesh_primitive& v)
{
	if (!p)
		return false;
	__try {
		*reinterpret_cast<prim::mesh_primitive*>(p) = v;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void PlayerChams::on_sort_primitives(void* entries, std::uint32_t count)
{
	if (!count || !entries || count > (1u << 20))
		return;

	const auto overlayMatCount = std::clamp(m_overlayMaterialCount.load(std::memory_order_acquire), 0, kMaxOverlayMaterials);
	if (overlayMatCount <= 0)
		return;

	const auto total = static_cast<int>(count);
	if (total <= 1)
		return;

	std::vector<prim::mesh_primitive> sorted;
	sorted.reserve(total);

	for (auto i = 0; i < total; ++i) {
		const auto p = reinterpret_cast<std::uint8_t*>(entries) + static_cast<std::size_t>(i) * prim::kStride;
		prim::mesh_primitive prim2{};
		if (!SehReadPrimitive(p, prim2))
			return;
		sorted.push_back(prim2);
	}

	const auto overlayBegin = std::stable_partition(
		sorted.begin(), sorted.end(), [this](const auto& p) {
			return !is_overlay_material(reinterpret_cast<CMaterial2*>(p.material));
		});
	const auto overlayCount = static_cast<int>(std::distance(overlayBegin, sorted.end()));
	if (overlayCount <= 0 || overlayCount >= total)
		return;

	for (auto i = 0; i < total; ++i) {
		auto* p = reinterpret_cast<std::uint8_t*>(entries) + static_cast<std::size_t>(i) * prim::kStride;
		if (!SehWritePrimitive(p, sorted[i]))
			return;
	}
}

} // namespace chams

// ============================================================================
// Item chams - mercey features::esp::item::chams
// ============================================================================

namespace chams {

void ItemChams::apply_layer(void* primitiveBuffer, OriginalFn original, void* a1,
	void* sceneObject, void* sceneView, const ImVec4& color, int materialId)
{
	prim::output_buffer before{};
	const auto prevCount = prim::read_buffer(primitiveBuffer, before) ? before.count() : -1;

	original(a1, sceneObject, sceneView, primitiveBuffer);

	prim::output_buffer after{};
	const auto newCount = prim::read_buffer(primitiveBuffer, after) ? after.count() : -1;
	if (newCount < 0 || prevCount < 0 || prevCount >= newCount)
		return;

	CMaterial2* material = FindMaterial(materialId);
	if (!material)
		return;

	const auto packed = PackColor(color);
	for (auto i = prevCount; i < newCount; ++i) {
		const auto p = after.at(i);
		if (!p)
			continue;
		prim::replace_primitive(reinterpret_cast<void*>(p), material, packed);
	}
}

std::uint32_t ItemChams::get_item_group(std::uint32_t schemaHash)
{
	switch (schemaHash) {
	case HASH("C_DEagle"):
	case HASH("C_WeaponElite"):
	case HASH("C_WeaponFiveSeven"):
	case HASH("C_WeaponGlock"):
	case HASH("C_WeaponHKP2000"):
	case HASH("C_WeaponUSPSilencer"):
	case HASH("C_WeaponP250"):
	case HASH("C_WeaponCZ75a"):
	case HASH("C_WeaponTec9"):
	case HASH("C_WeaponRevolver"):
		return 0;

	case HASH("C_WeaponMAC10"):
	case HASH("C_WeaponMP5SD"):
	case HASH("C_WeaponMP7"):
	case HASH("C_WeaponMP9"):
	case HASH("C_WeaponBizon"):
	case HASH("C_WeaponP90"):
	case HASH("C_WeaponUMP45"):
		return 1;

	case HASH("C_AK47"):
	case HASH("C_WeaponM4A1"):
	case HASH("C_WeaponM4A1Silencer"):
	case HASH("C_WeaponAug"):
	case HASH("C_WeaponFamas"):
	case HASH("C_WeaponGalilAR"):
	case HASH("C_WeaponSG556"):
		return 2;

	case HASH("C_WeaponNOVA"):
	case HASH("C_WeaponSawedoff"):
	case HASH("C_WeaponXM1014"):
	case HASH("C_WeaponMag7"):
		return 3;

	case HASH("C_WeaponAWP"):
	case HASH("C_WeaponG3SG1"):
	case HASH("C_WeaponSCAR20"):
	case HASH("C_WeaponSSG08"):
	case HASH("C_WeaponM249"):
	case HASH("C_WeaponNegev"):
		return 4;

	case HASH("C_HEGrenade"):
	case HASH("C_Flashbang"):
	case HASH("C_SmokeGrenade"):
	case HASH("C_MolotovGrenade"):
	case HASH("C_IncendiaryGrenade"):
	case HASH("C_DecoyGrenade"):
	case HASH("C_C4"):
	case HASH("C_WeaponTaser"):
	case HASH("C_Item_Healthshot"):
	case HASH("C_Knife"):
		return 5;

	default:
		return UINT32_MAX;
	}
}

bool ItemChams::on_generate_primitives(C_BaseEntity* ownerEntity, std::uint32_t ownerHash,
	void* sceneObject, void* primitiveBuffer, OriginalFn original, void* a1, void* sceneView)
{
	if (!Config::itemChams && !Config::itemChamsInvisible)
		return false;

	const auto groupId = get_item_group(ownerHash);
	if (groupId == UINT32_MAX)
		return false;

	const bool groupOn[] = {
		Config::itemChamsPistol,
		Config::itemChamsSmg,
		Config::itemChamsRifle,
		Config::itemChamsShotgun,
		Config::itemChamsSniper,
		Config::itemChamsUtility
	};
	if (!groupOn[groupId])
		return false;

	// World items only: no owner handle, no parent scene node
	const CBaseHandle ownerHandle = SehOwnerEntityHandle(ownerEntity);
	if (ownerHandle.valid())
		return false;

	CGameSceneNode* node = SehSceneNode(ownerEntity);
	if (node) {
		__try {
			const auto offParent = SchemaFinder::Get(HASH("CGameSceneNode->m_pParent"));
			if (!offParent)
				return false;
			CGameSceneNode* parent = nullptr;
			if (!Mem::ReadField(node, offParent, parent) || parent)
				return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	if (!Config::itemChamsInvisible && !Config::itemChams)
		return false;

	// mercey apply order: secondary first, then primary
	if (Config::itemChamsInvisible)
		apply_layer(primitiveBuffer, original, a1, sceneObject, sceneView, Config::colItemChamsIgnoreZ, Config::itemChamsMaterialXQZ);
	if (Config::itemChams) {
		apply_layer(primitiveBuffer, original, a1, sceneObject, sceneView, Config::colItemChams, Config::itemChamsMaterial);
	} else if (Config::itemChamsInvisible) {
		// XQZ-only: trailing depth-tested pass keeps visible items normal -
		// cham colour shows only where the world occludes them.
		original(a1, sceneObject, sceneView, primitiveBuffer);
	}

	return true;
}

} // namespace chams

// ============================================================================
// Hook wiring - mercey cheat::generate_primitives / sort_primitives
// ============================================================================

namespace chams {

static PlayerChams g_playerChams{};
static ItemChams g_itemChams{};

bool OnGeneratePrimitives(void* a1, void* sceneObj, void* sceneView, void* drawList,
	std::int64_t(__fastcall* original)(void*, void*, void*, void*),
	std::int64_t* outRet)
{
	if (!sceneObj || !drawList)
		return false;
	if (H::SessionMapLeaving() || H::SessionPostMatch() || !H::SessionEntityReady())
		return false;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return false;
	if (!Materials::ready())
		return false;

	const CBaseHandle ownerHandle = SehSceneOwnerHandle(sceneObj);
	if (!ownerHandle.valid() || ownerHandle.raw() == 0)
		return false;

	// Resolve through the full handle so a recycled entity slot cannot receive
	// another pawn's chams after a death or round transition.
	C_BaseEntity* ownerEntity = nullptr;
	__try { ownerEntity = I::GameEntity->Instance->Get<C_BaseEntity>(ownerHandle); }
	__except (EXCEPTION_EXECUTE_HANDLER) { ownerEntity = nullptr; }
	if (!ownerEntity || !Mem::ValidEntity(ownerEntity))
		return false;

	const std::uint32_t ownerHash = SehSchemaHash(ownerEntity);
	if (!ownerHash)
		return false;

	const auto ofn = reinterpret_cast<PlayerChams::OriginalFn>(original);

	if (g_playerChams.on_generate_primitives(ownerEntity, ownerHash, sceneObj, drawList, ofn, a1, sceneView))
		return true;

	if (g_itemChams.on_generate_primitives(ownerEntity, ownerHash, sceneObj, drawList, ofn, a1, sceneView))
		return true;

	return false;
}

void OnSortPrimitives(void* entries, std::uint32_t count)
{
	g_playerChams.on_sort_primitives(entries, count);
}

} // namespace chams

