#pragma once
// No park, no cs2.exe patch, no extra client insecure JMPs.
// Install/Uninstall stay as no-ops so existing call sites compile.
namespace SecureAllow {
inline bool Install() { return true; }
inline void Uninstall() {}
inline void KeepAllowed() {}
inline bool IsInstalled() { return false; }
}
