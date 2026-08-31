"""Quality at a fixed low-latency operating point, against x264.

The scene sweep in rd-bench.py integrates over a QP range, which weights
near-lossless coding heavily and says little about a stream that has to fit a
wire. This measures the thing a live encoder is actually asked for: one bitrate,
one resolution, one frame rate, no lookahead and no frame reordering on either
side.

    python3 tools/live-bench.py [-b BPS] [-s WxH] [-n FRAMES] [--fps N]
"""
import subprocess, sys, os, re, argparse, tempfile
import numpy as np

BIN = os.environ.get('FDV_BIN', './build/fdv')

def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True)

def scenes():
    out = sh(f'{BIN} scenes').stdout
    return [l.split()[0] for l in out.split('\n')
            if re.match(r'^  [a-z]\S*\s+\d+x\d+', l)]

def psnr(a, b):
    r = sh(f'ffmpeg -y -r 30 -i {a} -r 30 -i {b} -lavfi psnr -f null -')
    m = re.search(r'PSNR y:([0-9.]+)', r.stderr + r.stdout)
    return float(m.group(1)) if m else None

def main(bps, size, n, fps):
    w, h = (int(x) for x in size.split('x'))
    work = tempfile.mkdtemp(prefix='live-')
    src, ref = f'{work}/s.y4m', f'{work}/r.y4m'
    gop = 2 * fps                     # two seconds, the usual streaming compromise
    print(f'{bps/1e6:.2f} Mbps, {size}@{fps}, keyint {gop}, no B-frames either side\n')
    print(f'{"scene":<10} {"fdv kbps":>9} {"fdv dB":>8} {"x264 kbps":>10} {"x264 dB":>8} {"delta":>7}')
    rows = []
    for sc in scenes():
        if sh(f'{BIN} gen {sc} {src} {n} -s {size}').returncode:
            continue
        # fdv: average-bitrate control, one-second buffer, no reordering
        if sh(f'{BIN} encode {src} {work}/f.fdv -b {bps} -k {gop}').returncode:
            continue
        sh(f'{BIN} decode {work}/f.fdv {work}/fd.y4m')
        fk = os.path.getsize(f'{work}/f.fdv') * 8 * fps / n / 1000
        fp = psnr(f'{work}/fd.y4m', src)
        # x264: zerolatency turns off lookahead and B-frames, which is the
        # same constraint fdv is under.
        sh(f'ffmpeg -y -i {src} -c:v libx264 -preset veryfast -tune zerolatency '
           f'-b:v {bps} -maxrate {bps} -bufsize {bps} -g {gop} -bf 0 '
           f'-pix_fmt yuv420p {work}/x.mp4')
        sh(f'ffmpeg -y -i {work}/x.mp4 -pix_fmt yuv420p {work}/xd.y4m')
        xk = os.path.getsize(f'{work}/x.mp4') * 8 * fps / n / 1000
        xp = psnr(f'{work}/xd.y4m', src)
        for f in ('fd.y4m', 'xd.y4m', 'x.mp4', 'f.fdv'):
            sh(f'rm -f {work}/{f}')
        if fp is None or xp is None:
            continue
        rows.append((sc, fk, fp, xk, xp, fp - xp))
        print(f'{sc:<10} {fk:>9.0f} {fp:>8.2f} {xk:>10.0f} {xp:>8.2f} {fp-xp:>+7.2f}')
    sh(f'rm -rf {work}')
    if rows:
        d = [r[5] for r in rows]
        rows.sort(key=lambda r: r[5])
        print(f'\nmean {np.mean(d):+.2f} dB, median {np.median(d):+.2f} dB '
              f'(positive = fdv is better at the same target)')
        print(f'worst {rows[0][0]} {rows[0][5]:+.2f} dB, best {rows[-1][0]} {rows[-1][5]:+.2f} dB')
        print(f'ahead on {sum(1 for x in d if x > 0)}/{len(d)}')

p = argparse.ArgumentParser()
p.add_argument('-b', type=int, default=1000000)
p.add_argument('-s', default='1280x720')
p.add_argument('-n', type=int, default=90)
p.add_argument('--fps', type=int, default=30)
a = p.parse_args()
main(a.b, a.s, a.n, a.fps)
