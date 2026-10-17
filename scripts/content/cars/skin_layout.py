"""The projected body skin of a generated car: where each `car_skin` face
lands in the skin texture, shared by the Blender build (which writes the
UVs) and the livery painter (which draws in the same frame).

Plain Python, no bpy: both sides import it.

A skin is one square image, SIZE pixels, in five regions. Each face of a
`car_skin` slot is projected flat onto the region its normal faces, so a
livery is painted as four drawings of the car - two flanks, a plan and the
two ends - in metres, the way a livery designer's sheet is laid out:

    +---------------------------------------+
    | LEFT  flank (+X, nose at the left)    |   v 0.67 .. 1.00
    +---------------------------------------+
    | RIGHT flank (-X, nose at the right)   |   v 0.34 .. 0.66
    +-----------------------------+---------+
    | TOP plan (nose at the left, | FRONT   |   v 0.17 .. 0.33
    | the car's left at the foot) +---------+
    |                             | REAR    |   v 0.00 .. 0.16
    +-----------------------------+---------+

Each flank and end is laid out as someone standing beside it sees it, so
text drawn upright in the region reads true on the car, on both sides.
UVs are Blender's (v up); the painter flips v to image rows. The build frame
is the cars' own: nose on -Y, +X the car's left (the driver's side), z up.

The `box` both sides use is (y_nose, y_tail, half_width, height), metres:
the car's extent, held fixed per car so the painter and the build agree
whatever the mesh adds.
"""

SIZE = 2048

# (u0, v0, u1, v1) in Blender UV space.
LEFT = (0.0, 0.67, 1.0, 1.0)
RIGHT = (0.0, 0.34, 1.0, 0.66)
TOP = (0.0, 0.0, 0.725, 0.33)
FRONT = (0.735, 0.17, 1.0, 0.33)
REAR = (0.735, 0.0, 1.0, 0.16)
REGIONS = dict(left=LEFT, right=RIGHT, top=TOP, front=FRONT, rear=REAR)

# A face whose normal points this far up is on the plan; one this far
# along the car is on an end; the rest are flank.
UP_FROM = 0.45
END_FROM = 0.55
# Kept off a region's edge so the mips do not bleed into the next one.
INSET = 0.004


def _clamp01(t):
    return 0.0 if t < 0.0 else 1.0 if t > 1.0 else t


def place(region, a, b):
    """(u, v) of the point `a` along and `b` up a region (both 0..1)."""
    u0, v0, u1, v1 = REGIONS[region]
    a = INSET + (1.0 - 2.0 * INSET) * _clamp01(a)
    b = INSET + (1.0 - 2.0 * INSET) * _clamp01(b)
    return (u0 + (u1 - u0) * a, v0 + (v1 - v0) * b)


def local(region, box, x, y, z):
    """Where the build-frame point (x, y, z) lies in `region`, as (a, b),
    0..1 along and up it (the inverse of what the painter draws)."""
    y0, y1, hw, h = box
    length = y1 - y0
    if region == "left":
        return ((y - y0) / length, z / h)
    if region == "right":
        return ((y1 - y) / length, z / h)
    if region == "top":
        return ((y - y0) / length, (hw - x) / (2.0 * hw))
    if region == "front":
        return ((x + hw) / (2.0 * hw), z / h)
    return ((hw - x) / (2.0 * hw), z / h)


def region_of(normal, centre):
    """The region a face with this normal and centre is projected on."""
    nx, ny, nz = normal
    if nz > UP_FROM:
        return "top"
    if abs(ny) > END_FROM and abs(ny) > abs(nx):
        return "front" if ny < 0.0 else "rear"
    return "left" if centre[0] >= 0.0 else "right"


def uv(normal, centre, point, box):
    """The UV of `point` on a face with this normal and centre."""
    region = region_of(normal, centre)
    return place(region, *local(region, box, *point))


# ------------------------------------------------------------- the painter's side
def to_px(region, a, b, size=SIZE):
    """Image pixel (column, row) of the point `a` along and `b` up a region."""
    u, v = place(region, a, b)
    return (u * size, (1.0 - v) * size)


def region_px(region, size=SIZE):
    """The region's pixel rectangle (left, top, right, bottom)."""
    u0, v0, u1, v1 = REGIONS[region]
    return (u0 * size, (1.0 - v1) * size, u1 * size, (1.0 - v0) * size)
