#pragma once
#include <cstdint>
#include <vector>
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "..\..\..\cs2\entity\C_CSPlayerPawn\C_CSPlayerPawn.h"
#include "..\..\..\cs2\entity\C_BaseEntity\C_BaseEntity.h"

class LocalPlayerCached {
public:
	int handle = 0;
	int health = 0;
	int armor = 0;
	int team = 0;
	int lastTeam = 0; // retained after death for team filter
bool alive = false;
	bool active = false;
	Vector_t position{};

	void reset() {
		position = Vector_t();
		team = 0;
		health = 0;
		armor = 0;
		handle = 0;
		alive = false;
		active = false;
		// lastTeam intentionally kept
	}
};

enum PlayerType_t : int {
	none = -1,
	enemy = 0,
	team = 1,
};

struct PlayerCache {
	CBaseHandle handle{};
	PlayerType_t type = none;
	int health = 0;
	int maxHealth = 100;
	int armor = 0;
	int team_num = 0;
	Vector_t position{};
	Vector_t viewOffset{};
	char name[128]{};
	std::uint64_t steamId = 0;
	char weapon_name[64]{};
	char weapon_key[48]{}; // icon lookup key: ak47 / m4a1_silencer
	const char* icon_glyph = nullptr;
	int clip = -1;
	int maxClip = -1;

	// Flags
	bool flashed = false;
	bool bomb = false;      // carrying / planting C4
	bool scoped = false;
	bool reloading = false;
	bool defusing = false;
	bool has_helmet = false;
	bool has_defuser = false;
	int money = -1;         // <0 unknown
	int rank = 0;           // m_iCompetitiveRanking
	// Nade inventory bits: H=HE F=Flash S=Smoke M=Molly/Inc D=Decoy
	bool nade_he = false;
	bool nade_flash = false;
	bool nade_smoke = false;
	bool nade_molly = false;
	bool nade_decoy = false;

	// Line-of-sight from local eye (true if no wall / hit is this pawn)
	bool visible = true;
	// Sticky vis sample - reused when pawn barely moved (skip Trace every frame)
	bool visCached = false;
	float visSampleX = 0.f;
	float visSampleY = 0.f;
	float visSampleZ = 0.f;
};

enum WorldEspKind : int {
	WORLD_WEAPON = 0,
	WORLD_BOMB,
	WORLD_SMOKE,
	WORLD_MOLOTOV,
	WORLD_HE,
	WORLD_FLASH,
	WORLD_DECOY,
};

struct WorldCache {
	WorldEspKind kind = WORLD_WEAPON;
	Vector_t position{};
	Vector_t land_position{}; // predicted landing spot for accurate warn distance
	char label[64]{};
	char weapon_key[48]{}; // for WORLD_WEAPON icon draw
	const char* icon_glyph = nullptr;
	float timer = -1.f; // bomb/defuse seconds remaining; <0 = none
	float radius = 0.f; // smoke cloud / molly fire ring (units)
	int bomb_site = -1; // 0=A, 1=B
	bool defusing = false;
	bool defused = false;
	float blow_left = -1.f;
	float blow_full = 40.f;
	float defuse_left = -1.f;
	float defuse_full = 10.f;
	bool effect_active = false; // smoke popped / inferno burning
	bool use_badge = false;     // cool warn-style icon (smoke/molly)
	// C_Inferno lit fire samples (yougey DrawInferno hull)
	Vector_t fire_pos[64]{};
	int fire_count = 0;
	float fire_half_width = 0.f; // C_Inferno->m_maxFireHalfWidth
};

// Shared planted-C4 state for bomb HUD widget (independent of world ESP draw)
struct PlantedBombInfo {
	bool active = false;
	int site = -1;           // 0=A, 1=B
	float blowLeft = -1.f;   // seconds to explosion (kept after defuse)
	float defuseLeft = -1.f; // seconds to finish defuse; <0 if not defusing
	float defuseLength = 10.f; // kit 5 / no kit 10 (C_PlantedC4->m_flDefuseLength)
	bool defusing = false;
	bool defused = false;    // successfully defused - widget stays up
	Vector_t position{};
};
extern PlantedBombInfo g_plantedBomb;

namespace Esp {
	class Visuals {
	public:
		void init();
		void esp();
	private:
		bool ensureViewMatrix();
		void drawPlayers();
		void drawWorld();
		ViewMatrix viewMatrix;
	};
	void cache();
	void ResetWorldFxTimers(); // clear smoke/fire/decoy expiry (LevelInit)
	// Map unload - drop player/world cache + effect timers
	void InvalidateCaches();
	// Single source of truth for Present gating (main.cpp needOverlay / needEspCache).
	// True when anything consumes the player cache / world entity walk.
	bool NeedPlayerCache();
	bool NeedWorldCache();
}

extern LocalPlayerCached cached_local;
// Present writes back buffer then publishes via EspPublishPlayers (atomic swap).
// Readers use EspPlayersSnapshot / EspLookupVisible - never iterate globals mid-write.
extern std::vector<PlayerCache> cached_players;
extern std::vector<WorldCache> cached_world;
// Copy published players (no SEH). maxOut caps; returns count.
int EspPlayersSnapshot(PlayerCache* out, int maxOut);
// Visible flag from published cache; true if unknown (fail-open).
bool EspLookupVisible(std::uint32_t handleRaw);
// False if handle not in published cache (caller may fall back to a trace).
bool EspTryLookupVisible(std::uint32_t handleRaw, bool& outVisible);

