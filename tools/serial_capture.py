"""Read badge logs without toggling reset; optional bounded capture to a UTF-8 file."""
import argparse
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=float, default=60)
parser.add_argument("--output", type=Path)
parser.add_argument("--stop-file", type=Path, help="Stop when this file exists")
args = parser.parse_args()
device = serial.Serial(port=None, baudrate=115200, timeout=0.2)
device.dtr = False
device.rts = False
device.port = args.port
device.open()
deadline = time.monotonic() + args.seconds
output = args.output.open("w", encoding="utf-8") if args.output else None
try:
    while time.monotonic() < deadline and not (args.stop_file and args.stop_file.exists()):
        data = device.readline()
        if data:
            line = data.decode("utf-8", errors="replace")
            print(line, end="", flush=True)
            if output:
                output.write(line)
                output.flush()
except KeyboardInterrupt:
    pass
finally:
    device.close()
    if output:
        output.close()
