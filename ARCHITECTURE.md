# fdv — Fast Decode Video

A from-scratch experimental video encoder/decoder. No x264/ffmpeg, no
standard-compatibility goal. The organizing constraint is **decode speed on
modern CPUs**, and every design fork is resolved in favor of the decoder.

---

## 1. North star: decode is the asymmetric path

A codec is intentionally asymmetric. Video is encoded once (offline, many cores,
slow is fine) but decoded billions of times (phones, browsers, battery). So
"fast decode" is not a feature — it is the constraint that drives the design.
At every fork between *compresses a bit better* and *decodes faster*, we choose
decode.

The enemy of decode speed is the **serial dependency**. Two classic offenders we
deliberately design around:

- **Adaptive arithmetic coding** (CABAC, as in H.264/HEVC): each symbol's decode
  depends on the previous symbol updating the probability model. Inherently
  serial — no SIMD, hard to parallelize. This is the one we ended up buying
  back, deliberately and in a bounded way: adaptation is worth 22-29% on cheap
  frames, far more than "a bit better", so it is applied to the streams short
  enough that decoding them serially costs a fraction of a millisecond, and the
  bulk streams stay on parallel rANS. The rule is a cap, not a hope — see §4.
- **Intra-prediction chains**: a block's prediction reads its neighbor's
  *reconstructed* pixels, serializing block decode.

So the architecture is organized around **breaking serial dependencies**:
table-driven entropy decode that interleaves, small fixed-size SIMD-friendly
transforms, and tile-based spatial parallelism.

### Locked design targets
- **Decode target:** pure software on modern CPUs — AVX2 / ARM NEON intrinsics,
  cache-friendly tiles, interleaved rANS. No hardware/ASIC constraints.
- **Goal milestone:** full video — encode→decode a short clip *with motion
  compensation*. The intra-only image codec is an internal checkpoint on the way,
  not the finish line.
- **Reference model:** P-frames only, with a **2-frame forward reference pool**
  (16x16 INTER RD-picks and signals the reference; SKIP/8x8/INTRA use the
  nearest). Optional periodic **key frames** (`keyint`): frame f is intra when
  f%keyint==0, and the reference pool resets at each key frame so P-frames never
  reference across it (seek/error-resilience). No frame reordering, no B-frames.
- **Block structure:** 16×16 coding tree units. Intra picks its block size from
  a rate-distortion quadtree down to 4×4; motion is still fixed 16×16 with an
  8×8 split. Depth is capped at two levels deliberately — a quadtree is a serial
  recursion per block, and the measured gain is nearly all in the first two
  levels.

### The trade-off, stated honestly
Fast-decode choices cost compression efficiency: fewer intra modes and shallow
partitioning leave bits on the table. We are explicitly buying decode speed and
parallelism with bitrate.

The corollary is that not every gap is a fast-decode gap, and it is worth being
honest about which is which. The largest one found so far was neither the
entropy coder nor the block size in itself: it was that the encoder's *rate
estimate* charged eight bits for every byte it emitted, so it preferred coding
choices that produced fewer bytes over ones that produced cheaper bytes. That
cost nothing while every block was 4×4, and −23% BD-rate once the quadtree gave
it something to choose between. A decode-speed constraint did not put it there
and does not keep it there.

"Static rANS tables lose a sliver vs. fully-adaptive CABAC" is what this
document used to say, and it was wrong by an order of magnitude. Measured, a
transmitted static model costs **22-29% of a cheap frame** against a model that
adapts as it codes. That is not a sliver, and it was the single largest
compression gap left in the codec.

What it is *not* is an argument for CABAC. Adapting from generic initial states
measures +89% to +140% worse here -- these streams are short and violently
skewed, so a model that starts uniform never earns back what it spends learning.
The part worth taking is adaptation primed from history both sides already hold,
and it is taken only for streams short enough that decoding them serially is
affordable. See *The entropy model was always transmitted* in the README.

---

## 2. The decode pipeline (data flow)

```
bitstream
   │
   ▼
[1] demux / headers ──────────► sequence + frame + tile params
   │
   ▼
[2] entropy decode (rANS) ────► quantized coeffs, modes, motion vectors
   │
   ▼
[3] dequantize ───────────────► transform-domain coefficients
   │
   ▼
[4] inverse transform ────────► residual (spatial domain)
   │                                    │
[5] prediction                          │
   ├─ intra: from neighbor pixels       │
   └─ inter: ref frame + MV + interp    │
   │                                    │
   ▼                                    ▼
[6] reconstruct ─────────────► predicted + residual = pixels
   │
   ▼
[7] in-loop filter (deblock) ─► clean frame
   │
   ├──► output
   └──► reference frame pool (for future inter frames)
```

The encoder is this pipeline run backwards, plus the expensive search: mode
decision, motion estimation, rate-distortion optimization (RDO), rate control.

**Every video codec contains an image codec.** Steps 1–7 minus inter prediction
*is* a still-image codec. That is our bootstrap.

---

## 3. Building blocks (modules)

| # | Module | Job | The decision that matters for fast decode |
|---|--------|-----|-------------------------------------------|
| 1 | Frame/plane buffers | Planar YUV storage, strides, alignment, padded borders, reference pool | SIMD-aligned planes; padded borders so motion comp never branches on edges |
| 2 | Bit I/O | Raw bit reader/writer, byte alignment, 64-bit refill | Foundation for #3 |
| 3 | Entropy coder | Symbols ↔ bits | **rANS, interleaved, static per-tile tables** — the whole ballgame for decode speed |
| 4 | Transform | Spatial ↔ frequency | Separable **integer** DCT (4×4, 8×8), butterfly add/shift, bit-exact |
| 5 | Quantization | Lossy step: coeffs → small ints | Quant matrices + QP; dead-zone quant |
| 6 | Intra prediction | Predict block from same-frame neighbors | Few modes (DC/planar/angular), pipeline-friendly |
| 7 | Inter prediction | Predict block from reference frames | MVs, sub-pel interpolation (short filters), ref management |
| 8 | In-loop filter | Hide block artifacts | Deblock (+ optional CDEF-style); in-loop so refs stay clean |
| 9 | Partitioning | Split frame into coding/transform blocks | SIMD-aligned; quadtree capped at two levels, not deep |
| 10 | Bitstream syntax | Headers + container | Defines the format; tiles for parallelism |
| 11 | Tiling / threading | Independent regions decode in parallel | Reset entropy + prediction at tile edges |
| 12 | Encoder search/RDO/rate ctrl | Pick modes, motion, bit allocation | Encoder-only; slow is fine |

---

## 4. v0 spec (deliberately simple, room to grow)

- **Pixels:** YUV 4:2:0, 8-bit, planar. (10-bit later.)
- **Block grid:** 16×16 coding tree units; intra splits by RD quadtree to 8×8
  and 4×4, motion stays 16×16 with an 8×8 split. Transforms at 4×4 and 8×8,
  chosen by RD per aligned 8×8 region.
- **Transform:** separable integer DCT, 4×4 first. Bit-exact forward + inverse,
  round-trip tested.
- **Entropy:** two coders, chosen per stream per frame by coding both ways and
  keeping the smaller. Interleaved rANS with static per-frame frequency tables
  carries the bulk streams: branch-light and parallelizable, the core "new
  generation fast decode" bet. Streams under `FDV_AD_CAP` symbols instead go to
  an adaptive range coder primed from decoded history, which transmits no model
  at all — worth 22-29% on cheap frames, at the price of decoding those streams
  serially. The cap is what keeps that price bounded. Key frames use the same
  coder from a flat prior, since they must decode standalone and so have no
  history to prime from; their streams are long enough that learning from
  uniform costs nothing.
- **Prediction:** intra by RD quadtree — nine H.264 directions at 4×4, and
  DC / vertical / horizontal / plane at 8×8 and 16×16 — then translational
  inter, **P-frames only** (two forward refs, ¼-pel, short interp filter).
- **Loop filter:** simple deblock, added after reconstruct works.
- **Parallelism:** tiles designed into the bitstream syntax from day one, even
  before we thread it.

---

## 5. Build order (dependency-driven)

1. **Frame/plane buffers** — the substrate. ✓ `FRAME`
2. **Integer transform + quant** — round-trip tested across QP. ✓
   `TRANSFORM`
3. **Bit I/O → rANS entropy coder** — round-trip tested (single + interleaved). ✓
   `BITS`, `RANS`
4. **Intra prediction + reconstruct** → first end-to-end still-image codec,
   bit-exact, 74 dB @ QP0. ✓ `INTRA`, `IMAGE`
5. **Deblocking filter** — in-loop, cuts boundary energy ~74%, lifts P-frame
   PSNR. ✓ `DEBLOCK`
6. **Inter prediction + reference management** → *it is now a video codec*.
   P-frames, quarter-pel MC, motion search, no drift. ✓ **(Goal milestone.)**
   `INTER`, `VIDEO`
7. **Tiling/threading, then encoder RDO/rate control** — independent tiles,
   bit-exact threaded decode, QP rate control + Lagrangian RDO. ✓
   `TILES`, `RC`

**Status: all seven building blocks complete.** 14 test suites pass, clean build
with `-Wall -Wextra -Wshadow -pthread`. `make test` runs the whole suite;
`make fuzz` re-runs the decoder fuzz harness *and* the unit suite under
AddressSanitizer + UndefinedBehaviorSanitizer, with `-fno-sanitize-recover` so
undefined behaviour aborts instead of printing a line and carrying on.

### Known simplifications / future work
- Video path is **full YUV 4:2:0** (✓): I-frames code all three planes, P-frames
  do chroma MC at luma-MV/2 with the luma SKIP/INTER mode.
- P-frame per-block RDO chooses among **SKIP / INTER 16x16 / INTER 8x8 / INTRA**
  (✓ J = D + λR; the 8x8 split diamond-searches each quadrant independently for
  divergent intra-MB motion). Sub-8x8 partitions and a full quadtree are future
  work.
- Motion estimation: **diamond search seeded from (0,0) and the neighbor-MV
  predictor** (✓ EPZS-style, O(iterations)). Richer predictor sets (collocated,
  accelerator MV) are future work.
- Luma sub-pel MC is the **H.264 6-tap half-pel filter** + quarter-pel averaging
  (✓); chroma stays bilinear. An 8-tap (HEVC-style) filter is possible future work.
- Intra is a **rate-distortion quadtree** over 16×16 coding tree units (✓
  `intra_node`): a unit is coded as one 16×16 prediction, or split into four
  8×8 nodes, each of which is one prediction or four 4×4 leaves. 4×4 leaves use
  the 9 H.264 directional modes; larger leaves use DC / vertical / horizontal /
  H.264's plane fit (`fdv_intra_nxn`). This is where the intra path's −23%
  BD-rate came from, and the reason is symbol count rather than modelling: a
  flat region now costs one mode symbol per 256 pixels instead of sixteen.
  Depth is capped at two levels; a 32×32 or 64×64 CTU is future work.
- **Rate estimates grow with magnitude** (✓ `fdv_bits_val`): the RD decisions
  charge an exp-Golomb length for a coefficient and small constants for
  structure symbols, rather than the flat eight bits per emitted byte they used
  to. The old model valued "fewer bytes" over "cheaper bytes", which cost
  nothing while every block was 4×4 and a great deal once the quadtree gave the
  encoder large blocks and 8×8 transforms to choose between. The exact
  mode-pruning floors are derived from the same constants so they stay exact.
  Rate measured from the frame's own coded statistics (a two-pass encode) was
  built and measured at roughly zero: exp-Golomb is the entropy of a geometric
  source, and coefficient levels are close to geometric. What had been wrong was
  the model's *shape*, not its accuracy.
- Motion partitioning is still **fixed 16×16 macroblocks** with an 8×8 split;
  the quadtree is intra-only so far.
- **NEON SIMD kernels** (✓ all bit-identical to scalar over random blocks):
  `fdct4x4`/`idct4x4`/`dequant4x4` (transform + dequant, decode hot path),
  `sad_kernel` (motion-search SAD, hottest encoder loop), and `mc_chroma`
  (bilinear chroma MC, 4/8-wide). AVX2 and a NEON 8x8 idct are future work.
- Coefficient coding uses an **end-of-block count prefix** (✓ emit only up to
  the last nonzero zigzag coeff; trailing zeros are free) — ~12–22% smaller than
  the naive all-16-levels scheme. Run-length between nonzeros and context-adaptive
  entropy are future work.
- **Context-adaptive entropy** (✓): symbols are routed into separate streams by
  what they are, and the encoder tries a single shared rANS model against
  per-stream models and emits whichever is smaller behind a 1-byte flag. On
  small frames the shared model wins (the second table outweighs the modelling
  gain); on large frames the split wins. Never regresses.
  The P-frame path (`VIDEO`) has fourteen streams: four macroblock-mode
  contexts, reference indices, both motion-vector components, intra sub-modes,
  merge indices, and the five coefficient streams. The intra path (`IMAGE`) has
  ten: split flags by node size, large-leaf modes, 4×4 leaf modes under the
  neighbour-agreement context, and the same five coefficient streams.
- **Coefficients are separated by transform size** (✓): the 4×4 and 8×8
  transforms have their own end-of-block counts and their own levels. An 8×8
  count runs 0..64 against the 4×4's 0..16, and an 8×8 transform concentrates a
  block's energy into much larger coefficients. Pooling them cost 32 KB on one
  high-rate intra frame *at an identical symbol count* — the same symbols, coded
  worse because two distributions shared a table. The transform-size region
  flags and chroma's coded-block flag were likewise riding in the 4×4 count
  stream; moving them to their own is worth a further −1.6% on both paths, with
  every scene improved and none regressed.
- **Compact frequency tables** (✓ `rans_write_freqs`/`rans_read_freqs`): the
  per-frame rANS model is transmitted as a count + (symbol, varint-freq) pairs
  over the nonzero alphabet instead of 256 fixed-width entries. Cut the video
  test ~69% (the 512-byte tables had dominated the small frames). Negligible at
  large frame sizes but free.
- **RDOQ-lite** (✓ `rdoq4x4`): after quant, trailing coefficients are dropped
  when D + λ·rate favors it — applied in both the intra (`IMAGE`) and inter
  (code_residual) paths. Full per-level trellis RDOQ is future work.
- **Transform-size adaptation in the inter path** (✓): `code_residual`/
  `decode_residual` choose per aligned 8x8 region between four 4x4 transforms and
  one 8x8 transform (`fdct8x8`+`coeff8`) by RD, signaled with a 1-byte flag
  (n==4 stays a single flagless 4x4). Helps smooth regions; on textured content
  the flag is small signaling overhead (the flag is common to both options, so
  it doesn't bias the choice). **Intra 8x8** in progress: the 8x8 intra
  predictor (`intra_nxn`, DC/V/H, ✓ tested) is in place; wiring the per-region
  4x4-vs-8x8 RD choice into `IMAGE` is the next step. The 8x8 DCT is not yet
  multiply-free (int64 inverse).
- **Decoder robustness against malformed/truncated bitstreams** (✓
  `tests/test_fuzz.c`, `make fuzz`): every value the decoder reads from an untrusted
  stream is now bounds-checked, so corrupt input yields a clean `-1` rather than
  an over-read, over-write, or crash. Specifically:
  - *Symbol streams* — `rd_byte`/`rd_count`/`rd_level` (bitpack) never read past
    the decoded buffer and reject out-of-range counts/modes/levels; the parse
    rejects ref-index > 1, EOB counts > 16 (4x4) / > 64 (8x8), and unknown modes.
  - *rANS tables/payloads* — `rans_read_freqs_bounded` refuses truncated tables,
    duplicate symbols, and frequencies that don't sum to `RANS_SCALE` (which
    would gap or overflow the slot table); `rans_decode_bounded` consumes at most
    the declared payload length, feeding zero bytes rather than over-reading.
  - *Headers* — every `get32`/dimension field is length-guarded; dimensions are
    capped (≤ 8192, correct multiple), `qp` is range-checked against the
    quant-table size (52 entries — an out-of-range `qp` would index `QSTEP8`
    out of bounds in `dequant8x8`), and the decoded symbol count is bounded by
    what the frame geometry allows, so a hostile header can't force an absurd
    `malloc`. I-frame plane sub-headers are checked to match the expected plane
    size before `image_decode` writes.
  - *Motion vectors* — a corrupt MV that would make `mc_luma`/`mc_chroma` read
    outside the reference's padded border is rejected (`mv_in_bounds`).
  - *Integer UB* — the zigzag encoders avoid shifting negatives, and
    `dequant8x8` saturates `level*QSTEP` into `int32`, so garbage coefficients
    can't trigger signed-overflow UB. Valid streams are unaffected (bit-exact).
  The fuzz harness feeds every truncation length plus 40,000 byte-flipped copies
  each of valid video, image, and tiled-video (`vtile`, threaded) streams
  (exact-size buffers) and asserts no crash; under `make fuzz` ASan+UBSan confirm
  zero memory errors and zero UB. The bounded rANS decode also stops
  renormalizing at the input end, so a corrupt stream that drives the state to
  zero can't spin forever — a decoder must never hang on malformed input.
  (Adding the tiled-container case is what surfaced the `qp` over-read above: a
  band sub-stream's `qp` byte lives in the container body, which the plain video
  fuzz had kept intact.)
- **Tile-parallel video decode** (✓ `VTILE`): a frame is
  split into horizontal bands of whole 16x16 MB rows, and each band is coded as
  an INDEPENDENT video sub-stream via the base codec (the `VIDEO` section) on a smaller
  frame. Because a band is just the verified codec run on fewer rows, round-trip
  correctness is inherited; because bands share no prediction/entropy/reference
  state, the decoder reconstructs them concurrently with one pthread per band
  group (the still-image path's tiling, lifted to video). Threaded output is
  bit-identical to serial, and a single-band container reproduces the plain
  video codec exactly. The trade-off is the usual one: prediction can't cross a
  band edge (each band's first frame is intra, motion search sees only
  band-local border-replicated references), so coding efficiency dips slightly
  near boundaries — a separate container, gated from the single-stream format,
  so existing streams are unaffected. Usable from the CLI via `enctiled`/
  `dectiled` (the latter takes a thread count) and exercised end-to-end by the
  `demo-vtile` Make target. v1 decodes each band into a temp buffer then
  scatters into the output planes; a strided-plane decode that writes in place
  (avoiding the copy) is future work.

Each step is independently testable. Step 4 is the first "it actually works"
moment — a complete intra-only image codec. Step 6 is the milestone.

---

## 6. Conventions

- Language: C (C11), no external deps.
- Install: `make install` puts `fdv`, `fpl` and `fcap` in `$(PREFIX)/bin`, the
  two headers in `$(PREFIX)/include` and the scenes in
  `$(PREFIX)/share/fdv/scenes`, `PREFIX` defaulting to `~/.local`.
  `make uninstall` reverses it.
- Build: `make`; tests are small round-trip harnesses. Output goes to `build/`.
- **Compile to `.o`, then link as a separate step.** On macOS clang runs
  `dsymutil` whenever a single invocation both compiles and links with `-g`,
  scattering `.dSYM` bundles through the tree. Splitting the two keeps full
  debug info (in the `.o` files, via the linker's debug map — lldb works) and
  emits no `.dSYM`. `-g` belongs in `CFLAGS`, never in `LDFLAGS`.
- Integer-only in the decode hot path; no float drift.
- Hot loops: data layout and block sizes chosen for AVX2/NEON.
- Shared serialization primitives (LEB varints, zigzag map, LE fields, 4x4 scan)
  live in the `BITPACK` section, so encode/decode sides cannot drift.

### File layout

The library is **one header**: `include/fdv.h`. Include it for the
declarations; define `FDV_IMPLEMENTATION` in exactly one translation unit per
program to pull in the code as well. Both halves are divided into the same
sixteen numbered sections (`LOG`, `FRAME`, `BITS`, `BITPACK`, `RANS`, `COEFF8`,
`TRANSFORM`, `INTRA`, `INTER`, `DEBLOCK`, `IMAGE`, `VIDEO`, `TILES`, `VTILE`,
`RC`, `FDV`) in the same order, marked by matching banner comments. The
build-order list in §5 is that order. `LOG` is numbered 0 because it is
cross-cutting: every other section may log, and it depends on nothing.

The scene renderer is deliberately *outside* that header, in
`include/fdv_scene.h`. It generates content to feed the codec and the codec
never calls it; it is also the only place file I/O lives. `fdv.h` opens no
file, which is what keeps a decoder's object code free of test-content and
path-handling machinery.

    include/fdv.h        the whole codec: encode, decode, container, profiling
    include/fdv_scene.h  scene description language + renderer, and all file I/O
    src/fdv.c            the fdv command-line tool, including the pipeline driver
    src/fpl.c            fpl, the raylib player for .fdv streams
    src/fcap.m           fcap, macOS camera capture (the only OS-dependent file)
    tests/test_enc.c     the whole unit-test suite, one binary
    tests/test_fuzz.c    decoder robustness harness
    scenes/              23 .scn files
    tools/               benchmark and BD-rate scripts
    build/               all objects and binaries; `make clean` removes it

The rule the sections encode: **nothing in a section depends on a section below
it.** Merging the former per-module files into one unit traded compiler-enforced
layering for that convention, so keep section order when editing, and keep a
section's file-local statics inside it. Helpers genuinely shared across sections
(`clipb`/`clip255`, `ceil_div`) live in the *shared internals* block at the top
of the implementation — one definition each, so the copies cannot drift the way
the per-file duplicates did.

### Streaming encode, and keeping the OS out

`fdv_video_encode` wants every frame up front. That suits a file on disk and
nothing else: a camera hands over one frame at a time and never says how many
are coming. `fdv_enc_*` is the incremental form, built the same way as the
streaming decoder — the state the encode loop carried between iterations moves
into a context.

One thing genuinely cannot be streamed: the frame count sits in the payload
header, and a recorder does not know it until it stops. So the *coded* stream
accumulates in memory and the count is patched in at the end. That is affordable
precisely because it is the coded stream and not the frames — a minute of 720p
at 800 kbps is about 6 MB, against 5 GB of raw. Output is byte-identical to
`fdv_video_encode` over the same frames, which the test suite requires.

### Rate control, and why the obvious design fails

Average-bitrate control is one knob -- the frame QP -- and the whole difficulty
is in not over-driving it. Two designs were built and the first one failed in an
instructive way.

The obvious one models frame cost as `bits * qstep^k`, smooths it, and inverts
it to pick a QP for a target. It is a feedback loop with a plant model, and it is
only stable if `k` is right. It is not: measured over QP 16..32 on camera content
the exponent runs between 1.0 and 1.34, and it is not constant even within one
clip. Whenever the assumed `k` is above the true one, the estimate *rises* with
QP -- so a frame that overspent asks for more QP, which makes the next estimate
larger still. The loop ran to QP 39 and stayed there. Sweeping `k` did not fix
it, because no single value is right everywhere.

The replacement has no plant model at all: correct the operating point by the log
of how far the last frame missed its target, with a gain below what the relation
needs. That cannot run away for any content, because the correction is bounded by
construction rather than by the accuracy of an estimate. It converges in a
handful of frames and the QP range across a clip fell from 25 steps to 9.

The second lesson was about allocation. Handing a key frame "five frames' worth
of budget" makes its allowance depend on how full the buffer happens to be when
it lands; at the start of a stream that produced a QP 41 key frame, and every
P-frame after it then spent bits repairing a bad reference -- filling the buffer,
raising QP further. Allocating over the whole key-frame interval instead, with
the key frame counted as ten P-frames, removes the coupling: the key frame's
share is a property of the interval, not of the moment.

Both of these are the same shape of mistake. A control loop whose gain depends on
an estimate is only as stable as the estimate, and an allocation that depends on
current state couples decisions that should be independent.

Threading the capture path needed no new codec work either, only a streaming
front end for machinery that already existed. `fdv_vtile_encode` had defined
independent horizontal bands since the tiling milestone, but only as a
whole-array call; `fdv_enc_open_tiled` owns one ordinary streaming encoder per
band and drives them a frame at a time, so its output is byte-identical to the
whole-array path at any thread count. That identity is the test, and it holds
because the split is spatial: bands never see each other, so the thread count
cannot change a decision.

This mattered more than any instruction-level tuning left in the encoder. A
P-frame is 29 of every 30 frames and was entirely single-threaded -- intra
frames had the wavefront, P-frames had nothing -- so a 14-core machine encoded
camera content on one core. Bands cost a few percent of bitrate at the edges,
which is the usual tiling trade and the right one when the encoder is what sets
a capture's frame rate.

`fdv_enc_repeat` is the one piece of this that had to live in the library. It
writes an all-SKIP frame without consulting mode decision, and the reason is a
property of RD that only shows up at a low qp: the Lagrangian comparison stops
choosing SKIP for a byte-identical frame once lambda is small enough that coding
the reference's own quantization noise looks worthwhile. At QP 24 re-encoding
identical pixels costs 0.7 ms; at QP 10 it costs 26.5 ms, most of a fresh
frame. A recorder holding a slot *to keep time* would fall further behind for
doing so and spiral. Forcing the mode is not a heuristic here -- for a frame
that is by construction identical to its reference, SKIP is the answer mode
decision should have reached, arrived at without paying for the search.

The capture side owns one problem the library does not: **when** a frame was
taken. Frames arrive irregularly whenever the encoder is slower than the camera,
and a constant-rate format cannot represent that, so `fcap` resamples onto a
fixed grid from the presentation timestamps and repeats the previous frame for
any slot that came up empty. Repeats are all-SKIP and cost a few hundred bytes,
which is what makes the constant-rate container the right call rather than a
compromise — the alternative would be per-frame durations in the bitstream, paid
for by every decoder forever, to describe a problem that only recording has.

**Capture is where the operating system stops.** `src/fcap.m` is the only file
in the project that is not portable C, and the boundary is deliberately narrow:
AVFoundation's job ends at an I420 frame, and everything past that is the same
library calls any other program makes. The codec should be portable even though
capture cannot be, so the split is kept strict rather than convenient — no
`#ifdef __APPLE__` anywhere in `fdv.h`, and the build simply skips `fcap` off
Darwin.

### Streaming decode

`fdv_decode` takes a whole file and returns a whole sequence, which needs all of
it resident — fine for a test harness, useless for anything long. `fdv_dec_*` is
the incremental form, and the change is smaller than it sounds: the state
`fdv_video_decode` carried on its stack between loop iterations (the two-frame
reference pool, the stream position, how many references are valid yet) moves
into a context, and the loop body becomes a function.

A tile-parallel file needs no special handling. Its bands are already
independent sub-streams over shorter frames, so the decoder holds one
sub-decoder per band and steps them together, scattering each into its rows.
That path is serial across bands where the whole-file path threads them: at one
frame per display refresh threading buys nothing, and the threaded route still
exists for whole-file work.

Seeking is bounded by the key-frame interval, as it must be. A P-frame is
defined against its predecessors, so landing on one means replaying from the key
frame at or before it — the decoder skips the blobs before that key frame
without decoding them, then decodes forward with the output discarded. Seeking
*forward* from the current position rolls on rather than replaying.

The property worth testing is that none of this changes the picture:
`tests/test_enc.c` decodes a clip both ways and requires the frames to be
identical, sequentially and after seeking backwards to every frame in turn, at
two key-frame intervals.

### The .fdv container

The coded streams carry no magic, no frame rate, and the single-stream and
tile-parallel forms begin with *different* fields — so a file on its own could
not say what it was or how fast to play it. Section 15 wraps them in a 12-byte
head (magic, version, kind, fps, payload length) and reads the geometry back out
of whichever payload header the kind selects.

Recording the kind rather than sniffing it is deliberate. The two headers are
distinguishable in practice — both start with 16-bit fields that are multiples
of 16 for dimensions — but only in practice, and a container that guesses is a
container that eventually guesses wrong on someone's file.

`fdv_decode` dispatches on the kind so callers never repeat that logic, and
`fdv_read` applies the same bounds the decoders enforce (dimensions, QP range,
declared payload length against the actual file) so a caller can size buffers
from the struct *before* decoding anything. That last part matters: the player
allocates for `nframes * w * h * 3/2` on the strength of a file header, which is
exactly the sort of number an attacker would like to choose.

### Instrumentation

Decode speed is the constraint, so the instrumentation is tiered by what it
costs:

- **Frame and stage logging** (`FDV_LOG_INFO`, `FDV_LOG_FRAME`; `-v` / `-vv`)
  is emitted once per frame or per stage, behind a level test. One predictable
  branch per frame is free at any resolution, so it is always compiled in.
- **The statistics sink** (`fdv_stats_set`) fills a caller-owned array with one
  record per frame: coded size, macroblock mode tallies, which entropy model
  won, symbol counts. Also per-frame, also always compiled in. It is a plain
  global with no locking, so `vtile_decode` suspends it around its worker
  threads rather than let band decodes race on it.
- **Per-macroblock tracing** (`FDV_LOG_BLOCK`; `-vvv`) is emitted from inside
  the block loops, where a per-block branch would be a real cost. It is behind
  `-DFDV_TRACE` and compiles to nothing otherwise; `make trace` builds
  `build/codec-trace` with it enabled.

The rule: anything per-frame may be a runtime check, anything per-block must be
compile-time. `make bench` is the check that this held — decode throughput and
the coded bitstream size are both unchanged by the instrumentation.

### Encoder speed

Profiled, not guessed — and after the first round, profiled from *inside*.
`make profile` compiles in a zone profiler whose zones nest, so each stage
reports self and inclusive time. That distinction is the point: motion search
measured 10% self and 31% inclusive, which says its cost is really motion
compensation. Call counts are often the more useful column.

Timing costs ~18 ns per zone pair, so zones sit at block and frame granularity,
never around a per-pixel kernel; the report states its own overhead and flags
rows whose per-call cost is close to it. Profile the encoder alone (`enc enc
... --profile`) rather than the pipeline, or scene rendering and PSNR
verification land in "unaccounted".

Cumulative: 1080p encode 6.0 ms/frame, decode 1.2 ms; 720p 3.1 ms and 0.57 ms.
Roughly 50x on encode and 4x on decode from the first working version, at equal
or slightly better quality across all eight built-in scenes.

**The largest wins were arguments, not instructions.**

*Rate floors.* Every mode must emit some syntax even when every coefficient
quantizes to zero — 80 bits for INTRA, 96 for INTER 16x16, 136 for INTER 8x8.
A candidate whose floor exceeds the best cost so far cannot win, so it need not
be evaluated. With ~96% of macroblocks ending as SKIP, most of the mode search
was provably dead work. Exact: the bitstream is unchanged byte for byte, checked
scene by scene against a build with the pruning disabled. Worth ~10x.

*The wavefront.* Intra prediction is described as serial because each block
reads reconstructed neighbours, but the dependency only reaches one column right
on the row above. Once row r-1 is two blocks ahead, row r can run beside it.
Bit-exact; the only real constraint is that appending to a shared symbol cursor
cannot be parallel, so each row writes its own slice and they are reassembled in
raster order. **It scaled badly at first** because the per-row progress counters
were adjacent 4-byte atomics sharing a cache line, and neighbouring rows are
exactly the ones that poll each other. Padding each to its own line was the
whole difference — worth remembering that a wavefront's synchronisation array is
a false-sharing trap by construction.

*Everything else* was computing less: phase-dispatch and block-separable
interpolation, incremental RDOQ (O(n) not O(n^2)), integer distortion sums, and
SIMD where the format's rounding rules happen to be single NEON instructions
(`vrshrq_n_s16(s,5)` == `(s+16)>>5`, `vrhaddq_u8` == `(a+b+1)>>1`). Deblocking
vectorised via `vld4q_u8`, which deinterleaves exactly the four samples an edge
needs; it had been 50% of decode.

Two heuristics were kept, both measured first: SATD-screened intra mode
selection (see README for the numbers, and for the two guards that make it safe)
and a cross-shaped sub-pel refinement at ±0.02 dB.

Thread budget matters: the intra wavefront runs inside plane-parallel intra
frames, so eight threads for luma and two per chroma plane is the measured peak
on a 14-core box — past that the spin-waits cost more than the parallelism
returns.

Still open: the 8x8 inverse transform is an int64 matrix multiply rather than a
butterfly, and it is computed for every region even when the 4x4 option wins
(that trial buys about 0.7 dB for 3.4% rate, so it earns its place, but not
cheaply).

### Generating content

`include/fdv_scene.h` turns a text scene description into a deterministic I420 clip, so
the codec can be exercised without sourcing test footage. It matters for more
than convenience: the scene files each target a specific encoder path, and
the pipeline's per-frame mode tallies confirm they actually reach it. The
`divergent` scene, for instance, stacks two 8x8 blocks with opposed motion
inside each 16x16 macroblock, and the encode log shows 24-44 macroblocks per
frame taking the 8x8 split — a claim that is checked rather than asserted.

Rendering is a pure function of (scene, frame index): no history, no `rand()`,
everything derived from an explicit seed. Two runs produce identical bytes, so
PSNR and bitrate comparisons across runs and machines mean something.

## 7. Usage

The `fdv` tool (`src/fdv.c`) makes the codec runnable on real files (planar I420):

```
make
./build/fdv scenes                                    # list built-in scenes
./build/fdv pipeline motion                           # render+encode+decode+verify
./build/fdv gen <scene> <out.yuv|.y4m>         # render a clip
./build/fdv enc <in.yuv> <w> <h> <nframes> <qp> <out.bin>   # encode raw I420
./build/fdv enctarget <in.yuv> <w> <h> <nframes> <bytes> <out.bin>  # QP-search to a size budget
./build/fdv dec <in.bin> <out.yuv>                          # decode back to I420
./build/fdv enctiled <in.yuv> <w> <h> <nframes> <qp> <band_mbrows> <out.bin> [keyint] [threads]  # tile-parallel encode
./build/fdv dectiled <in.bin> <out.yuv> [threads]           # tile-parallel decode (default 4 threads)
./build/fdv ency4m <in.y4m> <qp> <out.bin>                  # encode a 4:2:0 Y4M clip
./build/fdv decy4m <in.bin> <out.y4m>                       # decode back to Y4M
./build/fdv compare <a.yuv> <b.yuv> <w> <h> <nframes>       # per-plane PSNR between two clips
./build/fdv selftest [w] [h] [nframes] [qp]                 # in-memory round trip
./build/fdv bench [w] [h] [nframes] [qp] [iters]            # encode/decode throughput
make demo / make demo-y4m / make demo-vtile / make bench   # self-test / Y4M / tiled / throughput
```

Measured throughput (320×192, this arm64 box): decode ~340 Mpix/s (~5500 fps),
encode ~3 Mpix/s — the ~100x asymmetry the design targets.

`make test` runs the unit-test suite (14 sections plus the fuzz harness);
`make` builds the tests and the `fdv` tool into `build/`.

_This is a living document. Update it as decisions change._
