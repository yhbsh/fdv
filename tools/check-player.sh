#!/bin/bash
# Regression check for fpl.
#
# The unit suite cannot reach the player: it needs a window, a GPU and a clock.
# So this drives the real binary and checks the three things that can silently
# break without any of them looking broken from the outside.
#
#   1. the colour conversion       (--dump, against an independent reference)
#   2. the GPU path                (--grab reads the drawn frame back)
#   3. the playback loop           (--log-frames: order, rate, looping)
#
# Needs a display. Exits non-zero on the first failure.

set -u
cd "$(dirname "$0")/.."
BIN=build/fdv
PLAY=build/fpl
WORK=build/_checkplayer
fail() { echo "FAIL: $*"; exit 1; }

[ -x "$BIN" ]  || fail "$BIN not built (run: make)"
[ -x "$PLAY" ] || fail "$PLAY not built (run: make player)"
mkdir -p "$WORK"

# Its own fixture, at its own size: this check compares pixels, so it must not
# break the day a scene in the library is re-authored at another resolution.
cat > "$WORK/check.scn" <<'SCN'
# fixture for tools/check-player.sh
name check
size 320 192
frames 12
fps 30
qp 20
bg luma=36
grad dir=v from=20 to=90
rect x=16 y=24 w=64 h=48 luma=200 cb=90 cr=170 vx=2 vy=1 bounce=1
circle x=180 y=100 r=28 luma=120 cb=200 cr=60 vx=-1.5 vy=0.7 bounce=1
stamp x=6 y=6 scale=3
SCN

echo "1. colour conversion"
"$BIN" encode "$WORK/check.scn" "$WORK/c.fdv" >/dev/null || fail "encode"
"$BIN" decode "$WORK/c.fdv" "$WORK/c.yuv" >/dev/null || fail "decode"
"$PLAY" "$WORK/c.fdv" --dump 6 "$WORK/dump.ppm" >/dev/null || fail "--dump"
python3 - "$WORK" <<'PY' || exit 1
import sys
W = sys.argv[1]
w, h, f, fs = 320, 192, 6, 320*192*3//2
d = open(f"{W}/c.yuv", "rb").read()[f*fs:(f+1)*fs]
Y, U, V = d[:w*h], d[w*h:w*h+(w//2)*(h//2)], d[w*h+(w//2)*(h//2):]
raw = open(f"{W}/dump.ppm", "rb").read()
got = raw[raw.index(b"255\n")+4:]
clamp = lambda v: 0 if v < 0 else (255 if v > 255 else v)
bad = 0
for j in range(h):
    for x in range(w):
        c = Y[j*w+x]; u = U[(j//2)*(w//2)+(x//2)]-128; v = V[(j//2)*(w//2)+(x//2)]-128
        exp = (clamp(c+((359*v)>>8)), clamp(c-((88*u+183*v)>>8)), clamp(c+((454*u)>>8)))
        o = (j*w+x)*3
        if (got[o], got[o+1], got[o+2]) != exp: bad += 1
print(f"   {w*h} pixels, {bad} differ from the reference")
raise SystemExit(1 if bad else 0)
PY

echo "2. GPU path"
"$PLAY" "$WORK/c.fdv" --grab 6 "$WORK/grab.ppm" >/dev/null || fail "--grab"
python3 - "$WORK" <<'PY' || exit 1
import sys
W = sys.argv[1]
def rd(p):
    d = open(p, "rb").read(); i = d.index(b"255\n")+4
    hdr = d[:i].split(); return int(hdr[1]), int(hdr[2]), d[i:]
gw, gh, g = rd(f"{W}/grab.ppm")
dw, dh, d = rd(f"{W}/dump.ppm")
if gw % dw or gh % dh:
    print(f"   grabbed {gw}x{gh}, expected a whole multiple of {dw}x{dh}")
    raise SystemExit(1)
k = gw // dw                      # a Retina framebuffer captures at 2x
bad = sum(1 for y in range(dh) for x in range(dw)
          if abs(g[((y*k+k//2)*gw + x*k+k//2)*3] - d[(y*dw+x)*3]) > 2)
print(f"   drawn frame read back at {gw}x{gh} ({k}x), {bad} of {dw*dh} pixels differ")
raise SystemExit(1 if bad else 0)
PY

echo "3. playback loop"
# Wait for the first frame before timing anything. Opening a window is not
# instant, and on a loaded machine it can take seconds -- measuring from launch
# would report a rate for a player that had not started yet.
: > "$WORK/frames.log"
"$PLAY" "$WORK/c.fdv" --log-frames >/dev/null 2>"$WORK/frames.log" &
PID=$!
for _ in $(seq 1 100); do
    [ -s "$WORK/frames.log" ] && break
    sleep 0.1
done
if [ ! -s "$WORK/frames.log" ]; then
    kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    fail "the player produced no frames in 10s (no display?)"
fi
sleep 2.5
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
python3 - "$WORK" <<'PY' || exit 1
import sys
W = sys.argv[1]
rows = [l.split() for l in open(f"{W}/frames.log") if l.startswith("t=")]
if len(rows) < 30:
    print(f"   only {len(rows)} frames displayed — the loop is not advancing")
    raise SystemExit(1)
ts = [float(a.split("=")[1]) for a, _ in rows]
fr = [int(b.split("=")[1]) for _, b in rows]
# Drop the first half-second: window creation and the first texture upload are
# not representative of steady-state pacing.
warm = next((i for i, t in enumerate(ts) if t - ts[0] > 0.5), 0)
ts, fr = ts[warm:], fr[warm:]
if len(fr) < 20:
    print(f"   only {len(fr)} frames after warm-up — the loop is not advancing")
    raise SystemExit(1)
span = ts[-1] - ts[0]
fps = (len(fr) - 1) / span
# every step is +1, or a wrap back to 0 at the end of the clip
nf = max(fr) + 1
bad = [i for i in range(1, len(fr))
       if not (fr[i] == fr[i-1] + 1 or (fr[i] == 0 and fr[i-1] == nf - 1))]
print(f"   {len(fr)} frames in {span:.2f}s = {fps:.1f} fps (want ~30), "
      f"{len(bad)} out-of-order, wraps at {nf}")
raise SystemExit(1 if (bad or not 25.0 <= fps <= 35.0) else 0)
PY

echo "4. window placement"
# A window can be correct in every pixel and still open half off the screen.
# Check the geometry it reports for a small clip and for one larger than the
# display -- the second is where resizing without repositioning goes wrong.
"$BIN" encode "$WORK/check.scn" "$WORK/big.fdv" -n 3 -s 1080p >/dev/null || fail "encode 1080p"
for clip in "$WORK/c.fdv" "$WORK/big.fdv"; do
    "$PLAY" "$clip" >"$WORK/pos.log" 2>&1 &
    PID=$!
    sleep 1.2
    kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    python3 tools/_checkpos.py "$WORK/pos.log" "$clip" || exit 1
done

rm -rf "$WORK"
echo "fpl: colour, GPU readback, playback and placement all check out"
