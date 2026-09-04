#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../debug/debug.h"
#include "../../../utils/memory/memsafe/memsafe.h"
#include "../../../utils/memory/patternscan/patternscan.h"
#include "../../../utils/memory/gaa/gaa.h"
#include "../../w2s/w2s.h"
#include "../../../../cs2/datatypes/viewmatrix/viewmatrix.h"
#include "../../../interfaces/interfaces.h"
#include "../../../utils/schema/schema.h"
#include "../../../utils/fnv1a/fnv1a.h"
#include "../../../offsets/offsets.h"
#include "../../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../utils/console/console.h"

#include <Windows.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>

// DRAWLEGS - Firstperson Legs render passes
void __fastcall H::hkDrawLegs(void* a1, void* a2, void* a3, void* a4, void* a5) {
	if (Config::remove_legs)
		return;
	auto original = DrawLegs.GetOriginal();
	if (original) {
		__try { original(a1, a2, a3, a4, a5); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("DrawLegs original", GetExceptionCode()); }
	}
}

// DRAWSMOKEVERTEX - smoke volume draw dispatcher
std::int64_t __fastcall H::hkDrawSmokeVertex(void* a1, void* a2, int a3, int a4, void* a5, void* a6) {
	if (Config::remove_smoke)
		return 0;
	auto original = DrawSmokeVertex.GetOriginal();
	if (original) {
		std::int64_t ret = 0;
		__try { ret = original(a1, a2, a3, a4, a5, a6); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("DrawSmokeVertex original", GetExceptionCode()); ret = 0; }
		return ret;
	}
	return 0;
}

// RENDERDECALS - bullet/blood/explosion decal passes
void* __fastcall H::hkRenderDecals(void* a1, void* a2, char a3, char a4) {
	if (Config::remove_decals)
		return nullptr;
	auto original = RenderDecals.GetOriginal();
	if (original)
		return original(a1, a2, a3, a4);
	return nullptr;
}

// Keep weather / map precip when blanket particle remove is on.
static bool ParticleNameIsWeather(const char* name) {
	if (!name || !Mem::IsUserPtr(const_cast<char*>(name)))
		return false;
	__try {
		if (!name[0])
			return false;
		char buf[160]{};
		for (int i = 0; i < 159; ++i) {
			const char c = name[i];
			if (!c) { buf[i] = 0; break; }
			buf[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + 32) : c);
		}
		buf[159] = 0;
		// Keep custom bin/* weather (falling_snow / ember / stars) + stock rain_fx
		return std::strstr(buf, "rain") || std::strstr(buf, "snow")
			|| std::strstr(buf, "ash") || std::strstr(buf, "weather")
			|| std::strstr(buf, "precip") || std::strstr(buf, "fog")
			|| std::strstr(buf, "dust") || std::strstr(buf, "env_fx")
			|| std::strstr(buf, "ember") || std::strstr(buf, "star")
			|| std::strstr(buf, "falling_") || std::strstr(buf, "bin/");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// IDA CacheParticleEffect @ 0x18078EE10 - real effect spawn.
void* __fastcall H::hkCacheParticleEffect(void* mgr, unsigned int* outIndex, const char* name,
	int attach, void* entity, void* a6, void* a7, int a8)
{
	auto original = CacheParticleEffect.GetOriginal();
	if (!original)
		return outIndex;
	void* ret = outIndex;
	__try { ret = original(mgr, outIndex, name, attach, entity, a6, a7, a8); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("CacheParticleEffect original", GetExceptionCode()); ret = outIndex; }
	return ret;
}

// -- Smoke color -------------------------------------------------------------
// Entity m_vSmokeColor is NOT re-read by SmokeRenderer each frame.
// DrawSmokeArray (IDA 0x180CB4190) pulls RGB from smoke VOLUME objects:
// volume+0xD0 = primary RGB (v24[0..2])
// volume+0x100 = secondary RGB (v24[12..14])
// Active volumes live in a linked list:
// head index: word at dword_1823FDFC0
// entries: base[index*16] = volume*, next word @ +10
// Resolved from gather pattern (sub_180CB7EF0).

namespace {
	// IDA SmokeConstantBuffer source floats
	constexpr uintptr_t kVolColor0 = 0xD0;  // primary RGB
	constexpr uintptr_t kVolColor1 = 0x100; // secondary RGB
	constexpr uintptr_t kVolStride = 16;    // list entry size
	constexpr int kMaxSmokeVols = 16;       // engine cap ("Unable to render more than %d smokes")

	// dump schema fallback for entity field
	constexpr uint32_t kSmokeColorOffDump = 0x1284;
	constexpr uint32_t kSmokeDidOffDump = 0x127C;

	std::uint16_t* g_pSmokeHead = nullptr; // word head index (0xFFFF = empty)
	void* g_pSmokeListBase = nullptr;      // array base (not double ptr)
	bool g_smokeListResolved = false;
	bool g_smokeListFailed = false;

	void ResolveSmokeVolumeList() {
		if (g_smokeListResolved || g_smokeListFailed)
			return;

		// sub_180CB7EF0: movzx eax, word ptr [rip+head]; xor esi,esi
		const uintptr_t headHit = M::patternScan("client",
			"0F B7 05 ? ? ? ? 33 F6 4D 89 6B 10");
		// mov rcx, [rip+list]; movzx edx, word ptr [rip+flags]
		const uintptr_t listHit = M::patternScan("client",
			"48 8B 0D ? ? ? ? 0F B7 15 ? ? ? ? 4D 89 63 08");

		if (!headHit || !listHit) {
			g_smokeListFailed = true;
			Con::OffsetMiss("SmokeVolumeList (head/list)");
			return;
		}

		g_pSmokeHead = reinterpret_cast<std::uint16_t*>(
			M::getAbsoluteAddress(headHit, 3));
		// getAbsoluteAddress on mov rcx,[rip+disp] -> address of the qword (list base storage)
		// code loads the qword into rcx and uses it as array base
		void** ppList = reinterpret_cast<void**>(
			M::getAbsoluteAddress(listHit, 3));
		if (!g_pSmokeHead || !ppList) {
			g_smokeListFailed = true;
			Con::OffsetMiss("SmokeVolumeList GAA");
			return;
		}
		// Store the address of the global qword; read *ppList each tick
		// (list base can be null until first smoke)
		g_pSmokeListBase = ppList; // actually points at the global holding the base
		g_smokeListResolved = true;
		Con::Ok("SmokeVolumeList head@%p listGlob@%p",
			(void*)g_pSmokeHead, g_pSmokeListBase);
	}

	void TintOneVolume(void* vol, float r, float g, float b) {
		if (!vol || !Mem::IsUserPtr(vol))
			return;
		__try {
			if (!Mem::IsReadable(vol, kVolColor1 + 12))
				return;
			float* c0 = reinterpret_cast<float*>(
				reinterpret_cast<char*>(vol) + kVolColor0);
			float* c1 = reinterpret_cast<float*>(
				reinterpret_cast<char*>(vol) + kVolColor1);
			// Only write if values look like colors (not garbage / huge)
			auto sane = [](float v) {
				return std::isfinite(v) && v > -4.f && v < 8.f;
			};
			if (sane(c0[0]) && sane(c0[1]) && sane(c0[2])) {
				c0[0] = r; c0[1] = g; c0[2] = b;
			}
			if (sane(c1[0]) && sane(c1[1]) && sane(c1[2])) {
				c1[0] = r; c1[1] = g; c1[2] = b;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// Walk active smoke volume list; return how many tinted.
	int TintSmokeVolumes(float r, float g, float b) {
		ResolveSmokeVolumeList();
		if (!g_smokeListResolved || !g_pSmokeHead || !g_pSmokeListBase)
			return 0;

		int n = 0;
		__try {
			const std::uint16_t head = *g_pSmokeHead;
			if (head == 0xFFFFu)
				return 0;

			// g_pSmokeListBase is address of global qword -> actual array base
			void* base = *reinterpret_cast<void**>(g_pSmokeListBase);
			if (!base || !Mem::IsUserPtr(base))
				return 0;

			std::uint16_t idx = head;
			for (int guard = 0; guard < kMaxSmokeVols && idx != 0xFFFFu; ++guard) {
				// entry @ base + idx*16: [0]=volume*, [10]=next index word
				auto* entry = reinterpret_cast<std::uint8_t*>(base) + (static_cast<std::size_t>(idx) * kVolStride);
				if (!Mem::IsReadable(entry, kVolStride))
					break;
				void* vol = *reinterpret_cast<void**>(entry);
				const std::uint16_t next = *reinterpret_cast<std::uint16_t*>(entry + 10);
				if (vol)
					TintOneVolume(vol, r, g, b);
				++n;
				if (next == idx)
					break;
				idx = next;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return n;
	}

	// Backup: still poke entity field (may help killfeed / other paths)
	void TintSmokeEntities(float r, float g, float b) {
		if (!I::GameEntity || !I::GameEntity->Instance)
			return;

		// Resolve schema offsets once - this runs inside a render hook.
		static uint32_t s_colOff = 0;
		static uint32_t s_didOff = 0;
		if (!s_colOff) {
			s_colOff = SchemaFinder::Get(
				hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_vSmokeColor"));
			if (!s_colOff)
				s_colOff = kSmokeColorOffDump;
		}
		if (!s_didOff) {
			s_didOff = SchemaFinder::Get(
				hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_bDidSmokeEffect"));
			if (!s_didOff)
				s_didOff = kSmokeDidOffDump;
		}
		const uint32_t colOff = s_colOff;
		const uint32_t didOff = s_didOff;

		const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
		if (nMax <= 0 || nMax > 8192)
			return;

		int checked = 0;
		for (int i = 1; i <= nMax && checked < 64; ++i) {
			auto* ent = I::GameEntity->Instance->Get(i);
			if (!Mem::ValidEntity(ent))
				continue;

			// Quick designer check first to skip dump_class_info on non-smoke slots
			CEntityIdentity* id = nullptr;
			if (Mem::ReadField(ent, Offset::m_pEntity(), id) && id && Mem::Valid(id, 0x28)) {
				const char* designer = nullptr;
				if (Mem::ReadField(id, Offset::m_designerName(), designer) && designer && Mem::IsReadable(designer, 12)) {
					if (!std::strstr(designer, "smoke"))
						continue;
				}
			}

			char clsName[128]{};
			if (!Mem::SchemaClassName(ent, clsName, sizeof(clsName)))
				continue;
			if (HASH(clsName) != HASH("C_SmokeGrenadeProjectile"))
				continue;
			++checked;

			auto* base = reinterpret_cast<std::uint8_t*>(ent);
			bool did = false;
			__try {
				if (Mem::IsReadable(base + didOff, 1))
					did = base[didOff] != 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!did)
				continue;

			__try {
				float* col = reinterpret_cast<float*>(base + colOff);
				if (!Mem::IsReadable(col, 12))
					continue;
				col[0] = r;
				col[1] = g;
				col[2] = b;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
	}

	void ApplySmokeTintNow() {
		if (!Config::smoke_color || Config::remove_smoke)
			return;
		if (Config::loading.load(std::memory_order_acquire))
			return;
		if (H::SessionMapLeaving() || H::SessionPostMatch())
			return;

		const float r = std::clamp(Config::smoke_color_value.x, 0.f, 1.f);
		const float g = std::clamp(Config::smoke_color_value.y, 0.f, 1.f);
		const float b = std::clamp(Config::smoke_color_value.z, 0.f, 1.f);

		// Primary path: volume list used by SmokeRenderer
		const int vols = TintSmokeVolumes(r, g, b);
		// Entity field as backup (schema / other consumers)
		TintSmokeEntities(r, g, b);
		(void)vols;
	}
} // namespace

// DRAWSMOKEARRAY - IDA 0x180CB4190 SmokeConstantBuffer batch
// Tint volumes BEFORE original so CB picks up new RGB at +0xD0.
std::int64_t __fastcall H::hkDrawSmokeArray(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
	if (Config::remove_smoke)
		return 0;
	if (Config::smoke_color) {
		// Throttle to ~10 Hz: ApplySmokeTintNow walks the volume list + a full
		// entity sweep - per draw call it was measurable FPS tax with the color
		// option on (same reason ApplySmokeColorTick is throttled).
		static std::atomic<ULONGLONG> s_lastTintMs{ 0 };
		const ULONGLONG now = GetTickCount64();
		ULONGLONG last = s_lastTintMs.load(std::memory_order_relaxed);
		if (now - last >= 100
			&& s_lastTintMs.compare_exchange_strong(last, now, std::memory_order_relaxed))
			ApplySmokeTintNow();
	}
	auto original = DrawSmokeArray.GetOriginal();
	if (original) {
		std::int64_t ret = 0;
		__try { ret = original(a1, a2, a3, a4, a5, a6); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("DrawSmokeArray original", GetExceptionCode()); ret = 0; }
		return ret;
	}
	return 0;
}

// Called from World::Update (~frame) when smoke_color on.
// DrawSmokeArray already tints right before draw - this is backup only.
// Throttle: full entity walk every frame was free FPS tax with smoke_color on.
void ApplySmokeColorTick() {
	if (!Config::smoke_color || Config::remove_smoke)
		return;
	if (Config::loading.load(std::memory_order_acquire))
		return;

	static std::uint64_t s_lastMs = 0;
	const std::uint64_t now = GetTickCount64();
	if (s_lastMs != 0 && now < s_lastMs + 80ull) // ~12.5 Hz backup
		return;
	s_lastMs = now;
	ApplySmokeTintNow();
}

// -- Particle color (fire / explosion) ---------------------------------------
// UC Particle Modulation (particles.dll ParticleDrawArray):
// pattern: 48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55 @ particles.dll
// IDA sub_1802826D0
// ParticleContext_t: data* @ +0x48, float RGB @ +0x50/+0x54/+0x58
// Name: a2+0x48 -> +0x18 nameptr -> +0x8 char** -> *psz
namespace ParticleColorFx {
	struct ParticleData_t {
		char pad0[0x18];
		void* m_pNamePtr; // +0x18
	};
	struct ParticleName_t {
		char pad0[0x8];
		const char** m_pszName; // +0x8
	};
	struct ParticleContext_t {
		char pad0[0x48];
		ParticleData_t* m_pData; // +0x48
		float m_flRed;           // +0x50
		float m_flGreen;         // +0x54
		float m_flBlue;          // +0x58
	};

	// Lowercase name into buf; return false on bad ptr / empty.
	bool LowerName(const char* name, char* buf, int cap) {
		if (!name || !buf || cap < 4 || !Mem::IsReadable(name, 4))
			return false;
		__try {
			for (int i = 0; i < cap - 1; ++i) {
				const char c = name[i];
				if (!c) { buf[i] = 0; break; }
				buf[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + 32) : c);
			}
			buf[cap - 1] = 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return buf[0] != 0;
	}

	// IDA client strings: groundfire_remnant.vpcf, inferno_bodyburn, molotov_fire
	// Match broad fire/molly/inferno tokens - old only inferno_fx missed live names.
	bool NameIsInferno(const char* lower) {
		if (!lower) return false;
		if (std::strstr(lower, "inferno")) return true;
		if (std::strstr(lower, "molotov")) return true;
		if (std::strstr(lower, "incendiary")) return true;
		if (std::strstr(lower, "incgrenade")) return true;
		// flame/burn with molotov context also inferno
		if (std::strstr(lower, "flame") && (std::strstr(lower, "molo") || std::strstr(lower, "inferno"))) return true;
		return false;
	}

	bool NameIsFire(const char* lower) {
		if (!lower) return false;
		if (NameIsInferno(lower)) return true;
		if (std::strstr(lower, "groundfire")) return true;
		if (std::strstr(lower, "burning")) return true;
		if (std::strstr(lower, "flame")) return true;
		if (std::strstr(lower, "burn")) return true;
		if (std::strstr(lower, "ember")) return true;
		// bare fire particle paths (avoid "fire" in unrelated names via path)
		if (std::strstr(lower, "fire") && (std::strstr(lower, "fx")
			|| std::strstr(lower, "particle") || std::strstr(lower, ".vpcf")
			|| std::strstr(lower, "burn") || std::strstr(lower, "flame")
			|| std::strstr(lower, "molotov") || std::strstr(lower, "inferno")
			|| std::strstr(lower, "env_")))
			return true;
		// fallback: any path containing fire
		if (std::strstr(lower, "fire"))
			return true;
		return false;
	}

	// HE / C4 / generic explosion FX - exclude molotov (handled as fire/inferno).
	bool NameIsExplosion(const char* lower) {
		if (!lower) return false;
		if (NameIsInferno(lower) || std::strstr(lower, "groundfire"))
			return false;
		if (std::strstr(lower, "explosions_fx")) return true;
		if (std::strstr(lower, "explosion_he")) return true;
		if (std::strstr(lower, "hegrenade")) return true;
		if (std::strstr(lower, "he_grenade")) return true;
		if (std::strstr(lower, "c4_explosion")) return true;
		if (std::strstr(lower, "bomb_explosion")) return true;
		if (std::strstr(lower, "explosion")) return true;
		return false;
	}

	// IDA ParticleDrawArray a2: data* @ +0x48, RGB oword @ +0x50.
	// Name layout varies by build - try several chains.
	const char* ContextName(ParticleContext_t* ctx) {
		if (!ctx || !Mem::IsReadable(ctx, 0x60))
			return nullptr;
		__try {
			void* data = ctx->m_pData;
			if (!data || !Mem::IsReadable(data, 0x30))
				return nullptr;
			const auto base = reinterpret_cast<std::uintptr_t>(data);

			// Path A (UC): data+0x18 -> obj+0x8 -> char*
			{
				void* nameObj = *reinterpret_cast<void**>(base + 0x18);
				if (nameObj && Mem::IsReadable(nameObj, 0x10)) {
					const char** ppsz = *reinterpret_cast<const char***>(
						reinterpret_cast<std::uintptr_t>(nameObj) + 0x8);
					if (ppsz && Mem::IsReadable(ppsz, 8)) {
						const char* n = *ppsz;
						if (n && Mem::IsReadable(n, 1) && n[0])
							return n;
					}
					// obj itself may be CUtlString / direct char*
					const char* direct = *reinterpret_cast<const char**>(nameObj);
					if (direct && Mem::IsReadable(direct, 1) && direct[0] > 0x20)
						return direct;
					// CUtlString at nameObj+0x0
					const char* s0 = *reinterpret_cast<const char**>(reinterpret_cast<uintptr_t>(nameObj));
					if (s0 && Mem::IsReadable(s0, 4) && s0[0] > 0x20 && s0[0] < 0x7f)
						return s0;
				}
			}
			// Path B: data+0x8 / +0x10 direct CUtlString ptr - try wider window
			for (std::uintptr_t off : { 0x0ull, 0x8ull, 0x10ull, 0x18ull, 0x20ull, 0x28ull, 0x30ull }) {
				if (!Mem::IsReadable(reinterpret_cast<void*>(base + off), 8))
					continue;
				const char* n = *reinterpret_cast<const char**>(base + off);
				if (n && Mem::IsReadable(n, 4) && n[0] > 0x20 && n[0] < 0x7f) {
					// validate plausible path
					if (n[0] == 'p' || n[0] == 'm' || n[0] == 'i' || n[0] == 'g' || n[0] == 'e')
						return n;
					// fallback: any readable string with '/' or '_' or '.'
					for (int k = 0; k < 48 && n[k]; ++k) {
						if (n[k] == '/' || n[k] == '_' || n[k] == '.')
							return n;
					}
				}
				// try double-indirect (CUtlString -> char* at +0x0 or +0x8)
				void* p = *reinterpret_cast<void**>(base + off);
				if (p && Mem::IsReadable(p, 8)) {
					const char* s = *reinterpret_cast<const char**>(p);
					if (s && Mem::IsReadable(s, 4) && s[0] > 0x20 && s[0] < 0x7f)
						return s;
					const char* s2 = *reinterpret_cast<const char**>(reinterpret_cast<uintptr_t>(p) + 8);
					if (s2 && Mem::IsReadable(s2, 4) && s2[0] > 0x20 && s2[0] < 0x7f)
						return s2;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		return nullptr;
	}

	void ApplyTint(ParticleContext_t* ctx, float r, float g, float b) {
		if (!ctx)
			return;
		r = std::clamp(r, 0.f, 1.f);
		g = std::clamp(g, 0.f, 1.f);
		b = std::clamp(b, 0.f, 1.f);
		__try {
			// IDA: *(OWORD*)(a2+80) is RGB - write floats @ +0x50
			ctx->m_flRed = r;
			ctx->m_flGreen = g;
			ctx->m_flBlue = b;
		} __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("removals.particleColorFx"); }
	}
} // namespace ParticleColorFx

// particles.dll ParticleDrawArray - live RGB modulation
// IDA sub_1802826D0: data @ a2+0x48, RGB oword @ a2+0x50
void* __fastcall H::hkParticleDrawArray(void* a1, void* a2, void* a3, void* a4, void* a5) {
	const bool wantFire = Config::fire_color;
	const bool wantInferno = Config::inferno_color;
	const bool wantExpl = Config::explosion_color;
	auto original = ParticleDrawArray.GetOriginal();

	if ((wantFire || wantInferno || wantExpl) && a2 && Mem::IsReadable(a2, 0x60)
		&& !H::SessionMapLeaving() && !H::SessionPostMatch()) {
		auto* ctx = reinterpret_cast<ParticleColorFx::ParticleContext_t*>(a2);
		enum : std::uint8_t { kOther = 0, kFire = 1, kExpl = 2, kInferno = 3 };
		struct ClassSlot {
			void* data = nullptr;
			std::uint8_t cls = kOther;
			ULONGLONG stamp = 0; // last (re)classification time - heap addresses recycle
		};
		static ClassSlot s_cls[256]{};
		if (H::SessionMapLeaving()) {
			// Freed particle contexts' heap addresses get reused by different
			// particle types across rounds - stale pointer-keyed slots would
			// tint the wrong class (explosion as inferno). Wipe on map leave.
			for (auto& s : s_cls) s = {};
		}
		const ULONGLONG nowMs = GetTickCount64();
		std::uint8_t kind = kOther;
		void* dataKey = nullptr;
		bool haveClass = false;
		dataKey = ctx->m_pData;
		if (dataKey) {
			const auto u = reinterpret_cast<std::uintptr_t>(dataKey);
			const int home = static_cast<int>(((u >> 4) ^ (u >> 9)) & 255u);
			for (int p = 0; p < 4; ++p) {
				const ClassSlot& s = s_cls[(home + p) & 255];
				if (s.data == dataKey) {
					if (nowMs - s.stamp <= 2000) {
						kind = s.cls;
						haveClass = true;
					}
					// stale entry -> fall through to re-classify (address may
					// have been recycled by a different particle type)
					break;
				}
			}
			if (!haveClass) {
				const char* name = ParticleColorFx::ContextName(ctx);
				char lower[192]{};
				if (name && ParticleColorFx::LowerName(name, lower, sizeof(lower))) {
					if (ParticleColorFx::NameIsInferno(lower))
						kind = kInferno;
					else if (ParticleColorFx::NameIsFire(lower))
						kind = kFire;
					else if (ParticleColorFx::NameIsExplosion(lower))
						kind = kExpl;
					else
						kind = kOther;
				}
				int write = home;
				for (int p = 0; p < 4; ++p) {
					const int i = (home + p) & 255;
					if (!s_cls[i].data || s_cls[i].data == dataKey) {
						write = i;
						break;
					}
				}
				s_cls[write].data = dataKey;
				s_cls[write].cls = kind;
				s_cls[write].stamp = nowMs;
				haveClass = true;
			}
		}
		if (haveClass) {
			if (wantInferno && kind == kInferno) {
				ParticleColorFx::ApplyTint(ctx,
					Config::inferno_color_value.x,
					Config::inferno_color_value.y,
					Config::inferno_color_value.z);
			} else if (wantFire && kind == kFire) {
				ParticleColorFx::ApplyTint(ctx,
					Config::fire_color_value.x,
					Config::fire_color_value.y,
					Config::fire_color_value.z);
			} else if (wantFire && kind == kInferno && !wantInferno) {
				// Fire tint fallback: molotov/inferno particles should still tint when only Fire enabled
				ParticleColorFx::ApplyTint(ctx,
					Config::fire_color_value.x,
					Config::fire_color_value.y,
					Config::fire_color_value.z);
			} else if (wantExpl && kind == kExpl) {
				ParticleColorFx::ApplyTint(ctx,
					Config::explosion_color_value.x,
					Config::explosion_color_value.y,
					Config::explosion_color_value.z);
			}
		} else if ((wantFire || wantInferno) && !dataKey) {
			// dataKey null is rare (no particle data) - fallback only here to avoid over-tinting kOther.
			if (wantInferno) {
				ParticleColorFx::ApplyTint(ctx,
					Config::inferno_color_value.x,
					Config::inferno_color_value.y,
					Config::inferno_color_value.z);
			} else if (wantFire) {
				ParticleColorFx::ApplyTint(ctx,
					Config::fire_color_value.x,
					Config::fire_color_value.y,
					Config::fire_color_value.z);
			}
		}
	}

	if (original)
		return original(a1, a2, a3, a4, a5);
	return nullptr;
}

namespace {
	bool LocalIsScoped() {
		C_CSPlayerPawn* lp = H::SafeLocalPlayer();
		if (!lp)
			return false;
		bool scoped = false;
		__try { scoped = lp->m_bIsScoped(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return scoped;
	}
} // namespace

bool __fastcall H::hkDrawCrosshair(void* a1) {
	if (Config::remove_crosshair)
		return false;
	// Force crosshair: show when unscoped; vanish when scoped (real sniper feel)
	if (Config::force_crosshair) {
		if (LocalIsScoped())
			return false;
		return true;
	}
	auto original = DrawCrosshair.GetOriginal();
	if (original) {
		bool ret = true;
		__try { ret = original(a1); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("DrawCrosshair original", GetExceptionCode()); ret = true; }
		return ret;
	}
	return true;
}

// Pattern removed this build (not in patterns dump). force_crosshair is DrawCrosshair-only.
// If a future dump re-adds this site, hook install will resume automatically.
/* removed-hook symbols (not in current hooks.h)
bool __fastcall H::hkWeaponHidesCrosshair(void* zoomData) {
	if (Config::force_crosshair) {
		// true = weapon hides crosshair. Scoped -> hide; unscoped -> force show.
		if (LocalIsScoped())
			return true;
		return false;
	}
	auto original = WeaponHidesCrosshair.GetOriginal();
	if (original) return original(zoomData);
	return false;
}

// Legacy symbol kept so vcxproj/hooks link; dump pattern pointed at dem-file code.
// Real work is ApplyPostProcessRemovalTick (ConVar poke).
void __fastcall H::hkUpdatePostProcessing(void* a1, void* a2) {
	(void)a1;
	(void)a2;
	// No-op if ever installed - do not call dem path.
}
*/

