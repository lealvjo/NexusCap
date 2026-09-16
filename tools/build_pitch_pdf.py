#!/usr/bin/env python3
"""Gera assets/pitch/NexusCap_Pitch.pdf (v2.1.0)."""
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
    canvas.drawString(18 * mm, 10 * mm, "Nexus Cap v2.1.0")
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
        title="Nexus Cap 2.1.0",
        author="NexusCap",
    )
    story = []

    if LOGO.exists():
        story.append(Image(str(LOGO), width=28 * mm, height=28 * mm, hAlign="CENTER"))
        story.append(Spacer(1, 4 * mm))

    story.append(Paragraph("Nexus Cap", st["title"]))
    story.append(Paragraph("Viewer HDMI nativo · RenoDX / Neural Rendering · Frame Gen opcional", st["sub"]))
    story.append(Paragraph("<b>Versão 2.1.0</b> — C++17 · D3D11 · DirectShow · Windows", st["sub"]))

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
            "Baixe <b>NexusCap-Setup-2.1.0.exe</b> na Release do GitHub.",
            "Instale em <b>%LOCALAPPDATA%\\NexusCap</b> (gravável — evite Program Files).",
            "Abra o app, escolha a placa, marque RenoDX se quiser NR (Delete na captura).",
            "Frame Gen opcional: Lossless Scaling na Steam · Page Down no CFG — única instalação extra.",
            "<b>Sincronizar imagem</b> vem ON (VSYNC) — menos tearing percebido.",
        ],
        start=1,
    ):
        story.append(Paragraph(f"{i}. {line}", st["body"]))

    story.append(Paragraph("Arquitetura 2.0", st["h1"]))
    story.append(
        Paragraph(
            "Contrato novo: com RenoDX, o Present fica <b>nativo</b> (resolução do sinal). "
            "A escala do CFG vai só para o <b>Feed</b> (<font face='Courier'>work_resolution</font>) — "
            "sem double-downsample. FrameSlot faz recycle NV12; lobby desliga Techniques MGF se RenoDX off.",
            st["body"],
        )
    )
    story.append(
        Paragraph(
            "HDMI → DirectShow → FrameSlot (recycle) → D3D11 Present (VSYNC)<br/>"
            "&nbsp;&nbsp;&nbsp;&nbsp;→ dlss5-feed (escala) → RenoDX NR (NeuralUplift) → Lossless FG (opc.)",
            st["mono"],
        )
    )

    arch = PITCH / "arch_diagram.png"
    if arch.exists():
        story.append(Spacer(1, 2 * mm))
        story.append(Image(str(arch), width=170 * mm, height=85 * mm, hAlign="CENTER"))
        story.append(Paragraph("Diagrama de referência (pipeline Present + FX)", st["footer"]))

    story.append(PageBreak())
    story.append(Paragraph("O que mudou no 2.1.0", st["h1"]))
    bullets = [
        "Mini overlay definitiva: 1 linha colorida ADV + CPU/GPU dinamicos + sparkline.",
        "Escala so no Feed com RenoDX · Present nativo (imagem mais nitida).",
        "NR no boot confia NeuralUplift (sem pulse que desligava o enhance).",
        "VSYNC default apos A/B (TEAR + ALLOW_TEARING sentia mais tremido).",
        "Warmup de 120 frames nas metricas (hitch NGX ~2,5 s fora do fps medio).",
        "Setup completo para a comunidade — so Lossless Scaling e extra (FG opcional).",
    ]
    for b in bullets:
        story.append(Paragraph(f"• {b}", st["body"]))

    story.append(Paragraph("Métricas (HDMI real VEDO 1440p60)", st["h1"]))
    data = [
        [Paragraph("<b>Métrica</b>", st["body"]), Paragraph("<b>2.1.0 (base 2.0.1)</b>", st["body"])],
        ["fps steady (janela 600)", "~59,94"],
        ["p95 present / e2e", "~2,1–2,4 / ~1,9–2,0 ms"],
        ["stalls / clusters", "~0 / 0 no regime"],
        ["duração validada", "~19,5 min · hitch baixo"],
    ]
    t = Table(data, colWidths=[70 * mm, 90 * mm])
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
    story.append(Spacer(1, 4 * mm))

    story.append(Paragraph("vs DLSS5 MGF X2 (melhor sessão conhecida)", st["h2"]))
    story.append(
        Paragraph(
            "X2: caminho curto Sobrepor + feeder ~1,07 ms, FPS ~58–60, VSync off — excelente feeling. "
            "NexusCap 2.1.0: Present medido estável com VSYNC, NR boot confiável, sem double-scale, "
            "mini overlay completa, lobby/CFG/FG/logs e telemetria 600f. No pacote produto + "
            "estabilidade de sessão longa, o Cap leva; feeder ms cru ainda pode favorecer o X2.",
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
