#!/usr/bin/env python3
"""Generate NexusCap sales pitch PDF (with live preview + real prints)."""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

ROOT = Path(r"c:\Users\Leo\Documents\NexusCap")
ASSETS = ROOT / "assets"
PITCH = ASSETS / "pitch"
PITCH.mkdir(parents=True, exist_ok=True)
LOGO = ASSETS / "logo.png"
OUT = PITCH / "NexusCap_Pitch.pdf"

PRINT_LOBBY = PITCH / "print_lobby_preview.jpg"
PRINT_CFG = PITCH / "print_cfg.png"
PRINT_HUD = PITCH / "print_hud_adv.png"
PRINT_SESSION = PITCH / "print_session_fgx4.jpg"
LIVE = PITCH / "shot_live.png"

BG = (8, 12, 18)
CARD = (16, 24, 36)
CYAN = (30, 170, 230)
WHITE = (236, 242, 248)
MUTED = (140, 160, 180)
ACCENT = (244, 162, 97)
GREEN = (80, 220, 160)


def register_fonts() -> tuple[str, str]:
    for bold, regular in (
        (r"C:\Windows\Fonts\segoeuib.ttf", r"C:\Windows\Fonts\segoeui.ttf"),
        (r"C:\Windows\Fonts\arialbd.ttf", r"C:\Windows\Fonts\arial.ttf"),
    ):
        if Path(bold).exists() and Path(regular).exists():
            pdfmetrics.registerFont(TTFont("PitchBold", bold))
            pdfmetrics.registerFont(TTFont("Pitch", regular))
            return "PitchBold", "Pitch"
    return "Helvetica-Bold", "Helvetica"


BOLD, REG = register_fonts()
PAGE = landscape(A4)
W, H = PAGE


def fill_bg(c: canvas.Canvas) -> None:
    c.setFillColorRGB(*(x / 255 for x in BG))
    c.rect(0, 0, W, H, fill=1, stroke=0)
    c.setFillColorRGB(*(x / 255 for x in CYAN))
    c.rect(0, 0, 6, H, fill=1, stroke=0)


def draw_text(c, text, x, y, size, bold=False, color=WHITE, align="left"):
    c.setFillColorRGB(*(v / 255 for v in color))
    c.setFont(BOLD if bold else REG, size)
    if align == "center":
        c.drawCentredString(x, y, text)
    elif align == "right":
        c.drawRightString(x, y, text)
    else:
        c.drawString(x, y, text)


def wrap_draw(c, text, x, y, max_w, size, color=MUTED, leading=None):
    leading = leading or size * 1.35
    c.setFont(REG, size)
    c.setFillColorRGB(*(v / 255 for v in color))
    words = text.split()
    line, yy = "", y
    for w in words:
        trial = (line + " " + w).strip()
        if c.stringWidth(trial, REG, size) <= max_w:
            line = trial
        else:
            c.drawString(x, yy, line)
            yy -= leading
            line = w
    if line:
        c.drawString(x, yy, line)
        yy -= leading
    return yy


def draw_image_fit(c, path: Path, x, y, max_w, max_h):
    if not path.exists():
        return
    im = Image.open(path)
    iw, ih = im.size
    scale = min(max_w / iw, max_h / ih)
    dw, dh = iw * scale, ih * scale
    # bottom-left anchor inside box
    c.drawImage(ImageReader(str(path)), x, y + (max_h - dh), width=dw, height=dh, mask="auto")


def make_arch_diagram(dst: Path) -> Path:
    w, h = 1400, 720
    im = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(im)
    try:
        font_b = ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf", 28)
        font_s = ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf", 16)
    except OSError:
        font_b = font_s = ImageFont.load_default()

    boxes = [
        (40, 280, 260, 440, "HDMI\nConsole / PC", CYAN),
        (320, 280, 560, 440, "Placa captura\nDirectShow", CYAN),
        (620, 180, 980, 540, "NexusCap  C++17\nLobby preview\n+ Present sessão", (62, 146, 204)),
        (1040, 120, 1340, 280, "RenoDX / NR\n(opcional)", ACCENT),
        (1040, 320, 1340, 480, "Frame Gen\nmira-lock", ACCENT),
        (1040, 520, 1340, 640, "Monitor\nsaída limpa", GREEN),
    ]
    for x0, y0, x1, y1, label, col in boxes:
        d.rounded_rectangle([x0, y0, x1, y1], radius=18, fill=CARD, outline=col, width=3)
        d.multiline_text((x0 + 24, y0 + 36), label, fill=WHITE, font=font_b, spacing=8)

    def arrow(a, b):
        d.line([a, b], fill=CYAN, width=4)
        x, y = b
        d.polygon([(x, y), (x - 14, y - 8), (x - 14, y + 8)], fill=CYAN)

    arrow((260, 360), (320, 360))
    arrow((560, 360), (620, 360))
    arrow((980, 260), (1040, 200))
    arrow((980, 360), (1040, 400))
    arrow((1190, 480), (1190, 520))
    d.text((40, 40), "Arquitetura: um Present. Zero recaptura de tela.", fill=WHITE, font=font_b)
    d.text((40, 90), "Sinal HDMI → placa → NexusCap → enhance/FG → monitor", fill=MUTED, font=font_s)
    im.save(dst)
    return dst


def page_cover(c):
    fill_bg(c)
    if LOGO.exists():
        c.drawImage(ImageReader(str(LOGO)), W / 2 - 45, H - 155, width=90, height=90, mask="auto")
    draw_text(c, "NEXUSCAP", W / 2, H - 195, 36, True, WHITE, "center")
    draw_text(
        c,
        "Captura HDMI nativa · Preview ao vivo · Alterações da CFG aparecem na hora",
        W / 2,
        H - 228,
        12,
        False,
        MUTED,
        "center",
    )
    pills = [
        (W * 0.18, H * 0.40, "PREVIEW\nao vivo"),
        (W * 0.50, H * 0.40, "CFG → PREVIEW\nna hora"),
        (W * 0.82, H * 0.40, "FG + RenoDX\nsem hitch"),
    ]
    for cx, cy, label in pills:
        c.setFillColorRGB(*(x / 255 for x in CARD))
        c.roundRect(cx - 100, cy - 40, 200, 90, 12, fill=1, stroke=0)
        c.setStrokeColorRGB(*(x / 255 for x in CYAN))
        c.setLineWidth(1.5)
        c.roundRect(cx - 100, cy - 40, 200, 90, 12, fill=0, stroke=1)
        a, b = label.split("\n")
        draw_text(c, a, cx, cy + 12, 12, True, CYAN, "center")
        draw_text(c, b, cx, cy - 8, 11, False, WHITE, "center")
    draw_text(c, "Pitch comercial  ·  Versão 1.0  ·  Privado", W / 2, 36, 10, False, MUTED, "center")
    c.showPage()


def page_problem(c):
    fill_bg(c)
    draw_text(c, "O problema que ninguém quer admitir", 40, H - 50, 22, True)
    wrap_draw(
        c,
        "Viewers que aplicam filtro capturando a tela (ou a janela de outro app) geram hitch, latência e inconsistência. O compositor e o capturador brigam pelo mesmo frame.",
        40,
        H - 85,
        W - 80,
        12,
    )
    c.setFillColorRGB(*(x / 255 for x in CARD))
    c.roundRect(40, 80, (W - 100) / 2, H - 200, 14, fill=1, stroke=0)
    c.roundRect(60 + (W - 100) / 2, 80, (W - 100) / 2, H - 200, 14, fill=1, stroke=0)
    left_x, right_x = 60, 80 + (W - 100) / 2
    draw_text(c, "Abordagem frágil", left_x, H - 160, 14, True, ACCENT)
    for i, line in enumerate(
        [
            "• Sinal → app → tela",
            "• Recaptura da saída",
            "• Hitch aleatório",
            "• Latência empilhada",
            "• FG amplifica o problema",
            "• Ajuste “no escuro”",
        ]
    ):
        draw_text(c, line, left_x, H - 195 - i * 28, 11)
    draw_text(c, "NexusCap", right_x, H - 160, 14, True, GREEN)
    for i, line in enumerate(
        [
            "• HDMI direto da placa",
            "• Um Present D3D11",
            "• Preview ao vivo no lobby",
            "• CFG muda → preview muda",
            "• Frametime previsível",
            "• FG mira-lock (flow 100)",
        ]
    ):
        draw_text(c, line, right_x, H - 195 - i * 28, 11)
    c.showPage()


def page_preview(c):
    """Hero slide: live preview + instant CFG feedback."""
    fill_bg(c)
    draw_text(c, "Preview ao vivo — você vê antes de entrar", 40, H - 40, 20, True)
    wrap_draw(
        c,
        "No lobby o HDMI já está rodando. Qualquer alteração na CFG (Frame Gen, escala, RenoDX, presets, atalhos) reflete no preview na hora — sem reiniciar, sem “adivinhar” o resultado.",
        40,
        H - 68,
        W - 80,
        11,
    )

    # big lobby print
    img = PRINT_LOBBY if PRINT_LOBBY.exists() else LIVE
    draw_image_fit(c, img, 30, 55, W * 0.58, H - 150)

    x = W * 0.62
    y = H - 120
    points = [
        ("AO VIVO", "Sinal da placa no letterbox"),
        ("CFG aberta", "Ajuste e veja no preview"),
        ("HUD na hora", "FPS / FG / RenoDX atualizam"),
        ("Iniciar", "Só então libera a placa"),
    ]
    for title, sub in points:
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y - 30, W - x - 28, 58, 10, fill=1, stroke=0)
        draw_text(c, title, x + 14, y + 6, 11, True, CYAN)
        draw_text(c, sub, x + 14, y - 12, 9, False, MUTED)
        y -= 72

    draw_text(c, "Ghost of Tsushima  ·  2560×1440@60  ·  FG x2 no preview", 40, 28, 9, False, MUTED)
    c.showPage()


def page_cfg_live(c):
    fill_bg(c)
    draw_text(c, "CFG → Preview na hora", 40, H - 40, 20, True)
    wrap_draw(
        c,
        "Painel Configurações (CFG) fica ao lado do preview. Liga Frame Gen, muda x2/x3/x4, escala GPU, RenoDX/NR ou preset visual — o HUD e o feedback no preview mudam imediatamente.",
        40,
        H - 68,
        W - 80,
        11,
    )

    # CFG screenshot left-ish, callouts right
    if PRINT_CFG.exists():
        draw_image_fit(c, PRINT_CFG, 30, 40, W * 0.38, H - 130)
    if PRINT_LOBBY.exists() or LIVE.exists():
        draw_image_fit(c, PRINT_LOBBY if PRINT_LOBBY.exists() else LIVE, W * 0.42, 40, W * 0.55, H - 130)

    draw_text(c, "CFG", 40, H - 95, 9, True, CYAN)
    draw_text(c, "Preview (mesmo instante)", W * 0.42, H - 95, 9, True, GREEN)
    c.showPage()


def page_arch(c, arch: Path):
    fill_bg(c)
    draw_text(c, "Arquitetura — uma passagem limpa", 40, H - 45, 20, True)
    wrap_draw(
        c,
        "Regra de ouro: a captura nunca olha para o framebuffer já filtrado. Preview no lobby = sinal real; sessão = o mesmo pipeline, sem recapturar a tela.",
        40,
        H - 75,
        W - 80,
        11,
    )
    c.drawImage(ImageReader(str(arch)), 30, 40, width=W - 60, height=H - 140, preserveAspectRatio=True, mask="auto")
    c.showPage()


def page_session(c):
    fill_bg(c)
    draw_text(c, "Sessão — métricas ADV + Frame Gen", 40, H - 40, 18, True)
    draw_text(
        c,
        "Gameplay limpo  ·  60 → 240 com FG x4  ·  STABLE  ·  e2e ~1 ms  ·  RenoDX ON",
        40,
        H - 60,
        10,
        False,
        CYAN,
    )

    # session hero
    img = PRINT_SESSION if PRINT_SESSION.exists() else LIVE
    draw_image_fit(c, img, 30, 45, W * 0.62, H - 120)

    x = W * 0.66
    y = H - 105
    for title, sub in [
        ("FG x4", "60 → 240 LSFG3 mira-lock"),
        ("STABLE", "frametime flat no gráfico"),
        ("e2e ~1 ms", "latência de Present baixa"),
        ("Analyze ON", "prova medida, não feeling"),
        ("C++17", "hot path sem GC"),
    ]:
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y - 28, W - x - 28, 52, 10, fill=1, stroke=0)
        draw_text(c, title, x + 12, y + 4, 11, True, CYAN)
        draw_text(c, sub, x + 12, y - 14, 9, False, MUTED)
        y -= 66
    c.showPage()


def page_hud(c):
    fill_bg(c)
    draw_text(c, "HUD avançado — transparência total", 40, H - 40, 18, True)
    wrap_draw(
        c,
        "FPS cap | FG, frametime p95, sync, RenoDX, CPU/GPU/RAM/VRAM/PWR, last600. O mesmo nível de detalhe que o preview já adiantava no lobby.",
        40,
        H - 68,
        W - 80,
        11,
    )
    img = PRINT_HUD if PRINT_HUD.exists() else PRINT_SESSION
    draw_image_fit(c, img, 80, 40, W - 160, H - 130)
    c.showPage()


def page_product(c):
    fill_bg(c)
    draw_text(c, "O produto — o que o usuário sente", 40, H - 45, 20, True)
    cards = [
        ("Preview ao vivo", "HDMI no lobby com letterbox. AO VIVO antes do Iniciar."),
        ("CFG na hora", "Mudou FG, escala, RenoDX ou preset → preview e HUD atualizam."),
        ("Sessão limpa", "Esc volta ao lobby. HUD compacto ou ADV com frametime."),
        ("Escala 25–100%", "4K na placa, 50% no GPU se precisar."),
        ("RenoDX / NR", "Enhance no Present; Delete liga/desliga e persiste."),
        ("Frame Gen", "Lossless auto/Pasta. Mira-lock flow 100 anti-ghost."),
    ]
    cols, rows = 3, 2
    gap = 16
    cw = (W - 80 - gap * (cols - 1)) / cols
    ch = (H - 120 - gap * (rows - 1)) / rows
    for i, (title, body) in enumerate(cards):
        col, row = i % cols, i // cols
        x = 40 + col * (cw + gap)
        y = H - 100 - (row + 1) * ch - row * gap
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y, cw, ch, 12, fill=1, stroke=0)
        c.setStrokeColorRGB(*(v / 255 for v in CYAN))
        c.setLineWidth(1)
        c.roundRect(x, y, cw, ch, 12, fill=0, stroke=1)
        draw_text(c, title, x + 16, y + ch - 28, 12, True)
        wrap_draw(c, body, x + 16, y + ch - 52, cw - 32, 9, MUTED)
    c.showPage()


def page_cpp(c):
    fill_bg(c)
    draw_text(c, "Por que C++17 — e por que isso vende", 40, H - 50, 20, True)
    wrap_draw(
        c,
        "O caminho do frame é nativo, determinístico e medível — por isso o preview consegue acompanhar a CFG em tempo real.",
        40,
        H - 80,
        W - 80,
        11,
    )
    rows = [
        ("Latência", "Sem GC no hot path. Cada ms conta em captura + FG."),
        ("APIs Windows", "DirectShow, D3D11, WASAPI, ReShade — no mesmo processo."),
        ("Preview vivo", "Mesmo sinal da placa no lobby; CFG não precisa “aplicar e reabrir”."),
        ("Controle GPU", "Decode NV12, scale e Present sob nosso código."),
        ("Qualidade", "Testes unitários, analyze JSONL, HUD de hitch/frametime."),
        ("Marca", "Logo no app, no .exe e na experiência do lobby."),
    ]
    y = H - 130
    for title, body in rows:
        c.setFillColorRGB(*(v / 255 for v in CYAN))
        c.circle(52, y + 6, 5, fill=1, stroke=0)
        draw_text(c, title, 70, y, 12, True)
        draw_text(c, body, 200, y, 11, False, MUTED)
        y -= 42
    c.showPage()


def page_close(c):
    fill_bg(c)
    if LOGO.exists():
        c.drawImage(ImageReader(str(LOGO)), W / 2 - 40, H - 150, width=80, height=80, mask="auto")
    draw_text(c, "NexusCap", W / 2, H - 190, 28, True, WHITE, "center")
    draw_text(
        c,
        "HDMI nativo. Preview ao vivo. CFG que você vê na hora.",
        W / 2,
        H - 220,
        13,
        False,
        MUTED,
        "center",
    )
    c.setFillColorRGB(*(v / 255 for v in CARD))
    c.roundRect(W / 2 - 230, H / 2 - 70, 460, 120, 14, fill=1, stroke=0)
    draw_text(c, "Próximo passo", W / 2, H / 2 + 25, 12, True, CYAN, "center")
    draw_text(c, "Demo ao vivo · build Release · repo privado v1.0", W / 2, H / 2 - 5, 12, False, WHITE, "center")
    draw_text(c, "github.com/lealvjo/NexusCap", W / 2, H / 2 - 35, 11, False, MUTED, "center")
    draw_text(c, "Feito em C++17  ·  Windows x64  ·  D3D11 / DirectShow", W / 2, 40, 9, False, MUTED, "center")
    c.showPage()


def main() -> None:
    arch = make_arch_diagram(PITCH / "arch_diagram.png")
    c = canvas.Canvas(str(OUT), pagesize=PAGE)
    c.setTitle("NexusCap — Pitch")
    c.setAuthor("NexusCap")
    page_cover(c)
    page_problem(c)
    page_preview(c)
    page_cfg_live(c)
    page_arch(c, arch)
    page_session(c)
    page_hud(c)
    page_product(c)
    page_cpp(c)
    page_close(c)
    c.save()
    print(OUT)


if __name__ == "__main__":
    main()
