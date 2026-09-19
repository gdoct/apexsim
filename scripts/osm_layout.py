#!/usr/bin/env python3
"""Build a circuit's real-world layout dossier from OpenStreetMap.

The centerlines under ``content/tracks/real`` come from public GPS traces,
so the road itself is where it should be -- but everything *beside* it
(which side the pit lane runs, where the grandstands stand and what they
are called, the pit building, the landmarks, where the trees actually are)
was invented by a procedural enrichment pass.  The scenes therefore look
like a circuit but not like *the* circuit.

This script fetches each circuit's surroundings from OpenStreetMap (public
data, ODbL), georeferences them onto the track's own coordinate frame, and
writes ``content/tracks/real/<Stem>.layout.json``: a small, readable,
reviewable dossier of real-world facts.  Turning a dossier into props is
``ats-dress``'s job (Rust, deterministic, tested); nothing here writes a
``.ats``.

    python scripts/osm_layout.py Zandvoort Spa   # refresh two dossiers
    python scripts/osm_layout.py --all           # every circuit with a bbox
    python scripts/osm_layout.py Monza --offline # reuse the cached extract

Georeferencing is a fit, not an assumption: the OSM raceway ways are
matched onto the YAML centerline by an FFT rotation/translation search
followed by trimmed ICP, and a dossier is only written when the fit
actually covers the centerline.  The ``fit`` block reports it.
"""

from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np
import yaml

REPO = Path(__file__).resolve().parent.parent
TRACK_DIR = REPO / "content" / "tracks" / "real"
CACHE_DIR = REPO / "content" / "tracks" / "osm-cache"
OSM_API = "https://api.openstreetmap.org/api/0.6/map.json"
R_EARTH = 6378137.0
ATTRIBUTION = (
    "OpenStreetMap contributors, ODbL 1.0 "
    "(https://www.openstreetmap.org/copyright)"
)
LAYOUT_FORMAT = "apex-track-layout"
LAYOUT_VERSION = 1

# Bounding boxes (min_lon, min_lat, max_lon, max_lat) around each circuit,
# split into tiles where the OSM API's 50k-node ceiling bites (Le Mans runs
# through a town and its forests).
BBOXES: dict[str, list[tuple[float, float, float, float]]] = {
    "Zandvoort": [(4.528, 52.383, 4.552, 52.396)],
    "Spa": [(5.955, 50.428, 5.988, 50.448)],
    "Monza": [(9.275, 45.612, 9.300, 45.635)],
    "Silverstone": [(-1.035, 52.063, -0.995, 52.083)],
    "Oschersleben": [(11.265, 52.020, 11.295, 52.035)],
    "Austin": [(-97.652, 30.122, -97.626, 30.145)],
    "Hockenheim": [(8.552, 49.318, 8.582, 49.338)],
    "BrandsHatch": [(0.250, 51.350, 0.275, 51.364)],
    "Spielberg": [(14.752, 47.212, 14.780, 47.228)],
    "Suzuka": [(136.525, 34.835, 136.555, 34.855)],
    "Nuerburgring": [(6.930, 50.325, 6.965, 50.345)],
    "Catalunya": [(2.246, 41.560, 2.275, 41.580)],
    "Budapest": [(19.236, 47.572, 19.262, 47.588)],
    "Sakhir": [(50.495, 26.020, 50.525, 26.045)],
    "Norisring": [(11.110, 49.428, 11.132, 49.440)],
    # MoscowRaceway is deliberately NOT registered here (see below): a bbox
    # whose fit fails would abort every `--all` run at this entry (build()
    # raises SystemExit, uncaught in main()'s loop), breaking `--all` for
    # every stem sorted after it. Once the fit is fixed, add:
    #   "MoscowRaceway": [(36.243, 55.987, 36.283, 56.005)],
    # That bbox is already corrected from the task card's seed
    # (36.070, 55.955, 36.105, 55.972), which is ~12 km off: the real
    # circuit (per Wikipedia and OSM Nominatim, "Moscow Raceway"
    # leisure=sports_centre + highway=raceway ways) sits at 55.996N 36.266E,
    # near Shelud'kovo/Fedyukovo, Volokolamsky District. Even at the
    # corrected location the fit still fails: OSM's "Moscow Raceway"
    # raceway way (the only one in range) covers only ~72% of the
    # centerline at ~4 m rmse, short of the 90% / 2.5 m gate (and short of
    # the Le Mans exception's 3.0 m rmse too). The circuit has ~18
    # published layout variants (FIM vs. GP10 differ by a back-straight
    # chicane / turn 8), and the uncovered stretches are scattered across
    # ~7 short spans rather than one contiguous chunk, which looks like OSM
    # traced a different variant than this YAML's rather than a
    # thin-coverage or public-road case. See docs/ADDITIONAL_TRACKS.md
    # hand-back for the full investigation; no dossier was written.
    # MexicoCity is deliberately NOT registered here either, for the same
    # --all-abort reason: the best achievable fit (using only the OSM ways
    # actually named "Autodromo Hermanos Rodriguez", excluding a duplicate
    # Formula E layer and a stray motocross track sharing the venue)
    # explains 82.0% of the centerline (2824/4297 m) at 4.05 m rmse
    # (raceway_matched 1.0 -- what it does match is precise). The
    # unexplained ~580 m is the Foro Sol stadium chicane (T13-T15):
    # Estadio GNP Seguros and Estadio Alfredo Harp Helu are mapped only as
    # stadium/building polygons in OSM, with no raceway linework at all
    # through the interior -- a real gap in the source data, not a
    # filtering or fit problem, so no code change can close it. Once OSM
    # gains linework through the stadium, add:
    #   "MexicoCity": [(-99.102, 19.396, -99.080, 19.413)],
    "Montreal": [(-73.540, 45.490, -73.510, 45.515)],
    "LeMans": [
        (0.180, 47.910, 0.240, 47.945),
        (0.180, 47.940, 0.215, 47.960),
        (0.215, 47.940, 0.240, 47.960),
        (0.180, 47.958, 0.212, 47.966),
        (0.212, 47.958, 0.240, 47.966),
        # Station 4746-8906 m of the YAML centerline (the back half of the
        # Ligne Droite des Hunaudieres/Mulsanne straight, through the
        # Mulsanne and Indianapolis corners) falls entirely outside the
        # five tiles above -- confirmed by projecting every centerline
        # sample through the fit's own enu() and checking bbox membership,
        # not by eye. That is why that whole stretch comes out barren: no
        # OSM data was ever fetched there, not even the real roadside
        # forest (well documented in circuit photography as tree-lined for
        # most of the straight's length). This tile closes the gap with a
        # margin past WOOD_RANGE_M/STRUCTURE_RANGE_M on every side.
        (0.148, 47.905, 0.188, 47.940),
    ],
    # Interlagos sits in dense urban Sao Paulo; the seed bbox alone exceeds
    # the API's 50k-node ceiling, so it is split into quadrants.
    "SaoPaulo": [
        (-46.712, -23.712, -46.700, -23.703),
        (-46.700, -23.712, -46.688, -23.703),
        (-46.712, -23.703, -46.700, -23.695),
        (-46.700, -23.703, -46.688, -23.695),
    ],
    "Shanghai": [(121.205, 31.328, 121.235, 31.352)],
    "Sepang": [(101.725, 2.750, 101.752, 2.772)],
    "Sochi": [(39.945, 43.398, 39.972, 43.415)],
    "Melbourne": [(144.955, -37.860, 144.985, -37.838)],
}

# Grandstands OSM does not have, from each circuit's own published
# seating map.  A run of seats is given as a station span and a side, and
# the script lays its front along the centerline there -- so a stand round
# a bend comes out curved, exactly like one traced from a building
# outline.  `gap_m` is the distance from the road edge to the front row.
MANUAL_STANDS: dict[str, list[dict]] = {
    # Only the Hoofdtribune is in OSM.  The rest are from the circuit's
    # own tribune map (https://dutchgp.com/en/tribunes/,
    # https://grandprixguides.com/circuit/netherlands): the Arena and
    # Hairpin stands are on the infield side, Eastside faces Arena-In
    # across the track, Ben Pon looks at the exit of the banked final
    # corner from the start of the straight.
    # Spa's own grandstand map (https://www.spa-francorchamps.be/) puts
    # seats at Les Combes, Bruxelles, Pouhon and the Bus Stop that OSM has
    # no outline for; each sits on the outside of its bend.
    "Spa": [
        dict(name="Tribune Les Combes", from_m=2320, to_m=2470, side="outside", depth_m=14),
        dict(name="Tribune Bruxelles", from_m=2990, to_m=3140, side="outside", depth_m=14),
        dict(name="Tribune Pouhon", from_m=3890, to_m=4060, side="outside", depth_m=14),
        dict(name="Tribune Bus Stop", from_m=6650, to_m=6820, side="outside", depth_m=16),
    ],
    # The Automobile Club de l'Ouest numbers its tribunes; those along the
    # pit straight are in OSM, the ones out on the public-road section are
    # not (https://www.lemans.org/ spectator map).
    "LeMans": [
        dict(name="Tribune Tertre Rouge", from_m=1840, to_m=1990, side="outside", depth_m=14),
        dict(name="Tribune Mulsanne", from_m=7500, to_m=7660, side="outside", depth_m=14),
        dict(name="Tribune Indianapolis", from_m=8950, to_m=9090, side="outside", depth_m=14),
        dict(name="Tribune Arnage", from_m=10060, to_m=10200, side="outside", depth_m=14),
        dict(name="Tribune Virage Porsche", from_m=11500, to_m=11700, side="outside", depth_m=14),
    ],
    "Zandvoort": [
        dict(name="Pit Grandstand", from_m=70, to_m=230, side="left", depth_m=18),
        dict(name="Tarzan", from_m=330, to_m=500, side="left", depth_m=26, covered=False),
        dict(name="Tarzan-In", from_m=380, to_m=470, side="right", depth_m=12),
        dict(name="Hairpin", from_m=3150, to_m=3330, side="right", depth_m=14),
        dict(name="Eastside", from_m=3330, to_m=3800, side="left", depth_m=16),
        dict(name="Arena", from_m=3300, to_m=3760, side="right", depth_m=16),
        dict(name="Ben Pon", from_m=4030, to_m=4240, side="left", depth_m=20),
    ],
    # COTA's own grandstand map (circuitoftheamericas.com/ticket/grandstands-
    # and-reserved-seating-f1/) and third-party guides (oversteer48.com/cota-
    # turn-4/, oversteer48.com/cota-turn-9/) both put these on the outside
    # (right-hand side, as seen driving the lap) of the track; neither has an
    # OSM building outline, unlike the Main Grandstand and the Turn 1/12/15/
    # 19-20 stands, which OSM already traces. The Turn 4 grandstand runs the
    # length of the T3-T4-T5 sequence and is partly covered (premium upper
    # rows under a roof); the Turn 9 grandstand, between T9 and T10, is tall
    # (37 rows) open bleachers with no roof.
    "Austin": [
        dict(name="Turn 4 Grandstand", from_m=1050, to_m=1400, side="right", depth_m=18, covered=True),
        dict(name="Turn 9 Grandstand", from_m=2017, to_m=2154, side="right", depth_m=22, covered=False),
    ],
    # OSM's grandstand outlines cover the Motodrom bowl and the pit
    # straight but not the Mercedes-Tribüne, a 1,300-tonne permanent steel
    # stand built in 2002 alongside the new Spitzkehre. It looks onto turn
    # 8 (the hairpin's exit) and the acceleration zone into the Motodrom
    # (https://stahlbau-queck.de/projekte/projekte-details/mercedes-tribuene-hockenheimring,
    # https://www.thef1spectator.com/hockenheim-f1-travel-guide/where-to-watch/).
    # Station span located from the hairpin's own heading reversal (the
    # fit's coverage gap sits exactly there); on the outside of the
    # right-hand hairpin, which the sources' photos agree with.
    "Hockenheim": [
        dict(name="Mercedes-Tribüne", from_m=2000, to_m=2250, side="outside", depth_m=20),
    ],
    # Interlagos letters its stands. OSM already traces the pit-straight
    # ones (an unnamed stand at station ~76 m and another at ~4272 m -
    # geometrically Grandstands B and A - plus "M" itself, named in OSM,
    # at station ~189 m) so only the stands OSM has no outline for are
    # added here, from the circuit's own tribune guide
    # (https://f1saopaulo.com.br/en/stands/, https://www.brasilf1.com/en/map-of-the-grandstands-18):
    # D overlooks the first two Senna S apexes, R sits over Curva do Sol
    # and the start of the back straight, G is the bleacher stand at the
    # back straight's braking zone into Descida do Lago, and the T4 stand
    # is the newer grandstand built for the 2021 Descida do Lago
    # reprofile. The task brief's placements for M (Bico de Pato) and R
    # (Juncao) do not match these sources or OSM's own traced "M"; this
    # table follows the verified sources instead.
    "SaoPaulo": [
        dict(name="D", from_m=290.9, to_m=500.0, side="outside", depth_m=14, covered=True),
        dict(name="R", from_m=592.5, to_m=782.2, side="outside", depth_m=14, covered=True),
        dict(name="G", from_m=1150.0, to_m=1368.1, side="outside", depth_m=10, covered=False),
        dict(name="T4", from_m=1368.1, to_m=1607.1, side="outside", depth_m=13, covered=False),
    ],
    # Suzuka: OSM tags most of the circuit's stands `grandstand=yes` (picked
    # up generically, see is_stand()) and even names most of them with the
    # circuit's own letters, so almost nothing here is needed. The one gap
    # is Grandstand M at the Spoon Curve (https://www.japan.gp/en/
    # map-of-the-grandstands-27: "M - around the Spoon Curve"), which OSM
    # carries as a plain unnamed building (station ~3676, right, no
    # grandstand tag) rather than a stand outline. Spoon Curve itself has no
    # named OSM way either, so the span is placed from the circuit's own
    # corner order: after the hairpin (`station_m` 2957.7 in `corners`) and
    # before the west straight (4055.9), astride that unnamed building.
    "Suzuka": [
        dict(name="M", from_m=3600, to_m=3760, side="right", depth_m=16, covered=False),
    ],
    # Sepang's own OSM building=grandstand way for the Main Grandstand is
    # dropped by front_edge_is_broken() (see its docstring): it traces the
    # whole 1.3 km double-fronted complex as one polygon ("1.3 km long
    # double frontage Main Grandstand... dominating two long straights",
    # https://www.sepangcircuit.com/main-grandstand), between the pit
    # straight and the back straight, which oriented_box() and
    # front_edge() cannot turn into a sane single front. Authored here as
    # its two real faces instead: the pit-straight face is on the
    # circuit's right (the same side as the pit lane -- confirmed by this
    # card's own risk note, which is only true if both stands are on the
    # pit lane's side: "the double-sided main stand is inside 20 m of the
    # pit lane on the pit-straight side and will be dropped there
    # (correct)"), and the back-straight face carries the same name and
    # side since it is physically the other side of one stand.
    "Sepang": [
        dict(name="Main Grandstand", from_m=20, to_m=560, side="right", depth_m=20, covered=True),
        dict(name="Main Grandstand", from_m=4300, to_m=4950, side="right", depth_m=20, covered=True),
        # K2 Hillstand: an open grass mound at the Turn 1/2 hairpin,
        # opposite the K1 Grandstand OSM already gives us
        # (sepangcircuit.com spectator guide: "K2 Hillstand... open-air
        # grassy viewing area", the cheap uncovered option beside K1).
        dict(name="K2 Hillstand", from_m=650, to_m=850, side="inside", depth_m=15, covered=False),
        # Turn 4 grandstand. Sepang's own corner numbering is not in OSM
        # (no named raceway sections at all here) and no published station
        # exists to check it against, so this is placed from the YAML
        # centerline's own curvature: the fourth significant apex after
        # the line (632 m T1/T2, ~792 m, a shallow ~1030-1110 m kink, then
        # this one at ~1607 m -- a single sustained corner, matching T4's
        # long, high-speed real-world character). Approximate; flagged in
        # the hand-back.
        dict(name="Turn 4 Grandstand", from_m=1550, to_m=1720, side="outside", depth_m=14, covered=False),
    ],
    # OSM has this one only as a plain `building=yes` named "Tribuna F", so
    # extract() files it under structures (a building) rather than stands;
    # it sits in the same run of grandstands as (osm-traced) Tribuna E and
    # J/K along the straight after Turn 1, and the letter matches the
    # circuit's own tribune list (entradasmontmelo.com/tribunas,76.html:
    # A, B, C, E, F, G, H, J, K, L, M, N, T1, T10), so it is promoted here
    # rather than left to render as a generic building. Span and depth are
    # taken from that same OSM footprint (station 768.2, length 97.4 m,
    # depth 17.7 m, uncovered like its neighbours).
    "Catalunya": [
        dict(name="Tribuna F", from_m=719.5, to_m=816.9, side="left", depth_m=17.7, covered=False),
    ],
    # Hungaroring's own ticket map (hungaryticketsgp.com/en/map-of-grandstands,
    # motorsporttickets.com's grandstand guide, grandprixgrandtours.com) names
    # stands by colour; OSM only traces the single big covered "Super Gold"
    # complex on the main straight (station ~2-271, source "osm" below) and
    # mistags the "Silver 5" terrace as a plain building rather than a
    # grandstand (it survives as an auto-extracted `structures` entry near
    # station 3355 instead). Spans here are geometric estimates against this
    # track's own centerline (turn order + the named-way boundary at station
    # 600.6, which lines up with the main straight/T1 kink) rather than
    # GPS-matched OSM outlines -- see the Budapest hand-back for the
    # reasoning. Silver 2-4 and the rest of Gold 1-4 are not broken out
    # separately: sources only place them qualitatively ("near the grid",
    # "near the final corner") with no distinguishing span, and duplicating
    # a station guess with no anchor felt worse than leaving them out.
    "Budapest": [
        # motorsporttickets.com: "Silver 1 is the closest to the grid" --
        # the grid forms just past the OSM-traced Super Gold stand.
        dict(name="Silver 1", from_m=280, to_m=380, side="left", depth_m=10),
        # Turn 1 ("Piquet" corner, official 40th-anniversary corner names,
        # formula1.com): a natural, uncovered bank right at the braking
        # zone/exit, per the T1 Grandstand guides (oversteer48.com).
        dict(name="T1 Stand", from_m=600, to_m=760, side="outside", depth_m=9),
        # grandprixgrandtours.com / motorsporttickets.com: "Bronze 1 & 2
        # grandstands are positioned overlooking turns 5 and 7", historically
        # three (Bronze 1-3; two were later renamed Chicane 1/2). These are
        # the hillside natural terraces through the infield esses.
        dict(name="Bronze 1", from_m=1500, to_m=1700, side="outside", depth_m=9),
        dict(name="Bronze 2", from_m=1994, to_m=2190, side="outside", depth_m=9),
        dict(name="Bronze 3", from_m=2210, to_m=2400, side="outside", depth_m=9),
        # The general-admission terraces through the Schumacher (T12) /
        # Senna (T13) / Szisz (T14) finishing sequence, clear of the
        # OSM-traced "Silver 5" structure around station 3355.
        dict(name="T12-T14 Stand", from_m=3600, to_m=3950, side="outside", depth_m=8),
    ],
    # OSM only traces the Main Grandstand (opposite the pits, station ~143).
    # The circuit numbers the rest and sells them under those numbers
    # (f1.sochiautodrom.ru ticket pages confirm "T2 Grandstand" and
    # "T3 Grandstand" by name; RaceFans/enterF1 confirm T4/T5 as a pair
    # and T10/T13/T15 as corners with dedicated stands, though their own
    # ticket pages for those five were unreachable this run -- the site is
    # gone since the circuit lost its F1 date). Spans/sides are this
    # track's own centerline geometry (a heading-curvature scan of the
    # YAML nodes, matched in order to the published turn descriptions),
    # not GPS-traced outlines, same approach as Budapest's Bronze/T1/T12-14
    # entries above.
    "Sochi": [
        # "T2 Grandstand (Vitaly Petrov)": the tight ~90 degree right
        # braking zone, described as facing the Olympic Park's Central
        # Square (the Medals Plaza) with two big screens.
        dict(name="T2 Grandstand", from_m=1140, to_m=1230, side="outside", depth_m=12),
        # "T3 Grandstand": along the long (~600 m), constant-radius left
        # sweeper round the outside of the plaza/fountain -- the circuit's
        # own description of T3. Placed centrally on the bend rather than
        # spanning all 590 m of it.
        dict(name="T3 Grandstand", from_m=1400, to_m=1700, side="outside", depth_m=12),
        # T4/T5: sold and described together ("T4 and T5 grandstands...").
        # T4 is the right-hander directly after the T3 sweeper ends; T5 is
        # the tighter right just past it, closest point to the Black Sea.
        dict(name="T4 Grandstand", from_m=2000, to_m=2110, side="outside", depth_m=12),
        dict(name="T5 Grandstand", from_m=2430, to_m=2560, side="outside", depth_m=12),
        # T10: the tight 90 degree right onto the back straight.
        dict(name="T10 Grandstand", from_m=3550, to_m=3660, side="outside", depth_m=12),
        # T13: the heavy-braking hairpin at the end of the ~1 km back
        # straight ("100 degree right, an excellent place to pass").
        dict(name="T13 Grandstand", from_m=4670, to_m=4790, side="outside", depth_m=12),
        # T15: the first (left) half of the T15-16 chicane that turns the
        # lap back in behind the pit building.
        dict(name="T15 Grandstand", from_m=5040, to_m=5160, side="outside", depth_m=12),
    ],
    # Albert Park is entirely temporary seating -- OSM maps none of it (the
    # only `building=grandstand` ways in the bbox belong to Lakeside
    # Stadium, the athletics venue the circuit runs past, not the GP; see
    # the `structures`/hand-back note). Names, order and rough locations
    # are from the Australian Grand Prix Corporation's own grandstand map
    # (f1-australia.com/en/map-of-the-grandstands-22,
    # grandprixgrandtours.com/australia-circuit-guide/), cross-checked
    # against oversteer48.com's per-stand pages; spans are this dossier's
    # own corner geometry (no OSM corner names exist to check against — see
    # `corners: []` — so these are read off the fitted centerline's own
    # curvature, not a published chainage). The task card's "Piquet" is
    # this circuit's "Hill" stand since 2023 (grandprix.com.au confirms the
    # rename; same stand, not duplicated) and its "Sadler" could not be
    # found on any current or archived seating map or ticketing page
    # checked here -- omitted rather than invented.
    "Melbourne": [
        # Fangio: main straight, grid/pit-stop/chequered-flag views, so it
        # runs opposite the pit building (pits are the lake side / right;
        # see MANUAL_PIT_LANE) for most of the straight before Turn 1.
        dict(name="Fangio", from_m=30, to_m=330, side="left", depth_m=18, covered=False),
        # Moss: "just past the pit building... the run into turn 1" --
        # same (pit-straight) side as the pits, right up against Turn 1.
        dict(name="Moss", from_m=260, to_m=395, side="right", depth_m=14, covered=False),
        # Jones/Brabham: opposite faces of the Turn 1-2 chicane (the first
        # right-left in this dossier's own geometry, station ~348-562).
        dict(name="Jones", from_m=345, to_m=565, side="outside", depth_m=14, covered=False),
        dict(name="Brabham", from_m=345, to_m=565, side="inside", depth_m=12, covered=False),
        # Hill (the ex-Piquet stand, renamed 2023): Turn 3, outside.
        dict(name="Hill", from_m=1050, to_m=1160, side="outside", depth_m=14, covered=False),
        # Stewart: Turn 5 exit.
        dict(name="Stewart", from_m=1600, to_m=1720, side="outside", depth_m=12, covered=False),
        # Waite: the Turn 6-7 double corner (right into left-left here).
        dict(name="Waite", from_m=1860, to_m=2030, side="outside", depth_m=14, covered=False),
        # Clark: "beyond Turn 8", the long right sweeper after the Turn
        # 6-7 complex in this dossier's geometry.
        dict(name="Clark", from_m=2180, to_m=2320, side="outside", depth_m=14, covered=False),
        # Webber: Turn 11, inside. In the old (this YAML's) 16-turn
        # numbering this is the corner after the since-removed Turn 9-10
        # chicane (station ~2530-2690 here) and the back straight.
        dict(name="Webber", from_m=3300, to_m=3420, side="inside", depth_m=12, covered=False),
        # Lauda: Turn 12, outside, right after Webber's corner.
        dict(name="Lauda", from_m=3440, to_m=3560, side="outside", depth_m=14, covered=False),
        # Schumacher: "final section of the lap", outside of the following
        # double-right in this dossier's geometry.
        dict(name="Schumacher", from_m=3740, to_m=3860, side="outside", depth_m=16, covered=False),
        # Prost and Senna: "next to each other, exit of the last turn and
        # start of the main straight" -- adjacent spans on the final
        # corner (station ~4774-4899 here) leading onto the pit straight.
        dict(name="Prost", from_m=4780, to_m=4870, side="outside", depth_m=16, covered=False),
        dict(name="Senna", from_m=4870, to_m=4950, side="outside", depth_m=18, covered=False),
    ],
    # The circuit numbers its stands rather than naming them
    # (https://gpcanada.ca/en/type-de-billet/grandstands/); none of the
    # numbered stands are traced in OSM (only three small, unnamed
    # buildings near the rowing basin, which turned out on inspection to be
    # Place des Nations, an Expo 67 amphitheatre wikidata:Q101013014 -- not
    # a grandstand at all -- and were dropped, see the hand-back). Stations
    # are estimated from the centerline's own curvature (no named OSM
    # corners) and cross-checked against each stand's own description
    # (grandstandguide/oversteer48/gpcanada); not verified against
    # satellite imagery (no image access this run), so treat the spans as
    # approximate pending a look. Pit lane confirmed on the left (OSM's own
    # "Circuit Gilles Villeneuve pit lane" way), so GS1/2 face it from the
    # right.
    "Montreal": [
        dict(name="Grandstand 1", from_m=4300, to_m=30, side="right", depth_m=14),
        dict(name="Grandstand 2", from_m=30, to_m=170, side="right", depth_m=12),
        dict(name="Grandstand 12", from_m=200, to_m=280, side="left", depth_m=12),
        dict(name="Grandstand 11", from_m=280, to_m=380, side="left", depth_m=12),
        dict(name="Grandstand 33", from_m=1230, to_m=1340, side="inside", depth_m=12),
        dict(name="Grandstand 31", from_m=1900, to_m=1970, side="outside", depth_m=12),
        dict(name="Grandstand 21", from_m=1970, to_m=2010, side="outside", depth_m=12),
        dict(name="Grandstand 22", from_m=2010, to_m=2040, side="outside", depth_m=10),
        dict(name="Grandstand 15", from_m=2040, to_m=2080, side="outside", depth_m=12),
        dict(name="Grandstand 34", from_m=2080, to_m=2120, side="outside", depth_m=12),
        dict(name="Grandstand 24", from_m=2120, to_m=2160, side="outside", depth_m=12),
        dict(name="Grandstand 47", from_m=2170, to_m=2230, side="left", depth_m=12),
        dict(name="Grandstand 46", from_m=2230, to_m=2290, side="right", depth_m=12),
    ],
}

# Landmarks that span the road and are not in OSM as bridges.
MANUAL_CROSSINGS: dict[str, list[dict]] = {
    # The Dunlop bridge has arched over the climb out of the start straight
    # since 1932 and is the one structure everyone draws when they draw Le
    # Mans; OSM maps the footbridges beside it but not the arch itself.
    # The tyre bridge: the arch shaped like a tyre that the lap passes under
    # on the crest just after the Dunlop chicane. OSM has it (as
    # man_made=bridge, "Passerelle Goodyear") but only as a footbridge, so it
    # would come out as a plain truss; this entry takes its place and wears
    # the kit's tyre brand, the real sponsor not being in the kit's signage.
    "LeMans": [dict(name="Tyre bridge", station_m=1043.0, kind="arch", brand="piretti")],
    # A spectator footbridge crosses the Parabolika partway along its
    # ~800 m length, joining the infield (accessible from the Motodrom
    # side) to the outer paddock roads; OSM has no bridge=yes way on this
    # stretch at all (checked directly against the extract), so this is
    # authored from the circuit's own general layout rather than traced.
    # Placed at the corner's midpoint, which is also roughly where the old
    # forest-straight/Parabolika transition sits.
    "Hockenheim": [dict(name="Parabolika footbridge", station_m=1400.0, kind="footbridge")],
    # The spectator footbridge on the climb out of Niki Lauda Kurve (T1),
    # carrying fans from the paddock/pit side over to the hillside Red Bull
    # Tribune. Not in OSM (nothing bridge-tagged near the track in this
    # bbox is within 350 m of the centerline -- checked directly against the
    # cached extract), and no aerial source gives its exact position, so the
    # station is placed by judgement partway up the climb, just before the
    # Red Bull Tribune's OSM-traced footprint begins (station_m 646.7):
    # authored per docs/ADDITIONAL_TRACKS.md 6.19, an approximation to flag
    # in the hand-back rather than a sourced fact.
    "Spielberg": [dict(name="T1 climb footbridge", station_m=600.0, kind="footbridge", brand="kronos")],
    # The pit building's two cantilevered "wing" roofs (the Press Centre and
    # the Sky Restaurant, ~38 m up) reach out over the front straight from
    # the tower at the west and east ends of the paddock/grandstand-A
    # complex OSM traces at stations 8-338 -- confirmed by web search
    # (en.wikipedia.org: "wing-like viewing platforms crossing the circuit
    # at either end"). The kit has no cantilever-wing asset, so these are
    # laid as tyre-bridge stand-ins; the real shape is not represented.
    "Shanghai": [
        dict(name="Press Centre Wing", station_m=15.0, kind="arch", brand="piretti"),
        dict(name="Sky Restaurant Wing", station_m=335.0, kind="arch", brand="piretti"),
    ],
}

# Point features OSM does not carry, but that are part of what the place
# looks like on a race weekend.
MANUAL_LANDMARKS: dict[str, list[dict]] = {
    # The airship over the pits is as much a part of the 24 Hours as the
    # fairground at the Esses (which OSM does have, as a big wheel).
    "LeMans": [
        # The airship loiters over the Dunlop crest, where the pit straight,
        # its tribunes and every car on the grid look straight at it, and it
        # is turned broadside to the start line so it shows its length (and
        # its brand) rather than its nose. 150 m up keeps it inside a
        # cockpit's view from the grid.
        dict(kind="blimp", name="Airship", station_m=900.0, side="right", offset_m=90.0,
             altitude_m=150.0, broadside_to_m=0.0, brand="piretti"),
        # The fun fair inside the Esses, which runs all through the night.
        dict(kind="big_wheel", station_m=1010.0, side="right", offset_m=110.0),
    ],
    # Sepang was floodlit in 2018 (64 poles up to 43 m, evenly round the
    # whole 5.543 km lap; installed for local FIA/FIM racing, not F1/MotoGP
    # broadcast use -- https://www.thestar.com.my/sport/motorsport/2017/12/14/
    # sepang-circuit-to-hold-night-events-with-installation-of-floodlights/).
    # OSM has none of the poles mapped. A representative spread round the
    # lap (outside of each corner, clear of the stands) stands in for the
    # real 64; exact pole positions are not published.
    "Sepang": [
        dict(kind="floodlight", station_m=300.0, side="right", offset_m=40.0),
        dict(kind="floodlight", station_m=650.0, side="right", offset_m=40.0),
        dict(kind="floodlight", station_m=1650.0, side="left", offset_m=40.0),
        dict(kind="floodlight", station_m=2600.0, side="left", offset_m=40.0),
        dict(kind="floodlight", station_m=3900.0, side="right", offset_m=40.0),
        dict(kind="floodlight", station_m=4600.0, side="right", offset_m=40.0),
        dict(kind="floodlight", station_m=5200.0, side="right", offset_m=40.0),
    ],
    # The Sakhir Tower (ten storeys, behind the pits) is in OSM as a plain
    # building=yes outline with no building:levels tag, so the automatic
    # tower heuristic in dress.rs (building_asset) never picks it: its
    # footprint is square but 37.7 m across, over the 30 m the heuristic
    # requires, and it carries no storey count. Authored here instead, at
    # the OSM outline's own station/side/offset so the dedup above drops
    # the OSM building row and only the control-tower kit asset (a
    # stand-in for the real tower, which the kit does not model) remains.
    "Sakhir": [
        dict(kind="tower", name="Sakhir Tower", station_m=582.3, side="right", offset_m=73.9),
    ],
    # The 2014 Winter Olympics cauldron still stands in the Medals Plaza,
    # inside the loop of T3; OSM has it only as a tourism=attraction node
    # ("Олимпийский огонь" / "Olympic flame"), which the automatic
    # landmark scan does not look at (it only picks up man_made=tower /
    # tower:type=lighting / attraction=big_wheel). The kit has no cauldron
    # asset, so this is authored as the "tower" stand-in the task card
    # accepts, at the node's own real position converted through this
    # dossier's own fit (station/offset computed from the OSM node
    # 2660040429 at 43.4051613N 39.9547853E).
    "Sochi": [
        dict(kind="tower", name="Olympic Cauldron", station_m=2001.7, side="left", offset_m=140.0),
    ],
    # The Casino de Montreal (the former France/Quebec Expo 67 pavilions) is
    # the building every TV shot of the Casino Straight shows in the
    # background; its own footprint is not in this bbox's extract (only the
    # access road, "Avenue du Casino", is), so it is authored from its real
    # position beside the Casino Straight. The kit has no casino/exhibition-
    # hall asset, so this reuses the control-tower stand-in, as Austin and
    # Sakhir do for buildings the kit can't represent.
    "Montreal": [
        dict(kind="tower", station_m=2350.0, side="left", offset_m=150.0),
    ],
    # The rusted-steel bull statue in the fan zone beside the Mitte/Centre
    # Grandstand -- "The massive steel Red Bull statue is in the Yellow
    # Zone next to the Mitte / Centre Grandstand" (oversteer48.com's Red
    # Bull Ring general-admission guide). Not in OSM at all (no node/way
    # near the track tags it), and the kit had no statue asset until this
    # entry, so it is placed at the "Tribuene Mitte" stand's own station
    # (3149.7 m, left) just beyond its outer edge (the stand's
    # offset_m 47 + depth_m 43.5 = 90.5 m), in the plaza next to it rather
    # than on top of it.
    "Spielberg": [
        dict(kind="statue", name="Red Bull Statue", station_m=3149.7, side="left", offset_m=90.0),
    ],
}

# For a circuit whose pit lane leaves no separate polyline in OSM at all --
# a public-road street circuit where the pits front directly onto the same
# public road the lap itself uses (Albert Park's permanent pit building
# fronts onto Aughtie Drive, which *is* the pit straight here) -- there is
# nothing for `pit_lane_from`'s geometric search to find. Authored the same
# way a `MANUAL_STANDS` front is: a station span, a side and a gap from the
# road edge, laid along the centerline with `Track.edge_run`.
MANUAL_PIT_LANE: dict[str, dict] = {
    # The pit building (3/12 Aughtie Drive; Development Victoria's Albert
    # Park Pit Building project) sits on the lake side of the main
    # straight, i.e. the right as driven -- matching the task card and
    # https://www.development.vic.gov.au/projects/albert-park. The span
    # runs from the start/finish line to just short of the Turn 1/2
    # chicane (this dossier's own corner geometry turns hard right at
    # ~station 348), which is where the pit exit rejoins; published pit
    # lane length figures cluster around 380 m (e.g. motorsport press
    # coverage of the 2019 pit-lane widening), which this span reproduces.
    # No OSM way represents the actual lane (cars pit on the same
    # Aughtie Drive pavement the lap uses); this is an estimate, not a
    # trace, and is noted as such in the dossier's own `source` field.
    "Melbourne": dict(from_m=0.0, to_m=380.0, side="right", gap_m=8.0),
}

# Buildings OSM maps only as a tagless multipolygon relation (a ring of
# `barrier=fence` ways with the real tags on the relation, not any member),
# which extract()'s structures scan -- way-only, like the rest of this
# script -- cannot see at all. Precomputed once against the fitted
# centerline (the same one-off approach as the Sakhir Tower landmark
# above) rather than adding relation parsing for a single building.
MANUAL_STRUCTURES: dict[str, list[dict]] = {
    # Lakeside Stadium (leisure=stadium relation 15400771, an athletics/
    # soccer venue, not part of the Grand Prix) backs directly onto the
    # circuit along the Albert Road Drive infield link -- the task card's
    # "Lakeside Stadium (a building)" landmark. Box computed from the
    # relation's own member-way outline (fence rings) through this
    # dossier's fit transform: centre (-344.0, 1092.3), 256.0 x 229.8 m,
    # yaw 1.7701 rad, 14.5 m off the road at station 1568.9, right.
    "Melbourne": [
        dict(
            name="Lakeside Stadium",
            station_m=1568.9,
            side="right",
            offset_m=14.5,
            length_m=256.0,
            depth_m=229.8,
            yaw_rad=1.7701,
            centre=[-344.03, 1092.31],
            levels=0,
            osm_building="stadium",
            area_m2=42694,
        ),
    ],
}

# A patch of real woodland OSM cannot supply -- either because the bbox
# never reached it (see the BBOXES comment for the stem) or, generically,
# because a public-road circuit's verges are not tagged as forest even
# where they plainly are. `side` follows the same "left"/"right"/"outside"/
# "inside" convention MANUAL_STANDS uses; the belt runs from `near_m`
# (default 15 m, inside dress.rs's own TREE_BELT_NEAR_M=22 so the whole
# planting band lands inside the ring) out to `near_m + depth_m` (default
# 95 m, past TREE_BELT_FAR_M=90) beyond the road edge, following the road's
# own curve over the span -- built with Track.edge_run, the same helper
# MANUAL_STANDS' front rows use.
MANUAL_WOODS: dict[str, list[dict]] = {
    # The real Ligne Droite des Hunaudieres (Mulsanne straight) is
    # tree-lined for most of its length (widely documented in circuit
    # photography and coverage; pine forest is specifically noted right
    # around the Auberge de Mulsanne, near this straight's second chicane
    # and corner). Station 4746-8906 m is exactly the stretch the BBOXES
    # gap above leaves with no OSM data at all -- the same span
    # MANUAL_STANDS already carries "Tribune Mulsanne" (7500-7660) and
    # "Tribune Indianapolis" (8950-9090) for, for the identical reason.
    # Pine (needleleaved) on both sides, matching the real straight.
    "LeMans": [
        dict(from_m=4700, to_m=8950, side="left", leaf="needleleaved",
             near_m=15, depth_m=95),
        dict(from_m=4700, to_m=8950, side="right", leaf="needleleaved",
             near_m=15, depth_m=95),
    ],
}

# Circuits sharing their bbox with other lit sports venues, where an OSM
# `tower:type=lighting` node is real but is not the circuit's own. See the
# comment where this is used, in the landmark extraction loop.
DAY_RACE_NO_FLOODLIGHTS = frozenset({"Melbourne"})

# Circuits where every OSM `building=grandstand`/`leisure=grandstand` hit in
# range is confirmed (by hand, against the circuit's own seating map) to
# belong to a different, co-located venue rather than the circuit itself.
NO_OSM_STANDS = frozenset({"Melbourne"})

# How far from the road a feature still belongs to the circuit.
STAND_RANGE_M = 260.0
# How far a hand-placed landmark may be from the real one the scan found
# before the two are taken for the same thing (§MANUAL_LANDMARKS).
MANUAL_LANDMARK_YIELD_M = 120.0
# The surroundings layers (§`extract_surroundings`). Barriers are circuit
# furniture and only count close in -- a fence 200 m out is a farm's.
BARRIER_RANGE_M = 70.0
ROAD_RANGE_M = 400.0
AREA_RANGE_M = 400.0
POI_RANGE_M = 300.0
# Everything tagged within this of the road is counted in the dossier's
# `unclassified` census, whether a rule claims it or not.
CENSUS_RANGE_M = 250.0
STRUCTURE_RANGE_M = 140.0
WOOD_RANGE_M = 260.0


# ---------------------------------------------------------------- fetching


def fetch(stem: str, offline: bool) -> list[Path]:
    import urllib.request

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    out = []
    for i, box in enumerate(BBOXES[stem]):
        path = CACHE_DIR / f"{stem}.{i}.json"
        if not path.exists():
            if offline:
                raise SystemExit(f"{stem}: no cached extract at {path}")
            url = f"{OSM_API}?bbox={box[0]},{box[1]},{box[2]},{box[3]}"
            print(f"  fetching {url}")
            with urllib.request.urlopen(url, timeout=300) as r:
                body = r.read()
            if not body.lstrip().startswith(b"{"):
                raise SystemExit(f"{stem}: OSM refused tile {i}: {body[:200]!r}")
            path.write_bytes(body)
            time.sleep(1.0)
        out.append(path)
    return out


# ---------------------------------------------------------------- geometry


def enu(lon, lat, lon0, lat0):
    return (
        math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0)),
        math.radians(lat - lat0) * R_EARTH,
    )


def densify(pts: np.ndarray, step: float, closed: bool) -> np.ndarray:
    out = []
    n = len(pts)
    for i in range(n if closed else n - 1):
        a, b = pts[i], pts[(i + 1) % n]
        d = float(np.hypot(*(b - a)))
        for j in range(max(int(d / step), 1)):
            out.append(a + (b - a) * (j / max(int(d / step), 1)))
    out.append(pts[0] if closed else pts[-1])
    return np.array(out)


def simplify(pts: np.ndarray, tol: float) -> np.ndarray:
    """Douglas-Peucker, so a wood's outline costs a handful of vertices."""
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    ab = b - a
    n = float(np.hypot(*ab))
    if n < 1e-6:
        d = np.hypot(*(pts - a).T)
    else:
        d = np.abs(np.cross(np.tile(ab, (len(pts), 1)), pts - a)) / n
    i = int(d.argmax())
    if d[i] <= tol:
        return np.array([a, b])
    return np.vstack([simplify(pts[: i + 1], tol)[:-1], simplify(pts[i:], tol)])


class NearestGrid:
    """Uniform-grid nearest neighbour.  Every point set here is a densely
    sampled polyline, so a 3x3 cell probe at the sample spacing finds the
    true nearest point and the whole fit stays linear in the node count."""

    def __init__(self, pts: np.ndarray, cell: float = 12.0):
        self.pts = pts
        self.cell = cell
        self.lo = pts.min(0) - cell
        self.buckets: dict[tuple[int, int], list[int]] = {}
        for i, (cx, cy) in enumerate(((pts - self.lo) / cell).astype(int)):
            self.buckets.setdefault((int(cx), int(cy)), []).append(i)

    def query(self, q: np.ndarray, max_rings: int = 64) -> tuple[np.ndarray, np.ndarray]:
        """Nearest point and its distance.  The ring search widens until it
        finds something, so this answers for a wood 200 m off the road as
        well as for a point on it."""
        q = np.asarray(q, dtype=float).reshape(-1, 2)
        idx = ((q - self.lo) / self.cell).astype(int)
        best_d = np.full(len(q), np.inf)
        best_i = np.zeros(len(q), dtype=int)
        for i, (cx, cy) in enumerate(idx):
            cx, cy = int(cx), int(cy)
            cand: list[int] = []
            hit_ring = None
            for r in range(max_rings + 1):
                for dx in range(-r, r + 1):
                    for dy in range(-r, r + 1):
                        if r and max(abs(dx), abs(dy)) != r:
                            continue
                        cand += self.buckets.get((cx + dx, cy + dy), ())
                if cand and hit_ring is None:
                    hit_ring = r
                # one ring past the first hit, so a diagonal neighbour wins
                if hit_ring is not None and r > hit_ring:
                    break
            if not cand:
                continue
            c = np.asarray(cand)
            d = ((self.pts[c] - q[i]) ** 2).sum(1)
            j = int(d.argmin())
            best_d[i] = math.sqrt(d[j])
            best_i[i] = c[j]
        return best_d, best_i


# ------------------------------------------------------------- the track


class Track:
    """The YAML centerline, resampled, with station / side lookups."""

    STEP = 2.0

    def __init__(self, stem: str):
        self.stem = stem
        self.data = yaml.safe_load(
            (TRACK_DIR / f"{stem}.yaml").read_text(encoding="utf-8")
        )
        nodes = self.data["nodes"]
        raw = np.array([[n["x"], n["y"]] for n in nodes])
        dw = self.data.get("default_width", 10.0)
        wl = np.array([n.get("width_left") or dw / 2 for n in nodes])
        wr = np.array([n.get("width_right") or dw / 2 for n in nodes])
        self.closed = bool(self.data.get("closed_loop", True))
        self.pts = densify(raw, self.STEP, self.closed)
        # widths resampled onto the same stations
        seg = np.hypot(*np.diff(np.vstack([raw, raw[:1]]) if self.closed else raw, axis=0).T)
        s_raw = np.concatenate([[0.0], np.cumsum(seg)])[: len(raw)]
        self.station = np.concatenate(
            [[0.0], np.cumsum(np.hypot(*np.diff(self.pts, axis=0).T))]
        )
        self.total = float(self.station[-1])
        self.width_left = np.interp(self.station, s_raw, wl, period=self.total)
        self.width_right = np.interp(self.station, s_raw, wr, period=self.total)
        d = np.gradient(self.pts, axis=0)
        self.heading = np.arctan2(d[:, 1], d[:, 0])
        # A coarse cell keeps the ring search shallow for the thousands
        # of town buildings inside the Sarthe's bounding box; the search
        # always scans one ring past the first hit, so the answer is still
        # the true nearest.
        self.grid = NearestGrid(self.pts, cell=40.0)

    def locate(self, q) -> tuple[np.ndarray, np.ndarray]:
        """(station, signed lateral: positive left) for each query point."""
        q = np.asarray(q, dtype=float).reshape(-1, 2)
        _, j = self.grid.query(q)
        h = self.heading[j]
        d = q - self.pts[j]
        lat = -np.sin(h) * d[:, 0] + np.cos(h) * d[:, 1]
        along = np.cos(h) * d[:, 0] + np.sin(h) * d[:, 1]
        return (self.station[j] + along) % self.total, lat

    def index_at(self, station: float) -> int:
        """The sample nearest a station. Densifying splits each source
        segment into equal parts, so the spacing is only *about* STEP and
        the index has to be searched, not divided for."""
        return int(np.searchsorted(self.station, station % self.total).clip(0, len(self.pts) - 1))

    def half_width(self, station: float, side: str) -> float:
        i = self.index_at(station)
        return float(self.width_left[i] if side == "left" else self.width_right[i])

    def turn_side(self, from_m: float, to_m: float, which: str) -> str:
        """`outside` / `inside` of the bend over a station span, resolved
        from the course's own curvature - so a dossier entry can say where
        a stand is the way a spectator map does."""
        span = (to_m - from_m) % self.total if to_m < from_m else to_m - from_m
        i0, i1 = self.index_at(from_m), self.index_at(from_m + span)
        h0, h1 = self.heading[i0], self.heading[i1]
        turn = (h1 - h0 + math.pi) % (2 * math.pi) - math.pi
        left_turn = turn > 0
        if which == "inside":
            return "left" if left_turn else "right"
        return "right" if left_turn else "left"

    def offset_point(self, station: float, side: str, beyond_edge: float) -> np.ndarray:
        """A point `beyond_edge` metres past the road edge on `side`."""
        i = self.index_at(station)
        lat = self.half_width(station, side) + beyond_edge
        sign = 1.0 if side == "left" else -1.0
        h = self.heading[i]
        return self.pts[i] + sign * lat * np.array([-math.sin(h), math.cos(h)])

    def edge_run(self, from_m: float, to_m: float, side: str, beyond_edge: float) -> np.ndarray:
        """A polyline following the road edge over a station span, which is
        how a hand-placed stand gets the same curve as a traced one."""
        span = (to_m - from_m) % self.total if to_m < from_m else to_m - from_m
        steps = max(int(span / 10.0), 1)
        return np.array(
            [
                self.offset_point(from_m + span * i / steps, side, beyond_edge)
                for i in range(steps + 1)
            ]
        )


# --------------------------------------------------------------- OSM reading


class Osm:
    def __init__(self, paths: list[Path]):
        self.nodes: dict[int, tuple[float, float]] = {}
        self.node_tags: dict[int, dict] = {}
        self.ways: list[dict] = []
        self.relations: list[dict] = []
        # The tiles overlap at their seams, so the same way arrives more
        # than once; a duplicated way breaks chaining (a pit lane joined to
        # its own copy doubles back) and double-counts buildings.
        seen_ways: set[int] = set()
        seen_relations: set[int] = set()
        for p in paths:
            for e in json.loads(p.read_text(encoding="utf-8"))["elements"]:
                if e["type"] == "node":
                    self.nodes[e["id"]] = (e["lon"], e["lat"])
                    if e.get("tags"):
                        self.node_tags[e["id"]] = e["tags"]
                elif e["type"] == "way":
                    if e["id"] in seen_ways:
                        continue
                    seen_ways.add(e["id"])
                    self.ways.append(e)
                elif e["type"] == "relation":
                    if e["id"] in seen_relations:
                        continue
                    seen_relations.add(e["id"])
                    self.relations.append(e)
        lons = [n[0] for n in self.nodes.values()]
        lats = [n[1] for n in self.nodes.values()]
        self.lon0 = (min(lons) + max(lons)) / 2
        self.lat0 = (min(lats) + max(lats)) / 2
        self.ways_by_id: dict[int, dict] = {w["id"]: w for w in self.ways}

    def way_xy(self, w) -> np.ndarray | None:
        pts = [self.nodes[n] for n in w["nodes"] if n in self.nodes]
        if len(pts) < 2:
            return None
        return np.array([enu(p[0], p[1], self.lon0, self.lat0) for p in pts])


def osm_name(t: dict) -> str | None:
    """A way/node's own name, falling back through OSM's localised name
    tags.  Montreal's pit lane carries only `name:en`/`name:fr` and no
    plain `name` at all; a circuit whose ways are named only in their own
    language would be silently unnamed without this."""
    return t.get("name") or t.get("name:en") or t.get("name:fr") or t.get("name:de")


def relation_raceway_roles(osm: Osm) -> dict[int, str]:
    """Way id -> its role in an OSM route relation tagged highway=raceway.

    Every circuit so far tags each stretch of tarmac highway=raceway
    directly. A street circuit run on public roads can instead model the
    lap as a single `type=circuit` relation carrying the raceway tag, with
    every member way tagged only as the ordinary street it is (Norisring:
    Ben-Gurion-Ring and the Beuthener/Zeppelin/Karl-Steigelmann streets are
    highway=secondary/unclassified/service, and it is relation 1889538,
    "Norisring", that carries highway=raceway). Folding those members in is
    what lets the fit and the pit-lane search see the lap at all; a
    directly-tagged circuit is unaffected, since none of the others has
    such a relation.
    """
    roles: dict[int, str] = {}
    for r in osm.relations:
        if (r.get("tags") or {}).get("highway") != "raceway":
            continue
        for m in r.get("members", []):
            if m.get("type") == "way":
                roles.setdefault(m["ref"], m.get("role") or "")
    return roles


def is_pit_way(t: dict) -> bool:
    name = (osm_name(t) or "").lower()
    # German circuits name the pit lane "Boxengasse" (Oschersleben) or
    # "Boxenstraße" (Spielberg; also Hockenheim, the Nürburgring):
    # no "pit" substring at all, so the English-only check missed both.
    # "boxen" (rather than the whole word) is the generic match, since any
    # German compound built on it names the same thing; `osm_name()`'s
    # localised-tag fallback catches a "Pit Lane" translation tag OSM
    # carries on some ways (Spielberg's does, Montreal's pit lane only has
    # name:en/name:fr at all) even when the local-language name has neither.
    # The Hungaroring's is "Bokszutca" (Hungarian, the same "box" root +
    # "utca" = street): same gap, different language, so it needs its own
    # substring -- "boxen" doesn't match the Hungarian spelling.
    return (
        t.get("raceway") in ("pitlane", "pit_lane")
        or "pit" in name
        or "boxen" in name
        or "bokszutca" in name
    )


# A street circuit run on public roads only for one weekend a year (Albert
# Park; Norisring and others share the risk) carries no `highway=raceway`
# tag at all, and OSM has never mapped one under a circuit-name relation
# either -- there is nothing raceway-specific left to select on. The classes
# below are the ones an ordinary road lap is built from.
STREET_CIRCUIT_HIGHWAYS = frozenset(
    {
        "primary",
        "primary_link",
        "secondary",
        "secondary_link",
        "tertiary",
        "tertiary_link",
        "trunk",
        "trunk_link",
        "unclassified",
    }
)


def raceway_cloud(osm: Osm) -> np.ndarray:
    roles = relation_raceway_roles(osm)
    pts = []
    for w in osm.ways:
        t = w.get("tags") or {}
        role = roles.get(w["id"])
        if t.get("highway") != "raceway" and role is None:
            continue
        if role == "pit_lane":
            continue
        if is_pit_way(t) or "kart" in (osm_name(t) or "").lower():
            continue
        xy = osm.way_xy(w)
        if xy is not None:
            pts.append(densify(xy, 4.0, closed=False))
    if pts:
        return np.concatenate(pts)
    # No raceway ways at all: fall back to the drivable public-road classes
    # in the bbox and let the coarse FFT correlation plus trimmed ICP find
    # the lap among the surrounding street grid -- the loop is still the
    # strongest self-similar shape in the cloud, and `build()`'s
    # coverage/rmse gate (including its partial-fit exception, exercised
    # first by Le Mans) is what catches a case where that isn't true rather
    # than this function guessing right.
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("highway") not in STREET_CIRCUIT_HIGHWAYS:
            continue
        xy = osm.way_xy(w)
        if xy is not None:
            pts.append(densify(xy, 4.0, closed=False))
    if not pts:
        raise SystemExit("no raceway (or street-circuit road) ways in the extract")
    return np.concatenate(pts)


# ------------------------------------------------------------------ the fit


def coarse_fit(cloud: np.ndarray, cl: np.ndarray, steps=720, res=None, keep=10):
    span = max(np.ptp(cl[:, 0]), np.ptp(cl[:, 1]))
    # The correlation grid covers the whole circuit, so its cell grows with
    # the circuit: a 6 m cell over the Sarthe's 5 km is a 2000-square FFT
    # per rotation.  ICP takes the fit the rest of the way regardless.
    if res is None:
        res = max(6.0, span / 400.0)
    lo = np.array([-span * 1.2, -span * 1.2])
    hi = -lo
    shape = (int((hi[1] - lo[1]) / res) + 1, int((hi[0] - lo[0]) / res) + 1)

    def grid(pts):
        g = np.zeros(shape)
        ix = ((pts[:, 0] - lo[0]) / res).astype(int)
        iy = ((pts[:, 1] - lo[1]) / res).astype(int)
        ok = (ix >= 0) & (iy >= 0) & (ix < shape[1]) & (iy < shape[0])
        np.add.at(g, (iy[ok], ix[ok]), 1.0)
        return np.minimum(g, 1.0)

    c_osm = cloud.mean(0)
    f_osm = np.fft.rfft2(grid(cloud - c_osm))
    c_cl = cl.mean(0)
    # A site with several layouts on it (Silverstone's GP, National, Stowe
    # and karting tracks share tarmac) gives the correlation more than one
    # strong peak, and the tallest is not always the right one - so keep
    # the best few and let the caller score them by how much of the
    # centerline each one actually explains.
    cands = []
    for k in range(steps):
        th = 2 * math.pi * k / steps
        c, s = math.cos(th), math.sin(th)
        corr = np.fft.irfft2(
            f_osm * np.conj(np.fft.rfft2(grid((cl - c_cl) @ np.array([[c, s], [-s, c]])))),
            shape,
        )
        iy, ix = divmod(int(np.argmax(corr)), corr.shape[1])
        dy = iy if iy < shape[0] // 2 else iy - shape[0]
        dx = ix if ix < shape[1] // 2 else ix - shape[1]
        cands.append((float(corr[iy, ix]), th, np.array([dx * res, dy * res])))
    cands.sort(key=lambda c: -c[0])
    out = []
    for score, th, shift in cands:
        # non-maximum suppression: candidates within 6 deg of one already
        # kept are the same peak seen again
        if any(abs((th - o[0] + math.pi) % (2 * math.pi) - math.pi) < math.radians(6) for o in out):
            continue
        c, s = math.cos(-th), math.sin(-th)
        a = np.array([[c, -s], [s, c]])  # track = osm @ a.T + t
        out.append((th, a, -(c_osm + shift) @ a.T + c_cl))
        if len(out) >= keep:
            break
    return [(a, t) for _, a, t in out]


def icp(cloud: np.ndarray, cl: np.ndarray, a, t, iters=15, trim=0.55):
    grid = NearestGrid(cl)
    for _ in range(iters):
        # shallow probes: a point with no track cell nearby is an extra
        # circuit on the same site and gets trimmed away anyway
        d, j = grid.query(cloud @ a.T + t, max_rings=2)
        finite = d[np.isfinite(d)]
        cut = float(np.quantile(finite, trim)) if len(finite) else 3.0
        keep = d <= max(cut, 3.0)
        if keep.sum() < 8:
            break
        x, y = cloud[keep], cl[j[keep]]
        mx, my = x.mean(0), y.mean(0)
        u, _, vt = np.linalg.svd((x - mx).T @ (y - my))
        r = (u @ vt).T
        if np.linalg.det(r) < 0:
            vt[-1] *= -1
            r = (u @ vt).T
        a, t = r, my - mx @ r.T
    p = cloud @ a.T + t
    d, _ = grid.query(p, max_rings=2)
    inl = d[d < 10.0]
    cov, _ = NearestGrid(p).query(cl, max_rings=2)
    return a, t, {
        "rmse_m": round(float(np.sqrt((inl**2).mean())) if len(inl) else 999.0, 2),
        "raceway_matched": round(float((d < 10).mean()), 3),
        "centerline_covered": round(float((cov < 6.0).mean()), 3),
        "centerline_covered_m": round(float((cov < 6.0).sum()) * Track.STEP),
    }


# --------------------------------------------------------------- extraction


def oriented_box(poly: np.ndarray):
    """(centre, length, depth, yaw) of the minimum-area box round a ring."""
    c = poly.mean(0)
    d = poly - c
    best = None
    for i in range(len(poly) - 1):
        e = poly[i + 1] - poly[i]
        n = float(np.hypot(*e))
        if n < 0.5:
            continue
        u = e / n
        r = np.array([[u[0], u[1]], [-u[1], u[0]]])
        p = d @ r.T
        ext = np.ptp(p, axis=0)
        area = ext[0] * ext[1]
        if best is None or area < best[0]:
            best = (area, ext, math.atan2(u[1], u[0]), p.mean(0) @ r + c)
    if best is None:
        return c, 1.0, 1.0, 0.0
    _, ext, yaw, centre = best
    if ext[1] > ext[0]:
        ext = ext[::-1]
        yaw += math.pi / 2
    return centre, float(ext[0]), float(ext[1]), float((yaw + math.pi) % math.pi)


def front_edge(poly: np.ndarray, track: Track) -> np.ndarray:
    """The run of the outline that faces the road: the longest chain of
    vertices nearer the track than the outline's midpoint distance.  A
    rectangular stand yields its long side, a stand built round a bend
    yields the curve, and laying bays along it reproduces the shape."""
    ring = poly[:-1] if np.allclose(poly[0], poly[-1]) else poly
    d, _ = track.grid.query(ring)
    thresh = (d.min() + d.max()) / 2
    near = d <= thresh
    n = len(ring)
    if not near.any():
        return ring
    best = (0, 0, 0)
    for start in range(n):
        if not near[start] or near[(start - 1) % n]:
            continue
        k = 0
        while k < n and near[(start + k) % n]:
            k += 1
        chain = np.array([ring[(start + i) % n] for i in range(k)])
        length = float(np.hypot(*np.diff(chain, axis=0).T).sum()) if k > 1 else 0.0
        if length > best[0]:
            best = (length, start, k)
    _, start, k = best
    if k < 2:
        return ring
    chain = np.array([ring[(start + i) % n] for i in range(k)])
    # A huge or oddly-shaped building (Shanghai's Grandstand A, traced as
    # one OSM way, runs the full pit straight and wraps round both faces
    # of the complex) can still win the "nearer than the midpoint
    # distance" test on both its near and far sides, so the longest near
    # chain jumps from one side of the road to the other and back --
    # laying bays along it would cut straight across the track. Keep only
    # the longest run that stays on one side of the centerline; an
    # ordinary stand's near edge never leaves its own side, so this is a
    # no-op for it.
    lat = track.locate(chain)[1]
    signs = np.sign(lat)
    nonzero = signs[signs != 0]
    if len(nonzero) and len(set(nonzero)) > 1:
        best_run = (0.0, 0, 0)
        i = 0
        m = len(signs)
        while i < m:
            j = i
            while j + 1 < m and signs[j + 1] == signs[i]:
                j += 1
            run_len = (
                float(np.hypot(*np.diff(chain[i : j + 1], axis=0).T).sum())
                if j > i
                else 0.0
            )
            if run_len > best_run[0]:
                best_run = (run_len, i, j)
            i = j + 1
        _, i, j = best_run
        if j > i:
            chain = chain[i : j + 1]
    return chain


def front_edge_is_broken(front: np.ndarray) -> bool:
    """A simple stand's front is a straight or gently curved edge traced
    corner to corner, so a short-long-short run of segments (two end caps
    either side of the long face) is the ordinary shape of a rectangular
    building and not what this catches, however long the long face is
    (Oschersleben's 360 m Zuschauertribuene, Silverstone's 208 m Becketts,
    Monza's 220 m Tribuna Laterale Destra all look exactly like this).

    What it does catch is a longer chain -- at least two short segments on
    *each* side of the outlier, i.e. the trace keeps following a real
    facted or curved edge after the jump rather than immediately closing
    off a simple box -- where one segment is both long in absolute terms
    and dwarfs its neighbours: `front_edge()` only ever walks vertices
    that were genuinely adjacent in the outline, so following a real edge
    keeps segment lengths within a similar order of magnitude of each
    other. A lone outlier that much longer than the rest means two
    disjoint near-clusters got stitched across open ground -- the outline
    is really a complex spanning more than one feature (Sepang's Main
    Grandstand, one OSM way for the whole double-fronted stand between two
    straights 650 m apart) and cannot be read as one stand's face."""
    if len(front) < 5:
        return False
    segs = np.sort(np.hypot(*np.diff(front, axis=0).T))[::-1]
    biggest, second = float(segs[0]), float(segs[1])
    return biggest > 150.0 and biggest > 4.0 * max(second, 1.0) and biggest > 0.5 * float(segs.sum())


def ring_area(poly: np.ndarray) -> float:
    x, y = poly[:, 0], poly[:, 1]
    return float(abs(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1))) / 2)


def round_pts(p, nd=2):
    return [[round(float(x), nd), round(float(y), nd)] for x, y in np.asarray(p)]


def chain_ways(ways: list[np.ndarray], tol=3.0) -> list[np.ndarray]:
    """Join polylines that share an endpoint into runs."""
    runs = [w.copy() for w in ways]
    merged = True
    while merged:
        merged = False
        for i in range(len(runs)):
            for j in range(len(runs)):
                if i == j or runs[i] is None or runs[j] is None:
                    continue
                a, b = runs[i], runs[j]
                if np.hypot(*(a[-1] - b[0])) < tol:
                    runs[i] = np.vstack([a, b[1:]])
                elif np.hypot(*(a[-1] - b[-1])) < tol:
                    runs[i] = np.vstack([a, b[::-1][1:]])
                elif np.hypot(*(a[0] - b[0])) < tol:
                    runs[i] = np.vstack([a[::-1], b[1:]])
                elif np.hypot(*(a[0] - b[-1])) < tol:
                    runs[i] = np.vstack([b, a[1:]])
                else:
                    continue
                runs[j] = None
                merged = True
                break
            if merged:
                break
        runs = [r for r in runs if r is not None]
    return runs


# Set by `extract` so the surroundings pass can place a node with the same
# fit transform the rest of the dossier uses.
_TO_TRACK = None


def to_track_node(osm, nid: int):
    if _TO_TRACK is None or nid not in osm.nodes:
        return None
    return _TO_TRACK(np.array([enu(*osm.nodes[nid], osm.lon0, osm.lat0)]))[0]


# ----------------------------------------------------- surroundings layers

# What a circuit is set in, beyond the stands and the pit building: the
# barriers that line it, the service roads and lanes around it, the fields,
# car parks and camp sites it sits among, and the handful of point features
# worth standing a mesh up for. All of it is in the extract already and was
# thrown away until now, which is why every circuit's surroundings were
# invented rather than observed.

# `barrier=*` values that are circuit protection or a real fence line.
BARRIER_KINDS = {
    "guard_rail": "guard_rail",
    "tyres": "tyres",
    "wall": "wall",
    "block": "wall",
    "jersey_barrier": "wall",
    "retaining_wall": "wall",
    "fence": "fence",
    "hedge": "hedge",
}

# `highway=*` values kept as roads, mapped to the class the dresser lays.
ROAD_KINDS = {
    "motorway": "major",
    "motorway_link": "major",
    "trunk": "major",
    "primary": "major",
    "secondary": "major",
    "tertiary": "minor",
    "unclassified": "minor",
    "residential": "minor",
    "living_street": "minor",
    "service": "service",
    "track": "track",
    "footway": "path",
    "path": "path",
    "steps": "path",
    "cycleway": "path",
    "pedestrian": "path",
}

# Area layers: (tag key, tag value) -> the kind the dresser dresses.
AREA_KINDS = {
    ("amenity", "parking"): "parking",
    ("tourism", "camp_site"): "camp_site",
    ("tourism", "caravan_site"): "camp_site",
    ("landuse", "grass"): "grass",
    ("landuse", "village_green"): "grass",
    ("landuse", "meadow"): "meadow",
    ("landuse", "farmland"): "farmland",
    ("landuse", "farmyard"): "farmyard",
    ("landuse", "residential"): "residential",
    ("landuse", "industrial"): "industrial",
    ("landuse", "cemetery"): "cemetery",
    ("natural", "scrub"): "scrub",
    ("natural", "heath"): "scrub",
    ("natural", "grassland"): "meadow",
    ("natural", "water"): "water",
    ("natural", "shingle"): "shingle",
    ("natural", "sand"): "sand",
    ("natural", "bare_rock"): "rock",
    ("leisure", "park"): "grass",
    ("leisure", "golf_course"): "grass",
    ("leisure", "pitch"): "pitch",
}

# `waterway=*` values kept as a line on the ground. A stream is drawn and
# planted like a road is, which is why the two share a shape.
WATER_KINDS = {
    "river": "river",
    "stream": "stream",
    "ditch": "ditch",
    "drain": "ditch",
    "canal": "river",
}

# Point features worth a prop, by the tag that identifies them. A camp
# site or a car park is usually a polygon (`AREA_KINDS`), but the Red Bull
# Ring's five named camps are nodes, so both are looked for here as well
# and the dresser clusters around the point when it has no outline.
POI_KINDS = {
    ("tourism", "camp_site"): "camp_site",
    ("tourism", "caravan_site"): "camp_site",
    ("amenity", "restaurant"): "food",
    ("amenity", "fast_food"): "food",
    ("amenity", "cafe"): "food",
    ("amenity", "bar"): "food",
    ("amenity", "biergarten"): "food",
    ("amenity", "place_of_worship"): "chapel",
    ("amenity", "fuel"): "fuel",
    ("amenity", "toilets"): "toilets",
    ("tourism", "information"): "info",
    ("tourism", "artwork"): "artwork",
    ("power", "tower"): "pylon",
    ("barrier", "gate"): "gate",
    ("barrier", "lift_gate"): "gate",
    ("highway", "street_lamp"): "lamp",
}

# Tags the rest of the pipeline consumes outside this pass -- the course
# itself, the stands, the structures and the woods -- so a way carrying one
# is not reported as dropped.
CENSUS_CLAIMED_ELSEWHERE = {
    ("highway", "raceway"),
    ("building", "grandstand"),
    ("leisure", "grandstand"),
    ("natural", "wood"),
    ("landuse", "forest"),
    ("landuse", "orchard"),
}


def _ring_or_line(p, tol: float):
    """Simplify a way and say whether it closed on itself. An OSM area is a
    closed way: its first and last node are the same one."""
    closed = len(p) > 3 and float(np.hypot(*(p[0] - p[-1]))) < 0.5
    return simplify(p, tol), closed


def _poi_entry(track: Track, kind: str, t: dict, centre) -> dict:
    s, lat = track.locate(centre[None, :])
    entry = {
        "kind": kind,
        "station_m": round(float(s[0]), 1),
        "side": "left" if lat[0] > 0 else "right",
        "centre": round_pts(centre[None, :])[0],
    }
    name = osm_name(t)
    if name:
        entry["name"] = name
    return entry


def _first_match(t: dict, table: dict):
    for (key, value), out in table.items():
        if t.get(key) == value:
            return out
    return None


def extract_surroundings(stem: str, track: Track, osm: Osm, xy) -> dict:
    """The barrier, road, area and point-of-interest layers, plus a census
    of everything tagged near the road that no rule claims.

    `xy` is `extract`'s own way-to-track-frame helper, so a way outside the
    circuit's bounding box costs nothing here either. Nothing in here is
    allowed to fail a dossier: a layer that comes out empty means the
    mapping has none of it, which the census then says out loud."""
    barriers: list[dict] = []
    roads: list[dict] = []
    waterways: list[dict] = []
    areas: list[dict] = []
    poi: list[dict] = []
    census: dict[str, int] = {}

    def near(p) -> float:
        d, _ = track.grid.query(p, max_rings=30)
        return float(d.min())

    for w in osm.ways:
        t = w.get("tags") or {}
        if not t:
            continue
        p = xy(w)
        if p is None:
            continue
        d = near(p)
        claimed = "building" in t or any(
            t.get(k) == v for k, v in CENSUS_CLAIMED_ELSEWHERE
        )

        barrier = BARRIER_KINDS.get(t.get("barrier", ""))
        if barrier is not None and d < BARRIER_RANGE_M:
            line, closed = _ring_or_line(p, 1.0)
            if len(line) >= 2:
                entry = {"kind": barrier, "line": round_pts(line, 1)}
                if closed:
                    entry["closed"] = True
                barriers.append(entry)
                claimed = True

        # The lap itself and the pit lane are the course, not scenery.
        road = ROAD_KINDS.get(t.get("highway", ""))
        if road is not None and d < ROAD_RANGE_M:
            line, _ = _ring_or_line(p, 3.0)
            if len(line) >= 2:
                entry = {"kind": road, "line": round_pts(line, 1)}
                name = osm_name(t)
                if name:
                    entry["name"] = name
                if t.get("bridge"):
                    entry["bridge"] = True
                if t.get("tunnel"):
                    entry["tunnel"] = True
                roads.append(entry)
                claimed = True

        water = WATER_KINDS.get(t.get("waterway", ""))
        if water is not None and d < ROAD_RANGE_M:
            line, _ = _ring_or_line(p, 3.0)
            if len(line) >= 2:
                entry = {"kind": water, "line": round_pts(line, 1)}
                name = osm_name(t)
                if name:
                    entry["name"] = name
                if t.get("tunnel") or t.get("culvert"):
                    entry["tunnel"] = True
                waterways.append(entry)
                claimed = True

        area_kind = _first_match(t, AREA_KINDS)
        # Woodland has its own layer already (`woods`), with the leaf type.
        if t.get("natural") == "wood" or t.get("landuse") in ("forest", "orchard"):
            area_kind = None
        if area_kind is not None and d < AREA_RANGE_M:
            ring, closed = _ring_or_line(p, 4.0)
            if closed and len(ring) >= 4 and ring_area(ring) >= 200.0:
                entry = {"kind": area_kind, "ring": round_pts(ring, 1)}
                name = osm_name(t)
                if name:
                    entry["name"] = name
                areas.append(entry)
                claimed = True

        # A point feature mapped as a small building or enclosure rather
        # than as a node -- a chapel usually is one -- takes its centre.
        poi_kind = _first_match(t, POI_KINDS)
        if poi_kind is not None and area_kind is not None:
            # Mapped as a polygon: the area layer already has its outline,
            # which is strictly better than a point.
            poi_kind = None
        if poi_kind is not None and d < POI_RANGE_M:
            poi.append(_poi_entry(track, poi_kind, t, p.mean(0)))
            claimed = True

        # Whatever no rule above wanted, so the next circuit's gaps show
        # up in the dossier instead of in a screenshot.
        if not claimed and d < CENSUS_RANGE_M:
            for key in ("barrier", "highway", "landuse", "natural", "leisure",
                        "amenity", "tourism", "waterway", "man_made", "power"):
                if key in t:
                    label = f"{key}={t[key]}"
                    census[label] = census.get(label, 0) + 1
                    break

    for nid, t in osm.node_tags.items():
        poi_kind = _first_match(t, POI_KINDS)
        if poi_kind is None:
            continue
        c = to_track_node(osm, nid)
        if c is None or near(c[None, :]) > POI_RANGE_M:
            continue
        poi.append(_poi_entry(track, poi_kind, t, c))

    barriers.sort(key=lambda e: (e["kind"], e["line"][0]))
    roads.sort(key=lambda e: (e["kind"], e["line"][0]))
    waterways.sort(key=lambda e: (e["kind"], e["line"][0]))
    areas.sort(key=lambda e: (e["kind"], e["ring"][0]))
    poi.sort(key=lambda e: (e["station_m"], e["kind"]))
    return {
        "barriers": barriers,
        "roads": roads,
        "waterways": waterways,
        "areas": areas,
        "poi": poi,
        "unclassified": dict(sorted(census.items(), key=lambda kv: (-kv[1], kv[0]))),
    }


def manual_pit_lane(track: Track, spec: dict) -> dict:
    """A pit lane authored along the road edge, for a circuit whose pit lane
    OSM gets wrong or leaves out (see MANUAL_PIT_LANE)."""
    nodes = track.edge_run(spec["from_m"], spec["to_m"], spec["side"], spec.get("gap_m", 6.0))
    length = float(np.hypot(*np.diff(nodes, axis=0).T).sum())
    return {
        "side": spec["side"],
        "length_m": round(length, 1),
        "nodes": round_pts(nodes),
        "source": "authored",
    }


def extract(stem: str, track: Track, osm: Osm, to_track, fit_report) -> dict:
    lo = track.pts.min(0) - 400.0
    hi = track.pts.max(0) + 400.0
    # Members of a street circuit's route relation (Norisring's
    # Beuthener Strasse et al): the lap's own tarmac, tagged only as an
    # ordinary street, so anything below that would otherwise take a raised
    # or bridged stretch of it for a structure crossing the road needs to
    # know about it too.
    relation_roles = relation_raceway_roles(osm)

    def xy(w):
        p = osm.way_xy(w)
        if p is None:
            return None
        p = to_track(p)
        # Nothing this far outside the circuit's own bounding box can be
        # part of it; skipping it here keeps the distance queries shallow.
        if (p.max(0) < lo).any() or (p.min(0) > hi).any():
            return None
        return p

    global _TO_TRACK
    _TO_TRACK = to_track

    corners = []
    pit_ways: list[np.ndarray] = []
    # A way can be both directly tagged highway=raceway (picked up by the
    # loop below) and a role=pit_lane member of a type=circuit relation
    # (Monza is both: its "Pit Lane" way carries the tag *and* belongs to
    # relation 284565) -- track which way ids already went into pit_ways so
    # the relation pass after this loop doesn't add the same geometry a
    # second time. chain_ways() would otherwise treat two identical copies
    # as sharing both endpoints and loop them into one chain at roughly
    # double the real length.
    pit_way_ids: set[int] = set()
    # Ways that are neither part of the fitted main loop (they run wide of
    # it) nor named/tagged as anything in particular -- a circuit whose pit
    # lane carries no "pit" name and no raceway=pitlane tag (nothing in the
    # Shanghai extract does) still has it in here, just unlabelled.
    untagged_ways: list[np.ndarray] = []
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("highway") != "raceway":
            continue
        p = xy(w)
        if p is None:
            continue
        d, _ = track.grid.query(p, max_rings=30)
        if is_pit_way(t):
            if float(np.median(d)) < 80.0 and len(p) > 2:
                pit_ways.append(p)
                pit_way_ids.add(w["id"])
            continue
        name = osm_name(t)
        if not name or float(np.median(d)) > 12.0:
            if (
                not name
                and len(p) > 2
                and 12.0 < float(np.median(d)) < 90.0
            ):
                untagged_ways.append(p)
            continue
        mid = p[len(p) // 2 : len(p) // 2 + 1]
        s0, _ = track.locate(p[:1])
        s1, _ = track.locate(p[-1:])
        sm, _ = track.locate(mid)
        corners.append(
            {
                "name": name,
                "station_m": round(float(sm[0]), 1),
                "from_m": round(float(s0[0]), 1),
                "to_m": round(float(s1[0]), 1),
            }
        )
    corners.sort(key=lambda c: c["station_m"])
    # one entry per name (OSM splits long sections)
    seen: dict[str, dict] = {}
    for c in corners:
        seen.setdefault(c["name"], c)
    corners = sorted(seen.values(), key=lambda c: c["station_m"])

    # A street circuit modelled as a route relation (see
    # relation_raceway_roles) carries no highway=raceway tag on its member
    # ways, so the loop above never saw them; only its "pit_lane" role
    # member is useful here -- the members' own names are ordinary street
    # names, not corner names, so nothing is added to `corners` for them.
    for way_id, role in relation_roles.items():
        if role != "pit_lane" or way_id in pit_way_ids:
            continue
        w = osm.ways_by_id.get(way_id)
        if w is None:
            continue
        p = xy(w)
        if p is not None and len(p) > 2:
            pit_ways.append(p)

    def pit_lane_from(ways: list[np.ndarray]) -> dict | None:
        # A circuit has more than one lane tagged "pit" (Spa's support pit
        # lane, the Bugatti lanes at Le Mans, the roads behind the garages),
        # and chaining joins whatever shares an end. Clip every chain to the
        # part that runs beside the track, keep the ones of a pit lane's
        # length, and take whichever reaches the start/finish line.
        candidates = []
        for run in chain_ways(ways):
            run = densify(run, 8.0, closed=False)
            d, _ = track.grid.query(run, max_rings=4)
            near = d < 70.0
            span = None
            i = 0
            while i < len(run):
                if not near[i]:
                    i += 1
                    continue
                j = i
                while j + 1 < len(run) and near[j + 1]:
                    j += 1
                if span is None or (j - i) > (span[1] - span[0]):
                    span = (i, j)
                i = j + 1
            if span is None or span[1] - span[0] < 2:
                continue
            clipped = run[span[0] : span[1] + 1]
            length = float(np.hypot(*np.diff(clipped, axis=0).T).sum())
            # Interlagos's is confirmed (press coverage of its pit-stop time
            # loss) as the longest pit lane on the F1 calendar at roughly
            # 1100-1400 m, well past the 1200 m ceiling tuned on the first
            # six circuits; 1600 m still rejects a chain that grabbed
            # unrelated roads while admitting a real long lane.
            if not 120.0 <= length <= 1600.0:
                continue
            s_here, _ = track.locate(clipped)
            to_line = float(np.minimum(s_here, track.total - s_here).min())
            candidates.append((to_line, length, clipped))
        if not candidates:
            return None
        # Of the lanes that reach the start/finish line, the circuit's own
        # is the long one: the others are a support paddock's or a link
        # road that happens to pass it.
        at_line = [c for c in candidates if c[0] < 60.0]
        if at_line:
            at_line.sort(key=lambda c: -c[1])
            candidates = at_line
        else:
            candidates.sort(key=lambda c: c[0])
        best = candidates[0][2]
        s_best, lat = track.locate(best)
        side = "left" if float(np.median(lat)) > 0 else "right"
        # In course order, so the entry is the first node.
        ang = np.unwrap(s_best / track.total * 2 * math.pi)
        if float(np.median(np.diff(ang))) < 0:
            best = best[::-1]
        return {
            "side": side,
            "length_m": round(candidates[0][1], 1),
            "nodes": round_pts(densify(best, 12.0, closed=False)),
        }

    # A hand-authored pit lane wins outright. It exists precisely because a
    # human looked at a circuit and found OSM's version wrong or absent, so
    # letting an automatic match override it throws that judgement away.
    # This is not hypothetical: rebuilt from the current Albert Park
    # extract, the untagged-way fallback below matches a 711 m "pit lane"
    # that runs straight across the race track, where the real one is a
    # 370 m run along the start straight -- and the pit walls the bake
    # generates along it stood in the middle of the road, where the AI
    # spent a quarter of every race stuck against them.
    pit = None
    if stem in MANUAL_PIT_LANE:
        pit = manual_pit_lane(track, MANUAL_PIT_LANE[stem])
    if pit is None:
        pit = pit_lane_from(pit_ways)
    if pit is None:
        # Nothing was tagged as a pit lane at all. Fall back to any
        # raceway way that runs wide of the main loop instead of on it
        # (the untagged pool collected above): the same near/length/
        # reaches-the-line filter above is strict enough (a contiguous
        # 120-1600 m run that comes back within 60 m of the start/finish
        # station) that an unrelated paddock spur will not pass it by
        # accident, and every existing dossier already finds its pit lane
        # from a tagged way, so this path never fires for them.
        pit = pit_lane_from(untagged_ways)

    stands = []
    structures = []
    for w in osm.ways:
        t = w.get("tags") or {}
        p = xy(w)
        if p is None or len(p) < 4:
            continue
        is_stand = (
            t.get("building") == "grandstand"
            or t.get("leisure") == "grandstand"
            or t.get("grandstand") == "yes"
        )
        # A footbridge over the track is mapped as building=bridge; it is
        # a crossing, not a building to stand beside the road.
        is_building = "building" in t and not is_stand and t.get("building") != "bridge"
        if not (is_stand or is_building):
            continue
        d, _ = track.grid.query(p, max_rings=30)
        near = float(d.min())
        if is_stand and near > STAND_RANGE_M:
            continue
        if is_building and (near > STRUCTURE_RANGE_M or ring_area(p) < 250.0):
            continue
        centre, length, depth, yaw = oriented_box(p)
        front = front_edge(p, track) if is_stand else None
        if is_stand and front_edge_is_broken(front):
            continue
        s, lat = track.locate(centre[None, :])
        entry = {
            "name": osm_name(t),
            "station_m": round(float(s[0]), 1),
            "side": "left" if lat[0] > 0 else "right",
            "offset_m": round(near, 1),
            "length_m": round(length, 1),
            "depth_m": round(depth, 1),
            "yaw_rad": round(yaw, 4),
            "centre": round_pts(centre[None, :])[0],
        }
        if is_stand:
            entry["source"] = "osm"
            entry["covered"] = t.get("covered") == "yes" or "roof:shape" in t
            entry["front"] = round_pts(simplify(front, 2.0))
            stands.append(entry)
        else:
            entry["levels"] = int(t.get("building:levels", 0) or 0)
            entry["osm_building"] = t.get("building")
            entry["area_m2"] = round(ring_area(p))
            structures.append(entry)
    for spec in MANUAL_STANDS.get(stem, []):
        side = spec["side"]
        if side in ("outside", "inside"):
            side = track.turn_side(spec["from_m"], spec["to_m"], side)
        front = track.edge_run(spec["from_m"], spec["to_m"], side, spec.get("gap_m", 10.0))
        length = float(np.hypot(*np.diff(front, axis=0).T).sum())
        centre = front.mean(0)
        s, _ = track.locate(centre[None, :])
        head = front[-1] - front[0]
        stands.append(
            {
                "name": spec["name"],
                "source": "seating map",
                "station_m": round(float(s[0]), 1),
                "side": side,
                "offset_m": spec.get("gap_m", 10.0),
                "length_m": round(length, 1),
                "depth_m": float(spec.get("depth_m", 16.0)),
                "covered": bool(spec.get("covered", False)),
                "yaw_rad": round(float(math.atan2(head[1], head[0])), 4),
                "centre": round_pts(centre[None, :])[0],
                "front": round_pts(front),
            }
        )
    # A MANUAL_STANDS entry promoting a named OSM building (tagged plain
    # `building=yes`, so extract() filed it as a structure rather than a
    # stand) replaces that structure instead of doubling it up as a
    # building standing behind its own grandstand.
    manual_stand_names = {spec["name"] for spec in MANUAL_STANDS.get(stem, [])}
    structures = [s for s in structures if s.get("name") not in manual_stand_names]
    structures.extend(MANUAL_STRUCTURES.get(stem, []))
    if stem in NO_OSM_STANDS:
        # Every `building=grandstand`/`leisure=grandstand` way within range
        # here belongs to Lakeside Stadium (represented instead as the
        # `MANUAL_STRUCTURES` building above) rather than the Grand Prix --
        # the task's own seating-map research confirms OSM maps none of
        # Albert Park's real (all-temporary) grandstands, so an "osm"
        # source hit for this stem is always someone else's building.
        stands = [s for s in stands if s.get("source") != "osm"]
    stands.sort(key=lambda e: e["station_m"])
    structures.sort(key=lambda e: e["station_m"])

    crossings = []
    for w in osm.ways:
        t = w.get("tags") or {}
        if not t.get("bridge") or t.get("bridge") == "no":
            continue
        p = xy(w)
        if p is None:
            continue
        d, _ = track.grid.query(densify(p, 3.0, closed=False), max_rings=4)
        if d.min() > 6.0:
            continue
        if t.get("highway") == "raceway" or relation_roles.get(w["id"]) is not None:
            continue  # the road's own bridge, not something over it
        hit = densify(p, 3.0, closed=False)[int(d.argmin())][None, :]
        s, _ = track.locate(hit)
        crossings.append(
            {
                "name": osm_name(t),
                "station_m": round(float(s[0]), 1),
                "kind": "footbridge" if t.get("highway") in ("footway", "path", "steps") else "road",
            }
        )
    for spec in MANUAL_CROSSINGS.get(stem, []):
        crossings.append(dict(spec, source="authored"))
    crossings.sort(key=lambda c: c["station_m"])
    dedup = []
    for c in crossings:
        if dedup and abs(c["station_m"] - dedup[-1]["station_m"]) < 25:
            # The same crossing mapped twice: an authored entry is there on
            # purpose (it says what the structure *is*), so it wins.
            if c.get("source") == "authored" and dedup[-1].get("source") != "authored":
                dedup[-1] = c
            continue
        dedup.append(c)
    crossings = dedup

    landmarks = []
    for nid, t in osm.node_tags.items():
        kind = None
        if t.get("attraction") == "big_wheel":
            kind = "big_wheel"
        elif t.get("man_made") in ("communications_tower", "tower") and t.get("tower:type") not in (
            "lighting",
            "communication",
            "radar",
        ):
            # A cell mast or a Rijkswaterstaat radar tower near the venue is
            # not part of the circuit (Zandvoort's dunes carry both); the
            # kit's "tower" is a control/observation tower like Monza's
            # Torre Nord/Sud, which OSM tags with no tower:type at all.
            kind = "tower"
        elif t.get("tower:type") == "lighting" or t.get("highway") == "floodlight":
            kind = "floodlight"
        elif (
            t.get("tourism") == "artwork"
            and t.get("artwork_type") in ("sculpture", "statue", "installation")
            and any(spec["kind"] == "statue" for spec in MANUAL_LANDMARKS.get(stem, []))
        ):
            # A circuit's own monument is mapped as artwork, not as anything
            # the scans above look for: the Red Bull Ring's bull ("Der Bulle
            # vom Spielberg", node 5443064309) is the case this exists for.
            #
            # It only fires where MANUAL_LANDMARKS declares a statue. The kit
            # has one statue mesh, and it is a bull; taking every sculpture
            # near every circuit at face value put eight bulls round Montreal
            # and seven round Interlagos. So the manual entry says *that*
            # the circuit has its monument, the survey says exactly *where*,
            # and the yield rule below lets the survey win.
            kind = "statue"
        if kind == "floodlight" and stem in DAY_RACE_NO_FLOODLIGHTS:
            # `tower:type=lighting` alone does not say *whose* floodlight a
            # pole is; a permanent circuit built for it (Sakhir: 141 of
            # these, all real masts round the lap) is a different case from
            # a public park that also hosts other lit sports grounds.
            # Albert Park's own athletics stadium, cricket/AFL oval, tennis
            # and bowls courts and golf driving range each have ordinary
            # floodlights of their own within the bbox, and the Australian
            # GP runs by day with none of its own -- so every such node here
            # would be a false circuit landmark, not a missing one.
            kind = None
        if not kind:
            continue
        p = to_track(np.array([enu(*osm.nodes[nid], osm.lon0, osm.lat0)]))
        d, _ = track.grid.query(p, max_rings=30)
        if d[0] > 300:
            continue
        s, lat = track.locate(p)
        landmarks.append(
            {
                "kind": kind,
                "name": osm_name(t),
                "station_m": round(float(s[0]), 1),
                "side": "left" if lat[0] > 0 else "right",
                "centre": round_pts(p)[0],
            }
        )
    for w in osm.ways:
        t = w.get("tags") or {}
        # A way can carry both attraction=big_wheel and grandstand=yes (a
        # mapping slip at Suzuka: a stand beside the fairground wheel picked
        # up the wheel's tag too); the stands loop above already claims it.
        if t.get("attraction") != "big_wheel" or t.get("grandstand") == "yes":
            continue
        p = xy(w)
        if p is None:
            continue
        c = p.mean(0)[None, :]
        d, _ = track.grid.query(c, max_rings=30)
        if d[0] > 300:
            continue
        s, lat = track.locate(c)
        landmarks.append(
            {
                "kind": "big_wheel",
                "name": osm_name(t),
                "station_m": round(float(s[0]), 1),
                "side": "left" if lat[0] > 0 else "right",
                "centre": round_pts(c)[0],
            }
        )
    for spec in MANUAL_LANDMARKS.get(stem, []):
        p = track.offset_point(spec["station_m"], spec["side"], spec.get("offset_m", 40.0))
        # A hand-placed landmark is an estimate standing in for a fact.
        # Once the automatic scan finds the same thing -- same kind, within
        # MANUAL_LANDMARK_YIELD_M -- the survey wins and the estimate is
        # dropped, so the entry can stay in the table for circuits and OSM
        # versions that still need it.
        if any(
            l["kind"] == spec["kind"]
            and l.get("source") != "authored"
            and math.hypot(*(np.array(l["centre"]) - p)) < MANUAL_LANDMARK_YIELD_M
            for l in landmarks
        ):
            continue
        entry = {
            "kind": spec["kind"],
            "name": spec.get("name"),
            "source": "authored",
            "station_m": spec["station_m"],
            "side": spec["side"],
            "centre": round_pts(p[None, :])[0],
        }
        if spec.get("brand"):
            entry["brand"] = spec["brand"]
        if "altitude_m" in spec:
            entry["altitude_m"] = spec["altitude_m"]
        if "broadside_to_m" in spec:
            # Long axis across the line of sight from that station.
            eye = track.offset_point(spec["broadside_to_m"], "left", -track.half_width(spec["broadside_to_m"], "left"))
            sight = p - eye
            entry["yaw_rad"] = round(math.atan2(sight[1], sight[0]) + math.pi / 2, 4)
        landmarks.append(entry)
    landmarks.sort(key=lambda e: e["station_m"])

    woods = []
    for w in osm.ways:
        t = w.get("tags") or {}
        # A plantation (`landuse=orchard`) is tree cover a circuit can sit
        # in exactly like a forest -- Sepang's oil-palm estate is mapped
        # this way, never as `natural=wood` -- so it is generically
        # treated as woods too. `leaf_type` is essentially never tagged on
        # an orchard, so default to broadleaved rather than the `mixed`
        # conifer/broadleaf set a real forest falls back to.
        is_orchard = t.get("landuse") == "orchard"
        if t.get("natural") != "wood" and t.get("landuse") not in ("forest",) and not is_orchard:
            continue
        leaf = t.get("leaf_type") or ""
        if leaf in ("broadleaved", "needleleaved", "mixed"):
            pass
        elif is_orchard:
            leaf = "broadleaved"
        else:
            leaf = "mixed"
        p = xy(w)
        if p is None or len(p) < 4:
            continue
        d, _ = track.grid.query(p, max_rings=30)
        if d.min() > WOOD_RANGE_M:
            continue
        ring = simplify(p, 6.0)
        if len(ring) < 4 or ring_area(ring) < 400:
            continue
        woods.append({"leaf": leaf, "ring": round_pts(ring, 1)})

    for spec in MANUAL_WOODS.get(stem, []):
        side = spec["side"]
        if side in ("outside", "inside"):
            side = track.turn_side(spec["from_m"], spec["to_m"], side)
        near = float(spec.get("near_m", 15.0))
        far = near + float(spec.get("depth_m", 95.0))
        near_run = track.edge_run(spec["from_m"], spec["to_m"], side, near)
        far_run = track.edge_run(spec["from_m"], spec["to_m"], side, far)
        ring = np.vstack([near_run, far_run[::-1]])
        woods.append({"leaf": spec.get("leaf", "mixed"), "ring": round_pts(ring, 1)})

    # An authored landmark and an OSM building can be the same real
    # structure (Sakhir's control tower is mapped as building=yes, not as
    # the man_made=tower node the automatic landmark scan looks for): keep
    # the landmark, which says what the structure *is*, and drop the
    # generic building row it would otherwise also become. Mirrors the
    # crossings dedup above; only fires when a manual landmark actually
    # lands within a building's footprint, so it is a no-op for every
    # dossier without one.
    if any(l.get("source") == "authored" for l in landmarks):
        kept = []
        for s in structures:
            near = any(
                l.get("source") == "authored" and math.hypot(*(np.array(s["centre"]) - np.array(l["centre"])))
                < max(s["length_m"], s["depth_m"]) / 2 + 5.0
                for l in landmarks
            )
            if not near:
                kept.append(s)
        structures = kept

    return {
        "format": LAYOUT_FORMAT,
        "version": LAYOUT_VERSION,
        "source_track": f"{stem}.yaml",
        "track_name": track.data.get("name", stem),
        "attribution": ATTRIBUTION,
        "osm_bbox": [list(b) for b in BBOXES[stem]],
        "fit": fit_report,
        "corners": corners,
        "pit_lane": pit,
        "stands": stands,
        "structures": structures,
        "crossings": crossings,
        "landmarks": landmarks,
        "woods": woods,
        **extract_surroundings(stem, track, osm, xy),
    }


# ----------------------------------------------------------- georeferencing


def fit_track(stem: str, offline: bool) -> tuple[Track, Osm, np.ndarray, np.ndarray, dict]:
    """Georeference one circuit onto its own YAML frame.

    Returns `(track, osm, a, t, report)`, where a point in the OSM extract's
    ENU frame lands in the track frame at `p @ a.T + t`.  The whole alignment
    lives here -- the coarse FFT search, the trimmed ICP, and the gate that
    refuses a fit which does not explain the centerline -- because anything
    else that puts real-world data into a track's frame (the DEM, via
    `scripts/dem_fetch.py`) has to use the *same* alignment the dossier was
    built from: two independent fits agreeing to within a metre would still
    leave the hills a metre off the grandstands standing on them.
    """
    paths = fetch(stem, offline)
    track = Track(stem)
    osm = Osm(paths)
    cloud = raceway_cloud(osm)
    t0 = time.time()
    best = None
    for a0, t0_ in coarse_fit(cloud, track.pts):
        a, t, report = icp(cloud, track.pts, a0, t0_)
        if best is None or report["centerline_covered"] > best[2]["centerline_covered"]:
            best = (a, t, report)
        # Good enough to stop searching: either the fit explains the whole
        # centerline, or (Le Mans, where most of the lap is public road in
        # OSM) it explains kilometres of it to within a couple of metres.
        if report["centerline_covered"] > 0.98 or (
            report["centerline_covered_m"] >= 1500 and report["rmse_m"] <= 2.0
        ):
            break
    a, t, report = best
    print(
        f"   fit: rmse {report['rmse_m']} m, centerline covered "
        f"{report['centerline_covered'] * 100:.1f}%, raceway matched "
        f"{report['raceway_matched'] * 100:.1f}% ({time.time() - t0:.0f}s)"
    )
    # Most circuits are raceway end to end, so the fit should explain the
    # whole centerline.  Le Mans is not: two thirds of the Sarthe is public
    # road in OSM (the N138 down to Mulsanne), so there the test is that
    # the permanent section matches to within a couple of metres over
    # kilometres - which no wrong alignment can do.
    ok = report["centerline_covered"] >= 0.9 or (
        report["centerline_covered_m"] >= 1500 and report["rmse_m"] <= 3.0
    )
    if not ok:
        raise SystemExit(
            f"{stem}: fit explains only {report['centerline_covered_m']} m of "
            f"centerline at {report['rmse_m']} m rmse - refusing to write a "
            "dossier from it"
        )
    return track, osm, a, t, report


def build(stem: str, offline: bool) -> dict:
    print(f"== {stem}")
    track, osm, a, t, report = fit_track(stem, offline)
    to_track = lambda p: np.asarray(p, dtype=float).reshape(-1, 2) @ a.T + t
    layout = extract(stem, track, osm, to_track, report)
    print(
        f"   {len(layout['corners'])} named corners, "
        f"{len(layout['stands'])} stands, {len(layout['structures'])} structures, "
        f"{len(layout['crossings'])} crossings, {len(layout['landmarks'])} landmarks, "
        f"{len(layout['woods'])} woods, pit lane "
        + (layout["pit_lane"]["side"] if layout["pit_lane"] else "MISSING")
    )
    print(
        f"   surroundings: {len(layout['barriers'])} barrier run(s), "
        f"{len(layout['roads'])} road(s), {len(layout['waterways'])} waterway(s), "
        f"{len(layout['areas'])} area(s), {len(layout['poi'])} point(s) of interest"
    )
    dropped = layout["unclassified"]
    if dropped:
        top = ", ".join(f"{k} x{v}" for k, v in list(dropped.items())[:6])
        print(f"   unclaimed near the road: {sum(dropped.values())} ({top})")
    return layout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("tracks", nargs="*", help="track stems, e.g. Monza")
    ap.add_argument("--all", action="store_true", help="every circuit with a bbox")
    ap.add_argument("--offline", action="store_true", help="use the cache only")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    args = ap.parse_args()

    stems = sorted(BBOXES) if args.all else args.tracks
    if not stems:
        ap.print_help()
        return 1
    for stem in stems:
        if stem not in BBOXES:
            raise SystemExit(f"no bbox for {stem}; add one to BBOXES")
        layout = build(stem, args.offline)
        if args.dry_run:
            continue
        out = TRACK_DIR / f"{stem}.layout.json"
        out.write_text(json.dumps(layout, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"   wrote {out.relative_to(REPO)} ({out.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
