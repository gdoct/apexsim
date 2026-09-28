"""Reader (and a test writer) for Assetto Corsa's kn5 model files.

Layout (every integer little-endian, every string `i32 length + UTF-8`):

    "sc6969", i32 version (6; if > 5 one extra i32)
    i32 texture_count, then per texture: i32 active, str name, i32 size, bytes
    i32 material_count, then per material: str name, str shader,
        u8 alpha_blend_mode, u8 alpha_tested, i32 depth_mode,
        i32 prop_count x (str name, f32 value, 36 bytes),
        i32 slot_count x (str slot, i32 slot_index, str texture)
    the node tree, depth first: i32 type, str name, i32 child_count, u8 active,
        type 1 (dummy):   16 f32, a row-major 4x4 with the translation in
                          the last row (DirectX); a child's world transform
                          is `local @ parent`
        type 2 (mesh):    u8 x3, i32 vertex_count x 44 bytes (pos, normal,
                          uv, tangent), i32 index_count x u16, i32 material,
                          i32 layer, f32 lod_in, f32 lod_out, 4 f32 sphere,
                          u8 renderable
        type 3 (skinned): u8 x3, i32 bone_count x (str, 64 bytes), vertices
                          of 76 bytes (the 44 plus four bone weights and four
                          bone indices as f32), then indices, material, layer,
                          lod_in and lod_out as a mesh but no sphere and no
                          renderable byte; cars carry them (the belts, the
                          shift boot), tracks do not

A kn5 whose content was encrypted by Custom Shaders Patch has no `sc6969`
magic, or carries textures that are not images; it is refused, never
decrypted.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

MAGIC = b"sc6969"

VERTEX_DTYPE = np.dtype(
    [("pos", "<f4", 3), ("nrm", "<f4", 3), ("uv", "<f4", 2), ("tan", "<f4", 3)]
)
assert VERTEX_DTYPE.itemsize == 44

_IMAGE_MAGICS = (b"DDS ", b"\x89PNG", b"\xff\xd8\xff", b"BM")


class Kn5Error(Exception):
    pass


class EncryptedKn5(Kn5Error):
    """A kn5 whose content was encrypted by a mod author; refused outright."""


@dataclass
class Kn5Texture:
    name: str
    data: bytes

    @property
    def size(self) -> int:
        return len(self.data)


@dataclass
class Kn5Material:
    name: str
    shader: str
    alpha_blend: int
    alpha_tested: bool
    depth_mode: int
    props: dict[str, float]
    slots: dict[str, str]

    def texture(self, slot: str = "txDiffuse") -> str | None:
        name = self.slots.get(slot)
        return name or None


@dataclass
class Kn5Mesh:
    name: str
    parent: str
    #: Row-major 4x4, translation in the last row: world = local @ parent.
    transform: np.ndarray
    vertices: np.ndarray
    #: (n, 3) uint32 triangle corners.
    triangles: np.ndarray
    material: int
    layer: int
    lod_in: float
    lod_out: float
    renderable: bool
    active: bool

    @property
    def triangle_count(self) -> int:
        return int(self.triangles.shape[0])

    def world_positions(self) -> np.ndarray:
        """(n, 3) float64 positions with the node chain applied."""
        p = self.vertices["pos"].astype(np.float64)
        if _is_identity(self.transform):
            return p
        m = self.transform.astype(np.float64)
        return p @ m[:3, :3] + m[3, :3]

    def world_normals(self) -> np.ndarray:
        n = self.vertices["nrm"].astype(np.float64)
        if not _is_identity(self.transform):
            n = n @ self.transform.astype(np.float64)[:3, :3]
        length = np.linalg.norm(n, axis=1, keepdims=True)
        length[length == 0] = 1.0
        return n / length


@dataclass
class Kn5Dummy:
    name: str
    parent: str
    #: World transform, same convention as a mesh's.
    transform: np.ndarray
    active: bool

    @property
    def position(self) -> np.ndarray:
        return self.transform[3, :3].astype(np.float64)


@dataclass
class Kn5File:
    path: Path
    version: int
    textures: dict[str, Kn5Texture] = field(default_factory=dict)
    materials: list[Kn5Material] = field(default_factory=list)
    meshes: list[Kn5Mesh] = field(default_factory=list)
    dummies: list[Kn5Dummy] = field(default_factory=list)

    @property
    def triangle_count(self) -> int:
        return sum(m.triangle_count for m in self.meshes)


def _is_identity(m: np.ndarray) -> bool:
    return bool(np.allclose(m, np.eye(4, dtype=m.dtype), atol=1e-6))


class _Reader:
    __slots__ = ("data", "at")

    def __init__(self, data: bytes):
        self.data = data
        self.at = 0

    def take(self, n: int) -> bytes:
        end = self.at + n
        if n < 0 or end > len(self.data):
            raise Kn5Error(f"truncated kn5 at offset {self.at} (wanted {n} bytes)")
        out = self.data[self.at:end]
        self.at = end
        return out

    def i32(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def u8(self) -> int:
        return self.take(1)[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]

    def string(self) -> str:
        n = self.i32()
        if n < 0 or n > 1 << 20:
            raise Kn5Error(f"implausible string length {n} at offset {self.at - 4}")
        return self.take(n).decode("utf-8", "replace")


def read_kn5(path: Path, *, textures: bool = True) -> Kn5File:
    """Read a kn5. With `textures=False` the texture bytes are dropped after
    the table is walked (the physics kn5 needs none)."""
    path = Path(path)
    data = path.read_bytes()
    r = _Reader(data)
    if r.take(6) != MAGIC:
        raise EncryptedKn5(f"{path.name}: not a kn5 (no sc6969 magic); encrypted or damaged")
    version = r.i32()
    if version > 5:
        r.i32()
    out = Kn5File(path=path, version=version)

    for _ in range(r.i32()):
        # A protected mod's texture table does not parse as Kunos writes it
        # (the RSS cars carry an extra field in every record), so a table
        # that runs off the file is reported as encryption, not damage.
        try:
            r.i32()  # active
            name = r.string()
            size = r.i32()
            blob = r.take(size)
        except Kn5Error as e:
            raise EncryptedKn5(
                f"{path.name}: the texture table is unreadable ({e}); the file looks encrypted"
            ) from None
        # startswith, not a four-byte slice: the JPEG and BMP magics are
        # shorter than four bytes and a slice compare never matched them,
        # which refused every kn5 carrying a JPEG as "encrypted".
        if size >= 4 and not blob.startswith(_IMAGE_MAGICS):
            raise EncryptedKn5(
                f"{path.name}: texture {name!r} is not an image; the file looks encrypted"
            )
        if textures:
            out.textures[name] = Kn5Texture(name, bytes(blob))

    for _ in range(r.i32()):
        name = r.string()
        shader = r.string()
        alpha_blend = r.u8()
        alpha_tested = bool(r.u8())
        depth_mode = r.i32()
        props: dict[str, float] = {}
        for _ in range(r.i32()):
            pname = r.string()
            props[pname] = r.f32()
            r.take(36)
        slots: dict[str, str] = {}
        for _ in range(r.i32()):
            slot = r.string()
            r.i32()
            slots[slot] = r.string()
        out.materials.append(
            Kn5Material(name, shader, alpha_blend, alpha_tested, depth_mode, props, slots)
        )

    identity = np.eye(4, dtype=np.float32)

    def node(parent_transform: np.ndarray, parent_name: str) -> None:
        kind = r.i32()
        name = r.string()
        children = r.i32()
        active = bool(r.u8())
        transform = parent_transform
        if kind == 1:
            local = np.frombuffer(r.take(64), dtype="<f4").reshape(4, 4)
            transform = local @ parent_transform
            out.dummies.append(Kn5Dummy(name, parent_name, transform, active))
        elif kind in (2, 3):
            r.take(3)
            if kind == 3:
                for _ in range(r.i32()):
                    r.string()
                    r.take(64)
            nverts = r.i32()
            stride = 76 if kind == 3 else 44
            raw = r.take(nverts * stride)
            if kind == 3:
                # Skinned vertices carry bone weights after the tangent;
                # take the leading 44 bytes of each.
                raw = np.frombuffer(raw, dtype=np.uint8).reshape(nverts, 76)[:, :44].tobytes()
            vertices = np.frombuffer(raw, dtype=VERTEX_DTYPE).copy()
            nidx = r.i32()
            indices = np.frombuffer(r.take(nidx * 2), dtype="<u2").astype(np.uint32)
            material = r.i32()
            layer = r.i32()
            lod_in = r.f32()
            lod_out = r.f32()
            renderable = True
            if kind == 2:
                r.take(16)
                renderable = bool(r.u8())
            if material < 0 or material >= len(out.materials):
                raise Kn5Error(
                    f"{path.name}: mesh {name!r} names material {material} of {len(out.materials)}"
                )
            if nidx % 3 != 0:
                raise Kn5Error(f"{path.name}: mesh {name!r} has {nidx} indices")
            if nverts and indices.size and int(indices.max()) >= nverts:
                raise Kn5Error(f"{path.name}: mesh {name!r} indexes past its {nverts} vertices")
            out.meshes.append(
                Kn5Mesh(
                    name=name,
                    parent=parent_name,
                    transform=transform,
                    vertices=vertices,
                    triangles=indices.reshape(-1, 3),
                    material=material,
                    layer=layer,
                    lod_in=lod_in,
                    lod_out=lod_out,
                    renderable=renderable,
                    active=active,
                )
            )
        else:
            raise Kn5Error(f"{path.name}: unknown node type {kind} at offset {r.at}")
        for _ in range(children):
            node(transform, name)

    node(identity, "")
    if r.at != len(data):
        # Custom Shaders Patch's car protection appends its encrypted payload
        # after an intact tree as named blocks (`acd.checksum.e`, ...): the
        # geometry reads, but it is not the car the author shipped.
        if b".checksum" in data[r.at:r.at + 64]:
            raise EncryptedKn5(
                f"{path.name}: carries Custom Shaders Patch encrypted data after the model"
            )
        raise Kn5Error(f"{path.name}: {len(data) - r.at} bytes after the node tree")
    return out


def write_kn5(path: Path, textures: dict[str, bytes], materials: list[Kn5Material],
              nodes: list) -> None:
    """Write a kn5 (for the tests: a synthetic track). `nodes` is a tree of
    `("dummy", name, matrix4x4, [children])` and
    `("mesh", name, vertices(VERTEX_DTYPE), triangles(n,3), material, lod_in,
    lod_out, renderable)` tuples under an implicit root dummy; a
    `("skinned", name, vertices, triangles, material, lod_in, lod_out,
    bone_names)` tuple writes a type 3 node (zero weights, identity bones)."""
    out = bytearray()

    def s(text: str) -> None:
        b = text.encode("utf-8")
        out.extend(struct.pack("<i", len(b)))
        out.extend(b)

    out.extend(MAGIC)
    out.extend(struct.pack("<ii", 6, 0))
    out.extend(struct.pack("<i", len(textures)))
    for name, blob in textures.items():
        out.extend(struct.pack("<i", 1))
        s(name)
        out.extend(struct.pack("<i", len(blob)))
        out.extend(blob)
    out.extend(struct.pack("<i", len(materials)))
    for m in materials:
        s(m.name)
        s(m.shader)
        out.extend(struct.pack("<BBi", m.alpha_blend, int(m.alpha_tested), m.depth_mode))
        out.extend(struct.pack("<i", len(m.props)))
        for k, v in m.props.items():
            s(k)
            out.extend(struct.pack("<f", v))
            out.extend(bytes(36))
        out.extend(struct.pack("<i", len(m.slots)))
        for k, v in m.slots.items():
            s(k)
            out.extend(struct.pack("<i", 0))
            s(v)

    def node(n) -> None:
        if n[0] == "dummy":
            _, name, matrix, children = n
            out.extend(struct.pack("<i", 1))
            s(name)
            out.extend(struct.pack("<iB", len(children), 1))
            out.extend(np.asarray(matrix, dtype="<f4").reshape(16).tobytes())
            for c in children:
                node(c)
        else:
            skinned = n[0] == "skinned"
            _, name, vertices, triangles, material, lod_in, lod_out, extra = n
            out.extend(struct.pack("<i", 3 if skinned else 2))
            s(name)
            out.extend(struct.pack("<iB", 0, 1))
            out.extend(bytes(3))
            if skinned:
                out.extend(struct.pack("<i", len(extra)))
                for bone in extra:
                    s(bone)
                    out.extend(np.eye(4, dtype="<f4").tobytes())
            vertices = np.asarray(vertices, dtype=VERTEX_DTYPE)
            out.extend(struct.pack("<i", len(vertices)))
            if skinned:
                raw = np.zeros((len(vertices), 76), dtype=np.uint8)
                raw[:, :44] = np.frombuffer(vertices.tobytes(), dtype=np.uint8).reshape(-1, 44)
                out.extend(raw.tobytes())
            else:
                out.extend(vertices.tobytes())
            idx = np.asarray(triangles, dtype="<u2").reshape(-1)
            out.extend(struct.pack("<i", len(idx)))
            out.extend(idx.tobytes())
            out.extend(struct.pack("<iiff", material, 0, lod_in, lod_out))
            if not skinned:
                out.extend(bytes(16))
                out.extend(struct.pack("<B", int(extra)))

    node(("dummy", "root", np.eye(4), nodes))
    Path(path).write_bytes(bytes(out))
