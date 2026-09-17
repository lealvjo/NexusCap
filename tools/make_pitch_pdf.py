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
PRINT_HUD = PITCH / "print_hud_adv_nr.png"
PRINT_HUD_OFF = PITCH / "print_hud_adv.png"
PRINT_SESSION = PITCH / "print_session_nr_on.jpg"
PRINT_SESSION_FG = PITCH / "print_session_nr_on_fg.jpg"
PRINT_NR_ON = PITCH / "print_compare_nr_on.jpg"
PRINT_NR_OFF = PITCH / "print_compare_nr_off.jpg"
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
        (620, 180, 980, 540, "NexusCap  (C++)\nLobby + preview\n+ sessão na tela", (62, 146, 204)),
        (1040, 120, 1340, 280, "Filtro RenoDX\n(opcional)", ACCENT),
        (1040, 320, 1340, 480, "Frame Gen\nmira firme", ACCENT),
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
    d.text((40, 40), "Arquitetura: uma saída limpa. Sem recapturar a tela.", fill=WHITE, font=font_b)
    d.text((40, 90), "Sinal HDMI → placa → NexusCap → filtro/Frame Gen → monitor", fill=MUTED, font=font_s)
    im.save(dst)
    return dst


def page_cover(c):
    fill_bg(c)
    if LOGO.exists():
        c.drawImage(ImageReader(str(LOGO)), W / 2 - 45, H - 148, width=90, height=90, mask="auto")
    draw_text(c, "NEXUSCAP", W / 2, H - 188, 36, True, WHITE, "center")
    # Hero sell: DLSS 5 first — that's what converts.
    c.setFillColorRGB(*(x / 255 for x in CARD))
    c.roundRect(W / 2 - 280, H - 268, 560, 58, 12, fill=1, stroke=0)
    c.setStrokeColorRGB(*(x / 255 for x in GREEN))
    c.setLineWidth(2)
    c.roundRect(W / 2 - 280, H - 268, 560, 58, 12, fill=0, stroke=1)
    draw_text(c, "DLSS 5  ·  Neural Rendering na imagem", W / 2, H - 240, 16, True, GREEN, "center")
    draw_text(
        c,
        "Já vem no instalador  ·  filtro em ~2 ms  ·  sem recapturar a tela",
        W / 2,
        H - 258,
        10,
        False,
        MUTED,
        "center",
    )
    draw_text(
        c,
        "Captura HDMI nativa  ·  Preview ao vivo  ·  Frame Gen opcional (Lossless Scaling)",
        W / 2,
        H - 300,
        11,
        False,
        MUTED,
        "center",
    )
    pills = [
        (W * 0.18, H * 0.38, "DLSS 5\njá no instalador"),
        (W * 0.50, H * 0.38, "filtro ~2 ms\nimagem em 100%"),
        (W * 0.82, H * 0.38, "Frame Gen\nopcional (Steam)"),
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
    draw_text(c, "Pitch comercial  ·  Versao 2.4.0  ·  Setup na Release GitHub", W / 2, 36, 10, False, MUTED, "center")
    c.showPage()


def page_problem(c):
    fill_bg(c)
    draw_text(c, "O problema que ninguém quer admitir", 40, H - 50, 22, True)
    wrap_draw(
        c,
        "Apps que “filtram” capturando a tela (ou a janela de outro programa) engasgam, atrasam a imagem e ficam inconsistentes. Dois programas brigam pelo mesmo quadro na tela.",
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
            "• Recaptura o que já saiu",
            "• Engasgos aleatórios",
            "• Atraso se acumulando",
            "• Frame Gen piora o caos",
            "• Ajuste “no escuro”",
        ]
    ):
        draw_text(c, line, left_x, H - 195 - i * 28, 11)
    draw_text(c, "NexusCap + DLSS 5", right_x, H - 160, 14, True, GREEN)
    for i, line in enumerate(
        [
            "• HDMI direto da placa",
            "• DLSS 5 na imagem (incluso)",
            "• Filtro rápido (~2 ms)",
            "• Preview ao vivo no lobby",
            "• Ritmo de frames estável",
            "• Frame Gen opcional (mira firme)",
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
        "No lobby o HDMI já está rodando. Qualquer mudança nos ajustes (Frame Gen, escala, filtro RenoDX, presets, atalhos) aparece no preview na hora — sem reiniciar, sem “adivinhar” o resultado.",
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
        ("AO VIVO", "HDMI da placa no lobby"),
        ("DLSS 5 / NR", "Liga e vê o filtro na hora"),
        ("Frame Gen", "x2 / x3 / x4 no preview"),
        ("Iniciar", "Só então libera a placa"),
    ]
    for title, sub in points:
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y - 30, W - x - 28, 58, 10, fill=1, stroke=0)
        draw_text(c, title, x + 14, y + 6, 11, True, CYAN)
        draw_text(c, sub, x + 14, y - 12, 9, False, MUTED)
        y -= 72

    draw_text(c, "God of War  ·  1440p60  ·  DLSS 5 ON no preview  ·  Nexus Cap v2.4.0", 40, 28, 9, False, MUTED)
    c.showPage()


def page_cfg_live(c):
    fill_bg(c)
    draw_text(c, "Ajustes → Preview na hora", 40, H - 40, 20, True)
    wrap_draw(
        c,
        "Painel de ajustes ao lado do preview. Frame Gen, escala, DLSS 5 / Neural Rendering e presets "
        "atualizam a imagem na hora — sem reiniciar.",
        40,
        H - 68,
        W - 80,
        11,
    )

    if PRINT_CFG.exists():
        draw_image_fit(c, PRINT_CFG, 30, 40, W * 0.38, H - 130)
    if PRINT_LOBBY.exists() or LIVE.exists():
        draw_image_fit(c, PRINT_LOBBY if PRINT_LOBBY.exists() else LIVE, W * 0.42, 40, W * 0.55, H - 130)

    draw_text(c, "CFG — Frame Gen + DLSS 5", 40, H - 95, 9, True, CYAN)
    draw_text(c, "Lobby — preview ao vivo", W * 0.42, H - 95, 9, True, GREEN)
    c.showPage()


def page_lossless_req(c):
    """Frame Gen dependency — Lossless Scaling is NOT bundled."""
    fill_bg(c)
    draw_text(c, "Frame Gen — requisito importante", 40, H - 45, 22, True, ACCENT)
    wrap_draw(
        c,
        "O NexusCap NÃO inclui o Lossless Scaling. Frame Gen (gerar frames extras para suavizar) é opcional: quem quiser usar precisa ter o Lossless Scaling instalado pela Steam.",
        40,
        H - 80,
        W - 80,
        12,
        WHITE,
    )

    # two cards
    c.setFillColorRGB(*(x / 255 for x in CARD))
    c.roundRect(40, H / 2 - 20, (W - 100) / 2, 200, 14, fill=1, stroke=0)
    c.roundRect(60 + (W - 100) / 2, H / 2 - 20, (W - 100) / 2, 200, 14, fill=1, stroke=0)

    left = 60
    right = 80 + (W - 100) / 2
    draw_text(c, "1. Ter o Lossless Scaling", left, H / 2 + 150, 13, True, CYAN)
    wrap_draw(
        c,
        "Instale pelo Steam. Sem o LosslessScaling.exe, captura HDMI e o filtro RenoDX funcionam normalmente — só o Frame Gen fica indisponível.",
        left,
        H / 2 + 120,
        (W - 100) / 2 - 40,
        10,
        MUTED,
    )

    draw_text(c, "2. Caminho do programa", right, H / 2 + 150, 13, True, CYAN)
    wrap_draw(
        c,
        "O app tenta achar sozinho (pasta Steam). Se não encontrar, abra Ajustes → Frame Gen → botão Pasta e selecione o LosslessScaling.exe. Auto limpa o caminho manual.",
        right,
        H / 2 + 120,
        (W - 100) / 2 - 40,
        10,
        MUTED,
    )

    c.setFillColorRGB(*(x / 255 for x in CARD))
    c.roundRect(40, 50, W - 80, 70, 12, fill=1, stroke=0)
    c.setStrokeColorRGB(*(x / 255 for x in ACCENT))
    c.setLineWidth(1.5)
    c.roundRect(40, 50, W - 80, 70, 12, fill=0, stroke=1)
    draw_text(c, "Resumo", 56, 95, 11, True, ACCENT)
    draw_text(
        c,
        "Lossless Scaling = fora do instalador  ·  busca automática primeiro  ·  Ajustes → Pasta se falhar",
        56,
        70,
        11,
        False,
        WHITE,
    )
    c.showPage()


def page_arch(c, arch: Path):
    fill_bg(c)
    draw_text(c, "Arquitetura — uma passagem limpa", 40, H - 45, 20, True)
    wrap_draw(
        c,
        "Regra de ouro: a captura nunca “fotografa” a imagem já filtrada na tela. Preview no lobby = sinal real da placa; na sessão é o mesmo caminho, sem recapturar o monitor.",
        40,
        H - 75,
        W - 80,
        11,
    )
    c.drawImage(ImageReader(str(arch)), 30, 40, width=W - 60, height=H - 140, preserveAspectRatio=True, mask="auto")
    c.showPage()


def page_session(c):
    fill_bg(c)
    draw_text(c, "Sessão ao vivo — God of War · 1440p60 · imagem em 100%", 40, H - 40, 18, True)
    draw_text(
        c,
        "DLSS 5 ligado  ·  filtro ~1,5–2 ms  ·  Frame Gen x4 (60→240)  ·  estável",
        40,
        H - 60,
        10,
        False,
        CYAN,
    )

    img = PRINT_SESSION if PRINT_SESSION.exists() else LIVE
    draw_image_fit(c, img, 30, 45, W * 0.62, H - 120)

    x = W * 0.66
    y = H - 105
    for title, sub in [
        ("DLSS 5", "Filtro na imagem em ~2 ms"),
        ("Captura HDMI", "Direto da placa, sem gambiarra"),
        ("Frame Gen x4", "60 → 240 fps (Lossless)"),
        ("Imagem 100%", "Sem baixar qualidade pra aliviar"),
        ("Estável", "Sem engasgo falso no HUD"),
    ]:
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y - 28, W - x - 28, 52, 10, fill=1, stroke=0)
        draw_text(c, title, x + 12, y + 4, 11, True, CYAN)
        draw_text(c, sub, x + 12, y - 14, 9, False, MUTED)
        y -= 66
    c.showPage()


def page_hud(c):
    fill_bg(c)
    draw_text(c, "Painel de métricas — fácil de ler", 40, H - 40, 18, True)
    wrap_draw(
        c,
        "Com DLSS 5 ligado, o filtro na imagem fica em cerca de 2 milissegundos "
        "(qualidade cheia). O HUD mostra isso ao vivo — FPS, Frame Gen, CPU/GPU.",
        40,
        H - 68,
        W - 80,
        11,
    )
    img = PRINT_HUD if PRINT_HUD.exists() else PRINT_SESSION
    draw_image_fit(c, img, 80, 50, W - 160, H - 160)
    draw_text(
        c,
        "Exemplo real: captura ~1 ms  ·  filtro DLSS 5 ~1,5–2 ms  ·  Frame Gen 60→240",
        40,
        28,
        9,
        False,
        MUTED,
    )
    c.showPage()


def page_compare_nr(c):
    """Side-by-side NR/DLSS5 ON vs OFF — user prints."""
    fill_bg(c)
    draw_text(c, "Com vs sem DLSS 5", 40, H - 40, 18, True)
    wrap_draw(
        c,
        "Mesma cena (God of War), 1440p60, qualidade cheia. Liga o DLSS 5 e a imagem melhora — "
        "sem precisar “filmar” a própria tela. O filtro continua rápido (~2 ms).",
        40,
        H - 68,
        W - 80,
        11,
    )
    mid = W / 2
    gap = 16
    box_w = mid - 40 - gap / 2
    box_h = H - 140
    if PRINT_NR_ON.exists():
        draw_text(c, "COM DLSS 5", 40, H - 95, 11, True, GREEN)
        draw_image_fit(c, PRINT_NR_ON, 40, 40, box_w, box_h - 20)
    if PRINT_NR_OFF.exists():
        draw_text(c, "SEM DLSS 5", mid + gap / 2, H - 95, 11, True, ACCENT)
        draw_image_fit(c, PRINT_NR_OFF, mid + gap / 2, 40, box_w, box_h - 20)
    c.showPage()


def page_latency(c):
    """Plain-language latency for non-technical audience."""
    fill_bg(c)
    draw_text(c, "Por que parece tão fluido", 40, H - 42, 20, True, GREEN)
    wrap_draw(
        c,
        "O HDMI vem da placa. O DLSS 5 entra só na hora de mostrar a imagem — "
        "rápido (~2 ms) e com qualidade cheia. Sem o atraso de recapturar a tela.",
        40,
        H - 72,
        W - 80,
        11,
    )

    cards = [
        ("DLSS 5 ligado", "~2 ms", "Tempo do filtro na imagem (qualidade 100%)."),
        ("Só a captura", "~1 ms", "Pegar o HDMI da placa até o app."),
        ("Um frame @60", "~16 ms", "Sobra folga enorme — por isso não engasga."),
    ]
    cw = (W - 80 - 24) / 3
    for i, (title, big, body) in enumerate(cards):
        x = 40 + i * (cw + 12)
        y = H / 2 - 20
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y, cw, 140, 12, fill=1, stroke=0)
        draw_text(c, title, x + 16, y + 108, 12, True, CYAN)
        draw_text(c, big, x + 16, y + 70, 22, True, GREEN)
        wrap_draw(c, body, x + 16, y + 45, cw - 32, 10, MUTED)

    if PRINT_LOBBY.exists():
        draw_image_fit(c, PRINT_LOBBY, 40, 36, W * 0.48, H / 2 - 90)
    if PRINT_SESSION.exists():
        draw_image_fit(c, PRINT_SESSION, W * 0.52, 36, W * 0.45, H / 2 - 90)
    c.showPage()


def page_product(c):
    fill_bg(c)
    draw_text(c, "O produto — o que o usuário sente", 40, H - 45, 20, True)
    cards = [
        ("Preview ao vivo", "HDMI no lobby com barras pretas. AO VIVO antes do Iniciar."),
        ("Ajustes na hora", "Mudou Frame Gen, escala, RenoDX ou preset → preview atualiza."),
        ("Sessão limpa", "Esc volta ao lobby. Painel compacto ou avançado com gráficos."),
        ("Escala 25–100%", "4K na placa, metade na GPU se precisar aliviar."),
        ("Filtro RenoDX", "Melhora a imagem na saída; Delete liga/desliga e lembra."),
        ("Frame Gen", "Opcional. Exige Lossless Scaling (Steam). Auto ou Pasta nos ajustes."),
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


def page_hotkeys(c):
    """Default shortcuts — plain language for non-dev audience."""
    fill_bg(c)
    draw_text(c, "Atalhos padrão (podem mudar nos ajustes)", 40, H - 42, 20, True)
    wrap_draw(
        c,
        "Na sessão e no lobby, teclas rápidas controlam Frame Gen, filtro e painéis. Tudo remapeável em Ajustes → Atalhos.",
        40,
        H - 70,
        W - 80,
        11,
    )

    keys = [
        ("Esc", "Volta ao lobby (não fecha o app de cara)"),
        ("Page Down", "Liga / desliga Frame Gen"),
        ("Page Up", "Cicla Frame Gen: x2 → x3 → x4"),
        ("Delete", "Liga / desliga o filtro RenoDX (Neural Rendering)"),
        ("Home", "Overlay RenoDX (presets / detalhes do filtro)"),
        ("F3", "Mostra / esconde o painel de métricas"),
        ("F4", "Analyze — grava prova de desempenho"),
        ("F11", "Tela cheia"),
        ("F12", "Abre o painel de ajustes (lobby)"),
    ]

    left_x, right_x = 48, W / 2 + 10
    col_w = W / 2 - 70
    y0 = H - 115
    row_h = 38
    for i, (key, desc) in enumerate(keys):
        col = 0 if i < 5 else 1
        row = i if i < 5 else i - 5
        x = left_x if col == 0 else right_x
        y = y0 - row * row_h
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(x, y - 8, col_w, 32, 8, fill=1, stroke=0)
        draw_text(c, key, x + 12, y + 2, 11, True, CYAN)
        draw_text(c, desc, x + 118, y + 2, 10, False, WHITE)

    c.setFillColorRGB(*(v / 255 for v in CARD))
    c.roundRect(40, 36, W - 80, 48, 10, fill=1, stroke=0)
    draw_text(
        c,
        "Dica: Frame Gen e filtro RenoDX são independentes — pode deixar o Frame Gen ligado e só desligar o filtro com Delete.",
        56,
        52,
        10,
        False,
        MUTED,
    )
    c.showPage()


def page_cpp(c):
    fill_bg(c)
    draw_text(c, "Por que C++ — e por que isso importa pra você", 40, H - 50, 20, True)
    wrap_draw(
        c,
        "O caminho do quadro (da placa até a tela) é nativo e previsível. Sem “pausas de limpeza de memória” no meio do jogo — por isso o preview acompanha os ajustes em tempo real e o ritmo fica estável.",
        40,
        H - 80,
        W - 80,
        11,
    )
    rows = [
        ("Resposta rápida", "Sem pausas automáticas de memória no caminho da imagem. Cada milissegundo conta."),
        ("Tudo no mesmo app", "Captura, desenho na tela, áudio e filtro visual juntos — sem gambiarra entre programas."),
        ("Preview vivo", "Mesmo sinal da placa no lobby; ajustes não precisam “aplicar e reabrir”."),
        ("Controle da imagem", "Conversão de cor, escala e saída pra tela sob nosso código."),
        ("Qualidade", "Testes automáticos, Analyze com log, painel de engasgos e ritmo de frames."),
    ]
    y = H - 130
    for title, body in rows:
        c.setFillColorRGB(*(v / 255 for v in CYAN))
        c.circle(52, y + 6, 5, fill=1, stroke=0)
        draw_text(c, title, 70, y, 12, True)
        draw_text(c, body, 200, y, 10, False, MUTED)
        y -= 42
    c.showPage()


def page_install(c):
    """Community install — requirements + step-by-step."""
    fill_bg(c)
    draw_text(c, "Instalacao — passo a passo", 40, H - 42, 22, True)
    wrap_draw(
        c,
        "Mande so o instalador NexusCap-Setup. Nao precisa pasta manual nem o codigo. Download na Release do GitHub.",
        40,
        H - 72,
        W - 80,
        11,
        WHITE,
    )

    # Requirements strip
    c.setFillColorRGB(*(x / 255 for x in CARD))
    c.roundRect(40, H - 200, W - 80, 95, 12, fill=1, stroke=0)
    draw_text(c, "Requisitos", 56, H - 125, 12, True, CYAN)
    reqs = [
        "Placa de captura HDMI (DirectShow)",
        "GPU NVIDIA (RenoDX / Neural Rendering)",
        "Windows 64-bit",
        "Frame Gen opcional: Lossless Scaling (Steam)",
    ]
    x0 = 56
    for i, t in enumerate(reqs):
        draw_text(c, "•  " + t, x0 + (i % 2) * ((W - 120) / 2), H - 150 - (i // 2) * 22, 10, False, WHITE)

    steps = [
        ("1", "Baixe NexusCap-Setup-2.4.0.exe na Release do GitHub"),
        ("2", "Execute o setup — instala em %LOCALAPPDATA%\\NexusCap (nao Program Files)"),
        ("3", "Abra Nexus Cap, escolha a placa e ajuste no lobby (preview ao vivo)"),
        ("4", "Marque RenoDX se quiser NR — na captura, Delete liga/desliga"),
        ("5", "Frame Gen (opcional): instale Lossless Scaling na Steam; Page Down liga/desliga"),
        ("6", "Iniciar — Esc volta ao lobby"),
    ]
    y = H - 230
    for num, text in steps:
        c.setFillColorRGB(*(v / 255 for v in CARD))
        c.roundRect(40, y - 14, W - 80, 36, 8, fill=1, stroke=0)
        c.setFillColorRGB(*(v / 255 for v in CYAN))
        c.circle(62, y + 4, 11, fill=1, stroke=0)
        draw_text(c, num, 62, y - 1, 11, True, BG, "center")
        draw_text(c, text, 88, y - 1, 11, False, WHITE)
        y -= 44

    c.setFillColorRGB(*(v / 255 for v in CARD))
    c.roundRect(40, 28, W - 80, 42, 10, fill=1, stroke=0)
    c.setStrokeColorRGB(*(v / 255 for v in ACCENT))
    c.setLineWidth(1.2)
    c.roundRect(40, 28, W - 80, 42, 10, fill=0, stroke=1)
    draw_text(
        c,
        "github.com/lealvjo/NexusCap/releases  ·  Setup ~142 MB  ·  Lossless Scaling NAO incluso",
        56,
        42,
        10,
        False,
        WHITE,
    )
    c.showPage()


def page_close(c):
    fill_bg(c)
    if LOGO.exists():
        c.drawImage(ImageReader(str(LOGO)), W / 2 - 40, H - 150, width=80, height=80, mask="auto")
    draw_text(c, "NexusCap", W / 2, H - 190, 28, True, WHITE, "center")
    draw_text(
        c,
        "HDMI nativo. Preview ao vivo. Frame Gen opcional (Lossless Scaling nao incluso).",
        W / 2,
        H - 220,
        12,
        False,
        MUTED,
        "center",
    )
    c.setFillColorRGB(*(v / 255 for v in CARD))
    c.roundRect(W / 2 - 250, H / 2 - 80, 500, 140, 14, fill=1, stroke=0)
    draw_text(c, "Download para a comunidade", W / 2, H / 2 + 35, 12, True, CYAN, "center")
    draw_text(c, "NexusCap-Setup-2.4.0.exe  (Release GitHub)", W / 2, H / 2 + 5, 12, False, WHITE, "center")
    draw_text(c, "github.com/lealvjo/NexusCap/releases/tag/v2.4.0", W / 2, H / 2 - 25, 11, False, MUTED, "center")
    draw_text(c, "Instala em %LOCALAPPDATA%\\NexusCap", W / 2, H / 2 - 50, 10, False, MUTED, "center")
    draw_text(c, "Feito em C++  ·  Windows 64-bit  ·  placa de captura + GPU NVIDIA", W / 2, 40, 9, False, MUTED, "center")
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
    page_lossless_req(c)
    page_install(c)
    page_arch(c, arch)
    page_session(c)
    page_hud(c)
    page_compare_nr(c)
    page_latency(c)
    page_product(c)
    page_hotkeys(c)
    page_cpp(c)
    page_close(c)
    c.save()
    print(OUT)


if __name__ == "__main__":
    main()
