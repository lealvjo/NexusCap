"""Save the primary monitor to a JPEG. Used for pitch stills."""
import sys
from pathlib import Path

from PIL import ImageGrab

out = Path(sys.argv[1])
img = ImageGrab.grab()
img.convert("RGB").save(out, "JPEG", quality=90, optimize=True)
print(f"wrote {out} {out.stat().st_size} {img.size}")
