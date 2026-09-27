#!/usr/bin/env python3
"""Audit Games8Th against the bundled cs2-dumper output and installed CS2.

Exit codes:
  0 - all required dump values and critical byte patterns match
  1 - one or more required checks failed
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFSETS_H = ROOT / "Games8Th/source/Games8Th/offsets/offsets.h"
DUMP_DIR = ROOT / "dumper/output"
GAME_BIN = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
)
CLIENT = GAME_BIN / r"csgo\bin\win64\client.dll"
ENGINE = GAME_BIN / r"bin\win64\engine2.dll"

PATTERNS = {
    "CreateMove": (CLIENT, "85 D2 0F 85 ?? ?? ?? ?? 48 8B C4 44 88 40", True),
    "FrameStageNotify": (CLIENT, "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 83 EC ?? 48 8B F9 33 ED", True),
    "SetViewAngle": (CLIENT, "85 D2 75 3D 48 63 81", False),
    "GetCmdBySeq": (CLIENT, "40 53 48 83 EC 20 8B DA E8 ?? ?? ?? ?? 4C 8B C0", True),
    "GetCmdArray": (CLIENT, "48 89 4C 24 08 41 56 41 57 48 83 EC 48 4C 63 FA", True),
    "GetEntityCmdSlot": (CLIENT, "48 83 EC 08 4C 8B 0D ?? ?? ?? ?? 4C 8B DA 48 8B", True),
    "ForceButtonsDown": (CLIENT, "40 53 57 41 56 48 81 EC 30 02 00 00 48 83 79 38", False),
    "pCSGOInput_1": (CLIENT, "48 8B 0D ?? ?? ?? ?? 4C 8B C6 8B 10 E8", False),
    "pCSGOInput_2": (CLIENT, "4C 8B 05 ?? ?? ?? ?? 41 8B 80 50 0B 00 00 85 C0", False),
    "UserCmdTable": (CLIENT, "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CF 4C 8B F8", True),
    "GetLocalPlayer": (CLIENT, "48 83 EC 28 83 F9 FF 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 30 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 4C 8D 05", False),
    "GetBaseEntity": (CLIENT, "4C 8D 49 ?? 81 FA", False),
    "NetworkGameClient": (ENGINE, "48 89 3D ?? ?? ?? ?? FF 87", False),
    "EngineIsInGame": (ENGINE, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 15 80 B8 1F 14", False),
    "EngineIsConnected": (ENGINE, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0B 83 B8 30 02 00 00 02 0F", False),
    "EngineBuildNumber": (ENGINE, "89 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 0D", False),
}

FB_FIELDS = {
    "m_iHealth": ("C_BaseEntity", "m_iHealth"),
    "m_iTeamNum": ("C_BaseEntity", "m_iTeamNum"),
    "m_pGameSceneNode": ("C_BaseEntity", "m_pGameSceneNode"),
    "m_vecViewOffset": ("C_BaseModelEntity", "m_vecViewOffset"),
    "m_clrRender": ("C_BaseModelEntity", "m_clrRender"),
    "m_nRenderMode": ("C_BaseModelEntity", "m_nRenderMode"),
    "m_vOldOrigin": ("C_BasePlayerPawn", "m_vOldOrigin"),
    "m_pMovementServices": ("C_BasePlayerPawn", "m_pMovementServices"),
    "m_pWeaponServices": ("C_BasePlayerPawn", "m_pWeaponServices"),
    "m_vecAbsOrigin": ("CGameSceneNode", "m_vecAbsOrigin"),
    "m_modelState": ("CSkeletonInstance", "m_modelState"),
    "m_ModelName": ("CModelState", "m_ModelName"),
    "m_pAimPunchServices": ("C_CSPlayerPawn", "m_pAimPunchServices"),
    "m_iShotsFired": ("C_CSPlayerPawn", "m_iShotsFired"),
    "m_predictableBaseAngle": ("CCSPlayer_AimPunchServices", "m_predictableBaseAngle"),
    "m_predictableBaseAngleVel": ("CCSPlayer_AimPunchServices", "m_predictableBaseAngleVel"),
    "m_unpredictableBaseAngle": ("CCSPlayer_AimPunchServices", "m_unpredictableBaseAngle"),
    "m_pEntity": ("C_BaseEntity", "m_pEntity"),
    "m_hOwnerEntity": ("C_BaseEntity", "m_hOwnerEntity"),
    "m_designerName": ("CEntityIdentity", "m_designerName"),
    "m_pParent": ("CGameSceneNode", "m_pParent"),
    "m_flDetonateTime": ("C_BaseGrenade", "m_flDetonateTime"),
    "m_bombsiteCenterA": ("C_CSPlayerResource", "m_bombsiteCenterA"),
    "m_bombsiteCenterB": ("C_CSPlayerResource", "m_bombsiteCenterB"),
}


def parse_pattern(spec: str) -> tuple[bytes, bytes]:
    tokens = spec.split()
    needle = bytes(0 if t in {"?", "??"} else int(t, 16) for t in tokens)
    mask = bytes(0 if t in {"?", "??"} else 1 for t in tokens)
    return needle, mask


def scan(data: bytes, spec: str, limit: int = 16) -> list[int]:
    needle, mask = parse_pattern(spec)
    fixed = next((i for i, m in enumerate(mask) if m), None)
    if fixed is None:
        return []
    anchor = bytes([needle[fixed]])
    out: list[int] = []
    pos = 0
    while len(out) < limit:
        found = data.find(anchor, pos + fixed)
        if found < 0:
            break
        start = found - fixed
        end = start + len(needle)
        if start >= 0 and end <= len(data) and all(
            not mask[i] or data[start + i] == needle[i] for i in range(len(needle))
        ):
            out.append(start)
        pos = found + 1 - fixed
    return out


def namespace_constants(text: str, namespace: str) -> dict[str, int]:
    marker = f"namespace {namespace}"
    start = text.find(marker)
    if start < 0:
        return {}
    brace = text.find("{", start)
    depth = 0
    end = -1
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    block = text[brace + 1 : end]
    return {
        name: int(value, 0)
        for name, value in re.findall(
            r"constexpr\s+(?:std::)?(?:u?int32_t|ptrdiff_t)\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)",
            block,
        )
    }


def class_fields(raw: dict, class_name: str) -> dict[str, int]:
    cls = raw.get("client.dll", {}).get("classes", {}).get(class_name, {})
    fields = cls.get("fields", {})
    out: dict[str, int] = {}
    for name, item in fields.items():
        if isinstance(item, dict) and "offset" in item:
            out[name] = int(item["offset"])
        elif isinstance(item, int):
            out[name] = item
    return out


def main() -> int:
    failed = False
    source = OFFSETS_H.read_text(encoding="utf-8")
    offsets = json.loads((DUMP_DIR / "offsets.json").read_text(encoding="utf-8"))
    schemas = json.loads((DUMP_DIR / "client_dll.json").read_text(encoding="utf-8"))

    print("[offsets] Global namespace vs offsets.json")
    global_values = namespace_constants(source, "Global")
    expected_globals = dict(offsets["client.dll"])
    expected_globals.update(
        {f"engine_{k}": v for k, v in offsets["engine2.dll"].items() if k in {"dwBuildNumber", "dwNetworkGameClient", "dwWindowHeight", "dwWindowWidth"}}
    )
    for name, actual in sorted(global_values.items()):
        expected = expected_globals.get(name)
        ok = expected == actual
        print(f"  {'OK' if ok else 'FAIL':4} {name:36} source=0x{actual:X} dump={('-' if expected is None else f'0x{expected:X}')}")
        failed |= not ok

    print("[offsets] FB namespace vs client_dll.json")
    fb_values = namespace_constants(source, "FB")
    for const, (cls, field) in FB_FIELDS.items():
        expected = class_fields(schemas, cls).get(field)
        actual = fb_values.get(const)
        # m_pEntity is a C++ base-layout identity pointer and is intentionally
        # absent from cs2-dumper's schema output. Keep its validated +0x10.
        if const == "m_pEntity" and expected is None:
            expected = 0x10
        ok = expected is not None and actual == expected
        print(f"  {'OK' if ok else 'FAIL':4} {const:30} source={('-' if actual is None else f'0x{actual:X}')} dump={('-' if expected is None else f'0x{expected:X}')} ({cls}->{field})")
        failed |= not ok

    print("[patterns] installed CS2 binaries")
    cache: dict[Path, bytes] = {}
    pattern_hits: dict[str, list[int]] = {}
    for name, (path, spec, required) in PATTERNS.items():
        if not path.is_file():
            print(f"  FAIL {name:20} missing binary: {path}")
            failed |= required
            continue
        data = cache.setdefault(path, path.read_bytes())
        hits = scan(data, spec)
        pattern_hits[name] = hits
        ok = bool(hits)
        level = "OK" if ok else ("FAIL" if required else "WARN")
        print(f"  {level:4} {name:20} count={len(hits):2} rva={[hex(x) for x in hits]}")
        failed |= required and not ok

    # These pairs have source fallbacks; at least one pattern in each pair must hit.
    for group, names in {
        "pCSGOInput": ("pCSGOInput_1", "pCSGOInput_2"),
    }.items():
        ok = any(pattern_hits.get(n) for n in names)
        print(f"  {'OK' if ok else 'FAIL':4} {group:20} fallback group")
        failed |= not ok

    print("AUDIT_RESULT=" + ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
