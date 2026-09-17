#!/usr/bin/env python3
"""Pitch 3.0 escuro (tom do deck) — produto original + escada 1x/1,5x."""
from __future__ import annotations

from pathlib import Path

from PIL import Image as PILImage
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

ROOT = Path(__file__).resolve().parents[1]
PITCH = ROOT / "assets" / "pitch"
OUT = PITCH / "NexusCap_Pitch.pdf"
LOGO = ROOT / "assets" / "logo.png"

W, H = 960, 540
BG = (10 / 255, 12 / 255, 16 / 255)
INK = (0.92, 0.94, 0.97)
MUTED = (0.58, 0.64, 0.72)
ACCENT = (0.35, 0.75, 0.90)
GOOD = (0.40, 0.84, 0.58)
WARN = (1.0, 0.62, 0.35)
BAD = (1.0, 0.42, 0.42)
CARD = (16 / 255, 22 / 255, 32 / 255)


def fonts() -> tuple[str, str]:
    segoe = Path(r"C:\Windows\Fonts\segoeui.ttf")
    bold = Path(r"C:\Windows\Fonts\segoeuib.ttf")
    if segoe.is_file():
        pdfmetrics.registerFont(TTFont("Pitch", str(segoe)))
        pdfmetrics.registerFont(TTFont("PitchBold", str(bold if bold.is_file() else segoe)))
        return "Pitch", "PitchBold"
    return "Helvetica", "Helvetica-Bold"


def fill(c: canvas.Canvas) -> None:
    c.setFillColorRGB(*BG)
    c.rect(0, 0, W, H, fill=1, stroke=0)


def fit(c: canvas.Canvas, path: Path, x: float, y: float, w: float, h: float) -> None:
    if not path.is_file():
        return
    im = PILImage.open(path)
    iw, ih = im.size
    s = min(w / iw, h / ih)
    dw, dh = iw * s, ih * s
    c.drawImage(ImageReader(path), x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, preserveAspectRatio=True, mask="auto")


def accent_bar(c: canvas.Canvas) -> None:
    c.setFillColorRGB(*ACCENT)
    c.rect(0, 0, 5, H, fill=1, stroke=0)


def kicker(c: canvas.Canvas, regular: str, text: str) -> None:
    accent_bar(c)
    c.setFillColorRGB(*ACCENT)
    c.setFont(regular, 10)
    c.drawString(36, H - 26, text)


def title(c: canvas.Canvas, bold: str, text: str, y: float = H - 56) -> None:
    c.setFillColorRGB(*INK)
    c.setFont(bold, 24)
    c.drawString(36, y, text)


def card(c: canvas.Canvas, x: float, y: float, w: float, h: float) -> None:
    c.setFillColorRGB(*CARD)
    c.roundRect(x, y, w, h, 10, fill=1, stroke=0)


def body_lines(c: canvas.Canvas, regular: str, lines: list[str], y0: float, leading: float = 18) -> None:
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    y = y0
    for line in lines:
        c.drawString(36, y, line)
        y -= leading


def photo_slide(c, regular, bold, kicker_s, ttl, stats, note, img: Path, tag: str, tag_rgb) -> None:
    fill(c)
    kicker(c, regular, kicker_s)
    title(c, bold, ttl)
    c.setFillColorRGB(*tag_rgb)
    c.roundRect(W - 196, H - 52, 160, 24, 6, fill=1, stroke=0)
    c.setFillColorRGB(*BG)
    c.setFont(bold, 10)
    c.drawCentredString(W - 116, H - 44, tag)
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, stats)
    fit(c, img, 36, 52, W - 72, H - 150)
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 10)
    c.drawString(36, 28, note)
    c.showPage()


def main() -> None:
    regular, bold = fonts()
    c = canvas.Canvas(str(OUT), pagesize=(W, H))
    c.setTitle("Nexus Cap 3.0")
    c.setAuthor("NexusCap")
    scene = "NexusCap 3.0  ·  HDMI de qualquer consola  ·  1440p60  ·  VSYNC"

    # 1 cover — foco sem abrir o pipeline
    fill(c)
    accent_bar(c)
    if LOGO.is_file():
        fit(c, LOGO, 36, H - 92, 44, 44)
    kicker(c, regular, scene)
    c.setFillColorRGB(*INK)
    c.setFont(bold, 34)
    c.drawString(36, H - 148, "Nexus Cap")
    c.setFillColorRGB(*ACCENT)
    c.setFont(bold, 16)
    c.drawString(36, H - 176, "DLSS 5 desde a captura. Qualidade nativa. Ritmo de 60.")
    body_lines(
        c,
        regular,
        [
            "Sinal HDMI direto de qualquer consola — PS5, Xbox, Switch ou outro HDMI. Sem recapturar o PC.",
            "O DLSS 5 corre no Present, da consola ao monitor. A saída fica na resolução do sinal.",
            "Neste pitch as stills são de uma sessão PS5. O caminho é o mesmo em qualquer HDMI.",
        ],
        H - 208,
        16,
    )
    p15 = PITCH / "print_compare_15x.jpg"
    if p15.is_file():
        fit(c, p15, 36, 22, W - 72, 196)
    c.showPage()

    # 2 o foco (cards)
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "O foco")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    c.drawString(36, H - 80, "Performance sem largar qualidade. HDMI de qualquer consola. DLSS 5 no caminho real.")
    cards = [
        ("Qualquer consola", "PS5, Xbox, Switch, PC.\nHDMI da placa → NexusCap.\nNão é recaptura da mesa."),
        ("DLSS 5 no Present", "Enhance no frame que vai ao\nmonitor — da captura à saída,\nnuma só cadeia."),
        ("Res do sinal intacta", "1440p fica 1440p. A escala do\nFeed é tua e é à mão. Sem\ndownscale silencioso."),
        ("1,5x é o modo justo", "Mesma res, ~60 fps, imagem\nmais de jogo. 2x/3x existem,\nmas pesam e rendem pouco."),
    ]
    cw, ch, gap = 210, 168, 14
    y_card = 86
    for i, (head, copy) in enumerate(cards):
        x = 36 + i * (cw + gap)
        card(c, x, y_card, cw, ch)
        c.setFillColorRGB(*ACCENT)
        c.roundRect(x + 14, y_card + ch - 18, 22, 4, 2, fill=1, stroke=0)
        c.setFillColorRGB(*INK)
        c.setFont(bold, 13)
        c.drawString(x + 14, y_card + ch - 42, head)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 10)
        ty = y_card + ch - 64
        for line in copy.split("\n"):
            c.drawString(x + 14, ty, line)
            ty -= 14
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 9)
    c.drawString(36, 36, "Uma cadeia. Uma saída. HDMI de qualquer consola.")
    c.showPage()

    # 3 what + install
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "O que é")
    body_lines(
        c,
        regular,
        [
            "Lê o HDMI direto de qualquer consola (DirectShow), desenha em D3D11 e aplica enhance no Present.",
            "Não recaptura a mesa nem uma janela de jogo no PC — latência previsível,",
            "sem o hitch clássico de compositor + capturador.",
        ],
        H - 88,
        16,
    )
    title(c, bold, "Instalação", y=H - 160)
    steps = [
        "1.  NexusCap-Setup-3.0.0.exe na Release do GitHub.",
        "2.  Instala em %LOCALAPPDATA%\\NexusCap  (evita Program Files).",
        "3.  Abre, escolhe a placa, RenoDX se quiseres NR. Pass Count: 1,5x recomendado.",
        "4.  Frame Gen opcional: Lossless Scaling na Steam  ·  Page Down no CFG.",
        "5.  VSYNC ON  ·  Ctrl+Espaço = HUD  ·  reports bug/perf no CFG.",
    ]
    body_lines(c, regular, steps, H - 190, 20)
    c.showPage()

    # 3 architecture
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Arquitetura")
    body_lines(
        c,
        regular,
        [
            "Consola → placa HDMI → NexusCap. Present nativo (resolução do sinal). Escala do CFG só no Feed.",
            "NV12-first · sem fake no lobby. Quem quiser menos carga baixa a escala à mão.",
        ],
        H - 88,
        16,
    )
    arch = PITCH / "arch_diagram.png"
    if arch.is_file():
        fit(c, arch, 20, 36, W - 40, H - 150)
    c.showPage()

    # 4 lobby (print do utilizador)
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Lobby")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, "Preview ao vivo do HDMI da consola  ·  placa, modo e formato à esquerda  ·  sessão à direita")
    lobby = PITCH / "print_lobby_preview.jpg"
    if lobby.is_file():
        fit(c, lobby, 36, 36, W - 72, H - 130)
    c.showPage()

    # 5 CFG
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Menu de configuração")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, "A engrenagem CFG. Afinas o pipeline sem sair do preview.")
    cfg_copy = [
        ("Frame Gen", "Opcional. Lossless Scaling na Steam. Liga/desliga no CFG."),
        ("Escala do Feed", "Manual. Captura e saída ficam nativas. 100% é o default."),
        ("Neural Rendering", "RenoDX no preview. Presets Default / Natural / Cinematic."),
        ("Pass Count", "1x / 1,5x / 2x / 3x. O 1,5x é o recomendado: mesma res, ~60 fps."),
    ]
    y = H - 108
    for head, line in cfg_copy:
        card(c, 36, y - 44, 430, 50)
        c.setFillColorRGB(*ACCENT)
        c.setFont(bold, 11)
        c.drawString(50, y - 14, head)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 10)
        c.drawString(50, y - 32, line)
        y -= 58
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 11)
    c.drawString(36, 40, "1,5x no CFG. Escala à mão, se a pessoa quiser.")
    cfg = PITCH / "print_cfg.jpg"
    if cfg.is_file():
        fit(c, cfg, 490, 28, 434, H - 120)
    c.showPage()

    # prova PS5
    ps5 = PITCH / "print_ps5_home.jpg"
    if ps5.is_file():
        photo_slide(
            c,
            regular,
            bold,
            "HDMI de qualquer consola  ·  sessão PS5 neste pitch  ·  2560×1440 @ 60",
            "Sinal direto da consola",
            "UI da consola no mesmo caminho  ·  God of War Ragnarök  ·  1,5x  ·  ~60 fps",
            "Aqui um PS5 — o caminho é o mesmo em Xbox, Switch ou outro HDMI. Não é janela no PC.",
            ps5,
            "CONSOLA",
            ACCENT,
        )

    # 5–9 ladder photos
    k = "HDMI de qualquer consola  ·  sessão PS5  ·  God of War  ·  2560×1440 @ 60"
    slides = [
        (PITCH / "print_compare_nroff.jpg", "Sem DLSS / NR", "Captura nua · mesma resolução · o ponto de partida",
         "Aqui vê-se o HDMI sem Neural Rendering. É de onde se cresce.", "BASE", MUTED),
        (PITCH / "print_compare_1x.jpg", "1x  ·  NR ligado",
         "L600  59,9 fps   ·   present ~3,5 ms   ·   stalls 0   ·   1440p nativo",
         "Já limpa a imagem e mantém o ritmo do sinal.", "~60 FPS", GOOD),
        (PITCH / "print_compare_15x.jpg", "1,5x  ·  o modo justo",
         "L600  59,9 fps   ·   present ~7,3 ms   ·   stalls 0   ·   1440p nativo",
         "Mesma resolução, mesmo 60 fps — pele e cenário mais de jogo.", "RECOMENDADO", GOOD),
        (PITCH / "print_compare_2x.jpg", "2x  ·  mais pesado",
         "L600  40 fps   ·   present ~25 ms   ·   stalls 305   ·   1440p nativo",
         "Pouco ganho visual. Look mais lambido, e o 60 fps cai.", "~40 FPS", BAD),
        (PITCH / "print_compare_3x.jpg", "3x  ·  o teto",
         "L600  ~27 fps   ·   present ~33 ms   ·   stall storm   ·   1440p nativo",
         "Ainda mais processado. Não é o modo do dia a dia.", "PESADO", WARN),
    ]
    for img, ttl, stats, note, tag, rgb in slides:
        if img.is_file():
            photo_slide(c, regular, bold, k, ttl, stats, note, img, tag, rgb)

    # ladder overview
    fill(c)
    kicker(c, regular, k)
    title(c, bold, "A escada, com desempenho")
    thumbs = [
        (PITCH / "print_compare_nroff.jpg", "Sem NR", "partida"),
        (PITCH / "print_compare_1x.jpg", "1x", "59,9 fps"),
        (PITCH / "print_compare_15x.jpg", "1,5x", "59,9 fps"),
        (PITCH / "print_compare_2x.jpg", "2x", "40 fps"),
        (PITCH / "print_compare_3x.jpg", "3x", "~27 fps"),
    ]
    thumbs = [(p, a, b) for p, a, b in thumbs if p.is_file()]
    n = max(len(thumbs), 1)
    gap = 12
    tw = (W - 72 - gap * (n - 1)) / n
    y0 = 210
    for i, (img, label, perf) in enumerate(thumbs):
        x = 36 + i * (tw + gap)
        fit(c, img, x, y0, tw, 168)
        hi = label == "1,5x"
        c.setFillColorRGB(*(GOOD if hi else INK))
        c.setFont(bold, 11)
        c.drawString(x, y0 - 16, label)
        c.setFillColorRGB(*(GOOD if hi else MUTED))
        c.setFont(regular, 9)
        c.drawString(x, y0 - 30, perf)
    body_lines(
        c,
        regular,
        [
            "1x e 1,5x: mesma res, mesmo ~60 fps. O salto visível está no 1,5x.",
            "2x e 3x: mais custo, pouco a ganhar, imagem mais descaracterizada.",
        ],
        150,
        16,
    )
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 13)
    c.drawString(36, 96, "Usa o 1,5x. Escala do Feed é manual, se a pessoa quiser.")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 9)
    c.drawString(36, 28, "Sessão real · HDMI de consola (aqui PS5) · VSYNC · FG off · escala 100%.  Nexus Cap v3.0.0")
    c.showPage()

    # latency + vs x2
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Latência e vs X2")
    rows = [
        ("present + NR  1x", "~3,5 ms", "cabe @60"),
        ("present + NR  1,5x", "~7,3 ms", "cabe @60  ·  recomendado"),
        ("present + NR  2x", "~25 ms", "~40 fps"),
        ("cap  (captura pura)", "~1 ms", "sem o hook NR"),
    ]
    y = H - 100
    c.setFont(bold, 11)
    for label, val, note in rows:
        c.setFillColorRGB(*CARD)
        c.roundRect(36, y - 8, W - 72, 36, 8, fill=1, stroke=0)
        c.setFillColorRGB(*INK)
        c.setFont(regular, 12)
        c.drawString(48, y + 4, label)
        c.setFillColorRGB(*GOOD if "1,5x" in label or "1x" in label and "2x" not in label else MUTED)
        if "2x" in label:
            c.setFillColorRGB(*BAD)
        c.setFont(bold, 12)
        c.drawString(280, y + 4, val)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 11)
        c.drawString(420, y + 4, note)
        y -= 44
    body_lines(
        c,
        regular,
        [
            "X2: feeling forte, mas stalls em lote no Present/feeder.",
            "NexusCap: enhance no Present, VSYNC nosso, telemetria no HUD.",
            "No 3.0 o 1,5x é onde a imagem cresce sem largar os 60 fps.",
            "Requisitos: qualquer consola HDMI · placa DirectShow · GPU NVIDIA · Windows 64-bit.",
        ],
        y - 8,
        16,
    )
    c.showPage()
    c.save()
    print("wrote", OUT, OUT.stat().st_size)


if __name__ == "__main__":
    main()
