#include "grenade_helper.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../bones/bones.h"
#include "../gamemode/gamemode.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../offsets/offsets.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/datatypes/schema/ISchemaClass/ISchemaClass.h"
#include "../../../../external/imgui/imgui.h"
#include "../../../../external/json/json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>

#include "../../utils/console/console.h"
#include "../../keybinds/keybinds.h"
#include "../w2s/w2s.h"
#include "../../menu/menu.h"
#include "../visuals/assets/weapon_icons.hpp"
#include "../visuals/weapon_icon_draw.h"
#include "../notify/notify.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../aim/aim_common.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"

#include <cfloat>

extern ID3D11Device* pDevice;

namespace GrenadeHelper {
namespace {

constexpr float kAimDrawDist = 350.f;   // world aim ball locked to stand+saved angles
constexpr
float kCircleRadius = 14.f;   // visual stand pad
constexpr float kAimExactR = 4.5f;      // feet lock for full aim reticle
constexpr float kAimPreviewR = 12.f;    // on-pad preview (dim) before exact lock
constexpr float kEyeLift = 64.f;
constexpr float kUnitsToM = 0.0254f;    // Source units -> meters
constexpr float kWalkSpeedMin = 80.f;   // CS2 shift-walk ~100; 25 tagged idle strafe as Walk
constexpr float kRunSpeedMin = 135.f;
constexpr ULONGLONG kThrowCommitGraceMs = 200; // same as nade_pred: wait for ThrowTime/clip

std::
vector<Lineup> g_lineups;
// Index into g_lineups - never store raw Lineup* (vector realloc invalidates pointers)
int g_currentIdx = -1;
bool g_loaded = false;
bool g_captureKeyWasDown = false;
char g_mapCache[128] = {};
// Indices of lineups for current map (avoids scanning all maps every frame)
std::
vector<int> g_mapIndices;
char g_mapIndicesKey[128] = {};

// Armed capture: freeze stand feet on arm; refresh aim each frame until throw.
// Motion: recency timestamps - whole-session sticky false-tagged stand after
// run-to-spot / early hop. Jumpthrow still needs ~500ms window (bind releases space).

// Multi-input capture keys - logged as an ordered event timeline.
enum CapKey : std::uint8_t {
	CapKey_Fwd = 0, CapKey_Back, CapKey_Left, CapKey_Right,
	CapKey_Jump, CapKey_Duck, CapKey_Walk,
	CapKey_Count
};
struct CapInputEvent {
	ULONGLONG ms = 0;      // relative to armMs - keeps the throw-order visible
	std::uint8_t key = 0;  // CapKey_*
	bool down = false;
};
constexpr int kMaxCapEvents = 96;

struct CaptureSession {
	bool armed = false;
	Vector_t pos{};           // stand feet (arm snapshot)
	QAngle_t ang{};           // live aim - updated every armed frame
	NadeKind kind = NadeKind::Any;
	char name[64]{};
	ULONGLONG armMs = 0;
	bool attackHeld = false;
	bool sawAttack = false;      // M1/M2 while holding a grenade (not menu click)
	ULONGLONG lastJumpMs = 0;
	ULONGLONG lastRunMs = 0;
	ULONGLONG lastWalkMs = 0;
	ULONGLONG lastDuckMs = 0;
	ULONGLONG lastLeftMs = 0;
	ULONGLONG lastRightMs = 0;
	ULONGLONG lastBackMs = 0;
	ULONGLONG attackDownMs = 0;  // when real attack press started (0 = none)
	float eyeLift = kEyeLift;    // view offset Z at last stationary snapshot
	C_CSWeaponBase* cookWep = nullptr;
	int cookClip = -1;
	float cookThrowBaseline = 0.f;
	bool throwCommitted = false;
	ULONGLONG pinReleaseMs = 0;

	// -- Multi-input capture: ordered key-event log for the throw recipe --
	// Records the whole armed session (pre-pin run-ups included) so sequences
	// like "hold A -> jump -> release" survive into the saved lineup.
	CapInputEvent events[kMaxCapEvents]{};
	int eventN = 0;
	bool prevDown[CapKey_Count]{};
};
CaptureSession g_cap{};
constexpr ULONGLONG kCaptureTimeoutMs = 20000;
// How long before release a jump/run/duck still counts (jumpthrow bind lag).
constexpr ULONGLONG kMotionRecentMs = 550;
// Ignore residual LMB from "Arm Capture" button / F6 chord.
constexpr ULONGLONG kArmIgnoreAtkMs = 280;
// Click vs pin-hold: need hold this long before release counts as throw.
constexpr ULONGLONG kMinAtkHoldMs = 70;

void InvalidateMapIndex() {
	g_mapIndices.clear();
	g_mapIndicesKey[0] = '\0';
}

std::
filesystem::
path LineupFolder() {
	std::
filesystem::
path folder;
	char* userProfile = nullptr;
	size_t len = 0;
	if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile && len > 0) {
		folder = userProfile;
		free(userProfile);
		folder /= "Documents";
		folder /= "Games8Th";
		folder /= "GrenadeHelpers";
	} else {
		folder = "Games8Th";
		folder /= "GrenadeHelpers";
	}
	std::
error_code ec;
	std::
filesystem::
create_directories(folder, ec);
	return folder;
}

// Legacy paths (NadeLineups pre-rename, Nade.ineups older typo) - migrate so old captures still load.
std::
filesystem::
path LegacyLineupFolder() {
	char* userProfile = nullptr;
	size_t len = 0;
	const bool hasProfile = _dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile && len > 0;
	std::error_code ec;
	for (const char* legacy : { "NadeLineups", "Nade.ineups" }) {
		std::
filesystem::
path folder;
		if (hasProfile) {
			folder = userProfile;
			folder /= "Documents";
			folder /= "Games8Th";
			folder /= legacy;
		} else {
			folder = "Games8Th";
			folder /= legacy;
		}
		if (std::
filesystem::
exists(folder / "lineups.json", ec))
			return folder;
	}
	if (hasProfile)
		free(userProfile);
	return {};
}

std::
filesystem::
path LineupFilePath() {
	return LineupFolder() / "lineups.json";
}

std::
filesystem::
path LegacyLineupFilePath() {
	return LegacyLineupFolder() / "lineups.json";
}

void AngleVectors(const QAngle_t& ang, Vector_t& forward) {
	const float pitch = ang.x * (3.14159265f / 180.f);
	const float yaw = ang.y * (3.14159265f / 180.f);
	const float cp = std::
cos(pitch);
	const float sp = std::
sin(pitch);
	const float cy = std::
cos(yaw);
	const float sy = std::
sin(yaw);
	forward = { cp * cy, cp * sy, -sp };
}

// Aim marker is ALWAYS stand feet + eye lift + saved aim angles * dist.
// Never current camera / current eye - so it stays where you captured it.
float EyeLiftFromAng(const QAngle_t& ang) {
	// ang.z stores captured view-offset Z (crouch ~46, stand ~64). Roll is unused.
	return (ang.z >= 32.f && ang.z <= 80.f) ? ang.z : kEyeLift;
}

float StandEyeLift(C_CSPlayerPawn* local) {
	if (!local)
		return kEyeLift;
	static uint32_t s_off = 0;
	if (!s_off)
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseModelEntity->m_vecViewOffset"));
	if (s_off) {
		Vector_t view{};
		if (Mem::ReadField(local, s_off, view) && std::isfinite(view.z)
			&& view.z >= 32.f && view.z <= 80.f)
			return view.z;
	}
	return kEyeLift;
}

Vector_t AimWorldFromStand(const Vector_t& stand, const QAngle_t& ang, float dist) {
	Vector_t fwd{};
	AngleVectors(ang, fwd);
	const float lift = EyeLiftFromAng(ang);
	return {
		stand.x + fwd.x * dist,
		stand.y + fwd.y * dist,
		stand.z + lift + fwd.z * dist
	};
}

bool IsFiniteVec(const Vector_t& v) {
	return std::
isfinite(v.x) && std::
isfinite(v.y) && std::
isfinite(v.z);
}

bool IsFiniteAng(const QAngle_t& a) {
	return std::
isfinite(a.x) && std::
isfinite(a.y);
}

bool GetLocalViewAngles(QAngle_t& out) {
	// Prefer combat path (stable / punch-free). Input ptr alone fails mid-menu / leave.
	QAngle_t a{};
	if (AimCommon::
GetViewAngles(a) && std::
isfinite(a.x) && std::
isfinite(a.y)) {
		out = { a.x, a.y, 0.f };
		return true;
	}
	if (!Input::
GetViewAngles || !Input::
viewAngleContext)
		return false;
	const uintptr_t viewPtr = Input::
GetViewAngles(Input::
viewAngleContext, 0);
	if (!viewPtr || !Mem::
IsReadable(reinterpret_cast<void*>(viewPtr), sizeof(Vector_t)))
		return false;
	Vector_t v{};
	__try {
		v = *reinterpret_cast<const Vector_t*>(viewPtr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!std::
isfinite(v.x) || !std::
isfinite(v.y))
		return false;
	out = { v.x, v.y, 0.f };
	return true;
}

const char* SehClassName(CEntityInstance* ent) {
	// ponytail: static not thread_local - manual-map TLS not initialized, Present thread only
	static char buf[128];
	buf[0] = '\0';
	if (!Mem::SchemaClassName(ent, buf, sizeof(buf)) || !buf[0])
		return nullptr;
	return buf;
}

const char* SehDesignerName(CEntityInstance* ent) {
	if (!ent || !Mem::
ValidEntity(ent))
		return nullptr;
	CEntityIdentity* id = nullptr;
	if (!Mem::
ReadField(ent, Offset::m_pEntity(), id) || !id || !Mem::
Valid(id, 0x28)) {
		__try {
			id = ent->m_pEntityIdentity();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	if (!id || !Mem::
Valid(id, 0x28))
		return nullptr;
	const char* p = nullptr;
	if (!Mem::
ReadField(id, Offset::m_designerName(), p) || !p) {
		__try {
			p = id->m_designerName();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	if (!p || !Mem::
IsReadable(p, 2) || !p[0])
		return nullptr;
	return p;
}

NadeKind ClassifyHeld(C_CSWeaponBase* wep) {
	if (!wep)
		return NadeKind::
Any;
	const char* a = SehClassName(reinterpret_cast<CEntityInstance*>(wep));
	const char* b = SehDesignerName(reinterpret_cast<CEntityInstance*>(wep));
	a = a ? a : "";
	b = b ? b : "";
	if (std::
strstr(a, "SmokeGrenade") || std::
strstr(b, "smokegrenade"))
		return NadeKind::
Smoke;
	if (std::
strstr(a, "Molotov") || std::
strstr(a, "Incendiary")
		|| std::
strstr(b, "molotov") || std::
strstr(b, "incgrenade") || std::
strstr(b, "incendiary"))
		return NadeKind::
Molly;
	if (std::
strstr(a, "HEGrenade") || std::
strstr(b, "hegrenade"))
		return NadeKind::
HE;
	if (std::
strstr(a, "Flashbang") || std::
strstr(b, "flashbang"))
		return NadeKind::
Flash;
	if (std::
strstr(a, "Decoy") || std::
strstr(b, "decoy"))
		return NadeKind::
Decoy;
	return NadeKind::
Any;
}

static std::string CleanMapName(const char* in) {
	if (!in || !in[0])
		return "";

	// Find the last path component
	const char* base = in;
	for (const char* p = in; *p; ++p) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}

	std::string s = base;
	// Strip extensions
	if (s.size() >= 4 && _stricmp(s.c_str() + s.size() - 4, ".vpk") == 0)
		s.resize(s.size() - 4);
	else if (s.size() >= 4 && _stricmp(s.c_str() + s.size() - 4, ".bsp") == 0)
		s.resize(s.size() - 4);

	// To lowercase
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	// Canonical mapping for standard maps (so dust_2 / dust / dust2 all map to dust2)
	if (s == "dust" || s == "dust2" || s == "dust_2" || s == "de_dust2" || s == "de_dust") return "dust2";
	if (s == "mirage" || s == "de_mirage") return "mirage";
	if (s == "inferno" || s == "de_inferno") return "inferno";
	if (s == "overpass" || s == "de_overpass") return "overpass";
	if (s == "nuke" || s == "de_nuke") return "nuke";
	if (s == "vertigo" || s == "de_vertigo") return "vertigo";
	if (s == "ancient" || s == "de_ancient") return "ancient";
	if (s == "anubis" || s == "de_anubis") return "anubis";
	if (s == "train" || s == "de_train") return "train";
	if (s == "cache" || s == "de_cache") return "cache";
	if (s == "office" || s == "cs_office") return "office";
	if (s == "italy" || s == "cs_italy") return "italy";

	// Strip common prefixes
	static const char* kPrefixes[] = {
		"de_", "cs_", "ar_", "aim_", "awp_", "surf_", "kz_", "mg_", "bhop_", "fy_", "dz_", "gd_"
	};
	for (const char* pref : kPrefixes) {
		const size_t prefLen = std::strlen(pref);
		if (s.size() > prefLen && s.rfind(pref, 0) == 0) {
			s = s.substr(prefLen);
			break;
		}
	}

	return s;
}

bool MapsEqual(const std::string& a, const char* b) {
	if (!b || !b[0])
		return a.empty();
	if (a.empty())
		return false;
	// Exact raw match first
	if (_stricmp(a.c_str(), b) == 0)
		return true;
	const std::string ca = CleanMapName(a.c_str());
	const std::string cb = CleanMapName(b);
	if (ca.empty() || cb.empty())
		return false;
	if (ca == cb)
		return true;
	// Substring match for workshop paths (e.g. workshop/3070244462/de_mirage contains mirage)
	if (ca.find(cb) != std::string::npos || cb.find(ca) != std::string::npos)
		return true;
	return false;
}

void RebuildMapIndex() {
	g_mapIndices.clear();
	if (!g_mapCache[0]) {
		g_mapIndicesKey[0] = '\0';
		return;
	}
	std::
snprintf(g_mapIndicesKey, sizeof(g_mapIndicesKey), "%s", g_mapCache);
	g_mapIndices.reserve(64);
	for (int i = 0; i < static_cast<int>(g_lineups.size()); ++i) {
		if (!g_lineups[i].map.empty() && MapsEqual(g_lineups[i].map, g_mapCache))
			g_mapIndices.push_back(i);
	}
}

void EnsureMapIndex() {
	if (!g_mapCache[0]) {
		InvalidateMapIndex();
		return;
	}
	if (g_mapIndicesKey[0] && _stricmp(g_mapIndicesKey, g_mapCache) == 0)
		return;
	RebuildMapIndex();
}

bool KindMatches(NadeKind want, NadeKind held) {
	if (want == NadeKind::
Any)
		return true;
	if (held == NadeKind::
Any)
		return !Config::
grenade_helper_only_held;
	return want == held;
}

ImU32 ColA(const ImVec4& c, float a) {
	const int aa = static_cast<int>(std::
clamp(c.w * a * 255.f, 0.f, 255.f));
	return IM_COL32(
		static_cast<int>(c.x * 255.f),
		static_cast<int>(c.y * 255.f),
		static_cast<int>(c.z * 255.f),
		aa);
}

ImU32 ColRGB(int r, int g, int b, float a) {
	return IM_COL32(r, g, b, static_cast<int>(std::
clamp(a * 255.f, 0.f, 255.f)));
}

bool WorldToScreen2(const ViewMatrix& vm, const Vector_t& w, ImVec2& out) {
	Vector_t s{};
	if (!vm.WorldToScreen(w, s))
		return false;
	out = ImVec2{ s.x, s.y };
	return true;
}

// Project to screen, or screen-edge when off-frustum / behind (view-space, no flip).
bool ProjectOrEdge(const ViewMatrix& vm, const Vector_t& world, ImVec2& out, bool& onScreen,
	const Vector_t& eye, const QAngle_t& viewAng)
{
	(void)
vm;
	float ox = 0.f, oy = 0.f;
	if (!W2S::
ProjectOrEdge(world, ox, oy, onScreen, 28.f, eye, viewAng))
		return false;
	out = ImVec2{ ox, oy };
	return true;
}

// Clean glossy HUD - dark glass lit from above, soft depth, crisp hairlines.
namespace Ui {
	constexpr float kR     = 8.f;   // pill / chip radius
	constexpr float kRsm   = 6.f;   // small chip radius
	const    ImU32 kGlass  = IM_COL32(17, 19, 24, 224);   // glass body
	constexpr ImU32 kSheen = IM_COL32(255, 255, 255, 14);  // broad upper light
	constexpr ImU32 kGloss = IM_COL32(255, 255, 255, 26);  // tight gloss streak
	constexpr ImU32 kEdge  = IM_COL32(255, 255, 255, 40);  // lit top edge
	constexpr ImU32 kInk   = IM_COL32(0, 0, 0, 120);
	constexpr ImU32 kHair  = IM_COL32(255, 255, 255, 20);
	constexpr ImU32 kText  = IM_COL32(240, 242, 246, 255);
	constexpr ImU32 kMuted = IM_COL32(160, 164, 174, 255);
}

// Glass panel core: drop shadow -> body -> broad sheen -> tight gloss -> lit
// top edge -> hairline frame. Every pill/chip composes on this.
void GlossPanel(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, float r,
	ImU32 body, ImU32 border)
{
	const float h = mx.y - mn.y;
	dl->AddRectFilled(ImVec2(mn.x + 1.5f, mn.y + 2.f), ImVec2(mx.x + 1.5f, mx.y + 3.5f),
		IM_COL32(0, 0, 0, 55), r);
	dl->AddRectFilled(mn, mx, body, r);
	if (h > 6.f) {
		dl->AddRectFilled(ImVec2(mn.x + 1.f, mn.y + 1.f),
			ImVec2(mx.x - 1.f, mn.y + h * 0.52f), Ui::kSheen,
			r > 1.f ? r - 1.f : 1.f);
		dl->AddRectFilled(ImVec2(mn.x + 2.f, mn.y + 1.5f),
			ImVec2(mx.x - 2.f, mn.y + h * 0.34f), Ui::kGloss,
			r > 2.f ? (r - 1.f) * 0.7f : 1.f);
	}
	dl->AddLine(ImVec2(mn.x + r * 0.8f, mn.y + 0.75f),
		ImVec2(mx.x - r * 0.8f, mn.y + 0.75f), Ui::kEdge, 1.0f);
	dl->AddRect(mn, mx, border, r, 0, 1.0f);
}

// Soft filled pill with 1px hairline border - glossy build.
void DrawPill(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 fill, ImU32 border, float r)
{
	GlossPanel(dl, mn, mx, r, fill, border);
}

void DrawOffscreenHint(ImDrawList* dl, const ImVec2& edge, const ImVec2& center,
	float alpha, const ImVec4& col, const char* name, float dist)
{
	if (!dl || alpha < 0.05f)
		return;
	float dx = edge.x - center.x;
	float dy = edge.y - center.y;
	const float len = std::
sqrt(dx * dx + dy * dy);
	if (len > 0.001f) { dx /= len; dy /= len; }
	else { dx = 0.f; dy = -1.f; }

	const float px = -dy, py = dx;
	const float tip = 7.f;
	const float base = 4.5f;
	const ImVec2 t0{ edge.x + dx * tip, edge.y + dy * tip };
	const ImVec2 t1{ edge.x - dx * 1.4f + px * base, edge.y - dy * 1.4f + py * base };
	const ImVec2 t2{ edge.x - dx * 1.4f - px * base, edge.y - dy * 1.4f - py * base };
	dl->AddTriangleFilled(t0, t1, t2, ColA(col, alpha * 0.82f));
	dl->AddTriangle(t0, t1, t2, Ui::
kInk, 1.0f);

	char line[64]{};
	const float meters = dist * kUnitsToM;
	if (name && name[0])
		std::
snprintf(line, sizeof(line), "%s  ?  %.0fm", name, meters);
	else
		std::
snprintf(line, sizeof(line), "%.0fm", meters);
	const ImVec2 ts = ImGui::
CalcTextSize(line);
	const float padX = 8.f, padY = 3.5f;
	ImVec2 tp{ edge.x - dx * 18.f - ts.x * 0.5f, edge.y - dy * 18.f - ts.y * 0.5f };
	const ImVec2 mn{ floorf(tp.x - padX), floorf(tp.y - padY) };
	const ImVec2 mx{ mn.x + ts.x + padX * 2.f, mn.y + ts.y + padY * 2.f };

	GlossPanel(dl, mn, mx, 5.f, ColRGB(13, 15, 19, alpha * 0.92f),
		ColA(col, alpha * 0.38f));
	dl->AddText(ImVec2(mn.x + padX, mn.y + padY), ColRGB(250, 250, 252, alpha), line);
}

// Ground ring - soft double stroke when active, single hairline idle. Dot center.
bool DrawStandPad(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
	float radius, ImU32 ringCol, ImU32 fillCol, int segs = 32, bool active = false)
{
	if (!dl || !vm.viewMatrix || radius <= 1.f)
		return false;
	if (!std::
isfinite(center.x) || !std::
isfinite(center.y) || !std::
isfinite(center.z))
		return false;

	ImVec2 pts[64];
	int n = 0;
	const int maxSegs = (std::
min)(segs, 63);
	for (int i = 0; i <= maxSegs; ++i) {
		const float a = (static_cast<float>(i) / maxSegs) * 6.2831853f;
		const Vector_t w{
			center.x + std::
cos(a) * radius,
			center.y + std::
sin(a) * radius,
			center.z + 1.2f
		};
		ImVec2 s{};
		if (!WorldToScreen2(vm, w, s))
			continue;
		if (!std::
isfinite(s.x) || !std::
isfinite(s.y))
			continue;
		if (s.x < -4000.f || s.x > 8000.f || s.y < -4000.f || s.y > 8000.f)
			continue;
		if (n < 64)
			pts[n++] = s;
	}
	if (n < 3)
		return false;

	const bool fillOk = n >= (maxSegs * 3) / 4;
	if (fillOk && active && (fillCol >> IM_COL32_A_SHIFT) > 0)
		dl->AddConvexPolyFilled(pts, n, fillCol);

	const float thick = active ? 1.65f : 1.15f;

	// Active: soft accent glow under the ring strokes
	if (active) {
		for (int i = 0; i + 1 < n; ++i)
			dl->AddLine(pts[i], pts[i + 1], ringCol, thick + 3.4f);
		if (n >= 2)
			dl->AddLine(pts[n - 1], pts[0], ringCol, thick + 3.4f);
	}

	// Soft ink understroke, then ring
	for (int i = 0; i + 1 < n; ++i)
		dl->AddLine(pts[i], pts[i + 1], Ui::
kInk, thick + 1.0f);
	if (n >= 2)
		dl->AddLine(pts[n - 1], pts[0], Ui::
kInk, thick + 1.0f);
	for (int i = 0; i + 1 < n; ++i)
		dl->AddLine(pts[i], pts[i + 1], ringCol, thick);
	if (n >= 2)
		dl->AddLine(pts[n - 1], pts[0], ringCol, thick);

	// Active: soft outer glow pass under the ring (accent-tinted, no neon)
	if (active) {
		for (int i = 0; i + 1 < n; ++i)
			dl->AddLine(pts[i], pts[i + 1], ringCol, thick + 3.2f);
		if (n >= 2)
			dl->AddLine(pts[n - 1], pts[0], ringCol, thick + 3.2f);
	}

	ImVec2 mid{};
	for (int i = 0; i < n; ++i) {
		mid.x += pts[i].x;
		mid.y += pts[i].y;
	}
	const float inv = 1.f / static_cast<float>(n);
	mid.x *= inv;
	mid.y *= inv;

	// Glossy center: ink disc -> color core -> specular highlight (no cross tick)
	dl->AddCircleFilled(mid, active ? 2.6f : 2.0f, Ui::
kInk, 12);
	dl->AddCircleFilled(mid, active ? 1.5f : 1.15f, ringCol, 10);
	if (active)
		dl->AddCircleFilled(ImVec2(mid.x - 0.5f, mid.y - 0.55f), 0.6f,
			IM_COL32(255, 255, 255, 170), 8);

	return true;
}

void DrawFeetCross(ImDrawList* dl, const ImVec2& c, float size, ImU32 col, ImU32 outline) {
	(void)
outline;
	const float s = size > 1.f ? size : 2.2f;
	dl->AddCircleFilled(ImVec2(c.x + 0.8f, c.y + 1.f), s + 1.0f, IM_COL32(0, 0, 0, 60), 12);
	dl->AddCircleFilled(c, s + 1.0f, Ui::
kInk, 12);
	dl->AddCircleFilled(c, s, col, 12);
	dl->AddCircleFilled(ImVec2(c.x - s * 0.3f, c.y - s * 0.35f), s * 0.4f,
		IM_COL32(255, 255, 255, 150), 8);
}

static const char* KindWeaponKey(NadeKind k) {
	switch (k) {
	case NadeKind::
HE: return "hegrenade";
	case NadeKind::
Flash: return "flashbang";
	case NadeKind::
Smoke: return "smokegrenade";
	case NadeKind::
Molly: return "molotov";
	case NadeKind::
Decoy: return "decoy";
	default: return nullptr;
	}
}

// Circular kind badge - brighter disc + bright glyph (was too dark in-game)
void DrawKindIcon(ImDrawList* dl, const ImVec2& c, NadeKind kind, float alpha, float pxSize,
	const ImVec4& accent, bool locked = false)
{
	if (!dl || alpha < 0.05f || pxSize < 6.f)
		return;
	const char* key = KindWeaponKey(kind);
	if (!key)
		return;
	const float r = pxSize * 0.82f;
	const float a = std::
clamp(alpha, 0.f, 1.f);

	// Glossy badge: drop shadow -> glass body -> sheen -> rim
	dl->AddCircleFilled(ImVec2(c.x + 1.f, c.y + 1.4f), r, IM_COL32(0, 0, 0, (int)(a * 60.f)), 24);
	dl->AddCircleFilled(c, r + 1.2f, ColRGB(0, 0, 0, a * 0.55f), 24);
	dl->AddCircleFilled(c, r, ColRGB(38, 41, 48, a * 0.96f), 24);
	dl->AddCircleFilled(ImVec2(c.x - r * 0.26f, c.y - r * 0.34f), r * 0.55f,
		IM_COL32(255, 255, 255, (int)(a * 22.f)), 16);
	dl->AddCircle(c, r,
		locked ? ColA(accent, a * 0.95f) : ColRGB(255, 255, 255, a * 0.30f),
		24, locked ? 1.45f : 1.15f);

	const int ai = static_cast<int>(a * 255.f);
	// Undefeated glyph first, atlas fallback
	auto it = weapon_icons::
icon_table.find(key);
	if (it != weapon_icons::
icon_table.end() && !it->second.empty() && g_WeaponIconFont) {
		const char* glyph = it->second.c_str();
		const ImVec2 isz = g_WeaponIconFont->CalcTextSizeA(pxSize, FLT_MAX, 0.f, glyph);
		const float ix = floorf(c.x - isz.x * 0.5f);
		const float iy = floorf(c.y - isz.y * 0.55f);
		// Soft shadow + bright glyph
		dl->AddText(g_WeaponIconFont, pxSize, ImVec2(ix + 1.f, iy + 1.f), IM_COL32(0, 0, 0, (ai * 100) / 255), glyph);
		dl->AddText(g_WeaponIconFont, pxSize, ImVec2(ix, iy),
			locked ? IM_COL32(255, 255, 255, ai) : IM_COL32(248, 248, 250, ai), glyph);
		return;
	}
	WeaponIconDraw::EnsureReady(pDevice);
	if (WeaponIconDraw::Has(key)) {
		const float ih = pxSize * 1.05f;
		WeaponIconDraw::DrawCentered(dl, c.x, c.y - ih * 0.5f,
			locked ? IM_COL32(255, 255, 255, ai) : IM_COL32(248, 248, 250, ai), key, ih);
	}
}

// Aim marker - single soft DOT only (no crosshair arms; distinct from game crosshair)
void DrawAimReticle(ImDrawList* dl, const ImVec2& c, float a, const ImVec4& accent, bool locked,
	const char* throwLabel, const char* kindLabel)
{
	if (a < 0.04f)
		return;
	const float core = locked ? 3.4f : 2.4f;
	const ImU32 ring = ColA(accent, locked ? (a * 0.95f) : (a * 0.60f));

	// Glossy marker: soft glow under, ink under, accent ring, bright core, specular
	if (locked) {
		dl->AddCircleFilled(c, core + 4.2f, ColA(accent, a * 0.16f), 16);
	}
	dl->AddCircleFilled(c, core + 1.6f, Ui::
kInk, 16);
	dl->AddCircle(c, core + 0.6f, ring, 16, locked ? 1.5f : 1.15f);
	dl->AddCircleFilled(c, core * 0.55f, ColRGB(252, 252, 254, a), 12);
	dl->AddCircleFilled(ImVec2(c.x - core * 0.22f, c.y - core * 0.26f), core * 0.22f,
		IM_COL32(255, 255, 255, (int)(a * 150.f)), 8);

	char chip[48]{};
	if (throwLabel && throwLabel[0] && kindLabel && kindLabel[0])
		std::
snprintf(chip, sizeof(chip), "%s  ?  %s", throwLabel, kindLabel);
	else if (throwLabel && throwLabel[0])
		std::
snprintf(chip, sizeof(chip), "%s", throwLabel);
	else if (kindLabel && kindLabel[0])
		std::
snprintf(chip, sizeof(chip), "%s", kindLabel);
	if (!chip[0])
		return;

	const ImVec2 ts = ImGui::
CalcTextSize(chip);
	const float padX = 8.f, padY = 3.5f;
	const ImVec2 min{ floorf(c.x - ts.x * 0.5f - padX), floorf(c.y + core + 9.f) };
	const ImVec2 max{ min.x + ts.x + padX * 2.f, min.y + ts.y + padY * 2.f };
	
	const float bgA = locked ? (a * 0.92f) : (a * 0.70f);
	GlossPanel(dl, min, max, 5.f, ColRGB(13, 15, 19, bgA),
		ColA(accent, locked ? (a * 0.40f) : (a * 0.15f)));
	dl->AddText(ImVec2(min.x + padX, min.y + padY), locked ? ColA(accent, a * 0.95f) : ColRGB(200, 202, 210, a * 0.85f), chip);
}

struct LabelRow {
	const char* title;
	const char* meta;
	bool active;
	ImVec4 accent;
};

void DrawMultiLabelChip(ImDrawList* dl, const ImVec2& anchor, const LabelRow* rows, int count, float alpha)
{
	if (count <= 0 || alpha < 0.04f)
		return;

	float maxTitleW = 0.f;
	float maxMetaW = 0.f;
	float totalH = 0.f;
	const float padX = 8.f;
	const float padY = 4.f;
	const float gap = 2.f;
	const float rowGap = 4.f;
	const float barW = 3.f;
	const float barSpace = 4.f;
	bool anyActive = false;

	ImVec2 titleSzs[8];
	ImVec2 metaSzs[8];
	
	for (int i = 0; i < count; ++i) {
		titleSzs[i] = ImGui::
CalcTextSize(rows[i].title);
		metaSzs[i] = (rows[i].meta && rows[i].meta[0]) ? ImGui::
CalcTextSize(rows[i].meta) : ImVec2{ 0.f, 0.f };
		maxTitleW = (std::
max)(maxTitleW, titleSzs[i].x);
		maxMetaW = (std::
max)(maxMetaW, metaSzs[i].x);
		float rowH = titleSzs[i].y + (metaSzs[i].y > 0.f ? metaSzs[i].y + gap : 0.f);
		totalH += rowH;
		if (rows[i].active) anyActive = true;
	}
	totalH += padY * 2.f + rowGap * (count - 1);

	const float contentW = (std::
max)(maxTitleW, maxMetaW);
	const float totalW = padX + (anyActive ? (barW + barSpace) : 0.f) + contentW + padX;
	
	const ImVec2 min{ floorf(anchor.x - totalW * 0.5f), floorf(anchor.y - totalH - 16.f) };
	const ImVec2 max{ min.x + totalW, min.y + totalH };

	ImU32 borderCol = ColRGB(255, 255, 255, alpha * 0.10f);
	for (int i = 0; i < count; ++i) {
		if (rows[i].active) {
			borderCol = ColA(rows[i].accent, alpha * 0.38f);
			break;
		}
	}
	GlossPanel(dl, min, max, 6.f, ColRGB(13, 15, 19, alpha * 0.92f), borderCol);

	float curY = min.y + padY;
	for (int i = 0; i < count; ++i) {
		float tx = min.x + padX;
		float rowH = titleSzs[i].y + (metaSzs[i].y > 0.f ? metaSzs[i].y + gap : 0.f);
		
		if (rows[i].active) {
			dl->AddRectFilled(ImVec2(min.x + padX, curY - 1.f), ImVec2(min.x + padX + barW, curY + rowH + 1.f),
				ColA(rows[i].accent, alpha * 0.90f), 1.f);
		}
		if (anyActive) tx += barW + barSpace;
		
		dl->AddText(ImVec2(tx, curY), ColRGB(250, 250, 252, alpha), rows[i].title);
		if (rows[i].meta && rows[i].meta[0]) {
			float my = curY + titleSzs[i].y + gap;
			dl->AddText(ImVec2(tx, my), rows[i].active ? ColA(rows[i].accent, alpha * 0.90f) : ColRGB(160, 162, 170, alpha), rows[i].meta);
		}
		curY += rowH + rowGap;
	}
}

void DrawGuideLine(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col, bool locked) {
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float len = std::
sqrt(dx * dx + dy * dy);
	if (len < 16.f)
		return;
	const float inv = 1.f / len;
	const float trim = locked ? 16.f : 12.f;
	const ImVec2 p0{ a.x + dx * (trim * inv), a.y + dy * (trim * inv) };
	const ImVec2 p1{ a.x + dx * ((len - trim) * inv), a.y + dy * ((len - trim) * inv) };
	dl->AddLine(p0, p1, Ui::
kInk, locked ? 1.7f : 1.3f);
	dl->AddLine(p0, p1, col, locked ? 1.0f : 0.85f);
}

void SyncMapCache() {
	char prev[128]{};
	std::
snprintf(prev, sizeof(prev), "%s", g_mapCache);
	GameMode::
EnsureMap();
	const char* m = GameMode::
BaseMap();
	if (m && m[0])
		std::
snprintf(g_mapCache, sizeof(g_mapCache), "%s", m);
	else
		g_mapCache[0] = '\0';
	// Map leave / change: drop index + selected lineup (stale g_currentIdx after leave).
	if (_stricmp(prev, g_mapCache) != 0) {
		InvalidateMapIndex();
		g_currentIdx = -1;
		// Armed capture is map-bound - cancel if map went empty or changed.
		if (g_cap.armed && (!g_mapCache[0] || (prev[0] && _stricmp(prev, g_mapCache) != 0)))
			g_cap = {};
	}
}

C_CSPlayerPawn* GetLocalSeh() {
	C_CSPlayerPawn* local = H::
SafeLocalPlayer();
	if (!local || !Mem::
ValidEntity(local))
		return nullptr;
	return local;
}

C_CSWeaponBase* GetWeaponSeh(C_CSPlayerPawn* local) {
	if (!local)
		return nullptr;
	C_CSWeaponBase* wep = nullptr;
	__try {
		wep = local->GetActiveWeapon();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!wep || !Mem::
ValidEntity(wep))
		return nullptr;
	return wep;
}

void CopyMapNameSeh(const char* mapName, char* out, size_t outSz) {
	if (!out || outSz == 0)
		return;
	out[0] = '\0';
	if (!mapName || !Mem::
IsReadable(mapName, 1))
		return;
	char tmp[128]{};
	__try {
		if (mapName[0] && mapName[0] > 32) {
			std::
snprintf(tmp, sizeof(tmp), "%s", mapName);
			for (char* p = tmp; *p; ++p) {
				if (*p == '.') { *p = '\0'; break; }
			}
			const char* base = tmp;
			for (char* p = tmp; *p; ++p) {
				if (*p == '/' || *p == '\\')
					base = p + 1;
			}
			std::
snprintf(out, outSz, "%s", base);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out[0] = '\0';
	}
}

} // namespace

const char* KindName(NadeKind k) {
	switch (k) {
	case NadeKind::
HE: return "HE";
	case NadeKind::
Flash: return "Flash";
	case NadeKind::
Smoke: return "Smoke";
	case NadeKind::
Molly: return "Molly";
	case NadeKind::
Decoy: return "Decoy";
	default: return "Any";
	}
}

const char* ThrowName(ThrowType t) {
	switch (t) {
	case ThrowType::Walk: return "Walk";
	case ThrowType::Run: return "Run";
	case ThrowType::Jump: return "Stand+Jump";
	case ThrowType::Crouch: return "Crouch";
	case ThrowType::RunJump: return "Run+Jump";
	case ThrowType::CrouchJump: return "Crouch+Jump";
	case ThrowType::CrouchWalk: return "Crouch+Walk";
	case ThrowType::WalkJump: return "Walk+Jump";
	case ThrowType::LeftJump: return "Left+Jump";
	case ThrowType::RightJump: return "Right+Jump";
	case ThrowType::BackJump: return "Back+Jump";
	case ThrowType::Left: return "Left";
	case ThrowType::Right: return "Right";
	case ThrowType::Back: return "Back";
	case ThrowType::Stand:
	default: return "Stand";
	}
}

std::vector<Lineup>& AllMut() { return g_lineups; }

const std::vector<Lineup>& All() { return g_lineups; }
const Lineup* Current() {
	if (g_currentIdx < 0 || g_currentIdx >= static_cast<int>(g_lineups.size()))
		return nullptr;
	return &g_lineups[static_cast<size_t>(g_currentIdx)];
}
const char* CurrentMap() {
	SyncMapCache();
	return g_mapCache;
}

bool IsCurrentMap(const Lineup& L) {
	SyncMapCache();
	if (!g_mapCache[0] || L.map.empty())
		return false;
	return MapsEqual(L.map, g_mapCache);
}

int CountCurrentMap() {
	SyncMapCache();
	if (!g_mapCache[0])
		return 0;
	EnsureMapIndex();
	return static_cast<int>(g_mapIndices.size());
}

void Init() {
	if (!g_loaded)
		Load();
}

void Shutdown() {
	g_currentIdx = -1;
	g_cap = {};
	g_lineups.clear();
	g_loaded = false;
	InvalidateMapIndex();
	g_captureKeyWasDown = false;
}

void OnLevelInit(const char* mapName) {
	g_currentIdx = -1;
	g_cap = {};
	CopyMapNameSeh(mapName, g_mapCache, sizeof(g_mapCache));
	InvalidateMapIndex();
	if (!g_loaded)
		Load();
}

// Sample pawn + keys once per armed frame.
// Refresh aim every frame. Motion uses recency + while-attack latches (not whole-session).
static void SampleCaptureMotion(C_CSPlayerPawn* local) {
	if (!local || !g_cap.armed)
		return;

	// Live aim + stand lock only until the pin drops. Commit lands >=200 ms
	// after release (grace) and defers further on menu focus / net lag - past
	// release, live angles are no longer throw angles, and a landed-stationary
	// sample would re-anchor stand to the landing spot.
	const bool released = g_cap.throwCommitted || g_cap.pinReleaseMs != 0;

	// Live aim while armed - arm-only freeze saved wrong angles after re-aim.
	QAngle_t liveAng{};
	if (!released && GetLocalViewAngles(liveAng) && IsFiniteAng(liveAng)) {
		g_cap.ang = liveAng;
		g_cap.ang.z = g_cap.eyeLift;
	}

	std::uint32_t flags = 0;
	Vector_t vel{};
	__try {
		flags = local->m_fFlags();
		vel = local->m_vecAbsVelocity();
		if (!IsFiniteVec(vel))
			vel = local->m_vecVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		flags = 0;
		vel = Vector_t{};
	}

	const ULONGLONG now = GetTickCount64();
	const float speed2d = (std::isfinite(vel.x) && std::isfinite(vel.y))
		? std::sqrt(vel.x * vel.x + vel.y * vel.y) : 0.f;
	const bool onGround = (flags & FL_ONGROUND) != 0;

	// Update locked stand position as long as the player is stationary.
	// This naturally captures corner micro-adjustments and the true start pad of a run-throw.
	if (!released && speed2d < 5.f && onGround) {
		Vector_t origin{};
		if (Bones::GetOrigin(local, origin) && Bones::IsValidPos(origin)) {
			g_cap.pos = origin;
			g_cap.eyeLift = StandEyeLift(local);
			g_cap.ang.z = g_cap.eyeLift;
		}
	}

	const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	const bool wDown = (GetAsyncKeyState('W') & 0x8000) != 0;
	const bool aDown = (GetAsyncKeyState('A') & 0x8000) != 0;
	const bool dDown = (GetAsyncKeyState('D') & 0x8000) != 0;
	const bool sDown = (GetAsyncKeyState('S') & 0x8000) != 0;
	const bool shiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0
		|| (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;

	// Jump: space or rising - bare airborne latched whole sessions as Jump.
	const bool rising = std::isfinite(vel.z) && vel.z > 40.f;
	const bool airborne = !onGround && std::isfinite(vel.z) && std::fabs(vel.z) > 20.f;
	const bool ducking = (flags & (FL_DUCKING | FL_ANIMDUCKING)) != 0
		|| (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0
		|| (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;

	// Check relative velocity direction against view angles
	float fwdDot = 0.f, rightDot = 0.f;
	if (speed2d > 20.f && IsFiniteAng(g_cap.ang)) {
		Vector_t fwd{}, right{};
		const float yawRad = g_cap.ang.y * (3.14159265f / 180.f);
		fwd = { std::cos(yawRad), std::sin(yawRad), 0.f };
		right = { -std::sin(yawRad), std::cos(yawRad), 0.f };
		const Vector_t normVel = { vel.x / speed2d, vel.y / speed2d, 0.f };
		fwdDot = fwd.x * normVel.x + fwd.y * normVel.y;
		rightDot = right.x * normVel.x + right.y * normVel.y;
	}

	const bool pinning = g_cap.sawAttack;
	if (pinning) {
		if (spaceDown || rising || airborne)
			g_cap.lastJumpMs = now;
		if (ducking)
			g_cap.lastDuckMs = now;
		if (shiftDown || (speed2d > kWalkSpeedMin && speed2d <= kRunSpeedMin))
			g_cap.lastWalkMs = now;
		if (speed2d > kRunSpeedMin || (wDown && !shiftDown))
			g_cap.lastRunMs = now;

		// Directional movement checks (keys or velocity alignment)
		if (aDown || rightDot < -0.45f)
			g_cap.lastLeftMs = now;
		if (dDown || rightDot > 0.45f)
			g_cap.lastRightMs = now;
		if (sDown || fwdDot < -0.45f)
			g_cap.lastBackMs = now;
	}

	// Input event log - whole armed session, pre-pin run-ups included.
	// Edge-triggered: only state changes become events, so the ring fills
	// with real steps (A? ... A? SPACE? ...) not per-frame key states.
	{
		const bool keysNow[CapKey_Count] = {
			wDown, sDown, aDown, dDown, spaceDown, ducking, shiftDown
		};
		for (std::uint8_t k = 0; k < CapKey_Count; ++k) {
			if (keysNow[k] == g_cap.prevDown[k])
				continue;
			g_cap.prevDown[k] = keysNow[k];
			if (g_cap.eventN < kMaxCapEvents) {
				const ULONGLONG rel = (now >= g_cap.armMs) ? (now - g_cap.armMs) : 0;
				g_cap.events[g_cap.eventN++] = CapInputEvent{ rel, k, keysNow[k] };
			}
		}
		// Attack release (the throw) is logged by the commit path via
		// pinReleaseMs - it is always the recipe's final step.
	}
}

// Compress the input event log into an ordered recipe - "A 0.8s -> SPACE -> throw".
// Deliberate inputs only: released segments shorter than 250 ms are aim-noise
// and dropped; keys still held at the throw are marked with '+'. The attack
// release (pin) is always the final step. Empty result = plain throw.
static void BuildInputRecipe(const CaptureSession& cap, char* out, size_t outN) {
	out[0] = '\0';
	if (outN < 16)
		return;
	static constexpr const char* kKeyNames[CapKey_Count] = {
		"W", "S", "A", "D", "SPACE", "CTRL", "SHIFT"
	};
	ULONGLONG downMs[CapKey_Count] = {};
	bool down[CapKey_Count] = {};
	char steps[4][40];
	int stepN = 0;

	auto emitStep = [&](std::uint8_t key, float durSec, bool held) {
		if (stepN >= 4)
			return;
		char* s = steps[stepN];
		if (held)
			std::snprintf(s, sizeof(steps[0]), "%s+", kKeyNames[key]);
		else if (durSec >= 0.3f)
			std::snprintf(s, sizeof(steps[0]), "%s %.1fs", kKeyNames[key], durSec);
		else
			std::snprintf(s, sizeof(steps[0]), "%s", kKeyNames[key]);
		++stepN;
	};

	for (int i = 0; i < cap.eventN && stepN < 4; ++i) {
		const CapInputEvent& ev = cap.events[i];
		if (ev.key >= CapKey_Count)
			continue;
		if (ev.down) {
			downMs[ev.key] = ev.ms;
			down[ev.key] = true;
		} else if (down[ev.key]) {
			down[ev.key] = false;
			const float dur = static_cast<float>(ev.ms - downMs[ev.key]) / 1000.f;
			if (dur >= 0.25f)
				emitStep(ev.key, dur, false);
		}
	}
	for (std::uint8_t k = 0; k < CapKey_Count && stepN < 4; ++k) {
		if (down[k])
			emitStep(k, 0.f, true);
	}
	if (stepN == 0)
		return; // plain throw - no recipe noise

	size_t pos = 0;
	for (int i = 0; i < stepN && pos < outN; ++i) {
		const int w = std::snprintf(out + pos, outN - pos, "%s%s",
			(i ? " -> " : ""), steps[i]);
		if (w < 0)
			break;
		pos += static_cast<size_t>(w);
	}
	if (pos < outN)
		std::snprintf(out + pos, outN - pos, " -> throw");
}

static bool RecentMs(ULONGLONG stamp, ULONGLONG now, ULONGLONG window) {
	return stamp != 0 && now >= stamp && (now - stamp) <= window;
}

static ThrowType DetectThrowStyle(C_CSPlayerPawn* local, const CaptureSession& cap) {
	if (!local)
		return ThrowType::Stand;

	std::uint32_t flags = 0;
	Vector_t vel{};
	__try {
		flags = local->m_fFlags();
		vel = local->m_vecAbsVelocity();
		if (!IsFiniteVec(vel))
			vel = local->m_vecVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		flags = 0;
		vel = Vector_t{};
	}

	const ULONGLONG now = GetTickCount64();
	const float speed2d = (std::isfinite(vel.x) && std::isfinite(vel.y))
		? std::sqrt(vel.x * vel.x + vel.y * vel.y) : 0.f;
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool duckingNow = (flags & (FL_DUCKING | FL_ANIMDUCKING)) != 0;
	const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	const bool shiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0
		|| (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
	const bool aDown = (GetAsyncKeyState('A') & 0x8000) != 0;
	const bool dDown = (GetAsyncKeyState('D') & 0x8000) != 0;
	const bool sDown = (GetAsyncKeyState('S') & 0x8000) != 0;
	const bool wDown = (GetAsyncKeyState('W') & 0x8000) != 0;
	const bool rising = std::isfinite(vel.z) && vel.z > 40.f;
	const bool airborne = !onGround && std::isfinite(vel.z) && std::fabs(vel.z) > 20.f;

	const bool hadPin = cap.sawAttack;
	const bool jumping = hadPin
		&& (RecentMs(cap.lastJumpMs, now, kMotionRecentMs) || spaceDown || rising || airborne);
	const bool ducking = (hadPin && RecentMs(cap.lastDuckMs, now, kMotionRecentMs))
		|| duckingNow;
	const bool running = hadPin
		&& (RecentMs(cap.lastRunMs, now, kMotionRecentMs)
			|| (speed2d > kRunSpeedMin && !shiftDown && onGround)
			|| (wDown && !shiftDown && speed2d > 25.f));
	const bool walking = hadPin
		&& (RecentMs(cap.lastWalkMs, now, kMotionRecentMs)
			|| shiftDown
			|| (speed2d > kWalkSpeedMin && speed2d <= kRunSpeedMin && onGround));
	const bool movingLeft = hadPin && (RecentMs(cap.lastLeftMs, now, kMotionRecentMs) || aDown);
	const bool movingRight = hadPin && (RecentMs(cap.lastRightMs, now, kMotionRecentMs) || dDown);
	const bool movingBack = hadPin && (RecentMs(cap.lastBackMs, now, kMotionRecentMs) || sDown);

	// Priority 1: Crouch combinations
	if (ducking) {
		if (jumping)
			return ThrowType::CrouchJump;
		if (running || walking || speed2d > 20.f)
			return ThrowType::CrouchWalk;
		return ThrowType::Crouch;
	}

	// Priority 2: Jump + Directional combinations
	if (jumping) {
		if (movingLeft && !running)
			return ThrowType::LeftJump;
		if (movingRight && !running)
			return ThrowType::RightJump;
		if (movingBack && !running)
			return ThrowType::BackJump;
		if (running)
			return ThrowType::RunJump;
		if (walking)
			return ThrowType::WalkJump;
		return ThrowType::Jump;
	}

	// Priority 3: Ground Directional throws
	if (movingLeft && !running)
		return ThrowType::Left;
	if (movingRight && !running)
		return ThrowType::Right;
	if (movingBack && !running)
		return ThrowType::Back;

	// Priority 4: Run / Walk / Stand
	if (running || (hadPin && speed2d > kRunSpeedMin))
		return ThrowType::Run;
	if (walking)
		return ThrowType::Walk;

	return ThrowType::Stand;
}

bool IsCapturing() { return g_cap.armed; }

// Dump-verified C_BaseCSGrenade fields - m_bPinPulled / m_bIsHeldByPlayer / m_fThrowTime.
// Bind throws (+jump;+attack alias) never press a physical mouse button, so
// the grenade's own pin + throw-time is the attack signal for them.
constexpr uint32_t kOffPinPulled = 0x1CE3;
constexpr uint32_t kOffHeldByPlayer = 0x1CE2;
constexpr uint32_t kOffThrowTime = 0x1CE8;

uint32_t SchemaOr(const char* field, uint32_t fallback) {
	const uint32_t off = SchemaFinder::Get(hash_32_fnv1a_const(field));
	if (off >= 0x100 && off < 0x20000)
		return off;
	return fallback;
}

bool SehReadBoolOff(void* ent, uint32_t off) {
	if (!ent || !off)
		return false;
	const auto* p = reinterpret_cast<const uint8_t*>(ent) + off;
	if (!Mem::IsReadable(p, 1))
		return false;
	__try {
		return *p != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

float SehReadFloatOff(void* ent, uint32_t off) {
	if (!ent || !off)
		return 0.f;
	const auto* p = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(ent) + off);
	if (!Mem::IsReadable(p, sizeof(float)))
		return 0.f;
	__try {
		const float v = *p;
		return v;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.f;
	}
}

int SehReadClip(C_CSWeaponBase* wep) {
	if (!wep || !Mem::ValidEntity(wep))
		return -1;
	int clip = -1;
	__try { clip = wep->m_iClip1(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	return clip;
}

void ReadGrenadePinState(C_CSWeaponBase* wep, bool& pinPulled, bool& heldByPlayer) {
	pinPulled = false;
	heldByPlayer = false;
	if (!wep || !Mem::ValidEntity(wep))
		return;
	const uint32_t offPin = SchemaOr("C_BaseCSGrenade->m_bPinPulled", kOffPinPulled);
	const uint32_t offHeld = SchemaOr("C_BaseCSGrenade->m_bIsHeldByPlayer", kOffHeldByPlayer);
	pinPulled = SehReadBoolOff(wep, offPin);
	heldByPlayer = SehReadBoolOff(wep, offHeld);
}

float ReadGrenadeThrowTime(C_CSWeaponBase* wep) {
	if (!wep || !Mem::ValidEntity(wep))
		return 0.f;
	const float v = SehReadFloatOff(wep, SchemaOr("C_BaseCSGrenade->m_fThrowTime", kOffThrowTime));
	return std::isfinite(v) ? v : 0.f;
}

void ResetCookLatch() {
	g_cap.attackHeld = false;
	g_cap.attackDownMs = 0;
	g_cap.cookWep = nullptr;
	g_cap.cookClip = -1;
	g_cap.cookThrowBaseline = 0.f;
	g_cap.throwCommitted = false;
	g_cap.pinReleaseMs = 0;
}

void CancelCapture() {
	if (!g_cap.armed)
		return;
	g_cap = {};
	Notify::
Warn("Lineup capture", "Cancelled");
}

ThrowType CurrentDetected() {
	if (!g_cap.armed)
		return ThrowType::
Stand;
	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return ThrowType::
Stand;
	return DetectThrowStyle(local, g_cap);
}

bool ArmCapture(const char* name, NadeKind kindOverride) {
	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return false;

	Vector_t pos{};
	if (!Bones::
GetOrigin(local, pos) || !Bones::
IsValidPos(pos))
		return false;
	if (std::
fabs(pos.x) < 1.f && std::
fabs(pos.y) < 1.f && std::
fabs(pos.z) < 1.f)
		return false;

	QAngle_t ang{};
	if (!GetLocalViewAngles(ang) || !IsFiniteAng(ang))
		return false;

	if (!GameMode::
EnsureMap()) {
		Notify::
Error("Lineup capture", "Could not read map name");
		return false;
	}
	SyncMapCache();
	if (!g_mapCache[0]) {
		Notify::
Error("Lineup capture", "Map name empty");
		return false;
	}

	g_cap = {};
	g_cap.armed = true;
	g_cap.pos = pos; // feet locked at arm (lineup stand pad)
	g_cap.eyeLift = StandEyeLift(local);
	g_cap.ang = ang; // aim refreshed every SampleCaptureMotion frame
	g_cap.ang.z = g_cap.eyeLift;
	g_cap.armMs = GetTickCount64();
	g_cap.attackHeld = false;
	g_cap.sawAttack = false;
	g_cap.lastJumpMs = 0;
	g_cap.lastRunMs = 0;
	g_cap.lastDuckMs = 0;
	g_cap.attackDownMs = 0;
	std::
snprintf(g_cap.name, sizeof(g_cap.name), "%s",
		(name && name[0]) ? name : "Lineup");
	// Do not sample motion yet - ignore residual LMB from Arm button / keybind.

	if (kindOverride != NadeKind::
Any)
		g_cap.kind = kindOverride;
	else
		g_cap.kind = ClassifyHeld(GetWeaponSeh(local));

	Notify::
Info("Lineup", "Throw to save ? capture key again = cancel");
	Con::
Ok("GrenadeHelper armed capture \"%s\" kind=%s - waiting for throw",
		g_cap.name, KindName(g_cap.kind));
	return true;
}

bool Capture(const char* name, ThrowType throwType, NadeKind kindOverride) {
	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return false;

	Vector_t pos{};
	if (!Bones::
GetOrigin(local, pos) || !Bones::
IsValidPos(pos))
		return false;
	if (std::
fabs(pos.x) < 1.f && std::
fabs(pos.y) < 1.f && std::
fabs(pos.z) < 1.f)
		return false;

	QAngle_t ang{};
	if (!GetLocalViewAngles(ang) || !IsFiniteAng(ang))
		return false;

	Lineup L{};
	L.name = (name && name[0]) ? name : "Lineup";
	if (!GameMode::
EnsureMap()) {
		Con::
Error("GrenadeHelper capture failed - could not read map name");
		return false;
	}
	SyncMapCache();
	const char* map = g_mapCache[0] ? g_mapCache : GameMode::
BaseMap();
	if (!map || !map[0]) {
		Con::
Error("GrenadeHelper capture failed - map name empty after EnsureMap");
		return false;
	}
	L.map = map;
	L.throwType = throwType;
	L.pos = pos;
	L.aimAngles = ang;
	L.aimAngles.z = StandEyeLift(local);
	L.target = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);

	if (kindOverride != NadeKind::
Any)
		L.kind = kindOverride;
	else
		L.kind = ClassifyHeld(GetWeaponSeh(local));
	L.fromPack = false;

	g_lineups.push_back(std::
move(L));
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();
	Con::
Ok("GrenadeHelper captured \"%s\" map=%s kind=%s throw=%s ang=%.1f/%.1f",
		g_lineups.back().name.c_str(),
		g_lineups.back().map.c_str(),
		KindName(g_lineups.back().kind),
		ThrowName(g_lineups.back().throwType),
		g_lineups.back().aimAngles.x, g_lineups.back().aimAngles.y);
	char toast[96]{};
	std::
snprintf(toast, sizeof(toast), "%s ? %s",
		g_lineups.back().name.c_str(), ThrowName(g_lineups.back().throwType));
	Notify::
Success("Lineup saved", toast);
	return true;
}
static bool FinishArmedCapture(C_CSPlayerPawn* local) {
	if (!g_cap.armed || !local)
		return false;

	// Refresh map if arm happened mid-load / empty cache
	SyncMapCache();
	if (!g_mapCache[0]) {
		GameMode::
EnsureMap();
		SyncMapCache();
	}
	if (!g_mapCache[0]) {
		Notify::
Error("Lineup capture", "Map name empty");
		g_cap = {};
		return false;
	}
	if (!IsFiniteVec(g_cap.pos) || !IsFiniteAng(g_cap.ang)
		|| (std::
fabs(g_cap.pos.x) < 1.f && std::
fabs(g_cap.pos.y) < 1.f && std::
fabs(g_cap.pos.z) < 1.f)) {
		Notify::
Error("Lineup capture", "Invalid stand / angles");
		g_cap = {};
		return false;
	}

	// Last sample: refresh aim + motion window at mouse release
	SampleCaptureMotion(local);
	const ThrowType style = DetectThrowStyle(local, g_cap);
	// Input recipe - ordered multi-input sequence (jumpthrow / run-ups / duck).
	char recipe[96]{};
	BuildInputRecipe(g_cap, recipe, sizeof(recipe));
	Lineup L{};
	L.name = g_cap.name[0] ? g_cap.name : "Lineup";
	L.map = g_mapCache;
	L.throwType = style;
	L.inputs = recipe;
	L.pos = g_cap.pos; // stand feet from arm
	// Prefer live aim at throw (Sample already wrote g_cap.ang)
	L.aimAngles = g_cap.ang;
	if (!IsFiniteAng(L.aimAngles)) {
		QAngle_t a{};
		if (GetLocalViewAngles(a) && IsFiniteAng(a))
			L.aimAngles = a;
	}
	L.aimAngles.z = (g_cap.eyeLift >= 32.f && g_cap.eyeLift <= 80.f) ? g_cap.eyeLift : kEyeLift;
	L.target = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);
	L.kind = g_cap.kind;
	// At release weapon may still be nade - reclassify if arm was Any.
	if (L.kind == NadeKind::
Any)
		L.kind = ClassifyHeld(GetWeaponSeh(local));
	L.fromPack = false;

	g_lineups.push_back(std::
move(L));
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();

	char msg[128]{};
	if (recipe[0])
		std::snprintf(msg, sizeof(msg), "%s ? %s ? %s",
			g_lineups.back().name.c_str(), ThrowName(g_lineups.back().throwType), recipe);
	else
		std::snprintf(msg, sizeof(msg), "%s ? %s",
			g_lineups.back().name.c_str(), ThrowName(g_lineups.back().throwType));
	Notify::
Success("Lineup saved", msg);
	Con::
Ok("GrenadeHelper auto-captured \"%s\" throw=%s inputs=\"%s\"",
		g_lineups.back().name.c_str(), ThrowName(g_lineups.back().throwType), recipe);
	g_cap = {};
	return true;
}

bool RemoveAt(int index) {
	if (index < 0 || index >= static_cast<int>(g_lineups.size()))
		return false;
	g_lineups.erase(g_lineups.begin() + index);
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();
	return true;
}

bool RenameAt(int index, const char* newName) {
	if (index < 0 || index >= static_cast<int>(g_lineups.size()) || !newName || !newName[0])
		return false;
	g_lineups[static_cast<size_t>(index)].name = newName;
	Save();
	return true;
}

bool SetThrowAt(int index, ThrowType t) {
	if (index < 0 || index >= static_cast<int>(g_lineups.size()))
		return false;
	if (t < ThrowType::Stand || t >= ThrowType::Count)
		return false;
	g_lineups[static_cast<size_t>(index)].throwType = t;
	Save();
	return true;
}

bool SetEnabledAt(int index, bool enabled) {
	if (index < 0 || index >= static_cast<int>(g_lineups.size()))
		return false;
	g_lineups[static_cast<size_t>(index)].enabled = enabled;
	Save();
	return true;
}

void ClearCurrentMap() {
	SyncMapCache();
	// Only wipe user captures for this map - pack entries reloaded from packs/.
	g_lineups.erase(std::
remove_if(g_lineups.begin(), g_lineups.end(),
		[](const Lineup& L) {
			return !L.fromPack && MapsEqual(L.map, g_mapCache);
		}),
		g_lineups.end());
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();
}

bool Save() {
	nlohmann::json j = nlohmann::json::array();
	for (const auto& L : g_lineups) {
		if (L.fromPack)
			continue; // community packs stay in packs/; never rewrite into user file
		j.push_back({
			{ "name", L.name },
			{ "map", L.map },
			{ "kind", static_cast<int>(L.kind) },
			{ "throw", static_cast<int>(L.throwType) },
			{ "pos", { L.pos.x, L.pos.y, L.pos.z } },
			{ "ang", { L.aimAngles.x, L.aimAngles.y, L.aimAngles.z } },
			{ "target", { L.target.x, L.target.y, L.target.z } },
			{ "enabled", L.enabled },
			{ "inputs", L.inputs }
		});
	}
	const auto path = LineupFilePath();
	std::ofstream out(path);
	if (!out)
		return false;
	out << j.dump(2);
	return true;
}

// Games8Th kind: Any=0 HE=1 Flash=2 Smoke=3 Molly=4 Decoy=5
// CS2-DMA Type: Flash=0 Smoke=1 HE=2 Molly=3
static NadeKind KindFromDmaType(int t) {
	switch (t) {
	case 0: return NadeKind::
Flash;
	case 1: return NadeKind::
Smoke;
	case 2: return NadeKind::
HE;
	case 3: return NadeKind::
Molly;
	default: return NadeKind::
Any;
	}
}

static NadeKind KindFromWeaponString(const std::
string& s) {
	std::
string l = s;
	for (char& c : l) c = static_cast<char>(std::
tolower(static_cast<unsigned char>(c)));
	if (l.find("smoke") != std::
string::
npos) return NadeKind::
Smoke;
	if (l.find("flash") != std::
string::
npos) return NadeKind::
Flash;
	if (l.find("molotov") != std::
string::
npos || l.find("incen") != std::
string::npos
		|| l.find("incgrenade") != std::
string::
npos) return NadeKind::
Molly;
	if (l.find("hegrenade") != std::
string::
npos || l == "he" || l.find("weapon_he") != std::
string::npos)
		return NadeKind::
HE;
	if (l.find("decoy") != std::
string::
npos) return NadeKind::
Decoy;
	return NadeKind::
Any;
}

static ThrowType ClampThrow(int v) {
	if (v < 0 || v >= static_cast<int>(ThrowType::
Count))
		return ThrowType::
Stand;
	return static_cast<ThrowType>(v);
}

static ThrowType ThrowFromDmaStyle(int style) {
	// DMA: 0 Stand, 1 Run, 2 Jump, 3 Crouch, 4 Run+Jump
	switch (style) {
	case 1: return ThrowType::
Run;
	case 2: return ThrowType::
Jump;
	case 3: return ThrowType::
Crouch;
	case 4: return ThrowType::
RunJump;
	case 0:
	default: return ThrowType::
Stand;
	}
}

static ThrowType ThrowFromString(std::string tt) {
	for (char& c : tt)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

	if (tt.find("CROUCH") != std::string::npos || tt.find("DUCK") != std::string::npos) {
		if (tt.find("JUMP") != std::string::npos)
			return ThrowType::CrouchJump;
		if (tt.find("WALK") != std::string::npos || tt.find("RUN") != std::string::npos)
			return ThrowType::CrouchWalk;
		return ThrowType::Crouch;
	}

	if (tt.find("LEFT") != std::string::npos || tt.find("A+") != std::string::npos || tt.find("+A") != std::string::npos) {
		if (tt.find("JUMP") != std::string::npos)
			return ThrowType::LeftJump;
		return ThrowType::Left;
	}
	if (tt.find("RIGHT") != std::string::npos || tt.find("D+") != std::string::npos || tt.find("+D") != std::string::npos) {
		if (tt.find("JUMP") != std::string::npos)
			return ThrowType::RightJump;
		return ThrowType::Right;
	}
	if (tt.find("BACK") != std::string::npos || tt.find("S+") != std::string::npos || tt.find("+S") != std::string::npos) {
		if (tt.find("JUMP") != std::string::npos)
			return ThrowType::BackJump;
		return ThrowType::Back;
	}

	if (tt.find("RUN") != std::string::npos && tt.find("JUMP") != std::string::npos)
		return ThrowType::RunJump;
	if (tt.find("WALK") != std::string::npos && tt.find("JUMP") != std::string::npos)
		return ThrowType::WalkJump;

	// 'W' is also inside STAND - do not treat STAND+JUMP as a run-jump.
	if (tt.find("JUMP") != std::string::npos && tt.find("STAND") == std::string::npos
		&& (tt.find("W+") != std::string::npos || tt.find("+W") != std::string::npos
			|| tt.find("WJUMP") != std::string::npos || tt.find("W JUMP") != std::string::npos))
		return ThrowType::RunJump;
	// Stand+Jump / StandJump / Jumpthrow -> Jump (stand jumpthrow)
	if (tt.find("JUMP") != std::string::npos)
		return ThrowType::Jump;
	if (tt.find("WALK") != std::string::npos || tt.find("SHIFT") != std::string::npos)
		return ThrowType::Walk;
	if (tt.find("RUN") != std::string::npos || tt.find("FORWARD") != std::string::npos || tt == "W")
		return ThrowType::Run;
	return ThrowType::Stand;
}

// DMA Chinese names encode style (Style field is often 0). Hex UTF-8 - no /utf-8 needed.
// ?=\xE8\xB7\xB3 ?=\xE8\xB7\x91 ?=\xE8\xB9\xB2 ?=\xE9\x9D\x99
// ?=\xE8\xB5\xB0 ?=\xE6\xAD\xA5 ?=\xE5\x8F\xB0 ?=\xE7\x82\xB9
// ?=\xE6\x8A\x95 ?=\xE5\x8F\x8C ?=\xE9\x94\xAE
// 
// 
static bool ContainsBytes(const std::
string& s, const char* bytes) {
	return s.find(bytes) != std::
string::
npos;
}

static ThrowType ThrowFromName(const std::
string& name) {
	if (name.empty())
		return ThrowType::
Stand;

	// Prefer instruction after '|' (??|W?)
	std::
string instr = name;
	const size_t pipe = name.find_last_of('|');
	if (pipe != std::
string::
npos && pipe + 1 < name.size())
		instr = name.substr(pipe + 1);

	constexpr const char* kJump = "\xE8\xB7\xB3";
	constexpr const char* kRun = "\xE8\xB7\x91";
	constexpr const char* kCrouch = "\xE8\xB9\xB2";
	constexpr const char* kQuiet = "\xE9\x9D\x99";
	constexpr const char* kWalkZ = "\xE8\xB5\xB0";
	constexpr const char* kStep = "\xE6\xAD\xA5";
	constexpr const char* kPlat = "\xE5\x8F\xB0";
	constexpr const char* kPoint = "\xE7\x82\xB9";
	constexpr const char* kThrowCh = "\xE6\x8A\x95";
	constexpr const char* kDouble = "\xE5\x8F\x8C";
	constexpr const char* kKey = "\xE9\x94\xAE";

	// Strip location false-positives ?? / ??
	std::
string cn = instr;
	const std::
string jumpPlat = std::
string(kJump) + kPlat;
	const std::
string jumpPoint = std::
string(kJump) + kPoint;
	for (;;) {
		size_t p = cn.find(jumpPlat);
		if (p == std::
string::
npos) break;
		cn.erase(p, jumpPlat.size());
	}
	for (;;) {
		size_t p = cn.find(jumpPoint);
		if (p == std::
string::
npos) break;
		cn.erase(p, jumpPoint.size());
	}

	std::
string en = name;
	for (char& c : en)
		c = static_cast<char>(std::
tolower(static_cast<unsigned char>(c)));

	// English pack suffixes / UI labels first (order matters).
	// "stand+jump" is Jump, not RunJump. "runjump" / "run+jump" is RunJump.
	if (en.find("runjump") != std::
string::
npos
		|| en.find("run+jump") != std::
string::
npos
		|| en.find("run + jump") != std::
string::
npos
		|| en.find("run-jump") != std::
string::
npos)
		return ThrowType::
RunJump;
	// Trailing " - Run" / " - Jump" from our packs (avoid place names with "jump")
	const size_t dash = en.rfind(" - ");
	if (dash != std::
string::
npos) {
		const std::
string tail = en.substr(dash + 3);
		if (tail == "runjump" || tail.find("run+jump") != std::
string::
npos)
			return ThrowType::
RunJump;
		if (tail == "jump" || tail == "stand+jump" || tail == "jumpthrow")
			return ThrowType::
Jump;
		if (tail == "crouch" || tail == "duck")
			return ThrowType::
Crouch;
		if (tail == "run" || tail == "running")
			return ThrowType::
Run;
		if (tail == "walk" || tail == "shift")
			return ThrowType::
Walk;
		if (tail == "stand" || tail == "normal")
			return ThrowType::
Stand;
	}

	const bool wJump = (instr.find('W') != std::
string::
npos || instr.find('w') != std::
string::npos)
		&& ContainsBytes(instr, kJump);
	const bool runJumpCn = ContainsBytes(cn, kRun) && ContainsBytes(cn, kJump);
	const bool jumpCn = ContainsBytes(cn, kJump)
		|| (ContainsBytes(instr, kJump) && ContainsBytes(instr, kThrowCh))
		|| (ContainsBytes(instr, kDouble) && ContainsBytes(instr, kKey));
	const bool runCn = ContainsBytes(cn, kRun);
	const bool crouchCn = ContainsBytes(cn, kCrouch);
	const bool walkCn = (ContainsBytes(instr, kQuiet) && ContainsBytes(instr, kWalkZ))
		|| (ContainsBytes(instr, kQuiet) && ContainsBytes(instr, kStep));

	// English freeform (user renames / sapphyrus)
	const bool crouchEn = en.find("crouch") != std::string::npos || en.find("duck") != std::string::npos;
	const bool leftEn = en.find("left") != std::string::npos || en.find(" a ") != std::string::npos || (en.length() >= 2 && en.substr(0, 2) == "a ") || (en.length() >= 2 && en.substr(en.length() - 2) == " a") || en == "a" || en.find("a+") != std::string::npos;
	const bool rightEn = en.find("right") != std::string::npos || en.find(" d ") != std::string::npos || (en.length() >= 2 && en.substr(0, 2) == "d ") || (en.length() >= 2 && en.substr(en.length() - 2) == " d") || en == "d" || en.find("d+") != std::string::npos;
	const bool backEn = en.find("back") != std::string::npos || en.find(" s ") != std::string::npos || (en.length() >= 2 && en.substr(0, 2) == "s ") || (en.length() >= 2 && en.substr(en.length() - 2) == " s") || en == "s" || en.find("s+") != std::string::npos;

	const bool runEn = en.find("running") != std::string::npos
		|| en.find("w+") != std::string::npos
		|| en.find(" w ") != std::string::npos
		|| (en.length() >= 2 && en.substr(0, 2) == "w ")
		|| (en.length() >= 2 && en.substr(en.length() - 2) == " w")
		|| en == "w"
		|| en.find("forward") != std::string::npos
		|| (en.find("run") != std::string::npos
			&& en.find("runjump") == std::string::npos
			&& en.find("run+jump") == std::string::npos);
	const bool jumpEn = en.find("jumpthrow") != std::string::npos
		|| en.find("stand+jump") != std::string::npos
		|| en.find("jump") != std::string::npos;
	const bool walkEn = en.find("walk") != std::string::npos || en.find("silent") != std::string::npos;

	// Crouch combinations
	if (crouchCn || crouchEn) {
		if (jumpCn || jumpEn)
			return ThrowType::CrouchJump;
		if (walkCn || walkEn || runCn || runEn)
			return ThrowType::CrouchWalk;
		return ThrowType::Crouch;
	}

	// Directional jump throws
	if (jumpCn || jumpEn) {
		if (leftEn)
			return ThrowType::LeftJump;
		if (rightEn)
			return ThrowType::RightJump;
		if (backEn)
			return ThrowType::BackJump;
		if (wJump || runJumpCn || (runEn && en.find("stand+jump") == std::string::npos))
			return ThrowType::RunJump;
		if (walkEn || walkCn)
			return ThrowType::WalkJump;
		return ThrowType::Jump;
	}

	// Directional ground throws
	if (leftEn)
		return ThrowType::Left;
	if (rightEn)
		return ThrowType::Right;
	if (backEn)
		return ThrowType::Back;

	if (runCn || runEn)
		return ThrowType::Run;
	if (walkCn || walkEn)
		return ThrowType::Walk;
	return ThrowType::Stand;
}

// DMA Style field mapping varies between Chinese forks.
// Prioritize explicitly matched throw names (Jump, Run, W) over Style integers.
static ThrowType ResolveThrow(int stored, const std::
string& name) {
	const ThrowType fromStyle = ThrowFromDmaStyle(stored);
	const ThrowType fromName = ThrowFromName(name);
	
	if (fromName != ThrowType::
Stand)
		return fromName;
	
	return fromStyle;
}

// Set true while ingesting packs/ so Save never rewrites community data.
static bool g_ingestFromPack = false;

static bool PushLineupIfValid(Lineup& L) {
	if (!IsFiniteVec(L.pos) || !IsFiniteAng(L.aimAngles))
		return false;
	// Reject zero / near-zero feet (bad JSON / failed parse)
	if (std::
fabs(L.pos.x) < 1.f && std::
fabs(L.pos.y) < 1.f && std::
fabs(L.pos.z) < 1.f)
		return false;
	if (L.map.empty())
		return false;
	L.target = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);
	L.fromPack = g_ingestFromPack;
	g_lineups.push_back(std::
move(L));
	return true;
}

// Safe float from JSON number or numeric string (avoids .get throw on bad packs).
// 
// 
static float JsonFloat(const nlohmann::
json& v, float def = 0.f) {
	try {
		if (v.is_number_float()) return v.get<float>();
		if (v.is_number_integer()) return static_cast<float>(v.get<std::
int64_t>());
		if (v.is_string()) return std::
stof(v.get<std::
string>());
	} catch (...) {}
	return def;
}

// Games8Th native: { name,map,kind,throw,pos,ang }
// throw is ThrowType enum (0 Stand, 1 Jump, 2 Walk, 3 Run, 4 Crouch, 5 RunJump)
// - NOT DMA Style ints. Prefer stored throw; name only if throw missing/Stand.
// 
// 
static void ParseTempleEntry(const nlohmann::
json& e) {
	if (!e.is_object())
		return;
	Lineup L{};
	L.name = e.value("name", "Lineup");
	L.map = e.value("map", "");
	L.enabled = e.value("enabled", true);
	{
		const int k = e.value("kind", 0);
		L.kind = static_cast<NadeKind>(std::
clamp(k, 0, 5));
	}
	{
		const int thr = e.value("throw", 0);
		const ThrowType stored = ClampThrow(thr);
		const ThrowType fromName = ThrowFromName(L.name);
		// Own captures: trust throw field. Name upgrade only when throw==Stand.
		L.throwType = (stored != ThrowType::
Stand) ? stored
			: (fromName != ThrowType::
Stand ? fromName : ThrowType::
Stand);
	}
	L.inputs = e.value("inputs", ""); // legacy lineups -> empty, recipe hidden
	if (e.contains("pos") && e["pos"].is_array() && e["pos"].size() >= 3) {
		L.pos.x = JsonFloat(e["pos"][0]);
		L.pos.y = JsonFloat(e["pos"][1]);
		L.pos.z = JsonFloat(e["pos"][2]);
	}
	if (e.contains("ang") && e["ang"].is_array() && e["ang"].size() >= 2) {
		L.aimAngles.x = JsonFloat(e["ang"][0]);
		L.aimAngles.y = JsonFloat(e["ang"][1]);
		L.aimAngles.z = (e["ang"].size() >= 3) ? JsonFloat(e["ang"][2]) : 0.f;
	}
	PushLineupIfValid(L);
}

// CS2-DMA grenade-helper: { "infos":[ { Name,Style,Type,Position[],Angle[] } ] }
// 
// 
static void ParseDmaInfos(const nlohmann::
json& root, const std::
string& mapHint) {
	if (!root.contains("infos") || !root["infos"].is_array())
		return;
	for (const auto& e : root["infos"]) {
		if (!e.is_object())
			continue;
		Lineup L{};
		L.name = e.value("Name", e.value("name", "Lineup"));
		// Prefer entry map field if present; else filename hint
		L.map = e.value("Map", e.value("map", mapHint));
		if (L.map.empty())
			L.map = mapHint;
		L.kind = KindFromDmaType(e.value("Type", 1));
		// Style is DMA int 0..4 - ResolveThrow maps once (do NOT pre-map ThrowType)
		L.throwType = ResolveThrow(e.value("Style", 0), L.name);
		if (e.contains("Position") && e["Position"].is_array() && e["Position"].size() >= 3) {
			L.pos.x = JsonFloat(e["Position"][0]);
			L.pos.y = JsonFloat(e["Position"][1]);
			L.pos.z = JsonFloat(e["Position"][2]);
		}
		if (e.contains("Angle") && e["Angle"].is_array() && e["Angle"].size() >= 2) {
			L.aimAngles.x = JsonFloat(e["Angle"][0]);
			L.aimAngles.y = JsonFloat(e["Angle"][1]);
			L.aimAngles.z = 0.f;
		}
		PushLineupIfValid(L);
	}
}

// sapphyrus / AimTux style (often CS:GO coords - may be off on remade maps):
// { "csgo_grenades":[ { map,name,grenade,throwType,x,y,z,pitch,yaw } ] }
// 
// 
static void ParseSapphyrus(const nlohmann::
json& root) {
	const char* key = root.contains("csgo_grenades") ? "csgo_grenades"
		: (root.contains("grenades") ? "grenades" : nullptr);
	if (!key || !root[key].is_array())
		return;
	for (const auto& e : root[key]) {
		if (!e.is_object())
			continue;
		Lineup L{};
		L.name = e.value("name", "Lineup");
		L.map = e.value("map", "");
		L.kind = KindFromWeaponString(e.value("grenade", e.value("weapon", "")));
		// Sapphyrus throwType is a string - map to ThrowType, then name can only
		// upgrade Stand (same policy as Temple).
		{
			const ThrowType fromStr = ThrowFromString(
				e.value("throwType", e.value("throw_type", "NORMAL")));
			const ThrowType fromName = ThrowFromName(L.name);
			L.throwType = (fromStr != ThrowType::
Stand) ? fromStr
				: (fromName != ThrowType::
Stand ? fromName : ThrowType::
Stand);
		}
		try {
			L.pos.x = std::
stof(e.value("x", "0"));
			L.pos.y = std::
stof(e.value("y", "0"));
			L.pos.z = std::
stof(e.value("z", "0"));
			L.aimAngles.x = std::
stof(e.value("pitch", "0"));
			L.aimAngles.y = std::
stof(e.value("yaw", "0"));
			L.aimAngles.z = 0.f;
		} catch (...) {
			continue;
		}
		PushLineupIfValid(L);
	}
}

static void IngestJsonFile(const std::
filesystem::
path& path, const std::
string& mapHint) {
	std::
ifstream in(path);
	if (!in)
		return;
	nlohmann::
json j;
	try {
		in >> j;
	} catch (...) {
		return;
	}
	// packs/: accept DMA / sapphyrus / Temple. Main lineups.json is Temple array.
	if (j.is_array()) {
		for (const auto& e : j)
			ParseTempleEntry(e);
		return;
	}
	if (!j.is_object())
		return;
	if (j.contains("infos"))
		ParseDmaInfos(j, mapHint);
	else if (j.contains("csgo_grenades") || j.contains("grenades"))
		ParseSapphyrus(j);
	else if (j.contains("name") || j.contains("pos"))
		ParseTempleEntry(j);
}

static std::
string MapHintFromFilename(const std::
filesystem::
path& path) {
	std::
string stem = path.stem().string();
	// de_mirage.json / de_mirage_lineups.json -> de_mirage
	if (stem.rfind("de_", 0) == 0 || stem.rfind("cs_", 0) == 0) {
		const auto us = stem.find('_', 3);
		if (us != std::
string::
npos && us > 3) {
			// keep full de_mirage even if extra suffix after second token? prefer whole stem if starts de_
		}
		// strip common suffixes
		const char* suffixes[] = { "_lineups", "_grenades", "_helper", "-lineups" };
		for (const char* s : suffixes) {
			const size_t n = std::
strlen(s);
			if (stem.size() > n && _stricmp(stem.c_str() + stem.size() - n, s) == 0) {
				stem.resize(stem.size() - n);
				break;
			}
		}
		return stem;
	}
	return stem;
}

bool Load() {
	g_lineups.clear();
	g_currentIdx = -1;
	g_loaded = true;
	InvalidateMapIndex();

	auto path = LineupFilePath();
	bool fromLegacy = false;
	if (!std::
filesystem::
exists(path)) {
		const auto legacy = LegacyLineupFilePath();
		if (!legacy.empty() && std::
filesystem::
exists(legacy)) {
			path = legacy;
			fromLegacy = true;
		}
	}

	size_t mainCount = g_lineups.size();
	if (std::
filesystem::
exists(path))
		IngestJsonFile(path, "");
	mainCount = g_lineups.size();

	// Drop community packs next to lineups.json:
	// Documents/Games8Th/GrenadeHelpers/packs/de_mirage.json
	const auto packs = LineupFolder() / "packs";
	std::
error_code ec;
	if (std::
filesystem::
is_directory(packs, ec)) {
		g_ingestFromPack = true;
		for (const auto& ent : std::
filesystem::
directory_iterator(packs, ec)) {
			if (ec) break;
			if (!ent.is_regular_file())
				continue;
			const auto ext = ent.path().extension().string();
			if (_stricmp(ext.c_str(), ".json") != 0)
				continue;
			if (std::
filesystem::
equivalent(ent.path(), path, ec))
				continue;
			IngestJsonFile(ent.path(), MapHintFromFilename(ent.path()));
		}
		g_ingestFromPack = false;
	}

	// Collapse near-identical stand+angle copies (main file + packs overlap)
	{
		std::
vector<Lineup> uniq;
		uniq.reserve(g_lineups.size());
		constexpr float kPosEps = 8.f;
		constexpr float kAngEps = 1.5f;
		for (auto& L : g_lineups) {
			bool dup = false;
			for (const auto& U : uniq) {
				if (!MapsEqual(L.map, U.map.c_str()))
					continue;
				if (L.kind != U.kind)
					continue;
				if (L.throwType != U.throwType)
					continue;
				const float dx = L.pos.x - U.pos.x;
				const float dy = L.pos.y - U.pos.y;
				const float dz = L.pos.z - U.pos.z;
				if (dx * dx + dy * dy + dz * dz > kPosEps * kPosEps)
					continue;
				if (std::
fabs(L.aimAngles.x - U.aimAngles.x) > kAngEps)
					continue;
				if (std::
fabs(L.aimAngles.y - U.aimAngles.y) > kAngEps)
					continue;
				dup = true;
				break;
			}
			if (!dup)
				uniq.push_back(std::
move(L));
		}
		g_lineups.swap(uniq);
	}

	InvalidateMapIndex();
	if (!g_lineups.empty() && fromLegacy)
		Save();
	Con::
Ok("GrenadeHelper loaded %zu entries", g_lineups.size());
	return true;
}

void Update() {
	if (!Config::
grenade_helper) {
		if (g_cap.armed)
			g_cap = {};
		return;
	}
	// Map leave only. Death grace (EntityOk, 800ms) used to wipe armed
	// capture + hide pads every TDM death. Ready = pawn + join, no death wait.
	if (H::SessionMapLeaving() || !H::SessionEntityReady()) {
		if (g_cap.armed)
			g_cap = {};
		g_currentIdx = -1;
		return;
	}
	// Never reload every frame on empty list (disk thrash). Load once if not loaded.
	if (!g_loaded)
		Load();
	SyncMapCache();
	if (!g_mapCache[0]) {
		GameMode::
EnsureMap();
		SyncMapCache();
	}

	// Capture key: arm session (or cancel if already armed)
	const bool keyDown = Config::
grenade_helper_capture && keybind.isActive(Config::
grenade_helper_capture);
	if (keyDown && !g_captureKeyWasDown) {
		if (g_cap.armed)
			CancelCapture();
		else {
			ArmCapture(Config::
grenade_helper_capture_name,
				static_cast<NadeKind>(std::
clamp(Config::
grenade_helper_capture_kind, 0, 5)));
		}
	}
	g_captureKeyWasDown = keyDown;

	// Armed: wait for real grenade pin + release. Sample every frame (aim + motion).
	if (g_cap.armed) {
		const ULONGLONG now = GetTickCount64();
		if (now - g_cap.armMs > kCaptureTimeoutMs) {
			g_cap = {};
			Notify::
Warn("Lineup capture", "Timed out");
		} else if (C_CSPlayerPawn* local = GetLocalSeh()) {
			// Ignore residual LMB from Arm button / capture keybind chord
			const bool armGrace = (now - g_cap.armMs) < kArmIgnoreAtkMs;
			const bool wantMouse = ImGui::
GetCurrentContext()
				&& ImGui::
GetIO().WantCaptureMouse;
			const bool atkRaw = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			C_CSWeaponBase* wep = GetWeaponSeh(local);
			const NadeKind heldNow = ClassifyHeld(wep);
			const bool holdingNade = heldNow != NadeKind::
Any;

			bool pinPulled = false;
			bool pinHeldByPlayer = false;
			float throwTime = 0.f;
			int clip = -1;
			if (holdingNade) {
				ReadGrenadePinState(wep, pinPulled, pinHeldByPlayer);
				throwTime = ReadGrenadeThrowTime(wep);
				clip = SehReadClip(wep);
			}

			// Bind throws never press a physical mouse - pin is the attack signal.
			// Never read grenade offsets on knife/gun (garbage after throw/switch).
			const bool pinning = holdingNade && !armGrace && !wantMouse
				&& ((pinPulled && pinHeldByPlayer) || atkRaw);

			if (pinning) {
				g_cap.sawAttack = true;
				if (!g_cap.attackHeld) {
					g_cap.attackHeld = true;
					g_cap.attackDownMs = now;
					g_cap.cookWep = wep;
					g_cap.cookClip = clip;
					g_cap.cookThrowBaseline = throwTime;
					g_cap.throwCommitted = false;
					g_cap.pinReleaseMs = 0;
				}
				if (g_cap.kind == NadeKind::
Any)
					g_cap.kind = heldNow;
			}

			const bool sameCook = g_cap.cookWep && wep == g_cap.cookWep && holdingNade;
			const bool throwTimeFresh = sameCook && throwTime > 0.f
				&& throwTime != g_cap.cookThrowBaseline;
			const bool clipDropped = sameCook && g_cap.cookClip > 0 && clip >= 0
				&& clip < g_cap.cookClip;
			if (g_cap.attackHeld && (throwTimeFresh || clipDropped))
				g_cap.throwCommitted = true;

			if (g_cap.attackHeld && !g_cap.throwCommitted) {
				if (!sameCook) {
					// Pin already dropped on this nade then weapon left hand -> throw.
					// Pin still down + switch -> cancelled cook, stay armed.
					if (g_cap.pinReleaseMs != 0)
						g_cap.throwCommitted = true;
					else {
						ResetCookLatch();
						g_cap.sawAttack = false;
					}
				} else if (!pinPulled && !atkRaw) {
					if (g_cap.pinReleaseMs == 0)
						g_cap.pinReleaseMs = now;
					const ULONGLONG holdMs = (g_cap.attackDownMs != 0 && now >= g_cap.attackDownMs)
						? (now - g_cap.attackDownMs) : 0ull;
					const bool motionThrow = RecentMs(g_cap.lastJumpMs, now, kMotionRecentMs)
						|| RecentMs(g_cap.lastRunMs, now, kMotionRecentMs);
					const bool graceOk = g_cap.pinReleaseMs != 0
						&& (now - g_cap.pinReleaseMs) >= kThrowCommitGraceMs;
					if (graceOk && g_cap.sawAttack && !wantMouse
						&& (holdMs >= kMinAtkHoldMs || motionThrow))
						g_cap.throwCommitted = true;
				}
			}

			// After release/commit detection - the released gate inside must see
			// pinReleaseMs on the exact drop frame, before it can re-sample aim.
			SampleCaptureMotion(local);

			if (g_cap.throwCommitted && !wantMouse)
				FinishArmedCapture(local);
		} else {
			// Local gone (leave / death) - abort capture, don't hang armed.
			g_cap = {};
		}
	}

	g_currentIdx = -1;
	if (!g_mapCache[0])
		return;

	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return;

	Vector_t origin{};
	if (!Bones::
GetOrigin(local, origin) || !Bones::
IsValidPos(origin))
		return;

	NadeKind held = ClassifyHeld(GetWeaponSeh(local));
	if (Config::
grenade_helper_only_held && held == NadeKind::
Any)
		return; // not holding a nade

	EnsureMapIndex();
	
	QAngle_t viewAng{};
	GetLocalViewAngles(viewAng);

	// Clamp select radius so bad configs don't scan half the map every frame
	const float selectR = std::
clamp(Config::
grenade_helper_select_dist, 50.f, 2000.f);
	const float aimPreviewR2 = kAimPreviewR * kAimPreviewR;
	
	float bestDist2 = selectR * selectR;
	int bestIdxDist = -1;
	float bestDistKind = selectR * selectR;
	int bestIdxDistKind = -1;
	
	float bestAngDist = FLT_MAX;
	int bestIdxAng = -1;

	for (int idx : g_mapIndices) {
		if (idx < 0 || idx >= static_cast<int>(g_lineups.size()))
			continue;
		const Lineup& L = g_lineups[static_cast<size_t>(idx)];
		if (Config::grenade_helper_only_held && !KindMatches(L.kind, held))
			continue;
		const float dx = origin.x - L.pos.x;
		const float dy = origin.y - L.pos.y;
		const float dz = origin.z - L.pos.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		const float fxy2 = dx * dx + dy * dy;
		const bool kindPref = (held == NadeKind::Any)
			|| L.kind == NadeKind::Any || L.kind == held;
		
		if (d2 < bestDist2) {
			bestDist2 = d2;
			bestIdxDist = idx;
		}
		if (kindPref && d2 < bestDistKind) {
			bestDistKind = d2;
			bestIdxDistKind = idx;
		}
		
		// If standing on pad, prioritize by crosshair proximity (matching nade first)
		if (fxy2 <= aimPreviewR2) {
			float pitchDiff = viewAng.x - L.aimAngles.x;
			float yawDiff = viewAng.y - L.aimAngles.y;
			while (yawDiff > 180.f) yawDiff -= 360.f;
			while (yawDiff < -180.f) yawDiff += 360.f;
			float angDist = pitchDiff * pitchDiff + yawDiff * yawDiff;
			if (!kindPref)
				angDist += 10000.f;
			if (angDist < bestAngDist) {
				bestAngDist = angDist;
				bestIdxAng = idx;
			}
		}
	}
	
	g_currentIdx = (bestIdxAng != -1) ? bestIdxAng
		: (bestIdxDistKind != -1 ? bestIdxDistKind : bestIdxDist);
}

void Draw(const ViewMatrix& vm) {
	if (!Config::
grenade_helper)
		return;
	if (H::SessionMapLeaving() || !H::SessionEntityReady())
		return;

	// Capturing banner - soft stone pill, muted warm accent
	if (g_cap.armed) {
		ImDrawList* hud = ImGui::
GetForegroundDrawList();
		if (hud) {
			const ImVec2 ds = ImGui::
GetIO().DisplaySize;
			const ULONGLONG leftMs = (g_cap.armMs + kCaptureTimeoutMs > GetTickCount64())
				? (g_cap.armMs + kCaptureTimeoutMs - GetTickCount64()) : 0ull;
			const float leftSec = static_cast<float>(leftMs) * 0.001f;
			char line[160]{};
			std::
snprintf(line, sizeof(line),
				"Capture  %s  ?  %s  ?  throw to save  ?  %.0fs",
				g_cap.name[0] ? g_cap.name : "Lineup",
				KindName(g_cap.kind), leftSec);
			// Live recipe so multi-input throws are visible while capturing
			char recipe[96]{};
			BuildInputRecipe(g_cap, recipe, sizeof(recipe));
			char line2[224]{};
			if (recipe[0])
				std::
snprintf(line2, sizeof(line2), "%s   ?   %s", line, recipe);
			else
				std::
snprintf(line2, sizeof(line2), "%s", line);
			const ImVec2 ts = ImGui::
CalcTextSize(line2);
			const float padL = 22.f, padR = 14.f, padY = 7.f;
			const float totalW = ts.x + padL + padR;
			const ImVec2 min{ floorf(ds.x * 0.5f - totalW * 0.5f), 30.f };
			const ImVec2 max{ min.x + totalW, min.y + ts.y + padY * 2.f };
			const float rr = (max.y - min.y) * 0.5f;
			DrawPill(hud, min, max, IM_COL32(16, 18, 23, 234), IM_COL32(255, 255, 255, 34), rr);
			hud->AddCircleFilled(ImVec2(min.x + 11.f, (min.y + max.y) * 0.5f), 2.8f,
				IM_COL32(120, 190, 255, 230), 10);
			hud->AddCircleFilled(ImVec2(min.x + 10.6f, (min.y + max.y) * 0.5f - 0.6f), 1.1f,
				IM_COL32(255, 255, 255, 180), 8);
			hud->AddText(ImVec2(min.x + padL, min.y + padY), IM_COL32(238, 242, 248, 255), line2);
			if (vm.viewMatrix && IsFiniteVec(g_cap.pos)) {
				Vector_t s{};
				if (vm.WorldToScreen(g_cap.pos, s)) {
					const ImVec2 sp{ s.x, s.y };
					hud->AddCircleFilled(sp, 9.5f, IM_COL32(0, 0, 0, 110), 24);
					hud->AddCircle(sp, 8.f, IM_COL32(120, 190, 255, 210), 24, 1.4f);
					hud->AddCircle(sp, 8.f, IM_COL32(255, 255, 255, 40), 24, 1.0f);
					hud->AddCircleFilled(sp, 2.0f, IM_COL32(120, 190, 255, 235), 12);
					hud->AddCircleFilled(ImVec2(sp.x - 0.6f, sp.y - 0.7f), 0.7f,
						IM_COL32(255, 255, 255, 180), 8);
					const Vector_t aimW = AimWorldFromStand(g_cap.pos, g_cap.ang, kAimDrawDist);
					Vector_t as{};
					if (vm.WorldToScreen(aimW, as)) {
						hud->AddLine(sp, ImVec2(as.x, as.y), IM_COL32(0, 0, 0, 100), 1.5f);
						hud->AddLine(sp, ImVec2(as.x, as.y), IM_COL32(120, 190, 255, 95), 0.95f);
						hud->AddCircle(ImVec2(as.x, as.y), 3.5f, IM_COL32(120, 190, 255, 200), 16, 1.15f);
						hud->AddCircleFilled(ImVec2(as.x, as.y), 1.2f, IM_COL32(248, 248, 250, 220), 8);
					}
				}
			}
		}
	}

	if (!vm.viewMatrix)
		return;
	if (!g_loaded)
		Load();
	if (g_lineups.empty())
		return;
	SyncMapCache();
	if (!g_mapCache[0]) {
		GameMode::
EnsureMap();
		SyncMapCache();
	}
	if (!g_mapCache[0])
		return;

	ImDrawList* dl = ImGui::
GetBackgroundDrawList();
	if (!dl)
		return;

	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return;

	Vector_t origin{};
	if (!Bones::
GetOrigin(local, origin) || !Bones::
IsValidPos(origin))
		return;

	Vector_t eye = Bones::
GetEyePos(local);
	if (!Bones::
IsValidPos(eye))
		eye = origin;
	QAngle_t viewAng{};
	if (!GetLocalViewAngles(viewAng))
		viewAng = {};

	NadeKind held = NadeKind::
Any;
	if (Config::
grenade_helper_only_held) {
		held = ClassifyHeld(GetWeaponSeh(local));
		if (held == NadeKind::
Any)
			return;
	}

	EnsureMapIndex();
	if (g_mapIndices.empty())
		return;

	const float standDist = std::
clamp(Config::
grenade_helper_stand_dist, 50.f, 2500.f);
	// Perf: ProjectOrEdge per marker. Full pad only on nearest few.
	// Looking away -> edge arrow (not drop).
	constexpr int kMaxDraw = 24;
	constexpr int kMaxDetailed = 3;
	constexpr int kMaxLabels = 5;

	struct Cluster {
		int indices[8];
		int count;
		Vector_t pos;
		float distSq;
	};
	Cluster clusters[64];
	int nCluster = 0;
	const float standDistSq = standDist * standDist;

	for (int idx : g_mapIndices) {
		if (idx < 0 || idx >= static_cast<int>(g_lineups.size()))
			continue;
		const Lineup& L = g_lineups[static_cast<size_t>(idx)];
		if (!L.enabled)
			continue;
		if (!KindMatches(L.kind, held))
			continue;
		if (!IsFiniteVec(L.pos) || !IsFiniteAng(L.aimAngles))
			continue;
		const float dx = origin.x - L.pos.x;
		const float dy = origin.y - L.pos.y;
		const float dz = origin.z - L.pos.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > standDistSq)
			continue;
		
		bool merged = false;
		for (int c = 0; c < nCluster; ++c) {
			const float cdx = clusters[c].pos.x - L.pos.x;
			const float cdy = clusters[c].pos.y - L.pos.y;
			const float cdz = clusters[c].pos.z - L.pos.z;
			if (cdx * cdx + cdy * cdy + cdz * cdz < 16.f) { // 4 units
				if (clusters[c].count < 8)
					clusters[c].indices[clusters[c].count++] = idx;
				merged = true;
				break;
			}
		}
		if (merged) continue;
		
		if (nCluster < 64) {
			clusters[nCluster].indices[0] = idx;
			clusters[nCluster].count = 1;
			clusters[nCluster].pos = L.pos;
			clusters[nCluster].distSq = d2;
			nCluster++;
		} else {
			int worst = 0;
			for (int i = 1; i < nCluster; ++i) {
				if (clusters[i].distSq > clusters[worst].distSq)
					worst = i;
			}
			if (d2 < clusters[worst].distSq) {
				clusters[worst].indices[0] = idx;
				clusters[worst].count = 1;
				clusters[worst].pos = L.pos;
				clusters[worst].distSq = d2;
			}
		}
	}
	if (nCluster == 0)
		return;

	// Partial selection sort - nearest first
	const int drawN = (std::
min)(nCluster, kMaxDraw);
	for (int i = 0; i < drawN; ++i) {
		int best = i;
		for (int j = i + 1; j < nCluster; ++j) {
			if (clusters[j].distSq < clusters[best].distSq)
				best = j;
		}
		if (best != i) {
			const Cluster tmp = clusters[i];
			clusters[i] = clusters[best];
			clusters[best] = tmp;
		}
	}

	const ImVec4 standCol = Config::
grenade_helper_color;
	const ImVec4 aimCol = Config::
grenade_helper_aim_color;
	const ImVec2 disp = ImGui::
GetIO().DisplaySize;
	const ImVec2 screenCenter{ disp.x * 0.5f, disp.y * 0.5f };

	// Config aim_dist: how close feet must be for full aim reticle (clamped)
	const float aimLockR = std::
clamp(Config::
grenade_helper_aim_dist, kAimExactR, 40.f);
	const float aimLockR2 = aimLockR * aimLockR;
	const float aimPreviewR2 = kAimPreviewR * kAimPreviewR;
	const float padR2 = kCircleRadius * kCircleRadius;

	for (int rank = 0; rank < drawN; ++rank) {
		const Cluster& cl = clusters[rank];
		const float dist = std::
sqrt(cl.distSq);
		
		int bestLi = cl.indices[0];
		bool isCurrent = false;
		bool onExact = false;
		bool onPad = false;
		bool onPreview = false;
		
		for (int i = 0; i < cl.count; ++i) {
			const Lineup& lTmp = g_lineups[static_cast<size_t>(cl.indices[i])];
			const float fdx = origin.x - lTmp.pos.x;
			const float fdy = origin.y - lTmp.pos.y;
			const float fxy2 = fdx * fdx + fdy * fdy;
			if (fxy2 <= padR2) onPad = true;
			if (fxy2 <= aimLockR2) onExact = true;
			if (fxy2 <= aimPreviewR2) onPreview = true;
			if (g_currentIdx == cl.indices[i]) {
				isCurrent = true;
				bestLi = cl.indices[i];
			}
		}
		
		const Lineup& L = g_lineups[static_cast<size_t>(bestLi)];
		const bool detailed = rank < kMaxDetailed || onPad || isCurrent;

		const float rawA = 1.f - (dist / standDist);
		float standA = std::
clamp(rawA * 1.55f, 0.f, 1.f);
		if (isCurrent)
			standA = (std::
min)(1.f, standA + 0.18f);
		if (onExact)
			standA = (std::
min)(1.f, standA + 0.12f);

		ImVec2 sp{};
		bool onScreen = false;
		if (!ProjectOrEdge(vm, L.pos, sp, onScreen, eye, viewAng))
			continue;

		// Looking away / feet under camera - edge arrow
		if (!onScreen) {
			const char* hintName = (cl.count > 1) ? "Multiple Lineups" : L.name.c_str();
			DrawOffscreenHint(dl, sp, screenCenter, standA * 0.92f,
				isCurrent || onExact ? aimCol : standCol,
				(rank < kMaxLabels || isCurrent || onPad) ? hintName : nullptr,
				dist);
		} else {
			const ImVec4 iconAccent = onExact || isCurrent ? aimCol : standCol;
			const bool hasIcon = L.kind != NadeKind::
Any;

			if (detailed) {
				const ImU32 ring = ColA(standCol, standA * (onPad ? 0.88f : 0.48f));
				const ImU32 fill = onExact
					? ColA(standCol, standA * 0.07f)
					: (onPad ? ColA(standCol, standA * 0.03f) : IM_COL32(0, 0, 0, 0));
				DrawStandPad(dl, vm, L.pos, kCircleRadius, ring, fill,
					onPad ? 28 : 18, onExact || isCurrent || onPad);
			} else if (!hasIcon) {
				dl->AddCircleFilled(sp, 3.8f, Ui::
kInk, 12);
				dl->AddCircle(sp, 3.5f, ColA(standCol, standA * 0.60f), 12, 1.05f);
			}

			if (hasIcon) {
				const float iconPx = detailed ? (onExact ? 17.f : (onPad ? 15.f : 13.f)) : 12.f;
				DrawKindIcon(dl, sp, L.kind, standA * (detailed ? 0.96f : 0.72f),
					iconPx, iconAccent, onExact || isCurrent);
			} else {
				DrawFeetCross(dl, sp, onExact ? 2.4f : 1.9f,
					ColA(iconAccent, standA),
					ColRGB(0, 0, 0, standA * 0.35f));
			}

			if (rank < kMaxLabels || onPad || isCurrent) {
				LabelRow rows[8];
				char metas[8][128]; // active row can carry the input recipe
				const float meters = dist * kUnitsToM;
				
				for (int i = 0; i < cl.count; ++i) {
					const Lineup& liTmp = g_lineups[static_cast<size_t>(cl.indices[i])];
					bool rowActive = (g_currentIdx == cl.indices[i]) || (onExact && cl.count == 1);

					// Recipe only on the active row - far-lineup chips stay short.
					if (rowActive && !liTmp.inputs.empty()) {
						std::
snprintf(metas[i], sizeof(metas[i]), "%s ? %s",
							ThrowName(liTmp.throwType), liTmp.inputs.c_str());
					}
					else if (meters >= 1.f)
						std::
snprintf(metas[i], sizeof(metas[i]), "%s  ?  %.0fm", ThrowName(liTmp.throwType), meters);
					else
						std::
snprintf(metas[i], sizeof(metas[i]), "%s", ThrowName(liTmp.throwType));

					rows[i].title = liTmp.name.c_str();
					rows[i].meta = metas[i];
					rows[i].active = rowActive;
					rows[i].accent = iconAccent;
				}
				DrawMultiLabelChip(dl, sp, rows, cl.count, standA);
			}
		}

		// Aim: full reticle on exact lock; dim preview when on pad
		if (!onPreview)
			continue;

		for (int i = 0; i < cl.count; ++i) {
			const Lineup& liTmp = g_lineups[static_cast<size_t>(cl.indices[i])];
			const Vector_t aimWorld = AimWorldFromStand(liTmp.pos, liTmp.aimAngles, kAimDrawDist);
			ImVec2 screenAim{};
			bool aimOn = false;
			if (!ProjectOrEdge(vm, aimWorld, screenAim, aimOn, eye, viewAng) || !aimOn)
				continue;

			bool isAimExact = (g_currentIdx == cl.indices[i]) && onExact;
			const float aimA = isAimExact ? 1.f : 0.65f;
			DrawAimReticle(dl, screenAim, aimA, Config::
grenade_helper_aim_color, isAimExact,
				ThrowName(liTmp.throwType), KindName(liTmp.kind));
			if (onScreen)
				DrawGuideLine(dl, sp, screenAim, ColA(aimCol, isAimExact ? 0.38f : 0.20f), isAimExact);
		}
	}
}

} // namespace GrenadeHelper


