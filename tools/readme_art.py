"""Draw README artwork and a simulated input GIF from the actual C renderer.

Requires the font from fetch_font.py and frames produced by render_readme.c
and tests/render_preview.c. Only screen pages are shown, without device shells.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "build/NotoSansCJKsc-Regular.otf"
OUT = ROOT / "docs/assets"
OUT.mkdir(exist_ok=True)
SCALE = 2
INK = "#253747"
BLUE = "#225BCE"
MUTED = "#617888"
ICE = "#EAF1F5"

def font(size):
    return ImageFont.truetype(str(FONT), round(size * SCALE))

def xy(box):
    return tuple(round(n * SCALE) for n in box)

def text(draw, pos, value, size, color=INK, anchor="lt"):
    draw.text(xy(pos), value, font=font(size), fill=color, anchor=anchor)

def ellipse(draw, box, fill, outline=None, width=1):
    draw.ellipse(xy(box), fill=fill, outline=outline, width=round(width * SCALE))

def line(draw, points, fill, width=1):
    draw.line([xy(p) for p in points], fill=fill, width=round(width * SCALE), joint="curve")

def paste_screen(canvas, index, x, y, width=288):
    with Image.open(ROOT / "build" / f"motion-{index:02d}.ppm") as screen:
        size = (round(width * SCALE), round(width * 4 / 3 * SCALE))
        canvas.paste(screen.resize(size, Image.Resampling.NEAREST), xy((x, y)))

def cover():
    canvas = Image.new("RGB", (1440 * SCALE, 550 * SCALE), ICE)
    d = ImageDraw.Draw(canvas)
    text(d, (54, 48), "IINE × ESP32-C3", 25, BLUE)
    text(d, (49, 123), "口袋手柄，", 65)
    text(d, (49, 207), "即刻可见。", 65)
    text(d, (55, 310), "选中配对，按下即见反馈。", 24, MUTED)
    text(d, (55, 353), "BLE 直连 · 中文界面", 22, MUTED)
    for x, name, caption in ((594, "devices", "选择与配对"),
                             (861, "private_controls", "实时操作"),
                             (1128, "private_raw", "数据诊断")):
        text(d, (x, 58), caption, 21, INK)
        with Image.open(ROOT / "build" / f"{name}.ppm") as page:
            canvas.paste(page.resize((240 * SCALE, 320 * SCALE), Image.Resampling.NEAREST), xy((x, 103)))
    line(d, [(55, 471), (1368, 471)], "#CBD9E2", 1)
    text(d, (55, 490), "v0.3.1  /  L1241 实测适配", 18, BLUE)
    text(d, (1368, 490), "固件 C 渲染器 · 模拟输入预览", 17, MUTED, "rt")
    canvas.resize((1440, 550), Image.Resampling.LANCZOS).save(OUT / "cover.png")

def animation():
    frames = []
    for i in range(64):
        canvas = Image.new("RGB", (1100 * SCALE, 528 * SCALE), "#F1F5F7")
        d = ImageDraw.Draw(canvas)
        paste_screen(canvas, i, 48, 56, 312)
        text(d, (424, 46), "看见每一次操作", 39)
        text(d, (426, 108), "摇杆转动、按键按下与释放，实时反馈。", 22, MUTED)
        line(d, [(426, 165), (1034, 165)], "#D1DEE5", 1)
        if i < 16:
            title, sub = "左摇杆 · 绕一圈", "位置与坐标同步更新"
        elif i < 32:
            title, sub = "右摇杆 · 绕一圈", "双摇杆分别显示，方向一眼可见"
        elif i < 48:
            title, sub = "A / B / X / Y", "按住点亮，松开恢复"
        elif i < 56:
            title, sub = "十字键 · 四个方向", "上、右、下、左逐一反馈"
        elif i < 62:
            title, sub = "组合输入 · A + B + ZL", "同时按下，各自保留状态"
        else:
            title, sub = "全部松开", "摇杆回中，按键复位"
        text(d, (426, 205), title, 30, BLUE)
        text(d, (426, 257), sub, 21, MUTED)
        for n in range(4):
            x = 446 + n * 94
            active = (32 <= i < 48 and (i - 32) // 4 == n and i % 4 < 3) or (56 <= i < 62 and n < 2)
            ellipse(d, (x, 321, x + 62, 383), BLUE if active else "#FFFFFF", None if active else "#C9D7DF", 2)
            text(d, (x + 31, 351), "ABXY"[n], 24, "#FFFFFF" if active else INK, "mm")
        text(d, (426, 441), "固件 C 渲染器 · 模拟输入演示，非实机录像", 18, MUTED)
        # Fixed palette avoids flicker between frames.
        rgb = canvas.resize((1100, 528), Image.Resampling.LANCZOS)
        if not frames:
            palette = rgb.quantize(colors=192)
        frames.append(rgb.quantize(palette=palette, dither=Image.Dither.NONE))
    frames[0].save(OUT / "input-demo.gif", save_all=True, append_images=frames[1:],
                   duration=140, loop=0, optimize=True, disposal=1)
    frames[0].convert("RGB").save(OUT / "input-demo-still.png")

cover()
animation()
for p in OUT.iterdir():
    print(p.name, p.stat().st_size)
