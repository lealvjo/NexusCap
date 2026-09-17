"""Diagrama de arquitetura escuro para o pitch 3.0."""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pitch" / "arch_diagram.png"

W, H = 1920, 720
BG = (10, 12, 16)
CARD = (16, 22, 32)
INK = (230, 236, 244)
MUTED = (140, 156, 176)
CYAN = (70, 180, 220)
ORANGE = (232, 140, 72)
GREEN = (90, 196, 130)
GOLD = (80, 210, 140)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    windir = Path(r"C:\Windows\Fonts")
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    p = windir / name
    if p.is_file():
        return ImageFont.truetype(str(p), size)
    return ImageFont.load_default()


def round_box(d: ImageDraw.ImageDraw, xy, fill, outline, width=3, r=22):
    d.rounded_rectangle(xy, radius=r, fill=fill, outline=outline, width=width)


def center_text(d, box, lines, fill, f, gap=6):
    x0, y0, x1, y1 = box
    total = sum(d.textbbox((0, 0), t, font=f)[3] for t in lines) + gap * (len(lines) - 1)
    y = y0 + (y1 - y0 - total) / 2
    for t in lines:
        bb = d.textbbox((0, 0), t, font=f)
        tw = bb[2] - bb[0]
        d.text(((x0 + x1 - tw) / 2, y), t, font=f, fill=fill)
        y += bb[3] - bb[1] + gap


def arrow(d, a, b, color):
    d.line([a, b], fill=color, width=4)
    # chevron
    import math
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    l = 14
    for da in (2.7, -2.7):
        x = b[0] - l * math.cos(ang + da)
        y = b[1] - l * math.sin(ang + da)
        d.line([b, (x, y)], fill=color, width=4)


def main():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    f_title = font(32, True)
    f_sub = font(18)
    f_box = font(22, True)
    f_sm = font(18)

    d.text((56, 36), "Arquitetura: HDMI de qualquer consola. Sem recapturar a tela.", font=f_title, fill=INK)
    d.text((56, 82), "Consola  →  placa HDMI  →  NexusCap  →  RenoDX 1,5x  →  monitor", font=f_sub, fill=MUTED)

    boxes = [
        ((56, 280, 320, 470), CYAN, ["HDMI", "Qualquer consola"]),
        ((380, 280, 680, 470), CYAN, ["Placa captura", "DirectShow · NV12"]),
        ((740, 200, 1180, 550), CYAN, ["NexusCap  (C++)", "Lobby + preview", "Sessão nativa 1440p"]),
        ((1280, 150, 1860, 320), GOLD, ["RenoDX  ·  modo 1,5x", "NR no Present  ·  ~60 fps"]),
        ((1280, 360, 1860, 500), ORANGE, ["Frame Gen  até 4x", "Lossless Scaling · auto"]),
        ((1280, 540, 1860, 660), GREEN, ["Monitor", "saída limpa"]),
    ]
    for box, col, lines in boxes:
        round_box(d, box, CARD, col)
        center_text(d, box, lines, INK, f_box if len(lines) <= 2 else f_sm)

    arrow(d, (320, 375), (380, 375), CYAN)
    arrow(d, (680, 375), (740, 375), CYAN)
    arrow(d, (1180, 280), (1280, 235), GOLD)
    arrow(d, (1180, 430), (1280, 430), ORANGE)
    arrow(d, (1570, 500), (1570, 540), GREEN)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    im.save(OUT)
    print("wrote", OUT, OUT.stat().st_size)


if __name__ == "__main__":
    main()
