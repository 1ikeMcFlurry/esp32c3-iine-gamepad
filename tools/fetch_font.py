"""Download the pinned OFL font for regenerating UI assets; not needed to build firmware."""
import hashlib
from pathlib import Path
from urllib.request import urlopen

REVISION = "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
URL = (f"https://raw.githubusercontent.com/notofonts/noto-cjk/{REVISION}/"
       "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf")
SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
target = Path(__file__).resolve().parents[1] / "build/NotoSansCJKsc-Regular.otf"
if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == SHA256:
    print(f"Verified cached font: {target}")
else:
    with urlopen(URL, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit("Font SHA-256 mismatch; refusing to use the download")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f"Downloaded verified font: {target}")
