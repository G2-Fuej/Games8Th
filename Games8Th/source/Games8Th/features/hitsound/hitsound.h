#pragma once

#include <cstddef>

namespace Hitsound {
	// User-writable folder outside the game tree (BSecureAllowed-safe).
	// Documents\Games8Th\Hitsounds - never under game\csgo\.
	// Resolved at runtime via FolderPath().
const char* FolderPath();

	// Hooks::init -> Install: ensure user folder + scan *.wav
	void Install();
	void Shutdown();

	// Rescan folder for *.wav (also re-creates user folder if missing)
	void RefreshList();

	// Dropdown helpers
	int Count();
	const char* NameAt(int index);          // basename e.g. "hit1.wav"
	int IndexOf(const char* fileName);      // -1 if missing

	// Play selected (or head/kill override). Safe from game-event thread.
	void Play(bool head, bool kill);

	// Preview currently selected normal sound (menu Test button)
	void PreviewSelected();
}


