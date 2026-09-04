#include "skin_preview.h"
#include "skin_sdk.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <Windows.h>

#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/vfunc/vfunc.h"

static SkinPreview g_preview;

namespace
{
	enum EImageFormat : uint32_t { EIMGFMT_RGBA8888 = 4 };

	struct PanoramaImageData_t {
		PanoramaImageData_t() {
			ZeroMemory(this, sizeof(*this));
			m_szImagePath = nullptr;
			m_iWidth = -1;
			m_iHeight = -1;
			m_iUnk1 = -1;
			m_iUnk2 = -1;
			m_flScale = 1.333f;
			m_iUnk3 = 1;
			m_iUnk4 = 1;
		}
		const char* m_szImagePath;
		int m_iWidth;
		int m_iHeight;
		int m_iUnk1;
		int m_iUnk2;
		float m_flScale;
		char pad0[0x30];
		int m_iUnk3;
		char pad1[0x18];
		int m_iUnk4;
		char pad2[0x2C];
	};

	class CPanoramaTextureDx11 {
	public:
		char pad0[0x10];
		ID3D11ShaderResourceView* m_pSRV_SRGB;
		ID3D11ShaderResourceView* m_pSRV_UNORM;
	};
	class CSource2UITexture {
	public:
		class CData { public: CPanoramaTextureDx11* m_pDx11Texture; };
		char pad0[0x28];
		CData* m_pData;
		int m_iWidth;
		int m_iHeight;
	};
	class CImageProxySource {
	public:
		CSource2UITexture* GetTextureID() {
			using Fn = CSource2UITexture*(__fastcall*)(CImageProxySource*);
			return (*reinterpret_cast<Fn**>(this))[4](this);
		}
		void AddRef() {
			using Fn = void(__fastcall*)(CImageProxySource*);
			(*reinterpret_cast<Fn**>(this))[10](this);
		}
		void Release() {
			using Fn = void(__fastcall*)(CImageProxySource*);
			(*reinterpret_cast<Fn**>(this))[11](this);
		}
		ID3D11ShaderResourceView* GetNativeTexture() {
			if (!this || !Mem::IsUserPtr(this)) return nullptr;
			CSource2UITexture* pTex = nullptr;
			__try {
				pTex = GetTextureID();
			} __except (EXCEPTION_EXECUTE_HANDLER) { pTex = nullptr; }
			if (!pTex || !Mem::IsUserPtr(pTex)) return nullptr;
			CSource2UITexture::CData* pData = nullptr;
			__try {
				pData = pTex->m_pData;
			} __except (EXCEPTION_EXECUTE_HANDLER) { pData = nullptr; }
			if (!pData || !Mem::IsUserPtr(pData)) return nullptr;
			CPanoramaTextureDx11* dx = nullptr;
			__try {
				dx = pData->m_pDx11Texture;
			} __except (EXCEPTION_EXECUTE_HANDLER) { dx = nullptr; }
			if (!dx || !Mem::IsUserPtr(dx)) return nullptr;
			__try {
				return dx->m_pSRV_SRGB ? dx->m_pSRV_SRGB : dx->m_pSRV_UNORM;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}
	};
	class CImageResourceManager {
	public:
		CImageProxySource* LoadImageInternal(const char* szPath, EImageFormat fmt) {
			PanoramaImageData_t imageData{};
			imageData.m_szImagePath = szPath;
			using Fn = CImageProxySource*(__fastcall*)(CImageResourceManager*, void*, void*,
				const char*, EImageFormat, PanoramaImageData_t*);
			return (*reinterpret_cast<Fn**>(this))[0](this, nullptr, nullptr, szPath, fmt, &imageData);
		}
	};
	class CUIEngineSource2 {
	public:
		CImageResourceManager* GetResourceManager() {
			using Fn = CImageResourceManager*(__fastcall*)(CUIEngineSource2*);
			return (*reinterpret_cast<Fn**>(this))[24](this);
		}
	};
	class IPanoramaUIEngine {
	public:
		CUIEngineSource2* AccessUIEngine() {
			using Fn = CUIEngineSource2*(__fastcall*)(IPanoramaUIEngine*);
			return (*reinterpret_cast<Fn**>(this))[13](this);
		}
	};

	struct ImgCacheEntry { void* proxy = nullptr; bool confirmedMiss = false; };
	static std::unordered_map<std::string, ImgCacheEntry> g_cache;
	static std::unordered_map<std::string, std::string> g_modelPathHit;
	static int g_loadsThisFrame = 0;
	static DWORD g_loadFrameTick = 0;
	constexpr int kMaxNewLoadsPerFrame = 6;

	static void BeginFrameBudget() {
		const DWORD t = GetTickCount();
		if (t != g_loadFrameTick) {
			g_loadFrameTick = t;
			g_loadsThisFrame = 0;
		}
	}
	static std::string ToS2R(const std::string& iconPath) {
		if (iconPath.empty()) return {};
		std::string path = iconPath;
		if (path.size() > 2 && path.compare(path.size() - 2, 2, "_c") == 0)
			path.resize(path.size() - 2);
		if (path.compare(0, 6, "s2r://") != 0)
			path = "s2r://" + path;
		return path;
	}
	static ID3D11ShaderResourceView* SEH_GetNative(void* pImg) {
		ID3D11ShaderResourceView* out = nullptr;
		__try { out = reinterpret_cast<CImageProxySource*>(pImg)->GetNativeTexture(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
		return out;
	}
	static CUIEngineSource2* SEH_AccessUI(IPanoramaUIEngine* p) {
		CUIEngineSource2* out = nullptr;
		__try { out = p->AccessUIEngine(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return out;
	}
	static CImageResourceManager* SEH_GetResMgr(CUIEngineSource2* p) {
		CImageResourceManager* out = nullptr;
		__try { out = p->GetResourceManager(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return out;
	}
	static CImageProxySource* SEH_Load(CImageResourceManager* pMgr, const char* path) {
		CImageProxySource* out = nullptr;
		__try { out = pMgr->LoadImageInternal(path, EIMGFMT_RGBA8888); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return out;
	}
	static void SEH_AddRef(void* p) {
		__try { reinterpret_cast<CImageProxySource*>(p)->AddRef(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	static void SEH_Release(void* p) {
		__try { reinterpret_cast<CImageProxySource*>(p)->Release(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	static IPanoramaUIEngine* GetPanorama() {
		static IPanoramaUIEngine* s = nullptr;
		if (!s)
			s = I::Get<IPanoramaUIEngine>("panorama.dll", "PanoramaUIEngine001");
		return s;
	}
	static bool LooksLikeGlove(const char* n) {
		if (!n || !n[0]) return false;
		return strstr(n, "glove") || strstr(n, "handwrap") || strstr(n, "sporty_")
			|| strstr(n, "slick_") || strstr(n, "motorcycle_") || strstr(n, "specialist_")
			|| strstr(n, "bloodhound") || strstr(n, "hydra") || strstr(n, "brokenfang")
			|| strstr(n, "leather_") || strstr(n, "studded_");
	}
	static bool LooksLikeKnife(const char* n) {
		if (!n || !n[0]) return false;
		return strstr(n, "knife") || strstr(n, "bayonet") || strstr(n, "karambit")
			|| strstr(n, "butterfly") || strstr(n, "dagger") || strstr(n, "bowie")
			|| strstr(n, "falchion") || strstr(n, "flip") || strstr(n, "gut")
			|| strstr(n, "tactical") || strstr(n, "ursus") || strstr(n, "canis")
			|| strstr(n, "cord") || strstr(n, "push") || strstr(n, "stiletto")
			|| strstr(n, "widowmaker") || strstr(n, "skeleton") || strstr(n, "kukri")
			|| strstr(n, "outdoor") || strstr(n, "gypsy");
	}

	static int LoadSrv(const std::string& iconPath, ID3D11ShaderResourceView** outSrv, bool* outLoading)
	{
		*outSrv = nullptr;
		if (outLoading) *outLoading = false;
		if (iconPath.empty()) return -1;
		BeginFrameBudget();
		const std::string path = ToS2R(iconPath);
		if (path.empty()) return -1;

		auto it = g_cache.find(path);
		if (it != g_cache.end()) {
			if (it->second.confirmedMiss) return -1;
			if (!it->second.proxy) return -1;
			auto* srv = SEH_GetNative(it->second.proxy);
			if (!srv) {
				if (outLoading) *outLoading = true;
				return 0;
			}
			*outSrv = srv;
			return 1;
		}
		if (!SkinSdk::FileExistsGame(iconPath.c_str())) {
			g_cache[path] = { nullptr, true };
			return -1;
		}
		if (g_loadsThisFrame >= kMaxNewLoadsPerFrame) {
			if (outLoading) *outLoading = true;
			return 0;
		}
		auto* pPanorama = GetPanorama();
		if (!pPanorama) return -1;
		auto* pUI = SEH_AccessUI(pPanorama);
		if (!pUI) return -1;
		auto* pRes = SEH_GetResMgr(pUI);
		if (!pRes) return -1;
		++g_loadsThisFrame;
		auto* pImage = SEH_Load(pRes, path.c_str());
		if (!pImage) {
			g_cache[path] = { nullptr, true };
			return -1;
		}
		SEH_AddRef(pImage);
		g_cache[path] = { pImage, false };
		auto* srv = SEH_GetNative(pImage);
		if (!srv) {
			if (outLoading) *outLoading = true;
			return 0;
		}
		*outSrv = srv;
		return 1;
	}
}

void SkinPreview::Shutdown()
{
	for (auto& kv : g_cache) {
		if (kv.second.proxy)
			SEH_Release(kv.second.proxy);
	}
	g_cache.clear();
	g_modelPathHit.clear();
}

ImTextureID SkinPreview::GetTexture(const std::string& iconPath)
{
	ID3D11ShaderResourceView* srv = nullptr;
	bool loading = false;
	const int r = LoadSrv(iconPath, &srv, &loading);
	if (r == 1 && srv)
		return reinterpret_cast<ImTextureID>(srv);
	return (ImTextureID)0;
}

ImTextureID SkinPreview::GetModelTexture(const char* simpleName)
{
	if (!simpleName || !simpleName[0])
		return (ImTextureID)0;
	auto hit = g_modelPathHit.find(simpleName);
	if (hit != g_modelPathHit.end()) {
		if (hit->second.empty())
			return (ImTextureID)0;
		return GetTexture(hit->second);
	}
	const char* names[8]{};
	int nn = 0;
	names[nn++] = simpleName;
	if (!strncmp(simpleName, "weapon_", 7) && simpleName[7])
		names[nn++] = simpleName + 7;
	static const char* const kFmts[] = {
		"panorama/images/econ/weapons/base_weapons/%s_png.vtex_c",
		"panorama/images/econ/weapons/melee/%s_png.vtex_c",
		"panorama/images/econ/weapons/knife/%s_png.vtex_c",
		"panorama/images/econ/weapons/%s_png.vtex_c",
		"panorama/images/econ/default_generated/%s_light_png.vtex_c",
		"panorama/images/econ/weapons/base_weapons/%s_large_png.vtex_c",
		"panorama/images/econ/weapons/melee/%s_large_png.vtex_c",
		"panorama/images/econ/weapons/gloves/%s_large_png.vtex_c",
	};
	char shortName[128]{};
	if (LooksLikeGlove(simpleName)) {
		const char* g = strstr(simpleName, "_gloves");
		if (g && g != simpleName) {
			const size_t n = static_cast<size_t>(g - simpleName);
			if (n < sizeof(shortName)) {
				memcpy(shortName, simpleName, n);
				shortName[n] = '\0';
				if (shortName[0]) names[nn++] = shortName;
			}
		}
	}
	char altBlood[64]{};
	if (!strcmp(simpleName, "bloodhound_gloves")) {
		strcpy_s(altBlood, "studded_bloodhound_gloves");
		names[nn++] = altBlood;
	}
	char buf[512];
	bool anyPending = false;
	for (int ni = 0; ni < nn; ++ni) {
		if (!names[ni] || !names[ni][0]) continue;
		for (const char* fmt : kFmts) {
			if (strstr(fmt, "gloves/") && !LooksLikeGlove(simpleName))
				continue;
			if ((strstr(fmt, "melee/") || strstr(fmt, "knife/")) && !LooksLikeKnife(simpleName))
				continue;
			if (sprintf_s(buf, fmt, names[ni]) <= 0)
				continue;
			ID3D11ShaderResourceView* srv = nullptr;
			bool loading = false;
			const int r = LoadSrv(buf, &srv, &loading);
			if (r == 1 && srv) {
				g_modelPathHit[simpleName] = buf;
				return reinterpret_cast<ImTextureID>(srv);
			}
			if (r == 0 || loading) {
				anyPending = true;
				break;
			}
		}
		if (anyPending) break;
	}
	if (anyPending)
		return (ImTextureID)0;
	g_modelPathHit[simpleName] = {};
	return (ImTextureID)0;
}

ImTextureID SkinPreview::GetPaintTexture(const char* simpleName, const char* kitToken)
{
	if (!simpleName || !simpleName[0] || !kitToken || !kitToken[0])
		return (ImTextureID)0;
	const char* names[4]{};
	int nn = 0;
	names[nn++] = simpleName;
	char alt[64]{};
	if (!strcmp(simpleName, "bloodhound_gloves")) { strcpy_s(alt, "studded_bloodhound_gloves"); names[nn++] = alt; }
	else if (!strcmp(simpleName, "studded_bloodhound_gloves")) { strcpy_s(alt, "bloodhound_gloves"); names[nn++] = alt; }
	else if (!strcmp(simpleName, "hydra_gloves")) { strcpy_s(alt, "studded_hydra_gloves"); names[nn++] = alt; }
	else if (!strcmp(simpleName, "brokenfang_gloves")) { strcpy_s(alt, "studded_brokenfang_gloves"); names[nn++] = alt; }
	else if (!strcmp(simpleName, "handwraps")) { strcpy_s(alt, "leather_handwraps"); names[nn++] = alt; }

	char buf[512];
	bool anyPending = false;
	static const char* const kFmts[] = {
		"panorama/images/econ/default_generated/%s_%s_light_png.vtex_c",
		"panorama/images/econ/default_generated/%s_%s_light_large_png.vtex_c",
	};
	for (int ni = 0; ni < nn; ++ni) {
		for (const char* fmt : kFmts) {
			if (sprintf_s(buf, fmt, names[ni], kitToken) <= 0)
				continue;
			ID3D11ShaderResourceView* srv = nullptr;
			bool loading = false;
			const int r = LoadSrv(buf, &srv, &loading);
			if (r == 1 && srv)
				return reinterpret_cast<ImTextureID>(srv);
			if (r == 0 || loading) {
				anyPending = true;
				break;
			}
		}
		if (anyPending) break;
	}
	if (anyPending)
		return (ImTextureID)0;
	return GetModelTexture(simpleName);
}

std::string SkinPreview::AgentPath(const char* modelOrIcon)
{
	if (!modelOrIcon || !modelOrIcon[0])
		return {};
	const char* base = strrchr(modelOrIcon, '/');
	if (!base) base = strrchr(modelOrIcon, '\\');
	base = base ? base + 1 : modelOrIcon;
	std::string name = base;
	const auto dot = name.find_last_of('.');
	if (dot != std::string::npos)
		name.resize(dot);
	if (name.find("customplayer_") == 0)
		return "panorama/images/econ/characters/" + name + "_png.vtex_c";
	return "panorama/images/econ/characters/customplayer_" + name + "_png.vtex_c";
}

SkinPreview& GetSkinPreview() { return g_preview; }

