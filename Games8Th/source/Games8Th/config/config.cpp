#include "config.h"
#include "../keybinds/keybinds.h"
#include "../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <map>

// CBA to make proper atm, it's 03:42 right now.
// For now just stores config values don't mind it too much
//
// (FYI THIS IS A HORRID SOLUTION BUT FUNCTIONS) 

namespace Config {
	std::atomic<bool> loading{ false };
	bool config_compress = true; // default: LFAC + XPRESS_HUFF; toggle in menu to write plain JSON
	float ui_font_size = 16.0f;
	bool esp = false;
	bool glow = false;
	bool glow_team = true;
	bool glow_enemy = true;
	bool glow_only_visible = false;
 // Glow ESP owns these colours alone
	ImVec4 glow_color = ImVec4(0.25f, 0.85f, 1.f, 1.f);
	ImVec4 glow_color_invis = ImVec4(1.f, 0.35f, 0.85f, 1.f);
	bool glow_world_weapons = false;
	ImVec4 glow_world_weapon_color = ImVec4(0.95f, 0.90f, 0.55f, 1.0f);
	bool glow_world_grenades = false;
	bool showHealth = false;
	bool showArmor = false;
	bool showDistance = false;
	bool showWeapon = false;
	bool showWeaponIcon = false;
 bool teamCheck = true; // match team_check default (ESP/glow)
	bool espFill = false;
	bool showNameTags = false;
	bool esp_name_avatar = false;
	bool esp_skeleton = false;
	float esp_skeleton_thickness = 1.5f;
	ImVec4 esp_skeleton_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 esp_skeleton_color_invisible = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
	bool esp_skeleton_head = true;
	bool esp_vis_check = true; // on by default (user request) - was off to avoid Present traces, now enabled at inject

	bool flag_flashed = false;
	bool flag_bomb = false;
	bool flag_scoped = false;
	bool flag_reloading = false;
	bool flag_defusing = false;
	bool flag_money = false;
	bool flag_kit = false;
	bool flag_helmet = false;
	bool flag_nades = false;
	bool esp_rank = false;
	bool esp_3d_box = false;
	bool esp_oof = false;
	float esp_oof_radius = 280.f;
	float esp_oof_size = 14.f;
	ImVec4 esp_oof_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
	ImVec4 esp_3d_box_color = ImVec4(1.f, 0.45f, 0.2f, 0.95f);
	ImVec4 esp_rank_color = ImVec4(0.85f, 0.9f, 1.f, 1.f);

	bool float_damage = false;
	float float_damage_duration = 1.1f;
	float float_damage_speed = 55.f;
	ImVec4 float_damage_color = ImVec4(1.f, 1.f, 1.f, 1.f);
	ImVec4 float_damage_head_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
	ImVec4 float_damage_kill_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);

	bool world_esp_weapons = false;
	bool world_esp_weapon_icon = true;
	bool world_esp_weapon_distance = false;
	bool world_esp_bomb = false; // off by default ? idle inject must not force work
	bool world_esp_bomb_timer = true;
	bool world_esp_smoke = false;
	bool world_esp_molotov = false;
	bool world_esp_he = false;
	bool world_esp_flash = false;
	bool world_esp_decoy = false;
	ImVec4 world_esp_weapon_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 world_esp_weapon_distance_color = ImVec4(0.85f, 0.85f, 0.90f, 1.0f);
	ImVec4 world_esp_bomb_color = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
	ImVec4 world_esp_smoke_color = ImVec4(0.75f, 0.80f, 0.90f, 1.0f);
	ImVec4 world_esp_molotov_color = ImVec4(1.0f, 0.50f, 0.15f, 1.0f);
	ImVec4 world_esp_he_color = ImVec4(1.0f, 0.70f, 0.25f, 1.0f);
	ImVec4 world_esp_flash_color = ImVec4(0.95f, 0.95f, 0.55f, 1.0f);
	ImVec4 world_esp_decoy_color = ImVec4(0.65f, 0.85f, 0.55f, 1.0f);

	bool Night = false;
	float night_exposure = 0.45f; // darkness 0..1 (0 = none, 1 = darkest)

	bool skybox = false;
	ImVec4 skybox_color = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);

	bool lighting = false;
	ImVec4 lighting_color = ImVec4(1.0f, 0.92f, 0.75f, 1.0f);

	bool map_color = false;
	ImVec4 map_color_value = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);


	bool custom_fog = false;
	ImVec4 custom_fog_color = ImVec4(0.58f, 0.62f, 0.85f, 1.0f);
	float custom_fog_start = 100.f;
	float custom_fog_end = 3000.f;
	float custom_fog_falloff = 1.f;

	bool weather = false;
	int weather_mode = 1; // Snow default when enabled (1..4)
	float weather_intensity = 0.55f;

	// Chams - mercey defaults (chams::ChamIds indices)
	bool enemyChams = false;
	bool enemyChamsInvisible = false;
	int chamsMaterial = 3;    // flat
	int chamsMaterialXQZ = 14; // flat_ignorez
	ImVec4 colVisualChams = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 150.f / 255.f);
	ImVec4 colVisualChamsIgnoreZ = ImVec4(1.f, 208.f / 255.f, 243.f / 255.f, 118.f / 255.f);
	bool teamChams = false;
	bool teamChamsInvisible = false;
	int teamChamsMaterial = 3;
	int teamChamsMaterialXQZ = 14;
	ImVec4 teamcolVisualChams = ImVec4(1.f, 1.f, 1.f, 1.f);
	ImVec4 teamcolVisualChamsIgnoreZ = ImVec4(1.f, 1.f, 1.f, 1.f);
	bool localChams = false;
	int localChamsMaterial = 5; // outlines
	ImVec4 colLocalChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 175.f / 255.f);
	bool ragdollChams = false;
	int ragdollChamsMaterial = 3;
	ImVec4 colRagdollChams = ImVec4(140.f / 255.f, 140.f / 255.f, 150.f / 255.f, 190.f / 255.f);
	bool armChams = false;
	int armChamsMaterial = 5;
	ImVec4 colArmChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 1.f);
	bool viewmodelChams = false;
	int viewmodelChamsMaterial = 6; // glow
	ImVec4 colViewmodelChams = ImVec4(217.f / 255.f, 173.f / 255.f, 202.f / 255.f, 175.f / 255.f);
	bool itemChams = false;
	bool itemChamsInvisible = false;
	int itemChamsMaterial = 3;
	int itemChamsMaterialXQZ = 14;
	ImVec4 colItemChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 1.f);
	ImVec4 colItemChamsIgnoreZ = ImVec4(1.f, 208.f / 255.f, 243.f / 255.f, 118.f / 255.f);
	bool itemChamsPistol = false;
	bool itemChamsSmg = false;
	bool itemChamsRifle = false;
	bool itemChamsShotgun = false;
	bool itemChamsSniper = false;
	bool itemChamsUtility = false;

	float espThickness = 1.0f;
	float espFillOpacity = 0.5f;
	ImVec4 espColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	ImVec4 espColorInvisible = ImVec4(0.45f, 0.55f, 1.0f, 1.0f);

	int esp_box_style = ESP_BOX_FULL;
	float esp_box_width = 0.42f;

	int esp_pos_name = ESP_POS_TOP;
	int esp_pos_weapon = ESP_POS_BOTTOM;
	int esp_pos_weapon_icon = ESP_POS_BOTTOM;
	int esp_pos_distance = ESP_POS_BOTTOM;
	int esp_pos_health = ESP_POS_LEFT;
	int esp_pos_armor = ESP_POS_LEFT;
	float esp_bar_width = 3.0f;
	bool esp_health_auto = true;
	ImVec4 esp_health_color = ImVec4(0.20f, 0.90f, 0.25f, 1.0f);
	ImVec4 esp_armor_color = ImVec4(0.27f, 0.63f, 1.0f, 1.0f);
	ImVec4 esp_name_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 esp_weapon_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 esp_weapon_icon_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 esp_distance_color = ImVec4(0.71f, 0.86f, 1.0f, 1.0f);

	bool fovEnabled = false;
	bool aspect_ratio_enabled = false;
	float aspect_ratio = 1.777778f; // 16:9
	float fov = 90.0f;

	bool viewmodel_changer = false;
	float viewmodel_fov = 68.f;
	float viewmodel_x = 0.f;
	float viewmodel_y = 0.f;
	float viewmodel_z = 0.f;

	bool thirdperson = false;
	float thirdperson_distance = 150.f;
	int thirdperson_key = 0x04; // VK_MBUTTON
	int thirdperson_key_mode = 2; // Toggle

	float antiflash_amount = 0.f;

	bool remove_legs = false;
	bool remove_smoke = false;
	bool remove_decals = false;
	bool smoke_color = false;
	ImVec4 smoke_color_value = ImVec4(0.55f, 0.70f, 0.95f, 1.f);
	bool fire_color = false;
	ImVec4 fire_color_value = ImVec4(1.f, 0.35f, 0.05f, 1.f);
	bool inferno_color = false;
	ImVec4 inferno_color_value = ImVec4(1.f, 0.35f, 0.05f, 1.f);
	bool explosion_color = false;
	ImVec4 explosion_color_value = ImVec4(0.f, 1.f, 1.f, 1.f);
	bool remove_crosshair = false;
	bool force_crosshair = false;
	bool autowall_xhair = false;
	int autowall_xhair_style = 0;
	float autowall_xhair_size = 6.f;
	ImVec4 autowall_xhair_can = ImVec4(0.25f, 1.f, 0.35f, 1.f);
	ImVec4 autowall_xhair_cant = ImVec4(1.f, 0.28f, 0.28f, 1.f);
	bool remove_recoil = false;

	bool auto_pistol = false;
	float auto_pistol_delay_ms = 0.f;
	bool enemy_spectate = false;
	bool enemy_spectate_thirdperson = false;
	bool auto_defuse = false;

	bool scope_custom_lines = false;
	float scope_line_size = 1.f;
	float scope_line_gap = 4.f;
	float scope_line_thickness = 0.5f;
	ImVec4 scope_line_color = ImVec4(0.f, 0.f, 0.f, 1.f);
	bool scope_zoom_fov = false;
	float scope_fov_1 = 40.f;
	float scope_fov_2 = 15.f;
	bool scope_hide_viewmodel = false;

	bool aimbot = false;
	float aimbot_fov = 5.f;
	float aimbot_smooth = 5.f; // 0 = instant, higher = smoother
	float aimbot_humanize = 0.f; // 0 = off (recommended)
	int aimbot_smooth_mode = SMOOTH_LINEAR;

	bool autofire = false;
	bool autofire_silent = false;
	float autofire_fov = 5.f;
	float autofire_hitchance = 70.f; // default on - 0 = off
	int autofire_mode = AF_MODE_HITCHANCE;
	bool autofire_autostop = false;
	bool autofire_autoscope = false;
	bool autofire_scoped_only = false;
	bool autofire_autowall = false;
	bool autowall = true; // keybind host - Always = on whenever AF/TR AW checkbox on
	int autowall_key = 0;
	int autowall_key_mode = 0; // Always
	bool mindamage_override = false;
	int mindamage_override_key = 0;
	int mindamage_override_key_mode = 1; // Hold
	float mindamage_override_value = 10.f;
	float autofire_mindamage = 1.f;    // visible
	float autofire_mindamage_aw = 1.f; // through wall
	int autofire_key = 0x06; // VK_XBUTTON2
	int autofire_key_mode = 1; // Hold
	int autofire_target_select = AF_TARGET_CROSSHAIR;
	bool autofire_vis_check = true;
	bool autofire_flash_check = true;
	bool autofire_smoke_check = false;
	bool autofire_focus_target = true;
	bool autofire_multipoint_dynamic = true;
	bool autofire_body_if_lethal = false;
	bool autofire_prefer_body = false;
	bool autofire_hitboxes[HB_COUNT] = {
		true,  // head
		true,  // neck
		true,  // chest
		false, // stomach
		false, // pelvis
		false, // arms
		false, // legs
		false  // feet
	};
	// Multipoint enable - Head/Chest/Stomach/Pelvis only (neck/limbs unused)
	bool autofire_multipoint[HB_COUNT] = {
		true,  // head
		false, // neck (not in MP list)
		true,  // chest
		false, // stomach
		false, // pelvis
		false, // arms
		false, // legs
		false  // feet
	};
	float autofire_multipoint_scale[HB_COUNT] = {
		0.55f, // head
		0.50f, // neck
		0.60f, // chest
		0.55f, // stomach
		0.50f, // pelvis
		0.45f, // arms
		0.45f, // legs
		0.40f  // feet
	};

	bool team_check = true;
	bool aim_vis_check = true;
	bool aim_smoke_check = false;
	bool aim_flash_check = false;
	bool aim_scoped_only = false;
	float aim_reaction_delay_ms = 0.f;
	float aim_target_switch_delay_ms = 0.f;
	float aim_first_shot_delay_ms = 0.f;
	// default: head + neck + chest
	bool aim_hitboxes[HB_COUNT] = {
		true,  // head
		true,  // neck
		true,  // chest
		false, // stomach
		false, // pelvis
		false, // arms
		false, // legs
		false  // feet
	};
	bool rcs = false;
	bool rcs_standalone = false;
	float rcs_scale_x = 0.5f;
	float rcs_scale_y = 0.5f;
	float rcs_smooth = 0.f;
	bool fov_circle = false;
	bool fov_circle_autofire = false;
	bool fov_circle_magnet = false;
	ImVec4 fovCircleColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 fovCircleColorAf = ImVec4(1.0f, 0.45f, 0.15f, 0.90f);
	ImVec4 fovCircleColorMagnet = ImVec4(0.35f, 0.85f, 1.0f, 0.90f);
	bool anti_aim = false;
	int anti_aim_mode = 0;
	int anti_aim_pitch_mode = AA_PITCH_OFF;
	float anti_aim_pitch_angle = 0.f;
	float anti_aim_pitch_jitter_min = -45.f;
	float anti_aim_pitch_jitter_max = 45.f;
	int anti_aim_yaw_mode = AA_YAW_OFF;
	float anti_aim_yaw_angle = 180.f;
	bool anti_aim_yaw_at_target = false;
	int anti_aim_manual_key_left = 0;
	int anti_aim_manual_key_right = 0;
	int anti_aim_manual_key_back = 0;
	bool anti_aim_hideshots = false;
	bool anti_aim_avoid_backstab = true;   // velocity-style: yaw away from nearby knife enemies
	bool anti_aim_yaw_adjust = true;      // velocity-style: +33deg compensate model roll

	AimWeaponProfile weapon_profiles[WG_COUNT]{};
	int weapon_group_ui = WG_RIFLE;
	int weapon_group_active = WG_GENERAL;

	int aimbot_key = 0x05; // VK_XBUTTON1
	int aimbot_key_mode = 1; // Hold

	bool triggerbot = false;
	int triggerbot_key = 0x12; // VK_MENU (Alt) - avoids autofire mouse5 clash
	int triggerbot_key_mode = 1; // Hold
	float trigger_delay_ms = 0.f;   // 0 = instant (flicks); raise for humanize
	float trigger_hitchance = 0.f;  // 0 = off; HC slows flick response
	bool trigger_autowall = false;
	float trigger_mindamage = 1.f;
	float trigger_mindamage_aw = 1.f;
	bool trigger_scoped_only = false;
	bool trigger_flash_check = true;
	bool trigger_smoke_check = false;
	bool trigger_hitboxes[HB_COUNT] = {
		true,  // HEAD
		true,  // NECK
		true,  // CHEST
		false, // STOMACH
		false, // PELVIS
		false, // ARMS
		false, // LEGS
		false  // FEET
	};
	bool trigger_autostop = false;
	int trigger_mode = TR_MODE_HITCHANCE;
	bool trigger_magnet = false;
	float trigger_magnet_smooth = 12.f;
	float trigger_magnet_fov = 4.f;
	bool trigger_magnet_silent = false;
	bool trigger_magnet_head_prio = true;
	float trigger_magnet_deadzone = 0.12f;
	bool trigger_magnet_hitboxes[HB_COUNT] = {};

	static AimWeaponProfile MakeDefaultProfile() {
		AimWeaponProfile p{};
		p.aimbot_fov = 5.f;
		p.aimbot_smooth = 5.f;
		p.aimbot_humanize = 0.f;
		p.aimbot_smooth_mode = SMOOTH_LINEAR;
		p.aim_vis_check = true;
		p.aim_smoke_check = false;
		p.aim_flash_check = false;
		p.aim_scoped_only = false;
		for (int i = 0; i < HB_COUNT; ++i)
			p.aim_hitboxes[i] = false;
		p.aim_hitboxes[HB_HEAD] = true;
		p.aim_hitboxes[HB_NECK] = true;
		p.aim_hitboxes[HB_CHEST] = true;
		p.aim_reaction_delay_ms = 0.f;
		p.aim_target_switch_delay_ms = 0.f;
		p.aim_first_shot_delay_ms = 0.f;
		p.rcs = false;
		p.rcs_standalone = false;
		p.rcs_scale_x = 0.5f;
		p.rcs_scale_y = 0.5f;
		p.rcs_smooth = 0.f;
		p.autofire_fov = 5.f;
		p.autofire_hitchance = 70.f;
		p.autofire_mode = AF_MODE_HITCHANCE;
		p.autofire_autostop = false;
		p.autofire_autoscope = false;
		p.autofire_scoped_only = false;
		p.autofire_autowall = false;
		p.autofire_mindamage = 1.f;
		p.autofire_mindamage_aw = 1.f;
		p.mindamage_override = false;
		p.mindamage_override_value = 10.f;
		p.autofire_target_select = AF_TARGET_CROSSHAIR;
		p.autofire_vis_check = true;
		p.autofire_flash_check = true;
		p.autofire_smoke_check = false;
		p.autofire_focus_target = true;
		p.autofire_multipoint_dynamic = true;
		p.autofire_body_if_lethal = false;
		p.autofire_prefer_body = false;
		for (int i = 0; i < HB_COUNT; ++i) {
			p.autofire_hitboxes[i] = false;
			p.autofire_multipoint[i] = false;
			p.autofire_multipoint_scale[i] = 0.50f;
		}
		p.autofire_hitboxes[HB_HEAD] = true;
		p.autofire_hitboxes[HB_NECK] = true;
		p.autofire_hitboxes[HB_CHEST] = true;
		p.autofire_multipoint[HB_HEAD] = true;
		p.autofire_multipoint[HB_CHEST] = true;
		p.autofire_multipoint_scale[HB_HEAD] = 0.55f;
		p.autofire_multipoint_scale[HB_NECK] = 0.50f;
		p.autofire_multipoint_scale[HB_CHEST] = 0.60f;
		p.autofire_multipoint_scale[HB_STOMACH] = 0.55f;
		p.autofire_multipoint_scale[HB_PELVIS] = 0.50f;
		p.autofire_multipoint_scale[HB_ARMS] = 0.45f;
		p.autofire_multipoint_scale[HB_LEGS] = 0.45f;
		p.autofire_multipoint_scale[HB_FEET] = 0.40f;
		p.trigger_delay_ms = 0.f;
		p.trigger_hitchance = 0.f;
		p.trigger_autowall = false;
		p.trigger_mindamage = 1.f;
		p.trigger_mindamage_aw = 1.f;
		p.trigger_scoped_only = false;
		p.trigger_flash_check = true;
		p.trigger_smoke_check = false;
		for (int i = 0; i < HB_COUNT; ++i)
			p.trigger_hitboxes[i] = false;
		p.trigger_hitboxes[HB_HEAD] = true;
		p.trigger_hitboxes[HB_NECK] = true;
		p.trigger_hitboxes[HB_CHEST] = true;
		p.trigger_autostop = false;
		p.trigger_mode = TR_MODE_HITCHANCE;
		p.trigger_magnet = false;
		p.trigger_magnet_smooth = 12.f;
		p.trigger_magnet_fov = 4.f;
		p.trigger_magnet_silent = false;
		p.trigger_magnet_head_prio = true;
		p.trigger_magnet_deadzone = 0.12f;
		for (int i = 0; i < HB_COUNT; ++i)
			p.trigger_magnet_hitboxes[i] = false;
		return p;
	}

	void InitWeaponProfilesDefaults() {
		const AimWeaponProfile base = MakeDefaultProfile();
		for (int g = 0; g < WG_COUNT; ++g)
			weapon_profiles[g] = base;

		// Snipers - tighter FOV, higher hitchance (autostop off unless user enables)
		weapon_profiles[WG_SNIPER].aimbot_fov = 3.f;
		weapon_profiles[WG_SNIPER].autofire_fov = 3.f;
		weapon_profiles[WG_SNIPER].aimbot_smooth = 3.f;
		weapon_profiles[WG_SNIPER].autofire_hitchance = 80.f;
		weapon_profiles[WG_SNIPER].autofire_autostop = false;
		weapon_profiles[WG_SNIPER].autofire_mindamage = 70.f;
		weapon_profiles[WG_SNIPER].autofire_mindamage_aw = 90.f;
		weapon_profiles[WG_SNIPER].trigger_mindamage = 70.f;
		weapon_profiles[WG_SNIPER].trigger_mindamage_aw = 90.f;
		weapon_profiles[WG_SNIPER].trigger_scoped_only = false;
		weapon_profiles[WG_SNIPER].trigger_delay_ms = 0.f;
		weapon_profiles[WG_SNIPER].trigger_hitchance = 0.f;
		for (int i = 0; i < HB_COUNT; ++i)
			weapon_profiles[WG_SNIPER].trigger_hitboxes[i] = false;
		weapon_profiles[WG_SNIPER].trigger_hitboxes[HB_HEAD] = true;

		// Pistols - wider FOV, less RCS
		weapon_profiles[WG_PISTOL].aimbot_fov = 8.f;
		weapon_profiles[WG_PISTOL].autofire_fov = 8.f;
		weapon_profiles[WG_PISTOL].autofire_hitchance = 60.f;
		weapon_profiles[WG_PISTOL].rcs = false;

		// Rifles - default RCS-friendly
		weapon_profiles[WG_RIFLE].rcs = true;
		weapon_profiles[WG_RIFLE].rcs_scale_x = 0.55f;
		weapon_profiles[WG_RIFLE].rcs_scale_y = 0.55f;

		// SMG - slightly wider
		weapon_profiles[WG_SMG].aimbot_fov = 7.f;
		weapon_profiles[WG_SMG].autofire_fov = 7.f;
		weapon_profiles[WG_SMG].autofire_hitchance = 65.f;

		// Shotgun - close FOV, low hitchance
		weapon_profiles[WG_SHOTGUN].aimbot_fov = 10.f;
		weapon_profiles[WG_SHOTGUN].autofire_fov = 10.f;
		weapon_profiles[WG_SHOTGUN].autofire_hitchance = 40.f;
		weapon_profiles[WG_SHOTGUN].autofire_mindamage = 20.f;

		// LMG - wider spray, RCS
		weapon_profiles[WG_LMG].aimbot_fov = 6.f;
		weapon_profiles[WG_LMG].autofire_fov = 6.f;
		weapon_profiles[WG_LMG].rcs = true;
		weapon_profiles[WG_LMG].autofire_hitchance = 55.f;

		weapon_group_ui = WG_RIFLE;
		weapon_group_active = WG_GENERAL;
		ApplyWeaponGroup(nullptr);
	}

	const char* WeaponGroupName(int group) {
		switch (group) {
		case WG_GENERAL: return "General";
		case WG_PISTOL:  return "Pistols";
		case WG_SMG:     return "SMGs";
		case WG_RIFLE:   return "Rifles";
		case WG_SHOTGUN: return "Shotguns";
		case WG_SNIPER:  return "Snipers";
		case WG_LMG:     return "LMGs";
		default:         return "General";
		}
	}

	int ClassifyWeaponGroup(C_CSWeaponBase* weapon) {
		if (!weapon)
			return WG_GENERAL;
		__try {
			if (weapon->IsNonGunWeapon())
				return WG_GENERAL;
			auto* vdata = weapon->Data();
			if (vdata) {
				const int t = vdata->m_WeaponType();
				// CCSWeaponType: pistol=1 smg=2 rifle=3 shotgun=4 sniper=5 machinegun=6
				if (t >= 1 && t <= 6)
					return t; // maps 1:1 onto WG_PISTOL..WG_LMG
			}
			// Defindex fallback
			const std::uint16_t def = weapon->m_iItemDefinitionIndex();
			switch (def) {
			case 1: case 2: case 3: case 4: case 30: case 32: case 36:
			case 61: case 63: case 64:
				return WG_PISTOL;
			case 17: case 19: case 23: case 24: case 26: case 33: case 34:
				return WG_SMG;
			case 7: case 8: case 10: case 13: case 16: case 39: case 60:
				return WG_RIFLE;
			case 25: case 27: case 29: case 35:
				return WG_SHOTGUN;
			case 9: case 11: case 38: case 40:
				return WG_SNIPER;
			case 14: case 28:
				return WG_LMG;
			default:
				return WG_GENERAL;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return WG_GENERAL;
		}
	}

	AimWeaponProfile& MenuAimProfile() {
		if (weapon_group_ui < 0 || weapon_group_ui >= WG_COUNT)
			weapon_group_ui = WG_GENERAL;
		return weapon_profiles[weapon_group_ui];
	}

	void PullLiveIntoProfile(int group) {
		if (group < 0 || group >= WG_COUNT)
			return;
		AimWeaponProfile& p = weapon_profiles[group];
		p.aimbot_fov = aimbot_fov;
		p.aimbot_smooth = aimbot_smooth;
		p.aimbot_humanize = aimbot_humanize;
		p.aimbot_smooth_mode = aimbot_smooth_mode;
		p.aim_vis_check = aim_vis_check;
		p.aim_smoke_check = aim_smoke_check;
		p.aim_flash_check = aim_flash_check;
		p.aim_scoped_only = aim_scoped_only;
		p.autofire_scoped_only = autofire_scoped_only;
		p.trigger_scoped_only = trigger_scoped_only;
		std::memcpy(p.aim_hitboxes, aim_hitboxes, sizeof(aim_hitboxes));
		p.aim_reaction_delay_ms = aim_reaction_delay_ms;
		p.aim_target_switch_delay_ms = aim_target_switch_delay_ms;
		p.aim_first_shot_delay_ms = aim_first_shot_delay_ms;
		p.rcs = rcs;
		p.rcs_standalone = rcs_standalone;
		p.rcs_scale_x = rcs_scale_x;
		p.rcs_scale_y = rcs_scale_y;
		p.rcs_smooth = rcs_smooth;
		p.autofire_fov = autofire_fov;
		p.autofire_hitchance = autofire_hitchance;
		p.autofire_mode = autofire_mode;
		p.autofire_autostop = autofire_autostop;
		p.autofire_autoscope = autofire_autoscope;
		p.autofire_autowall = autofire_autowall;
		p.autofire_mindamage = autofire_mindamage;
		p.autofire_mindamage_aw = autofire_mindamage_aw;
		p.mindamage_override = mindamage_override;
		p.mindamage_override_value = mindamage_override_value;
		p.autofire_target_select = autofire_target_select;
		p.autofire_vis_check = autofire_vis_check;
		p.autofire_flash_check = autofire_flash_check;
		p.autofire_smoke_check = autofire_smoke_check;
		p.autofire_focus_target = autofire_focus_target;
		p.autofire_multipoint_dynamic = autofire_multipoint_dynamic;
		p.autofire_body_if_lethal = autofire_body_if_lethal;
		p.autofire_prefer_body = autofire_prefer_body;
		std::memcpy(p.autofire_hitboxes, autofire_hitboxes, sizeof(autofire_hitboxes));
		std::memcpy(p.autofire_multipoint, autofire_multipoint, sizeof(autofire_multipoint));
		std::memcpy(p.autofire_multipoint_scale, autofire_multipoint_scale, sizeof(autofire_multipoint_scale));
		p.trigger_delay_ms = trigger_delay_ms;
		p.trigger_hitchance = trigger_hitchance;
		p.trigger_autowall = trigger_autowall;
		p.trigger_mindamage = trigger_mindamage;
		p.trigger_mindamage_aw = trigger_mindamage_aw;
		p.aim_scoped_only = aim_scoped_only;
		p.autofire_scoped_only = autofire_scoped_only;
		p.trigger_scoped_only = trigger_scoped_only;
		p.trigger_flash_check = trigger_flash_check;
		p.trigger_smoke_check = trigger_smoke_check;
		std::memcpy(p.trigger_hitboxes, trigger_hitboxes, sizeof(trigger_hitboxes));
		p.trigger_autostop = trigger_autostop;
		p.trigger_mode = trigger_mode;
		p.trigger_magnet = trigger_magnet;
		p.trigger_magnet_smooth = trigger_magnet_smooth;
		p.trigger_magnet_fov = trigger_magnet_fov;
		p.trigger_magnet_silent = trigger_magnet_silent;
		p.trigger_magnet_head_prio = trigger_magnet_head_prio;
		p.trigger_magnet_deadzone = trigger_magnet_deadzone;
		std::memcpy(p.trigger_magnet_hitboxes, trigger_magnet_hitboxes, sizeof(trigger_magnet_hitboxes));
	}

	void ApplyProfileToLive(int group) {
		if (group < 0 || group >= WG_COUNT)
			group = WG_GENERAL;
		const AimWeaponProfile& p = weapon_profiles[group];
		aimbot_fov = p.aimbot_fov;
		aimbot_smooth = p.aimbot_smooth;
		aimbot_humanize = p.aimbot_humanize;
		aimbot_smooth_mode = p.aimbot_smooth_mode;
		if (aimbot_smooth_mode < 0 || aimbot_smooth_mode >= SMOOTH_MODE_COUNT)
			aimbot_smooth_mode = SMOOTH_LINEAR;
		aim_vis_check = p.aim_vis_check;
		aim_smoke_check = p.aim_smoke_check;
		aim_flash_check = p.aim_flash_check;
		aim_scoped_only = p.aim_scoped_only;
		autofire_scoped_only = p.autofire_scoped_only;
		trigger_scoped_only = p.trigger_scoped_only;
		std::memcpy(aim_hitboxes, p.aim_hitboxes, sizeof(aim_hitboxes));
		aim_reaction_delay_ms = p.aim_reaction_delay_ms;
		aim_target_switch_delay_ms = p.aim_target_switch_delay_ms;
		aim_first_shot_delay_ms = p.aim_first_shot_delay_ms;
		rcs = p.rcs;
		rcs_standalone = p.rcs_standalone;
		rcs_scale_x = p.rcs_scale_x;
		rcs_scale_y = p.rcs_scale_y;
		rcs_smooth = p.rcs_smooth;
		// autofire_silent is global - never overwrite from weapon profile
		autofire_fov = p.autofire_fov;
		autofire_hitchance = p.autofire_hitchance;
		autofire_mode = p.autofire_mode;
		if (autofire_mode < 0 || autofire_mode >= AF_MODE_COUNT)
			autofire_mode = AF_MODE_HITCHANCE;
		autofire_autostop = p.autofire_autostop;
		autofire_autoscope = p.autofire_autoscope;
		autofire_autowall = p.autofire_autowall;
		autofire_mindamage = p.autofire_mindamage;
		autofire_mindamage_aw = p.autofire_mindamage_aw;
		mindamage_override = p.mindamage_override;
		mindamage_override_value = p.mindamage_override_value;
		autofire_target_select = p.autofire_target_select;
		if (autofire_target_select < 0 || autofire_target_select >= AF_TARGET_COUNT)
			autofire_target_select = AF_TARGET_CROSSHAIR;
		autofire_vis_check = p.autofire_vis_check;
		autofire_flash_check = p.autofire_flash_check;
		autofire_smoke_check = p.autofire_smoke_check;
		autofire_focus_target = p.autofire_focus_target;
		autofire_multipoint_dynamic = p.autofire_multipoint_dynamic;
		autofire_body_if_lethal = p.autofire_body_if_lethal;
		autofire_prefer_body = p.autofire_prefer_body;
		std::memcpy(autofire_hitboxes, p.autofire_hitboxes, sizeof(autofire_hitboxes));
		std::memcpy(autofire_multipoint, p.autofire_multipoint, sizeof(autofire_multipoint));
		std::memcpy(autofire_multipoint_scale, p.autofire_multipoint_scale, sizeof(autofire_multipoint_scale));
		trigger_delay_ms = p.trigger_delay_ms;
		trigger_hitchance = p.trigger_hitchance;
		trigger_autowall = p.trigger_autowall;
		trigger_mindamage = p.trigger_mindamage;
		trigger_mindamage_aw = p.trigger_mindamage_aw;
		trigger_flash_check = p.trigger_flash_check;
		trigger_smoke_check = p.trigger_smoke_check;
		std::memcpy(trigger_hitboxes, p.trigger_hitboxes, sizeof(trigger_hitboxes));
		trigger_autostop = p.trigger_autostop;
		trigger_mode = p.trigger_mode;
		if (trigger_mode < 0 || trigger_mode >= TR_MODE_COUNT)
			trigger_mode = TR_MODE_HITCHANCE;
		trigger_magnet = p.trigger_magnet;
		trigger_magnet_smooth = p.trigger_magnet_smooth;
		trigger_magnet_fov = p.trigger_magnet_fov;
		if (trigger_magnet_fov < 0.5f) trigger_magnet_fov = 0.5f;
		if (trigger_magnet_fov > 30.f) trigger_magnet_fov = 30.f;
		trigger_magnet_silent = p.trigger_magnet_silent;
		trigger_magnet_head_prio = p.trigger_magnet_head_prio;
		trigger_magnet_deadzone = p.trigger_magnet_deadzone;
		if (trigger_magnet_deadzone < 0.f) trigger_magnet_deadzone = 0.f;
		if (trigger_magnet_deadzone > 1.f) trigger_magnet_deadzone = 1.f;
		std::memcpy(trigger_magnet_hitboxes, p.trigger_magnet_hitboxes, sizeof(trigger_magnet_hitboxes));
	}

	void ApplyWeaponGroup(C_CSWeaponBase* weapon) {
		const int g = ClassifyWeaponGroup(weapon);
		weapon_group_active = g;
		ApplyProfileToLive(g);
	}

	bool bhop = false;
	bool autostrafe = false;
	int autostrafe_mode = 1; // 0 mouse, 1 WASD yaw subtick
	bool jumpbug = false;
	int jumpbug_key = 0;
	int jumpbug_key_mode = 0;
	bool edgejump = false;
	int edgejump_key = 0;
	int edgejump_key_mode = 0;
	bool hitlog = false;
	bool hitlog_console = false;
	float hitlog_duration = 4.f;
	ImVec2 hitlog_pos = ImVec2(-1.f, -1.f);
	float hitlog_width = 268.f;
	int hitlog_max_rows = 8;
	bool hitlog_show_hp = true;
	bool hitlog_show_stats = true;
	ImVec4 hitlog_color = ImVec4(0.92f, 0.92f, 0.95f, 0.95f);
	ImVec4 hitlog_head_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);
	ImVec4 hitlog_kill_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
	bool bullet_impact_effect = false;
	int bullet_impact_effect_type = 0;
	ImVec4 bullet_impact_effect_fill_color = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 85.f / 255.f);
	ImVec4 bullet_impact_effect_edge_color = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 1.f);
	ImVec4 bullet_impact_effect_color_spark = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 1.f);
	float bullet_impact_effect_duration = 2.5f;
	bool bullet_impact_effect_glow = true;
	float bullet_impact_effect_glow_strength = 1.f;
	bool bullet_tracers = false;
	ImVec4 bullet_tracer_color = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 1.f);
	float bullet_tracer_duration = 0.5f;
	bool auto_accept = false;
	bool scoreboard_weapons = false;
	bool unlock_inventory = true; // Andromeda parity: knife/weapon skins need IsLoadoutAllowed hook true (knife defs 500+ blocked in MM otherwise)

	bool skin_knife = false;
	bool skin_glove = false;
	bool skin_agent = false;
	int skin_knife_def = 0;
	int skin_glove_def = 0;
	int skin_agent_t = 0;
	int skin_agent_ct = 0;
	int skin_knife_paint = 0;
	int skin_glove_paint = 0;
	float skin_knife_wear = 0.f;
	float skin_glove_wear = 0.f;
	int skin_knife_seed = 0;
	int skin_glove_seed = 0;
	bool skin_knife_stattrak = false;
	bool skin_glove_stattrak = false;
	char skin_knife_tag[64]{};
	char skin_glove_tag[64]{};
	std::map<int, WeaponSkin> skin_weapons;

	bool custom_paint_enabled = false;
	ImVec4 custom_paint_color0 = ImVec4(1.f, 1.f, 1.f, 1.f);
	ImVec4 custom_paint_color1 = ImVec4(1.f, 1.f, 1.f, 1.f);
	ImVec4 custom_paint_color2 = ImVec4(1.f, 1.f, 1.f, 1.f);
	ImVec4 custom_paint_color3 = ImVec4(1.f, 1.f, 1.f, 1.f);
	bool custom_paint_glove_enabled = false;
	ImVec4 custom_paint_glove_color = ImVec4(1.f, 1.f, 1.f, 1.f);

	// SRWLOCK guards skin_weapons: menu/Present thread writes (skin menu
	// apply/disable, config Load/Save/Reset), game thread reads (FSN skin
	// walk + hkUnlockInventory). Zero-init safe for manual mapping.
	static SRWLOCK g_skinMapLock = SRWLOCK_INIT;

	bool SkinWeapon_Find(int def, WeaponSkin& out) {
		AcquireSRWLockShared(&g_skinMapLock);
		const auto it = skin_weapons.find(def);
		if (it == skin_weapons.end()) {
			ReleaseSRWLockShared(&g_skinMapLock);
			return false;
		}
		out = it->second;
		ReleaseSRWLockShared(&g_skinMapLock);
		return true;
	}

	bool SkinWeapon_Has(int def) {
		AcquireSRWLockShared(&g_skinMapLock);
		const bool has = skin_weapons.find(def) != skin_weapons.end();
		ReleaseSRWLockShared(&g_skinMapLock);
		return has;
	}

	bool SkinWeapon_Empty() {
		AcquireSRWLockShared(&g_skinMapLock);
		const bool empty = skin_weapons.empty();
		ReleaseSRWLockShared(&g_skinMapLock);
		return empty;
	}

	void SkinWeapon_Set(int def, const WeaponSkin& ws) {
		AcquireSRWLockExclusive(&g_skinMapLock);
		skin_weapons[def] = ws;
		ReleaseSRWLockExclusive(&g_skinMapLock);
	}

	void SkinWeapon_Erase(int def) {
		AcquireSRWLockExclusive(&g_skinMapLock);
		skin_weapons.erase(def);
		ReleaseSRWLockExclusive(&g_skinMapLock);
	}

	void SkinWeapon_Clear() {
		AcquireSRWLockExclusive(&g_skinMapLock);
		skin_weapons.clear();
		ReleaseSRWLockExclusive(&g_skinMapLock);
	}

	std::map<int, WeaponSkin> SkinWeapon_Snapshot() {
		AcquireSRWLockShared(&g_skinMapLock);
		std::map<int, WeaponSkin> copy = skin_weapons;
		ReleaseSRWLockShared(&g_skinMapLock);
		return copy;
	}

	bool sound_esp = false;
	float sound_esp_duration = 1.4f;
	float sound_esp_ring_size = 1.f;
	ImVec4 sound_esp_color = ImVec4(0.35f, 0.85f, 1.f, 0.95f);

	bool vote_reveal = false;
	bool vote_auto = false;
	int vote_auto_choice = 0; // Yes
	float vote_auto_delay_ms = 250.f;

	bool backtrack = false;
	float backtrack_ms = 100.f;
	bool backtrack_skeleton = true;

	// OFF by default - true made AnyVisualFeature() always true -> FSN ran
	// World/ESP every frame and tripped 2nd-queue insecure on idle inject.
	bool hitmarker = false;
	bool hitmarker_screen = true;
	bool hitmarker_world = true;
	bool hitmarker_show_damage = false;
	float hitmarker_size = 14.f;
	float hitmarker_thickness = 2.2f;
	float hitmarker_world_size = 11.f;
	float hitmarker_duration = 1.f;
	ImVec4 hitmarker_color = ImVec4(1.f, 1.f, 1.f, 0.95f);
	ImVec4 hitmarker_head_color = ImVec4(1.f, 0.85f, 0.15f, 1.f);
	ImVec4 hitmarker_kill_color = ImVec4(1.f, 0.22f, 0.22f, 1.f);

	bool hitsound = false;
	char hitsound_file[160] = "";
	char hitsound_head[160] = "";
	char hitsound_kill[160] = "";

	bool watermark = false; // off by default - idle Present = pure original

	// Widgets that scan entities default OFF - bomb/spectators forced full
	// Get() walk even with "ESP only" -> multi-queue insecure.
	bool widget_keybinds = false;
	bool widget_bomb = false;
	bool widget_spectators = false;
	ImVec2 widget_keybinds_pos = ImVec2(-1.f, -1.f);
	ImVec2 widget_bomb_pos = ImVec2(-1.f, -1.f);
	ImVec2 widget_spectators_pos = ImVec2(-1.f, -1.f);
	bool widget_keybinds_only_when_active = false;
	bool widget_keybinds_show_all = true;
	ImVec4 menu_accent = ImVec4(0.45f, 0.72f, 0.98f, 1.00f);
	ImVec4 menu_bg = ImVec4(0.068f, 0.070f, 0.078f, 0.98f);
	ImVec4 menu_child_bg = ImVec4(0.102f, 0.105f, 0.118f, 1.00f);
	ImVec4 menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
	ImVec4 menu_border = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
	ImVec4 menu_text = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
	ImVec4 menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
	float menu_rounding = 4.0f;
	bool menu_sidebar_labels = false;
int menu_preset = 0;
bool fastladder = false;
	float menu_opacity = 0.98f;
	bool menu_compact = true;
	float menu_glass = 0.12f;
	bool menu_widgets_follow = true;
	int menu_dpi_scale = 100;
	float menu_w = 0.f;
	float menu_h = 0.f;
	float menu_x = -1.f;
	float menu_y = -1.f;

	ImVec4 widget_keybinds_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
	ImVec4 widget_bomb_urgent = ImVec4(0.82f, 0.38f, 0.38f, 0.95f);
	bool widget_bomb_show_damage = true;
	bool widget_bomb_show_defuse = true;
	ImVec4 widget_spectators_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
	bool widget_spectators_show_avatars = true;
	int widget_spectators_max = 8;
	bool widget_radar = false;
	ImVec2 widget_radar_pos = ImVec2(-1.f, -1.f);
	float widget_radar_size = 160.f;
	ImVec4 widget_radar_accent = ImVec4(0.52f, 0.72f, 0.80f, 0.95f);
	int widget_radar_shape = 0; // 0 circle, 1 square

	bool grenade_helper = false;
	bool grenade_helper_only_held = false; // show all kinds nearby; enable to filter by held nade
	float grenade_helper_stand_dist = 450.f;
	float grenade_helper_aim_dist = 20.f;
	float grenade_helper_select_dist = 550.f;
	// Soft stone stand, muted champagne aim (premium, no neon)
	ImVec4 grenade_helper_color = ImVec4(0.86f, 0.87f, 0.89f, 0.92f);
	ImVec4 grenade_helper_aim_color = ImVec4(0.83f, 0.66f, 0.38f, 1.0f);
	bool grenade_helper_capture = true; // keybind host (always on; press captures)
	int grenade_helper_capture_key = VK_F6;
	int grenade_helper_capture_key_mode = 1; // Hold
	int grenade_helper_capture_throw = 0;
	int grenade_helper_capture_kind = 0;
	char grenade_helper_capture_name[64] = "Lineup";

	bool nadepred_enable = false;
	bool nadepred_show_bounces = true;
	bool nadepred_in_air = true;
	bool nadepred_air_labels = true;
	ImVec4 nadepred_color = ImVec4(0.55f, 0.85f, 1.0f, 0.92f);

	void ResetToDefaults() {
		esp = false;
		glow = false;
		glow_team = true;
		glow_enemy = true;
		glow_only_visible = false;
		glow_color = ImVec4(0.25f, 0.85f, 1.f, 1.f);
		glow_color_invis = ImVec4(1.f, 0.35f, 0.85f, 1.f);
		glow_world_weapons = false;
		glow_world_weapon_color = ImVec4(0.95f, 0.90f, 0.55f, 1.0f);
		glow_world_grenades = false;
		showHealth = false;
		showArmor = false;
		showDistance = false;
		showWeapon = false;
		showWeaponIcon = false;
		teamCheck = true;
		espFill = false;
		showNameTags = false;
		esp_name_avatar = false;
		esp_skeleton = false;
		esp_skeleton_thickness = 1.5f;
		esp_skeleton_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		esp_skeleton_color_invisible = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
		esp_skeleton_head = true;
		esp_vis_check = true; // on by default (user request)
		flag_flashed = false;
		flag_bomb = false;
		flag_scoped = false;
		flag_reloading = false;
		flag_defusing = false;
		flag_money = false;
		flag_kit = false;
		flag_helmet = false;
		flag_nades = false;
		esp_rank = false;
		esp_3d_box = false;
		esp_oof = false;
		esp_oof_radius = 280.f;
		esp_oof_size = 14.f;
		esp_oof_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
		esp_3d_box_color = ImVec4(1.f, 0.45f, 0.2f, 0.95f);
		esp_rank_color = ImVec4(0.85f, 0.9f, 1.f, 1.f);
		float_damage = false;
		float_damage_duration = 1.1f;
		float_damage_speed = 55.f;
		float_damage_color = ImVec4(1.f, 1.f, 1.f, 1.f);
		float_damage_head_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
		float_damage_kill_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);
		world_esp_weapons = false;
		world_esp_weapon_icon = true;
		world_esp_weapon_distance = false;
		world_esp_bomb = false;
		world_esp_bomb_timer = true;
		world_esp_smoke = false;
		world_esp_molotov = false;
		world_esp_he = false;
		world_esp_flash = false;
		world_esp_decoy = false;
		world_esp_weapon_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		world_esp_weapon_distance_color = ImVec4(0.85f, 0.85f, 0.90f, 1.0f);
		world_esp_bomb_color = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
		world_esp_smoke_color = ImVec4(0.75f, 0.80f, 0.90f, 1.0f);
		world_esp_molotov_color = ImVec4(1.0f, 0.50f, 0.15f, 1.0f);
		world_esp_he_color = ImVec4(1.0f, 0.70f, 0.25f, 1.0f);
		world_esp_flash_color = ImVec4(0.95f, 0.95f, 0.55f, 1.0f);
		world_esp_decoy_color = ImVec4(0.65f, 0.85f, 0.55f, 1.0f);
		Night = false;
		night_exposure = 0.45f;
		skybox = false;
		skybox_color = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
		lighting = false;
		lighting_color = ImVec4(1.0f, 0.92f, 0.75f, 1.0f);
		map_color = false;
		custom_fog = false;
		custom_fog_color = ImVec4(0.58f, 0.62f, 0.85f, 1.0f);
		custom_fog_start = 100.f;
		custom_fog_end = 3000.f;
		custom_fog_falloff = 1.f;
		map_color_value = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);
		weather = false;
		weather_mode = 1;
		weather_intensity = 0.55f;
		enemyChams = false;
		enemyChamsInvisible = false;
		chamsMaterial = 3;
		chamsMaterialXQZ = 14;
		colVisualChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 150.f / 255.f);
		colVisualChamsIgnoreZ = ImVec4(1.f, 208.f / 255.f, 243.f / 255.f, 118.f / 255.f);
		teamChams = false;
		teamChamsInvisible = false;
		teamChamsMaterial = 3;
		teamChamsMaterialXQZ = 14;
		teamcolVisualChams = ImVec4(1.f, 1.f, 1.f, 1.f);
		teamcolVisualChamsIgnoreZ = ImVec4(1.f, 1.f, 1.f, 1.f);
		localChams = false;
		localChamsMaterial = 5;
		colLocalChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 175.f / 255.f);
		ragdollChams = false;
		ragdollChamsMaterial = 3;
		colRagdollChams = ImVec4(140.f / 255.f, 140.f / 255.f, 150.f / 255.f, 190.f / 255.f);
		armChams = false;
		armChamsMaterial = 5;
		colArmChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 1.f);
		viewmodelChams = false;
		viewmodelChamsMaterial = 6;
		colViewmodelChams = ImVec4(217.f / 255.f, 173.f / 255.f, 202.f / 255.f, 175.f / 255.f);
		itemChams = false;
		itemChamsInvisible = false;
		itemChamsMaterial = 3;
		itemChamsMaterialXQZ = 14;
		colItemChams = ImVec4(173.f / 255.f, 192.f / 255.f, 1.f, 1.f);
		colItemChamsIgnoreZ = ImVec4(1.f, 208.f / 255.f, 243.f / 255.f, 118.f / 255.f);
		itemChamsPistol = false;
		itemChamsSmg = false;
		itemChamsRifle = false;
		itemChamsShotgun = false;
		itemChamsSniper = false;
		itemChamsUtility = false;
		espThickness = 1.0f;
		espFillOpacity = 0.5f;
		espColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		espColorInvisible = ImVec4(0.45f, 0.55f, 1.0f, 1.0f);
		esp_box_style = ESP_BOX_FULL;
		esp_box_width = 0.42f;
		esp_pos_name = ESP_POS_TOP;
		esp_pos_weapon = ESP_POS_BOTTOM;
		esp_pos_weapon_icon = ESP_POS_BOTTOM;
		esp_pos_distance = ESP_POS_BOTTOM;
		esp_pos_health = ESP_POS_LEFT;
		esp_pos_armor = ESP_POS_LEFT;
		esp_bar_width = 3.0f;
		esp_health_auto = true;
		esp_health_color = ImVec4(0.20f, 0.90f, 0.25f, 1.0f);
		esp_armor_color = ImVec4(0.27f, 0.63f, 1.0f, 1.0f);
		esp_name_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		esp_weapon_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		esp_weapon_icon_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		esp_distance_color = ImVec4(0.71f, 0.86f, 1.0f, 1.0f);
		fovEnabled = false;
		fov = 90.0f;
		aspect_ratio_enabled = false;
		aspect_ratio = 1.777778f;
		viewmodel_changer = false;
		viewmodel_fov = 68.f;
		viewmodel_x = 0.f;
		viewmodel_y = 0.f;
		viewmodel_z = 0.f;
		thirdperson = false;
		thirdperson_distance = 150.f;
		thirdperson_key = 0x04;
		thirdperson_key_mode = 2;
		antiflash_amount = 0.f;
		remove_legs = false;
		remove_smoke = false;
		remove_decals = false;
		smoke_color = false;
		smoke_color_value = ImVec4(0.55f, 0.70f, 0.95f, 1.f);
		fire_color = false;
		fire_color_value = ImVec4(1.f, 0.35f, 0.05f, 1.f);
		inferno_color = false;
		inferno_color_value = ImVec4(1.f, 0.35f, 0.05f, 1.f);
		explosion_color = false;
		explosion_color_value = ImVec4(0.f, 1.f, 1.f, 1.f);
		remove_crosshair = false;
		force_crosshair = false;
		autowall_xhair = false;
		autowall_xhair_style = 0;
		autowall_xhair_size = 6.f;
		autowall_xhair_can = ImVec4(0.25f, 1.f, 0.35f, 1.f);
		autowall_xhair_cant = ImVec4(1.f, 0.28f, 0.28f, 1.f);
		remove_recoil = false;
		auto_pistol = false;
		auto_pistol_delay_ms = 0.f;
		enemy_spectate = false;
		enemy_spectate_thirdperson = false;
		auto_defuse = false;
		scope_custom_lines = false;
		scope_line_size = 1.f;
		scope_line_gap = 4.f;
		scope_line_thickness = 0.5f;
		scope_line_color = ImVec4(0.f, 0.f, 0.f, 1.f);
		scope_zoom_fov = false;
		scope_fov_1 = 40.f;
		scope_fov_2 = 15.f;
		scope_hide_viewmodel = false;
		aimbot = false;
		aimbot_fov = 5.f;
		aimbot_smooth = 5.f;
		aimbot_humanize = 0.f;
		aimbot_smooth_mode = SMOOTH_LINEAR;
		autofire = false;
		autofire_silent = false;
		autofire_fov = 5.f;
		autofire_hitchance = 70.f;
		autofire_mode = AF_MODE_HITCHANCE;
		autofire_autostop = false;
		autofire_autoscope = false;
		autofire_scoped_only = false;
		autofire_autowall = false;
		autowall = true;
		autowall_key = 0;
		autowall_key_mode = 0;
		mindamage_override = false;
		mindamage_override_key = 0;
		mindamage_override_key_mode = 1;
		mindamage_override_value = 10.f;
		autofire_mindamage = 1.f;
		autofire_mindamage_aw = 1.f;
		autofire_key = 0x06;
		autofire_key_mode = 1;
		autofire_target_select = AF_TARGET_CROSSHAIR;
		autofire_vis_check = true;
		autofire_flash_check = true;
		autofire_smoke_check = false;
		autofire_focus_target = true;
		autofire_multipoint_dynamic = true;
		autofire_body_if_lethal = false;
		autofire_prefer_body = false;
		for (int i = 0; i < HB_COUNT; ++i) {
			autofire_hitboxes[i] = false;
			autofire_multipoint[i] = false;
			autofire_multipoint_scale[i] = 0.50f;
		}
		autofire_hitboxes[HB_HEAD] = true;
		autofire_hitboxes[HB_NECK] = true;
		autofire_hitboxes[HB_CHEST] = true;
		autofire_multipoint[HB_HEAD] = true;
		autofire_multipoint[HB_CHEST] = true;
		autofire_multipoint_scale[HB_HEAD] = 0.55f;
		autofire_multipoint_scale[HB_NECK] = 0.50f;
		autofire_multipoint_scale[HB_CHEST] = 0.60f;
		autofire_multipoint_scale[HB_STOMACH] = 0.55f;
		autofire_multipoint_scale[HB_PELVIS] = 0.50f;
		autofire_multipoint_scale[HB_ARMS] = 0.45f;
		autofire_multipoint_scale[HB_LEGS] = 0.45f;
		autofire_multipoint_scale[HB_FEET] = 0.40f;
		triggerbot = false;
		triggerbot_key = 0x12;
		triggerbot_key_mode = 1;
		trigger_delay_ms = 0.f;
		trigger_hitchance = 0.f;
		trigger_autowall = false;
		trigger_mindamage = 1.f;
		trigger_mindamage_aw = 1.f;
		trigger_scoped_only = false;
		trigger_flash_check = true;
		trigger_smoke_check = false;
		for (int i = 0; i < HB_COUNT; ++i)
			trigger_hitboxes[i] = false;
		trigger_hitboxes[HB_HEAD] = true;
		trigger_hitboxes[HB_NECK] = true;
		trigger_hitboxes[HB_CHEST] = true;
		trigger_autostop = false;
		trigger_mode = TR_MODE_HITCHANCE;
		trigger_magnet = false;
		trigger_magnet_smooth = 12.f;
		trigger_magnet_fov = 4.f;
		trigger_magnet_silent = false;
		trigger_magnet_head_prio = true;
		trigger_magnet_deadzone = 0.12f;
		for (int i = 0; i < HB_COUNT; ++i)
			trigger_magnet_hitboxes[i] = false;
		team_check = true;
		aim_vis_check = true;
		aim_smoke_check = false;
		aim_flash_check = false;
		aim_scoped_only = false;
		aim_reaction_delay_ms = 0.f;
		aim_target_switch_delay_ms = 0.f;
		aim_first_shot_delay_ms = 0.f;
		for (int i = 0; i < HB_COUNT; ++i)
			aim_hitboxes[i] = false;
		aim_hitboxes[HB_HEAD] = true;
		aim_hitboxes[HB_NECK] = true;
		aim_hitboxes[HB_CHEST] = true;
		rcs = false;
		rcs_standalone = false;
		rcs_scale_x = 0.5f;
		rcs_scale_y = 0.5f;
		rcs_smooth = 0.f;
		fov_circle = false;
		fov_circle_autofire = false;
		fov_circle_magnet = false;
		fovCircleColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		fovCircleColorAf = ImVec4(1.0f, 0.45f, 0.15f, 0.90f);
			fovCircleColorMagnet = ImVec4(0.35f, 0.85f, 1.0f, 0.90f);
		anti_aim = false;
		anti_aim_mode = 0;
		anti_aim_pitch_mode = AA_PITCH_OFF;
		anti_aim_pitch_angle = 0.f;
		anti_aim_pitch_jitter_min = -45.f;
		anti_aim_pitch_jitter_max = 45.f;
		anti_aim_yaw_mode = AA_YAW_OFF;
		anti_aim_yaw_angle = 180.f;
		anti_aim_yaw_at_target = false;
		anti_aim_manual_key_left = 0;
		anti_aim_manual_key_right = 0;
		anti_aim_manual_key_back = 0;
		anti_aim_hideshots = false;
		anti_aim_avoid_backstab = true;
		anti_aim_yaw_adjust = true;
		weapon_group_active = WG_GENERAL;
		aimbot_key = 0x05;
		aimbot_key_mode = 1;
		bhop = false;
		autostrafe = false;
		autostrafe_mode = 1; // match definition default (1 = WASD yaw subtick)
		jumpbug = false;
		edgejump = false;
		edgejump_key = 0;
		edgejump_key_mode = 0;
		hitlog = false;
		hitlog_console = false;
		hitlog_duration = 4.f;
		hitlog_pos = ImVec2(-1.f, -1.f);
		hitlog_width = 268.f;
		hitlog_max_rows = 8;
		hitlog_show_hp = true;
		hitlog_show_stats = true;
		hitlog_color = ImVec4(0.92f, 0.92f, 0.95f, 0.95f);
		hitlog_head_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);
		hitlog_kill_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
		bullet_impact_effect = false;
		bullet_impact_effect_type = 0;
		bullet_impact_effect_fill_color = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 85.f / 255.f);
		bullet_impact_effect_edge_color = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 1.f);
		bullet_impact_effect_color_spark = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 1.f);
		bullet_impact_effect_duration = 2.5f;
		bullet_impact_effect_glow = true;
		bullet_impact_effect_glow_strength = 1.f;
		bullet_tracers = false;
		bullet_tracer_color = ImVec4(173.f / 255.f, 192.f / 255.f, 255.f / 255.f, 1.f);
		bullet_tracer_duration = 0.5f;
		auto_accept = false;
		scoreboard_weapons = false;
		unlock_inventory = true; // keep Andromeda parity - was false drifted from init true
		skin_knife = false;
		skin_glove = false;
		skin_agent = false;
		skin_knife_def = 0;
		skin_glove_def = 0;
		skin_agent_t = 0;
		skin_agent_ct = 0;
		skin_knife_paint = 0;
		skin_glove_paint = 0;
		skin_knife_wear = 0.f;
		skin_glove_wear = 0.f;
		skin_knife_seed = 0;
		skin_glove_seed = 0;
		skin_knife_stattrak = false;
		skin_glove_stattrak = false;
		skin_knife_tag[0] = 0;
		skin_glove_tag[0] = 0;
		SkinWeapon_Clear();
		custom_paint_enabled = false;
		custom_paint_color0 = ImVec4(1.f, 1.f, 1.f, 1.f);
		custom_paint_color1 = ImVec4(1.f, 1.f, 1.f, 1.f);
		custom_paint_color2 = ImVec4(1.f, 1.f, 1.f, 1.f);
		custom_paint_color3 = ImVec4(1.f, 1.f, 1.f, 1.f);
		custom_paint_glove_enabled = false;
		custom_paint_glove_color = ImVec4(1.f, 1.f, 1.f, 1.f);
		sound_esp = false;
		sound_esp_duration = 1.4f;
		sound_esp_ring_size = 1.f;
		sound_esp_color = ImVec4(0.35f, 0.85f, 1.f, 0.95f);
		vote_reveal = false;
		vote_auto = false;
		vote_auto_choice = 0;
		vote_auto_delay_ms = 250.f;
		backtrack = false;
		backtrack_ms = 100.f;
		backtrack_skeleton = true;
		hitmarker = false;
		hitmarker_screen = true;
		hitmarker_world = true;
		hitmarker_show_damage = false;
		hitmarker_size = 14.f;
		hitmarker_thickness = 2.2f;
		hitmarker_world_size = 11.f;
		hitmarker_duration = 1.f;
		hitmarker_color = ImVec4(1.f, 1.f, 1.f, 0.95f);
		hitmarker_head_color = ImVec4(1.f, 0.85f, 0.15f, 1.f);
		hitmarker_kill_color = ImVec4(1.f, 0.22f, 0.22f, 1.f);
		hitsound = false;
		hitsound_file[0] = 0;
		hitsound_head[0] = 0;
		hitsound_kill[0] = 0;
		watermark = false;
		widget_keybinds = false;
		widget_bomb = false;
		widget_spectators = false;
		widget_keybinds_pos = ImVec2(-1.f, -1.f);
		widget_bomb_pos = ImVec2(-1.f, -1.f);
		widget_spectators_pos = ImVec2(-1.f, -1.f);
		widget_keybinds_only_when_active = false;
		widget_keybinds_show_all = true;
		menu_accent = ImVec4(0.45f, 0.72f, 0.98f, 1.00f);
		menu_bg = ImVec4(0.068f, 0.070f, 0.078f, 0.98f);
		menu_child_bg = ImVec4(0.102f, 0.105f, 0.118f, 1.00f);
		menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
		menu_border = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
		menu_text = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
		menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
		menu_rounding = 4.0f;
		menu_opacity = 0.98f;
		menu_preset = 0;
		menu_sidebar_labels = false;
		menu_compact = true;
		menu_glass = 0.12f;
		menu_widgets_follow = true;
		menu_dpi_scale = 100;

		widget_keybinds_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
		widget_bomb_urgent = ImVec4(0.82f, 0.38f, 0.38f, 0.95f);
		widget_bomb_show_damage = true;
		widget_bomb_show_defuse = true;
		widget_spectators_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
		widget_spectators_show_avatars = true;
		widget_spectators_max = 8;
		widget_radar = false;
		widget_radar_pos = ImVec2(-1.f, -1.f);
		widget_radar_size = 160.f;
		widget_radar_accent = ImVec4(0.52f, 0.72f, 0.80f, 0.95f);
		widget_radar_shape = 0;
		grenade_helper = false;
		grenade_helper_only_held = false;
		grenade_helper_stand_dist = 450.f;
		grenade_helper_aim_dist = 20.f;
		grenade_helper_select_dist = 550.f;
		grenade_helper_color = ImVec4(0.86f, 0.87f, 0.89f, 0.92f);
		grenade_helper_aim_color = ImVec4(0.83f, 0.66f, 0.38f, 1.0f);
		grenade_helper_capture = true;
		grenade_helper_capture_key = VK_F6;
		grenade_helper_capture_key_mode = 1;
		grenade_helper_capture_throw = 0;
		grenade_helper_capture_kind = 0;
		std::snprintf(grenade_helper_capture_name, sizeof(grenade_helper_capture_name), "Lineup");

		nadepred_enable = false;
		nadepred_show_bounces = true;
		nadepred_in_air = true;
		nadepred_air_labels = true;
		nadepred_color = ImVec4(0.55f, 0.85f, 1.0f, 0.92f);
		weapon_group_ui = WG_RIFLE;
		InitWeaponProfilesDefaults();

		// Keep live keybind objects in sync with Config::* defaults
		keybind.resetToDefaults();
		aimbot_key = keybind.getKey(aimbot);
		aimbot_key_mode = keybind.getMode(aimbot);
		autofire_key = keybind.getKey(autofire);
		autofire_key_mode = keybind.getMode(autofire);
		autowall_key = keybind.getKey(autowall);
		autowall_key_mode = keybind.getMode(autowall);
		triggerbot_key = keybind.getKey(triggerbot);
		triggerbot_key_mode = keybind.getMode(triggerbot);
		thirdperson_key = keybind.getKey(thirdperson);
		thirdperson_key_mode = keybind.getMode(thirdperson);
		grenade_helper_capture_key = keybind.getKey(grenade_helper_capture);
		grenade_helper_capture_key_mode = keybind.getMode(grenade_helper_capture);
		// No SkinChanger::RefreshAll() here: ResetToDefaults runs mid-Load with
		// skin maps empty/partial - the forced walk would fight the JSON apply
		// loop on the menu thread. ConfigManager::Load calls RefreshAll once
		// after all values are in.
	}
}

namespace {
	struct WeaponProfileBoot {
		WeaponProfileBoot() { Config::InitWeaponProfilesDefaults(); }
	};
	static WeaponProfileBoot g_weaponProfileBoot;
}

