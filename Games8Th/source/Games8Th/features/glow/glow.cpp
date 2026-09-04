#include "glow.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../gamemode/gamemode.h"
#include "../visuals/visuals.h"

#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Glow {
namespace {

// schema: C_BaseModelEntity->m_Glow offset (cached)
std::uint32_t g_mGlowOff = 0;
bool g_mGlowTried = false;

// IDA:
// GlowObjectManager_GetInstance / pGlowManager -> qword_1823A0708
// OnGlowTypeChanged -> 0x180B4BE30
// ManageGlowSceneObject -> 0x180B1B2B0
// GetGlowColor -> 0x180B499F0
using FnGlowMgrGet = void*(__fastcall*)();
using FnOnGlowTypeChanged = void(__fastcall*)(CGlowProperty* glow);

FnGlowMgrGet g_glowMgrGet = nullptr;
FnOnGlowTypeChanged g_onGlowTypeChanged = nullptr;
void* g_glowManager = nullptr;
bool g_engTried = false;

void NotifyGlowChanged(CGlowProperty* glow) {
	if (!glow || !g_onGlowTypeChanged)
		return;
	__try {
		g_onGlowTypeChanged(glow);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

std::uint32_t ResolveMGlow() {
	if (g_mGlowTried)
		return g_mGlowOff;
	g_mGlowTried = true;
	g_mGlowOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseModelEntity->m_Glow"));
	if (!g_mGlowOff)
		g_mGlowOff = 0xDE0;
	return g_mGlowOff;
}

bool IsPlayerPawn(C_BaseEntity* ent) {
	if (!ent)
		return false;
	return ent->IsBasePlayer();
}

bool PlayerVisible(C_CSPlayerPawn* local, C_CSPlayerPawn* target) {
	if (!local || !target || !Trace::Ready())
		return true; // fail-open colour (visible palette)

	// Prefer ESP cache (same-frame vis) - avoids duplicate bone+trace work.
	// Read the atomically-published double-buffer, NEVER the live cached_players
	// vector Present rebuilds mid-frame (cross-thread iteration = UAF).
	if (target->handle().valid()) {
		bool vis = true;
		if (EspTryLookupVisible(target->handle().raw(), vis))
			return vis;
	}

	const Vector_t eye = Bones::GetEyePos(local);
	if (!Bones::IsValidPos(eye))
		return true;

	Vector_t samples[6]{};
	int n = 0;
	uintptr_t ba = 0;
	Vector_t origin{};
	float height = 0.f;
	Bones::Map map{};
	if (Bones::GetBoneArrayReadonly(target, ba, origin, height)
		&& Bones::ResolveMapCached(target, ba, origin, height, map)) {
		const int slots[] = {
			Bones::S_HEAD, Bones::S_PELVIS,
			Bones::S_ARM_L_R, Bones::S_ARM_L_L,
			Bones::S_ANKLE_R, Bones::S_ANKLE_L
		};
		for (int s : slots) {
			Vector_t p{};
			if (n < 6 && Bones::GetSlotPos(ba, map, s, p) && Bones::IsValidPos(p))
				samples[n++] = p;
		}
	}
	if (n < 1) {
		samples[0] = Bones::GetEyePos(target);
		n = 1;
	}
	return Trace::IsBodyVisible(eye, samples, n, local, target);
}

// Shared target filter: Config::glow_color* only.
// Returns false when this glow prop must stay stock / off.
bool ResolvePlayerGlow(CGlowProperty* glow, ImVec4& outCol, bool* outEnable) {
	if (outEnable)
		*outEnable = false;
	if (!glow || !glow->ok())
		return false;

	if (H::SessionMapLeaving())
		return false;
	if (!I::EngineClient || !I::EngineClient->in_game())
		return false;

	if (Config::loading.load(std::memory_order_acquire))
		return false;

	if (!Config::glow)
		return false;

	void* owner = glow->owner();
	if (!Mem::ValidEntity(owner))
		return false;

	auto* inst = reinterpret_cast<CEntityInstance*>(owner);
	CEntityIdentity* id = inst->m_pEntityIdentity();
	if (!id || !Mem::Valid(id, sizeof(void*)) || !id->valid())
		return false;

	auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(owner);
	if (!IsPlayerPawn(reinterpret_cast<C_BaseEntity*>(pawn)))
		return false;

	int hp = 0;
	__try { hp = Mem::ClampHealth(pawn->m_iHealth()); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (hp < 1)
		return false;

	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (!Mem::ValidEntity(local) || local == pawn)
		return false;

	int team = 0, localTeam = 0;
	__try {
		team = static_cast<int>(pawn->m_iTeamNum());
		localTeam = static_cast<int>(local->m_iTeamNum());
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!Mem::ValidTeam(team) || !Mem::ValidTeam(localTeam))
		return false;
	const bool isEnemy = (team != localTeam);

	if (GameMode::WantTeamCheck(Config::teamCheck) && !isEnemy)
		return false;
	if (!Config::glow_team && !isEnemy)
		return false;
	if (!Config::glow_enemy && isEnemy)
		return false;

	const bool visible = PlayerVisible(local, pawn);
	if (Config::glow_only_visible && !visible) {
		// Tell callers to disable - do not leave prior glow stamp
		if (outEnable)
			*outEnable = false;
		return false;
	}

 // Always Config::glow_color* - nothing else is ever read here.
	outCol = visible ? Config::glow_color : Config::glow_color_invis;
	// Clamp alpha; 0 would drop the scene object (ManageGlow requires a>0).
	outCol.w = std::clamp(outCol.w, 0.05f, 1.f);
	if (outEnable)
		*outEnable = true;
	return true;
}

// GetGlowColor -> ManageGlow run back-to-back in B04B30, all on the game
// render thread. Carries pure Config glow float4 so ManageGlow never inherits
// mesh pre-fill. Plain globals (NOT thread_local) - manual mappers don't set
// up the TLS directory, so thread_local reads garbage -> instant crash on the
// first glow draw. Same-thread re-entrancy semantics are identical to TLS.
float g_tlForceRgba[4] = {};
bool g_tlForceValid = false;

void ClearForceColor() {
	g_tlForceValid = false;
}

void PushForceColor(const ImVec4& col) {
	g_tlForceRgba[0] = std::clamp(col.x, 0.f, 1.f);
	g_tlForceRgba[1] = std::clamp(col.y, 0.f, 1.f);
	g_tlForceRgba[2] = std::clamp(col.z, 0.f, 1.f);
	g_tlForceRgba[3] = std::clamp(col.w, 0.05f, 1.f);
	g_tlForceValid = true;
}

void WriteRgba4(float* out, const ImVec4& col) {
	if (!out || !Mem::IsReadable(out, 16))
		return;
	out[0] = std::clamp(col.x, 0.f, 1.f);
	out[1] = std::clamp(col.y, 0.f, 1.f);
	out[2] = std::clamp(col.z, 0.f, 1.f);
	out[3] = std::clamp(col.w, 0.05f, 1.f);
}

// IDA ManageGlow writes colour @ scene+0xD0 (208), backface @ scene+0xF8 (248).
void ForceSceneObjectColor(CGlowProperty* glow, const ImVec4& col) {
	if (!glow || !glow->ok())
		return;
	void* scene = glow->scene_object();
	if (!scene || !Mem::IsUserPtr(scene) || !Mem::IsReadable(scene, 0x100))
		return;
	auto* base = reinterpret_cast<std::uint8_t*>(scene);
	float* rgba = reinterpret_cast<float*>(base + 0xD0);
	float* backface = reinterpret_cast<float*>(base + 0xF8);
	__try {
		if (Mem::IsReadable(rgba, 16)) {
			rgba[0] = std::clamp(col.x, 0.f, 1.f);
			rgba[1] = std::clamp(col.y, 0.f, 1.f);
			rgba[2] = std::clamp(col.z, 0.f, 1.f);
			rgba[3] = std::clamp(col.w, 0.05f, 1.f);
		}
		if (Mem::IsReadable(backface, 4))
			*backface = 1.f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void StampPlayerGlow(CGlowProperty* glow, const ImVec4& col) {
	void* owner = glow->owner();
	if (Mem::ValidEntity(owner))
		IsolateFromMeshTint(owner);
	ApplyGlowColor(glow, FromImVec4(col));
 // Flashing multiplies GetGlowColor RGB - kill it so mesh can't tint via flash path
	glow->flashing() = false;
	glow->glow_type() = 3;
	glow->glowing() = true;
	// Do NOT push TL force here - ApplyPlayer stamps many pawns per frame;
	// TL is only set on the GetGlowColor / ApplyGlowScene path for the live draw.
}

// True when owner is a player pawn we may control - never clear world weapon/bomb glow.
// ALSO requires the prop to sit exactly at owner's m_Glow offset: the DrawGlow
// hook target is a getter with several call sites, some passing non-glow objects
// at +0xDE0 - the offset check rejects those before any write.
bool OwnerIsPlayerPawn(CGlowProperty* glow) {
	if (!glow || !glow->ok())
		return false;
	void* owner = glow->owner();
	if (!Mem::ValidEntity(owner))
		return false;

	static void* s_owner = nullptr;
	static bool s_isPawn = false;
	static std::uint32_t s_frame = 0;
	const std::uint32_t frame = H::g_presentFrame.load(std::memory_order_relaxed);
	bool isPawn = false;
	if (s_owner == owner && s_frame == frame) {
		isPawn = s_isPawn;
	} else {
		isPawn = IsPlayerPawn(reinterpret_cast<C_BaseEntity*>(owner));
		s_owner = owner;
		s_isPawn = isPawn;
		s_frame = frame;
	}
	if (!isPawn)
		return false;

	const auto propAddr = reinterpret_cast<std::uintptr_t>(glow);
	const auto ownerAddr = reinterpret_cast<std::uintptr_t>(owner);
	if (propAddr < ownerAddr)
		return false;
	if (propAddr - ownerAddr != static_cast<std::uintptr_t>(ResolveMGlow()))
		return false;
	return true;
}

// World glow: ApplyWorld stamps props, but GetGlowColor only forced player
// colours - dropped weapons often rendered with mesh/default tint (looked bugged).
// Keep a small sticky list of entities we enabled so disable clears them.
constexpr int kWorldGlowTrackMax = 96;
void* g_worldGlowEnts[kWorldGlowTrackMax]{};
int g_worldGlowN = 0;

void TrackWorldGlowEnt(void* ent) {
	if (!ent)
		return;
	for (int i = 0; i < g_worldGlowN; ++i) {
		if (g_worldGlowEnts[i] == ent)
			return;
	}
	if (g_worldGlowN < kWorldGlowTrackMax)
		g_worldGlowEnts[g_worldGlowN++] = ent;
}

void UntrackWorldGlowEnt(void* ent) {
	for (int i = 0; i < g_worldGlowN; ++i) {
		if (g_worldGlowEnts[i] != ent)
			continue;
		g_worldGlowEnts[i] = g_worldGlowEnts[g_worldGlowN - 1];
		--g_worldGlowN;
		return;
	}
}

bool IsTrackedWorldGlow(void* ent) {
	if (!ent)
		return false;
	for (int i = 0; i < g_worldGlowN; ++i) {
		if (g_worldGlowEnts[i] == ent)
			return true;
	}
	return false;
}

// True when the glow owner entity is a weapon (held on a player or lying on
// the ground). World-weapon glow was removed - never resolve or restamp these.
// weapon_c4 is exempt (dropped-bomb glow). Also used to un-stamp stale stamps
// so a previously-glowing weapon can't follow the player after a pickup.
static bool IsWeaponGlowOwner(void* owner) {
	__try {
		if (!owner || !Mem::ValidEntity(owner))
			return false;
		if (!Mem::IsReadable(static_cast<std::uint8_t*>(owner) + 0x10, sizeof(void*)))
			return false;
		void* id = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(owner) + 0x10);
		if (!id || !Mem::Valid(id, 0x28))
			return false;
		if (!Mem::IsReadable(static_cast<std::uint8_t*>(id) + 0x20, sizeof(void*)))
			return false;
		const char* designer = *reinterpret_cast<const char**>(static_cast<std::uint8_t*>(id) + 0x20);
		if (!designer || !Mem::IsReadable(designer, 8) || designer[0] == '\0')
			return false;
		if (std::strncmp(designer, "weapon_", 7) != 0)
			return false;
		// C4 keeps the ground-bomb glow path
		return std::strncmp(designer, "weapon_c4", 9) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// World entity still has glowing=true from ApplyWorld - force its colour.
bool ResolveWorldGlow(CGlowProperty* glow, ImVec4& outCol) {
	if (!glow || !glow->ok())
		return false;
	void* owner = glow->owner();
	if (!Mem::ValidEntity(owner) || OwnerIsPlayerPawn(glow))
		return false;
	// Weapons are never world-glow targets - and a stale glowing=true stamp here
	// would restamp the weapon GLOWING while an enemy carries it. Un-stamp it.
	if (IsWeaponGlowOwner(owner)) {
		if (glow->glowing()) {
			glow->glowing() = false;
			NotifyGlowChanged(glow);
		}
		UntrackWorldGlowEnt(owner);
		return false;
	}
	// Prefer tracked (weapons we enabled this session)
	if (!IsTrackedWorldGlow(owner) && !glow->glowing())
		return false;
	// Colour: dedicated weapon glow colour when weapons toggle on
	if (Config::glow_world_weapons)
		outCol = Config::glow_world_weapon_color;
	else if (Config::glow_world_grenades)
		outCol = Config::world_esp_he_color;
	else
		return false;
	outCol.w = std::clamp(outCol.w, 0.15f, 1.f);
	return true;
}

void SehOnDrawGlow(CGlowProperty* glow) {
	__try {
		if (!glow || !glow->ok())
			return;

		if (H::SessionMapLeaving())
			return; // entity/scene teardown - never stamp dying glow props

		if (Config::loading.load(std::memory_order_acquire))
			return;

		// All glow toggles off: skip class dumps. Only unstamp leftovers we set.
		if (!AnyEnabled()) {
			ClearForceColor();
			if (glow->glowing()) {
				void* owner = glow->owner();
				if (OwnerIsPlayerPawn(glow)
					|| (owner && IsTrackedWorldGlow(owner))) {
					glow->glowing() = false;
					UntrackWorldGlowEnt(owner);
					NotifyGlowChanged(glow);
				}
			}
			return;
		}

		if (!I::EngineClient || !I::EngineClient->in_game())
			return;


		// World weapons/bomb/nades: restamp every DrawGlow so colour sticks
		if (!OwnerIsPlayerPawn(glow)) {
			ImVec4 wcol{};
			if (ResolveWorldGlow(glow, wcol)) {
				void* owner = glow->owner();
				if (Mem::ValidEntity(owner))
					IsolateFromMeshTint(owner);
				ApplyGlowColor(glow, FromImVec4(wcol));
				glow->flashing() = false;
				glow->glow_type() = 3;
				glow->glow_range() = 0;
				glow->glow_range_min() = 0;
				glow->glowing() = true;
				NotifyGlowChanged(glow);
			}
			return;
		}

		if (!Config::glow) {
			glow->glowing() = false;
			return;
		}

		ImVec4 col{};
		bool enable = false;
		if (!ResolvePlayerGlow(glow, col, &enable) || !enable) {
			// glow_only_visible / team filter / dead - clear player stamp only
			glow->glowing() = false;
			return;
		}

		StampPlayerGlow(glow, col);
		NotifyGlowChanged(glow);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("Glow::OnDrawGlow", GetExceptionCode());
	}
}

void SehApplyWorld(void* ent, ColorRgba col, bool enable) {
	__try {
		if (!Mem::ValidEntity(ent))
			return;
		CGlowProperty* g = ModelGlow(ent);
		if (!g || !g->ok())
			return;
		if (enable) {
			IsolateFromMeshTint(ent);
			// Floor alpha - ManageGlow drops a?0; too-low alpha looked "off"
			if (col.a < 40)
				col.a = 40;
			ApplyGlowColor(g, col);
			g->flashing() = false;
			// Same type as player outline (3). Re-stamp every cache tick.
			g->glow_type() = 3;
			g->glow_range() = 0;
			g->glow_range_min() = 0;
			g->glowing() = true;
			TrackWorldGlowEnt(ent);
			NotifyGlowChanged(g);
			// Immediate scene colour if object already exists
			ForceSceneObjectColor(g, ImVec4{
				col.r * (1.f / 255.f), col.g * (1.f / 255.f),
				col.b * (1.f / 255.f), col.a * (1.f / 255.f) });
		} else {
			g->glowing() = false;
			UntrackWorldGlowEnt(ent);
			NotifyGlowChanged(g);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		// silent - world entities recycle often
	}
}

void SehApplyPlayer(void* pawn, bool visible) {
	__try {
		if (!Mem::ValidEntity(pawn) || !Config::glow)
			return;
		if (H::SessionMapLeaving())
			return;
		if (Config::loading.load(std::memory_order_acquire))
			return;
		if (!I::EngineClient || !I::EngineClient->in_game())
			return;

		auto* p = reinterpret_cast<C_CSPlayerPawn*>(pawn);
		if (!IsPlayerPawn(reinterpret_cast<C_BaseEntity*>(p)))
			return;
		const int hp = Mem::ClampHealth(p->m_iHealth());
		if (hp < 1)
			return;

		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!Mem::ValidEntity(local) || local == p)
			return;

		const int team = static_cast<int>(p->m_iTeamNum());
		const int localTeam = static_cast<int>(local->m_iTeamNum());
		if (!Mem::ValidTeam(team) || !Mem::ValidTeam(localTeam))
			return;
		const bool isEnemy = (team != localTeam);

		if (GameMode::WantTeamCheck(Config::teamCheck) && !isEnemy)
			return;
		if (!Config::glow_team && !isEnemy)
			return;
		if (!Config::glow_enemy && isEnemy)
			return;

		CGlowProperty* g = ModelGlow(pawn);
		if (!g || !g->ok())
			return;

		// glow_only_visible: must clear sticky glow - early return left prior stamp on
		if (Config::glow_only_visible && !visible) {
			g->glowing() = false;
			NotifyGlowChanged(g);
			return;
		}

		ImVec4 col = visible ? Config::glow_color : Config::glow_color_invis;
		col.w = std::clamp(col.w, 0.05f, 1.f);
		StampPlayerGlow(g, col);
		// No OnGlowTypeChanged spam every cache tick - property stamp is enough
		// for GetGlowColor force + ManageGlow to pick up.
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

bool SehForceSceneColor(CGlowProperty* glow, float* outRgba) {
	__try {
		if (!outRgba || !glow || !glow->ok())
			return false;
		if (!AnyEnabled())
			return false;

		ImVec4 col{};
		bool enable = false;
		// Player glow path
		if (ResolvePlayerGlow(glow, col, &enable) && enable) {
			WriteRgba4(outRgba, col);
			StampPlayerGlow(glow, col);
			PushForceColor(col);
			return true;
		}
		// World dropped weapon / bomb / nade - was missing -> wrong mesh tint
		if (ResolveWorldGlow(glow, col)) {
			WriteRgba4(outRgba, col);
			ApplyGlowColor(glow, FromImVec4(col));
			glow->flashing() = false;
			glow->glowing() = true;
			PushForceColor(col);
			return true;
		}
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehForceManageColor(float* color4) {
	__try {
		if (!color4 || !g_tlForceValid || !Mem::IsReadable(color4, 16))
			return false;
		color4[0] = g_tlForceRgba[0];
		color4[1] = g_tlForceRgba[1];
		color4[2] = g_tlForceRgba[2];
		color4[3] = g_tlForceRgba[3];
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

std::int64_t SehOnApplyGlowScene(
	CGlowProperty* glow,
	void* sceneNode,
	std::int64_t(__fastcall* original)(CGlowProperty*, void*))
{
	ImVec4 col{};
	bool enable = false;
	bool owned = false;

	__try {
		if (!AnyEnabled()) {
			ClearForceColor();
		} else if (glow && glow->ok()
			&& ResolvePlayerGlow(glow, col, &enable) && enable) {
			// Pre-stamp so B499F0 override path + ManageGlow get pure glow colours
			// before any mesh/entity pre-fill can win.
			StampPlayerGlow(glow, col);
			PushForceColor(col);
			owned = true;
		} else if (glow && glow->ok() && ResolveWorldGlow(glow, col)) {
			// Dropped weapon / bomb / nade - force same as player path
			ApplyGlowColor(glow, FromImVec4(col));
			glow->flashing() = false;
			glow->glow_type() = 3;
			glow->glowing() = true;
			PushForceColor(col);
			owned = true;
		} else {
			ClearForceColor();
			// Only clear player glows we filter out - never touch world weapon/bomb glow.
			// Old path set glowing=false on every non-player -> world ESP glow dead.
			if (glow && glow->ok() && OwnerIsPlayerPawn(glow))
				glow->glowing() = false;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ClearForceColor();
		owned = false;
	}

	std::int64_t ret = 0;
	if (original) {
		__try {
			ret = original(glow, sceneNode);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ret = 0;
		}
	}

 // Post: last-mile scene object colour (IDA scene+0xD0).
	if (owned) {
		__try {
			ForceSceneObjectColor(glow, col);
			StampPlayerGlow(glow, col);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	ClearForceColor();
	return ret;
}

} // namespace

void* SehGlowMgrGet(FnGlowMgrGet fn) {
	if (!fn)
		return nullptr;
	__try {
		return fn();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool Init() {
	if (g_engTried)
		return g_onGlowTypeChanged != nullptr || g_glowMgrGet != nullptr;
	g_engTried = true;

	g_glowMgrGet = reinterpret_cast<FnGlowMgrGet>(M::FindPattern("client",
		"48 8B 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 41 38 C3"));
	if (!g_glowMgrGet) {
		g_glowMgrGet = reinterpret_cast<FnGlowMgrGet>(M::FindPattern("client",
			"48 8B 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 41"));
	}

	g_onGlowTypeChanged = reinterpret_cast<FnOnGlowTypeChanged>(M::FindPattern("client",
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 05 ? ? ? ?"));
	if (!g_onGlowTypeChanged) {
		g_onGlowTypeChanged = reinterpret_cast<FnOnGlowTypeChanged>(M::FindPattern("client",
			"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B F1"));
	}

	g_glowManager = SehGlowMgrGet(g_glowMgrGet);

	Con::Info("Glow mgr=%p OnTypeChanged=%p",
		g_glowManager, (void*)g_onGlowTypeChanged);
	if (g_onGlowTypeChanged)
		Con::Ok("Glow: OnGlowTypeChanged ready (manager register)");
	else
		Con::OffsetMiss("Glow::OnGlowTypeChanged");
	if (!g_glowMgrGet)
		Con::OffsetMiss("Glow::GlowObjectManager_GetInstance");
	return true;
}

void* GlowManager() {
	if (!g_engTried)
		Init();
	return g_glowManager;
}

ColorRgba FromImVec4(const ImVec4& c) {
	auto ch = [](float v) -> std::uint8_t {
		if (v < 0.f) v = 0.f;
		if (v > 1.f) v = 1.f;
		return static_cast<std::uint8_t>(v * 255.f + 0.5f);
	};
	return ColorRgba{ ch(c.x), ch(c.y), ch(c.z), ch(c.w) };
}

void IsolateFromMeshTint(void* baseModelEntity) {
	// IDA B04B30: a8 = *(entity+0xE38) written to scene+248; if < 1.0 a
	// side-path runs on scene object. Force 1.0 so glow stays on the pure
	// colour float4 path (not mesh-linked).
	if (!Mem::ValidEntity(baseModelEntity))
		return;
	static std::uint32_t s_off = 0;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		s_off = SchemaFinder::Get(
			hash_32_fnv1a_const("C_BaseModelEntity->m_flGlowBackfaceMult"));
		if (!s_off)
			s_off = 0xE38; // client_dll.hpp
	}
	if (!s_off)
		return;
	auto* p = reinterpret_cast<float*>(
		reinterpret_cast<std::uint8_t*>(baseModelEntity) + s_off);
	if (!Mem::IsReadable(p, 4))
		return;
	__try {
		*p = 1.f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void ApplyGlowColor(CGlowProperty* glow, const ColorRgba& col) {
	if (!glow || !glow->ok())
		return;
	// Engine B499F0: if override is (0,0,0,255) it keeps prior float RGB
	// (often mesh/entity fill). Keep at least one channel live.
	// Alpha is passed through - ManageGlow needs a > 0 to keep the scene object.
	ColorRgba c = col;
	if (c.r == 0 && c.g == 0 && c.b == 0)
		c.r = 1;
	if (c.a == 0)
		c.a = 13; // ~0.05 - matches ForceSceneColor floor
	glow->color_override() = c;
 // Float RGB @ +0x8 - scene path; must not track vertex tint
	if (float* f = glow->glow_color_f3()) {
		if (Mem::IsReadable(f, 12)) {
			f[0] = c.r * (1.f / 255.f);
			f[1] = c.g * (1.f / 255.f);
			f[2] = c.b * (1.f / 255.f);
		}
	}
}

CGlowProperty* ModelGlow(void* baseModelEntity) {
	if (!Mem::ValidEntity(baseModelEntity))
		return nullptr;
	const std::uint32_t off = ResolveMGlow();
	if (!off)
		return nullptr;
	auto* g = reinterpret_cast<CGlowProperty*>(
		reinterpret_cast<std::uint8_t*>(baseModelEntity) + off);
	if (!g->ok())
		return nullptr;
	return g;
}

void* __fastcall OnDrawGlow(CGlowProperty* glow) {
	// IDA sub_180B49B30: getter that returns glow_type @ +0x30 (not void draw).
	// hkDrawGlow mutates via this then calls original for the real return.
	SehOnDrawGlow(glow);
	if (!glow || !glow->ok())
		return nullptr;
	return reinterpret_cast<void*>(
		static_cast<std::uintptr_t>(
			static_cast<unsigned int>(glow->glow_type())));
}

bool ForceSceneColor(CGlowProperty* glow, float* outRgba) {
	return SehForceSceneColor(glow, outRgba);
}

bool ForceManageColor(float* color4) {
	return SehForceManageColor(color4);
}

std::int64_t OnApplyGlowScene(CGlowProperty* glow, void* sceneNode,
	std::int64_t(__fastcall* original)(CGlowProperty*, void*))
{
	return SehOnApplyGlowScene(glow, sceneNode, original);
}

void ApplyPlayer(void* pawn, bool visible) {
	SehApplyPlayer(pawn, visible);
}

void ApplyWorld(void* baseModelEntity, const ImVec4& color, bool enable) {
	SehApplyWorld(baseModelEntity, FromImVec4(color), enable);
}

} // namespace Glow

