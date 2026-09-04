#pragma once
#include "../../utils/math/vector/vector.h"
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include <cstdint>
#include <string>
#include <vector>

// 9xth grenade lineup helper (stand circle + aim marker).
// Stand = abs origin (GetAbsOrigin / scene+0x330). Aim = view angles -> stand + viewOffset.z + fwd*350.
// ang.z in JSON is captured eye lift (crouch ~46 / stand ~64), not roll.
namespace GrenadeHelper {
enum class ThrowType : int {
    Stand = 0,   // was Normal - keep value for old JSON
    Jump = 1,    // was Jump
    Walk = 2,
    Run = 3,
    Crouch = 4,
    RunJump = 5,
    CrouchJump = 6,
    CrouchWalk = 7,
    WalkJump = 8,
    LeftJump = 9,
    RightJump = 10,
    BackJump = 11,
    Left = 12,
    Right = 13,
    Back = 14,
    // Back-compat aliases
    Normal = Stand,
    Count = 15
};

enum class NadeKind : int {
    Any = 0,
    HE,
    Flash,
    Smoke,
    Molly,
    Decoy
};

struct Lineup {
    std::string name;
    std::string map; // base map name (de_mirage, ...)
    NadeKind kind = NadeKind::Any;
    ThrowType throwType = ThrowType::Normal;
    // Captured input recipe, ordered - e.g. "A 0.8s -> SPACE -> throw".
    // Empty for legacy lineups (they show throwType only).
    std::string inputs;
    Vector_t pos{};  // stand feet
    QAngle_t aimAngles{};
    Vector_t target{}; // eye + forward * 8192 (aim marker world)
    bool enabled = true;
    // true = loaded from packs/ (community). Save() skips these so user file stays clean.
    bool fromPack = false;
};

void Init();
void Shutdown();
void OnLevelInit(const char* mapName);
void Update();
// CreateMove / Present - capture key + nearest target
void Draw(const ViewMatrix& vm);
// Menu / hotkey - ArmCapture waits for throw; auto-detects Jump/Run/Walk/Crouch
bool ArmCapture(const char* name, NadeKind kindOverride = NadeKind::Any);
void CancelCapture();
bool IsCapturing();
// Live auto-detected throw style while armed (Stand when idle) - menu feedback
ThrowType CurrentDetected();
bool Capture(const char* name, ThrowType throwType, NadeKind kindOverride = NadeKind::Any);
bool RemoveAt(int index);
bool RenameAt(int index, const char* newName);
bool SetThrowAt(int index, ThrowType t);
bool SetEnabledAt(int index, bool enabled);
void ClearCurrentMap();
bool Save();
bool Load();
const std::vector<Lineup>& All();
std::vector<Lineup>& AllMut();
const Lineup* Current();
const char* CurrentMap();
bool IsCurrentMap(const Lineup& L);
int CountCurrentMap();
const char* KindName(NadeKind k);
const char* ThrowName(ThrowType t);
} // namespace GrenadeHelper

