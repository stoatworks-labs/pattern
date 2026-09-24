"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A setting the CPU reads but never acts on
is quieter still. A control can therefore be completely dead while everything
compiles, links, loads and renders. Nothing else in this repo catches that.

So: render each parameter at both ends of its range against a context that makes
it mean something, and report any that made no difference at all.

    python3 tools/sweep.py [--size WxH] [--jobs N] [--binary PATH]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**A pattern editor with no audio writes nothing.** Every render here plays the
harness's synthetic drum pattern (`--audio beats`, 125 BPM), because half the
controls decide what is heard and how it is written. A sweep run in silence
would report the whole Listening group dead, correctly.

**The clock controls need time to disagree.** Tempo Source's Host (the SDK's
120 BPM) and Manual (125) put the cursor on the same row for the first two
seconds. Four seconds apart they are a row apart. Swing is worst: it moves the
odd rows' start, so it is swept at a frame where the phase is inside an odd
row (frame 249 at 120 BPM: raw phase 33.2).

**Keep Notes only shows when a pass finds nothing to write.** The drum pattern
is periodic, so every row it wrote on pass 0 it writes again on pass 1 and
Keep on and off look the same. It is swept with the audio stopping at four
seconds and the shot taken at ten: pass 1 then enters sixteen empty rows,
which Keep off clears and Keep on does not.

**Every name must be unique.** `--set` finds a parameter by name and takes the
first match. `pntest --names` fails on a duplicate for that reason.

**Never sweep the About block.** Those are buttons that open a web browser.
`pntest --list` marks them, and marks the audio buffer, which the host writes.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "pntest")
SCRATCH = tempfile.mkdtemp(prefix="pnsweep")

WIDTH, HEIGHT = 640, 360
FRAMES = 240          # four seconds at 60 fps

SKIP = {}

MANUAL = {"Tempo Source": 2}

# `_frames`, `_until` and `_low`/`_high` are harness or sweep settings, not
# parameters; anything starting with an underscore is stripped before --set.
CONTEXT = {
    "Tempo Source": {"_high": 2},
    "BPM": MANUAL,
    "Speed": {},
    # An odd row's start moves with swing; frame 249 at 120 BPM is inside one.
    "Swing": {"_frames": 249},
    "Rows": {},
    "Keep Notes": {"_frames": 600, "_until": 4},
    "Channels": {},
    "Bin Law": {},
    "Bin Value": {},
    "Sensitivity": {},
    "Band Split": {},
    "Fold Octaves": {},
    "Theme": {"_high": 2},
    "Rows Visible": {},
    "Show VU": {},
    "Show Effects": {},
    "Scale": {},
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}",
            "--frames", str(frames), "--fps", "60", "--audio", "beats", "--audio-bpm", "125"]
    if "_until" in overrides:
        args += ["--audio-until", str(overrides["_until"])]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, low, high, context = job

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi)
    fraction, count = difference(a, b)
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--binary", default=BIN)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    BIN = args.binary
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind == "buffer":
            skipped.append((name, "the host's spectrum; the harness plays a drum pattern into it"))
            continue
        if kind == "text" or name in SKIP:
            skipped.append((name, SKIP.get(name, "free text")))
            continue
        work.append((pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept at {WIDTH}x{HEIGHT}, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
