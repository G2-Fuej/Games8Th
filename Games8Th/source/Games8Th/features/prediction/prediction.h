#pragma once
#include "../../utils/math/vector/vector.h"
#include <cstdint>

// Engine + local movement prediction.
// IDA-verified 2026-08-19 (client 919f9a64 / engine2 0f22e756, imagebase 0x180000000):
// ProcessMovement 0x180A1B5C0 (moveSvc*, CUserCmd*) free fn - unique
// RunCommand_Context 0x180A1CF90 free fn (moveSvc*, cmd*)
// internally: vfunc[46] SetPredictionCommand ->
// tickbase++ (GetTickBase @ controller+0x6B8) -> gv setup -> ProcessMovement ->
// vfunc[47] Reset -> gv restore. Canonical path. Do NOT call from CreateMove
// (extra pred slot -> "full repredict will occur").
// pPrediction getter 0x180B91AB0 -> &unk_1823A5140 (RVA 0x23A5140)
// pattern: 48 8D 05 ? ? ? ? C3 CC...40 53 56 41 54 (unique, 1 hit)
// CPrediction ctor 0x180B6A4B0: in_prediction BYTE +0x34, first_prediction WORD +0xF0
// engine2 RunPrediction 0x180065C50 - CNetworkGameClient::ClientSidePredict(this, reason)
// ProcessSubTickInput 0x180B04240 (this, slot, a3) - 3 args, max 32 steps
// ModernSubtickJumpCheck 0x1808829F0
// CreateNewSubtickMoveStep 0x1804E2400 (E8 from CreateMove fill @ 0x180B0A16C)
// MovementServices_CheckJumpButton 0x180B0D530
// WriteSubtickFromEntry 0x180C8FE60 (src, dst, char, double, int, ctx)
// ForceButtonsDown 0x180A11630 (moveSvc*, button)
// GlobalVars off_1820B05F0 (RVA 0x20B05F0, IDA 2026-08-25 - 1140 refs; old 0x2090D60/0x2094D38/0x2095D48 dead):
// curtime+0x30 frametime+0x34 tickcount+0x44 field20+0x50 threadId+0x58
// MS vfuncs: 46=SetPredictionCommand(cmd), 47=Reset(). vfunc 32 is NOT RunCommand.
// Dump ModernJump: CCSPlayer_MovementServices+0x6C8, m_flLastLandedFrac +0x24
//
// KNOWN BROKEN (do not use - dump names, schema/UI hits):
// QueueForceSubtickMove -> schema desc for CPlayer_MovementServices (m_nImpulse...).
// ProcessForceSubtickMoves -> schema registrar (m_arrForceSubtickMoveWhen).
// SetupMove dump pattern -> buywheel UI ("buywheel-cant-afford"), not movement.
// Force-subtick API (Queue/Process/Flush) + SetupMove resolve as disabled at Init.
class CUserCmd;
class C_CSPlayerPawn;

namespace Pred {
constexpr float kTickInterval = 1.f / 64.f;
constexpr float kGravity = 800.f;
constexpr float kSvAirAccelerate = 12.f;
constexpr float kSvAccelerate = 5.5f;
constexpr float kSvFriction = 5.2f;
constexpr float kSvStopSpeed = 80.f;
constexpr float kAirWishSpeed = 30.f;
constexpr float kMaxMove = 450.f;
// Dump RVA fallbacks (client.dll base + rva) - IDA 2026-08-25
constexpr std::uintptr_t kRvaGlobalVars = 0x20B05F0;
constexpr std::uintptr_t kRvaPrediction = 0x23A5140;
// CPrediction: +0x34 BYTE in_prediction, +0xF0 WORD first_prediction (ctor)
constexpr std::uintptr_t kPredOffInPrediction = 0x34;
constexpr std::uintptr_t kPredOffFirstPrediction = 0xF0;
// Movement services vfuncs (IDA RunCommand uses *a1+368 / +376)
constexpr std::size_t kVfuncSetPredictionCommand = 46;
constexpr std::size_t kVfuncResetPredictionCommand = 47;
constexpr std::size_t kVfuncRunCommand = 32;
// ModernJump dump offsets
constexpr std::uintptr_t kModernJumpOff = 0x6C8;
constexpr std::uintptr_t kLastLandedFracOff = 0x24;
constexpr std::uintptr_t kLastLandedTickOff = 0x20;

struct MoveState {
    Vector_t origin{};
    Vector_t velocity{};
    bool onGround = false;
    float surfaceFriction = 1.f;
    float maxSpeed = 250.f;
};

// Snapshot after last successful engine RunCommand / ProcessMovement / local fallback
struct PredictedState {
    Vector_t origin{};
    Vector_t absOrigin{};
    Vector_t velocity{};
    Vector_t absVelocity{};
    Vector_t eye{};
    std::
uint32_t flags = 0;       // post-sim (same as postFlags)
    std::
uint32_t preFlags = 0;
    std::
uint32_t postFlags = 0;
    int tickBase = 0;
    float curtime = 0.f;
    float frametime = kTickInterval;
    float landFrac = 0.f;          // m_ModernJump.m_flLastLandedFrac post-sim
    bool hasLandFrac = false;
    bool onGround = false;
    bool valid = false;
    bool fromEngine = false;
    bool usedRunCommand = false;
};

struct EngineAddrs {
    void* pPrediction = nullptr;           // CPrediction*
    void* runCommand = nullptr;            // RunCommand_Context free fn (preferred)
    void* processMovement = nullptr;
    void* processSubtickInput = nullptr;
    void* modernSubtickJumpCheck = nullptr;
    void* createNewSubtickMoveStep = nullptr;
    void* checkJumpButton = nullptr;
    void* writeSubtickFromEntry = nullptr;
    void* forceButtonsDown = nullptr;
    void* queueForceSubtickMove = nullptr;
    void* processForceSubtickMoves = nullptr;
    void* setupMove = nullptr;
    void* globalVars = nullptr;            // CGlobalVarsBase*
    void* getTickBase = nullptr;           // GetTickBase(controller) - dump 0x902560 E8 ? ? ? ? EB ? 48 8B 05 ? ? ? ? 8B 40
    bool resolved = false;
};

bool Init();
const EngineAddrs& Engines();
// WriteSubtickFromEntry - engine protobuf writer for input history (silent stamp).
bool StampInputHistory(
    void* historyEntryPb,
    int renderTick,
    float renderFrac,
    int playerTick,
    float playerFrac,
    const QAngle_t& angles,
    const Vector_t& shootPos);

// ---- CreateMove prediction ----
// Original CreateMove already filled the pred slot ring. Do NOT call
// RunCommand / SetPredCmd / tickbase++ - that records an extra slot:
// "server acknowledged to slot 4 client only predicted into 3 slots"
// Start: snapshot live post-CM pawn (simulate is ProcessMovement-only
// and only for nade/rage, never every CreateMove).
// End: no restore (nothing mutated). No CPrediction::Update.
bool Start(CUserCmd* cmd);
void End();
// Map unload / death - drop cached postFlags / pawn
void Invalidate();
bool Active();
const PredictedState& Last();
// True when last Start used free RunCommand / ProcessMovement (not local math).
bool LastFromEngine();
// Prefer predicted state when valid (engine or local), else live pawn.
Vector_t Velocity(C_CSPlayerPawn* pawn);
std::
uint32_t Flags(C_CSPlayerPawn* pawn);
Vector_t Origin(C_CSPlayerPawn* pawn);
Vector_t Eye(C_CSPlayerPawn* pawn);
float LandFrac(C_CSPlayerPawn* pawn);
bool OnGround(C_CSPlayerPawn* pawn);

// ---- Local Source-style math (bhop / autostrafer) ----
void Accelerate(
    Vector_t& vel,
    const Vector_t& wishdir,
    float wishspeed,
    float accel,
    float frametime,
    float maxspeed,
    float surfaceFriction = 1.f);

void AirAccelerate(
    Vector_t& vel,
    const Vector_t& wishdir,
    float wishspeed,
    float accel,
    float frametime,
    float surfaceFriction = 1.f);

void Friction(
    Vector_t& vel,
    bool onGround,
    float friction,
    float stopspeed,
    float frametime,
    float surfaceFriction = 1.f);

void ApplyGravity(Vector_t& vel, float gravity, float frametime);

MoveState SimulateTick(
    const MoveState& in,
    float yawDeg,
    float forwardMove,
    float sideMove,
    float frametime = kTickInterval);

float VelocityYawDeg(const Vector_t& vel);
float IdealStrafeDeltaDeg(float speed2d, float wishspeed = kAirWishSpeed);

bool PredictLandingFrac(
    const Vector_t& origin,
    const Vector_t& mins,
    const Vector_t& vel,
    float groundZ,
    float& outFrac,
    float gravity = kGravity,
    float frametime = kTickInterval,
    float window = 32.f);

// Hull-trace land this tick (uneven ground / stairs / ramps).
// True when airborne falling and TraceHull hits floor within one tick.
// outFrac = subtick when contact happens (for IN_JUMP press).
bool PredictLandThisTick(
    C_CSPlayerPawn* pawn,
    const Vector_t& vel,
    float& outFrac,
    float frametime = kTickInterval);

// Straight-down hull: distance to floor under feet (0 = standing on floor).
// Used with free-fall timing for high drops / stairs when multi-step hull is noisy.
bool ProbeFloorDistance(C_CSPlayerPawn* pawn, float& outDist, float maxDist = 128.f);

float PredictSpeedAfterStrafe(
    const Vector_t& vel,
    float yawDeg,
    float sideSign,
    float frametime = kTickInterval,
    float surfaceFriction = 1.f);

float PredictSpeedAfterWishMoves(
    const Vector_t& vel,
    float viewYawDeg,
    float wishYawDeg,
    float frametime = kTickInterval,
    float surfaceFriction = 1.f);
} // namespace Pred
