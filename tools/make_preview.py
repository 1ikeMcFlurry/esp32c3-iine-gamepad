"""Publish previews from the actual C renderer using simulated input, never device photos."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
font_path = root / "build/NotoSansCJKsc-Regular.otf"
if not font_path.is_file():
    raise SystemExit("Run python tools/fetch_font.py first")
title = ImageFont.truetype(str(font_path), 30)
label = ImageFont.truetype(str(font_path), 17)
small = ImageFont.truetype(str(font_path), 14)
panels = [
    ("devices", "01  选择与配对"), ("private_controls", "02  实时操作"),
    ("private_raw", "03  数据诊断"), ("private_unknown", "未知协议"),
    ("pairing", "配对中"), ("private_wait", "等待输入"),
    ("live", "标准 HID"), ("forget", "取消配对"),
]

def compose(items, columns, destination):
    rows = (len(items) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * 276 + 32, rows * 382 + 142), "#eef3f9")
    draw = ImageDraw.Draw(sheet)
    draw.text((32, 20), "IINE × ESP32-C3", font=title, fill="#17273e")
    draw.text((32, 67), "C 渲染器预览 · 模拟数据 · 非实机照片", font=small, fill="#586b83")
    for i, (name, caption) in enumerate(items):
        x, y = 32 + (i % columns) * 276, 115 + (i // columns) * 382
        draw.rounded_rectangle((x - 6, y - 6, x + 245, y + 325), radius=12, fill="#ffffff", outline="#d1dce9")
        with Image.open(root / "build" / f"{name}.ppm") as screen:
            sheet.paste(screen, (x, y))
        draw.text((x, y + 336), caption, font=label, fill="#17273e")
    sheet.save(destination)

(root / "docs").mkdir(exist_ok=True)
compose(panels[:3], 3, root / "docs/quick-preview.png")
compose(panels, 4, root / "docs/ui-preview.png")
