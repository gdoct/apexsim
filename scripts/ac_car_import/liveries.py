"""AC's skins -> texture liveries (docs/content/cars.md, "Texture liveries").

A skin folder overrides kn5 textures by file name. One skin (the first in
AC's order, or `--skin`) is baked into the GLBs and is livery 0, "the model
as authored"; every other skin becomes a `[[livery]]` table whose `skin`
replaces the base colour of every `car_skin*` slot and whose `textures`
replace the other slots it repaints. A livery lists every texture *any*
skin overrides, taking the kn5's own where this skin has none, so stepping
from one livery to another never leaves the previous one's banner behind.
"""

from __future__ import annotations

import hashlib
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from ac_import.kn5 import Kn5File

from .data import Skin
from .model import MaterialPlan, encode_texture


@dataclass
class Livery:
    name: str
    folder: str
    skin: str | None
    textures: list[tuple[str, str]]
    preview: str | None


@dataclass
class LiveryPlan:
    default: Skin | None
    liveries: list[Livery] = field(default_factory=list)
    #: relative path -> bytes, every file the liveries name
    files: dict[str, bytes] = field(default_factory=dict)
    overridden: list[str] = field(default_factory=list)


def skin_overrides(skin: Skin, kn5: Kn5File, wanted: set[str]) -> dict[str, Path]:
    """kn5 texture name -> the skin's file replacing it, for the textures
    in `wanted` (the diffuse maps the GLBs draw)."""
    by_lower = {name.lower(): name for name in kn5.textures}
    out: dict[str, Path] = {}
    for lower, path in skin.files.items():
        name = by_lower.get(lower)
        if name and name in wanted:
            out[name] = path
    return out


def apply_default_skin(kn5: Kn5File, skin: Skin | None, wanted: set[str]) -> list[str]:
    """Bake a skin's textures into the kn5 in memory; returns what it replaced."""
    if skin is None:
        return []
    replaced = []
    for name, path in sorted(skin_overrides(skin, kn5, wanted).items()):
        kn5.textures[name].data = path.read_bytes()
        replaced.append(name)
    return replaced


def safe_folder(name: str) -> str:
    out = "".join(c if c.isalnum() or c in "-_." else "_" for c in name).strip("._")
    return out or "skin"


def plan_liveries(kn5_original: dict[str, bytes], kn5: Kn5File, skins: list[Skin], default: Skin | None,
                  plan: MaterialPlan, skin_size: int, texture_size: int) -> LiveryPlan:
    wanted = set(plan.alpha_textures)
    out = LiveryPlan(default=default)
    others = [s for s in skins if default is None or s.name != default.name]
    overrides = {s.name: skin_overrides(s, kn5, wanted) for s in skins}
    union = sorted({t for s in skins for t in overrides[s.name]})
    out.overridden = union
    if not others or not union:
        return out
    slots_of: dict[str, list[str]] = {}
    for slot, info in sorted(plan.report.items()):
        tex = info.get("texture")
        if tex in union and not (slot == "car_skin" or slot.startswith("car_skin_")):
            slots_of.setdefault(tex, []).append(slot)

    written: dict[str, str] = {}

    def put(folder: str, tex: str, source: bytes) -> str:
        size = skin_size if tex == plan.skin_texture else texture_size
        alpha = plan.alpha_textures.get(tex, False)
        key = hashlib.sha1(source + bytes([alpha]) + size.to_bytes(4, "little")).hexdigest()
        if key in written:
            return written[key]
        t = encode_texture(source, size, alpha)
        stem = tex.rsplit(".", 1)[0]
        rel = f"skins/{folder}/{stem}.{t.ext}"
        out.files[rel] = t.data
        written[key] = rel
        return rel

    for skin in others:
        folder = safe_folder(skin.name)
        own = overrides[skin.name]
        skin_path = None
        textures: list[tuple[str, str]] = []
        for tex in union:
            src_path = own.get(tex)
            if src_path is not None:
                rel = put(folder, tex, src_path.read_bytes())
            else:
                rel = put("_kn5", tex, kn5_original[tex])
            if tex == plan.skin_texture:
                skin_path = rel
            else:
                textures.extend((slot, rel) for slot in slots_of.get(tex, []))
        preview = None
        if skin.preview is not None:
            preview = f"skins/{folder}/preview{skin.preview.suffix.lower()}"
            out.files[preview] = skin.preview.read_bytes()
        if skin_path or textures:
            out.liveries.append(Livery(skin.display_name, folder, skin_path, textures, preview))
    return out


def write_files(car_dir: Path, files: dict[str, bytes]) -> None:
    skins = car_dir / "skins"
    if skins.is_dir():
        shutil.rmtree(skins)
    for rel, data in sorted(files.items()):
        p = car_dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
