#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for the native port (GTK4 / libadwaita).

Picks the game folder, edits the port's settings (bbport.ini: upscaler, preset, ...) and the
start-up tweaks passed as environment variables to run.sh, starts and stops the game and shows
its output. Launcher settings live in ~/.config/bbport-launcher/settings.json.
Russian, English, Brazilian Portuguese and Simplified Chinese (bbport_i18n: the Russian
text is the key).
"""

import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from bbport_assets import fsr411_problem
from bbport_i18n import language, set_language, tr

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk, Pango  # noqa: E402

PORT_DIR = Path(__file__).resolve().parent.parent  # native_probe (or the package's copy)
sys.path.insert(0, str(PORT_DIR / 'scripts'))
sys.path.insert(0, str(PORT_DIR / 'tools'))
from mods import discover as discover_mods  # noqa: E402
import game_check  # noqa: E402
from patches import external_patches  # noqa: E402
import fsr4_wizard  # noqa: E402
from bbport_config import (
    PORT_DIR, PACKAGED, DATA_DIR, CONFIG_DIR, CONFIG_FILE, MAX_LOG_LINES,
    PC_MODEL_SUBTITLE, PACKAGE_ONLY_ENV,
    UI_LANGUAGES, UPSCALERS, PRESETS, OUTPUT_RES, EFFECTS, MODEL_LOD,
    FPS_MODES, PRESENT_MODES, DRAW_PIPE, LANGUAGES, LIVE_RESOLUTION,
    READBACKS, PREUPLOAD, VK_RECORD_THREADS, DEFAULTS, INI_DEFAULTS, CONTROLS,
    MEMORY_MODELS,
    load_settings, save_settings, ini_path, load_ini, save_ini, patches_dir,
    game_environment,
)
from bbport_fsr_builder import (
    FSR4CAP_DIR, FSR411_BUILD_ERRORS, Fsr411Manager, fsr411_dir, fsr411_build_command,
)
from bbport_devices import (
    connected_gamepads, connected_displays, format_mouse_btn,
)
from bbport_ui_helpers import (
    flat_button, open_folder, combo_row, combo_value, FolderList,
)


class LauncherWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Bloodborne")
        self.set_default_size(980, 780)
        self.setup_css()
        self.settings = load_settings()
        set_language(self.settings.get("ui_language", ""))
        self.ini, self.ini_lines = load_ini()
        self.process = None
        self.stream = None
        self.fsr411_mgr = Fsr411Manager(self)
        self.best_detected_dll = None
        self.best_detected_loader = None
        self.build()
        self.connect("close-request", self.on_close)

    @property
    def fsr411_build(self):
        return self.fsr411_mgr.build_proc

    @fsr411_build.setter
    def fsr411_build(self, val):
        self.fsr411_mgr.build_proc = val

    def setup_css(self):
        css = """
        .tab-bar-button {
            padding: 7px 14px;
            font-weight: 600;
            font-size: 13px;
        }
        .tab-bar-button:checked {
            background-color: rgba(255, 255, 255, 0.12);
            color: #ffffff;
        }
        .log-toolbar {
            background-color: #161922;
            border-bottom: 1px solid rgba(255, 255, 255, 0.08);
            padding: 8px 12px;
        }
        .log-terminal {
            background-color: #0b0d13;
            color: #c9d1d9;
            font-family: monospace;
            font-size: 12px;
            line-height: 1.4;
        }
        button.suggested-action {
            background: linear-gradient(180deg, #b01e1e 0%, #8e1515 100%);
            color: #ffffff;
            border: 1px solid #731111;
        }
        button.suggested-action:hover {
            background: linear-gradient(180deg, #c42424 0%, #9e1717 100%);
        }
        button.suggested-action:active {
            background: #731111;
        }
        """
        provider = Gtk.CssProvider()
        provider.load_from_data(css.encode())
        display = Gdk.Display.get_default()
        if display:
            Gtk.StyleContext.add_provider_for_display(
                display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def build(self):
        """(Re)creates the window's content in the current launcher language."""
        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()

        title_lbl = Gtk.Label(label="Bloodborne")
        title_lbl.add_css_class("title")
        header.set_title_widget(title_lbl)

        self.launch_button = Gtk.Button()
        self.launch_button.connect("clicked", self.on_launch)
        header.pack_end(self.launch_button)
        toolbar.add_top_bar(header)

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.stack)
        toolbar.set_content(self.toasts)
        self.set_content(toolbar)

        log_text = self.log_text() if hasattr(self, "log_view") else ""

        tab_items = [
            ("overview", tr("Главная"), "user-home-symbolic", self.build_overview_page),
            ("graphics", tr("Графика"), "video-display-symbolic", self.build_graphics_page),
            ("performance", tr("Производительность"), "speedometer-symbolic", self.build_performance_page),
            ("controls", tr("Управление"), "input-gaming-symbolic", self.build_controls_page),
            ("mods", tr("Моды и патчи"), "application-x-addon-symbolic", self.build_mods_page),
            ("log", tr("Журнал"), "utilities-terminal-symbolic", self.build_log_page),
        ]

        self.tab_buttons = {}
        tab_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        tab_box.add_css_class("linked")
        tab_box.set_halign(Gtk.Align.CENTER)

        group = None
        for name, title, icon, build_fn in tab_items:
            page_widget = build_fn()
            self.stack.add_titled_with_icon(page_widget, name, title, icon)

            btn = Gtk.ToggleButton()
            btn.add_css_class("tab-bar-button")
            if group is None:
                group = btn
            else:
                btn.set_group(group)

            btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
            img = Gtk.Image.new_from_icon_name(icon)
            lbl = Gtk.Label(label=title)
            lbl.set_ellipsize(Pango.EllipsizeMode.NONE)
            btn_box.append(img)
            btn_box.append(lbl)
            btn.set_child(btn_box)

            def on_tab_toggled(button, tab_name=name):
                if button.get_active():
                    self.stack.set_visible_child_name(tab_name)

            btn.connect("toggled", on_tab_toggled)
            tab_box.append(btn)
            self.tab_buttons[name] = btn

        if group is not None:
            group.set_active(True)

        def on_stack_visible_child_changed(_stack, _param):
            active_name = self.stack.get_visible_child_name()
            if active_name in self.tab_buttons:
                btn = self.tab_buttons[active_name]
                if not btn.get_active():
                    btn.set_active(True)

        self.stack.connect("notify::visible-child-name", on_stack_visible_child_changed)

        tab_scroll = Gtk.ScrolledWindow()
        tab_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.NEVER)
        tab_scroll.set_propagate_natural_width(True)
        tab_scroll.set_child(tab_box)
        tab_scroll.set_halign(Gtk.Align.CENTER)
        tab_scroll.set_hexpand(True)
        tab_scroll.set_margin_top(6)
        tab_scroll.set_margin_bottom(8)

        toolbar.add_top_bar(tab_scroll)
        self.log_view.get_buffer().set_text(log_text)
        self.update_launch_button()
        self.update_game_status()
        self.update_user_status()
        self.update_upscaler_status()
        self.update_fsr411_auto_row()

    def update_launch_button(self):
        running = self.process is not None
        label = tr("Остановить") if running else tr("Запустить")
        self.launch_button.set_label(label)
        path = self.game_dir()
        ok = bool(self.settings["game_dir"]) and (path / "eboot.bin").is_file()
        can_launch = (ok or running) and (self.fsr411_build is None)
        self.launch_button.set_sensitive(can_launch)
        self.launch_button.remove_css_class("destructive-action" if not running else "suggested-action")
        self.launch_button.add_css_class("destructive-action" if running else "suggested-action")
        if hasattr(self, "fsr411_button"):
            self.fsr411_button.set_sensitive(not running and self.fsr411_build is None)
        if hasattr(self, "fsr411_auto_button"):
            self.fsr411_auto_button.set_sensitive(not running and self.fsr411_build is None and getattr(self, "best_detected_dll", None) is not None)

    def update_fsr411_auto_row(self):
        if not hasattr(self, "fsr411_auto_row"):
            return
        building = self.fsr411_build is not None
        best_dll, best_loader = fsr4_wizard.auto_select_best_dll()
        if best_dll:
            self.best_detected_dll = best_dll
            self.best_detected_loader = best_loader
            p_str = str(best_dll)
            src = "Goverlay" if "goverlay" in p_str else ("Steam" if "Steam" in p_str else "Sistema")
            self.fsr411_auto_row.set_subtitle(f"[{src}] {best_dll.name} ({best_dll.parent.name})")
            self.fsr411_auto_button.set_sensitive(not building and self.process is None)
        else:
            self.best_detected_dll = None
            self.best_detected_loader = None
            self.fsr411_auto_row.set_subtitle(tr("DLL не найдена в системе"))
            self.fsr411_auto_button.set_sensitive(False)

    def on_fsr411_auto_setup(self, _btn):
        if getattr(self, "best_detected_dll", None):
            self.on_fsr411_dll(str(self.best_detected_dll), str(self.best_detected_loader) if self.best_detected_loader else None)
        else:
            self.toasts.add_toast(Adw.Toast(title=tr("DLL не найдена в системе")))

    # --- Overview page -------------------------------------------------------------------

    def build_overview_page(self):
        page = Adw.PreferencesPage()

        # Game and files
        game = Adw.PreferencesGroup(title=tr("Игра"))
        self.game_row = Adw.ActionRow(title=tr("Папка игры (CUSA03173)"))
        self.game_status = Gtk.Image()
        self.game_row.add_suffix(self.game_status)
        self.game_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку с eboot.bin"),
                                             self.on_choose_game))
        self.game_row.add_suffix(flat_button("system-file-manager-symbolic",
                                             tr("Открыть в файловом менеджере"),
                                             lambda _b: open_folder(self, self.game_dir())))
        game.add(self.game_row)

        self.user_row = Adw.ActionRow(title=tr("Папка сохранений"))
        self.user_status = Gtk.Image()
        self.user_row.add_suffix(self.user_status)
        self.user_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку сохранений"),
                                             self.on_choose_user))
        self.user_row.add_suffix(flat_button("system-file-manager-symbolic",
                                             tr("Открыть в файловом менеджере"),
                                             lambda _b: open_folder(self, self.user_dir())))
        self.user_reset = flat_button("edit-undo-symbolic", tr("Вернуть папку по умолчанию"),
                                      self.on_reset_user)
        self.user_row.add_suffix(self.user_reset)
        game.add(self.user_row)

        self.language_row = combo_row(tr("Язык системы"), None, LANGUAGES, self.settings["language"])
        game.add(self.language_row)

        self.ui_language_row = combo_row(tr("Язык лаунчера") + " / Launcher language", None,
                                         UI_LANGUAGES, self.settings.get("ui_language", ""))
        self.ui_language_row.connect("notify::selected", self.on_ui_language)
        game.add(self.ui_language_row)
        page.add(game)

        # Auto: the new memory model on AMD, the 0.3 one elsewhere; or either by hand.
        mode = Adw.PreferencesGroup(title=tr("Режим работы"))
        self.memory_model_row = combo_row(tr("Модель памяти и трансляции"), tr(PC_MODEL_SUBTITLE),
                                          MEMORY_MODELS, self.settings.get("memory_model", "auto"))
        mode.add(self.memory_model_row)
        page.add(mode)

        return page

    # --- Graphics page -------------------------------------------------------------------

    def build_graphics_page(self):
        page = Adw.PreferencesPage()

        screen = Adw.PreferencesGroup(title=tr("Экран"))
        self.output_row = combo_row(tr("Разрешение вывода"),
                                    tr("Апскейлер дорисовывает кадр; Steam Deck — 720p"),
                                    OUTPUT_RES, self.ini.get("output_res", "1920x1080"))
        screen.add(self.output_row)
        self.live_row = combo_row(tr("Смена разрешения на лету"),
                                  tr("Без перезапуска, но медленнее на Steam Deck и старых GPU"),
                                  LIVE_RESOLUTION, self.ini.get("live_resolution", "0"))
        screen.add(self.live_row)
        self.fullscreen_row = Adw.SwitchRow(title=tr("Полноэкранный режим"),
                                            active=self.settings["fullscreen"])
        screen.add(self.fullscreen_row)
        # Issue #69: the window (and fullscreen) went to the monitor SDL calls primary.
        self.display_row = Adw.ComboRow(title=tr("Монитор"))
        self.display_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                lambda _button: self.fill_displays()))
        self.fill_displays()
        screen.add(self.display_row)
        self.present_row = combo_row(tr("Режим показа кадров"), None, PRESENT_MODES,
                                     self.settings["present_mode"])
        screen.add(self.present_row)
        self.hdr_row = Adw.SwitchRow(title=tr("Разрешить HDR"), active=self.settings["hdr"])
        screen.add(self.hdr_row)
        page.add(screen)

        upscaler = Adw.PreferencesGroup(
            title=tr("Апскейлер"),
            description=tr("Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)"))
        self.upscaler_row = combo_row(tr("Апскейлер"), None, UPSCALERS, self.ini["upscaler"])
        self.upscaler_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        upscaler.add(self.upscaler_row)

        self.preset_row = combo_row(tr("Пресет"), None, PRESETS, int(self.ini.get("preset", "4")))
        self.preset_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        self.output_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        upscaler.add(self.preset_row)

        self.sharpen_row = Adw.SwitchRow(title=tr("Резкость (RCAS)"),
                                         active=self.ini.get("sharpen") == "1")
        upscaler.add(self.sharpen_row)
        self.sharpness_row = Adw.SpinRow.new_with_range(0.0, 2.0, 0.05)
        self.sharpness_row.set_title(tr("Сила резкости"))
        self.sharpness_row.set_digits(2)
        self.sharpness_row.set_value(float(self.ini.get("sharpness", "0.5")))
        upscaler.add(self.sharpness_row)
        self.motion_row = Adw.SwitchRow(
            title=tr("Векторы движения объектов"),
            subtitle=tr("Меньше гостинга на персонажах; стоит около 10% FPS"),
            active=self.ini.get("object_motion") == "1")
        upscaler.add(self.motion_row)
        self.frame_gen_row = Adw.SwitchRow(
            title=tr("Генерация кадров (FSR 3.1)"),
            subtitle=tr("Удваивает частоту кадров с помощью интерполяции"),
            active=self.ini.get("frame_generation") == "1")
        upscaler.add(self.frame_gen_row)
        self.show_fps_row = Adw.SwitchRow(title=tr("Показывать FPS"),
                                          active=self.ini.get("show_fps") == "1")
        upscaler.add(self.show_fps_row)
        page.add(upscaler)

        fsr_auto_group = Adw.PreferencesGroup(
            title=tr("Автоматизация FSR 4.1.1"),
            description=tr("FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler / Goverlay)"))

        self.fsr411_auto_row = Adw.ActionRow(title=tr("FSR 4.1.1 из Goverlay / OptiScaler"))
        self.fsr411_auto_button = Gtk.Button(valign=Gtk.Align.CENTER, label=tr("Auto-detectar e Configurar"))
        self.fsr411_auto_button.add_css_class("suggested-action")
        self.fsr411_auto_button.connect("clicked", self.on_fsr411_auto_setup)
        self.fsr411_auto_row.add_suffix(self.fsr411_auto_button)
        fsr_auto_group.add(self.fsr411_auto_row)

        self.fsr411_row = Adw.ActionRow(title=tr("Выбрать DLL вручную…"))
        self.fsr411_button = Gtk.Button(valign=Gtk.Align.CENTER)
        self.fsr411_button.connect("clicked", self.on_fsr411_button)
        self.fsr411_row.add_suffix(self.fsr411_button)
        fsr_auto_group.add(self.fsr411_row)
        page.add(fsr_auto_group)
        self.update_fsr411_row()
        self.update_fsr411_auto_row()

        effects = Adw.PreferencesGroup(title=tr("Эффекты игры"),
                                       description=tr("Патчи игры, применяются при запуске"))
        self.lod_row = combo_row(tr("Детализация моделей"), None, MODEL_LOD,
                                 self.ini.get("model_lod", "0"))
        effects.add(self.lod_row)
        self.effect_rows = {}
        for key, title, default in EFFECTS:
            row = Adw.SwitchRow(title=tr(title),
                                active=self.ini.get(key, "1" if default else "0") == "1")
            if key == "debug_menu":
                row.set_subtitle(tr("Нужна папка adhoc из мода Nexus #253 (шрифты adhoc/font) — в "
                                    "dvdroot_ps4 игры или модом; без неё патч не применяется"))
            elif key == "puddle_reflections":
                row.set_subtitle(tr("Отражения в лужах"))
            self.effect_rows[key] = row
            effects.add(row)
        # The title's "play online / play offline": the port has no PSN.
        self.skip_network_row = Adw.SwitchRow(
            title=tr("Пропускать выбор «по сети / вне сети»"),
            subtitle=tr("Игра сразу открывает главное меню в режиме вне сети (патч игры)"),
            active=self.settings.get("skip_network_choice", True))
        effects.add(self.skip_network_row)
        page.add(effects)

        return page

    # --- Performance page ----------------------------------------------------------------

    def build_performance_page(self):
        page = Adw.PreferencesPage()

        frames = Adw.PreferencesGroup(title=tr("Частота кадров"))
        self.fps_row = combo_row(tr("Режим"), tr("Какой патч частоты кадров применить к игре"),
                                 FPS_MODES, self.settings["fps_mode"])
        frames.add(self.fps_row)
        self.limit_row = Adw.SpinRow.new_with_range(0, 480, 1)
        self.limit_row.set_title(tr("Ограничение FPS"))
        self.limit_row.set_subtitle(tr("0 — без ограничения; укажите число, чтобы ограничить FPS"))
        self.limit_row.set_value(self.settings["fps_limit"])
        frames.add(self.limit_row)
        page.add(frames)

        perf = Adw.PreferencesGroup(title=tr("Производительность GPU"))
        self.pipe_row = combo_row(
            tr("Двухстадийный конвейер GPU"),
            tr("Быстрее на 20–30%; при нестабильности выключите"), DRAW_PIPE,
            self.settings["draw_pipe"])
        perf.add(self.pipe_row)
        self.record_threads_row = combo_row(
            tr("Потоки записи Vulkan"),
            tr("Параллельная запись командных буферов Vulkan (до 5 потоков на 20+ поточных CPU)"),
            VK_RECORD_THREADS, self.settings.get("vk_record_threads", "auto"))
        perf.add(self.record_threads_row)
        page.add(perf)

        mem = Adw.PreferencesGroup(title=tr("Память и VRAM"))
        self.readbacks_row = combo_row(tr("Чтение данных GPU процессором"), None, READBACKS,
                                       self.settings["readbacks"])
        mem.add(self.readbacks_row)
        self.preupload_row = combo_row(
            tr("Фоновая загрузка в видеопамять"),
            tr("Меньше рывков при подгрузке зон"), PREUPLOAD,
            self.settings.get("preupload", ""))
        mem.add(self.preupload_row)
        page.add(mem)

        dev = Adw.PreferencesGroup(title=tr("Для разработчика"))
        self.mangohud_row = Adw.SwitchRow(title="MangoHud", active=self.settings["mangohud"])
        dev.add(self.mangohud_row)
        self.save_log_row = Adw.SwitchRow(
            title=tr("Сохранять журнал и статистику в файл"),
            subtitle=tr("В папку logs в каталоге данных: для разбора рывков и вылетов"),
            active=self.settings.get("save_log", False))
        dev.add(self.save_log_row)
        self.crash_diag_row = Adw.SwitchRow(
            title=tr("Диагностика вылетов"),
            subtitle=tr("Проверяет кучу игры и записывает записи в её память; немного медленнее"),
            active=self.settings.get("crash_diag", False))
        dev.add(self.crash_diag_row)
        self.as_0_3_row = Adw.SwitchRow(
            title=tr("Синхронизация как в 0.3"),
            subtitle=tr("Для поиска регрессий: старая модель памяти и ожидания копий как в релизе 0.3"),
            active=self.settings.get("as_0_3", False))
        dev.add(self.as_0_3_row)
        self.stats_row = Adw.SwitchRow(title=tr("Статистика кадров в журнале"),
                                       subtitle="BB_FRAME_STATS",
                                       active=self.settings["frame_stats"])
        dev.add(self.stats_row)
        self.profile_row = Adw.SwitchRow(title=tr("Профиль GPU в журнале"),
                                         subtitle="BB_GPU_PROFILE",
                                         active=self.settings["gpu_profile"])
        dev.add(self.profile_row)
        self.validation_row = Adw.SwitchRow(title=tr("Слои валидации Vulkan"),
                                            subtitle=tr("Сильно замедляет"),
                                            active=self.settings["vk_validation"])
        dev.add(self.validation_row)
        self.extra_row = Adw.EntryRow(title=tr("Доп. переменные (ИМЯ=значение через пробел)"),
                                      text=self.settings["extra_env"])
        dev.add(self.extra_row)
        page.add(dev)

        return page

    # --- Controls page -------------------------------------------------------------------

    def build_controls_page(self):
        page = Adw.PreferencesPage()

        # --- Keyboard & Mouse (Combined) -------------------------------------------------
        kbm_group = Adw.PreferencesGroup(
            title=tr("Клавиатура и мышь"),
            description=tr("Управление камерой аналоговым движением мыши, как правым стиком. Нажмите иконку клавиатуры или мыши для назначения."))

        self.mouse_sens_row = Adw.SpinRow.new_with_range(0.1, 5.0, 0.1)
        self.mouse_sens_row.set_title(tr("Чувствительность камеры (мышь)"))
        self.mouse_sens_row.set_subtitle(tr("Скорость поворота камеры аналоговым движением мыши"))
        try:
            cur_sens = float(self.ini.get("mouse_sensitivity", "1.00") or 1.0)
        except (ValueError, TypeError):
            cur_sens = 1.0
        self.mouse_sens_row.set_value(cur_sens)
        self.mouse_sens_row.connect("notify::value", lambda *_: self.on_mouse_setting_changed())
        kbm_group.add(self.mouse_sens_row)

        self.mouse_inv_y_row = Adw.SwitchRow(title=tr("Инвертировать по вертикали (Y)"),
                                             active=self.ini.get("mouse_invert_y") == "1")
        self.mouse_inv_y_row.connect("notify::active", lambda *_: self.on_mouse_setting_changed())
        kbm_group.add(self.mouse_inv_y_row)

        self.mouse_inv_x_row = Adw.SwitchRow(title=tr("Инвертировать по горизонтали (X)"),
                                             active=self.ini.get("mouse_invert_x") == "1")
        self.mouse_inv_x_row.connect("notify::active", lambda *_: self.on_mouse_setting_changed())
        kbm_group.add(self.mouse_inv_x_row)

        self.mouse_capture_row = Adw.SwitchRow(
            title=tr("Захват курсора в игре"),
            subtitle=tr("Блокирует курсор в окне во время игры; Insert или F10 освобождают"),
            active=self.ini.get("mouse_capture", "1") != "0")
        self.mouse_capture_row.connect("notify::active", lambda *_: self.on_mouse_setting_changed())
        kbm_group.add(self.mouse_capture_row)

        self.kbm_expander = Adw.ExpanderRow(
            title=tr("Назначение клавиш и мыши"),
            subtitle=tr("Клавиатура и мышь объединены; применяется при запуске игры"))
        kbm_expander = self.kbm_expander
        self.kbm_rows = {}
        self.control_rows = {}
        for name, label, key_default, pad_default, mouse_default in CONTROLS:
            if key_default is None and mouse_default is None:
                continue
            row = Adw.ActionRow(title=tr(label))
            row.add_suffix(flat_button("input-keyboard-symbolic", tr("Назначить клавишу"),
                                       lambda _b, n=name: self.assign_control("key", n)))
            if not name.startswith("move_"):
                row.add_suffix(flat_button("input-mouse-symbolic", tr("Назначить кнопку мыши"),
                                           lambda _b, n=name: self.assign_control("mouse", n)))
            row.add_suffix(flat_button("edit-undo-symbolic", tr("Сбросить"),
                                       lambda _b, n=name: self.reset_kbm_control(n)))
            kbm_expander.add_row(row)
            self.kbm_rows[name] = (row, key_default, mouse_default)
            self.control_rows[("key", name)] = (row, key_default)
            self.control_rows[("mouse", name)] = (row, mouse_default)
            self.show_kbm_control(name)
        kbm_group.add(kbm_expander)
        page.add(kbm_group)

        # --- Gamepad ---------------------------------------------------------------------
        gamepad_group = Adw.PreferencesGroup(title=tr("Геймпад"))
        self.gamepad_row = Adw.ComboRow(title=tr("Контроллер"))
        self.gamepad_row.connect("notify::selected", lambda *_: self.show_gamepad())
        self.gamepad_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                lambda _button: self.fill_gamepads()))
        self.fill_gamepads()
        gamepad_group.add(self.gamepad_row)

        self.pad_expander = Adw.ExpanderRow(
            title=tr("Назначение кнопок геймпада"),
            subtitle=tr("Применяется при запуске игры"))
        pad_expander = self.pad_expander
        self.pad_rows = {}
        for name, label, key_default, pad_default, mouse_default in CONTROLS:
            if pad_default is None:
                continue
            row = Adw.ActionRow(title=tr(label))
            row.add_suffix(flat_button("input-gaming-symbolic", tr("Назначить кнопку"),
                                       lambda _b, n=name: self.assign_control("pad", n)))
            row.add_suffix(flat_button("edit-undo-symbolic", tr("Сбросить"),
                                       lambda _b, n=name: self.set_control("pad", n, None)))
            pad_expander.add_row(row)
            self.pad_rows[name] = (row, pad_default)
            self.control_rows[("pad", name)] = (row, pad_default)
            self.show_pad_control(name)
        gamepad_group.add(pad_expander)
        page.add(gamepad_group)

        return page

    # --- Mods page -----------------------------------------------------------------------

    def build_mods_page(self):
        page = Adw.PreferencesPage()

        self.mods_group = Adw.PreferencesGroup(
            title=tr("Моды"), description=tr(
                "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
                "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске."))
        self.mods_enabled_row = Adw.SwitchRow(title=tr("Загружать моды"),
                                               active=self.settings["mods_enabled"])
        self.mods_group.add(self.mods_enabled_row)
        self.mods_folder_row = Adw.ActionRow(title=tr("Папка модов"))
        self.mods_folder_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку модов"),
                                                    self.on_choose_mods))
        self.mods_folder_row.add_suffix(flat_button("system-file-manager-symbolic", tr("Открыть папку модов"),
                                                    lambda _b: open_folder(self, self.mods_dir())))
        self.mods_folder_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                    self.on_refresh_mods))
        self.mods_group.add(self.mods_folder_row)
        self.mod_list = FolderList(self.mods_group)
        self.refresh_mods()
        page.add(self.mods_group)

        self.patches_group = Adw.PreferencesGroup(
            title=tr("Сторонние патчи"),
            description=tr("XML-патчи для версии 01.09 из папки патчей. "
                           "Применяются при запуске."))
        self.patches_folder_row = Adw.ActionRow(title=tr("Папка патчей"))
        self.patches_folder_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку патчей"),
                                                       self.on_choose_patches))
        self.patches_folder_row.add_suffix(flat_button(
            "system-file-manager-symbolic", tr("Открыть папку патчей"),
            lambda _b: open_folder(self, patches_dir(self.settings))))
        self.patches_folder_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                       self.on_refresh_patches))
        self.patches_group.add(self.patches_folder_row)
        self.patch_list = FolderList(self.patches_group)
        self.refresh_patches()
        page.add(self.patches_group)

        return page

    def on_ui_language(self, row, _param):
        choice = combo_value(row)
        if choice == self.settings.get("ui_language", ""):
            return
        self.store()
        self.settings["ui_language"] = choice
        save_settings(self.settings)
        if self.process:
            # The log keeps streaming into the current page; rebuild on the next start.
            set_language(choice)
            self.toasts.add_toast(Adw.Toast(
                title=tr("Применится после перезапуска лаунчера, пока игра запущена")))
            return
        set_language(choice)
        # After this handler returns: the row that emitted the signal is replaced.
        GLib.idle_add(lambda: self.build() and False)

    def game_dir(self):
        return Path(self.settings["game_dir"]).expanduser()

    def user_dir(self):
        return Path(self.settings["user_dir"]).expanduser() if self.settings["user_dir"] \
            else DATA_DIR / "user"

    def update_user_status(self):
        path = self.user_dir()
        custom = bool(self.settings["user_dir"])
        self.user_row.set_subtitle(str(path) if custom else tr("По умолчанию: {}").format(path))
        saves = path / "savedata"
        found = saves.is_dir() and any(saves.iterdir())
        self.user_status.set_from_icon_name("object-select-symbolic" if found else "document-new-symbolic")
        self.user_status.set_tooltip_text(tr("Найдены сохранения") if found
                                          else tr("Сохранений пока нет: игра создаст их здесь"))
        self.user_reset.set_sensitive(custom)

    def choose_folder(self, title, current, done):
        dialog = Gtk.FileDialog(title=title)
        if Path(current).is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(current)))

        def finish(dialog, result):
            try:
                folder = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            if folder:
                done(folder.get_path())
        dialog.select_folder(self, None, finish)

    def on_choose_user(self, _button):
        def chosen(path):
            self.settings["user_dir"] = path
            self.update_user_status()
            self.store()
        self.choose_folder(tr("Папка сохранений"), self.user_dir(), chosen)

    def on_reset_user(self, _button):
        self.settings["user_dir"] = ""
        self.update_user_status()
        self.store()

    def update_upscaler_status(self):
        value = combo_value(self.upscaler_row)
        if value == "fsr4":
            ok = (PORT_DIR / "fsr4_shaders").is_dir()
            hint = tr("Ассеты найдены") if ok else tr("Нет ассетов: tools/fetch_fsr4_assets.sh")
        elif value == "fsr411":
            directory = fsr411_dir()
            problem = fsr411_problem(directory, combo_value(self.output_row),
                                     int(combo_value(self.preset_row)))
            if not problem:
                has_fp8 = (directory / "fp8").is_dir()
                hint = tr("Ассеты найдены (FP8 / Float8 RDNA4)") if has_fp8 else tr("Ассеты для выбранного режима найдены")
            else:
                hint = tr("{}. Соберите FSR 4.1.1 из своей DLL кнопкой «Выбрать DLL…» ниже").format(problem)
        else:
            hint = tr("Сглаживание в разрешении вывода без модели FSR") if value == "taa" else None
        self.preset_row.set_sensitive(value not in ("taa", "off"))
        self.sharpen_row.set_sensitive(value != "off")
        self.sharpness_row.set_sensitive(value != "off")
        self.upscaler_row.set_subtitle(hint or "")

    def game_problem(self, path):
        """game_check.problem for the folder, kept until its eboot.bin or param.sfo changes (the
        image hash reads ~90 MB)."""
        try:
            key = (str(path),) + tuple(
                (f.stat().st_size, f.stat().st_mtime_ns) for f in (path / "eboot.bin", path / "sce_sys/param.sfo"))
        except OSError:
            key = (str(path),)
        if getattr(self, "_game_problem_key", None) != key:
            self._game_problem_key = key
            self._game_problem = game_check.problem(path)
        return self._game_problem

    def update_game_status(self):
        path = self.game_dir()
        ok = bool(self.settings["game_dir"]) and (path / "eboot.bin").is_file()
        self.game_row.set_subtitle(str(path) if self.settings["game_dir"] else tr("не выбрана"))
        # Other versions of the game start and then crash in its code (issues #7, #13, #14).
        problem = self.game_problem(path) if ok else None
        if not ok:
            tooltip = tr("Нет eboot.bin в папке")
        elif problem:
            kind, title, version = problem
            tooltip = {
                "missing_update": tr("Нужно обновление 1.09: скопируйте файлы дампа обновления 1.09 в папку игры с заменой (найдена версия {})").format(version),
                "wrong_eboot": tr("eboot.bin не от версии 1.09: скопируйте eboot.bin из дампа обновления 1.09 в папку игры с заменой"),
                "other_title": tr("Поддерживается только CUSA03173 с обновлением 1.09 (найдено {})").format(title),
                "unreadable": tr("eboot.bin не читается как расшифрованный исполняемый файл PS4: сделайте дамп заново"),
                "damaged_files": tr("Файлы игры повреждены при распаковке: шейдеры не распаковываются, игра зависнет на загрузке. Распакуйте игру и обновление 1.09 заново исправленным инструментом (issue #81)"),
            }[kind]
            self.game_row.set_subtitle(f"{path}\n{tooltip}")
        else:
            tooltip = tr("Bloodborne CUSA03173, версия 1.09")
        self.game_status.set_from_icon_name("object-select-symbolic" if ok and not problem else "dialog-warning-symbolic")
        self.game_status.set_tooltip_text(tooltip)
        self.update_launch_button()

    def on_choose_game(self, _button):
        def chosen(path):
            self.settings["game_dir"] = path
            self.update_game_status()
            self.store()
        self.choose_folder(tr("Папка игры (с eboot.bin)"), self.game_dir(), chosen)

    def show_kbm_control(self, name):
        if not hasattr(self, "kbm_rows") or name not in self.kbm_rows:
            return
        row, key_default, mouse_default = self.kbm_rows[name]
        key_val = self.ini.get(f"key.{name}")
        mouse_val = self.ini.get(f"mouse.{name}")

        key_str = key_val if key_val is not None else key_default
        mouse_str = mouse_val if mouse_val is not None else mouse_default

        key_display = key_str if key_str else tr("не назначено")
        mouse_display = format_mouse_btn(mouse_str) if mouse_str else tr("не назначено")

        if name.startswith("move_"):
            row.set_subtitle(tr("Клавиатура: {}").format(key_display))
        else:
            row.set_subtitle(tr("Клавиатура: {} • Мышь: {}").format(key_display, mouse_display))

    def show_pad_control(self, name):
        if not hasattr(self, "pad_rows") or name not in self.pad_rows:
            return
        row, pad_default = self.pad_rows[name]
        val = self.ini.get(f"pad.{name}")
        if val is None:
            row.set_subtitle(tr("{} (по умолчанию)").format(pad_default) if pad_default else tr("не назначено"))
        else:
            row.set_subtitle(val or tr("не назначено"))

    def show_control(self, kind, name):
        if kind in ("key", "mouse"):
            self.show_kbm_control(name)
        elif kind == "pad":
            self.show_pad_control(name)
        elif hasattr(self, "control_rows") and (kind, name) in self.control_rows:
            row, default = self.control_rows[(kind, name)]
            value = self.ini.get(f"{kind}.{name}")
            if value is None:
                row.set_subtitle(tr("{} (по умолчанию)").format(default) if default else tr("не назначено"))
            else:
                row.set_subtitle(value or tr("не назначено"))

    def reset_kbm_control(self, name):
        self.ini[f"key.{name}"] = None
        self.ini[f"mouse.{name}"] = None
        self.show_kbm_control(name)
        self.store()

    def set_control(self, kind, name, value):
        """value: the binding, or None for the default."""
        self.ini[f"{kind}.{name}"] = value
        self.show_control(kind, name)
        self.store()

    def assign_control(self, kind, name):
        tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
        try:
            process = Gio.Subprocess.new([str(tool), "--read-input", kind],
                                         Gio.SubprocessFlags.STDOUT_PIPE)
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось запустить: {}").format(error.message)))
            return
        if kind in ("key", "mouse") and hasattr(self, "kbm_rows") and name in self.kbm_rows:
            row = self.kbm_rows[name][0]
        elif kind == "pad" and hasattr(self, "pad_rows") and name in self.pad_rows:
            row = self.pad_rows[name][0]
        elif hasattr(self, "control_rows") and (kind, name) in self.control_rows:
            row, _default = self.control_rows[(kind, name)]
        else:
            return

        if kind == "mouse":
            prompt = tr("Кликните кнопкой мыши… (Esc — отмена)")
        elif kind == "key":
            prompt = tr("Нажмите клавишу на клавиатуре… (Esc — отмена)")
        else:
            prompt = tr("Нажмите кнопку на геймпаде… (Esc — отмена)")
        row.set_subtitle(prompt)

        def done(proc, result):
            try:
                _ok, out, _err = proc.communicate_utf8_finish(result)
            except GLib.Error:
                out = ""
            words = (out or "").strip().split(" ", 1)
            if len(words) == 2 and words[0] == kind:
                self.set_control(kind, name, words[1])
            else:
                self.show_control(kind, name)
        process.communicate_utf8_async(None, None, done)

    def on_mouse_setting_changed(self):
        if hasattr(self, "mouse_sens_row"):
            self.ini["mouse_sensitivity"] = f"{self.mouse_sens_row.get_value():.2f}"
            self.ini["mouse_invert_y"] = "1" if self.mouse_inv_y_row.get_active() else "0"
            self.ini["mouse_invert_x"] = "1" if self.mouse_inv_x_row.get_active() else "0"
            self.ini["mouse_capture"] = "1" if self.mouse_capture_row.get_active() else "0"
            self.store()

    def fill_gamepads(self):
        """The controller choices: the first connected one, the connected ones, and the saved
        choice while it is not connected."""
        current = self.settings.get("gamepad", "")
        choices = [(tr("Авто"), "", tr("Первый подключённый"))]
        choices += [(name, guid, name) for guid, name in connected_gamepads()]
        if current and current not in [guid for _, guid, _ in choices]:
            name = self.settings.get("gamepad_name") or current
            choices.append((tr("{} (не подключён)").format(name), current, name))
        self.gamepad_row.set_model(Gtk.StringList.new([label for label, _, _ in choices]))
        self.gamepad_row.values = [guid for _, guid, _ in choices]
        self.gamepad_row.names = [name for _, _, name in choices]
        self.gamepad_row.set_selected(self.gamepad_row.values.index(current) if current in self.gamepad_row.values else 0)
        self.show_gamepad()

    def fill_displays(self):
        """The monitor choices: the primary one, the connected ones, and the saved choice while it
        is not connected."""
        current = self.settings.get("display", "")
        choices = [(tr("Основной"), "")] + [(label, value) for value, label in connected_displays()]
        if current and current not in [value for _, value in choices]:
            choices.append((tr("{} (не подключён)").format(current), current))
        self.display_row.set_model(Gtk.StringList.new([label for label, _ in choices]))
        self.display_row.values = [value for _, value in choices]
        self.display_row.set_selected(self.display_row.values.index(current) if current in self.display_row.values else 0)

    def show_gamepad(self):
        names = getattr(self.gamepad_row, "names", None)
        selected = names[self.gamepad_row.get_selected()] if names else ""
        self.gamepad_row.set_subtitle("\n".join(
            line for line in (selected, tr("Выбранный берётся, как только подключится")) if line))

    def store(self):
        s = self.settings
        s["gamepad"] = combo_value(self.gamepad_row)
        s["gamepad_name"] = self.gamepad_row.names[self.gamepad_row.get_selected()]
        s["display"] = combo_value(self.display_row)
        s["skip_network_choice"] = self.skip_network_row.get_active()
        s["language"] = combo_value(self.language_row)
        s["fullscreen"] = self.fullscreen_row.get_active()
        s["present_mode"] = combo_value(self.present_row)
        s["hdr"] = self.hdr_row.get_active()
        s["fps_mode"] = combo_value(self.fps_row)
        s["fps_limit"] = int(self.limit_row.get_value())
        s["draw_pipe"] = combo_value(self.pipe_row)
        s["vk_record_threads"] = combo_value(self.record_threads_row)
        s["readbacks"] = combo_value(self.readbacks_row)
        s["preupload"] = combo_value(self.preupload_row)
        s["mangohud"] = self.mangohud_row.get_active()
        s["frame_stats"] = self.stats_row.get_active()
        s["save_log"] = self.save_log_row.get_active()
        s["crash_diag"] = self.crash_diag_row.get_active()
        s["memory_model"] = combo_value(self.memory_model_row)
        s["as_0_3"] = self.as_0_3_row.get_active()
        s["gpu_profile"] = self.profile_row.get_active()
        s["vk_validation"] = self.validation_row.get_active()
        s["extra_env"] = self.extra_row.get_text().strip()
        s["mods_enabled"] = self.mods_enabled_row.get_active()
        self.save_mod_profile()
        self.save_patch_profile()
        save_settings(s)
        self.ini.update({
            "upscaler": combo_value(self.upscaler_row),
            "preset": str(combo_value(self.preset_row)),
            "sharpen": "1" if self.sharpen_row.get_active() else "0",
            "sharpness": f"{self.sharpness_row.get_value():.2f}",
            "object_motion": "1" if self.motion_row.get_active() else "0",
            "frame_generation": "1" if self.frame_gen_row.get_active() else "0",
            "show_fps": "1" if self.show_fps_row.get_active() else "0",
            "output_res": combo_value(self.output_row),
            "model_lod": combo_value(self.lod_row),
            "live_resolution": combo_value(self.live_row),
            **({
                "mouse_sensitivity": f"{self.mouse_sens_row.get_value():.2f}",
                "mouse_invert_y": "1" if self.mouse_inv_y_row.get_active() else "0",
                "mouse_invert_x": "1" if self.mouse_inv_x_row.get_active() else "0",
                "mouse_capture": "1" if self.mouse_capture_row.get_active() else "0",
            } if hasattr(self, "mouse_sens_row") else {}),
            **{key: "1" if row.get_active() else "0" for key, row in self.effect_rows.items()},
            "menu_language": "pt" if self.settings.get("ui_language") == "pt_BR" else (
                "ru" if self.settings.get("ui_language") == "ru" else (
                    "en" if self.settings.get("ui_language") == "en" else self.ini.get("menu_language", "pt")
                )
            ),
        })
        save_ini(self.ini, self.ini_lines)
        self.ini, self.ini_lines = load_ini()

    def environment(self):
        return game_environment(self.settings)

    # --- mods ----------------------------------------------------------------------------

    def mods_dir(self):
        return Path(self.settings.get("mods_dir") or DATA_DIR / "mods").expanduser()

    def save_mod_profile(self):
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        rows = self.mod_list.rows
        profile = {"order": [name for name, _ in rows],
                   "disabled": [name for name, row in rows if not row.get_active()]}
        (DATA_DIR / "mods.json").write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    def refresh_mods(self):
        self.mod_list.clear()
        self.mods_folder_row.set_subtitle(str(self.mods_dir()))
        try:
            profile = json.loads((DATA_DIR / "mods.json").read_text(encoding='utf-8'))
        except (OSError, ValueError):
            profile = {}
        available = discover_mods(self.mods_dir())
        order = list(dict.fromkeys(n for n in [*profile.get("order", []), *available] if n in available))
        for name in order:
            row = Adw.SwitchRow(title=name, active=name not in profile.get("disabled", []))
            row.add_suffix(flat_button("go-up-symbolic", tr("Загрузить раньше"),
                                        lambda _b, n=name: self.move_mod(n, -1)))
            row.add_suffix(flat_button("go-down-symbolic", tr("Загрузить позже"),
                                        lambda _b, n=name: self.move_mod(n, 1)))
            self.mod_list.add(name, row)
        if not order:
            self.mods_folder_row.set_subtitle(f"{self.mods_dir()} — {tr('Модов нет')}")

    def move_mod(self, name, direction):
        rows = self.mod_list.rows
        index = next(i for i, (n, _) in enumerate(rows) if n == name)
        target = index + direction
        if 0 <= target < len(rows):
            rows[index], rows[target] = rows[target], rows[index]
            self.save_mod_profile()
            self.refresh_mods()

    def on_refresh_mods(self, _button):
        self.save_mod_profile()
        self.refresh_mods()

    def on_choose_mods(self, _button):
        self.save_mod_profile()

        def chosen(path):
            self.settings["mods_dir"] = path
            self.refresh_mods()
            self.store()
        self.choose_folder(tr("Папка модов"), self.mods_dir(), chosen)

    # --- third-party patches -------------------------------------------------------------

    def save_patch_profile(self):
        """patches.json: the switches that differ from each file's isEnabled."""
        enabled, disabled = [], []
        for key, row in self.patch_list.rows:
            if row.get_active() != row.default:
                (enabled if row.get_active() else disabled).append(key)
        path = DATA_DIR / "patches.json"
        try:
            profile = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            profile = {}
        # Patches of files not listed now (another folder) keep their choice.
        shown = {key for key, _ in self.patch_list.rows}
        profile = {"enabled": sorted({k for k in profile.get("enabled", []) if k not in shown} | set(enabled)),
                   "disabled": sorted({k for k in profile.get("disabled", []) if k not in shown} | set(disabled))}
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    def refresh_patches(self):
        self.patch_list.clear()
        directory = patches_dir(self.settings)
        try:
            profile = json.loads((DATA_DIR / "patches.json").read_text(encoding='utf-8'))
        except (OSError, ValueError):
            profile = {}
        found = external_patches(directory)
        for key, _path, meta in found:
            default = meta.get("isEnabled", "false").lower() == "true"
            active = key in profile.get("enabled", []) or (default and key not in profile.get("disabled", []))
            subtitle = key.split("/", 1)[0]
            if meta.get("Author"):
                subtitle += " · " + tr("Автор: {}").format(meta.get("Author"))
            row = Adw.SwitchRow(title=GLib.markup_escape_text(meta.get("Name") or key),
                                subtitle=GLib.markup_escape_text(subtitle), active=active)
            if meta.get("Note"):
                row.set_tooltip_text(meta.get("Note").replace("\\n", "\n"))
            row.default = default
            self.patch_list.add(key, row)
        self.patches_folder_row.set_subtitle(
            str(directory) if found else f"{directory} — {tr('Патчей нет')}")

    def on_refresh_patches(self, _button):
        self.save_patch_profile()
        self.refresh_patches()

    def on_choose_patches(self, _button):
        self.save_patch_profile()

        def chosen(path):
            self.settings["patches_dir"] = path
            self.refresh_patches()
            self.store()
        self.choose_folder(tr("Папка патчей"), patches_dir(self.settings), chosen)

    # --- log page ------------------------------------------------------------------------

    def build_log_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)

        toolbar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        toolbar.add_css_class("log-toolbar")

        self.log_status_badge = Gtk.Label(label=tr("Игра не запущена"))
        self.log_status_badge.add_css_class("badge")
        self.log_status_badge.add_css_class("badge-warning")
        toolbar.append(self.log_status_badge)

        spacer = Gtk.Box()
        spacer.set_hexpand(True)
        toolbar.append(spacer)

        copy_btn = Gtk.Button(label=tr("Скопировать"), icon_name="edit-copy-symbolic")
        copy_btn.add_css_class("flat")
        copy_btn.connect("clicked", self.on_copy_log)
        toolbar.append(copy_btn)

        clear_btn = Gtk.Button(label=tr("Очистить"), icon_name="edit-clear-symbolic")
        clear_btn.add_css_class("flat")
        clear_btn.connect("clicked", self.on_clear_log)
        toolbar.append(clear_btn)

        open_logs_btn = Gtk.Button(label=tr("Открыть папку журнала"), icon_name="folder-open-symbolic")
        open_logs_btn.add_css_class("flat")
        open_logs_btn.connect("clicked", lambda _b: open_folder(self, DATA_DIR / "logs"))
        toolbar.append(open_logs_btn)

        self.autoscroll_check = Gtk.CheckButton(label=tr("Автопрокрутка"), active=True)
        toolbar.append(self.autoscroll_check)

        box.append(toolbar)

        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_view.add_css_class("log-terminal")
        self.log_view.set_top_margin(10)
        self.log_view.set_bottom_margin(10)
        self.log_view.set_left_margin(12)
        self.log_view.set_right_margin(12)
        scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        self.log_scroller = scroller
        box.append(scroller)
        bar = Gtk.ActionBar()
        self.export_log_button = Gtk.Button(label=tr("Экспорт журнала…"))
        self.export_log_button.connect("clicked", self.on_export_log)
        bar.pack_end(self.export_log_button)
        self.copy_log_button = Gtk.Button(label=tr("Копировать"))
        self.copy_log_button.connect("clicked", self.on_copy_log)
        bar.pack_end(self.copy_log_button)
        box.append(bar)
        buffer = self.log_view.get_buffer()
        buffer.connect("changed", self.on_log_changed)
        self.on_log_changed(buffer)
        return box

    def on_log_changed(self, buffer):
        has_text = buffer.get_char_count() > 0
        if hasattr(self, "copy_log_button"):
            self.copy_log_button.set_sensitive(has_text)
        if hasattr(self, "export_log_button"):
            self.export_log_button.set_sensitive(has_text)

    def log_text(self):
        buffer = self.log_view.get_buffer()
        return buffer.get_text(*buffer.get_bounds(), False)

    def on_copy_log(self, _button):
        self.get_clipboard().set(self.log_text())
        self.toasts.add_toast(Adw.Toast(title=tr("Журнал скопирован")))

    def on_clear_log(self, _button):
        self.log_view.get_buffer().set_text("")

    def on_export_log(self, _button):
        text = self.log_text()
        dialog = Gtk.FileDialog(title=tr("Экспорт журнала"))
        dialog.set_initial_name(f"bbport-{time.strftime('%Y%m%d_%H%M%S')}.log")

        def finish(dialog, result):
            try:
                chosen = dialog.save_finish(result)
            except GLib.Error:
                return
            if not chosen or not chosen.get_path():
                return
            try:
                Path(chosen.get_path()).write_text(text, encoding="utf-8")
            except OSError as error:
                self.toasts.add_toast(Adw.Toast(
                    title=tr("Не удалось сохранить журнал: {}").format(error.strerror or error)))
                return
            self.toasts.add_toast(Adw.Toast(title=tr("Журнал сохранён: {}").format(chosen.get_path())))
        dialog.save(self, None, finish)

    def append_log(self, text):
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        if getattr(self, "autoscroll_check", None) is None or self.autoscroll_check.get_active():
            adj = self.log_scroller.get_vadjustment()
            GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    # --- process -------------------------------------------------------------------------

    def on_launch(self, _button):
        if self.process:
            self.stop_game()
            return
        self.store()
        self.log_view.get_buffer().set_text("")
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in self.environment().items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            # setsid: the game and its helpers form one process group, stopped together.
            self.process = launcher.spawnv(["setsid", "bash", str(PORT_DIR / "run.sh")])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось запустить: {}").format(error.message)))
            return
        self.stream = Gio.DataInputStream.new(self.process.get_stdout_pipe())
        self.read_line()
        self.process.wait_async(None, self.on_exit)
        self.update_launch_button()
        if hasattr(self, "log_status_badge"):
            pid = self.process.get_identifier()
            self.log_status_badge.set_label(tr("Игра запущена (PID {})").format(pid))
            self.log_status_badge.remove_css_class("badge-warning")
            self.log_status_badge.add_css_class("badge-success")
        self.stack.set_visible_child_name("log")

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        self.append_log(line + "\n")
        self.read_line()

    def stop_game(self):
        if not self.process:
            return
        pid = int(self.process.get_identifier())
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.process.force_exit()
        GLib.timeout_add_seconds(3, self.kill_if_running, pid)

    def kill_if_running(self, pid):
        if self.process:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        return False

    def on_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        self.process = None
        self.update_launch_button()
        self.update_game_status()
        if hasattr(self, "log_status_badge"):
            self.log_status_badge.set_label(tr("Игра не запущена"))
            self.log_status_badge.remove_css_class("badge-success")
            self.log_status_badge.add_css_class("badge-warning")
        self.append_log(tr("\n— игра завершилась (код {}) —\n").format(status))

    def on_close(self, _window):
        self.store()
        if self.process:
            self.stop_game()
        if self.fsr411_build:
            self.stop_fsr411_build()
        return False

    # --- FSR 4.1.1 from the user's DLL -----------------------------------------------------

    def update_fsr411_row(self, progress=None):
        self.fsr411_mgr.update_row(progress)

    def alert(self, heading, body):
        if hasattr(Adw, "AlertDialog"):
            dialog = Adw.AlertDialog(heading=heading, body=body)
            dialog.add_response("ok", "OK")
            dialog.present(self)
        else:
            self.toasts.add_toast(Adw.Toast(title=f"{heading}: {body}", timeout=10))

    def choose_dll(self, title, done):
        self.fsr411_mgr.choose_dll(title, done)

    def on_fsr411_button(self, _button):
        self.fsr411_mgr.on_button_clicked()

    def on_fsr411_dll(self, upscaler, loader=None):
        self.fsr411_mgr.on_dll_selected(upscaler, loader)

    def start_fsr411_build(self, upscaler, loader):
        self.fsr411_mgr.start_build(upscaler, loader)

    def stop_fsr411_build(self):
        self.fsr411_mgr.stop()


class LauncherApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="io.github.bbport.Launcher",
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        window = self.get_active_window() or LauncherWindow(self)
        window.present()


def play():
    """--play: the game with the saved settings, no window (Steam Deck game mode)."""
    settings = load_settings()
    if not (Path(settings["game_dir"]).expanduser() / "eboot.bin").is_file():
        print("bbport: choose the game folder in the launcher first", file=sys.stderr)
        return 1
    os.chdir(PORT_DIR)
    os.execvpe("bash", ["bash", str(PORT_DIR / "run.sh")], game_environment(settings))


def build_fsr411(arguments):
    """--build-fsr411 <upscaler DLL> [loader DLL]: the launcher's FSR 4.1.1 build, without a window."""
    if not 1 <= len(arguments) <= 2:
        print("usage: --build-fsr411 <amd_fidelityfx_upscaler_dx12.dll> [loader DLL]", file=sys.stderr)
        return 1
    command, env = fsr411_build_command(*arguments)
    return subprocess.call(command, env=env, cwd=PORT_DIR)


if __name__ == "__main__":
    if "--play" in sys.argv[1:]:
        sys.exit(play())
    if "--build-fsr411" in sys.argv[1:]:
        sys.exit(build_fsr411(sys.argv[sys.argv.index("--build-fsr411") + 1:]))
    sys.exit(LauncherApp().run(sys.argv))
