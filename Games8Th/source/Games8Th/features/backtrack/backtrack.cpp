#include "backtrack.h"

#include "../visuals/visuals.h"
#include "../bones/bones.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../trace/trace.h"
#include "../../../../external/imgui/imgui.h"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace Backtrack {
namespace {

constexpr int kMaxRecords = 32;          // 500ms @64Hz - UI cap is 200ms
constexpr std::uint64_t kStaleMs = 600;  // ring drop when pawn unseen this long

struct Record {
	std::uint64_t ms = 0;
	Vector_t head{};
	Vector_t chest{};
	Vector_t pelvis{};
	Vector_t origin{};
};

struct Ring {
	Record rec[kMaxRecords]{};
	int head = 0;   // next write slot
	int count = 0;
	std::uint64_t lastSeen = 0;
};

std::unordered_map<void*, Ring> g_rings;

// Rings are written on the game thread (CreateMove) and read on the Present
// thread (DrawGhosts). SRW keeps record copies tear-free; contention is one
// short exclusive burst per tick.
SRWLOCK g_lock = SRWLOCK_INIT;

float Dot(const Vector_t& a, const Vector_t& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
float LenSq(const Vector_t& v) {
	return v.x * v.x + v.y * v.y + v.z * v.z;
}

void RecordPawn(void* pawn, std::uint64_t now, Ring& ring) {
	auto* p = static_cast<C_CSPlayerPawn*>(pawn);
	Record r{};
	r.ms = now;
	if (!Bones::GetOrigin(p, r.origin)) {
		__try { r.origin = p->m_vOldOrigin(); } __except (EXCEPTION_EXECUTE_HANDLER) { r.origin = Vector_t{}; }
		if (!Bones::IsValidPos(r.origin))
			r.origin = p->getPosition();
	}
	if (!Bones::IsValidPos(r.origin))
		return;

	Vector_t slots[Bones::S_COUNT]{};
	bool valid[Bones::S_COUNT]{};
	const int count = Bones::CollectSkeletonPoints(p, slots, valid, true);

	if (count > 0 && valid[Bones::S_HEAD] && Bones::IsValidPos(slots[Bones::S_HEAD]))
		r.head = slots[Bones::S_HEAD];
	else
		r.head = r.origin + Vector_t{ 0.f, 0.f, 64.f };

	if (count > 0 && valid[Bones::S_PELVIS] && Bones::IsValidPos(slots[Bones::S_PELVIS]))
		r.pelvis = slots[Bones::S_PELVIS];
	else
		r.pelvis = r.origin + Vector_t{ 0.f, 0.f, 32.f };

	if (count > 0 && valid[Bones::S_SPINE2] && Bones::IsValidPos(slots[Bones::S_SPINE2]))
		r.chest = slots[Bones::S_SPINE2];
	else
		r.chest = (r.head + r.pelvis) * 0.5f;

	ring.rec[ring.head] = r;
	ring.head = (ring.head + 1) % kMaxRecords;
	if (ring.count < kMaxRecords)
		++ring.count;
}

} // namespace

int ExtraTicks() {
	if (!Config::backtrack || Config::backtrack_ms <= kInterpMs)
		return 0;
	const float over = Config::backtrack_ms - kInterpMs;
	int t = static_cast<int>(over / kTickMs + 0.5f);
	return (t < 0) ? 0 : (t > kMaxClaimTicks ? kMaxClaimTicks : t);
}

float TargetDepthMs() {
	return kInterpMs + static_cast<float>(ExtraTicks()) * kTickMs;
}

void Tick(void* localPawn) {
	(void)localPawn;
	const std::uint64_t now = GetTickCount64();

	if (!Config::backtrack || !I::GameEntity || !I::GameEntity->Instance) {
		AcquireSRWLockExclusive(&g_lock);
		if (!g_rings.empty())
			g_rings.clear();
		ReleaseSRWLockExclusive(&g_lock);
		return;
	}

	C_CSPlayerPawn* local = H::SafeLocalAlive();
	if (!local) {
		AcquireSRWLockExclusive(&g_lock);
		if (!g_rings.empty())
			g_rings.clear();
		ReleaseSRWLockExclusive(&g_lock);
		return;
	}

	// Published player cache (atomic double buffer - never iterate
	// cached_players off the Present thread).
	PlayerCache snapshot[64];
	const int n = EspPlayersSnapshot(snapshot, 64);

	AcquireSRWLockExclusive(&g_lock);

	for (int i = 0; i < n; ++i) {
		const PlayerCache& ep = snapshot[i];
		if (!ep.handle.valid() || ep.health <= 0)
			continue;
		auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(ep.handle);
		if (!pawn || !Mem::ValidEntity(pawn) || pawn == local)
			continue;

		Ring& ring = g_rings[pawn];
		ring.lastSeen = now;
		RecordPawn(pawn, now, ring);
	}

	// Drop rings not refreshed recently (left / died / recycled pointer).
	for (auto it = g_rings.begin(); it != g_rings.end();) {
		if (now - it->second.lastSeen > kStaleMs)
			it = g_rings.erase(it);
		else
			++it;
	}

	ReleaseSRWLockExclusive(&g_lock);
}

// Shared pick: record nearest TargetDepthMs within tolerance. Caller holds lock.
static const Record* PickRecord(const Ring& ring, const std::uint64_t now) {
	const float wantMs = TargetDepthMs();
	const Record* best = nullptr;
	float bestDiff = 1e9f;
	for (int i = 0; i < ring.count; ++i) {
		const Record& r = ring.rec[(ring.head - 1 - i + kMaxRecords * 2) % kMaxRecords];
		const float age = static_cast<float>(now - r.ms);
		if (age < 0.f)
			continue;
		const float diff = std::fabs(age - wantMs);
		if (diff < bestDiff) {
			bestDiff = diff;
			best = &r;
		}
		if (age > wantMs + 60.f)
			break;
	}
	if (!best && ring.count > 0)
		best = &ring.rec[(ring.head - 1 + kMaxRecords) % kMaxRecords];
	return best;
}


// Present thread. Ghost skeleton at target depth: pelvis->chest->head + head dot.
void DrawGhosts(const ViewMatrix& vm) {
	if (!Config::backtrack || !Config::backtrack_skeleton || Config::backtrack_ms <= 0.f)
		return;

	struct GhostCopy {
		Vector_t head{}, chest{}, pelvis{}, origin{};
	};
	GhostCopy ghosts[64];
	int n = 0;

	const std::uint64_t now = GetTickCount64();
	AcquireSRWLockShared(&g_lock);
	for (auto& kv : g_rings) {
		if (n >= 64)
			break;
		Ring& ring = kv.second;
		if (ring.count == 0)
			continue;
		const Record* r = PickRecord(ring, now);
		if (!r)
			continue;
		ghosts[n].head = r->head;
		ghosts[n].chest = r->chest;
		ghosts[n].pelvis = r->pelvis;
		ghosts[n].origin = r->origin;
		++n;
	}
	ReleaseSRWLockShared(&g_lock);
	if (n == 0)
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const ImU32 col = IM_COL32(255, 150, 45, 220);   // vibrant orange ghost
	const ImU32 colDim = IM_COL32(255, 150, 45, 120);
	const ImU32 shadow = IM_COL32(0, 0, 0, 160);

	for (int i = 0; i < n; ++i) {
		const GhostCopy& g = ghosts[i];
		Vector_t vHead, vChest, vPelvis, vOrigin;
		if (!vm.WorldToScreen(g.head, vHead)) continue;
		if (!vm.WorldToScreen(g.chest, vChest)) continue;
		if (!vm.WorldToScreen(g.pelvis, vPelvis)) continue;
		const bool hasOrigin = vm.WorldToScreen(g.origin, vOrigin);

		const ImVec2 sHead(vHead.x, vHead.y);
		const ImVec2 sChest(vChest.x, vChest.y);
		const ImVec2 sPelvis(vPelvis.x, vPelvis.y);

		// Shadow understrokes
		dl->AddLine(sPelvis, sChest, shadow, 2.8f);
		dl->AddLine(sChest, sHead, shadow, 2.8f);

		// Spine lines
		dl->AddLine(sPelvis, sChest, col, 1.8f);
		dl->AddLine(sChest, sHead, col, 1.8f);

		// Head sphere & dot
		dl->AddCircleFilled(sHead, 4.0f, colDim);
		dl->AddCircle(sHead, 4.0f, col, 12, 1.2f);
		dl->AddCircleFilled(sHead, 2.0f, IM_COL32(255, 255, 255, 240));

		// Chest dot
		dl->AddCircleFilled(sChest, 2.5f, col);
		// Pelvis dot
		dl->AddCircleFilled(sPelvis, 2.5f, col);

		if (hasOrigin) {
			dl->AddRect(
				ImVec2(vOrigin.x - 3.f, vOrigin.y - 3.f),
				ImVec2(vOrigin.x + 3.f, vOrigin.y + 3.f), colDim);
		}
	}
}

void ApplyCommand(CUserCmd* cmd) {
	if (!cmd || !Config::backtrack || Config::backtrack_ms <= 0.f)
		return;

	const int extra = ExtraTicks();
	if (extra <= 0)
		return;

	const bool isAttack1 = (cmd->nButtons.nValue & IN_ATTACK) != 0;
	const bool isAttack2 = (cmd->nButtons.nValue & IN_SECOND_ATTACK) != 0;
	if (!isAttack1 && !isAttack2)
		return;

	auto& f = cmd->csgoUserCmd.inputHistoryField;
	// nCurrentSize must also be inside the allocated span - a corrupted size
	// here walks past tElements and the -= extra below becomes a wild write.
	if (!f.pRep || f.nCurrentSize <= 0 || f.nCurrentSize > 128
		|| f.pRep->nAllocatedSize <= 0 || f.pRep->nAllocatedSize > 128
		|| f.nCurrentSize > f.pRep->nAllocatedSize)
		return;

	const int lastIdx = f.nCurrentSize - 1;

	// Ensure attack history indices are set so the server's StartLagCompensation processes the rewind
	if (isAttack1 && cmd->csgoUserCmd.nAttack1StartHistoryIndex < 0) {
		cmd->csgoUserCmd.SetAttack1StartHistoryIndex(lastIdx);
	}
	if (isAttack2 && cmd->csgoUserCmd.nAttack2StartHistoryIndex < 0) {
		cmd->csgoUserCmd.SetAttack2StartHistoryIndex(lastIdx);
	}

	for (int i = 0; i < f.nCurrentSize; ++i) {
		auto* e = f.pRep->tElements[i];
		if (!e)
			continue;

		if (e->nRenderTickCount > extra) {
			e->nRenderTickCount -= extra;
			e->flRenderTickFraction = 0.0f;
			e->SetBits(INPUT_HISTORY_BITS_RENDERTICKCOUNT | INPUT_HISTORY_BITS_RENDERTICKFRACTION);
		}
		if (e->nPlayerTickCount > extra) {
			e->nPlayerTickCount -= extra;
			e->flPlayerTickFraction = 0.0f;
			e->SetBits(INPUT_HISTORY_BITS_PLAYERTICKCOUNT | INPUT_HISTORY_BITS_PLAYERTICKFRACTION);
		}

		if (e->sv_interp0) {
			e->sv_interp0->nSrcTick = -1;
			e->sv_interp0->nDstTick = -1;
			e->sv_interp0->flFraction = 0.0f;
			e->sv_interp0->SetBits(0x7);
		}
		if (e->sv_interp1) {
			e->sv_interp1->nSrcTick = -1;
			e->sv_interp1->nDstTick = -1;
			e->sv_interp1->flFraction = 0.0f;
			e->sv_interp1->SetBits(0x7);
		}
		if (e->cl_interp) {
			e->cl_interp->flFraction = 0.0f;
			e->cl_interp->SetBits(0x1);
		}
	}
}

void Reset() {
	AcquireSRWLockExclusive(&g_lock);
	g_rings.clear();
	ReleaseSRWLockExclusive(&g_lock);
}

} // namespace Backtrack

