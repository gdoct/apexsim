"""The realism reference sheet (docs/game/realism.md): each of the owner's reference
frames in out/screenshots/ beside its game shot from a g8-* run, into out/realism/."""
import html
import shutil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2] / "out"
REFS = ROOT / "screenshots"
OUT = ROOT / "realism"
V = ROOT / "verify"

# (circuit, view, reference file, run, shot number, note)
PAIRS = [
    ("Spa", "Cockpit, GT3", "Spa-Francorchamps GT3 onboard driver view Eau Rouge.png", "g8-spa-gt3", 2, "down the hill to the dip"),
    ("Spa", "TV trackside", "Spa-Francorchamps GT3 rear broadcast camera Eau Rouge Raidillon.png", "g8-spa-gt3", 8, "behind the dip, up the climb"),
    ("Spa", "Chase / spectator", "Spa-Francorchamps GT3 rear view chase camera Eau Rouge Raidillon.png", "g8-spa-gt3", 9, "spectator behind the dip"),
    ("Spa", "Grandstand straight", "Spa-Francorchamps finishing line.png", "g8-spa-gt3", 10, "from the hairpin back up the straight"),
    ("Spa", "Grandstand straight", "Spa-Francorchamps main straight.png", "g8-spa-gt3", 11, "from above the line"),
    ("Spa", "Pit lane", "Spa-Francorchamps pit lane garages pit wall gantry.png", "g8-spa-gt3", 14, "from above"),
    ("Spa", "Woods", "Spa-Francorchamps woods 2.png", "g8-spa-gt3", 12, "aerial over the dip"),
    ("Spa", "Chase", None, "g8-spa-gt3", 20, "chase near rung"),
    ("Spa", "Cockpit, formula", None, "g8-spa-formula", 2, "no reference frame"),
    ("Monza", "Cockpit, GT3", "Monza GT3 onboard in a McLaren 720S GT3 Evo.png", "g8-monza-gt3", 6, ""),
    ("Monza", "Pit lane", "Monza GT3 onboard pitlane.png", "g8-monza-gt3", 12, "eye height beside the garages"),
    ("Monza", "Chase", "Monza chase.png", "g8-monza-gt3", 20, "chase near rung"),
    ("Monza", "Cockpit, formula", "Monza f1 onboard bridge.png", "g8-monza-formula", 2, "reference is another sim's render"),
    ("Monza", "Woods", "Monza trees 2.png", "g8-monza-gt3", 10, "in the Lesmo woods, eye height"),
    ("Monza", "Woods", "Monza trees.png", "g8-monza-gt3", 11, "aerial over the Lesmo woods"),
    ("Monza", "TV trackside", "Monza tv cam.png", "g8-monza-gt3", 8, "low along the barrier"),
    ("Monza", "TV director", None, "g8-monza-gt3", 17, "a director cut through foliage"),
    ("Nordschleife", "Cockpit, formula", "F1 nurburgring onboard.png", "g8-nordschleife-formula", 5, "T-cam"),
    ("Nordschleife", "Chase", "Nordschleife chase.png", "g8-nordschleife-gt3", 15, "chase near rung"),
    ("Nordschleife", "Woods", "Nordschleife cinematic cam.png", "g8-nordschleife-gt3", 10, "aerial, 9.6 km"),
    ("Nordschleife", "TV trackside", "Nordschleife cinematic camera.png", "g8-nordschleife-gt3", 7, "low, facing the traffic"),
    ("Nordschleife", "Cockpit, GT3", "Nordschleife gt3 onboard.png", "g8-nordschleife-gt3", 6, ""),
    ("Nordschleife", "Cockpit, GT3", "Nordschleife gt3 onboard graffiti.png", "g8-nordschleife-gt3", 5, ""),
    ("Nordschleife", "Woods", "Nordschleife trees 1.png", "g8-nordschleife-gt3", 8, "down to a bend, 8 km"),
    ("Nordschleife", "Woods", "Nordschleife trees 2.png", "g8-nordschleife-gt3", 9, "eye height, 1.5 km"),
    ("Nordschleife", "TV director", None, "g8-nordschleife-gt3", 12, "a director cut behind the armco"),
    ("Nordschleife", "Pit lane", "nurburgring pit lane.png", "g8-nuerburgring-pit", 1, "the GP circuit's lane"),
]

H = 720


def fit(im: Image.Image) -> Image.Image:
    im = im.convert("RGB")
    return im.resize((round(im.width * H / im.height), H), Image.LANCZOS)


def label(im, text):
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype("arial.ttf", 26)
    except OSError:
        f = ImageFont.load_default()
    w = d.textlength(text, font=f)
    d.rectangle([0, 0, w + 20, 40], fill=(0, 0, 0))
    d.text((10, 6), text, fill=(255, 210, 0), font=f)


def main():
    OUT.mkdir(exist_ok=True)
    (OUT / "shots").mkdir(exist_ok=True)
    rows = []
    for i, (circuit, view, ref, run, shot, note) in enumerate(PAIRS, 1):
        shot_path = V / run / "shots" / f"{shot:02d}.jpg"
        kept = OUT / "shots" / f"{run}-{shot:02d}.jpg"
        shutil.copy(shot_path, kept)
        game = fit(Image.open(kept))
        label(game, f"game: {run} #{shot}")
        if ref:
            real = fit(Image.open(REFS / ref))
            label(real, "reference")
            sheet = Image.new("RGB", (real.width + 8 + game.width, H), (20, 20, 20))
            sheet.paste(real, (0, 0))
            sheet.paste(game, (real.width + 8, 0))
        else:
            sheet = game
        name = f"{i:02d}.jpg"
        sheet.save(OUT / name, quality=88)
        rows.append((i, circuit, view, ref or "(none)", run, shot, note, name))
    with open(OUT / "index.html", "w", encoding="utf-8") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Realism sheet</title>"
                "<style>body{background:#111;color:#ddd;font:15px system-ui;margin:16px}"
                "img{max-width:100%;display:block;margin:4px 0 28px}h2{margin:0}</style>"
                "<h1>G8 realism reference sheet</h1><p>Reference left, game right. "
                "Ranking: apexsim/docs/game/realism.md.</p>")
        for i, c, v, ref, run, shot, note, name in rows:
            f.write(f"<h2>{i}. {html.escape(c)}: {html.escape(v)}</h2><p>{html.escape(ref)} | "
                    f"{run} #{shot} {html.escape(note)}</p><img src='{name}'>")
    print(f"{len(rows)} pairs in {OUT}")


if __name__ == "__main__":
    main()
