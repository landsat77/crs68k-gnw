#!/usr/bin/env python3
"""The homebrew cover (coverflow of the Retro-Go launcher) from an image.

The firmware caps covers at 186x100 and 10 KiB (HW JPEG scratch). A bigger
image is scaled down to fit (keeping its shape), a smaller one is kept as
is; it is saved as a baseline JPEG (no ICC profile) at the best quality that
fits in 10 KiB.
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

from PIL import Image

COVER_MAX_WIDTH = 186
COVER_MAX_HEIGHT = 100
COVER_SIZE_MAX = 10 * 1024


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="src", type=Path, required=True, help="cover image (PNG)")
    ap.add_argument("--out", type=Path, required=True, help="output JPEG")
    args = ap.parse_args()

    im = Image.open(args.src).convert("RGB")
    scale = min(1.0, COVER_MAX_WIDTH / im.width, COVER_MAX_HEIGHT / im.height)
    if scale < 1.0:
        im = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))), Image.LANCZOS)
    for q in range(95, 30, -5):
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=q, optimize=True, progressive=False)
        if buf.tell() <= COVER_SIZE_MAX:
            args.out.write_bytes(buf.getvalue())
            print(f"cover: {im.width}x{im.height}, quality {q}, {buf.tell()} bytes")
            return 0
    print("cover: cannot fit in 10 KiB", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
