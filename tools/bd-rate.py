import subprocess, sys, os, re, math
import numpy as np
S, SIZE, N = sys.argv[1], sys.argv[2], int(sys.argv[3])
def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True)
def psnr(a, b):
    r = sh(f'ffmpeg -y -r 30 -i {a} -r 30 -i {b} -lavfi psnr -f null -')
    m = re.search(r'PSNR y:([0-9.]+)', r.stderr + r.stdout)
    return float(m.group(1)) if m else None
scenes = [l.split()[0] for l in sh('./build/fdv scenes').stdout.split('\n')
          if re.match(r'^  [a-z]\S*\s+\d+x\d+', l)]
print(f'  {"scene":<10} {"fdv extra bitrate for equal quality":>36}')
rows=[]
for sc in scenes:
    ref=f'{S}/b.y4m'
    if sh(f'./build/fdv gen {sc} {ref} {N} -s {SIZE}').returncode: continue
    F=[]; X=[]
    for qp in (16,22,28,34):
        sh(f'./build/fdv encode {ref} {S}/b.fdv -q {qp} -k 60')
        sh(f'./build/fdv decode {S}/b.fdv {S}/bf.y4m')
        p=psnr(f'{S}/bf.y4m',ref); sz=os.path.getsize(f'{S}/b.fdv')
        if p: F.append((sz*8*30/N/1000, p))
        sh(f'rm -f {S}/bf.y4m')
    for crf in (16,22,28,34):
        sh(f'ffmpeg -y -r 30 -i {ref} -c:v libx264 -preset medium -crf {crf} -g 60 -bf 0 -pix_fmt yuv420p {S}/b.mp4')
        sh(f'ffmpeg -y -i {S}/b.mp4 -pix_fmt yuv420p {S}/bx.y4m')
        p=psnr(f'{S}/bx.y4m',ref); sz=os.path.getsize(f'{S}/b.mp4')
        if p: X.append((sz*8*30/N/1000, p))
        sh(f'rm -f {S}/bx.y4m')
    sh(f'rm -f {ref} {S}/b.fdv {S}/b.mp4')
    if len(F)<4 or len(X)<4: continue
    # BD-rate: integrate log10(rate) over the overlapping PSNR range
    def fit(pts):
        r=np.log10([p[0] for p in pts]); d=[p[1] for p in pts]
        return np.polyfit(d,r,3), min(d), max(d)
    pf,lf,hf=fit(F); px,lx,hx=fit(X)
    lo,hi=max(lf,lx),min(hf,hx)
    if hi<=lo: continue
    iF=np.polyval(np.polyint(pf),[lo,hi]); iX=np.polyval(np.polyint(px),[lo,hi])
    bd=(10**(((iF[1]-iF[0])-(iX[1]-iX[0]))/(hi-lo))-1)*100
    rows.append((sc,bd))
    print(f'  {sc:<10} {bd:+34.0f}%')
print()
rows.sort(key=lambda r:-r[1])
print(f'  median {np.median([r[1] for r in rows]):+.0f}%,  worst {rows[0][0]} {rows[0][1]:+.0f}%,  best {rows[-1][0]} {rows[-1][1]:+.0f}%')
