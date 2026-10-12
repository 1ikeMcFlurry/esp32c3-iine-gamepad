"""Run portable C tests on Linux/WSL with GCC and ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
parser.add_argument("--preview", action="store_true", help="Also render PPM fixtures into build/")
args = parser.parse_args()
build = root / "build/host"
build.mkdir(parents=True, exist_ok=True)
flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g", "-I", "main"]
cases = [
    ("test_hid_input", ["main/hid_input.c", "tests/test_hid_input.c"], []),
    ("test_iine_protocol", ["main/iine_protocol.c", "tests/test_iine_protocol.c"],
     ["tests/fixtures/l1241_fw2702_input.txt"]),
    ("test_gamepad_ui", ["main/gamepad_ui.c", "main/gamepad_render.c", "tests/test_gamepad_ui.c"], []),
]
if args.preview:
    cases.append(("render_preview", ["main/gamepad_ui.c", "main/gamepad_render.c", "tests/render_preview.c"], [str(root / "build")]))
for name, sources, arguments in cases:
    executable = build / name
    subprocess.run([args.cc, *flags, *sources, "-o", str(executable)], cwd=root, check=True)
    subprocess.run([str(executable), *arguments], cwd=root, check=True)
    print(f"PASS: {name}", flush=True)
