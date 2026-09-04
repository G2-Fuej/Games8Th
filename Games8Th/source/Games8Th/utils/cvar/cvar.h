#pragma once

// Cached convar read helper (hot-path safe).
// Resolves a ConVar object once (client -> engine2 -> server), then reads
// the value union at data+0x58. Any failure returns the caller's fallback.
//
// Resolution scans the whole client.dll image for the convar name + the
// paired lea - do NOT let the first in-game use trigger it (seconds-long
// hitch). Call Warmup() at inject for every convar read on hot paths.
namespace Cvar {

float Float(const char* name, float fallback);
// Resolve the hot-path convars eagerly (sensitivity, sv_gravity, ...).
void Warmup();

} // namespace Cvar

