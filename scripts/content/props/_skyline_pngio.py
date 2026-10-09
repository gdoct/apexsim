"""Minimal 8-bit RGB PNG reader/writer (numpy + zlib only) for skyline_night.py."""
import struct, zlib
import numpy as np


def decode_rgb8(d):
    p = 8; idat = b""; w = h = None
    while p < len(d):
        l, t = struct.unpack(">I4s", d[p:p + 8]); c = d[p + 8:p + 8 + l]; p += 12 + l
        if t == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", c)
            assert (bd, ct, il) == (8, 2, 0), "only 8-bit RGB, non-interlaced"
        if t == b"IDAT":
            idat += c
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, 1 + w * 3)
    out = np.zeros((h, w * 3), np.int32); bpp = 3
    for y in range(h):
        f = raw[y, 0]; line = raw[y, 1:].astype(np.int32)
        prev = out[y - 1] if y else np.zeros(w * 3, np.int32)
        if f == 0:
            out[y] = line
        elif f == 2:
            out[y] = (line + prev) & 255
        elif f == 1:
            r = line.copy()
            for i in range(bpp, w * 3):
                r[i] = (r[i] + r[i - bpp]) & 255
            out[y] = r
        elif f == 3:
            r = line.copy()
            for i in range(w * 3):
                a = r[i - bpp] if i >= bpp else 0
                r[i] = (r[i] + ((a + prev[i]) >> 1)) & 255
            out[y] = r
        elif f == 4:
            r = line.copy()
            for i in range(w * 3):
                a = r[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
                pa = abs(b - c); pb = abs(a - c); pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                r[i] = (r[i] + pr) & 255
            out[y] = r
    return out.reshape(h, w, 3).astype(np.uint8)


def encode_rgb8(a):
    h, w, _ = a.shape
    raw = np.concatenate([np.zeros((h, 1), np.uint8), a.reshape(h, w * 3)], 1).tobytes()
    def ch(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
