"""Damage parts for the shipped cars: writes each car's `[[damage_part]]`
tables into its car.toml (docs/CAR_MODELS.md, Damage parts).

    python scripts/content/cars/damage_parts.py                  # every car in content/cars/default
    python scripts/content/cars/damage_parts.py posh-gt3rs       # one car
    python scripts/content/cars/damage_parts.py --dry-run        # print the tables, write nothing
    python scripts/content/cars/damage_parts.py --preview        # also draw build/damage_parts/<folder>.png

Needs numpy (and matplotlib for --preview). Plain Python, not Blender: it
reads the body GLB the builders wrote.

A part is a box in the car's frame (metres: forward, left, up); the game
cuts every triangle whose centre lies in it out of the body into its own
mesh, and throws it off when its zone's damage reaches `detach_pct`. The
boxes are found from the geometry, not typed in, so a rebuilt car gets
boxes that fit it:

  - **The front.** An open-wheeler's front wing is the wide, low span ahead
    of the front tyres: the box starts where the body, scanned back from
    the nose, stops being wider than half the car, and takes the nose tip
    with it (a real nose comes off with its wing). A closed car loses its
    nose: everything ahead of the front tyres and below its splitter
    height (the lower nose, the splitter and the dive planes).
  - **The rear wing** sits over a gap: behind the rear axle, between the
    central pylon (or fin) and the endplates, the heights fall into
    clusters split by bands 4 cm tall with (nearly) nothing in them (the
    pylons and endplates crossing them are left out). The wing is the
    highest cluster over such a gap that stays clear of the axle (a cluster
    reaching forward to it is the deck) and holds a real share of the band
    (not a wing tip alone); the part is everything above the
    gap, back from the station where the body ahead stops reaching that
    high (never past the rear axle).

Look at `--preview` after a builder change: a box that takes a roof or
leaves half a wing behind shows at once. `ApexSim.Cars.Damage.RepoParts`
checks every part holds some bodywork and none most of the car.
"""
import json
import os
import struct
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
CARS_DIR = os.path.join(REPO, "content", "cars", "default")
PREVIEW_DIR = os.path.join(REPO, "build", "damage_parts")
TABLE = "[damage_part]"
LIVERY_MARKER = "# --- liveries:"

# The zone's damage, percent, at which a part comes off. A hit does
# 0.2 x (v - 2.5 m/s)^1.6 percent (server damage.rs): 30% is a 27 m/s
# shunt or a race of knocks; an F1 front wing is the most fragile thing on
# the grid, a GT3's bumper among the toughest.
DETACH = {
    "F1": {"front": 30, "rear": 45},
    "Hypercar": {"front": 45, "rear": 50},
    "LMP2": {"front": 45, "rear": 50},
    "GT3": {"front": 50, "rear": 55},
}
OPEN_WHEEL = {"F1"}
# Up to where a closed car's nose comes off, as a share of the car's height.
NOSE_HEIGHT_SHARE = {"Hypercar": 0.36, "LMP2": 0.36, "GT3": 0.38}
# Rear wings the gap rule cannot find, as (floor, front) in metres, from the
# side view (`--preview`). Kept across runs; a rebuilt car may need a look.
MANUAL_REAR_WING = {
    # The boot rises under the wing to within a centimetre of it all along
    # its chord, so no band of heights behind the axle is empty.
    "murcetes-amd-gt3": (1.04, -1.88),
}


# ---------------------------------------------------------------- the GLB
def load_triangles(path):
    """Every triangle's centre and its corners, car frame (forward, left, up), metres."""
    with open(path, "rb") as f:
        data = f.read()
    json_len = struct.unpack("<I", data[12:16])[0]
    gltf = json.loads(data[20:20 + json_len])
    off = 20 + json_len
    bin_len = struct.unpack("<I", data[off:off + 4])[0]
    blob = data[off + 8:off + 8 + bin_len]

    def accessor(i):
        a = gltf["accessors"][i]
        view = gltf["bufferViews"][a["bufferView"]]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        dtype = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a["componentType"]]
        start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = view.get("byteStride", 0)
        size = n * np.dtype(dtype).itemsize
        if stride and stride != size:
            raw = np.frombuffer(blob, dtype=np.uint8, count=stride * a["count"], offset=start).reshape(a["count"], stride)
            return raw[:, :size].copy().view(dtype).reshape(a["count"], n)
        return np.frombuffer(blob, dtype=dtype, count=a["count"] * n, offset=start).reshape(a["count"], n)

    tris = []
    for node in gltf["nodes"]:
        if "mesh" not in node:
            continue
        if any(k in node for k in ("matrix", "rotation", "scale")) or any(node.get("translation", [0, 0, 0])):
            sys.exit("%s: a node with a transform; this script reads the builders' flat GLBs only" % path)
        for prim in gltf["meshes"][node["mesh"]]["primitives"]:
            pos = accessor(prim["attributes"]["POSITION"]).astype(np.float64)
            idx = accessor(prim["indices"]).ravel() if "indices" in prim else np.arange(len(pos))
            tris.append(pos[idx].reshape(-1, 3, 3))
    corners = np.concatenate(tris)
    # glTF: +Z forward (the nose), +X left, +Y up.
    corners = corners[:, :, [2, 0, 1]]
    return corners.mean(axis=1), corners


def read_toml(path):
    """The few car.toml keys this needs: class and the [wheels] axles and radii."""
    out, table = {}, ""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line.startswith("["):
                table = line.strip("[]").strip()
                continue
            if "=" not in line:
                continue
            key, value = (s.strip() for s in line.split("=", 1))
            if table == "" and key == "class":
                out["class"] = value.strip('"')
            elif table == "" and key == "model":
                out["model"] = value.strip('"')
            elif table == "wheels" and key in ("front_axle_m", "rear_axle_m", "front_radius_m", "rear_radius_m"):
                out[key] = float(value)
    return out


# --------------------------------------------------------------- the boxes
def front_part(c, car, cls):
    """The front wing of an open-wheeler, the nose of a closed car: (name, min, max)."""
    fwd, left, up = c[:, 0], c[:, 1], c[:, 2]
    nose = fwd.max()
    half = np.abs(left).max()
    tyre_front = car["front_axle_m"] + car["front_radius_m"]
    if cls in OPEN_WHEEL:
        # Back from the nose while the low body is still wider than half the car.
        low = up < 0.5
        start = nose
        step = 0.02
        y = nose
        while y > tyre_front:
            s = low & (fwd > y - step) & (fwd <= y)
            if s.any() and np.abs(left[s]).max() > 0.55 * half:
                start = y - step
            elif start < nose:
                break
            y -= step
        lo = max(start - 0.02, tyre_front + 0.02)
        return "front_wing", [lo, -half - 0.1, up.min() - 0.1], [nose + 0.1, half + 0.1, 0.5]
    height = up.max() - up.min()
    top = up.min() + NOSE_HEIGHT_SHARE[cls] * height
    return "nose", [tyre_front + 0.08, -half - 0.1, up.min() - 0.1], [nose + 0.1, half + 0.1, top]


def rear_wing(c, car, folder):
    """The rear wing: everything above the gap under it, back from where the body below stops."""
    fwd, left, up = c[:, 0], c[:, 1], c[:, 2]
    tail = fwd.min()
    half = np.abs(left).max()
    if folder in MANUAL_REAR_WING:
        floor, front = MANUAL_REAR_WING[folder]
        return "rear_wing", [tail - 0.1, -half - 0.1, floor], [front, half + 0.1, up.max() + 0.1]
    height = up.max() - up.min()
    # Behind the rear tyres' middle, between the central pylon or fin and
    # the endplates: there the wing's main plane spans the band and the air
    # under it is empty.
    rear = fwd < car["rear_axle_m"] - 0.5 * car["rear_radius_m"]
    band = rear & (np.abs(left) > 0.08) & (np.abs(left) < 0.5 * half)
    if band.sum() < 100:
        return None
    step = 0.01
    bins = np.arange(up.min(), up.max() + step, step)
    counts, _ = np.histogram(up[band], bins=bins)
    empty = counts <= max(2, 0.01 * counts.max())
    # The band's heights fall into clusters split by gaps 4 cm tall or more
    # with (nearly) nothing in them. The wing is the cluster with the most
    # in it that sits over such a gap and does not run forward to the axle
    # (that is the deck or the bodywork); its floor is a centimetre under it.
    cutoff = car["rear_axle_m"] - 0.5 * car["rear_radius_m"]
    clusters = []  # (bottom bin, top bin) of each cluster that sits over a gap
    top, run = None, 0
    for i in range(len(counts) - 1, -1, -1):
        if not empty[i]:
            if top is None:
                top = i
            if run >= 4 and top is not None and top > i + run:
                clusters.append((i + run + 1, top))
                top = i
            run = 0
        else:
            run += 1
    # The highest that clears the axle and holds a real share of the band
    # (a wing tip or a light alone is not a wing).
    candidates = []
    for lo, hi in clusters:
        if bins[lo] < up.min() + 0.3 * height:
            continue
        sel = band & (up >= bins[lo]) & (up < bins[hi + 1])
        if sel.any() and fwd[sel].max() <= cutoff - 0.05:
            candidates.append((bins[lo] - 0.01, sel.sum()))
    enough = max(150, 0.15 * max((n for _, n in candidates), default=0))
    floor = next((f for f, n in candidates if n >= enough), None)
    if floor is None:
        return None
    # Forward from the tail while the body still reaches above the wing's
    # floor, across the gaps between the endplates and the planes (12 cm).
    front = tail
    y = tail
    misses = 0
    while y < car["rear_axle_m"]:
        s = (fwd >= y) & (fwd < y + 0.02)
        if s.any() and up[s].max() > floor:
            front = y + 0.02
            misses = 0
        else:
            misses += 1
            if misses > 6:
                break
        y += 0.02
    return "rear_wing", [tail - 0.1, -half - 0.1, floor], [front, half + 0.1, up.max() + 0.1]


def parts_for(folder):
    car_dir = os.path.join(CARS_DIR, folder)
    car = read_toml(os.path.join(car_dir, "car.toml"))
    cls = car.get("class", "")
    if cls not in DETACH or "front_axle_m" not in car:
        return car, None, None
    centres, corners = load_triangles(os.path.join(car_dir, car["model"]))
    parts = []
    name, lo, hi = front_part(centres, car, cls)
    parts.append((name, "front", DETACH[cls]["front"], lo, hi))
    wing = rear_wing(centres, car, folder)
    if wing:
        parts.append((wing[0], "rear", DETACH[cls]["rear"], wing[1], wing[2]))
    return car, parts, (centres, corners)


# ----------------------------------------------------------------- writing
def table_lines(parts):
    out = []
    for name, zone, detach, lo, hi in parts:
        out += [
            "[[damage_part]]",
            "# written by scripts/content/cars/damage_parts.py from the body's geometry",
            'name = "%s"' % name,
            'zone = "%s"' % zone,
            "detach_pct = %d" % detach,
            "min_m = [%.3f, %.3f, %.3f]   # forward, left, up" % tuple(lo),
            "max_m = [%.3f, %.3f, %.3f]" % tuple(hi),
            "",
        ]
    return out


def write_tables(toml_path, block):
    """Every `[[damage_part]]` table replaced by `block`, above the liveries'
    marker (which liveries.py rewrites below), as carlib.write_table does."""
    with open(toml_path, encoding="utf-8", newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.replace("\r\n", "\n").split("\n")
    out, skip = [], False
    for line in lines:
        head = line.strip()
        if head.startswith("["):
            skip = head == "[%s]" % TABLE
        elif head.startswith(LIVERY_MARKER):
            skip = False
        if not skip:
            out.append(line)
    marker = next((i for i, l in enumerate(out) if l.startswith(LIVERY_MARKER)), None)
    if marker is None:
        while out and not out[-1].strip():
            out.pop()
        out += [""] + block
    else:
        while marker > 0 and not out[marker - 1].strip():
            out.pop(marker - 1)
            marker -= 1
        out[marker:marker] = [""] + block
    while out and not out[-1].strip():
        out.pop()
    out.append("")
    with open(toml_path, "w", encoding="utf-8", newline="") as f:
        f.write(nl.join(out))


def preview(folder, parts, centres):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle

    os.makedirs(PREVIEW_DIR, exist_ok=True)
    fig, axes = plt.subplots(2, 1, figsize=(12, 7.5))
    colours = ["#d62728", "#1f77b4", "#2ca02c", "#9467bd"]
    inside = np.full(len(centres), -1)
    for k, (_, _, _, lo, hi) in enumerate(parts):
        hit = (inside < 0) & np.all((centres >= lo) & (centres <= hi), axis=1)
        inside[hit] = k
    for ax, (a, b, label) in zip(axes, [(0, 2, "side: forward / up"), (0, 1, "top: forward / left")]):
        ax.scatter(centres[inside < 0, a], centres[inside < 0, b], s=0.6, c="#9a9a9a", linewidths=0)
        for k, (name, _, detach, lo, hi) in enumerate(parts):
            sel = inside == k
            ax.scatter(centres[sel, a], centres[sel, b], s=1.2, c=colours[k], linewidths=0)
            ax.add_patch(Rectangle((lo[a], lo[b]), hi[a] - lo[a], hi[b] - lo[b], fill=False, ec=colours[k], lw=1.2))
            ax.text(lo[a], hi[b] + 0.02, "%s (%d%%, %d tris)" % (name, detach, sel.sum()), color=colours[k], fontsize=8)
        ax.set_aspect("equal")
        ax.set_title("%s %s" % (folder, label), fontsize=9)
    fig.tight_layout()
    path = os.path.join(PREVIEW_DIR, folder + ".png")
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return path


def main(argv):
    dry = "--dry-run" in argv
    want_preview = "--preview" in argv
    folders = [a for a in argv if not a.startswith("--")] or sorted(
        d for d in os.listdir(CARS_DIR) if os.path.isfile(os.path.join(CARS_DIR, d, "car.toml")))
    for folder in folders:
        car, parts, geometry = parts_for(folder)
        if not parts:
            print("%-28s no rules for class %r; left alone" % (folder, car.get("class")))
            continue
        block = table_lines(parts)
        summary = ", ".join("%s %d%%" % (p[0], p[2]) for p in parts)
        if dry:
            print("# %s\n%s" % (folder, "\n".join(block)))
        else:
            write_tables(os.path.join(CARS_DIR, folder, "car.toml"), block)
            print("%-28s %s" % (folder, summary))
        if want_preview:
            print("    ", preview(folder, parts, geometry[0]))


if __name__ == "__main__":
    main(sys.argv[1:])
