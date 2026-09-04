#pragma once

// Lightweight AD (hypothesis) debug log.
// Debug: rate-limited lines via Con (console + file + ODS).
// Release: no-ops.

#ifdef _DEBUG
void initDebug();

// hyp: short id (A/B/W/BOOT) loc: file:func msg: tag dataJson: optional JSON object
void ADLog(const char* hyp, const char* loc, const char* msg, const char* dataJson);
void ADLogf(const char* hyp, const char* loc, const char* msg, const char* fmt, ...);

#else

inline void initDebug() {}
inline void ADLog(const char*, const char*, const char*, const char*) {}
inline void ADLogf(const char*, const char*, const char*, const char*, ...) {}

#endif

