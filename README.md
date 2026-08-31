# fdv — Fast Decode Video

A from-scratch experimental video encoder/decoder in C11, no external
dependencies. The organizing principle is **decode speed on modern CPUs**: a
codec is encoded once but decoded billions of times, so every design fork is
resolved in favor of the decoder — table-driven branch-light entropy decoding,
small SIMD-friendly integer transforms, and tiles that decode in parallel.

It grew incrementally from a 16×16 image generator into a full YUV 4:2:0
P-frame codec with a usable command-line tool. See **ARCHITECTURE.md** for the
design rationale and the build-order history.

## Quick start — no `.yuv` file needed

The codec ships with a scene renderer, so a test clip is a description rather
than a download:

```
make
make install                  # -> ~/.local/bin/{fdv,fpl} + include/fdv.h
fdv scenes                    # the scene library, and what each one stresses
fdv encode motion clip.fdv    # a description in, a coded stream out
fpl clip.fdv             # watch it
```

`make install` puts `fdv` and `fpl` in `$(PREFIX)/bin` and `fdv.h` in
`$(PREFIX)/include`, `PREFIX` defaulting to `~/.local`; it warns if that is not on your `PATH`. `make
uninstall` removes them. `make play SCENE=stress` encodes a scene and opens it
in one step. `fdv pipeline motion` still runs the whole
render/encode/decode/verify chain with its numbers.

That prints every stage with its numbers and leaves the artifacts in
`build/pipeline/`: `source.y4m` and `decoded.y4m` (both open in ffplay/mpv),
plus `stream.bin`. To write your own clip, start from an existing scene and edit it:

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

Each program is compiled to a `.o` and linked in a separate step. That is
deliberate: on macOS, clang runs `dsymutil` whenever one invocation both
compiles and links with `-g`, which is what scatters `.dSYM` bundles through a
tree. Compiling and linking separately keeps full debug info — it lives in the
`.o` files and lldb follows the linker's debug map — and produces no `.dSYM` at
all. `-g` is in `CFLAGS`, never in `LDFLAGS`.

On an arm64 box, on the synthetic scenes, single-stream: **1080p encode 6.0
ms/frame, decode 1.2 ms/frame**; **720p encode 3.1 ms/frame, decode 0.57
ms/frame**.

Those scenes are full of SKIP macroblocks and are 15-20x easier than camera
input, so they are a regression check, not a performance claim. On real 720p
camera content: encode **45.8 ms/frame single-threaded, 7.8 ms on 8 bands**;
decode **0.69 ms/frame**. See **Compression** below.

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

The buffer is what bounds latency -- it is the decoder-side buffer the stream
implies, so its size *is* the buffering the stream demands. One second by
default.

`fdv info` reads the QP back out of the frames rather than reporting the
header's, which under rate control is only whatever the encoder opened with.

## Compression

### Measuring this honestly

An earlier version of this section claimed fdv was within **1.23x** of x264.
That came from one clip of a person sitting still in front of a webcam -- the
easiest content this codec will ever see, over 80% SKIP macroblocks. Quoting it
as a general figure was wrong.

The measure now is **BD-rate**: how much more bitrate fdv needs for the same
quality, integrated over a four-point sweep of each codec's own quality control.
`tools/bd-rate.py` runs it across the whole scene library, rendered losslessly by
`fdv gen`.

```
python3 tools/bd-rate.py <workdir> 960x544 90
```

At the time of writing, against x264 preset medium with no B-frames:

| scene | fdv needs | | scene | fdv needs |
|-------|-----------|-|-------|-----------|
| `stress`   |  **-88%** | | `pan`      |  +2% |
| `chroma`   |  **-31%** | | `still`    |  +4% |
| `plaza`    |  **-26%** | | `cut`      | +14% |
| `veil`     |  **-24%** | | `detail`   | +14% |
| `grain`    |  **-19%** | | `rain`     | +21% |
| `spin`     |  **-15%** | | `vista`    | +23% |
| `divergent`|  **-13%** | | `strobe`   | +24% |
| `skyline`  |   **-8%** | | `valley`   | +28% |
| `confetti` |   **-1%** | | `tiny`     | +30% |
| | | | `motion`   | +31% |
| | | | `churn`    | +47% |
| | | | `swarm`    | +47% |
| | | | `mosaic`   | +65% |
| | | | `wipe`    | +114% |

**Mean +11%, median +14%** across twenty-three scenes, and fdv is *ahead* of
x264 on nine of them. Both figures are quoted because the median lands between
two scenes and moves for reasons that have nothing to do with the codec.

An aggregate is only comparable against the *same* set. The per-scene numbers
are what carry across.

Where this came from, since the numbers moved a long way: the median was **+64%**
before the work described in the rest of this section. Six changes did it -- the
intra quadtree, a rate model that grows with coefficient magnitude, separating
coefficients by transform size, letting SKIP choose which motion vector it
inherits, deblocking on the 8x8 grid, and giving the block flags their own
entropy stream. Each has a section below with what it was measured at, and three
of them came from reading what HEVC does and asking what the reason was.

Measured against itself rather than against x264, over the same scene library,
those six changes come to **-24.9% mean / -22.5% median all-intra** (18 of 19
scenes) and **-28.6% / -27.7% on the video path** (21 of 22). One scene
regressed on both: `wipe`, by 2.5% and 0.8%.

The shape of the table is still the useful part. We are far *ahead* on the
densest, most expensive content (`stress`, `grain`, `plaza`, `veil`) and behind
on hard-edged synthetic content (`wipe`, `mosaic`, `swarm`) -- which is close to
the reverse of where this started, when the gap was on the *cheap* content and
had the signature of a fixed per-frame cost. That cost is what the quadtree
removed.

### What the harder scenes say

Five scenes were written to cover things the original set could not express --
colour that moves independently of brightness, rotation, translucency, per-object
grain. They needed the scene language extended, which is the point of having one.

The interesting result is `spin`, which is nothing but rotating textured
rectangles: **+4%**, near parity. Every motion vector in this codec is a
translation, so a turning edge cannot be predicted by shifting the previous
frame -- but that is equally true of x264, and both fall back on coding residual.
Motion *complexity* is not where this codec loses.

Nor is colour. `chroma` (+46%), `plaza` (+49%) and `veil` (+63%) all sit well
inside the median, and `veil` is almost entirely blended translucent layers.

The losses stay where they were: cheap, structure-dominated content -- `tiny`,
`pan`, `cut`, `motion`. That is a useful negative result. It says the next work
belongs in what a frame costs *before* any residual is coded, not in prediction
or in chroma.

### Where a frame's bytes go before any residual

Breaking a P-frame into its coded streams, at QP 28, 1280x720, bytes per frame:

| scene | kbps | modes | rest | counts | levels | tables |
|-------|------|-------|------|--------|--------|--------|
| `still`  |  68 | 10 | 18 | 16 | 39 | **12** |
| `tiny`   |  56 | 13 | 17 | 22 | 20 | **30** |
| `pan`    | 170 | 70 | 188 | 80 | 71 | **118** |
| `motion` | 340 | 69 | 200 | 240 | 644 | 5 |

Modes are already down to two hundredths of a bit per macroblock, so a
hierarchical skip flag -- one bit retiring a whole 32x32 or 64x64 region, the way
HEVC and AV1 do it -- would buy almost nothing here even though 98% of
macroblocks on `still` are skipped. Measuring that first saved building it.

**Frequency tables were the real cost**: a sixth of `pan`, near a third of
`tiny`. Reuse existed but was one decision for all seven streams together, so a
single stream whose distribution had moved forced all seven to resend. The
choice is per stream now -- seven bits instead of one -- and the mixed path is
available even on the first frame after a key frame, where there is nothing to
reuse and it simply means "fresh tables, separate models".

| scene | before | after |
|-------|--------|-------|
| `tiny`   |  56 kbps |  43 kbps |
| `still`  |  68 kbps |  62 kbps |
| `pan`    | 170 kbps | 160 kbps |

**Ten streams, and what that costs.** Splitting the rest of the structure by
role -- reference index, motion vector x, motion vector y, intra sub-mode -- was
worth 9% to 31% of that stream (`pan` -30%, `wipe` -31%, `skyline` -25%): a
reference index is almost always zero, a vector component is a signed delta with
a long tail, a sub-mode picks among nine directions, and they had been sharing a
model.

But going from seven streams to ten made the cheap scenes *worse* -- `still`
+72% to +80%, `tiny` +172% to +188% -- because each stream costs a frequency
table, and several are routinely empty. A frame with no intra macroblocks sends
no sub-modes, yet still paid a table and a length for them. Skipping empty
streams outright, and varint-coding the payload lengths, more than repaid it:
`pan` 160 to 143 kbps, `tiny` 43 to 42.

Mean BD-rate +79% to **+74%**, and every one of the twenty-one scenes improved
or held.

### The rate controller is not the problem

Worth separating, because "the bitrate control is bad" and "the codec needs more
bits" look identical from the outside. Encoding each scene at a fixed QP chosen
to land on whatever bitrate ABR actually produced isolates the controller's own
loss:

**+0.40 dB mean, +1.35 dB worst.** That is the whole cost of holding a bitrate
rather than being told the quantizer, and it is small. What remains is coding
efficiency.

(Several scenes come out far under target -- `pan` asks for 1200 kbps and uses
388. That is correct: at 67 dB the picture is already transparent and there is
nothing to buy. ABR undershooting easy content is not a fault.)

### Coefficient counts had no way to say "nothing here"

Every coded macroblock emitted an end-of-block count for every block, whether or
not the block had a single coefficient in it. Counts do not shrink when the
quantizer rises -- so this was a floor no amount of quantization could get under.
On a panning clip at QP **45**, where the coefficients themselves were down to 20
bytes a frame, the counts were still **292**.

Each 8x8 region's flag now carries a third value meaning nothing was coded there,
and chroma has one flag for the whole block. At QP 45 the counts went from 292
bytes a frame to 33, and the floor on that clip dropped from 209 to 135 kbps.

The floor is now the structure stream -- modes, references and motion vectors --
at about 1.3 bits per macroblock. Splitting it by role the way the coefficient
stream was split measures at 12-25% smaller, and is the next thing to do.

### The intra codec never got the entropy work

Every entropy improvement above -- the counts/levels split, table reuse, the
coded-block pattern -- had been applied to the P-frame path only. The intra
codec still had the original two-stream arrangement, and intra frames are most
of a cheap scene's bits, which is exactly where the BD-rate table is worst.

Splitting its coefficient stream the same way is worth far more there than it was
for P-frames. An intra frame is 4x4 blocks and most come out empty, so at QP 24
its counts are 39,000 symbols at 0.08 bits while its levels are 3,400 at 2.8
bits -- two distributions an order of magnitude apart, sharing one model. The
split takes **40% off the coefficient stream** and 15-30% off the whole key
frame at identical PSNR:

| qp | before | after |
|----|--------|-------|
| 16 | 5.6 kB | 3.9 kB |
| 22 | 6.9 kB | 5.5 kB |
| 28 | 5.3 kB | 4.5 kB |

Median BD-rate went from +110% to +101%. Decode cost 0.69 to 0.80 ms/frame at
720p -- still well inside budget, and still ahead of libavcodec's H.264.

### The intra mode was the largest single cost in a key frame

Breaking a key frame into its streams said plainly where the rest of it was:

| qp | frame | modes | share |
|----|-------|-------|-------|
| 20 | 5508 B | 3170 B | **58%** |
| 24 | 5366 B | 3547 B | **66%** |
| 32 | 3805 B | 2760 B | **73%** |

One mode was sent for every 4x4 block, chroma included. It is also very far
from random: where a block's left and above neighbours chose the *same* mode,
this block usually chooses it too -- that is what a flat or uniformly textured
region looks like. Where they disagree, the block is on an edge and the mode is
genuinely uncertain. Two distributions, one model.

Modes now go through two models chosen by whether those neighbours agree. The
context is rebuilt on the decoding side from blocks already decoded, so nothing
extra is transmitted. Key frames, at identical PSNR:

| qp | before | after |
|----|--------|-------|
| 16 | 4015 B | 3657 B |
| 22 | 5638 B | 4346 B |
| 28 | 4639 B | 3488 B |

Nine contexts, one per predicted mode, were measured too: 22.3% off the mode
stream against 21.6% for two, which does not pay for four times the table bytes.

**That was as far as better coding could go, and it was not far enough.** At QP
24 the mode stream on smooth content is 646 B over 32,640 blocks -- 0.16 bits a
block, already at its entropy floor. What cost was the *number* of symbols, and
three attempts to reduce it by steering the decision all failed:

- **Charging the mode in the mode decision.** Against the neighbours'
  prediction it made the mode stream 8-110% *larger*, the same trap as
  most-probable-mode: the pooled distribution is already skewed toward DC, and
  subtracting a position-varying predictor spreads that mass. Against DC it
  changed nothing at all, 646 B to 647 B at any penalty -- which is the real
  answer, because the residual term already prefers the cheap mode wherever it
  is genuinely free.
- **A per-macroblock "all sixteen modes agree" flag.** Worse everywhere, by
  0.4% to 63%. Per-4x4 prediction is genuinely better than one prediction for
  a whole macroblock, so the flag is mostly zero and buys nothing.
- **A bounded tie-break** letting the predicted mode win when it came within N
  bits of the leader: +3.8% BD-rate, worse at every slack from 1 to 32 bits.

The fix had to be structural, and it is the section below.

### The quadtree: fewer mode symbols, not cheaper ones

A 16x16 coding tree unit is now coded either as one 16x16 prediction, or split
into four 8x8 nodes, each of which is one prediction or four 4x4 leaves. The
split decision is the same Lagrangian as the mode decision, so a larger block
wins only when it is genuinely cheaper. A flat region costs one mode symbol per
256 pixels instead of sixteen; a detailed region still gets 4x4 blocks.

Larger leaves predict with DC, vertical, horizontal and **H.264's plane fit** --
a least-squares linear ramp through the two edges. That last one matters: a
smooth gradient is what most of these frames are, and the flat three can none of
them express it, so each leaves the gradient itself in the residual for the
transform to code, at every block.

Two things had to be fixed for it to pay:

**Coefficients had to be separated by transform size.** The 4x4 and 8x8
transforms were sharing one end-of-block count stream and one level stream. An
8x8 count runs 0..64 against the 4x4's 0..16, and an 8x8 transform concentrates
a block's energy into much larger coefficients. Pooling them cost 32 KB on one
high-rate intra frame *at an identical symbol count* -- the same symbols, coded
worse because two distributions shared a table.

**The rate model had to grow with magnitude**, which is its own section above.

Measured over the scene library at 960x544, against the codec immediately
before:

| | mean | median | improved |
|---|---|---|---|
| all-intra (keyint=1) | **-22.9%** | -20.5% | 18 of 19 |
| video (keyint=60) | **-21.0%** | -17.3% | 21 of 22 |

The scenes the old codec was worst on are the ones that moved most: `plaza`
-38%, `veil` -56%, `chroma` -62%, `motion` -26%. Four scenes -- `pan`, `still`,
`tiny`, `cut` -- could not be scored before at all, because the old intra path's
rate went *up* with QP on smooth content and left fewer than three points on the
rate-distortion frontier. They are monotonic now.

One scene regressed: `wipe`, +3.0%.

There is a correctness note buried in this. A quadtree visits a unit's cells in
z-order, and under z-order the cell above-right of a lower-left quadrant belongs
to the quadrant coded *after* it -- five of every sixteen cells lose a
neighbour that raster order always had. Reading it anyway would pull in
uninitialised samples in the encoder and stale ones in the decoder, which is a
mismatch rather than an inefficiency, so availability is derived exactly and
both sides run the identical function.

### Macroblock modes carry the same skew

Skipping is contagious. A macroblock whose left and above neighbours both had
nothing to code is overwhelmingly likely to have nothing either -- that is what
a still region looks like -- while one bordering coded blocks sits on the edge of
something moving. One model was serving both.

The macroblock mode now goes through four, chosen on the pair of neighbours,
which is what H.264 conditions its skip flag on. Measured on the mode stream
alone: `pan` -60%, `cut` -45%, `skyline` -40%, `still` -38%. Two contexts
(neighbours agree or not) were measured too and give -30% to -45%; the pair is
worth the extra two tables.

That took the P-frame structure to seven coded streams, and seven fixed 32-bit
symbol counts is 28 bytes a frame -- nothing on a busy frame, several percent of
a still one. They are varints now, which is worth more than it sounds: it turned
a 14%-worse result on `still` into a 3% better one.

Mean BD-rate went +94% to **+85%**. Decode is 0.83 ms/frame at 720p against
libavcodec H.264's 1.20, so the budget still holds.

It also explains an oddity: coded size is not monotonic in QP. At QP 16 the mode
decision happens to agree everywhere (992 B of modes) and at QP 20 it does not
(3170 B), so QP 20 costs *more* bits for *less* quality. Nothing is wrong with
the quantizer -- it was checked, and it coarsens smoothly.

**H.264's most-probable-mode was tried here and lost.** Modes are 93-100%
predictable from the smaller of their left and above neighbours, and conditioning
on that measures 20-26% smaller as conditional entropy. But coding `mode - mpm`
does not collect it: conditional entropy is only reachable that way when the
conditional distributions are translates of one another, and these are not. The
pooled distribution is already heavily skewed toward DC, and subtracting a
position-varying predictor spreads that mass instead of concentrating it.
Measured across five scenes it came out 0.3% to 1.8% *worse*, better on only one,
so it was reverted. Collecting that 20-26% needs a model per predictor value --
real context modelling, the way the coefficient streams are split -- not a
relabelling.

Coding a single frame with nothing to predict from, 960x544:

| qp | fdv | x264 |
|----|-----|------|
| 16 | 5.6 kB @ 57.72 dB | 4.7 kB @ **66.54 dB** |
| 22 | 6.9 kB @ 51.48 dB | 4.3 kB @ **59.25 dB** |
| 28 | 5.3 kB @ 46.37 dB | 3.5 kB @ **53.94 dB** |

About **8 dB behind at fewer bits**. On cheap scenes key frames are most of the
stream, which is exactly where the BD-rate table is worst, so this is one problem
and not two. It is the next thing worth fixing. (There is also an anomaly in
there: the qp 16 frame is *smaller* than the qp 22 frame, which cannot be right
and is not yet explained.)

### Entropy tables were sent every frame

Three frequency tables per frame is a fixed cost, and the frames that can least
afford it are the cheap ones -- on a static scene the tables came to **36% of
the coded P-frame**, against 3% on a busy one.

A frame can now reuse the previous frame's tables when that codes smaller,
chosen per frame by trying both, so it cannot lose. The chain resets at every key
frame, so seeking to one still needs nothing before it.

The first attempt at this fired **once in 88 frames**, and the reason is worth
recording: a table can only code a symbol it has a frequency for, so one symbol
the previous frame happened not to use disqualified the whole table -- measured,
99% of the time. The cached copy now reserves one slot per symbol out of 4096,
derived from the transmitted table by the same rule on both sides. Coverage went
from 0% to 92-100% and the median BD-rate from +130% to +117%.

### The entropy model was always transmitted, never learned

Every model this codec used was fitted to one frame and sent with it. The
alternative -- a model both sides *derive* from frames they have already coded,
so nothing is sent -- is what CABAC does, and the question was whether it is
worth having here, given that the whole design is built on rANS being parallel
to decode and CABAC being serial.

Three things were measured before writing any of it, on the coded streams
themselves, at QP 28.

**Adapting from a flat prior -- roughly what CABAC's context init tables
approximate -- is a disaster.** These streams are short and violently skewed, so
a model that starts uniform pays more to learn the distribution than a
transmitted table costs:

| | `tiny` | `still` | `pan` | `motion` | `swarm` | `churn` |
|---|---|---|---|---|---|---|
| flat-prior adaptive | +140% | +111% | +89% | +35% | +6.9% | +0.7% |

**Deriving a table from history and sending nothing is also worse** -- the same
prior with the adaptation switched off costs +0.8% to +30.5%. History is a
blurred average of many frames, and a table fitted to *this* frame beats it by
more than the table costs.

**Doing both wins, and by a lot.** Start from the history, then let the model
chase this frame's statistics as it codes:

| | `tiny` | `still` | `pan` | `motion` | `detail` | `swarm` | `churn` |
|---|---|---|---|---|---|---|---|
| history-primed adaptive | **-32%** | **-31%** | **-15%** | -7.9% | -5.5% | -2.3% | -0.6% |

Neither half is worth anything alone. On a SKIP-dominated stream the dominant
symbol's weight climbs within a few hundred symbols and its cost collapses,
which no transmitted table can match at any price -- but only if the model
starts somewhere sane.

So: not CABAC, but the mechanism inside it. What it costs is that adaptation is
serial -- symbol *i+1*'s model depends on symbol *i*, exactly the dependency
static rANS was chosen to avoid. That is bounded by applying it only to streams
below `FDV_AD_CAP` symbols, which is where the entire win is anyway: the big
coefficient streams on busy frames gain under 1% and stay on parallel rANS.

Three details carry most of the result, and each was wrong in the first version:

- **All the adaptive streams share one range-coded blob.** A range coder costs
  about four bytes to flush. Ten streams flushing separately is forty bytes a
  frame, which is a quarter of a cheap frame and more than the whole gain.
- **The prior is normalized to a fixed total** rather than used raw. Otherwise
  three things fight: a longer history is a better prior, but it inflates the
  model total, and the coder's `range / total` division gives back in precision
  what the model gained. Separating them was worth 1-2%.
- **The model halves past a bound** while coding, so the adaptation rate can be
  chosen on its merits instead of being capped by that same precision limit.
  Before this, every sweep ran monotonically to the edge -- the sign that the
  knob being turned was not the one that mattered.

Which streams went adaptive is *derived*, not transmitted: the decoder has every
symbol count before it reads any payload, so it applies the same rule for free.
The frame is still coded both ways and the smaller kept, so the mode cannot lose.

At a fixed QP, against the same encoder with the coder switched off (960x544,
60-frame clips):

| | `tiny` | `pan` | `still` | `veil` | `motion` | `detail` | `plaza` | `swarm` |
|---|---|---|---|---|---|---|---|---|
| bitrate | **-29%** | **-23%** | **-22%** | -11% | -8.7% | -5.0% | -3.7% | -0.5% |

PSNR is identical to two decimals on every one -- this is entropy coding, and
changes nothing about what is reconstructed. Across the scene library, over
60-frame clips, **mean BD-rate +67% to +55% and median +62% to +53%, improving
eighteen of twenty-one scenes and worsening none.**

(Aggregates quoted inside a section are the before-and-after of *that* change,
at the time it was made. They are not the codec's current standing, which is at
the top of this section and a long way below any of them. A per-change figure is
only meaningful against the build it was measured on.)

**What it costs to decode**, 720p, single stream, against the same build with
`FDV_AD_CAP` set to zero:

| | `still` | `veil` | `motion` | `plaza` |
|---|---|---|---|---|
| off | 0.63 ms | 0.74 ms | 0.71 ms | 0.83 ms |
| on | 0.80 ms | 1.02 ms | 1.04 ms | 1.27 ms |

That is the honest cost: **+27% to +52%**, and on the busiest scene it puts
decode marginally past libavcodec's H.264 (1.20 ms), where before it was
comfortably ahead. The cost and the gain move in opposite directions -- `still`
pays 27% for 21% fewer bits, `plaza` pays 52% for 3.5% -- because a busy frame's
symbols are mostly in the big streams that stay on rANS.

`FDV_AD_CAP` is the whole knob. At 1024 instead of 4096, decode costs about
5% instead of 40% and the median BD-rate is +56% instead of +53%. 4096 is
chosen because the gap to x264 is still the larger problem; a build that wants
the decode margin back can move one constant.

### Key frames could not use the history, so they learn from nothing instead

The adaptive coder above primes from frames both sides have already decoded. A
key frame has none by construction -- seeking to one must need nothing before it
-- so the only option there is the flat prior, which is exactly the case that
lost by +89% to +140% on the P-frames' short streams.

It wins on every scene at every quantizer anyway, by -0.1% to -11.5%. The
difference is length: a P-frame's mode-context stream is a few hundred symbols,
where a model starting from uniform never earns back what it spends learning,
while an intra frame's is 57,600 at 720p and the learning cost amortizes to
nothing. What it buys after that is *within-frame* adaptation -- a key frame's
statistics change from flat regions to detailed ones, and one static table per
stream cannot follow that. On `mosaic` at QP 16 it saves 3861 bytes where the
four frequency tables it replaces were only 189, so nine tenths of the gain is
the tracking, not the tables.

**Mean BD-rate +55% to +53%, median +53% to +49%**, improving thirteen of
twenty-one scenes and worsening one by a point. (Measured over 60-frame clips,
against the same build with `FDV_AD_CAP` set to zero. Aggregates only compare
within one run -- the table at the top of this section uses 90-frame clips and
reads differently for that reason alone.)

### Where a key frame's bits actually were

Profiling a key frame before the quadtree told the blunt story that motivated
it:

| QP 24 | `tiny` | `pan` | `still` | `mosaic` | `detail` |
|---|---|---|---|---|---|
| intra modes | **90%** | **80%** | **62%** | 9% | 6% |
| coefficients | 10% | 20% | 38% | 91% | 94% |

On smooth content the frame was almost entirely *mode signalling* -- one mode
per 4x4 block, 32,640 of them at 960x544 -- and those were exactly the scenes
where the intra path was worst. The three attempts to fix it by steering the
mode decision, and the structural fix that did work, are two sections above.

One more thing was measured here and did not pay: **charging the motion search
for its vector.** Motion vectors are 24-37% of a P-frame at QP 34, the operating
point a 1 Mbps stream runs at, and the search scored SAD with no rate term at
all -- the classic omission. Adding `lambda_me * bits(mv - pred)` over a
half-to-four-times sweep moved coded size between -2.3% and +1.6% and averaged
zero, because the EPZS seed already starts the search at the predicted vector
and the diamond is local. What *did* collect on those bits was letting SKIP
choose which vector to inherit -- see below.

### Chroma had no end-of-block

Luma coded an end-of-block count per block and chroma did not -- every 4x4
chroma block emitted all sixteen coefficients whether or not it had any. On
moving content that was a quarter of every coefficient symbol in the stream,
99% of them zero and 97% of them trailing zeros a count removes outright. They
were not only wasted bits: mixed into the level stream they dragged its model
toward zero, which made every real coefficient dearer than it needed to be.

Giving chroma the same count luma always had, at 1200 kbps:

| content | before | after |
|---------|--------|-------|
| `swarm` | 34.87 dB | 36.02 dB |
| `stress` | 33.14 dB | 34.60 dB |
| camera pan | 48.56 dB | 49.20 dB |
| mandelbrot | 29.64 dB | 30.02 dB |

### Key frames follow the P operating point

Rate control ran a separate feedback loop per frame type. At a two-second
interval the P loop sees sixty samples for every one the key-frame loop gets, so
the key-frame loop converged sixty times more slowly -- on hard content the
first key frame of a stream landed at **QP 16 and cost an eighth of the whole
stream**, and six key frames later the loop still had not caught up. The clip
overshot its 1200 kbps target by 38%.

Key frames now sit at the P operating point minus an offset, so they benefit
from every P-frame's evidence, and a key frame that still lands more than twice
its allocation is requantized once. `stress` went from 1681 to 1453 kbps and
from 33.14 to 35.26 dB.

### The Lagrangian constant, and then the rate model underneath it

Mode decision minimizes `J = D + lambda*R`, and the textbook H.264 constant is
`lambda = 0.85 * 2^((QP-12)/3)` -- with **R in bits**. This encoder used to
count R as 8 bits per symbol byte, but those bytes go through an entropy coder
that spends about 2 bits on each. R as counted was roughly four times R as paid,
so rate was weighted four times too heavily. Sweeping a scale factor at a fixed
960 kbps put the optimum at exactly a quarter, which is what the arithmetic
predicts, and that was worth **+0.85 dB at equal bitrate** for one number.

Correcting the constant left the model itself wrong in a way one constant cannot
fix: "eight bits per byte" charges the same for a byte carrying zero as for one
carrying two hundred, and the coder does not. That made the encoder prefer
choices producing *fewer* bytes over ones producing *cheaper* bytes.

While every block was 4x4 and every transform the same size there was nothing
for that bias to act on. The intra quadtree gave it plenty: on dense grain at QP
16 the encoder coded half the frame with large blocks and 8x8 transforms and
paid 29% more bits for the same picture, because its model said those were
cheaper and they were not. It measured **+70% BD-rate on `stress`** -- a
regression created entirely by giving a broken rate model something to choose
between.

Rate is now an exp-Golomb length for a coefficient (1 bit for zero, 3 for one,
5 up to six, growing with magnitude) and small constants for structure symbols.
That lands about half of what the coder really spends, so lambda halves to
**0.425** to match -- swept over 0.2125, 0.30, 0.425 and 0.6375, with a flat
enough curve either side that the value is not delicate. The exact mode-pruning
floors are derived from the same constants rather than written out, so a floor
cannot be left behind at the old scale and start pruning candidates that could
have won.

Together with the quadtree this is where the intra path's **-23% BD-rate** came
from.

**Measuring the rate instead of estimating it was then tried, and is not worth
it.** Coding each frame twice -- once to find out what its symbols cost, then
again against -log2(p) per symbol per stream -- measures **+0.03% mean and
-1.22% median** BD-rate at the current lambda, and **-0.24% mean** at the best
of a five-point lambda sweep, for twice the encode time. A third pass is no
better than the second, so it is not that the table is stale.

The reason is worth keeping, because "the rate estimate is only a guess" reads
like an obvious thing left to fix and it is not one: **exp-Golomb is the entropy
of a geometric source**, and coefficient levels are close to geometric. Where
they are not -- dense grain, fine checkerboards -- measuring helps and helps a
lot (`stress` -27.7%, `detail` -3.1%); everywhere else it moves nothing, and on
`cut` it costs 12% because the numeric scale shifts under a lambda tuned for the
guess.

So what was wrong with the original model was never its accuracy. It was its
*shape*: eight bits for every emitted byte is flat where the true cost grows.
Getting the shape right was worth -23%; getting the magnitude right on top of
that is worth nothing.

### Key frames

`-k` sets the interval, on `fcap` and on `fdv encode`. It takes either a frame
count or a duration:

```
fcap out.fdv -k 2s      # every two seconds, whatever the frame rate
fcap out.fdv -k 90      # exactly 90 frames
fcap out.fdv -k 0       # one key frame, at the start, and no more
```

Seconds are the useful unit and the default is `2s`. The interval that matters
is a duration -- it bounds how long a viewer joining a stream waits for a
decodable frame -- and a frame count silently halves it when the frame rate
doubles: `-k 60` is two seconds at 30 fps and one second at 60. The header
stores the interval in a byte, so it caps at 255 frames; `fcap` says so rather
than quietly clamping.

### Why the default is two seconds

At a fixed 1 Mbps, key frames were eating a quarter of the stream:

| keyint | I-frame share | PSNR |
|--------|---------------|------|
|  30 (1s) | 26% | 45.53 dB |
|  60 (2s) | 13% | 45.96 dB |
| 120 (4s) |  7% | 46.07 dB |
| 250 (8s) |  4% | 46.17 dB |

The default is now 60 -- two seconds, the usual streaming compromise, and where
most of the gain already is. Longer intervals cost a viewer joining mid-stream a
longer wait for a decodable frame.

### SKIP could not say which vector it was inheriting

A SKIP macroblock had exactly one motion vector available to it: the running
predictor, which is the last vector coded in this row. That is a good guess on
uniform motion and a bad one at an object boundary, where the macroblock *above*
is right and the one to the left is wrong -- and SKIP had no way to say so. It
either took the wrong vector, or stopped being SKIP and paid for a coded one.

SKIP now names which vector it inherits, from a list both sides build out of
macroblocks already coded: the running predictor first, then the macroblock
above and the one above-right, deduplicated, capped at three. Only the index
travels, and only when there is more than one candidate. Index 0 reproduces
exactly what SKIP did before, which is what bounds the downside to the cost of
the index symbol. A candidate carries its reference index too, so a SKIP can now
inherit the further reference; it could only ever use the nearest one.

**-6.8% mean BD-rate, -4.6% median, 22 of 23 scenes**, and the one regression is
+0.2%. It lands where you would expect -- where the right vector is not the one
to the left:

| scene | | scene | |
|---|---|---|---|
| `skyline` (pan with occlusion) | **-29.7%** | `divergent` | -13.3% |
| `chroma` | **-24.0%** | `veil` | -11.1% |
| `tiny` | -13.9% | `confetti` | -10.0% |

This is HEVC's merge mode, and it is worth reading against the median-predictor
result in the next section, which measured -0.3% and looks like it rules this
out. It does not: improving the vector a delta is measured *against* is worth
nothing here, because the existing predictor is already good. Removing the delta
is a different lever.

### The flags were riding in the coefficient counts

The transform-size region flag (0, 1 or 2 — nothing here, four 4x4 transforms,
or one 8x8) and chroma's coded-block flag (0 or 1) were both being written into
the end-of-block *count* stream, which carries values 0..16. A three-symbol
alphabet and a seventeen-symbol one, sharing one model. This is the same mistake
this codebase has now measured three times, and it is worth stating as a rule:
if you can describe two symbol kinds in different sentences, they want different
models.

Giving the flags their own stream: **-1.6% on the intra path and -1.6% on the
video path, every scene improved and none regressed.** That is unusually clean
for a coding change, and it is because nothing about the decisions moved --
these are the same symbols in the same order, coded against a distribution that
is actually theirs.

### A quadtree over macroblocks, and why it is not the same problem

The inter side looks like the intra side did. At a realistic quantizer **90% to
99.8% of macroblocks come out SKIP**, so a frame spends two thousand mode
symbols saying the picture has not changed — apparently the same fix: 64x64 and
32x32 coding tree units whose unsplit leaf means "nothing moved here, using this
vector".

It was built, and it does what it was built to do — on `tiny` it cut structure
symbols from 2078 a frame to 338, on `skyline` from 2886 to 959. It is still not
worth having: **BD-rate +0.12% with one level, +0.37% with two, and decode 18%
to 31% slower.** Reverted.

The reason the analogy fails is the useful part. An intra mode is one of nine,
one per 4x4 block, and costs about 0.16 bits × 32640 — real money, which is why
the intra quadtree was worth -23%. An inter mode symbol sits in a stream that is
99% SKIP, where it costs about **a fiftieth of a bit**: two thousand of them
come to roughly forty bits a frame. No amount of structure saves forty bits and
still pays for its own flags.

What the tree does collect is per-macroblock *merge indices*, which is why
`skyline` gains 5.4% and `pan` — uniform motion, where every candidate dedups to
one and no index is sent — loses 5.9%. Those cancel.

Two things learned on the way, recorded because they are easy to repeat:

- **A node coder has to compare like with like.** Returning only the rate of the
  symbols a macroblock emitted, and leaving its distortion inside its own
  decision, makes splitting look nearly free: +3.7% BD-rate, and worst on
  exactly the static content the tree was supposed to help.
- **Pricing a symbol from the coded history is circular.** Leaves win, few mode
  symbols get emitted, the few that are look expensive, and more leaves win. The
  counterfactual a leaf-versus-split comparison needs is what a mode symbol
  would cost *if every macroblock signalled for itself* — the macroblock tally,
  not the symbol stream.

### The deblocking filter was on the wrong grid

It filtered every 4x4 edge. Half of those are not block boundaries at all --
inside a 16x16 intra leaf coded with an 8x8 transform there is nothing there to
smooth -- so filtering them softened the picture for nothing. That was always
somewhat true and became clearly true once the quadtree started choosing large
blocks: switching the filter off entirely measured **-0.9% mean** on the intra
path, meaning it was destroying about as much as it repaired.

Moving to the 8x8 grid is worth **-1.0% BD-rate all-intra and -2.2% on the video
path**, and cuts 720p decode from 0.83 to 0.78 ms/frame. Same trade HEVC made,
same reason: it is the rare change that buys quality and speed together.

### What was measured and rejected

Three things that looked like they should help and do not, each cheaper to
measure than to write:

- **A median motion-vector predictor** (H.264's left/top/top-right), against the
  existing "previous macroblock in the row": **-0.3%**, on a stream where motion
  vectors are 9% of the bits. Near-static camera footage already predicts close
  to zero. Note what this does *not* say: improving the vector a delta is
  measured against is worth nothing here, but *removing the delta* is worth
  -6.8% -- see the merge section below. They are different levers and it is easy
  to read the first as ruling out the second.
- **Merge with a residual** (HEVC's other half, an inherited vector on a block
  that still codes a residual): **+0.18%**, 13 of 23 scenes worse. It has little
  to remove for exactly the reason above, and costs a fifth symbol in the
  macroblock-mode alphabet on every block that does not take it.
- **HEVC's intra boundary filter** (pulling the first row and column of a DC,
  vertical or horizontal prediction back toward the neighbours the prediction
  ignored): **+3.6% BD-rate** applied to 4x4 blocks, worst on hard edges
  (`wipe` +18.6%, `mosaic` +15.3%), and **+0.3%** applied to the 16x16 and 8x8
  intra path. At 4x4 it is not a boundary filter at all -- it rewrites seven of
  sixteen samples -- and what it mostly does is smear real edges.
- **A wider motion search** (range 16 to 32, 4 subpel points to 8): +0.04 dB for
  +1.4% bitrate. Motion estimation is not the bottleneck here.
- **Dropping intra macroblocks from P-frames**, on the theory that 14% was
  suspiciously high: costs both bitrate *and* quality (773 kbps at 44.99 dB
  against 719 at 45.10). On noisy camera content intra genuinely wins for some
  blocks.

Deblocking's *strength* was swept too and is at its best: neither halving nor
raising it helps. Its *grid* was wrong, though -- see below.

### Where the bits go, and what moved

Profiling the P-frame payload on camera content settled what was worth doing:

- **rANS itself was already within 0.6% of the zero-order entropy of its own
  symbol stream.** Nothing to win in the coder; only a better *model* could pay.
- **Coefficients are 91% of symbols**, structure 9%. Frequency tables cost
  2.8-5.2% of the payload.
- **A better motion-vector predictor was worth −0.3%.** The H.264-style
  median of left/top/top-right was measured against the existing predictor and
  is not the lever on this content -- near-static camera footage already
  predicts close to zero. Worth knowing before writing it.

What did pay: **end-of-block counts and coefficient levels were sharing one
frequency table.** They are two different distributions -- counts run about 1.2
bits, levels 2.7 -- and pooling them wasted about a sixth of the coefficient
bits. They now travel as separate streams with a model each, along with the
transform-size flags, which are structural rather than levels.

| qp | before | after | |
|----|--------|-------|-|
| 16 | 2861 kbps | 2707 kbps | −5.4% |
| 20 | 1597 kbps | 1455 kbps | −8.9% |
| 24 |  938 kbps |  825 kbps | −12.0% |
| 28 |  559 kbps |  475 kbps | −14.9% |
| 32 |  355 kbps |  296 kbps | −16.8% |

**PSNR is unchanged to two decimals at every qp** -- this is entropy coding, so
it is the same picture in fewer bytes. Decode got faster too, 1.13 to 0.69
ms/frame at 720p: narrower alphabets per stream make each rANS table friendlier
to the cache.

Splitting further -- by scan position, by block size -- was measured at under a
percent more, which does not pay for the extra tables.

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
fdv enc in.yuv 1920 1088 60 20 clip.fdv 0 30   # your own raw I420
fdv encode stress big.fdv -t 4 -j 8   # tile-parallel container
fdv info clip.fdv -v                  # what is actually in the file
fdv decode clip.fdv out.y4m           # back to raw, for other tools
fpl clip.fdv                     # watch it
```

Every command that writes a stream writes the same container, so anything
`fdv` produces is something `fdv info` and `fpl` can open. `fdv enc`
takes an explicit frame rate because raw I420 does not carry one.

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

Frames are planar I420 (a `w*h` luma plane, then `(w/2)*(h/2)` U and V).
Dimensions must be multiples of 16.

```
./build/fdv encode    <scene|in.y4m> <out.fdv> [opts]   # -> a .fdv stream
./build/fdv info      <in.fdv> [-v]                           # dump a stream
./build/fdv decode    <in.fdv> <out.yuv|.y4m> [threads] [-v]  # stream -> raw
./build/fdv scenes                                            # list the scene library
./build/fdv scene     <name>                                  # print a scene, to copy
./build/fdv gen       <scene> <out.yuv|.y4m> [frames] [-v]  # render a clip
./build/fdv pipeline  <scene> [options]                # render+encode+decode+verify
./build/fdv enc       <in.yuv> <w> <h> <nframes> <qp> <out.bin> [keyint]  # encode raw I420
./build/fdv dec       <in.bin> <out.yuv>                            # decode to I420
./build/fdv enctiled  <in.yuv> <w> <h> <nframes> <qp> <band_mbrows> <out.bin> [keyint] [threads]  # tile-parallel encode
./build/fdv dectiled  <in.bin> <out.yuv> [threads]                  # tile-parallel decode (default 4 threads)
./build/fdv enctarget <in.yuv> <w> <h> <nframes> <bytes> <out.bin>  # QP-search to a size budget
./build/fdv ency4m    <in.y4m> <qp> <out.bin>                       # encode a 4:2:0 Y4M clip
./build/fdv decy4m    <in.bin> <out.y4m>                            # decode to Y4M
./build/fdv compare   <a.yuv> <b.yuv> <w> <h> <nframes>             # per-plane PSNR
./build/fdv bench     [w] [h] [nframes] [qp] [iters]                # encode/decode throughput
./build/fdv selftest  [w] [h] [nframes] [qp]                        # in-memory round trip
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

Typical workflow: `enc` → `dec` → `compare` the decoded output against the
original to measure quality, or `enctarget` to hit a byte budget directly.

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
- **Intra:** a rate-distortion quadtree over 16×16 coding tree units — one
  16×16 prediction, or four 8×8 nodes, or sixteen 4×4 leaves, whichever costs
  less. 9 H.264-style directional modes at 4×4; DC, vertical, horizontal and
  H.264's plane fit at 8×8 and 16×16.
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
- **Tooling:** `fdv` CLI (`.fdv` streams, raw I420 + Y4M, size-targeted rate
  control, PSNR compare, self-test) and `fpl`, a raylib player.
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
`FDV_AD_CAP` (set it to 0 to build without the adaptive coder), `FDV_AD_SCALE`,
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
| 720p encode | 17.0 ms | 17.3 ms | 56.3 ms |
| 720p decode | **0.58 ms** | **0.74 ms** | **2.30 ms** |
| 1080p encode | 25.3 ms | 20.3 ms | 104.6 ms |
| 1080p decode | **1.10 ms** | **1.42 ms** | **4.69 ms** |

Decode got faster and encode got slower, both for the same reason: the intra
quadtree tries three block sizes where there used to be one, and the decoder
then has fewer, larger blocks to reconstruct. Deblocking on the 8x8 grid
accounts for the rest of the decode side.

(1080 is not a multiple of 16, so "1080p" here is 1920x1088 — the same padding
real encoders code and then crop. See *Limitations*.)

An earlier version of this table quoted a single mean — 3.1 ms encode and
0.57 ms decode at 720p — measured on the easiest scenes in the library. Treat
any single figure here with suspicion: `plaza` costs five times what `motion`
does to decode and eleven times what `still` costs to encode, and real camera
content sits at the hard end, not the easy one.

Of the decode figures, the adaptive entropy coder accounts for **+26% to +56%**,
scaling inversely with the gain it buys — see *The entropy model was always
transmitted*. It costs nothing measurable to encode: the encoder codes each
frame both ways to choose, and that second pass disappears against motion
search.

The **key frame** is where that lands hardest. P-frames bound how much they
decode serially with `FDV_AD_CAP`; an intra frame has no such cap — all four of
its streams are adaptive, tens of thousands of symbols each — so a 720p key
frame goes from 3.2 ms to 11.5 ms, roughly tripling the worst frame in the
stream. At one key frame in sixty it barely moves the average, and 11.5 ms is
still a third of a 30 fps frame budget, but it is the worst-case number to know
if you are decoding to a deadline. `FDV_AD_CAP=0` turns the whole thing off.

Threaded, 8 bands on 8 threads, decode drops to roughly a quarter of the
single-stream figure. It is not free: measured on `plaza` at 960x544 and equal
PSNR, bands cost **+2.3% of the bitrate at 2 bands, +8.3% at 4 and +14.9% at
7**, which is why the comparisons above are all single-stream.

That penalty is a design choice this codec has not yet revisited. A band here is
a *fully independent video* -- its own key frames, and a reference frame that is
only its own rows with replicated borders -- so motion crossing a band edge
predicts from replicated pixels rather than real ones. Three internal boundaries
at four bands affect roughly 9% of the frame, which is very close to the 8.3%
measured.

HEVC's tiles break entropy state and intra prediction at a tile edge but let
motion compensation read the whole reference picture, because that picture is
already complete before the current frame starts. Doing the same here would
remove most of the penalty for the same parallelism. It is a bigger change than
it sounds, and the shape of it is worth writing down:

- The **container does not need to change.** It is band-major, but the streaming
  decoder already walks it frame-major with one cursor per band, which is the
  order shared references need.
- The **reference pool does.** It currently lives inside each band's decoder
  state, sized to that band. It has to become one full-size pool, rotated once
  per frame after every band has reconstructed into a shared frame — which
  means splitting per-band entropy and position state from the shared pool in
  four drivers (whole-file encode and decode, streaming encode and decode) and
  in the seek path.
- The **encode loop has to inverse**: frame by frame across bands, rather than
  band by band across frames.
- The **bitstream changes without the syntax changing**, so an old stream would
  decode into a wrong picture rather than fail. That needs a container version
  bump to be safe, which is the part that makes this all-or-nothing rather than
  incremental.

The band-aware coding loop itself is the easy half: `pframe_encode` and
`pframe_decode` need a row range and a band-local macroblock context, and the
intra path needs nothing at all, because a band is already exactly the
sub-rectangle the image codec takes.

Encode is ~50x faster than the first working version and decode ~4x, at equal
or slightly better quality. Four ideas did most of it.

**1. Prove what the answer must be, instead of searching faster.** Every coding
mode has a floor: the syntax it must emit even when every coefficient quantizes
to zero. A 16x16 residual is four 8x8 regions, each needing a transform-size
flag plus at minimum an empty-coefficient byte.

| mode | floor |
|------|-------|
| INTRA | 3 mode + 3 sub-mode + 4x2 residual = **14 bits** |
| INTER 16x16 | 3 mode + 3 ref + 2 mvd + 4x2 residual = **16 bits** |
| INTER 8x8 | 3 mode + 4x2 mvd + 4x2 residual = **19 bits** |

(Those are estimated bits, not counted bytes -- see *The Lagrangian constant*.
The floors are derived from the same cost constants the decisions use rather
than written out as literals, because the exactness depends on them agreeing: a
floor left behind at an older scale would prune candidates that can in fact
win.)

A candidate whose floor already exceeds the best cost so far cannot win, so it
need not be evaluated at all. Since roughly 96% of macroblocks end up SKIP, most
of the mode search was provably dead work. This is **exact** — it prunes only
decisions it can prove, and the coded bitstream is unchanged byte for byte,
verified scene by scene against a build with the pruning disabled. Worth ~10x.

**2. Parallelism hiding inside something "serial".** Intra prediction is
usually called serial because each block reads its neighbours' reconstructed
pixels. But the dependency is narrow: a block needs the row above only as far
as one column to its right. That is a wavefront — once row `r-1` is two blocks
ahead, row `r` can run alongside it, and a 480-column row keeps eight rows in
flight. Output is identical; only the symbol streams need care, since appending
to a shared cursor is the one thing that genuinely cannot be parallel, so each
row writes its own slice and they are reassembled in raster order.

The first attempt scaled badly. The per-row progress counters were adjacent
4-byte atomics — the same cache line — and neighbouring rows are exactly the
ones that poll each other, so the line ping-ponged between cores on every block.
Padding each counter to its own cache line was the whole difference.

**3. Compute only what the answer needs.** The quarter-pel interpolator
evaluated every half-pel intermediate and returned one of them; dispatching on
the phase first cuts eleven of sixteen phases to at most two filters, and doing
a block at a time makes the filter separable so each tap is computed once.
RDOQ's candidates form a chain, so the trial block carries forward: O(n) where
it was O(n^2).

**4. SIMD where the arithmetic lines up.** Three NEON instructions are exactly
the rounding rules the format specifies — `vrshrq_n_s16(s,5)` is `(s+16)>>5`,
`vrshrq_n_s32(s,10)` is `(s+512)>>10`, `vrhaddq_u8` is `(a+b+1)>>1` — which is
what lets a SIMD interpolator stay bit-exact rather than merely close. The
deblocking filter vectorised because its four samples sit adjacent at every
fourth column, precisely what `vld4q_u8` deinterleaves, and its per-edge test
becomes a mask instead of a branch; that halved decode, where deblocking had
been 50% of the time.

### What this cost

Two changes are heuristics rather than proofs, and both were measured before
being kept.

*Intra mode screening.* Predict all nine modes, rank by SATD, give full
rate-distortion evaluation only to the best few. Ranking by plain SAD instead
costs **14.8 dB** on a fine checkerboard — SAD cannot see that a pixel-wise
close prediction may leave a residual the transform handles badly. And the
screen must be off below QP 12, where near-lossless reconstruction leaves the
modes nearly tied on distortion and SATD can rank but not choose; without that
gate QP 0 lost 4.6 dB. With both guards the cost is about a third of a dB on
that checkerboard and nothing elsewhere.

*Cross-shaped sub-pel refinement* (four probes per stage instead of eight).
Each probe is a full motion compensation plus a SAD, and this is where most of
the encoder's motion-compensation calls come from. Measured at ±0.02 dB, with
*smaller* streams on two of three scenes.

Across the eight built-in scenes the net effect of the whole optimisation round
is −0.06 to +0.14 dB, with coded size slightly smaller on seven of eight.
`INTRA_RD_CANDIDATES`, `INTRA_PRUNE_NUM`/`_DEN` and `ME_SUBPEL_PTS` in
`include/fdv.h` restore the exhaustive behaviour.

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
  slower decode — see the section on it. Extending the *intra* tree past two
  levels to a 32×32 coding tree unit is untried and is the more promising half.
- **Sub-8×8 motion partitioning** is not there, and the mode mix says it would
  not pay: INTER-8×8 is already only 0.1% to 1.5% of macroblocks.
- **The rate estimate is a stand-in, and that turns out to be fine.**
  Exp-Golomb lengths for coefficients, constants for structure symbols. The
  two-pass measured alternative was built and measured at roughly zero — see
  *The Lagrangian constant* for why, and for the one case where it does help.
- **Tiles are fully independent videos**, so band seams cost +8.3% of the
  bitrate at four bands. HEVC's tile semantics — break entropy and intra
  prediction at the edge, but let motion compensation read the whole reference
  picture — would remove most of that, and needs the encode loop inverted.
- **No B-frames or hierarchical GOP.** Probably the largest single coding gain
  still on the table, and not a fast-decode trade: bi-prediction is a SIMD
  average with no serial dependency.
- The 8×8 transform uses a fixed-point matrix multiply (int64 inverse), not yet
  a multiply-free butterfly.
- SIMD is **NEON (arm64)** only; an AVX2 path for x86 decode is future work.
- On very small frames the per-frame header dominates, so the entropy and
  transform-size gains mostly show at realistic resolutions. Frequency tables
  are much less of that than they were — most streams now carry no table at
  all — but the per-stream symbol counts remain.

## License

Experimental / educational. No warranty.
