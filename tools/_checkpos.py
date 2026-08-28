"""Check that fpl's window lands wholly inside the display."""
import re, sys

line = ""
for l in open(sys.argv[1]):
    if "display" in l and "window" in l:
        line = l.strip()
if not line:
    print(f"   {sys.argv[2]}: the player reported no window geometry")
    raise SystemExit(1)

m = re.search(r"display (\d+)x(\d+), window (\d+)x(\d+) \([^)]*\) at (-?\d+),(-?\d+)", line)
if not m:
    print(f"   unparseable: {line}")
    raise SystemExit(1)

mw, mh, ww, wh, x, y = map(int, m.groups())
bad = []
if x < 0 or y < 0:             bad.append("origin off the top/left")
if x + ww > mw or y + wh > mh: bad.append("extends past the display")
if ww > mw or wh > mh:         bad.append("larger than the display")
print(f"   {ww}x{wh} at {x},{y} on {mw}x{mh}" +
      ("  <- " + "; ".join(bad) if bad else "  (fits)"))
raise SystemExit(1 if bad else 0)
