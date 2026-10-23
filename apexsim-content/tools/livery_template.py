#!/usr/bin/env python3
"""A car's livery template: what a player paints their own livery against.

    python tools/livery_template.py posh-gt3rs [--size 2048] [--out DIR]

Writes, for one car folder (default or custom), into
`cars/custom/liveries/<car folder>/template/` unless `--out` says otherwise:

- `<slot>_uv.png` for every slot a livery paints (`car_skin*`, `car_logo`):
  the slot's UV layout as dark wireframe on transparent, at `--size`
  pixels, to lay under a painting in any editor;
- `<slot>_works.png|jpg`: the texture the model carries in that slot, if
  any, as a starting point;
- `livery.toml.example`: an `[[livery]]` table naming those files, to copy
  into `../<livery name>/livery.toml` beside the painted textures.

A player's livery is that folder: `cars/custom/liveries/<car>/<name>/` with
a `livery.toml` holding one `[[livery]]` table (the car.toml format,
apexsim-content/docs/cars.md) whose paths are relative to it. The game lists
it after the car's own liveries, on the player's own car only.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
DTYPES = {5121: "<u1", 5123: "<u2", 5125: "<u4", 5126: "<f4"}


def is_paintable(slot: str) -> bool:
    """A slot a livery paints with a texture: `car_skin`, `car_skin_*`, `car_logo`."""
    s = slot.lower()
    return s == "car_logo" or s == "car_skin" or s.startswith("car_skin_")


class Glb:
    def __init__(self, path: Path):
        data = path.read_bytes()
        magic, version, _ = struct.unpack_from("<III", data, 0)
        if magic != GLB_MAGIC or version != 2:
            raise ValueError(f"{path}: not a glTF 2 binary")
        length, kind = struct.unpack_from("<II", data, 12)
        if kind != CHUNK_JSON:
            raise ValueError(f"{path}: first chunk is not JSON")
        self.json = json.loads(data[20:20 + length])
        self.bin_start = 20 + length + 8
        self.data = data

    def view_bytes(self, index: int) -> bytes:
        view = self.json["bufferViews"][index]
        start = self.bin_start + view.get("byteOffset", 0)
        return self.data[start:start + view["byteLength"]]

    def accessor(self, index: int) -> np.ndarray:
        acc = self.json["accessors"][index]
        view = self.json["bufferViews"][acc["bufferView"]]
        width = COMPONENTS[acc["type"]]
        dtype = np.dtype(DTYPES[acc["componentType"]])
        start = self.bin_start + view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = view.get("byteStride", 0) or dtype.itemsize * width
        if stride == dtype.itemsize * width:
            out = np.frombuffer(self.data, dtype=dtype, count=acc["count"] * width, offset=start)
            return out.reshape(-1, width)
        rows = [np.frombuffer(self.data, dtype=dtype, count=width, offset=start + i * stride)
                for i in range(acc["count"])]
        return np.stack(rows)


def slot_triangles(glb: Glb) -> dict[str, list[np.ndarray]]:
    """Every paintable slot's triangles in UV space, (n, 3, 2) arrays."""
    materials = glb.json.get("materials", [])
    out: dict[str, list[np.ndarray]] = {}
    for mesh in glb.json.get("meshes", []):
        for prim in mesh.get("primitives", []):
            mat = prim.get("material")
            if mat is None or "TEXCOORD_0" not in prim["attributes"]:
                continue
            name = materials[mat].get("name", f"material_{mat}")
            if not is_paintable(name):
                continue
            uv = glb.accessor(prim["attributes"]["TEXCOORD_0"]).astype(np.float64)
            if "indices" in prim:
                idx = glb.accessor(prim["indices"]).reshape(-1).astype(np.int64)
            else:
                idx = np.arange(len(uv), dtype=np.int64)
            idx = idx[: len(idx) // 3 * 3].reshape(-1, 3)
            out.setdefault(name, []).append(uv[idx])
    return out


def slot_images(glb: Glb) -> dict[str, tuple[bytes, str]]:
    """Each paintable slot's own base colour image: (bytes, extension)."""
    out: dict[str, tuple[bytes, str]] = {}
    textures = glb.json.get("textures", [])
    images = glb.json.get("images", [])
    for mat in glb.json.get("materials", []):
        name = mat.get("name", "")
        if not is_paintable(name) or name in out:
            continue
        info = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if not info:
            continue
        image = images[textures[info["index"]]["source"]]
        if "bufferView" not in image:
            continue
        ext = "jpg" if image.get("mimeType") == "image/jpeg" else "png"
        out[name] = (glb.view_bytes(image["bufferView"]), ext)
    return out


def draw_uv(triangles: list[np.ndarray], size: int) -> Image.Image:
    """Dark UV wireframe on transparent; v runs down the image, as glTF's does."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for tris in triangles:
        # Tiled UVs land in their tile's place: the template is the 0-1 square.
        pts = np.mod(tris, 1.0) * (size - 1)
        for tri in pts:
            a, b, c = (tuple(p) for p in tri)
            draw.line([a, b, c, a], fill=(16, 16, 16, 220), width=1)
    return img


def car_folder(content: Path, name: str) -> Path:
    for sub in ("default", "custom"):
        folder = content / "cars" / sub / name
        if (folder / "car.toml").is_file():
            return folder
    raise SystemExit(f"no car folder named {name!r} under cars/default or cars/custom")


def body_model(folder: Path) -> Path:
    for line in (folder / "car.toml").read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            break
        if stripped.startswith("model") and "=" in stripped:
            return folder / stripped.split("=", 1)[1].split("#", 1)[0].strip().strip('"')
    raise SystemExit(f"{folder / 'car.toml'} names no top-level model")


def write_template(folder: Path, out: Path, size: int) -> list[str]:
    glb = Glb(body_model(folder))
    out.mkdir(parents=True, exist_ok=True)
    tris = slot_triangles(glb)
    images = slot_images(glb)
    written: list[str] = []
    for slot in sorted(tris):
        draw_uv(tris[slot], size).save(out / f"{slot}_uv.png")
        written.append(f"{slot}_uv.png")
    for slot, (data, ext) in sorted(images.items()):
        (out / f"{slot}_works.{ext}").write_bytes(data)
        written.append(f"{slot}_works.{ext}")

    lines = [
        "# Copy to ../<your livery>/livery.toml and paint the files it names there.",
        "[[livery]]",
        'name = "My livery"',
        "paint = [0.60, 0.05, 0.04]      # car_paint, linear RGB (optional)",
        "# accent = [0.9, 0.9, 0.9]      # car_accent (optional)",
    ]
    skins = [s for s in sorted(tris) if s.lower().startswith("car_skin")]
    if "car_logo" in tris:
        lines.append('logo = "logo.png"              # car_logo, against car_logo_uv.png')
    if "car_skin" in tris:
        lines.append('skin = "skin.png"              # every car_skin* slot, against car_skin_uv.png')
    others = [s for s in skins if s != "car_skin"]
    if others:
        pairs = ", ".join(f'"{s}={s}.png"' for s in others)
        lines.append(f"# textures = [{pairs}]")
    (out / "livery.toml.example").write_text("\n".join(lines) + "\n", encoding="utf-8")
    written.append("livery.toml.example")
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("car", help="the car's folder name under cars/default or cars/custom")
    parser.add_argument("--size", type=int, default=2048, help="template size in pixels (default 2048)")
    parser.add_argument("--out", type=Path, help="where to write (default cars/custom/liveries/<car>/template)")
    args = parser.parse_args(argv)
    content = Path.cwd()
    folder = car_folder(content, args.car)
    out = args.out or content / "cars" / "custom" / "liveries" / folder.name / "template"
    written = write_template(folder, out, args.size)
    if not any(name.endswith("_uv.png") for name in written):
        print(f"{folder.name}: its body has no car_skin or car_logo slot; a livery can still set paint and accent")
    print(f"{out}: " + ", ".join(written))
    return 0


if __name__ == "__main__":
    sys.exit(main())
