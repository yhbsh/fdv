#!/bin/sh
# Compare fdv against x264 across content types, at a fixed bitrate.
#
# The point of this script is the *set*. A single clip is not a benchmark: a
# static talking head is almost all SKIP macroblocks and flatters this codec
# badly -- measured on one, fdv looked within 1.23x of x264; measured across
# these four, the honest range is from beating it to eight decibels behind.
#
# Needs ffmpeg with libx264. Writes into a work directory (default ./build/bench).
set -e
WORK=${WORK:-build/bench}
KBPS=${KBPS:-1200}
BANDS=${BANDS:-1}            # 1 = single stream, the fair comparison with x264
SIZE=${SIZE:-960x544}        # must be a multiple of 16 in both directions
SECS=${SECS:-12}
FDV=${FDV:-./build/fdv}
mkdir -p "$WORK"

W=${SIZE%x*}; H=${SIZE#*x}
FRAMES=$((SECS * 30))

say() { printf '%s\n' "$*"; }

# --- the content ----------------------------------------------------------
# swarm and stress come from the scene library, rendered losslessly: encoding a
# clip through fdv to make a reference would feed the codec its own artifacts.
if [ ! -f "$WORK/swarm.y4m" ]; then
    say "rendering scenes..."
    $FDV gen swarm  "$WORK/swarm.y4m"  $FRAMES -s $SIZE >/dev/null
    $FDV gen stress "$WORK/stress.y4m" $FRAMES -s $SIZE >/dev/null
fi
# A zooming fractal: fine detail everywhere and continuous motion, which is as
# hard as synthetic content gets.
if [ ! -f "$WORK/mandel.y4m" ]; then
    say "rendering mandelbrot..."
    ffmpeg -y -f lavfi -i "mandelbrot=size=$SIZE:rate=30" -t $SECS \
           -pix_fmt yuv420p "$WORK/mandel.y4m" 2>/dev/null
fi
# Real camera texture with real motion, if a capture is supplied. Crops 1:1 --
# never upscale to make a pan, it blurs away the detail that makes content hard
# and both codecs then score identically on nothing.
if [ -n "$CAMERA" ] && [ ! -f "$WORK/pan.y4m" ]; then
    say "panning over $CAMERA..."
    ffmpeg -y -r 30 -i "$CAMERA" -vf \
      "crop=$W:$H:x='if(lt(n,90),160,if(lt(n,180),160+(n-90)*1.6,if(lt(n,270),304+(n-180)*3.5,160)))':y='if(lt(n,270),88,88+56*sin((n-270)/9))'" \
      -pix_fmt yuv420p "$WORK/pan.y4m" 2>/dev/null
fi

metric() {   # metric <decoded.y4m> <reference.y4m> <psnr|ssim>
    if [ "$3" = psnr ]; then
        ffmpeg -y -r 30 -i "$1" -r 30 -i "$2" -lavfi psnr -f null - 2>&1 |
            grep -o 'PSNR y:[0-9.]*' | cut -d: -f2
    else
        ffmpeg -y -r 30 -i "$1" -r 30 -i "$2" -lavfi ssim -f null - 2>&1 |
            grep -o 'All:[0-9.]*' | head -1 | cut -d: -f2
    fi
}

say ""
say "$KBPS kbps, $SIZE, ${SECS}s, fdv with $BANDS band(s)"
say ""
printf '  %-8s %-6s %9s %9s %8s\n' clip codec kbps PSNR SSIM
for clip in swarm stress mandel pan; do
    ref="$WORK/$clip.y4m"
    [ -f "$ref" ] || continue

    ffmpeg -y -r 30 -i "$ref" -c:v libx264 -preset medium -b:v ${KBPS}k \
           -g 60 -bf 0 -pix_fmt yuv420p "$WORK/x.mp4" 2>/dev/null
    ffmpeg -y -i "$WORK/x.mp4" -pix_fmt yuv420p "$WORK/x.y4m" 2>/dev/null
    xk=$(( $(wc -c < "$WORK/x.mp4") * 8 / SECS / 1000 ))
    xp=$(metric "$WORK/x.y4m" "$ref" psnr); xs=$(metric "$WORK/x.y4m" "$ref" ssim)
    rm -f "$WORK/x.y4m"
    printf '  %-8s %-6s %9s %9.2f %8.4f\n' "$clip" x264 "$xk" "$xp" "$xs"

    $FDV encode "$ref" "$WORK/f.fdv" -b ${KBPS}k -k 60 \
        ${BANDS:+$( [ "$BANDS" -gt 1 ] && echo "-t $(( (H/16 + BANDS - 1) / BANDS )) -j $BANDS" )} \
        >/dev/null 2>&1 || true
    if [ -f "$WORK/f.fdv" ]; then
        $FDV decode "$WORK/f.fdv" "$WORK/f.y4m" >/dev/null 2>&1
        fk=$(( $(wc -c < "$WORK/f.fdv") * 8 / SECS / 1000 ))
        fp=$(metric "$WORK/f.y4m" "$ref" psnr); fs=$(metric "$WORK/f.y4m" "$ref" ssim)
        rm -f "$WORK/f.y4m"
        printf '  %-8s %-6s %9s %9.2f %8.4f\n' "" fdv "$fk" "$fp" "$fs"
    fi
done
say ""
say "reference clips kept in $WORK (delete to re-render)"
