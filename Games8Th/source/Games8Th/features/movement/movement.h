#pragma once
#include <memory>
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../config/config.h"

class Movement {
public:
	// Mouse autostrafe (mode 0) - call BEFORE Pred::Start (live FL_ONGROUND).
	void OnCreateMove(CUserCmd* user_cmd);
};

extern std::
unique_ptr<Movement> g_movement;

