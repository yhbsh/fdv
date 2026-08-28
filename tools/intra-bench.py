"""All-intra rate/quality sweep, and BD-rate between two runs of it.

The intra path is measured against *itself* before and after a change, so this
needs no reference encoder: `run` writes a CSV of (scene, qp, kbps, psnr) and
`cmp` integrates two of them into a per-scene BD-rate.  Every frame is coded as
a key frame (keyint=1), which is what isolates the intra codec.

    python3 tools/intra-bench.py run before.csv [-n FRAMES] [-s WxH]
    python3 tools/intra-bench.py cmp before.csv after.csv
"""
import subprocess, sys, os, re, argparse, tempfile
import numpy as np

QPS = (16, 22, 28, 34)
FPS = 30


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True)


def scenes():
    out = sh('./build/fdv scenes').stdout
    return [l.split()[0] for l in out.split('\n')
            if re.match(r'^  [a-z]\S*\s+\d+x\d+', l)]


def run(csv_path, nframes, size):
    w, h = (int(x) for x in size.split('x'))
    work = tempfile.mkdtemp(prefix='intrabench-')
    src, bs, dec = f'{work}/s.yuv', f'{work}/s.fdv', f'{work}/d.yuv'
    rows = []
    print(f'{"scene":<10} {"qp":>3} {"kbps":>9} {"PSNR Y":>8}')
    for sc in scenes():
        if sh(f'./build/fdv gen {sc} {src} {nframes} -s {size}').returncode:
            continue
        for qp in QPS:
            # keyint=1 -> every frame intra, so this measures the intra codec alone
            if sh(f'./build/fdv enc {src} {w} {h} {nframes} {qp} {bs} 1').returncode:
                continue
            sh(f'./build/fdv dec {bs} {dec}')
            m = re.search(r'PSNR Y\s+([0-9.]+)',
                          sh(f'./build/fdv compare {src} {dec} {w} {h} {nframes}').stdout)
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


def bd_rate(ref, new):
    """Bjontegaard delta rate: negative means `new` needs fewer bits."""
    def fit(pts):
        r = np.log10([p[0] for p in pts])
        d = [p[1] for p in pts]
        return np.polyfit(d, r, 3), min(d), max(d)
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
        if len(A[sc]) < 4 or len(B[sc]) < 4:
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
c = sub.add_parser('cmp'); c.add_argument('before'); c.add_argument('after')
a = p.parse_args()
run(a.csv, a.n, a.s) if a.cmd == 'run' else cmp_(a.before, a.after)
