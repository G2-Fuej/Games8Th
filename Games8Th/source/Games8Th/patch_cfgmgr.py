# -*- coding: utf-8 -*-
import io

src = r"D:\GPT\G8\CS2-Internel-cheat--main\Games8Th\source\Games8Th\config\configmanager.h"
with io.open(src, "r", encoding="utf-8", newline="") as f:
    text = f.read()

# --- SAVE section: add after anti_aim_hideshots save ---
save_old = '            j["anti_aim_hideshots"] = Config::anti_aim_hideshots;'
save_new = '            j["anti_aim_hideshots"] = Config::anti_aim_hideshots;\n            j["anti_aim_avoid_backstab"] = Config::anti_aim_avoid_backstab;\n            j["anti_aim_yaw_adjust"] = Config::anti_aim_yaw_adjust;'
assert save_old in text, "SAVE block not found"
text = text.replace(save_old, save_new, 1)

# --- LOAD section: add after anti_aim_hideshots load ---
load_old = '            Config::anti_aim_hideshots = j.value("anti_aim_hideshots", false);'
load_new = '            Config::anti_aim_hideshots = j.value("anti_aim_hideshots", false);\n            Config::anti_aim_avoid_backstab = j.value("anti_aim_avoid_backstab", true);\n            Config::anti_aim_yaw_adjust = j.value("anti_aim_yaw_adjust", true);'
assert load_old in text, "LOAD block not found"
text = text.replace(load_old, load_new, 1)

with io.open(src, "w", encoding="utf-8", newline="") as f:
    f.write(text)
print("OK: configmanager.h save+load updated")
