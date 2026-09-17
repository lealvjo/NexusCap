# Compat: gera o mesmo PDF que tools/build_pitch_pdf.py (pitch 3.0 original + escada).
from pathlib import Path
import runpy

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).with_name("build_pitch_pdf.py")), run_name="__main__")
