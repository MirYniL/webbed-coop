import argparse
import asyncio
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import winreg
import zipfile
from pathlib import Path

from link import PORTHOLE_APP, link, porthole_exe, steam_libraries

APP = Path(__file__).resolve().parent
EXE_SHA = "c63cecb63ed471bc68b2e5a0f90c4a0cf15a57266b7af1e2d80a0b04d0116dde"
DATA_SHA = "6caf485d6027afa3270ef0ab19595b95e137188168d1244a4a5c05bf9e85398c"
PATCHED_SHA = "305e6fe920715e78668502c2440846abc0a604749edcaef70421be8243c8d701"


def digest(file):
    with file.open("rb") as stream:
        h = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
        return h.hexdigest()


def locate(explicit):
    if explicit:
        return Path(explicit).resolve()
    for root in steam_libraries():
        game = root / "steamapps" / "common" / "Webbed"
        if (game / "webbed.exe").is_file():
            return game
    raise RuntimeError('Укажите папку игры параметром --game "путь к Webbed".')


def patch(source, target):
    with (
        zipfile.ZipFile(APP / "resources.delta") as delta,
        source.open("rb") as src,
        target.open("wb") as out,
    ):
        layout = json.loads(delta.read("layout.json"))
        if src.read(4) != b"FORM":
            raise RuntimeError("Неизвестный формат data.win.")
        src.read(4)
        out.write(b"FORM" + struct.pack("<I", sum(8 + new for _, _, new in layout)))
        for tag, old, new in layout:
            if src.read(8) != struct.pack("<4sI", tag.encode(), old):
                raise RuntimeError(f"Неподдерживаемый блок {tag}.")
            buf = src.read(old)
            if tag in delta.namelist():
                diff = delta.read(tag)
                buf = (
                    int.from_bytes(buf[:new], "little") ^ int.from_bytes(diff, "little")
                ).to_bytes(new, "little")
            out.write(struct.pack("<4sI", tag.encode(), new))
            out.write(buf)
    if digest(target) != PATCHED_SHA:
        raise RuntimeError("Контрольная сумма результата не совпала.")


FILES = ("WebbedCoop.dll", "WebbedCoopLink.exe")
BACKUP = "WebbedCoop.backup"


def install(game, remove):
    data = game / "data.win"
    backup = game / BACKUP
    original = backup / "data.win"
    manifest = backup / "files.json"
    if digest(game / "webbed.exe") != EXE_SHA:
        raise RuntimeError("Нужна Windows-версия Webbed из Steam, сборка 7679547.")
    current = digest(data)
    if current not in (DATA_SHA, PATCHED_SHA):
        raise RuntimeError(
            "Файл data.win отличается от поддерживаемой версии Webbed. Проверьте версию игры и другие моды."
        )
    if original.exists() and digest(original) != DATA_SHA:
        raise RuntimeError("Резервная копия data.win отличается от оригинала.")
    if current == PATCHED_SHA and not original.is_file():
        raise RuntimeError("Для этой установки отсутствует резервная копия оригинала.")
    recorded = (
        json.loads(manifest.read_text(encoding="utf8")) if manifest.exists() else {}
    )
    sources = {FILES[0]: APP / FILES[0], FILES[1]: Path(sys.executable)}
    hashes = {name: digest(path) for name, path in sources.items()}
    for name in FILES:
        if (game / name).exists() and digest(game / name) not in (
            hashes[name],
            recorded.get(name),
        ):
            raise RuntimeError(f"В папке игры найден другой файл {name}.")
    if (
        remove
        and current == DATA_SHA
        and not any((game / name).exists() for name in FILES)
    ):
        return
    if (
        not remove
        and current == PATCHED_SHA
        and all(
            (game / name).exists() and digest(game / name) == hashes[name]
            for name in FILES
        )
        and recorded == hashes
    ):
        return
    with tempfile.TemporaryDirectory(prefix="WebbedCoop-", dir=game) as directory:
        stage = Path(directory)
        plans = []
        if not remove:
            if not original.exists():
                backup.mkdir(exist_ok=True)
                shutil.copyfile(data, stage / "original")
                if digest(stage / "original") != DATA_SHA:
                    raise RuntimeError("Ошибка резервного копирования.")
                os.replace(stage / "original", original)
            for name, source in sources.items():
                shutil.copyfile(source, stage / name)
                plans.append((game / name, stage / name))
            (stage / "manifest").write_text(json.dumps(hashes), encoding="utf8")
            plans.append((manifest, stage / "manifest"))
            if current != PATCHED_SHA:
                patch(data, stage / "data.win")
                plans.append((data, stage / "data.win"))
        else:
            if current == PATCHED_SHA:
                shutil.copyfile(original, stage / "data.win")
                plans.append((data, stage / "data.win"))
            plans.extend((game / name, None) for name in FILES)
            plans.append((manifest, None))
        undo = []
        try:
            for i, (target, source) in enumerate(plans):
                prior = None
                if target.exists():
                    prior = stage / f"prior-{i}"
                    shutil.copyfile(target, prior)
                if source:
                    os.replace(source, target)
                elif target.exists():
                    target.unlink()
                undo.append((target, prior))
        except BaseException:
            for target, prior in reversed(undo):
                if prior:
                    os.replace(prior, target)
                else:
                    target.unlink(missing_ok=True)
            raise


def box(text, style=0):
    return ctypes.windll.user32.MessageBoxW(None, text, "Webbed • Кооператив", style)


def main():
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--install", action="store_true")
    group.add_argument("--uninstall", action="store_true")
    parser.add_argument("--game")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--link", type=int)
    args = parser.parse_args()
    if args.link:
        asyncio.run(link(args.link))
        return 0
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateMutexW.restype = ctypes.c_void_p
    lock = kernel.CreateMutexW(None, False, "Local\\WebbedCoopSetup-7679547")
    if not lock or ctypes.get_last_error() == 183:
        return 2
    try:
        game = locate(args.game)
        remove = args.uninstall
        if (
            not args.quiet
            and not (remove or args.install)
            and (game / BACKUP / "files.json").is_file()
        ):
            if (
                box(
                    "Кооператив уже установлен. Удалить мод и вернуть исходную версию игры?",
                    0x24,
                )
                != 6
            ):
                return 0
            remove = True
        try:
            install(game, remove)
        except PermissionError:
            if args.quiet or ctypes.windll.shell32.IsUserAnAdmin():
                raise
            kernel.CloseHandle(ctypes.c_void_p(lock))
            lock = None
            params = subprocess.list2cmdline(
                ["--uninstall" if remove else "--install", "--game", str(game)]
            )
            if (
                ctypes.windll.shell32.ShellExecuteW(
                    None, "runas", str(sys.executable), params, None, 1
                )
                <= 32
            ):
                raise RuntimeError("Windows не предоставила доступ к папке игры.")
            return 0
        if not args.quiet:
            if remove:
                box("Оригинальная версия Webbed восстановлена.", 0x40)
            elif porthole_exe():
                box(
                    "Кооператив установлен. Запустите Webbed из Steam и откройте меню мода клавишей F2.",
                    0x40,
                )
            elif (
                box(
                    "Кооператив установлен. Для игры через интернет нужен бесплатный Porthole из Steam. Открыть его установку?",
                    0x44,
                )
                == 6
            ):
                os.startfile(f"steam://install/{PORTHOLE_APP}")
        return 0
    except Exception as exc:
        if not args.quiet:
            box(str(exc), 0x10)
        elif sys.stderr:
            print(str(exc), file=sys.stderr)
        return 1
    finally:
        if lock:
            kernel.CloseHandle(ctypes.c_void_p(lock))


if __name__ == "__main__":
    sys.exit(main())
