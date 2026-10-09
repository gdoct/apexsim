r"""Marina Bay batch A3 - civic / hotel / landmark buildings and the monument.

    ASSET = "all"   # or one of BUILD
    exec(open(r"E:\apexsim\scripts\content\props\build_marina_bay_a3.py").read())
Originals only: no trademarked sculptures (the Merlion is deliberately not modelled).
"""
exec(open(r"E:\apexsim\scripts\content\props\marina_common.py").read())
try:
    ASSET
except NameError:
    ASSET = "all"


def steps(b, m, x0, x1, y_front, n=4, h=0.18, d=0.4, mat="stone"):
    for i in range(n):
        b.box(m[mat], (x0 - i * 0.3, y_front - (i + 1) * d, 0), (x1 + i * 0.3, y_front, h * (n - i)))


def balustrade(b, m, x0, x1, y0, y1, z, h=0.9):
    for (ax, ay, bx, by) in ((x0, y0, x1, y0), (x0, y1, x1, y1), (x0, y0, x0, y1), (x1, y0, x1, y1)):
        b.box(m["stone"], (min(ax, bx) - 0.08, min(ay, by) - 0.08, z + h - 0.1), (max(ax, bx) + 0.08, max(ay, by) + 0.08, z + h))
    for x in np.arange(x0, x1 + 0.01, 1.6):
        for y in (y0, y1):
            b.box(m["stone"], (x - 0.1, y - 0.1, z), (x + 0.1, y + 0.1, z + h))


# --------------------------------------------------------- building/building_colonnade_hotel
def colonnade_hotel():
    m = mb_mats(); b = B("building_colonnade_hotel")
    L, D, H = 96.0, 54.0, 21.0
    b.box(m["stone"], (-L / 2 - 0.5, -D / 2 - 0.5, 0), (L / 2 + 0.5, D / 2 + 0.5, 1.4))                # plinth
    b.box(m["classic"], (-L / 2, -D / 2, 1.4), (L / 2, D / 2, H))
    b.box(m["stone"], (-L / 2 - 0.7, -D / 2 - 0.7, H), (L / 2 + 0.7, D / 2 + 0.7, H + 1.3))             # cornice
    frustum(b, m["tile"], (-L / 2 - 0.4, L / 2 + 0.4, -D / 2 - 0.4, D / 2 + 0.4), (-L / 2 + 14, L / 2 - 14, -D / 2 + 12, D / 2 - 12), H + 1.3, H + 4.4)
    # two-storey Doric colonnade across the front, recessed gallery behind
    xs = np.arange(-40.5, 40.6, 4.5)
    yc = -D / 2 - 3.0
    b.box(m["stone"], (-42, yc - 0.2, 1.4), (42, -D / 2, 1.8))                                          # gallery floor
    colonnade(b, m["stone"], m["stone"], xs, yc, 1.8, 9.6, r=0.5)
    b.box(m["stone"], (-42.2, yc - 0.6, 9.6), (42.2, -D / 2 + 0.3, 10.4))                               # first entablature
    colonnade(b, m["stone"], m["stone"], xs, yc, 10.4, 18.4, r=0.45)
    b.box(m["stone"], (-42.2, yc - 0.7, 18.4), (42.2, -D / 2 + 0.3, 19.5))                              # main entablature
    balustrade(b, m, -42.2, 42.2, yc - 0.55, yc - 0.55, 19.5, h=0.9)
    # central pediment, entrance steps
    gable(b, m["stone"], -9.2, 9.2, yc - 0.7, -D / 2 + 0.3, 19.5, 3.4)
    steps(b, m, -8, 8, yc - 0.2, n=4)
    # corner pavilions
    for s in (-1, 1):
        b.box(m["stone"], (s * (L / 2 - 6) - 6, -D / 2 - 0.9, 1.4), (s * (L / 2 - 6) + 6, -D / 2, 19.5))
    # chimneys / roof lanterns
    for x in (-30, -10, 10, 30):
        b.box(m["stone"], (x - 1.0, -2, H + 3.8), (x + 1.0, 0, H + 7.0))
    return b.finish()


# --------------------------------------------------------- building/building_domed_court
def domed_court():
    m = mb_mats(); b = B("building_domed_court")
    L, D, WH = 80.0, 50.0, 11.5
    b.box(m["stone"], (-L / 2 - 0.5, -D / 2 - 0.5, 0), (L / 2 + 0.5, D / 2 + 0.5, 1.2))
    b.box(m["classic"], (-L / 2, -D / 2, 1.2), (L / 2, D / 2, WH))
    b.box(m["stone"], (-L / 2 - 0.6, -D / 2 - 0.6, WH), (L / 2 + 0.6, D / 2 + 0.6, WH + 1.2))
    balustrade(b, m, -L / 2, L / 2, -D / 2, D / 2, WH + 1.2, h=0.9)
    # projecting end pavilions
    for s in (-1, 1):
        b.box(m["classic"], (s * (L / 2 - 9) - 9, -D / 2 - 2.0, 1.2), (s * (L / 2 - 9) + 9, -D / 2, WH))
    # hexastyle Corinthian portico + pediment + steps
    yc = -D / 2 - 5.0
    xs = np.linspace(-11.0, 11.0, 6)
    b.box(m["stone"], (-13.5, yc - 0.2, 1.2), (13.5, -D / 2, 1.7))
    colonnade(b, m["stone"], m["stone"], xs, yc, 1.7, 11.0, r=0.62)
    b.box(m["stone"], (-13.8, yc - 0.8, 11.0), (13.8, -D / 2 + 0.2, 12.4))
    gable(b, m["stone"], -13.8, 13.8, yc - 0.8, -D / 2 + 0.2, 12.4, 3.6)
    steps(b, m, -12.5, 12.5, yc - 0.2, n=5, h=0.24, d=0.5)
    # drum + dome + lantern
    pts = ellipse_pts(0, 0, 10.6, 10.6, 32)
    prism_ngon(b, m["classic"], pts, WH + 1.2, 16.6)
    prism_ngon(b, m["stone"], ellipse_pts(0, 0, 11.3, 11.3, 32), 16.6, 17.4)
    for k in range(16):
        a = 2 * math.pi * k / 16
        b.cylinder(m["stone"], (10.9 * math.cos(a), 10.9 * math.sin(a), WH + 1.2), 0.35, 4.6, segs=8)
    P = ellipsoid_pts((0, 0, 17.4), 10.2, 10.2, 5.8, 32, 8)
    shell(b, m["copper"], P, (0, 0, 17.4))
    b.cylinder(m["stone"], (0, 0, 23.0), 1.5, 2.6, segs=12)
    b.cone(m["copper"], (0, 0, 25.5), 1.7, 0.1, 2.4, segs=12)
    return b.finish(recalc=False)


# --------------------------------------------------------- building/building_colonnade_civic
def colonnade_civic():
    m = mb_mats(); b = B("building_colonnade_civic")
    L, D, H = 96.0, 44.0, 19.0
    b.box(m["stone"], (-L / 2 - 0.5, -D / 2 - 0.5, 0), (L / 2 + 0.5, D / 2 + 0.5, 1.4))
    b.box(m["classic"], (-L / 2, -D / 2, 1.4), (L / 2, D / 2, H))
    b.box(m["stone"], (-L / 2 - 0.6, -D / 2 - 0.6, H), (L / 2 + 0.6, D / 2 + 0.6, H + 1.4))
    balustrade(b, m, -L / 2 - 0.6, L / 2 + 0.6, -D / 2 - 0.6, D / 2 + 0.6, H + 1.4, h=1.0)
    yc = -D / 2 - 3.2
    xs = np.linspace(-44.0, 44.0, 18)
    b.box(m["stone"], (-45.5, yc - 0.3, 1.4), (45.5, -D / 2, 1.9))
    colonnade(b, m["stone"], m["stone"], xs, yc, 1.9, 17.6, r=0.78)
    b.box(m["stone"], (-45.8, yc - 1.0, 17.6), (45.8, -D / 2 + 0.3, 19.0))
    gable(b, m["stone"], -9.5, 9.5, yc - 1.0, -D / 2 + 0.3, 19.0, 3.0)
    steps(b, m, -10, 10, yc - 0.3, n=6, h=0.22, d=0.5)
    b.box(m["stone"], (-8, -2, H + 1.4), (8, 6, H + 4.2))                                       # attic over the centre
    return b.finish()


# --------------------------------------------------------- building/building_clock_tower_hall
def clock_tower_hall():
    m = mb_mats(); b = B("building_clock_tower_hall")
    # hall: two gabled wings left/right, entrance block with tower in front
    b.box(m["stone"], (-31.5, -16.5, 0), (31.5, 20.5, 1.0))
    for (x0, x1) in ((-31, -6), (6, 31)):
        b.box(m["classic"], (x0, -16, 1.0), (x1, 20, 14.5))
        gable(b, m["tile"], x0 - 0.6, x1 + 0.6, -16.6, 20.6, 14.5, 4.2, ridge='y')
    b.box(m["classic"], (-6, -16, 1.0), (6, 20, 17.5))                                         # centre block
    gable(b, m["tile"], -6.4, 6.4, -16.6, 20.6, 17.5, 3.0, ridge='x')
    # tower
    TX, TY, W = 0.0, -20.0, 9.0
    b.box(m["stone"], (TX - W / 2 - 0.4, TY - W / 2, 0), (TX + W / 2 + 0.4, TY + W / 2 + 4, 1.2))
    b.box(m["classic"], (TX - W / 2, TY - W / 2, 1.2), (TX + W / 2, TY + W / 2, 27))
    b.box(m["stone"], (TX - W / 2 - 0.3, TY - W / 2 - 0.3, 27), (TX + W / 2 + 0.3, TY + W / 2 + 0.3, 28))
    b.box(m["stone"], (TX - W / 2 + 0.4, TY - W / 2 + 0.4, 28), (TX + W / 2 - 0.4, TY + W / 2 - 0.4, 40))   # clock stage
    for (dx, dy, ax) in ((0, -1, 'Y'), (0, 1, 'Y'), (-1, 0, 'X'), (1, 0, 'X')):
        cx, cy, cz = TX + dx * (W / 2 - 0.4), TY + dy * (W / 2 - 0.4), 34.0
        sg = dx + dy                                      # outward sign along the face normal
        if ax == 'Y':
            b.cylinder(m["steel_dark"], (cx, cy + (0.0 if sg > 0 else -0.14), cz), 2.9, 0.14, segs=28, axis='Y')
            yy = cy + (0.14 if sg > 0 else -0.14)
            b.cylinder(m["white"], (cx, yy - (0.0 if sg > 0 else 0.02), cz), 2.6, 0.04, segs=28, axis='Y')
            yh = yy + (0.06 if sg > 0 else -0.08)
            b.box(m["steel_dark"], (cx - 0.1, yh, cz), (cx + 0.1, yh + 0.04, cz + 2.0))
            b.box(m["steel_dark"], (cx, yh, cz - 0.1), (cx + 1.4, yh + 0.04, cz + 0.1))
        else:
            b.cylinder(m["steel_dark"], (cx + (0.0 if sg > 0 else -0.14), cy, cz), 2.9, 0.14, segs=28, axis='X')
            xx = cx + (0.14 if sg > 0 else -0.14)
            b.cylinder(m["white"], (xx - (0.0 if sg > 0 else 0.02), cy, cz), 2.6, 0.04, segs=28, axis='X')
            xh = xx + (0.06 if sg > 0 else -0.08)
            b.box(m["steel_dark"], (xh, cy - 0.1, cz), (xh + 0.04, cy + 0.1, cz + 2.0))
            b.box(m["steel_dark"], (xh, cy, cz - 0.1), (xh + 0.04, cy + 1.4, cz + 0.1))
    frustum(b, m["copper"], (TX - W / 2 - 0.2, TX + W / 2 + 0.2, TY - W / 2 - 0.2, TY + W / 2 + 0.2), (TX - 0.5, TX + 0.5, TY - 0.5, TY + 0.5), 40.0, 51.5)
    b.bar(m["steel"], (TX, TY, 51.5), (TX, TY, 56.0), 0.12, segs=6)
    # entrance portico between tower and hall
    b.box(m["stone"], (-9, -24.5, 0), (9, -15.5, 0.9))
    colonnade(b, m["stone"], m["stone"], np.linspace(-7.5, 7.5, 4), -24.0, 0.9, 6.6, r=0.45)
    b.box(m["stone"], (-9, -24.6, 6.6), (9, -15.6, 7.4))
    return b.finish()


# --------------------------------------------------------- building/building_five_towers
def five_towers():
    m = mb_mats(); b = B("building_five_towers")
    # podium + convention hall
    b.box(m["stone"], (-72, -36, 0), (72, 36, 1.0))
    b.box(m["glass_clear"], (-70, -34, 1.0), (70, 30, 11.0))
    b.box(m["white"], (-72, -36, 11.0), (72, 34, 12.0))
    for x in range(-68, 70, 8):
        b.cylinder(m["steel"], (x, -36.8, 1.0), 0.3, 10.0, segs=8)
    # five towers on an arc ("the hand")
    spec = [(-56, 6, 18.5, 58.0), (-28, 0, 20.0, 146.0), (0, -4, 21.0, 160.0), (28, 0, 20.0, 150.0), (56, 6, 18.5, 138.0)]
    for (x, y, w, h) in spec:
        c = 3.2
        pts = [(x - w / 2 + c, y - w / 2), (x + w / 2 - c, y - w / 2), (x + w / 2, y - w / 2 + c), (x + w / 2, y + w / 2 - c),
               (x + w / 2 - c, y + w / 2), (x - w / 2 + c, y + w / 2), (x - w / 2, y + w / 2 - c), (x - w / 2, y - w / 2 + c)]
        prism_ngon(b, m["glass_grey"], pts, 12.0, h)
        for zb in range(36, int(h) - 6, 36):
            b.box(m["white"], (x - w / 2 - 0.25, y - w / 2 - 0.25, zb), (x + w / 2 + 0.25, y + w / 2 + 0.25, zb + 1.2))
        b.box(m["steel_dark"], (x - w / 2 + 2.5, y - w / 2 + 2.5, h), (x + w / 2 - 2.5, y + w / 2 - 2.5, h + 4.0))
        b.box(m["white"], (x - w / 2 - 0.3, y - w / 2 - 0.3, h - 1.2), (x + w / 2 + 0.3, y + w / 2 + 0.3, h))
        b.box(m["glass_clear"], (x - 4, y - w / 2 - 0.4, 1.0), (x + 4, y - w / 2 + 0.0, 12.0)) if False else None
    return b.finish()


# --------------------------------------------------------- misc/fountain_basin
def fountain_basin():
    m = mb_mats(); b = B("fountain_basin")
    R = 24.0
    prism_ngon(b, m["stone"], ellipse_pts(0, 0, R, R, 72), 0, 0.9)
    prism_ngon(b, m["water"], ellipse_pts(0, 0, R - 1.3, R - 1.3, 72), 0.9, 0.92)
    prism_ngon(b, m["concrete"], ellipse_pts(0, 0, 6.5, 6.5, 40), 0.9, 1.6)
    # bronze ring standing on the dais + jets
    torus_y(b, m["bronze"], (0, 0, 5.9), 4.0, 0.5, segs=48, rings=8)
    b.box(m["bronze"], (-1.2, -0.6, 1.6), (1.2, 0.6, 2.2))
    for k in range(18):
        a = 2 * math.pi * k / 18
        b.cone(m["white"], (13.5 * math.cos(a), 13.5 * math.sin(a), 0.92), 0.12, 0.03, 3.2, segs=5)
    for k in range(10):
        a = 2 * math.pi * k / 10
        b.cone(m["white"], (9.0 * math.cos(a), 9.0 * math.sin(a), 0.92), 0.14, 0.03, 4.6, segs=5)
    b.cone(m["white"], (0, 0, 1.6), 0.25, 0.04, 5.0, segs=6)
    return b.finish()


# --------------------------------------------------------- building/building_club_pavilion
def club_pavilion():
    m = mb_mats(); b = B("building_club_pavilion")
    L, D = 46.0, 24.0
    b.box(m["stone"], (-L / 2 - 0.3, -D / 2 - 4.3, 0), (L / 2 + 0.3, D / 2 + 0.3, 0.9))
    b.box(m["colonial"], (-L / 2, -D / 2, 0.9), (L / 2, D / 2, 11.0))
    # two-storey verandah on the road side
    for z0 in (0.9, 6.0):
        b.box(m["white"], (-L / 2 + 2, -D / 2 - 4.0, z0 - 0.15 if z0 > 1 else z0), (L / 2 - 2, -D / 2, z0 + 0.2))
        colonnade(b, m["white"], m["white"], np.arange(-L / 2 + 3, L / 2 - 2.9, 3.6), -D / 2 - 3.8, z0 + 0.2, z0 + 4.8, r=0.2)
        b.box(m["white"], (-L / 2 + 2, -D / 2 - 4.1, z0 + 4.8), (L / 2 - 2, -D / 2, z0 + 5.15))
        for x in np.arange(-L / 2 + 3, L / 2 - 2.9, 0.9):
            b.box(m["white"], (x - 0.02, -D / 2 - 3.9, z0 + 0.2), (x + 0.02, -D / 2 - 3.8, z0 + 1.2))
        b.box(m["white"], (-L / 2 + 2, -D / 2 - 3.92, z0 + 1.15), (L / 2 - 2, -D / 2 - 3.78, z0 + 1.25))
    # hipped tile roof, central gable, turret
    frustum(b, m["tile"], (-L / 2 - 1.2, L / 2 + 1.2, -D / 2 - 5.0, D / 2 + 1.0), (-L / 2 + 8, L / 2 - 8, -2, 2), 11.0, 16.2)
    gable(b, m["white"], -7, 7, -D / 2 - 4.0, -D / 2 + 1.0, 11.0, 4.6)
    b.box(m["colonial"], (-2, -2, 16.0), (2, 2, 19.5))
    frustum(b, m["tile"], (-2.4, 2.4, -2.4, 2.4), (-0.15, 0.15, -0.15, 0.15), 19.5, 22.5)
    for x in (-14, 14):
        b.box(m["white"], (x - 0.8, 3, 14.0), (x + 0.8, 4.6, 17.5))
    return b.finish()


# --------------------------------------------------------- building/building_gothic_church
def gothic_church():
    m = mb_mats(); b = B("building_gothic_church")
    b.box(m["stone"], (-9.2, -26, 0), (9.2, 24, 0.6))
    b.box(m["plaster"], (-8, -20, 0.6), (8, 22, 12.5))                                         # nave
    gable(b, m["tile"], -8.6, 8.6, -20.4, 22.4, 12.5, 6.4, ridge='y')
    b.box(m["plaster"], (-20, 3, 0.6), (20, 15, 12.5))                                         # transept
    gable(b, m["tile"], -20.4, 20.4, 2.6, 15.4, 12.5, 6.4, ridge='x')
    b.box(m["plaster"], (-5.2, -29, 0.6), (5.2, -19, 30.0))                                   # tower
    b.box(m["stone"], (-5.6, -29.4, 30.0), (5.6, -18.6, 30.8))
    b.cone(m["tile"], (0, -24, 30.8), 4.4, 0.18, 30.5, segs=8)
    for (x, y) in ((-4.2, -20.6), (4.2, -20.6), (-4.2, -28.4), (4.2, -28.4)):
        b.cone(m["plaster"], (x, y, 30.8), 0.7, 0.05, 5.5, segs=6)                              # pinnacles
    # lancet windows + buttresses along the nave
    for y in np.arange(-16, 20, 4.5):
        for sx in (-1, 1):
            b.box(m["glass_pit"], (sx * 8.0 - 0.07 if sx < 0 else 7.93, y - 0.7, 3.5), (sx * 8.0 + 0.07 if sx < 0 else 8.07, y + 0.7, 9.0))
            gable(b, m["glass_pit"], 0, 0, 0, 0, 0, 0) if False else None
            b.box(m["plaster"], (sx * 8.0 - 0.5 if sx < 0 else 8.0, y + 2.1, 0.6), (sx * 8.0 + 0.0 if sx < 0 else 8.5, y + 2.7, 10.5))
    for sx in (-1, 1):                                                                         # lancets in transept ends
        b.box(m["glass_pit"], (sx * 20.0 - 0.07, 8.0, 3), (sx * 20.0 + 0.07, 10.0, 11.5))
    # west door + rose window
    b.box(m["steel_dark"], (-1.4, -29.1, 0.6), (1.4, -28.9, 5.5))
    b.cylinder(m["glass_pit"], (0, -29.12, 14.0), 1.7, 0.1, segs=20, axis='Y')
    for z in (21.5,):
        b.box(m["glass_pit"], (-0.6, -29.1, z), (0.6, -28.9, z + 4.5))
    return b.finish()


# --------------------------------------------------------- building/building_deco_theatre
def deco_theatre():
    m = mb_mats(); b = B("building_deco_theatre")
    L, D = 44.0, 30.0
    b.box(m["stone"], (-L / 2 - 0.3, -D / 2 - 0.3, 0), (L / 2 + 0.3, D / 2 + 0.3, 0.6))
    b.box(m["deco"], (-L / 2, -D / 2, 0.6), (L / 2, D / 2, 16))
    b.box(m["deco"], (-14, -D / 2 + 3, 16), (14, D / 2 - 3, 26))
    b.box(m["deco"], (-8, -D / 2 + 6, 26), (8, D / 2 - 6, 34))
    for (x0, x1, y0, z0, z1) in ((-14, 14, -D / 2 + 3, 16, 26), (-8, 8, -D / 2 + 6, 26, 34)):
        for x in np.arange(x0 + 1, x1, 2.0):
            b.box(m["stone"], (x - 0.2, y0 - 0.5, z0), (x + 0.2, y0, z1))
    for x in np.arange(-L / 2 + 2, L / 2, 4.0):
        b.box(m["stone"], (x - 0.3, -D / 2 - 0.5, 4), (x + 0.3, -D / 2, 16))
    # neon bands + marquee
    for (x0, x1, y, z) in ((-L / 2, L / 2, -D / 2 - 0.05, 16.0), (-14, 14, -D / 2 + 2.95, 26.0), (-8, 8, -D / 2 + 5.95, 34.0)):
        b.box(m["lit"], (x0, y - 0.15, z - 0.35), (x1, y + 0.05, z))
    b.box(m["steel"], (-12, -D / 2 - 5.0, 4.8), (12, -D / 2, 5.4))
    b.box(m["lit"], (-12, -D / 2 - 5.05, 5.0), (12, -D / 2 - 4.95, 5.3))
    for x in (-11, 11):
        b.bar(m["steel"], (x, -D / 2 - 4.7, 0), (x, -D / 2 - 4.7, 4.8), 0.09, segs=6)
    b.bar(m["steel"], (0, 0, 34), (0, 0, 42), 0.15, segs=6)
    return b.finish()


# --------------------------------------------------------- misc/monument_four_columns
def monument_four_columns():
    m = mb_mats(); b = B("monument_four_columns")
    b.box(m["stone"], (-13, -13, 0), (13, 13, 0.5))
    b.box(m["stone"], (-11, -11, 0.5), (11, 11, 1.1))
    for i in range(5):
        h = 0.3 * (i + 1)
        b.box(m["stone"], (-13 - 0.4 * (4 - i) * 0 - 0.0, -13 - 0.0, 0), (13, 13, 0.0)) if False else None
    for sx in (-1, 1):
        for sy in (-1, 1):
            c0 = Vector((sx * 4.8, sy * 4.8, 1.1)); c1 = Vector((sx * 3.9, sy * 3.9, 66.0))
            sec = []
            for (c, r, z) in ((c0, 2.3, 1.1), (Vector((sx * 4.6, sy * 4.6, 12)), 1.9, 12.0), (c1, 1.15, 66.0)):
                ring = []
                for k in range(8):
                    a = 2 * math.pi * k / 8 + math.pi / 8
                    ring.append((c.x + r * math.cos(a), c.y + r * math.sin(a), z))
                sec.append(ring)
            b.loft(m["concrete"], sec, closed=True, cap_ends=True)
    b.box(m["bronze"], (-2.4, -11.1, 1.1), (2.4, -10.9, 3.2))                                   # plaque
    return b.finish(recalc=True)


BUILD = {
    "building_colonnade_hotel": ("building", colonnade_hotel),
    "building_domed_court": ("building", domed_court),
    "building_colonnade_civic": ("building", colonnade_civic),
    "building_clock_tower_hall": ("building", clock_tower_hall),
    "building_five_towers": ("building", five_towers),
    "fountain_basin": ("misc", fountain_basin),
    "building_club_pavilion": ("building", club_pavilion),
    "building_gothic_church": ("building", gothic_church),
    "building_deco_theatre": ("building", deco_theatre),
    "monument_four_columns": ("misc", monument_four_columns),
}

def build(asset):
    clear_scene(); ground(); kind, fn = BUILD[asset]; fn(); return kind

def export(asset):
    return export_tree(BUILD[asset][0], asset)

if ASSET != "all":
    KIND = build(ASSET)
    RESULT = {"asset": ASSET, "kind": KIND, "tris": tri_count(ASSET), "bounds": bounds(ASSET)}
