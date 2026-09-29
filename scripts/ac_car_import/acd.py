"""Assetto Corsa's `data.acd`: the car's physics INIs and LUTs in one file.

Kunos pack every car's `data/` folder this way; Content Manager and every
AC tool read it routinely. It is not the Custom Shaders Patch encryption the
importers refuse. Layout (docs/AC_CAR_IMPORT.md, appendix):

    optional header: i32 -1111, i32 version
    records:         i32 name_len, name (clear), i32 n, n x 4 bytes

Only every fourth byte of a record's body carries data:
`plain[i] = (raw[4i] - key[i mod len(key)]) mod 256`, where the key is the
text of eight numbers joined by `-`, each a hash of the lower-cased folder
name. A wrong key shows at once: the names come out right and the contents
as noise, which `read_acd` checks for.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

HEADER_MAGIC = -1111


class AcdError(Exception):
    """A data.acd that cannot be read as this car's."""


def _i32(v: int) -> int:
    """Wrap to a signed 32-bit integer, as the C the key was written in."""
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _cdiv(a: int, b: int) -> int:
    """C integer division: truncates toward zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def _cmod(a: int, b: int) -> int:
    return a - _cdiv(a, b) * b


def acd_key(folder_name: str) -> str:
    """The key for a car folder (`ks_porsche_911_gt3_r_2016` gives
    `145-191-144-93-26-0-15-55`)."""
    s = [ord(ch) for ch in folder_name.lower()]
    n = len(s)

    k1 = 0
    for c in s:
        k1 = _i32(k1 + c)

    k2 = 0
    for i in range(0, n - 1, 2):
        k2 = _i32(k2 * s[i])
        k2 = _i32(k2 - s[i + 1])

    k3 = 0
    for i in range(1, n - 3, 3):
        k3 = _i32(k3 * s[i])
        k3 = _i32(_cdiv(k3, s[i + 1] + 27))
        k3 = _i32(k3 + (-27 - s[i - 1]))

    k4 = 0x1683
    for c in s[1:]:
        k4 = _i32(k4 - c)

    k5 = 0x42
    for i in range(1, n - 4, 4):
        k5 = _i32((s[i] + 15) * k5)
        k5 = _i32(k5 * (s[i - 1] + 15))
        k5 = _i32(k5 + 22)

    k6 = 0x65
    for i in range(0, n - 2, 2):
        k6 = _i32(k6 - s[i])

    # The published description adds 0xab inside the modulus, which pins
    # this part at 171 for every name; the ciphertext says it is a plain
    # running modulus from 0xab.
    k7 = 0xAB
    for i in range(0, n - 2, 2):
        k7 = _cmod(k7, s[i])

    k8 = 0xAB
    for i in range(0, n - 1):
        k8 = _i32(_cdiv(k8, s[i]))
        k8 = _i32(k8 + s[i + 1])

    return "-".join(str(k & 0xFF) for k in (k1, k2, k3, k4, k5, k6, k7, k8))


def decrypt(raw: bytes, key: str) -> bytes:
    body = np.frombuffer(raw, dtype=np.uint8)[0::4].astype(np.int32)
    kb = np.frombuffer(key.encode("ascii"), dtype=np.uint8).astype(np.int32)
    ks = np.resize(kb, body.shape[0])
    return ((body - ks) % 256).astype(np.uint8).tobytes()


def _looks_like_text(blob: bytes) -> bool:
    if not blob:
        return True
    printable = sum(1 for b in blob[:4096] if b in (9, 10, 13) or 32 <= b < 127 or b >= 160)
    return printable / min(len(blob), 4096) > 0.95


def read_acd(path: Path, folder_name: str | None = None) -> dict[str, bytes]:
    """Every file in a data.acd, by name (lower-cased), decrypted with the
    key of `folder_name` (default: the folder the file sits in)."""
    path = Path(path)
    data = path.read_bytes()
    key = acd_key(folder_name or path.parent.name)
    at = 0
    if len(data) >= 8 and struct.unpack_from("<i", data, 0)[0] == HEADER_MAGIC:
        at = 8
    out: dict[str, bytes] = {}
    while at < len(data):
        if at + 4 > len(data):
            raise AcdError(f"{path}: truncated at offset {at}")
        (name_len,) = struct.unpack_from("<i", data, at)
        at += 4
        if name_len <= 0 or name_len > 1024 or at + name_len + 4 > len(data):
            raise AcdError(f"{path}: implausible record name length {name_len} at offset {at - 4}")
        name = data[at:at + name_len].decode("latin-1")
        at += name_len
        (n,) = struct.unpack_from("<i", data, at)
        at += 4
        if n < 0 or at + 4 * n > len(data):
            raise AcdError(f"{path}: record {name!r} runs past the end of the file")
        out[name.lower()] = decrypt(data[at:at + 4 * n], key)
        at += 4 * n
    # Every car has car.ini, and it is text; noise there is a wrong key (a
    # renamed folder) or a packer that is not Kunos's.
    car_ini = out.get("car.ini")
    if car_ini is None:
        raise AcdError(f"{path}: holds no car.ini")
    if not _looks_like_text(car_ini) or b"[" not in car_ini[:256]:
        raise AcdError(
            f"{path}: car.ini does not decrypt to text with the key of folder "
            f"{folder_name or path.parent.name!r}; was the folder renamed?"
        )
    return out


def read_car_data(car_dir: Path) -> tuple[dict[str, bytes], str]:
    """The car's data files, from `data/` when it is unpacked, else
    `data.acd`; and which of the two it was."""
    car_dir = Path(car_dir)
    unpacked = car_dir / "data"
    if unpacked.is_dir() and (unpacked / "car.ini").is_file():
        files = {p.name.lower(): p.read_bytes() for p in sorted(unpacked.iterdir()) if p.is_file()}
        return files, "data/"
    acd = car_dir / "data.acd"
    if acd.is_file():
        return read_acd(acd, car_dir.name), "data.acd"
    raise AcdError(f"{car_dir}: neither data/ nor data.acd")
