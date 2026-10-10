"""Availability of the FSR 4.1.1 model required by a selected output/preset."""
from pathlib import Path
from bbport_i18n import tr


def fsr411_problem(directory: Path, output: str, preset: int) -> str | None:
    w, h = map(int, output.split("x"))
    model = ("t2160" if w > 1920 or h > 1080 else "t1080") + ("_m1" if preset == 4 else "_m0")
    model_dir = directory / model
    if not model_dir.is_dir() and (directory / "fp8" / model).is_dir():
        model_dir = directory / "fp8" / model
    passes = ["spd", "prepass", "pass0_post"]
    for i in range(1, 13):
        passes.extend([f"pass{i}", f"pass{i}_post"])
    passes.extend(["postpass", "rcas"])
    for name in passes:
        path = model_dir / (name + ".spv")
        if not path.is_file() or path.stat().st_size < 20 or path.stat().st_size % 4:
            return tr("Нет или повреждён файл {}").format(f"{model}/{name}.spv")
    path = model_dir / "initializer.bin"
    if not path.is_file() or path.stat().st_size != 131072:
        return tr("Нет или повреждён файл {}").format(f"{model}/initializer.bin")
    return None
