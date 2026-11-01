"""Build every generated car, or the ones named, in a few Blender sessions.

    python tools/generators/cars/build_cars.py [-j 8] [posh_gt3rs f1 ...]

A name is a car script's stem (`posh_gt3rs`) or a kit (`gt3`, `f1`, ...),
which builds all of that kit's cars. Run with plain Python it shares the
cars out over `-j` headless Blenders (`$BLENDER`, else Blender 5.1); each
of those runs this script again inside Blender, where it builds its share
one car after another. Each car script runs as if started on its own (it
resets the scene and reloads carlib and its kit), so the output is the same
as one Blender per car without paying Blender's start for every car.
"""
import os, re, runpy, subprocess, sys, time

BLENDER = os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe")
_SELF = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
if not os.path.isdir(os.path.join(_SELF, "kits")):
    _SELF = os.path.join(os.environ.get("APEXSIM_CONTENT", r"E:\apexsim\apexsim-content"),
                         "tools", "generators", "cars")


def cars():
    """{stem: kit} for every car script in this folder."""
    out = {}
    for f in sorted(os.listdir(_SELF)):
        if f.endswith(".py"):
            text = open(os.path.join(_SELF, f), encoding="utf-8").read()
            m = re.search(r'^kit = carlib\.load\("kits/(\w+)"\)', text, re.M)
            if m:
                out[f[:-3]] = m.group(1)
    return out


def pick(names):
    known = cars()
    unknown = [n for n in names if n not in known and n not in known.values()]
    if unknown:
        raise SystemExit("no car or kit named: " + " ".join(unknown))
    return [s for (s, k) in known.items() if not names or s in names or k in names]


def build(stems):
    """Inside Blender: run each car script in turn."""
    failed = []
    t0 = time.time()
    for stem in stems:
        t = time.time()
        try:
            runpy.run_path(os.path.join(_SELF, stem + ".py"), run_name="__main__")
            print("built %s in %.1f s" % (stem, time.time() - t), flush=True)
        except Exception:  # keep going: one broken car should not stop the rest
            import traceback
            traceback.print_exc()
            failed.append(stem)
    print("%d cars in %.0f s" % (len(stems), time.time() - t0))
    return failed


def launch(stems, jobs):
    """Outside Blender: share the cars out over `jobs` headless Blenders."""
    shares = [stems[i::jobs] for i in range(jobs) if stems[i::jobs]]
    t0 = time.time()
    procs = [subprocess.Popen([BLENDER, "-b", "--factory-startup", "--python-exit-code", "1",
                               "--python", os.path.join(_SELF, "build_cars.py"), "--"] + share,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                              errors="replace")
             for share in shares]
    failed = []
    for share, p in zip(shares, procs):
        out = p.communicate()[0]
        built = set(re.findall(r"^built (\w+) in", out, re.M))
        for line in out.splitlines():
            if line.startswith(("built ", "Traceback", "  File", "    ")) or "Error" in line:
                print(line)
        failed += [s for s in share if s not in built]
    print("%d cars in %.0f s with %d Blenders%s" % (len(stems), time.time() - t0, len(shares),
                                                     ", failed: " + " ".join(failed) if failed else ""))
    return failed


try:
    import bpy  # noqa: F401
    IN_BLENDER = True
except ImportError:
    IN_BLENDER = False

if IN_BLENDER:
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if build(pick(args)):
        sys.exit(1)
elif __name__ == "__main__":
    args = sys.argv[1:]
    jobs = 8
    if "-j" in args:
        i = args.index("-j")
        jobs = int(args[i + 1])
        del args[i:i + 2]
    if launch(pick(args), max(1, jobs)):
        sys.exit(1)
