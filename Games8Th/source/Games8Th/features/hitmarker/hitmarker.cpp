#include "hitmarker.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/math/vector/vector.h"
#include "../bones/bones.h"
#include "../bullet_impact/bullet_impact.h"
#include "../hitsound/hitsound.h"
#include "../hitlog/hitlog.h"
#include "../aim/aim_common.h"
#include "../trace/trace.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/handle.h"
#include "../../../cs2/sdk/IGameEvent.h"
#include "../w2s/w2s.h"
#include "../../../../external/imgui/imgui.h"

#include <Windows.h>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cstdint>

namespace {
	constexpr int kMaxMarks = 24;
	constexpr int kMaxFloats = 32;
	constexpr float kScreenLife = 0.42f;
	constexpr float kWorldLife = 1.15f;

	struct Mark {
		bool active = false;
		bool world = false;
		bool kill = false;
		bool head = false;
		float born = 0.f;
		float life = kScreenLife;
		float size = 12.f;
		Vector_t worldPos{};
		int damage = 0;
	};

	struct FloatDmg {
		bool active = false;
		bool kill = false;
		bool head = false;
		float born = 0.f;
		float life = 1.f;
		Vector_t worldPos{};
		int damage = 0;
	};

	Mark g_marks[kMaxMarks]{};
	int g_write = 0;
	FloatDmg g_floats[kMaxFloats]{};
	int g_floatWrite = 0;
	int g_floatSeed = 0;

	float g_screenBorn = -1.f;
	float g_screenLife = kScreenLife;
	bool g_screenKill = false;
	bool g_screenHead = false;
	int g_screenDmg = 0;

	struct PendingWorld {
		bool active = false;
		std::uint32_t pawnHandle = 0;
		int hitgroup = 0;
		int damage = 0;
		bool kill = false;
		bool head = false;
		int tries = 0;
		bool havePos = false;
		Vector_t worldPos{};
		float queuedAt = 0.f;
	};
	constexpr int kMaxPending = 8;
	PendingWorld g_pending[kMaxPending]{};
	int g_pendingWrite = 0;

	// Prevents double marks when player_death fires after player_hurt(kill=true)
	// for the same shot. Also lets us re-use the resolved hit pos from hurt.
	struct RecentVictimMark {
		std::uint32_t pawnHandle = 0;
		float time = 0.f;
	};
	constexpr int kMaxRecentMarks = 8;
	RecentVictimMark g_recentMarks[kMaxRecentMarks]{};
	int g_recentMarkWrite = 0;
	static SRWLOCK g_hitLock = SRWLOCK_INIT;

	void NoteVictimMarked(std::uint32_t pawnHandle) {
		if (!pawnHandle) return;
		RecentVictimMark& r = g_recentMarks[g_recentMarkWrite];
		g_recentMarkWrite = (g_recentMarkWrite + 1) % kMaxRecentMarks;
		r.pawnHandle = pawnHandle;
		r.time = static_cast<float>(ImGui::GetTime());
	}

	bool WasVictimMarked(std::uint32_t pawnHandle, float maxAge) {
		if (!pawnHandle) return false;
		const float now = static_cast<float>(ImGui::GetTime());
		for (int i = 0; i < kMaxRecentMarks; ++i) {
			if (g_recentMarks[i].pawnHandle == pawnHandle
				&& (now - g_recentMarks[i].time) <= maxAge)
				return true;
		}
		return false;
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

	std::uint32_t PawnHandleFromController(CCSPlayerController* ctrl) {
		if (!ctrl || !Mem::ValidEntity(ctrl)) return 0;
		__try {
			CBaseHandle hPawn = ctrl->m_hPlayerPawn();
			return hPawn.raw();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	C_CSPlayerPawn* PawnFromHandleRaw(std::uint32_t ph) {
		if (!ph || !I::GameEntity || !I::GameEntity->Instance) return nullptr;
		CBaseHandle h(ph & ENT_ENTRY_MASK, ph >> NUM_SERIAL_NUM_SHIFT_BITS);
		return I::GameEntity->Instance->Get<C_CSPlayerPawn>(h);
	}

	void PushWorld(const Vector_t& pos, bool kill, bool head, int dmg) {
		AcquireSRWLockExclusive(&g_hitLock);
		Mark& m = g_marks[g_write];
		g_write = (g_write + 1) % kMaxMarks;
		m.active = true;
		m.world = true;
		m.kill = kill;
		m.head = head;
		m.born = Now();
		m.life = kWorldLife * std::clamp(Config::hitmarker_duration, 0.25f, 2.5f);
		m.size = Config::hitmarker_world_size;
		m.worldPos = pos;
		m.damage = dmg;
		ReleaseSRWLockExclusive(&g_hitLock);
	}

	void PushFloatDamage(const Vector_t& pos, bool kill, bool head, int dmg) {
		if (!Config::float_damage || dmg <= 0)
			return;
		FloatDmg& f = g_floats[g_floatWrite];
		g_floatWrite = (g_floatWrite + 1) % kMaxFloats;
		f.active = true;
		f.kill = kill;
		f.head = head;
		f.born = Now();
		f.life = std::clamp(Config::float_damage_duration, 0.3f, 3.f);
		f.worldPos = pos;

		// Scatter overlapping numbers
		const float ang = g_floatSeed * 2.39996f; // golden angle
		const float rad = 8.f + (g_floatSeed % 3) * 6.f;
		f.worldPos.x += std::cos(ang) * rad;
		f.worldPos.y += std::sin(ang) * rad;
		f.worldPos.z += 4.f + (g_floatSeed % 4) * 3.f;
		g_floatSeed++;

		f.damage = dmg;
	}

	void PulseScreen(bool kill, bool head, int dmg) {
		g_screenBorn = Now();
		g_screenLife = kScreenLife * std::clamp(Config::hitmarker_duration, 0.25f, 2.5f);
		g_screenKill = kill;
		g_screenHead = head;
		g_screenDmg = dmg;
	}

	// Exact fire ray for the hitmarker raycast: last stamped fire (aimbot /
	// autofire / trigger / manual shot) or rebuilt from the current view +
	// aim punch when the stamp is stale / absent.
	bool GetFireRay(Vector_t& eye, Vector_t& dir) {
		Vector_t lastEye{}, lastDir{};
		if (BulletFx::GetLastFire(lastEye, lastDir)) {
			eye = lastEye;
			dir = lastDir;
			return true;
		}

		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local)
			return false;
		Vector_t e = Bones::GetShootPos(local);
		if (!Bones::IsValidPos(e))
			return false;
		QAngle_t view{};
		if (!AimCommon::GetViewAngles(view) || !view.IsValid())
			return false;
		QAngle_t punch{};
		if (AimCommon::GetFirePunch(local, punch) && punch.IsValid()) {
			view.x += punch.x;
			view.y += punch.y;
		}
		view.z = 0.f;
		view.Normalize();
		Vector_t d{};
		view.ToDirections(&d, nullptr, nullptr);
		const float len = d.Length();
		if (len < 1e-4f || !std::isfinite(len))
			return false;
		eye = e;
		dir = Vector_t{ d.x / len, d.y / len, d.z / len };
		return true;
	}

	// Hitgroup capsule/bone, then nearby bullet_impact, then matching fire-ray.
	bool ResolveVictimPos(C_CSPlayerPawn* pawn, int hitgroup, Vector_t& out, bool exactOnly) {
		if (!pawn || !Mem::ValidEntity(pawn)) return false;

		int hb = Bones::HitgroupToHitbox(hitgroup);
		if (hb < 0)
			hb = (hitgroup == 1) ? Config::HB_HEAD : Config::HB_CHEST;

		Vector_t bone{};
		bool haveBone = false;
		__try {
			haveBone = Bones::GetHitboxPoint(pawn, hb, bone) && Bones::IsValidPos(bone);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			haveBone = false;
		}
		if (!haveBone && hb == Config::HB_HEAD) {
			Vector_t slots[Bones::S_COUNT]{};
			bool valid[Bones::S_COUNT]{};
			__try {
				(void)Bones::CollectSkeletonPoints(pawn, slots, valid, false);
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			if (valid[Bones::S_HEAD] && Bones::IsValidPos(slots[Bones::S_HEAD])) {
				bone = slots[Bones::S_HEAD];
				haveBone = true;
			}
		}

		if (haveBone) {
			Vector_t impact{};
			if (BulletFx::FindNearestImpact(bone, impact, 0.50f, 42.f) && Bones::IsValidPos(impact)) {
				out = impact;
				return true;
			}
		}

		Vector_t eye{}, dir{};
		if (GetFireRay(eye, dir) && (Trace::Ready() || Trace::Init())) {
			const Vector_t end = eye + dir * 8192.f;
			C_CSPlayerPawn* local = H::SafeLocalPlayer();
			Trace::CGameTrace tr{};
			if (Trace::TraceLine(eye, end, local, tr, Trace::kMaskShot, Trace::kFilterLayerVis, 0)) {
				const bool hit = tr.fraction() < 1.f || tr.startsolid();
				if (hit && Trace::HitsTarget(tr.hit_entity(), pawn)) {
					const Vector_t p = tr.endpos();
					if (Bones::IsValidPos(p)) {
						const int thg = tr.hitgroup();
						if (hitgroup >= 1 && thg == hitgroup) {
							out = p;
							return true;
						}
						if (haveBone) {
							const float dx = p.x - bone.x;
							const float dy = p.y - bone.y;
							const float dz = p.z - bone.z;
							if (dx * dx + dy * dy + dz * dz <= 48.f * 48.f) {
								out = p;
								return true;
							}
						}
					}
				}
			}
		}

		if (haveBone) {
			out = bone;
			return true;
		}

		Vector_t origin{};
		if (Bones::GetOrigin(pawn, origin) && Bones::IsValidPos(origin)) {
			Vector_t impact{};
			if (BulletFx::FindNearestImpact(origin, impact, 0.35f, 96.f) && Bones::IsValidPos(impact)
				&& impact.z >= origin.z + 20.f) {
				out = impact;
				return true;
			}
			if (!exactOnly) {
				origin.z += (hitgroup == 1) ? 64.f : 52.f;
				out = origin;
				return true;
			}
		}
		return false;
	}

	void EmitWorldMark(const Vector_t& pos, bool kill, bool head, int dmg) {
		if (!Bones::IsValidPos(pos)) return;
		if (Config::hitmarker && Config::hitmarker_world)
			PushWorld(pos, kill, head, dmg);
		PushFloatDamage(pos, kill, head, dmg);
	}

	void QueueWorld(CCSPlayerController* victim, int hitgroup, int dmg, bool kill, bool head) {
		if (!victim) return;

		C_CSPlayerPawn* pawn = nullptr;
		__try {
			CBaseHandle hp = victim->m_hPlayerPawn();
			if (hp.valid() && I::GameEntity && I::GameEntity->Instance)
				pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hp);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}

		const std::uint32_t ph = PawnHandleFromController(victim);

		// Kill shots: resolve NOW using the still-alive pawn. Deferring to
		// DrainPendingWorld means the next-frame pawn is ragdolling / dying,
		// so the hitbox capsule fallback lands at the wrong world position.
		// Accept any resolution tier here (impact -> ray -> hitbox capsule).
		if (kill) {
			Vector_t anchor{};
			if (pawn && ResolveVictimPos(pawn, hitgroup, anchor, /*exactOnly=*/false)) {
				EmitWorldMark(anchor, kill, head, dmg);
				NoteVictimMarked(ph);
			}
			return;
		}

		// Non-kill: prefer exact points, defer if unavailable.
		Vector_t anchor{};
		if (pawn && ResolveVictimPos(pawn, hitgroup, anchor, true)) {
			EmitWorldMark(anchor, kill, head, dmg);
			NoteVictimMarked(ph);
			return;
		}

		if (!ph)
			return;

		PendingWorld& p = g_pending[g_pendingWrite];
		g_pendingWrite = (g_pendingWrite + 1) % kMaxPending;
		p.active = true;
		p.pawnHandle = ph;
		p.hitgroup = hitgroup;
		p.damage = dmg;
		p.kill = kill;
		p.head = head;
		p.tries = 0;
		p.queuedAt = Now();
	}

	void DrainPendingWorld() {
		if (!Config::hitmarker && !Config::float_damage)
			return;

		for (int i = 0; i < kMaxPending; ++i) {
			PendingWorld& p = g_pending[i];
			if (!p.active) continue;

			Vector_t anchor{};
			C_CSPlayerPawn* pawn = PawnFromHandleRaw(p.pawnHandle);
			bool exactOnly = (Now() - p.queuedAt < 0.15f);
			if (pawn && ResolveVictimPos(pawn, p.hitgroup, anchor, exactOnly)) {
				EmitWorldMark(anchor, p.kill, p.head, p.damage);
				NoteVictimMarked(p.pawnHandle);
				p.active = false;
				continue;
			}

			if (Now() - p.queuedAt < 1.0f) {
				continue;
			}

			p.active = false;
		}
	}

	ImU32 ColorFor(bool kill, bool head, float alpha) {
		ImVec4 c = Config::hitmarker_color;
		if (kill)
			c = Config::hitmarker_kill_color;
		else if (head)
			c = Config::hitmarker_head_color;
		c.w *= alpha;
		return ImGui::ColorConvertFloat4ToU32(c);
	}

	// Hit markers: 4 diagonal arms, alpha gradient
	// 0 at the outer tip -> full toward the center, spawn expand-in (quartic
	// ease-out, ~50 ms), ease-out fade (1-progress?). `t` = age seconds.
	void DrawVelArms(ImDrawList* dl, ImVec2 c, float t, float life, float sizeMul, ImU32 col, float thickness) {
		if (!dl || sizeMul < 1.f) return;
		const float progress = life > 0.f ? std::clamp(t / life, 0.f, 1.f) : 1.f;
		const float easeOut = 1.f - progress * progress;

		const float expandProgress = (std::min)(t * 20.f, 1.f);
		const float easeExpand = 1.f - std::pow(1.f - expandProgress, 4.f);
		const float expandAmount = 6.f * (1.f - easeExpand);

		const float scale = sizeMul / 10.f;
		const float arm = (10.f + expandAmount) * scale;
		const float gap = (3.f + expandAmount * 0.3f) * scale;
		if (arm < 1.f) return;

		const float g = gap * 0.70710678f;
		const float a = arm * 0.70710678f;
		const int colA = (int)((col >> 24) & 0xFF);

		auto armSeg = [&](ImVec2 dir) {
			const ImVec2 pIn(c.x + dir.x * g, c.y + dir.y * g);
			const ImVec2 pOut(c.x + dir.x * a, c.y + dir.y * a);
			// tip (t=1) alpha 0 -> center (t=0) alpha colA*easeOut
			constexpr int kSeg = 6;
			for (int i = 0; i < kSeg; ++i) {
				const float t0 = static_cast<float>(i) / kSeg;
				const float t1 = static_cast<float>(i + 1) / kSeg;
				const float a0 = 1.f - t0;
				const float a1 = 1.f - t1;
				const ImU32 c0 = (col & 0x00FFFFFFu)
					| (static_cast<std::uint32_t>(colA * easeOut * a0) << 24);
				const ImU32 c1 = (col & 0x00FFFFFFu)
					| (static_cast<std::uint32_t>(colA * easeOut * a1) << 24);
				const ImVec2 q0(pIn.x + (pOut.x - pIn.x) * t0, pIn.y + (pOut.y - pIn.y) * t0);
				const ImVec2 q1(pIn.x + (pOut.x - pIn.x) * t1, pIn.y + (pOut.y - pIn.y) * t1);
				dl->AddLine(q0, q1, c0, thickness);
				(void)c1;
			}
		};

		armSeg(ImVec2(1.f, 1.f));
		armSeg(ImVec2(-1.f, -1.f));
		armSeg(ImVec2(1.f, -1.f));
		armSeg(ImVec2(-1.f, 1.f));
	}

	// Damage label: above the marker (20 px), sin/cos shake that dies
	// within ~125 ms, fades with the marker.
	void DrawDamageLabel(ImDrawList* dl, ImVec2 c, float t, int dmg, ImU32 col, float easeOut) {
		if (!dl || dmg <= 0) return;
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", dmg);
		const ImVec2 ts = ImGui::CalcTextSize(buf);
		const float shakeProg = (std::max)(0.f, 1.f - t * 8.f);
		const float sx = std::sin(t * 50.f) * shakeProg * 3.f;
		const float sy = std::cos(t * 45.f) * shakeProg * 2.f;
		const ImVec2 tp(c.x - ts.x * 0.5f + sx, c.y - 20.f - ts.y + sy);
		const int a = (int)(((col >> 24) & 0xFF) * easeOut);
		const ImU32 shadow = IM_COL32(0, 0, 0, (int)(200.f * easeOut));
		dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), shadow, buf);
		dl->AddText(tp, col, buf);
		(void)a;
	}

	void DrawScreenMarker(ImDrawList* dl) {
		if (!Config::hitmarker || !Config::hitmarker_screen) return;
		if (g_screenBorn < 0.f) return;

		const float t = Now() - g_screenBorn;
		if (t >= g_screenLife) {
			g_screenBorn = -1.f;
			return;
		}

		const ImGuiIO& io = ImGui::GetIO();
		const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		const ImU32 col = ColorFor(g_screenKill, g_screenHead, 1.f);
		DrawVelArms(dl, c, t, g_screenLife, Config::hitmarker_size, col, 1.25f);

		if (Config::hitmarker_show_damage && g_screenDmg > 0)
			DrawDamageLabel(dl, c, t, g_screenDmg, col, 1.f - (t / g_screenLife) * (t / g_screenLife));
	}

	void DrawWorldMarkers(ImDrawList* dl, const ViewMatrix& vm) {
		if (!Config::hitmarker || !Config::hitmarker_world) return;
		if (!dl) return;

		const float now = Now();
		for (int i = 0; i < kMaxMarks; ++i) {
			Mark& m = g_marks[i];
			if (!m.active || !m.world) continue;
			const float t = now - m.born;
			if (t >= m.life) {
				m.active = false;
				continue;
			}

			Vector_t scr{};
			if (!vm.WorldToScreen(m.worldPos, scr)) continue;

			const ImVec2 c(scr.x, scr.y);
			const ImU32 col = ColorFor(m.kill, m.head, 1.f);
			DrawVelArms(dl, c, t, m.life, m.size, col, 1.25f);

			if (Config::hitmarker_show_damage && m.damage > 0) {
				const float easeOut = 1.f - (t / m.life) * (t / m.life);
				DrawDamageLabel(dl, c, t, m.damage, col, easeOut);
			}
		}
	}

	void DrawFloatingDamage(ImDrawList* dl, const ViewMatrix& vm) {
		if (!Config::float_damage || !dl) return;
		const float now = Now();
		const float speed = std::clamp(Config::float_damage_speed, 10.f, 200.f);
		for (int i = 0; i < kMaxFloats; ++i) {
			FloatDmg& f = g_floats[i];
			if (!f.active) continue;
			const float t = now - f.born;
			if (t >= f.life) {
				f.active = false;
				continue;
			}
			Vector_t scr{};
			if (!vm.WorldToScreen(f.worldPos, scr)) continue;
			const float u = t / f.life;
			const float fade = (u > 0.55f) ? (1.f - (u - 0.55f) / 0.45f) : 1.f;
			const float alpha = std::clamp(fade, 0.f, 1.f);
			
			// Bouncing physics with gravity
			const float speedUp = speed * 1.8f;
			float zOffset = speedUp * t - 0.5f * 600.f * t * t; // v0*t - 0.5*g*t^2
			if (zOffset < 0.f && t > 0.1f) {
				// bounce once
				const float bounceTime = t - (speedUp * 2.f / 600.f); // time since first bounce
				zOffset = (std::max)(0.f, (speedUp * 0.4f) * bounceTime - 0.5f * 600.f * bounceTime * bounceTime);
			}
			
			const float rise = zOffset;
			const float pop = std::sin((std::min)(u * 12.f, 3.14159f)) * 0.5f;
			const float scale = 1.f + pop;

			ImVec4 cv = Config::float_damage_color;
			if (f.kill) cv = Config::float_damage_kill_color;
			else if (f.head) cv = Config::float_damage_head_color;
			cv.w *= alpha;
			const ImU32 col = ImGui::ColorConvertFloat4ToU32(cv);

			char buf[16];
			snprintf(buf, sizeof(buf), "%d", f.damage);
			ImFont* font = ImGui::GetFont();
			const float fs = ImGui::GetFontSize() * scale;
			const ImVec2 ts = font ? font->CalcTextSizeA(fs, FLT_MAX, 0.f, buf) : ImGui::CalcTextSize(buf);
			const ImVec2 tp(scr.x - ts.x * 0.5f, scr.y - rise - ts.y * 0.5f);
			const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(210.f * alpha));
			if (font) {
				dl->AddText(font, fs, ImVec2(tp.x + 1.f, tp.y + 1.f), shadow, buf);
				dl->AddText(font, fs, tp, col, buf);
			} else {
				dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), shadow, buf);
				dl->AddText(tp, col, buf);
			}
		}
	}

	void HandleHurt(IGameEvent* ev) {
		CCSPlayerController* localCtrl = LocalController();
		if (!localCtrl) return;

		CCSPlayerController* attacker = ev->GetPlayerController("attacker");
		if (!attacker || attacker != localCtrl) return;

		CCSPlayerController* victim = ev->GetPlayerController("userid");
		if (victim && victim == localCtrl) return;

		const int dmg = static_cast<int>(ev->GetInt64("dmg_health"));
		const int hitgroup = static_cast<int>(ev->GetInt64("hitgroup"));
		const int health = static_cast<int>(ev->GetInt64("health"));
		if (dmg <= 0) return;

		const bool head = (hitgroup == 1);
		const bool kill = (health <= 0);

		if (Config::hitsound)
			Hitsound::Play(head, kill);

		if (Config::hitlog) {
			char nm[64]{};
			if (victim) {
				__try { victim->ReadSanitizedName(nm, sizeof(nm)); }
				__except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = 0; }
			}
			HitLog::Push(nm[0] ? nm : "player", dmg, hitgroup, head, kill, health);
		}

		if (Config::hitmarker && Config::hitmarker_screen)
			PulseScreen(kill, head, dmg);

		if (victim && ((Config::hitmarker && Config::hitmarker_world) || Config::float_damage))
			QueueWorld(victim, hitgroup, dmg, kill, head);
	}

	void HandleDeath(IGameEvent* ev) {
		CCSPlayerController* localCtrl = LocalController();
		if (!localCtrl) return;

		CCSPlayerController* attacker = ev->GetPlayerController("attacker");
		if (!attacker || attacker != localCtrl) return;

		CCSPlayerController* victim = ev->GetPlayerController("userid");
		if (!victim || victim == localCtrl) return;

		// player_death has "headshot" (bool), not "hitgroup" - reading hitgroup
		// yields 0, so head-slot resolution collapsed to spine on kills.
		const bool head = ev->GetInt64("headshot", 0) != 0;
		const int hitgroup = head ? 1 : 0;

		// If player_hurt(kill=true) already fired for this victim (same shot),
		// its resolution used the still-alive pawn and is more accurate. Skip
		// the death-time re-resolve which would run on a ragdolling pawn.
		const std::uint32_t vh = PawnHandleFromController(victim);
		const bool alreadyMarked = WasVictimMarked(vh, 0.5f);

		if (Config::hitmarker && Config::hitmarker_screen && !alreadyMarked)
			PulseScreen(true, head, g_screenDmg > 0 ? g_screenDmg : 0);

		if (alreadyMarked) return;

		if ((Config::hitmarker && Config::hitmarker_world) || Config::float_damage)
			QueueWorld(victim, hitgroup, g_screenDmg, true, head);
	}

} // namespace

void Hitmarker::NoteLastFire(const Vector_t& eye, const QAngle_t& fireAngles) {
	BulletFx::NoteLastFire(eye, fireAngles);
}

void Hitmarker::Install() {
	IGameEvent::InitPatterns();
	g_screenBorn = -1.f;
	g_write = 0;
	g_floatWrite = 0;
	g_pendingWrite = 0;
	g_recentMarkWrite = 0;
	for (int i = 0; i < kMaxMarks; ++i) g_marks[i] = Mark{};
	for (int i = 0; i < kMaxFloats; ++i) g_floats[i] = FloatDmg{};
	for (int i = 0; i < kMaxPending; ++i) g_pending[i] = PendingWorld{};
	for (int i = 0; i < kMaxRecentMarks; ++i) g_recentMarks[i] = RecentVictimMark{};
}

uintptr_t Hitmarker::ReportHitAddr() {
	return 0;
}

void Hitmarker::Shutdown() {
	g_screenBorn = -1.f;
	for (int i = 0; i < kMaxMarks; ++i) g_marks[i].active = false;
	for (int i = 0; i < kMaxFloats; ++i) g_floats[i].active = false;
	for (int i = 0; i < kMaxPending; ++i) g_pending[i].active = false;
	for (int i = 0; i < kMaxRecentMarks; ++i) g_recentMarks[i].pawnHandle = 0;
}

void Hitmarker::OnGameEvent(void* gameEvent) {
	if (!gameEvent) return;
	if (!Config::hitmarker && !Config::hitsound && !Config::float_damage
		&& !Config::hitlog)
		return;

	IGameEvent* ev = static_cast<IGameEvent*>(gameEvent);
	const char* name = nullptr;
	__try {
		name = ev->GetName();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (!name || !name[0]) return;

	__try {
		if (std::strcmp(name, "player_hurt") == 0)
			HandleHurt(ev);
		else if (std::strcmp(name, "player_death") == 0)
			HandleDeath(ev);
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Hitmarker::Draw(const ViewMatrix& vm) {
	if (!Config::hitmarker && !Config::float_damage)
		return;
	DrainPendingWorld();
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl) return;
	if (Config::hitmarker) {
		DrawWorldMarkers(dl, vm);
		DrawScreenMarker(dl);
	}
	DrawFloatingDamage(dl, vm);
}

void Hitmarker::DrawPreview(ImDrawList* dl, ImVec2 boxMin, ImVec2 boxMax, int mode) {
	if (!dl) return;
	if (boxMax.x <= boxMin.x || boxMax.y <= boxMin.y) return;

	const bool kill = (mode == 2);
	const bool head = (mode == 1);
	const int sampleDmg = kill ? 100 : (head ? 110 : 27);

	const float life = kScreenLife * std::clamp(Config::hitmarker_duration, 0.25f, 2.5f);
	const float t = std::fmod(Now(), life);
	const float u = (life > 1e-4f) ? (t / life) : 0.f;
	const float pop = (u < 0.12f) ? (u / 0.12f) : 1.f;
	const float fade = (u > 0.55f) ? (1.f - (u - 0.55f) / 0.45f) : 1.f;
	const float alpha = std::clamp(fade, 0.f, 1.f) * Config::hitmarker_color.w;
	const float scale = 0.75f + 0.45f * pop * (1.f - u * 0.35f);
	const float thick = Config::hitmarker_thickness;

	dl->PushClipRect(boxMin, boxMax, true);

	const float midY = (boxMin.y + boxMax.y) * 0.5f;
	const bool showWorld = Config::hitmarker_world;
	const bool showScreen = Config::hitmarker_screen || !showWorld;

	auto softGuide = [&](ImVec2 c) {
		dl->AddLine(ImVec2(c.x - 16.f, c.y), ImVec2(c.x + 16.f, c.y), IM_COL32(255, 255, 255, 18), 1.f);
		dl->AddLine(ImVec2(c.x, c.y - 16.f), ImVec2(c.x, c.y + 16.f), IM_COL32(255, 255, 255, 18), 1.f);
	};

	if (showScreen && showWorld) {
		const float midX = (boxMin.x + boxMax.x) * 0.5f;
		dl->AddLine(ImVec2(midX, boxMin.y + 8.f), ImVec2(midX, boxMax.y - 8.f),
			IM_COL32(255, 255, 255, 28), 1.f);

		const ImVec2 cScr((boxMin.x + midX) * 0.5f, midY);
		softGuide(cScr);
		const ImU32 colS = ColorFor(kill, head, 1.f);
		DrawVelArms(dl, cScr, t, life, Config::hitmarker_size, colS, thick);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, cScr, t, sampleDmg, colS, 1.f - u * u);

		const ImVec2 cW((midX + boxMax.x) * 0.5f, midY);
		softGuide(cW);
		const ImU32 colW = ColorFor(kill, head, 1.f);
		DrawVelArms(dl, cW, t, life, Config::hitmarker_world_size, colW, thick);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, cW, t, sampleDmg, colW, 1.f - u * u);

		dl->AddText(ImVec2(boxMin.x + 6.f, boxMin.y + 4.f), IM_COL32(180, 180, 190, 140), "Screen");
		dl->AddText(ImVec2(midX + 6.f, boxMin.y + 4.f), IM_COL32(180, 180, 190, 140), "World");
	} else if (showWorld) {
		const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, midY);
		softGuide(c);
		const ImU32 colW = ColorFor(kill, head, 1.f);
		DrawVelArms(dl, c, t, life, Config::hitmarker_world_size, colW, thick);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, c, t, sampleDmg, colW, 1.f - u * u);
	} else {
		const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, midY);
		softGuide(c);
		const ImU32 col = ColorFor(kill, head, 1.f);
		DrawVelArms(dl, c, t, life, Config::hitmarker_size, col, thick);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, c, t, sampleDmg, col, 1.f - u * u);
	}

	dl->PopClipRect();
}

