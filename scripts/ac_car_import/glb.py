"""A small glTF 2.0 binary (GLB) writer: what the client's reader
(`Cars/ApexGlbReader.cpp`) takes and nothing more.

One node, one mesh, one primitive per material (the reader flattens the
tree and makes a section per material anyway); POSITION, NORMAL,
TEXCOORD_0 and uint32 indices; materials with a base colour texture,
metallic/roughness factors, emissive factor, alpha mode and
`KHR_materials_clearcoat`; images embedded as PNG or JPEG. Output is
byte-for-byte deterministic for the same input.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

FLOAT = 5126
UINT32 = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
REPEAT = 10497
LINEAR = 9729
LINEAR_MIPMAP_LINEAR = 9987


@dataclass
class GlbMaterial:
    name: str
    base_color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0)
    #: Index into the builder's images, or None for a flat colour.
    texture: int | None = None
    metallic: float = 0.0
    roughness: float = 0.6
    emissive: tuple[float, float, float] = (0.0, 0.0, 0.0)
    alpha_mode: str = "OPAQUE"  # OPAQUE | MASK | BLEND
    alpha_cutoff: float = 0.5
    double_sided: bool = True
    clearcoat: float = 0.0
    clearcoat_roughness: float = 0.1


@dataclass
class _Primitive:
    positions: np.ndarray
    normals: np.ndarray
    uvs: np.ndarray
    indices: np.ndarray
    material: int


@dataclass
class GlbBuilder:
    images: list[tuple[bytes, str]] = field(default_factory=list)
    materials: list[GlbMaterial] = field(default_factory=list)
    primitives: list[_Primitive] = field(default_factory=list)
    _image_keys: dict[str, int] = field(default_factory=dict)

    def add_image(self, key: str, data: bytes, mime: str) -> int:
        if key in self._image_keys:
            return self._image_keys[key]
        self.images.append((data, mime))
        self._image_keys[key] = len(self.images) - 1
        return self._image_keys[key]

    def add_material(self, material: GlbMaterial) -> int:
        self.materials.append(material)
        return len(self.materials) - 1

    def add_primitive(self, positions: np.ndarray, normals: np.ndarray, uvs: np.ndarray,
                      triangles: np.ndarray, material: int) -> None:
        if len(triangles) == 0:
            return
        self.primitives.append(_Primitive(
            np.ascontiguousarray(positions, dtype="<f4"),
            np.ascontiguousarray(normals, dtype="<f4"),
            np.ascontiguousarray(uvs, dtype="<f4"),
            np.ascontiguousarray(triangles, dtype="<u4").reshape(-1),
            material,
        ))

    @property
    def triangle_count(self) -> int:
        return sum(p.indices.size // 3 for p in self.primitives)

    def bounds(self) -> tuple[np.ndarray, np.ndarray]:
        lo = np.min([p.positions.min(axis=0) for p in self.primitives], axis=0)
        hi = np.max([p.positions.max(axis=0) for p in self.primitives], axis=0)
        return lo.astype(np.float64), hi.astype(np.float64)

    def to_bytes(self) -> bytes:
        blob = bytearray()
        views: list[dict] = []
        accessors: list[dict] = []

        def view(data: bytes, target: int | None) -> int:
            while len(blob) % 4:
                blob.append(0)
            entry = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
            if target is not None:
                entry["target"] = target
            blob.extend(data)
            views.append(entry)
            return len(views) - 1

        def accessor(array: np.ndarray, kind: str, component: int, target: int, bounds: bool = False) -> int:
            entry = {
                "bufferView": view(array.tobytes(), target),
                "componentType": component,
                "count": int(array.shape[0]),
                "type": kind,
            }
            if bounds:
                entry["min"] = [float(v) for v in array.min(axis=0)]
                entry["max"] = [float(v) for v in array.max(axis=0)]
            accessors.append(entry)
            return len(accessors) - 1

        prims = []
        for p in self.primitives:
            attrs = {
                "POSITION": accessor(p.positions, "VEC3", FLOAT, ARRAY_BUFFER, bounds=True),
                "NORMAL": accessor(p.normals, "VEC3", FLOAT, ARRAY_BUFFER),
                "TEXCOORD_0": accessor(p.uvs, "VEC2", FLOAT, ARRAY_BUFFER),
            }
            idx = accessor(p.indices, "SCALAR", UINT32, ELEMENT_ARRAY_BUFFER)
            prims.append({"attributes": attrs, "indices": idx, "material": p.material, "mode": 4})

        images = []
        for data, mime in self.images:
            images.append({"bufferView": view(data, None), "mimeType": mime})

        uses_clearcoat = False
        materials = []
        for m in self.materials:
            pbr: dict = {
                "baseColorFactor": [round(float(c), 6) for c in m.base_color],
                "metallicFactor": round(float(m.metallic), 6),
                "roughnessFactor": round(float(m.roughness), 6),
            }
            if m.texture is not None:
                pbr["baseColorTexture"] = {"index": m.texture}
            entry: dict = {"name": m.name, "pbrMetallicRoughness": pbr, "doubleSided": bool(m.double_sided)}
            if any(e > 0 for e in m.emissive):
                entry["emissiveFactor"] = [round(float(e), 6) for e in m.emissive]
            if m.alpha_mode != "OPAQUE":
                entry["alphaMode"] = m.alpha_mode
                if m.alpha_mode == "MASK":
                    entry["alphaCutoff"] = round(float(m.alpha_cutoff), 6)
            if m.clearcoat > 0:
                uses_clearcoat = True
                entry["extensions"] = {"KHR_materials_clearcoat": {
                    "clearcoatFactor": round(float(m.clearcoat), 6),
                    "clearcoatRoughnessFactor": round(float(m.clearcoat_roughness), 6),
                }}
            materials.append(entry)

        gltf: dict = {
            "asset": {"version": "2.0", "generator": "ApexSim ac_car_import"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"name": "body", "mesh": 0}],
            "meshes": [{"name": "body", "primitives": prims}],
            "materials": materials,
            "accessors": accessors,
            "bufferViews": views,
            "buffers": [{"byteLength": 0}],
        }
        if images:
            gltf["images"] = images
            gltf["samplers"] = [{"magFilter": LINEAR, "minFilter": LINEAR_MIPMAP_LINEAR, "wrapS": REPEAT, "wrapT": REPEAT}]
            gltf["textures"] = [{"sampler": 0, "source": i} for i in range(len(images))]
        if uses_clearcoat:
            gltf["extensionsUsed"] = ["KHR_materials_clearcoat"]
        while len(blob) % 4:
            blob.append(0)
        gltf["buffers"][0]["byteLength"] = len(blob)

        js = json.dumps(gltf, separators=(",", ":"), ensure_ascii=True).encode("ascii")
        js += b" " * ((4 - len(js) % 4) % 4)
        total = 12 + 8 + len(js) + 8 + len(blob)
        out = bytearray(struct.pack("<III", GLB_MAGIC, 2, total))
        out += struct.pack("<II", len(js), CHUNK_JSON) + js
        out += struct.pack("<II", len(blob), CHUNK_BIN) + blob
        return bytes(out)

    def write(self, path: Path) -> int:
        data = self.to_bytes()
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(data)
        return len(data)


def read_glb_json(path: Path) -> dict:
    """The JSON chunk of a GLB (for the checks and the tests)."""
    data = Path(path).read_bytes()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2:
        raise ValueError(f"{path}: not a glTF 2 binary")
    length, kind = struct.unpack_from("<II", data, 12)
    if kind != CHUNK_JSON:
        raise ValueError(f"{path}: first chunk is not JSON")
    return json.loads(data[20:20 + length])
