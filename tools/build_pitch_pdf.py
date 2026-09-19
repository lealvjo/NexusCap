#!/usr/bin/env python3
"""Pitch 3.0 escuro (tom do deck): produto original + escada 1x/1,5x."""
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
    c.setTitle("Nexus Cap 3.1")
    c.setAuthor("NexusCap")
    scene = "NexusCap 3.1  ·  HDMI de qualquer consola  ·  1440p60  ·  VSYNC"

    # 1 cover: foco sem abrir o pipeline
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
            "Sinal HDMI direto de qualquer consola: PS5, Xbox, Switch ou outro HDMI. Sem recapturar o PC.",
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
        ("DLSS 5 no Present", "Enhance no frame que vai ao\nmonitor, da captura à saída,\nnuma só cadeia."),
        ("Res do sinal intacta", "Captura e saída nativas. Se pesar,\nbaixas a escala do DLSS ou o\nmodo da placa, à mão."),
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
            "Não recaptura a mesa nem uma janela de jogo no PC. Latência previsível,",
            "sem o hitch clássico de compositor + capturador.",
        ],
        H - 88,
        16,
    )
    title(c, bold, "Instalação", y=H - 160)
    steps = [
        "1.  NexusCap-Setup-3.1.7.exe na Release do GitHub.",
        "2.  Instala em %LOCALAPPDATA%\\NexusCap  — pasta da tua conta, sem administrador.",
        "3.  Abre, escolhe a placa, RenoDX se quiseres NR. Pass Count: 1,5x recomendado.",
        "4.  Frame Gen: se o Lossless Scaling estiver na Steam, o NexusCap liga até 4x sozinho.",
        "5.  VSYNC ON  ·  Ctrl+Espaço = HUD  ·  reports bug/perf no CFG.",
    ]
    body_lines(c, regular, steps, H - 190, 20)
    c.showPage()

    # Instalador menos invasivo (sem UAC de propósito)
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Instalador menos invasivo")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    c.drawString(36, H - 80, "O setup não pede admin. Não é menos seguro — é menos poder no sistema.")
    inv = [
        ("Pasta do utilizador", "%LOCALAPPDATA%\\NexusCap.\nComo Steam, Discord,\nSpotify: a tua conta."),
        ("Sem aviso de admin", "Sem o UAC “permitir\nalterações no dispositivo”.\nO setup não mexe no PC."),
        ("Menos invasivo", "Pedir admin só para parecer\n“oficial” seria pior: poder\nno sistema inteiro, à toa."),
        ("RenoDX precisa gravar", "ini / cfg / cache ao lado\ndo exe. Program Files\nbloqueia a escrita."),
    ]
    cw, ch, gap = 210, 188, 14
    y_card = 72
    for i, (head, copy) in enumerate(inv):
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
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 11)
    c.drawString(36, 36, "Um exe. Sem pasta manual. Sem mexer no sistema. Instala e usa.")
    c.showPage()

    # 3 architecture
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Arquitetura")
    body_lines(
        c,
        regular,
        [
            "Consola → placa HDMI → NexusCap. Present nativo. Escala do CFG = trabalho do DLSS, à mão.",
            "Modo da placa = o que o HDMI anuncia (4K30, 1440p60…). Sem downscale silencioso.",
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

    # A imagem na tua frente (preview, Present, OBS) — sem nomear outros apps
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "A imagem na tua frente")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    c.drawString(36, H - 80, "Vês o HDMI direto. O que está no ecran é o frame. O OBS pega o mesmo Present.")
    vis = [
        ("Lobby ao vivo", "Preview HDMI antes de Iniciar.\nVês o sinal da consola,\nnão um recorte da mesa."),
        ("Captura no Present", "O monitor mostra o frame real.\nDLSS 5 corre nesse Present,\nna resolução do sinal."),
        ("OBS vê o mesmo", "Game Capture pega o swapchain.\nHUD e mini overlay queimados\nna imagem — o OBS vê igual."),
        ("1x nativo e leve", "100% · ~59,9 fps · stalls 0.\nFeed ~1 ms. VSYNC no Hz.\nRitmo estável, imagem nítida."),
    ]
    cw, ch, gap = 210, 188, 14
    y_card = 72
    for i, (head, copy) in enumerate(vis):
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
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 11)
    c.drawString(36, 36, "Sem recapturar a mesa. A imagem que vês é a que sai.")
    c.showPage()

    # 5 CFG
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Menu de configuração")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, "A engrenagem CFG. Afinas o pipeline sem sair do preview.")
    cfg_copy = [
        ("Frame Gen", "Se tiveres Lossless Scaling, liga sozinho. Até 4x."),
        ("Escala do Feed", "Res em que o DLSS trabalha. Se pesar, baixa à mão."),
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
    c.drawString(36, 40, "1,5x no CFG. Se pesar: escala do DLSS ou modo da placa.")
    cfg = PITCH / "print_cfg.jpg"
    if cfg.is_file():
        fit(c, cfg, 490, 28, 434, H - 120)
    c.showPage()

    # Se ficar pesado: escala DLSS + modo da placa
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Se um modo ficar pesado")
    body_lines(
        c,
        regular,
        [
            "Dois controlos, os dois à mão. A gente não baixa a resolução por baixo dos panos.",
            "1.  Escala do Feed: a resolução em que o DLSS 5 trabalha. Captura e saída ficam nativas.",
            "2.  Modo da placa: o HDMI que a tua captura anuncia. Cada placa tem o teto dela.",
            "Neste pitch: 2560×1440 @ 60. A placa chega a 4K30; 2K60 é a escolha para o ritmo.",
        ],
        H - 88,
        17,
    )
    card(c, 36, 56, 430, 210)
    card(c, 494, 56, 430, 210)
    c.setFillColorRGB(*ACCENT)
    c.setFont(bold, 12)
    c.drawString(56, 240, "Modo da placa")
    c.drawString(514, 240, "Escala do DLSS (Feed)")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 10)
    c.drawString(56, 220, "O que a captura HDMI aguenta.")
    c.drawString(514, 220, "Onde o Neural Rendering trabalha.")
    mode = PITCH / "print_ui_mode.jpg"
    scale = PITCH / "print_ui_scale.jpg"
    if mode.is_file():
        fit(c, mode, 56, 88, 390, 110)
    if scale.is_file():
        fit(c, scale, 514, 88, 390, 110)
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 10)
    c.drawString(56, 70, "Aqui: 2K60  ·  a placa vai até 4K30")
    c.drawString(514, 70, "100% default  ·  baixa se 2x/3x pesar")
    c.showPage()

    # Frame Gen: copy + overlay
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Frame Gen até 4x")
    body_lines(
        c,
        regular,
        [
            "Se a pessoa tiver o Lossless Scaling na Steam, o NexusCap integra automaticamente.",
            "Geração de frames até 4x no Present, sem recapturar a mesa, sem segundo capturador.",
            "Otimizações nossas para latência e ghosting, embutidas no perfil que gravamos.",
            "O ponto crítico: menos ghost na mira e no HUD, não o default genérico da Steam.",
            "Demo: DLSS 5 em 1x  +  FG x4  ·  captura ~60 fps  ·  saída ~240.",
        ],
        H - 88,
        17,
    )
    hud = PITCH / "print_fg4x_hud.jpg"
    if hud.is_file():
        c.setFillColorRGB(*GOOD)
        c.setFont(bold, 10)
        c.drawString(36, 218, "Overlay ao vivo  ·  cap|FG x4 ON  ·  DLSS5 1x  ·  1440p nativo")
        fit(c, hud, 36, 40, W - 72, 168)
    c.showPage()

    if (PITCH / "print_fg4x.jpg").is_file():
        photo_slide(
            c,
            regular,
            bold,
            "HDMI de qualquer consola  ·  sessão PS5  ·  DLSS 5 1x  ·  FG x4",
            "Demo: 1x + FG 4x",
            "cap ~60 fps  ·  FG ~240 fps  ·  LSFG3  ·  saída 1:1 nativa",
            "Lossless Scaling entra sozinho. Mira e HUD com menos ghost. Perfil nosso.",
            PITCH / "print_fg4x.jpg",
            "FG x4",
            ACCENT,
        )

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
            "Aqui um PS5. O caminho é o mesmo em Xbox, Switch ou outro HDMI. Não é janela no PC.",
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
         "Mesma resolução, mesmo 60 fps. Pele e cenário mais de jogo.", "RECOMENDADO", GOOD),
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
            "2x e 3x: mais custo. Se pesar, baixa a escala do DLSS ou o modo da placa.",
        ],
        150,
        16,
    )
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 13)
    c.drawString(36, 96, "Usa o 1,5x. Se pesar: escala do Feed ou modo HDMI, à mão.")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 9)
    c.drawString(36, 28, "Sessão real · HDMI de consola (aqui PS5) · VSYNC · FG off · escala 100%.  Nexus Cap v3.1.0")
    c.showPage()

    # latency + vs x2
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Latência e vs X2")
    rows = [
        ("present + NR  1x", "~3,5 ms", "cabe @60"),
        ("present + NR  1,5x", "~7,3 ms", "cabe @60  ·  recomendado"),
        ("present + NR  2x", "~25 ms", "~40 fps"),
        ("present + NR  3x", "~33 ms", "~27 fps"),
        ("cap  (captura pura)", "~1 ms", "sem o hook NR"),
    ]
    y = H - 96
    c.setFont(bold, 11)
    for label, val, note in rows:
        c.setFillColorRGB(*CARD)
        c.roundRect(36, y - 8, W - 72, 34, 8, fill=1, stroke=0)
        c.setFillColorRGB(*INK)
        c.setFont(regular, 12)
        c.drawString(48, y + 4, label)
        if "2x" in label or "3x" in label:
            c.setFillColorRGB(*BAD)
        elif "1,5x" in label or "1x" in label:
            c.setFillColorRGB(*GOOD)
        else:
            c.setFillColorRGB(*MUTED)
        c.setFont(bold, 12)
        c.drawString(280, y + 4, val)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 11)
        c.drawString(420, y + 4, note)
        y -= 40
    body_lines(
        c,
        regular,
        [
            "X2: feeling forte, mas stalls em lote no Present/feeder.",
            "NexusCap: enhance no Present, VSYNC nosso, telemetria no HUD.",
            "No 3.1 o 1,5x é onde a imagem cresce sem largar os 60 fps.",
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
