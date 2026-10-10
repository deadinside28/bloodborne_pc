# SPDX-License-Identifier: GPL-2.0-or-later
"""Configuration and settings manager for the Bloodborne launcher.

Handles bbport.ini and settings.json persistence, default options,
path resolution, and game launch environment generation.
"""

import json
import os
import sys
from pathlib import Path

from bbport_i18n import language

PORT_DIR = Path(__file__).resolve().parent.parent  # native_probe (or the package's copy)
sys.path.insert(0, str(PORT_DIR / "scripts"))
sys.path.insert(0, str(PORT_DIR / "tools"))

# Packaged (AppImage): generated files, saves and bbport.ini live in BB_DATA_DIR.
PACKAGED = bool(os.environ.get("BB_PREBUILT"))
DATA_DIR = Path(os.environ.get("BB_DATA_DIR", PORT_DIR))
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "bbport-launcher"
CONFIG_FILE = CONFIG_DIR / "settings.json"
MAX_LOG_LINES = 5000

# The memory model (run.sh: BB_PC_MODEL): "auto" leaves it to the GPU (the new one on AMD, the
# 0.3 one elsewhere), or either by hand. The new one goes through the layer's memory module on
# every GPU (no sparse binding of the game's memory; BB_LAYER_MEMORY=0: AMD's sparse arena).
PC_MODEL_SUBTITLE = (
    "Новая: видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды "
    "графики переводятся, а не эмулируются. Старая — модель памяти 0.3 со всеми "
    "исправлениями. Если драйвер не проходит проверку при запуске — старая"
)
MEMORY_MODELS = [("Авто: новая на AMD, старая на других", "auto"), ("Новая", "new"),
                 ("Старая (как в 0.3)", "old")]

# The package's own environment must not reach Proton's container
PACKAGE_ONLY_ENV = (
    "LD_LIBRARY_PATH", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
    "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "__EGL_VENDOR_LIBRARY_DIRS", "GDK_PIXBUF_MODULE_FILE",
    "GIO_EXTRA_MODULES", "GI_TYPELIB_PATH", "FONTCONFIG_FILE", "GSETTINGS_SCHEMA_DIR", "PYTHONPATH",
    "PYTHONHOME",
)

# Choices: (label, value). The first entry is the default. Labels are translated when shown.
UI_LANGUAGES = [("Как в системе", ""), ("Português (Brasil)", "pt_BR"), ("English", "en"), ("Русский", "ru"), ("简体中文", "zh_CN")]
UPSCALERS = [("FSR 4", "fsr4"), ("FSR 4.1.1", "fsr411"), ("FSR 3", "fsr3"),
             ("TAA (нативное сглаживание)", "taa"), ("DLSS (NVIDIA RTX)", "dlss"), ("Выключен", "off")]
PRESETS = [("Native AA", 0), ("Quality (x1.5)", 1), ("Balanced (x1.7)", 2),
           ("Performance (x2)", 3), ("Ultra Performance (x3)", 4)]
OUTPUT_RES = [("1280×720 (Steam Deck)", "1280x720"), ("1920×1080", "1920x1080"), ("2560×1440", "2560x1440"), ("3840×2160", "3840x2160")]

# Game effects (patches applied at start): bbport.ini key, title, default.
EFFECTS = [
    ("effect_chromatic_aberration", "Хроматическая аберрация", True),
    ("effect_dof", "Глубина резкости (DoF)", True),
    ("effect_motion_blur", "Размытие в движении", True),
    ("effect_ssao", "Затенение SSAO", True),
    ("effect_game_aa", "Собственное сглаживание игры", True),
    ("effect_dynamic_shadows", "Тени от динамических источников", True),
    ("effect_ssr", "Отражения SSR (не было в игре)", False),
    ("puddle_reflections", "Отражения в лужах", False),
    ("skip_intro", "Пропуск заставок при запуске", True),
    ("debug_camera", "Свободная камера (Cross + L3 / Space + Z)", False),
    ("debug_menu", "Debug menu (левый touchpad / Tab; нужны шрифты)", False),
]
MODEL_LOD = [("Как в игре", "0"), ("Максимальная (-2)", "-2"), ("Ниже (1)", "1"),
             ("Минимальная (2)", "2")]
FPS_MODES = [("Без ограничения (патч)", "uncap"), ("60", "60"), ("90", "90"),
             ("30 (как на PS4)", "30")]
PRESENT_MODES = [("Mailbox", "Mailbox"), ("FIFO (VSync)", "Fifo"),
                 ("FIFO Relaxed", "FifoRelaxed"), ("Immediate", "Immediate")]

DRAW_PIPE = [("Гибридный (Рекомендуется)", "2", "Ускорение в игре, стабильность в кат-сценах"),
             ("Включён", "1"),
             ("Выключен", "0", "Стабильнее, стандартный режим")]
LANGUAGES = [("Английский", "1"), ("Русский", "8"), ("Японский", "0"), ("Французский", "2"),
             ("Испанский", "3"), ("Немецкий", "4"), ("Итальянский", "5")]
LIVE_RESOLUTION = [("Авто (по видеокарте)", "auto"), ("Выключена (быстрее)", "0"), ("Включена", "1")]
READBACKS = [("Relaxed", "", "По умолчанию"), ("Выключены", "0", "Быстрее, но лица могут искажаться"),
             ("Precise", "2", "Экспериментально; может зависать при запуске")]
PREUPLOAD = [("Обычная", "", "Без лишней видеопамяти"), ("Полная", "2", "Около 3 ГБ видеопамяти сверху"),
             ("Выключена", "0")]

VK_RECORD_THREADS = [("Авто", "auto"), ("1", "1"), ("2", "2"), ("3", "3"), ("4", "4"), ("5", "5")]

DEFAULTS = {
    "ui_language": "",
    "game_dir": "" if PACKAGED else str(PORT_DIR.parent / "CUSA03173"),
    "user_dir": "",
    "mods_dir": "",
    "mods_enabled": True,
    "patches_dir": "",
    "language": "1",
    "fullscreen": False,
    "hdr": False,
    "present_mode": "Mailbox",
    "gamepad": "",
    "gamepad_name": "",
    "display": "",
    "skip_network_choice": True,
    "fps_mode": "uncap",
    "fps_limit": 0,
    "draw_pipe": "2",
    "vk_record_threads": "auto",
    "readbacks": "",
    "preupload": "",
    "mangohud": False,
    "frame_stats": False,
    "save_log": False,
    "crash_diag": False,
    "memory_model": "auto",
    "as_0_3": False,
    "gpu_profile": False,
    "vk_validation": False,
    "extra_env": "",
}

# bbport.ini keys the launcher edits; the rest of the file is kept.
INI_DEFAULTS = {
    "upscaler": "fsr4",
    "preset": "4",
    "sharpen": "1",
    "sharpness": "0.50",
    "object_motion": "1",
    "frame_generation": "0",
    "show_fps": "1",
    "output_res": "1920x1080",
    "model_lod": "0",
    "live_resolution": "0",
    "draw_pipe": "2",
    **{key: "1" if default else "0" for key, _, default in EFFECTS},
}

# Controls (runtime_pad.c): input, label, default keyboard keys, default gamepad buttons, default mouse buttons.
CONTROLS = [
    ("cross", "Крест", "Space", "a", None),
    ("circle", "Круг", "Left Shift", "b", None),
    ("square", "Квадрат", "E", "x", None),
    ("triangle", "Треугольник", "Q", "y", None),
    ("l1", "L1", "1", "leftshoulder", None),
    ("r1", "R1", "3", "rightshoulder", "left"),
    ("l2", "L2", "R", "lefttrigger", "right"),
    ("r2", "R2", "F", "righttrigger", None),
    ("l3", "L3", "Z", "leftstick", None),
    ("r3", "R3", "C", "rightstick", "middle"),
    ("options", "Options", "Return", "start", None),
    ("touchpad", "Тачпад, левая половина (жесты)", "Tab", "back, touchpad", None),
    ("touchpad_right", "Тачпад, правая половина (личные вещи)", "Backspace", "", None),
    ("up", "Крестовина вверх", "I", "dpup", None),
    ("down", "Крестовина вниз", "K", "dpdown", None),
    ("left", "Крестовина влево", "J", "dpleft", None),
    ("right", "Крестовина вправо", "L", "dpright", None),
    ("move_up", "Движение вперёд", "W", None, None),
    ("move_down", "Движение назад", "S", None, None),
    ("move_left", "Движение влево", "A", None, None),
    ("move_right", "Движение вправо", "D", None, None),
    ("look_up", "Камера вверх", "Up", None, "motion_up"),
    ("look_down", "Камера вниз", "Down", None, "motion_down"),
    ("look_left", "Камера влево", "Left", None, "motion_left"),
    ("look_right", "Камера вправо", "Right", None, "motion_right"),
]


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(CONFIG_FILE.read_text()))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False))


def ini_path():
    return Path(os.environ.get("BB_CONFIG", DATA_DIR / "bbport.ini"))


def load_ini():
    values = dict(INI_DEFAULTS)
    lines = []
    try:
        lines = ini_path().read_text().splitlines()
    except OSError:
        pass
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites edited keys in place, appends missing ones, preserves comments."""
    written = set()
    out = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in values:
                if values[key] is not None:
                    out.append(f"{key}={values[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# bbport settings (in-game menu: Insert / L3+R3)")
    for key, value in values.items():
        if key not in written and value is not None:
            out.append(f"{key}={value}")
    ini_path().write_text("\n".join(out) + "\n")


def patches_dir(settings):
    return Path(settings.get("patches_dir") or DATA_DIR / "patches").expanduser()


def game_environment(s):
    """Environment for run.sh from the launcher settings."""
    env = dict(os.environ)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = s["user_dir"]
    env["BB_MODS_DIR"] = str(Path(s.get("mods_dir") or DATA_DIR / "mods").expanduser())
    env["BB_MODS_CONFIG"] = str(DATA_DIR / "mods.json")
    env["BB_MODS_ENABLED"] = "1" if s.get("mods_enabled", True) else "0"
    env["BB_PATCHES_DIR"] = str(patches_dir(s))
    env["BB_PATCHES_CONFIG"] = str(DATA_DIR / "patches.json")
    env["BB_LANGUAGE"] = s["language"]
    env["BB_MENU_LANGUAGE"] = language()
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    env["BB_PRESENT_MODE"] = s["present_mode"]
    if s.get("gamepad"):
        env["BB_GAMEPAD"] = s["gamepad"]
    if s.get("display"):
        env["BB_DISPLAY"] = s["display"]
    env["BB_SKIP_NETWORK_CHOICE"] = "1" if s.get("skip_network_choice", True) else "0"
    if s["hdr"]:
        env["BB_HDR"] = "1"
    env["BB_FPS"] = s["fps_mode"]
    if s["fps_limit"] > 0:
        env["BB_FPS_LIMIT"] = str(s["fps_limit"])
    if s["draw_pipe"]:
        env["BB_DRAW_PIPE"] = s["draw_pipe"]
    if s.get("vk_record_threads") and s["vk_record_threads"] != "auto":
        env["BB_VK_RECORD_THREADS"] = s["vk_record_threads"]
    if s["readbacks"]:
        env["BB_READBACKS"] = s["readbacks"]
    if s.get("preupload"):
        env["BB_PREUPLOAD"] = s["preupload"]
    if s["mangohud"]:
        env["MANGOHUD"] = "1"
    if s["frame_stats"]:
        env["BB_FRAME_STATS"] = "1"
    if s.get("save_log"):
        env["BB_SAVE_LOG"] = "1"
    model = s.get("memory_model", "auto")
    if model in ("new", "old"):
        env["BB_PC_MODEL"] = "1" if model == "new" else "0"
    if s.get("as_0_3"):
        env["BB_AS_0_3"] = "1"
    if s.get("crash_diag"):
        env["BB_FREE_CHECK"] = "1"
        env["BB_WRITE_LOG"] = "1"
        env["BB_PRODUCER_CHECK"] = "1"
    if s["gpu_profile"]:
        env["BB_GPU_PROFILE"] = "1"
    if s["vk_validation"]:
        env["BB_VK_VALIDATION"] = "1"
    for item in s["extra_env"].split():
        if "=" in item:
            key, value = item.split("=", 1)
            env[key] = value
    return env
