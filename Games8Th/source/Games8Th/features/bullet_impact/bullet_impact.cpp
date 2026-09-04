#include "bullet_impact.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/math/vector/vector.h"
#include "../bones/bones.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/handle.h"
#include "../../../cs2/sdk/IGameEvent.h"

#include <Windows.h>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cstdint>

namespace {

	constexpr int kMaxImpacts = 64;

	struct ImpactPt {
		Vector_t pos{};
		Vector_t eye{};
		float time = 0.f;
		bool valid = false;
	};
	ImpactPt g_impacts[kMaxImpacts]{};
	int g_impactWrite = 0;

	struct LastFireRay {
		Vector_t eye{};
		Vector_t dir{};
		float time = 0.f;
		bool valid = false;
	};
	LastFireRay g_lastFire{};

	static SRWLOCK g_impactLock = SRWLOCK_INIT;

	// FireEventClientSide does not receive every client event. Register the
	// bullet listener on the same GameEventManager path used by mercey.
	struct GameEventListener {
		void** vtable = nullptr;
		int debugId = 0;
	};

	using AddListenerFn = bool(__fastcall*)(void*, GameEventListener*, const char*, bool);
	using RemoveListenerFn = void(__fastcall*)(void*, GameEventListener*);

	void** g_ppEventManager = nullptr;
	void* g_eventManager = nullptr;
	void* g_listenerVtable[3]{};
	GameEventListener g_listener{};
	bool g_listenerActive = false;

	void* __fastcall ListenerFireEvent(void*, void* event) {
		BulletFx::OnGameEvent(event);
		return nullptr;
	}

	int __fastcall ListenerGetDebugId(void* self) {
		if (!self)
			return 0;
		return reinterpret_cast<GameEventListener*>(self)->debugId;
	}

	// Keep SEH in a POD-only frame. MSVC C2712 rejects __try in the resolver
	// because M::FindPattern takes a temporary std::string.
	bool CallAddListener(AddListenerFn add, void* manager, GameEventListener* listener) noexcept {
		if (!add || !manager || !listener)
			return false;
		bool ok = false;
		__try { ok = add(manager, listener, "bullet_impact", false); }
		__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
		return ok;
	}

	void CallRemoveListener(RemoveListenerFn remove, void* manager, GameEventListener* listener) noexcept {
		if (!remove || !manager || !listener)
			return;
		__try { remove(manager, listener); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	bool RegisterListener() {
		const auto hit = M::FindPattern("client",
			"48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? 45 33 C9 45 33 C0 48 8B 01 FF 50 30");
		if (!hit)
			return false;

		g_ppEventManager = reinterpret_cast<void**>(M::GetAbsoluteAddress(hit, 3));
		if (!g_ppEventManager || !Mem::IsReadable(g_ppEventManager, sizeof(void*)))
			return false;
		g_eventManager = Mem::ReadPtr(g_ppEventManager);
		if (!g_eventManager || !Mem::Valid(g_eventManager, sizeof(void*)))
			return false;

		void** managerVtable = nullptr;
		if (!Mem::ReadField(g_eventManager, 0, managerVtable)
			|| !managerVtable
			|| !Mem::IsReadable(managerVtable, 6 * sizeof(void*)))
			return false;

		AddListenerFn add = nullptr;
		if (!Mem::ReadField(managerVtable, 3 * sizeof(void*), add) || !add)
			return false;

		g_listenerVtable[0] = nullptr;
		g_listenerVtable[1] = reinterpret_cast<void*>(&ListenerFireEvent);
		g_listenerVtable[2] = reinterpret_cast<void*>(&ListenerGetDebugId);
		g_listener.vtable = g_listenerVtable;
		g_listener.debugId = 1;

		if (!CallAddListener(add, g_eventManager, &g_listener))
			return false;

		g_listenerActive = true;
		Con::Ok("BulletFx: GameEventManager listener registered");
		return true;
	}

	void UnregisterListener() {
		if (!g_listenerActive || !g_eventManager)
			return;

		void** managerVtable = nullptr;
		RemoveListenerFn remove = nullptr;
		if (Mem::ReadField(g_eventManager, 0, managerVtable)
			&& managerVtable
			&& Mem::ReadField(managerVtable, 5 * sizeof(void*), remove)
			&& remove) {
			CallRemoveListener(remove, g_eventManager, &g_listener);
		}

		g_listenerActive = false;
		g_eventManager = nullptr;
		g_ppEventManager = nullptr;
	}

	float Now() {
		return static_cast<float>(ImGui::GetTime());
	}

	CCSPlayerController* LocalController() {
		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local) return nullptr;
		if (!I::GameEntity || !I::GameEntity->Instance) return nullptr;
		__try {
			CBaseHandle hCtrl = local->m_hController();
			if (hCtrl.valid())
				return I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		return nullptr;
	}

	bool CopyLastFire(LastFireRay& out) {
		AcquireSRWLockShared(&g_impactLock);
		out = g_lastFire;
		ReleaseSRWLockShared(&g_impactLock);
		return out.valid;
	}

	void PushImpact(const Vector_t& pos) {
		if (!Bones::IsValidPos(pos))
			return;

		const float now = Now();
		LastFireRay lastFire{};
		Vector_t eye{};
		if (CopyLastFire(lastFire) && (now - lastFire.time) <= 1.f
			&& Bones::IsValidPos(lastFire.eye)) {
			eye = lastFire.eye;
		} else if (C_CSPlayerPawn* local = H::SafeLocalPlayer()) {
			eye = Bones::GetShootPos(local);
		}

		AcquireSRWLockExclusive(&g_impactLock);
		ImpactPt& e = g_impacts[g_impactWrite];
		g_impactWrite = (g_impactWrite + 1) % kMaxImpacts;
		e.pos = pos;
		e.eye = eye;
		e.time = now;
		e.valid = true;
		ReleaseSRWLockExclusive(&g_impactLock);
	}

	void HandleBulletImpact(IGameEvent* ev) {
		CCSPlayerController* localCtrl = LocalController();
		if (localCtrl) {
			CCSPlayerController* shooter = ev->GetPlayerController("userid");
			if (shooter && shooter != localCtrl)
				return;
		}

		float x = ev->GetFloat("x");
		float y = ev->GetFloat("y");
		float z = ev->GetFloat("z");

		if (std::fabs(x) < 1.f && std::fabs(y) < 1.f && std::fabs(z) < 1.f)
			return;

		PushImpact(Vector_t{ x, y, z });
	}

	ImU32 ImpactColor(ImVec4 color, float alpha) {
		color.w = std::clamp(color.w * alpha, 0.f, 1.f);
		return ImGui::ColorConvertFloat4ToU32(color);
	}

	void DrawImpactCube(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& pos, float alpha) {
		constexpr float halfSize = 1.75f;
		const Vector_t corners[8] = {
			pos + Vector_t(-halfSize, -halfSize, -halfSize),
			pos + Vector_t( halfSize, -halfSize, -halfSize),
			pos + Vector_t( halfSize,  halfSize, -halfSize),
			pos + Vector_t(-halfSize,  halfSize, -halfSize),
			pos + Vector_t(-halfSize, -halfSize,  halfSize),
			pos + Vector_t( halfSize, -halfSize,  halfSize),
			pos + Vector_t( halfSize,  halfSize,  halfSize),
			pos + Vector_t(-halfSize,  halfSize,  halfSize),
		};
		ImVec2 screen[8]{};
		for (int i = 0; i < 8; ++i) {
			Vector_t projected{};
			if (!vm.WorldToScreen(corners[i], projected))
				return;
			screen[i] = ImVec2(projected.x, projected.y);
		}

		static constexpr int faces[6][4] = {
			{ 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
			{ 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 },
		};
		static constexpr int edges[12][2] = {
			{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
			{ 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
		};

		const ImU32 fill = ImpactColor(Config::bullet_impact_effect_fill_color, alpha);
		const ImU32 edge = ImpactColor(Config::bullet_impact_effect_edge_color, alpha);
		if (Config::bullet_impact_effect_glow) {
			const float glowAlpha = alpha * std::clamp(Config::bullet_impact_effect_glow_strength, 0.f, 2.f);
			const ImU32 glowFill = ImpactColor(Config::bullet_impact_effect_fill_color, glowAlpha * 0.45f);
			const ImU32 glowEdge = ImpactColor(Config::bullet_impact_effect_edge_color, glowAlpha);
			for (const auto& face : faces) {
				ImVec2 poly[4] = { screen[face[0]], screen[face[1]], screen[face[2]], screen[face[3]] };
				dl->AddConvexPolyFilled(poly, 4, glowFill);
			}
			for (const auto& line : edges)
				dl->AddLine(screen[line[0]], screen[line[1]], glowEdge, 3.f);
		}

		for (const auto& face : faces) {
			ImVec2 poly[4] = { screen[face[0]], screen[face[1]], screen[face[2]], screen[face[3]] };
			dl->AddConvexPolyFilled(poly, 4, fill);
		}
		for (const auto& line : edges)
			dl->AddLine(screen[line[0]], screen[line[1]], edge, 1.f);
	}

	void DrawImpactSparks(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& pos, float alpha) {
		Vector_t projected{};
		if (!vm.WorldToScreen(pos, projected))
			return;
		const ImVec2 center(projected.x, projected.y);
		const ImU32 color = ImpactColor(Config::bullet_impact_effect_color_spark, alpha);
		const float outer = 5.f + (1.f - alpha) * 11.f;
		for (int i = 0; i < 8; ++i) {
			const float angle = static_cast<float>(i) * 0.785398163f;
			const ImVec2 inner(center.x + std::cos(angle) * 2.f, center.y + std::sin(angle) * 2.f);
			const ImVec2 end(center.x + std::cos(angle) * outer, center.y + std::sin(angle) * outer);
			dl->AddLine(inner, end, color, 1.25f);
		}
	}

	void DrawBulletEffects(ImDrawList* dl, const ViewMatrix& vm) {
		if (!dl)
			return;
		const int type = std::clamp(Config::bullet_impact_effect_type, 0, 2);
		const bool showOverlay = Config::bullet_impact_effect && (type == 0 || type == 2);
		const bool showSparks = Config::bullet_impact_effect && (type == 1 || type == 2);
		if (!showOverlay && !showSparks && !Config::bullet_tracers)
			return;

		const float now = Now();
		const float impactLife = (std::max)(Config::bullet_impact_effect_duration, 0.1f);
		const float tracerLife = (std::max)(Config::bullet_tracer_duration, 0.1f);
		AcquireSRWLockExclusive(&g_impactLock);
		for (auto& impact : g_impacts) {
			if (!impact.valid)
				continue;
			const float age = now - impact.time;
			const float maxLife = (std::max)(showOverlay || showSparks ? impactLife : 0.f,
				Config::bullet_tracers ? tracerLife : 0.f);
			if (age < 0.f || age >= maxLife) {
				if (age >= maxLife)
					impact.valid = false;
				continue;
			}

			if (Config::bullet_tracers && age < tracerLife && Bones::IsValidPos(impact.eye)) {
				Vector_t eyeScreen{}, impactScreen{};
				if (vm.WorldToScreen(impact.eye, eyeScreen) && vm.WorldToScreen(impact.pos, impactScreen)) {
					const float fade = 1.f - std::clamp(age / tracerLife, 0.f, 1.f);
					dl->AddLine(ImVec2(eyeScreen.x, eyeScreen.y), ImVec2(impactScreen.x, impactScreen.y),
						ImpactColor(Config::bullet_tracer_color, fade), 1.5f);
				}
			}

			if (age < impactLife) {
				const float progress = std::clamp(age / impactLife, 0.f, 1.f);
				const float fade = 1.f - progress * progress;
				if (showOverlay)
					DrawImpactCube(dl, vm, impact.pos, fade);
				if (showSparks)
					DrawImpactSparks(dl, vm, impact.pos, fade);
			}
		}
		ReleaseSRWLockExclusive(&g_impactLock);
	}

} // namespace

namespace BulletFx {

	void Install() {
		UnregisterListener();
		Reset();
		if (!RegisterListener())
			Con::Warn("BulletFx: GameEventManager listener unavailable; using FireEventClientSide fallback");
	}

	void Reset() {
		AcquireSRWLockExclusive(&g_impactLock);
		for (int i = 0; i < kMaxImpacts; ++i) g_impacts[i] = ImpactPt{};
		g_impactWrite = 0;
		g_lastFire = LastFireRay{};
		ReleaseSRWLockExclusive(&g_impactLock);
	}

	void Shutdown() {
		UnregisterListener();
		Reset();
	}

	bool ListenerActive() {
		return g_listenerActive;
	}

	void OnGameEvent(void* gameEvent) {
		if (!gameEvent) return;
		IGameEvent* ev = static_cast<IGameEvent*>(gameEvent);
		const char* name = nullptr;
		__try {
			name = ev->GetName();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
		if (!name || !name[0]) return;
		if (std::strcmp(name, "bullet_impact") != 0) return;
		__try {
			HandleBulletImpact(ev);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	void Draw(const ViewMatrix& vm) {
		if (!Config::bullet_impact_effect && !Config::bullet_tracers)
			return;
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		if (!dl) return;
		DrawBulletEffects(dl, vm);
	}

	void NoteLastFire(const Vector_t& eye, const QAngle_t& fireAngles) {
		if (!Bones::IsValidPos(eye) || !fireAngles.IsValid())
			return;
		Vector_t dir{};
		QAngle_t ang = fireAngles;
		ang.z = 0.f;
		ang.x = std::clamp(ang.x, -89.f, 89.f);
		ang.Normalize();
		ang.ToDirections(&dir, nullptr, nullptr);
		const float len = dir.Length();
		if (len < 1e-4f || !std::isfinite(len))
			return;
		AcquireSRWLockExclusive(&g_impactLock);
		g_lastFire.eye = eye;
		g_lastFire.dir = Vector_t{ dir.x / len, dir.y / len, dir.z / len };
		g_lastFire.time = Now();
		g_lastFire.valid = true;
		ReleaseSRWLockExclusive(&g_impactLock);
	}

	bool FindNearestImpact(const Vector_t& anchor, Vector_t& out, float maxAge, float maxDist) {
		const float now = Now();
		const float maxD2 = maxDist * maxDist;
		float best = 1e20f;
		bool found = false;
		AcquireSRWLockShared(&g_impactLock);
		for (int i = 0; i < kMaxImpacts; ++i) {
			if (!g_impacts[i].valid) continue;
			if ((now - g_impacts[i].time) > maxAge) continue;
			const float dx = g_impacts[i].pos.x - anchor.x;
			const float dy = g_impacts[i].pos.y - anchor.y;
			const float dz = g_impacts[i].pos.z - anchor.z;
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 > maxD2 || d2 >= best) continue;
			best = d2;
			out = g_impacts[i].pos;
			found = true;
		}
		ReleaseSRWLockShared(&g_impactLock);
		return found;
	}

	bool GetLastFire(Vector_t& eye, Vector_t& dir) {
		LastFireRay lastFire{};
		if (!CopyLastFire(lastFire))
			return false;
		if ((Now() - lastFire.time) > 1.0f)
			return false;
		if (!Bones::IsValidPos(lastFire.eye) || !Bones::IsValidPos(lastFire.dir))
			return false;
		eye = lastFire.eye;
		dir = lastFire.dir;
		return true;
	}

} // namespace BulletFx

