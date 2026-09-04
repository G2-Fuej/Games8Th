#pragma once
#include "../../../external/imgui/imgui.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <cstring>
class C_CSWeaponBase;
// CBA to make proper atm, it's 03:42 right now.// For now just stores config values don't mind it too much//// (FYI THIS IS A HORRID SOLUTION BUT FUNCTIONS) 
namespace Config {
extern bool esp;
	extern bool showHealth;
	extern bool showArmor;
	extern bool showDistance;
	extern bool showWeapon;
	extern bool showWeaponIcon;
 // CS2 icon font under player / world	
extern bool teamCheck;
	extern bool espFill;
	extern float espThickness;
	extern float espFillOpacity;
	extern ImVec4 espColor;
	extern ImVec4 espColorInvisible;
 // box when occluded (vis check on)

extern bool showNameTags;
	extern bool esp_name_avatar;
 // Steam avatar beside name ESP	
extern bool esp_skeleton;
	extern float esp_skeleton_thickness;
	extern ImVec4 esp_skeleton_color;
	extern ImVec4 esp_skeleton_color_invisible;
	extern bool esp_skeleton_head;
	extern bool esp_vis_check;
	// Glow ESP ( DrawGlow path)

extern bool glow;
	extern bool glow_team;
	extern bool glow_enemy;
	extern bool glow_only_visible;
	extern ImVec4 glow_color;
 // visible	
extern ImVec4 glow_color_invis;
 // behind wall	
extern bool glow_world_weapons;
	extern ImVec4 glow_world_weapon_color;
	extern bool glow_world_grenades;
	// ESP style (right-click settings)

enum EspBoxStyle : int { ESP_BOX_FULL = 0, ESP_BOX_CORNER = 1 }
;
	extern int esp_box_style;
 // EspBoxStyle
extern float esp_box_width;
 // width = height * this (0.28-0.70)

// Per-element ESP slot (name / weapon / icon / distance / bars)
enum EspElemPos : int { ESP_POS_TOP = 0, ESP_POS_BOTTOM = 1, ESP_POS_LEFT = 2, ESP_POS_RIGHT = 3 };
extern int esp_pos_name;
extern int esp_pos_weapon;
extern int esp_pos_weapon_icon;
extern int esp_pos_distance;
extern int esp_pos_health;
extern int esp_pos_armor;

extern float esp_bar_width;
 // health/armor bar px	
extern bool esp_health_auto;
 // true = green->red by HP	
extern ImVec4 esp_health_color;
	extern ImVec4 esp_armor_color;
	extern ImVec4 esp_name_color;
	extern ImVec4 esp_weapon_color;
	extern ImVec4 esp_weapon_icon_color;
	extern ImVec4 esp_distance_color;
	// Flags ESP	
extern bool flag_flashed;
	extern bool flag_bomb;
	extern bool flag_scoped;
	extern bool flag_reloading;
	extern bool flag_defusing;
	extern bool flag_money;
 // $account	
extern bool flag_kit;
 // defuse kit	
extern bool flag_helmet;
 // helmet	
extern bool flag_nades;
 // H/F/S/M/D held	
extern bool esp_rank;
 // competitive rank under name	
extern bool esp_3d_box;
 // oriented collision AABB wireframe	
extern bool esp_oof;
 // offscreen arrows	
extern float esp_oof_radius;
 // px from screen center	
extern float esp_oof_size;
 // arrow size	
extern ImVec4 esp_oof_color;
	extern ImVec4 esp_3d_box_color;
	extern ImVec4 esp_rank_color;
	// Floating damage numbers (world, independent of hitmarker X)

extern bool float_damage;
	extern float float_damage_duration;
	extern float float_damage_speed;
 // px/sec up	
extern ImVec4 float_damage_color;
	extern ImVec4 float_damage_head_color;
	extern ImVec4 float_damage_kill_color;
	// World ESP (dropped items / bomb / projectiles)

extern bool world_esp_weapons;
	extern bool world_esp_weapon_icon;     // dropped weapon icon (vs text name)
	extern bool world_esp_weapon_distance; // distance under dropped weapon only
	extern bool world_esp_bomb;
	extern bool world_esp_bomb_timer;
	extern bool world_esp_smoke;
	extern bool world_esp_molotov;
	extern bool world_esp_he;
	extern bool world_esp_flash;
	extern bool world_esp_decoy;
extern ImVec4 world_esp_weapon_color;
	extern ImVec4 world_esp_weapon_distance_color;
extern ImVec4 world_esp_bomb_color;
	extern ImVec4 world_esp_smoke_color;
	extern ImVec4 world_esp_molotov_color;
	extern ImVec4 world_esp_he_color;
	extern ImVec4 world_esp_flash_color;
extern ImVec4 world_esp_decoy_color;
	// Chams - mercey port (enemy: visible + XQZ, own material + colour each)
	extern bool enemyChams;               // visible layer
	extern bool enemyChamsInvisible;      // XQZ layer
	extern int chamsMaterial;             // visible material (chams::ChamIds)
	extern int chamsMaterialXQZ;          // XQZ material
	extern ImVec4 colVisualChams;
	extern ImVec4 colVisualChamsIgnoreZ;
	extern bool teamChams;
	extern bool teamChamsInvisible;
	extern int teamChamsMaterial;
	extern int teamChamsMaterialXQZ;
	extern ImVec4 teamcolVisualChams;
	extern ImVec4 teamcolVisualChamsIgnoreZ;
	// Local body (thirdperson) + corpse
	extern bool localChams;
	extern int localChamsMaterial;
	extern ImVec4 colLocalChams;
	extern bool ragdollChams;
	extern int ragdollChamsMaterial;
	extern ImVec4 colRagdollChams;
	// Viewmodel: hands + weapon
	extern bool armChams;
	extern int armChamsMaterial;
	extern ImVec4 colArmChams;
	extern bool viewmodelChams;
	extern int viewmodelChamsMaterial;
	extern ImVec4 colViewmodelChams;
	// World item chams (weapons/nades - mercey item groups)
	extern bool itemChams;
	extern bool itemChamsInvisible;
	extern int itemChamsMaterial;
	extern int itemChamsMaterialXQZ;
	extern ImVec4 colItemChams;
	extern ImVec4 colItemChamsIgnoreZ;
	extern bool itemChamsPistol;
	extern bool itemChamsSmg;
	extern bool itemChamsRifle;
	extern bool itemChamsShotgun;
	extern bool itemChamsSniper;
	extern bool itemChamsUtility;
	extern bool fovEnabled;
	extern float fov;
	// Aspect ratio (GetScreenAspectRatio / engine2 hook)

extern bool aspect_ratio_enabled;
	extern float aspect_ratio;
 // width/height; e.g. 1.777... = 16:9	// Viewmodel (CalcViewModel / GetViewModelOffsets hook)

extern bool viewmodel_changer;
	extern float viewmodel_fov;
	extern float viewmodel_x;
	extern float viewmodel_y;
	extern float viewmodel_z;
	// Third person (OverrideView hook)

extern bool thirdperson;
	extern float thirdperson_distance;
	extern int thirdperson_key;
	extern int thirdperson_key_mode;
  // 0 Always, 1 Hold, 2 Toggle


extern float antiflash_amount;
 // 0 = full flash, 100 = fully removed	// Visuals -> Removals (early-
	extern bool remove_legs;
 // DRAWLEGS - firstperson legs	
extern bool remove_smoke;
 // DrawSmokeVertex + DrawSmokeArray	
extern bool remove_decals;
 // RENDERDECALS	
extern bool smoke_color;
 // t
	// tint C_SmokeGrenadeProjectile m_vSmokeColor	
extern ImVec4 smoke_color_value;
	extern bool fire_color;
 // t
	// tint fire particles (non-inferno) via ParticleDrawArray (particles.dll)

extern ImVec4 fire_color_value;
	extern bool inferno_color;
 // t
	// separate molotov/inferno tint - Nade Pred tab (independent of Fire Color)

extern ImVec4 inferno_color_value;
	extern bool explosion_color;
 // t
	// tint HE / explosion particles via ParticleDrawArray	
extern ImVec4 explosion_color_value;
	extern bool remove_crosshair;
 // DrawCrosshair - hide	
extern bool force_crosshair;
 // DrawCrosshair - show on snipers unscoped; hide when scoped	
// Autowall crosshair - center overlay over game reticle.	// green = can penetrate wall/prop (or clear LOS), red = cannot.	
extern bool autowall_xhair;
	extern int autowall_xhair_style;
 // 0 = dot, 1 = box	
extern float autowall_xhair_size;
	extern ImVec4 autowall_xhair_can;
 // penetrable / clear	
extern ImVec4 autowall_xhair_cant;
 // blocked	// Visual recoil only - strip aim-punch from CViewSetup angles (OverrideView).	// Does NOT zero GetRemovedAimPunch used by fire/seed/RCS (IDA 0x18088BBB0).	
extern bool remove_recoil;
	// Scope - custom lines (DrawScopeOverlay kill + ImGui redraw)

extern bool scope_custom_lines;
	extern float scope_line_size;
 // 0..1 arm length (1 = full screen)

extern float scope_line_gap;
 // center hole px	
extern float scope_line_thickness;
 // line width px (0.1 hairline .. 6)

extern ImVec4 scope_line_color;
	// Scope zoom FOV (GetRenderFov + OverrideView) - per zoom stage	
extern bool scope_zoom_fov;
	extern float scope_fov_1;
 // first zoom (m_zoomLevel == 1)

extern float scope_fov_2;
 // second zoom (m_zoomLevel >= 2)
// Hide weapon + viewmodel arms while scoped	
extern bool scope_hide_viewmodel;
	extern bool Night;
	extern float night_exposure;
 // darkness 0..1 (0 = none, 1 = darkest)
// Skybox t
	// tint via C_EnvSky::m_vTintColor (+ lighting-only) (+ lighting-only)

extern bool skybox;
	extern ImVec4 skybox_color;
	// Global light (CGlobalLightBase::m_LightColor) via GLOBALLIGHTUPDATESTATE	
extern bool lighting;
	extern ImVec4 lighting_color;
	// World/map mesh t
	// tint via DrawArray (non-player meshes) (non-player meshes)

extern bool map_color;
	extern ImVec4 map_color_value;
	// Custom gradient fog (SetupFog shader params)

extern bool custom_fog;
	extern ImVec4 custom_fog_color;
	extern float custom_fog_start;
	extern float custom_fog_end;
	extern float custom_fog_falloff;
	// Weather via engine particles + rain_fx
	extern bool weather;
	extern int weather_mode;
	extern float weather_intensity; // 0..1 density
	extern bool aimbot;
	extern float aimbot_fov;
	extern float aimbot_smooth;
 // 0 = snap, 50 = slowest	
extern float aimbot_humanize;
 // 0 = off (default), 100 = subtle speed variance + tiny bias	// Visible-aim smooth curve

enum AimSmoothMode : int {
SMOOTH_CONSTANT = 0, // fixed deg/sec every frame
SMOOTH_LINEAR = 1,   // strong far -> soft near (settle)
SMOOTH_SINE = 2,     // sine distance + mild wave (organic)
SMOOTH_MODE_COUNT	
}
;
	extern int aimbot_smooth_mode;
	// Auto-pistol (Misc) - re-edge IN_ATTACK on semi pistols while M1 held	
extern bool auto_pistol;
	extern float auto_pistol_delay_ms;
 // extra wait after fire ready (0 = ASAP)
// Enemy spectate while dead (comp/MM - client force observer target)

extern bool enemy_spectate;
	extern bool enemy_spectate_thirdperson;
 // chase vs in-eye	// Bomb helpers	
extern bool auto_defuse;
 // StartDefuse when near planted C4	// Autofire - own keybind + FOV (separate from Aimbot FOV)

extern bool autofire;
	extern bool autofire_silent;
 // GLOBAL - not per weapon group	
extern float autofire_fov;
 // lock FOV degrees (own circle when AF key active)

extern float autofire_hitchance;
 // 0 = off, 1..100 = Monte Carlo % (HC mode)

extern bool autofire_autostop;
 // counter-strafe before shot for accuracy	
extern bool autofire_autoscope;
 // scope weapons: zoom when hittable target in AF FOV	
extern bool autofire_scoped_only;
 // snipers: only fire while scoped (off by default)

extern bool autofire_autowall; // penetrate walls when checking damage
// Global AW keybind host (Always/Hold/Toggle). AF/TR pen also need their checkbox.
extern bool autowall;
extern int autowall_key;
extern int autowall_key_mode; // 0 Always, 1 Hold, 2 Toggle

// Min damage override keybind & value
extern bool mindamage_override;
extern int mindamage_override_key;
extern int mindamage_override_key_mode; // 0 Always, 1 Hold, 2 Toggle
extern float mindamage_override_value;

// Autofire shot-value gate (Aimbot ignores). Trigger has its own pair.
// If target HP < mindmg, lethal still passes. 0 = any hit.
extern float autofire_mindamage; // visible / non-pen
extern float autofire_mindamage_aw; // through walls (pen)

extern int autofire_key;
	extern int autofire_key_mode;
 // 0 Always, 1 Hold, 2 Toggle	// Autofire accuracy mode - separate from trigger; Hitchance vs Seed Nospread	
enum AutofireMode : int {
AF_MODE_HITCHANCE = 0,
AF_MODE_SEED_NOSPREAD = 1,
AF_MODE_COUNT	}
;
	extern int autofire_mode;
 // AutofireMode	// Autofire target priority (-style SortTargets)

enum AfTargetSelect : int {
AF_TARGET_CROSSHAIR = 0, // lowest FOV to aim po
AF_TARGET_DISTANCE = 1,  // closest player origin
AF_TARGET_DAMAGE = 2,    // highest estimated damage (+ lethal prefer)
AF_TARGET_COUNT	
}
;
	extern int autofire_target_select;
 // who-priority: crosshair / distance / damage	// Target-selection filters (who is valid)

extern bool autofire_vis_check;
 // LOS required (when AW off)

extern bool autofire_flash_check;
 // skip while local is flashed	
extern bool autofire_smoke_check;
 // skip aims through active smoke volumes	
extern bool autofire_focus_target;
 // don't switch targets while already shooting	
extern bool autofire_multipoint_dynamic;
 // shrink MP scale by bloomxdistance	// Body aim policy (hitbox pick among enabled AF hitboxes)

extern bool autofire_body_if_lethal;
 // oneshot body (chest/stomach/pelvis) > head	
extern bool autofire_prefer_body;
 // soft body bias; head still allowed	// Multi-select hitboxes (shared index for aim / autofire / trigger lists)

enum AimHitbox : int {
HB_HEAD = 0,
HB_NECK,
HB_CHEST,
HB_STOMACH,
HB_PELVIS,
HB_ARMS,
HB_LEGS,
HB_FEET,
HB_COUNT	}
;
	// Autofire - two separate lists (same HB indices):
// hitboxes = which bones AF may lock on	// multipoint = which of those also aim edge/extra points (
	extern bool autofire_hitboxes[HB_COUNT];
	extern bool autofire_multipoint[HB_COUNT];
	extern float autofire_multipoint_scale[HB_COUNT];
 // 0 = center, 1 = full edge (MP on only)

extern bool team_check;
	// Auto team-check from game mode (DM/FFA -> off). Applies to aim + ESP.
	extern bool aim_vis_check;
	extern bool aim_smoke_check;
 // skip aims through smoke	
extern bool aim_flash_check;
 // pause while flashed	
extern bool aim_scoped_only;
 // snipers: only aim while scoped	// Humanization delays (ms, 0 = off / instant)

extern float aim_reaction_delay_ms;
 // detection -> start aiming	
extern float aim_target_switch_delay_ms;
	// old target -> new target
extern float aim_first_shot_delay_ms;
 // lock -> allow first attack	
extern bool aim_hitboxes[HB_COUNT];
 // aimbot lock hitboxes

extern bool rcs;
 // aimbot RCS (compensate when locking)

extern bool rcs_standalone;
 // independent RCS while shooting	
extern float rcs_scale_x;
	extern float rcs_scale_y;
	extern float rcs_smooth;
 // 0 = instant, higher = lagged/spongy RCS (humanize)

extern bool fov_circle;
 // aimbot FOV ring	
extern bool fov_circle_autofire;
 // autofire FOV ring (independent)

extern bool fov_circle_magnet;
 // trigger magnet FOV ring	
extern ImVec4 fovCircleColor;
	extern ImVec4 fovCircleColorAf;
	extern ImVec4 fovCircleColorMagnet;

	// Anti-Aim settings migrated from the old Games8Th UI.
	extern bool anti_aim;
	enum AAPitchMode : int {
		AA_PITCH_OFF = 0, AA_PITCH_UP, AA_PITCH_DOWN, AA_PITCH_CUSTOM,
		AA_PITCH_JITTER, AA_PITCH_RANDOM_JITTER, AA_PITCH_SWITCH_JITTER,
		AA_PITCH_THIRD_WAY_JITTER, AA_PITCH_COUNT
	};
	extern int anti_aim_pitch_mode;
	extern float anti_aim_pitch_angle;
	extern float anti_aim_pitch_jitter_min;
	extern float anti_aim_pitch_jitter_max;
	enum AAYawMode : int { AA_YAW_OFF = 0, AA_YAW_STATIC, AA_YAW_COUNT };
	extern int anti_aim_yaw_mode;
	extern float anti_aim_yaw_angle;
	extern bool anti_aim_yaw_at_target;
	extern int anti_aim_manual_key_left;
	extern int anti_aim_manual_key_right;
	extern int anti_aim_manual_key_back;
	extern bool anti_aim_hideshots;
	extern bool anti_aim_avoid_backstab;   // velocity-style: yaw away from nearby knife enemies
	extern bool anti_aim_yaw_adjust;      // velocity-style: +33deg compensate model roll
	extern int anti_aim_mode;
	// Trigger accuracy mode (per weapon group)

enum TriggerMode : int {
TR_MODE_HITCHANCE = 0,     // Monte Carlo % + movement gates
TR_MODE_SEED_NOSPREAD = 1, // exact SPREADSEEDGEN ray wait (no angle rewrite)
TR_MODE_COUNT	
}
;
	// ---- Per weapon-group aim/autofire profiles ----	// CS2 CCSWeaponType: knife=0 pistol=1 smg=2 rifle=3 shotgun=4 sniper=5 lmg=6	
enum WeaponGroup : int {
WG_GENERAL = 0, // knife / unknown / fallback
WG_PISTOL,
WG_SMG,
WG_RIFLE,
WG_SHOTGUN,
WG_SNIPER,
WG_LMG,
WG_COUNT	
}
;
	struct AimWeaponProfile {
float aimbot_fov = 5.f;
		float aimbot_smooth = 5.f;
		float aimbot_humanize = 0.f;
 // 0..100 - subtle only (no shake)

int aimbot_smooth_mode = SMOOTH_LINEAR;
		bool aim_vis_check = true;
		bool aim_smoke_check = false;
		bool aim_flash_check = false;
		bool aim_scoped_only = false;
		bool aim_hitboxes[HB_COUNT]{}
;
		float aim_reaction_delay_ms = 0.f;
		float aim_target_switch_delay_ms = 0.f;
		float aim_first_shot_delay_ms = 0.f;
		bool rcs = false;
		bool rcs_standalone = false;
		float rcs_scale_x = 0.5f;
		float rcs_scale_y = 0.5f;
		float rcs_smooth = 0.f;
		float autofire_fov = 5.f;
		float autofire_hitchance = 70.f;
		int autofire_mode = AF_MODE_HITCHANCE;
		bool autofire_autostop = false;
		bool autofire_autoscope = false;
		bool autofire_scoped_only = false;
		bool autofire_autowall = false;
		float autofire_mindamage = 1.f;
		float autofire_mindamage_aw = 1.f;
		bool mindamage_override = false;
		float mindamage_override_value = 10.f;
		int autofire_target_select = AF_TARGET_CROSSHAIR;
		bool autofire_vis_check = true;
		bool autofire_flash_check = true;
		bool autofire_smoke_check = false;
		bool autofire_focus_target = true;
		bool autofire_multipoint_dynamic = true;
		bool autofire_body_if_lethal = false;
		bool autofire_prefer_body = false;
		bool autofire_hitboxes[HB_COUNT]{}
;
 // lock-on hitboxes		
bool autofire_multipoint[HB_COUNT]{}
;
	// edge multipoint enable (subset)

float autofire_multipoint_scale[HB_COUNT]{}
;
		// Triggerbot (per group) - fires only when crosshair already on enemy		
float trigger_delay_ms = 0.f;
 // 0 = instant; flicks ignore delay anyway		
float trigger_hitchance = 0.f;
 // 0 = off (HC mode only)

bool trigger_autowall = false;
		float trigger_mindamage = 1.f;
 // visible - independent of autofire		
float trigger_mindamage_aw = 1.f;
 // wallbang - independent of autofire		
bool trigger_scoped_only = false;
 // useful for AWPs		
bool trigger_flash_check = true;
 // skip while local is flashed		
bool trigger_smoke_check = false;
 // skip fire through smoke		
bool trigger_hitboxes[HB_COUNT]{}
;
 // lock/fire hitboxes (no multipo
	// list)

bool trigger_autostop = false;
		int trigger_mode = TR_MODE_HITCHANCE;
 // Hitchance | Seed Nospread (separate modules)
// Magnet (per group - same as other trigger knobs)

bool trigger_magnet = false;
		float trigger_magnet_smooth = 12.f;
		float trigger_magnet_fov = 4.f;
		bool trigger_magnet_silent = false;
 // stamp cmd only (no camera)

bool trigger_magnet_head_prio = true;
 // head > body when FOV close		
float trigger_magnet_deadzone = 0.12f;
 // deg - stop micro-pull		
bool trigger_magnet_hitboxes[HB_COUNT]{}
;
 // empty = use trigger_hitboxes	
}
;
	extern AimWeaponProfile weapon_profiles[WG_COUNT];
	extern int weapon_group_ui;
 // menu editor selection	
extern int weapon_group_active;
	// runtime group from held weapon
	AimWeaponProfile& MenuAimProfile();

void InitWeaponProfilesDefaults();
	// profile[group] -> live Config::* (does not change weapon_group_active)
	void ApplyProfileToLive(int group);
	void ApplyWeaponGroup(::
C_CSWeaponBase* weapon);
 // classify + ApplyProfileToLive	
void PullLiveIntoProfile(int group);
	// live Config::* -> profile (migrate / copy)
const char* WeaponGroupName(int group);
	int ClassifyWeaponGroup(::
C_CSWeaponBase* weapon);
	// Live mirrors (filled by ApplyWeaponGroup)

extern float trigger_delay_ms;
	extern float trigger_hitchance;
	extern bool trigger_autowall;
	extern float trigger_mindamage;
 // visible - independent of autofire_mindamage	
extern float trigger_mindamage_aw;
 // wallbang - independent of autofire_mindamage_aw	
extern bool trigger_scoped_only;
	extern bool trigger_flash_check;
	extern bool trigger_smoke_check;
	extern bool trigger_hitboxes[HB_COUNT];
	extern bool trigger_autostop;
	extern int trigger_mode;
 // TriggerMode	// Magnet: soft-aim into trigger FOV while key held (not only when already on-target)

extern bool trigger_magnet;
	extern float trigger_magnet_smooth;
 // 0 = snap, 50 = slow (no aimbot humanize)

extern float trigger_magnet_fov;
 // deg FOV to pick bone (default 4)

extern bool trigger_magnet_silent;
	extern bool trigger_magnet_head_prio;
	extern float trigger_magnet_deadzone;
	extern bool trigger_magnet_hitboxes[HB_COUNT];
	// Triggerbot master + keybind (global)

extern bool triggerbot;
	extern int triggerbot_key;
	extern int triggerbot_key_mode;
 // 0 Always, 1 Hold, 2 Toggle	// keybind persist (aimbot)

extern int aimbot_key;
	extern int aimbot_key_mode;
 // 0 Always, 1 Hold, 2 Toggle	
extern bool bhop;
	// bhop stamina zero - writes CCSPlayer_MovementServices->m_flStamina = 0 client-side.	// Server predicts stamina independently -> local write creates position/height desync	// visible on peek. Default OFF; enable only if you accept the desync trade-off for	// consistent jump height.
	extern bool autostrafe;
	extern int autostrafe_mode;
 // 0 mouse (legit), 1 vectorial (silent)
	// mercey jumpbug - hold space in air, duck/jump subticks at land frac
	extern bool jumpbug;
	extern int jumpbug_key;
	extern int jumpbug_key_mode;
// Edgejump (Misc movement) - keybinds via keybind system	
extern bool edgejump;
	extern int edgejump_key;
	extern int edgejump_key_mode;
	// Fastladder - 89? ladder boost
	extern bool fastladder;
	// Hit log panel (Misc)

extern bool hitlog;
	extern bool hitlog_console;
 // also echo hits to game developer console	
extern float hitlog_duration;
	extern ImVec2 hitlog_pos;
 // <0 = 
	extern float hitlog_width;
 // panel width px	
extern int hitlog_max_rows;
 // visible rows (4..16)

extern bool hitlog_show_hp;
 // remaining victim HP after hit	
extern bool hitlog_show_stats;
 // session H/HS/K/DMG footer	
extern ImVec4 hitlog_color;
extern ImVec4 hitlog_head_color;
extern ImVec4 hitlog_kill_color;
	// Bullet feedback copied from the mercey impacts feature.
extern bool bullet_impact_effect;
extern int bullet_impact_effect_type; // 0 overlay, 1 sparks, 2 both
extern ImVec4 bullet_impact_effect_fill_color;
extern ImVec4 bullet_impact_effect_edge_color;
extern ImVec4 bullet_impact_effect_color_spark;
extern float bullet_impact_effect_duration;
extern bool bullet_impact_effect_glow;
extern float bullet_impact_effect_glow_strength;
extern bool bullet_tracers;
extern ImVec4 bullet_tracer_color;
extern float bullet_tracer_duration;
	// subtick_move + pred_upgrade: always on (hardcoded, no config)

extern bool auto_accept;
	// TAB scoreboard weapon icons via panorama
	extern bool scoreboard_weapons;
	// Misc - mid-match loadout (P2000/USP, M4A4/M4A1-S): IsLoadoutAllowed + ignore server slot override
	extern bool unlock_inventory;

	struct WeaponSkin {
		int paint = 0;
		float wear = 0.f;
		int seed = 0;
		bool stattrak = false;
		char tag[64]{};
	};
	extern bool skin_knife;
	extern bool skin_glove;
	extern bool skin_agent;
	extern int skin_knife_def;
	extern int skin_glove_def;
	extern int skin_agent_t;
	extern int skin_agent_ct;
	extern int skin_knife_paint;
	extern int skin_glove_paint;
	extern float skin_knife_wear;
	extern float skin_glove_wear;
	extern int skin_knife_seed;
	extern int skin_glove_seed;
	extern bool skin_knife_stattrak;
	extern bool skin_glove_stattrak;
	extern char skin_knife_tag[64];
	extern char skin_glove_tag[64];
	extern std::map<int, WeaponSkin> skin_weapons;

	// Custom paintkit colors (UC 754206 silvhook: g_vColor0..3 + g_vColorTint) - IDA 0x1807C7363 / 0x1807C7636
	extern bool custom_paint_enabled;
	extern ImVec4 custom_paint_color0;
	extern ImVec4 custom_paint_color1;
	extern ImVec4 custom_paint_color2;
	extern ImVec4 custom_paint_color3;
	extern bool custom_paint_glove_enabled;
	extern ImVec4 custom_paint_glove_color;

	// Thread-safe accessors for skin_weapons. The menu/Present thread mutates
	// the map (apply/disable/config Load/Save) while the game thread reads it
	// every FSN frame (SkinChanger Resolve / IsSkinnedWeaponId / HUD gate /
	// hkUnlockInventory). Direct std::map access from both sides is heap
	// corruption - ALL access must go through these.
	bool SkinWeapon_Find(int def, WeaponSkin& out);
	bool SkinWeapon_Has(int def);
	bool SkinWeapon_Empty();
	void SkinWeapon_Set(int def, const WeaponSkin& ws);
	void SkinWeapon_Erase(int def);
	void SkinWeapon_Clear();
	// Whole-map copy under lock (config save iteration).
	std::map<int, WeaponSkin> SkinWeapon_Snapshot();
 // Misc ? accept matchmaking popup via Lobby ReadyUp	// Sound ESP (Visuals) ? ground walk rings on ESP-visible enemies	
extern bool sound_esp;
	extern float sound_esp_duration;
 // marker life sec	
extern float sound_esp_ring_size;
 // scale 0.5..3	
extern ImVec4 sound_esp_color;
	// Vote reveal / 
	extern bool vote_reveal;
 // toast + chat who voted yes/no	
extern bool vote_auto;
 //
	// auto cast after delay	
extern int  vote_auto_choice;
 // 0 Yes, 1 No	
extern float vote_auto_delay_ms;
	// Backtrack - self-recorded position history (Misc)
	extern bool backtrack;
	extern float backtrack_ms;
	extern bool backtrack_skeleton;
	// Hitmarker (Misc) - COD screen X + world 3D at hit bone	
extern bool hitmarker;
	extern bool hitmarker_screen;
	extern bool hitmarker_world;
	extern bool hitmarker_show_damage;
	extern float hitmarker_size;
  // screen arm length px	
extern float hitmarker_thickness;
	extern float hitmarker_world_size;
 // world arm length px	
extern float hitmarker_duration;
 // life scale 0.25..2.5	
extern ImVec4 hitmarker_color;
	extern ImVec4 hitmarker_head_color;
	extern ImVec4 hitmarker_kill_color;
	// Hitsound (Misc) - custom .wav from Documents/Games8Th/Hitsounds	
extern bool hitsound;
	extern char hitsound_file[160];
 // normal hit basename	
extern char hitsound_head[160];
 // empty = use hitsound_file	
extern char hitsound_kill[160];
 // empty = use hitsound_file	// HUD watermark (Misc)

extern bool watermark;
	// Menu design (Config -> Design)

extern ImVec4 menu_accent;
	extern ImVec4 menu_bg;
	extern ImVec4 menu_child_bg;
	extern ImVec4 menu_sidebar_bg;
	extern ImVec4 menu_border;
	extern ImVec4 menu_text;
	extern ImVec4 menu_text_muted;
	extern float menu_rounding;
 // 2..8	
extern float menu_opacity;
extern bool menu_compact;
extern float menu_glass;
extern bool menu_widgets_follow;
 // tighter spacing	
extern int menu_dpi_scale;
extern float menu_w;
extern float menu_h;
extern float menu_x;
extern float menu_y;

extern int menu_preset;
 // last applied preset index 0..7	
extern bool menu_sidebar_labels;
 // show tab names under icons	
// Overlay widgets (Misc)

extern bool widget_keybinds;
	extern bool widget_bomb;
	extern bool widget_spectators;

	extern ImVec2 widget_keybinds_pos;
 // <0 = 
	extern ImVec2 widget_bomb_pos;
	extern ImVec2 widget_spectators_pos;
	extern bool widget_keybinds_only_when_active;
 // hide until a bind is active	
extern bool widget_keybinds_show_all;
 // false = active binds only	
extern ImVec4 widget_keybinds_accent;
	extern ImVec4 widget_bomb_urgent;
	extern bool widget_bomb_show_damage;
	extern bool widget_bomb_show_defuse;
	extern ImVec4 widget_spectators_accent;
	extern bool widget_spectators_show_avatars;
	extern int widget_spectators_max;
 // 1..16	
extern bool widget_radar;
	extern ImVec2 widget_radar_pos;
	extern float widget_radar_size;
	extern ImVec4 widget_radar_accent;
	// 0 = circle, 1 = square	
extern int widget_radar_shape;
	// Grenade lineup helper ( 9xth style - stand circle + aim marker)

extern bool grenade_helper;
	extern bool grenade_helper_only_held;
 // only show when holding matching nade	
extern float grenade_helper_stand_dist;
 // fade/draw stand circle	
extern float grenade_helper_aim_dist;
 // show aim marker when this close to stand	
extern float grenade_helper_select_dist;
 // nearest-lineup pick radius	
extern ImVec4 grenade_helper_color;
	extern ImVec4 grenade_helper_aim_color;
	// Capture keybind host (like aimbot/trigger) - edge-press captures	
extern bool grenade_helper_capture;
	extern int grenade_helper_capture_key;
	extern int grenade_helper_capture_key_mode;
 // 0 Always, 1 Hold, 2 Toggle	
extern int grenade_helper_capture_throw;
 // 0 Stand 1 Stand+Jump 2 Walk 3 Run 4 Crouch 5 Run+Jump	
extern int grenade_helper_capture_kind;
 // 0 Any ... 5 Decoy	
extern char grenade_helper_capture_name[64];
	// Live grenade trajectory preview (held weapon)
extern bool nadepred_enable;
extern bool nadepred_show_bounces;
extern bool nadepred_in_air;
extern bool nadepred_air_labels;
extern ImVec4 nadepred_color;
	// Restore every field to config.cpp defaults (used before Load)

void ResetToDefaults();
	// True while ConfigManager::Load mutates Config::* - World/Glow must skip
	// so ResetToDefaults (Night briefly off) cannot race FSN / Present.
extern std::
atomic<bool> loading;
	// Save configs as XPRESS_HUFF-compressed .json (LFAC magic header). Load
	// auto-detects both formats. Default ON;
	// toggle OFF if you want to hand-edit	// the JSON on disk.
extern bool config_compress;
	// UI font size - fixed built-in chain (segoeui.ttf -> arial.ttf). 
extern float ui_font_size;
 // 12..24 px, default 16
}


