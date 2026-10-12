"""Build the standalone ESP32-C3 IINE demo; flash only when --flash is explicit."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"), help="ESP-IDF v5.5.3 directory (defaults to IDF_PATH)")
    parser.add_argument("--tools", type=Path, default=Path(os.environ.get("IDF_TOOLS_PATH", str(Path.home() / ".espressif"))))
    parser.add_argument("--menuconfig", action="store_true")
    parser.add_argument("--flash", action="store_true", help="Write app at the existing factory partition only")
    parser.add_argument("--monitor", action="store_true")
    parser.add_argument("--port", help="Serial port, e.g. COM5")
    args = parser.parse_args()
    if (args.flash or args.monitor) and not args.port:
        parser.error("--flash / --monitor require an explicit --port")
    project = Path(__file__).resolve().parent
    if args.idf is None:
        parser.error("Activate ESP-IDF v5.5.3 first, or pass --idf and --tools explicitly.")
    idf = args.idf.resolve()
    if not (idf / "tools/idf.py").is_file():
        parser.error("ESP-IDF not found. Install v5.5.3 and pass --idf / --tools, or use idf.py directly.")
    env = os.environ.copy()
    # IDF's installed-target registry keys include the literal IDF_PATH string.
    # Keep forward slashes on Windows, matching the official install location.
    env.update(IDF_PATH=idf.as_posix(), IDF_TOOLS_PATH=args.tools.resolve().as_posix(),
               IDF_PYTHON_ENV_PATH=sys.prefix, IDF_TARGET="esp32c3", PYTHONUTF8="1")
    exported = subprocess.run(
        [sys.executable, str(idf / "tools/idf_tools.py"), "export", "--format", "key-value"],
        env=env, capture_output=True, text=True, encoding="utf-8")
    if exported.returncode:
        print(exported.stderr, file=sys.stderr)
        return exported.returncode
    for line in exported.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            env[key] = value.replace("%PATH%", env["PATH"]).replace("$PATH", env["PATH"])
    env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env["PATH"]
    command = [sys.executable, str(idf / "tools/idf.py"), "-B", str(project / "build")]
    if args.port:
        command += ["-p", args.port]
    if args.menuconfig:
        command += ["menuconfig"]
    command += ["build"]
    if args.flash:
        command += ["app-flash"]
    if args.monitor:
        command += ["monitor"]
    return subprocess.call(command, cwd=project, env=env)


if __name__ == "__main__":
    sys.exit(main())
