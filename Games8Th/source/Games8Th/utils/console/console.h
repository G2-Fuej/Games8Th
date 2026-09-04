#pragma once
#include <Windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

// Games8Th debug log. Debug: console + file + ODS. Release: SEH ODS only.
//
//   0.123  OK   boot      pid 4120  iso 1
//   1.204  WRN  pattern   client        entity_set_model
//   1.204  WRN  offset    Pred::pPrediction
//   2.500  SEH  present   ACCESS_VIOLATION  Present.esp
//
// Columns: time (sec) | level | tag | message
// Quiet boot: Ok/Info file-only. Misses print as normal lines at QuietBootEnd.
// Byte signatures stay in the log file.
//
// Env: GAMES8TH_LOG_VERBOSE / TRACE / FLUSH / TID / ABS
//
// API:
// Con::Ok / Info / Warn / Error / Trace / Seh
// Con::Tag / Detail / Once / Rate / Section / Stats / Hex
// Con::QuietBoot / QuietBootEnd / OffsetMiss / PatternMiss

#ifdef _DEBUG
namespace Con {

enum class Level : int {
	Trace = 0,
	Ok    = 1,
	Info  = 2,
	Warn  = 3,
	Error = 4,
	Seh   = 5,
};

// Fast init: pass console handle + optional FILE*. Opens nothing itself.
void Init(HANDLE console, FILE* logFile);
void Shutdown();
// Drain pending batch to disk (safe to call from Present occasionally).
void Flush();

void SetMinLevel(Level level);
Level GetMinLevel();
void SetAlwaysFlush(bool on);
void SetShowTid(bool on);

// Boot quiet: Ok/Info -> file only (no console). Warn/Error/SEH still console.
// Cuts inject load time - hundreds of pattern/schema OK lines were WriteConsole-bound.
// GAMES8TH_LOG_VERBOSE=1 forces full console during boot.
void QuietBoot(bool on);
bool IsQuietBoot();
// Console summary + miss table, then leave quiet. Byte signatures stay in the log file.
void QuietBootEnd(const char* label = nullptr);

void Print(Level level, const char* fmt, ...);
void VPrint(Level level, const char* fmt, va_list args);

void Tag(const char* tag, Level level, const char* fmt, ...);
void VTag(const char* tag, Level level, const char* fmt, va_list args);

// Indented detail under last event
void Detail(const char* fmt, ...);
void VDetail(const char* fmt, va_list args);

void Trace(const char* fmt, ...);
void Ok(const char* fmt, ...);
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

void Seh(const char* where, DWORD code);
void SehOnce(const char* where, DWORD code);
void SehForce(const char* where, DWORD code);

void OffsetMiss(const char* name, uintptr_t value = 0);
void PatternMiss(const char* module, const char* name, const char* signature = nullptr);

void Once(const char* key, const char* fmt, ...);
void Rate(const char* key, DWORD intervalMs, const char* fmt, ...);
void RateAt(const char* key, DWORD intervalMs, Level level, const char* fmt, ...);

void Hex(const char* label, const void* data, size_t len);
void Section(const char* title);
void Stats();

// Compact boot banner (one block, not a wall of Info lines)
void BootBanner(const char* logPath, int isoLevel, unsigned long pid);

struct ScopedTimer {
	const char* name;
	ULONGLONG   t0;
	DWORD       slowMs;
	explicit ScopedTimer(const char* n, DWORD slowThresholdMs = 5) noexcept;
	~ScopedTimer();
	ScopedTimer(const ScopedTimer&) = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;
};

} // namespace Con

#define TW_TRACE(...)  ::Con::Trace(__VA_ARGS__)
#define TW_OK(...)     ::Con::Ok(__VA_ARGS__)
#define TW_INFO(...)   ::Con::Info(__VA_ARGS__)
#define TW_WARN(...)   ::Con::Warn(__VA_ARGS__)
#define TW_ERR(...)    ::Con::Error(__VA_ARGS__)
#define TW_SEH(w, c)   ::Con::Seh((w), (c))
#define TW_SEH_FORCE(w, c) ::Con::SehForce((w), (c))
#define TW_ONCE(k, ...) ::Con::Once((k), __VA_ARGS__)
#define TW_RATE(k, ms, ...) ::Con::Rate((k), (ms), __VA_ARGS__)
#define TW_TAG(tag, lvl, ...) ::Con::Tag((tag), (lvl), __VA_ARGS__)
#define TW_DETAIL(...) ::Con::Detail(__VA_ARGS__)
#define TW_FN_ENTER()  ::Con::Trace(">> %s", __FUNCTION__)
#define TW_FN_LEAVE()  ::Con::Trace("<< %s", __FUNCTION__)
#define TW_TIMER(name) ::Con::ScopedTimer _tw_timer_##__LINE__((name))
#define TW_SEH_CATCH(where) ::Con::Seh((where), GetExceptionCode())

#else // Release

namespace Con {

enum class Level : int {
	Trace = 0, Ok = 1, Info = 2, Warn = 3, Error = 4, Seh = 5,
};

inline void Init(HANDLE, FILE*) {}
inline void Shutdown() {}
inline void Flush() {}
inline void SetMinLevel(Level) {}
inline Level GetMinLevel() { return Level::Ok; }
inline void SetAlwaysFlush(bool) {}
inline void SetShowTid(bool) {}
inline void QuietBoot(bool) {}
inline bool IsQuietBoot() { return false; }
inline void QuietBootEnd(const char* = nullptr) {}
inline void Print(Level, const char*, ...) {}
inline void VPrint(Level, const char*, va_list) {}
inline void Tag(const char*, Level, const char*, ...) {}
inline void VTag(const char*, Level, const char*, va_list) {}
inline void Detail(const char*, ...) {}
inline void VDetail(const char*, va_list) {}
inline void Trace(const char*, ...) {}
inline void Ok(const char*, ...) {}
inline void Info(const char*, ...) {}
inline void Warn(const char*, ...) {}
inline void Error(const char*, ...) {}
void Seh(const char* where, DWORD code);
inline void SehOnce(const char* where, DWORD code) { Seh(where, code); }
inline void SehForce(const char* where, DWORD code) { Seh(where, code); }
inline void OffsetMiss(const char*, uintptr_t = 0) {}
inline void PatternMiss(const char*, const char*, const char* = nullptr) {}
inline void Once(const char*, const char*, ...) {}
inline void Rate(const char*, DWORD, const char*, ...) {}
inline void RateAt(const char*, DWORD, Level, const char*, ...) {}
inline void Hex(const char*, const void*, size_t) {}
inline void Section(const char*) {}
inline void Stats() {}
inline void BootBanner(const char*, int, unsigned long) {}

struct ScopedTimer {
	explicit ScopedTimer(const char*, DWORD = 5) noexcept {}
	~ScopedTimer() = default;
	ScopedTimer(const ScopedTimer&) = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;
};

} // namespace Con

#define TW_TRACE(...)      ((void)0)
#define TW_OK(...)         ((void)0)
#define TW_INFO(...)       ((void)0)
#define TW_WARN(...)       ((void)0)
#define TW_ERR(...)        ((void)0)
#define TW_SEH(w, c)       ::Con::Seh((w), (c))
#define TW_SEH_FORCE(w, c) ::Con::SehForce((w), (c))
#define TW_ONCE(k, ...)    ((void)0)
#define TW_RATE(k, ms, ...) ((void)0)
#define TW_TAG(tag, lvl, ...) ((void)0)
#define TW_DETAIL(...)     ((void)0)
#define TW_FN_ENTER()      ((void)0)
#define TW_FN_LEAVE()      ((void)0)
#define TW_TIMER(name)     ((void)0)
#define TW_SEH_CATCH(where) ::Con::Seh((where), GetExceptionCode())

#endif

