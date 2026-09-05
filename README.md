<img width="2559" height="1439" alt="Screenshot 2026-07-23 211211" src="https://github.com/user-attachments/assets/0df9504c-5666-4039-8c92-c45237a6222f" />
# Games8Th — CS2 Internal Cheat

> **Original author:** [Lefri501](https://github.com/Lefri501) (Lefrizzel Ai) — original project: [Lefri501/CS2-Internel-cheat-](https://github.com/Lefri501/CS2-Internel-cheat-)
>
> **Fork / continuation:** [G2-Fuej](https://github.com/G2-Fuej) — this repository (`Games8Th`) is a heavily modified fork of the original project.
>
> **License:** MIT — original copyright (c) 2025 Lefrizzel Ai, fork copyright (c) 2026 Games8Th. See [LICENSE](LICENSE).

---

## What we changed (vs. the original)

| Area | Change |
|------|--------|
| **Branding** | Renamed the project from *Lefrizzel Ai* to **Games8Th** |
| **Menu UI** | Redesigned the ImGui menu — fixed 520×500 layout, modern card-based sidebar design, removed the top-left `/INTERNAL READY` watermark |
| **Language** | Added a full **Simplified Chinese** translation layer (`config/language.h`, 440+ keys) — all labels, tooltips, combo items and hitsound placeholders are now translatable at runtime |
| **Menu UI** | Off-screen window position is auto-corrected every frame — menu can no longer get stuck partially off-screen with an empty content column |
| **Menu UI** | Fixed UI flicker/jitter — frame-rate independent menu open animation, scrollbar-width reserve (no more limit-cycle width/hight oscillation), pixel-aligned pair-fill gap patching in two-column card layouts |
| **Anti-Aim** | Added a new Anti-Aim page| **Anti-Aim** | Added a new Anti-Aim page (pitch / yaw modes, jitter, manual direction keys) based on the previous design, plus **Avoid Backstab** (points yaw away from nearby knife-wielding enemies) and **Yaw Adjust** (+33° model-roll compensation) borrowed from the Velocity source |
| **Configuration** | New anti-aim options are saved/loaded through the JSON config system |

Internal DLL cheat for Counter-Strike 2. C++23, D3D11 overlay (ImGui), SafetyHook detours, custom schema-based SDK.

> **Disclaimer:** For educational/research purposes only. Use in CS2 violates Valve's ToS and can get you VAC banned. Use at your own risk.

> **Stability (v1.4):** Crash fixes and general bug fixes. More stable on map change, respawn and disconnect.

> **VAC:** Fixed VAC / "insecure client" error after 1-2 games. Launch the game normally from Steam without extra launch options (no `-insecure`, no `-allow_third_party_software`).

---

## Features

| Category | Details |
|----------|---------|
| **Aimbot** | Per-weapon profiles, FOV / smoothing / RCS, multipoint hitboxes, visibility check, hitchance, target-switch delays, reaction & first-shot humanization, sticky hitbox |
| **Triggerbot** | Per-weapon config, seed-based hitchance, silent aim, autowall |
| **Autofire** | Auto-fires on valid target with hitchance or nospread seed |
| **ESP** | Box, health, name, skeleton, flags — world-to-screen via copied view matrix |
| **Chams** | Flat, illuminated, glow, ghost, latex; XQZ, arm & viewmodel support |
| **Glow** | Per-player + world item glow via DrawGlow property manipulation |
| **Visuals** | FOV changer, viewmodel offset, thirdperson, antiflash, scope / smoke / decal / particle removal |
| **World** | Night mode, skybox swap, light & map tint, weather (rain / snow / ash) |
| **Skinchanger** | Knife, gloves, weapon skins (70 indexed slots), agent models, killfeed spoofing, custom knife model |
| **Movement** | BHop, autostrafe (mouse + silent), jumpbug |
| **NadePred** | Grenade throw preview with in-air path and landing radius |
| **Config** | JSON-based config with per-weapon profile system |
| **Menu** | ImGui DX11, tabbed layout |

---

## Build

Requirements:
- Visual Studio 2022 (v145 toolset)
- Windows SDK 10.0.26100.0
- DirectX SDK (June 2010)

Steps:
1. Open `Games8Th.sln`
2. Select **Release | x64**
3. Build

Output: `Games8Th.dll`

Dependencies are vendored in `external/` (imgui, nlohmann/json, safetyhook).

---

## Inject

Use any manual-map injector. The DLL detects manual-map injection and sets up SEH, static init and the security cookie itself.

Known to work with:
- Process Hacker Native Injector
- Extreme Injector (manual map mode)

Unload with F4.

---

## Project Structure

```
Games8Th.sln                     # Solution file
Games8Th/
├── external/                     # Vendored dependencies
├── source/
│   ├── main.cpp                  # DllMain, Present hook
│   ├── cs2/                      # CS2 SDK data types, entities
│   └── Games8Th/
│       ├── features/             # Cheat features
│       ├── hooks/                # Detour hooks
│       ├── interfaces/           # CS2 interface wrappers
│       ├── menu/                 # ImGui menu + HUD
│       ├── config/               # Config system
│       ├── renderer/             # Renderer init
│       └── utils/                # Math, memory, security, schema
cs2 dump/                         # SDK offsets dump (patterns, offsets, schemas)
```

---

## Update Log

### v1.5.0 (current)
- **Robust CJK font chain:** the menu now tries 6 common Chinese-capable
  Windows fonts (msyh.ttc, msyhl.ttc, simhei.ttf, simsun.ttc, Deng.ttf,
  simkai.ttf) before falling back to arial/ImGui default. Previously a
  lost msyh.ttc made every Chinese label render as blank space while icons
  and ASCII stayed visible, looking like "sidebar flickers, content blank".

### v1.4.4
- **Animation stall net now wall-clock based:** the 2.5s (150-frame) fallback could
  take 5s at 30Hz; it is now a deterministic 1-second timeout that snaps the menu
  to full opacity whenever the open animation ever stalls below 0.35 alpha.
  Eliminates any remaining "sidebar visible but content blank" state on any refresh rate.

### v1.4.3
- **Menu open animation fix:** the fade-in is now guaranteed to complete. When `DeltaTime` was tiny (very high FPS / glitchy time source) the exponential ease `k = 1-exp(-dt/tau)` collapsed toward 0, so `g_anim.open` stalled at ~0.05: the ImGui content (cards, text, scrollbars) rendered almost fully transparent while the chrome (sidebar, header, window frame drawn via `AddRectFilled`) stayed opaque — looking like "sidebar flickers against the game, right-side UI is blank". A DeltaTime floor (1/240s) plus a 2-second stall safety net keep the animation bounded.
- Window on-screen clamp from v1.4.2 is retained.

### v1.4.2

- **Off-screen menu fix:** the window position is now clamped back into the display every frame. A stale `imgui.ini` / `menu_size.json` position (e.g. dragged off to the bottom-right corner) used to win over `SetNextWindowPos(Cond_Once)`, leaving the content column outside the visible screen — the sidebar edge flickered against the game while the right-side UI appeared empty. The corrected position is persisted.
- **Window start position:** clamped on both bounds so a negative or oversized saved position can never place the menu off-screen on first open.

### v1.4.1

- **UI flicker fix:** menu open/close animation is now frame-rate independent (exponential ease-out instead of linear ramp) — no speed jumps on stutter frames
- **UI flicker fix:** content area reserves the scrollbar width up front, so the two AutoResizeY card columns no longer change width when the scrollbar appears/disappears (this used to re-wrap text, bounce card heights and toggle the scrollbar every frame)
- **UI flicker fix:** the pair-fill gap patch between side-by-side cards is pixel-aligned and ignores sub-2px jitter, so the bottom edge no longer bounces frame to frame

### v1.4

- **Crash fixes:** fixed crashes on map change, respawn and leaving a match
- **Bug fixes:** general fixes across visuals, movement and weapon logic

### v1.3
- **Trigger/Autofire scope fix:** scope / smoke / flash checks are now independent per mode — aimbot's checks no longer block triggerbot (trigger has its own smoke/flash/scope as shown in menu, autofire has its own). Fixed shared `aim_scoped_only` bug in `menu.cpp:602`/`756`, `config.cpp:551`, `triggerbot.cpp:1433`, `autofire.cpp:996`
- **Knife spawn fix:** fixed knife bugged model/animation for ~3s after spawn/respawn — skinchanger now reapplies for 90 frames on `m_flLastSpawnTimeIndex` change and fixes viewmodel even on spawn (`skinchanger.cpp:753`)

### v1.2
- **Crash fixes:** fixed random crashes on map change, respawn, leaving a match and after 2-3 rounds (safer hook teardown, entity validation, SEH hardening)
- **VAC fix:** fixed VAC / "insecure client" error after 1-2 games (stable launch-state bypass — launch normally from Steam, no `-insecure` / `-allow_third_party_software` needed)
- **Feature fixes:** aimbot / triggerbot / autofire, hitchance, nospread, visuals, chams / glow, world / fog / weather and movement are now more reliable
- **New skinchanger:** complete rewrite — faster, cleaner and more stable; knife / gloves / weapon skins (70+ slots), agents and preview improved
- **Performance:** removed bloat, optimized hooks / rendering / pattern scan — lower CPU/GPU overhead and less stutter

### v1.1
- Fixed crash when leaving a match (hooks now restore safely on their own thread)
- Fixed "insecure client" / VAC error after 1-2 games (bypasses the launch-state check; launch the game without extra launch options)
- Fixed aimbot/triggerbot not working while in spawn protection (TDM)
- Autofire now respects reaction, target-switch and first-shot delays in all modes
- Autofire smoothing now works with all smooth modes and humanization — no more silent-aim feel with silent aim off
- Menu UI fixes: garbled text, Config tab clipping, added missing Hitsound enable toggle

### v1.0
- Initial release build.
- Menu UI fixes: garbled text in Aim/Visuals tabs, Config tab card clipping (font manager + design section cut off).
- Added SDK offsets dump, project analysis doc.

---

## License

MIT — see [LICENSE](LICENSE).
