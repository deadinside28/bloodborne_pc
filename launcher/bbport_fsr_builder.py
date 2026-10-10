# SPDX-License-Identifier: GPL-2.0-or-later
"""FSR 4.1.1 asset building and validation orchestration for BBPort.

Prepares commands and environments for tools/fsr4cap/build_assets.sh
and maps exit codes to localized error diagnostics.
"""

import os
import re
import signal
from pathlib import Path

from bbport_config import DATA_DIR, PACKAGED, PACKAGE_ONLY_ENV, PORT_DIR
from bbport_i18n import tr

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

FSR4CAP_DIR = PORT_DIR / "tools" / "fsr4cap"

# build_assets.sh exit statuses
FSR411_BUILD_ERRORS = {
    2: "DLL не подходит: подробности в журнале",
    3: (
        "Не найден подходящий Proton: установите GE-Proton 10 или новее (ProtonUp-Qt) или Proton "
        "Experimental / Proton-CachyOS в Steam. Если они есть — запустите в Steam любую игру с "
        "этим Proton, чтобы Steam поставил его рантайм"
    ),
    4: "Не хватает программ для сборки: список в журнале",
    5: (
        "Ни один Proton не запустил FSR 4.1 из этой DLL. Либо они слишком старые (подходят "
        "GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), либо DLL не включает FSR 4.1 на "
        "этой видеокарте (Steam Deck?): тогда соберите на ПК с Radeon RX 7000/9000 и скопируйте "
        "папку fsr4_411. Подробности в журнале"
    ),
    6: (
        "Записанные проходы не совпали с тем, что повторяет bbport (другая версия DLL или "
        "новая видеокарта?): подробности в журнале"
    ),
    7: "Не найден загрузчик FidelityFX 2.x",
    8: (
        "На NixOS запись идёт на самой системе (systemd --user) через umu-launcher из nix-shell, "
        "а здесь их не нашлось: установите umu-launcher или Nix. Подробности в журнале"
    ),
}


def fsr411_dir():
    """Where the FSR 4.1.1 assets are looked up, as run.sh does."""
    if os.environ.get("BB_FSR411_DIR"):
        return Path(os.environ["BB_FSR411_DIR"])
    return PORT_DIR / "fsr4_411" if (PORT_DIR / "fsr4_411").is_dir() else DATA_DIR / "fsr4_411"


def fsr411_build_command(upscaler, loader=None):
    """tools/fsr4cap/build_assets.sh for the user's DLL: command and environment."""
    env = dict(os.environ)
    if PACKAGED:
        for key in PACKAGE_ONLY_ENV:
            env.pop(key, None)
        env["BB_FSR4CAP_WORK"] = str(DATA_DIR / "fsr4cap")
        env["BB_FSR4CAP_CLEAN"] = "1"
    tools = FSR4CAP_DIR / "bin"
    if (tools / "dxil-spirv").is_file() and (tools / "fsr4cap.exe").is_file():
        env["BB_FSR4CAP_TOOLS"] = str(tools)
    env["BB_FSR411_OUT"] = os.environ.get("BB_FSR411_DIR") or str(
        (DATA_DIR if PACKAGED else PORT_DIR) / "fsr4_411")
    command = ["bash", str(FSR4CAP_DIR / "build_assets.sh"), str(upscaler)]
    return command + ([str(loader)] if loader else []), env


class Fsr411Manager:
    """Manages FSR 4.1.1 inspection, DLL picker, asynchronous build process, and UI feedback."""

    def __init__(self, window):
        self.window = window
        self.build_proc = None
        self.cancelled = False

    def is_building(self):
        return self.build_proc is not None

    def update_row(self, progress=None):
        building = self.is_building()
        self.window.fsr411_button.set_label(tr("Отменить") if building else tr("Выбрать DLL…"))
        if hasattr(self.window, "fsr411_auto_button"):
            self.window.fsr411_auto_button.set_sensitive(
                not building and getattr(self.window, "best_detected_dll", None) is not None)
        if progress:
            self.window.fsr411_row.set_subtitle(progress)
            if hasattr(self.window, "fsr411_auto_row"):
                self.window.fsr411_auto_row.set_subtitle(progress)
        elif not building:
            self.window.fsr411_row.set_subtitle(tr(
                "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка "
                "FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или "
                "Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)"))
            self.window.update_fsr411_auto_row()

    def choose_dll(self, title, done):
        dialog = Gtk.FileDialog(title=title)
        dll = Gtk.FileFilter()
        dll.set_name("DLL")
        dll.add_pattern("*.dll")
        dll.add_pattern("*.DLL")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(dll)
        dialog.set_filters(filters)
        dialog.set_default_filter(dll)
        folder = self.window.settings.get("fsr411_dll_dir", "")
        if folder and Path(folder).is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(folder))

        def finish(d, result):
            try:
                chosen = d.open_finish(result)
            except GLib.Error:
                return
            if chosen and chosen.get_path():
                done(chosen.get_path())
        dialog.open(self.window, None, finish)

    def on_button_clicked(self):
        if self.is_building():
            self.stop()
            return
        self.choose_dll(tr("DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)"), self.on_dll_selected)

    def on_dll_selected(self, upscaler, loader=None):
        import dll_info
        self.window.settings["fsr411_dll_dir"] = str(Path(upscaler).parent)
        status, _problem, details = dll_info.inspect(upscaler, loader)
        kind = details.get("kind")
        version = details.get("version", "?")
        if status == dll_info.UNSUPPORTED:
            if kind == "fsr4_0":
                body = tr(
                    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на "
                    "видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не "
                    "запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, "
                    "FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.").format(
                        Path(upscaler).name, version)
            elif kind == "not_fsr4":
                body = tr("{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll "
                          "версии 4.1.x.").format(Path(upscaler).name)
            else:
                models = ", ".join(details.get("models", [])) or tr("нет")
                body = tr(
                    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x "
                    "от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или "
                    "amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и "
                    "подобные) устроены иначе и пока не поддерживаются.").format(
                        Path(upscaler).name, version, models)
            self.window.alert(tr("Эта DLL не подходит"), body)
            return
        if status == dll_info.NO_LOADER:
            if kind == "loader_is_upscaler":
                self.window.alert(tr("Это не загрузчик"), tr(
                    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll "
                    "(у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и "
                    "DLL апскейлера.").format(Path(loader).name))
                return
            if loader:
                self.window.alert(tr("Загрузчик не подходит"), tr(
                    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL "
                    "апскейлера.").format(Path(loader).name, details.get("loader_version", "?")))
                return
            self.window.toasts.add_toast(Adw.Toast(title=tr(
                "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры"),
                timeout=8))
            self.choose_dll(tr("Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или "
                               "amd_fidelityfx_dx12.dll)"),
                            lambda path: self.on_dll_selected(upscaler, path))
            return
        self.start_build(upscaler, details.get("loader"))

    def start_build(self, upscaler, loader):
        command, env = fsr411_build_command(upscaler, loader)
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in env.items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            self.build_proc = launcher.spawnv(["setsid", *command])
        except GLib.Error as error:
            self.window.toasts.add_toast(Adw.Toast(title=tr("Не удалось запустить: {}").format(error.message)))
            return
        self.window.append_log(tr("\n— сборка FSR 4.1.1 из {} —\n").format(upscaler))
        stream = Gio.DataInputStream.new(self.build_proc.get_stdout_pipe())
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self._on_line)
        self.build_proc.wait_async(None, self._on_built)
        self.update_row(tr("Подготовка…"))
        self.window.update_launch_button()

    def _on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        self.window.append_log(line + "\n")
        if self.build_proc:
            step = re.match(r"\[\s*(\d+)/(\d+)\]", line)
            if step:
                self.update_row(tr("Запись проходов DLL под Proton: {} из {}").format(*step.groups()))
            elif line.startswith("Translating"):
                self.update_row(tr("Перевод проходов в SPIR-V…"))
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self._on_line)

    def stop(self):
        if not self.build_proc:
            return
        pid = int(self.build_proc.get_identifier())
        self.cancelled = True
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.build_proc.force_exit()

    def _on_built(self, process, result):
        from bbport_config import UPSCALERS
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        cancelled = self.cancelled
        self.cancelled = False
        self.build_proc = None
        self.update_row()
        self.window.update_launch_button()
        self.window.append_log(tr("— сборка FSR 4.1.1 завершилась (код {}) —\n").format(status))
        if status == 0:
            for index, (_label, value) in enumerate(UPSCALERS):
                if value == "fsr411":
                    self.window.upscaler_row.set_selected(index)
            self.window.update_upscaler_status()
            self.window.store()
            self.window.toasts.add_toast(Adw.Toast(title=tr("FSR 4.1.1 собран и выбран")))
        elif cancelled:
            self.window.toasts.add_toast(Adw.Toast(title=tr("Сборка FSR 4.1.1 отменена")))
        else:
            message = FSR411_BUILD_ERRORS.get(status, "Сборка не удалась (код {}): подробности в журнале")
            self.window.alert(tr("FSR 4.1.1 не собран"), tr(message).format(status))
