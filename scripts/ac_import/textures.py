"""AC's DDS textures into the DDS files the client streams straight into
compressed textures: BC1 (DXT1) for opaque maps and BC3 (DXT5) for maps with
alpha, always with a full mip chain, never wider than `--max-texture`.

A source that already is BC1/BC3 with its mips is copied as it is (the
authors' encoder beat this one), dropping top mips when it is too large.
Anything else (most Kunos textures ship without mips; some are
uncompressed) is decoded with Pillow, resized if needed, mipped by a box
filter and encoded here with a range fit, which is quick and good enough
for scenery seen from a car.
"""

from __future__ import annotations

import io
import struct
from dataclasses import dataclass

import numpy as np
from PIL import Image

DDS_MAGIC = b"DDS "
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDPF_FOURCC = 0x4
DDSCAPS_COMPLEX = 0x8
DDSCAPS_TEXTURE = 0x1000
DDSCAPS_MIPMAP = 0x400000

BLOCK_BYTES = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16, b"ATI2": 16, b"BC5U": 16}


@dataclass
class DdsInfo:
    fourcc: bytes | None
    width: int
    height: int
    mips: int
    rgb_bits: int = 0

    @property
    def compressed(self) -> bool:
        return self.fourcc is not None and self.fourcc in BLOCK_BYTES


@dataclass
class ConvertedTexture:
    data: bytes
    fourcc: bytes
    width: int
    height: int
    mips: int
    has_alpha: bool
    #: Linear RGB average of the top mip, for the flat mode.
    average_linear: tuple[float, float, float]
    how: str


def dds_info(blob: bytes) -> DdsInfo | None:
    if len(blob) < 128 or blob[:4] != DDS_MAGIC:
        return None
    h = struct.unpack("<31I", blob[4:128])
    height, width, mips = h[2], h[3], max(h[6], 1)
    pf_flags = h[19]
    fourcc = blob[84:88] if pf_flags & DDPF_FOURCC else None
    return DdsInfo(fourcc=fourcc, width=width, height=height, mips=mips, rgb_bits=h[21])


def block_size(fourcc: bytes, width: int, height: int) -> int:
    return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * BLOCK_BYTES[fourcc]


def mip_dims(width: int, height: int, level: int) -> tuple[int, int]:
    return max(1, width >> level), max(1, height >> level)


def full_chain(width: int, height: int) -> int:
    """How many mips `mip_chain` makes: halves while both sides stay at
    least a block (4 px) wide."""
    n = 1
    while width // 2 >= 4 and height // 2 >= 4:
        width, height = width // 2, height // 2
        n += 1
    return n


def write_dds_bytes(fourcc: bytes, width: int, height: int, mips: list[bytes]) -> bytes:
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE
    caps = DDSCAPS_TEXTURE
    if len(mips) > 1:
        flags |= DDSD_MIPMAPCOUNT
        caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    header = struct.pack("<4sIIIIIII", DDS_MAGIC, 124, flags, height, width, len(mips[0]), 0, len(mips))
    header += bytes(11 * 4)
    header += struct.pack("<IIIIIIII", 32, DDPF_FOURCC, int.from_bytes(fourcc, "little"), 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", caps, 0, 0, 0, 0)
    assert len(header) == 128
    return header + b"".join(mips)


def decode_image(blob: bytes) -> np.ndarray:
    """Any texture Pillow reads (DDS, PNG, JPEG) as (h, w, 4) uint8 RGBA."""
    im = Image.open(io.BytesIO(blob))
    im.load()
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    return np.asarray(im, dtype=np.uint8).copy()


def _fit(rgba: np.ndarray, max_size: int) -> np.ndarray:
    h, w = rgba.shape[:2]
    scale = 1.0
    while max(w, h) * scale > max_size:
        scale /= 2.0
    nw, nh = max(4, int(w * scale)) // 4 * 4, max(4, int(h * scale)) // 4 * 4
    if (nw, nh) == (w, h):
        return rgba
    im = Image.fromarray(rgba, "RGBA").resize((nw, nh), Image.LANCZOS)
    return np.asarray(im, dtype=np.uint8).copy()


def mip_chain(rgba: np.ndarray) -> list[np.ndarray]:
    """Box-filtered halves down to 4 px on the longer side."""
    chain = [rgba]
    cur = rgba.astype(np.float32)
    while max(cur.shape[0], cur.shape[1]) > 4:
        h, w = cur.shape[:2]
        h2, w2 = max(1, h // 2), max(1, w // 2)
        cur = cur[: h2 * 2, : w2 * 2]
        cur = (cur[0::2, 0::2] + cur[1::2, 0::2] + cur[0::2, 1::2] + cur[1::2, 1::2]) * 0.25
        if h2 < 4 or w2 < 4:
            break
        chain.append(np.clip(np.rint(cur), 0, 255).astype(np.uint8))
    return chain


def _blocks(rgba: np.ndarray) -> np.ndarray:
    """(blocks, 16, 4) float32, blocks in raster order, edges padded by repeat."""
    h, w = rgba.shape[:2]
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w):
        rgba = np.pad(rgba, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
    b = rgba.reshape(ph // 4, 4, pw // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    return b.astype(np.float32)


def _to565(rgb: np.ndarray) -> np.ndarray:
    r = np.clip(np.rint(rgb[:, 0] * 31 / 255), 0, 31).astype(np.uint32)
    g = np.clip(np.rint(rgb[:, 1] * 63 / 255), 0, 63).astype(np.uint32)
    b = np.clip(np.rint(rgb[:, 2] * 31 / 255), 0, 31).astype(np.uint32)
    return (r << 11) | (g << 5) | b


def _from565(c: np.ndarray) -> np.ndarray:
    r = ((c >> 11) & 31).astype(np.float32) * 255 / 31
    g = ((c >> 5) & 63).astype(np.float32) * 255 / 63
    b = (c & 31).astype(np.float32) * 255 / 31
    return np.stack([r, g, b], axis=1)


def encode_bc1_blocks(blocks: np.ndarray) -> np.ndarray:
    """(n, 16, 4) -> (n, 8) uint8 colour blocks: a bounding-box range fit,
    always in the four-colour mode (c0 > c1) unless the block is flat."""
    rgb = blocks[:, :, :3]
    mx = rgb.max(axis=1)
    mn = rgb.min(axis=1)
    c0 = _to565(mx)
    c1 = _to565(mn)
    swap = c0 < c1
    c0s = np.where(swap, c1, c0)
    c1s = np.where(swap, c0, c1)
    p0 = _from565(c0s)
    p1 = _from565(c1s)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], axis=1)  # (n, 4, 3)
    d = rgb[:, :, None, :] - palette[:, None, :, :]
    dist = (d * d).sum(axis=3)
    idx = np.argmin(dist, axis=2).astype(np.uint32)  # (n, 16)
    flat = c0s == c1s
    idx[flat] = 0
    packed = np.zeros(blocks.shape[0], dtype=np.uint32)
    for i in range(16):
        packed |= idx[:, i] << (2 * i)
    out = np.zeros((blocks.shape[0], 8), dtype=np.uint8)
    out[:, 0] = c0s & 0xFF
    out[:, 1] = (c0s >> 8) & 0xFF
    out[:, 2] = c1s & 0xFF
    out[:, 3] = (c1s >> 8) & 0xFF
    for i in range(4):
        out[:, 4 + i] = (packed >> (8 * i)) & 0xFF
    return out


def encode_bc3_blocks(blocks: np.ndarray) -> np.ndarray:
    """(n, 16, 4) -> (n, 16) uint8: an eight-level alpha block then the BC1 colours."""
    alpha = blocks[:, :, 3]
    a0 = alpha.max(axis=1)
    a1 = alpha.min(axis=1)
    a0q = np.clip(np.rint(a0), 0, 255).astype(np.uint64)
    a1q = np.clip(np.rint(a1), 0, 255).astype(np.uint64)
    a0f = a0q.astype(np.float32)
    a1f = a1q.astype(np.float32)
    # Palette order of the eight-alpha mode: a0, a1, then six steps a0 -> a1.
    steps = np.stack([a0f, a1f] + [((7 - i) * a0f + i * a1f) / 7 for i in range(1, 7)], axis=1)
    d = np.abs(alpha[:, :, None] - steps[:, None, :])
    idx = np.argmin(d, axis=2).astype(np.uint64)
    idx[a0q == a1q] = 0
    packed = np.zeros(blocks.shape[0], dtype=np.uint64)
    for i in range(16):
        packed |= idx[:, i] << np.uint64(3 * i)
    out = np.zeros((blocks.shape[0], 16), dtype=np.uint8)
    out[:, 0] = a0q & 0xFF
    out[:, 1] = a1q & 0xFF
    for i in range(6):
        out[:, 2 + i] = (packed >> np.uint64(8 * i)) & np.uint64(0xFF)
    out[:, 8:] = encode_bc1_blocks(blocks)
    return out


def encode_mip(rgba: np.ndarray, fourcc: bytes) -> bytes:
    blocks = _blocks(rgba)
    if fourcc == b"DXT1":
        return encode_bc1_blocks(blocks).tobytes()
    return encode_bc3_blocks(blocks).tobytes()


def average_linear(rgba: np.ndarray) -> tuple[float, float, float]:
    srgb = rgba[:, :, :3].astype(np.float32) / 255.0
    linear = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
    mean = linear.reshape(-1, 3).mean(axis=0)
    return (float(mean[0]), float(mean[1]), float(mean[2]))


def has_alpha(rgba: np.ndarray) -> bool:
    return bool((rgba[:, :, 3] < 250).mean() > 0.002)


def convert_texture(blob: bytes, max_size: int = 2048) -> ConvertedTexture:
    """A DDS the client can stream, from whatever AC carried."""
    info = dds_info(blob)
    rgba: np.ndarray | None = None
    if info and info.fourcc in (b"DXT1", b"DXT5") and info.width % 4 == 0 and info.height % 4 == 0 \
            and info.mips >= full_chain(info.width, info.height) - 1 and info.width > 0 and info.height > 0:
        # Verbatim, dropping top mips until it fits.
        fourcc = info.fourcc
        drop = 0
        while max(info.width, info.height) >> drop > max_size and info.mips - drop > 1:
            drop += 1
        offset = 128
        mips: list[bytes] = []
        for level in range(info.mips):
            w, h = mip_dims(info.width, info.height, level)
            size = block_size(fourcc, w, h)
            if offset + size > len(blob):
                break
            if level >= drop:
                mips.append(blob[offset:offset + size])
            offset += size
        if mips:
            w, h = mip_dims(info.width, info.height, drop)
            rgba = decode_image(blob)
            avg = average_linear(rgba)
            data = write_dds_bytes(fourcc, w, h, mips)
            return ConvertedTexture(data=data, fourcc=fourcc, width=w, height=h, mips=len(mips),
                                    has_alpha=fourcc == b"DXT5", average_linear=avg,
                                    how="copied" if drop == 0 else f"copied, top {drop} mip(s) dropped")
    rgba = decode_image(blob)
    rgba = _fit(rgba, max_size)
    alpha = has_alpha(rgba)
    fourcc = b"DXT5" if alpha else b"DXT1"
    chain = mip_chain(rgba)
    mips = [encode_mip(level, fourcc) for level in chain]
    data = write_dds_bytes(fourcc, rgba.shape[1], rgba.shape[0], mips)
    src = (info.fourcc.decode("ascii", "replace") if info and info.fourcc else
           (f"rgb{info.rgb_bits}" if info else "image"))
    return ConvertedTexture(data=data, fourcc=fourcc, width=rgba.shape[1], height=rgba.shape[0],
                            mips=len(mips), has_alpha=alpha, average_linear=average_linear(rgba),
                            how=f"re-encoded from {src}")


def texture_bytes(converted: ConvertedTexture) -> int:
    return len(converted.data) - 128
