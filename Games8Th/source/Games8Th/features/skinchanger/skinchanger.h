#pragma once

#include <cstdint>

class IGameEvent;

namespace SkinChanger
{
	void Init();
	void OnFrameStageNotify(int stage);
	void OnFireEventClientSide(void* gameEvent);
	void RefreshAll();
	// Single-item skin change (menu pick): no force reapply - WalkWeapons'
	// per-weapon signature diff applies exactly the changed weapon next FSN.
	// This only schedules the HUD icon refresh burst.
	void NotifySkinsChanged();
}

