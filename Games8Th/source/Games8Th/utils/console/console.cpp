#ifdef _DEBUG

#include "console.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <ctime>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace Con {
namespace {

// -- State ------------------------------------------------------------------
HANDLE g_console = nullptr;
FILE*  g_log = nullptr;

Level  g_minLevel = Level::
Ok;
bool   g_alwaysFlush = false;
bool   g_showTid = false;
bool   g_absTime = false; // wall clock vs +seconds from boot
bool   g_quietBoot = false; // Ok/Info skip console (file only)
	bool   g_forceVerbose = false; // GAMES8TH_LOG_VERBOSE=1
bool   g_vt = false; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
ULONGLONG g_quietT0 = 0;
volatile LONG g_cntPatMiss = 0;
volatile LONG g_cntOffMiss = 0;
volatile LONG g_cntSchemaMiss = 0;

enum class MissKind : unsigned char { Pattern, Offset, Schema };

struct MissRec {
	MissKind kind{};
	char module[20]{};
	char name[72]{};
	char sig[96]{};
};
constexpr int kMissCap = 64;
MissRec g_misses[kMissCap]{};
int g_missStored = 0;
bool g_missOverflow = false;

ULONGLONG g_t0 = 0;
int    g_linesSinceFlush = 0;

CRITICAL_SECTION g_cs{};
volatile LONG g_csReady = 0;

volatile LONG g_cntOk = 0;
volatile LONG g_cntInfo = 0;
volatile LONG g_cntWarn = 0;
volatile LONG g_cntError = 0;
volatile LONG g_cntSeh = 0;
volatile LONG g_cntTrace = 0;
volatile LONG g_cntSuppressed = 0;

// FNV-1a rate / once tables - O(1) average, no string scan on hot path
struct RateSlot {
	std::
uint32_t hash = 0;
	DWORD lastTick = 0;
};
struct OnceSlot {
	std::
uint32_t hash = 0;
	bool used = false;
};
constexpr int kRateSlots = 128;
constexpr int kOnceSlots = 96;
RateSlot g_rate[kRateSlots] = {};
OnceSlot g_once[kOnceSlots] = {};

// -- Init helpers -----------------------------------------------------------
void EnsureCs()
{
	if (InterlockedCompareExchange(&g_csReady, 1, 0) == 0) {
		InitializeCriticalSection(&g_cs);
		InterlockedExchange(&g_csReady, 2);
	} else {
		while (InterlockedCompareExchange(&g_csReady, 2, 2) != 2)
			Sleep(0);
	}
}

struct Lock {
	Lock() { EnsureCs(); EnterCriticalSection(&g_cs); }
	~Lock() { LeaveCriticalSection(&g_cs); }
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
};

[[nodiscard]] std::
uint32_t HashKey(const char* s) noexcept
{
	// FNV-1a 32
	std::
uint32_t h = 2166136261u;
	if (!s)
		return h;
	for (; *s; ++s) {
		h ^= static_cast<std::
uint8_t>(*s);
		h *= 16777619u;
	}
	return h ? h : 1u; // never 0 (empty marker)
}

bool EnvOn(const char* a, const char* b)
{
	char e[8]{};
	if (GetEnvironmentVariableA(a, e, sizeof(e)) == 0 && b)
		GetEnvironmentVariableA(b, e, sizeof(e));
	return e[0] == '1' || e[0] == 'y' || e[0] == 'Y';
}

// -- Rate / once ------------------------------------------------------------
bool RateAllow(const char* key, DWORD intervalMs)
{
	if (!key || !key[0])
		return true;
	const std::
uint32_t h = HashKey(key);
	const DWORD now = GetTickCount();
	const int slot = static_cast<int>(h % static_cast<std::
uint32_t>(kRateSlots));

	// Probe linear from hash slot
	for (int n = 0; n < kRateSlots; ++n) {
		const int i = (slot + n) % kRateSlots;
		if (g_rate[i].hash == 0) {
			g_rate[i].hash = h;
			g_rate[i].lastTick = now;
			return true;
		}
		if (g_rate[i].hash == h) {
			if (now - g_rate[i].lastTick < intervalMs) {
				InterlockedIncrement(&g_cntSuppressed);
				return false;
			}
			g_rate[i].lastTick = now;
			return true;
		}
	}
	return true; // table full - allow
}

bool OnceAllow(const char* key)
{
	if (!key || !key[0])
		return true;
	const std::
uint32_t h = HashKey(key);
	const int slot = static_cast<int>(h % static_cast<std::
uint32_t>(kOnceSlots));
	for (int n = 0; n < kOnceSlots; ++n) {
		const int i = (slot + n) % kOnceSlots;
		if (!g_once[i].used) {
			g_once[i].used = true;
			g_once[i].hash = h;
			return true;
		}
		if (g_once[i].hash == h) {
			InterlockedIncrement(&g_cntSuppressed);
			return false;
		}
	}
	return true;
}

// -- Names / colors ---------------------------------------------------------
const char* SehName(DWORD code)
{
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS";
	case EXCEPTION_DATATYPE_MISALIGNMENT: return "MISALIGNMENT";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIV0";
	case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID";
	case EXCEPTION_FLT_OVERFLOW:          return "FLT_OVERFLOW";
	case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSN";
	case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIV0";
	case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
	case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSN";
	case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
	case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
	case EXCEPTION_GUARD_PAGE:            return "GUARD_PAGE";
	default:                              return "EXCEPTION";
	}
}

WORD ColorFor(Level level)
{
	switch (level) {
	case Level::Trace: return FOREGROUND_INTENSITY;
	case Level::Ok:    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	case Level::Info:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	case Level::Warn:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	case Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
	case Level::Seh:   return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
	default:           return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	}
}

const char* LvlTag(Level level)
{
	switch (level) {
	case Level::Trace: return "TRC";
	case Level::Ok:    return "OK ";
	case Level::Info:  return "INF";
	case Level::Warn:  return "WRN";
	case Level::Error: return "ERR";
	case Level::Seh:   return "SEH";
	default:           return "???";
	}
}

// VT color for the 3-char level token (bright)
const char* LvlAnsi(Level level)
{
	switch (level) {
	case Level::Trace: return "90";
	case Level::Ok:    return "92";
	case Level::Info:  return "37";
	case Level::Warn:  return "93";
	case Level::Error: return "91";
	case Level::Seh:   return "95";
	default:           return "37";
	}
}

void Bump(Level level)
{
	switch (level) {
	case Level::Trace: InterlockedIncrement(&g_cntTrace); break;
	case Level::Ok:    InterlockedIncrement(&g_cntOk); break;
	case Level::Info:  InterlockedIncrement(&g_cntInfo); break;
	case Level::Warn:  InterlockedIncrement(&g_cntWarn); break;
	case Level::Error: InterlockedIncrement(&g_cntError); break;
	case Level::Seh:   InterlockedIncrement(&g_cntSeh); break;
	}
}

void TryEnableVt(HANDLE h)
{
	g_vt = false;
	if (!h || h == INVALID_HANDLE_VALUE)
		return;
	DWORD mode = 0;
	if (!GetConsoleMode(h, &mode))
		return;
	mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	if (SetConsoleMode(h, mode))
		g_vt = true;
}

// Seconds from inject, fixed 7-char column: "  0.123"
int FormatTime(char* out, size_t outSz)
{
	if (g_absTime) {
		SYSTEMTIME st{};
		GetLocalTime(&st);
		return _snprintf_s(out, outSz, _TRUNCATE, "%02u:%02u:%02u",
			st.wHour, st.wMinute, st.wSecond);
	}
	const double sec = (GetTickCount64() - g_t0) / 1000.0;
	if (sec < 10000.0)
		return _snprintf_s(out, outSz, _TRUNCATE, "%8.3f", sec);
	if (sec < 100000.0)
		return _snprintf_s(out, outSz, _TRUNCATE, "%7.0fs", sec);
	return _snprintf_s(out, outSz, _TRUNCATE, "%7.0f", sec);
}

void FormatTag(char* out, size_t outSz, const char* tag)
{
	if (!out || outSz < 9)
		return;
	memset(out, ' ', 8);
	out[8] = '\0';
	if (!tag || !tag[0])
		return;
	size_t n = 0;
	while (tag[n] && n < 8)
		++n;
	memcpy(out, tag, n);
}

constexpr int kMsgCol = 25; // time8 + 2 + lvl3 + 2 + tag8 + 2

void WriteUnlocked(WORD attr, const char* conText, const char* fileText, bool flushNow, bool quietFileOnly)
{
	const char* file = fileText ? fileText : conText;
	if (!file)
		return;

	if (!quietFileOnly) {
		if (IsDebuggerPresent())
			OutputDebugStringA(file);

		if (g_console) {
			const char* vis = (g_vt && conText) ? conText : file;
			if (!g_vt)
				SetConsoleTextAttribute(g_console, attr);
			DWORD written = 0;
			WriteConsoleA(g_console, vis, static_cast<DWORD>(strlen(vis)), &written, nullptr);
			if (!g_vt) {
				SetConsoleTextAttribute(g_console,
					FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
			}
		}
	}

	if (g_log) {
		fputs(file, g_log);
		if (flushNow || g_alwaysFlush) {
			fflush(g_log);
			g_linesSinceFlush = 0;
		} else if (++g_linesSinceFlush >= 64) {
			fflush(g_log);
			g_linesSinceFlush = 0;
		}
	} else if (!g_console && !quietFileOnly) {
		fputs(file, stdout);
		if (flushNow)
			fflush(stdout);
	}
}

void FlushUnlocked()
{
	if (g_log) {
		fflush(g_log);
		g_linesSinceFlush = 0;
	}
}

void FinishLine(char* line, int n)
{
	if (n < 0)
		n = 0;
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
		--n;
	line[n++] = '\n';
	line[n] = '\0';
}

void Emit(Level level, const char* tag, const char* body)
{
	if (!body)
		return;
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;

	const bool quietFileOnly = g_quietBoot && !g_forceVerbose
		&& level < Level::Warn;

	char ts[16];
	FormatTime(ts, sizeof(ts));

	char tagCol[12];
	FormatTag(tagCol, sizeof(tagCol), tag);
	const char* lvl = LvlTag(level);

	char tidBuf[12]{};
	if (g_showTid)
		_snprintf_s(tidBuf, sizeof(tidBuf), _TRUNCATE, " t%04lX", GetCurrentThreadId() & 0xFFFFu);

	char fileLine[4608];
	int n = _snprintf_s(fileLine, sizeof(fileLine) - 2, _TRUNCATE,
		"%s  %s  %s  %s%s", ts, lvl, tagCol, body, tidBuf);
	FinishLine(fileLine, n);

	char conLine[4700];
	if (g_vt) {
		n = _snprintf_s(conLine, sizeof(conLine) - 2, _TRUNCATE,
			"\x1b[90m%s\x1b[0m  \x1b[%sm%s\x1b[0m  \x1b[90m%s\x1b[0m  %s%s",
			ts, LvlAnsi(level), lvl, tagCol, body, tidBuf);
		FinishLine(conLine, n);
	}

	const bool flushNow = (level >= Level::Error) || g_alwaysFlush;
	{
		Lock lock;
		WriteUnlocked(ColorFor(level), g_vt ? conLine : fileLine, fileLine, flushNow, quietFileOnly);
	}
	Bump(level);
}

void EmitBare(Level level, const char* text)
{
	if (!text)
		return;
	char fileLine[512];
	int n = _snprintf_s(fileLine, sizeof(fileLine) - 2, _TRUNCATE, "%s", text);
	FinishLine(fileLine, n);

	char conLine[540];
	if (g_vt) {
		n = _snprintf_s(conLine, sizeof(conLine) - 2, _TRUNCATE,
			"\x1b[%sm%s\x1b[0m", LvlAnsi(level), text);
		FinishLine(conLine, n);
	}

	Lock lock;
	WriteUnlocked(ColorFor(level), g_vt ? conLine : fileLine, fileLine, false, false);
}

void EmitDetail(const char* body)
{
	if (!body)
		return;
	if (static_cast<int>(Level::Info) < static_cast<int>(g_minLevel))
		return;

	char pad[32];
	memset(pad, ' ', kMsgCol);
	pad[kMsgCol] = '\0';

	char fileLine[4608];
	int n = _snprintf_s(fileLine, sizeof(fileLine) - 2, _TRUNCATE, "%s%s", pad, body);
	FinishLine(fileLine, n);

	char conLine[4700];
	if (g_vt) {
		n = _snprintf_s(conLine, sizeof(conLine) - 2, _TRUNCATE,
			"\x1b[90m%s%s\x1b[0m", pad, body);
		FinishLine(conLine, n);
	}

	const bool quietFileOnly = g_quietBoot && !g_forceVerbose;
	Lock lock;
	WriteUnlocked(FOREGROUND_INTENSITY, g_vt ? conLine : fileLine, fileLine, false, quietFileOnly);
}

void CopyField(char* dst, size_t dstSz, const char* src)
{
	if (!dst || dstSz == 0)
		return;
	if (!src || !src[0]) {
		dst[0] = '\0';
		return;
	}
	strncpy_s(dst, dstSz, src, _TRUNCATE);
}

void ShortModule(char* out, size_t outSz, const char* module)
{
	if (!out || outSz == 0)
		return;
	if (!module || !module[0] || (module[0] == '-' && !module[1])) {
		CopyField(out, outSz, "-");
		return;
	}
	const char* base = module;
	for (const char* p = module; *p; ++p) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}
	CopyField(out, outSz, base);
	const size_t n = strlen(out);
	if (n > 4 && _stricmp(out + (n - 4), ".dll") == 0)
		out[n - 4] = '\0';
}

bool LooksLikeByteSig(const char* s)
{
	if (!s || !s[0])
		return false;
	int hex = 0, sp = 0, q = 0, other = 0;
	for (int i = 0; s[i] && i < 48; ++i) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
			++hex;
		else if (c == ' ' || c == '\t')
			++sp;
		else if (c == '?')
			++q;
		else
			++other;
	}
	if (other)
		return false;
	return (q > 0 && hex >= 2) || (hex >= 4 && sp >= 1);
}

bool EndsWithPatternWord(const char* name)
{
	if (!name)
		return false;
	const size_t n = strlen(name);
	return n > 8 && _stricmp(name + (n - 8), " pattern") == 0;
}

void CompactSigLabel(const char* sig, char* out, size_t outSz)
{
	if (!out || outSz == 0)
		return;
	if (!sig || !sig[0]) {
		CopyField(out, outSz, "(unnamed sig)");
		return;
	}
	while (*sig == ' ' || *sig == '\t')
		++sig;
	const size_t n = strlen(sig);
	if (n <= 36) {
		CopyField(out, outSz, sig);
		return;
	}
	char head[40]{};
	memcpy(head, sig, 36);
	int cut = 35;
	while (cut > 12 && head[cut] != ' ')
		--cut;
	head[cut] = '\0';
	_snprintf_s(out, outSz, _TRUNCATE, "%s ...", head);
}

void EmitDetailFile(const char* body)
{
	if (!body)
		return;
	char pad[32];
	memset(pad, ' ', kMsgCol);
	pad[kMsgCol] = '\0';
	char fileLine[4608];
	int n = _snprintf_s(fileLine, sizeof(fileLine) - 2, _TRUNCATE, "%s%s", pad, body);
	FinishLine(fileLine, n);
	Lock lock;
	WriteUnlocked(FOREGROUND_INTENSITY, fileLine, fileLine, false, true);
}

bool StoreMissUnlocked(MissKind kind, const char* module, const char* name, const char* sig)
{
	char mod[20]{};
	ShortModule(mod, sizeof(mod), module);
	const char* nm = (name && name[0]) ? name : "?";
	for (int i = 0; i < g_missStored; ++i) {
		if (g_misses[i].kind == kind
			&& strncmp(g_misses[i].module, mod, sizeof(g_misses[i].module) - 1) == 0
			&& strncmp(g_misses[i].name, nm, sizeof(g_misses[i].name) - 1) == 0)
			return false;
	}
	if (g_missStored >= kMissCap) {
		g_missOverflow = true;
		return true;
	}
	MissRec& r = g_misses[g_missStored++];
	r.kind = kind;
	CopyField(r.module, sizeof(r.module), mod);
	CopyField(r.name, sizeof(r.name), nm);
	if (sig && sig[0])
		CopyField(r.sig, sizeof(r.sig), sig);
	else
		r.sig[0] = '\0';
	return true;
}

void EmitMissLine(MissKind kind, const char* module, const char* name)
{
	if (kind == MissKind::Pattern) {
		char mod[20]{};
		ShortModule(mod, sizeof(mod), module);
		Tag("pattern", Level::Warn, "%-12s  %s", mod, name ? name : "?");
	} else if (kind == MissKind::Schema) {
		Tag("schema", Level::Warn, "%s", name ? name : "?");
	} else {
		Tag("offset", Level::Warn, "%s", name ? name : "?");
	}
}

void DumpMissTable()
{
	if (g_missStored <= 0 && g_cntPatMiss == 0 && g_cntOffMiss == 0 && g_cntSchemaMiss == 0)
		return;

	for (int pass = 0; pass < 3; ++pass) {
		const MissKind kind = (pass == 0) ? MissKind::Pattern
			: (pass == 1) ? MissKind::Offset : MissKind::Schema;
		for (int i = 0; i < g_missStored; ++i) {
			if (g_misses[i].kind != kind)
				continue;
			EmitMissLine(g_misses[i].kind, g_misses[i].module, g_misses[i].name);
			if (g_misses[i].sig[0])
				EmitDetailFile(g_misses[i].sig);
		}
	}
	if (g_missOverflow) {
		const long total = (long)(g_cntPatMiss + g_cntOffMiss + g_cntSchemaMiss);
		char more[80];
		_snprintf_s(more, sizeof(more), _TRUNCATE,
			"+%ld more (cap %d) -- see log",
			total - (long)g_missStored, kMissCap);
		EmitDetail(more);
	}
}

} // namespace

// -- Public -----------------------------------------------------------------

void Init(HANDLE console, FILE* logFile)
{
	EnsureCs();
	Lock lock;

	g_console = console;
	g_log = logFile;
	g_t0 = GetTickCount64();
	g_minLevel = Level::
Ok;
	g_alwaysFlush = false;
	g_showTid = false;
	g_absTime = false;
	g_quietBoot = false;
	g_forceVerbose = false;
	g_vt = false;
	g_quietT0 = 0;
	g_linesSinceFlush = 0;
	g_cntPatMiss = 0;
	g_cntOffMiss = 0;
	g_cntSchemaMiss = 0;
	g_missStored = 0;
	g_missOverflow = false;
	memset(g_misses, 0, sizeof(g_misses));

	if (EnvOn("GAMES8TH_LOG_TRACE", "games8th_LOG_TRACE"))
		g_minLevel = Level::
Trace;
	if (EnvOn("GAMES8TH_LOG_FLUSH", "games8th_LOG_FLUSH"))
		g_alwaysFlush = true;
	if (EnvOn("GAMES8TH_LOG_TID", "games8th_LOG_TID"))
		g_showTid = true;
	if (EnvOn("GAMES8TH_LOG_ABS", "games8th_LOG_ABS"))
		g_absTime = true;
	if (EnvOn("GAMES8TH_LOG_VERBOSE", "games8th_LOG_VERBOSE"))
		g_forceVerbose = true;

	TryEnableVt(g_console);

	// Default: quiet boot until QuietBootEnd (full OK spam -> file only)
	if (!g_forceVerbose)
		g_quietBoot = true;
	g_quietT0 = g_t0;

	// One compact session header in the file only (console gets BootBanner)
	if (g_log) {
		SYSTEMTIME st{};
		GetLocalTime(&st);
		fprintf(g_log,
			"\n=== Games8Th debug  %04u-%02u-%02u %02u:%02u:%02u  pid %lu ===\n"
			"    quiet_boot=%d  verbose=%d  (set GAMES8TH_LOG_VERBOSE=1 for full console)\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			GetCurrentProcessId(),
			g_quietBoot ? 1 : 0, g_forceVerbose ? 1 : 0);
		fflush(g_log);
	}
}

void QuietBoot(bool on)
{
	g_quietBoot = on;
	if (on)
		g_quietT0 = GetTickCount64();
}

bool IsQuietBoot()
{
	return g_quietBoot;
}

void QuietBootEnd(const char* label)
{
	(void)label;
	const ULONGLONG dt = GetTickCount64() - (g_quietT0 ? g_quietT0 : g_t0);
	g_quietBoot = false;

	const long misses = (long)(g_cntPatMiss + g_cntOffMiss + g_cntSchemaMiss);
	char body[256];
	if (misses > 0)
		_snprintf_s(body, sizeof(body), _TRUNCATE,
			"ready  %ld ok  %ld err  %ld miss  %llu ms",
			(long)g_cntOk, (long)g_cntError, misses, (unsigned long long)dt);
	else
		_snprintf_s(body, sizeof(body), _TRUNCATE,
			"ready  %ld ok  %ld err  %llu ms",
			(long)g_cntOk, (long)g_cntError, (unsigned long long)dt);
	Emit(misses > 0 ? Level::Warn : Level::Ok, "boot", body);
	DumpMissTable();
	Flush();
}

void Shutdown()
{
	if (InterlockedCompareExchange(&g_csReady, 2, 2) != 2)
		return;
	EnterCriticalSection(&g_cs);
	FlushUnlocked();
	g_console = nullptr;
	g_log = nullptr;
	LeaveCriticalSection(&g_cs);
}

void Flush()
{
	if (InterlockedCompareExchange(&g_csReady, 2, 2) != 2)
		return;
	Lock lock;
	FlushUnlocked();
}

void SetMinLevel(Level level) { g_minLevel = level; }
Level GetMinLevel() { return g_minLevel; }
void SetAlwaysFlush(bool on) { g_alwaysFlush = on; }
void SetShowTid(bool on) { g_showTid = on; }

void VPrint(Level level, const char* fmt, va_list args)
{
	if (!fmt)
		return;
	// Early out before format - hot path when min-level filters Trace
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;
	char body[4096];
	vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
	Emit(level, nullptr, body);
}

void Print(Level level, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(level, fmt, args);
	va_end(args);
}

void VTag(const char* tag, Level level, const char* fmt, va_list args)
{
	if (!fmt)
		return;
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;
	char body[4096];
	vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
	Emit(level, tag, body);
}

void Tag(const char* tag, Level level, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VTag(tag, level, fmt, args);
	va_end(args);
}

void VDetail(const char* fmt, va_list args)
{
	if (!fmt)
		return;
	char body[4096];
	vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
	EmitDetail(body);
}

void Detail(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VDetail(fmt, args);
	va_end(args);
}

void Trace(const char* fmt, ...)
{
	va_list a; va_start(a, fmt); VPrint(Level::
Trace, fmt, a); va_end(a);
}
void Ok(const char* fmt, ...)
{
	va_list a; va_start(a, fmt); VPrint(Level::
Ok, fmt, a); va_end(a);
}
void Info(const char* fmt, ...)
{
	va_list a; va_start(a, fmt); VPrint(Level::
Info, fmt, a); va_end(a);
}
void Warn(const char* fmt, ...)
{
	va_list a; va_start(a, fmt); VPrint(Level::
Warn, fmt, a); va_end(a);
}
void Error(const char* fmt, ...)
{
	va_list a; va_start(a, fmt); VPrint(Level::
Error, fmt, a); va_end(a);
}

void Seh(const char* where, DWORD code)
{
	if (!where)
		where = "?";
	char key[128];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "seh:%s:%08X", where, code);
	{
		Lock lock;
		if (!RateAllow(key, 1500))
			return;
	}
	// Always ODS (even if min-level would hide) - DbgView in Release-like use
	char ods[384];
	_snprintf_s(ods, sizeof(ods), _TRUNCATE,
		"[SEH] %s - %s (0x%08X)\n", where, SehName(code), code);
	OutputDebugStringA(ods);

	// Extract component name from "component.action" for tag column
	char sehTag[12] = "seh";
	const char* dot = strchr(where, '.');
	if (dot && (dot - where) < 10) {
		size_t len = dot - where;
		memcpy(sehTag, where, len);
		sehTag[len] = '\0';
	}
	char body[320];
	_snprintf_s(body, sizeof(body), _TRUNCATE,
		"%s  %s (0x%08X)", where, SehName(code), code);
	Emit(Level::
Seh, sehTag, body);
}

void SehOnce(const char* where, DWORD code)
{
	Seh(where, code);
}

void SehForce(const char* where, DWORD code)
{
	if (!where)
		where = "?";
	char ods[384];
	_snprintf_s(ods, sizeof(ods), _TRUNCATE,
		"[SEH!] %s - %s (0x%08X)\n", where, SehName(code), code);
	OutputDebugStringA(ods);
	char sehTag[12] = "seh";
	const char* dot = strchr(where, '.');
	if (dot && (dot - where) < 10) {
		size_t len = dot - where;
		memcpy(sehTag, where, len);
		sehTag[len] = '\0';
	}
	char body[320];
	_snprintf_s(body, sizeof(body), _TRUNCATE,
		"! %s  %s (0x%08X)", where, SehName(code), code);
	Emit(Level::
Seh, sehTag, body);
}

void OffsetMiss(const char* name, uintptr_t value)
{
	if (!name || !name[0])
		return;

	if (LooksLikeByteSig(name)) {
		PatternMiss("-", name, nullptr);
		return;
	}
	if (EndsWithPatternWord(name)) {
		char stripped[96];
		CopyField(stripped, sizeof(stripped), name);
		stripped[strlen(stripped) - 8] = '\0';
		PatternMiss("-", stripped, nullptr);
		return;
	}

	const bool schema = (_strnicmp(name, "schema", 6) == 0);
	const MissKind kind = schema ? MissKind::Schema : MissKind::Offset;
	const char* label = name;
	if (schema && _strnicmp(name, "schema ", 7) == 0)
		label = name + 7;

	char shown[96];
	if (value && !schema)
		_snprintf_s(shown, sizeof(shown), _TRUNCATE, "%s  (got 0x%llX)", label, (unsigned long long)value);
	else
		CopyField(shown, sizeof(shown), label);

	bool fresh = false;
	{
		Lock lock;
		fresh = StoreMissUnlocked(kind, "-", shown, nullptr);
	}
	if (!fresh)
		return;
	if (schema)
		InterlockedIncrement(&g_cntSchemaMiss);
	else
		InterlockedIncrement(&g_cntOffMiss);
	if (g_quietBoot && !g_forceVerbose)
		return;
	EmitMissLine(kind, "-", shown);
}

void PatternMiss(const char* module, const char* name, const char* signature)
{
	if (!module)
		module = "-";

	const char* sig = signature;
	char labelBuf[72]{};
	const char* label = name;

	if (label && label[0] && LooksLikeByteSig(label)) {
		if (!sig)
			sig = label;
		CompactSigLabel(label, labelBuf, sizeof(labelBuf));
		label = labelBuf;
	} else if (!label || !label[0]) {
		if (sig && sig[0]) {
			CompactSigLabel(sig, labelBuf, sizeof(labelBuf));
			label = labelBuf;
		} else {
			label = "?";
		}
	}

	char sigStore[96]{};
	if (sig && sig[0]) {
		const size_t n = strlen(sig);
		if (n <= 90)
			CopyField(sigStore, sizeof(sigStore), sig);
		else
			_snprintf_s(sigStore, sizeof(sigStore), _TRUNCATE, "%.86s...", sig);
	}

	bool fresh = false;
	{
		Lock lock;
		fresh = StoreMissUnlocked(MissKind::Pattern, module, label, sigStore[0] ? sigStore : nullptr);
	}
	if (!fresh)
		return;
	InterlockedIncrement(&g_cntPatMiss);
	if (!(g_quietBoot && !g_forceVerbose))
		EmitMissLine(MissKind::Pattern, module, label);
	if (sigStore[0] && !(g_quietBoot && !g_forceVerbose))
		EmitDetailFile(sigStore);
}

void Once(const char* key, const char* fmt, ...)
{
	{
		Lock lock;
		if (!OnceAllow(key ? key : "?"))
			return;
	}
	va_list args;
	va_start(args, fmt);
	VPrint(Level::
Info, fmt, args);
	va_end(args);
}

void Rate(const char* key, DWORD intervalMs, const char* fmt, ...)
{
	{
		Lock lock;
		if (!RateAllow(key ? key : "?", intervalMs ? intervalMs : 1000))
			return;
	}
	va_list args;
	va_start(args, fmt);
	VPrint(Level::
Info, fmt, args);
	va_end(args);
}

void RateAt(const char* key, DWORD intervalMs, Level level, const char* fmt, ...)
{
	{
		Lock lock;
		if (!RateAllow(key ? key : "?", intervalMs ? intervalMs : 1000))
			return;
	}
	va_list args;
	va_start(args, fmt);
	VPrint(level, fmt, args);
	va_end(args);
}

void Hex(const char* label, const void* data, size_t len)
{
	if (!data || len == 0) {
		Tag("hex", Level::
Info, "%s  <empty>", label ? label : "?");
		return;
	}
	constexpr size_t kMax = 128; // was 256 - half the detail noise
	const size_t n = (len > kMax) ? kMax : len;
	const auto* p = static_cast<const unsigned char*>(data);

	Tag("hex", Level::
Info, "%s  %zu B @ %p%s",
		label ? label : "?", len, data, len > kMax ? "  (first 128)" : "");

	char row[96];
	for (size_t off = 0; off < n; off += 16) {
		int pos = _snprintf_s(row, sizeof(row), _TRUNCATE, "%04zX  ", off);
		if (pos < 0) pos = 0;
		const size_t lineLen = (n - off > 16) ? 16 : (n - off);
		for (size_t i = 0; i < lineLen; ++i) {
			if (static_cast<size_t>(pos) >= sizeof(row) - 4)
				break;
			int w = _snprintf_s(row + pos, sizeof(row) - pos, _TRUNCATE, "%02X ", p[off + i]);
			if (w < 0) break;
			pos += w;
		}
		EmitDetail(row);
	}
}

void Section(const char* title)
{
	char body[96];
	_snprintf_s(body, sizeof(body), _TRUNCATE, "-- %s", title ? title : "");
	Emit(Level::Info, "", body);
}

void Stats()
{
	if (g_cntPatMiss > 0 || g_cntOffMiss > 0 || g_cntSchemaMiss > 0)
		Tag("stats", Level::Info,
			"%ld ok  %ld warn  %ld err  %ld seh  %ld pattern  %ld offset  %ld schema",
			(long)g_cntOk, (long)g_cntWarn, (long)g_cntError, (long)g_cntSeh,
			(long)g_cntPatMiss, (long)g_cntOffMiss, (long)g_cntSchemaMiss);
	else if (g_cntSuppressed > 0)
		Tag("stats", Level::Info,
			"%ld ok  %ld warn  %ld err  %ld seh  %ld suppressed",
			(long)g_cntOk, (long)g_cntWarn, (long)g_cntError, (long)g_cntSeh,
			(long)g_cntSuppressed);
	else
		Tag("stats", Level::Info,
			"%ld ok  %ld warn  %ld err  %ld seh",
			(long)g_cntOk, (long)g_cntWarn, (long)g_cntError, (long)g_cntSeh);
}

void BootBanner(const char* logPath, int isoLevel, unsigned long pid)
{
	const bool wasQuiet = g_quietBoot;
	g_quietBoot = false;

	EmitBare(Level::Ok, "");
	EmitBare(Level::Ok, "  Games8Th  debug");
	char line[512];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "  pid %lu    iso %d", pid, isoLevel);
	EmitBare(Level::Ok, line);
	if (logPath && logPath[0]) {
		_snprintf_s(line, sizeof(line), _TRUNCATE, "  %s", logPath);
		EmitBare(Level::Info, line);
	}
	EmitBare(Level::Ok, "");

	g_quietBoot = wasQuiet;
}

ScopedTimer::ScopedTimer(const char* n, DWORD slowThresholdMs) noexcept
	: name(n ? n : "?")
	, t0(GetTickCount64())
	, slowMs(slowThresholdMs)
{
}

ScopedTimer::~ScopedTimer()
{
	const ULONGLONG dt = GetTickCount64() - t0;
	if (dt >= slowMs)
		Tag("timer", Level::
Info, "%s  %llu ms", name, (unsigned long long)
dt);
	else
		Tag("timer", Level::
Trace, "%s  %llu ms", name, (unsigned long long)
dt);
}

} // namespace Con

#else // -- Release: SEH via ODS only ----------------------------------------

#include "console.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace Con {
namespace {
	struct SehRateSlot {
		std::
uint32_t hash = 0;
		DWORD lastTick = 0;
	};
	constexpr int kSehSlots = 64;
	SehRateSlot g_sehRate[kSehSlots] = {};
	CRITICAL_SECTION g_sehCs{};
	volatile LONG g_sehReady = 0;

	void EnsureSehCs()
	{
		if (InterlockedCompareExchange(&g_sehReady, 1, 0) == 0) {
			InitializeCriticalSection(&g_sehCs);
			InterlockedExchange(&g_sehReady, 2);
		} else {
			while (InterlockedCompareExchange(&g_sehReady, 2, 2) != 2)
				Sleep(0);
		}
	}

	std::
uint32_t HashKey(const char* s)
	{
		std::
uint32_t h = 2166136261u;
		if (!s) return h;
		for (; *s; ++s) {
			h ^= static_cast<std::
uint8_t>(*s);
			h *= 16777619u;
		}
		return h ? h : 1u;
	}

	const char* SehNameRel(DWORD code)
	{
		switch (code) {
		case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSN";
		case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIV0";
		case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
		case EXCEPTION_GUARD_PAGE: return "GUARD_PAGE";
		case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
		default: return "EXCEPTION";
		}
	}

	bool RateOk(const char* key, DWORD intervalMs)
	{
		EnsureSehCs();
		const std::
uint32_t h = HashKey(key);
		const DWORD now = GetTickCount();
		const int slot = static_cast<int>(h % static_cast<std::
uint32_t>(kSehSlots));
		EnterCriticalSection(&g_sehCs);
		for (int n = 0; n < kSehSlots; ++n) {
			const int i = (slot + n) % kSehSlots;
			if (g_sehRate[i].hash == 0) {
				g_sehRate[i].hash = h;
				g_sehRate[i].lastTick = now;
				LeaveCriticalSection(&g_sehCs);
				return true;
			}
			if (g_sehRate[i].hash == h) {
				if (now - g_sehRate[i].lastTick < intervalMs) {
					LeaveCriticalSection(&g_sehCs);
					return false;
				}
				g_sehRate[i].lastTick = now;
				LeaveCriticalSection(&g_sehCs);
				return true;
			}
		}
		LeaveCriticalSection(&g_sehCs);
		return true;
	}
} // namespace

void Seh(const char* where, DWORD code)
{
	if (!where)
		where = "?";
	char key[128];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "seh:%s:%08X", where, code);
	if (!RateOk(key, 1500))
		return;
	char buf[384];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"[Games8Th SEH] %s - %s (0x%08X)\n", where, SehNameRel(code), code);
	OutputDebugStringA(buf);
}

} // namespace Con

#endif // _DEBUG


