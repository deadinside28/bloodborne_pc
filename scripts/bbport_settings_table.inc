// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: every bbport.ini setting and its default, in one place. The game includes this file
// (gpu/shim/bbport_settings: the defaults are applied before bbport.ini is read, and the game
// effects list comes from here); the Linux launcher (launcher/bbport_launcher.py) and the Windows
// setup (tools/setup/BbportSetup.cs) read its lines. Keep one entry per line, with plain string
// literals.
//
// BB_SETTING(key, default): the default as bbport.ini writes it.
// BB_EFFECT(key, menu label, default 0/1): game effects, switched by patches at start
// (scripts/patches.py EFFECTS); the label is English (the menus translate it).

BB_SETTING("upscaler", "fsr3")
BB_SETTING("preset", "0")
BB_SETTING("sharpen", "1")
BB_SETTING("sharpness", "0.30")
BB_SETTING("jitter", "1")
BB_SETTING("reactive", "0")
BB_SETTING("object_motion", "1")
BB_SETTING("reactive_scale", "1.00")
BB_SETTING("reactive_threshold", "0.20")
BB_SETTING("reactive_max", "0.90")
BB_SETTING("debug_view", "0")
BB_SETTING("show_fps", "0")
BB_SETTING("menu_language", "en")
BB_SETTING("fsr4_auto_exposure", "1")
BB_SETTING("fsr4_invert_jitter", "0")
BB_SETTING("model_lod", "0")
BB_SETTING("output_res", "1920x1080")
BB_SETTING("fullscreen", "0")
BB_SETTING("live_resolution", "0")

BB_EFFECT("effect_chromatic_aberration", "Chromatic aberration", 1)
BB_EFFECT("effect_dof", "Depth of field (DoF)", 1)
BB_EFFECT("effect_motion_blur", "Motion blur", 1)
BB_EFFECT("effect_ssao", "SSAO ambient occlusion", 1)
BB_EFFECT("effect_game_aa", "The game's own anti-aliasing", 1)
BB_EFFECT("effect_dynamic_shadows", "Shadows from dynamic lights", 1)
BB_EFFECT("effect_ssr", "SSR reflections (not in the original game)", 0)
BB_EFFECT("skip_intro", "Skip intro videos at startup", 0)
BB_EFFECT("debug_camera", "Free camera (Cross + L3)", 0)
BB_EFFECT("debug_menu", "Debug menu (needs font files)", 0)
