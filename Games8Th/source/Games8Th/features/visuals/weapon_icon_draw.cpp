#include "weapon_icon_draw.h"
#include "assets/weapon_icon_atlas.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <cstring>
#include <unordered_map>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../../../external/stb_image.h"

namespace WeaponIconDraw {
namespace {

ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11Device* g_srvDevice = nullptr; // device the SRV was created on
bool g_tried = false;
DWORD g_nextRetryMs = 0; // bounded retry after a failed texture create
std::unordered_map<std::string, int> g_nameToIdx;

void BuildMap()
{
	if (!g_nameToIdx.empty())
		return;
	for (int i = 0; i < weapon_icon_atlas::kCount; ++i) {
		g_nameToIdx[weapon_icon_atlas::kEntries[i].name] = i;
		// aliases
		const char* n = weapon_icon_atlas::kEntries[i].name;
		if (std::strcmp(n, "m4a1") == 0)
			g_nameToIdx["m4a4"] = i;
		if (std::strcmp(n, "galilar") == 0)
			g_nameToIdx["galil"] = i;
		if (std::strcmp(n, "hkp2000") == 0)
			g_nameToIdx["p2000"] = i;
		if (std::strcmp(n, "sg556") == 0)
			g_nameToIdx["sg553"] = i;
	}
}

int FindIdx(const char* key)
{
	if (!key || !key[0])
		return -1;
	BuildMap();
	// strip weapon_ prefix
	const char* k = key;
	if (std::strncmp(k, "weapon_", 7) == 0)
		k += 7;
	auto it = g_nameToIdx.find(k);
	if (it != g_nameToIdx.end())
		return it->second;
	// knife_* -> knife
	if (std::strncmp(k, "knife", 5) == 0) {
		it = g_nameToIdx.find("knife");
		if (it != g_nameToIdx.end())
			return it->second;
	}
	return -1;
}

bool CreateTexture(ID3D11Device* device)
{
	if (!device)
		return false;
	int w = 0, h = 0, ch = 0;
	unsigned char* rgba = stbi_load_from_memory(
		weapon_icon_atlas::kPngData,
		weapon_icon_atlas::kPngSize,
		&w, &h, &ch, 4);
	if (!rgba || w != weapon_icon_atlas::kWidth || h != weapon_icon_atlas::kHeight) {
		if (rgba) stbi_image_free(rgba);
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA sub{};
	sub.pSysMem = rgba;
	sub.SysMemPitch = static_cast<UINT>(w * 4);

	ID3D11Texture2D* tex = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, &sub, &tex)) || !tex) {
		stbi_image_free(rgba);
		return false;
	}
	stbi_image_free(rgba);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	const HRESULT hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
	tex->Release();
	if (FAILED(hr) || !srv)
		return false;
	g_srv = srv;
	return true;
}

} // namespace

void EnsureReady(ID3D11Device* device)
{
	if (!device)
		return;
	// Device changed (TDR / driver reset / recreate): the old SRV belongs to
	// a dead device - drop it and rebuild on the new one.
	if (g_srv && g_srvDevice != device) {
		g_srv->Release();
		g_srv = nullptr;
		g_tried = false;
	}
	if (g_srv)
		return;
	// Failed create: retry at most every 5 s instead of never (latch) or
	// every frame (stb decode + CreateTexture2D churn).
	if (g_tried) {
		if (GetTickCount() < g_nextRetryMs)
			return;
		g_nextRetryMs = GetTickCount() + 5000;
	} else {
		g_tried = true;
		g_nextRetryMs = GetTickCount() + 5000;
	}
	BuildMap();
	if (CreateTexture(device))
		g_srvDevice = device;
}

bool Has(const char* key)
{
	return FindIdx(key) >= 0;
}

float DrawCentered(ImDrawList* dl, float cx, float y, ImU32 col, const char* key, float heightPx)
{
	if (!dl || !g_srv)
		return 0.f;
	const int idx = FindIdx(key);
	if (idx < 0)
		return 0.f;

	const auto& e = weapon_icon_atlas::kEntries[idx];
	const float u0 = (e.col * weapon_icon_atlas::kCellW) / static_cast<float>(weapon_icon_atlas::kWidth);
	const float v0 = (e.row * weapon_icon_atlas::kCellH) / static_cast<float>(weapon_icon_atlas::kHeight);
	const float u1 = ((e.col + 1) * weapon_icon_atlas::kCellW) / static_cast<float>(weapon_icon_atlas::kWidth);
	const float v1 = ((e.row + 1) * weapon_icon_atlas::kCellH) / static_cast<float>(weapon_icon_atlas::kHeight);

	const float aspect = static_cast<float>(weapon_icon_atlas::kCellW)
		/ static_cast<float>(weapon_icon_atlas::kCellH);
	float h = heightPx > 4.f ? heightPx : 18.f;
	float w = h * aspect;
	// Cap width so long rifles don't dominate; keep height (guns are wide)
	if (w > h * 2.6f) {
		w = h * 2.6f;
	}

	const float x0 = floorf(cx - w * 0.5f);
	const float y0 = floorf(y);
	const float x1 = floorf(x0 + w);
	const float y1 = floorf(y0 + h);

	const ImTextureID tex = (ImTextureID)(intptr_t)g_srv;
	// soft shadow
	const ImU32 shadow = IM_COL32(0, 0, 0, (col >> 24) * 160 / 255);
	dl->AddImage(tex, ImVec2(x0 + 1.f, y0 + 1.f), ImVec2(x1 + 1.f, y1 + 1.f),
		ImVec2(u0, v0), ImVec2(u1, v1), shadow);
	dl->AddImage(tex, ImVec2(x0, y0), ImVec2(x1, y1),
		ImVec2(u0, v0), ImVec2(u1, v1), col);

	return floorf(h + 2.f);
}

} // namespace WeaponIconDraw

