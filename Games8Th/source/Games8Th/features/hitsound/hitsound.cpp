#include "hitsound.h"

#include "../../utils/console/console.h"
#include "../../config/config.h"

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {
	constexpr int kMaxFiles = 256;
	constexpr int kNameLen = 160;

	struct Entry {
		char name[kNameLen]{};
	};

	Entry g_files[kMaxFiles]{};
	int g_count = 0;
	CRITICAL_SECTION g_cs{};
	volatile LONG g_csReady = 0;

	char g_folder[MAX_PATH]{};
	bool g_folderReady = false;

	void EnsureCs() {
		if (InterlockedCompareExchange(&g_csReady, 1, 0) == 0) {
			InitializeCriticalSection(&g_cs);
			InterlockedExchange(&g_csReady, 2);
		} else {
			while (InterlockedCompareExchange(&g_csReady, 2, 2) != 2)
				Sleep(0);
		}
	}

	bool ResolveUserFolder() {
		if (g_folderReady && g_folder[0])
			return true;
		g_folder[0] = 0;
		char profile[MAX_PATH]{};
		DWORD n = GetEnvironmentVariableA("USERPROFILE", profile, sizeof(profile));
		if (n != 0 && n < sizeof(profile)) {
			std::
snprintf(g_folder, sizeof(g_folder),
				"%s\\Documents\\Games8Th\\Hitsounds", profile);
			g_folderReady = true;
			return true;
		}
		char appData[MAX_PATH]{};
		if (GetEnvironmentVariableA("APPDATA", appData, sizeof(appData)) > 0) {
			std::
snprintf(g_folder, sizeof(g_folder),
				"%s\\Games8Th\\Hitsounds", appData);
			g_folderReady = true;
			return true;
		}
		return false;
	}

	bool EndsWithWav(const char* name) {
		if (!name) return false;
		const size_t n = std::
strlen(name);
		if (n < 4) return false;
		const char* e = name + (n - 4);
		return e[0] == '.'
			&& (e[1] == 'w' || e[1] == 'W')
			&& (e[2] == 'a' || e[2] == 'A')
			&& (e[3] == 'v' || e[3] == 'V');
	}

	void BuildPath(char* out, size_t outSz, const char* fileName) {
		if (!out || outSz == 0) return;
		out[0] = 0;
		if (!fileName || !fileName[0]) return;
		if (!ResolveUserFolder()) return;
		if (std::
strchr(fileName, '\\') || std::
strchr(fileName, '/') || std::
strstr(fileName, ".."))
			return;
		std::
snprintf(out, outSz, "%s\\%s", g_folder, fileName);
	}

	char g_okName[kNameLen]{};
	char g_okPath[MAX_PATH]{};
	bool g_okValid = false;

	void PlayFile(const char* fileName) {
		if (fileName && fileName[0]) {
			if (g_okValid && _stricmp(g_okName, fileName) == 0) {
				PlaySoundA(g_okPath, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
				return;
			}

			char path[MAX_PATH]{};
			BuildPath(path, sizeof(path), fileName);
			if (path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
				std::
snprintf(g_okName, sizeof(g_okName), "%s", fileName);
				std::
snprintf(g_okPath, sizeof(g_okPath), "%s", path);
				g_okValid = true;
				PlaySoundA(g_okPath, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
				return;
			}
		}
		MessageBeep(MB_OK);
	}

	const char* PickName(bool head, bool kill) {
		if (kill && Config::
hitsound_kill[0])
			return Config::
hitsound_kill;
		if (head && Config::
hitsound_head[0])
			return Config::
hitsound_head;
		if (Config::
hitsound_file[0])
			return Config::
hitsound_file;
		return nullptr;
	}

	bool EnsureHitsoundFolder() {
		if (!ResolveUserFolder())
			return false;
		std::
error_code ec;
		std::
filesystem::
create_directories(g_folder, ec);
		if (std::
filesystem::
is_directory(g_folder, ec))
			return true;
		return GetFileAttributesA(g_folder) != INVALID_FILE_ATTRIBUTES;
	}
}

const char* Hitsound::
FolderPath() {
	if (!ResolveUserFolder())
		return "";
	return g_folder;
}

void Hitsound::
Install() {
	EnsureCs();
	EnsureHitsoundFolder();
	RefreshList();
}

void Hitsound::
Shutdown() {
	PlaySoundA(nullptr, nullptr, 0);
	g_okValid = false;
	g_okName[0] = 0;
	g_okPath[0] = 0;
	if (InterlockedCompareExchange(&g_csReady, 2, 2) == 2) {
		EnterCriticalSection(&g_cs);
		g_count = 0;
		LeaveCriticalSection(&g_cs);
	}
}

void Hitsound::
RefreshList() {
	EnsureCs();
	EnsureHitsoundFolder();
	EnterCriticalSection(&g_cs);
	g_count = 0;
	g_okValid = false;

	const char* folder = FolderPath();
	if (!folder || !folder[0]) {
		LeaveCriticalSection(&g_cs);
		return;
	}

	char search[MAX_PATH]{};
	std::
snprintf(search, sizeof(search), "%s\\*.wav", folder);

	WIN32_FIND_DATAA fd{};
	HANDLE h = FindFirstFileA(search, &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			if (!EndsWithWav(fd.cFileName))
				continue;
			if (g_count >= kMaxFiles)
				break;
			std::
snprintf(g_files[g_count].name, kNameLen, "%s", fd.cFileName);
			++g_count;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	std::
sort(g_files, g_files + g_count, [](const Entry& a, const Entry& b) {
		return _stricmp(a.name, b.name) < 0;
	});

	if (g_count > 0 && !Config::
hitsound_file[0])
		std::
snprintf(Config::
hitsound_file, sizeof(Config::
hitsound_file), "%s", g_files[0].name);

	LeaveCriticalSection(&g_cs);
}

int Hitsound::
Count() {
	EnsureCs();
	EnterCriticalSection(&g_cs);
	const int n = g_count;
	LeaveCriticalSection(&g_cs);
	return n;
}

const char* Hitsound::
NameAt(int index) {
	EnsureCs();
	EnterCriticalSection(&g_cs);
	const char* r = nullptr;
	if (index >= 0 && index < g_count)
		r = g_files[index].name;
	LeaveCriticalSection(&g_cs);
	return r;
}

int Hitsound::
IndexOf(const char* fileName) {
	if (!fileName || !fileName[0]) return -1;
	EnsureCs();
	EnterCriticalSection(&g_cs);
	int found = -1;
	for (int i = 0; i < g_count; ++i) {
		if (_stricmp(g_files[i].name, fileName) == 0) {
			found = i;
			break;
		}
	}
	LeaveCriticalSection(&g_cs);
	return found;
}

void Hitsound::
Play(bool head, bool kill) {
	if (!Config::
hitsound)
		return;
	const char* name = PickName(head, kill);
	if (name && name[0]) {
		PlayFile(name);
	} else {
		MessageBeep(MB_OK);
	}
}

void Hitsound::
PreviewSelected() {
	const char* name = Config::
hitsound_file[0] ? Config::
hitsound_file : nullptr;
	if (!name && Count() > 0)
		name = NameAt(0);
	if (name)
		PlayFile(name);
}


