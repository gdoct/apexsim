"""Render the fetched PDF maps to PNG pages, so they can be read as images.

    python docs/circuit-references/render_maps.py            # every stem
    python docs/circuit-references/render_maps.py Sudoku     # one stem

Each `maps/<Stem>/<name>.pdf` becomes `maps/<Stem>/png/<name>.p<N>.png`,
about 2400 px on the long side (labels stay legible, the file stays under
an image reader's limit). Pages already rendered are skipped. Long media
kits are cut at `--pages` (default 12); the site plan in one is often
further in, so pass a larger number for it. Needs PyMuPDF
(`pip install pymupdf`).
"""

import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
MAPS = HERE / "maps"
LONG_SIDE_PX = 2400


def render(pdf: pathlib.Path, max_pages: int) -> int:
    import pymupdf

    out = pdf.parent / "png"
    out.mkdir(exist_ok=True)
    made = 0
    with pymupdf.open(pdf) as doc:
        for i, page in enumerate(doc):
            if i >= max_pages:
                break
            dst = out / f"{pdf.stem}.p{i + 1}.png"
            if dst.exists():
                continue
            zoom = LONG_SIDE_PX / max(page.rect.width, page.rect.height)
            page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom)).save(dst)
            made += 1
    return made


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stems", nargs="*", help="stems to render (default: all)")
    ap.add_argument("--pages", type=int, default=12, help="pages per PDF at most")
    args = ap.parse_args()
    dirs = [MAPS / s for s in args.stems] if args.stems else sorted(d for d in MAPS.iterdir() if d.is_dir())
    for d in dirs:
        if not d.is_dir():
            sys.exit(f"no maps for {d.name}: run fetch_maps.ps1 first")
        for pdf in sorted(d.glob("*.pdf")):
            try:
                made = render(pdf, args.pages)
            except Exception as e:  # a damaged download: say so, carry on
                print(f"{d.name}/{pdf.name}: {e}")
                continue
            print(f"{d.name}/{pdf.name}: {made} page(s) rendered")


if __name__ == "__main__":
    main()
