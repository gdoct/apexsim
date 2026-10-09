r"""Night windows for the ten `skyline_*` backdrop buildings (content/props/building).

The skyline GLBs were first built in Blender (their build script is not in the
repo, only the .blend/.glb pairs) with flat `skyline_glass_*` colours and no
emissive map, so the Unreal night pass (ApexPropLibrary.cpp `NightGlowOf`,
which lights a slot by NAME) left them black.  This script rewrites the GLBs
in place, with no Blender needed (pure numpy):

  skyline_glass_blue   -> mb_glass_blue
  skyline_glass_teal   -> mb_glass_teal
  skyline_glass_bronze -> mb_glass_bronze
  skyline_glass_grey   -> mb_glass_grey
  skyline_glass_green  -> mb_glass_teal   (nearest hue; no mb_glass_green exists)

The mb_glass_* slots are the Marina Bay curtain-wall atlases
(marina_common.facade_material): colour / roughness / a warm lit-window
emissive map, tiled in METRES through KHR_texture_transform (scale 1/tile,
offset (0, 1 - 1/tile): Blender's V flip), so a facade's UVs here are
(metres along the face, 1 - metres up the face).  Floor lines therefore fall
on the model's y = 0 grid; each facade gets a whole-cell random shift so
neighbouring faces and buildings do not repeat the same lit pattern.
Triangles are un-shared first so every face carries its own UVs; curved
(cylinder) facades are unrolled by angle with the circumference rounded to a
whole number of tiles so there is no seam; near-horizontal glass faces are
pinned to a mullion strip (never lit).  Everything else (positions, normals,
other slots, pivots) is rewritten untouched.  The .blend files next to the
GLBs are NOT updated (no bpy here).

Run:  python -I scripts/content/props/skyline_night.py [asset ...]
Idempotent (re-reads its own output).
"""
import json, math, os, struct, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from _skyline_pngio import decode_rgb8, encode_rgb8

ROOT = os.environ.get("APEXSIM_ROOT", os.path.abspath(os.path.join(HERE, "..", "..", "..")))
BUILDING = os.path.join(ROOT, "content", "props", "building")
TEXDIR = os.path.join(ROOT, "content", "props", "_textures")
ASSETS = ["slab_a", "slab_b", "step", "twin", "pyramid", "cylinder", "podium", "needle", "lowrise", "crane"]

MAP = {
    "skyline_glass_blue": "mb_glass_blue", "mb_glass_blue": "mb_glass_blue",
    "skyline_glass_teal": "mb_glass_teal", "mb_glass_teal": "mb_glass_teal",
    "skyline_glass_green": "mb_glass_teal",
    "skyline_glass_bronze": "mb_glass_bronze", "mb_glass_bronze": "mb_glass_bronze",
    "skyline_glass_grey": "mb_glass_grey", "mb_glass_grey": "mb_glass_grey",
    "mb_glass_clear": "mb_glass_clear",
}
# cols, rows, tile_m, mullion  (as marina_common.mb_mats builds them)
ATLAS = {
    "mb_glass_blue": (8, 4, 16.0, 0.07), "mb_glass_teal": (8, 4, 16.0, 0.07),
    "mb_glass_bronze": (8, 4, 16.0, 0.07), "mb_glass_grey": (6, 3, 18.0, 0.07),
    "mb_glass_clear": (4, 2, 12.0, 0.04),
}
METALLIC = 0.25


def read_glb(p):
    d = open(p, "rb").read()
    jl = struct.unpack_from("<I", d, 12)[0]
    j = json.loads(d[20:20 + jl]); off = 20 + jl
    bl = struct.unpack_from("<I", d, off)[0]
    return j, d[off + 8: off + 8 + bl]


def write_glb(p, j, b):
    js = json.dumps(j, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    b += b"\0" * (-len(b) % 4)
    total = 12 + 8 + len(js) + 8 + len(b)
    open(p, "wb").write(struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js
                        + struct.pack("<II", len(b), 0x004E4942) + b)


def accessor(j, b, i):
    a = j["accessors"][i]; bv = j["bufferViews"][a["bufferView"]]
    dt = {5126: "<f4", 5123: "<u2", 5125: "<u4"}[a["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
    o = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    return np.frombuffer(b, dt, a["count"] * n, o).reshape(a["count"], n).copy()


def tex_images(mb):
    """(emissive, base colour, metallicRoughness) PNG bytes for an mb_glass slot."""
    emis = open(os.path.join(TEXDIR, mb + "_emis.png"), "rb").read()
    col = open(os.path.join(TEXDIR, mb + "_col.png"), "rb").read()
    r = decode_rgb8(open(os.path.join(TEXDIR, mb + "_rough.png"), "rb").read())
    g = r[..., 1]   # glTF packing as the Blender exporter writes it: R=255, G=roughness, B=255
    packed = np.stack([np.full_like(g, 255), g, np.full_like(g, 255)], -1)
    return emis, col, encode_rgb8(packed)


def facade_uv(name, mb, pos, nrm, idx):
    """Un-share the triangles and give each a metre-based facade UV -> pos, nrm, uv, idx."""
    cols, rows, tile, mull = ATLAS[mb]
    cw, ch = tile / cols, tile / rows
    mx_u = tile * 0.5 * int((1024 // cols) * mull) / 1024.0          # middle of the first mullion, metres
    tris = idx.reshape(-1, 3)
    P = pos[tris].reshape(-1, 3); N = nrm[tris].reshape(-1, 3)
    UV = np.zeros((len(P), 2), np.float32)
    cx = 0.5 * (pos[:, 0].min() + pos[:, 0].max()); cz = 0.5 * (pos[:, 2].min() + pos[:, 2].max())
    rad = float(np.mean(np.hypot(pos[:, 0] - cx, pos[:, 2] - cz)))
    turns = max(1, round(2 * math.pi * rad / tile))
    up = np.array([0.0, 1.0, 0.0])
    side = nrm[np.abs(nrm[:, 1]) < 0.5]                 # a faceted cylinder has many wall directions: unroll it
    round_tower = len({int(round(math.degrees(math.atan2(n[2], n[0])))) for n in side}) > 8
    for t in range(len(tris)):
        p = P[3 * t:3 * t + 3].astype(np.float64); n = N[3 * t:3 * t + 3].astype(np.float64)
        gn = np.cross(p[1] - p[0], p[2] - p[0]); gl = np.linalg.norm(gn)
        gn = gn / gl if gl > 1e-12 else n[0]
        if np.dot(gn, n.mean(0)) < 0:
            gn = -gn
        ang = max(math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(n[0], n[k])))))) for k in (1, 2))
        h = np.hypot(p[:, 0] - cx, p[:, 2] - cz)
        if abs(gn[1]) > 0.92:          # roof / floor slab: pinned to a mullion strip, never lit
            UV[3 * t:3 * t + 3] = (mx_u, 1.0)
            continue
        if (ang > 3.0 or round_tower) and h.min() > 1e-3:    # curved facade: unroll by angle
            a = np.arctan2(p[:, 2] - cz, p[:, 0] - cx)
            a = a[0] + (a - a[0] + math.pi) % (2 * math.pi) - math.pi
            rng = np.random.default_rng(zlib.crc32(name.encode()))
            u = a / (2 * math.pi) * turns * tile + rng.integers(0, cols) * cw
            v = p[:, 1] + rng.integers(0, rows) * ch
        else:
            nn = gn.copy(); hn = math.hypot(nn[0], nn[2])
            tan = np.array([nn[2], 0.0, -nn[0]]) / hn
            s = up - nn * np.dot(up, nn); s /= np.linalg.norm(s)
            key = "%s|%d|%.1f" % (name, round(math.atan2(nn[2], nn[0]) / (math.pi / 8)), float(np.dot(p[0], nn)))
            rng = np.random.default_rng(zlib.crc32(key.encode()))
            u = p @ tan + rng.integers(0, cols) * cw
            v = p @ s + rng.integers(0, rows) * ch
        for k in range(3):
            UV[3 * t + k] = (u[k], 1.0 - v[k])
    return P.astype(np.float32), N.astype(np.float32), UV, np.arange(len(P), dtype=np.uint32)


def process(asset):
    path = os.path.join(BUILDING, "skyline_%s.glb" % asset)
    j, b = read_glb(path)
    old_mats = j["materials"]
    out_buf = bytearray(); bviews = []; accs = []

    def pad():
        while len(out_buf) % 4:
            out_buf.append(0)

    def add(arr, comp, typ, target, minmax=False):
        pad()
        raw = np.ascontiguousarray(arr).tobytes()
        bviews.append({"buffer": 0, "byteLength": len(raw), "byteOffset": len(out_buf), "target": target})
        out_buf.extend(raw)
        a = {"bufferView": len(bviews) - 1, "componentType": comp, "count": int(len(arr)), "type": typ}
        if minmax:
            a["max"] = [float(x) for x in arr.max(0)]; a["min"] = [float(x) for x in arr.min(0)]
        accs.append(a)
        return len(accs) - 1

    new_mats = []; mat_index = {}; images = []; textures = []; changed = 0
    ext_used = set(j.get("extensionsUsed", [])) - {"KHR_texture_transform"}

    def mat_for(old_i):
        om = old_mats[old_i]; mb = MAP.get(om["name"])
        key = mb or ("keep", old_i)
        if key in mat_index:
            return mat_index[key]
        if mb is None:
            m = dict(om)
        else:
            tile = ATLAS[mb][2]
            tr = {"KHR_texture_transform": {"offset": [0, 1 - 1 / tile], "scale": [1 / tile, 1 / tile]}}
            def tx(data, nm):
                images.append((nm, data)); textures.append({"sampler": 0, "source": len(images) - 1})
                return {"extensions": json.loads(json.dumps(tr)), "index": len(textures) - 1}
            emis, col, rough = tex_images(mb)
            m = {"doubleSided": True, "emissiveFactor": [1, 1, 1], "emissiveTexture": tx(emis, mb + "_emis"), "name": mb,
                 "pbrMetallicRoughness": {"baseColorTexture": tx(col, mb + "_col"), "metallicFactor": METALLIC,
                                          "metallicRoughnessTexture": tx(rough, mb + "_rough")}}
            ext_used.add("KHR_texture_transform")
        new_mats.append(m); mat_index[key] = len(new_mats) - 1
        return mat_index[key]

    new_prims = []
    for pr in j["meshes"][0]["primitives"]:
        at = pr["attributes"]
        assert set(at) == {"POSITION", "NORMAL", "TEXCOORD_0"}, at
        pos = accessor(j, b, at["POSITION"]); nrm = accessor(j, b, at["NORMAL"])
        uv = accessor(j, b, at["TEXCOORD_0"]); idx = accessor(j, b, pr["indices"]).reshape(-1).astype(np.uint32)
        mb = MAP.get(old_mats[pr["material"]]["name"])
        if mb:
            pos, nrm, uv, idx = facade_uv("skyline_" + asset, mb, pos, nrm, idx); changed += 1
        comp = 5125 if idx.max() > 65535 else 5123
        ap = add(pos.astype("<f4"), 5126, "VEC3", 34962, True)
        an = add(nrm.astype("<f4"), 5126, "VEC3", 34962)
        au = add(uv.astype("<f4"), 5126, "VEC2", 34962)
        ai = add(idx.astype("<u2" if comp == 5123 else "<u4"), comp, "SCALAR", 34963)
        new_prims.append({"attributes": {"POSITION": ap, "NORMAL": an, "TEXCOORD_0": au}, "indices": ai,
                          "material": mat_for(pr["material"])})
    img_json = []
    for nm, data in images:
        pad()
        bviews.append({"buffer": 0, "byteLength": len(data), "byteOffset": len(out_buf)}); out_buf.extend(data)
        img_json.append({"bufferView": len(bviews) - 1, "mimeType": "image/png", "name": nm})
    j["meshes"][0]["primitives"] = new_prims
    j["materials"] = new_mats; j["accessors"] = accs; j["bufferViews"] = bviews
    j["buffers"] = [{"byteLength": len(out_buf)}]
    for k in ("images", "textures", "samplers", "extensionsUsed", "extensionsRequired"):
        j.pop(k, None)
    if images:
        j["images"] = img_json; j["textures"] = textures; j["samplers"] = [{"magFilter": 9729, "minFilter": 9987}]
        j["extensionsUsed"] = sorted(ext_used); j["extensionsRequired"] = ["KHR_texture_transform"]
    elif ext_used:
        j["extensionsUsed"] = sorted(ext_used)
    write_glb(path, j, bytes(out_buf))
    return changed, os.path.getsize(path)


if __name__ == "__main__":
    for a in (sys.argv[1:] or ASSETS):
        print(a, process(a))
