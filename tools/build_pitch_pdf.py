#!/usr/bin/env python3
"""Gera assets/pitch/NexusCap_Pitch.pdf (v2.4.0)."""
from __future__ import annotations

from pathlib import Path

from reportlab.lib.colors import HexColor, white
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    Image,
    PageBreak,
    Paragraph,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pitch" / "NexusCap_Pitch.pdf"
PITCH = ROOT / "assets" / "pitch"
LOGO = ROOT / "assets" / "logo.png"

INK = HexColor("#0d1b2a")
ACCENT = HexColor("#1b9aaa")
MUTED = HexColor("#4a5568")
CARD = HexColor("#f0f4f8")


def styles():
    base = getSampleStyleSheet()
    return {
        "title": ParagraphStyle(
            "t",
            parent=base["Title"],
            fontName="Helvetica-Bold",
            fontSize=26,
            textColor=INK,
            alignment=TA_CENTER,
            spaceAfter=6,
        ),
        "sub": ParagraphStyle(
            "s",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=11,
            textColor=MUTED,
            alignment=TA_CENTER,
            spaceAfter=14,
        ),
        "h1": ParagraphStyle(
            "h1",
            parent=base["Heading1"],
            fontName="Helvetica-Bold",
            fontSize=16,
            textColor=INK,
            spaceBefore=10,
            spaceAfter=8,
        ),
        "h2": ParagraphStyle(
            "h2",
            parent=base["Heading2"],
            fontName="Helvetica-Bold",
            fontSize=12,
            textColor=ACCENT,
            spaceBefore=8,
            spaceAfter=4,
        ),
        "body": ParagraphStyle(
            "b",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=10,
            textColor=INK,
            leading=14,
            alignment=TA_LEFT,
            spaceAfter=6,
        ),
        "mono": ParagraphStyle(
            "m",
            parent=base["Code"],
            fontName="Courier",
            fontSize=8,
            textColor=INK,
            leading=11,
            backColor=CARD,
            leftIndent=4,
            rightIndent=4,
            spaceBefore=4,
            spaceAfter=8,
        ),
        "footer": ParagraphStyle(
            "f",
            parent=base["Normal"],
            fontName="Helvetica",
            fontSize=8,
            textColor=MUTED,
            alignment=TA_CENTER,
        ),
    }


def footer(canvas, doc):
    canvas.saveState()
    canvas.setFillColor(MUTED)
    canvas.setFont("Helvetica", 8)
    canvas.drawString(18 * mm, 10 * mm, "Nexus Cap v2.4.0")
    canvas.drawRightString(A4[0] - 18 * mm, 10 * mm, f"{doc.page}")
    canvas.restoreState()


def main():
    st = styles()
    doc = SimpleDocTemplate(
        str(OUT),
        pagesize=A4,
        leftMargin=18 * mm,
        rightMargin=18 * mm,
        topMargin=16 * mm,
        bottomMargin=16 * mm,
        title="Nexus Cap 2.4.0",
        author="NexusCap",
    )
    story = []

    if LOGO.exists():
        story.append(Image(str(LOGO), width=28 * mm, height=28 * mm, hAlign="CENTER"))
        story.append(Spacer(1, 4 * mm))

    story.append(Paragraph("Nexus Cap", st["title"]))
    story.append(Paragraph("Viewer HDMI nativo · RenoDX / Neural Rendering · Frame Gen opcional", st["sub"]))
    story.append(Paragraph("<b>Versão 2.4.0</b> — C++17 · D3D11 · DirectShow · Windows", st["sub"]))

    story.append(Paragraph("O que é", st["h1"]))
    story.append(
        Paragraph(
            "Captura o sinal <b>direto da placa HDMI</b> (DirectShow), desenha em D3D11 e aplica "
            "enhance no Present. Não recaptura a própria tela — por isso latência previsível e "
            "sem o hitch clássico de compositor + capturador.",
            st["body"],
        )
    )

    story.append(Paragraph("Instalação (comunidade)", st["h1"]))
    for i, line in enumerate(
        [
            "Baixe <b>NexusCap-Setup-2.4.0.exe</b> na Release do GitHub.",
            "Instale em <b>%LOCALAPPDATA%\\NexusCap</b> (gravável — evite Program Files).",
            "Abra o app, escolha a placa, marque RenoDX se quiser NR (Delete na captura).",
            "Frame Gen opcional: Lossless Scaling na Steam · Page Down no CFG — única instalação extra.",
            "<b>Sincronizar imagem</b> vem ON (VSYNC) — menos tearing percebido.",
            "<b>Ctrl+Espaço</b> mostra/esconde o HUD · reports bug/perf no CFG.",
        ],
        start=1,
    ):
        story.append(Paragraph(f"{i}. {line}", st["body"]))

    story.append(Paragraph("Arquitetura", st["h1"]))
    story.append(
        Paragraph(
            "Com RenoDX, o Present fica <b>nativo</b> (resolução do sinal). "
            "A escala do CFG vai só para o <b>Feed</b> (<font face='Courier'>work_resolution</font>) — "
            "sem double-downsample. Captura <b>NV12-first</b>; lobby só lista modos com NV12; "
            "sem fake/tela branca se o open falhar.",
            st["body"],
        )
    )
    story.append(
        Paragraph(
            "HDMI → DirectShow (NV12) → FrameSlot → D3D11 Present (VSYNC)<br/>"
            "&nbsp;&nbsp;&nbsp;&nbsp;→ dlss5-feed (escala) → RenoDX NR → Lossless FG (opc.)",
            st["mono"],
        )
    )

    arch = PITCH / "arch_diagram.png"
    if arch.exists():
        story.append(Spacer(1, 2 * mm))
        story.append(Image(str(arch), width=170 * mm, height=85 * mm, hAlign="CENTER"))
        story.append(Paragraph("Diagrama de referência (pipeline Present + FX)", st["footer"]))

    story.append(PageBreak())
    story.append(Paragraph("O que mudou no 2.4.0", st["h1"]))
    bullets = [
        "Captura NV12-first · rejeita downscale silencioso · sem fake no lobby.",
        "Hitch / STABLE do overlay escalam com o FPS da captura.",
        "Latencia clara: present+NR ≈ processamento c/ DLSS5 (~2 ms) · cap = captura pura.",
        "Mini overlay / HUD / reports · Ctrl+Espaco · Setup completo (FG via Steam).",
    ]
    for b in bullets:
        story.append(Paragraph(f"• {b}", st["body"]))

    story.append(Paragraph("Latencia (qualquer modo · escala 100%)", st["h1"]))
    story.append(
        Paragraph(
            "Com <b>DLSS5/NR ativo</b>, o <b>present</b> (todo o processamento no Present) fica tipicamente em "
            "<b>~2 ms</b>. <b>cap</b> e so a captura pura no app (~1–2 ms). Sem jargao e2e.",
            st["body"],
        )
    )
    data = [
        [
            Paragraph("<b>Metrica</b>", st["body"]),
            Paragraph("<b>O que e</b>", st["body"]),
            Paragraph("<b>Tipico</b>", st["body"]),
        ],
        [
            Paragraph("<b>present + NR</b>", st["body"]),
            "Processamento no Present com DLSS5/RenoDX ligado",
            "~2 ms (2–4 ms)",
        ],
        [
            Paragraph("<b>cap</b>", st["body"]),
            "Captura pura no app (sem o hook NR)",
            "~1–2 ms",
        ],
        [
            "Budget do sinal",
            "Intervalo HDMI (~16 ms @60 / ~33 ms @30)",
            "cabe folgado",
        ],
    ]
    t = Table(data, colWidths=[32 * mm, 85 * mm, 48 * mm])
    t.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, 0), CARD),
                ("GRID", (0, 0), (-1, -1), 0.4, MUTED),
                ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
                ("LEFTPADDING", (0, 0), (-1, -1), 6),
                ("RIGHTPADDING", (0, 0), (-1, -1), 6),
                ("TOPPADDING", (0, 0), (-1, -1), 4),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
            ]
        )
    )
    story.append(t)
    story.append(Spacer(1, 3 * mm))
    story.append(
        Paragraph(
            "Mini overlay: <font face='Courier'>p 2.1ms+NR · e2e 1.2</font> · "
            "HUD F3: <font face='Courier'>present … (+RenoDX/NR)</font>",
            st["body"],
        )
    )

    story.append(Paragraph("vs DLSS5 MGF X2", st["h2"]))
    story.append(
        Paragraph(
            "X2: caminho curto Sobrepor + feeder — feeling forte, mas Present/swapchain e "
            "metrônomo do Feeder geravam stalls em lote. NexusCap: mesmo enhance no Present, "
            "Present mais limpo (VSYNC nosso), telemetria, hitch dinâmico e latência explícita "
            "no HUD — produto + sessão estável.",
            st["body"],
        )
    )

    story.append(Paragraph("Requisitos", st["h1"]))
    story.append(
        Paragraph(
            "Placa HDMI DirectShow · GPU NVIDIA para RenoDX · Windows 64-bit · "
            "Lossless Scaling (Steam) só se quiser Frame Gen.",
            st["body"],
        )
    )

    story.append(Spacer(1, 8 * mm))
    story.append(Paragraph("Repo privado · github.com/lealvjo/NexusCap · Setup só na Release", st["footer"]))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
