#include "autowall.h"

#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../config/config.h"
#include "../trace/trace.h"
#include "../bones/bones.h"
#include "../aim/aim_common.h"
#include "../../hooks/hooks.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace AutoWall {
namespace {

// IDA client.dll FireBullet pen path (imagebase 0x180000000) - re-verified 2026-08-18:
// InitTraceData 0x18083E870
// CreateTrace 0x180842A00 (user pattern 4D 8D 71)
// HandleBulletPen 0x1808606F0 - return 0 continue / 1 stop; surf_pen<0.1 / dmg<1 stops
// GetTraceInfo 0x180845110 - hit_entity@+8, hitbox_data@+16 when seg type==1
// DamageToPoint 0x180844FF0 - loops ALL surfaces (aim path only)
// FreeTraceData 0x18083F5B0
// FireBullet core 0x180847660 - mask 0x1C300B, filter|0x4000000000, byte|2
// Xhair: first solid surface only. Do NOT score DamageToPoint's last entry -
// that walks the full weapon range and false-blocks on later map walls.
constexpr const char* kPatInitTraceData =
	"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 79 08 33 F6 C7 47 08 80 00 00 00";
constexpr const char* kPatInitTraceDataLoose =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8D 79 ? 33 F6 C7 47";
constexpr const char* kPatCreateTrace =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 F2 0F 10 02";
constexpr const char* kPatCreateTraceLoose =
	"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? F2 0F 10 02";
// dump TestSurfaces
constexpr const char* kPatDamageToPoint =
	"40 53 57 41 56 48 83 EC 50 8B 84 24 90 00 00 00";
constexpr const char* kPatDamageToPointLoose =
	"40 53 57 41 56 48 83 EC ? 8B 84 24";
// dump TraceHandleBulletPen
constexpr const char* kPatHandleBulletPen =
	"48 8B C4 44 89 48 20 48 89 50 10 48 89 48 08 55";
constexpr const char* kPatHandleBulletPenLoose =
	"48 8B C4 44 89 48 ? 48 89 50 ? 48 89 48 ? 55 57";
constexpr const char* kPatGetTraceInfo =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 80 00 00 00 48 8B E9 0F 29 74 24";
constexpr const char* kPatGetTraceInfoLoose =
	"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 48 8B E9 0F 29 74 24";
constexpr const char* kPatInitFilter =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F B6 41 ? 33 FF 24";
// Unique: lea rdi, [rcx+0x1C38] (embedded surfaces @ +7224)
constexpr const char* kPatFreeTraceData =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 33 F6 48 8D B9 38 1C 00 00";

// IDA InitTraceData: surfaces count@7200, ptr@7208->embedded@7224; segs ptr@+8
constexpr std::size_t kOffSurfacesCount = 0x1C20; // 7200
constexpr std::size_t kOffSurfacesPtr   = 0x1C28; // 7208
constexpr std::size_t kOffSegmentsPtr   = 0x08;
constexpr std::size_t kSegStride        = 56;
constexpr std::size_t kTraceDataSize    = 0x2200; // >= 7444
constexpr std::size_t kFilterSize       = 0x70;
// FireBullet InitFilter mask = 0x1C300B
constexpr std::uint64_t kMaskShotPen    = 0x1C300Bull;

struct TraceInfo {
	float  unk;        // +0
	float  distance;   // +4
	float  damage;     // +8  written by DamageToPoint
	std::int32_t pen_count; // +12
	std::uint16_t enter_idx; // +16
	std::uint16_t exit_idx;  // +18
	std::uint32_t flags;     // +20  bit0 = continue / solid
};
static_assert(sizeof(TraceInfo) == 24);

// InitTraceInfo / GetTraceInfo layout (FireBullet copies ~208-byte slots)
// hitbox_data -> CTraceHitboxData: Hitgroup @ +0x38 (UC + IDA GetTraceInfo path)
struct GameTraceAw {
	void* surface;      // +0
	void* hit_entity;   // +8
	void* hitbox_data;  // +16
	std::uint8_t pad[0x1E8];
};
static_assert(sizeof(GameTraceAw) >= 0x200);

// Prefer engine hitgroup from segment; else mapped aim hitbox.
int ReadTraceHitgroup(void* hitboxData, int fallbackHg) {
	if (!hitboxData || !Mem::Valid(hitboxData, 0x40))
		return fallbackHg;
	int hg = 0;
	__try {
		hg = *reinterpret_cast<int*>(
			reinterpret_cast<std::uint8_t*>(hitboxData) + 0x38);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return fallbackHg;
	}
	// Valid CS2 hitgroups: 1 head .. 8 neck (0/invalid -> keep aimbox map)
	if (hg >= 1 && hg <= 8)
		return hg;
	return fallbackHg;
}

using FnInitTraceData = void*(__fastcall*)(void* trace);
using FnCreateTrace = void(__fastcall*)(void* trace, const Vector_t* start, const Vector_t* delta,
	void* filter, int penCount, bool unk);
using FnDamageToPoint = void(__fastcall*)(void* trace, float damage, float pen, float rangeMod,
	int penCount, int team, void* unused);
using FnGetTraceInfo = void(__fastcall*)(void* trace, GameTraceAw* out, float distance, void* seg);
using FnInitFilter = void(__fastcall*)(void* filter, void* skip, std::uint64_t mask, int layer, std::uint16_t a5);
using FnFreeTraceData = void(__fastcall*)(void* trace);
// IDA 0x1808606F0 - DamageToPoint's only HandleBulletPen callee.
// char HandleBulletPen(TraceData*, PenStats*, TraceInfo* surface, int team, void* dbg)
using FnHandleBulletPen = char(__fastcall*)(void* trace, void* stats, void* surface, int team, void* unused);

FnInitTraceData g_initTraceData = nullptr;
FnCreateTrace g_createTrace = nullptr;
FnDamageToPoint g_damageToPoint = nullptr;
FnGetTraceInfo g_getTraceInfo = nullptr;
FnInitFilter g_initFilter = nullptr;
FnFreeTraceData g_freeTraceData = nullptr;
FnHandleBulletPen g_handleBulletPen = nullptr;
bool g_ready = false;
bool g_gamePath = false;

// DamageToPoint packs this then passes it to HandleBulletPen (IDA 0x180844FF0).
struct PenStats {
	float damage;    // +0  mutated in place
	float pen;       // +4  CCSWeaponBaseVData::m_flPenetration
	float rangeMod;  // +8  m_flRangeModifier
	float deltaLen;  // +12 length(CreateTrace delta) - FireBullet reads trace+7428
	int   penCount;  // +16 remaining pens; 0 -> HandleBulletPen stops
	char  stopped;   // +20 written by HandleBulletPen
	char  pad[3]{};
};
static_assert(sizeof(PenStats) == 24);
static_assert(offsetof(PenStats, penCount) == 16);
static_assert(offsetof(PenStats, stopped) == 20);

constexpr std::size_t kOffDeltaVec = 0x1D04; // 7428 - CreateTrace writes dir*range here

bool SehEntityIsPlayer(void* ent, void* local)
{
	if (!ent || ent == local)
		return false;
	bool yes = false;
	__try {
		yes = reinterpret_cast<C_BaseEntity*>(ent)->IsBasePlayer();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		yes = false;
	}
	return yes;
}

// --- perf: pen is ~0.1-0.5ms each; AF multipointxenemies tanks FPS ---
// Cache quantized eye/aim results; budget game-pen calls per ms tick.
// NEVER cache budget-miss as fail - that blocked real wallbangs for TTL.
constexpr int kCacheSlots = 64;
constexpr int kPosQuant = 4;          // tighter quant -> less multipoint reuse error
constexpr std::uint64_t kCacheTtlMs = 32;
constexpr int kMaxGamePenPerMs = 28;  // CreateTrace path / tick (was 10 - starved scan+shoot)
constexpr int kMaxSoftPenPerMs = 40;  // TraceLine fallback if SEH

struct PenCacheKey {
	void* target = nullptr;
	void* weapon = nullptr;
	int hitbox = -1;
	int allowPen = 0;
	int eyeQ[3]{};
	int aimQ[3]{};
};

struct PenCacheSlot {
	PenCacheKey key{};
	Result r{};
	std::uint64_t ms = 0;
	bool valid = false;
};

PenCacheSlot g_cache[kCacheSlots]{};
std::uint64_t g_budgetMs = 0;
int g_gamePenUsed = 0;
int g_softPenUsed = 0;

int Quant(float v) {
	return static_cast<int>(v) / kPosQuant;
}

void MakeKey(const Vector_t& eye, const Vector_t& aim, int hitbox,
	void* weapon, void* target, bool allowPen, PenCacheKey& k)
{
	k.target = target;
	k.weapon = weapon;
	k.hitbox = hitbox;
	k.allowPen = allowPen ? 1 : 0;
	k.eyeQ[0] = Quant(eye.x); k.eyeQ[1] = Quant(eye.y); k.eyeQ[2] = Quant(eye.z);
	k.aimQ[0] = Quant(aim.x); k.aimQ[1] = Quant(aim.y); k.aimQ[2] = Quant(aim.z);
}

bool KeyEq(const PenCacheKey& a, const PenCacheKey& b) {
	return a.target == b.target && a.weapon == b.weapon
		&& a.hitbox == b.hitbox && a.allowPen == b.allowPen
		&& a.eyeQ[0] == b.eyeQ[0] && a.eyeQ[1] == b.eyeQ[1] && a.eyeQ[2] == b.eyeQ[2]
		&& a.aimQ[0] == b.aimQ[0] && a.aimQ[1] == b.aimQ[1] && a.aimQ[2] == b.aimQ[2];
}

std::uint32_t KeyHash(const PenCacheKey& k) {
	// FNV-1a-ish mix of pointers + quants
	std::uint64_t h = 14695981039346656037ull;
	auto mix = [&](std::uint64_t v) {
		h ^= v;
		h *= 1099511628211ull;
	};
	mix(reinterpret_cast<std::uint64_t>(k.target));
	mix(reinterpret_cast<std::uint64_t>(k.weapon));
	mix(static_cast<std::uint64_t>(k.hitbox) | (static_cast<std::uint64_t>(k.allowPen) << 16));
	mix(static_cast<std::uint64_t>(k.eyeQ[0]) | (static_cast<std::uint64_t>(k.eyeQ[1]) << 21)
		| (static_cast<std::uint64_t>(k.eyeQ[2]) << 42));
	mix(static_cast<std::uint64_t>(k.aimQ[0]) | (static_cast<std::uint64_t>(k.aimQ[1]) << 21)
		| (static_cast<std::uint64_t>(k.aimQ[2]) << 42));
	return static_cast<std::uint32_t>(h ^ (h >> 32));
}

void TickBudget() {
	const std::uint64_t now = GetTickCount64();
	if (now != g_budgetMs) {
		g_budgetMs = now;
		g_gamePenUsed = 0;
		g_softPenUsed = 0;
	}
}

bool CacheLookup(const PenCacheKey& key, Result& out) {
	const std::uint64_t now = GetTickCount64();
	const int idx = static_cast<int>(KeyHash(key) % kCacheSlots);
	// primary + 2 linear probes
	for (int p = 0; p < 3; ++p) {
		const int i = (idx + p) % kCacheSlots;
		const PenCacheSlot& s = g_cache[i];
		if (!s.valid || now - s.ms > kCacheTtlMs)
			continue;
		if (KeyEq(s.key, key)) {
			out = s.r;
			return true;
		}
	}
	return false;
}

void CacheStore(const PenCacheKey& key, const Result& r) {
	const int idx = static_cast<int>(KeyHash(key) % kCacheSlots);
	// prefer empty / expired, else overwrite primary
	int best = idx;
	const std::uint64_t now = GetTickCount64();
	for (int p = 0; p < 3; ++p) {
		const int i = (idx + p) % kCacheSlots;
		if (!g_cache[i].valid || now - g_cache[i].ms > kCacheTtlMs) {
			best = i;
			break;
		}
	}
	g_cache[best].key = key;
	g_cache[best].r = r;
	g_cache[best].ms = now;
	g_cache[best].valid = true;
}

std::uint8_t* FindClient(const char* pat) {
	auto* p = M::FindPattern("client.dll", pat);
	if (!p)
		p = M::FindPattern("client", pat);
	return p;
}

int HitboxToHitgroup(int hb) {
	switch (hb) {
	case Config::HB_HEAD:    return 1;
	case Config::HB_NECK:    return 8;
	case Config::HB_CHEST:   return 2;
	case Config::HB_STOMACH: return 3;
	case Config::HB_PELVIS:  return 3;
	case Config::HB_ARMS:    return 4;
	case Config::HB_LEGS:    return 6;
	case Config::HB_FEET:    return 7;
	default:                 return 2;
	}
}

struct WeaponStats {
	float damage = 30.f;
	float pen = 1.f;
	float range = 8192.f;
	float rangeMod = 0.98f;
	float armorRatio = 0.5f;
	float hsMult = 4.f;
};

bool ReadWeaponStats(C_CSWeaponBase* weapon, WeaponStats& s) {
	if (!weapon)
		return false;
	auto* vdata = weapon->Data();
	if (!vdata)
		return false;

	s.damage = static_cast<float>(vdata->m_nDamage());
	s.pen = vdata->m_flPenetration();
	s.range = vdata->m_flRange();
	s.rangeMod = vdata->m_flRangeModifier();
	s.armorRatio = vdata->m_flArmorRatio();
	s.hsMult = vdata->m_flHeadshotMultiplier();

	if (!std::isfinite(s.damage) || s.damage < 1.f || s.damage > 500.f)
		s.damage = 30.f;
	if (!std::isfinite(s.pen) || s.pen < 0.f || s.pen > 10.f)
		s.pen = 1.f;
	if (!std::isfinite(s.range) || s.range < 1.f)
		s.range = 8192.f;
	if (!std::isfinite(s.rangeMod) || s.rangeMod <= 0.f || s.rangeMod > 1.f)
		s.rangeMod = 0.98f;
	if (!std::isfinite(s.armorRatio) || s.armorRatio < 0.05f || s.armorRatio > 5.f)
		s.armorRatio = 0.5f;
	if (!std::isfinite(s.hsMult) || s.hsMult < 0.5f || s.hsMult > 20.f)
		s.hsMult = 4.f;
	return true;
}

// CCSPlayer_ItemServices::m_bHasHelmet @ +0x49 (verified dump)
bool HasHelmet(C_CSPlayerPawn* target) {
	if (!target)
		return false;
	void* svc = nullptr;
	__try {
		svc = target->m_pItemServices();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!svc || !Mem::Valid(svc, 0x50))
		return false;
	__try {
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(svc) + 0x49);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

float ApplyRangeFalloff(float damage, float dist, float rangeMod) {
	if (damage <= 0.f)
		return 0.f;
	return damage * std::pow(rangeMod, dist / 500.f);
}

void ApplyArmor(float& damage, int armor, float armorRatio) {
	if (armor <= 0 || damage <= 0.f)
		return;
	// VData stores 2x ratio; ScaleDamage uses *0.5 (CS2)
	const float ratio = armorRatio * 0.5f;
	float dmgHealth = damage * ratio;
	const float dmgArmor = (damage - dmgHealth) * 0.5f;
	if (dmgArmor > static_cast<float>(armor))
		dmgHealth = damage - static_cast<float>(armor) * 0.5f;
	if (dmgHealth < 1.f)
		dmgHealth = 1.f;
	damage = dmgHealth;
}

void ScaleDamage(float& damage, int hitgroup, C_CSPlayerPawn* target, const WeaponStats& ws) {
	if (!target || !Mem::ValidEntity(target) || damage <= 0.f)
		return;

	switch (hitgroup) {
	case 1: damage *= ws.hsMult; break;           // head
	case 3: damage *= 1.25f; break;               // stomach / pelvis
	case 6: case 7: damage *= 0.75f; break;       // legs / feet
	default: break;                               // chest / arms / neck
	}

	int armor = 0;
	__try {
		armor = target->m_ArmorValue();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (armor <= 0)
		return;

	if (hitgroup == 6 || hitgroup == 7)
		return;

	const bool head = (hitgroup == 1);
	if (head && !HasHelmet(target))
		return;

	ApplyArmor(damage, armor, ws.armorRatio);
}

Result EstimateVisible(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* target)
{
	Result r{};
	WeaponStats ws{};
	if (!target)
		return r;
	if (!ReadWeaponStats(weapon, ws)) {
		ws.damage = 40.f;
		ws.rangeMod = 0.98f;
		ws.armorRatio = 0.5f;
		ws.hsMult = 4.f;
	}

	const float dist = eye.Distance(aimPoint);
	float dmg = ApplyRangeFalloff(ws.damage, dist, ws.rangeMod);
	ScaleDamage(dmg, HitboxToHitgroup(hitbox), target, ws);
	r.damage = (std::max)(dmg, 1.f);
	r.hit = true;
	r.penetrated = false;
	return r;
}

// Target-less crosshair pen probe - FireBullet's first-surface path, not
// DamageToPoint's full-map loop.
//
// IDA FireBullet 0x180847660:
//   InitTraceData -> InitFilter(mask 0x1C300B, layer 3, a5=15) + extras
//   CreateTrace(eye, dir*range, filter, pens=4, true)
//   first surface: GetTraceInfo on enter seg, then HandleBulletPen
//
// DamageToPoint walks EVERY surface along the whole weapon range. Scoring
// the last entry made almost every look Blocked (later map walls eat the
// bullet). The overlay only cares about the wall under the reticle.
//
// Return: 2 = Clear, 1 = Penetrable, 0 = Blocked, -1 = SEH / not ready.
// C-only: POD locals, no C++ dtors (C2712 with __try/__except).
int CheckCrosshairPenetrationGameSeh(
	const Vector_t& eye,
	const Vector_t& dirNormalized,
	float range,
	float damage,
	float pen,
	float rangeMod,
	int team,
	void* local)
{
	if (!g_initTraceData || !g_createTrace || !g_handleBulletPen
		|| !g_getTraceInfo || !g_initFilter || !g_freeTraceData)
		return -1;

	alignas(16) std::uint8_t traceBuf[kTraceDataSize];
	alignas(16) std::uint8_t filter[kFilterSize];
	std::memset(traceBuf, 0, sizeof(traceBuf));
	std::memset(filter, 0, sizeof(filter));

	Vector_t delta = dirNormalized * range;
	int result = -1;
	bool inited = false;

	__try {
		g_initTraceData(traceBuf);
		inited = true;

		g_initFilter(filter, local, kMaskShotPen, 3, 15);
		{
			auto* f = filter;
			*reinterpret_cast<std::uint64_t*>(f + 16) |= 0x4000000000ull;
			f[57] |= 2;
		}

		g_createTrace(traceBuf, &eye, &delta, filter, 4, true);

		const int surfaces = *reinterpret_cast<int*>(traceBuf + kOffSurfacesCount);
		auto* infos = *reinterpret_cast<TraceInfo**>(traceBuf + kOffSurfacesPtr);

		if (surfaces <= 0 || !infos) {
			result = 2; // nothing to penetrate
		} else if (surfaces > 64) {
			result = -1;
		} else {
			int solidIdx = -1;
			for (int i = 0; i < surfaces; ++i) {
				if ((infos[i].flags & 1u) != 0) {
					solidIdx = i;
					break;
				}
			}
			if (solidIdx < 0) {
				result = 2; // air-only segments
			} else {
				TraceInfo& first = infos[solidIdx];
				void* segs = *reinterpret_cast<void**>(traceBuf + kOffSegmentsPtr);
				const std::uint16_t enterIdx = static_cast<std::uint16_t>(first.enter_idx & 0x7FFF);
				if (segs && enterIdx < 128) {
					auto* seg = reinterpret_cast<std::uint8_t*>(segs)
						+ kSegStride * static_cast<std::size_t>(enterIdx);
					alignas(16) GameTraceAw gt{};
					g_getTraceInfo(traceBuf, &gt, first.distance, seg);
					if (SehEntityIsPlayer(gt.hit_entity, local))
						result = 2; // pawn under reticle - LOS, not a wall
				}

				if (result < 0) {
					float dx = *reinterpret_cast<float*>(traceBuf + kOffDeltaVec);
					float dy = *reinterpret_cast<float*>(traceBuf + kOffDeltaVec + 4);
					float dz = *reinterpret_cast<float*>(traceBuf + kOffDeltaVec + 8);
					float deltaLen = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (!std::isfinite(deltaLen) || deltaLen < 1.f)
						deltaLen = range;

					PenStats stats{};
					stats.damage = damage;
					stats.pen = pen;
					stats.rangeMod = rangeMod;
					stats.deltaLen = deltaLen;
					stats.penCount = 4;
					stats.stopped = 0;

					const char stopped = g_handleBulletPen(
						traceBuf, &stats, &first, team, nullptr);
					result = (!stopped && std::isfinite(stats.damage) && stats.damage >= 1.f)
						? 1 : 0;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		result = -1;
	}

	if (inited && g_freeTraceData) {
		__try {
			g_freeTraceData(traceBuf);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	return result;
}

bool TraceToExitSimple(const Vector_t& start, const Vector_t& dir, Vector_t& exitOut, void* skip) {
	// IDA 0x180860570 HandleBulletPen - NO separate TraceToExit callee.
	// Exit geometry lives in TraceInfo enter_idx/exit_idx after CreateTrace.
	// Soft path: step out of solid, confirm free air. Search far enough for
	// thick props (IDA has no hard thickness kill - only dmg < 1).
	bool wasSolid = true;
	for (float step = 0.25f; step <= 90.f; step += 0.5f) {
		const Vector_t p = start + dir * step;
		const Vector_t p2 = p + dir * 4.f;
		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(p, p2, skip, tr, Trace::kMaskShotPen))
			continue;
		const bool solid = tr.startsolid();
		if (wasSolid && !solid && (!Trace::DidHit(tr) || tr.fraction() > 0.02f)) {
			exitOut = p + dir * 0.25f;
			return true;
		}
		if (!solid && !Trace::DidHit(tr)) {
			exitOut = p + dir * 0.25f;
			return true;
		}
		wasSolid = solid || (Trace::DidHit(tr) && tr.fraction() < 0.02f);
	}
	return false;
}

// IDA HandleBulletPen 0x1808606F0:
// inv = clamp(1/surf_pen); base = max(0, 3/wpn_pen*1.25)*(inv*3) + scale*dmg
// thickness^2 * inv / 24 + base; dmg -= loss; if dmg < 1 -> dead
// thin (<6u) type 71/89 -> scale 0.05 + pen=3; type 76 -> pen=2; 85/87 -> pen=3
float PenLoss(float damage, float weaponPen, float thickness, float surfPen,
	float scaleMod = 0.16f)
{
	const float invSurf = 1.f / (std::max)(0.1f, surfPen);
	const float wpen = (std::max)(0.05f, weaponPen);
	const float base = (std::max)(0.f, (3.f / wpen) * 1.25f) * (invSurf * 3.f)
		+ scaleMod * damage;
	return (thickness * thickness * invSurf) / 24.f + base;
}

// Soft TraceLine pen - ONLY when game CreateTrace path unavailable.
// Fail closed: real surface data, real exit, IDA loss, target entity hit after ?1 wall.
Result FirePenTrace(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target)
{
	Result r{};
	WeaponStats ws{};
	if (!target)
		return r;
	if (!ReadWeaponStats(weapon, ws)) {
		ws.damage = 40.f;
		ws.pen = 1.f;
		ws.range = 8192.f;
		ws.rangeMod = 0.98f;
		ws.armorRatio = 0.5f;
		ws.hsMult = 4.f;
	}
	// Weapon with no pen power cannot wallbang
	if (ws.pen < 0.05f)
		return r;
	if (!Trace::Ready())
		return r;

	Vector_t dir = aimPoint - eye;
	const float fullDist = dir.Length();
	if (fullDist < 1.f)
		return r;
	dir = dir / fullDist;

	float damage = ws.damage;
	Vector_t cur = eye;
	float traveled = 0.f;
	// IDA FireBullet pen count typically 4
	int pensLeft = 4;
	int walls = 0;

	constexpr int kMaxWalls = 4;
	for (int iter = 0; iter < 16 && damage >= 1.f && walls <= kMaxWalls; ++iter) {
		const float remain = (std::min)(ws.range - traveled, fullDist - traveled + 4.f);
		if (remain <= 1.f)
			break;
		const Vector_t next = cur + dir * remain;

		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(cur, next, local, tr, Trace::kMaskShotPen))
			break;

		void* hit = tr.hit_entity();
		const float frac = (std::clamp)(tr.fraction(), 0.f, 1.f);
		traveled += remain * frac;

		// Hit target after ?1 wall only
		if (Trace::HitsTarget(hit, target)) {
			if (walls == 0)
				return r; // visible - not pen path
			if (damage < 1.f)
				return r;
			float dmg = ApplyRangeFalloff(damage, traveled, ws.rangeMod);
			ScaleDamage(dmg, HitboxToHitgroup(hitbox), target, ws);
			if (dmg < 1.f)
				return r;
			r.damage = dmg;
			r.penetrated = true;
			r.hit = true;
			return r;
		}

		// Clear / past aim without target -> miss
		if (!Trace::DidHit(tr) || frac >= 0.999f)
			break;

		if (pensLeft <= 0 || walls >= kMaxWalls)
			break;

		// Wall/prop - real surface pen (IDA: surf_pen < 0.1 -> stop)
		float surfPen = 0.f;
		float surfHard = 1.f;
		if (!Trace::GetHitSurfaceData(tr, surfPen, surfHard)
			|| !std::isfinite(surfPen) || surfPen < 0.1f) {
			break;
		}
		// IDA mods: keep raw pen, clamp floor only (type 76/85 force higher later)
		surfPen = std::clamp(surfPen, 0.1f, 32.f);

		const Vector_t enter = tr.endpos();
		Vector_t exitPos{};
		if (!TraceToExitSimple(enter, dir, exitPos, local))
			break;

		const float thickness = (exitPos - enter).Length();
		if (thickness < 0.05f)
			break;
		// IDA: no hard thickness kill - only dmg < 1 after loss.
		// Soft path: allow up to ~90u (matches exit search); absurd slabs die via PenLoss.
		if (thickness > 90.f)
			break;

		// Thin surfaces (IDA 71/89 < 6u): scale 0.05 instead of 0.16
		const float scaleMod = (thickness < 6.f) ? 0.05f : 0.16f;
		const float loss = PenLoss(damage, ws.pen, thickness, surfPen, scaleMod);
		damage -= loss;
		if (damage < 1.f)
			break;
		traveled += thickness;
		cur = exitPos + dir * 1.0f;
		--pensLeft;
		++walls;
	}
	return r;
}

// SEH-only core - no C++ objects with destructors (C2712)
struct GameFireIn {
	const Vector_t* eye;
	const Vector_t* delta;
	void* filter;
	void* local;
	void* target;
	float damage;
	float pen;
	float rangeMod;
	float dist;       // eye -> aim (for falloff when final surface dmg wiped)
	int pens;
	int team;
	int hitgroup;
	float armorRatio;
	float hsMult;
	int armor;
	bool helmet;
};

struct GameFireOut {
	float damage;
	bool hit;
	bool penetrated;
	int seh;
	// raw scan results from SEH core (scaled outside)
	float rawDmg;
	bool rawPen;
	bool rawOk;
	int rawHitgroup; // from hitbox_data if set, else caller map
};

void ScaleGameDamage(const GameFireIn& in, float dmg, bool penetrated, int hitgroup, GameFireOut& out) {
	if (dmg < 1.f)
		return;
	// Prefer engine hitgroup (trace) over aim-hitbox map
	const int hg = (hitgroup >= 1 && hitgroup <= 8) ? hitgroup : in.hitgroup;
	switch (hg) {
	case 1: dmg *= in.hsMult; break;
	case 3: dmg *= 1.25f; break;
	case 6: case 7: dmg *= 0.75f; break;
	default: break;
	}
	if (in.armor > 0) {
		// Legs/feet: no armor (CS2 + UC)
		const bool leg = (hg == 6 || hg == 7);
		const bool head = (hg == 1);
		if (!leg && (!head || in.helmet)) {
			const float ratio = in.armorRatio * 0.5f;
			float dmgHealth = dmg * ratio;
			const float dmgArmor = (dmg - dmgHealth) * 0.5f;
			if (dmgArmor > static_cast<float>(in.armor))
				dmgHealth = dmg - static_cast<float>(in.armor) * 0.5f;
			if (dmgHealth < 1.f)
				dmgHealth = 1.f;
			dmg = dmgHealth;
		}
	}
	out.damage = dmg;
	out.penetrated = penetrated;
	out.hit = true;
}

// SEH core only - POD locals, no C++ dtors (C2712)
GameFireOut FireGameSeh(const GameFireIn& in) {
	GameFireOut out{};
	if (!g_initTraceData || !g_createTrace || !g_damageToPoint || !g_initFilter || !g_freeTraceData)
		return out;

	alignas(16) std::uint8_t traceBuf[kTraceDataSize];
	std::memset(traceBuf, 0, sizeof(traceBuf));

	float priorMax = 0.f;
	int targetIdx = -1;
	int surfaces = 0;
	int seh = 0;
	bool inited = false;
	int hitHg = in.hitgroup;
	int hitHgCaptured = in.hitgroup;

	__try {
		g_initTraceData(traceBuf);
		inited = true;

		// FireBullet: InitFilter(filter, skip, 0x1C300B, 3, 15) then set extras
		// IDA FireBullet @ 0x180847530: mask 0x1C300B (NOT UC 0x1C100B)
		g_initFilter(in.filter, in.local, kMaskShotPen, 3, 15);
		{
			auto* f = static_cast<std::uint8_t*>(in.filter);
			// filter+16 |= 0x4000000000 (FireBullet after InitFilter)
			*reinterpret_cast<std::uint64_t*>(f + 16) |= 0x4000000000ull;
			f[57] |= 2; // FireBullet: v230 |= 2
		}

		// Prefer DamageToPoint over UC's per-surface HandleBulletPen loop:
		// DamageToPoint IS the FireBullet pen driver (only xref to HandleBulletPen).
		// UC then multiplies range again -> under-damages wallbangs.
		g_createTrace(traceBuf, in.eye, in.delta, in.filter, in.pens, true);
		g_damageToPoint(traceBuf, in.damage, in.pen, in.rangeMod, in.pens, in.team, nullptr);

		surfaces = *reinterpret_cast<int*>(traceBuf + kOffSurfacesCount);
		auto* infos = *reinterpret_cast<TraceInfo**>(traceBuf + kOffSurfacesPtr);
		void* segs = *reinterpret_cast<void**>(traceBuf + kOffSegmentsPtr);

		// ONLY accept when GetTraceInfo.hit_entity matches target.
		// Residual last-surface dmg without entity match = wall exit into air
		// (false wallbang) - removed 2026-08-08 after IDA re-check.
		// DamageToPoint writes surface dmg@+8 (post pen+range) pens@+12 remaining.
		if (infos && surfaces > 0 && surfaces <= 64 && g_getTraceInfo && segs && in.target) {
			for (int i = 0; i < surfaces; ++i) {
				const TraceInfo& info = infos[i];
				if (info.damage >= 1.f && info.damage > priorMax)
					priorMax = info.damage;

				// Prefer exit segment (wall out); enter if exit empty
				const std::uint16_t idx = info.exit_idx ? info.exit_idx : info.enter_idx;
				if (idx >= 128)
					continue;
				auto* seg = reinterpret_cast<std::uint8_t*>(segs)
					+ kSegStride * static_cast<std::size_t>(idx);
				alignas(16) GameTraceAw gt{};
				g_getTraceInfo(traceBuf, &gt, info.distance, seg);
				if (Trace::HitsTarget(gt.hit_entity, in.target)) {
					targetIdx = i;
					hitHgCaptured = ReadTraceHitgroup(gt.hitbox_data, in.hitgroup);
					if (info.damage >= 1.f)
						priorMax = info.damage;
				}
				// Also probe enter_idx when exit differs (player often on enter)
				if (info.enter_idx && info.enter_idx != idx && info.enter_idx < 128) {
					auto* segE = reinterpret_cast<std::uint8_t*>(segs)
						+ kSegStride * static_cast<std::size_t>(info.enter_idx);
					alignas(16) GameTraceAw gtE{};
					g_getTraceInfo(traceBuf, &gtE, info.distance, segE);
					if (Trace::HitsTarget(gtE.hit_entity, in.target)) {
						targetIdx = i;
						hitHgCaptured = ReadTraceHitgroup(gtE.hitbox_data, in.hitgroup);
						if (info.damage >= 1.f)
							priorMax = info.damage;
					}
				}
			}

			if (targetIdx >= 0) {
				const TraceInfo& ti = infos[targetIdx];
				const float dmg = ti.damage;
				if (!std::isfinite(dmg) || dmg < 1.f) {
					// Target surface with wiped dmg = bullet died before/at hit
					out.rawOk = false;
				} else {
					// pen_count = pens remaining AFTER this surface (IDA DamageToPoint)
					const bool didPen =
						(targetIdx > 0) || (ti.pen_count < in.pens);
					if (in.pens > 0 && !didPen && targetIdx == 0) {
						// First surface = target, full pens left -> visible hit
						out.rawOk = true;
						out.rawDmg = dmg;
						out.rawPen = false;
					} else if (in.pens > 0 && !didPen) {
						out.rawOk = false;
					} else {
						out.rawOk = true;
						out.rawDmg = dmg;
						// pens==0 path is visible-only: never mark penetrated
						out.rawPen = didPen;
					}
				}
			}
			(void)priorMax;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		seh = 1;
	}

	// Always free if InitTraceData ran - DamageToPoint may heap-grow surfaces
	if (inited && g_freeTraceData) {
		__try {
			g_freeTraceData(traceBuf);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			seh = 1;
		}
	}

	out.seh = seh;
	if (seh) {
		out.hit = false;
		out.damage = 0.f;
		return out;
	}
	if (out.rawOk) {
		// Hitgroup captured before FreeTraceData - hitbox_data is trace-owned.
		hitHg = hitHgCaptured;
		out.rawHitgroup = hitHg;
		ScaleGameDamage(in, out.rawDmg, out.rawPen, hitHg, out);
	}
	return out;
}

// outSeh: true if game path crashed (caller may soft-fallback)
Result FireGame(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	bool* outSeh = nullptr)
{
	Result r{};
	if (outSeh) *outSeh = false;
	WeaponStats ws{};
	if (!ReadWeaponStats(weapon, ws) || !g_gamePath)
		return r;

	Vector_t delta = aimPoint - eye;
	const float dist = delta.Length();
	if (dist < 1.f)
		return r;
	// CreateTrace expects direction * range (FireBullet: dir * weapon range)
	delta = delta * (ws.range / dist);

	alignas(16) std::uint8_t filter[kFilterSize]{};

	GameFireIn in{};
	in.eye = &eye;
	in.delta = &delta;
	in.filter = filter;
	in.local = local;
	in.target = target;
	in.damage = ws.damage;
	in.pen = ws.pen;
	in.rangeMod = ws.rangeMod;
	in.dist = dist;
	in.pens = allowPen ? 4 : 0;
	in.team = static_cast<int>(local->m_iTeamNum());
	in.hitgroup = HitboxToHitgroup(hitbox);
	in.armorRatio = ws.armorRatio;
	in.hsMult = ws.hsMult;
	in.armor = target ? target->m_ArmorValue() : 0;
	in.helmet = HasHelmet(target);

	const GameFireOut o = FireGameSeh(in);
	if (o.seh) {
		if (outSeh) *outSeh = true;
		return r;
	}
	r.damage = o.damage;
	r.hit = o.hit;
	r.penetrated = o.penetrated;
	return r;
}

} // namespace

bool Init() {
	g_initTraceData = reinterpret_cast<FnInitTraceData>(FindClient(kPatInitTraceData));
	if (!g_initTraceData)
		g_initTraceData = reinterpret_cast<FnInitTraceData>(FindClient(kPatInitTraceDataLoose));
	g_createTrace = reinterpret_cast<FnCreateTrace>(FindClient(kPatCreateTrace));
	if (!g_createTrace)
		g_createTrace = reinterpret_cast<FnCreateTrace>(FindClient(kPatCreateTraceLoose));
	g_damageToPoint = reinterpret_cast<FnDamageToPoint>(FindClient(kPatDamageToPoint));
	if (!g_damageToPoint)
		g_damageToPoint = reinterpret_cast<FnDamageToPoint>(FindClient(kPatDamageToPointLoose));
	g_getTraceInfo = reinterpret_cast<FnGetTraceInfo>(FindClient(kPatGetTraceInfo));
	if (!g_getTraceInfo)
		g_getTraceInfo = reinterpret_cast<FnGetTraceInfo>(FindClient(kPatGetTraceInfoLoose));
	g_initFilter = reinterpret_cast<FnInitFilter>(FindClient(kPatInitFilter));
	g_freeTraceData = reinterpret_cast<FnFreeTraceData>(FindClient(kPatFreeTraceData));
	g_handleBulletPen = reinterpret_cast<FnHandleBulletPen>(FindClient(kPatHandleBulletPen));
	if (!g_handleBulletPen)
		g_handleBulletPen = reinterpret_cast<FnHandleBulletPen>(FindClient(kPatHandleBulletPenLoose));

	// Game path needs GetTraceInfo to verify target surface - else no invent dmg.
	g_gamePath = g_initTraceData && g_createTrace && g_damageToPoint
		&& g_initFilter && g_freeTraceData && g_getTraceInfo;
	g_ready = true;

	if (g_gamePath)
		Con::Ok("AutoWall: game pen ready (Init+Create+Damage+GetInfo+Free)%s",
			g_handleBulletPen ? " +HandleBulletPen" : " [HandleBulletPen miss - DamageToPoint still calls it]");
	else {
		Con::Ok("AutoWall: TraceLine pen fallback ONLY (strict fail-closed)");
		if (!g_initTraceData) Con::OffsetMiss("AutoWall::InitTraceData");
		if (!g_createTrace)   Con::OffsetMiss("AutoWall::CreateTrace");
		if (!g_damageToPoint) Con::OffsetMiss("AutoWall::DamageToPoint");
		if (!g_getTraceInfo)  Con::OffsetMiss("AutoWall::GetTraceInfo");
		if (!g_initFilter)    Con::OffsetMiss("AutoWall::InitFilter");
		if (!g_freeTraceData) Con::OffsetMiss("AutoWall::FreeTraceData");
	}
	if (!g_handleBulletPen)
		Con::OffsetMiss("AutoWall::HandleBulletPen");
	return true;
}

bool Ready() {
	return g_ready;
}

Result Fire(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen)
{
	Result r{};
	if (!weapon || !local || !target)
		return r;
	if (!g_ready)
		Init();

	TickBudget();

	// Quantized cache - multipoints within ~8u reuse last pen result
	PenCacheKey key{};
	MakeKey(eye, aimPoint, hitbox, weapon, target, allowPen, key);
	if (CacheLookup(key, r))
		return r;

	// Hard LOS: EstimateVisible ONLY when first solid hit is the target.
	// Never treat "no hit" as clear (filter miss ? free wallbang).
	// Never trust IsVisible alone.
	bool clearLos = false;
	bool blockedByWorld = false;
	if (!Trace::Ready()) {
		// No TraceShape - never invent pen; visible estimate only if caller forbids pen
		if (!allowPen) {
			r = EstimateVisible(eye, aimPoint, hitbox, weapon, target);
			CacheStore(key, r);
			return r;
		}
		CacheStore(key, r);
		return r;
	}

	{
		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(eye, aimPoint, local, tr, Trace::kMaskShotPen)) {
			CacheStore(key, r);
			return r;
		}
		void* hit = tr.hit_entity();
		if (Trace::HitsTarget(hit, target)) {
			clearLos = true;
		} else if (Trace::DidHit(tr) && tr.fraction() < 0.999f) {
			blockedByWorld = true;
		} else {
			// No solid between eye and aimPoint - still not EstimateVisible
			// unless hull also clear of non-target (world brush catch).
			Trace::CGameTrace th{};
			const Vector_t mins{ -0.5f, -0.5f, -0.5f };
			const Vector_t maxs{  0.5f,  0.5f,  0.5f };
			if (Trace::TraceHull(eye, aimPoint, mins, maxs, local, th, Trace::kMaskShotPen)) {
				void* hh = th.hit_entity();
				if (Trace::HitsTarget(hh, target))
					clearLos = true;
				else if (Trace::DidHit(th) && th.fraction() < 0.97f)
					blockedByWorld = true;
				else
					clearLos = true; // air path, target capsule may miss line
			} else {
				CacheStore(key, r);
				return r;
			}
		}
	}

	// Visible: armor/range estimate only - never pen invent (cheap, always cache)
	if (clearLos && !blockedByWorld) {
		r = EstimateVisible(eye, aimPoint, hitbox, weapon, target);
		CacheStore(key, r);
		return r;
	}

	// Wall / prop between eye and aim
	if (!allowPen) {
		CacheStore(key, r);
		return r; // wallbang disabled - fail closed
	}

	// Weapon must be able to pen (VData m_flPenetration)
	{
		WeaponStats ws{};
		if (!ReadWeaponStats(weapon, ws) || ws.pen < 0.05f) {
			CacheStore(key, r);
			return r;
		}
	}

	// Prefer game CreateTrace + DamageToPoint + HandleBulletPen (IDA verified).
	// Budget: skip when flooded - do NOT cache miss (TTL would block real pens).
	if (g_gamePath) {
		if (g_gamePenUsed < kMaxGamePenPerMs) {
			++g_gamePenUsed;
			bool seh = false;
			Result game = FireGame(eye, aimPoint, hitbox, weapon, local, target, true, &seh);
			if (!seh) {
				if (game.hit && game.damage >= 1.f) {
					// Wall path requires real pen proof from DamageToPoint surfaces
					if (blockedByWorld && !game.penetrated) {
						CacheStore(key, r);
						return r;
					}
					if (blockedByWorld)
						game.penetrated = true;
					CacheStore(key, game);
					return game;
				}
				// Game clean + no target hit = cannot pen this wall/prop
				CacheStore(key, r);
				return r;
			}
			// SEH - fall through to soft
		}
		// over budget: try soft, else uncached miss
	}

	if (g_softPenUsed < kMaxSoftPenPerMs) {
		++g_softPenUsed;
		Result pen = FirePenTrace(eye, aimPoint, hitbox, weapon, local, target);
		if (pen.hit && pen.damage >= 1.f && pen.penetrated) {
			CacheStore(key, pen);
			return pen;
		}
		// Real miss from soft path - cache
		CacheStore(key, r);
		return r;
	}

	// Over both budgets: do not cache - next tick re-evaluates
	return r;
}

bool PassesMinDamage(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamage)
{
	const Result r = Fire(eye, aimPoint, hitbox, weapon, local, target, allowPen);
	if (!r.hit || !std::isfinite(r.damage) || r.damage < 1.f)
		return false;

	if (minDamage <= 0.f)
		return true;

	// HP-aware: if target HP < mindmg -> require lethal (dmg >= HP)
	float need = minDamage;
	if (target && Mem::ValidEntity(target)) {
		int hp = 0;
		__try { hp = target->m_iHealth(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { hp = 0; }
		if (hp > 0 && static_cast<float>(hp) < need)
			need = static_cast<float>(hp);
	}
	return r.damage + 0.01f >= need;
}

XhairPen CheckCrosshairPenetration(
	const Vector_t& eye,
	const Vector_t& dir,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local)
{
	if (!g_ready)
		Init();
	if (!weapon || !local)
		return XhairPen::NoData;

	WeaponStats ws{};
	if (!ReadWeaponStats(weapon, ws))
		return XhairPen::NoData;

	Vector_t d = dir;
	const float dl = d.Length();
	if (!std::isfinite(dl) || dl < 1e-4f)
		return XhairPen::NoData;
	d = d / dl;

	const float range = (ws.range > 1.f) ? ws.range : 8192.f;

	if (ws.pen < 0.05f) {
		// No pen power - still report Clear vs Blocked via a LOS trace.
		if (!Trace::Ready())
			return XhairPen::NoData;
		const Vector_t end = eye + d * range;
		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(eye, end, local, tr, Trace::kMaskShotPen))
			return XhairPen::NoData;
		return (!Trace::DidHit(tr) || tr.fraction() >= 0.999f)
			? XhairPen::Clear : XhairPen::Blocked;
	}

	// Native path: InitTraceData -> CreateTrace -> first solid GetTraceInfo
	// -> HandleBulletPenetration. Matches FireBullet 0x180847660.
	{
		const int localTeam = static_cast<int>(local->m_iTeamNum());
		const int gameResult = CheckCrosshairPenetrationGameSeh(
			eye, d, range, ws.damage, ws.pen, ws.rangeMod, localTeam, local);
		if (gameResult == 2)
			return XhairPen::Clear;
		if (gameResult == 1)
			return XhairPen::Penetrable;
		if (gameResult == 0)
			return XhairPen::Blocked;
		// -1 = SEH / missing HandleBulletPen -> soft sim
	}

	if (!Trace::Ready())
		return XhairPen::NoData;

	float damage = ws.damage;
	Vector_t cur = eye;
	float traveled = 0.f;
	int pensLeft = 4;
	int walls = 0;
	constexpr int kMaxWalls = 4;

	for (int iter = 0; iter < 12 && damage >= 1.f && walls <= kMaxWalls; ++iter) {
		const float remain = range - traveled;
		if (remain <= 1.f)
			break;
		const Vector_t next = cur + d * remain;

		Trace::CGameTrace t{};
		if (!Trace::TraceLine(cur, next, local, t, Trace::kMaskShotPen))
			break;

		const float frac = std::clamp(t.fraction(), 0.f, 1.f);
		traveled += remain * frac;

		if (!Trace::DidHit(t) || frac >= 0.999f)
			return (walls > 0 && damage >= 1.f)
				? XhairPen::Penetrable : XhairPen::Clear;

		void* hit = t.hit_entity();
		if (walls == 0 && SehEntityIsPlayer(hit, local))
			return XhairPen::Clear;

		if (pensLeft <= 0 || walls >= kMaxWalls)
			return XhairPen::Blocked;

		float surfPen = 0.f, surfHard = 1.f;
		const bool haveSurf = Trace::GetHitSurfaceData(t, surfPen, surfHard)
			&& std::isfinite(surfPen) && surfPen >= 0.1f;
		if (!haveSurf) {
			if (surfPen > 0.f && surfPen < 0.1f)
				return XhairPen::Blocked;
			surfPen = 1.5f;
		}
		surfPen = std::clamp(surfPen, 0.1f, 32.f);

		const Vector_t enter = t.endpos();
		Vector_t exitPos{};
		if (!TraceToExitSimple(enter, d, exitPos, local))
			return XhairPen::Blocked;

		const float thickness = (exitPos - enter).Length();
		if (thickness < 0.05f)
			return XhairPen::Blocked;
		if (thickness > 120.f)
			return XhairPen::Blocked;

		const float scaleMod = (thickness < 6.f) ? 0.05f : 0.16f;
		damage -= PenLoss(damage, ws.pen, thickness, surfPen, scaleMod);
		if (damage < 1.f)
			return XhairPen::Blocked;

		traveled += thickness;
		cur = exitPos + d * 1.f;
		--pensLeft;
		++walls;
	}

	return (damage >= 1.f && walls > 0) ? XhairPen::Penetrable : XhairPen::Blocked;
}

// Present-thread traces are the multi-queue insecure surface (visuals.cpp:445).
// Cache the pen result on the game thread; Present only draws it.
static XhairPen s_cachedPen = XhairPen::NoData;
static SRWLOCK s_penLock = SRWLOCK_INIT;
static ULONGLONG s_lastPenMs = 0;

void TickXhairCache()
{
	if (!Config::autowall_xhair) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (now - s_lastPenMs < 55)
		return;
	s_lastPenMs = now;

	if (H::SessionMapLeaving() || H::SessionPostMatch()) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	if (!H::SessionEntityOk()) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	C_CSPlayerPawn* lp = H::SafeLocalAlive();
	if (!lp || !Mem::ValidEntity(lp)) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	int hp = 0;
	__try { hp = lp->m_iHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	if (hp <= 0) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	C_CSWeaponBase* wep = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(wep) || wep->IsNonGunWeapon()) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	const Vector_t eye = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(eye)) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	QAngle_t va{};
	if (!AimCommon::GetViewAngles(va) || !va.IsValid()) {
		AcquireSRWLockExclusive(&s_penLock);
		s_cachedPen = XhairPen::NoData;
		ReleaseSRWLockExclusive(&s_penLock);
		return;
	}
	va.z = 0.f;
	Vector_t dir{};
	va.ToDirections(&dir, nullptr, nullptr);

	XhairPen st = XhairPen::NoData;
	__try { st = CheckCrosshairPenetration(eye, dir, wep, lp); }
	__except (EXCEPTION_EXECUTE_HANDLER) { st = XhairPen::NoData; }

	AcquireSRWLockExclusive(&s_penLock);
	s_cachedPen = st;
	ReleaseSRWLockExclusive(&s_penLock);
}

XhairPen GetCachedXhairPen()
{
	AcquireSRWLockShared(&s_penLock);
	XhairPen v = s_cachedPen;
	ReleaseSRWLockShared(&s_penLock);
	return v;
}

void InvalidateXhairCache()
{
	AcquireSRWLockExclusive(&s_penLock);
	s_cachedPen = XhairPen::NoData;
	ReleaseSRWLockExclusive(&s_penLock);
	s_lastPenMs = 0;
}

} // namespace AutoWall

