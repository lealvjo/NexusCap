"""Diagrama de arquitetura 4.0: HDMI ou Benchmark sem placa."""
from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pitch" / "arch_diagram.png"

W, H = 1920, 780
BG = (10, 12, 16)
CARD = (16, 22, 32)
INK = (230, 236, 244)
MUTED = (140, 156, 176)
CYAN = (70, 180, 220)
ORANGE = (232, 140, 72)
GREEN = (90, 196, 130)
GOLD = (255, 196, 72)


def font(size: int, bold: bool = False):
    windir = Path(r"C:\Windows\Fonts")
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    p = windir / name
    if p.is_file():
        return ImageFont.truetype(str(p), size)
    return ImageFont.load_default()


def round_box(d, xy, fill, outline, width=3, r=20):
    d.rounded_rectangle(xy, radius=r, fill=fill, outline=outline, width=width)


def center_text(d, box, lines, fill, f, gap=5):
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
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    l = 14
    for da in (2.7, -2.7):
        x = b[0] - l * math.cos(ang + da)
        y = b[1] - l * math.sin(ang + da)
        d.line([b, (x, y)], fill=color, width=4)


def main():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    f_title = font(30, True)
    f_sub = font(18)
    f_box = font(20, True)
    f_sm = font(16)

    d.text((48, 28), "Arquitetura 4.0. Com placa ou sem placa.", font=f_title, fill=INK)
    d.text(
        (48, 72),
        "Tem HDMI: o console entra no Present. Ainda não tem placa: o Benchmark valida o seu PC no mesmo caminho.",
        font=f_sub,
        fill=MUTED,
    )

    # Two inputs
    hdmi = (48, 160, 360, 300)
    bench = (48, 430, 360, 590)
    plate = (400, 160, 680, 300)
    core = (740, 240, 1140, 520)
    nr = (1220, 140, 1600, 280)
    ov = (1220, 310, 1600, 430)
    fg = (1220, 460, 1600, 580)
    mon = (1660, 280, 1872, 500)

    boxes = [
        (hdmi, CYAN, ["Tem placa HDMI", "Qualquer console", "PS5, Xbox, Switch"]),
        (plate, CYAN, ["Placa DirectShow", "NV12 nativo", "sem recapturar a mesa"]),
        (bench, GOLD, ["Ainda sem placa?", "Benchmark 40 s", "valida o seu PC"]),
        (core, CYAN, ["NexusCap", "Lobby, CFG, overlay", "Present D3D11", "DLSS 5 no frame"]),
        (nr, GREEN, ["RenoDX no Present", "1,5x recomendado", "2x otimizado"]),
        (ov, GREEN, ["Overlay nosso", "FPS, GPU, STABLE", "sem Afterburner"]),
        (fg, ORANGE, ["Frame Gen até 4x", "se tiver LS", "perfil NexusCap"]),
        (mon, GREEN, ["Monitor", "saída nativa", "ritmo do sinal"]),
    ]
    for box, col, lines in boxes:
        round_box(d, box, CARD, col)
        center_text(d, box, lines, INK, f_box if len(lines) <= 2 else f_sm)

    arrow(d, (360, 230), (400, 230), CYAN)
    arrow(d, (680, 230), (740, 320), CYAN)
    arrow(d, (360, 510), (740, 440), GOLD)
    arrow(d, (1140, 300), (1220, 210), GREEN)
    arrow(d, (1140, 380), (1220, 370), GREEN)
    arrow(d, (1140, 460), (1220, 520), ORANGE)
    arrow(d, (1600, 390), (1660, 390), GREEN)

    d.text((48, 720), "O clipe do Benchmark é The Last of Us Parte II. A nota é do seu PC. O CFG HDMI não muda.",
           font=f_sm, fill=MUTED)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    im.save(OUT)
    print("wrote", OUT, OUT.stat().st_size)


if __name__ == "__main__":
    main()
