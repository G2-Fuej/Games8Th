#include "notify.h"

#include "../../utils/console/console.h"
#include "../../../../external/imgui/imgui.h"
#include "../../config/config.h"
#include "../../utils/memory/patternscan/patternscan.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Notify {
namespace {

// Compact glossy glass pill - bottom-right stack.
constexpr int   kMaxToasts   = 6;
constexpr float kDefaultLife = 3.2f;
constexpr float kAnimIn      = 0.12f;
constexpr float kAnimOut     = 0.16f;
constexpr float kToastMinW   = 168.f;
constexpr float kToastMaxW   = 228.f;
constexpr float kPadX        = 8.f;
constexpr float kPadY        = 4.f;
constexpr float kGap         = 3.f;
constexpr float kMargin      = 12.f;
constexpr float kTopClear    = 48.f;
constexpr float kDotR        = 2.6f;
constexpr float kTitleMsgGap = 0.f;
constexpr float kBarH        = 1.25f;
constexpr float kStackLerp   = 22.f;
constexpr float kDedupeWin   = 0.55f;

struct Toast {
	std::
string title;
	std::
string message;
	Type type = Type::
Info;
	float born = 0.f;
	float life = kDefaultLife;
	float layoutY = -1.f;
	int   count = 1;
};

std::
vector<Toast> g_toasts;
static SRWLOCK g_toastLock = SRWLOCK_INIT;

float DefaultLife(Type t) {
	switch (t) {
	case Type::
Error:   return 4.2f;
	case Type::
Warn:    return 3.8f;
	case Type::
Success: return 3.0f;
	case Type::
Info:
	default:            return kDefaultLife;
	}
}

ImVec4 MenuAccent() {
	return Config::menu_accent;
}

// Semantic override only - Info (and chrome) follow Config::menu_accent like
// watermark / bomb. Success / Warn / Error match bomb's urgent/defused path.
ImVec4 TypeColor(Type t) {
	switch (t) {
	case Type::Success: return ImVec4(0.38f, 0.82f, 0.58f, 1.f);
	case Type::Warn:    return ImVec4(0.92f, 0.70f, 0.32f, 1.f);
	case Type::Error:   return ImVec4(0.92f, 0.38f, 0.42f, 1.f);
	case Type::Info:
	default:            return MenuAccent();
	}
}

ImU32 ToU32(ImVec4 c) {
	return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 WithA(ImVec4 c, float a) {
	c.w = std::clamp(c.w * a, 0.f, 1.f);
	return ToU32(c);
}

ImU32 PlateColor(float a) {
	const float op = std::clamp(Config::menu_opacity, 0.55f, 1.f);
	ImVec4 bg = Config::menu_bg;
	const int alpha = static_cast<int>(std::clamp(
		(bg.w > 0.05f ? bg.w : 1.f) * (200.f * op + 30.f) * a, 160.f, 240.f));
	bg.w = alpha / 255.f;
	return ToU32(bg);
}

ImU32 BorderColor(float a) {
	ImVec4 b = Config::menu_border;
	if (b.w < 0.04f)
		b = ImVec4(1.f, 1.f, 1.f, 0.07f);
	b.w = std::clamp(b.w * a, 0.f, 1.f);
	return ToU32(b);
}

float EaseOutCubic(float t) {
	t = std::
clamp(t, 0.f, 1.f);
	const float u = 1.f - t;
	return 1.f - u * u * u;
}

float AnimIn(const Toast& t)  { return (std::
min)(kAnimIn,  t.life * 0.28f); }
float AnimOut(const Toast& t) { return (std::
min)(kAnimOut, t.life * 0.32f); }

float ToastAlpha(const Toast& toast, float now) {
	const float age = now - toast.born;
	const float ain = AnimIn(toast);
	const float aout = AnimOut(toast);
	if (age < 0.f)
		return 0.f;
	if (age < ain)
		return EaseOutCubic(age / (std::
max)(0.001f, ain));
	if (age > toast.life - aout) {
		const float t = (toast.life - age) / (std::
max)(0.001f, aout);
		return EaseOutCubic(std::
clamp(t, 0.f, 1.f));
	}
	return 1.f;
}

float ToastSlideX(const Toast& toast, float now) {
	const float age = now - toast.born;
	const float ain = AnimIn(toast);
	const float aout = AnimOut(toast);
	if (age < ain) {
		const float e = EaseOutCubic(age / (std::
max)(0.001f, ain));
		return (1.f - e) * 18.f;
	}
	if (age > toast.life - aout) {
		const float t = 1.f - std::
clamp((toast.life - age) / (std::
max)(0.001f, aout), 0.f, 1.f);
		return EaseOutCubic(t) * 12.f;
	}
	return 0.f;
}

float ToastWidth() {
	const float sw = ImGui::
GetIO().DisplaySize.x;
	return std::
clamp(sw * 0.17f, kToastMinW, kToastMaxW);
}

float ContentLeft(float toastX) {
	return toastX + kPadX + kDotR * 2.f + 6.f;
}

float ContentWidth(float toastW) {
	return toastW - kPadX * 2.f - kDotR * 2.f - 6.f;
}

float Rounding() {
	return std::clamp(Config::menu_rounding, 2.f, 8.f);
}

ImVec4 TextMuted() {
	ImVec4 m = Config::menu_text_muted;
	m.w = 1.f;
	return m;
}

float MeasureHeight(const Toast& toast, float toastW) {
	const float lineH = ImGui::
GetFontSize();
	const float contentW = ContentWidth(toastW);

	float h = kPadY + lineH;

	if (!toast.message.empty()) {
		const float wrapW = (std::
max)(40.f, contentW);
		const ImVec2 ms = ImGui::
CalcTextSize(toast.message.c_str(), nullptr, false, wrapW);
		h += kTitleMsgGap + ms.y;
	}

	h += kPadY + kBarH;
	return h;
}

const char* FitTitle(const char* title, float maxW, std::
string& scratch) {
	if (maxW < 8.f)
		return "";
	if (!title || !title[0])
		return "";
	ImVec2 ts = ImGui::
CalcTextSize(title);
	if (ts.x <= maxW)
		return title;

	const size_t len = std::
strlen(title);
	size_t lo = 0, hi = len;
	while (lo < hi) {
		const size_t mid = (lo + hi + 1) / 2;
		scratch.assign(title, mid);
		scratch += "...";
		if (ImGui::
CalcTextSize(scratch.c_str()).x <= maxW)
			lo = mid;
		else
			hi = mid - 1;
	}
	if (lo == 0) {
		scratch = "...";
		return scratch.c_str();
	}
	scratch.assign(title, lo);
	scratch += "...";
	return scratch.c_str();
}

} // namespace

void Push(const char* title, const char* message, Type type, float durationSec) {
	if (!title || !title[0])
		return;

	const float now = static_cast<float>(ImGui::
GetTime());
	const char* msg = (message && message[0]) ? message : "";

	AcquireSRWLockExclusive(&g_toastLock);
	for (auto it = g_toasts.rbegin(); it != g_toasts.rend(); ++it) {
		if (it->type != type)
			continue;
		if (it->title != title)
			continue;
		if (it->message != msg)
			continue;
		if ((now - it->born) > kDedupeWin)
			break;
		it->count = (std::
min)(it->count + 1, 99);
		it->born = now;
		it->life = (std::
max)(0.85f, durationSec > 0.1f ? durationSec : DefaultLife(type));
		ReleaseSRWLockExclusive(&g_toastLock);
		return;
	}

	Toast t;
	t.title = title;
	t.message = msg;
	t.type = type;
	t.born = now;
	t.life = (std::
max)(0.85f, durationSec > 0.1f ? durationSec : DefaultLife(type));
	t.layoutY = -1.f;
	t.count = 1;

	g_toasts.push_back(std::
move(t));
	while (static_cast<int>(g_toasts.size()) > kMaxToasts)
		g_toasts.erase(g_toasts.begin());
	ReleaseSRWLockExclusive(&g_toastLock);
}

void PushFmt(Type type, const char* title, const char* fmt, ...) {
	char buf[384]{};
	if (fmt && fmt[0]) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
	}
	Push(title, buf[0] ? buf : nullptr, type);
}

bool HasPending() {
	AcquireSRWLockShared(&g_toastLock);
	bool r = !g_toasts.empty();
	ReleaseSRWLockShared(&g_toastLock);
	return r;
}

void Clear() {
	AcquireSRWLockExclusive(&g_toastLock);
	g_toasts.clear();
	ReleaseSRWLockExclusive(&g_toastLock);
}

void Render() {
	AcquireSRWLockExclusive(&g_toastLock);
	if (g_toasts.empty()) { ReleaseSRWLockExclusive(&g_toastLock); return; }

	const float now = static_cast<float>(ImGui::
GetTime());
	const float dt = ImGui::
GetIO().DeltaTime;

	g_toasts.erase(
		std::
remove_if(g_toasts.begin(), g_toasts.end(),
			[now](const Toast& t) { return (now - t.born) >= t.life; }),
		g_toasts.end());
	if (g_toasts.empty()) { ReleaseSRWLockExclusive(&g_toastLock); return; }

	ImDrawList* dl = ImGui::
GetForegroundDrawList();
	const ImVec2 ds = ImGui::
GetIO().DisplaySize;
	const float toastW = ToastWidth();
	const float r = Rounding();
	const float lineH = ImGui::GetFontSize();
	const ImVec4 muted = TextMuted();

	float stackBottom = ds.y - kMargin;
	std::
vector<float> targetTops(g_toasts.size(), 0.f);

	for (int i = static_cast<int>(g_toasts.size()) - 1; i >= 0; --i) {
		const float h = MeasureHeight(g_toasts[static_cast<size_t>(i)], toastW);
		targetTops[static_cast<size_t>(i)] = stackBottom - h;
		stackBottom = targetTops[static_cast<size_t>(i)] - kGap;
	}

	if (!targetTops.empty() && targetTops.front() < kTopClear) {
		const float shift = kTopClear - targetTops.front();
		for (float& t : targetTops)
			t += shift;
	}

	for (size_t i = 0; i < g_toasts.size(); ++i) {
		Toast& toast = g_toasts[i];
		const float target = targetTops[i];
		if (toast.layoutY < 0.f)
			toast.layoutY = target + 8.f;
		else {
			const float k = 1.f - std::
exp(-kStackLerp * dt);
			toast.layoutY += (target - toast.layoutY) * k;
		}

		const float a = ToastAlpha(toast, now);
		if (a <= 0.01f)
			continue;

		const float h = MeasureHeight(toast, toastW);
		const float slide = ToastSlideX(toast, now);
		const float x = ds.x - kMargin - toastW + slide;
		const float y = toast.layoutY;

		const ImVec4 typeCol = TypeColor(toast.type);
		const ImVec4 accent = MenuAccent();
		const ImVec2 p0(floorf(x), floorf(y));
		const ImVec2 p1(p0.x + toastW, p0.y + h);

		dl->AddRectFilled(
			ImVec2(p0.x + 1.f, p0.y + 2.f),
			ImVec2(p1.x + 1.f, p1.y + 2.f),
			IM_COL32(0, 0, 0, static_cast<int>(90.f * a)), r);
		dl->AddRectFilled(p0, p1, PlateColor(a), r);
		{
			const float sheenH = (std::min)(h * 0.48f, 12.f);
			dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + sheenH),
				IM_COL32(255, 255, 255, static_cast<int>(20.f * a)), r,
				ImDrawFlags_RoundCornersTop);
		}
		dl->AddRect(p0, p1, BorderColor(a), r, 0, 1.f);

		const float dotX = p0.x + kPadX + kDotR;
		const float dotY = p0.y + kPadY + lineH * 0.5f;
		dl->AddCircleFilled(ImVec2(dotX, dotY), kDotR, WithA(typeCol, a), 12);

		const float contentX = ContentLeft(p0.x);
		const float contentW = ContentWidth(toastW);
		float textY = floorf(p0.y + kPadY);

		float countW = 0.f;
		if (toast.count > 1) {
			char cnt[8]{};
			std::snprintf(cnt, sizeof(cnt), "x%d", toast.count);
			const ImVec2 cs = ImGui::CalcTextSize(cnt);
			countW = cs.x;
			dl->AddText(
				ImVec2(floorf(p1.x - kPadX - cs.x), textY),
				WithA(muted, a * 0.85f), cnt);
		}

		{
			const float titleMaxW = contentW - (countW > 0.f ? countW + 8.f : 0.f);
			std::string scratch;
			const char* drawn = FitTitle(toast.title.c_str(), titleMaxW, scratch);
			dl->AddText(ImVec2(floorf(contentX), textY), WithA(accent, a), drawn);
		}

		textY += lineH;

		if (!toast.message.empty()) {
			textY += kTitleMsgGap;
			const float wrapW = (std::max)(40.f, contentW);
			dl->AddText(
				ImGui::GetFont(), ImGui::GetFontSize(),
				ImVec2(floorf(contentX), floorf(textY)),
				WithA(muted, a * 0.82f),
				toast.message.c_str(), nullptr, wrapW);
		}

		const float lifeT = std::clamp((now - toast.born) / toast.life, 0.f, 1.f);
		const float barY = p1.y - kBarH;
		const float inset = 6.f;
		const float fullW = toastW - inset * 2.f;
		const float barW = fullW * (1.f - lifeT);
		dl->AddRectFilled(
			ImVec2(p0.x + inset, barY),
			ImVec2(p1.x - inset, p1.y),
			IM_COL32(255, 255, 255, static_cast<int>(16.f * a)), 1.f);
		if (barW > 0.6f) {
			dl->AddRectFilled(
				ImVec2(p0.x + inset, barY),
				ImVec2(p0.x + inset + barW, p1.y),
				WithA(accent, a * 0.85f), 1.f);
		}
	}
	ReleaseSRWLockExclusive(&g_toastLock);
}

namespace {

void SehHudChat(uintptr_t fn, const char* msg) {
	if (!fn || !msg)
		return;
	__try {
		using Fn = void(__fastcall*)(void*, int, const char*, ...);
		reinterpret_cast<Fn>(fn)(nullptr, 0, "%s", msg);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

} // namespace

void ChatPrintf(const char* fmt, ...) {
	static uintptr_t s_fn = 0;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		s_fn = M::
patternScan("client",
			"4C 89 44 24 18 4C 89 4C 24 20 53 B8 40 10 00 00");
		if (!s_fn)
			s_fn = M::
patternScan("client.dll",
				"4C 89 44 24 18 4C 89 4C 24 20 53 B8 40 10 00 00");
	}
	if (!s_fn || !fmt)
		return;
	char buf[512]{};
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	SehHudChat(s_fn, buf);
}

using FnConMsg = void(__cdecl*)(const char* fmt, ...);
using FnConColorMsg = void(__cdecl*)(const void* color, const char* fmt, ...);
using FnMsg = void(__cdecl*)(const char* fmt, ...);
using FnFindChannel = int(__cdecl*)(const char* name);
using FnSetChannelVerbosity = void(__cdecl*)(int channel, int verbosity);
// IDA tier0 LoggingSystem_RegisterLoggingChannel(name, fn, flags, verbosity, color)
// returns channel id; tag shown in ~ console as (name)
using FnRegisterChannel = int(__cdecl*)(const char* name, void* fn, int flags, int verbosity, int color);
using FnSetChannelColor = void(__cdecl*)(int channel, int color);
// LoggingSystem_Log(channel, severity, Color, fmt, ...)
using FnLogColor = int(__cdecl*)(int channel, int severity, int color, const char* fmt, ...);
using FnLogDirect3 = int(__cdecl*)(int channel, int severity, const char* text);
// LogDirect with Color: (ch, sev, Color, text)
using FnLogDirectColor = int(__cdecl*)(int channel, int severity, int color, const char* text);

// Brand channel name - appears as (Games8Th) instead of (Console)
constexpr const char* kBrandChannel = "Games8Th";
// LS_MESSAGE = 2 (same as prior logDirect severity)
constexpr int kLogMsg = 2;
// LS_HIGHEST verbosity so channel always prints
constexpr int kVerbosity = 2;

struct Tier0Console {
	FnConMsg conMsg = nullptr;
	FnConColorMsg conColorMsg = nullptr;
	FnMsg msg = nullptr;
	FnFindChannel findChannel = nullptr;
	FnSetChannelVerbosity setVerbosity = nullptr;
	FnRegisterChannel registerChannel = nullptr;
	FnSetChannelColor setChannelColor = nullptr;
	FnLogColor logColor = nullptr;
	FnLogDirect3 logDirect = nullptr;
	FnLogDirectColor logDirectColor = nullptr;
	int chBrand = -1;
	int chConsole = -1;
	int chGeneral = -1;
	int chDevConsole = -1;
	bool ready = false;
};

static int PackColor(int r, int g, int b, int a)
{
	r = std::
clamp(r, 0, 255);
	g = std::
clamp(g, 0, 255);
	b = std::
clamp(b, 0, 255);
	a = std::
clamp(a, 0, 255);
	// Color as int RGBA little-endian (matches SetChannelColor dword write)
	return (a << 24) | (b << 16) | (g << 8) | r;
}

static Tier0Console& GetTier0Console()
{
	static Tier0Console t{};
	if (t.ready)
		return t;

	HMODULE tier0 = GetModuleHandleA("tier0.dll");
	if (!tier0)
		tier0 = LoadLibraryA("tier0.dll");
	if (!tier0)
		return t;

	t.conMsg = reinterpret_cast<FnConMsg>(
		GetProcAddress(tier0, "?ConMsg@@YAXPEBDZZ"));
	if (!t.conMsg)
		t.conMsg = reinterpret_cast<FnConMsg>(GetProcAddress(tier0, "ConMsg"));

	t.conColorMsg = reinterpret_cast<FnConColorMsg>(
		GetProcAddress(tier0, "?ConColorMsg@@YAXAEBVColor@@PEBDZZ"));

	t.msg = reinterpret_cast<FnMsg>(GetProcAddress(tier0, "Msg"));

	t.findChannel = reinterpret_cast<FnFindChannel>(
		GetProcAddress(tier0, "LoggingSystem_FindChannel"));
	t.setVerbosity = reinterpret_cast<FnSetChannelVerbosity>(
		GetProcAddress(tier0, "LoggingSystem_SetChannelVerbosity"));
	t.registerChannel = reinterpret_cast<FnRegisterChannel>(
		GetProcAddress(tier0, "LoggingSystem_RegisterLoggingChannel"));
	t.setChannelColor = reinterpret_cast<FnSetChannelColor>(
		GetProcAddress(tier0, "LoggingSystem_SetChannelColor"));
	// Prefer Color overload: Log(ch, sev, Color, fmt, ...)
	t.logColor = reinterpret_cast<FnLogColor>(GetProcAddress(tier0,
		"?LoggingSystem_Log@@YA?AW4LoggingResponse_t@@HW4LoggingSeverity_t@@VColor@@PEBDZZ"));
	t.logDirect = reinterpret_cast<FnLogDirect3>(
		GetProcAddress(tier0, "LoggingSystem_LogDirect"));
	t.logDirectColor = reinterpret_cast<FnLogDirectColor>(GetProcAddress(tier0,
		"?LoggingSystem_LogDirect@@YA?AW4LoggingResponse_t@@HW4LoggingSeverity_t@@VColor@@PEBD@Z"));

	if (t.findChannel) {
		t.chConsole = t.findChannel("Console");
		t.chGeneral = t.findChannel("General");
		t.chDevConsole = t.findChannel("DeveloperConsole");
		// Reuse brand channel if already registered this process
		t.chBrand = t.findChannel(kBrandChannel);
	}

	// IDA: RegisterLoggingChannel(name, callback=null, flags=0, verbosity, color)
	if (t.chBrand < 0 && t.registerChannel) {
		const int brandCol = PackColor(80, 220, 255, 255);
		__try {
			t.chBrand = t.registerChannel(kBrandChannel, nullptr, 0, kVerbosity, brandCol);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			t.chBrand = -1;
		}
	}
	if (t.chBrand >= 0) {
		if (t.setVerbosity) {
			__try { t.setVerbosity(t.chBrand, kVerbosity); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("notify.init.setVerbosity"); }
		}
		if (t.setChannelColor) {
			__try { t.setChannelColor(t.chBrand, PackColor(80, 220, 255, 255)); }
			__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("notify.init.setChannelColor"); }
		}
	}

	if (t.conMsg || t.msg || t.conColorMsg || t.logDirect || t.logColor || t.chBrand >= 0)
		t.ready = true;
	return t;
}

static void EnsureChannelOpen(Tier0Console& t, int ch)
{
	if (ch < 0 || !t.setVerbosity)
		return;
	__try {
		t.setVerbosity(ch, kVerbosity);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("notify.ensureChannelOpen");
	}
}

static void SehConMsg(FnConMsg fn, const char* msg)
{
	if (!fn || !msg) return;
	__try { fn("%s\n", msg); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("notify.sehConMsg"); }
}

static void SehMsg(FnMsg fn, const char* msg)
{
	if (!fn || !msg) return;
	__try { fn("%s\n", msg); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("notify.sehMsg"); }
}

// Source Color = 4 bytes RGBA (tier0 ConColorMsg AEBVColor)
static void SehConColorMsg(FnConColorMsg fn, const char* msg, int r, int g, int b, int a)
{
	if (!fn || !msg) return;
	std::
uint8_t col[4] = {
		static_cast<std::
uint8_t>(std::
clamp(r, 0, 255)),
		static_cast<std::
uint8_t>(std::
clamp(g, 0, 255)),
		static_cast<std::
uint8_t>(std::
clamp(b, 0, 255)),
		static_cast<std::
uint8_t>(std::
clamp(a, 0, 255))
	};
	__try { fn(col, "%s\n", msg); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("notify.sehConColorMsg"); }
}

static void SehLogDirect(FnLogDirect3 fn, int ch, const char* msg)
{
	if (!fn || ch < 0 || !msg) return;
	__try { fn(ch, kLogMsg, msg); }
	__except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("notify.sehLogDirect"); }
}

// Prefer brand channel -> (Games8Th) tag in console
static bool EmitBrand(Tier0Console& t, const char* line, int r, int g, int b, int a)
{
	if (t.chBrand < 0 || !line)
		return false;
	EnsureChannelOpen(t, t.chBrand);
	const int col = PackColor(r, g, b, a);
	if (t.logColor) {
		bool ok = false;
		__try {
			t.logColor(t.chBrand, kLogMsg, col, "%s\n", line);
			ok = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
		if (ok)
			return true;
	}
	if (t.logDirectColor) {
		bool ok = false;
		__try {
			t.logDirectColor(t.chBrand, kLogMsg, col, line);
			ok = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
		if (ok)
			return true;
	}
	if (t.logDirect) {
		SehLogDirect(t.logDirect, t.chBrand, line);
		return true;
	}
	return false;
}

static void SanitizeMsg(char* msg)
{
	if (!msg)
		return;
	for (char* p = msg; *p; ++p) {
		if (*p == '\r' || *p == '\t')
			*p = ' ';
	}
}

void ConsolePrintf(const char* fmt, ...) {
	if (!fmt)
		return;

	char msg[512]{};
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (!msg[0])
		return;
	SanitizeMsg(msg);

	char line[520]{};
	std::
snprintf(line, sizeof(line), "%s\n", msg);

	Tier0Console& t = GetTier0Console();
	if (!t.ready)
		return;

	// Brand first so hitlog / feature text shows (Games8Th)
	if (EmitBrand(t, msg, 180, 220, 255, 255))
		return;

	EnsureChannelOpen(t, t.chConsole);
	EnsureChannelOpen(t, t.chGeneral);
	EnsureChannelOpen(t, t.chDevConsole);

	if (t.conColorMsg) {
		SehConColorMsg(t.conColorMsg, msg, 180, 220, 255, 255);
		return;
	}
	if (t.conMsg) {
		SehConMsg(t.conMsg, msg);
		return;
	}
	if (t.logDirect && t.chConsole >= 0) {
		SehLogDirect(t.logDirect, t.chConsole, line);
		return;
	}
	if (t.msg) {
		SehMsg(t.msg, msg);
		return;
	}
	if (t.logDirect && t.chGeneral >= 0)
		SehLogDirect(t.logDirect, t.chGeneral, line);
}

void ConsoleColorPrintf(int r, int g, int b, int a, const char* fmt, ...)
{
	if (!fmt)
		return;

	char msg[512]{};
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (!msg[0])
		return;
	SanitizeMsg(msg);

	char line[520]{};
	std::
snprintf(line, sizeof(line), "%s\n", msg);

	Tier0Console& t = GetTier0Console();
	if (!t.ready)
		return;

	if (EmitBrand(t, msg, r, g, b, a))
		return;

	EnsureChannelOpen(t, t.chConsole);
	EnsureChannelOpen(t, t.chGeneral);
	EnsureChannelOpen(t, t.chDevConsole);

	if (t.conColorMsg) {
		SehConColorMsg(t.conColorMsg, msg, r, g, b, a);
		return;
	}
	if (t.conMsg) {
		SehConMsg(t.conMsg, msg);
		return;
	}
	if (t.logDirect && t.chConsole >= 0) {
		SehLogDirect(t.logDirect, t.chConsole, line);
		return;
	}
	if (t.msg) {
		SehMsg(t.msg, msg);
		return;
	}
	if (t.logDirect && t.chGeneral >= 0)
		SehLogDirect(t.logDirect, t.chGeneral, line);
}

} // namespace Notify


