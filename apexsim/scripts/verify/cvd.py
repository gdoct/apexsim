#!/usr/bin/env python3
"""Show a shot as a colour-blind eye sees it, for docs/verification.md R20.

    python scripts/verify/cvd.py out/verify/n22-colour-vision/shots/02.jpg deutan out.png
    python scripts/verify/cvd.py <shot> protan|deutan|tritan <out> [left,top,right,bottom]

Writes the shot (or the box of it) above its simulation at full severity
(Machado, Oliveira and Fernandes 2009, applied in linear RGB), so the two
read side by side: the HUD's states should still tell apart below.
"""

import sys

import numpy as np
from PIL import Image

MATRICES = {
    "protan": [[0.152286, 1.052583, -0.204868], [0.114503, 0.786281, 0.099216], [-0.003882, -0.048116, 1.051998]],
    "deutan": [[0.367322, 0.860646, -0.227968], [0.280085, 0.672501, 0.047413], [-0.011820, 0.042940, 0.968881]],
    "tritan": [[1.255528, -0.076749, -0.178779], [-0.078411, 0.930809, 0.147602], [0.004733, 0.691367, 0.303900]],
}


def to_linear(c: np.ndarray) -> np.ndarray:
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def simulate(image: Image.Image, kind: str) -> Image.Image:
    linear = to_linear(np.asarray(image.convert("RGB"), dtype=np.float64) / 255.0)
    seen = to_srgb(linear @ np.array(MATRICES[kind]).T)
    return Image.fromarray((seen * 255.0).round().astype(np.uint8))


def main() -> None:
    if len(sys.argv) not in (4, 5) or sys.argv[2] not in MATRICES:
        raise SystemExit(__doc__)
    shot, kind, out = sys.argv[1:4]
    image = Image.open(shot).convert("RGB")
    if len(sys.argv) == 5:
        image = image.crop(tuple(int(v) for v in sys.argv[4].split(",")))
    pair = Image.new("RGB", (image.width, image.height * 2))
    pair.paste(image, (0, 0))
    pair.paste(simulate(image, kind), (0, image.height))
    pair.save(out)


if __name__ == "__main__":
    main()
