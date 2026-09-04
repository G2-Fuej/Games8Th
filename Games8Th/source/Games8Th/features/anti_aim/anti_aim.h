#pragma once

class CUserCmd;

namespace AntiAim {
void OnCreateMove(CUserCmd* cmd);
extern bool g_aa_active;
extern float g_realPitch;
extern float g_realYaw;
}
