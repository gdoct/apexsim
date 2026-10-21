"""Compare two AI survey outputs: per track and in total.

    python scripts/survey_compare.py <before.txt> <after.txt>

Each file is the `--nocapture` output of
`survey_ai_races_on_every_circuit` (docs/server/ai.md), one at the commit
before a change and one after. A track whose contact, off-road or retired
total moves by more than 2 or half its value is starred.
"""
import re
import sys
from collections import defaultdict

LINE = re.compile(
    r"^\s*(\w+)\s+(\S+): out of shape at 3 s (\d+), car-seconds of contact\s+([\d.]+), "
    r"off\s+([\d.]+), sliding\s+([\d.]+), retired (\d+), passes (\d+)"
)
KEYS = ("shape", "contact", "off", "sliding", "retired", "passes")


def read(path):
    out = defaultdict(lambda: [0.0] * len(KEYS))
    for line in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(line)
        if m:
            vals = [float(v) for v in m.groups()[2:]]
            row = out[m.group(1)]
            for i, v in enumerate(vals):
                row[i] += v
    return out


a, b = read(sys.argv[1]), read(sys.argv[2])
print(f"{'track':18}" + "".join(f"{k:>18}" for k in KEYS))
tot_a, tot_b = [0.0] * len(KEYS), [0.0] * len(KEYS)
for track in sorted(set(a) | set(b)):
    ra, rb = a.get(track, [0.0] * len(KEYS)), b.get(track, [0.0] * len(KEYS))
    for i in range(len(KEYS)):
        tot_a[i] += ra[i]
        tot_b[i] += rb[i]
    cells = "".join(f"{ra[i]:8.1f} ->{rb[i]:7.1f}" for i in range(len(KEYS)))
    flag = " *" if any(abs(ra[i] - rb[i]) > max(2.0, 0.5 * ra[i]) for i in (1, 2, 4)) else ""
    print(f"{track:18}{cells}{flag}")
print(f"{'TOTAL':18}" + "".join(f"{tot_a[i]:8.1f} ->{tot_b[i]:7.1f}" for i in range(len(KEYS))))
