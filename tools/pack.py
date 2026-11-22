"""Builds dist/Webbed-Coop-Setup.exe: the native installer with its payload appended.

Payload (installed next to webbed.exe):
  WebbedCoop.dll                 the GameMaker extension (src/)
  WebbedCoop/pythonw.exe ...     the official, signed CPython runtime from python.org
  WebbedCoop/python311.zip       its standard library, compiled
  WebbedCoop/app/*.py            the network helper (helper/)
  data.delta                     data.win patch (registers the extension) as runs of XOR difference

Usage: python tools/pack.py <setup-stub.exe> <WebbedCoop.dll> <CPython 3.11 x64 folder> <out.exe>
"""

import io
import json
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXCLUDE = ["tkinter", "test", "unittest", "pydoc", "doctest", "idlelib", "lib2to3", "distutils", "setuptools",
           "pip", "sqlite3", "pdb", "turtle", "turtledemo", "ensurepip", "venv"]
ALWAYS = ["encodings", "asyncio", "json", "ctypes", "concurrent", "importlib", "collections", "re"]
RUNTIME = [
    "pythonw.exe", "python311.dll", "python3.dll", "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt",
]
EXTENSIONS = [
    "_asyncio.pyd", "_ctypes.pyd", "libffi-8.dll", "_overlapped.pyd", "_queue.pyd", "_socket.pyd", "select.pyd",
]


def stdlib_zip(lib: Path) -> bytes:
    """Only the standard library the helper imports (found by modulefinder), plus whole packages
    it leans on at run time, compiled into one zip."""
    import modulefinder

    finder = modulefinder.ModuleFinder(path=[str(ROOT / "helper"), str(lib)], excludes=EXCLUDE)
    finder.run_script(str(ROOT / "helper" / "coop.py"))
    picked = {Path(m.__file__) for m in finder.modules.values() if m.__file__ and m.__file__.endswith(".py")}
    for package in ALWAYS:
        picked.update((lib / package).rglob("*.py"))
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(picked):
            try:
                rel = path.relative_to(lib)
            except ValueError:
                continue  # the helper's own files go to app/
            if "__pycache__" in rel.parts:
                continue
            code = compile(path.read_bytes(), "<stdlib>/" + rel.as_posix(), "exec", optimize=1)
            import importlib._bootstrap_external as bootstrap

            data = bootstrap._code_to_timestamp_pyc(code, 0, 0)
            z.writestr(rel.with_suffix(".pyc").as_posix(), bytes(data))
    return buf.getvalue()


def raw_delta(zip_path: Path) -> bytes:
    """resources.delta (zip: layout.json + per-chunk XOR) -> flat format read by setup.c."""
    out = bytearray()
    with zipfile.ZipFile(zip_path) as z:
        layout = json.loads(z.read("layout.json"))
        out += struct.pack("<I", len(layout))
        for tag, old, new in layout:
            diff = z.read(tag) if tag in z.namelist() else b""
            runs = list(nonzero_runs(diff))
            out += tag.encode("ascii") + struct.pack("<III", old, new, len(runs))
            for start, end in runs:
                out += struct.pack("<II", start, end - start) + diff[start:end]
    return bytes(out)


def nonzero_runs(diff, gap=32):
    """Stretches of the XOR difference that are not zero; short zero gaps stay inside a run."""
    i, n = 0, len(diff)
    while i < n:
        while i < n and not diff[i]:
            i += 1
        if i >= n:
            return
        start, zeros = i, 0
        while i < n and zeros <= gap:
            zeros = zeros + 1 if not diff[i] else 0
            i += 1
        yield start, i - zeros


def main(stub, dll, python, target):
    python = Path(python)
    files = {"WebbedCoop.dll": Path(dll).read_bytes(), "data.delta": raw_delta(ROOT / "installer" / "resources.delta")}
    for name in RUNTIME:
        files[f"WebbedCoop/{name}"] = (python / name).read_bytes()
    for name in EXTENSIONS:
        files[f"WebbedCoop/{name}"] = (python / "DLLs" / name).read_bytes()
    files["WebbedCoop/python311.zip"] = stdlib_zip(python / "Lib")
    # Embedded-style path file: the runtime ignores PYTHONPATH and any other Python on the PC.
    files["WebbedCoop/python311._pth"] = b"python311.zip\r\n.\r\napp\r\n"
    for name in ("link.py", "coop.py"):
        files[f"WebbedCoop/app/{name}"] = (ROOT / "helper" / name).read_bytes()
    for name in ("LICENSE",):
        files[f"WebbedCoop/{name}"] = (ROOT / name).read_bytes()

    exe = bytearray(Path(stub).read_bytes())
    start = len(exe)
    for name, data in files.items():
        raw = name.encode("utf-8")
        exe += struct.pack("<I", len(raw)) + raw + struct.pack("<I", len(data)) + data
    exe += struct.pack("<Q", start) + b"WBCOOP02"
    Path(target).parent.mkdir(parents=True, exist_ok=True)
    Path(target).write_bytes(exe)
    print(f"{target}: {len(exe) / 1e6:.1f} MB, {len(files)} files")


if __name__ == "__main__":
    main(*sys.argv[1:5])
