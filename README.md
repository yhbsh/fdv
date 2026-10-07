# fdv — Fast Decode Video

A from-scratch experimental video encoder/decoder in C11, no external
dependencies. The organizing principle is **decode speed on modern CPUs**: a
codec is encoded once but decoded billions of times, so every design fork is
resolved in favor of the decoder — table-driven branch-light entropy decoding,
small SIMD-friendly integer transforms, and tiles that decode in parallel.

It grew incrementally from a 16×16 image generator into a full YUV 4:2:0
P-frame codec with a usable command-line tool. See **ARCHITECTURE.md** for the
design rationale.

## Quick start — no `.yuv` file needed

The codec ships with a scene renderer, so a test clip is a description rather
than a download:

```
make
make install                  # -> ~/.local/bin/{fdv,fpl,fcap} + include/fdv.h
fdv scenes                    # the scene library, and what each one stresses
fdv encode motion clip.fdv    # a description in, a coded stream out
fpl clip.fdv                  # watch it
```

`make install` puts `fdv`, `fpl` and `fcap` in `$(PREFIX)/bin`, the headers in
`$(PREFIX)/include` and the scenes in `$(PREFIX)/share/fdv/scenes`, `PREFIX`
defaulting to `~/.local`; it warns if that is not on your `PATH`. `make
uninstall` removes them. `make play SCENE=stress` encodes a scene and opens it
in one step.

`fdv pipeline motion` runs the whole render/encode/decode/verify chain, prints
every stage with its numbers and leaves the artifacts in `build/pipeline/`:
`source.y4m` and `decoded.y4m` (both open in ffplay/mpv), plus `stream.bin`. To write your own clip, start from an existing scene and edit it:

```
./build/fdv scene motion > my.scn
./build/fdv pipeline my.scn -q 24 -v
```

## Build & test

```
make            # build the CLI, the tests and the player into build/
make install    # install fdv, fpl, fdv.h and the scenes (override PREFIX=)
make uninstall  # remove them again
make player     # build build/fpl (needs raylib)
make play       # encode a scene and open it  (SCENE=stress)
make check-player  # colour / GPU / playback checks (needs a display)
make test       # unit-test suite (14 sections) + the decoder fuzz harness
make pipeline   # end-to-end run   (SCENE=stress ARGS='-v')
make scenes     # list the scene files
make fuzz       # decoder robustness fuzz under ASan + UBSan
make tsan       # data-race check over the tile-parallel encode/decode paths
make profile    # build/fdv-profile, with the in-codec zone profiler
make trace      # build/fdv-trace, with per-macroblock logging
make demo       # CLI self-test (synthesize, encode, decode, report PSNR/size)
make demo-y4m   # round-trip a synthesized Y4M clip through the CLI
make demo-vtile # round-trip through the tile-parallel video path (threaded decode)
make bench      # encode/decode throughput (Mpix/s + fps)
make help       # the above, from the Makefile
```

Each program is compiled to a `.o` and linked in a separate step, with `-g` in
`CFLAGS` and never in `LDFLAGS`: on macOS a single compile-and-link with `-g`
runs `dsymutil` and leaves `.dSYM` bundles behind. Debug info stays in the `.o`
files, and lldb follows the linker's debug map to it.

On an arm64 box, single-stream at 720p, one number will not do: `plaza` costs
**29.1 ms** a frame to encode and **2.57 ms** to decode, `motion` **8.1** and
**0.69**. What a frame costs depends almost entirely on how much of it is SKIP,
so see **Speed** below for the table rather than a mean.

The synthetic scenes are 15-20x easier than camera input and are a regression
check, not a performance claim. On real 720p camera content: encode **45.8
ms/frame single-threaded, 7.8 ms on 8 bands**; decode **0.69 ms/frame**. See
**Recording from a camera** below.

Builds clean under `clang -std=c11 -O2 -Wall -Wextra -Wshadow -pthread`.

## Streaming at a fixed bitrate

`-b` replaces the fixed quantizer with average-bitrate control:

```
fcap out.fdv -b 1000k -j 8       # 1 Mbps, 720p30
fdv encode motion clip.fdv -b 1500k
```

Measured on 12 s of steady 720p30 camera footage, it lands within about 1% of
target and stays there:

| target | achieved | PSNR |
|--------|----------|------|
|  500 kbps |  507 kbps (+1.4%) | 43.87 dB |
| 1000 kbps | 1006 kbps (+0.6%) | 46.89 dB |
| 2000 kbps | 2000 kbps (+0.0%) | 49.02 dB |

### How it decides

There is no lookahead and no frame reordering, because both buy compression with
latency and this is meant to go down a wire. The loop only ever sees frames it
has already encoded.

**Bits are allocated over a key-frame interval, not per frame.** The interval's
budget is split between its one key frame and its P-frames, with the key frame
counted as ten of them. The per-frame version of this -- "give a key frame five
frames' worth" -- makes its allowance depend on how full the buffer happens to be
when it lands, and the first frame of a stream came out at **QP 41** as a result:
every P-frame after it then spent bits repairing a bad reference, which filled
the buffer, which pushed QP to 38. A death spiral out of one bad allocation.

**Correction is plain feedback, with no model of bits against QP.** The obvious
design estimates frame complexity as `bits * qstep^k` and inverts it. That
relation is not a fixed power law -- measured between 1.0 and 1.34 over QP 16..32
on the same content -- and whenever `k` guesses high the estimate *rises* with
QP, which asks for more QP, which raises it again. It ran away to QP 39 and sat
there. Feedback with a gain below what the relation needs cannot do that,
whatever the content, so that is what it does: one QP step per doubling of the
bit error, capped at one step per frame.

**The loop is deliberately slow.** Constant QP is very nearly RD-optimal on
stationary content, so the controller's job is to hit an average while
disturbing QP as little as it can. Faster loops hit the bitrate just as
accurately and look worse doing it -- and cost real quality, because an
over-quantized frame goes into the reference pool and every frame after it pays
to repair it. Measured at 1 Mbps, four loop speeds:

| gain / step / drain | achieved | PSNR | QP range |
|---------------------|----------|------|----------|
| 2.0 / 2 / 2s | 1019 kbps | 46.03 dB | 14..27 |
| 1.0 / 1 / 2s | 1010 kbps | 45.96 dB | 15..24 |
| 0.5 / 1 / 8s | 1079 kbps | 46.23 dB | 15..24 |
| 0.3 / 1 / 16s | 1099 kbps | 46.26 dB | 15..24 |

The slowest loops score best and miss the target by 10%, which is the wrong
trade when the point is not exceeding a link. The default is the second row.

**Every frame is held to its allocation.** The one-step-per-frame loop is right
for steady state and useless when a frame is wrong by a factor of sixty, which
the first P-frame of `grain` once was (261 KB against a 4 KB budget). A frame
landing more than 1.5x over is coded again, coarser, up to three times; nothing
has to be unwound because the reference pool is updated only afterwards, and
the quantizer it ends at becomes the operating point so the next frame does not
repeat the same guess. The ceiling is QP 51, the coarsest the tables define.
Key frames sit at the P operating point minus an offset rather than running
their own loop, which at a two-second interval would see one sample for every
sixty the P loop gets.

**The operating point is a real number, carried by lambda.** On noise-like
content rate is not a gentle function of QP -- 720p `grain` is 22.2 Mbps at QP
32, 1.80 at 34 and 114 kbps at 36, so no quantizer produces 1 Mbps and an
integer loop dithers across the gap and lands at half the target. The integer
part of the operating point picks the quantizer the format carries; the
fraction scales lambda toward the next one (1.15x per step, measured: at 1.40
lambda pushes past `grain`'s own cliff), which makes RDOQ zero more
coefficients without coarsening the ones that survive. The decoder is not told
and does not need to be, and fixed-QP output is bit-identical. `grain` at
1 Mbps went from 522 kbps to 922.

What holding a bitrate costs, against a fixed QP chosen to land on the same
rate: **+0.40 dB mean, +1.35 dB worst.** The rest is coding efficiency. Easy
content landing far under target (`pan` asks for 1200 kbps and uses 388, at
67 dB) is correct: there is nothing left to buy.

The buffer is what bounds latency -- it is the decoder-side buffer the stream
implies, so its size *is* the buffering the stream demands. One second by
default.

`fdv info` reads the QP back out of the frames rather than reporting the
header's, which under rate control is only whatever the encoder opened with.

### Key frames

`-k` sets the interval, on `fcap` and on `fdv encode`. It takes either a frame
count or a duration:

```
fcap out.fdv -k 2s      # every two seconds, whatever the frame rate
fcap out.fdv -k 90      # exactly 90 frames
fcap out.fdv -k 0       # one key frame, at the start, and no more
```

Seconds are the useful unit and the default is `2s`: the interval bounds how
long a viewer joining a stream waits for a decodable frame, and a frame count
silently halves it when the frame rate doubles. The header stores the interval
in a byte, so it caps at 255 frames; `fcap` says so rather than quietly
clamping. At a fixed 1 Mbps:

| keyint | I-frame share | PSNR |
|--------|---------------|------|
|  30 (1s) | 26% | 45.53 dB |
|  60 (2s) | 13% | 45.96 dB |
| 120 (4s) |  7% | 46.07 dB |
| 250 (8s) |  4% | 46.17 dB |

Two seconds is the usual streaming compromise, and where most of the gain
already is.

## Compression

### Against x264

The measure is **BD-rate**: how much more bitrate fdv needs for the same
quality, integrated over a four-point sweep of each codec's own quality control.
`tools/bd-rate.py` runs it across the whole scene library, rendered losslessly by
`fdv gen`:

```
python3 tools/bd-rate.py <workdir> 960x544 90
```

Against x264 preset medium with no B-frames:

| scene | fdv needs | | scene | fdv needs |
|-------|-----------|-|-------|-----------|
| `stress`   |  **-88%** | | `still`    |  +1% |
| `chroma`   |  **-32%** | | `pan`      |  +1% |
| `plaza`    |  **-28%** | | `cut`      |  +5% |
| `veil`     |  **-23%** | | `detail`   | +10% |
| `grain`    |  **-20%** | | `rain`     | +19% |
| `divergent`|  **-17%** | | `strobe`   | +21% |
| `spin`     |  **-17%** | | `tiny`     | +22% |
| `skyline`  |  **-11%** | | `vista`    | +29% |
| `confetti` |   **-4%** | | `motion`   | +32% |
| | | | `swarm`    | +38% |
| | | | `churn`    | +46% |
| | | | `valley`   | +50% |
| | | | `mosaic`   | +62% |
| | | | `wipe`    | +114% |

**Mean +9%, median +5%** across twenty-three scenes, and fdv is *ahead* of x264
on nine of them. It is far ahead on the densest, most expensive content and
behind on hard-edged synthetic content. An aggregate is only comparable against
the *same* scene set and clip length; the per-scene numbers are what carry
across. (An earlier version of this README quoted 1.23x of x264 from one clip of
a person sitting still in front of a webcam, over 80% SKIP. Never quote one
clip.)

At the live operating point -- 720p30, 1 Mbps, two-second key frames,
`-preset veryfast -tune zerolatency -bf 0` on both sides -- `tools/live-bench.py`
measures fdv **+7.4 dB mean, +3.3 dB median**, ahead on 23 of 23 scenes.
`tools/bench-x264.sh` makes the same fixed-bitrate comparison over rendered,
fractal and (with `CAMERA=`) real footage, and reports SSIM as well.

To measure a change against the codec itself rather than against x264,
`tools/rd-bench.py run` writes a rate/quality sweep to a CSV and
`tools/rd-bench.py cmp before.csv after.csv` turns two of them into per-scene
BD-rate. `-k 1` isolates the intra path; `-k 60` is the video path. Every
per-change figure below was measured that way, against the build immediately
before it -- they are not the codec's current standing, which is the table
above.

### What the bits are spent on, and what each piece was worth

**Intra is a rate-distortion quadtree.** A 64x64 coding tree unit is coded as
one prediction or split, four levels down to 4x4 leaves; 4x4 leaves use the nine
H.264 directions, larger leaves DC, vertical, horizontal and H.264's plane fit.
Before it, intra modes were 62-90% of a smooth key frame: one per 4x4 block, at
an entropy floor of 0.16 bits each, so the win was in the *number* of symbols,
not their cost. Together with the rate model below: **-22.9% BD-rate all-intra,
-21.0% video**. The 32x32 level added -3.6%, 64x64 a further -0.6%, neither
costing encode or decode time. Trial scratch lives in one arena per walk, a set
per level, because stack arrays at 64 needed ~440 KB against a pthread's 512 KB.

**The rate model grows with magnitude.** RD decisions charge an exp-Golomb
length per coefficient and small constants for structure symbols, with lambda
`0.425 * 2^((QP-12)/3)`. The original model charged 8 bits per emitted byte,
which is flat where the true cost grows: once the quadtree gave it large blocks
and 8x8 transforms to choose between, it picked them because they produced
*fewer* bytes, not *cheaper* ones -- **+70% on `stress`**. Measuring the rate
instead, by coding each frame twice, is worth +0.03% mean for twice the encode
time: exp-Golomb is the entropy of a geometric source, and coefficient levels
are close to geometric. The shape mattered; the magnitude does not.

**Symbols are split into streams by what they are.** The rule, learned five
times over: if you can describe two symbol kinds in different sentences, they
want different models. Each split, and what it measured:

| split | worth |
|---|---|
| end-of-block counts from coefficient levels (P-frames) | -5% to -17% bitrate at fixed QP; decode 1.13 to 0.69 ms |
| the same split in the intra codec | -40% of the coefficient stream, -15-30% of a key frame |
| 4x4 from 8x8 transform coefficients | 32 KB on one high-rate intra frame, same symbol count |
| transform-size and coded-block flags out of the counts | -1.6% intra, -1.6% video, no scene worse |
| chroma counts and levels from luma's | -1.0% mean, 19 of 23 scenes |
| reference index, MV x, MV y, intra sub-mode | 9-31% of that stream |

Mode streams are context-coded on neighbours already decoded, so nothing is
transmitted for the context: intra 4x4 modes by whether left and above agree
(nine contexts measured no better than two), macroblock modes by the pair of
neighbours, as H.264 does for its skip flag (`pan` -60% of the mode stream).
Splitting coefficients further, by scan position or block size, measured under
a percent.

**Empty regions say so.** The transform-size flag has a third value for "nothing
coded here" and chroma has a coded-block flag, so an empty block emits no
end-of-block count. At QP 45 on a pan, counts went from 292 bytes a frame to
33. Chroma also has an end-of-block count at all; it used to emit all sixteen
coefficients of every block.

**Frequency tables are cheap or absent.** They are sparse (symbol, varint
frequency) pairs; each stream may reuse the previous frame's table, chosen per
stream by trying both, with one slot per symbol reserved so a symbol the last
frame happened not to use does not disqualify it (that took reuse from firing 1
frame in 88 to 92-100%); empty streams are skipped outright and lengths are
varints. The chain resets at every key frame, so a key frame still decodes
alone.

**Short streams use an adaptive range coder primed from history.** Both sides
derive a model from frames already decoded, then let it adapt to this frame as
it codes, so no model is sent. Neither half is any use alone -- adapting from a
flat prior costs +89% to +140% on short streams, a history table without
adaptation +0.8% to +30.5% -- but together: **-15% to -32%** on cheap scenes.
Three details carried most of it: all adaptive streams share one range-coded
blob (one flush, not ten), the prior is normalized to a fixed total so a longer
history does not cost coder precision, and the model halves past a bound.
Adaptation is serial, so a frame spends a budget of `FDV_AD_BUDGET` (65536)
symbols on its shortest streams first, which is also where a transmitted table
costs most: -1.20% BD-rate, against -0.41% for the per-stream cap it replaced,
which also did not bound decode (lifting it took 1080p `grain` from 21 to 63
ms). Which streams qualify is derived from the symbol counts, not transmitted,
and the frame is coded both ways so this cannot lose. Key frames have no
history, so they adapt from the flat prior; their streams are long enough to
amortize it and it still wins, -0.1% to -11.5%. The decode cost is in *Speed*.

**SKIP names the vector it inherits** (HEVC's merge): the running predictor,
then the macroblocks above and above-right, deduplicated, capped at three, with
their reference index. Index 0 is the old SKIP. **-6.8% mean, 22 of 23
scenes**; `skyline` -29.7%, `chroma` -24.0%.

**Deblocking runs on the 8x8 grid**, not every 4x4 edge: -1.0% all-intra,
-2.2% video, and faster decode. Its strength was swept and is at its best.

### Measured and rejected

Each of these looked like it should help. All were built and measured.

- **H.264's most-probable-mode** for intra: 0.3-1.8% worse. Coding `mode - mpm`
  only reaches the conditional entropy when the conditional distributions are
  translates of one another, and these are not; it needs a model per predictor
  value, which is what the agreement context is.
- **Steering the intra mode decision** toward cheap modes -- charging the mode
  against its prediction, a per-macroblock "all modes agree" flag, a bounded
  tie-break: worse in every variant. The fix had to be structural (the quadtree).
- **A rate term in the motion search** (`lambda_me * bits(mvd)`): averages zero.
  The EPZS seed already starts at the predicted vector.
- **A median MV predictor**: -0.3%. Improving what a delta is measured against
  is worth nothing here; removing the delta (merge) is worth -6.8%.
- **Merge with a residual**: +0.18%.
- **HEVC's intra boundary filter**: +3.6% at 4x4, +0.3% on larger blocks.
- **A wider motion search**: +0.04 dB for +1.4% bitrate.
- **No intra macroblocks in P-frames**: worse on both rate and quality.
- **A hierarchical skip flag / an inter quadtree over macroblocks**: +0.12% with
  one level, +0.37% with two, decode 18-31% slower. An inter mode symbol sits in
  a stream that is 99% SKIP and costs about a fiftieth of a bit; there is
  nothing for a tree to save.
- **Deblocking boundary strength** (skip edges that provably carry no new
  step): +0.12% video, +0.24% intra. On static content the in-loop filter is
  doing cumulative work on blocking every SKIP since the key frame copied
  forward, which a per-frame argument cannot see -- and deblocking is only 5-8%
  of decode, so there is no speed argument either.
- **Gating INTER8 on uneven error across quadrants**: +2.21%. Content where all
  four quadrants move differently spreads the error evenly, and that is exactly
  what the split is for.

### Traps worth not repeating

- **Re-coding a frame must restore the entropy history.** A P-frame folds its
  symbol counts into the history and may store its tables, so a requantize retry
  advances it twice while the decoder advances it once -- a mismatch only on
  scenes that retry.
- **Z-order is not raster order.** Inside a quadtree the above-right cell of a
  lower-left quadrant is coded *after* it; availability has to be derived
  exactly and identically on both sides, or the encoder reads uninitialised
  samples and the decoder stale ones.
- **Price like with like.** A node coder that returns rate but keeps distortion
  inside its own decision makes splitting look free. Pricing a symbol from the
  coded history is circular: leaves win, the few remaining mode symbols look
  expensive, and more leaves win.
- **Route the merge, not just the writes.** A trial that writes into scratch
  streams and then merges them must merge into the same streams the decoder
  reads.

## Recording from a camera (macOS)

```
fcap out.fdv                 # until Ctrl-C
fcap out.fdv -t 10 -q 20     # ten seconds, better quality
fcap out.fdv -s 480p         # smaller, so the encoder keeps up
fcap out.fdv --fps 15        # a grid the encoder can actually fill
fcap out.fdv -j 8            # more encode bands, more cores
fcap out.fdv -b 1000k -j 8   # hold an average bitrate, for streaming
fcap out.fdv -k 4s           # key-frame interval, in seconds or frames
fcap --list                  # which cameras are there
fpl out.fdv                  # watch it
```

`fcap` is the **only** platform-dependent file in the project. `fdv.h` stays
plain C11 with no operating system in it, and the boundary between the two is
an I420 frame: AVFoundation gets frames that far, and from there it is the same
`fdv_enc_*` calls any other program makes. On anything but macOS `make` skips it
and builds everything else.

Two details it does not paper over. The camera delivers **NV12** — 4:2:0 with
the chroma planes interleaved — so it de-interleaves them into the separate
planes the codec wants. And dimensions are **cropped** to a multiple of 16 for
the block grid, not scaled: 1080 is not one, so 1080p records as 1920x1072 and
`fcap` says so.

### Timing: a frame goes where it was taken

The first version of this wrote one average frame rate and encoded whatever
survived back to back. It played badly, and why is worth keeping. When the
encoder falls behind, AVFoundation drops frames in **bursts**, so the gap
between two frames that *do* arrive is one camera period, or two, or three.
Over four seconds at 720p:

| real gap between kept frames | count |
|------------------------------|-------|
| 33 ms (nothing dropped)      | 94 |
| 67 ms (one dropped)          | 18 |
| 100 ms (two dropped)         | 2 |

Replaying all of them at a constant 40 ms runs most frames at 0.84x and snaps
the ones with a dropped neighbour forward at 1.7x to 2.5x. That reads as a
stutter several times a second — which is a different complaint from a low
frame rate, and has a different fix.

So each frame is placed on a fixed grid by its capture timestamp
(`CMSampleBufferGetPresentationTimeStamp`), and a slot nothing arrived for is
filled by **holding** the previous frame. Motion then keeps its real speed even
when the encoder cannot keep up: what degrades is frame rate, not timing.

A hold goes through `fdv_enc_repeat`, which writes an all-SKIP frame directly
instead of handing the same pixels back to mode decision. That distinction turns
out to matter enormously at a low qp. Re-encoding identical pixels on 720p
camera content:

| qp | fresh frame | re-encode identical pixels | `fdv_enc_repeat` |
|----|-------------|----------------------------|------------------|
| 24 | 45 ms | 0.7 ms | 0.27 ms |
| 16 | 54 ms | 3.0 ms | 0.27 ms |
| 10 | 64 ms | **26.5 ms** | 0.27 ms |
| 4  | 68 ms | **29.7 ms** | 0.27 ms |

Below about QP 16 the RD comparison stops choosing SKIP even for a byte-identical
frame: lambda gets small enough that coding the reference's own quantization
noise scores better. So the encoder does nearly a full frame's work on a picture
it already has — and a recorder that holds a slot *to keep time* falls further
behind for doing so, holds more slots, and spirals. `fdv_enc_repeat` is
unconditionally all-SKIP, so a hold costs 0.27 ms at any qp.

`--fps` sets the grid, and the camera is asked for the same rate so it stops
sending frames that would only be converted and thrown away. The summary says
how many slots had to be held:

```
out.fdv: 1280x720, 84 frames in 6.0s (15 fps), 774 kB (1049 kbps)
  encode 38.0 ms/frame, 1 held (1% — 14 fps of new pictures)
```

### The encoder is threaded, and that is what sets the frame rate

Real camera content is far harder than the synthetic scenes: noise and texture
mean very few macroblocks reach the SKIP early-out, so encoding costs an order
of magnitude more than it does on `motion`. A single-threaded 720p P-frame at
qp 10 takes **83 ms**, which caps real motion around 12 fps no matter what grid
you ask for.

That is not a property of the codec — P-frames simply were not threaded. Intra
frames already used the wavefront; P-frames, which are 29 of every 30, ran on
one core while the other thirteen sat idle. `fcap` now splits the frame into
`-j` horizontal bands and codes them on separate cores, using the band layout
`fdv_vtile_encode` already defined. On 24 frames of 720p camera content:

| qp | 1 thread | 4 bands | 8 bands |
|----|----------|---------|---------|
| 10 | 83.4 ms | 26.4 ms (3.2x) | 13.7 ms (6.1x) |
| 24 | 45.8 ms | 14.9 ms (3.1x) | 7.8 ms (5.9x) |

Bands are not free: prediction and the entropy model both reset at every band
edge, costing about **+3% bitrate at 4 bands and +8% at 8**. Four is the default
because that buys most of the speed for little of the cost; `-j 1` turns bands
off and writes a single-stream file.

What it means in practice, at qp 24 on a 30 fps grid:

| capture | encode | held |
|---------|--------|------|
| 640x480   |  6.2 ms/frame | 0% |
| 1280x720  | 10.8 ms/frame | 1% |
| 1920x1072 | 21.9 ms/frame | 6% (0% at `-j 8`) |

And at 720p, where quality used to have to be traded for frame rate:

| qp | `-j 1` | `-j 4` (default) | `-j 8` |
|----|--------|------------------|--------|
| 10 | 69% held, 9 fps | 4% held, 26 fps | 1% held, 28 fps |
|  4 | — | — | 1% held, 28 fps |

Near-lossless capture at 30 fps is now just `fcap out.fdv -q 4 -j 8`. `fcap`
still says when encoding took most of the wall clock and suggests which knob to
reach for.

## Output

All three tools print the same shape: a bold verb and the file it concerns, then
dim labels in a fixed column with the number that matters in bold.

```
encode clip.fdv
  source     motion.scn, 1280x720, 300 frames @30 fps  (10.00 s)
  coding     qp 22, key frame every 60 frames (2.0 s)
  stream     560 kbps   (592x under raw 331.78 Mbps)
  speed      8.4 ms/frame  (119 fps)
```

**Rates, never sizes.** A coded stream in megabytes tells you nothing about
whether it fits down a wire, which is the only question anyone actually has, so
every figure is bits per second: the stream, the target and how far it missed,
per-frame peaks, even the raw source it is being compared against. The one
exception is `fpl --cache`, which really is a memory budget.

Per-unit figures are rates too, and mean different things for the two container
kinds. A frame is reported as the rate it *would* imply if sustained -- the peak
a buffer has to absorb. A band is a whole sub-stream over the same duration, so
its rate is simply its share of the total, and the bands sum to the stream.

**Colour** appears only when the output is really a terminal, so piping to a file
or a script still yields plain text -- which matters here, because the benchmark
tooling parses it. `NO_COLOR=1` turns it off, `FDV_COLOR=1` forces it on for the
usual case of piping into a pager. The library's own log lines are tinted by tag,
so a long `-vv` trace can be skimmed.

`fcap` shows a live line while recording -- elapsed, frames, the rate so far, and
how many frames were held -- on a terminal only, since a carriage return into a
log file is just noise.

## Streams and the player

A `.fdv` file is a coded stream with a 12-byte head that makes it
self-describing — magic, version, kind, frame rate, payload length. The coded
streams themselves carry no magic and no frame rate, and the two container kinds
begin with different fields, so without this a file could not say what it was or
how fast to play it.

```
fdv encode motion clip.fdv -q 20      # a scene from the library -> stream
fdv encode motion hd.fdv -s 1080p     # the same scene at another resolution
fdv encode my.scn clip.fdv            # your own scene file
fdv encode stress big.fdv -t 4 -j 8   # tile-parallel container
fdv info clip.fdv -v                  # what is actually in the file
fdv decode clip.fdv out.y4m           # back to raw, for other tools
fpl clip.fdv                     # watch it
```

Every command that writes a stream writes the same container, so anything
`fdv` produces is something `fdv info` and `fpl` can open. To code your own
footage, convert it to 4:2:0 Y4M first (`ffmpeg -i in.mp4 -pix_fmt yuv420p
in.y4m`); the frame rate comes from its header.

### Resolution

`-s` re-targets a scene at another size, on `encode`, `gen` and `pipeline`:

```
fdv encode motion hd.fdv -s 1080p      # 1920x1088
fdv encode motion big.fdv -s 3840x2160
fdv pipeline stress -s 720p
```

Shorthands: `360p` `480p` `720p` `1080p` `1440p` `4k`, or an explicit `WxH`.
Dimensions must be multiples of 16, so the shorthands give the nearest codable
size — `1080p` is 1920x**1088** and `480p` is **848**x480, the same padding real
encoders carry and crop on display.

This scales the *composition*, not just the frame: every object's position,
extent and velocity moves with the size, so a scene is the same picture at
4K that it is at 320x192. Colours, gradient endpoints and noise amplitude are
intensities and stay put; circles and the frame-number glyph scale by the mean
of the two axes so they do not distort when the aspect ratio changes.

Editing the scene's own `size` line does *not* do this — it changes the frame
and leaves the objects where they were, marooned in one corner.

`fdv info` walks the file and reports the head, the per-frame (or per-band)
sizes and the rate — the first place to look when a stream misbehaves, since it
distinguishes a bad file from a bad decoder:

```
$ fdv info clip.fdv
clip.fdv
  container   FDV v1, single-stream
  video       320x192, 30 frame(s) @30 fps  (1.00 s)
  coding      qp 20, keyint 0 (only the first frame is intra)
  size        29840 bytes head+payload, 29828 payload
  rate        994 B/frame, 92.7x vs raw I420, 239 kbps
  units       30 frame(s), 800..1970 bytes (mean 990)  [-v to list]
```

### fpl — the player

A raylib player, built when raylib is present (`make player`, or
`RAYLIB_PREFIX=` to point it elsewhere). It opens the window at the video's own
resolution and plays at the file's frame rate. Small clips are scaled up by a
whole-number factor and drawn with nearest-neighbour filtering, so a 320x192
test scene fills a usable window and every coded pixel stays a square block --
blocking and ringing show as they actually are, not smoothed away. Larger clips
are scaled down to fit, where smoothing is what you want.

| key | |
|-----|---|
| `space` | play / pause |
| `.` `,` | step one frame |
| `←` `→` | seek one second |
| `0` | restart |
| `l` | loop |
| `f` | fullscreen |
| `h` | hide the overlay |
| `r` | colour range (full / studio) |

`fpl` is quiet by default and takes the same verbosity flags as `fdv`:

```
fpl clip.fdv -v      # per-stage: what the decode did, and a size summary
fpl clip.fdv -vv     # per-frame: type, bytes, time
fpl clip.fdv -vvv    # per-macroblock (needs the `make trace` build)
```

```
$ fpl clip.fdv -vv
[decode] begin 1280x720 6 frame(s) qp=20 keyint=0  (26355 B stream, 1382400 B/frame out)
[decode] frame 0    I    10708 B consumed     2.913 ms  -> 1382400 B planar
[decode] frame 1    P     3226 B consumed     0.870 ms  -> 1382400 B planar
...
clip.fdv: 1280x720 6 frame(s) @30 fps, single-stream, qp 20, decoded in 7.7 ms (1.29 ms/frame)
  1 intra + 5 inter, 2722..10708 bytes (mean 4387), slowest frame 2.90 ms
```

On a tile-parallel stream the per-frame summary is empty: the decoder suspends
the statistics sink across its worker threads, since it is a plain global with
no locking. The log lines still come through, one `begin` per band.

`--log-frames` is a different thing — it reports each frame as it is *displayed*,
which is about playback pacing rather than decoding.

The overlay reports resolution, container kind, QP, frame position, file size,
bitrate and the first frame's decode cost; the progress bar ticks each key
frame, and clicking it scrubs.

Frames are decoded **on demand**, through `fdv_dec_*`, with a bounded cache
(`--cache MB`, default 64). Holding a whole clip would cost
`nframes x w x h x 3/2` — 3.1 MB per frame at 1080p, so ten minutes would want
tens of gigabytes. A short clip still ends up entirely cached, which is the same
behaviour arrived at honestly:

```
$ fpl clip.fdv -v
  streaming: first frame 3.03 ms, 12-frame cache (16 MB of 16 MB for the whole clip)

$ fpl big.fdv -v            # 60 frames of 1080p
  streaming: first frame 6.22 ms, 21-frame cache (63 MB of 179 MB for the whole clip)
```

Sequential playback never seeks — it rolls the decoder forward one frame — so it
holds rate even when the cache is far smaller than the clip. Measured at 30.1 fps
with a 4-frame cache on a 60-frame clip. Stepping *backwards* past the cache
costs a seek, which replays from the preceding key frame; that is a property of
inter-frame prediction, not of the cache. With `keyint 0` there is one key frame,
at the start.

Bands of a tile-parallel stream are decoded in order rather than in parallel: at
one frame per display refresh there is nothing to gain, and `fdv_decode` is still
the threaded whole-file path.

Two things it does not guess at. **Colour range**: `.fdv` does not record
whether the video is full-range (what the scene renderer produces, and what JPEG
uses) or BT.601 studio range, and showing one as the other crushes blacks or
clips highlights — so it defaults to full and `r` toggles. **Chroma upsampling**
is by repetition rather than interpolation, because that is what the decoder's
own reconstruction assumed; the point is to see the frame the codec actually
produced, not a prettier version of it.

### Checking it

A player is easy to declare working, so `make check-player` drives the real
binary and checks the three things the unit suite cannot reach:

- **`--dump N out.ppm`** writes one converted frame and exits without opening a
  window — the colour path, against an independently written reference.
- **`--grab N out.ppm`** draws a frame and reads it *back off the GPU*, which
  covers the texture format, the upload and the draw. On a Retina display the
  framebuffer comes back at 2x; with nearest-neighbour filtering at whole-number
  zoom, point-sampling recovers the source pixels exactly.
- **`--log-frames`** reports each displayed frame, so the playback loop's order,
  rate and looping are checkable rather than assumed.

Worth having: the window sizing was wrong for a long time without any of the
numbers noticing. `fit_window` refused to scale up, so a 320x192 built-in scene
opened in a 320x192 window — about 3% of a laptop display, which looks like a
broken player rather than a small video. Every pixel in it was correct.

## Command-line tool

Raw frames are planar I420 (a `w*h` luma plane, then `(w/2)*(h/2)` U and V).
Dimensions must be multiples of 16.

```
fdv encode   <scene|in.y4m> <out.fdv> [-q QP | -b RATE] [-k GOP] [-n N]
                                      [-s SIZE] [-t MBROWS] [-j THREADS]
fdv info     <in.fdv> [-v]                        # dump a stream
fdv decode   <in.fdv> <out.yuv|.y4m> [threads]    # stream -> raw
fdv scenes                                        # list the scene library
fdv scene    <name>                               # print a scene, to copy
fdv gen      <scene> <out.yuv|.y4m> [frames] [-s SIZE]   # render a clip
fdv pipeline <scene> [options]                    # render+encode+decode+verify
fdv compare  <a.yuv> <b.yuv> <w> <h> <nframes>    # per-plane PSNR
fdv bench    [w] [h] [nframes] [qp] [iters]       # encode/decode throughput
fdv selftest [w] [h] [nframes] [qp]               # in-memory round trip
```

`-v` and `-vv` work on `encode`, `decode`, `gen` and `pipeline`, and mean the
same thing everywhere: `-v` is a line per stage, `-vv` a line per coded frame —
its size, its timing, its macroblock mode mix and which entropy mode won it.
On anything that reads a scene, `-v` also dumps what was parsed, which is the
quickest way to see that a directive took the attributes you thought it did.
`-vvv` is per macroblock and needs `make trace`.

```
$ fdv encode scenes/vista.scn out.fdv -n 5 -vv
[encode] frame 0    I    86156 B     11.34 ms  3600 MB all-intra          entropy=adaptive
[encode] frame 1    P    39181 B     84.38 ms  skip 1835 inter16 707  inter8 952  intra 106  entropy=split
[encode] frame 2    P    40236 B    104.50 ms  skip 1446 inter16 1113 inter8 945  intra 96   entropy=adaptive
```

To measure quality by hand: `gen` a scene to `.yuv` as the reference, `encode`
the same scene, `decode` to `.yuv`, and `compare` the two. `pipeline` does all of that in one step.

## Features

- **Color / transforms:** YUV 4:2:0, 8-bit. Integer 4×4 and 8×8 DCT-family
  transforms; per-8×8-region 4×4-vs-8×8 transform-size RD selection, in both the
  intra and inter paths, with the two sizes' coefficients modelled separately.
- **Quantization:** dead-zone quant + RD coefficient truncation (RDOQ-lite).
- **Entropy:** two coders, picked per frame by coding both ways and keeping the
  smaller. Interleaved range-ANS (rANS) with end-of-block coefficient coding,
  compact (sparse) frequency tables, per-stream context models and per-stream
  reuse of the previous frame's tables; plus an adaptive range coder, primed
  from decoded history so it transmits no model at all, for the streams short
  enough to decode serially.
- **Intra:** a rate-distortion quadtree over 64×64 coding tree units, split
  level by level down to 4×4 leaves, whichever costs less. 9 H.264-style
  directional modes at 4×4; DC, vertical, horizontal and H.264's plane fit on
  the larger leaves.
- **Inter (P-frames):** per-macroblock RD choice among SKIP / INTER-16×16 /
  INTER-8×8 / INTRA; SKIP names which neighbouring vector it inherits (HEVC's
  merge), and can inherit that neighbour's reference too; two reference frames
  with per-block reference selection; quarter-pel motion with a 6-tap luma
  filter and bilinear chroma; diamond search seeded by an EPZS-style
  predictor.
- **In-loop deblocking filter**, on the 8×8 grid.
- **Fast decode:** NEON SIMD kernels for `idct4x4`, `fdct4x4`, motion-search SAD,
  and `dequant4x4` (each bit-identical to its scalar reference); spatially
  independent tiles decode in parallel.
- **Rate control:** average-bitrate control with no lookahead or reordering,
  for live streaming (`-b`).
- **Tooling:** `fdv` CLI (`.fdv` streams from scenes or Y4M, decode to I420 or
  Y4M, PSNR compare, self-test, bench), `fpl`, a raylib player, and `fcap`, a
  macOS camera recorder.
- **Content generation:** a scene description language (`include/fdv_scene.h`)
  turns a few lines of text into a deterministic I420 clip, so the codec can be
  exercised without sourcing test footage. Twenty-three scenes each target a
  specific encoder path.
- **Instrumentation:** per-stage and per-frame logging (`-v` / `-vv`) plus a
  per-frame statistics sink — coded size, macroblock mode mix, entropy-model
  choice, symbol counts. A zone profiler (`make profile`, then `--profile`)
  reports self and inclusive CPU time per stage from inside the codec.
  Per-macroblock tracing (`-vvv`) and the profiler are both compiled out of
  normal builds so the decode hot path is untouched.

## Layout

The library is **one header**. Include it for the declarations; in exactly one
translation unit per program, define `FDV_IMPLEMENTATION` first to pull in the
code as well:

```c
#define FDV_IMPLEMENTATION
#include "fdv.h"
```

Link with `-lm -pthread`. Nothing else — no external dependencies.

| Path | Role |
|------|------|
| `include/fdv.h` | the whole library, in one file: 16 banner-marked sections |
| `src/fdv.c` | the `fdv` command-line tool |
| `src/fpl.c` | `fpl`, the raylib player for `.fdv` streams |
| `src/fcap.m` | `fcap`, macOS camera capture — the only OS-dependent file |
| `tests/test_enc.c` | the unit suite, one binary (14 sections) |
| `tests/test_fuzz.c` | decoder robustness harness (truncation + bit-flip) |
| `tools/check-player.sh` | drives `fpl` — colour, GPU readback, playback, placement |
| `tools/rd-bench.py` | rate/quality sweep over the scenes, and BD-rate between two runs |
| `tools/bd-rate.py` | BD-rate against x264 over the scenes |
| `tools/live-bench.py` | quality against x264 at one live bitrate, per scene |
| `tools/bench-x264.sh` | fixed-bitrate comparison against x264, with SSIM |
| `scenes/` | the scene library, 23 `.scn` files |
| `build/` | every object and binary; `make clean` removes it |

Each program compiles its own copy of the library, so there is no shared object
to keep in step — which is the point of shipping it this way.

### Switches

Define these before the include, and consistently across a program:
`FDV_IMPLEMENTATION` changes the declarations, not only the code.

| define | |
|--------|---|
| `FDV_IMPLEMENTATION` | emit the implementation in this translation unit |
| `FDV_PROFILE` | compile in the zone profiler (it reads a clock per block boundary) |
| `FDV_TRACE` | compile in per-macroblock trace logging at `FDV_LOG_BLOCK` |

There is no switch for leaving the scene renderer out, because it is not in
here to leave out — it lives in `include/fdv_scene.h`, with its own
`FDV_SCENE_IMPLEMENTATION` and `FDV_SCENE_DIR`. Not including it is the switch.

Tunables are `#ifndef`-guarded and can be overridden the same way:
`FDV_AD_BUDGET` (set it to 0 to build without the adaptive coder), `FDV_AD_SCALE`,
`FDV_AD_INC`, `FDV_AD_MAX`, `FDV_AD_HIST`, `FDV_RC_GAIN`, `FDV_RC_STEP`,
`FDV_RC_DRAIN`, `FDV_LAMBDA0`.

A decoder that wants nothing but the codec:

```c
#define FDV_IMPLEMENTATION
#include "fdv.h"

fdv_info info;
if (fdv_read(file, len, &info) == 0)
    fdv_decode(&info, frames, /*threads=*/4);
```

### Sections

Sections run in dependency order, bottom-up; nothing in one depends on a
section below it. That is a convention rather than something the compiler
enforces, so keep the order when editing.

| # | Section | Role |
|---|---------|------|
| 0 | `LOG` | debug logging, per-frame statistics, zone profiler |
| 1 | `FRAME` | planar YUV frame/plane buffers (aligned, padded borders) |
| 2 | `BITS` | MSB-first bit reader/writer |
| 3 | `BITPACK` | shared LEB/zigzag/little-endian/scan primitives |
| 4 | `RANS` | range-ANS entropy coder + adaptive range coder, compact frequency tables |
| 5 | `COEFF8` | 8×8 zigzag + end-of-block coefficient serialization |
| 6 | `TRANSFORM` | 4×4 + 8×8 integer transforms, quant/dequant, RDOQ (+ NEON) |
| 7 | `INTRA` | intra prediction (9 4×4 modes + n×n DC/V/H) |
| 8 | `INTER` | motion compensation (6-tap luma, bilinear chroma) + motion search |
| 9 | `DEBLOCK` | in-loop deblocking filter (+ NEON) |
| 10 | `IMAGE` | intra-only still-image codec (I-frames and tiles) |
| 11 | `VIDEO` | P-frame sequence codec (the full pipeline) |
| 12 | `TILES` | independent still-image tiles + threaded parallel decode |
| 13 | `VTILE` | tile-parallel video: threaded encode and decode |
| 14 | `RC` | frame-level rate control / Lagrangian QP selection |
| 15 | `FDV` | self-describing `.fdv` file container |

The scene renderer used to be section 16 of this header. It is now a separate
header, `include/fdv_scene.h`, because it is content generation and file I/O and
this one should be neither — `fdv.h` does encode, decode, and the instrumentation
around them, and touches no file.

## Scene description

A scene is a few lines of text. Every attribute has a default, so a directive is
complete as soon as it names what it draws:

```
size 320 192          # luma dimensions, multiples of 16
frames 30
fps 30
qp 20                 # the pipeline's default quantizer for this scene
seed 12345            # noise and cut jumps derive from this

bg luma=36
grad    dir=v from=20 to=90
rect    x=16 y=24 w=64 h=48 luma=200 cb=90 cr=170 vx=2 vy=1 bounce=1
circle  x=180 y=100 r=28 luma=120 cb=200 cr=60 vx=-1.5 vy=0.7 bounce=1
checker x=0 y=128 w=320 h=64 cell=8 luma=220 luma2=30
noise   sigma=4
cut     frame=18      # hard scene change: everything jumps
stamp   x=6 y=6 scale=3   # burn the frame number into the picture
```

Objects paint in declaration order. Positions and sizes are luma pixels;
chroma is derived by halving. `vx`/`vy` are pixels per frame, and `bounce=1`
reflects off the frame edges rather than clamping (a clamped object stops
moving, which stops exercising the motion search).

Rendering is a pure function of `(scene, frame index)` — no history, no
`rand()` — so a frame can be rendered on its own and two runs always produce
identical bytes. That is what makes the pipeline's PSNR and bitrate numbers
comparable across runs and machines.

The `stamp` directive draws the frame number in a built-in 3x5 font. It is the
quickest way to confirm a decoded clip is correct, in order, and complete.

### World layers

Rectangles and circles make good diagnostics and a poor landscape. Six more
directives build one, and they share an idea: each is placed in *world*
coordinates and moved past a single `camera` by its own `depth`.

```
camera vx=2.6                  # one pan, for the whole scene

ridge   y=410 amp=132 freq=2.0 oct=6 depth=0.14 fade=0.16 luma=126 luma2=240
clouds  y=0 h=300 freq=2.1 oct=5 depth=0.03 speed=0.0005 thresh=142 alpha=118
water   y=496 h=132 freq=3.2 amp=20 oct=4 depth=0.60 speed=0.021 alpha=92
scatter x=-640 y=606 w=2560 h=30 r=0 iw=3 ih=12 count=3000 depth=0.86
grain   freq=300 amp=8 oct=2 depth=0.92
```

`depth` is the whole point. At 0 a layer is pinned to the camera, at 1 it moves
a pixel per pixel of pan, and above 1 it outruns it. One `camera` line therefore
produces a different velocity in every layer at once — which is what a real
camera move looks like to an encoder, and what a scene of sliding rectangles
can never produce.

The shapes come from value-noise fBm rather than from the white `noise`
directive: neighbouring points are *correlated*, which is what makes a ridgeline
look like a ridgeline. `freq` is in cycles across the frame width and `depth` is
a ratio, so both survive `-s` unchanged; `amp` and instance sizes are pixel
measurements and scale with the picture.

| directive | |
|---|---|
| `ridge` | a horizon from folded fBm, filled beneath with a ramp from `luma2` at the crest to `luma` below. `fade` is how much of the range's own relief the crest colour holds — small values put snow on the tops instead of hazing the whole face. |
| `clouds` | fBm thresholded at `thresh` into coverage, feathered past it, drifting at `speed` on top of the camera's parallax. |
| `water` | mirrors what is already drawn about its waterline, displaced by a moving ripple and dimmed. Declare it after whatever should appear in it. |
| `scatter` | `count` instances hashed across a world band `w` wide and wrapped, so the camera scrolls new ones in forever. `r` draws discs, `r=0` draws `iw`x`ih` rectangles. |
| `grain` | correlated surface detail in world coordinates, so it translates with the shot. |

`seed=` pins a layer's noise, so two layers can share one shape — a trunk under
its own canopy, or a rock face and the snow on it.

The contrast with `noise` is the point of `grain`. White noise is unpredictable
by construction, an entropy floor and nothing else. Correlated grain locked to
the world is the harder case: fine detail that *is* predictable, but only if
motion compensation gets the vector exactly right, because a quarter-pixel error
decorrelates it completely.

### Actual three dimensions

The layers above fake depth. `camera3`, `terrain` and `props` derive it:

```
camera3 x=0 y=0 z=-260 yaw=0.15 pitch=-0.05 fov=1.05 vz=2.4 dyaw=0.0016 agl=125
terrain freq=0.0021 amp=300 oct=9 far=2100 y=34 thresh=200 tex=4 luma=78 luma2=124
props   density=118 cell=11 r=2.7 ih=8 iw=0.5 jitter=0.60 luma=58 luma2=44
```

`terrain` marches one ray per screen column across a noise heightfield, front to
back, painting each column down from a watermark — so nearer ground occludes
farther ground for free and nothing is drawn twice. On the way it shades by
slope against a fixed sun, blends grass into rock by altitude, puts snow above
`thresh`, fills below `y` with rippling water, and fades everything toward
`haze` with distance. `props` scatters billboards on a world grid around the
camera, sorted back to front and clipped against the hills in front of them, so
flying forward scrolls new ones in forever.

`agl` is what makes it a flight rather than a slide: the camera holds that
height above whatever is below it, so the shot climbs ridges and drops into
valleys on its own. Props stand on whichever `terrain` was declared above them —
the same painter's order as the rest of the language, applied to a fact about
the world rather than to pixels.

World units throughout, and `fdv_scene_resize` deliberately leaves all of it
alone: a perspective projection is already resolution-independent, because the
focal length is derived from the frame width. Scaling it would move the camera,
not the picture.

**Why this belongs in a codec's test set.** Sliding layers give a handful of
discrete velocities. A perspective camera gives one that varies *continuously*
with depth, and a camera flying forward gives vectors that diverge from a focus
of expansion — every block moving a different amount in a different direction,
none of it a global translation. It shows up directly in the mode mix: `valley`,
the 2D scene, falls back to intra on 60-96 macroblocks per P-frame, and `vista`,
the same landscape in 3D, on **530-778**.

### The scene library

There are no built-in scenes — every one is a file in `scenes/`, so anything you
can run you can also open and edit. `fdv scenes` lists them, and each one's
description is its own first comment line, which keeps the blurb with the scene
instead of in a table that drifts away from it.

A bare name is resolved as a path, then `name.scn`, then in `$FDV_SCENES`,
`./scenes`, and the installed `share/fdv/scenes` — so `fdv encode motion
out.fdv` works from the source tree and from anywhere else after `make
install`.

Roughly half are aimed at a specific path through the encoder:

| scene | what it exercises |
|-------|-------------------|
| `still` | static content — collapses to ~99% SKIP |
| `pan` | uniform whole-frame motion — the MV predictor's best case |
| `motion` | several velocities — the general case |
| `divergent` | opposed 8x8 motion in one macroblock — forces the 8x8 split |
| `cut` | hard scene changes — off inter prediction and back to intra |
| `strobe` | a cut every four frames — the pathological version of `cut` |
| `grain` | heavy noise — entropy coder and the rate side of RD |
| `detail` | 4- and 16-pixel checkers — transform and quantizer |
| `mosaic` | four cell sizes at once — a spatial-frequency sweep |
| `tiny` | 64x64, 4 frames — the quick check |

and the rest are content that happens to be hard in interesting ways:

| scene | |
|-------|---|
| `rain` | seventy streaks sharing one direction over a static background |
| `confetti` | sixty pieces each on its own heading — worst case for one vector per block |
| `swarm` | forty circles on a spiral, curved edges at every angle |
| `skyline` | a city panning past at dusk, lit windows over moving blocks |
| `wipe` | a hard edge sweeping across texture — half-old, half-new blocks |
| `stress` | motion, texture, grain and a cut together |

They are all 1280x720, and they span a wide range of difficulty — which is the
point. A change that helps one should be checked against the others:

| | bitrate | vs raw |
|---|---|---|
| `still` | 170 kbps | 1952x |
| `wipe` | 283 kbps | 1173x |
| `motion` | 774 kbps | 429x |
| `detail` | 1586 kbps | 209x |
| `confetti` | 2094 kbps | 159x |
| `swarm` | 4770 kbps | 70x |
| `grain` | 57 Mbps | 5.8x |

Some parameters deliberately do **not** scale with the frame, and the scenes
that rely on them say so in their own comments. `divergent`'s blocks are 8x8
with 8-pixel offsets and slow velocities because a macroblock is 16x16 at every
resolution — scale those and the pair stops straddling one, and the scene
quietly stops testing anything. Checker `cell` sizes are spatial frequencies for
the same reason: a 4-pixel cell tests the 4x4 transform's limit whatever the
picture size. `-s` follows the same rule.

Any of them renders at any resolution: `fdv encode swarm out.fdv -s 1080p`.

## The pipeline

`fdv pipeline <scene>` runs render → encode → decode → verify and
reports each stage:

```
./build/fdv pipeline motion -q 22        # a different quantizer
./build/fdv pipeline stress -n 10 -v     # 10 frames, per-stage logs
./build/fdv pipeline motion -t 2 -j 4    # through the tile-parallel path
./build/fdv pipeline my.scn -o /tmp/run  # artifacts elsewhere
```

| Option | Meaning |
|--------|---------|
| `-q QP` | quantizer (default: the scene's, else 20) |
| `-k KEYINT` | key-frame interval |
| `-n FRAMES` | override the scene's frame count |
| `-t MBROWS` | use the tile-parallel path, MB rows per band |
| `-j THREADS` | decode threads for `-t` |
| `-o DIR` | artifact directory (default `build/pipeline`) |
| `-v` / `-vv` / `-vvv` | per-stage / per-frame / per-macroblock logging |

The encode stage reports, per frame, the coded size, the macroblock mode mix
(skip / inter16 / inter8 / intra), which entropy model won, and how many
structure and coefficient symbols were emitted — which is how the scene claims
above were checked rather than assumed.

`-vvv` needs `make trace`: per-macroblock lines come from inside the block loops,
so they are compiled out of every normal build rather than left behind a runtime
branch.

## Speed

Per frame, QP 20, arm64, single stream. Three scenes, because one number here
would be a lie: what a frame costs to code and to decode depends almost entirely
on how much of it is SKIP.

| | `still` | `motion` | `plaza` |
|---|---|---|---|
| 720p encode | 7.5 ms | 8.1 ms | 29.1 ms |
| 720p decode | **0.54 ms** | **0.69 ms** | **2.57 ms** |
| 1080p encode | 14.0 ms | 10.3 ms | 55.6 ms |
| 1080p decode | **1.22 ms** | **1.44 ms** | **5.46 ms** |

(1080 is not a multiple of 16, so "1080p" here is 1920x1088 — the same padding
real encoders code and then crop. See *Limitations*.)

Treat any single figure with suspicion: `plaza` costs five times what `motion`
does to decode and eleven times what `still` costs to encode, and real camera
content sits at the hard end, not the easy one.

Of the decode figures, the adaptive entropy coder accounts for **+26% to +56%**,
scaling inversely with the gain it buys (`still` pays 27% for 21% fewer bits,
`plaza` 52% for 3.5%). It costs nothing measurable to encode. The **key frame**
is where it lands hardest: P-frames bound their serial decode with
`FDV_AD_BUDGET`, but every stream of an intra frame is adaptive, so a 720p key
frame goes from 3.2 ms to 11.5 ms. That is still a third of a 30 fps frame
budget, but it is the worst-case number to know when decoding to a deadline.
`FDV_AD_BUDGET=0` turns the coder off.

### Bands

Threaded, 8 bands on 8 threads, decode drops to roughly a quarter of the
single-stream figure. Bands cost bitrate: on `plaza` at 960x544 and equal PSNR,
**+2.3% at 2 bands, +8.3% at 4 and +14.9% at 7**, which is why the comparisons
above are all single-stream.

That penalty is **not** motion prediction across the band edge. A shared
reference picture across bands (HEVC's tile semantics) was built and is worth
only -0.2% at four bands and -0.7% at seven. The overhead is constant per frame,
about 93 bytes per extra band, whatever the clip length:

| | 1 band | 4 bands | extra |
|---|---|---|---|
| 24 frames | 70667 B | 77705 B | 293 B/frame |
| 48 frames | 138539 B | 151713 B | 274 B/frame |
| 24 frames, all-intra | 149695 B | 175797 B | 1087 B/frame |

It is **entropy fragmentation**: each band carries its own symbol counts, its
own tables or adaptive models and its own history. The change that would pay is
shared frequency tables -- chosen once per frame across bands and sent once;
a rANS payload needs only its table and its bytes, so bands stay independently
decodable. Only the adaptive coder cannot be shared, because its state is
sequential.

### How encode got fast

Encode is ~50x faster than the first working version and decode ~4x, at equal
or slightly better quality.

**1. Prove what the answer must be, instead of searching faster.** Every coding
mode has a floor: the syntax it must emit even when every coefficient quantizes
to zero.

| mode | floor |
|------|-------|
| INTRA | 3 mode + 3 sub-mode + 4x2 residual = **14 bits** |
| INTER 16x16 | 3 mode + 3 ref + 2 mvd + 4x2 residual = **16 bits** |
| INTER 8x8 | 3 mode + 4x2 mvd + 4x2 residual = **19 bits** |

A candidate whose floor already exceeds the best cost so far cannot win, so it
is never evaluated. Since roughly 96% of macroblocks end up SKIP, most of the
mode search was provably dead work. This is **exact** -- the bitstream is
unchanged byte for byte against a build with the pruning disabled -- and worth
~10x. The floors are derived from the same cost constants the decisions use, so
they cannot drift to an older scale and prune candidates that could win.

**2. Parallelism hiding inside something "serial".** Intra prediction reads its
neighbours' reconstructed pixels, but a block needs the row above only as far
as one column to its right. That is a wavefront: once row `r-1` is two blocks
ahead, row `r` can run alongside it, and each row writes its own symbol slice,
reassembled in raster order. The first version scaled badly because the
per-row progress counters shared a cache line; padding each to its own was the
whole difference.

**3. Compute only what the answer needs.** The quarter-pel interpolator
dispatches on the phase first, cutting eleven of sixteen phases to at most two
filters, and works a block at a time so the filter is separable. RDOQ's
candidates form a chain, so the trial block carries forward: O(n) where it was
O(n^2). The integer-pel motion search scores SAD against the reference in place
rather than copying each candidate out first, which removed three quarters of
the calls into motion compensation.

**4. Screen instead of coding.** A P-frame macroblock used to do a dozen
residual codings. Intra sub-modes are now ranked by SATD and only the best is
coded (-0.04% BD-rate; keeping two was worse *and* slower), and the 8x8 split is
coded only when its four quadrant searches bring the SAD below 9/10 of one
vector's (+0.10%). That halved encode on every hard scene -- `grain` 95.1 to
48.6 ms/frame at 720p30, `valley` 65.7 to 32.6.

**5. SIMD where the arithmetic lines up.** Three NEON instructions are exactly
the rounding rules the format specifies — `vrshrq_n_s16(s,5)` is `(s+16)>>5`,
`vrshrq_n_s32(s,10)` is `(s+512)>>10`, `vrhaddq_u8` is `(a+b+1)>>1` — which is
what lets a SIMD interpolator stay bit-exact rather than merely close. The
deblocking filter vectorised because its four samples sit adjacent at every
fourth column, precisely what `vld4q_u8` deinterleaves, and its per-edge test
becomes a mask instead of a branch; that halved decode, where deblocking had
been 50% of the time.

Two of these are heuristics rather than proofs. *Intra mode screening* ranks
all nine modes by SATD and gives full RD evaluation only to the best few;
plain SAD instead costs 14.8 dB on a fine checkerboard, and the screen is off
below QP 12, where SATD can rank but not choose. *Cross-shaped sub-pel
refinement* probes four points per stage instead of eight, measured at
±0.02 dB. `FDV_INTRA_RD_CANDIDATES=9`, a very large `FDV_INTRA_PRUNE_NUM` and
`FDV_ME_SUBPEL_PTS=8` restore the exhaustive behaviour.

## Profiling

`make profile` builds `build/fdv-profile`, which takes `--profile` on any
command and reports where the CPU time went — measured from inside the codec
rather than sampled from outside:

```
$ ./build/fdv-profile pipeline motion -s 720p -n 8 --profile
zone                self ms  self %    incl ms  incl %        calls    ns/call
mc luma              165.43   27.9%     165.43   27.9%      5243789       31.5  (*)
motion search         84.17   14.2%     243.07   41.1%       147600      570.3
residual 8x8          55.99    9.5%      55.99    9.5%      1181517       47.4  (*)
rdoq                  43.46    7.3%      43.46    7.3%      3124983       13.9  (*)
...
```

Zones nest, so both numbers matter: motion search is 14% of the time by itself
but 41% inclusive, which says its cost is really motion *compensation*. The
call counts are often the more useful column — 5.2M `mc_luma` calls across 8
frames is 182 per macroblock, and that is the number worth attacking.

Reading a clock costs ~18 ns, so zones sit at block and frame granularity and
never around a per-pixel kernel. Rows whose per-call cost is within 3x of that
are marked `(*)`: read their share, not their ns/call. The report states its own
overhead so the numbers can be read honestly, and the whole facility compiles
out of every other build.

## Limitations / not done

- **P-frames only** — no B-frames (bidirectional prediction is out of scope).
- **Dimensions must be multiples of 16, with no crop field.** 1920x1080 is
  therefore not directly codable; 1080p means coding 1920x1088 and cropping on
  display, which is what H.264 and HEVC do internally — but they carry the crop
  in the bitstream and this format does not, so the caller has to know.
- **Motion is still fixed 16×16** with an 8×8 split, and deliberately so: a
  quadtree over macroblocks was built and measured at roughly zero for 18-31%
  slower decode — see *Measured and rejected*. The *intra* tree was the promising
  half, and it went the other way: it now runs from a 64×64 coding tree unit
  down to 4×4 leaves.
- **Sub-8×8 motion partitioning** is not there, and the mode mix says it would
  not pay: INTER-8×8 is already only 0.1% to 1.5% of macroblocks.
- **The rate estimate is a stand-in, and that turns out to be fine.**
  Exp-Golomb lengths for coefficients, constants for structure symbols. The
  two-pass measured alternative was built and measured at roughly zero — see
  *Compression* for why.
- **Bands are fully independent videos**, so they cost +8.3% of the bitrate at
  four. Almost all of that is per-band entropy modelling, not prediction across
  the seam — see *Bands* under *Speed*.
- **No B-frames or hierarchical GOP**, and not for decode-speed reasons —
  bi-prediction is a SIMD average with no serial dependency. They are out
  because this codec is aimed at low-latency live streaming, and a B-frame is
  bought with reordering: the encoder cannot emit it until it has seen a frame
  that comes after it. That is the one currency a live stream has none of.
  Probably the largest coding gain on the table, and deliberately left there.
- The 8×8 transform uses a fixed-point matrix multiply (int64 inverse), not yet
  a multiply-free butterfly.
- SIMD is **NEON (arm64)** only; an AVX2 path for x86 decode is future work.
- On very small frames the per-frame header dominates, so the entropy and
  transform-size gains mostly show at realistic resolutions. Frequency tables
  are much less of that than they were — most streams now carry no table at
  all — but the per-stream symbol counts remain.

## License

Experimental / educational. No warranty.
