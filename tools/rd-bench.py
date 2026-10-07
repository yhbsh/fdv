"""Rate/quality sweep over the scene library, and BD-rate between two runs of it.

The codec is measured against *itself* before and after a change, so this needs
no reference encoder: `run` writes a CSV of (scene, qp, kbps, psnr) and `cmp`
integrates two of them into a per-scene BD-rate.

`-k` picks what is being measured.  The default, `-k 1`, codes every frame as a
key frame, which isolates the intra codec; anything larger exercises the inter
path as well, and `-k 60` is the two-second interval a real stream runs at.
Compare like with like -- a run at one keyint tells you nothing about a run at
another.

    python3 tools/rd-bench.py run before.csv [-n FRAMES] [-s WxH] [-k KEYINT]
    python3 tools/rd-bench.py cmp before.csv after.csv
"""
import subprocess, sys, os, re, argparse, tempfile
import numpy as np

QPS = (16, 22, 28, 34)
BIN = os.environ.get('FDV_BIN', './build/fdv')
FPS = 30


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True)


def scenes():
    out = sh(f'{BIN} scenes').stdout
    return [l.split()[0] for l in out.split('\n')
            if re.match(r'^  [a-z]\S*\s+\d+x\d+', l)]


def run(csv_path, nframes, size, keyint):
    w, h = (int(x) for x in size.split('x'))
    work = tempfile.mkdtemp(prefix='intrabench-')
    # encode reads the .y4m; compare wants the same frames as raw I420.
    y4m, src = f'{work}/s.y4m', f'{work}/s.yuv'
    bs, dec = f'{work}/s.fdv', f'{work}/d.yuv'
    rows = []
    print(f'{"scene":<10} {"qp":>3} {"kbps":>9} {"PSNR Y":>8}')
    for sc in scenes():
        if (sh(f'{BIN} gen {sc} {y4m} {nframes} -s {size}').returncode or
                sh(f'{BIN} gen {sc} {src} {nframes} -s {size}').returncode):
            continue
        for qp in QPS:
            if sh(f'{BIN} encode {y4m} {bs} -q {qp} -k {keyint}').returncode:
                continue
            sh(f'{BIN} decode {bs} {dec}')
            m = re.search(r'PSNR Y\s+([0-9.]+)',
                          sh(f'{BIN} compare {src} {dec} {w} {h} {nframes}').stdout)
            if not m:
                continue
            kbps = os.path.getsize(bs) * 8 * FPS / nframes / 1000
            rows.append((sc, qp, kbps, float(m.group(1))))
            print(f'{sc:<10} {qp:>3} {kbps:>9.1f} {float(m.group(1)):>8.2f}')
    with open(csv_path, 'w') as f:
        f.write('scene,qp,kbps,psnr_y\n')
        for r in rows:
            f.write(f'{r[0]},{r[1]},{r[2]:.3f},{r[3]:.4f}\n')
    sh(f'rm -rf {work}')
    print(f'\nwrote {csv_path} ({len(rows)} points)')


def load(path):
    d = {}
    for line in open(path).read().strip().split('\n')[1:]:
        sc, qp, kbps, psnr = line.split(',')
        d.setdefault(sc, []).append((float(kbps), float(psnr)))
    return d


def frontier(pts):
    """Drop dominated operating points, keeping the achievable R-D frontier.

    This codec's intra rate is not monotonic in QP -- on smooth scenes a lower
    QP can cost fewer bits *and* give better quality, so some QPs are points no
    encoder should ever choose.  Fitting a curve through them produces nonsense
    (BD-rates of several hundred percent that flip sign on a rerun), so the
    dominated points are removed first: keep a point only when no other point
    has both a lower rate and a higher PSNR.
    """
    out, best = [], float('-inf')
    for r, d in sorted(pts):          # by increasing rate
        if d > best:                  # ...only if it buys quality over every cheaper point
            out.append((r, d))
            best = d
    return out


def bd_rate(ref, new):
    """Bjontegaard delta rate: negative means `new` needs fewer bits."""
    ref, new = frontier(ref), frontier(new)
    if len(ref) < 3 or len(new) < 3:
        return None
    def fit(pts):
        r = np.log10([p[0] for p in pts])
        d = [p[1] for p in pts]
        deg = min(3, len(pts) - 1)
        return np.polyfit(d, r, deg), min(d), max(d)
    pr, lr, hr = fit(ref)
    pn, ln, hn = fit(new)
    lo, hi = max(lr, ln), min(hr, hn)
    if hi <= lo:
        return None
    ir = np.polyval(np.polyint(pr), [lo, hi])
    inw = np.polyval(np.polyint(pn), [lo, hi])
    return (10 ** (((inw[1] - inw[0]) - (ir[1] - ir[0])) / (hi - lo)) - 1) * 100


def cmp_(a_path, b_path):
    A, B = load(a_path), load(b_path)
    print(f'{"scene":<10} {"BD-rate":>9}   (negative = after is smaller for equal quality)')
    rows = []
    for sc in sorted(set(A) & set(B)):
        if len(A[sc]) < 3 or len(B[sc]) < 3:
            continue
        bd = bd_rate(A[sc], B[sc])
        if bd is None:
            continue
        rows.append((sc, bd))
        print(f'{sc:<10} {bd:>+8.2f}%')
    if not rows:
        print('no comparable scenes')
        return
    v = [r[1] for r in rows]
    rows.sort(key=lambda r: r[1])
    print(f'\nmean {np.mean(v):+.2f}%,  median {np.median(v):+.2f}%')
    print(f'best  {rows[0][0]} {rows[0][1]:+.2f}%')
    print(f'worst {rows[-1][0]} {rows[-1][1]:+.2f}%')
    print(f'improved {sum(1 for x in v if x < 0)}/{len(v)}, '
          f'regressed {sum(1 for x in v if x > 0)}/{len(v)}')


p = argparse.ArgumentParser()
sub = p.add_subparsers(dest='cmd', required=True)
r = sub.add_parser('run'); r.add_argument('csv')
r.add_argument('-n', type=int, default=8); r.add_argument('-s', default='1280x720')
r.add_argument('-k', type=int, default=1, help='keyint; 1 = all-intra (default)')
c = sub.add_parser('cmp'); c.add_argument('before'); c.add_argument('after')
a = p.parse_args()
run(a.csv, a.n, a.s, a.k) if a.cmd == 'run' else cmp_(a.before, a.after)
