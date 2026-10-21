#!/usr/bin/env python3
"""Build a circuit's real-world layout dossier from OpenStreetMap.

The centerlines under ``tracks/default`` come from public GPS traces,
so the road itself is where it should be -- but everything *beside* it
(which side the pit lane runs, where the grandstands stand and what they
are called, the pit building, the landmarks, where the trees actually are)
was invented by a procedural enrichment pass.  The scenes therefore look
like a circuit but not like *the* circuit.

This script fetches each circuit's surroundings from OpenStreetMap (public
data, ODbL), georeferences them onto the track's own coordinate frame, and
writes ``tracks/default/<Stem>.layout.json``: a small, readable,
reviewable dossier of real-world facts.  Turning a dossier into props is
``ats-dress``'s job (Rust, deterministic, tested); nothing here writes a
``.ats``.

    python tools/osm_layout.py Zandervoort Spa   # refresh two dossiers
    python tools/osm_layout.py --all           # every circuit with a bbox
    python tools/osm_layout.py Monzarella --offline # reuse the cached extract

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

from track_dirs import track_dir, track_glob  # noqa: F401 (track_glob re-exported)

ROOT = Path(__file__).resolve().parent.parent
CACHE_DIR = ROOT / ".cache" / "osm"
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
    "Zandervoort": [(4.528, 52.383, 4.552, 52.396)],
    "SpaFrankenchamps": [(5.955, 50.428, 5.988, 50.448)],
    "Monzarella": [(9.275, 45.612, 9.300, 45.635)],
    "Shiverstone": [(-1.035, 52.063, -0.995, 52.083)],
    "Oskarsleben": [(11.265, 52.020, 11.295, 52.035)],
    "Armadillos": [(-97.652, 30.122, -97.626, 30.145)],
    "Hockeyheimring": [(8.552, 49.318, 8.582, 49.338)],
    "BrandsScratch": [(0.250, 51.350, 0.275, 51.364)],
    "RedPullRing": [(14.752, 47.212, 14.780, 47.228)],
    "Sudoku": [(136.525, 34.835, 136.555, 34.855)],
    "Nuerburgerring": [(6.930, 50.325, 6.965, 50.345)],
    # The whole Nordschleife and a few hundred metres round it. Not fetched
    # from the OSM map API (its 50k-node ceiling would need a dozen tiles):
    # the extract under .cache/osm/Mordschleife.0.json was cut
    # from the OSM planet file for this box, so run this with --offline.
    "Mordschleife": [(6.895, 50.315, 7.025, 50.400)],
    "Barcelunatic": [(2.246, 41.560, 2.275, 41.580)],
    "Hungoverring": [(19.236, 47.572, 19.262, 47.588)],
    "Bahrainless": [(50.495, 26.020, 50.525, 26.045)],
    "Snorisring": [(11.110, 49.428, 11.132, 49.440)],
    # Street circuit on public roads; the lap is OSM relation 421263 (see
    # STREET_CIRCUIT_RELATIONS). The box stops at 1.283 N, south of it is Gardens
    # by the Bay, which is backdrop only.
    "MandarinaBay": [(103.846, 1.283, 103.872, 1.299)],
    # MoscowMule is deliberately NOT registered here (see below): a bbox
    # whose fit fails would abort every `--all` run at this entry (build()
    # raises SystemExit, uncaught in main()'s loop), breaking `--all` for
    # every stem sorted after it. Once the fit is fixed, add:
    #   "MoscowMule": [(36.243, 55.987, 36.283, 56.005)],
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
    # thin-coverage or public-road case (the full investigation is in git
    # history: docs/ADDITIONAL_TRACKS.md); no dossier was written.
    # HermanosRodriQueso is deliberately NOT registered here either, for the same
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
    #   "HermanosRodriQueso": [(-99.102, 19.396, -99.080, 19.413)],
    "ChillyVilleneuve": [(-73.540, 45.490, -73.510, 45.515)],
    "Lemons": [
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
    "Interlaggos": [
        (-46.712, -23.712, -46.700, -23.703),
        (-46.700, -23.712, -46.688, -23.703),
        (-46.712, -23.703, -46.700, -23.695),
        (-46.700, -23.703, -46.688, -23.695),
    ],
    "Shanghaied": [(121.205, 31.328, 121.235, 31.352)],
    "Shebang": [(101.725, 2.750, 101.752, 2.772)],
    "Gnocchi": [(39.945, 43.398, 39.972, 43.415)],
    "AlbertParkour": [(144.955, -37.860, 144.985, -37.838)],
}

# Grandstands OSM does not have, from each circuit's own published
# seating map.  A run of seats is given as a station span and a side, and
# the script lays its front along the centerline there -- so a stand round
# a bend comes out curved, exactly like one traced from a building
# outline.  `gap_m` is the distance from the road edge to the front row.
MANUAL_STANDS: dict[str, list[dict]] = {
    # Marina Bay has no permanent stands: every one is temporary and none is
    # in OSM. These are the named grandstands of the 2025 Circuit Park Map
    # (the organiser's published seating map, 03-05 October 2025), placed
    # by reading each against the corner runs `drs_zones.py --report`
    # prints. The map is "not drawn to scale", so stations are good to
    # about +-100 m and sides to what the map shows (the 2019 Circuit Park
    # Map, an older 23-turn layout, was used for the Padang, Stamford,
    # Connaught and Empress stands, which that layout shares; its Float
    # stand is gone); check them against
    # `ats-dress --verbose` before relying on one.
    "MandarinaBay": [
        # The pit straight: the stands face the pit building across the road.
        dict(name="Super Pit Grandstand", from_m=4730, to_m=4900, side="right", depth_m=26, gap_m=6, family="street", covered=True),
        dict(name="Pit Grandstand", from_m=20, to_m=200, side="right", depth_m=26, gap_m=6, family="street", covered=True),
        # Turns 1-3.
        dict(name="Turn 1 Grandstand", from_m=405, to_m=470, side="right", depth_m=26, gap_m=6, family="street"),
        dict(name="Turn 2 Grandstand", from_m=480, to_m=545, side="right", depth_m=26, gap_m=6, family="street"),
        # The back of the lap, round the Flyer and along the bottom straight.
        dict(name="Promenade Grandstand", from_m=4470, to_m=4540, side="left", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Bayfront Grandstand", from_m=4545, to_m=4640, side="left", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Skyline Grandstand", from_m=4560, to_m=4680, side="right", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Marina Bay Grandstand", from_m=4685, to_m=4725, side="right", depth_m=14, gap_m=6, family="street_deck"),
        # Raffles Avenue and the Padang.
        dict(name="Raffles Grandstand", from_m=3980, to_m=4100, side="left", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Stamford Grandstand", from_m=1855, to_m=1960, side="right", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Padang A Grandstand", from_m=2340, to_m=2430, side="right", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Padang B Grandstand", from_m=2435, to_m=2520, side="right", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Connaught Grandstand", from_m=3440, to_m=3550, side="right", depth_m=14, gap_m=6, family="street_deck"),
        dict(name="Empress Grandstand", from_m=2705, to_m=2775, side="right", depth_m=14, gap_m=6, family="street_deck"),
        # The 2026 Circuit Park Map (maps/MandarinaBay/sgp-circuit-park-map-2026.pdf
        # p.1) has nothing on the right of the pit straight past the Pit
        # Grandstand but a superscreen and a viewing platform: Republic stands
        # by T5, inside the bend from the T4 run (park map REPUBLIC GRANDSTAND
        # beside the T5 marker; oversteer48-grandstand-map-2025.jpg). Moved off
        # the straight here so it is not lost; its exact span is the Stands
        # column's to confirm.
        dict(name="Republic Grandstand", from_m=850, to_m=960, side="right", depth_m=26, gap_m=6, family="street"),
        # Pit Exit and Chicane @ Turn 2 stand behind the pit-exit lane, not on
        # it (park map: PIT EXIT GRANDSTAND west of the exit line, CHICANE @
        # TURN 2 inside T2-T3 beyond it). The exit lane runs 8-15 m left of the
        # centerline here; an 18 m gap puts each front ~10 m past the lane's
        # centre.
        dict(name="Pit Exit Grandstand", from_m=300, to_m=395, side="left", depth_m=14, gap_m=18, family="street_deck"),
        dict(name="Chicane Turn 2 Grandstand", from_m=470, to_m=540, side="left", depth_m=14, gap_m=18, family="street_deck"),
    ],
    # Only the Hoofdtribune is in OSM.  The rest are from the circuit's
    # own tribune map (https://dutchgp.com/en/tribunes/,
    # https://grandprixguides.com/circuit/netherlands): the Arena and
    # Hairpin stands are on the infield side, Eastside faces Arena-In
    # across the track, Ben Pon looks at the exit of the banked final
    # corner from the start of the straight.  Nothing is laid in Tarzan
    # (turn 1) itself: its run-off and gravel reach 35 m from the road on
    # the outside, and a stand laid at the usual gap sat in the trap; the
    # Tarzan-In stand below ends at the turn-in.
    # Spa's own grandstand map (https://www.spa-francorchamps.be/) puts
    # seats at Les Combes, Bruxelles and Pouhon that OSM has no outline
    # for; each sits on the outside of its bend. The Bus Stop's are the
    # Gold 6 and Gold 11 stands below, on the left after the chicane.
    "SpaFrankenchamps": [
        dict(name="Tribune Les Combes", from_m=2320, to_m=2470, side="outside", depth_m=14),
        dict(name="Tribune Bruxelles", from_m=2990, to_m=3140, side="outside", depth_m=14),
        dict(name="Tribune Pouhon", from_m=3890, to_m=4060, side="outside", depth_m=14),
        # The pit straight's left (outside) stands that OSM has no outline for,
        # from the 2026 Belgian GP wayfinding map (2026-belgian-gp-wayfinding.pdf;
        # north points left on it), scaled from OSM's Tribune F1 (= Gold 1,
        # front st 13-103 L) which the map draws just after the chequered flag.
        # The pit lane and building are on the right; every Gold stand is
        # covered (dossier "Grandstands").
        # Gold 9bis and Gold 9: after Gold 1, toward La Source.
        dict(name="Gold 9bis", from_m=100, to_m=160, side="left", depth_m=14, covered=True),
        dict(name="Gold 9", from_m=165, to_m=235, side="left", depth_m=14, covered=True),
        # Gold 6 and Gold 11: between the chicane exit and the line, PMR
        # Chicane (wheelchair platform) between them; Gold 6 overlooks the
        # chicane and the pit entry (dossier). They replace "Tribune Bus Stop",
        # which the earlier table put on the right, on the pit-entry side.
        dict(name="Gold 6", from_m=6810, to_m=6865, side="left", depth_m=14, covered=True),
        dict(name="Gold 11", from_m=6885, to_m=6945, side="left", depth_m=14, covered=True),
    ],
    # The Automobile Club de l'Ouest numbers its tribunes; those along the
    # pit straight are in OSM, the ones out on the public-road section are
    # not (https://www.lemans.org/ spectator map).
    "Lemons": [
        dict(name="Tribune Tertre Rouge", from_m=1840, to_m=1990, side="outside", depth_m=14),
        dict(name="Tribune Mulsanne", from_m=7500, to_m=7660, side="outside", depth_m=14),
        dict(name="Tribune Indianapolis", from_m=8950, to_m=9090, side="outside", depth_m=14),
        dict(name="Tribune Arnage", from_m=10060, to_m=10200, side="outside", depth_m=14),
        dict(name="Tribune Virage Porsche", from_m=11500, to_m=11700, side="outside", depth_m=14),
    ],
    "Zandervoort": [
        dict(name="Pit Grandstand", from_m=70, to_m=230, side="left", depth_m=18),
        dict(name="Hairpin", from_m=3150, to_m=3330, side="right", depth_m=14),
        dict(name="Eastside", from_m=3330, to_m=3800, side="left", depth_m=16),
        dict(name="Arena", from_m=3300, to_m=3760, side="right", depth_m=16),
        dict(name="Ben Pon", from_m=4030, to_m=4240, side="left", depth_m=20),
        # 26 Tarzan-In Grandstands: left (west) of the straight's north end,
        # right beside the road before the braking zone, not in Tarzan's
        # outside run-off (2026-dutch-gp-wayfinding-positive.pdf; dossier
        # "facing T1 braking zone"). Span scaled on the map from the chequered
        # flag (st 0) to the Tarzan turn-in (~348): it starts just after the
        # Pit Grandstand 27 ends.
        dict(name="Tarzan-In Grandstands", from_m=265, to_m=335, side="left", depth_m=14),
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
    "Armadillos": [
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
    "Hockeyheimring": [
        dict(name="Mercedes-Tribüne", from_m=2000, to_m=2250, side="outside", depth_m=20),
        # Nordtribüne (A-C): wraps the outside of the Nordkurve (T1, 221-403)
        # from the north end of the Haupttribüne to the corner's exit, three
        # tiers deep (hori-streckenplan-2024.pdf "NORDTRIBÜNE";
        # gpd-grandstand-map-2019.jpg NORD A-C continuous with HAUPT E). Set
        # back like the traced LBBW-Haupttribüne (front 37-45 m off the
        # centerline) and starting clear of Hotel Motodrom (63.7 left). OSM
        # maps a 352 m curved unnamed building at 300.3 left, offset 42.5,
        # over it: the stand's roof, so covered.
        dict(name="Nordtribüne", from_m=100, to_m=400, side="outside", depth_m=35, gap_m=31, covered=True),
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
    "Interlaggos": [
        dict(name="D", from_m=290.9, to_m=500.0, side="outside", depth_m=14, covered=True),
        dict(name="R", from_m=592.5, to_m=782.2, side="outside", depth_m=14, covered=True),
        dict(name="G", from_m=1150.0, to_m=1368.1, side="outside", depth_m=10, covered=False),
        dict(name="T4", from_m=1368.1, to_m=1607.1, side="outside", depth_m=13, covered=False),
        # Grandstand A, the circuit's largest stand: one long uncovered bar on the
        # outside from the Cafe exit along Subida dos Boxes and Arquibancadas to
        # the start of the straight, over the pit entry (maps/Interlaggos:
        # gpsp-sector-map-2026.webp "A"; gpgt-grandstand-map-2025.webp, the long
        # A bar on the last curve; dossier "Setor A": uncovered, shaded by
        # trees). OSM traces only its top end (the unnamed stand at 4221-19), so
        # this run stops where that outline starts.
        dict(name="A", from_m=3420, to_m=4215, side="right", depth_m=20, gap_m=10, covered=False),
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
    "Sudoku": [
        dict(name="M", from_m=3600, to_m=3760, side="right", depth_m=16, covered=False),
        # A2: the stand at the end of the main straight into the First Curve,
        # past A1 (OSM, front st 364-485 L), on the left like it
        # (suzuka-official-seat-map.png; dossier "A1/A2: left side, end of the
        # main straight to T1 entry"). OSM has no outline for it. The seat map
        # is "for illustration purposes", so the span is scaled from A1's
        # drawn length (good to ~30 m).
        dict(name="A2", from_m=505, to_m=620, side="left", depth_m=15, covered=False),
    ],
    # Sepang's own OSM building=grandstand way for the Main Grandstand is
    # dropped by front_edge_is_broken() (see its docstring): it traces the
    # whole 1.3 km double-fronted complex as one polygon ("1.3 km long
    # double frontage Main Grandstand... dominating two long straights",
    # https://www.sepangcircuit.com/main-grandstand), between the pit
    # straight and the back straight, which oriented_box() and
    # front_edge() cannot turn into a sane single front. Authored here as
    # its two real faces instead: the stand sits between the two straights
    # (sepangcircuit.com's grandstand map, the dossier's "Start/finish"),
    # so it is on the circuit's left of both, opposite the pits on the pit
    # straight and north of the back straight.
    "Shebang": [
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
        # The Main Grandstand's two faces, spanned from OSM's own outline of it
        # (way 144247993, building=grandstand, dropped by front_edge_is_broken()
        # as one double-fronted polygon), read station by station against this
        # centerline: the pit-straight face runs from the V point at T15
        # (~5285) to ~325, 31-41 m left of the centreline (the palm-frond
        # canopy's sawtooth), with the Mall Area and the Welcome Centre beyond
        # its west end (gpd-grandstand-map-2026.jpg: "MAIN GRANDSTAND" from the
        # Mall Area to the red dot at the V); the back-straight face from ~4460
        # to ~5040, 31-44 m left. Front at 31 m from the centreline = 24 m past
        # the 7.3 m road edge. Covered (dossier "Start/finish").
        dict(name="Main Grandstand", from_m=5285, to_m=325, side="left", depth_m=20, gap_m=24, covered=True),
        dict(name="Main Grandstand", from_m=4460, to_m=5040, side="left", depth_m=20, gap_m=24, covered=True),
        # The Tower (Hibiscus) grandstand: the round stand at the V point,
        # inside the T15 left hairpin (gpd-grandstand-map-2026.jpg red dot;
        # OSM: the same outline's circular end, ~5055-5285, 6-26 m from the
        # centreline, and node 10127497356 "Grandstand Tower" at 5238, 37 m
        # left). Covered (dossier "Buildings": palm-frond canopy over the Main
        # and Tower grandstands).
        dict(name="Tower Grandstand", from_m=5070, to_m=5250, side="inside", depth_m=13, gap_m=6, covered=True),
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
    "Barcelunatic": [
        dict(name="Tribuna F", from_m=719.5, to_m=816.9, side="left", depth_m=17.7, covered=False),
        # Tribuna V, new on the 2026 spectator map (barcelona-2026-spectator-map,
        # maps/Barcelunatic/png/barcelona-2026-spectator-map.p1.png): on the
        # left of the straight between Principal (OSM, ends at 255) and J (OSM,
        # starts at 398). Placed by its share of the map's gap between the two
        # (62-162 of 170 px), at J's and Principal's 18-20 m off the road.
        dict(name="Tribuna V", from_m=305, to_m=390, side="left", depth_m=14, gap_m=20, covered=False),
        # Tribuna I: the outside of the final corner (T14) onto the straight,
        # on the 2026 spectator map just downstream of the Garden Club (OSM
        # "Garden Club", 4300 left); covered (dossier "Buildings").
        dict(name="Tribuna I", from_m=4335, to_m=4420, side="outside", depth_m=14, gap_m=12, covered=True),
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
    "Hungoverring": [
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
        # The pit straight's left side, from the 2026 grandstand maps
        # (maps/Hungoverring: hungaroring-gpticketshop-map-2027.jpg and
        # hungaroring-spectator-map-2026.jpg; their legends use the post-2025
        # names). Spans are read against OSM's "Hungaroring" stand (4350-245,
        # the dark Hungaroring + Platinum block on the ticket map, ~1.25 m per
        # pixel) and agree between the two maps to about 20 m. None carries the
        # ticket map's ROOF mark, so all are open.
        # Front row before the line, from T14's exit: Grand Prix 1, Grid 2,
        # Grid 3 (the ticket map's unlabelled pale block) and Grid 1, the last
        # against the Hungaroring stand.
        dict(name="Grand Prix 1", from_m=4090, to_m=4148, side="left", depth_m=14, gap_m=10, covered=False),
        dict(name="Grid 2", from_m=4155, to_m=4210, side="left", depth_m=14, gap_m=10, covered=False),
        dict(name="Grid 3", from_m=4215, to_m=4270, side="left", depth_m=14, gap_m=10, covered=False),
        dict(name="Grid 1", from_m=4276, to_m=4335, side="left", depth_m=14, gap_m=10, covered=False),
        # Second row behind them (spectator map "P", "C", "GP2" in the row
        # behind GR1-GP1; the ticket map's pale blocks and GRAND PRIX 2 one
        # column back): 6 m behind the front row's back.
        dict(name="Grand Prix 2", from_m=4088, to_m=4146, side="left", depth_m=16, gap_m=30, covered=False),
        dict(name="Club", from_m=4152, to_m=4210, side="left", depth_m=16, gap_m=30, covered=False),
        dict(name="Podium", from_m=4216, to_m=4300, side="left", depth_m=16, gap_m=30, covered=False),
        # After the line, between the Hungaroring stand and T1: Pit Exit 2,
        # Pit Exit 1 and T1, on the braking zone, not past the apex (T1 at 638).
        dict(name="Pit Exit 2", from_m=310, to_m=385, side="left", depth_m=14, gap_m=10, covered=False),
        dict(name="Pit Exit 1", from_m=405, to_m=490, side="left", depth_m=14, gap_m=10, covered=False),
        dict(name="T1", from_m=505, to_m=590, side="left", depth_m=14, gap_m=10, covered=False),
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
    "Gnocchi": [
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
        # T1 grandstand ("Sergey Sirotkin"): temporary, no roof, on the outside
        # of the flat-out T1 right kink (~420) just past the north-east end of
        # the Main Grandstand (dossier "Buildings and structures";
        # racefans-sochi-autodrom.jpg for T1, its label on the south-east, the
        # left, side). No map draws its extent; the span is centred on the kink.
        dict(name="T1 Grandstand", from_m=345, to_m=465, side="outside", depth_m=12),
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
    "AlbertParkour": [
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
        # The pit straight, from the 2026 Fangio grandstand map (oversteer48,
        # maps/AlbertParkour/oversteer48-fangio-grandstand-map-2026.jpg) read
        # against the entrance gates map (oversteer48-gates-entrances-map.jpg):
        # every stand is on the outside (driver's left), opposite the pits on
        # the lake side. Scale 2.1-2.16 m/px, from the maps' T14 and T1 nodes
        # (stations about 4815 and 360 here). Senna sits at the exit of T14 and
        # faces the pit entry and the support garages; covered (premium; the
        # dossier's S/F and buildings sections).
        dict(name="Senna", from_m=4880, to_m=4990, side="left", depth_m=18, covered=True),
        # Fangio: opposite the F1 garages, from mid-straight up to about 80 m
        # short of the line (the 2026 map, where Piastri took its T1 end).
        dict(name="Fangio", from_m=5020, to_m=5205, side="left", depth_m=18, covered=False),
        # Piastri (new for 2026): straddles the start/finish line, opposite the
        # podium (dossier S/F: "opposite Fangio sections B/C and Piastri
        # sections C-E"), between Fangio and Moss.
        dict(name="Piastri", from_m=5225, to_m=85, side="left", depth_m=18, covered=False),
        # Moss: the outside of the run into Turn 1, between Piastri and Jones
        # (both maps put it on the left, not the pit side).
        dict(name="Moss", from_m=235, to_m=330, side="left", depth_m=14, covered=False),
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
    "ChillyVilleneuve": [
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
        # Grandstand 1: on the infield (right) side of the main straight
        # opposite the pits, from the line back toward T14, stepped with its
        # tall end at the line (oversteer48-grandstand-map-2023.jpg and
        # oversteer48-ga-viewpoints-2026.jpg, ~1.75 m/px: ~120 px, about 200 m).
        # The start line and podium are in front of its right-hand end (dossier
        # "Start/finish"). Two runs under the one name: the tall block from the
        # line back ~95 m, then the lower steps.
        dict(name="Grandstand 1", from_m=4250, to_m=4343, side="right", depth_m=16, gap_m=10),
        dict(name="Grandstand 1", from_m=4140, to_m=4250, side="right", depth_m=10, gap_m=10),
        # Platine: the one covered stand, between the grid and T1 on the
        # infield side, set back from the straight (both oversteer48 maps:
        # ~40 px long, ~75 px past the line; dossier "the Platine covered
        # grandstand is between the grid and T1"). Replaces the unsourced
        # "Grandstand 2" (30-170), which no map shows.
        dict(name="Platine", from_m=95, to_m=165, side="right", depth_m=14, gap_m=25, covered=True),
    ],
    "Monzarella": [
        # Stand 25 on the pit straight, in the row of stands facing the pits
        # between 24 (Vedano, OSM 5565-5711 L) and 26A-C (Laterale Destra, OSM
        # 16-236 L): 2024-monza-seating-map.jpg and 2026-italian-gp-wayfinding.pdf
        # p.1 both put it there; OSM has no outline. Its station span is the gap
        # between the two OSM outlines, short of Torre Sud (5777 L). Roof not
        # stated on either map.
        dict(name="Tribuna 25", from_m=5715, to_m=5765, side="left", depth_m=10, gap_m=14, covered=False),
    ],
    "Nuerburgerring": [
        # The main grandstand T3 (rebuilt 2009, 5,000 seats, covered), opposite
        # the pit building on the outside of the straight and entered from the
        # ring boulevard behind it (dossier "Buildings and structures", from
        # de.wikipedia and nuerburgring.de; OSM has no outline). No fetched map
        # places it: the FIA 2020 map (p.2) draws no structures. The span is the
        # pit building's (OSM garage 4975-137 R), from just after the
        # BMW-Bruecke (4958) toward the Lindner hotel's end of the straight;
        # depth and gap keep its back row clear of the Ring Werk and Ring
        # Boulevard outlines (their road faces about 34 m from the centerline).
        dict(name="T3", from_m=5000, to_m=120, side="left", depth_m=18, gap_m=8, covered=True),
    ],
    "Shiverstone": [
        # Club Corner and Club A: OSM has both only as plain buildings named
        # "Club B" (way 227342448, 5641-5766, 42-73 m left) and "Club A" (way
        # 227342445, 5607-5631, 42-58 m left), so they were laid as
        # media_centre blocks. The 2026 spectator map
        # (2026-british-gp-spectator-map.pdf p1) draws Club A and Club Corner
        # there, on the outside of Club before the Hamilton Straight. Promoted
        # under their OSM names, spans and fronts from the footprints (front
        # 42 m from the centreline = 35 m past the road edge). Club A starts at
        # 5614, clear of the unnamed OSM building at 5599 beside it. Roof not
        # marked on the map.
        dict(name="Club A", from_m=5614, to_m=5634, side="left", depth_m=13, gap_m=35, covered=False),
        dict(name="Club B", from_m=5643, to_m=5766, side="left", depth_m=22, gap_m=35, covered=False),
        # Hamilton Straight B and Abbey A: on the 2026 spectator map between
        # Hamilton Straight A and Abbey B (OSM "Abbey", 278-402), on the
        # outside of the straight opposite the Wing. Spans scaled on the map
        # from the Wing's ends (OSM -56 / 319) and OSM Abbey's end (402).
        # Hamilton B stands in front of the Hilton Garden Inn (OSM 64-186,
        # 36-59 m left; the map's marker 2 is behind it), so it is a shallow
        # stand close to the fence; Abbey A sits at Abbey's own front line.
        dict(name="Hamilton Straight B", from_m=100, to_m=190, side="left", depth_m=12, gap_m=14, covered=False),
        dict(name="Abbey A", from_m=200, to_m=272, side="left", depth_m=14, gap_m=24, covered=False),
    ],
    "Snorisring": [
        # The Norisring's grandstands besides the Steintribüne are temporary
        # scaffold stands put up for the race weekend; none is in OSM. Placed
        # from the official 2026 Lageplan (norisring-lageplan-2026.pdf, V2-ORGA-
        # 260519), scaled along the straight from the Steintribüne's OSM front
        # (st 1957 at its west end, 44 at its east end; ~0.66 m per map px at
        # 2000 px width), which also puts Pre-start 1 at the pit entry (1929)
        # and Pre-start 2 at the pit exit (70). All three are right of the
        # straight (south, the Zeppelinfeld side), opposite the Steintribüne.
        # SPECK-Pumpen-Tribüne: the small stand just after the T8 kink, before
        # Pre-start 1 and the pit entry.
        dict(name="SPECK-Pumpen-Tribüne", from_m=1850, to_m=1910, side="right", depth_m=10, gap_m=6, covered=False),
        # Boxengassen-Tribüne: four open scaffold blocks behind the wall over
        # the first grid boxes (photo boxengassentribuene-draufsicht-2022.jpg,
        # ~18 rows, grass bank of the Zeppelinfeld's west wall beside it), east
        # of that bank (OSM's west-wall outline ends at st ~1986). gap_m 14
        # puts its front 22 m right, behind the pit lane (nodes at -14 to -15 m).
        dict(name="Boxengassen-Tribüne", from_m=1990, to_m=2055, side="right", depth_m=18, gap_m=14, covered=False),
        # Milwaukee-Tribüne: after START, between the Parc Ferme lane (pit exit)
        # and Hans-Kalb-Straße.
        dict(name="Milwaukee-Tribüne", from_m=95, to_m=185, side="right", depth_m=12, gap_m=6, covered=False),
    ],
}

# A stand's roof by name, from the circuit's own seating map, for the OSM
# stands (whose `covered` comes from their tags alone) and the
# `MANUAL_STANDS` ones alike, written as the stand's `roof`: "covered"
# roofs it, "open" keeps the dress pass from roofing it for its depth
# (25 m or more is otherwise taken to be roofed). Every stand of that name
# on the circuit takes it; an unnamed OSM stand is named by its station
# instead, "@<station_m>" as the dossier has it (OSM's outline does not
# move, so neither does its station).
STAND_COVER: dict[str, dict[str, str]] = {
    # The Main Grandstand opposite the pits is OSM's unnamed outline at
    # 102.8 m right (0-310): a three-level stand under a fabric canopy
    # (dossier "Main Grandstand").
    "Armadillos": {
        "@102.8": "covered",
    },
    "BrandsScratch": {
        # The aerial (oversteer48-grandstands.jpg) shows the Pit Straight
        # Grandstand under a red roof, the Paddock Hill Grandstand under a grey
        # one and the Hailwoods Hill stand as a small roofed block; OSM tags
        # none of them covered.
        "Pit Straight Grandstand": "covered",
        "Paddock Hill Grandstand": "covered",
        "Hailwoods Hill Grandstand": "covered",
        # Desiré Wilson is an open stand on the same aerial (the overhang over
        # its two east blocks is the Brabham-Stewart building behind it, laid
        # as its own structure); its 10.3 m depth keeps it open already, so the
        # entry only records the fact.
        "Desiré Wilson Grandstand": "open",
    },
    "Gnocchi": {
        # The Main Grandstand is one six-storey building under a single
        # cantilevered roof (photo tilke-sochi-09.jpg; dossier "permanent
        # six-storey, 400 m building with a roof over the seats"). Its 39 m
        # depth roofs it already; the entry records it from the source.
        "Главная трибуна": "covered",
    },
    "Bahrainless": {
        # The Main Grandstand's white tent-like PTFE roof (dossier S/F "Pit
        # building", "Modelling pitfalls"; roofed over its whole length on the
        # oversteer48 seating chart): written as a fact rather than left to the
        # 25 m-deep rule, which roofs it today only for its 29.4 m depth.
        "Main Grandstand": "covered",
    },
    "Barcelunatic": {
        # The Main Grandstand is permanent and covered, two tiers with the Sky
        # View lounge on top (dossier "Buildings"; the 2026 map's Sky View
        # Racing Lounge, item 4, on it). OSM's outline has no roof tag.
        "Graderia principal": "covered",
    },
    "Hungoverring": {
        # The ticket map marks Hungaroring and Hungaroring Platinum ROOF; OSM has
        # no roof tag on the outline (laid roofed today only by the depth rule).
        "Hungaroring": "covered",
    },
    "Interlaggos": {
        # Dossier "Setor M": covered, numbered seats, facing the garages; OSM's
        # "M" outline has no roof tag and is 21.4 m deep, under the depth rule.
        "M": "covered",
    },
    "Lemons": {
        # Dossier "Ford chicane grandstand": outside of the Ford chicane, "large,
        # covered" -- OSM's Tribune 23 at 13353, the stand on the approach to the
        # line, laid open from OSM tags (14.8 m deep).
        "Tribune 23 - Motul Raccordement": "covered",
    },
    "Monzarella": {
        # The pit-straight stands the dossier's "Buildings and structures"
        # table lists as covered: 1 Centrale, 4 Laterale Sinistra, 26A-C
        # Laterale Destra and 24 Vedano. OSM tags none of them, and all four
        # are under the 25 m depth that is otherwise taken to be roofed.
        "Tribuna Centrale": "covered",
        "Tribuna Laterale Sinistra": "covered",
        "Tribuna Laterale Destra": "covered",
        "Tribuna Vedano": "covered",
    },
    "RedPullRing": {
        # The 2026 spectator map (redbullring-2026-spectator-map.png) marks only
        # Grandstand Start Ziel and Grandstand North "(COVERED)". Start Ziel is
        # roofed today by the 25 m depth rule (36.7 m deep in OSM, the bank
        # behind included); said explicitly here. Steiermark, beside it at the
        # T1 end of the straight, is not marked covered and is 26.5 m deep in
        # OSM, which the depth rule would roof.
        "Start/Ziel Tribüne": "covered",
        "Steiermark Tribüne": "open",
    },
    "Shanghaied": {
        # Grandstand A's upper tiers are under the lotus-leaf roofs (dossier
        # "Buildings": "some roofs modelled on a lotus leaf"); roofed today
        # only because OSM's box is 140 m deep (the whole complex behind it).
        "A看台": "covered",
    },
    "Snorisring": {
        # The Steintribüne is the 1930s open stone terrace of the Zeppelinfeld
        # (dossier "Buildings": no roof, colonnades gone since 1967); its 35.7 m
        # OSM depth would otherwise roof it.
        "Steintribüne": "open",
        # Open scaffold bleachers (boxengassentribuene-draufsicht-2022.jpg).
        "Boxengassen-Tribüne": "open",
    },
    "SpaFrankenchamps": {
        # Gold 1, OSM's "Tribune F1": covered (dossier "Grandstands": Gold 1
        # covered; OSM carries no roof tag).
        "Tribune F1": "covered",
    },
    "Zandervoort": {
        # 27 Pit Grandstand is the circuit's only covered stand (dossier
        # "Buildings"); the Main Grandstand 28 (OSM Hoofdtribune, 28.4 m deep)
        # is therefore open, which the 25 m depth rule would otherwise roof.
        "Pit Grandstand": "covered",
        "Hoofdtribune": "open",
    },
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
    "Lemons": [dict(name="Tyre bridge", station_m=1043.0, kind="arch", brand="piretti")],
    # The spectator footbridge on the climb out of Niki Lauda Kurve (T1),
    # carrying fans from the paddock/pit side over to the hillside Red Bull
    # Tribune. Not in OSM (nothing bridge-tagged near the track in this
    # bbox is within 350 m of the centerline -- checked directly against the
    # cached extract), and no aerial source gives its exact position, so the
    # station is placed by judgement partway up the climb, just before the
    # Red Bull Tribune's OSM-traced footprint begins (station_m 646.7):
    # an approximation rather than a sourced fact.
    "RedPullRing": [dict(name="T1 climb footbridge", station_m=600.0, kind="footbridge", brand="kronos")],
    # The pit building's two cantilevered "wing" roofs (the Press Centre and
    # the Sky Restaurant, ~38 m up) reach out over the front straight from
    # the tower at the west and east ends of the paddock/grandstand-A
    # complex OSM traces at stations 8-338 -- confirmed by web search
    # (en.wikipedia.org: "wing-like viewing platforms crossing the circuit
    # at either end"); the 2026 gov map (shanghai-gov-2026-grandstand-
    # locations.png) draws the T1-end one as "SKY BRIDGE".
    "Shanghaied": [
        dict(name="Press Centre Wing", station_m=15.0, kind="sky_platform"),
        dict(name="Sky Restaurant Wing", station_m=335.0, kind="sky_platform"),
    ],
    # Monza's old banked oval (the Sopraelevata), its north banking crossing
    # over the GP road on the Serraglio-Ascari approach: the only road-over-
    # road crossing on the 2024 circuit map and the 2026 wayfinding map. At
    # the point where OSM's oval way ("anello alta velocità", a raceway
    # bridge the crossing scan skips) meets the lap, which the dossier also
    # carries as a corner (3593.4-3593.8).
    "Monzarella": [
        dict(name="Sopraelevata north banking", station_m=3593.6, kind="banked_oval"),
        # The footbridge just after the banking by stand 12 (2024 circuit
        # map, about 185 m past it, scaled on Serraglio 3299 and Ascari
        # 4087): an estimate. The banking takes the scanned footbridge at
        # 3586.3 (one crossing within 25 m), which is the oval's own path.
        dict(name="Ascari approach footbridge", station_m=3780.0, kind="footbridge"),
    ],
    # The temporary footbridge beside Fangio carrying the crowd over the
    # straight to the Main Stage / Fan Zone in the infield (dossier
    # "Grandstands": "a pedestrian bridge over the track next to it leads to
    # the Main Stage/Fan Zone"). No map draws it; set in the gap between
    # Senna (4880-4990) and Fangio (5020-5205), not across the stand's view.
    "AlbertParkour": [dict(name="Fangio footbridge", station_m=5005.0, kind="footbridge")],
    # The Circuit Rooftop, the building spanning the run into La Caixa (2026
    # spectator map item 7, drawn across the road; dossier T10 row). OSM's
    # "VIP Rooftop" outline projects to 3361.2 and the building pass skips it
    # as reaching across the road (R5): the kit's span building stands in.
    "Barcelunatic": [dict(name="Circuit Rooftop", station_m=3361.2, kind="building")],
    "Nuerburgerring": [
        # The BMW M Bridge over the start/finish straight (dossier "BMW M
        # Bridge"): OSM way 34444288, man_made=bridge, bridge=covered,
        # "BMW-Brücke", its centroid. A covered spectator bridge, so `link`;
        # it replaces the scan's `road` at 4957.7.
        dict(name="BMW-Brücke", kind="link", at=(6.9471772, 50.3352124), brand="northwind"),
        # The lap's own bridge between the Sprintstrecke junction and the
        # Müllenbach link: OSM way 1443047846 (highway=raceway, bridge=yes),
        # over a primary and an unclassified road, with footway and service
        # bridges alongside. Nothing crosses over the lap: the scan's `road`
        # at 1236.5 is those side bridges, which this replaces. Anchored on
        # the way's end nodes, as the Nordschleife's bridges are.
        dict(name="Muellenbach link bridge", kind="deck_track",
             from_at=(6.9395699, 50.3305247), to_at=(6.9394003, 50.330159)),
    ],
    # The Nordschleife's own bridges over the public roads it crosses: OSM
    # maps each as a `highway=raceway` way with `bridge=yes` (which the scan
    # above skips as "the road's own bridge"), the road under it crossing
    # it. Anchored on the way's two end nodes (lon, lat), so the span stays
    # where it is when the centerline moves; `deck_track` tiles parapets
    # over it. The overhead advertising bridge on the Doettinger Hoehe is the
    # "gantry" a Touristenfahrten lap is timed to; OSM has it as way
    # 1391805026 (`advertising=billboard`, layer 1), its real sponsor's
    # lettering replaced by a kit brand.
    "Mordschleife": [
        # way 41395673, over the K 73 just past the line.
        dict(name="K 73 bridge", kind="deck_track",
             from_at=(6.9491256, 50.3387732), to_at=(6.9488824, 50.3389832)),
        # way 41395681, over the B 257 at Quiddelbach.
        dict(name="Quiddelbach bridge", kind="deck_track",
             from_at=(6.9302948, 50.3425517), to_at=(6.9295538, 50.3428825)),
        # way 41395647, over the B 257 (Trierer Strasse) at Breidscheid.
        dict(name="Breidscheid bridge", kind="deck_track",
             from_at=(6.9499778, 50.3768618), to_at=(6.9501051, 50.3769996)),
        # way 41395652, over the L 92 at the foot of the Doettinger Hoehe.
        dict(name="L 92 bridge", kind="deck_track",
             from_at=(6.9831675, 50.3526741), to_at=(6.9828358, 50.352546)),
        # way 41395668, over the L 93 (Hauptstrasse) before T13.
        dict(name="L 93 bridge", kind="deck_track",
             from_at=(6.9515089, 50.3378554), to_at=(6.951275, 50.337751)),
        # way 1391805026's midpoint.
        dict(name="Gantry", kind="timing", at=(6.98077, 50.35175), brand="northwind"),
    ],
}

# What the dossier's OSM-derived crossings are called for a circuit whose
# bridges are not the kit's default: over a city street a "footbridge" is a
# covered link, and a "road" is an expressway viaduct.
CROSSING_KINDS: dict[str, dict[str, str]] = {
    "MandarinaBay": {"footbridge": "link", "road": "viaduct"},
    # The Nordschleife's two overhead crossings (stations 3965 and 19702:
    # OSM ways 41795617 and 29898554) are forest tracks (`highway=track`,
    # 3 m) on concrete bridges, not a sponsor's tyre arch.
    "Mordschleife": {"road": "overpass"},
}

# A street circuit's bridges that carry the lap itself (`bridge=yes` ways of
# the lap's route relation) become `deck_*` crossings spanning the way's
# stations: a bridge of at least this length is the wide deck, a shorter
# one the steel arch (Anderson Bridge, 70 m; Esplanade Bridge, 290 m).
DECK_WIDE_MIN_M = 120.0

# OSM building name (substring) -> the kit building that stands for it.
# "-" leaves a footprint out because another prop covers it. Names are the
# dossier's internal ones; what the player sees never shows a real name.
MANUAL_STRUCTURE_ASSETS: dict[str, dict[str, str]] = {
    "MandarinaBay": {
        # authored: the three footprints of the performing-arts centre are
        # one prop, `landmark_twin_domes`, laid on the wrapper's outline.
        "Esplanade Theatres on the Bay": "landmark_twin_domes",
        "Esplanade Concert Hall": "-",
        "Esplanade Theatre": "-",
        "The Fullerton Hotel": "building_colonnade_hotel",
        "Old Supreme Court": "building_domed_court",
        "Old City Hall": "building_colonnade_civic",
        "Singapore Cricket Club": "building_club_pavilion",
        "Singapore Recreation Club": "building_club_pavilion",
        "Saint Andrew's Cathedral": "building_gothic_church",
        "Suntec Singapore Convention": "building_five_towers",
        "Singapore Flyer Car Park": "-",
    },
}

# OSM-scanned landmarks that are not what the scan takes them for, by kind
# and station (within DROP_SCANNED_WITHIN_M): the scan keeps them through a
# `--manual-only` re-apply, so this is the only way to leave one out.
DROP_SCANNED_LANDMARKS: dict[str, list[tuple[str, float]]] = {
    # node 1304656417, a `man_made=tower` tagged "cellphone tower", 253 m
    # off Hawthorn: a phone mast, not race control (no kit piece for one).
    "BrandsScratch": [("tower", 3653.1)],
}
DROP_SCANNED_WITHIN_M = 25.0


def drop_scanned_landmarks(stem: str, landmarks: list[dict]) -> list[dict]:
    drops = DROP_SCANNED_LANDMARKS.get(stem, [])
    return [
        l for l in landmarks
        if l.get("source") == "authored"
        or not any(l["kind"] == k and abs(l["station_m"] - st) < DROP_SCANNED_WITHIN_M for k, st in drops)
    ]


# Landmark kinds OSM finds that a circuit draws with its own prop.
LANDMARK_KINDS: dict[str, dict[str, str]] = {
    "MandarinaBay": {"big_wheel": "big_wheel_xl"},
    # The COTA Observation Tower (dossier "Landmarks": 77 m, platform at 70
    # m, inside the T16-T18 carousel), OSM's `man_made=tower` at 4654.7
    # right: a viewing tower, not race control (R11).
    "Armadillos": {"tower": "observation_tower"},
    # OSM node 3705411753 at 1457.3 right, 53 m past the edge, tagged only
    # man_made=tower (no type, name or height). Race control is the TÜV
    # tower in the pit complex; a scaffold camera tower is the least wrong
    # stand-in for an untyped trackside tower.
    "Nuerburgerring": {"tower": "camera_tower"},
}

# Point features OSM does not carry, but that are part of what the place
# looks like on a race weekend.
# Centroid of OSM's `historic=castle` outline of Burg Nürburg, way
# 31010481 "Nürburg (Ruine)" (lon, lat).
NUERBURG_CASTLE = (6.95352, 50.34674)

# The meadow on the slope beside the Doettinger Hoehe the hillside letters
# stand on (see MANUAL_LANDMARKS): the mean of OSM way 496321264's
# (`landuse=meadow`) outline.
HILLSIDE_MEADOW_AT = (6.979309, 50.353606)

MANUAL_LANDMARKS: dict[str, list[dict]] = {
    # The skyline across the water that is on every picture of the circuit,
    # at the positions OSM has them (lon, lat). All of them stand 300-500 m
    # from the road, so they are backdrop, not neighbours.
    "MandarinaBay": [
        dict(kind="skypark", name="Three-tower hotel", at=(103.86085, 1.28390),
             yaw_toward=(103.86102, 1.28459)),
        dict(kind="lotus_museum", name="Lotus museum", at=(103.85923, 1.28611)),
        dict(kind="double_helix", name="Double helix bridge", at=(103.86049, 1.28746),
             yaw_toward=(103.86045, 1.28871)),
        # Superscreens on the right after the Pit Grandstand (park map
        # "superscreen" key): one past the stand's T1 end by the viewing
        # platform, one on the T1 approach by the Turn 1 Platform.
        dict(kind="screen", name="Pit straight superscreen", station_m=240.0, side="right", offset_m=15.0),
        dict(kind="screen", name="Turn 1 superscreen", station_m=320.0, side="right", offset_m=25.0),
    ],
    # Burg Nürburg, the castle ruin on its basalt cone in Nürburg, inside
    # the loop between the GP circuit and the Hatzenbach: the landmark every
    # view across the Nordschleife has on its skyline. Anchored on OSM's
    # `historic=castle` way (its centroid), not on a station, because it
    # stands half a kilometre from the road. `yaw_rad` turns the gate
    # toward the village.
    "Mordschleife": [
        dict(kind="castle", name="Burg Nürburg", at=NUERBURG_CASTLE, yaw_rad=0.6),
        # The Kaiser-Wilhelm-Turm, the 16.3 m stone lookout tower (1909) on
        # the Hohe Acht summit, 1.4 km off the Hohe Acht section: OSM way
        # 699590411 (`man_made=tower`, `tower:type=observation`), its
        # outline's centroid.
        dict(kind="lookout_tower", name="Kaiser-Wilhelm-Turm", at=(7.01126, 50.38608)),
        # The Touristenfahrten toll lanes at the entrance off the
        # Doettinger Hoehe: OSM's three `barrier=lift_gate` nodes 370475173,
        # 7179480525 and 7179480528 on the `fee=yes` service lanes (ways
        # 42746353, 769107219, 769107220); `yaw_across` is each lane's next
        # node, the arm lying across the lane.
        dict(kind="toll_gate", name="Toll lane 1", at=(6.9657432, 50.3459721),
             yaw_across=(6.9662091, 50.3461626)),
        dict(kind="toll_gate", name="Toll lane 2", at=(6.9657153, 50.3459997),
             yaw_across=(6.9661841, 50.346192)),
        dict(kind="toll_gate", name="Toll lane 3", at=(6.9656896, 50.346033),
             yaw_across=(6.9660839, 50.3461904)),
        # The hillside letters (`sign/hillside_letters`, the circuit's
        # display name; invented, the real circuit has none) on the open
        # slope right of the Doettinger Hoehe, in view down the whole
        # straight: the meadow HILLSIDE_MEADOW_AT names, which rises 13 m
        # over the road 230 m off it (the DEM over open ground). Turned
        # broadside to the line of sight from the straight before it.
        dict(kind="hillside_letters", name="Hillside letters", at=HILLSIDE_MEADOW_AT,
             broadside_to_m=17600.0),
        # The filling stations in sight of the lap, as roadside kit fuel
        # stations (fascia brands from the kit). The canopy reaches 31 m back
        # from its road-facing pivot, so each pivot is moved toward the road
        # from the mapped point.
        # ED "Döttinger Höhe" (OSM node 1711333738, amenity=fuel), beside the
        # straight: the node moved to 20 m off the edge, the shop behind.
        dict(kind="fuel_station", name="Döttinger Höhe fuel station",
             at=(6.9810968, 50.3516195), brand="hexon"),
        # Aral at Breidscheid (OSM way 1023567856, amenity=fuel), 73 m left of
        # the lap by the B 257: its canopy roof's centroid (an unnamed
        # building=roof, 21 x 13.8 m) moved 12 m toward the road, so the kit
        # canopy stands for that roof.
        dict(kind="fuel_station", name="Breidscheid fuel station",
             at=(6.9493698, 50.3780578), brand="velocet"),
        # TotalEnergies in Nürburg (OSM way 114676264, amenity=fuel), 187 m
        # left of the lap just past T13, in view across the GP side.
        dict(kind="fuel_station", name="Nürburg fuel station",
             at=(6.9479767, 50.3364997), brand="velocet"),
    ],
    # The airship over the pits is as much a part of the 24 Hours as the
    # fairground at the Esses (which OSM does have, as a big wheel).
    "Lemons": [
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
    "Shebang": [
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
    # requires, and it carries no storey count. Authored here instead, on
    # the OSM outline, so the dedup above drops the OSM building row. It is
    # the circuit's round glass tower (dossier: "circular glass-clad tower,
    # 30 m diameter, eight floors"), so the kit's glass tower, on the
    # outline's centre: the outline's offset_m is its nearest corner's
    # distance from the centerline, so the centre is 73.9 + 18.9 (half the
    # 37.7 m square) - 7.0 (the half width there) = 85.8 m beyond the edge.
    "Bahrainless": [
        dict(kind="glass_tower", name="Sakhir Tower", station_m=582.3, side="right", offset_m=85.8),
        # The three TV screens on the pit building facing the Main Grandstand:
        # oversteer48-main-grandstand-seating.jpg puts them at 13 %, 51 % and
        # 85 % of the stand, gpgt-circuit-map-bahrain.webp's three S/F giant
        # screens (beside the F1 Paddock Club, either side of the line) at
        # 27 %, 51 % and 73 %; taken as 20 %, 51 % and 80 % of OSM's stand
        # front (26-375 left). On the pit side, 42 m off the edge, behind the
        # garage row (lane centre 9 m off the edge).
        dict(kind="screen", name="Pit building screen", station_m=95.0, side="right", offset_m=42.0),
        dict(kind="screen", name="Pit building screen", station_m=205.0, side="right", offset_m=42.0),
        dict(kind="screen", name="Pit building screen", station_m=305.0, side="right", offset_m=42.0),
    ],
    # The 2014 Winter Olympics cauldron still stands in the Medals Plaza,
    # inside the loop of T3; OSM has it only as a tourism=attraction node
    # ("Олимпийский огонь" / "Olympic flame"), which the automatic
    # landmark scan does not look at (it only picks up man_made=tower /
    # tower:type=lighting / attraction=big_wheel). Anchored on that node,
    # 2660040429 (lands at 1999.7 left, 139 m beyond the edge).
    "Gnocchi": [
        dict(kind="cauldron", name="Olympic Cauldron", at=(39.9547853, 43.4051613)),
        # The Olympic ice venues (dossier "Buildings and structures"), each on
        # the centre of its own OSM outline, turned to the outline's yaw, so it
        # stands in place of the row of venue blocks the outline was laid as
        # (R19 drops that row). Not MANUAL_STRUCTURE_ASSETS: that applies only
        # on a full extract, and no extract of this circuit is cached.
        # Bolshoy Ice Dome, encircled by T4-T6 (OSM "Ледовый дворец «Большой»",
        # 201 x 151 m, 3088 right).
        dict(kind="arena", name="Bolshoy Ice Dome", at=(39.9499219, 43.4046023), yaw_rad=1.7731),
        # Shayba Arena (OSM 'Ледовая арена "Шайба"', 122 x 122 m, 2256 left).
        dict(kind="arena", name="Shayba Arena", at=(39.9519037, 43.4023960), yaw_rad=0.1549),
        # Iceberg Skating Palace (OSM "Дворец зимнего спорта «Айсберг»",
        # 174 x 147 m, 1262 right). The Adler Arena ("Сириус-Арена") is a
        # 273 x 162 m box the dome does not fit, and keeps its block row.
        dict(kind="arena", name="Iceberg Skating Palace", at=(39.9583236, 43.4074024), yaw_rad=2.2106),
    ],
    # The Casino de Montreal (the Expo 67 France pavilion, the Quebec
    # pavilion beside it), the backdrop of the Casino Straight: OSM's unnamed
    # 164 x 126 m outline at 3718.1 right, with Avenue du Casino beside it
    # and the "Pont du Pavillon de la France" footbridge on its west side;
    # Wikipedia's coordinate (45.5053 N, 73.5261 W) lands 30 m from its
    # centre. Placed 8 m off the outline's centre, away from the 113 x 49 m
    # annex at 3661 right, so it stands for the casino's own row only.
    "ChillyVilleneuve": [
        dict(kind="expo_pavilion", name="Casino de Montreal", at=(-73.5256713, 45.5052310), yaw_rad=0.8783),
        # The Biosphere (the Expo 67 US pavilion's dome) on Ile Sainte-Helene,
        # across the channel from the hairpin (dossier "Buildings"): Wikipedia
        # 45 30'51" N 73 31'53" W, 281 m beyond the edge at 2696 left.
        dict(kind="geodesic_dome", name="Biosphere", at=(-73.53139, 45.51417)),
        # The three TV screens on the roof of the 2019 paddock building, facing
        # Grandstand 1 (dossier "Paddock building (2019)"). The building is
        # skipped as pit complex and the bake lays single-storey garages, so
        # the screens stand behind the garage zone (pit lane at 11 m left,
        # garages to ~33 m; at 50 m the screen's footprint clears it), where
        # the 11 m screen shows over the garage roofs. They take the
        # dossier's "Paddocks" outline with them, which the bake's pit
        # complex replaces anyway.
        dict(kind="screen", name="Paddock building screen 1", station_m=4220.0, side="left", offset_m=50.0),
        dict(kind="screen", name="Paddock building screen 2", station_m=4270.0, side="left", offset_m=50.0),
        dict(kind="screen", name="Paddock building screen 3", station_m=4320.0, side="left", offset_m=50.0),
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
    "RedPullRing": [
        dict(kind="statue", name="Red Bull Statue", station_m=3149.7, side="left", offset_m=90.0),
        # The live screens of the pit straight on the 2026 wayfinding map
        # (redbullring-2026-wayfinding.pdf p1, "Live Screen" icons), stations
        # scaled from the chequered flag (st 0) and the Start/Finish bar's ends
        # (OSM 4188 / 298): one outside the T10 exit beyond the T10 end of
        # Start Ziel, by Steiermark Village; one inside, between the pit entry
        # road and the Champions Club, before the garage row begins; one inside
        # at the T1 end, past the last garage by the pit exit. The spectator
        # map's two TV icons on the straight (~st 4200 and ~100) agree with the
        # inside pair within about 100 m.
        dict(kind="screen", name="Start/Ziel T10 end screen", station_m=4140, side="left", offset_m=20),
        dict(kind="screen", name="Pit entry screen", station_m=4200, side="right", offset_m=25),
        dict(kind="screen", name="Pit exit screen", station_m=320, side="right", offset_m=25),
        # The voestalpine Wing, the media centre: a 92 x 20 m steel structure
        # shaped like a rear wing, above and behind the Start-Ziel grandstand
        # on the left (dossier "Start/finish"; redbullring-2026-wayfinding.pdf
        # p1, "Media Center" behind Start/Finish, about 135-150 scaled on the
        # stand's ends). Two 40 m modules on the stand's back (front 18.3 m
        # off, 36.7 m deep), showing over its roof.
        dict(kind="media_wing", name="voestalpine Wing 1", station_m=125.0, side="left", offset_m=57.0),
        dict(kind="media_wing", name="voestalpine Wing 2", station_m=165.0, side="left", offset_m=57.0),
    ],
    "Hockeyheimring": [
        # The giant screen inside the Nordkurve (gpd-grandstand-map-2019.jpg,
        # red stroke inside T1 at its entry), set past the pit exit road that
        # runs inside T1 (lane at ~-24 m) and beyond the end of the garage row.
        dict(kind="screen", name="Nordkurve screen", station_m=250.0, side="right", offset_m=40.0),
    ],
    "Armadillos": [
        # The four giant TVs along the pit building, facing the Main Grandstand
        # across the straight (dossier S/F "Pit building" and the structures
        # table; not drawn on the oversteer48 or F1 programme maps). Spread
        # evenly over OSM's "Pit Garages" building (114.7 m left, 363 m long,
        # so about -65..295). On the ground behind the garage row: 40 m off the
        # edge clears the pit zone (lane centre about 6 m off the edge).
        dict(kind="screen", name="Pit building screen", station_m=20.0, side="left", offset_m=40.0),
        dict(kind="screen", name="Pit building screen", station_m=105.0, side="left", offset_m=40.0),
        dict(kind="screen", name="Pit building screen", station_m=190.0, side="left", offset_m=40.0),
        dict(kind="screen", name="Pit building screen", station_m=275.0, side="left", offset_m=40.0),
    ],
    "Barcelunatic": [
        # The 2026 spectator map's "Live Screens" along the straight, all drawn
        # on the inside (right): one before the line at the pit-entry end, then
        # opposite the gap between Principal and V, opposite J, K and E. Read
        # against the OSM stands' own stations piecewise (the map is not to
        # scale: about 1.1-1.2 m/px). Within the pit lane's length they stand
        # behind the garage row (42 m off the edge); past the lane's end (615)
        # close to the road.
        dict(kind="screen", name="Live screen", station_m=4517.0, side="right", offset_m=42.0),
        dict(kind="screen", name="Live screen", station_m=287.0, side="right", offset_m=42.0),
        dict(kind="screen", name="Live screen", station_m=475.0, side="right", offset_m=42.0),
        dict(kind="screen", name="Live screen", station_m=628.0, side="right", offset_m=20.0),
        dict(kind="screen", name="Live screen", station_m=705.0, side="right", offset_m=20.0),
        # The control / scoreboard tower on the pit side near the pit exit,
        # opposite stand J (dossier S/F, from Grand Prix Grand Tours; not drawn
        # on the 2026 map). Behind OSM's Panorama Village (458-514, 27-47 m
        # off), which the pit complex replaces. The control_tower stands in.
        dict(kind="tower", name="Control tower", station_m=465.0, side="right", offset_m=55.0),
        # The F1 Fanzone stage outside T9 behind Tribuna N (2026 spectator
        # map p1, "F1 Fanzone Stage" icon; affine fit on 7 stand centres,
        # residuals 9-58 m): 2937 left about 170 m off, clear of N.
        dict(kind="stage", name="Fanzone stage", station_m=2940.0, side="left", offset_m=150.0),
    ],
    "Hungoverring": [
        # Giant screens on the pit/infield side of the straight, from the 2026
        # wayfinding map (maps/Hungoverring/hungary-2026-wayfinding.pdf p.1,
        # "Giant Screen" icons): three past the pit building's T14 end, facing
        # the Grid stands, and three past its T1 end, facing Pit Exit 2, Pit
        # Exit 1 and T1. Offset keeps them 20 m or more clear of the pit lane.
        dict(kind="screen", name="Grid screen 2", station_m=4180.0, side="right", offset_m=40.0),
        dict(kind="screen", name="Grid screen 3", station_m=4240.0, side="right", offset_m=40.0),
        dict(kind="screen", name="Grid screen 1", station_m=4300.0, side="right", offset_m=40.0),
        dict(kind="screen", name="Pit Exit 2 screen", station_m=345.0, side="right", offset_m=40.0),
        dict(kind="screen", name="Pit Exit 1 screen", station_m=450.0, side="right", offset_m=40.0),
        dict(kind="screen", name="T1 screen", station_m=550.0, side="right", offset_m=40.0),
        # The fan zone outside T12 (2026 wayfinding map p1, Blue Zone: "Giant
        # Wheel" and "Stage" icons past Fan 1-2; affine fit on 5 stands and
        # the T13/T14 apexes, residuals 6-43 m). The map's icons hug the road:
        # set back where the zone is.
        dict(kind="big_wheel", name="Giant Wheel", station_m=3495.0, side="left", offset_m=70.0),
        dict(kind="stage", name="Fanzone stage", station_m=3530.0, side="left", offset_m=90.0),
    ],
    "Oskarsleben": [
        # The Race-Tower / Presse Zentrum (mao-uebersichtskarte-2021.jpg item
        # 1), at the east end of the pit building, east of the Verwaltung (2)
        # and before the Start/Ziel line: race control, commentary boxes and a
        # press centre for about 100 media (dossier). OSM has no outline.
        # Station from the map scaled on the OSM Boxen and Verwaltung outlines
        # (the map runs about 35 m east of OSM). Set back behind the generated
        # garage row (boxes 3591-38 L), whose depth reaches about 36 m from the
        # centerline here.
        dict(kind="tower", name="Race-Tower", station_m=3625.0, side="left", offset_m=38.0),
    ],
    "Snorisring": [
        # Giant screens: the Lageplan draws a screen board at each end of the
        # Steintribüne, at the road's north edge, facing the straight.
        dict(kind="screen", name="Steintribüne west screen", station_m=1924, side="left", offset_m=10),
        dict(kind="screen", name="Steintribüne east screen", station_m=64, side="left", offset_m=10),
        # The DEKRA fan tower (Lageplan 2026 item 10: inside the
        # Dutzendteich-Kehre, left of the back leg, about 1520-1580, 30 m
        # out). The hairpin's inside is narrow there, so at 1500, 26 m out,
        # mid-way between the legs; the scaffold camera tower stands in for
        # the temporary tower.
        dict(kind="camera_tower", name="DEKRA fan tower", station_m=1500.0, side="left", offset_m=26.0),
    ],
    "SpaFrankenchamps": [
        # The wayfinding map's giant screen at the chicane exit, outside (left),
        # past Gold 6.
        dict(kind="screen", name="Chicane exit screen", station_m=6790, side="left", offset_m=10),
        # The Ferris wheel of the Combes fan zone (2026-belgian-gp-wayfinding
        # .pdf p1, the Ferris Wheel icon by the hatched F1 Fanzone at "Green
        # Area"): outside Les Combes past Silver 7 (OSM "Tribune Les Combes",
        # 2396 L), the icon about 115 px off the road at about 1.3 m/px.
        dict(kind="big_wheel", name="Combes fan zone wheel", station_m=2470.0, side="left", offset_m=150.0),
        # The F1 Fanzone stage (same map, the Stage icon at the Fanzone's
        # Raidillon end): outside the foot of Eau Rouge, behind the Fanzone
        # grandstand (OSM "Tribune RX", 786-998 L, about 220 m deep).
        dict(kind="stage", name="Fanzone stage", station_m=1080.0, side="left", offset_m=110.0),
    ],
    "Sudoku": [
        # Circuit-vision screen on the inside of the main straight opposite
        # A1/A2, beyond the pit lane (seat map, yellow bar).
        dict(kind="screen", name="Main straight screen", station_m=450, side="right", offset_m=30),
    ],
    "Zandervoort": [
        # Live screens inside the pit straight (wayfinding map): one at the
        # north end of the paddock-club row opposite Tarzan-In 26, past the
        # end of the garages, and one at the south end by the media centre.
        dict(kind="screen", name="Pit straight north screen", station_m=320, side="right", offset_m=30),
        dict(kind="screen", name="Pit straight south screen", station_m=4260, side="right", offset_m=30),
        # The Ferris wheel (2026-dutch-gp-wayfinding-positive.pdf p1, the
        # Ferris Wheel icon by the small Fanzone square): behind the middle of
        # Ben Pon 29 (4030-4240 L, 10 m off, 20 m deep). South of about 4200,
        # or it would stand in for the OSM lounge building at 4233 L.
        dict(kind="big_wheel", name="Fanzone wheel", station_m=4135.0, side="left", offset_m=70.0),
        # The F1 Fanzone stage (same map, in the hatched Fanzone's west part):
        # in the dunes outside Arie Luyendijk (3722-3990), about 1.5 m/px.
        dict(kind="stage", name="Fanzone stage", station_m=3850.0, side="left", offset_m=220.0),
    ],
    "Shiverstone": [
        # The Wing (dossier "Start/finish, pit lane, paddock": 390 m, three
        # storeys, media centre and race control under the wing roof; OSM way
        # 227332429, about -57..319 right). The real one stands over the
        # garages; the bake lays its own garage row on the pit lane, so the
        # modules stand behind it, fronts 48 m past the edge (3.4 m clear of
        # the pit zone at the Club end, the worst). The Club end (-57..0) is
        # left out: the pit entry lane cuts across it.
        dict(kind="media_wing", name="The Wing 1", station_m=20.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 2", station_m=60.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 3", station_m=100.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 4", station_m=140.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 5", station_m=180.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 6", station_m=220.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 7", station_m=260.0, side="right", offset_m=48.0),
        dict(kind="media_wing", name="The Wing 8", station_m=300.0, side="right", offset_m=48.0),
        # The giant wheel (2026-british-gp-spectator-map.pdf, "Giant Wheel"
        # east of the Luffield loop; 7-corner affine fit, 20-50 m): 2165,
        # 107 m off, nudged clear of the lighting masts and the Museum.
        dict(kind="big_wheel", name="Giant wheel", station_m=2163.0, side="left", offset_m=91.0),
        # The Main Stage (same map, beside Wellington Straight on the F1
        # Fanzone side): 1687 by the fit, set 30 m off, out of the run-off.
        dict(kind="stage", name="Main stage", station_m=1682.0, side="left", offset_m=30.0),
    ],
    "Nuerburgerring": [
        # Burg Nürburg on the skyline north of the GP circuit (dossier
        # "Landmarks"), the Nordschleife entry's anchor (its outline's
        # centroid) and its yaw carried into this dossier's frame.
        dict(kind="castle", name="Burg Nürburg", at=NUERBURG_CASTLE, yaw_rad=2.95),
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
    "AlbertParkour": dict(from_m=0.0, to_m=380.0, side="right", gap_m=8.0),
}

# How far from the track a mapped pit lane may run and still be the pit
# lane, m (`pit_lane_from` clips each chain to its longest stretch within
# this; 70 m by default, which cuts away a road that merely touches a lane).
# Silverstone's lane (OSM way chain from Vale to Abbey, 1129 m) leaves the
# track at the exit of Vale and cuts across the inside of Club up to 102 m
# from it before running behind the Wing; at 70 m the entry was clipped off
# and the lane was blended onto the track beside the grid instead.
PIT_LANE_REACH_M: dict[str, float] = {
    "Shiverstone": 120.0,
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
    "AlbertParkour": [
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
    "Lemons": [
        dict(from_m=4700, to_m=8950, side="left", leaf="needleleaved",
             near_m=15, depth_m=95),
        dict(from_m=4700, to_m=8950, side="right", leaf="needleleaved",
             near_m=15, depth_m=95),
    ],
}

# Circuits sharing their bbox with other lit sports venues, where an OSM
# `tower:type=lighting` node is real but is not the circuit's own. See the
# comment where this is used, in the landmark extraction loop.
# What each corner's trackside board says in the game (`display_name` in
# the dossier's `corners`; `ats-dress` lays a `corner_sign` from it and
# never from `name`). The real names stay in `name`, which the tools look
# corners up by, but many are trademarks: sponsors (Würth Kurve, Mobil 1
# Kurve, NGK-Schikane, the CM.com bocht, Hitachi's chicane at Suzuka) and
# famous people whose names are registered marks (S do Senna,
# Michael-Schumacher-S, Niki Lauda Kurve), so the game shows a sound-alike
# instead. `None` means no board: a way OSM names after the circuit itself
# (`Hungaroring`, `CM.com Circuit Zandvoort`), a connector or a rallycross
# loop is not a corner. A name not listed here is a place or a description
# (Eau Rouge, Paddock Hill, Karussell) and is shown as it is, so a refetch
# that turns up a new corner needs a look at this table. Suzuka's are in
# Japanese in OSM and the kit's boards only draw Latin text.
CORNER_DISPLAY: dict[str, dict[str, str | None]] = {
    "Armadillos": {
        "Short Course 2": None,
        "COTA Short Track": None,
    },
    "BrandsScratch": {
        "Brabham Straight": "Bramble Straight",
        "Hailwoods Hill": "Hailstones Hill",
        "Graham Hill Bend": "Grandma Hill Bend",
        "Cooper Straight": "Scooper Straight",
        "Surtees": "Sure-Tees",
        "Derek Minter Straight": "Dead Winter Straight",
        "Sheene Curve": "Sheen Curve",
        "Stirlings Bend": "Sterling Bend",
        "Clark Curve": "Clerk Curve",
    },
    "Hungoverring": {
        "Hungaroring": None,
    },
    "Barcelunatic": {
        "Circuit de Barcelona-Catalunya": None,
        "Rallycross": None,
        "AZ": None,
    },
    "Hockeyheimring": {
        "Ravenol-Kurve": "Ravioli-Kurve",
        "Bernie-Ecclestone-Kurve": "Birnie-Eggstone-Kurve",
        "Hochgeschwindigkeits-(Parabolika-)Kurve": "Parabolika",
        "Rallycross": None,
        "Mobil 1 Kurve": "Möbel-1-Kurve",
        "Sachs-Kurve": "Socken-Kurve",
    },
    "Lemons": {
        "Courbe Dunlop": "Courbe Dunflop",
        "Chicane Dunlop": "Chicane Dunflop",
        "Chicane Daytona": "Chicane Daytonic",
        "Virage Porsche": "Virage Porridge",
        "Virage Corvette": "Virage Serviette",
        "Chicane Ford": "Chicane Fjord",
    },
    "MandarinaBay": {
        # OSM's lap relation is one "corner" named for the circuit itself.
        "Marina Bay Street Circuit": None,
    },
    "ChillyVilleneuve": {
        "Circuit Gilles-Villeneuve": None,
    },
    "Monzarella": {
        "Autodromo nazionale di Monza - anello alta velocità": None,
        "Variante Ascari": "Variante Calamari",
        "Curva Alboreto": "Curva Alberello",
    },
    "Mordschleife": {
        "Sabine-Schmitz-Kurve": "Schnitzel-Kurve",
        "Lauda-Links": "Lauter-Links",
        "Stefan-Bellof-S": "Bello-S",
        "Nürburgring Nordschleife": None,
    },
    "Nuerburgerring": {
        "Nürburgring Sprintstrecke": None,
        "Anbindung zur Müllenbachschleife": None,
        "Ford-Kurve": "Furt-Kurve",
        "Michael-Schumacher-S": "Schuster-S",
        "Anbindung zur Sprintstrecke": None,
        "NGK-Schikane": "Zündkerzen-Schikane",
    },
    "Oskarsleben": {
        "Zeppelin-Kurve": "Zappelin-Kurve",
    },
    "Bahrainless": {
        "Grand Prix Circuit": None,
    },
    "Interlaggos": {
        "S do Senna": "S do Sonho",
    },
    "Shiverstone": {
        "Hamilton Straight": "Hamlet Straight",
    },
    "Gnocchi": {
        "Сириус Автодром": None,
    },
    "SpaFrankenchamps": {
        "Courbe Paul Frère": "Courbe des Frites",
    },
    "RedPullRing": {
        "Niki Lauda Kurve": "Knicki-Kurve",
        "Remus": "Romulus",
        "Schlossgold": "Schlosssilber",
        "Rauch": "Qualm",
        "Würth Kurve": "Wurst-Kurve",
        "Rindt": "Rind",
        "Red Bull Mobile": "Red Pull Mobile",
    },
    "Sudoku": {
        "1コーナー": "Turn 1",
        "2コーナー": "Turn 2",
        "S字": "Esses",
        "逆バンク": "Reverse Bank",
        "NIPPO コーナー": "Hippo Corner",
        "デグナー1": "Dagger 1",
        "デグナー2": "Dagger 2",
        "NISSINブレーキヘアピン": "Noodle Hairpin",
        "西ストレート": "Back Straight",
        "日立オートモティブシステムズシケイン": "Hit-and-Miss Chicane",
        # the same chicane's second half, under the sponsor's newer name
        "日立Astemoシケイン": None,
        "メインストレート": "Main Straight",
    },
    "Zandervoort": {
        "Tarzanbocht": "Tartanbocht",
        "CM.com Circuit Zandvoort": None,
        "Gerlachbocht": "Gelachbocht",
        "Hugenholtzbocht": "Hugenhoutbocht",
        "Rob Slotemakerbocht": "Slootmakerbocht",
        "CM.com bocht": "Sms-bocht",
        "Hans Ernst chicane": "Hansworst-chicane",
        "Arie Luyendykbocht": "Luierdijkbocht",
    },
}


def with_display_names(stem: str, corners: list[dict]) -> list[dict]:
    """Each corner with its `display_name` (see `CORNER_DISPLAY`), placed
    right after `name` so the dossier reads real name, then shown name."""
    table = CORNER_DISPLAY.get(stem, {})
    out = []
    for c in corners:
        shown = table.get(c["name"], c["name"])
        rest = {k: v for k, v in c.items() if k not in ("name", "display_name")}
        out.append({"name": c["name"], "display_name": shown, **rest})
    return out


# Road graffiti (the Nordschleife's): runs of pictures painted across the
# tarmac, expanded by `manual_graffiti` into the dossier's `graffiti` list
# and laid by `ats-dress` as `.ats` decals. Images are
# `props/decal/graffiti/<name>.png` (tools/generators/props/gen_graffiti.py).
# Stations are metres along the YAML centerline in race direction.
# Props placed by station and offset: `kind`, `asset`, `from_m` [`to_m`
# `every_m`], `side` (left | right | centre), `offset_m` from the road edge
# (from the centerline on `centre`), optional `yaw_deg` and `text`. The
# dossier carries them verbatim; `ats-dress` lays them. Marina Bay's start
# straight, from the 2025 night aerial and the circuit park map: the pit
# building and its lane on the left, the one long stand on the right.
# Offsets: the stands stand 6 m off the road and reach 26 m (so 32 m), the
# garages' road-facing edge is 15.6 m off the edge and the building reaches
# 34 m behind it.
MANUAL_FURNITURE: dict[str, list[dict]] = {
    "MandarinaBay": [
        # Overhead, beside the start lights.
        dict(kind="bridge", asset="banner_gantry", from_m=45, side="centre", offset_m=0),
        dict(kind="misc", asset="finish_tower_scaffold", from_m=8, side="right", offset_m=8),
        dict(kind="sign", asset="pit_entry_board", from_m=4570, side="left", offset_m=7),
        # LED advertising ribbon on top of the Tecpro in front of the stands.
        dict(kind="board", asset="led_ribbon_3m", from_m=4735, to_m=4900, every_m=3, side="right", offset_m=3.0, text="apexsim"),
        dict(kind="board", asset="led_ribbon_3m", from_m=20, to_m=395, every_m=3, side="right", offset_m=3.0, text="kronos"),
        # Globe lamps behind the stand and behind the pit building.
        dict(kind="light", asset="lamp_globe_pole", from_m=4735, to_m=330, every_m=30, side="right", offset_m=36),
        dict(kind="light", asset="lamp_globe_pole", from_m=40, to_m=240, every_m=24, side="left", offset_m=54),
        # Light balloons along the pit roof's edge, white and orange.
        dict(kind="light", asset="lamp_balloon_tether", from_m=55, to_m=235, every_m=40, side="left", offset_m=18.5),
        dict(kind="light", asset="lamp_balloon_tether_orange", from_m=75, to_m=235, every_m=40, side="left", offset_m=18.5),
        # The wordmark on the roof, the stairs at both ends, planters on the walkway.
        dict(kind="board", asset="roof_wordmark_block", from_m=75, to_m=195, every_m=60, side="left", offset_m=30, text="apexsim"),
        dict(kind="misc", asset="stair_zigzag_scaffold", from_m=28, side="left", offset_m=22),
        dict(kind="misc", asset="stair_zigzag_scaffold", from_m=242, side="left", offset_m=22),
        dict(kind="misc", asset="walkway_planter_4m", from_m=40, to_m=240, every_m=16, side="left", offset_m=60),
        # The fan zone behind the stand.
        dict(kind="attraction", asset="marquee_peak_12m", from_m=4750, to_m=330, every_m=28, side="right", offset_m=52),
        dict(kind="attraction", asset="led_wall_stage_16m", from_m=290, side="left", offset_m=40),
        # The Padang, the open green inside St Andrew's Road: a village of
        # marquees round a stage, planters and globe lamps (the organiser's
        # map puts the F1 village here).
        dict(kind="attraction", asset="marquee_peak_12m", from_m=2330, to_m=2590, every_m=34, side="left", offset_m=46),
        dict(kind="attraction", asset="marquee_peak_12m", from_m=2347, to_m=2570, every_m=34, side="left", offset_m=74),
        dict(kind="attraction", asset="led_wall_stage_16m", from_m=2460, side="left", offset_m=105),
        dict(kind="misc", asset="walkway_planter_4m", from_m=2330, to_m=2600, every_m=14, side="left", offset_m=30),
        dict(kind="light", asset="lamp_globe_pole", from_m=2330, to_m=2600, every_m=20, side="left", offset_m=60),
    ],
}

def generated_furniture(stem: str) -> list[dict]:
    """The lap's furniture that follows from its geometry: a banner gantry on
    the middle of every long straight, an LED ribbon on the outside barrier
    of each braking zone, and rows of globe lamps along both verges. Added
    to the hand-authored `MANUAL_FURNITURE` entries (the start straight, the
    Padang), which it leaves alone."""
    if stem != "MandarinaBay":
        return []
    import drs_zones as dz

    corners = dz.Corners(dz.load(stem))
    total, runs = corners.total, corners.runs
    brands = ["kronos", "hexon", "rolux", "piretti", "velocet", "northwind", "brix", "apexsim"]
    pit_straight = (4735.0, 400.0)  # hand-authored: the start straight

    def on_pit_straight(s: float) -> bool:
        return (s - pit_straight[0]) % total <= (pit_straight[1] - pit_straight[0]) % total

    bridges = [(2912.0, 2983.0), (3067.0, 3359.0)]  # Anderson, Esplanade
    out: list[dict] = []
    n = len(runs)
    for i, (a, b, sign) in enumerate(runs):
        nxt = runs[(i + 1) % n][0]
        gap = (nxt - b) % total
        mid = (b + gap / 2) % total
        if gap >= 190 and not on_pit_straight(mid) and not any(lo - 20 <= mid <= hi + 20 for lo, hi in bridges):
            out.append(dict(kind="bridge", asset="banner_gantry", from_m=round(mid), side="centre",
                            offset_m=0, text=brands[len(out) % len(brands)]))
    for i, (a, b, sign) in enumerate(runs):
        start = (a - 105) % total
        if on_pit_straight(start) or on_pit_straight((a - 20) % total):
            continue
        # The outside of a left-hander is its right-hand barrier.
        out.append(dict(kind="board", asset="led_ribbon_3m", from_m=round(start), to_m=round((a - 20) % total),
                        every_m=3, side="right" if sign > 0 else "left", offset_m=3.0,
                        text=brands[(i * 3) % len(brands)]))
    # Globe lamps along both verges, staggered, round the lap but for the start straight.
    for side, first in (("left", 430.0), ("right", 454.0)):
        out.append(dict(kind="light", asset="lamp_globe_pole", from_m=first, to_m=4720.0, every_m=48,
                        side=side, offset_m=9.0))
    return out


# Decal runs painted on the road beside the start straight: the yellow and
# blue band at each edge, tiled (`image` in `props/decal/marina/`).
MANUAL_DECAL_RUNS: dict[str, list[dict]] = {
    "MandarinaBay": [
        dict(name="Pit straight edge band", image="marina/edge_yellow_blue", from_m=4735, to_m=395,
             every_m=6, length_m=6, width_m=3.0, side="right", beyond_edge_m=-1.5),
        dict(name="Pit straight edge band", image="marina/edge_yellow_blue_r", from_m=4735, to_m=395,
             every_m=6, length_m=6, width_m=3.0, side="left", beyond_edge_m=-1.5),
    ],
}

MANUAL_GRAFFITI: dict[str, list[dict]] = {
    # Where the Nordschleife's crowds stand and paint: the spectator
    # places along the lap, each anchored on OSM's own locality node for
    # the section. The heaviest is the Döttinger Höhe (the long straight
    # the camps line during the 24 hours), Brünnchen and Pflanzgarten; the
    # rest carry a handful each. Authored from the circuit's spectator
    # guides; the pictures themselves are invented (gen_graffiti.py).
    "Mordschleife": [
        dict(name="Hatzenbach", at=(6.94162, 50.33820), before_m=150, after_m=150, every_m=55,
             images=["vollgas", "ring_frei", "arrows_right"]),
        dict(name="Flugplatz", at=(6.92595, 50.34659), before_m=250, after_m=80, every_m=45,
             images=["flat_out", "lift_nein", "go_go_go", "heart"]),
        dict(name="Schwedenkreuz", at=(6.92414, 50.35605), before_m=200, after_m=60, every_m=60,
             images=["nix_bremsen", "kalle", "eifel"]),
        dict(name="Adenauer Forst", at=(6.93087, 50.36629), before_m=120, after_m=120, every_m=50,
             images=["team_wurst", "jens_mia", "bambini"]),
        dict(name="Wehrseifen", at=(6.94347, 50.37622), before_m=150, after_m=50, every_m=50,
             images=["moppel", "hallo_mama", "smiley"]),
        dict(name="Breidscheid", at=(6.95041, 50.37638), before_m=120, after_m=120, every_m=60,
             images=["danke_ring", "hup_holland"]),
        dict(name="Bergwerk", at=(6.96053, 50.38072), before_m=150, after_m=150, every_m=50,
             images=["gruene_hoelle", "schneller", "opa_heinz"]),
        dict(name="Kesselchen", at=(6.97073, 50.37421), before_m=400, after_m=400, every_m=70,
             images=["kein_limit", "vollgas", "tom_lea", "benni_79", "arrows_right", "ich_war_hier"]),
        dict(name="Karussell", at=(6.98599, 50.37192), before_m=220, after_m=40, every_m=45,
             images=["allez_allez", "late_apex", "heart", "chequer"]),
        dict(name="Hohe Acht", at=(6.99525, 50.37667), before_m=100, after_m=150, every_m=50,
             images=["eifel", "go_go_go", "kalle"]),
        dict(name="Brünnchen", at=(7.00541, 50.36966), before_m=250, after_m=250, every_m=32,
             images=["gruene_hoelle", "jens_mia", "hup_holland", "vierundzwanzig", "moppel",
                     "smiley", "ring_frei", "danke_ring", "tom_lea", "flat_out"]),
        dict(name="Pflanzgarten", at=(6.99911, 50.36532), before_m=250, after_m=250, every_m=36,
             images=["lift_nein", "vollgas", "allez_allez", "team_wurst", "heart", "opa_heinz",
                     "arrows_right", "bambini"]),
        dict(name="Schwalbenschwanz", at=(6.98417, 50.35871), before_m=150, after_m=80, every_m=50,
             images=["kein_limit", "hallo_mama", "chequer"]),
        dict(name="Galgenkopf", at=(6.98589, 50.35540), before_m=80, after_m=150, every_m=55,
             images=["late_apex", "go_go_go", "arrows_left"]),
        dict(name="Döttinger Höhe", at=(6.98243, 50.35005), before_m=700, after_m=1300, every_m=45,
             images=["vierundzwanzig", "gruene_hoelle", "vollgas", "hup_holland", "ich_war_hier",
                     "benni_79", "kein_limit", "jens_mia", "schneller", "allez_allez", "chequer",
                     "danke_ring", "ring_frei", "smiley", "flat_out", "eifel"]),
    ],
}

DAY_RACE_NO_FLOODLIGHTS = frozenset({"AlbertParkour"})

# Circuits where every OSM `building=grandstand`/`leisure=grandstand` hit in
# range is confirmed (by hand, against the circuit's own seating map) to
# belong to a different, co-located venue rather than the circuit itself.
NO_OSM_STANDS = frozenset({"AlbertParkour"})

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
# A city's skyline is more than its near neighbours: for the stems in
# `WATER_STEMS` buildings count out to this range, from this size.
CITY_STRUCTURE_RANGE_M = 260.0
CITY_STRUCTURE_MIN_M2 = 600.0
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
            (track_dir(stem) / f"{stem}.yaml").read_text(encoding="utf-8")
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


# Street-circuit lap relations that do not carry highway=raceway themselves
# (type=circuit, members tagged as the ordinary streets they are). Listed by
# id so no other circuit's dossier changes: Monza has such a relation too.
STREET_CIRCUIT_RELATIONS = frozenset({421263})  # Marina Bay Street Circuit


def relation_raceway_roles(osm: Osm) -> dict[int, str]:
    """Way id -> its role in an OSM route relation tagged highway=raceway.

    Every circuit so far tags each stretch of tarmac highway=raceway
    directly. A street circuit run on public roads can instead model the
    lap as a single `type=circuit` relation carrying the raceway tag, with
    every member way tagged only as the ordinary street it is (Norisring:
    Ben-Gurion-Ring and the Beuthener/Zeppelin/Karl-Steigelmann streets are
    highway=secondary/unclassified/service, and it is relation 1889538,
    "Snorisring", that carries highway=raceway). Folding those members in is
    what lets the fit and the pit-lane search see the lap at all; a
    directly-tagged circuit is unaffected, since none of the others has
    such a relation.
    """
    roles: dict[int, str] = {}
    for r in osm.relations:
        if (r.get("tags") or {}).get("highway") != "raceway" and r.get("id") not in STREET_CIRCUIT_RELATIONS:
            continue
        for m in r.get("members", []):
            if m.get("type") == "way":
                role = m.get("role") or ""
                roles.setdefault(m["ref"], "pit_lane" if role == "pitlane" else role)
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

# Street furniture of a city circuit, mapped as nodes: only for the stems
# in `WATER_STEMS` (a city), so no other dossier gains a layer.
CITY_POI_KINDS = {
    ("natural", "tree"): "tree",
    ("railway", "subway_entrance"): "station",
    ("highway", "traffic_signals"): "signal",
    ("shelter_type", "public_transport"): "bus_shelter",
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


# Circuits whose dossier carries a `water` layer: the bay, the river, the
# basins, assembled from natural=water ways and multipolygon relations
# (outer rings, with the inner ones as holes). Others keep the few small
# water `areas` they always had, so their dossiers do not move.
WATER_STEMS = frozenset({"MandarinaBay"})
WATER_RANGE_M = 700.0


def _join_rings(segments: list[list[int]]) -> list[list[int]]:
    """Closed node rings from the open ways of a multipolygon's role, by
    joining segments end to end (reversing one where that is what fits)."""
    rings: list[list[int]] = []
    pool = [list(s) for s in segments if len(s) >= 2]
    while pool:
        cur = pool.pop(0)
        while cur[0] != cur[-1]:
            for i, seg in enumerate(pool):
                if seg[0] == cur[-1]:
                    cur += seg[1:]
                elif seg[-1] == cur[-1]:
                    cur += seg[::-1][1:]
                elif seg[-1] == cur[0]:
                    cur = seg[:-1] + cur
                elif seg[0] == cur[0]:
                    cur = seg[::-1][:-1] + cur
                else:
                    continue
                pool.pop(i)
                break
            else:
                break
        if cur[0] == cur[-1] and len(cur) >= 4:
            rings.append(cur)
    return rings


def extract_water(track: Track, osm: Osm) -> list[dict]:
    """Water bodies near the lap as even-odd ring sets in the track frame."""
    def ring_xy(ids: list[int]) -> np.ndarray | None:
        pts = [osm.nodes[n] for n in ids if n in osm.nodes]
        if len(pts) < 4:
            return None
        return _TO_TRACK(np.array([enu(lo, la, osm.lon0, osm.lat0) for lo, la in pts]))

    def near(p: np.ndarray) -> bool:
        d, _ = track.grid.query(p, max_rings=60)
        return float(d.min()) < WATER_RANGE_M

    bodies: list[dict] = []
    in_relation: set[int] = set()
    for r in osm.relations:
        t = r.get("tags") or {}
        if t.get("natural") != "water" or t.get("type") != "multipolygon":
            continue
        outer, inner = [], []
        for m in r.get("members", []):
            if m.get("type") != "way":
                continue
            w = osm.ways_by_id.get(m["ref"])
            if w is None:
                continue
            in_relation.add(w["id"])
            (inner if m.get("role") == "inner" else outer).append(w["nodes"])
        rings = []
        for ids in _join_rings(outer) + _join_rings(inner):
            p = ring_xy(ids)
            if p is not None:
                rings.append(p)
        if rings and any(near(p) for p in rings):
            bodies.append({"name": osm_name(t), "rings": [round_pts(simplify(p, 3.0), 1) for p in rings]})
    for w in osm.ways:
        t = w.get("tags") or {}
        if t.get("natural") != "water" or w["id"] in in_relation or w["nodes"][0] != w["nodes"][-1]:
            continue
        p = ring_xy(w["nodes"])
        if p is None or not near(p) or ring_area(p) < 200.0:
            continue
        bodies.append({"name": osm_name(t), "rings": [round_pts(simplify(p, 2.0), 1)]})
    bodies.sort(key=lambda b: (b["rings"][0][0], b.get("name") or ""))
    return [{k: v for k, v in b.items() if v is not None} for b in bodies]


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
        if poi_kind is None and stem in WATER_STEMS:
            poi_kind = _first_match(t, CITY_POI_KINDS)
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
    if stem in WATER_STEMS:
        # In a city every cafe is mapped; a stall for each would be absurd.
        poi = [e for e in poi if e["kind"] != "food"]
        areas = [e for e in areas if e["kind"] not in ("residential", "farmland", "farmyard", "camp_site")]
    poi.sort(key=lambda e: (e["station_m"], e["kind"]))
    out = {
        "barriers": barriers,
        "roads": roads,
        "waterways": waterways,
        "areas": areas,
        "poi": poi,
        "unclassified": dict(sorted(census.items(), key=lambda kv: (-kv[1], kv[0]))),
    }
    if stem in WATER_STEMS:
        out["water"] = extract_water(track, osm)
    return out


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

    reach_m = PIT_LANE_REACH_M.get(stem, 70.0)

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
            near = d < reach_m
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
        city = stem in WATER_STEMS
        if is_building and (
            near > (CITY_STRUCTURE_RANGE_M if city else STRUCTURE_RANGE_M)
            or ring_area(p) < (CITY_STRUCTURE_MIN_M2 if city else 250.0)
        ):
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
            for needle, asset in MANUAL_STRUCTURE_ASSETS.get(stem, {}).items():
                if entry["name"] and needle in entry["name"]:
                    entry["asset"] = asset
                    break
            structures.append(entry)
    stands.extend(manual_stands(stem, track))
    # A MANUAL_STANDS entry promoting a named OSM building (tagged plain
    # `building=yes`, so extract() filed it as a structure rather than a
    # stand) replaces that structure instead of doubling it up as a
    # building standing behind its own grandstand.
    structures = drop_promoted_structures(stem, structures)
    structures.extend(MANUAL_STRUCTURES.get(stem, []))
    if stem in NO_OSM_STANDS:
        # Every `building=grandstand`/`leisure=grandstand` way within range
        # here belongs to Lakeside Stadium (represented instead as the
        # `MANUAL_STRUCTURES` building above) rather than the Grand Prix --
        # the task's own seating-map research confirms OSM maps none of
        # Albert Park's real (all-temporary) grandstands, so an "osm"
        # source hit for this stem is always someone else's building.
        stands = [s for s in stands if s.get("source") != "osm"]
    stands = with_stand_cover(stem, stands)
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
    kinds = CROSSING_KINDS.get(stem, {})
    crossings = [dict(c, kind=kinds.get(c["kind"], c["kind"])) for c in crossings]
    if stem in WATER_STEMS:
        lap = {m["ref"] for r in osm.relations if r["id"] in STREET_CIRCUIT_RELATIONS
               for m in r["members"] if m["type"] == "way"}
        for w in osm.ways:
            t = w.get("tags") or {}
            if w["id"] not in lap or t.get("bridge") != "yes":
                continue
            p = to_track(osm.way_xy(w))
            s_all, _ = track.locate(p)
            b_lo, b_hi = float(np.min(s_all)), float(np.max(s_all))
            crossings.append(
                {
                    "name": t.get("bridge:name") or osm_name(t),
                    "station_m": round((b_lo + b_hi) / 2, 1),
                    "kind": "deck_wide" if b_hi - b_lo >= DECK_WIDE_MIN_M else "deck_arch",
                    "from_m": round(b_lo, 1),
                    "to_m": round(b_hi, 1),
                    "source": "authored",
                }
            )
    crossings = dedup_crossings(crossings + manual_crossings(stem, track, osm, to_track))

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
    kind_map = LANDMARK_KINDS.get(stem, {})
    if kind_map:
        landmarks = [dict(l, kind=kind_map.get(l["kind"], l["kind"])) for l in landmarks]
        # One prop for what OSM maps twice (the wheel and its hub node).
        unique: list[dict] = []
        for l in landmarks:
            if any(
                u["kind"] == l["kind"]
                and math.hypot(*(np.array(u["centre"]) - np.array(l["centre"]))) < 120.0
                for u in unique
            ):
                continue
            unique.append(l)
        landmarks = unique
    landmarks = drop_scanned_landmarks(stem, landmarks)
    landmarks += manual_landmarks(stem, track, osm, to_track, landmarks)
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
    structures = drop_structures_under_landmarks(structures, landmarks)

    return {
        "format": LAYOUT_FORMAT,
        "version": LAYOUT_VERSION,
        "source_track": f"{stem}.yaml",
        "track_name": track.data.get("name", stem),
        "track_display_name": track.data.get("display_name"),
        "attribution": ATTRIBUTION,
        "osm_bbox": [list(b) for b in BBOXES[stem]],
        "fit": fit_report,
        "corners": with_display_names(stem, corners),
        "pit_lane": pit,
        "stands": stands,
        "structures": structures,
        "crossings": crossings,
        "landmarks": landmarks,
        "woods": woods,
        **extract_surroundings(stem, track, osm, xy),
        **(
            {"graffiti": g}
            if (g := manual_graffiti(stem, track, osm, to_track) + manual_decal_runs(stem, track)
                + skid_marks(stem, track))
            else {}
        ),
        **(
            {"furniture": MANUAL_FURNITURE.get(stem, []) + generated_furniture(stem)}
            if stem in MANUAL_FURNITURE
            else {}
        ),
    }


# Skid marks: rubber laid by locked and sliding tyres into the tightest
# corners, on the racing line through the braking zone. Generated for these
# circuits from the centerline's own corners (`drs_zones.Corners`): every
# corner tighter than SKID_RADIUS_M gets one to three streaks
# (`graffiti/skid_*`, gen_graffiti.py), ending SKID_END_BEFORE_M before the
# turn-in, SKID_LENGTH_M long, 2.6 m wide (a car's two wheel tracks).
SKID_MARK_STEMS = frozenset({"Mordschleife"})
SKID_RADIUS_M = 70.0
SKID_LENGTH_M = 24.0
SKID_WIDTH_M = 2.6
SKID_END_BEFORE_M = 8.0
SKID_IMAGES = ("skid_long", "skid_short", "skid_late")


def raceline_lateral(track: Track):
    """The racing line's offset from the centerline (positive left) as a
    function of station, from the YAML's raceline points."""
    line = track.data.get("raceline") or []
    if len(line) < 3:
        return lambda s: 0.0
    pts = np.array([[p["x"], p["y"]] for p in line], dtype=float)
    s, lat = track.locate(pts)
    order = np.argsort(s)
    s, lat = s[order], lat[order]
    return lambda q: float(np.interp(q % track.total, s, lat, period=track.total))


def skid_marks(stem: str, track: Track) -> list[dict]:
    """Skid marks into every tight corner of a `SKID_MARK_STEMS` circuit,
    in the dossier's `graffiti` list (`ats-dress` lays them as decals)."""
    if stem not in SKID_MARK_STEMS:
        return []
    import drs_zones as dz

    corners = dz.Corners(dz.load(stem))
    lateral = raceline_lateral(track)
    out = []
    for i, (entry, exit_, _sign) in enumerate(corners.runs):
        inside = (corners.mid >= entry) & (corners.mid <= exit_)
        if not inside.any():
            continue
        peak = float(np.max(np.abs(corners.kappa[inside])))
        if peak < 1.0 / SKID_RADIUS_M:
            continue
        # One to three streaks, the tightest corners the most.
        n = 1 + (peak >= 1.0 / 45.0) + (peak >= 1.0 / 30.0)
        for k in range(n):
            end = entry - SKID_END_BEFORE_M - k * (SKID_LENGTH_M * 0.7)
            start = end - SKID_LENGTH_M
            mid = start + SKID_LENGTH_M / 2
            # On the line, but whole on the tarmac (a decal trimmed to the
            # road would squeeze the two tracks together).
            room_l = track.half_width(mid, "left") - SKID_WIDTH_M / 2 - 0.3
            room_r = track.half_width(mid, "right") - SKID_WIDTH_M / 2 - 0.3
            lat = min(max(lateral(mid) + (0.25, -0.2, 0.1)[k], -room_r), room_l)
            out.append(
                {
                    "image": f"graffiti/{SKID_IMAGES[(i + k) % len(SKID_IMAGES)]}",
                    "station_m": round(start % track.total, 1),
                    "lat_m": round(lat, 2),
                    "length_m": SKID_LENGTH_M,
                    "width_m": SKID_WIDTH_M,
                    "name": "Skid marks",
                }
            )
    return out


def manual_decal_runs(stem: str, track: Track) -> list[dict]:
    """`MANUAL_DECAL_RUNS` as one decal per `every_m`, centred `beyond_edge_m`
    outside the road edge on the run's side (so it is painted on the apron
    between the road and the wall). They ride in the dossier's `graffiti`
    list: `ats-dress` owns every decal whose image is in a dressed set."""
    out = []
    for run in MANUAL_DECAL_RUNS.get(stem, []):
        span = (run["to_m"] - run["from_m"]) % track.total
        n = int(span // run["every_m"]) + 1
        sign = 1.0 if run["side"] == "left" else -1.0
        for i in range(n):
            station = (run["from_m"] + i * run["every_m"]) % track.total
            half = track.half_width(station, run["side"])
            out.append(
                {
                    "image": run["image"],
                    "station_m": round(station, 1),
                    "lat_m": round(sign * (half + run["beyond_edge_m"]), 2),
                    "length_m": run["length_m"],
                    "width_m": run["width_m"],
                    "name": run["name"],
                }
            )
    return out


def manual_graffiti(stem: str, track: Track, osm: Osm, to_track) -> list[dict]:
    """Expand `MANUAL_GRAFFITI` into one entry per picture.

    A run is anchored on the map, not on a station: `at` is a (lon, lat)
    on the road (the section's own OSM locality node, say), projected onto
    the centerline through the dossier's fit, so a run stays where it is
    when the centerline is re-derived. It paints its images one after
    another every `every_m` metres from `before_m` ahead of that point to
    `after_m` past it, cycling through the list, alternating a little
    either side of the middle of the road the way a crowd's pictures lie;
    `lat_m` pins a run to one line instead. The dossier carries the
    expanded list, so `ats-dress` has nothing to decide."""
    out = []
    for run in MANUAL_GRAFFITI.get(stem, []):
        lon, lat_deg = run["at"]
        anchor, _ = track.locate(to_track(np.array([enu(lon, lat_deg, osm.lon0, osm.lat0)])))
        from_m = float(anchor[0]) - run.get("before_m", 0.0)
        images = run["images"]
        every = run.get("every_m", 40.0)
        length = run.get("length_m", 16.0)
        width = run.get("width_m", 6.5)
        span = run.get("before_m", 0.0) + run.get("after_m", 200.0)
        n = max(int(span // every), 1)
        for i in range(n):
            station = (from_m + i * every) % track.total
            if "lat_m" in run:
                lat = run["lat_m"]
            else:
                lat = (0.6, -0.8, 0.2, -0.3)[i % 4] * run.get("sway_m", 1.0)
            out.append(
                {
                    "image": f"graffiti/{images[i % len(images)]}",
                    "station_m": round(station, 1),
                    "lat_m": round(lat, 2),
                    "length_m": length,
                    "width_m": width,
                    "name": run["name"],
                }
            )
    return out


def manual_landmarks(stem: str, track: Track, osm, to_track, landmarks: list[dict]) -> list[dict]:
    """`MANUAL_LANDMARKS` as dossier entries, each `source: authored`. An
    entry is anchored on the map (`at`, lon/lat) or beside the road
    (`station_m`, `side`, `offset_m`); `landmarks` are what the scan found,
    which an estimate yields to. `osm` needs only `lon0` / `lat0`."""
    out: list[dict] = []
    for spec in MANUAL_LANDMARKS.get(stem, []):
        if "at" in spec:
            # Anchored on the map rather than beside the road: a castle on
            # its hill half a kilometre away is where it is, not "40 m to
            # the left of station N".
            p = to_track(np.array([enu(*spec["at"], osm.lon0, osm.lat0)]))[0]
            s_at, lat_at = track.locate(p[None, :])
            spec = {**spec, "station_m": round(float(s_at[0]), 1),
                    "side": "left" if lat_at[0] > 0 else "right"}
        else:
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
        if "yaw_rad" in spec:
            entry["yaw_rad"] = spec["yaw_rad"]
        if "yaw_toward" in spec:
            # Long axis along the line from the landmark to this point.
            q = to_track(np.array([enu(*spec["yaw_toward"], osm.lon0, osm.lat0)]))[0]
            entry["yaw_rad"] = round(math.atan2(q[1] - p[1], q[0] - p[0]), 4)
        if "yaw_across" in spec:
            # Long axis across the line from the landmark to this point (a
            # toll lane's arm across its lane).
            q = to_track(np.array([enu(*spec["yaw_across"], osm.lon0, osm.lat0)]))[0]
            entry["yaw_rad"] = round(math.atan2(q[1] - p[1], q[0] - p[0]) + math.pi / 2, 4)
        if "broadside_to_m" in spec:
            # Long axis across the line of sight from that station.
            eye = track.offset_point(spec["broadside_to_m"], "left", -track.half_width(spec["broadside_to_m"], "left"))
            sight = p - eye
            entry["yaw_rad"] = round(math.atan2(sight[1], sight[0]) + math.pi / 2, 4)
        out.append(entry)
    return out


def manual_crossings(stem: str, track: Track, osm, to_track) -> list[dict]:
    """`MANUAL_CROSSINGS` as dossier entries. A station may be given as a
    map anchor instead (`at`, or `from_at` / `to_at` for a span: lon/lat),
    projected onto the centerline through the fit like the graffiti."""
    out = []
    for spec in MANUAL_CROSSINGS.get(stem, []):
        entry = {k: v for k, v in spec.items() if k not in ("at", "from_at", "to_at")}

        def station(at):
            s, _ = track.locate(to_track(np.array([enu(*at, osm.lon0, osm.lat0)])))
            return float(s[0])

        if "at" in spec:
            entry["station_m"] = round(station(spec["at"]), 1)
        if "from_at" in spec and "to_at" in spec:
            lo, hi = station(spec["from_at"]), station(spec["to_at"])
            # In race order, whichever way the way was drawn.
            if (hi - lo) % track.total > track.total / 2:
                lo, hi = hi, lo
            span = (hi - lo) % track.total
            entry["from_m"] = round(lo, 1)
            entry["to_m"] = round(lo + span, 1)
            entry["station_m"] = round((lo + span / 2) % track.total, 1)
        out.append(dict(entry, source="authored"))
    return out


def dedup_crossings(crossings: list[dict]) -> list[dict]:
    """In station order, one entry per crossing: the same crossing mapped
    twice within 25 m keeps the authored entry (it says what the structure
    *is*)."""
    crossings = sorted(crossings, key=lambda c: c["station_m"])
    dedup = []
    for c in crossings:
        if dedup and abs(c["station_m"] - dedup[-1]["station_m"]) < 25:
            if c.get("source") == "authored" and dedup[-1].get("source") != "authored":
                dedup[-1] = c
            continue
        dedup.append(c)
    return dedup


# How far outside an OSM outline an authored landmark may stand and still
# be taken for the same structure.
LANDMARK_SAME_STRUCTURE_M = 5.0


def landmark_inside_structure(structure: dict, landmark: dict) -> bool:
    """True when the landmark's centre lies on the structure's own outline
    (its oriented length x depth box, `yaw_rad` along the length), give or
    take LANDMARK_SAME_STRUCTURE_M. A circle of the outline's longest half
    took a 308 m stand with any screen within 159 m of its middle, either
    side of the road (R19)."""
    dx = landmark["centre"][0] - structure["centre"][0]
    dy = landmark["centre"][1] - structure["centre"][1]
    yaw = structure.get("yaw_rad", 0.0)
    along = dx * math.cos(yaw) + dy * math.sin(yaw)
    across = -dx * math.sin(yaw) + dy * math.cos(yaw)
    return (abs(along) <= structure["length_m"] / 2 + LANDMARK_SAME_STRUCTURE_M
            and abs(across) <= structure["depth_m"] / 2 + LANDMARK_SAME_STRUCTURE_M)


def drop_structures_under_landmarks(structures: list[dict], landmarks: list[dict]) -> list[dict]:
    """An authored landmark and an OSM building can be the same real
    structure: keep the landmark, drop the building row it stands on."""
    authored = [l for l in landmarks if l.get("source") == "authored"]
    if not authored:
        return structures
    return [s for s in structures if not any(landmark_inside_structure(s, l) for l in authored)]


# ----------------------------------------------------------- georeferencing


def fit_track(stem: str, offline: bool) -> tuple[Track, Osm, np.ndarray, np.ndarray, dict]:
    """Georeference one circuit onto its own YAML frame.

    Returns `(track, osm, a, t, report)`, where a point in the OSM extract's
    ENU frame lands in the track frame at `p @ a.T + t`.  The whole alignment
    lives here -- the coarse FFT search, the trimmed ICP, and the gate that
    refuses a fit which does not explain the centerline -- because anything
    else that puts real-world data into a track's frame (the DEM, via
    `tools/dem_fetch.py`) has to use the *same* alignment the dossier was
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


def rename_dossiers(stems: list[str], dry_run: bool) -> int:
    """`--names-only`: the display names are editorial, not geometry, so
    changing one must not need the OSM extract (gitignored, and slow to
    fetch). Everything but the names is left byte for byte."""
    for stem in stems:
        out = track_dir(stem) / f"{stem}.layout.json"
        if not out.exists():
            continue
        layout = json.loads(out.read_text(encoding="utf-8"))
        track = yaml.safe_load((track_dir(stem) / f"{stem}.yaml").read_text(encoding="utf-8"))
        renamed = {}
        for k, v in layout.items():
            renamed[k] = v
            if k == "track_name":
                renamed["track_display_name"] = track.get("display_name")
        renamed["corners"] = with_display_names(stem, layout.get("corners", []))
        changed = [
            f"{c['name']} -> {c['display_name']}"
            for c in renamed["corners"]
            if c["display_name"] != c["name"]
        ]
        unknown = set(CORNER_DISPLAY.get(stem, {})) - {c["name"] for c in renamed["corners"]}
        print(f"{stem}: {len(renamed['corners'])} corners, {len(changed)} renamed or dropped")
        for line in changed:
            print(f"   {line}")
        for name in sorted(unknown):
            print(f"   (CORNER_DISPLAY names {name!r}, which the dossier does not have)")
        if not dry_run:
            out.write_text(json.dumps(renamed, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


def manual_stands(stem: str, track) -> list[dict]:
    """The `MANUAL_STANDS` entries laid along the centerline, as dossier
    stands (`source: "seating map"`)."""
    out = []
    for spec in MANUAL_STANDS.get(stem, []):
        side = spec["side"]
        if side in ("outside", "inside"):
            side = track.turn_side(spec["from_m"], spec["to_m"], side)
        front = track.edge_run(spec["from_m"], spec["to_m"], side, spec.get("gap_m", 10.0))
        length = float(np.hypot(*np.diff(front, axis=0).T).sum())
        centre = front.mean(0)
        s, _ = track.locate(centre[None, :])
        head = front[-1] - front[0]
        out.append(
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
                **({"family": spec["family"]} if "family" in spec else {}),
            }
        )
    return out


# How far a rebuilt seating-map stand may sit from the dossier's and still
# be the same entry: the centerline moves by centimetres when it is
# smoothed after the dossier is built, and that is not an edit.
SAME_STAND_M = 0.5


def same_stand_or(laid: list[dict], stand: dict) -> dict:
    """The dossier's own copy of `stand` when its table entry has not
    changed (same name, side, depth, roof and family, the front within
    `SAME_STAND_M`), so a re-apply leaves an untouched stand byte for byte;
    `stand` otherwise."""
    keys = ("name", "side", "offset_m", "depth_m", "covered", "family")
    for old in laid:
        if any(old.get(k) != stand.get(k) for k in keys):
            continue
        a, b = old.get("front", []), stand["front"]
        if abs(len(a) - len(b)) > 1 or abs(old["length_m"] - stand["length_m"]) > SAME_STAND_M:
            continue
        ends = (a[0], b[0]), (a[-1], b[-1]), (old["centre"], stand["centre"])
        if all(math.dist(p, q) <= SAME_STAND_M for p, q in ends):
            return old
    return stand


def drop_promoted_structures(stem: str, structures: list[dict]) -> list[dict]:
    """Structures less the named OSM buildings a `MANUAL_STANDS` entry
    stands in for."""
    names = {spec["name"] for spec in MANUAL_STANDS.get(stem, [])}
    return [s for s in structures if s.get("name") not in names]


def with_stand_cover(stem: str, stands: list[dict]) -> list[dict]:
    """`stands` with `STAND_COVER` applied as each stand's `roof`, which
    the dress pass takes over `covered` and the depth rule; a stand the
    table does not name loses any `roof` an earlier run gave it."""
    cover = STAND_COVER.get(stem, {})
    out = []
    for s in stands:
        roof = cover.get(s.get("name"), cover.get(f"@{s['station_m']}"))
        if roof not in (None, "covered", "open"):
            raise SystemExit(f"STAND_COVER[{stem!r}][{s.get('name')!r}]: {roof!r} is not covered or open")
        s = {k: v for k, v in s.items() if k != "roof"}
        if roof:
            s["roof"] = roof
        out.append(s)
    return out


def reapply_manual(stems: list[str], dry_run: bool) -> int:
    """`--manual-only`: re-apply the hand-authored layers (MANUAL_STANDS and
    STAND_COVER, MANUAL_LANDMARKS, MANUAL_CROSSINGS and CROSSING_KINDS, the
    graffiti, decal runs and skid marks, MANUAL_FURNITURE) to a checked-in dossier without the OSM
    extract, through the georeference the DEM sidecar records (the same fit
    the dossier was built from, `dem_fetch.py`). Everything the extract
    gave is left byte for byte. A circuit without a DEM sidecar is skipped."""
    import msgpack

    for stem in stems:
        out = track_dir(stem) / f"{stem}.layout.json"
        dem_path = track_dir(stem) / f"{stem}.dem.msgpack"
        if not out.exists() or not dem_path.exists():
            print(f"{stem}: no dossier or no DEM sidecar, skipped")
            continue
        georef = msgpack.unpackb(dem_path.read_bytes(), raw=False)["georef"]
        a = np.array(georef["a"], dtype=float).reshape(2, 2)
        t = np.array(georef["t"], dtype=float)
        to_track = lambda p: np.asarray(p, dtype=float).reshape(-1, 2) @ a.T + t  # noqa: E731

        class Frame:
            lon0 = georef["lon0"]
            lat0 = georef["lat0"]

        layout = json.loads(out.read_text(encoding="utf-8"))
        track = Track(stem)
        # The scan's landmarks as the dossier has them, renamed by
        # LANDMARK_KINDS (a mapped kind maps to itself, so this is the same
        # on a re-run) and without the ones DROP_SCANNED_LANDMARKS leaves out.
        kind_map = LANDMARK_KINDS.get(stem, {})
        scanned = drop_scanned_landmarks(
            stem,
            [dict(l, kind=kind_map.get(l["kind"], l["kind"]))
             for l in layout.get("landmarks", []) if l.get("source") != "authored"],
        )
        landmarks = scanned + manual_landmarks(stem, track, Frame, to_track, scanned)
        landmarks.sort(key=lambda e: e["station_m"])
        kinds = CROSSING_KINDS.get(stem, {})
        # The scan's crossings, and the street circuits' lap bridges (laid
        # from OSM as `deck_*`, marked authored), stay; the table's go.
        surveyed = [
            dict(c, kind=kinds.get(c["kind"], c["kind"]))
            for c in layout.get("crossings", [])
            if c.get("source") != "authored" or c["kind"] in ("deck_arch", "deck_wide")
        ]
        crossings = dedup_crossings(surveyed + manual_crossings(stem, track, Frame, to_track))
        graffiti = manual_graffiti(stem, track, Frame, to_track) + manual_decal_runs(stem, track) + skid_marks(stem, track)
        furniture = MANUAL_FURNITURE.get(stem, []) + generated_furniture(stem) if stem in MANUAL_FURNITURE else []
        # The seating-map stands are rebuilt from the table, the surveyed
        # ones kept, and the roofs re-read for both.
        stands = [s for s in layout.get("stands", []) if s.get("source") != "seating map"]
        laid = [s for s in layout.get("stands", []) if s.get("source") == "seating map"]
        manual = [same_stand_or(laid, s) for s in manual_stands(stem, track)]
        stands = with_stand_cover(stem, stands + manual)
        stands.sort(key=lambda e: e["station_m"])
        new = dict(layout)
        new["stands"] = stands
        new["landmarks"] = landmarks
        new["crossings"] = crossings
        new["structures"] = drop_structures_under_landmarks(
            drop_promoted_structures(stem, layout.get("structures", [])), landmarks
        )
        for key, value in (("graffiti", graffiti), ("furniture", furniture)):
            if value:
                new[key] = value
            else:
                new.pop(key, None)
        changed = [k for k in new if new.get(k) != layout.get(k)] + [k for k in layout if k not in new]
        print(f"{stem}: {len(stands)} stands, {len(landmarks)} landmarks, {len(crossings)} crossings, {len(graffiti)} decals"
              + (f"; changed: {', '.join(changed)}" if changed else "; unchanged"))
        if not dry_run and changed:
            out.write_text(json.dumps(new, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("tracks", nargs="*", help="track stems, e.g. Monzarella")
    ap.add_argument("--all", action="store_true", help="every circuit with a bbox")
    ap.add_argument("--offline", action="store_true", help="use the cache only")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    ap.add_argument(
        "--names-only",
        action="store_true",
        help="re-apply the display names (CORNER_DISPLAY, the YAML's display_name) "
        "to the existing dossiers without refetching",
    )
    ap.add_argument(
        "--manual-only",
        action="store_true",
        help="re-apply the hand-authored layers (stands, landmarks, crossings, graffiti, furniture) "
        "to the existing dossiers without the OSM extract, through the DEM sidecar's georeference",
    )
    args = ap.parse_args()

    stems = sorted(BBOXES) if args.all else args.tracks
    if not stems:
        ap.print_help()
        return 1
    if args.names_only:
        return rename_dossiers(stems, args.dry_run)
    if args.manual_only:
        return reapply_manual(stems, args.dry_run)
    for stem in stems:
        if stem not in BBOXES:
            raise SystemExit(f"no bbox for {stem}; add one to BBOXES")
        layout = build(stem, args.offline)
        if args.dry_run:
            continue
        out = track_dir(stem) / f"{stem}.layout.json"
        out.write_text(json.dumps(layout, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"   wrote {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
