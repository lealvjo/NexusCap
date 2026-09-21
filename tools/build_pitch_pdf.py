#!/usr/bin/env python3
"""Pitch 4.0 escuro: produto + escada 1x / 1,5x / 2x / 2x otimizado / 3x."""
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
    c.setTitle("Nexus Cap 4.1")
    c.setAuthor("NexusCap")
    scene = "NexusCap 4.1  ·  HDMI de qualquer console  ·  1440p60  ·  VSYNC"

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
            "Sinal HDMI direto de qualquer console: PS5, Xbox, Switch ou outro HDMI. Sem recapturar o PC.",
            "Ainda não tem placa de captura? O Benchmark valida como o NexusCap corre no seu PC.",
            "O DLSS 5 vai no Present, da captura ao monitor. A saída fica na resolução do sinal.",
        ],
        H - 208,
        16,
    )
    cover = PITCH / "print_cover.jpg"
    if cover.is_file():
        fit(c, cover, 36, 22, W - 72, 196)
    c.showPage()

    # 2 o foco (cards)
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "O foco")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    c.drawString(36, H - 80, "Performance sem largar qualidade. HDMI de qualquer console. DLSS 5 no caminho real.")
    cards = [
        ("Qualquer console", "PS5, Xbox, Switch, PC.\nHDMI da placa → NexusCap.\nNão é recaptura da mesa."),
        ("DLSS 5 no Present", "Enhance no frame que vai ao\nmonitor, da captura à saída,\nnuma só cadeia."),
        ("Res do sinal intacta", "Captura e saída nativas. Se pesar,\nbaixa a escala do DLSS ou o\nmodo da placa, na mão."),
        ("1,5x é o modo justo", "Mesma res, ~60 fps. Se for\na 2x, usa o otimizado: menos\nflicker e cintilação."),
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
    c.drawString(36, 36, "Uma cadeia. Uma saída. HDMI de qualquer console.")
    c.showPage()

    # 3 what + install
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "O que é")
    body_lines(
        c,
        regular,
        [
            "Lê o HDMI direto de qualquer console (DirectShow), desenha em D3D11 e aplica enhance no Present.",
            "Não recaptura a mesa nem uma janela de jogo no PC. Latência previsível,",
            "sem o hitch clássico de compositor + capturador.",
        ],
        H - 88,
        16,
    )
    title(c, bold, "Instalação", y=H - 160)
    steps = [
        "1.  NexusCap-Setup-4.1.2.exe na Release, ou o zip portable (extrai e roda).",
        "2.  Instala em %LOCALAPPDATA%\\NexusCap, pasta da sua conta, sem administrador.",
        "3.  Abre, escolhe a placa, RenoDX se quiser NR. Pass Count: 1,5x recomendado.",
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
    c.drawString(36, H - 80, "O setup não pede admin. Não é menos seguro. É menos poder no sistema.")
    inv = [
        ("Pasta do usuário", "%LOCALAPPDATA%\\NexusCap.\nComo Steam, Discord,\nSpotify: a sua conta."),
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
            "Tem placa: console, HDMI, NexusCap, RenoDX, monitor. Sem recapturar a mesa.",
            "Ainda não tem placa? O Benchmark corre no seu PC e diz como o NexusCap vai aguentar.",
            "4.0: 1,5x, 2x otimizado, overlay nosso, aviso se os motores saírem do stock.",
        ],
        H - 88,
        16,
    )
    arch = PITCH / "arch_diagram.png"
    if arch.is_file():
        fit(c, arch, 20, 36, W - 40, H - 150)
    c.showPage()

    # 4 lobby (print do usuario)
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Lobby")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, "Preview ao vivo do HDMI do console. Placa, modo e formato à esquerda, sessão à direita.")
    lobby = PITCH / "print_lobby_preview.jpg"
    if lobby.is_file():
        fit(c, lobby, 36, 36, W - 72, H - 130)
    c.showPage()

    # A imagem na sua frente (preview, Present, OBS) — sem nomear outros apps
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "A imagem na sua frente")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    c.drawString(36, H - 80, "Você vê o HDMI direto. O que está na tela é o frame. O OBS pega o mesmo Present.")
    vis = [
        ("Lobby ao vivo", "Preview HDMI antes de Iniciar.\nVocê vê o sinal do console,\nnão um recorte da mesa."),
        ("Captura no Present", "O monitor mostra o frame real.\nDLSS 5 corre nesse Present,\nna resolução do sinal."),
        ("OBS vê o mesmo", "Game Capture pega o swapchain.\nHUD e mini overlay queimados\nna imagem. O OBS vê igual."),
        ("Overlay nosso", "FPS, frametime, GPU e STABLE\nno Present. Sem Afterburner,\nsem app de terceiros."),
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
    c.drawString(36, 36, "Sem recapturar a mesa. A imagem que você vê é a que sai.")
    c.showPage()

    # 5 CFG
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Menu de configuração")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, "A engrenagem CFG. Pipeline, overlay e reports sem sair do preview.")
    cfg_copy = [
        ("Overlay Nexus Cap", "HUD próprio: FPS, frametime, GPU, STABLE. Sem Afterburner."),
        ("Frame Gen", "Se tiver Lossless Scaling, liga sozinho. Até 4x."),
        ("Escala e Pass", "Onde o DLSS trabalha. 1,5x é o modo justo. 2x usa o otimizado."),
        ("Neural Rendering", "RenoDX no Present. Presets Default, Natural, Cinematic."),
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
    c.drawString(36, 40, "Ctrl+Espaço mostra o overlay. É nosso. Não precisa de Afterburner.")
    cfg = PITCH / "print_cfg.jpg"
    if cfg.is_file():
        fit(c, cfg, 490, 28, 434, H - 120)
    c.showPage()

    # Presets 4.1 — um jogo, um look, um arquivo
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Um jogo. Um preset.")
    body_lines(
        c,
        regular,
        [
            "Montou o TLOU em 2x otimizado, cinema, FG x3. Salva. Acabou.",
            "Amanhã aplica. O amigo manda o arquivo. Você importa.",
            "Ctrl+M em cima do HDMI. F12 no lobby. A mesma lista.",
        ],
        H - 88,
        17,
    )
    preset_img = PITCH / "print_preset.jpg"
    if not preset_img.is_file():
        preset_img = PITCH / "print_preset.png"
    if preset_img.is_file():
        fit(c, preset_img, 36, 44, W - 72, H - 176)
    else:
        bits = [
            ("Salvar", "O look deste jogo,\ncom um nome."),
            ("Aplicar", "Pass, cena, FG, escala.\nDevolve tudo."),
            ("Compartilhar", "Um arquivo\n.nexuscap-preset."),
            ("Importar", "Entra na lista.\nAplicar é você."),
        ]
        cw, ch, gap = 210, 168, 14
        y_card = 56
        for i, (head, copy) in enumerate(bits):
            x = 36 + i * (cw + gap)
            card(c, x, y_card, cw, ch)
            c.setFillColorRGB(*ACCENT)
            c.roundRect(x + 14, y_card + ch - 18, 22, 4, 2, fill=1, stroke=0)
            c.setFillColorRGB(*INK)
            c.setFont(bold, 14)
            c.drawString(x + 14, y_card + ch - 48, head)
            c.setFillColorRGB(*MUTED)
            c.setFont(regular, 11)
            ty = y_card + ch - 74
            for line in copy.split("\n"):
                c.drawString(x + 14, ty, line)
                ty -= 16
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 11)
    c.drawString(36, 22, "Benchmark não mexe nisso. É o teu HDMI.")
    c.showPage()

    # Se ficar pesado: escala DLSS + modo da placa
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Se um modo ficar pesado")
    body_lines(
        c,
        regular,
        [
            "Dois controles, os dois na mão. A gente não baixa a resolução por baixo dos panos.",
            "1.  Escala do Feed: a resolução em que o DLSS 5 trabalha. Captura e saída ficam nativas.",
            "2.  Modo da placa: o HDMI que a sua captura anuncia. Cada placa tem o teto dela.",
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
            "HDMI de qualquer console  ·  sessão PS5  ·  DLSS 5 1x  ·  FG x4",
            "Demo: 1x + FG 4x",
            "cap ~60 fps  ·  FG ~240 fps  ·  LSFG3  ·  saída 1:1 nativa",
            "Lossless Scaling entra sozinho. Mira e HUD com menos ghost. Perfil nosso.",
            PITCH / "print_fg4x.jpg",
            "FG x4",
            ACCENT,
        )

    # Benchmark, sessao dedicada
    bk = "NexusCap 4.0, Benchmark, sem placa de captura, 40 s"
    fill(c)
    kicker(c, regular, bk)
    title(c, bold, "Ainda sem placa de captura? Valida no seu PC")
    body_lines(
        c,
        regular,
        [
            "Ainda não tem placa HDMI. Mesmo assim dá para ver como o NexusCap corre na sua máquina.",
            "O Benchmark não precisa do console nem da captura. Usa o seu PC, o Present real e um clipe.",
            "40 segundos de The Last of Us Parte II. Carga de jogo, não um teste sintético.",
            "A nota é do seu PC. Quando tiver placa, o HDMI entra no mesmo caminho.",
        ],
        H - 88,
        16,
    )
    menu = PITCH / "print_bench_menu.jpg"
    if menu.is_file():
        fit(c, menu, 520, 36, 404, H - 130)
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 11)
    c.drawString(36, 48, "Iniciar benchmark. Pass, 1080/1440/4K, 30 ou 60 Hz, FG até 4x.")
    c.showPage()

    fill(c)
    kicker(c, regular, bk)
    title(c, bold, "Como a nota é feita")
    body_lines(
        c,
        regular,
        [
            "Não é um chute nem um score genérico. É um modelo nosso.",
            "Uma equação com fps, present, latência, stalls, escala do Feed e Frame Gen.",
            "Em cima do número o NexusCap monta uma sugestão inteligente: que escala baixar,",
            "se o FG ajuda ou atrapalha, se a latência extra ainda dá para jogar.",
            "Parece IA porque é um modelo a ler a sua amostra e a escrever o próximo passo.",
            "Bom, Jogável com FG ou Não aguenta. Cada cartão diz o que mexer.",
        ],
        H - 88,
        17,
    )
    why_m = [
        ("Equação", "fps, present, latência,\nstalls, escala e FG.\nContas reais."),
        ("Sugestão", "O modelo diz se o FG\nvale a latência extra.\nEscala, Pass ou FG."),
        ("Do seu PC", "A nota é desta máquina.\nO CFG HDMI fica como\nestava."),
        ("40 segundos", "The Last of Us Parte II.\nRelógio só conta com\nNR e FG já ligados."),
    ]
    cw, ch, gap = 210, 118, 14
    for i, (head, copy) in enumerate(why_m):
        x = 36 + i * (cw + gap)
        card(c, x, 48, cw, ch)
        c.setFillColorRGB(*ACCENT)
        c.roundRect(x + 12, 152, 18, 3, 2, fill=1, stroke=0)
        c.setFillColorRGB(*INK)
        c.setFont(bold, 12)
        c.drawString(x + 12, 132, head)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 10)
        ty = 112
        for line in copy.split("\n"):
            c.drawString(x + 12, ty, line)
            ty -= 14
    c.showPage()

    run = PITCH / "print_bench_run.jpg"
    if run.is_file():
        photo_slide(
            c,
            regular,
            bold,
            bk,
            "40 segundos de The Last of Us Parte II",
            "O relógio só conta quando NR e FG já ligaram. Overlay nosso no topo.",
            "O vídeo é só para validar a carga. As imagens podem parecer um pouco menos nítidas. O resultado é real.",
            run,
            "40 S",
            ACCENT,
        )

    fill(c)
    kicker(c, regular, bk)
    title(c, bold, "A nota e as sugestões")
    body_lines(
        c,
        regular,
        [
            "O modelo valida fps e latência. Cada cartão diz o que mexer: escala, Pass ou Frame Gen.",
            "Se o FG liga, a sugestão olha a latência extra. Dá para jogar ou é melhor baixar a escala.",
        ],
        H - 80,
        15,
    )
    cards_img = [
        (PITCH / "print_bench_ok.jpg", "Bom", GOOD),
        (PITCH / "print_bench_playable.jpg", "Jogável com FG", WARN),
        (PITCH / "print_bench_fail.jpg", "Não aguenta", BAD),
        (PITCH / "print_bench_fg.jpg", "Latência do FG", ACCENT),
    ]
    cards_img = [(p, a, rgb) for p, a, rgb in cards_img if p.is_file()]
    cw, ch, gap = 430, 152, 14
    for i, (img, label, rgb) in enumerate(cards_img):
        col = i % 2
        row = i // 2
        x = 36 + col * (cw + gap)
        y = 236 - row * (ch + 16)
        card(c, x, y, cw, ch)
        c.setFillColorRGB(*rgb)
        c.setFont(bold, 10)
        c.drawString(x + 12, y + ch - 18, label)
        fit(c, img, x + 12, y + 10, cw - 24, ch - 36)
    c.showPage()

    fill(c)
    kicker(c, regular, bk)
    title(c, bold, "A mesma análise na gameplay real")
    body_lines(
        c,
        regular,
        [
            "As sugestões não ficam só no clipe. Quer a nota depois do seu HDMI?",
            "Na coluna SESSÃO marca Reportar performance. No fim da captura abre o mesmo modelo.",
            "Escalas, Pass, FG e latência: o que o seu PC aguentou de verdade, não um chute.",
        ],
        H - 88,
        17,
    )
    check = PITCH / "print_bench_hdmi_check.jpg"
    card(c, 36, 56, W - 72, 210)
    c.setFillColorRGB(*ACCENT)
    c.setFont(bold, 13)
    c.drawString(56, 236, "Reportar performance")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(56, 214, "No fim da captura abre a análise. O mesmo modelo do Benchmark.")
    if check.is_file():
        fit(c, check, 56, 78, 420, 120)
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 12)
    c.drawString(500, 120, "Marca a caixa. Joga. Recebe")
    c.drawString(500, 100, "a sugestão no fim da sessão.")
    c.showPage()

    # prova PS5
    ps5 = PITCH / "print_ps5_home.jpg"
    if ps5.is_file():
        photo_slide(
            c,
            regular,
            bold,
            "HDMI de qualquer console  ·  sessão PS5 neste pitch  ·  2560×1440 @ 60",
            "Sinal direto do console",
            "UI do console no mesmo caminho. God of War Ragnarok, 1,5x, cerca de 60 fps.",
            "Aqui um PS5. O caminho é o mesmo em Xbox, Switch ou outro HDMI. Não é janela no PC.",
            ps5,
            "CONSOLE",
            ACCENT,
        )

    # 5–9 ladder photos
    k = "HDMI de qualquer console, sessão PS5, God of War, 2560x1440 a 60"
    slides = [
        (PITCH / "print_compare_nroff.jpg", "Sem DLSS / NR",
         "L600  60,1 fps   ·   present 2,1 ms   ·   STABLE   ·   1440p nativo",
         "Aqui se vê o HDMI sem Neural Rendering. É de onde se cresce.", "BASE", MUTED),
        (PITCH / "print_compare_1x.jpg", "1x  ·  NR ligado",
         "L600  59,8 fps   ·   present 1,4 ms   ·   STABLE   ·   1440p nativo",
         "Já limpa a imagem e mantém o ritmo do sinal.", "~60 FPS", GOOD),
        (PITCH / "print_compare_15x.jpg", "1,5x  ·  o modo justo",
         "L600  60,1 fps   ·   present 4,4 ms   ·   STABLE   ·   1440p nativo",
         "Mesma resolução, mesmo 60 fps. Pele e cenário mais de jogo.", "RECOMENDADO", GOOD),
        (PITCH / "print_compare_2x.jpg", "2x  ·  default",
         "L600  40,0 fps   ·   present 20,8 ms   ·   UNSTABLE   ·   1440p nativo",
         "Os mesmos 2 evaluates. Mais flicker, cintilação e ghost na UI/HUD.", "~40 FPS", BAD),
        (PITCH / "print_compare_2xopt.jpg", "2x otimizado",
         "L600  40,0 fps   ·   FG x3   ·   PLAYABLE FG   ·   1440p nativo",
         "Mesmo custo. Gancho de cor/UI estável: menos flicker, menos cintilação, imagem mais limpa.",
         "MENOS FLICKER", GOOD),
        (PITCH / "print_compare_3x.jpg", "3x  ·  o teto",
         "L600  29,8 fps   ·   present 28,7 ms   ·   UNSTABLE   ·   1440p nativo",
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
        (PITCH / "print_compare_nroff.jpg", "Sem NR", "60,1 fps"),
        (PITCH / "print_compare_1x.jpg", "1x", "59,8 fps"),
        (PITCH / "print_compare_15x.jpg", "1,5x", "60,1 fps"),
        (PITCH / "print_compare_2x.jpg", "2x", "40 fps"),
        (PITCH / "print_compare_2xopt.jpg", "2x opt", "40 fps"),
        (PITCH / "print_compare_3x.jpg", "3x", "29,8 fps"),
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
            "2x default treme (flicker e cintilação). 2x otimizado: mesmo ~40 fps, imagem estável.",
        ],
        150,
        16,
    )
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 13)
    c.drawString(36, 96, "Usa o 1,5x. Se pesar: escala do Feed ou modo HDMI, na mão.")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 9)
    c.drawString(36, 28, "Sessão real · HDMI de console (aqui PS5) · VSYNC · FG off · escala 100%.  Nexus Cap v4.1.2")
    c.showPage()

    # 2x default vs 2x otimizado
    fill(c)
    kicker(c, regular, k)
    title(c, bold, "Por que o 2x otimizado")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 11)
    c.drawString(36, H - 78, "2x de verdade: os mesmos dois evaluates, o mesmo ~40 fps. O que muda é o gancho de cor e UI.")
    left = PITCH / "print_compare_2x.jpg"
    right = PITCH / "print_compare_2xopt.jpg"
    if left.is_file():
        fit(c, left, 36, 248, 430, 186)
    if right.is_file():
        fit(c, right, 494, 248, 430, 186)
    c.setFillColorRGB(*BAD)
    c.setFont(bold, 11)
    c.drawString(36, 232, "2x default  ·  treme")
    c.setFillColorRGB(*GOOD)
    c.drawString(494, 232, "2x otimizado  ·  estável")
    why = [
        ("Menos flicker", "O default lampeja na tela.\nO otimizado segura o quadro."),
        ("Menos cintilação", "Areia, neve e bordas finas\nparam de granilhar."),
        ("Menos ghost na UI", "HUD, mira e texto não\narrastam nem piscam."),
        ("Melhor imagem", "Pele e cenário mais limpos,\nsem pagar fps a mais."),
    ]
    cw, ch, gap = 210, 86, 14
    for i, (head, copy) in enumerate(why):
        x = 36 + i * (cw + gap)
        card(c, x, 36, cw, ch)
        c.setFillColorRGB(*ACCENT)
        c.roundRect(x + 12, 110, 18, 3, 2, fill=1, stroke=0)
        c.setFillColorRGB(*INK)
        c.setFont(bold, 11)
        c.drawString(x + 12, 92, head)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 9)
        ty = 74
        for line in copy.split("\n"):
            c.drawString(x + 12, ty, line)
            ty -= 12
    c.showPage()

    # latência — sem comparar com outro app
    fill(c)
    kicker(c, regular, scene)
    title(c, bold, "Latência do Present")
    rows = [
        ("present + NR  1x", "1,4 ms", "cabe @60"),
        ("present + NR  1,5x", "4,4 ms", "cabe @60  ·  recomendado"),
        ("present + NR  2x default", "20,8 ms", "~40 fps  ·  flicker"),
        ("present + NR  2x otimizado", "20,5 ms", "~40 fps  ·  menos cintilação"),
        ("present + NR  3x", "28,7 ms", "~30 fps"),
        ("cap  (captura pura)", "~1 ms", "sem o hook NR"),
    ]
    y = H - 96
    for label, val, note in rows:
        c.setFillColorRGB(*CARD)
        c.roundRect(36, y - 8, W - 72, 34, 8, fill=1, stroke=0)
        c.setFillColorRGB(*INK)
        c.setFont(regular, 12)
        c.drawString(48, y + 4, label)
        if "3x" in label or "default" in label:
            c.setFillColorRGB(*BAD)
        elif "otimizado" in label or "1,5x" in label or "1x" in label:
            c.setFillColorRGB(*GOOD)
        else:
            c.setFillColorRGB(*MUTED)
        c.setFont(bold, 12)
        c.drawString(300, y + 4, val)
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 11)
        c.drawString(430, y + 4, note)
        y -= 40
    body_lines(
        c,
        regular,
        [
            "Enhance no Present, VSYNC nosso, telemetria no HUD.",
            "No 4.0 o 1,5x cresce a imagem sem largar os 60. Se for a 2x, usa o otimizado.",
            "Requisitos: qualquer console HDMI, placa DirectShow, GPU NVIDIA, Windows 64-bit.",
        ],
        y - 8,
        16,
    )
    c.showPage()

    # Extra: testado em outros consoles
    extra = "NexusCap 4.0, testado e validado, HDMI de verdade"
    fill(c)
    kicker(c, regular, extra)
    title(c, bold, "E mais: outros consoles")
    c.setFillColorRGB(*MUTED)
    c.setFont(regular, 12)
    c.drawString(36, H - 80, "PS5 abriu o pitch. Switch 2 e Xbox Series S fecham: mesmo Present, mesmo DLSS 5.")
    extras = [
        ("PS5", "God of War e TLOU II\nno pitch. 1440p60.\nA referência da escada."),
        ("Switch 2", "Zelda Tears of the Kingdom.\nLobby + sessão. 2x otimizado\ncom Frame Gen x3."),
        ("Xbox Series S", "Red Dead Redemption.\nLobby + sessão. 2x otimizado\ncom Frame Gen x3."),
        ("Todos os modos", "1x, 1,5x, 2x, 2x otimizado\ne 3x no mesmo caminho.\nEstes prints são o 2x+ FG."),
    ]
    cw, ch, gap = 210, 168, 14
    y_card = 86
    for i, (head, copy) in enumerate(extras):
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
    c.drawString(36, 36, "Se o HDMI chega, o NexusCap trata. Console muda. O caminho não.")
    c.showPage()

    sw_l = PITCH / "print_switch2_lobby.jpg"
    sw_s = PITCH / "print_switch2_session.jpg"
    if sw_l.is_file() or sw_s.is_file():
        fill(c)
        kicker(c, regular, extra)
        title(c, bold, "Switch 2")
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 11)
        c.drawString(36, H - 78, "Tears of the Kingdom. 2x otimizado, Frame Gen x3, PLAYABLE FG. Cap 40, saída 120.")
        if sw_l.is_file():
            fit(c, sw_l, 36, 200, 430, 230)
            c.setFillColorRGB(*INK)
            c.setFont(bold, 11)
            c.drawString(36, 184, "Lobby HDMI")
        if sw_s.is_file():
            fit(c, sw_s, 494, 200, 430, 230)
            c.setFillColorRGB(*INK)
            c.setFont(bold, 11)
            c.drawString(494, 184, "Sessão 2x otimizado + FG")
        c.setFillColorRGB(*GOOD)
        c.setFont(bold, 11)
        c.drawString(36, 48, "Validado no Switch 2. Os outros Pass Count entram no mesmo Present.")
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 10)
        c.drawString(36, 28, "Overlay nosso no topo. Sem Afterburner. Sem recapturar a mesa.")
        c.showPage()

    xb_l = PITCH / "print_xbox_lobby.jpg"
    xb_s = PITCH / "print_xbox_session.jpg"
    if xb_l.is_file() or xb_s.is_file():
        fill(c)
        kicker(c, regular, extra)
        title(c, bold, "Xbox Series S")
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 11)
        c.drawString(36, H - 78, "Red Dead Redemption. 2x otimizado, Frame Gen x3, PLAYABLE FG. Cap 40, saída 120.")
        if xb_l.is_file():
            fit(c, xb_l, 36, 200, 430, 230)
            c.setFillColorRGB(*INK)
            c.setFont(bold, 11)
            c.drawString(36, 184, "Lobby HDMI")
        if xb_s.is_file():
            fit(c, xb_s, 494, 200, 430, 230)
            c.setFillColorRGB(*INK)
            c.setFont(bold, 11)
            c.drawString(494, 184, "Sessão 2x otimizado + FG")
        c.setFillColorRGB(*GOOD)
        c.setFont(bold, 11)
        c.drawString(36, 48, "Validado no Xbox Series S. 1x, 1,5x, 2x e 3x funcionam igual.")
        c.setFillColorRGB(*MUTED)
        c.setFont(regular, 10)
        c.drawString(36, 28, "Mesma placa, mesmo app, outro console. O DLSS 5 não pergunta qual é.")
        c.showPage()

    fill(c)
    kicker(c, regular, extra)
    title(c, bold, "Três consoles, um caminho")
    body_lines(
        c,
        regular,
        [
            "Os prints desta sessão são 2x otimizado com Frame Gen. É o look mais exigente que mostramos.",
            "1x, 1,5x, 2x default e 3x usam o mesmo Present. Muda o Pass, não o console.",
            "PS5, Switch 2, Xbox Series S. Se o HDMI chega, o NexusCap valida.",
        ],
        H - 88,
        17,
    )
    thumbs = [
        (PITCH / "print_ps5_session.jpg", "PS5"),
        (PITCH / "print_switch2_session.jpg", "Switch 2"),
        (PITCH / "print_xbox_session.jpg", "Xbox Series S"),
    ]
    thumbs = [(p, a) for p, a in thumbs if p.is_file()]
    n = max(len(thumbs), 1)
    gap = 16
    tw = (W - 72 - gap * (n - 1)) / n
    y0 = 72
    for i, (img, label) in enumerate(thumbs):
        x = 36 + i * (tw + gap)
        fit(c, img, x, y0, tw, 230)
        c.setFillColorRGB(*INK)
        c.setFont(bold, 12)
        c.drawString(x, y0 - 18, label)
    c.setFillColorRGB(*GOOD)
    c.setFont(bold, 13)
    c.drawString(36, 36, "Testado e validado. Não é só PS5.")
    c.showPage()

    c.save()
    print("wrote", OUT, OUT.stat().st_size)


if __name__ == "__main__":
    main()
