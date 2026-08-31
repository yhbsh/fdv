/* fdv — Fast Decode Video.  A clean-sheet video codec, in one header.
 *
 * A codec is encoded once and decoded billions of times, so every design fork
 * here is resolved in favour of the decoder: table-driven branch-light rANS
 * instead of CABAC, small SIMD-friendly integer transforms, and tiles that
 * decode in parallel.  At 720p a frame encodes in ~3 ms and decodes in ~0.6 ms.
 *
 * ------------------------------------------------------------------ usage --
 *
 * This is a single-header library in the usual style.  Include it wherever you
 * need the declarations; in exactly ONE translation unit per program, define
 * FDV_IMPLEMENTATION first to pull in the code as well:
 *
 *     #define FDV_IMPLEMENTATION
 *     #include "fdv.h"
 *
 * Every other file just does #include "fdv.h".
 *
 * Link with -lm and -pthread.  Nothing else: no external dependencies.
 *
 * --------------------------------------------------------------- switches --
 *
 *   FDV_IMPLEMENTATION   emit the implementation in this translation unit
 *   FDV_PROFILE          compile in the zone profiler (fdv_profile_report).
 *                        Off by default: it reads a clock per block boundary
 *   FDV_TRACE            compile in per-macroblock trace logging at
 *                        FDV_LOG_BLOCK.  Off by default for the same reason
 *   FDV_SCENE_DIR        where scene files are installed, as a string literal
 *   FDV_STATIC           give every function internal linkage, for embedding
 *                        two independent copies in one program
 *
 * Define these before the include, and consistently across the program: the
 * first two change the declarations, not only the code.
 *
 * ------------------------------------------------------------------- what --
 *
 * Sections run in dependency order, bottom-up; nothing in one depends on a
 * section below it.  That is a convention rather than something the compiler
 * enforces, so keep the order when editing.
 *
 *   0. LOG        debug logging, per-frame statistics, zone profiler
 *   1. FRAME      padded, aligned planar YUV buffers            (the substrate)
 *   2. BITS       MSB-first bit reader/writer
 *   3. BITPACK    LEB/zigzag/little-endian/scan primitives
 *   4. RANS       range-ANS entropy coder
 *   5. COEFF8     8x8 zigzag + end-of-block coefficient coding
 *   6. TRANSFORM  4x4 + 8x8 integer transforms, quant, RDOQ        (+NEON)
 *   7. INTRA      intra prediction
 *   8. INTER      motion compensation + motion search              (+NEON)
 *   9. DEBLOCK    in-loop deblocking filter                        (+NEON)
 *  10. IMAGE      intra-only still-image codec
 *  11. VIDEO      P-frame sequence codec
 *  12. TILES      independent still-image tiles, threaded decode
 *  13. VTILE      tile-parallel video, threaded encode and decode
 *  14. RC         rate control / Lagrangian QP selection
 *  15. FDV        self-describing .fdv file container
 *
 * Content generation lives in fdv_scene.h, not here. This header is an encoder,
 * a decoder and the instrumentation for both: it opens no files and knows
 * nothing about where frames come from.
 */

#ifndef FDV_H
#define FDV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* ===========================================================================
 * 0. LOG
 * Debug logging and per-frame statistics.
 * ======================================================================== */

/* Verbosity.  Decode speed is this codec's whole point, so the instrumentation
 * is deliberately coarse: everything below is per-frame or per-stage, never
 * per-block, and every call site is behind a level test that costs one
 * predictable branch per frame.  FDV_LOG_BLOCK is the exception — it is emitted
 * from the per-macroblock loops, so it is compiled in only under -DFDV_TRACE
 * (see `make trace`) and vanishes entirely from a normal build. */
enum {
    FDV_LOG_OFF   = 0,   /* silent (default) */
    FDV_LOG_INFO  = 1,   /* one line per stage: sizes, totals, timings */
    FDV_LOG_FRAME = 2,   /* one line per frame: type, bytes, mode mix */
    FDV_LOG_BLOCK = 3,   /* per-macroblock detail; needs -DFDV_TRACE */
};

extern int fdv_log_level;

/* Send log output to `out` (NULL means stderr) at the given level. */
void fdv_log_set(int level, FILE *out);


/* ---- Terminal presentation ----------------------------------------------
 *
 * Colour only when the destination really is a terminal, and never when
 * NO_COLOR is set. Piping output somewhere therefore yields plain text a script
 * can still parse, which matters here because the benchmark tooling parses it.
 * FDV_COLOR=1 forces it back on, for the usual case of piping into a pager. */
typedef struct {
    const char *dim, *bold, *red, *grn, *yel, *cyn, *mag, *rst;
} fdv_palette;
void fdv_palette_for(FILE *f, fdv_palette *p);

/* A bit rate, never a size: "847 kbps", "1.20 Mbps". Sizes in megabytes tell a
 * reader nothing about whether a stream fits down a wire, which is the only
 * question anyone actually has about a coded stream. */
const char *fdv_rate(char *buf, size_t n, double bits_per_second);

/* A duration: "1.2 s", "340 ms". */
const char *fdv_dur(char *buf, size_t n, double seconds);

/* Emit one log line, unconditionally.  Prefer the FDV_LOG macro, which skips
 * the call (and the argument evaluation) when the level is not enabled. */
void fdv_logf(const char *tag, const char *fmt, ...);

#define FDV_LOG(lvl, tag, ...)                                            \
    do { if (fdv_log_level >= (lvl)) fdv_logf((tag), __VA_ARGS__); } while (0)

/* Per-macroblock tracing: absent unless built with -DFDV_TRACE. */
#if defined(FDV_TRACE)
#define FDV_TRACE_LOG(tag, ...) FDV_LOG(FDV_LOG_BLOCK, (tag), __VA_ARGS__)
#else
#define FDV_TRACE_LOG(tag, ...) ((void)0)
#endif

/* Zone timing.  FDV_ZB/FDV_ZE must pair within one scope and one thread. */
#if defined(FDV_PROFILE)
void fdv_zone_begin(int zone);
void fdv_zone_end(int zone);
#define FDV_ZB(z) fdv_zone_begin(z)
#define FDV_ZE(z) fdv_zone_end(z)
#else
#define FDV_ZB(z) ((void)0)
#define FDV_ZE(z) ((void)0)
#endif

/* What one coded frame cost, filled in by fdv_video_encode / fdv_video_decode when a
 * sink is installed.  The mode counts are macroblock tallies and are encoder-
 * side only (the decoder does not re-derive them). */
typedef struct {
    int    index;           /* frame number within the sequence */
    int    is_intra;        /* I-frame (1) or P-frame (0) */
    int    qp;              /* the quantizer this frame actually used */
    size_t bytes;           /* coded size of this frame's blob */
    int    mb_skip;         /* macroblocks coded as SKIP      */
    int    mb_inter16;      /*                     INTER 16x16 */
    int    mb_inter8;       /*                     INTER 8x8   */
    int    mb_intra;        /*                     INTRA       */
    int    entropy_mode;    /* how the frame was entropy coded; fdv_emode_name */
    size_t sym_struct;      /* structure symbols emitted   */
    size_t sym_coeff;       /* coefficient symbols emitted */
    double ms;              /* wall-clock time to code (or decode) this frame */
} fdv_frame_stat;

typedef struct {
    fdv_frame_stat *frame;  /* caller-owned array */
    int             cap;    /* its capacity */
    int             n;      /* frames recorded so far */
} fdv_stats;

/* Install (or clear, with NULL) the stats sink.  Single-stream paths only:
 * fdv_video_encode/fdv_video_decode fill it, and fdv_vtile_decode suspends it while its
 * worker threads run, since the sink is a plain global with no locking. */
void      fdv_stats_set(fdv_stats *sink);
fdv_stats *fdv_stats_get(void);

/* --- zone profiler ------------------------------------------------------
 * Where the CPU time actually goes, measured from inside the codec rather
 * than sampled from outside.  Zones nest: each reports inclusive time and
 * self time (inclusive minus the zones it contains), which is what makes a
 * profile actionable — motion search's inclusive time is mostly motion
 * compensation, and only self time says so.
 *
 * Reading a clock costs tens of nanoseconds, so zones sit at block and frame
 * granularity, never around a per-pixel kernel, and the whole thing compiles
 * to nothing unless built with -DFDV_PROFILE (`make profile`).  The report
 * states its own measured overhead so the numbers can be read honestly. */
enum {
    FDV_Z_ME = 0,     /* motion search (integer + sub-pel refinement) */
    FDV_Z_MCLUMA,     /* luma motion compensation                     */
    FDV_Z_MCCHROMA,   /* chroma motion compensation                   */
    FDV_Z_INTRA,      /* intra prediction and mode evaluation         */
    FDV_Z_RESID4,     /* 4x4 residual: transform, quant, code         */
    FDV_Z_RESID8,     /* 8x8 residual: the transform-size trial       */
    FDV_Z_RDOQ,       /* rate-distortion coefficient truncation       */
    FDV_Z_ENTROPY,    /* rANS model build, encode, decode, tables     */
    FDV_Z_DEBLOCK,    /* in-loop filter                               */
    FDV_Z_COUNT
};

/* 1 when built with -DFDV_PROFILE, 0 otherwise. */
int  fdv_profile_available(void);
void fdv_profile_reset(void);

/* Merge this thread's counters into the shared totals.  Worker threads call
 * it before exiting; the reporter calls it for itself. */
void fdv_profile_flush(void);

/* Print the table.  `total_ms` is the wall time the caller is attributing, so
 * the report can show what fraction the zones account for. */
void fdv_profile_report(FILE *out, const char *label, double total_ms);

/* ===========================================================================
 * 1. FRAME
 * Padded, aligned planar YUV frame and plane buffers.
 * ======================================================================== */


/* SIMD alignment for plane rows and the data origin. 32 bytes = AVX2. */
#define FDV_ALIGN 32

/* Border (in pixels) replicated around every plane so motion compensation can
 * read past frame edges without bounds checks or branches. A multiple of
 * FDV_ALIGN keeps the data origin aligned. */
#define FDV_BORDER 32

/* A single image plane: a padded, strided, aligned 8-bit buffer.
 *
 *   base ── allocation start (free this)
 *   data ── points at pixel (0,0); the border sits to its left/above
 *   stride ─ bytes between vertically adjacent pixels (includes both borders)
 *
 * Valid x range is [-border, width+border), same for y, so neighbor and
 * sub-pel taps never fall outside allocated memory once borders are extended. */
typedef struct {
    uint8_t *base;
    uint8_t *data;
    int width;
    int height;
    int stride;
    int border;
} fdv_plane;

/* A planar YUV 4:2:0 frame: luma plus two half-resolution chroma planes. */
typedef struct {
    fdv_plane planes[3];      /* [0]=Y, [1]=U, [2]=V */
    int width;              /* luma width  */
    int height;             /* luma height */
    int subsample_x;        /* chroma = width  >> subsample_x (1 for 4:2:0) */
    int subsample_y;        /* chroma = height >> subsample_y (1 for 4:2:0) */
} fdv_frame;

/* Pointer to pixel (x,y) within a plane. x,y may be negative (into the border)
 * up to -plane->border, or up to width/height+border on the far side. */
static inline uint8_t *fdv_plane_at(const fdv_plane *p, int x, int y) {
    return p->data + (int64_t)y * p->stride + x;
}

/* Allocate a 4:2:0 frame of the given luma dimensions. Planes are zeroed in the
 * visible region; borders are undefined until fdv_frame_extend_borders(). Returns 0
 * on success, -1 on allocation failure. */
int  fdv_frame_alloc(fdv_frame *f, int width, int height);

/* Release all plane allocations and zero the struct. Safe on a zeroed frame. */
void fdv_frame_free(fdv_frame *f);

/* Replicate edge pixels outward into the border of every plane. Call after
 * writing the visible region and before using the frame as a motion-comp
 * reference. */
void fdv_frame_extend_borders(fdv_frame *f);


/* ===========================================================================
 * 2. BITS
 * MSB-first bit I/O over a caller-provided byte buffer.
 * ======================================================================== */


/* MSB-first bit I/O over a caller-provided byte buffer.
 *
 * Used for headers and fixed-length syntax elements; the bulk coefficient/symbol
 * stream goes through rANS (see rans.h). Both writer and reader keep a 64-bit
 * accumulator so up to 32 bits can be moved per call with at most one refill. */

typedef struct {
    uint8_t *buf;
    size_t   cap;     /* capacity in bytes */
    size_t   len;     /* bytes committed so far */
    uint64_t acc;     /* pending bits, right-justified */
    int      nbits;   /* valid bits in acc */
} fdv_bitwriter;

typedef struct {
    const uint8_t *buf;
    size_t   len;     /* readable bytes */
    size_t   pos;     /* next byte to load */
    uint64_t acc;     /* loaded bits, right-justified */
    int      nbits;   /* valid bits in acc */
} fdv_bitreader;

void   fdv_bw_init(fdv_bitwriter *w, uint8_t *buf, size_t cap);
/* Write the low n bits of val (n in 0..32), most-significant bit first. */
void   fdv_bw_put(fdv_bitwriter *w, uint32_t val, int n);
/* Pad the final partial byte with zero bits and return total bytes written. */
size_t fdv_bw_flush(fdv_bitwriter *w);

void     fdv_br_init(fdv_bitreader *r, const uint8_t *buf, size_t len);
/* Read n bits (n in 0..32), most-significant bit first. Reads past the end
 * return zero bits. */
uint32_t fdv_br_get(fdv_bitreader *r, int n);


/* ===========================================================================
 * 3. BITPACK
 * Serialization primitives shared by every bitstream layer.
 * ======================================================================== */


/* Shared serialization primitives used by the bitstream layers (image.c,
 * video.c, coeff8.c). Previously duplicated per file; centralized here so the
 * encode and decode sides cannot drift apart. */

extern const int fdv_zz4[16];   /* 4x4 zigzag scan: row-major index per scan position */

/* ZigZag map signed<->unsigned so small magnitudes stay small. */
uint32_t fdv_zz_enc(int v);
int      fdv_zz_dec(uint32_t u);

/* LEB128 varint over a byte buffer; fdv_leb_size is the byte length without writing. */
size_t   fdv_leb_put(uint8_t *b, size_t p, uint32_t u);
uint32_t fdv_leb_get(const uint8_t *b, size_t *p);
int      fdv_leb_size(uint32_t u);

/* Bounded reads for decode parsing: never read past `end`; on overflow set
 * *ok=0 and return 0, so a decoder can reject malformed input rather than
 * over-read. fdv_rd_count additionally rejects values above `max`; fdv_rd_level reads a
 * zigzag-mapped LEB capped at 5 bytes. */
int fdv_rd_byte(const uint8_t *s, size_t *p, size_t end, int *ok);
int fdv_rd_count(const uint8_t *s, size_t *p, size_t end, int max, int *ok);
uint32_t fdv_rd_count32(const uint8_t *s, size_t *p, size_t end, int *ok);
int fdv_rd_level(const uint8_t *s, size_t *p, size_t end, int *ok);

/* Little-endian fixed-width fields. */
size_t   fdv_put16(uint8_t *b, size_t p, unsigned v);
size_t   fdv_put32(uint8_t *b, size_t p, uint32_t v);
unsigned fdv_get16(const uint8_t *b, size_t *p);
uint32_t fdv_get32(const uint8_t *b, size_t *p);


/* ===========================================================================
 * 4. RANS
 * Static range-ANS byte entropy coder — the fast-decode lever.
 * ======================================================================== */


/* Static range-ANS (rANS) byte entropy coder over a 256-symbol alphabet.
 *
 * The probability model is a static per-stream frequency table normalized to a
 * power-of-two total (FDV_RANS_SCALE). Decode is table-driven and branch-light: one
 * lookup in a slot->symbol table plus a multiply/add and an occasional byte
 * refill. The interleaved variant runs two independent rANS states over a shared
 * byte stream so the two dependency chains overlap on a superscalar core — the
 * "fast decode" lever this codec is built around.
 *
 * Encoding emits bytes in reverse; decoding consumes them forward (rANS is a
 * LIFO stack). The encode functions handle the reversal internally and return a
 * forward-readable buffer. */

#define FDV_RANS_SCALE_BITS 12
#define FDV_RANS_SCALE      (1u << FDV_RANS_SCALE_BITS)   /* total frequency = 4096 */

/* Per-symbol encode parameters. */
typedef struct {
    uint16_t freq;   /* normalized frequency */
    uint16_t cum;    /* cumulative frequency (slot start) */
} fdv_rans_sym;

/* Decode tables: slot->symbol map plus per-symbol freq/cum. */
typedef struct {
    uint8_t  slot2sym[FDV_RANS_SCALE];
    uint16_t freq[256];
    uint16_t cum[256];
} fdv_rans_table;

/* Build encode + decode tables from a raw symbol histogram. Frequencies are
 * normalized so every symbol with a nonzero count keeps a nonzero frequency and
 * the total is exactly FDV_RANS_SCALE. */
void fdv_rans_build_tables(const uint32_t counts[256],
                       fdv_rans_sym enc[256], fdv_rans_table *dec);

/* Rebuild a decode table from an already-normalized frequency array (as
 * transmitted in a bitstream header). freq must sum to FDV_RANS_SCALE. */
void fdv_rans_dec_table_from_freq(const uint16_t freq[256], fdv_rans_table *dec);

/* The encoder's side of the same thing: a symbol's frequency and cumulative
 * base are all the encoder needs, so a table that arrived as frequencies can be
 * encoded with directly. */
void fdv_rans_enc_from_freq(const uint16_t freq[256], fdv_rans_sym enc[256]);

/* Compactly (de)serialize a frequency table: a 16-bit count of nonzero entries
 * followed by (symbol byte, varint frequency) pairs — far smaller than 256
 * fixed-width entries for the sparse alphabets the codec produces. Return the
 * new buffer offset. */
size_t fdv_rans_write_freqs(uint8_t *out, size_t o, const uint16_t freq[256]);
size_t fdv_rans_read_freqs(const uint8_t *in, size_t p, uint16_t freq[256]);
/* Bounded reader for untrusted input: never reads past `end`, rejects malformed
 * tables (truncated, duplicate symbol, or frequencies not summing to
 * FDV_RANS_SCALE — which would otherwise gap or overflow the decode slot table) by
 * setting *ok=0. On valid input it behaves exactly like fdv_rans_read_freqs. */
size_t fdv_rans_read_freqs_bounded(const uint8_t *in, size_t p, size_t end,
                               uint16_t freq[256], int *ok);
/* Byte length fdv_rans_write_freqs would emit for this table (without writing). */
size_t fdv_rans_freqs_size(const uint16_t freq[256]);

/* Single-state encode of n symbols into out (capacity out_cap). Returns the
 * length of the forward-readable encoded stream, written at out[0..len). */
size_t fdv_rans_encode(const uint8_t *syms, size_t n, const fdv_rans_sym enc[256],
                   uint8_t *out, size_t out_cap);
void   fdv_rans_decode(const uint8_t *in, size_t n, const fdv_rans_table *dec,
                   uint8_t *syms);
/* Bounded single-state decode for untrusted input: consumes at most `inlen`
 * bytes, feeding zero bytes rather than over-reading if a corrupt stream would
 * demand more. Valid streams stay within `inlen`, so output matches
 * fdv_rans_decode exactly. */
void   fdv_rans_decode_bounded(const uint8_t *in, size_t inlen, size_t n,
                           const fdv_rans_table *dec, uint8_t *syms);

/* Two-way interleaved variants (same stream format family, not interchangeable
 * with the single-state functions). */
size_t fdv_rans_encode_il2(const uint8_t *syms, size_t n, const fdv_rans_sym enc[256],
                       uint8_t *out, size_t out_cap);
void   fdv_rans_decode_il2(const uint8_t *in, size_t n, const fdv_rans_table *dec,
                       uint8_t *syms);

/* --- Adaptive range coder -------------------------------------------------
 *
 * The other half of section 4, and the opposite trade: rANS is static and
 * parallel, this is adaptive and serial.
 *
 * A static table has to be transmitted, and on cheap frames that table is a
 * sixth of the bitstream. The obvious fix -- derive the table from frames both
 * sides have already decoded, and send nothing -- measures *worse* (+26% on a
 * near-still scene): the history is a blurred average of many frames, and a
 * fresh table fitted to this frame beats it by more than the table costs.
 *
 * What wins is doing both: start from the history, then let the model chase
 * this frame's statistics as it codes. On a SKIP-dominated stream the dominant
 * symbol's weight climbs within a few hundred symbols and its cost collapses,
 * which no transmitted table can match at any price. Measured against the
 * current coder at QP 28: -32% on a near-still scene, -31% on a slow one, -15%
 * on a pan, -8% on real motion, -0.6% on chaos.
 *
 * That is CABAC's mechanism, and it is worth being precise about which part of
 * CABAC is worth having. Adapting from flat or generic initial states -- what
 * CABAC's context init tables approximate -- is a disaster here: +140% on the
 * same near-still scene, because these streams are short and violently skewed,
 * so a model starting from uniform pays more to learn the distribution than the
 * table costs. The prior is doing as much work as the adaptation; neither alone
 * is worth anything.
 *
 * The cost is that adaptation is serial: symbol i+1's model depends on symbol
 * i, which is exactly the dependency this codec's static rANS was chosen to
 * avoid. So it is applied only to streams below FDV_AD_CAP symbols, which is
 * where the entire win is anyway -- the big coefficient streams on busy frames
 * gain under 1% and stay on parallel rANS. That caps the serially-decoded
 * symbols per frame at a few thousand, well under a tenth of a millisecond.
 *
 * The model is a Fenwick tree over the 256-symbol alphabet, so cumulative
 * frequency, symbol search, and update are all eight steps rather than 256.
 * The coder underneath is a plain carry-propagating range coder. */

#ifndef FDV_AD_CAP
#define FDV_AD_CAP  4096    /* symbols; a longer stream stays on static rANS */
#endif
#ifndef FDV_AD_INC
#define FDV_AD_INC  128     /* weight added per symbol coded */
#endif
#ifndef FDV_AD_BASE
#define FDV_AD_BASE 1       /* floor weight, so every symbol stays codable */
#endif
#ifndef FDV_AD_SCALE
#define FDV_AD_SCALE 8192   /* total weight a full history is normalized to */
#endif
#ifndef FDV_AD_MAX
#define FDV_AD_MAX  65536   /* model halves past this, bounding range/total */
#endif
#ifndef FDV_AD_HIST
#define FDV_AD_HIST (1 << 21)  /* history halves past this: how far back it sees */
#endif

/* Weights plus a Fenwick tree over them. f[] is kept alongside the tree so a
 * symbol's own weight is a load rather than a second descent. */
typedef struct {
    uint32_t f[256];
    uint32_t t[257];        /* 1-indexed; t[i] covers f[i - (i & -i) .. i) */
    uint32_t tot;
} fdv_amodel;

/* Seed a model from accumulated history: FDV_AD_BASE + count. The floor is what
 * makes an unseen symbol cost ~11 bits instead of being uncodable, and it is a
 * quarter of the per-symbol increment, which sets how fast the frame's own
 * statistics overtake the history. */
void fdv_amodel_init(fdv_amodel *m, const uint32_t hist[256]);

typedef struct {
    uint8_t *out; size_t cap, n;
    uint64_t low; uint32_t range;
    uint8_t  cache; uint64_t csize;
    int      fail;              /* set if the output buffer ran out */
} fdv_aenc;

typedef struct {
    const uint8_t *in; size_t len, p;
    uint32_t code, range;
} fdv_adec;

void     fdv_aenc_init(fdv_aenc *e, uint8_t *out, size_t cap);
void     fdv_aenc_sym(fdv_aenc *e, fdv_amodel *m, unsigned s);
/* Flush and return the stream length, or 0 if the buffer overflowed. */
size_t   fdv_aenc_finish(fdv_aenc *e);
/* Bounded like the rANS decoder: a truncated or corrupt stream is fed zero
 * bytes rather than read past. */
void     fdv_adec_init(fdv_adec *d, const uint8_t *in, size_t len);
unsigned fdv_adec_sym(fdv_adec *d, fdv_amodel *m);


/* ===========================================================================
 * 5. COEFF8
 * 8x8 coefficient serialization: zigzag + end-of-block.
 * ======================================================================== */


/* 8x8 coefficient serialization: zigzag scan + end-of-block coding, the
 * counterpart to the inline 4x4 coefficient coding. This is the bitstream layer
 * needed to wire the 8x8 transform (transform.h) into the codec; the per-block
 * 4x4-vs-8x8 RD selection that uses it is the next step. */

extern const int fdv_zz8[64];   /* row-major index for each zigzag scan position */

/* Coefficient symbols, split by role.
 *
 * An end-of-block count and a transform-size flag have nothing statistically in
 * common with a coefficient level: measured on camera content, the counts run
 * about 1.2 bits under their own model where the levels run 2.7. Pooling them
 * into one frequency table -- which is what a single coefficient stream forces
 * -- was costing about a sixth of the coefficient bits, and coefficients are
 * over 80% of the frame. So they travel as two streams with a model each.
 *
 * Splitting further (by scan position, by block size) was measured too and adds
 * under a percent, which does not pay for the extra tables. */
/* The frequency tables a frame was coded with, kept so the next frame can reuse
 * them instead of sending its own.
 *
 * Tables are a fixed cost per frame and the frames that can least afford them
 * are the cheap ones: on a static scene three tables came to 36% of the coded
 * P-frame, against 3% on a busy one. That is exactly backwards, and it showed
 * up as this codec needing more than twice x264's bitrate on easy content while
 * beating it on hard content.
 *
 * Reuse is decided per frame by coding it both ways and keeping the smaller, so
 * it can never lose; a stale table that cannot represent some symbol the frame
 * actually uses is simply not a candidate. The cache resets at every key frame,
 * so seeking to one still needs nothing before it. */
typedef struct {
    uint16_t f1[256];
    uint16_t fs[16][256];       /* one per coded stream; see pframe_encode */
    int      have;
    /* Symbol counts accumulated over the P-frames since the last key frame.
     * Both sides build this from frames they have already coded or decoded, so
     * it is a model neither has to transmit -- see the adaptive coder in
     * section 4. Reset with the rest of the cache at a key frame, so seeking to
     * one still needs nothing before it. */
    uint32_t hist[16][256];
    int      hn;                /* P-frames accumulated; 0 = no prior yet */
} fdv_tabcache;

/* Where a block's coefficients are written, and where they are read back.
 *
 * Six streams, not two. The 4x4 and 8x8 transforms get their own end-of-block
 * counts and their own levels, because they are different distributions -- an
 * 8x8 count runs 0..64 against the 4x4's 0..16, and an 8x8 transform
 * concentrates a block's energy into coefficients several times larger.
 * Pooling those measured 32 KB on one high-rate intra frame at an identical
 * symbol count: the same symbols, coded worse for sharing a table.
 *
 * Chroma then gets its own pair again. It is about half the 4x4 coefficient
 * symbols in a P-frame and quantizes to nothing long before luma does, so its
 * counts sit hard against zero where luma's are spread; measured on the coded
 * streams, pooling the two costs 3-6% of the counts and 3-9% of the levels.
 * The intra path codes each plane in its own call and never mixes them, so it
 * leaves these two empty and pays nothing for them. */
typedef struct fdv_cw {
    uint8_t *n;  size_t np;     /* 4x4 end-of-block counts                   */
    uint8_t *l;  size_t lp;     /* 4x4 coefficient levels                    */
    uint8_t *n8; size_t np8;    /* 8x8 end-of-block counts                   */
    uint8_t *l8; size_t lp8;    /* 8x8 coefficient levels                    */
    uint8_t *fl; size_t flp;    /* transform-size region flags               */
    uint8_t *nc; size_t ncp;    /* chroma end-of-block counts                */
    uint8_t *lc; size_t lcp;    /* chroma coefficient levels                 */
    int chroma;                 /* which plane the coder is writing for      */
} fdv_cw;
typedef struct fdv_cr {
    const uint8_t *b;
    size_t np, nend, lp, lend;
    size_t np8, nend8, lp8, lend8;
    size_t flp, flend;
    size_t ncp, ncend, lcp, lcend;
    int chroma;
} fdv_cr;

/* Append level[64] as: a count = (last-nonzero zigzag index + 1) into the 8x8
 * count stream, then that many signed-LEB levels, in zigzag order, into the
 * 8x8 level stream. */
void fdv_coeff8_encode(const int16_t level[64], fdv_cw *w);

/* Inverse: read the count then the levels, the rest implicitly zero. */
void fdv_coeff8_decode(fdv_cr *r, int *ok, int16_t level[64]);


/* ===========================================================================
 * 6. TRANSFORM
 * Integer 4x4/8x8 transforms, quantization, RDOQ.
 * ======================================================================== */


/* 4x4 integer transform + quantization (H.264-style core transform).
 *
 * The forward/inverse core transforms are multiply-free (adds and shifts only)
 * — the non-power-of-two normalization that a true DCT needs is folded into the
 * quant/dequant scaling tables instead. That keeps the *decode* hot path
 * (dequant = one multiply per coeff, then add/shift butterflies) cheap and
 * SIMD-friendly, which is the whole point.
 *
 * Blocks are row-major 4x4: element (i,j) lives at index i*4 + j.
 *
 * The forward core gains a factor of ~16 over the input, so coefficients need
 * 32 bits; levels and residuals fit in 16. */

/* Forward core transform: residual -> raw coefficients W = Cf * X * Cf^T.
 * fdv_fdct4x4 dispatches to the SIMD kernel when available; the _scalar / _neon
 * variants are exposed for equivalence testing. */
void fdv_fdct4x4(const int16_t residual[16], int32_t coeff[16]);
void fdv_fdct4x4_scalar(const int16_t residual[16], int32_t coeff[16]);
#if defined(__ARM_NEON)
void fdv_fdct4x4_neon(const int16_t residual[16], int32_t coeff[16]);
#endif

/* Inverse core transform: dequantized coeffs -> residual, with the final
 * (x + 32) >> 6 normalization. fdv_idct4x4 dispatches to the SIMD kernel when one is
 * available; the _scalar / _neon variants are exposed for equivalence testing. */
void fdv_idct4x4(const int32_t dcoeff[16], int16_t residual[16]);
void fdv_idct4x4_scalar(const int32_t dcoeff[16], int16_t residual[16]);
#if defined(__ARM_NEON)
void fdv_idct4x4_neon(const int32_t dcoeff[16], int16_t residual[16]);
#endif

/* Dead-zone quantize raw coefficients at the given QP (0..51). */
void fdv_quant4x4(const int32_t coeff[16], int16_t level[16], int qp);

/* RD coefficient truncation: given quantized levels and the spatial residual
 * they came from, zero the trailing (high-frequency) zigzag coefficients when
 * that lowers the rate-distortion cost D + lambda*rate (rate = the end-of-block
 * count byte plus the LEB bytes of the retained levels). Modifies level in
 * place; only ever removes trailing coefficients, so it cannot worsen RD. */
void fdv_rdoq4x4(int16_t level[16], const int16_t residual[16], int qp, double lambda);

/* 8x8 transform. Chosen per aligned 8x8 region against four 4x4 transforms by
 * RD, in both the intra quadtree's leaves and the P-frame residual coder.
 * A fixed-point orthonormal integer DCT-II: forward fits int32, the inverse
 * accumulates in int64 and normalizes by >>24. Blocks are row-major 8x8. */
void fdv_fdct8x8(const int16_t residual[64], int32_t coeff[64]);
void fdv_idct8x8(const int32_t dcoeff[64], int16_t residual[64]);
void fdv_quant8x8(const int32_t coeff[64], int16_t level[64], int qp);
void fdv_dequant8x8(const int16_t level[64], int32_t dcoeff[64], int qp);

/* Sum of absolute Hadamard-transformed differences, used to rank intra
 * candidates cheaply. The _scalar variant is exposed for equivalence testing. */
int fdv_satd4x4(const uint8_t *src, int sstride, const uint8_t *pred);
int fdv_satd4x4_scalar(const uint8_t *src, int sstride, const uint8_t *pred);

/* Rescale levels back to the coefficient domain at the given QP. fdv_dequant4x4
 * dispatches to the SIMD kernel when available; variants exposed for testing. */
void fdv_dequant4x4(const int16_t level[16], int32_t dcoeff[16], int qp);
void fdv_dequant4x4_scalar(const int16_t level[16], int32_t dcoeff[16], int qp);
#if defined(__ARM_NEON)
void fdv_dequant4x4_neon(const int16_t level[16], int32_t dcoeff[16], int qp);
#endif


/* ===========================================================================
 * 7. INTRA
 * Intra prediction from reconstructed neighbors.
 * ======================================================================== */


/* 4x4 intra prediction from reconstructed neighbors — the nine H.264 modes.
 *
 * Neighbors are passed as: top[0..3] (the row above) plus top[4..7] (the
 * above-right extension), left[0..3] (the column to the left), and the
 * top-left corner sample. Directional modes assume their required neighbors
 * are present; the caller selects only applicable modes. Encoder and decoder
 * call this identically, keeping the loop bit-exact. */

enum {
    FDV_INTRA_DC    = 0,   /* mean of available neighbors                 */
    FDV_INTRA_VERT  = 1,   /* copy top down            (top)              */
    FDV_INTRA_HORIZ = 2,   /* copy left across         (left)             */
    FDV_INTRA_DDL   = 3,   /* diagonal down-left       (top + top-right)  */
    FDV_INTRA_DDR   = 4,   /* diagonal down-right      (top+left+corner)  */
    FDV_INTRA_VR    = 5,   /* vertical-right           (top+left+corner)  */
    FDV_INTRA_HD    = 6,   /* horizontal-down          (top+left+corner)  */
    FDV_INTRA_VL    = 7,   /* vertical-left            (top + top-right)  */
    FDV_INTRA_HU    = 8,   /* horizontal-up            (left)             */
    FDV_INTRA_NMODES = 9,
};

/* Fill pred[16] (row-major 4x4) for the given mode. */
void fdv_intra_predict_4x4(int mode, const uint8_t top[8], const uint8_t left[4],
                       uint8_t topleft, int have_top, int have_left,
                       uint8_t pred[16]);

/* n x n DC / vertical / horizontal intra prediction (n a multiple of 4) from the
 * row above (top[0..n-1]), the column to the left (left[0..n-1]) and the
 * top-left corner. Used by the larger prediction sizes, where one prediction
 * covers the whole block.
 *
 * PLANE is H.264's plane predictor: a least-squares linear ramp fitted to the
 * two edges, which is the mode a smooth gradient wants and the flat three
 * cannot express. It needs both edges and the corner; without them the caller
 * must not select it (fdv_intra_nn_mode_ok), and it falls back to DC. */
enum { FDV_INTRA_NN_DC = 0, FDV_INTRA_NN_V = 1, FDV_INTRA_NN_H = 2,
       FDV_INTRA_NN_PLANE = 3, FDV_INTRA_NN_NMODES = 4 };
void fdv_intra_nxn(int mode, const uint8_t *top, const uint8_t *left,
               uint8_t topleft, int n, int have_top, int have_left, uint8_t *pred);

/* Whether mode m's required neighbours are present at size n. */
int fdv_intra_nn_mode_ok(int m, int n, int have_top, int have_left);


/* ===========================================================================
 * 8. INTER
 * Motion-compensated prediction and motion search.
 * ======================================================================== */


/* Inter prediction: motion-compensated prediction from a single reference
 * frame (P-frames), with quarter-pel motion vectors via bilinear interpolation.
 *
 * Motion vectors are in quarter-pel units: integer pel = mv >> 2, fractional
 * phase = mv & 3. The reference plane's borders MUST be extended
 * (fdv_frame_extend_borders) so vectors pointing outside the frame read valid
 * edge-replicated samples with no bounds checks. */

/* Predict a bw x bh block whose top-left is (bx,by) in reference coordinates,
 * displaced by (mvx,mvy) quarter-pel, into dst (row stride dst_stride).
 *
 * Luma uses the H.264 6-tap half-pel filter + quarter-pel averaging; chroma
 * uses bilinear (lower-frequency content, half the resolution). */
void fdv_mc_luma(const fdv_plane *ref, int bx, int by, int bw, int bh,
             int mvx, int mvy, uint8_t *dst, int dst_stride);
void fdv_mc_chroma(const fdv_plane *ref, int bx, int by, int bw, int bh,
               int mvx, int mvy, uint8_t *dst, int dst_stride);

/* Motion search for the bw x bh block of `cur` (pointer to its (0,0), stride
 * cur_stride) at (bx,by), against `ref`. Seeds from (0,0) and the predicted
 * vector (pmx,pmy) in quarter-pel, then runs a diamond search and half-/
 * quarter-pel refinement, all clamped to +-range pels. Writes the best
 * quarter-pel vector to mvx and mvy, and returns its SAD. */
int fdv_me_search(const uint8_t *cur, int cur_stride, const fdv_plane *ref,
              int bx, int by, int bw, int bh, int range,
              int pmx, int pmy, int *mvx, int *mvy);

/* Sum of absolute differences between a bw x bh block of `cur` (row stride
 * cur_stride) and a tightly packed `pred` (row stride bw). SIMD-accelerated for
 * 16- and 8-wide blocks. Exposed for equivalence testing. */
int fdv_sad_kernel(const uint8_t *cur, int cur_stride, const uint8_t *pred,
               int bw, int bh);


/* ===========================================================================
 * 9. DEBLOCK
 * In-loop deblocking filter over the 8x8 grid.
 * ======================================================================== */


/* In-loop deblocking filter over the 8x8 grid.
 *
 * Block-transform coding leaves discontinuities at block boundaries; this filter
 * smooths the samples straddling each edge, gated by gradient thresholds that
 * scale with QP so genuine image edges (large steps) are preserved while coding
 * artifacts (small steps in otherwise-flat regions) are softened.
 *
 * The grid is 8x8, not 4x4. Half the edges on a 4x4 grid are not block
 * boundaries at all -- inside a 16x16 intra leaf with an 8x8 transform there is
 * nothing there to smooth -- so filtering them softens the picture for nothing.
 * Measured over the scene library, moving to the 8x8 grid is worth -1.0%
 * BD-rate *and* halves the filter's work, which is the same trade HEVC made and
 * for the same reason. Filtering the 4x4 grid had become close to free in
 * quality terms: switching the filter off entirely measured -0.9% mean, so it
 * was destroying about as much as it repaired.
 *
 * HEVC's other half of this, a per-edge boundary strength -- skip the edge
 * entirely when neither side coded a coefficient and both predict from the same
 * place, because then there is provably no step to repair -- was built and
 * measured at +0.12% BD-rate on the video path and +0.24% all-intra. It made
 * this filter about 40% faster and total decode about 2%, because deblocking is
 * only 5-8% of decode to begin with; that alone retires the idea, and any other
 * work aimed at making this function faster.
 *
 * The quality result is the more interesting half. Skipping edges that cannot
 * carry a *new* step still loses, because on static content the filter was
 * doing something the boundary-strength argument does not see: smoothing away,
 * a little more each frame, the blocking a key frame left behind and every SKIP
 * macroblock since has copied forward. `still` and `skyline` gain 2.1% and 1.1%
 * from skipping; `tiny` loses 0.8 dB at equal rate, and it is not an artefact.
 *
 * Applied identically by encoder and decoder after a frame is fully
 * reconstructed, so the loop stays bit-exact. Filters in place. */

void fdv_deblock_plane(uint8_t *plane, int w, int h, int stride, int qp);


/* ===========================================================================
 * 10. IMAGE
 * Intra-only single-plane image codec.
 * ======================================================================== */


/* Intra-only single-plane image codec: the first end-to-end pipeline.
 *
 * Walks the plane in raster order over 4x4 blocks, predicts each from
 * reconstructed neighbors (intra.h), transforms + quantizes the residual
 * (transform.h), and entropy-codes mode + coefficient symbols with a static
 * per-image rANS model (rans.h). Decode reverses it exactly.
 *
 * Dimensions must be multiples of 4. */

/* Encode src (w x h, row stride `stride`) at the given QP into out[0..cap).
 * Returns the encoded length, or 0 on error (buffer too small / bad dims).
 * If recon != NULL it receives the encoder's reconstruction (w*h, tightly
 * packed) — decode output is guaranteed bit-identical to it. */
size_t fdv_image_encode(const uint8_t *src, int w, int h, int stride, int qp,
                    uint8_t *out, size_t cap, uint8_t *recon);

/* Decode a stream produced by fdv_image_encode into dst (w*h, row stride `stride`).
 * Writes the decoded dimensions to w_out and h_out. Returns 0 on success. */
int fdv_image_decode(const uint8_t *in, size_t len,
                 uint8_t *dst, int stride, int *w_out, int *h_out);


/* ===========================================================================
 * 11. VIDEO
 * P-frame video codec, full YUV 4:2:0.
 * ======================================================================== */


/* P-frame video codec, full YUV 4:2:0.
 *
 * Each frame is I420: a w*h luma plane followed by (w/2)*(h/2) U and V planes,
 * so one frame buffer is w*h*3/2 bytes. Frame 0 is intra (each plane coded by
 * the intra image codec). Later frames are P-frames: every 16x16 luma
 * macroblock is motion-compensated from the previous reconstructed frame and
 * carries a per-block SKIP-vs-INTER decision (rate-distortion). Chroma reuses
 * the luma motion vector scaled by 1/2 (the 4:2:0 grid ratio). The in-loop
 * deblocked reconstruction is referenced, so encoder and decoder stay in
 * lock-step with no drift.
 *
 * Dimensions must be multiples of 16. */

/* Encode `nframes` I420 frames (frames[f], each w*h*3/2 bytes) at QP into out.
 * keyint is the key-frame interval: frame f is intra iff f==0 or (keyint>0 &&
 * f%keyint==0); keyint<=0 means only frame 0 is intra. Returns the encoded
 * length, or 0 on error. */
size_t fdv_video_encode(const uint8_t *const *frames, int nframes,
                    int w, int h, int qp, int keyint, uint8_t *out, size_t cap);

/* Decode into out (nframes consecutive I420 frames). Returns 0 on success. */
int fdv_video_decode(const uint8_t *in, size_t len, uint8_t *out,
                 int *nframes, int *w, int *h);


/* ===========================================================================
 * 12. TILES
 * Independent still-image tiles; threaded decode.
 * ======================================================================== */


/* Tiling: partition a luma frame into a grid of independent rectangular tiles,
 * each coded as a self-contained intra substream (prediction and entropy both
 * reset at tile boundaries). Independence is the point — tiles carry no
 * cross-tile dependencies, so a decoder can process them in any order and on as
 * many threads as it likes. This is the spatial-parallelism lever behind
 * fast decode.
 *
 * Frame and tile dimensions must be multiples of 4. Edge tiles are smaller. */

/* Encode src (w x h) at QP into out, using tiles of up to tile_w x tile_h.
 * Returns the encoded length, or 0 on error. */
size_t fdv_tiled_encode(const uint8_t *src, int w, int h, int qp,
                    int tile_w, int tile_h, uint8_t *out, size_t cap);

/* Decode serially into dst (row stride = w). Reports geometry via w_out/h_out. */
int fdv_tiled_decode(const uint8_t *in, size_t len, uint8_t *dst,
                 int *w_out, int *h_out);

/* Decode with up to nthreads worker threads (tiles are independent, so this is
 * an exact parallelization of fdv_tiled_decode). Falls back to serial for
 * nthreads <= 1. */
int fdv_tiled_decode_threaded(const uint8_t *in, size_t len, uint8_t *dst,
                          int *w_out, int *h_out, int nthreads);


/* ===========================================================================
 * 13. VTILE
 * Tile-parallel video: independent horizontal bands.
 * ======================================================================== */


/* Tile-parallel video: split each frame into horizontal bands of whole 16x16
 * macroblock rows and code each band as an INDEPENDENT video sub-stream (its
 * own intra/inter prediction, references, and entropy model). Because every
 * band is just the base codec (video.c) run on a smaller frame, round-trip
 * correctness is inherited and the bands carry no cross-band dependency, so the
 * decoder reconstructs them concurrently — the "fast decode" payoff applied to
 * video, mirroring the tiled still-image path (tiles.c).
 *
 * The cost is the usual tiling trade-off: prediction cannot cross a band edge
 * (each band's first frame is intra and its motion search sees only band-local,
 * border-replicated references), so coding efficiency drops slightly near band
 * boundaries. This is a distinct container from video.c's single-stream format;
 * plain fdv_video_encode/fdv_video_decode streams are unaffected.
 *
 * Dimensions must be multiples of 16. band_mbrows is the number of MB rows per
 * band (>=1); the final band may be shorter (still a multiple of 16). */

/* Encode nframes I420 frames into a tiled container, using up to nthreads
 * worker threads (nthreads<=1 encodes serially). Bands share no state, so the
 * output is byte-identical regardless of nthreads. Returns the encoded length,
 * or 0 on error.
 *
 * Threading costs memory: each worker holds its own copy of one band's source
 * frames plus an encode buffer, so peak usage scales with min(nthreads,bands)
 * rather than being constant. */
size_t fdv_vtile_encode(const uint8_t *const *frames, int nframes, int w, int h,
                    int qp, int keyint, int band_mbrows, int nthreads,
                    uint8_t *out, size_t cap);

/* Decode a tiled container into out (nframes consecutive I420 frames), using up
 * to nthreads worker threads (nthreads<=1 decodes serially). Returns 0 on
 * success; output is identical regardless of nthreads. */
int fdv_vtile_decode(const uint8_t *in, size_t len, uint8_t *out,
                 int *nframes, int *w, int *h, int nthreads);


/* ===========================================================================
 * 14. RC
 * Encoder-side rate control and frame-level RDO.
 * ======================================================================== */


/* Encoder-side rate control and frame-level rate-distortion optimization.
 *
 * Both work the QP knob: higher QP -> coarser quantization -> fewer bits but
 * more distortion. fdv_rc_qp_for_size targets a byte budget; fdv_rc_qp_rdo minimizes the
 * classic Lagrangian cost D + lambda*R, trading distortion against rate at a
 * chosen operating point. (Encoder-only; decode speed is unaffected.)
 *
 * Dimensions must be multiples of 4 (the intra image codec's constraint). */

/* Highest-quality (lowest) QP whose encoded size fits target_bytes. If even the
 * coarsest QP overshoots, returns the coarsest. out_bytes (optional) receives
 * the achieved size. */
int fdv_rc_qp_for_size(const uint8_t *src, int w, int h,
                   size_t target_bytes, size_t *out_bytes);

/* QP minimizing distortion (SSE) + lambda * rate (bytes). */
int fdv_rc_qp_rdo(const uint8_t *src, int w, int h, double lambda);


/* ---- Average bitrate, for live streaming ---------------------------------
 *
 * One knob -- the frame QP -- driven by a feedback loop. There is no lookahead
 * and no frame reordering, because both buy compression with latency and this
 * is meant to go down a wire. So the loop only ever sees frames it has already
 * encoded: a smoothed estimate of how expensive they were, and how full a leaky
 * bucket is.
 *
 * The bucket is the point. Holding an average alone would let the encoder spend
 * a second's worth of bits on one frame and starve the next thirty; the bucket
 * is the decoder-side buffer the stream implies, so its size *is* the latency
 * the stream demands. Frames are steered back toward half full rather than
 * being made to balance every frame, which would pump the quality visibly. */
typedef struct {
    int    bitrate;          /* bits per second, the average to hold */
    int    fps;
    double bufbits;          /* bucket size, i.e. the buffering it demands */
    int    qpmin, qpmax, keyint;
    double qp_p;             /* the operating point the loop holds */
    double ip_off;           /* how much finer a key frame is coded */
    double last_target;      /* what the frame just handed out was asked for */
    double fill;             /* bucket fullness, in bits */
    int    used_qp;
} fdv_rc;

/* bufsecs <= 0 defaults to one second; qpmin/qpmax <= 0 take the defaults. */
void fdv_rc_init(fdv_rc *rc, int bitrate, int fps, int w, int h, int keyint,
                 double bufsecs, int qpmin, int qpmax);
int  fdv_rc_pick(fdv_rc *rc, int is_intra);
void fdv_rc_update(fdv_rc *rc, int is_intra, size_t bytes);

/* A frame that was held rather than coded (see fdv_enc_repeat): it spent bits,
 * so the bucket has to know, but it says nothing about how hard the content is
 * and must not drag the complexity estimate down with it. */
void fdv_rc_skip(fdv_rc *rc, size_t bytes);


/* ===========================================================================
 * 15. FDV CONTAINER
 * Self-describing file wrapper around a coded stream.
 * ======================================================================== */

/* The coded streams themselves carry no magic and no frame rate, and the two
 * container kinds (single-stream and tile-parallel) start with different
 * fields — so a file alone could not say what it was or how fast to play it.
 * This wrapper fixes that: everything a player needs before it decodes, in a
 * fixed 12-byte head.
 *
 *   0..3   "FDV1"
 *   4      format version
 *   5      kind (0 = single-stream, 1 = tile-parallel)
 *   6..7   frames per second
 *   8..11  payload length, little-endian
 *   12..   the coded stream, exactly as fdv_video_encode/fdv_vtile_encode produced it
 */
#define FDV_MAGIC   "FDV1"
#define FDV_VERSION 1
#define FDV_HEAD    12

enum { FDV_VIDEO = 0, FDV_TILED = 1 };

typedef struct {
    int kind;
    int version;
    int fps;
    int w, h, nframes;      /* read back out of the coded stream's own header */
    int qp, keyint;
    int nbands;             /* tile-parallel only; 1 otherwise */
    const uint8_t *payload;
    size_t payload_len;
} fdv_info;

/* Write head + payload into out. Returns the total length, or 0 if it does not
 * fit. `out` may safely be the same buffer the payload sits in, offset by
 * FDV_HEAD. */
size_t fdv_wrap(uint8_t *out, size_t cap, int kind, int fps,
                 const uint8_t *payload, size_t payload_len);

/* Parse a .fdv file. Fills `info` and returns 0, or returns -1 if the file is
 * not a vela stream, is a version this build does not know, or is truncated.
 * Also reads the geometry out of the payload header so a caller can size its
 * buffers before decoding. */
int fdv_read(const uint8_t *in, size_t len, fdv_info *info);

/* Decode a parsed file into `out` (nframes consecutive I420 frames). Dispatches
 * on the kind, so callers do not have to. nthreads applies to the tiled kind. */
int fdv_decode(const fdv_info *info, uint8_t *out, int nthreads);

/* --- streaming decode ---------------------------------------------------
 *
 * fdv_decode above decodes a whole file in one call, which needs the entire
 * sequence resident: at 1080p that is 3.1 MB per frame, so a ten-minute clip
 * would want tens of gigabytes. This is the incremental form -- open once, pull
 * one frame at a time, keep only what you are using.
 *
 * The state a decoder carries between frames is small and entirely internal:
 * the two-frame reference pool, the stream position, and how many references
 * are valid yet. Holding it in a context rather than on fdv_video_decode's
 * stack is the whole change.
 *
 * Seeking is bounded by the key-frame interval, as it must be: a P-frame is
 * defined against its predecessors, so landing on one means decoding forward
 * from the key frame at or before it. With keyint 0 there is exactly one key
 * frame, at the start, and seeking backwards costs a full replay -- which is a
 * property of the stream, not of this decoder. */

typedef struct fdv_decoder fdv_decoder;

/* Open a decoder over a parsed file. `info` must stay valid (it points into
 * the caller's buffer) for the decoder's lifetime. Returns NULL on error. */
fdv_decoder *fdv_dec_open(const fdv_info *info);

/* Decode the next frame into `out`, which must hold fdv_dec_frame_size bytes.
 * Returns 1 when a frame was written, 0 at end of stream, -1 on error. */
int fdv_dec_next(fdv_decoder *d, uint8_t *out);

/* Position the decoder so the next fdv_dec_next returns frame `frame`.
 * Decodes forward from the preceding key frame, discarding what it passes.
 * Returns 0, or -1 on error. */
int fdv_dec_seek(fdv_decoder *d, int frame);

int    fdv_dec_pos(const fdv_decoder *d);          /* next frame index */
size_t fdv_dec_frame_size(const fdv_decoder *d);   /* bytes one frame needs */
void   fdv_dec_close(fdv_decoder *d);

/* --- streaming encode ---------------------------------------------------
 *
 * fdv_video_encode wants every frame up front, which suits a file on disk and
 * suits nothing else. A camera hands over one frame at a time and does not say
 * how many are coming; buffering them all would cost 3.1 MB per 1080p frame.
 *
 * This is the incremental form. Only the *coded* stream accumulates in memory,
 * and that is small -- a minute of 720p at 800 kbps is about 6 MB -- so the
 * frame count can be patched into the header at the end, which is the one thing
 * a streaming writer cannot know at the start.
 *
 * The output is byte-identical to fdv_video_encode over the same frames. */

typedef struct fdv_encoder fdv_encoder;

/* Open an encoder. Dimensions must be positive multiples of 16. */
fdv_encoder *fdv_enc_open(int w, int h, int qp, int keyint);

/* Push one I420 frame (w*h*3/2 bytes). Returns 0, or -1 on error. */
int fdv_enc_frame(fdv_encoder *e, const uint8_t *i420);

/* Hold the previous frame for one more slot: an all-SKIP frame written without
 * re-running mode decision. A recorder uses this to keep time when it cannot
 * encode fast enough; letting mode decision find SKIP itself is affordable at a
 * high qp and very much not at a low one. */
int fdv_enc_repeat(fdv_encoder *e);

/* Switch an encoder to average-bitrate control, replacing the fixed QP it was
 * opened with. Call it straight after opening. On a band-parallel encoder the
 * control lives on the parent and hands every band the same QP, so the bands
 * stay a spatial split of one picture rather than three independent streams
 * chasing the same budget. bufsecs <= 0 defaults to one second. */
int fdv_enc_set_bitrate(fdv_encoder *e, int bits_per_second, int fps, double bufsecs);

/* Open a band-parallel encoder. The frame is split into horizontal bands of
 * band_mbrows macroblock rows, each coded as an independent stream so the bands
 * can run on separate cores -- the same arrangement fdv_vtile_encode uses, and
 * it writes the same container. Every other fdv_enc_* call works unchanged.
 *
 * Bands cost bitrate, because prediction and the entropy model both reset at
 * every band edge: on 720p camera content, roughly +3% at 4 bands and +8% at 8.
 * Worth it when the encoder is what sets a capture's frame rate. */
fdv_encoder *fdv_enc_open_tiled(int w, int h, int qp, int keyint,
                                int band_mbrows, int nthreads);

int    fdv_enc_count(const fdv_encoder *e);   /* frames accepted so far */
size_t fdv_enc_bytes(const fdv_encoder *e);   /* coded bytes so far     */

/* Finish and hand back a complete .fdv file, head and all. Ownership passes to
 * the caller, which must free() it. The encoder is closed either way; returns
 * NULL if nothing was encoded or on error. */
uint8_t *fdv_enc_finish(fdv_encoder *e, int fps, size_t *len);

/* Abandon an encoder without producing a file. */
void fdv_enc_close(fdv_encoder *e);


#endif /* FDV_H */

/* ===========================================================================
 *
 *   IMPLEMENTATION
 *
 *   Everything below is compiled only where FDV_IMPLEMENTATION is defined.
 *   Exactly one translation unit per program should do that.
 *
 * ======================================================================== */

#ifdef FDV_IMPLEMENTATION
#ifndef FDV_IMPLEMENTATION_INCLUDED
#define FDV_IMPLEMENTATION_INCLUDED

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* fdv — a clean-sheet, fast-decode video codec.  The implementation.
 *
 * Laid out bottom-up in dependency order and sectioned by the same banners as
 * the declarations above; see this file's header comment for the map.  Nothing
 * in a section depends on a section below it.  Cross-section calls go through
 * those declarations, so only the file-local statics below are order-sensitive
 * — keep each section's helpers with their section.
 */



/* --- shared internals ---------------------------------------------------
 * Small helpers used across sections.  Each was duplicated per-file before the
 * merge; one definition now, so the copies cannot drift. */

/* Saturate to the 8-bit pixel range.  clipb keeps int for use inside integer
 * filter arithmetic; clip255 is the same clamp narrowed to a stored pixel. */
static int clipb(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static uint8_t clip255(int v) { return (uint8_t)clipb(v); }

/* Ceiling division for non-negative a and positive b (tile/band counts). */
static int ceil_div(int a, int b) { return (a + b - 1) / b; }

/* ===========================================================================
 * 0. LOG
 * Debug logging and per-frame statistics.
 * ======================================================================== */

int fdv_log_level = FDV_LOG_OFF;

static FILE *fdv_log_out = NULL;        /* NULL means stderr, resolved lazily */
static fdv_stats *fdv_stats_sink = NULL;

void fdv_palette_for(FILE *f, fdv_palette *p) {
    static const fdv_palette on  = { "\033[2m", "\033[1m", "\033[31m", "\033[32m",
                                     "\033[33m", "\033[36m", "\033[35m", "\033[0m" };
    static const fdv_palette off = { "", "", "", "", "", "", "", "" };
    const char *nc = getenv("NO_COLOR"), *fc = getenv("FDV_COLOR");
    int want = (fc && *fc && *fc != '0') || (f && isatty(fileno(f)));
    *p = (want && !(nc && *nc)) ? on : off;
}

const char *fdv_rate(char *buf, size_t n, double bps) {
    if      (bps >= 1e6) snprintf(buf, n, "%.2f Mbps", bps / 1e6);
    else if (bps >= 1e3) snprintf(buf, n, "%.0f kbps", bps / 1e3);
    else                 snprintf(buf, n, "%.0f bps",  bps);
    return buf;
}

const char *fdv_dur(char *buf, size_t n, double secs) {
    if      (secs >= 60.0) snprintf(buf, n, "%dm %04.1fs", (int)(secs / 60), secs - 60 * (int)(secs / 60));
    else if (secs >= 1.0)  snprintf(buf, n, "%.2f s", secs);
    else                   snprintf(buf, n, "%.0f ms", secs * 1000.0);
    return buf;
}

void fdv_log_set(int level, FILE *out) {
    fdv_log_level = level < FDV_LOG_OFF ? FDV_LOG_OFF : level;
    fdv_log_out   = out;
}

void fdv_logf(const char *tag, const char *fmt, ...) {
    FILE *out = fdv_log_out ? fdv_log_out : stderr;
    fdv_palette p;
    fdv_palette_for(out, &p);
    /* Tag colour by what the line is about, so a long trace can be skimmed:
     * the encoder's own progress reads differently from the container layer
     * and from anything that went wrong. */
    const char *c = p.dim;
    if      (!strcmp(tag, "encode") || !strcmp(tag, "decode")) c = p.cyn;
    else if (!strcmp(tag, "rc"))                               c = p.mag;
    else if (!strcmp(tag, "error")  || !strcmp(tag, "fail"))   c = p.red;
    else if (!strcmp(tag, "vtile")  || !strcmp(tag, "tile"))   c = p.yel;
    va_list ap;
    fprintf(out, "%s[%-6s]%s ", c, tag, p.rst);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

void       fdv_stats_set(fdv_stats *sink) { fdv_stats_sink = sink; }
fdv_stats *fdv_stats_get(void)            { return fdv_stats_sink; }

/* --- zone profiler -------------------------------------------------------
 * Accumulation is thread-local so the hot path never touches a lock; threads
 * merge into the shared table when they finish.  Everything here compiles
 * away unless FDV_PROFILE is defined -- except the three public entry points,
 * which stay so callers need no #ifdef of their own. */

#if defined(FDV_PROFILE)
static const char *const FDV_ZONE_NAME[FDV_Z_COUNT] = {
    "motion search", "mc luma", "mc chroma", "intra",
    "residual 4x4", "residual 8x8", "rdoq", "entropy", "deblock",
};
#endif

typedef struct {
    uint64_t self_ns;
    uint64_t incl_ns;
    uint64_t calls;
} fdv_zone_stat;

static fdv_zone_stat  fdv_zone_total[FDV_Z_COUNT];
static pthread_mutex_t fdv_zone_lock = PTHREAD_MUTEX_INITIALIZER;

#if defined(FDV_PROFILE)

/* CLOCK_UPTIME_RAW is the cheapest monotonic source on Darwin (no adjustment,
 * no syscall); CLOCK_MONOTONIC is the portable fallback. */
static uint64_t enc_now_ns(void) {
#if defined(__APPLE__) && defined(CLOCK_UPTIME_RAW)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#define FDV_ZSTACK_MAX 32
static _Thread_local fdv_zone_stat fdv_zone_local[FDV_Z_COUNT];
static _Thread_local struct {
    int      zone;
    uint64_t t0;
    uint64_t child_ns;      /* time spent in zones nested inside this one */
} fdv_zstack[FDV_ZSTACK_MAX];
static _Thread_local int fdv_zdepth = 0;

void fdv_zone_begin(int zone) {
    if (zone < 0 || zone >= FDV_Z_COUNT || fdv_zdepth >= FDV_ZSTACK_MAX) return;
    fdv_zstack[fdv_zdepth].zone     = zone;
    fdv_zstack[fdv_zdepth].child_ns = 0;
    fdv_zstack[fdv_zdepth].t0       = enc_now_ns();
    ++fdv_zdepth;
}

void fdv_zone_end(int zone) {
    if (fdv_zdepth <= 0) return;
    --fdv_zdepth;
    if (fdv_zstack[fdv_zdepth].zone != zone) return;   /* mismatched pair */
    uint64_t el = enc_now_ns() - fdv_zstack[fdv_zdepth].t0;
    fdv_zone_local[zone].incl_ns += el;
    fdv_zone_local[zone].self_ns += el - fdv_zstack[fdv_zdepth].child_ns;
    fdv_zone_local[zone].calls++;
    if (fdv_zdepth > 0) fdv_zstack[fdv_zdepth - 1].child_ns += el;
}

int fdv_profile_available(void) { return 1; }

void fdv_profile_flush(void) {
    pthread_mutex_lock(&fdv_zone_lock);
    for (int z = 0; z < FDV_Z_COUNT; ++z) {
        fdv_zone_total[z].self_ns += fdv_zone_local[z].self_ns;
        fdv_zone_total[z].incl_ns += fdv_zone_local[z].incl_ns;
        fdv_zone_total[z].calls   += fdv_zone_local[z].calls;
    }
    pthread_mutex_unlock(&fdv_zone_lock);
    memset(fdv_zone_local, 0, sizeof fdv_zone_local);
}

/* Time an empty begin/end pair so the report can state what it costs. */
static double fdv_zone_overhead_ns(void) {
    enum { N = 20000 };
    uint64_t t0 = enc_now_ns();
    for (int i = 0; i < N; ++i) { fdv_zone_begin(0); fdv_zone_end(0); }
    uint64_t el = enc_now_ns() - t0;
    memset(fdv_zone_local, 0, sizeof fdv_zone_local);   /* undo the calibration */
    return (double)el / N;
}

#else   /* profiling compiled out */

int  fdv_profile_available(void) { return 0; }
void fdv_profile_flush(void) { }

#endif

void fdv_profile_reset(void) {
    pthread_mutex_lock(&fdv_zone_lock);
    memset(fdv_zone_total, 0, sizeof fdv_zone_total);
    pthread_mutex_unlock(&fdv_zone_lock);
#if defined(FDV_PROFILE)
    memset(fdv_zone_local, 0, sizeof fdv_zone_local);
    fdv_zdepth = 0;
#endif
}

void fdv_profile_report(FILE *out, const char *label, double total_ms) {
    if (!out) out = stderr;
#if !defined(FDV_PROFILE)
    (void)label; (void)total_ms;
    fprintf(out, "profiling not compiled in — build with `make profile` and "
                 "run build/fdv-profile\n");
#else
    /* Flush before calibrating: the calibration loop runs real zone pairs, and
     * fdv_zone_overhead_ns clears the thread-local table afterwards to discard
     * them.  Doing it the other way round would throw away the measurement. */
    fdv_profile_flush();
    double overhead = fdv_zone_overhead_ns();

    pthread_mutex_lock(&fdv_zone_lock);
    fdv_zone_stat snap[FDV_Z_COUNT];
    memcpy(snap, fdv_zone_total, sizeof snap);
    pthread_mutex_unlock(&fdv_zone_lock);

    uint64_t sum_self = 0, sum_calls = 0;
    for (int z = 0; z < FDV_Z_COUNT; ++z) { sum_self += snap[z].self_ns; sum_calls += snap[z].calls; }

    fprintf(out, "\n=== cpu profile: %s ===\n", label ? label : "");
    fprintf(out, "%-16s %10s %7s %10s %7s %12s %10s\n",
            "zone", "self ms", "self %", "incl ms", "incl %", "calls", "ns/call");
    /* Heaviest first: a profile is read top-down. */
    int order[FDV_Z_COUNT];
    for (int z = 0; z < FDV_Z_COUNT; ++z) order[z] = z;
    for (int i = 1; i < FDV_Z_COUNT; ++i)
        for (int j = i; j > 0 && snap[order[j]].self_ns > snap[order[j - 1]].self_ns; --j) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    int coarse = 0;
    for (int i = 0; i < FDV_Z_COUNT; ++i) {
        int z = order[i];
        if (snap[z].calls == 0) continue;
        double self_ms = snap[z].self_ns / 1e6, incl_ms = snap[z].incl_ns / 1e6;
        double per = snap[z].calls ? (double)snap[z].self_ns / snap[z].calls : 0.0;
        /* A zone whose per-call cost is near the cost of timing it is mostly
         * measuring the timer. Say so rather than let it be read as data. */
        int noisy = per < 3.0 * overhead;
        if (noisy) coarse = 1;
        fprintf(out, "%-16s %10.2f %6.1f%% %10.2f %6.1f%% %12llu %10.1f%s\n",
                FDV_ZONE_NAME[z], self_ms,
                total_ms > 0 ? 100.0 * self_ms / total_ms : 0.0,
                incl_ms, total_ms > 0 ? 100.0 * incl_ms / total_ms : 0.0,
                (unsigned long long)snap[z].calls, per, noisy ? "  (*)" : "");
    }
    double acc = sum_self / 1e6;
    fprintf(out, "%-16s %10.2f %6.1f%%\n", "accounted", acc,
            total_ms > 0 ? 100.0 * acc / total_ms : 0.0);
    fprintf(out, "%-16s %10.2f %6.1f%%   (headers, copies, allocation, "
                 "and anything outside a zone)\n", "unaccounted",
            total_ms - acc, total_ms > 0 ? 100.0 * (total_ms - acc) / total_ms : 0.0);
    fprintf(out, "\ntiming a zone pair costs ~%.0f ns; %llu pairs were timed, so up to\n"
                 "%.0f ms of the figures above is the measurement itself (an upper bound --\n"
                 "the calibration loop is colder than the real call sites).\n",
            overhead, (unsigned long long)sum_calls, overhead * sum_calls / 1e6);
    if (coarse)
        fprintf(out, "(*) per-call cost is within 3x the timing cost: read the share, "
                     "not the ns/call.\n");
    fprintf(out, "Compare against the same run without --profile to see the true "
                 "wall-clock effect.\n");
#endif
}

/* Monotonic wall clock in milliseconds.  Used only for per-frame timing in the
 * stats sink, so it is read twice per frame at most — never inside a block or
 * pixel loop, where the syscall would dominate what it is trying to measure. */
static double fdv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Claim the next slot in the sink, or NULL when there is no sink or it is
 * full.  Callers fill the returned record directly. */
static fdv_frame_stat *stat_next(int index, int is_intra) {
    fdv_stats *s = fdv_stats_sink;
    if (!s || !s->frame || s->n >= s->cap) return NULL;
    fdv_frame_stat *r = &s->frame[s->n++];
    memset(r, 0, sizeof(*r));
    r->index = index;
    r->is_intra = is_intra;
    return r;
}

/* The encoder records its macroblock mode tallies here as it walks a P-frame;
 * fdv_video_encode folds them into the frame's stats record afterwards.  Kept out
 * of pframe_encode's signature so the mode-decision code stays untouched. */
/* Thread-local: the tile-parallel encoder runs one band per thread, and each
 * band drives its own frames through pframe_encode.  Sharing these would be a
 * data race for no benefit — the counters describe one frame on one thread. */
static _Thread_local int fdv_mb_tally[4];  /* [0]=SKIP [1]=INTER16 [2]=INTRA [3]=INTER8 */
static _Thread_local int fdv_entropy_mode;       /* -1 until the frame's first decision */
static _Thread_local size_t fdv_sym_struct, fdv_sym_coeff;

/* An I-frame codes three planes through fdv_image_encode, so the per-frame symbol
 * counts have to accumulate across those calls rather than be overwritten by
 * the last one.  fdv_video_encode calls this before each frame; the entropy flag
 * keeps the first (luma) decision, which is the one that describes the frame. */
/* One shared model, a model per stream, or the small streams handed to the
 * adaptive coder. A frame that reused the previous frame's tables reads as
 * whatever model arrangement it reused, which is the useful thing to see. */
static const char *fdv_emode_name(int m) {
    switch (m) {
        case 0: case 2: return "shared";
        case 1:         return "split";
        case 3:         return "adaptive";
        default:        return "-";
    }
}

static void fdv_frame_counters_reset(void) {
    fdv_entropy_mode = -1;
    fdv_sym_struct = fdv_sym_coeff = 0;
}
static void fdv_note_entropy(int emode, size_t nstruct, size_t ncoeff) {
    if (fdv_entropy_mode < 0) fdv_entropy_mode = emode;
    fdv_sym_struct += nstruct;
    fdv_sym_coeff  += ncoeff;
}

/* ===========================================================================
 * 1. FRAME
 * Padded, aligned planar YUV frame and plane buffers.
 * ======================================================================== */


/* Round up to a multiple of FDV_ALIGN. */
static int align_up(int n) {
    return (n + (FDV_ALIGN - 1)) & ~(FDV_ALIGN - 1);
}

/* Allocate one padded, aligned plane. width/height are the visible dimensions.
 * Returns 0 on success, -1 on failure (plane left zeroed). */
static int plane_alloc(fdv_plane *p, int width, int height) {
    const int border = FDV_BORDER;

    /* Stride spans left border + visible width + right border, rounded up so
     * every row starts FDV_ALIGN-aligned. The data origin then lands aligned
     * because border is itself a multiple of FDV_ALIGN. */
    int stride = align_up(width + 2 * border);
    int rows   = height + 2 * border;
    size_t size = (size_t)stride * rows;

    /* aligned_alloc requires size to be a multiple of the alignment. */
    size = (size + (FDV_ALIGN - 1)) & ~((size_t)FDV_ALIGN - 1);

    uint8_t *base = aligned_alloc(FDV_ALIGN, size);
    if (!base)
        return -1;
    memset(base, 0, size);

    p->base   = base;
    p->data   = base + (size_t)border * stride + border;
    p->width  = width;
    p->height = height;
    p->stride = stride;
    p->border = border;
    return 0;
}

int fdv_frame_alloc(fdv_frame *f, int width, int height) {
    memset(f, 0, sizeof(*f));
    f->width       = width;
    f->height      = height;
    f->subsample_x = 1;   /* 4:2:0 */
    f->subsample_y = 1;

    int cw = (width  + 1) >> f->subsample_x;
    int ch = (height + 1) >> f->subsample_y;

    if (plane_alloc(&f->planes[0], width, height) != 0 ||
        plane_alloc(&f->planes[1], cw, ch) != 0 ||
        plane_alloc(&f->planes[2], cw, ch) != 0) {
        fdv_frame_free(f);
        return -1;
    }
    return 0;
}

void fdv_frame_free(fdv_frame *f) {
    for (int i = 0; i < 3; ++i) {
        free(f->planes[i].base);
    }
    memset(f, 0, sizeof(*f));
}

/* Edge-extend one plane: replicate the outermost visible pixel into the border
 * on all four sides (and the four corners). */
static void plane_extend(fdv_plane *p) {
    const int b = p->border;
    const int w = p->width;
    const int h = p->height;

    /* Left and right borders of each visible row. */
    for (int y = 0; y < h; ++y) {
        uint8_t *row = fdv_plane_at(p, 0, y);
        memset(row - b, row[0], b);
        memset(row + w, row[w - 1], b);
    }

    /* Top and bottom borders: copy the (now horizontally extended) edge rows,
     * including their corners, across the full padded width. */
    int full = w + 2 * b;
    uint8_t *top    = fdv_plane_at(p, -b, 0);
    uint8_t *bottom = fdv_plane_at(p, -b, h - 1);
    for (int y = 1; y <= b; ++y) {
        memcpy(top - (size_t)y * p->stride, top, full);
        memcpy(bottom + (size_t)y * p->stride, bottom, full);
    }
}

void fdv_frame_extend_borders(fdv_frame *f) {
    for (int i = 0; i < 3; ++i) {
        plane_extend(&f->planes[i]);
    }
}

/* ===========================================================================
 * 2. BITS
 * MSB-first bit I/O over a caller-provided byte buffer.
 * ======================================================================== */


static uint32_t low_mask(int n) {
    return (n >= 32) ? 0xffffffffu : ((1u << n) - 1u);
}

void fdv_bw_init(fdv_bitwriter *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->acc = 0;
    w->nbits = 0;
}

void fdv_bw_put(fdv_bitwriter *w, uint32_t val, int n) {
    if (n <= 0) return;
    w->acc = (w->acc << n) | (val & low_mask(n));
    w->nbits += n;
    while (w->nbits >= 8) {
        w->nbits -= 8;
        if (w->len < w->cap)
            w->buf[w->len] = (uint8_t)(w->acc >> w->nbits);
        ++w->len;
    }
}

size_t fdv_bw_flush(fdv_bitwriter *w) {
    if (w->nbits > 0) {
        if (w->len < w->cap)
            w->buf[w->len] = (uint8_t)(w->acc << (8 - w->nbits));
        ++w->len;
        w->nbits = 0;
        w->acc = 0;
    }
    return w->len;
}

void fdv_br_init(fdv_bitreader *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->acc = 0;
    r->nbits = 0;
}

uint32_t fdv_br_get(fdv_bitreader *r, int n) {
    if (n <= 0) return 0;
    while (r->nbits < n) {
        uint8_t b = (r->pos < r->len) ? r->buf[r->pos] : 0;
        ++r->pos;
        r->acc = (r->acc << 8) | b;
        r->nbits += 8;
    }
    r->nbits -= n;
    return (uint32_t)(r->acc >> r->nbits) & low_mask(n);
}

/* ===========================================================================
 * 3. BITPACK
 * Serialization primitives shared by every bitstream layer.
 * ======================================================================== */


const int fdv_zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

uint32_t fdv_zz_enc(int v)      { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
int      fdv_zz_dec(uint32_t u) { return (int)(u >> 1) ^ -(int)(u & 1); }

size_t fdv_leb_put(uint8_t *b, size_t p, uint32_t u) {
    while (u >= 0x80) { b[p++] = (uint8_t)(u | 0x80); u >>= 7; }
    b[p++] = (uint8_t)u;
    return p;
}
uint32_t fdv_leb_get(const uint8_t *b, size_t *p) {
    uint32_t u = 0; int sh = 0; uint8_t c;
    do { c = b[(*p)++]; u |= (uint32_t)(c & 0x7f) << sh; sh += 7; } while (c & 0x80);
    return u;
}
int fdv_leb_size(uint32_t u) { int n = 1; while (u >= 0x80) { u >>= 7; ++n; } return n; }

int fdv_rd_byte(const uint8_t *s, size_t *p, size_t end, int *ok) {
    if (*p >= end) { *ok = 0; return 0; }
    return s[(*p)++];
}
/* A bounded varint, for header fields that are usually small. */
uint32_t fdv_rd_count32(const uint8_t *s, size_t *p, size_t end, int *ok) {
    uint32_t v = 0; int sh = 0;
    for (;;) {
        if (*p >= end || sh > 28) { *ok = 0; return 0; }
        uint8_t c = s[(*p)++];
        v |= (uint32_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) return v;
        sh += 7;
    }
}

int fdv_rd_count(const uint8_t *s, size_t *p, size_t end, int max, int *ok) {
    int c = fdv_rd_byte(s, p, end, ok);
    if (c < 0 || c > max) { *ok = 0; return 0; }
    return c;
}
int fdv_rd_level(const uint8_t *s, size_t *p, size_t end, int *ok) {
    uint32_t u = 0; int sh = 0;
    for (int k = 0; k < 5; ++k) {              /* a valid level is <= 5 LEB bytes */
        int c = fdv_rd_byte(s, p, end, ok);
        u |= (uint32_t)(c & 0x7f) << sh; sh += 7;
        if (!(c & 0x80)) break;
    }
    return fdv_zz_dec(u);
}

size_t fdv_put16(uint8_t *b, size_t p, unsigned v) { b[p]=(uint8_t)v; b[p+1]=(uint8_t)(v>>8); return p+2; }
size_t fdv_put32(uint8_t *b, size_t p, uint32_t v) { for(int i=0;i<4;++i) b[p+i]=(uint8_t)(v>>(8*i)); return p+4; }
unsigned fdv_get16(const uint8_t *b, size_t *p) { unsigned v=b[*p]|((unsigned)b[*p+1]<<8); *p+=2; return v; }
uint32_t fdv_get32(const uint8_t *b, size_t *p) { uint32_t v=0; for(int i=0;i<4;++i) v|=(uint32_t)b[*p+i]<<(8*i); *p+=4; return v; }

/* ===========================================================================
 * 4. RANS
 * Static range-ANS byte entropy coder — the fast-decode lever.
 * ======================================================================== */


/* Lower bound of the normalized rANS state interval; renormalization keeps the
 * 32-bit state in [RANS_L, RANS_L << 8) by emitting/consuming whole bytes. */
#define RANS_L (1u << 23)

typedef uint32_t RansState;

/* ---- Low-level state ops (Giesen-style byte rANS) ------------------------ */

static void enc_init(RansState *r) { *r = RANS_L; }

/* Encode one symbol; *pptr grows downward (bytes emitted in reverse). */
static void enc_put(RansState *r, uint8_t **pptr, uint32_t cum, uint32_t freq) {
    uint32_t x = *r;
    uint64_t x_max = ((uint64_t)(RANS_L >> FDV_RANS_SCALE_BITS) << 8) * freq;
    while (x >= x_max) {
        *--(*pptr) = (uint8_t)(x & 0xff);
        x >>= 8;
    }
    *r = ((x / freq) << FDV_RANS_SCALE_BITS) + (x % freq) + cum;
}

static void enc_flush(RansState *r, uint8_t **pptr) {
    uint32_t x = *r;
    *pptr -= 4;
    (*pptr)[0] = (uint8_t)(x >> 0);
    (*pptr)[1] = (uint8_t)(x >> 8);
    (*pptr)[2] = (uint8_t)(x >> 16);
    (*pptr)[3] = (uint8_t)(x >> 24);
}

static void dec_init(RansState *r, const uint8_t **pptr) {
    const uint8_t *p = *pptr;
    *r = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *pptr = p + 4;
}

static uint32_t dec_get(RansState *r) { return *r & (FDV_RANS_SCALE - 1); }

static void dec_advance(RansState *r, const uint8_t **pptr,
                        uint32_t cum, uint32_t freq) {
    uint32_t x = *r;
    x = freq * (x >> FDV_RANS_SCALE_BITS) + (x & (FDV_RANS_SCALE - 1)) - cum;
    while (x < RANS_L) {
        x = (x << 8) | *(*pptr)++;
    }
    *r = x;
}

/* ---- Table construction -------------------------------------------------- */

void fdv_rans_build_tables(const uint32_t counts[256],
                       fdv_rans_sym enc[256], fdv_rans_table *dec) {
    uint16_t freq[256];
    uint32_t total = 0;
    for (int i = 0; i < 256; ++i) total += counts[i];

    if (total == 0) {
        /* Degenerate: no data. Park all mass on symbol 0. */
        memset(freq, 0, sizeof(freq));
        freq[0] = FDV_RANS_SCALE;
    } else {
        uint32_t sum = 0;
        int big = 0;
        for (int i = 0; i < 256; ++i) {
            if (counts[i] == 0) { freq[i] = 0; continue; }
            uint32_t f = (uint32_t)((uint64_t)counts[i] * FDV_RANS_SCALE / total);
            if (f == 0) f = 1;          /* keep representable symbols codable */
            freq[i] = (uint16_t)f;
            sum += f;
            if (f > freq[big]) big = i;
        }
        /* Force the total to exactly FDV_RANS_SCALE by correcting the largest bin
         * (its frequency dominates, so the relative distortion is negligible). */
        freq[big] = (uint16_t)((int)freq[big] + ((int)FDV_RANS_SCALE - (int)sum));
    }

    uint16_t cum = 0;
    for (int s = 0; s < 256; ++s) {
        enc[s].freq = freq[s];
        enc[s].cum  = cum;
        dec->freq[s] = freq[s];
        dec->cum[s]  = cum;
        for (uint16_t k = 0; k < freq[s]; ++k)
            dec->slot2sym[cum + k] = (uint8_t)s;
        cum = (uint16_t)(cum + freq[s]);
    }
}

void fdv_rans_enc_from_freq(const uint16_t freq[256], fdv_rans_sym enc[256]) {
    uint16_t cum = 0;
    for (int s = 0; s < 256; ++s) {
        enc[s].freq = freq[s];
        enc[s].cum  = cum;
        cum = (uint16_t)(cum + freq[s]);
    }
}

void fdv_rans_dec_table_from_freq(const uint16_t freq[256], fdv_rans_table *dec) {
    uint16_t cum = 0;
    for (int s = 0; s < 256; ++s) {
        dec->freq[s] = freq[s];
        dec->cum[s]  = cum;
        for (uint16_t k = 0; k < freq[s]; ++k)
            dec->slot2sym[cum + k] = (uint8_t)s;
        cum = (uint16_t)(cum + freq[s]);
    }
}

size_t fdv_rans_write_freqs(uint8_t *out, size_t o, const uint16_t freq[256]) {
    int n = 0;
    for (int s = 0; s < 256; ++s) if (freq[s]) ++n;
    out[o++] = (uint8_t)(n & 0xff);
    out[o++] = (uint8_t)(n >> 8);
    for (int s = 0; s < 256; ++s) {
        if (!freq[s]) continue;
        out[o++] = (uint8_t)s;
        uint32_t f = freq[s];
        while (f >= 0x80) { out[o++] = (uint8_t)(f | 0x80); f >>= 7; }
        out[o++] = (uint8_t)f;
    }
    return o;
}

size_t fdv_rans_freqs_size(const uint16_t freq[256]) {
    size_t n = 2;                       /* the 16-bit count */
    for (int s = 0; s < 256; ++s) {
        if (!freq[s]) continue;
        n += 1;                          /* symbol byte */
        uint32_t f = freq[s];
        do { n += 1; f >>= 7; } while (f);
    }
    return n;
}

size_t fdv_rans_read_freqs(const uint8_t *in, size_t p, uint16_t freq[256]) {
    for (int s = 0; s < 256; ++s) freq[s] = 0;
    int n = in[p] | ((int)in[p + 1] << 8);
    p += 2;
    for (int k = 0; k < n; ++k) {
        int s = in[p++];
        uint32_t f = 0; int sh = 0; uint8_t c;
        do { c = in[p++]; f |= (uint32_t)(c & 0x7f) << sh; sh += 7; } while (c & 0x80);
        freq[s] = (uint16_t)f;
    }
    return p;
}

size_t fdv_rans_read_freqs_bounded(const uint8_t *in, size_t p, size_t end,
                               uint16_t freq[256], int *ok) {
    for (int s = 0; s < 256; ++s) freq[s] = 0;
    if (p + 2 > end) { *ok = 0; return p; }
    int n = in[p] | ((int)in[p + 1] << 8);
    p += 2;
    uint32_t sum = 0;
    for (int k = 0; k < n; ++k) {
        if (p >= end) { *ok = 0; return p; }
        int s = in[p++];
        uint32_t f = 0; int sh = 0; uint8_t c;
        do {
            if (p >= end || sh > 28) { *ok = 0; return p; }
            c = in[p++]; f |= (uint32_t)(c & 0x7f) << sh; sh += 7;
        } while (c & 0x80);
        if (freq[s] != 0) { *ok = 0; return p; }   /* duplicate symbol entry */
        freq[s] = (uint16_t)f;
        sum += f;
    }
    /* The decode slot table is sized exactly FDV_RANS_SCALE and is fully populated
     * only when the frequencies sum to FDV_RANS_SCALE with no symbol exceeding it;
     * anything else would leave gaps or overflow slot2sym, so reject it. */
    if (sum != FDV_RANS_SCALE) *ok = 0;
    return p;
}

void fdv_rans_decode_bounded(const uint8_t *in, size_t inlen, size_t n,
                         const fdv_rans_table *dec, uint8_t *syms) {
    const uint8_t *ptr = in, *end = in + inlen;
    uint32_t r = 0;
    for (int b = 0; b < 4; ++b) r |= (uint32_t)(ptr < end ? *ptr++ : 0) << (8 * b);
    for (size_t i = 0; i < n; ++i) {
        uint8_t s = dec->slot2sym[r & (FDV_RANS_SCALE - 1)];
        syms[i] = s;
        uint32_t x = dec->freq[s] * (r >> FDV_RANS_SCALE_BITS)
                   + (r & (FDV_RANS_SCALE - 1)) - dec->cum[s];
        /* Renormalize, but stop once the input is exhausted: a corrupt stream
         * can drive x to 0, and zero-feeding would never lift it past RANS_L,
         * spinning forever. Valid streams never need a byte past `end`, so this
         * is bit-exact for them. Remaining symbols decode from the stalled
         * state (bounded garbage) and the parse layer rejects them. */
        while (x < RANS_L && ptr < end) x = (x << 8) | *ptr++;
        r = x;
    }
}

/* ---- Single-state codec -------------------------------------------------- */

size_t fdv_rans_encode(const uint8_t *syms, size_t n, const fdv_rans_sym enc[256],
                   uint8_t *out, size_t out_cap) {
    uint8_t *ptr = out + out_cap;
    RansState r;
    enc_init(&r);
    for (size_t i = n; i-- > 0; )           /* reverse order */
        enc_put(&r, &ptr, enc[syms[i]].cum, enc[syms[i]].freq);
    enc_flush(&r, &ptr);

    size_t len = (size_t)(out + out_cap - ptr);
    memmove(out, ptr, len);                 /* compact to the front */
    return len;
}

void fdv_rans_decode(const uint8_t *in, size_t n, const fdv_rans_table *dec,
                 uint8_t *syms) {
    const uint8_t *ptr = in;
    RansState r;
    dec_init(&r, &ptr);
    for (size_t i = 0; i < n; ++i) {
        uint8_t s = dec->slot2sym[dec_get(&r)];
        syms[i] = s;
        dec_advance(&r, &ptr, dec->cum[s], dec->freq[s]);
    }
}

/* ---- Two-way interleaved codec ------------------------------------------- *
 * Two states alternate by symbol parity. Both share the byte stream; because
 * rANS is LIFO and decode walks symbols in the exact reverse order of encode
 * with the same parity-to-state mapping, the two chains stay independent and
 * overlap on a superscalar pipeline. */

size_t fdv_rans_encode_il2(const uint8_t *syms, size_t n, const fdv_rans_sym enc[256],
                       uint8_t *out, size_t out_cap) {
    uint8_t *ptr = out + out_cap;
    RansState r0, r1;
    enc_init(&r0);
    enc_init(&r1);
    for (size_t i = n; i-- > 0; ) {
        RansState *r = (i & 1) ? &r1 : &r0;
        enc_put(r, &ptr, enc[syms[i]].cum, enc[syms[i]].freq);
    }
    /* Flush r1 then r0 so r0's state bytes land first and are read first. */
    enc_flush(&r1, &ptr);
    enc_flush(&r0, &ptr);

    size_t len = (size_t)(out + out_cap - ptr);
    memmove(out, ptr, len);
    return len;
}

void fdv_rans_decode_il2(const uint8_t *in, size_t n, const fdv_rans_table *dec,
                     uint8_t *syms) {
    const uint8_t *ptr = in;
    RansState r0, r1;
    dec_init(&r0, &ptr);
    dec_init(&r1, &ptr);
    for (size_t i = 0; i < n; ++i) {
        RansState *r = (i & 1) ? &r1 : &r0;
        uint8_t s = dec->slot2sym[dec_get(r)];
        syms[i] = s;
        dec_advance(r, &ptr, dec->cum[s], dec->freq[s]);
    }
}

/* --- Adaptive range coder ------------------------------------------------- */

static void am_build(fdv_amodel *m) {
    uint32_t s = 0;
    m->t[0] = 0;
    for (int i = 0; i < 256; ++i) { m->t[i + 1] = m->f[i]; s += m->f[i]; }
    for (int i = 1; i <= 256; ++i) {
        int j = i + (i & -i);
        if (j <= 256) m->t[j] += m->t[i];
    }
    m->tot = s;
}

/* Normalizing the history to a fixed total separates three things that
 * otherwise fight each other: how far back the history sees (FDV_AD_HIST), how
 * fast this frame's statistics overtake it (FDV_AD_INC against FDV_AD_SCALE),
 * and how much precision the range coder keeps (its range/total division, which
 * wants the total small). Raising the history bound to get a better prior used
 * to cost 1-2% in coder precision; now it costs nothing.
 *
 * A history shorter than the scale is left unscaled rather than stretched up to
 * it -- few frames of evidence should make a weak prior that this frame's own
 * symbols can overrule quickly, which is exactly what a small total does. */
void fdv_amodel_init(fdv_amodel *m, const uint32_t hist[256]) {
    uint64_t s = 0;
    for (int i = 0; i < 256; ++i) s += hist[i];
    if (s <= FDV_AD_SCALE)
        for (int i = 0; i < 256; ++i) m->f[i] = FDV_AD_BASE + hist[i];
    else
        for (int i = 0; i < 256; ++i)
            m->f[i] = FDV_AD_BASE +
                (uint32_t)(((uint64_t)hist[i] * FDV_AD_SCALE + s / 2) / s);
    am_build(m);
}

/* Sum of f[0 .. s). */
static uint32_t am_cum(const fdv_amodel *m, unsigned s) {
    uint32_t r = 0;
    for (int i = (int)s; i > 0; i -= i & -i) r += m->t[i];
    return r;
}

/* Halving the whole model past a bound is what lets the adaptation rate be
 * chosen freely: without it, a fast rate inflates the total over a long stream,
 * and the coder's range/total division gives back in precision what the model
 * gained. With it the total is bounded, so precision is fixed and FDV_AD_INC
 * only sets how quickly the frame's own statistics take over -- the model
 * becomes an exponential decay with a half-life of FDV_AD_MAX/2 weight. */
static void am_add(fdv_amodel *m, unsigned s, uint32_t d) {
    m->f[s] += d;
    m->tot  += d;
    if (m->tot > FDV_AD_MAX) {
        for (int i = 0; i < 256; ++i) m->f[i] = (m->f[i] + 1) >> 1;
        am_build(m);                       /* recomputes the tree and the total */
        return;
    }
    for (int i = (int)s + 1; i <= 256; i += i & -i) m->t[i] += d;
}

/* Largest s with cum(s) <= v, plus that cum. Every weight is at least
 * FDV_AD_BASE, so no zero-width symbol can be landed on and the descent is
 * exact. v < tot is guaranteed by the caller, so s stays under 256.
 *
 * Folding the caller's `code / r` division into the descent as `cum * r <=
 * code` is the same predicate with one fewer division, and measures neither
 * faster nor slower at 720p: the divisions sit at the top of the symbol where
 * they overlap with the work after them, and the descent's eight dependent
 * loads are what the loop actually waits on. Kept in this form because it is
 * the clearer one. */
static unsigned am_find(const fdv_amodel *m, uint32_t v, uint32_t *cum) {
    unsigned pos = 0; uint32_t rem = v;
    for (unsigned pw = 128; pw; pw >>= 1) {
        unsigned np = pos + pw;
        if (np <= 256 && m->t[np] <= rem) { pos = np; rem -= m->t[np]; }
    }
    *cum = v - rem;
    return pos;
}

/* Carry-propagating range coder. `low` is 33 bits wide so a carry out of the
 * top can be seen and rippled into the bytes already staged in cache/csize. */
void fdv_aenc_init(fdv_aenc *e, uint8_t *out, size_t cap) {
    e->out = out; e->cap = cap; e->n = 0;
    e->low = 0; e->range = 0xFFFFFFFFu;
    e->cache = 0; e->csize = 1; e->fail = 0;
}

static void ae_shift(fdv_aenc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (uint32_t)(e->low >> 32) != 0) {
        uint8_t c = e->cache, carry = (uint8_t)(e->low >> 32);
        do {
            if (e->n < e->cap) e->out[e->n++] = (uint8_t)(c + carry);
            else               e->fail = 1;
            c = 0xFF;
        } while (--e->csize);
        e->cache = (uint8_t)((uint32_t)e->low >> 24);
    }
    e->csize++;
    e->low = (uint32_t)e->low << 8;
}

void fdv_aenc_sym(fdv_aenc *e, fdv_amodel *m, unsigned s) {
    uint32_t r = e->range / m->tot;
    e->low  += (uint64_t)r * am_cum(m, s);
    e->range = r * m->f[s];
    while (e->range < (1u << 24)) { ae_shift(e); e->range <<= 8; }
    am_add(m, s, FDV_AD_INC);
}

size_t fdv_aenc_finish(fdv_aenc *e) {
    for (int i = 0; i < 5; ++i) ae_shift(e);
    if (e->fail || e->n < 1) return 0;
    /* The first byte is the initial empty cache and is always zero: the first
     * shift happens with low below 2^32, so nothing can carry into it. Drop it
     * rather than spend a byte a frame saying so. */
    memmove(e->out, e->out + 1, e->n - 1);
    return e->n - 1;
}

void fdv_adec_init(fdv_adec *d, const uint8_t *in, size_t len) {
    d->in = in; d->len = len; d->p = 0;
    d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 4; ++i) {
        d->code = (d->code << 8) | (d->p < d->len ? d->in[d->p] : 0);
        d->p++;
    }
}

unsigned fdv_adec_sym(fdv_adec *d, fdv_amodel *m) {
    uint32_t r = d->range / m->tot;
    uint32_t v = d->code / r;
    if (v >= m->tot) v = m->tot - 1;          /* only reachable on corrupt input */
    uint32_t cum; unsigned s = am_find(m, v, &cum);
    d->code -= r * cum;
    d->range = r * m->f[s];
    while (d->range < (1u << 24)) {
        d->code = (d->code << 8) | (d->p < d->len ? d->in[d->p] : 0);
        d->p++;
        d->range <<= 8;
    }
    am_add(m, s, FDV_AD_INC);
    return s;
}

/* ===========================================================================
 * 5. COEFF8
 * 8x8 coefficient serialization: zigzag + end-of-block.
 * ======================================================================== */


/* Standard 8x8 zigzag: low frequencies first so the trailing zeros run together
 * and the end-of-block count cuts them off cheaply. */
const int fdv_zz8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* The 4x4 coders write through these, so the plane a block belongs to picks the
 * stream without any of them needing to know which plane they are on. */
static inline void cw_count(fdv_cw *w, unsigned v) {
    if (w->chroma) w->nc[w->ncp++] = (uint8_t)v;
    else           w->n[w->np++]   = (uint8_t)v;
}
/* Move a trial's 4x4 output into the stream the current plane owns. The trial
 * writes into its own buffers with no plane of its own, so the routing has to
 * happen here rather than at the point the bytes were produced. */
static inline void cw_merge4(fdv_cw *w, const uint8_t *n, size_t np,
                             const uint8_t *l, size_t lp) {
    if (w->chroma) {
        memcpy(w->nc + w->ncp, n, np); w->ncp += np;
        memcpy(w->lc + w->lcp, l, lp); w->lcp += lp;
    } else {
        memcpy(w->n + w->np, n, np); w->np += np;
        memcpy(w->l + w->lp, l, lp); w->lp += lp;
    }
}

static inline void cw_level(fdv_cw *w, uint32_t z) {
    if (w->chroma) w->lcp = fdv_leb_put(w->lc, w->lcp, z);
    else           w->lp  = fdv_leb_put(w->l,  w->lp,  z);
}
static inline int cr_count(fdv_cr *r, int cap, int *ok) {
    return r->chroma ? fdv_rd_count(r->b, &r->ncp, r->ncend, cap, ok)
                     : fdv_rd_count(r->b, &r->np,  r->nend,  cap, ok);
}
static inline int cr_level(fdv_cr *r, int *ok) {
    return r->chroma ? fdv_rd_level(r->b, &r->lcp, r->lcend, ok)
                     : fdv_rd_level(r->b, &r->lp,  r->lend,  ok);
}

void fdv_coeff8_encode(const int16_t level[64], fdv_cw *w) {
    int last = -1;
    for (int k = 0; k < 64; ++k) if (level[fdv_zz8[k]] != 0) last = k;
    w->n8[w->np8++] = (uint8_t)(last + 1);      /* 0..64 */
    for (int k = 0; k <= last; ++k)
        w->lp8 = fdv_leb_put(w->l8, w->lp8, fdv_zz_enc(level[fdv_zz8[k]]));
}

void fdv_coeff8_decode(fdv_cr *r, int *ok, int16_t level[64]) {
    for (int i = 0; i < 64; ++i) level[i] = 0;
    int cnt = fdv_rd_count(r->b, &r->np8, r->nend8, 64, ok);
    for (int k = 0; k < cnt; ++k)
        level[fdv_zz8[k]] = (int16_t)fdv_rd_level(r->b, &r->lp8, r->lend8, ok);
}

/* ===========================================================================
 * 6. TRANSFORM
 * Integer 4x4/8x8 transforms, quantization, RDOQ.
 * ======================================================================== */


#define FDV_PI 3.14159265358979323846

/* ---- Core transforms -----------------------------------------------------
 *
 * Forward matrix Cf has orthogonal rows with norms 2, sqrt(10), 2, sqrt(10):
 *
 *     1  1  1  1
 *     2  1 -1 -2
 *     1 -1 -1  1
 *     1 -2  2 -1
 *
 * The forward 1-D butterfly computes Cf * v; the inverse 1-D butterfly computes
 * the matching Ci * v (the H.264 inverse core, which uses >>1 in place of the
 * 2's so it stays multiply-free). Both are applied to rows then columns. */

static void fwd_1d(const int *in, int *out) {
    int a0 = in[0] + in[3];
    int a3 = in[0] - in[3];
    int a1 = in[1] + in[2];
    int a2 = in[1] - in[2];
    out[0] = a0 + a1;
    /* a3 and a2 are differences and may be negative; shifting a negative left
     * is undefined, so scale by multiplication instead. Same value, defined. */
    out[1] = a3 * 2 + a2;
    out[2] = a0 - a1;
    out[3] = a3 - a2 * 2;
}

static void inv_1d(const int *in, int *out) {
    int z0 = in[0] + in[2];
    int z1 = in[0] - in[2];
    int z2 = (in[1] >> 1) - in[3];
    int z3 = in[1] + (in[3] >> 1);
    out[0] = z0 + z3;
    out[1] = z1 + z2;
    out[2] = z1 - z2;
    out[3] = z0 - z3;
}

void fdv_fdct4x4_scalar(const int16_t residual[16], int32_t coeff[16]) {
    int tmp[16];
    int row[4], res[4];

    /* Horizontal pass: transform each row. */
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) row[j] = residual[i * 4 + j];
        fwd_1d(row, res);
        for (int j = 0; j < 4; ++j) tmp[i * 4 + j] = res[j];
    }
    /* Vertical pass: transform each column. */
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) row[i] = tmp[i * 4 + j];
        fwd_1d(row, res);
        for (int i = 0; i < 4; ++i) coeff[i * 4 + j] = res[i];
    }
}

void fdv_idct4x4_scalar(const int32_t dcoeff[16], int16_t residual[16]) {
    int tmp[16];
    int row[4], res[4];

    /* Horizontal pass. */
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) row[j] = dcoeff[i * 4 + j];
        inv_1d(row, res);
        for (int j = 0; j < 4; ++j) tmp[i * 4 + j] = res[j];
    }
    /* Vertical pass, then normalize by (x + 32) >> 6. */
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) row[i] = tmp[i * 4 + j];
        inv_1d(row, res);
        for (int i = 0; i < 4; ++i)
            residual[i * 4 + j] = (int16_t)((res[i] + 32) >> 6);
    }
}

#if defined(__ARM_NEON)
/* Transpose four int32x4 rows in place (rows <-> columns). */
static inline void transpose4(int32x4_t *a, int32x4_t *b,
                              int32x4_t *c, int32x4_t *d) {
    int32x4x2_t t0 = vtrnq_s32(*a, *b);
    int32x4x2_t t1 = vtrnq_s32(*c, *d);
    *a = vcombine_s32(vget_low_s32(t0.val[0]), vget_low_s32(t1.val[0]));
    *b = vcombine_s32(vget_low_s32(t0.val[1]), vget_low_s32(t1.val[1]));
    *c = vcombine_s32(vget_high_s32(t0.val[0]), vget_high_s32(t1.val[0]));
    *d = vcombine_s32(vget_high_s32(t0.val[1]), vget_high_s32(t1.val[1]));
}

/* One inverse 1-D butterfly applied to the four position-vectors a0..a3
 * (lanes = the four rows/columns), matching inv_1d exactly. */
static inline void bfly(int32x4_t a0, int32x4_t a1, int32x4_t a2, int32x4_t a3,
                        int32x4_t *o0, int32x4_t *o1, int32x4_t *o2, int32x4_t *o3) {
    int32x4_t z0 = vaddq_s32(a0, a2);
    int32x4_t z1 = vsubq_s32(a0, a2);
    int32x4_t z2 = vsubq_s32(vshrq_n_s32(a1, 1), a3);
    int32x4_t z3 = vaddq_s32(a1, vshrq_n_s32(a3, 1));
    *o0 = vaddq_s32(z0, z3);
    *o1 = vaddq_s32(z1, z2);
    *o2 = vsubq_s32(z1, z2);
    *o3 = vsubq_s32(z0, z3);
}

/* One forward 1-D butterfly (matches fwd_1d) over four position-vectors. */
static inline void bfly_f(int32x4_t v0, int32x4_t v1, int32x4_t v2, int32x4_t v3,
                          int32x4_t *o0, int32x4_t *o1, int32x4_t *o2, int32x4_t *o3) {
    int32x4_t a0 = vaddq_s32(v0, v3);
    int32x4_t a3 = vsubq_s32(v0, v3);
    int32x4_t a1 = vaddq_s32(v1, v2);
    int32x4_t a2 = vsubq_s32(v1, v2);
    *o0 = vaddq_s32(a0, a1);
    *o1 = vaddq_s32(vshlq_n_s32(a3, 1), a2);
    *o2 = vsubq_s32(a0, a1);
    *o3 = vsubq_s32(a3, vshlq_n_s32(a2, 1));
}

void fdv_fdct4x4_neon(const int16_t residual[16], int32_t coeff[16]) {
    int32x4_t r0 = vmovl_s16(vld1_s16(residual));
    int32x4_t r1 = vmovl_s16(vld1_s16(residual + 4));
    int32x4_t r2 = vmovl_s16(vld1_s16(residual + 8));
    int32x4_t r3 = vmovl_s16(vld1_s16(residual + 12));

    transpose4(&r0, &r1, &r2, &r3);
    int32x4_t b0, b1, b2, b3;
    bfly_f(r0, r1, r2, r3, &b0, &b1, &b2, &b3);
    transpose4(&b0, &b1, &b2, &b3);
    int32x4_t e0, e1, e2, e3;
    bfly_f(b0, b1, b2, b3, &e0, &e1, &e2, &e3);

    vst1q_s32(coeff,      e0);
    vst1q_s32(coeff + 4,  e1);
    vst1q_s32(coeff + 8,  e2);
    vst1q_s32(coeff + 12, e3);
}

void fdv_idct4x4_neon(const int32_t dcoeff[16], int16_t residual[16]) {
    int32x4_t r0 = vld1q_s32(dcoeff),     r1 = vld1q_s32(dcoeff + 4);
    int32x4_t r2 = vld1q_s32(dcoeff + 8), r3 = vld1q_s32(dcoeff + 12);

    /* Horizontal pass: transpose so each vector holds one position across all
     * rows, butterfly (lane = row), giving the row transform in column layout. */
    transpose4(&r0, &r1, &r2, &r3);
    int32x4_t b0, b1, b2, b3;
    bfly(r0, r1, r2, r3, &b0, &b1, &b2, &b3);

    /* Vertical pass: transpose back so each vector is a row of the intermediate,
     * butterfly the columns. The result lands row-major (e_i = result row i). */
    transpose4(&b0, &b1, &b2, &b3);
    int32x4_t e0, e1, e2, e3;
    bfly(b0, b1, b2, b3, &e0, &e1, &e2, &e3);

    int32x4_t off = vdupq_n_s32(32);
    e0 = vshrq_n_s32(vaddq_s32(e0, off), 6);
    e1 = vshrq_n_s32(vaddq_s32(e1, off), 6);
    e2 = vshrq_n_s32(vaddq_s32(e2, off), 6);
    e3 = vshrq_n_s32(vaddq_s32(e3, off), 6);
    vst1_s16(residual,      vmovn_s32(e0));
    vst1_s16(residual + 4,  vmovn_s32(e1));
    vst1_s16(residual + 8,  vmovn_s32(e2));
    vst1_s16(residual + 12, vmovn_s32(e3));
}
#endif /* __ARM_NEON */

void fdv_fdct4x4(const int16_t residual[16], int32_t coeff[16]) {
#if defined(__ARM_NEON)
    fdv_fdct4x4_neon(residual, coeff);
#else
    fdv_fdct4x4_scalar(residual, coeff);
#endif
}

void fdv_idct4x4(const int32_t dcoeff[16], int16_t residual[16]) {
#if defined(__ARM_NEON)
    fdv_idct4x4_neon(dcoeff, residual);
#else
    fdv_idct4x4_scalar(dcoeff, residual);
#endif
}

/* ---- Quantization --------------------------------------------------------
 *
 * Each coefficient position falls into one of three classes by parity, matching
 * the three distinct values of the forward scaling matrix (norm 4, norm 10, and
 * mixed). The MF (forward multiplier) and V (dequant rescale) tables are the
 * standard H.264 values, indexed by [QP % 6][class].
 *
 *   class 0: (i even, j even)   class 1: (i odd, j odd)   class 2: mixed */

static const int MF[6][3] = {
    {13107, 5243, 8066},
    {11916, 4660, 7490},
    {10082, 4194, 6554},
    { 9362, 3647, 5825},
    { 8192, 3355, 5243},
    { 7282, 2893, 4559},
};

static const int DEQUANT_V[6][3] = {
    {10, 16, 13},
    {11, 18, 14},
    {13, 20, 16},
    {14, 23, 18},
    {16, 25, 20},
    {18, 29, 23},
};

static int pos_class(int idx) {
    int i = idx >> 2, j = idx & 3;
    if ((i & 1) == 0 && (j & 1) == 0) return 0;
    if ((i & 1) == 1 && (j & 1) == 1) return 1;
    return 2;
}

void fdv_quant4x4(const int32_t coeff[16], int16_t level[16], int qp) {
    int m = qp % 6;
    int qbits = 15 + qp / 6;
    /* Dead-zone rounding offset: ~1/3 of a step (intra). */
    int f = (1 << qbits) / 3;

    for (int idx = 0; idx < 16; ++idx) {
        int cls = pos_class(idx);
        int w = coeff[idx];
        int sign = (w < 0) ? -1 : 1;
        long aw = (long)(w < 0 ? -w : w);
        int lvl = (int)((aw * MF[m][cls] + f) >> qbits);
        level[idx] = (int16_t)(sign * lvl);
    }
}

/* ---- RD coefficient truncation (RDOQ-lite) ------------------------------- */

static const int RDZZ[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static uint32_t rd_zz_enc(int v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
static int rd_leb_size(uint32_t u) { int n = 1; while (u >= 0x80) { u >>= 7; ++n; } return n; }

/* Rate estimate, in bits, for one coded value.
 *
 * The residual coders used to charge eight bits for every byte they emitted.
 * That is what a byte costs in the *container*, not what it costs in the
 * *coder*: rANS and the adaptive model both pay by entropy, and a level byte
 * carrying zero costs well under a bit where one carrying two hundred costs six
 * or seven. Charging them the same made the encoder value "fewer bytes" above
 * "cheaper bytes" -- a bias with nothing to act on while every block was 4x4
 * and every transform the same size, and a large one once the quadtree offered
 * 8x8 transforms and 16x16 predictions. On dense grain at QP 16 it took the
 * trade everywhere and paid 29% more bits for the same picture.
 *
 * An exp-Golomb code length is the standard stand-in for an entropy coder's
 * real cost, and it is what this returns: 1 bit for zero, 3 for one or two,
 * 5 up to six, and so on.
 *
 * It is a stand-in and not a measurement, which looks like the obvious thing
 * left to fix. It was tried and it is not. Coding each frame twice -- once to
 * find out what its symbols cost, then again against -log2(p) per symbol per
 * stream -- measures +0.03% mean and -1.22% median BD-rate at the current
 * lambda, and -0.24% mean at the best of a five-point lambda sweep. For twice
 * the encode time. A third pass is no better than the second, so it is not that
 * the table is stale.
 *
 * The reason is that exp-Golomb *is* the entropy of a geometric source, and
 * coefficient levels are close to geometric. Where they are not -- dense grain,
 * fine checkerboards -- measuring does help, and helps a lot (`stress` -27.7%,
 * `detail` -3.1%); everywhere else it moves nothing, and on one scene it costs
 * 12% because the numeric scale shifts under a lambda tuned for the guess.
 *
 * So what went wrong before this function existed was never accuracy. It was
 * *shape*: charging eight bits for every emitted byte is flat where the true
 * cost grows, and a flat rate term made the encoder prefer fewer bytes over
 * cheaper ones. Getting the shape right was worth -23%; getting the magnitude
 * right on top of that is worth nothing. */
static int fdv_bits_val(uint32_t v) {
    int n = 0;
    for (uint32_t x = v + 1; x > 1; x >>= 1) ++n;   /* floor(log2(v + 1)) */
    return 2 * n + 1;
}

static void fdv_rdoq4x4_inner(int16_t level[16], const int16_t res[16], int qp, double lambda) {
    int last = -1;
    for (int k = 0; k < 16; ++k) if (level[RDZZ[k]] != 0) last = k;
    if (last < 0) return;

    /* Candidate last-nonzero positions: each currently-nonzero zigzag index and
     * -1 (the all-zero block). For each, reconstruct and score D + lambda*rate.
     *
     * The candidates form a chain -- each drops exactly one more trailing
     * coefficient than the last -- so the trial block and its rate are carried
     * forward and one coefficient is removed per step, rather than rebuilt from
     * scratch every time. Same candidates, same order, same scores. */
    int16_t trial[16];
    memcpy(trial, level, sizeof trial);        /* nothing past `last` is nonzero */
    int bits = fdv_bits_val((uint32_t)(last + 1));   /* end-of-block count */
    for (int k = 0; k <= last; ++k)
        bits += fdv_bits_val(rd_zz_enc(level[RDZZ[k]]));

    double bestJ = -1.0;
    int bestL = last;
    for (int L = last; L >= -1; --L) {
        if (L < 0 || level[RDZZ[L]] != 0) {
            int32_t dc[16];
            int16_t rr[16];
            fdv_dequant4x4(trial, dc, qp);
            fdv_idct4x4(dc, rr);
            /* Integer SSD: every term is exact, and the total stays far inside
             * what a double represents exactly, so this scores identically to
             * accumulating in floating point per pixel. */
            int64_t D = 0;
            for (int i = 0; i < 16; ++i) {
                int d = res[i] - rr[i];
                D += (int64_t)d * d;
            }
            double J = (double)D + lambda * bits;
            if (bestJ < 0.0 || J < bestJ) { bestJ = J; bestL = L; }
        }
        if (L >= 0) {                          /* drop coefficient L, then step */
            bits -= 8 * rd_leb_size(rd_zz_enc(level[RDZZ[L]]));
            trial[RDZZ[L]] = 0;
        }
    }
    for (int k = bestL + 1; k < 16; ++k) level[RDZZ[k]] = 0;
}

/* Instrumented entry point; the work is in fdv_rdoq4x4_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_rdoq4x4(int16_t level[16], const int16_t res[16], int qp, double lambda) {
    FDV_ZB(FDV_Z_RDOQ);
    fdv_rdoq4x4_inner(level, res, qp, lambda);
    FDV_ZE(FDV_Z_RDOQ);
}

void fdv_dequant4x4_scalar(const int16_t level[16], int32_t dcoeff[16], int qp) {
    int m = qp % 6;
    int shift = qp / 6;
    for (int idx = 0; idx < 16; ++idx) {
        int cls = pos_class(idx);
        /* The level is signed, so scale by multiplication rather than a left
         * shift, which is undefined for negative values. */
        dcoeff[idx] = (int32_t)(level[idx] * DEQUANT_V[m][cls]) * (int32_t)(1 << shift);
    }
}

#if defined(__ARM_NEON)
void fdv_dequant4x4_neon(const int16_t level[16], int32_t dcoeff[16], int qp) {
    int m = qp % 6, shift = qp / 6;
    /* Fold the shift into the per-position scale: dcoeff = level * scale. */
    int32_t scale[16];
    for (int idx = 0; idx < 16; ++idx)
        scale[idx] = (int32_t)DEQUANT_V[m][pos_class(idx)] << shift;
    for (int b = 0; b < 16; b += 4) {
        int32x4_t l = vmovl_s16(vld1_s16(level + b));
        vst1q_s32(dcoeff + b, vmulq_s32(l, vld1q_s32(scale + b)));
    }
}
#endif

void fdv_dequant4x4(const int16_t level[16], int32_t dcoeff[16], int qp) {
#if defined(__ARM_NEON)
    fdv_dequant4x4_neon(level, dcoeff, qp);
#else
    fdv_dequant4x4_scalar(level, dcoeff, qp);
#endif
}

/* ---- 8x8 fixed-point orthonormal integer DCT-II -------------------------- *
 * Basis scaled by 64; AC rows are nudged to sum to zero so a flat block maps
 * to DC only. The forward/inverse cascade (no quant) reconstructs to within
 * fixed-point rounding; quant adds the lossy step. Not multiply-free — a
 * butterfly form is possible future work. */

static int32_t C8[8][8];
static int32_t QSTEP8[52];

/* The 8x8 tables are built on first use.  A plain `if (!ready) init()` flag is
 * a data race the moment more than one thread codes at once — and both the
 * tiled encoder and the tiled decoder do exactly that.  Every thread would
 * compute identical values, so it tends to look harmless, but a reader can
 * still observe a half-written table, which would corrupt a coefficient and
 * desynchronise encoder from decoder.  pthread_once makes it defined: exactly
 * one initialisation, and every later caller sees it complete. */
static pthread_once_t c8_once = PTHREAD_ONCE_INIT;

static void c8_init(void) {
    for (int u = 0; u < 8; ++u) {
        double a = (u == 0) ? sqrt(1.0 / 8.0) : sqrt(2.0 / 8.0);
        for (int x = 0; x < 8; ++x)
            C8[u][x] = (int32_t)lround(a * cos((2 * x + 1) * u * FDV_PI / 16.0) * 512.0);
    }
    for (int u = 1; u < 8; ++u) {           /* force AC rows to zero-sum */
        int s = 0;
        for (int x = 0; x < 8; ++x) s += C8[u][x];
        C8[u][0] -= s;
    }
    for (int qp = 0; qp < 52; ++qp) {
        double qstep = 0.625 * pow(2.0, qp / 6.0);
        QSTEP8[qp] = (int32_t)lround(262144.0 * qstep);   /* 512^2 * Qstep */
    }
}

static void fdv_fdct8x8_inner(const int16_t X[64], int32_t Y[64]) {
    pthread_once(&c8_once, c8_init);
    int32_t A[64];
    for (int u = 0; u < 8; ++u)
        for (int x = 0; x < 8; ++x) {
            int32_t s = 0;
            for (int y = 0; y < 8; ++y) s += C8[u][y] * (int32_t)X[y * 8 + x];
            A[u * 8 + x] = s;
        }
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            int32_t s = 0;
            for (int x = 0; x < 8; ++x) s += A[u * 8 + x] * C8[v][x];
            Y[u * 8 + v] = s;
        }
}

/* Instrumented entry point; the work is in fdv_fdct8x8_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_fdct8x8(const int16_t X[64], int32_t Y[64]) {
    FDV_ZB(FDV_Z_RESID8);
    fdv_fdct8x8_inner(X, Y);
    FDV_ZE(FDV_Z_RESID8);
}

static void fdv_idct8x8_inner(const int32_t Y[64], int16_t X[64]) {
    pthread_once(&c8_once, c8_init);
    int64_t B[64];
    for (int y = 0; y < 8; ++y)
        for (int v = 0; v < 8; ++v) {
            int64_t s = 0;
            for (int u = 0; u < 8; ++u) s += (int64_t)C8[u][y] * Y[u * 8 + v];
            B[y * 8 + v] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            int64_t s = 0;
            for (int v = 0; v < 8; ++v) s += B[y * 8 + v] * C8[v][x];
            X[y * 8 + x] = (int16_t)((s + ((int64_t)1 << 35)) >> 36);
        }
}

/* Instrumented entry point; the work is in fdv_idct8x8_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_idct8x8(const int32_t Y[64], int16_t X[64]) {
    FDV_ZB(FDV_Z_RESID8);
    fdv_idct8x8_inner(Y, X);
    FDV_ZE(FDV_Z_RESID8);
}

void fdv_quant8x8(const int32_t Y[64], int16_t level[64], int qp) {
    pthread_once(&c8_once, c8_init);
    int32_t q = QSTEP8[qp], f = q / 3;          /* dead-zone offset */
    for (int i = 0; i < 64; ++i) {
        int32_t c = Y[i];
        int sign = c < 0 ? -1 : 1;
        int32_t a = c < 0 ? -c : c;
        level[i] = (int16_t)(sign * ((a + f) / q));
    }
}

void fdv_dequant8x8(const int16_t level[64], int32_t Y[64], int qp) {
    pthread_once(&c8_once, c8_init);
    int32_t q = QSTEP8[qp];
    /* level comes from the bitstream; a corrupt stream can carry a full-range
     * int16, and level*q would overflow int32. Saturate so the inverse
     * transform stays defined (valid streams have small levels, unaffected). */
    for (int i = 0; i < 64; ++i) {
        int64_t v = (int64_t)level[i] * q;
        Y[i] = v < INT32_MIN ? INT32_MIN : (v > INT32_MAX ? INT32_MAX : (int32_t)v);
    }
}

/* ===========================================================================
 * 7. INTRA
 * Intra prediction from reconstructed neighbors.
 * ======================================================================== */


/* Reference samples are indexed with the top-left corner at index -1:
 *   T(-1)=corner, T(0..7)=top + top-right
 *   L(-1)=corner, L(0..3)=left
 * which lets the H.264 prediction equations be written directly. */

void fdv_intra_predict_4x4(int mode, const uint8_t top[8], const uint8_t left[4],
                       uint8_t topleft, int have_top, int have_left,
                       uint8_t pred[16]) {
    int Tn[9], Ln[5];
    Tn[0] = topleft;
    for (int i = 0; i < 8; ++i) Tn[i + 1] = top[i];
    Ln[0] = topleft;
    for (int i = 0; i < 4; ++i) Ln[i + 1] = left[i];
#define T(i) Tn[(i) + 1]
#define L(i) Ln[(i) + 1]
#define P(x, y) pred[(y) * 4 + (x)]

    switch (mode) {
    case FDV_INTRA_VERT:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) P(x, y) = (uint8_t)T(x);
        break;

    case FDV_INTRA_HORIZ:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) P(x, y) = (uint8_t)L(y);
        break;

    case FDV_INTRA_DDL:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int v = (x == 3 && y == 3)
                    ? (T(6) + 3 * T(7) + 2) >> 2
                    : (T(x + y) + 2 * T(x + y + 1) + T(x + y + 2) + 2) >> 2;
                P(x, y) = (uint8_t)v;
            }
        break;

    case FDV_INTRA_DDR:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int v;
                if (x > y)      v = (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2;
                else if (x < y) v = (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2;
                else            v = (T(0) + 2 * T(-1) + L(0) + 2) >> 2;
                P(x, y) = (uint8_t)v;
            }
        break;

    case FDV_INTRA_VR:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int z = 2 * x - y, v;
                if (z >= 0 && (z & 1) == 0)
                    v = (T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 1) >> 1;
                else if (z >= 0)
                    v = (T(x - (y >> 1) - 2) + 2 * T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 2) >> 2;
                else if (z == -1)
                    v = (L(0) + 2 * T(-1) + T(0) + 2) >> 2;
                else
                    v = (L(y - 1) + 2 * L(y - 2) + L(y - 3) + 2) >> 2;
                P(x, y) = (uint8_t)v;
            }
        break;

    case FDV_INTRA_HD:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int z = 2 * y - x, v;
                if (z >= 0 && (z & 1) == 0)
                    v = (L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 1) >> 1;
                else if (z >= 0)
                    v = (L(y - (x >> 1) - 2) + 2 * L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 2) >> 2;
                else if (z == -1)
                    v = (T(0) + 2 * T(-1) + L(0) + 2) >> 2;
                else
                    v = (T(x - 1) + 2 * T(x - 2) + T(x - 3) + 2) >> 2;
                P(x, y) = (uint8_t)v;
            }
        break;

    case FDV_INTRA_VL:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int v = ((y & 1) == 0)
                    ? (T(x + (y >> 1)) + T(x + (y >> 1) + 1) + 1) >> 1
                    : (T(x + (y >> 1)) + 2 * T(x + (y >> 1) + 1) + T(x + (y >> 1) + 2) + 2) >> 2;
                P(x, y) = (uint8_t)v;
            }
        break;

    case FDV_INTRA_HU:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int z = x + 2 * y, v;
                if (z < 5 && (z & 1) == 0)
                    v = (L(y + (x >> 1)) + L(y + (x >> 1) + 1) + 1) >> 1;
                else if (z < 5)
                    v = (L(y + (x >> 1)) + 2 * L(y + (x >> 1) + 1) + L(y + (x >> 1) + 2) + 2) >> 2;
                else if (z == 5)
                    v = (L(2) + 3 * L(3) + 2) >> 2;
                else
                    v = L(3);
                P(x, y) = (uint8_t)v;
            }
        break;

    case FDV_INTRA_DC:
    default: {
        int dc;
        if (have_top && have_left) {
            int s = 0;
            for (int k = 0; k < 4; ++k) s += top[k] + left[k];
            dc = (s + 4) >> 3;
        } else if (have_top) {
            int s = 0;
            for (int k = 0; k < 4; ++k) s += top[k];
            dc = (s + 2) >> 2;
        } else if (have_left) {
            int s = 0;
            for (int k = 0; k < 4; ++k) s += left[k];
            dc = (s + 2) >> 2;
        } else {
            dc = 128;
        }
        for (int i = 0; i < 16; ++i) pred[i] = (uint8_t)dc;
        break;
    }
    }
#undef T
#undef L
#undef P
}

int fdv_intra_nn_mode_ok(int m, int n, int have_top, int have_left) {
    switch (m) {
    case FDV_INTRA_NN_DC:    return 1;
    case FDV_INTRA_NN_V:     return have_top;
    case FDV_INTRA_NN_H:     return have_left;
    /* The plane fit reads both edges and the corner, and the fixed-point
     * constants below are H.264's, which exist for 8 and 16 only. */
    case FDV_INTRA_NN_PLANE: return have_top && have_left &&
                                    (n == 8 || n == 16 || n == 32);
    }
    return 0;
}

/* H.264's plane predictor, generalized over the two sizes it defines.
 *
 * H and V are the edge gradients, each a weighted difference of the samples
 * either side of the edge's midpoint; a is the DC term taken from the two far
 * corners. The prediction is then the ramp a + b*(x - c0) + c*(y - c0), which
 * reproduces a linear gradient exactly. That is what most of a smooth frame is,
 * and DC, vertical and horizontal can none of them express it: each leaves the
 * gradient itself in the residual, at every block, for the transform to code.
 *
 * The multipliers (5 >> 6 at n=16, 17 >> 5 at n=8) are the standard fixed-point
 * renderings of the least-squares slope over the respective edge length. */
static void plane_pred(const uint8_t *top, const uint8_t *left, uint8_t topleft,
                       int n, uint8_t *pred) {
    int half = n / 2;
    int H = 0, V = 0;
    for (int k = 1; k <= half; ++k) {
        int tl = (half - 1 - k) < 0 ? topleft : top[half - 1 - k];
        int ll = (half - 1 - k) < 0 ? topleft : left[half - 1 - k];
        H += k * (top[half - 1 + k] - tl);
        V += k * (left[half - 1 + k] - ll);
    }
    int a = 16 * (left[n - 1] + top[n - 1]);
    int b, c;
    /* The multiplier is the least-squares slope over the edge, rendered in
     * fixed point so that a straight ramp of gradient m comes back as b = 32m,
     * which is what the >>5 below expects. It works out to 16/sum(k^2) for
     * k = 1..n/2: 17/32 at 8, 5/64 at 16, 11/1024 at 32. */
    if      (n == 32) { b = (11 * H + 512) >> 10; c = (11 * V + 512) >> 10; }
    else if (n == 16) { b = (5 * H + 32) >> 6;    c = (5 * V + 32) >> 6; }
    else              { b = (17 * H + 16) >> 5;   c = (17 * V + 16) >> 5; }
    int c0 = half - 1;
    for (int y = 0; y < n; ++y) {
        int base = a + c * (y - c0) + 16;
        for (int x = 0; x < n; ++x)
            pred[y * n + x] = clip255((base + b * (x - c0)) >> 5);
    }
}

/* n x n intra prediction: one prediction covers the whole block. */
static void fdv_intra_nxn_inner(int mode, const uint8_t *top, const uint8_t *left,
               uint8_t topleft, int n,
               int have_top, int have_left, uint8_t *pred) {
    if (mode == FDV_INTRA_NN_PLANE &&
        fdv_intra_nn_mode_ok(mode, n, have_top, have_left)) {
        plane_pred(top, left, topleft, n, pred);
    } else if (mode == FDV_INTRA_NN_V && have_top) {
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) pred[y * n + x] = top[x];
    } else if (mode == FDV_INTRA_NN_H && have_left) {
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) pred[y * n + x] = left[y];
    } else {
        int dc;
        if (have_top && have_left) {
            int s = 0;
            for (int k = 0; k < n; ++k) s += top[k] + left[k];
            dc = (s + n) / (2 * n);
        } else if (have_top) {
            int s = 0;
            for (int k = 0; k < n; ++k) s += top[k];
            dc = (s + n / 2) / n;
        } else if (have_left) {
            int s = 0;
            for (int k = 0; k < n; ++k) s += left[k];
            dc = (s + n / 2) / n;
        } else {
            dc = 128;
        }
        for (int i = 0; i < n * n; ++i) pred[i] = (uint8_t)dc;
    }
}

/* Instrumented entry point; the work is in fdv_intra_nxn_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_intra_nxn(int mode, const uint8_t *top, const uint8_t *left,
               uint8_t topleft, int n,
               int have_top, int have_left, uint8_t *pred) {
    FDV_ZB(FDV_Z_INTRA);
    fdv_intra_nxn_inner(mode, top, left, topleft, n, have_top, have_left, pred);
    FDV_ZE(FDV_Z_INTRA);
}

/* ===========================================================================
 * 8. INTER
 * Motion-compensated prediction and motion search.
 * ======================================================================== */


#define PIX(xx, yy) (*fdv_plane_at(r, (xx), (yy)))


/* The 6-tap half-pel kernel (1, -5, 20, 20, -5, 1), unnormalized. */
static int tap6(int a, int b, int c, int d, int e, int f) {
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}
/* Unclipped horizontal / vertical 6-tap at integer position (x,y). */
static int hf(const fdv_plane *r, int x, int y) {
    const uint8_t *p = fdv_plane_at(r, x, y);
    return tap6(p[-2], p[-1], p[0], p[1], p[2], p[3]);
}
static int vf(const fdv_plane *r, int x, int y) {
    const uint8_t *p = fdv_plane_at(r, x, y);
    int s = r->stride;
    return tap6(p[-2 * s], p[-s], p[0], p[s], p[2 * s], p[3 * s]);
}

/* The three half-pel positions, each computed on demand.  Kept separate so a
 * quarter-pel phase pays only for the halves it actually blends: `half_j` is
 * seven 6-taps and is needed by just five of the sixteen phases. */
static int half_b(const fdv_plane *r, int x, int y) {   /* (2,0) horizontal half */
    return clipb((hf(r, x, y) + 16) >> 5);
}
static int half_h(const fdv_plane *r, int x, int y) {   /* (0,2) vertical half   */
    return clipb((vf(r, x, y) + 16) >> 5);
}
static int half_j(const fdv_plane *r, int x, int y) {   /* (2,2) centre          */
    int jj = tap6(hf(r, x, y - 2), hf(r, x, y - 1), hf(r, x, y),
                  hf(r, x, y + 1), hf(r, x, y + 2), hf(r, x, y + 3));
    return clipb((jj + 512) >> 10);
}

/* H.264 luma interpolation at quarter-pel phase (fx,fy) over integer base (x,y):
 * half positions use the 6-tap filter, quarter positions average two adjacent
 * half/integer samples.
 *
 * Dispatch first, then compute.  The straightforward version evaluates every
 * intermediate (b, h, j, m, s) and returns one of them, which costs ten 6-taps
 * per pixel no matter the phase; switching first costs at most two for eleven
 * of the sixteen phases.  The arithmetic per case is unchanged, so output stays
 * bit-identical — which matters, because the decoder calls this too. */
static int interp_sample(const fdv_plane *r, int x, int y, int fx, int fy) {
    switch (fy * 4 + fx) {
    case  0: return PIX(x, y);                                          /* G */
    case  1: return (PIX(x, y)     + half_b(r, x, y)     + 1) >> 1;     /* a */
    case  2: return half_b(r, x, y);                                    /* b */
    case  3: return (half_b(r, x, y) + PIX(x + 1, y)     + 1) >> 1;     /* c */
    case  4: return (PIX(x, y)     + half_h(r, x, y)     + 1) >> 1;     /* d */
    case  5: return (half_b(r, x, y) + half_h(r, x, y)   + 1) >> 1;     /* e */
    case  6: return (half_b(r, x, y) + half_j(r, x, y)   + 1) >> 1;     /* f */
    case  7: return (half_b(r, x, y) + half_h(r, x + 1, y) + 1) >> 1;   /* g */
    case  8: return half_h(r, x, y);                                    /* h */
    case  9: return (half_h(r, x, y) + half_j(r, x, y)   + 1) >> 1;     /* i */
    case 10: return half_j(r, x, y);                                    /* j */
    case 11: return (half_j(r, x, y) + half_h(r, x + 1, y) + 1) >> 1;   /* k */
    case 12: return (half_h(r, x, y) + PIX(x, y + 1)     + 1) >> 1;     /* n */
    case 13: return (half_h(r, x, y) + half_b(r, x, y + 1) + 1) >> 1;   /* p */
    case 14: return (half_j(r, x, y) + half_b(r, x, y + 1) + 1) >> 1;   /* q */
    case 15: return (half_h(r, x + 1, y) + half_b(r, x, y + 1) + 1) >> 1; /* r */
    }
    return PIX(x, y);
}

#if defined(__ARM_NEON)
/* --- NEON kernels for the luma interpolation ----------------------------
 * Each is the exact integer arithmetic of its scalar counterpart, done eight
 * lanes wide.  Three NEON instructions happen to be precisely what the H.264
 * rounding rules ask for, which is what keeps this bit-exact rather than
 * merely close:
 *
 *   vrshrq_n_s16(s, 5)   is  (s + 16) >> 5      -- the half-pel normalisation
 *   vrshrq_n_s32(s, 10)  is  (s + 512) >> 10    -- the centre normalisation
 *   vrhaddq_u8(a, b)     is  (a + b + 1) >> 1   -- the quarter-pel blend
 *
 * and vqmovun_s16 is the [0,255] clamp.  Block widths are only ever 8 or 16,
 * so eight lanes divides evenly and there is no scalar tail. */

/* Unclipped horizontal 6-tap at eight consecutive positions.
 * Range: (a+f) <= 510, 20*(c+d) <= 10200, 5*(b+e) <= 2550, so the result and
 * every intermediate stay inside int16. */
static inline int16x8_t neon_hf8(const uint8_t *p) {
    int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p - 2)));
    int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p - 1)));
    int16x8_t c = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p    )));
    int16x8_t d = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 1)));
    int16x8_t e = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 2)));
    int16x8_t f = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 3)));
    int16x8_t s = vaddq_s16(a, f);
    s = vmlaq_n_s16(s, vaddq_s16(c, d), 20);
    s = vmlsq_n_s16(s, vaddq_s16(b, e), 5);
    return s;
}

/* Unclipped vertical 6-tap at eight consecutive columns of one row. */
static inline int16x8_t neon_vf8(const uint8_t *p, int stride) {
    int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p - 2 * stride)));
    int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p -     stride)));
    int16x8_t c = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p             )));
    int16x8_t d = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p +     stride)));
    int16x8_t e = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 2 * stride)));
    int16x8_t f = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 3 * stride)));
    int16x8_t s = vaddq_s16(a, f);
    s = vmlaq_n_s16(s, vaddq_s16(c, d), 20);
    s = vmlsq_n_s16(s, vaddq_s16(b, e), 5);
    return s;
}

/* Half-pel normalise and clamp: clipb((t + 16) >> 5) for eight lanes. */
static inline uint8x8_t neon_half(int16x8_t t) {
    return vqmovun_s16(vrshrq_n_s16(t, 5));
}

/* The centre position: vertical 6-tap over six rows of already-horizontally
 * filtered int16 values, accumulated in int32 (the product overflows int16),
 * then clipb((jj + 512) >> 10).
 * Range: r2+r3 <= 21420 and r1+r4 >= -5100, both inside int16; the int32
 * accumulator peaks around 450k. */
static inline uint8x8_t neon_centre(int16x8_t r0, int16x8_t r1, int16x8_t r2,
                                    int16x8_t r3, int16x8_t r4, int16x8_t r5) {
    int16x4_t cd_lo = vadd_s16(vget_low_s16(r2),  vget_low_s16(r3));
    int16x4_t cd_hi = vadd_s16(vget_high_s16(r2), vget_high_s16(r3));
    int16x4_t be_lo = vadd_s16(vget_low_s16(r1),  vget_low_s16(r4));
    int16x4_t be_hi = vadd_s16(vget_high_s16(r1), vget_high_s16(r4));

    int32x4_t lo = vaddl_s16(vget_low_s16(r0),  vget_low_s16(r5));
    int32x4_t hi = vaddl_s16(vget_high_s16(r0), vget_high_s16(r5));
    lo = vmlal_n_s16(lo, cd_lo, 20);
    hi = vmlal_n_s16(hi, cd_hi, 20);
    lo = vmlsl_n_s16(lo, be_lo, 5);
    hi = vmlsl_n_s16(hi, be_hi, 5);

    int16x8_t n = vcombine_s16(vmovn_s32(vrshrq_n_s32(lo, 10)),
                               vmovn_s32(vrshrq_n_s32(hi, 10)));
    return vqmovun_s16(n);
}
#endif /* __ARM_NEON */

/* Largest luma block motion compensation ever sees (a 16x16 macroblock); the
 * 8x8 partitions and the sub-pel search reuse the same path. */
#define FDV_MC_MAXB 16

/* Motion-compensated luma prediction for a whole block.
 *
 * The per-pixel form of this (call interp_sample bw*bh times) recomputes the
 * same 6-tap filter outputs over and over: neighbouring output pixels share
 * nearly all their taps, and the centre position `j` re-runs six horizontal
 * filters per pixel.  Doing it a block at a time makes the filter separable —
 * one horizontal pass into scratch, one vertical pass out of it — so each tap
 * is computed once and reused down the column.
 *
 * The arithmetic per output pixel is identical to interp_sample's, in the same
 * order, so results stay bit-exact.  That matters twice over: the decoder runs
 * this too, and encoder/decoder must not drift. */
static void fdv_mc_luma_inner(const fdv_plane *ref, int bx, int by, int bw, int bh,
             int mvx, int mvy, uint8_t *dst, int dst_stride) {
    const fdv_plane *r = ref;
    int fx = mvx & 3, fy = mvy & 3;
    int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
    int phase = fy * 4 + fx;

    /* Integer position: no filtering at all, just a strided copy. */
    if (phase == 0) {
        for (int i = 0; i < bh; ++i)
            memcpy(dst + (size_t)i * dst_stride, fdv_plane_at(r, ix, iy + i), (size_t)bw);
        return;
    }

    /* Blocks larger than a macroblock, or not a multiple of the SIMD width,
     * never occur; keep the general path as a correctness backstop rather than
     * a silent out-of-bounds write. */
    if (bw > FDV_MC_MAXB || bh > FDV_MC_MAXB || (bw & 7)) {
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; ++j)
                dst[i * dst_stride + j] =
                    (uint8_t)interp_sample(r, ix + j, iy + i, fx, fy);
        return;
    }

    /* Each quarter-pel phase is one half/integer position, or the rounded mean
     * of two.  Naming follows H.264 figure 8-4: b/h/j are the half-pel
     * positions, m is h one column right, s is b one row down. */
    enum { S_G = 0, S_HPIX, S_MPIX, S_B, S_S, S_H, S_M, S_J, S_NONE };
    static const uint8_t BLEND[16][2] = {
        {S_G, S_NONE}, {S_G, S_B   }, {S_B, S_NONE}, {S_B, S_HPIX},  /* G a b c */
        {S_G, S_H   }, {S_B, S_H   }, {S_B, S_J   }, {S_B, S_M   },  /* d e f g */
        {S_H, S_NONE}, {S_H, S_J   }, {S_J, S_NONE}, {S_J, S_M   },  /* h i j k */
        {S_H, S_MPIX}, {S_H, S_S   }, {S_J, S_S   }, {S_M, S_S   },  /* n p q r */
    };
    int s0 = BLEND[phase][0], s1 = BLEND[phase][1];

    /* Only build the intermediates this phase actually reads. */
    int want_b = (s0 == S_B || s1 == S_B), want_s = (s0 == S_S || s1 == S_S);
    int want_h = (s0 == S_H || s1 == S_H), want_m = (s0 == S_M || s1 == S_M);
    int want_j = (s0 == S_J || s1 == S_J);

    uint8_t Bbuf[FDV_MC_MAXB * (FDV_MC_MAXB + 1)];       /* b, rows 0..bh (s is row i+1) */
    uint8_t Jbuf[FDV_MC_MAXB * FDV_MC_MAXB];

    /* h needs bw+1 columns (m reads one further right), but it is filled eight
     * lanes at a time, so the row has to have room for a whole final vector:
     * at bw=16 the last store begins at column 16 and writes through 23.  A
     * bw+1 stride would put those seven bytes into the next row -- or, on the
     * last row, past the buffer entirely.  Round the stride up instead. */
    uint8_t Hbuf[FDV_MC_MAXB * (FDV_MC_MAXB + 8)];       /* h, cols 0..bw (m is col j+1) */
    int hstride = bw + 8;

    if (want_b || want_s) {
        int rows = bh + (want_s ? 1 : 0);        /* s reads one row further down */
#if defined(__ARM_NEON)
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < bw; j += 8)
                vst1_u8(&Bbuf[i * bw + j],
                        neon_half(neon_hf8(fdv_plane_at(r, ix + j, iy + i))));
#else
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < bw; ++j)
                Bbuf[i * bw + j] = (uint8_t)clipb((hf(r, ix + j, iy + i) + 16) >> 5);
#endif
    }
    if (want_h || want_m) {
        int cols = bw + (want_m ? 1 : 0);        /* m reads one column further right */
#if defined(__ARM_NEON)
        /* cols is 8, 9, 16 or 17; the last vector may overrun the used columns
         * but stays inside the row's hstride, and the source reads land in the
         * plane's border, so both are in bounds. */
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < cols; j += 8)
                vst1_u8(&Hbuf[i * hstride + j],
                        neon_half(neon_vf8(fdv_plane_at(r, ix + j, iy + i), r->stride)));
#else
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < cols; ++j)
                Hbuf[i * hstride + j] = (uint8_t)clipb((vf(r, ix + j, iy + i) + 16) >> 5);
#endif
    }
    if (want_j) {
        /* Separable: the unclipped horizontal 6-tap over bh+5 rows, then the
         * vertical 6-tap down each column of that.  The per-pixel form redid
         * the horizontal pass six times per output sample. */
#if defined(__ARM_NEON)
        /* The horizontal pass fits int16, so the scratch is half the width of
         * the scalar version's and the vertical pass loads it in one go. */
        int16_t hrow[(FDV_MC_MAXB + 5) * FDV_MC_MAXB];
        for (int i = 0; i < bh + 5; ++i)
            for (int j = 0; j < bw; j += 8)
                vst1q_s16(&hrow[i * bw + j], neon_hf8(fdv_plane_at(r, ix + j, iy + i - 2)));
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; j += 8) {
                const int16_t *c = &hrow[i * bw + j];
                vst1_u8(&Jbuf[i * bw + j],
                        neon_centre(vld1q_s16(c),             vld1q_s16(c + bw),
                                    vld1q_s16(c + 2 * bw),    vld1q_s16(c + 3 * bw),
                                    vld1q_s16(c + 4 * bw),    vld1q_s16(c + 5 * bw)));
            }
#else
        int hrow[(FDV_MC_MAXB + 5) * FDV_MC_MAXB];
        for (int i = 0; i < bh + 5; ++i)
            for (int j = 0; j < bw; ++j)
                hrow[i * bw + j] = hf(r, ix + j, iy + i - 2);
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; ++j) {
                const int *c = &hrow[i * bw + j];
                int jj = tap6(c[0], c[bw], c[2 * bw], c[3 * bw], c[4 * bw], c[5 * bw]);
                Jbuf[i * bw + j] = (uint8_t)clipb((jj + 512) >> 10);
            }
#endif
    }

    /* Resolve each blend operand to a base pointer and stride, then run one
     * loop rather than a switch per pixel. */
    const uint8_t *base[S_NONE];
    int            step[S_NONE];
    base[S_G]    = fdv_plane_at(r, ix,     iy);      step[S_G]    = r->stride;
    base[S_HPIX] = fdv_plane_at(r, ix + 1, iy);      step[S_HPIX] = r->stride;
    base[S_MPIX] = fdv_plane_at(r, ix,     iy + 1);  step[S_MPIX] = r->stride;
    base[S_B]    = Bbuf;                         step[S_B]    = bw;
    base[S_S]    = Bbuf + bw;                    step[S_S]    = bw;
    base[S_H]    = Hbuf;                         step[S_H]    = hstride;
    base[S_M]    = Hbuf + 1;                     step[S_M]    = hstride;
    base[S_J]    = Jbuf;                         step[S_J]    = bw;

    const uint8_t *p0 = base[s0];
    int st0 = step[s0];
    if (s1 == S_NONE) {
        for (int i = 0; i < bh; ++i)
            memcpy(dst + (size_t)i * dst_stride, p0 + (size_t)i * st0, (size_t)bw);
    } else {
        const uint8_t *p1 = base[s1];
        int st1 = step[s1];
#if defined(__ARM_NEON)
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; j += 8)
                vst1_u8(&dst[i * dst_stride + j],
                        vrhadd_u8(vld1_u8(&p0[i * st0 + j]), vld1_u8(&p1[i * st1 + j])));
#else
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; ++j)
                dst[i * dst_stride + j] =
                    (uint8_t)((p0[i * st0 + j] + p1[i * st1 + j] + 1) >> 1);
#endif
    }
}

/* Instrumented entry point; the work is in fdv_mc_luma_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_mc_luma(const fdv_plane *ref, int bx, int by, int bw, int bh,
             int mvx, int mvy, uint8_t *dst, int dst_stride) {
    FDV_ZB(FDV_Z_MCLUMA);
    fdv_mc_luma_inner(ref, bx, by, bw, bh, mvx, mvy, dst, dst_stride);
    FDV_ZE(FDV_Z_MCLUMA);
}

#if defined(__ARM_NEON)
/* One bilinear chroma row of 8 (or 4) pixels: top = a*(4-fx)+b*fx,
 * bot = c*(4-fx)+d*fx, out = (top*(4-fy)+bot*fy+8)>>4 — bit-identical to scalar.
 * The 4-pel case loads 8 (borders make it safe) and stores the low 4. */
static void mc_chroma_row8(const uint8_t *src, int stride, int fx, int fy,
                           uint8_t *dst, int store4) {
    uint16x8_t a = vmovl_u8(vld1_u8(src));
    uint16x8_t b = vmovl_u8(vld1_u8(src + 1));
    uint16x8_t c = vmovl_u8(vld1_u8(src + stride));
    uint16x8_t d = vmovl_u8(vld1_u8(src + stride + 1));
    uint16x8_t top = vmlaq_n_u16(vmulq_n_u16(a, (uint16_t)(4 - fx)), b, (uint16_t)fx);
    uint16x8_t bot = vmlaq_n_u16(vmulq_n_u16(c, (uint16_t)(4 - fx)), d, (uint16_t)fx);
    uint16x8_t r = vmlaq_n_u16(vmulq_n_u16(top, (uint16_t)(4 - fy)), bot, (uint16_t)fy);
    uint8x8_t out = vshrn_n_u16(vaddq_u16(r, vdupq_n_u16(8)), 4);
    if (store4) vst1_lane_u32((uint32_t *)dst, vreinterpret_u32_u8(out), 0);
    else        vst1_u8(dst, out);
}
#endif

static void fdv_mc_chroma_inner(const fdv_plane *ref, int bx, int by, int bw, int bh,
               int mvx, int mvy, uint8_t *dst, int dst_stride) {
    int fx = mvx & 3, fy = mvy & 3;
    int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
#if defined(__ARM_NEON)
    if (bw == 8 || bw == 4) {
        for (int i = 0; i < bh; ++i)
            mc_chroma_row8(fdv_plane_at(ref, ix, iy + i), ref->stride, fx, fy,
                           dst + i * dst_stride, bw == 4);
        return;
    }
#endif
    for (int i = 0; i < bh; ++i) {
        const uint8_t *src = fdv_plane_at(ref, ix, iy + i);
        for (int j = 0; j < bw; ++j) {
            const uint8_t *p = src + j;
            int a = p[0], b = p[1];
            int c = p[ref->stride], d = p[ref->stride + 1];
            int top = a * (4 - fx) + b * fx;
            int bot = c * (4 - fx) + d * fx;
            dst[i * dst_stride + j] = (uint8_t)((top * (4 - fy) + bot * fy + 8) >> 4);
        }
    }
}

/* Instrumented entry point; the work is in fdv_mc_chroma_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_mc_chroma(const fdv_plane *ref, int bx, int by, int bw, int bh,
               int mvx, int mvy, uint8_t *dst, int dst_stride) {
    FDV_ZB(FDV_Z_MCCHROMA);
    fdv_mc_chroma_inner(ref, bx, by, bw, bh, mvx, mvy, dst, dst_stride);
    FDV_ZE(FDV_Z_MCCHROMA);
}

int fdv_sad_kernel(const uint8_t *cur, int cur_stride, const uint8_t *pred,
               int bw, int bh) {
#if defined(__ARM_NEON)
    if (bw == 16) {
        uint16x8_t acc = vdupq_n_u16(0);
        for (int i = 0; i < bh; ++i)
            acc = vpadalq_u8(acc, vabdq_u8(vld1q_u8(cur + (size_t)i * cur_stride),
                                          vld1q_u8(pred + i * 16)));
        return (int)vaddvq_u16(acc);
    }
    if (bw == 8) {
        uint16x8_t acc = vdupq_n_u16(0);
        for (int i = 0; i < bh; ++i)
            acc = vaddq_u16(acc, vmovl_u8(vabd_u8(vld1_u8(cur + (size_t)i * cur_stride),
                                                  vld1_u8(pred + i * 8))));
        return (int)vaddvq_u16(acc);
    }
#endif
    int sad = 0;
    for (int i = 0; i < bh; ++i)
        for (int j = 0; j < bw; ++j) {
            int d = cur[(size_t)i * cur_stride + j] - pred[i * bw + j];
            sad += d < 0 ? -d : d;
        }
    return sad;
}

static int block_sad(const uint8_t *cur, int cur_stride, const fdv_plane *ref,
                     int bx, int by, int bw, int bh, int mvx, int mvy) {
    uint8_t pred[16 * 16];
    fdv_mc_luma(ref, bx, by, bw, bh, mvx, mvy, pred, bw);
    return fdv_sad_kernel(cur + (size_t)by * cur_stride + bx, cur_stride, pred, bw, bh);
}

/* Sub-pel probes per refinement stage: 4 (cross) or 8 (full neighbourhood).
 * Lives here rather than beside FDV_ME_RANGE in the VIDEO section, because INTER
 * may not depend on a section below it. */
#ifndef FDV_ME_SUBPEL_PTS
#define FDV_ME_SUBPEL_PTS 4
#endif

/* What a structure symbol and a residual region flag cost, in bits, to the
 * rate-distortion decisions. Both are heavily skewed streams -- a mode is one
 * of nine with DC taking most of the mass, a region flag one of three -- so
 * their coded cost sits well under the byte they occupy. Coefficients are
 * charged by magnitude instead; see fdv_bits_val. */
#ifndef FDV_BITS_MODE
#define FDV_BITS_MODE 3
#endif
#ifndef FDV_BITS_FLAG
#define FDV_BITS_FLAG 2
#endif

/* The Lagrangian constant for J = D + lambda*R.
 *
 * The textbook H.264 figure is 0.85, and it assumes R is measured in *bits*.
 * R here is an estimate of bits rather than a count of them -- exp-Golomb
 * lengths for the coefficients, small constants for the structure symbols --
 * and it lands about half of what the entropy coder actually spends, because
 * both stand-ins are pessimistic about the skew the real models exploit.
 * Halving 0.85 to match is what a sweep over the scene library picks: 0.425 is
 * the best of 0.2125, 0.30, 0.425 and 0.6375 on all-intra BD-rate, and the
 * curve is flat enough either side that the exact value is not delicate.
 *
 * The previous constant was 0.2125, against a rate model that charged eight
 * bits for every emitted byte. That model was roughly four times what the coder
 * paid; this one is roughly two, and the constant tracks it. */
#ifndef FDV_LAMBDA0
#define FDV_LAMBDA0 0.425      /* 0.85 / 2 */
#endif

static int fdv_me_search_inner(const uint8_t *cur, int cur_stride, const fdv_plane *ref,
              int bx, int by, int bw, int bh, int range,
              int pmx, int pmy, int *mvx, int *mvy) {
    /* Diamond search on the integer grid (quarter-pel units, so a 1-pel step is
     * 4). Large diamond until the center is best, then one small diamond. This
     * is O(iterations) rather than O(range^2) — it scales to real resolutions at
     * the cost of being a local (not exhaustive) search. */
    static const int LDSP[8][2] = {
        {0, 8}, {0, -8}, {8, 0}, {-8, 0}, {4, 4}, {4, -4}, {-4, 4}, {-4, -4}
    };
    static const int SDSP[4][2] = {{0, 4}, {0, -4}, {4, 0}, {-4, 0}};
    /* Sub-pel probe pattern: cross by default, full eight-neighbourhood when
     * FDV_ME_SUBPEL_PTS is 8. */
    static const int SUBPEL[8][2] = {
        {0, -1}, {0, 1}, {-1, 0}, {1, 0},
        {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
    };
    int lim = range * 4;

    /* Charging the search for the vector it picks: tried, measured, rejected.
     *
     * Scoring SAD alone, with no term for what the vector costs to transmit, is
     * the classic motion-estimation mistake, and the profile said it should
     * matter here -- motion vectors are 24-37% of a P-frame at QP 34, which is
     * the operating point a 1 Mbps stream actually runs at. Adding
     * `lambda_me * bits(mv - pred)`, swept over half to four times the standard
     * sqrt(lambda), moved coded size between -2.3% and +1.6% and averaged zero.
     *
     * The reason is the EPZS seed just below: the search already starts at the
     * predicted vector and the diamond is local, so it never wanders far enough
     * from the predictor for a rate term to have anything to correct. That MV
     * cost is intrinsic -- one vector per macroblock on content that genuinely
     * moves -- not the product of a rate-blind search. Reducing it needs fewer
     * vectors, not better-chosen ones. */
    int best_x = 0, best_y = 0;
    int best = block_sad(cur, cur_stride, ref, bx, by, bw, bh, 0, 0);

    /* EPZS-style seed: also try the predicted MV (rounded to integer pel,
     * clamped), and start the diamond from whichever point is better. */
    {
        int px = (pmx >> 2) * 4, py = (pmy >> 2) * 4;
        if (px < -lim) px = -lim; else if (px > lim) px = lim;
        if (py < -lim) py = -lim; else if (py > lim) py = lim;
        if (px != 0 || py != 0) {
            int s = block_sad(cur, cur_stride, ref, bx, by, bw, bh, px, py);
            if (s < best) { best = s; best_x = px; best_y = py; }
        }
    }

    for (int iter = 0; iter < 64; ++iter) {
        int cx = best_x, cy = best_y, moved = 0;
        for (int k = 0; k < 8; ++k) {
            int nx = cx + LDSP[k][0], ny = cy + LDSP[k][1];
            if (nx < -lim || nx > lim || ny < -lim || ny > lim) continue;
            int s = block_sad(cur, cur_stride, ref, bx, by, bw, bh, nx, ny);
            if (s < best) { best = s; best_x = nx; best_y = ny; moved = 1; }
        }
        if (!moved) break;
    }

    {
        int cx = best_x, cy = best_y;
        for (int k = 0; k < 4; ++k) {
            int nx = cx + SDSP[k][0], ny = cy + SDSP[k][1];
            if (nx < -lim || nx > lim || ny < -lim || ny > lim) continue;
            int s = block_sad(cur, cur_stride, ref, bx, by, bw, bh, nx, ny);
            if (s < best) { best = s; best_x = nx; best_y = ny; }
        }
    }

    /* Sub-pel refinement: half- then quarter-pel.
     *
     * Each probe is a full motion compensation plus a SAD, and this loop is
     * where most of the encoder's motion-compensation calls come from. The
     * eight-neighbour form costs sixteen probes per search; the four-point
     * cross costs eight and, because the diagonal is reachable by taking one
     * step horizontally and then one vertically at the next stage, gives up
     * very little. A perfect match ends the search outright. */
    for (int step = 2; step >= 1 && best > 0; step >>= 1) {
        int cx = best_x, cy = best_y;
        for (int k = 0; k < FDV_ME_SUBPEL_PTS; ++k) {
            int dx = SUBPEL[k][0], dy = SUBPEL[k][1];
            int s = block_sad(cur, cur_stride, ref, bx, by, bw, bh,
                              cx + dx * step, cy + dy * step);
            if (s < best) { best = s; best_x = cx + dx * step; best_y = cy + dy * step; }
        }
    }

    *mvx = best_x;
    *mvy = best_y;
    return best;
}

/* Instrumented entry point; the work is in fdv_me_search_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
int fdv_me_search(const uint8_t *cur, int cur_stride, const fdv_plane *ref,
              int bx, int by, int bw, int bh, int range,
              int pmx, int pmy, int *mvx, int *mvy) {
    FDV_ZB(FDV_Z_ME);
    int r_ = fdv_me_search_inner(cur, cur_stride, ref, bx, by, bw, bh, range, pmx, pmy, mvx, mvy);
    FDV_ZE(FDV_Z_ME);
    return r_;
}

/* ===========================================================================
 * 9. DEBLOCK
 * In-loop deblocking filter over the 8x8 grid.
 * ======================================================================== */


static int     iclip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Filter the two samples either side of one block edge. `a` points at q0 (the
 * first sample on the high side); the p-side samples are at negative `step`
 * offsets. Mirrors the H.264 normal-strength luma filter, with thresholds
 * derived from QP rather than the full alpha/beta tables. */
static void filter_edge(uint8_t *a, int step, int alpha, int beta, int tc) {
    int p1 = a[-2 * step], p0 = a[-1 * step];
    int q0 = a[0],         q1 = a[ 1 * step];

    if (abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta) {
        int delta = ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3;
        delta = iclip(delta, -tc, tc);
        a[-1 * step] = clip255(p0 + delta);
        a[ 0]        = clip255(q0 - delta);
    }
}

#if defined(__ARM_NEON)
/* Sixteen block edges at once.  The filter is conditional per edge, so the
 * gate becomes a mask and the result is selected with vbslq_u8 rather than
 * branched on -- which is the whole reason this vectorises: the scalar version
 * spends its time on a three-term test it usually passes.
 *
 * Arithmetic matches filter_edge exactly. delta stays inside int16:
 * (q0-p0)*4 is [-1020,1020] and (p1-q1) is [-255,255]. */
static inline void deblock16_neon(uint8x16_t p1, uint8x16_t p0,
                                  uint8x16_t q0, uint8x16_t q1,
                                  uint8x16_t alpha, uint8x16_t beta,
                                  int16x8_t tcv, int16x8_t ntcv,
                                  uint8x16_t *p0_out, uint8x16_t *q0_out) {
    uint8x16_t keep = vandq_u8(vcltq_u8(vabdq_u8(p0, q0), alpha),
                     vandq_u8(vcltq_u8(vabdq_u8(p1, p0), beta),
                              vcltq_u8(vabdq_u8(q1, q0), beta)));

    int16x8_t p1l = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(p1)));
    int16x8_t p1h = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(p1)));
    int16x8_t p0l = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(p0)));
    int16x8_t p0h = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(p0)));
    int16x8_t q0l = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(q0)));
    int16x8_t q0h = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(q0)));
    int16x8_t q1l = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(q1)));
    int16x8_t q1h = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(q1)));

    /* ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3 — the +4 and >>3 together are a
     * rounding shift right. */
    int16x8_t dl = vaddq_s16(vshlq_n_s16(vsubq_s16(q0l, p0l), 2), vsubq_s16(p1l, q1l));
    int16x8_t dh = vaddq_s16(vshlq_n_s16(vsubq_s16(q0h, p0h), 2), vsubq_s16(p1h, q1h));
    dl = vrshrq_n_s16(dl, 3);
    dh = vrshrq_n_s16(dh, 3);
    dl = vminq_s16(vmaxq_s16(dl, ntcv), tcv);
    dh = vminq_s16(vmaxq_s16(dh, ntcv), tcv);

    uint8x16_t np0 = vcombine_u8(vqmovun_s16(vaddq_s16(p0l, dl)),
                                 vqmovun_s16(vaddq_s16(p0h, dh)));
    uint8x16_t nq0 = vcombine_u8(vqmovun_s16(vsubq_s16(q0l, dl)),
                                 vqmovun_s16(vsubq_s16(q0h, dh)));
    *p0_out = vbslq_u8(keep, np0, p0);
    *q0_out = vbslq_u8(keep, nq0, q0);
}
#endif /* __ARM_NEON */

static void fdv_deblock_plane_inner(uint8_t *plane, int w, int h, int stride, int qp) {
    /* Thresholds grow with QP: at low QP almost nothing is touched (genuine
     * detail preserved); at high QP the softening is stronger to match the
     * larger quantization steps. */
    int alpha = 6 + qp;
    int beta  = 2 + qp / 4;
    int tc    = 1 + qp / 10;

#if defined(__ARM_NEON)
    uint8x16_t valpha = vdupq_n_u8((uint8_t)alpha);
    uint8x16_t vbeta  = vdupq_n_u8((uint8_t)beta);
    int16x8_t  vtc    = vdupq_n_s16((int16_t)tc);
    int16x8_t  vntc   = vdupq_n_s16((int16_t)-tc);

    /* Vertical edges (filter horizontally across x boundaries). The four
     * samples of an edge are adjacent in memory, so vld4q_u8 at x-2 hands back
     * p1, p0, q0, q1 for sixteen edges spaced four columns apart. The grid is
     * eight, so two such loads are taken and their even lanes interleaved:
     * vuzp1q_u8 of the two gives the sixteen edges eight columns apart. The
     * inverse (vzip1q/vzip2q) puts the filtered samples back where they came
     * from, leaving the odd edges' bytes exactly as they were read. */
    int nedge = w / 8 - 1;                       /* edges at x = 8, 16, ... */
    int vec_edges = nedge >= 0 ? (nedge / 16) * 16 : 0;
    for (int y = 0; y < h; ++y) {
        uint8_t *row = &plane[y * stride];
        for (int k = 0; k < vec_edges; k += 16) {
            uint8_t *base = row + 8 + 8 * k - 2;
            uint8x16x4_t a = vld4q_u8(base);          /* edges k+0, +2, +4 ... */
            uint8x16x4_t b = vld4q_u8(base + 64);     /* the next sixteen      */
            uint8x16_t p1 = vuzp1q_u8(a.val[0], b.val[0]);
            uint8x16_t p0 = vuzp1q_u8(a.val[1], b.val[1]);
            uint8x16_t q0 = vuzp1q_u8(a.val[2], b.val[2]);
            uint8x16_t q1 = vuzp1q_u8(a.val[3], b.val[3]);
            /* The odd lanes belong to the edges on the 4x4 grid that this
             * filter no longer touches; they are carried through unchanged. */
            uint8x16_t odd0 = vuzp2q_u8(a.val[1], b.val[1]);
            uint8x16_t odd1 = vuzp2q_u8(a.val[2], b.val[2]);
            uint8x16_t np0, nq0;
            deblock16_neon(p1, p0, q0, q1, valpha, vbeta, vtc, vntc, &np0, &nq0);
            a.val[1] = vzip1q_u8(np0, odd0);
            b.val[1] = vzip2q_u8(np0, odd0);
            a.val[2] = vzip1q_u8(nq0, odd1);
            b.val[2] = vzip2q_u8(nq0, odd1);
            vst4q_u8(base, a);
            vst4q_u8(base + 64, b);
        }
        for (int x = 8 + 8 * vec_edges; x < w; x += 8)
            filter_edge(&row[x], 1, alpha, beta, tc);
    }

    /* Horizontal edges (filter vertically across y boundaries).  Here the four
     * samples are four rows apart and sixteen edges lie side by side, so plain
     * contiguous loads do it. */
    int wv = w & ~15;
    for (int y = 8; y < h; y += 8) {
        uint8_t *r0 = &plane[y * stride];
        for (int x = 0; x < wv; x += 16) {
            uint8x16_t p1 = vld1q_u8(r0 - 2 * stride + x);
            uint8x16_t p0 = vld1q_u8(r0 -     stride + x);
            uint8x16_t q0 = vld1q_u8(r0              + x);
            uint8x16_t q1 = vld1q_u8(r0 +     stride + x);
            uint8x16_t np0, nq0;
            deblock16_neon(p1, p0, q0, q1, valpha, vbeta, vtc, vntc, &np0, &nq0);
            vst1q_u8(r0 - stride + x, np0);
            vst1q_u8(r0          + x, nq0);
        }
        for (int x = wv; x < w; ++x)
            filter_edge(&r0[x], stride, alpha, beta, tc);
    }
#else
    /* Vertical edges first (filter horizontally across x boundaries). */
    for (int y = 0; y < h; ++y)
        for (int x = 8; x < w; x += 8)
            filter_edge(&plane[y * stride + x], 1, alpha, beta, tc);

    /* Then horizontal edges (filter vertically across y boundaries). */
    for (int y = 8; y < h; y += 8)
        for (int x = 0; x < w; ++x)
            filter_edge(&plane[y * stride + x], stride, alpha, beta, tc);
#endif
}

/* Instrumented entry point; the work is in fdv_deblock_plane_inner. FDV_ZB/FDV_ZE
 * compile to nothing without -DFDV_PROFILE. */
void fdv_deblock_plane(uint8_t *plane, int w, int h, int stride, int qp) {
    FDV_ZB(FDV_Z_DEBLOCK);
    fdv_deblock_plane_inner(plane, w, h, stride, qp);
    FDV_ZE(FDV_Z_DEBLOCK);
}

/* ===========================================================================
 * 10. IMAGE
 * Intra-only single-plane image codec.
 * ======================================================================== */



/* The largest intra block: a coding tree unit, split by rate-distortion down
 * to 4x4. Three levels at 32, which is what the quadtree walks. */
#ifndef FDV_CTU
#define FDV_CTU 32
#endif

/* Z-scan index of a 4x4 cell within its coding tree unit.
 *
 * The quadtree visits a CTU's cells in this order, so one cell is reconstructed
 * before another exactly when its index is lower. One level per split, the
 * vertical bit more significant than the horizontal at each, which is what
 * makes the visit order (0,0) (1,0) (0,1) (1,1) recursively. */
static int zidx4(int lx, int ly) {
    int z = 0;
    for (int b = FDV_CTU / 8; b; b >>= 1)      /* cells per side, halving */
        z = (z << 2) | ((ly & b) ? 2 : 0) | ((lx & b) ? 1 : 0);
    return z;
}

/* Whether the 4x4 cell containing (x,y) is already reconstructed while the
 * block at (bx,by) is being coded.
 *
 * Under the old fixed 4x4 raster walk this was free: the whole row above was
 * always finished, so top-right existed whenever the plane was wide enough.
 * The quadtree walks 16x16 CTUs in raster order and their cells in z-order, and
 * that is no longer true -- the cell above-right of a CTU's lower-left quadrant
 * belongs to the quadrant coded *after* it. Reading it would pull in
 * uninitialised samples in the encoder and stale ones in the decoder, which is
 * a mismatch rather than a mere inefficiency, so availability is derived
 * exactly. Both sides run this identical function. */
static int cell_done(int x, int y, int bx, int by) {
    int cx0 = bx & ~(FDV_CTU - 1), cy0 = by & ~(FDV_CTU - 1);
    if (y < cy0)             return 1;  /* a CTU row that has finished      */
    if (x < cx0)             return 1;  /* a CTU to the left, same row      */
    if (x >= cx0 + FDV_CTU)  return 0;  /* a CTU to the right: not yet      */
    return zidx4((x - cx0) >> 2, (y - cy0) >> 2) <
           zidx4((bx - cx0) >> 2, (by - cy0) >> 2);
}

/* Gather the reconstructed neighbors of the 4x4 block at (bx,by) from a plane
 * `buf` with the given stride: top[0..3] + top-right top[4..7], left[0..3],
 * the corner, and availability flags. Missing top-right replicates top[3]. */
static void gather_neighbors(const uint8_t *buf, int w, int stride, int bx, int by,
                             uint8_t top[8], uint8_t left[4], uint8_t *topleft,
                             int *ht, int *htr, int *hl, int *htl) {
    int have_top = by > 0, have_left = bx > 0;
    *ht = have_top; *hl = have_left;
    *htl = have_top && have_left;
    *htr = have_top && (bx + 4 < w) && cell_done(bx + 4, by - 1, bx, by);
    for (int j = 0; j < 8; ++j) top[j] = 0;
    for (int i = 0; i < 4; ++i) left[i] = 0;
    *topleft = 128;
    if (have_top) {
        for (int j = 0; j < 4; ++j) top[j] = buf[(by - 1) * stride + bx + j];
        if (*htr) for (int j = 4; j < 8; ++j) top[j] = buf[(by - 1) * stride + bx + j];
        else      for (int j = 4; j < 8; ++j) top[j] = top[3];
    }
    if (have_left) for (int i = 0; i < 4; ++i) left[i] = buf[(by + i) * stride + bx - 1];
    if (*htl) *topleft = buf[(by - 1) * stride + bx - 1];
}

/* Whether mode m's required neighbors are available. */
static int mode_ok(int m, int ht, int htr, int hl, int htl) {
    switch (m) {
    case FDV_INTRA_DC:    return 1;
    case FDV_INTRA_VERT:  return ht;
    case FDV_INTRA_HORIZ: return hl;
    case FDV_INTRA_HU:    return hl;
    case FDV_INTRA_DDL:   return ht && htr;
    case FDV_INTRA_VL:    return ht && htr;
    case FDV_INTRA_DDR:   case FDV_INTRA_VR: case FDV_INTRA_HD: return ht && hl && htl;
    }
    return 0;
}

#if defined(__ARM_NEON)
/* Two 4-pixel rows of a strided source, packed into one 8-lane vector so a 4x4
 * block is handled in two steps instead of sixteen. Exactly four bytes are read
 * per row -- a wider load would run past the block at a plane's right edge. */
static inline uint8x8_t ld_2rows(const uint8_t *s, int stride) {
    uint32_t a, b;
    memcpy(&a, s, 4);
    memcpy(&b, s + stride, 4);
    uint32x2_t v = vset_lane_u32(b, vdup_n_u32(a), 1);
    return vreinterpret_u8_u32(v);
}

/* The inverse: write two 4-pixel rows back to a strided destination. */
static inline void st_2rows(uint8_t *d, int stride, uint8x8_t v) {
    uint32x2_t u = vreinterpret_u32_u8(v);
    uint32_t a = vget_lane_u32(u, 0), b = vget_lane_u32(u, 1);
    memcpy(d, &a, 4);
    memcpy(d + stride, &b, 4);
}
#endif

/* Sum of absolute Hadamard-transformed differences.
 *
 * The screen below needs to rank predictions by roughly what they will cost to
 * code, and plain SAD does not: on textured content a prediction with a small
 * pixel-wise error can leave a high-frequency residual that quantizes badly,
 * while a worse-looking one leaves something the transform flattens to almost
 * nothing. Measured on a checkerboard scene, ranking by SAD cost 14.8 dB and
 * quadrupled the intra frame. A Hadamard butterfly is multiply-free and sees
 * the frequency content, which is what makes it a usable proxy. */
#if defined(__ARM_NEON)
/* Load four bytes as signed 16-bit differences. Exactly four -- a wider load
 * would read past the block, which at a plane's right edge is out of bounds. */
static inline int16x4_t satd_row(const uint8_t *s, const uint8_t *p) {
    uint32_t a, b;
    memcpy(&a, s, 4);
    memcpy(&b, p, 4);
    uint16x8_t d = vsubl_u8(vreinterpret_u8_u32(vdup_n_u32(a)),
                            vreinterpret_u8_u32(vdup_n_u32(b)));
    return vget_low_s16(vreinterpretq_s16_u16(d));
}

/* The Hadamard is separable and symmetric, so transforming columns before rows
 * gives the same matrix H*D*H^T the scalar version builds row-first -- and the
 * same sum of absolute values. Doing columns first means the butterflies run
 * across whole vectors, and only one transpose is needed in between.
 * Range: differences are at most 255 and each pass scales by 4, so 4080 is the
 * peak -- comfortably inside int16. */
static int fdv_satd4x4_neon(const uint8_t *src, int sstride, const uint8_t *pred) {
    int16x4_t r0 = satd_row(src + 0 * sstride, pred + 0);
    int16x4_t r1 = satd_row(src + 1 * sstride, pred + 4);
    int16x4_t r2 = satd_row(src + 2 * sstride, pred + 8);
    int16x4_t r3 = satd_row(src + 3 * sstride, pred + 12);

    int16x4_t a0 = vadd_s16(r0, r1), a1 = vsub_s16(r0, r1);
    int16x4_t a2 = vadd_s16(r2, r3), a3 = vsub_s16(r2, r3);
    int16x4_t b0 = vadd_s16(a0, a2), b1 = vadd_s16(a1, a3);
    int16x4_t b2 = vsub_s16(a0, a2), b3 = vsub_s16(a1, a3);

    int16x4x2_t t01 = vtrn_s16(b0, b1);
    int16x4x2_t t23 = vtrn_s16(b2, b3);
    int32x2x2_t u02 = vtrn_s32(vreinterpret_s32_s16(t01.val[0]),
                               vreinterpret_s32_s16(t23.val[0]));
    int32x2x2_t u13 = vtrn_s32(vreinterpret_s32_s16(t01.val[1]),
                               vreinterpret_s32_s16(t23.val[1]));
    int16x4_t c0 = vreinterpret_s16_s32(u02.val[0]);
    int16x4_t c1 = vreinterpret_s16_s32(u13.val[0]);
    int16x4_t c2 = vreinterpret_s16_s32(u02.val[1]);
    int16x4_t c3 = vreinterpret_s16_s32(u13.val[1]);

    int16x4_t e0 = vadd_s16(c0, c1), e1 = vsub_s16(c0, c1);
    int16x4_t e2 = vadd_s16(c2, c3), e3 = vsub_s16(c2, c3);
    int16x4_t f0 = vadd_s16(e0, e2), f1 = vadd_s16(e1, e3);
    int16x4_t f2 = vsub_s16(e0, e2), f3 = vsub_s16(e1, e3);

    int16x4_t sum = vadd_s16(vadd_s16(vabs_s16(f0), vabs_s16(f1)),
                             vadd_s16(vabs_s16(f2), vabs_s16(f3)));
    return (int)vaddlv_s16(sum);
}
#endif

int fdv_satd4x4_scalar(const uint8_t *src, int sstride, const uint8_t *pred) {
    int d[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            d[i * 4 + j] = src[i * sstride + j] - pred[i * 4 + j];

    for (int i = 0; i < 4; ++i) {                 /* rows */
        int a0 = d[i * 4 + 0] + d[i * 4 + 1], a1 = d[i * 4 + 0] - d[i * 4 + 1];
        int a2 = d[i * 4 + 2] + d[i * 4 + 3], a3 = d[i * 4 + 2] - d[i * 4 + 3];
        d[i * 4 + 0] = a0 + a2; d[i * 4 + 1] = a1 + a3;
        d[i * 4 + 2] = a0 - a2; d[i * 4 + 3] = a1 - a3;
    }
    int sum = 0;
    for (int j = 0; j < 4; ++j) {                 /* columns */
        int a0 = d[0 * 4 + j] + d[1 * 4 + j], a1 = d[0 * 4 + j] - d[1 * 4 + j];
        int a2 = d[2 * 4 + j] + d[3 * 4 + j], a3 = d[2 * 4 + j] - d[3 * 4 + j];
        int b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
        sum += abs(b0) + abs(b1) + abs(b2) + abs(b3);
    }
    return sum;
}

int fdv_satd4x4(const uint8_t *src, int sstride, const uint8_t *pred) {
#if defined(__ARM_NEON)
    return fdv_satd4x4_neon(src, sstride, pred);
#else
    return fdv_satd4x4_scalar(src, sstride, pred);
#endif
}

/* How many intra modes get the full rate-distortion treatment after the SATD
 * screen, and how far behind the leader a candidate may be before it is
 * dropped. Nine with no pruning is exhaustive; the defaults below cost about
 * a third of a dB on the hardest intra content (a fine checkerboard) and are
 * neutral or better elsewhere -- coded size came out equal or smaller than
 * exhaustive search on every scene measured. Raise FDV_INTRA_RD_CANDIDATES to 9
 * and FDV_INTRA_PRUNE_NUM very high to get the exhaustive behaviour back. */
#ifndef FDV_INTRA_RD_CANDIDATES
#define FDV_INTRA_RD_CANDIDATES 2
#endif

/* Below this QP the screen ranks but does not prune (see the comment at the
 * use site). */
#ifndef FDV_INTRA_SCREEN_MIN_QP
#define FDV_INTRA_SCREEN_MIN_QP 12
#endif

/* A candidate is dropped when its SATD exceeds NUM/DEN times the leader's. */
#ifndef FDV_INTRA_PRUNE_NUM
#define FDV_INTRA_PRUNE_NUM 5
#endif
#ifndef FDV_INTRA_PRUNE_DEN
#define FDV_INTRA_PRUNE_DEN 4
#endif

/* Charging the mode symbol in the mode decision: tried, measured, rejected.
 *
 * The mode stream is 62-90% of a cheap key frame, and it *grows* with QP
 * (`tiny`: 59 B at QP 16, 646 B at QP 24). The obvious reading is that at high
 * QP the residual quantizes to nothing, every candidate ties on rate, and the
 * winner is settled by distortion noise -- so neighbouring blocks pick
 * unrelated modes and the field stops being compressible. Adding the mode's own
 * cost to J should then break those ties toward the predictable answer.
 *
 * It does not. Charging non-predicted modes 1.5 to 6 bits against the
 * neighbours' prediction made the mode stream 8-110% *larger* -- the same trap
 * as the most-probable-mode experiment below: the pooled distribution is
 * heavily skewed toward DC, and steering blocks toward a position-varying
 * predictor spreads that mass. Charging against DC instead changed nothing at
 * all (646 B -> 647 B at any penalty), which is the real answer: the decision
 * was never arbitrary, because the residual term already prefers the cheap mode
 * wherever it is genuinely free.
 *
 * So 646 B over 32640 blocks is 0.16 bits a block -- the stream is already near
 * its floor, and what costs is the *number* of symbols, not their coding. The
 * fix had to be structural: fewer mode symbols per frame. That is what the
 * quadtree below does, and it is where the intra path's gain came from.
 *
 * Retried later as a bounded tie-break rather than a penalty -- nothing charged
 * to the losing modes, the predicted mode simply allowed to win when it came
 * within N bits of the leader, and predicted as intra_mode_ctx models it
 * (the agreed neighbour mode where left and above agree, DC otherwise) rather
 * than as H.264's min(left, above). Measured with tools/rd-bench.py over the
 * scene library: +3.8% BD-rate at the default two-candidate screen, +3.4% with
 * all nine modes evaluated. Worse at every slack from 1 to 32 bits. The earlier
 * conclusion holds, and the reason it holds is that the mode field's apparent
 * noise is not reachable from the decision: 8x8 regions of uniform mode do fall
 * from 96% at QP 16 to 73% at QP 22 on `pan`, but steering the choice back
 * toward the neighbour costs more distortion than the agreement saves.
 *
 * The same harness settled a second question. Evaluating all nine modes instead
 * of the two the SATD screen keeps is +27.7% BD-rate -- the screen is not a
 * compromise that costs coding efficiency, it is better than exhaustive search
 * here, which is worth knowing before anyone tries to "improve" it. */



/* One 4x4 intra leaf: pick a mode from the nine H.264 directions, reconstruct
 * it into `rec`, report the mode and append its coefficients.
 *
 * Returns the leaf's rate-distortion cost. That return value is what the
 * quadtree above weighs against coding the same area as one larger block, so it
 * has to be the cost of what was actually written: distortion measured on the
 * final reconstruction, rate on the levels really emitted. */
static double intra_leaf4(const uint8_t *src, int stride, uint8_t *rec, int w,
                        int bx, int by, int qp, double lambda,
                        int *mode_out, fdv_cw *cw) {
    uint8_t top[8], left[4], topleft;
    int ht, htr, hl, htl;
    gather_neighbors(rec, w, w, bx, by, top, left, &topleft, &ht, &htr, &hl, &htl);

    /* RD mode decision: minimize reconstructed SSD + lambda * coded bits.
     * Each candidate is fully transformed/quantized/reconstructed so the
     * decision sees true distortion and a rate estimate that grows with
     * coefficient magnitude (fdv_bits_val) -- the extra modes are used only
     * when they actually lower the cost. */
    double best_cost = -1.0;
    int     best_mode = FDV_INTRA_DC;
    int16_t best_level[16];
    uint8_t best_rec[16];

    /* Screening. Running the full chain -- transform, quantize, RDOQ,
     * reconstruct, estimate rate -- on all nine modes costs nine times
     * what one costs, and the modes that lose usually lose badly: a
     * directional predictor pointed the wrong way leaves an obviously
     * larger residual. So predict all nine (cheap), rank them by the
     * Hadamard-transformed residual (fdv_satd4x4, which sees frequency
     * content the way the real transform will), and spend the full
     * decision only on the most promising FDV_INTRA_RD_CANDIDATES.
     *
     * Unlike the SKIP early-out in the P-frame path, this is a
     * heuristic: SATD ranks residuals well but not perfectly, so the
     * mode chosen is occasionally not the one full RD would pick. See
     * README for the measured cost. */
    uint8_t cand_pred[FDV_INTRA_NMODES][16];
    int     cand_sad[FDV_INTRA_NMODES], cand_mode[FDV_INTRA_NMODES], ncand = 0;
    for (int m = 0; m < FDV_INTRA_NMODES; ++m) {
        if (!mode_ok(m, ht, htr, hl, htl)) continue;
        uint8_t *pp = cand_pred[ncand];
        fdv_intra_predict_4x4(m, top, left, topleft, ht, hl, pp);
        cand_sad[ncand] = fdv_satd4x4(&src[(by) * stride + bx], stride, pp);
        cand_mode[ncand] = m;
        ++ncand;
    }
    /* At very low QP the reconstruction is near-lossless, so the modes
     * are nearly tied on distortion and the choice turns almost purely
     * on rate -- which SATD tracks well enough to rank but not well
     * enough to pick a winner. Measured: at QP 0 screening down to six
     * candidates already costs 4.6 dB, while at QP 12 and above every
     * candidate count lands within noise. So below the threshold the
     * screen only orders the modes and all of them are still tried;
     * that is also the case where bits are plentiful and encode time
     * is the least scarce thing. */
    int ntry = (qp < FDV_INTRA_SCREEN_MIN_QP) ? ncand
             : (ncand < FDV_INTRA_RD_CANDIDATES ? ncand : FDV_INTRA_RD_CANDIDATES);
    for (int a = 0; a < ntry; ++a) {
        int bst = a;
        for (int b2 = a + 1; b2 < ncand; ++b2)
            if (cand_sad[b2] < cand_sad[bst]) bst = b2;
        if (bst != a) {
            int ts = cand_sad[a]; cand_sad[a] = cand_sad[bst]; cand_sad[bst] = ts;
            int tm = cand_mode[a]; cand_mode[a] = cand_mode[bst]; cand_mode[bst] = tm;
            uint8_t tp[16];
            memcpy(tp, cand_pred[a], 16);
            memcpy(cand_pred[a], cand_pred[bst], 16);
            memcpy(cand_pred[bst], tp, 16);
        }
    }

    /* Two passes. RDOQ is the expensive part of coding a block -- it
     * reconstructs once per candidate truncation -- and it is only ever
     * an improvement, applied to whichever mode wins. So compare the
     * candidates without it and run it once, on the winner. The mode
     * being chosen still sees true distortion and a true rate, just
     * before that final refinement. */
    int best_c = 0;
    for (int c = 0; c < ntry; ++c) {
        /* The candidates are ordered by SATD, so once one is far enough
         * behind the leader the rest are further still. Where a single
         * mode clearly fits the block -- flat areas, clean edges -- this
         * spends one full evaluation instead of FDV_INTRA_RD_CANDIDATES;
         * where several are genuinely close it still tries them all.
         * Pruning on the ratio rather than a fixed count is what keeps
         * textured blocks from being shortchanged. */
        if (c > 0 && qp >= FDV_INTRA_SCREEN_MIN_QP &&
            (int64_t)cand_sad[c] * FDV_INTRA_PRUNE_DEN >
            (int64_t)cand_sad[0] * FDV_INTRA_PRUNE_NUM)
            break;                       /* same QP gate as the screen */
        const uint8_t *pred = cand_pred[c];

        int16_t res[16], level[16];
        int32_t coeff[16], dcoeff[16];
        const uint8_t *sblk = &src[(size_t)by * stride + bx];
#if defined(__ARM_NEON)
        uint8x8_t s01 = ld_2rows(sblk, stride);
        uint8x8_t s23 = ld_2rows(sblk + 2 * stride, stride);
        vst1q_s16(res,     vreinterpretq_s16_u16(vsubl_u8(s01, vld1_u8(pred))));
        vst1q_s16(res + 8, vreinterpretq_s16_u16(vsubl_u8(s23, vld1_u8(pred + 8))));
#else
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                res[i * 4 + j] = (int16_t)(sblk[i * stride + j] - pred[i * 4 + j]);
#endif
        fdv_fdct4x4(res, coeff);
        fdv_quant4x4(coeff, level, qp);
        int16_t rres[16];
        fdv_dequant4x4(level, dcoeff, qp);
        fdv_idct4x4(dcoeff, rres);

        int32_t ssd_i = 0;
        int bits = FDV_BITS_MODE;           /* the mode symbol */
#if defined(__ARM_NEON)
        {
            /* reconstruct = clip255(pred + residual), then accumulate
             * the squared error against the source, eight at a time. */
            uint8x8_t r01 = vqmovun_s16(vaddq_s16(
                vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pred))), vld1q_s16(rres)));
            uint8x8_t r23 = vqmovun_s16(vaddq_s16(
                vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pred + 8))), vld1q_s16(rres + 8)));
            int16x8_t d01 = vreinterpretq_s16_u16(vsubl_u8(s01, r01));
            int16x8_t d23 = vreinterpretq_s16_u16(vsubl_u8(s23, r23));
            int32x4_t acc = vmull_s16(vget_low_s16(d01), vget_low_s16(d01));
            acc = vmlal_s16(acc, vget_high_s16(d01), vget_high_s16(d01));
            acc = vmlal_s16(acc, vget_low_s16(d23),  vget_low_s16(d23));
            acc = vmlal_s16(acc, vget_high_s16(d23), vget_high_s16(d23));
            ssd_i = vaddvq_s32(acc);
        }
#else
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                int r = clip255(pred[i * 4 + j] + rres[i * 4 + j]);
                int d = sblk[i * stride + j] - r;
                ssd_i += d * d;
            }
#endif
        int blast = -1;
        for (int k = 0; k < 16; ++k) if (level[fdv_zz4[k]] != 0) blast = k;
        bits += fdv_bits_val((uint32_t)(blast + 1));    /* end-of-block count */
        for (int k = 0; k <= blast; ++k) bits += fdv_bits_val(fdv_zz_enc(level[fdv_zz4[k]]));

        double J = (double)ssd_i + lambda * bits;
        if (best_cost < 0.0 || J < best_cost) {
            best_cost = J;
            best_mode = cand_mode[c];
            best_c = c;
        }
    }

    /* Re-code the winner, this time with RDOQ, and keep that. */
    {
        const uint8_t *pred = cand_pred[best_c];
        int16_t res[16], level[16], rres[16];
        int32_t coeff[16], dcoeff[16];
        const uint8_t *sblk = &src[(size_t)by * stride + bx];
#if defined(__ARM_NEON)
        vst1q_s16(res, vreinterpretq_s16_u16(
            vsubl_u8(ld_2rows(sblk, stride), vld1_u8(pred))));
        vst1q_s16(res + 8, vreinterpretq_s16_u16(
            vsubl_u8(ld_2rows(sblk + 2 * stride, stride), vld1_u8(pred + 8))));
#else
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                res[i * 4 + j] = (int16_t)(sblk[i * stride + j] - pred[i * 4 + j]);
#endif
        fdv_fdct4x4(res, coeff);
        fdv_quant4x4(coeff, level, qp);
        fdv_rdoq4x4(level, res, qp, lambda);
        fdv_dequant4x4(level, dcoeff, qp);
        fdv_idct4x4(dcoeff, rres);
#if defined(__ARM_NEON)
        vst1_u8(best_rec, vqmovun_s16(vaddq_s16(
            vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pred))), vld1q_s16(rres))));
        vst1_u8(best_rec + 8, vqmovun_s16(vaddq_s16(
            vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pred + 8))), vld1q_s16(rres + 8))));
#else
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                best_rec[i * 4 + j] =
                    (uint8_t)clip255(pred[i * 4 + j] + rres[i * 4 + j]);
#endif
        memcpy(best_level, level, sizeof(best_level));
    }

    const uint8_t *fblk = &src[(size_t)by * stride + bx];
    int32_t fssd = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            rec[(by + i) * w + bx + j] = best_rec[i * 4 + j];
            int d = fblk[i * stride + j] - best_rec[i * 4 + j];
            fssd += d * d;
        }

    *mode_out = best_mode;                  /* structure stream */
    int last = -1;
    for (int k = 0; k < 16; ++k) if (best_level[fdv_zz4[k]] != 0) last = k;
    int fbits = FDV_BITS_MODE + fdv_bits_val((uint32_t)(last + 1));
    /* Counts and levels go to separate models, as in the P-frame path -- and it
     * matters more here. Most 4x4 leaves come out empty, so the counts are tens
     * of thousands of symbols at a tenth of a bit while the levels are a few
     * thousand at three or four bits. Pooling two distributions that far apart
     * cost about 40% of the coefficient stream. */
    cw->n[cw->np++] = (uint8_t)(last + 1);
    for (int k = 0; k <= last; ++k) {
        uint32_t z = fdv_zz_enc(best_level[fdv_zz4[k]]);
        cw->lp = fdv_leb_put(cw->l, cw->lp, z);
        fbits += fdv_bits_val(z);
    }
    return (double)fssd + lambda * fbits;
}

/* A quadtree over macroblocks: built, measured, and not kept.
 *
 * The inter side looks like the intra side did. At a realistic quantizer 90% to
 * 99.8% of macroblocks come out SKIP, so a frame spends two thousand mode
 * symbols saying a picture has not changed -- the same shape as the intra
 * path's one mode per 4x4 block, and apparently the same fix: 64x64 and 32x32
 * coding tree units whose unsplit leaf means "nothing moved here, using this
 * vector", collapsing sixteen mode symbols into a flag and a merge index.
 *
 * It works, in the sense that it does what it was built to do: on `tiny` it cut
 * structure symbols from 2078 a frame to 338, on `skyline` from 2886 to 959.
 * It is still not worth having. BD-rate over the scene library came out +0.12%
 * with one level and +0.37% with two, and decode got 18% to 31% *slower*.
 *
 * The reason the two paths differ is worth keeping, because the analogy is a
 * good one and it is wrong. An intra mode is one of nine, one per 4x4 block,
 * and costs about 0.16 bits x 32640 -- real money. An inter mode symbol sits in
 * a stream that is 99% SKIP, where it costs about a fiftieth of a bit: 2040 of
 * them come to roughly forty bits a frame, and no amount of structure can save
 * forty bits and still pay for its own flags. What the tree actually collects
 * is per-macroblock *merge indices*, which is why `skyline` gains 5.4% and
 * `pan` -- uniform motion, where every candidate dedups to one and no index is
 * sent -- loses 5.9%. Those cancel.
 *
 * Two things learned on the way that are worth not relearning:
 *
 *   - A node coder has to compare like with like. Returning only the rate of
 *     the symbols a macroblock emitted, and leaving its distortion inside its
 *     own decision, makes splitting look nearly free and costs +3.7%.
 *   - Pricing the mode symbol from the *coded* symbol history is circular:
 *     leaves win, few modes are emitted, the few that are look expensive, and
 *     more leaves win. The counterfactual a leaf-versus-split comparison needs
 *     is what a mode symbol would cost if every macroblock signalled for
 *     itself, which is the macroblock tally, not the symbol stream.
 */

/* Which model a macroblock's mode is coded under: whether its left and above
 * neighbours were skipped.
 *
 * Skipping is contagious. A macroblock whose neighbours both had nothing to code
 * is overwhelmingly likely to have nothing either -- that is what a still region
 * looks like -- while one bordering coded blocks is on the edge of something
 * moving. Sharing a single model between those two cases wastes most of what the
 * mode symbol could tell you, and the mode symbol is a fifth of the stream on
 * cheap content, which is exactly where this codec was furthest behind.
 *
 * Splitting on the pair is worth 40% to 60% of the mode stream depending on
 * scene. It is the same thing H.264 conditions its skip flag on. Blocks off the
 * edge of the frame count as skipped, so the top-left corner starts in the
 * quietest model rather than an arbitrary one. */
#define FDV_MB_CTX 4
/* The structure stream's models. Four for the macroblock mode, keyed on the
 * neighbours, then one each for the things a coded macroblock says afterwards.
 * A reference index, a motion vector component, an intra sub-mode and a merge
 * index have nothing in common -- one is almost always zero, one is a signed
 * delta with a long tail, one picks among nine directions, one among three --
 * and pooling them cost 9% to 31% of the stream depending on scene. */
enum { PS_REF = FDV_MB_CTX, PS_MVX, PS_MVY, PS_SUB, PS_MERGE, PS_NSTRUCT };

static int mb_mode_ctx(const uint8_t *map, int mw, int mx, int my) {
    int L = mx > 0 ? map[my * mw + mx - 1] : 0;
    int A = my > 0 ? map[(my - 1) * mw + mx] : 0;
    return (L != 0 ? 2 : 0) + (A != 0 ? 1 : 0);
}

/* Which model an intra mode is coded under: whether the two neighbours it can
 * see agree with each other.
 *
 * The mode is the largest single cost in a key frame -- 58% to 73% of it, one
 * per 4x4 block -- and it is far from random: where the left and above blocks
 * chose the same mode, this block very likely chooses it too, because that is
 * what a flat or uniformly-textured region looks like. Where they disagree, the
 * block is on an edge and the mode is genuinely uncertain.
 *
 * Those are two different distributions and they were sharing one model.
 * Splitting them is worth 21.6% of the mode stream. Nine contexts, one per
 * predicted mode, were measured too and give 22.3% -- not enough more to pay
 * for four times the table bytes.
 *
 * DC stands in for a neighbour that is not there, so a block at the frame edge
 * lands in the "agree" model rather than an arbitrary one. */
static int intra_mode_ctx(const uint8_t *map, int mw, int cx, int cy) {
    int L = cx > 0 ? map[cy * mw + cx - 1] : FDV_INTRA_DC;
    int A = cy > 0 ? map[(cy - 1) * mw + cx] : FDV_INTRA_DC;
    return L == A ? 0 : 1;
}

/* Note for anyone tempted by H.264's most-probable-mode here, because it was
 * tried and it loses.
 *
 * Intra modes really are predictable from their neighbours -- a block's mode
 * matches the smaller of its left and above neighbour's 93% to 100% of the
 * time -- and the mode stream really is the largest single cost in a key frame,
 * 58% to 73% of it. Conditioning the mode on that prediction measures 20-26%
 * smaller as *conditional entropy*.
 *
 * But coding `mode - mpm` does not collect that. Conditional entropy is only
 * reachable that way if the conditional distributions are translates of one
 * another, and these are not: the pooled distribution is already heavily skewed
 * toward DC, and subtracting a predictor that varies by position spreads that
 * mass across symbols instead of concentrating it. Measured on five scenes it
 * came out between 0.3% and 1.8% *worse*, and better on only one.
 *
 * Collecting the 20-26% needs real context modelling -- a separate model per
 * predictor value, the way the coefficient streams are split -- not a
 * relabelling. That is worth doing; this was not.
 */

/* --- the intra quadtree ---------------------------------------------------
 *
 * Every block used to be 4x4, and one prediction mode was sent for each of
 * them. On a 960x544 plane that is 32640 mode symbols whether or not the
 * picture has anything to say, and profiling a key frame put mode signalling at
 * 62-90% of the frame on smooth content. At 0.16 bits a symbol that stream was
 * already at its entropy floor: what cost was the *number* of symbols, not the
 * coding of them, so no amount of better modelling could reach it.
 *
 * The block size is therefore chosen by rate-distortion rather than fixed. A
 * 16x16 coding tree unit is coded either as one 16x16 prediction or split into
 * four 8x8 nodes, each of which is again either one prediction or four 4x4
 * leaves. A flat region costs one mode symbol per 256 pixels instead of
 * sixteen; a detailed one still gets 4x4 blocks where they earn their keep. The
 * split decision is the same Lagrangian as the mode decision, so a larger block
 * wins only when it is genuinely cheaper.
 *
 * Larger leaves predict with fdv_intra_nxn -- DC, vertical, horizontal and the
 * plane fit, which is the one that can follow a gradient. 4x4 leaves keep the
 * nine H.264 directions. Either way the residual goes through code_residual,
 * which picks 4x4 or 8x8 transforms inside the leaf by an RD of its own.
 *
 * A node that runs off the edge of the plane is split with no flag sent: the
 * geometry says so and the decoder derives the same thing, which is what lets
 * chroma planes and odd tile sizes through without a special case. */


/* Worst case symbols from one coding tree unit, which is what the per-row
 * slices and the trial buffers are sized from.
 *
 * Structure: one split flag per node, and one mode per leaf -- at 32 that is
 * 1 + 4 + 16 flags and up to 64 modes. Counts: one end-of-block count per 4x4
 * block, so one per sixteen pixels. Flags: one transform-size flag per aligned
 * 8x8 region. Levels: sixteen coefficients of five LEB bytes per 4x4 block, and
 * the 8x8 transform's sixty-four over four regions comes to the same. */
#define FDV_CTU_CELLS       (FDV_CTU / 4 * FDV_CTU / 4)     /* 4x4 blocks */
#define FDV_CTU_MAX_STRUCT  (FDV_CTU_CELLS + FDV_CTU_CELLS / 3 + 2)
#define FDV_CTU_MAX_CNT     (FDV_CTU_CELLS + FDV_CTU_CELLS / 4 + 8)
#define FDV_CTU_MAX_LVL     (FDV_CTU * FDV_CTU * 5 + 64)

/* Why 32 and not 64.
 *
 * Every quadtree node holds two trials' worth of scratch on the stack, each
 * sized for the whole unit rather than for that node, and the recursion is one
 * frame per split level. At 32 that is about 90 KB, comfortable inside the
 * 512 KB a pthread gets by default. At 64 it is roughly 440 KB across four
 * levels and the encoder takes a SIGBUS on the threads iframe_encode spawns for
 * the chroma planes -- measured, not predicted.
 *
 * 64 is therefore untested rather than rejected. Reaching it means sizing the
 * trial buffers by the node instead of by the unit, which wants one scratch
 * arena per walk rather than four nested stack frames; that would cut the
 * current usage to about 14 KB as well. */
_Static_assert(FDV_CTU == 16 || FDV_CTU == 32,
               "a larger coding tree unit needs the trial scratch off the stack");


/* The intra path's symbol streams. Split flags are separated by node size
 * because a 16x16 splits far more often than an 8x8 does, and the mode of a
 * large leaf has a four-symbol alphabet against the 4x4 leaf's nine -- pooling
 * any of these would be pooling distributions with nothing in common. */
enum {
    IS_SPLIT32 = 0,   /* split flag of a 32x32 node                     */
    IS_SPLIT16,       /* split flag of a 16x16 node                     */
    IS_SPLIT8,        /* split flag of an 8x8 node                      */
    IS_MODEB,         /* prediction mode of an 8x8 or 16x16 leaf        */
    IS_MODE0,         /* 4x4 leaf mode, left and above neighbours agree */
    IS_MODE1,         /* 4x4 leaf mode, they disagree                   */
    IS_NSTRUCT,       /* ---- streams above are structure symbols ----  */
    IS_CNT = IS_NSTRUCT, /* 4x4 end-of-block counts, and region flags   */
    IS_LVL,           /* 4x4 coefficient levels                         */
    IS_CNT8,          /* 8x8 end-of-block counts                        */
    IS_LVL8,          /* 8x8 coefficient levels                         */
    IS_FLAG,          /* transform-size region flags                    */
    IS_N
};

/* Which stream a node's split flag belongs to. A 32x32 splits far more often
 * than an 8x8 does, so they are different distributions and get their own
 * models -- the same argument as everywhere else in this codec. */
static int split_stream(int n) {
    return n == 32 ? IS_SPLIT32 : n == 16 ? IS_SPLIT16 : IS_SPLIT8;
}

/* Where a node's symbols go while it is being coded.
 *
 * Structure symbols are appended to one array in coding order together with the
 * stream each belongs to, and partitioned into per-stream runs once the plane
 * is finished -- the same shape the P-frame path uses. Coefficients keep their
 * own two arrays because code_residual writes them through an fdv_cw. */
typedef struct {
    uint8_t *s, *sc;  size_t sp;      /* structure symbol, and its stream */
    fdv_cw   c;                       /* the four coefficient streams     */
} fdv_isw;

static void isw_put(fdv_isw *w, int stream, int sym) {
    w->s[w->sp] = (uint8_t)sym;
    w->sc[w->sp] = (uint8_t)stream;
    ++w->sp;
}

/* What a large leaf's mode looks like to the 4x4 mode context.
 *
 * intra_mode_ctx asks whether two neighbouring blocks predicted the same way,
 * and a 4x4 leaf beside a 16x16 one still wants an answer. The two mode sets
 * share DC, vertical and horizontal; the plane fit has no 4x4 counterpart and
 * maps to DC, which is where a gradient's neighbours mostly land anyway. */
static int nn_to_4x4_mode(int m) {
    switch (m) {
    case FDV_INTRA_NN_V: return FDV_INTRA_VERT;
    case FDV_INTRA_NN_H: return FDV_INTRA_HORIZ;
    default:             return FDV_INTRA_DC;
    }
}

/* code_residual and its inverse live with the P-frame path, which is where the
 * transform-size decision was first needed; the quadtree's larger leaves use
 * exactly the same coder. */
static void code_residual(const uint8_t *cur, int cstride, int ox, int oy,
                          const uint8_t *pred, int n, int qp, uint8_t *rec,
                          fdv_cw *w, double *ssd, int *bits);
static void decode_residual(fdv_cr *r, int *ok,
                            const uint8_t *pred, int n, int qp,
                            uint8_t *dst, int dstride, int ox, int oy);
static void decode_block4(fdv_cr *r, int *ok,
                          const uint8_t *pred, int pstride, int px, int py, int qp,
                          uint8_t *dst, int dstride, int dx, int dy);
static void gather_nb(const uint8_t *buf, int stride, int bx, int by, int n,
                      uint8_t *top, uint8_t *left, uint8_t *topleft,
                      int *ht, int *hl);

/* An 8x8 or 16x16 intra leaf: one prediction over the whole block, chosen by RD
 * among the modes its neighbours allow. Returns the leaf's cost. */
static double intra_leafn(const uint8_t *src, int stride, uint8_t *rec, int w,
                          int bx, int by, int n, int qp, double lambda,
                          int *mode_out, fdv_cw *cw) {
    uint8_t top[FDV_CTU], left[FDV_CTU], topleft;
    int ht, hl;
    gather_nb(rec, w, bx, by, n, top, left, &topleft, &ht, &hl);

    double best = -1.0;
    int    best_mode = FDV_INTRA_NN_DC;
    uint8_t best_rec[FDV_CTU * FDV_CTU];
    uint8_t bn[FDV_CTU_MAX_CNT], bl[FDV_CTU_MAX_LVL];
    uint8_t bn8[FDV_CTU_MAX_CNT], bl8[FDV_CTU_MAX_LVL];
    uint8_t bfl[FDV_CTU_MAX_CNT];
    fdv_cw bw = {0};

    for (int m = 0; m < FDV_INTRA_NN_NMODES; ++m) {
        if (!fdv_intra_nn_mode_ok(m, n, ht, hl)) continue;
        uint8_t pred[FDV_CTU * FDV_CTU], trec[FDV_CTU * FDV_CTU];
        uint8_t tn[FDV_CTU_MAX_CNT], tl[FDV_CTU_MAX_LVL];
        uint8_t tn8[FDV_CTU_MAX_CNT], tl8[FDV_CTU_MAX_LVL];
        uint8_t tfl[FDV_CTU_MAX_CNT];
        fdv_cw tw = { .n = tn, .l = tl, .n8 = tn8, .l8 = tl8, .fl = tfl };
        fdv_intra_nxn(m, top, left, topleft, n, ht, hl, pred);
        double D = 0.0;
        int bits = FDV_BITS_MODE;              /* the mode symbol */
        code_residual(src, stride, bx, by, pred, n, qp, trec, &tw, &D, &bits);
        double J = D + lambda * bits;
        if (best < 0.0 || J < best) {
            best = J; best_mode = m;
            memcpy(best_rec, trec, (size_t)n * n);
            memcpy(bn,  tn,  tw.np);  memcpy(bl,  tl,  tw.lp);
            memcpy(bn8, tn8, tw.np8); memcpy(bl8, tl8, tw.lp8);
            memcpy(bfl, tfl, tw.flp);
            bw = tw;
        }
    }

    for (int i = 0; i < n; ++i)
        memcpy(&rec[(size_t)(by + i) * w + bx], best_rec + (size_t)i * n, (size_t)n);
    memcpy(cw->n   + cw->np,  bn,  bw.np);  cw->np  += bw.np;
    memcpy(cw->l   + cw->lp,  bl,  bw.lp);  cw->lp  += bw.lp;
    memcpy(cw->n8  + cw->np8, bn8, bw.np8); cw->np8 += bw.np8;
    memcpy(cw->l8  + cw->lp8, bl8, bw.lp8); cw->lp8 += bw.lp8;
    memcpy(cw->fl  + cw->flp, bfl, bw.flp); cw->flp += bw.flp;
    *mode_out = best_mode;
    return best;
}

/* One quadtree node: code it as a leaf, code it as four children, keep the
 * cheaper. Returns the node's cost and leaves `rec`, `mmap` and `sink` holding
 * the winner.
 *
 * The two trials both write the node's area, so the split is tried first and
 * its results set aside. Neither trial reads anything inside the node that the
 * other wrote -- a child only ever reads neighbours outside the node, or cells
 * inside it that come earlier in z-order -- so the order costs nothing. */
static double intra_node(const uint8_t *src, int stride, uint8_t *rec, int w, int h,
                         int bx, int by, int n, int qp, double lambda,
                         uint8_t *mmap, int mmw, fdv_isw *sink) {
    int hn = n / 2;

    if (bx + n > w || by + n > h) {            /* implicit split, no flag */
        double J = 0.0;
        for (int k = 0; k < 4; ++k) {
            int cx = bx + (k & 1) * hn, cy = by + (k >> 1) * hn;
            if (cx >= w || cy >= h) continue;
            J += intra_node(src, stride, rec, w, h, cx, cy, hn, qp, lambda,
                            mmap, mmw, sink);
        }
        return J;
    }

    if (n == 4) {
        int mode = FDV_INTRA_DC;
        double J = intra_leaf4(src, stride, rec, w, bx, by, qp, lambda,
                               &mode, &sink->c);
        int cx = bx / 4, cy = by / 4;
        isw_put(sink, intra_mode_ctx(mmap, mmw, cx, cy) == 0 ? IS_MODE0 : IS_MODE1,
                mode);
        mmap[(size_t)cy * mmw + cx] = (uint8_t)mode;
        return J;
    }

    /* --- try the split ---------------------------------------------------- */
    uint8_t sS[FDV_CTU_MAX_STRUCT], sSC[FDV_CTU_MAX_STRUCT];
    uint8_t sN[FDV_CTU_MAX_CNT], sL[FDV_CTU_MAX_LVL];
    uint8_t sN8[FDV_CTU_MAX_CNT], sL8[FDV_CTU_MAX_LVL];
    uint8_t sFL[FDV_CTU_MAX_CNT];
    fdv_isw sw = { sS, sSC, 0,
                   { .n = sN, .l = sL, .n8 = sN8, .l8 = sL8, .fl = sFL } };
    double Jsplit = 0.0;
    for (int k = 0; k < 4; ++k)
        Jsplit += intra_node(src, stride, rec, w, h, bx + (k & 1) * hn,
                             by + (k >> 1) * hn, hn, qp, lambda, mmap, mmw, &sw);

    int cells = n / 4, mx0 = bx / 4, my0 = by / 4;
    uint8_t rsplit[FDV_CTU * FDV_CTU], msplit[FDV_CTU / 4 * FDV_CTU / 4];
    for (int i = 0; i < n; ++i)
        memcpy(rsplit + (size_t)i * n, &rec[(size_t)(by + i) * w + bx], (size_t)n);
    for (int i = 0; i < cells; ++i)
        memcpy(msplit + (size_t)i * cells, &mmap[(size_t)(my0 + i) * mmw + mx0],
               (size_t)cells);

    /* --- try the leaf ----------------------------------------------------- */
    uint8_t lN[FDV_CTU_MAX_CNT], lL[FDV_CTU_MAX_LVL];
    uint8_t lN8[FDV_CTU_MAX_CNT], lL8[FDV_CTU_MAX_LVL];
    uint8_t lFL[FDV_CTU_MAX_CNT];
    fdv_cw lcw = { .n = lN, .l = lL, .n8 = lN8, .l8 = lL8, .fl = lFL };
    int mode = FDV_INTRA_NN_DC;
    double Jleaf = intra_leafn(src, stride, rec, w, bx, by, n, qp, lambda,
                               &mode, &lcw);

    /* The flag itself costs the same either way, so it is not charged to
     * either side; charging both would only shift the comparison by a
     * constant. */
    int split = Jsplit < Jleaf;
    isw_put(sink, split_stream(n), split);

    if (split) {
        for (int i = 0; i < n; ++i)
            memcpy(&rec[(size_t)(by + i) * w + bx], rsplit + (size_t)i * n, (size_t)n);
        for (int i = 0; i < cells; ++i)
            memcpy(&mmap[(size_t)(my0 + i) * mmw + mx0], msplit + (size_t)i * cells,
                   (size_t)cells);
        memcpy(sink->s  + sink->sp, sS,  sw.sp);
        memcpy(sink->sc + sink->sp, sSC, sw.sp); sink->sp += sw.sp;
        memcpy(sink->c.n  + sink->c.np,  sN,  sw.c.np);  sink->c.np  += sw.c.np;
        memcpy(sink->c.l  + sink->c.lp,  sL,  sw.c.lp);  sink->c.lp  += sw.c.lp;
        memcpy(sink->c.n8 + sink->c.np8, sN8, sw.c.np8); sink->c.np8 += sw.c.np8;
        memcpy(sink->c.l8 + sink->c.lp8, sL8, sw.c.lp8); sink->c.lp8 += sw.c.lp8;
        memcpy(sink->c.fl + sink->c.flp, sFL, sw.c.flp); sink->c.flp += sw.c.flp;
        return Jsplit;
    }

    /* The leaf trial already left its reconstruction in `rec`; the mode map
     * still holds the split's per-cell modes and has to be overwritten. */
    isw_put(sink, IS_MODEB, mode);
    for (int i = 0; i < cells; ++i)
        memset(&mmap[(size_t)(my0 + i) * mmw + mx0],
               (uint8_t)nn_to_4x4_mode(mode), (size_t)cells);
    memcpy(sink->c.n  + sink->c.np,  lN,  lcw.np);  sink->c.np  += lcw.np;
    memcpy(sink->c.l  + sink->c.lp,  lL,  lcw.lp);  sink->c.lp  += lcw.lp;
    memcpy(sink->c.n8 + sink->c.np8, lN8, lcw.np8); sink->c.np8 += lcw.np8;
    memcpy(sink->c.l8 + sink->c.lp8, lL8, lcw.lp8); sink->c.lp8 += lcw.lp8;
    memcpy(sink->c.fl + sink->c.flp, lFL, lcw.flp); sink->c.flp += lcw.flp;
    return Jleaf;
}

/* The decoder's half of intra_node: the same recursion, reading the flags and
 * modes the encoder wrote instead of deciding them. */
static void intra_node_dec(const uint8_t *syms, size_t *cur, const size_t *end,
                           int *ok, uint8_t *dst, int stride, int w, int h,
                           int bx, int by, int n, int qp,
                           uint8_t *mmap, int mmw, fdv_cr *r) {
    if (!*ok) return;
    int hn = n / 2;

    if (bx + n > w || by + n > h) {
        for (int k = 0; k < 4; ++k) {
            int cx = bx + (k & 1) * hn, cy = by + (k >> 1) * hn;
            if (cx >= w || cy >= h) continue;
            intra_node_dec(syms, cur, end, ok, dst, stride, w, h, cx, cy, hn,
                           qp, mmap, mmw, r);
        }
        return;
    }

    if (n == 4) {
        int cx = bx / 4, cy = by / 4;
        int st = intra_mode_ctx(mmap, mmw, cx, cy) == 0 ? IS_MODE0 : IS_MODE1;
        int mode = fdv_rd_count(syms, &cur[st], end[st], FDV_INTRA_NMODES - 1, ok);
        if (!*ok) return;
        mmap[(size_t)cy * mmw + cx] = (uint8_t)mode;

        uint8_t top[8], left[4], topleft;
        int ht, htr, hl, htl;
        gather_neighbors(dst, w, stride, bx, by, top, left, &topleft,
                         &ht, &htr, &hl, &htl);
        uint8_t pred[16];
        fdv_intra_predict_4x4(mode, top, left, topleft, ht, hl, pred);
        decode_block4(r, ok, pred, 4, 0, 0, qp, dst, stride, bx, by);
        return;
    }

    int split = fdv_rd_count(syms, &cur[split_stream(n)],
                             end[split_stream(n)], 1, ok);
    if (!*ok) return;
    if (split) {
        for (int k = 0; k < 4; ++k)
            intra_node_dec(syms, cur, end, ok, dst, stride, w, h,
                           bx + (k & 1) * hn, by + (k >> 1) * hn, hn,
                           qp, mmap, mmw, r);
        return;
    }

    int mode = fdv_rd_count(syms, &cur[IS_MODEB], end[IS_MODEB],
                            FDV_INTRA_NN_NMODES - 1, ok);
    if (!*ok) return;
    uint8_t top[FDV_CTU], left[FDV_CTU], topleft;
    int ht, hl;
    gather_nb(dst, stride, bx, by, n, top, left, &topleft, &ht, &hl);
    uint8_t pred[FDV_CTU * FDV_CTU];
    fdv_intra_nxn(mode, top, left, topleft, n, ht, hl, pred);
    decode_residual(r, ok, pred, n, qp, dst, stride, bx, by);
    for (int i = 0; i < n / 4; ++i)
        memset(&mmap[(size_t)(by / 4 + i) * mmw + bx / 4],
               (uint8_t)nn_to_4x4_mode(mode), (size_t)(n / 4));
}

/* --- intra CTU walk -------------------------------------------------------
 *
 * Intra prediction is usually described as serial: every block reads its
 * neighbours' *reconstructed* pixels, so a block cannot start until the blocks
 * around it are done. But the dependency is narrow -- a coding tree unit needs
 * the row above only as far as one unit to its right, and the unit to its left.
 * That is a wavefront: once CTU row r-1 is two units ahead, row r can proceed
 * alongside it, and with a wide picture a handful of rows are in flight at once.
 *
 * This changes nothing about the output. Units still see exactly the neighbour
 * samples they saw serially, and the coded symbols are reassembled in raster
 * order afterwards -- each row writes into its own slice of the streams, since
 * appending to a shared cursor is the one thing that genuinely cannot be
 * parallel. The round-trip tests are what hold this to it. */

/* Per-row progress counters, one cache line apart.
 *
 * Packed as a plain int array these sit four bytes apart, so every row's
 * counter shares a line with its neighbours' -- and neighbours are exactly the
 * rows that poll each other. The line then ping-pongs between cores on every
 * unit and the wavefront stops scaling. Padding is the whole difference
 * between this being worth doing and not. */
/* Wavefront width. Wide is for luma, narrow for the chroma planes, which run
 * concurrently with it -- the three together must stay inside the machine, and
 * measured on a 14-core box 8+2+2 is the peak; past that the spin-waits start
 * costing more than the parallelism returns. */
#ifndef FDV_INTRA_WF_WIDE
#define FDV_INTRA_WF_WIDE 8
#endif
#ifndef FDV_INTRA_WF_NARROW
#define FDV_INTRA_WF_NARROW 2
#endif

#define FDV_INTRA_CACHELINE 128
typedef struct {
    _Atomic int v;
    char pad[FDV_INTRA_CACHELINE - sizeof(_Atomic int)];
} fdv_intra_prog;

typedef struct {
    const uint8_t *src;
    uint8_t       *rec;
    uint8_t       *mmap;          /* 4x4-granular mode map, for the mode context */
    uint8_t       *ws, *wsc, *wn, *wl, *wn8, *wl8, *wfl; /* per-row slices */
    size_t        *row_sp, *row_np, *row_lp, *row_np8, *row_lp8, *row_flp;
    size_t         s_stride, n_stride, l_stride;
    fdv_intra_prog    *progress;      /* units completed, per row */
    int            stride, w, h, qp, mmw, cols, rows;
    double         lambda;
    int            tid, nthreads;
} fdv_intra_ctx;

static void intra_ctu_row(fdv_intra_ctx *x, int r) {
    fdv_isw sink = { x->ws  + (size_t)r * x->s_stride,
                     x->wsc + (size_t)r * x->s_stride, 0,
                     { .n  = x->wn  + (size_t)r * x->n_stride,
                       .l  = x->wl  + (size_t)r * x->l_stride,
                       .n8 = x->wn8 + (size_t)r * x->n_stride,
                       .l8 = x->wl8 + (size_t)r * x->l_stride,
                       .fl = x->wfl + (size_t)r * x->n_stride } };
    int by = r * FDV_CTU;
    for (int c = 0; c < x->cols; ++c) {
        /* Wait until the row above has finished this unit and the next one:
         * that is exactly the top and top-right samples this unit reads. */
        if (r > 0) {
            int need = c + 2 < x->cols ? c + 2 : x->cols;
            while (atomic_load_explicit(&x->progress[r - 1].v, memory_order_acquire) < need)
                ; /* spin: the producer is a unit or two away, not milliseconds */
        }
        intra_node(x->src, x->stride, x->rec, x->w, x->h, c * FDV_CTU, by,
                   FDV_CTU, x->qp, x->lambda, x->mmap, x->mmw, &sink);
        atomic_store_explicit(&x->progress[r].v, c + 1, memory_order_release);
    }
    x->row_sp[r]  = sink.sp;
    x->row_np[r]  = sink.c.np;
    x->row_lp[r]  = sink.c.lp;
    x->row_np8[r] = sink.c.np8;
    x->row_lp8[r] = sink.c.lp8;
    x->row_flp[r] = sink.c.flp;
}

static void *intra_worker(void *v) {
    fdv_intra_ctx *x = v;
    for (int r = x->tid; r < x->rows; r += x->nthreads) intra_ctu_row(x, r);
    fdv_profile_flush();
    return NULL;
}

/* Threads are worth it only when there are enough units to amortise the
 * synchronisation, and enough columns for the wavefront to actually open up. */
static int intra_threads_for(int cols, int rows) {
    long px = (long)cols * rows * FDV_CTU * FDV_CTU;   /* pixels, not units */
    if (px < 128 * 1024) return 1;
    int t = px > 512 * 1024 ? FDV_INTRA_WF_WIDE : FDV_INTRA_WF_NARROW;
    if (cols < t * 2) t = cols / 2;
    return t < 1 ? 1 : t;
}

/* Walk the plane in coding tree units, serially or as a wavefront, and leave
 * the symbols in `sink` in raster order either way. */
static void intra_walk(const uint8_t *src, int stride, uint8_t *rec,
                       int w, int h, int qp, double lambda,
                       uint8_t *mmap, int mmw, fdv_isw *sink) {
    int cols = ceil_div(w, FDV_CTU), rows = ceil_div(h, FDV_CTU);
    int nthreads = intra_threads_for(cols, rows);

    if (nthreads <= 1) {                          /* small planes: stay serial */
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c)
                intra_node(src, stride, rec, w, h, c * FDV_CTU, r * FDV_CTU,
                           FDV_CTU, qp, lambda, mmap, mmw, sink);
        return;
    }

    size_t s_stride = (size_t)cols * FDV_CTU_MAX_STRUCT;
    size_t n_stride = (size_t)cols * FDV_CTU_MAX_CNT;
    size_t l_stride = (size_t)cols * FDV_CTU_MAX_LVL;
    fdv_intra_prog *progress = calloc((size_t)rows, sizeof(*progress));
    size_t *row_sp = calloc((size_t)rows, sizeof(*row_sp));
    size_t *row_np = calloc((size_t)rows, sizeof(*row_np));
    size_t *row_lp = calloc((size_t)rows, sizeof(*row_lp));
    size_t *row_np8 = calloc((size_t)rows, sizeof(*row_np8));
    size_t *row_lp8 = calloc((size_t)rows, sizeof(*row_lp8));
    size_t *row_flp = calloc((size_t)rows, sizeof(*row_flp));
    uint8_t *ws  = malloc(s_stride * (size_t)rows);
    uint8_t *wsc = malloc(s_stride * (size_t)rows);
    uint8_t *wn  = malloc(n_stride * (size_t)rows);
    uint8_t *wl  = malloc(l_stride * (size_t)rows);
    uint8_t *wn8 = malloc(n_stride * (size_t)rows);
    uint8_t *wl8 = malloc(l_stride * (size_t)rows);
    uint8_t *wfl = malloc(n_stride * (size_t)rows);
    pthread_t *tids = malloc((size_t)nthreads * sizeof(*tids));
    fdv_intra_ctx *ctx = malloc((size_t)nthreads * sizeof(*ctx));
    if (!progress || !row_sp || !row_np || !row_lp || !row_np8 || !row_lp8 ||
        !row_flp || !ws || !wsc || !wn || !wl || !wn8 || !wl8 || !wfl ||
        !tids || !ctx) {
        free(progress); free(row_sp); free(row_np); free(row_lp);
        free(row_np8); free(row_lp8); free(row_flp);
        free(ws); free(wsc); free(wn); free(wl); free(wn8); free(wl8); free(wfl);
        free(tids); free(ctx);
        for (int r = 0; r < rows; ++r)            /* fall back to the serial walk */
            for (int c = 0; c < cols; ++c)
                intra_node(src, stride, rec, w, h, c * FDV_CTU, r * FDV_CTU,
                           FDV_CTU, qp, lambda, mmap, mmw, sink);
        return;
    }

    fdv_intra_ctx proto = {src, rec, mmap, ws, wsc, wn, wl, wn8, wl8, wfl,
                       row_sp, row_np, row_lp, row_np8, row_lp8, row_flp,
                       s_stride, n_stride, l_stride,
                       progress, stride, w, h, qp, mmw, cols, rows, lambda,
                       0, nthreads};
    for (int t = 1; t < nthreads; ++t) {
        ctx[t] = proto; ctx[t].tid = t;
        if (pthread_create(&tids[t], NULL, intra_worker, &ctx[t]) != 0) {
            tids[t] = 0; intra_worker(&ctx[t]);
        }
    }
    ctx[0] = proto; ctx[0].tid = 0;
    intra_worker(&ctx[0]);
    for (int t = 1; t < nthreads; ++t)
        if (tids[t]) pthread_join(tids[t], NULL);

    /* Reassemble every stream in raster order. */
    for (int r = 0; r < rows; ++r) {
        memcpy(sink->s  + sink->sp, ws  + (size_t)r * s_stride, row_sp[r]);
        memcpy(sink->sc + sink->sp, wsc + (size_t)r * s_stride, row_sp[r]);
        sink->sp += row_sp[r];
        memcpy(sink->c.n  + sink->c.np,  wn  + (size_t)r * n_stride, row_np[r]);
        sink->c.np  += row_np[r];
        memcpy(sink->c.l  + sink->c.lp,  wl  + (size_t)r * l_stride, row_lp[r]);
        sink->c.lp  += row_lp[r];
        memcpy(sink->c.n8 + sink->c.np8, wn8 + (size_t)r * n_stride, row_np8[r]);
        sink->c.np8 += row_np8[r];
        memcpy(sink->c.l8 + sink->c.lp8, wl8 + (size_t)r * l_stride, row_lp8[r]);
        sink->c.lp8 += row_lp8[r];
        memcpy(sink->c.fl + sink->c.flp, wfl + (size_t)r * n_stride, row_flp[r]);
        sink->c.flp += row_flp[r];
    }

    free(progress); free(row_sp); free(row_np); free(row_lp);
    free(row_np8); free(row_lp8); free(row_flp);
    free(ws); free(wsc); free(wn); free(wl); free(wn8); free(wl8); free(wfl);
    free(tids); free(ctx);
}

size_t fdv_image_encode(const uint8_t *src, int w, int h, int stride, int qp,
                    uint8_t *out, size_t cap, uint8_t *recon) {
    if (w <= 0 || h <= 0 || (w & 3) || (h & 3)) return 0;

    int ccols = ceil_div(w, FDV_CTU), crows = ceil_div(h, FDV_CTU);
    size_t ctus = (size_t)ccols * crows;
    int mmw = w / 4, mmh = h / 4;

    uint8_t *rec  = malloc((size_t)w * h);
    uint8_t *mmap = calloc((size_t)mmw * mmh, 1);
    uint8_t *ss   = malloc(ctus * FDV_CTU_MAX_STRUCT);
    uint8_t *ssc  = malloc(ctus * FDV_CTU_MAX_STRUCT);
    uint8_t *cn   = malloc(ctus * FDV_CTU_MAX_CNT);
    uint8_t *cl   = malloc(ctus * FDV_CTU_MAX_LVL);
    uint8_t *cn8  = malloc(ctus * FDV_CTU_MAX_CNT);
    uint8_t *cl8  = malloc(ctus * FDV_CTU_MAX_LVL);
    uint8_t *cfl  = malloc(ctus * FDV_CTU_MAX_CNT);
    if (!rec || !mmap || !ss || !ssc || !cn || !cl || !cn8 || !cl8 || !cfl) {
        free(rec); free(mmap); free(ss); free(ssc);
        free(cn); free(cl); free(cn8); free(cl8); free(cfl);
        return 0;
    }

    fdv_isw sink = { ss, ssc, 0,
                     { .n = cn, .l = cl, .n8 = cn8, .l8 = cl8, .fl = cfl } };
    double lambda = FDV_LAMBDA0 * pow(2.0, (qp - 12) / 3.0);
    intra_walk(src, stride, rec, w, h, qp, lambda, mmap, mmw, &sink);
    free(mmap);

    /* In-loop deblock, applied after the whole plane is reconstructed (intra
     * prediction above used the unfiltered samples, exactly as the decoder
     * does). */
    fdv_deblock_plane(rec, w, h, w, qp);
    if (recon) memcpy(recon, rec, (size_t)w * h);
    free(rec);

    /* Partition the structure symbols into their streams, coding order kept
     * inside each. Every context is rebuilt by the decoder from what it has
     * already decoded, so none of it is transmitted. */
    size_t sn[IS_N];
    const uint8_t *sv[IS_N];
    {   uint8_t *part = malloc(sink.sp ? sink.sp : 1);
        if (!part) { free(ss); free(ssc); free(cn); free(cl);
                     free(cn8); free(cl8); free(cfl); return 0; }
        size_t a2 = 0;
        for (int k = 0; k < IS_NSTRUCT; ++k) {
            sv[k] = part + a2;
            size_t start = a2;
            for (size_t i = 0; i < sink.sp; ++i)
                if (ssc[i] == k) part[a2++] = ss[i];
            sn[k] = a2 - start;
        }
        memcpy(ss, part, sink.sp);
        for (int k = 0; k < IS_NSTRUCT; ++k) sv[k] = ss + (sv[k] - part);
        free(part);
    }
    sv[IS_CNT]  = cn;  sn[IS_CNT]  = sink.c.np;
    sv[IS_LVL]  = cl;  sn[IS_LVL]  = sink.c.lp;
    sv[IS_CNT8] = cn8; sn[IS_CNT8] = sink.c.np8;
    sv[IS_LVL8] = cl8; sn[IS_LVL8] = sink.c.lp8;
    sv[IS_FLAG] = cfl; sn[IS_FLAG] = sink.c.flp;

    size_t total = 0;
    for (int k = 0; k < IS_N; ++k) total += sn[k];

    FDV_ZB(FDV_Z_ENTROPY);
    uint8_t *all = malloc(total ? total : 1);
    if (!all) { FDV_ZE(FDV_Z_ENTROPY); free(ss); free(ssc); free(cn); free(cl);
                free(cn8); free(cl8); free(cfl); return 0; }
    {   size_t o2 = 0;
        for (int k = 0; k < IS_N; ++k) { memcpy(all + o2, sv[k], sn[k]); o2 += sn[k]; }
    }

    /* Candidate 1: one shared model over everything. */
    uint32_t c1[256] = {0};
    for (size_t i = 0; i < total; ++i) c1[all[i]]++;
    fdv_rans_sym e1[256]; fdv_rans_table d1;
    fdv_rans_build_tables(c1, e1, &d1);
    uint8_t *p1 = malloc(total * 2 + 1024);

    /* Candidate 2: a model per stream. */
    uint32_t ck[IS_N][256];
    fdv_rans_sym enc[IS_N][256];
    fdv_rans_table dec[IS_N];
    uint8_t *pay[IS_N] = {0};
    size_t   len[IS_N] = {0};
    int oom = (p1 == NULL);
    for (int k = 0; k < IS_N; ++k) {
        memset(ck[k], 0, sizeof ck[k]);
        for (size_t i = 0; i < sn[k]; ++i) ck[k][sv[k][i]]++;
        fdv_rans_build_tables(ck[k], enc[k], &dec[k]);
        pay[k] = malloc(sn[k] * 2 + 1024);
        if (!pay[k]) oom = 1;
    }
    if (oom) {
        FDV_ZE(FDV_Z_ENTROPY);
        free(ss); free(ssc); free(cn); free(cl); free(cn8); free(cl8); free(cfl);
        free(all); free(p1);
        for (int k = 0; k < IS_N; ++k) free(pay[k]);
        return 0;
    }
    size_t l1 = fdv_rans_encode(all, total, e1, p1, total * 2 + 1024);
    for (int k = 0; k < IS_N; ++k)
        len[k] = fdv_rans_encode(sv[k], sn[k], enc[k], pay[k], sn[k] * 2 + 1024);

    /* One varint symbol count per stream, common to all three candidates. */
    size_t hdr = 0;
    for (int k = 0; k < IS_N; ++k) hdr += fdv_leb_size((uint32_t)sn[k]);
    size_t size_one = hdr + fdv_rans_freqs_size(d1.freq) + fdv_leb_size((uint32_t)l1) + l1;
    size_t size_two = hdr;
    for (int k = 0; k < IS_N; ++k) {
        /* An empty stream says everything it has to say in its symbol count;
         * paying a frequency table for it costs more than it can ever save. */
        if (sn[k] == 0) continue;
        size_two += fdv_rans_freqs_size(dec[k].freq) + fdv_leb_size((uint32_t)len[k]) + len[k];
    }

    /* Candidate 3: the adaptive coder, which sends no tables at all.
     *
     * A key frame has no history to prime from -- it must decode standalone --
     * so this is the flat-prior case that loses badly on the P-frames' short
     * streams. Here it wins on most scenes at most QPs: an intra frame's
     * streams are orders of magnitude longer, so the cost of learning from
     * uniform amortizes away, and the model then tracks a frame whose
     * statistics change from flat regions to detailed ones -- which one static
     * table per stream cannot do at any price. */
    size_t size_ada = (size_t)-1, lada = 0;
    uint8_t *pada = NULL;
    if (FDV_AD_CAP > 0) {                  /* one switch turns the coder off */
        size_t cap2 = total * 3 + 4096;
        pada = malloc(cap2);
        if (pada) {
            uint32_t flat[256] = {0};
            fdv_aenc ae; fdv_aenc_init(&ae, pada, cap2);
            for (int k = 0; k < IS_N; ++k) {
                fdv_amodel m; fdv_amodel_init(&m, flat);
                for (size_t i = 0; i < sn[k]; ++i) fdv_aenc_sym(&ae, &m, sv[k][i]);
            }
            lada = fdv_aenc_finish(&ae);       /* 0 if the buffer ran out */
            if (lada) size_ada = hdr + fdv_leb_size((uint32_t)lada) + lada;
        }
    }

    /* 0: one shared model.  1: a model per stream.  2: one adaptive blob. */
    int imode = 0; size_t best = size_one;
    if (size_two < best) { best = size_two; imode = 1; }
    if (size_ada < best) { best = size_ada; imode = 2; }
    FDV_ZE(FDV_Z_ENTROPY);

    /* Reported as 3 so it shares the P-frame path's "adaptive" label; the two
     * paths number their modes independently. */
    size_t ncoeff = sn[IS_CNT] + sn[IS_LVL] + sn[IS_CNT8] + sn[IS_LVL8] + sn[IS_FLAG];
    fdv_note_entropy(imode == 2 ? 3 : imode, total - ncoeff, ncoeff);
    FDV_LOG(FDV_LOG_FRAME, "image",
            "%dx%d qp%-2d  split %zu/%zu  modes big=%zu small=%zu  "
            "coeff4 %zu/%zu coeff8 %zu/%zu  "
            "entropy=%s (shared %zu B vs split %zu B vs adaptive %zu B)",
            w, h, qp, sn[IS_SPLIT16], sn[IS_SPLIT8], sn[IS_MODEB],
            sn[IS_MODE0] + sn[IS_MODE1], sn[IS_CNT], sn[IS_LVL],
            sn[IS_CNT8], sn[IS_LVL8],
            fdv_emode_name(imode == 2 ? 3 : imode), size_one, size_two, size_ada);

    size_t o = 0;
    if (5 + best > cap) goto fail;
    o = fdv_put16(out, o, (unsigned)w);
    o = fdv_put16(out, o, (unsigned)h);
    out[o++] = (uint8_t)qp;
    out[o++] = (uint8_t)imode;
    for (int k = 0; k < IS_N; ++k) o = fdv_leb_put(out, o, (uint32_t)sn[k]);
    if (imode == 2) {
        o = fdv_leb_put(out, o, (uint32_t)lada);
        memcpy(out + o, pada, lada); o += lada;
    } else if (imode == 0) {
        o = fdv_rans_write_freqs(out, o, d1.freq);
        o = fdv_leb_put(out, o, (uint32_t)l1);
        memcpy(out + o, p1, l1); o += l1;
    } else {
        for (int k = 0; k < IS_N; ++k) {
            if (sn[k] == 0) continue;
            o = fdv_rans_write_freqs(out, o, dec[k].freq);
            o = fdv_leb_put(out, o, (uint32_t)len[k]);
            memcpy(out + o, pay[k], len[k]); o += len[k];
        }
    }
    free(ss); free(ssc); free(cn); free(cl); free(cn8); free(cl8); free(cfl);
    free(all); free(p1); free(pada);
    for (int k = 0; k < IS_N; ++k) free(pay[k]);
    return o;
fail:
    free(ss); free(ssc); free(cn); free(cl); free(cn8); free(cl8); free(cfl);
    free(all); free(p1); free(pada);
    for (int k = 0; k < IS_N; ++k) free(pay[k]);
    return 0;
}

int fdv_image_decode(const uint8_t *in, size_t len,
                 uint8_t *dst, int stride, int *w_out, int *h_out) {
    if (len < 6) return -1;
    size_t p = 0;
    int w = (int)fdv_get16(in, &p);
    int h = (int)fdv_get16(in, &p);
    int qp = in[p++];
    if (w <= 0 || h <= 0 || (w & 3) || (h & 3) || w > 8192 || h > 8192) return -1;
    if (qp < 0 || qp > 51) return -1;   /* qp indexes the quant tables (52 entries) */

    int imode = in[p++];
    if (imode > 2) return -1;

    /* What the plane's geometry allows each stream to hold. A header claiming
     * more than this is malformed, and without the check it would force an
     * absurd malloc or let a cursor run past what the walk can consume. */
    size_t ctus = (size_t)ceil_div(w, FDV_CTU) * ceil_div(h, FDV_CTU);
    size_t b4   = (size_t)(w / 4) * (h / 4);
    const size_t cap_s[IS_N] = {
        [IS_SPLIT32] = ctus,
        [IS_SPLIT16] = 4 * ctus,
        [IS_SPLIT8]  = 16 * ctus,
        [IS_MODEB]   = 21 * ctus,   /* one per node above 4x4 */
        [IS_MODE0]   = b4, [IS_MODE1] = b4,
        [IS_CNT]     = ctus * FDV_CTU_MAX_CNT,
        [IS_LVL]     = ctus * FDV_CTU_MAX_LVL,
        [IS_CNT8]    = ctus * FDV_CTU_MAX_CNT,
        [IS_LVL8]    = ctus * FDV_CTU_MAX_LVL,
        [IS_FLAG]    = ctus * FDV_CTU_MAX_CNT,
    };

    size_t sn[IS_N], off[IS_N], total = 0;
    {   int ok = 1;
        for (int k = 0; k < IS_N; ++k) {
            uint32_t v = fdv_rd_count32(in, &p, len, &ok);
            if (!ok || v > cap_s[k]) return -1;
            off[k] = total;
            sn[k]  = v;
            total += v;
        }
    }

    uint8_t *syms = malloc(total ? total : 1);
    if (!syms) return -1;

    if (imode == 2) {                            /* one adaptive blob, no tables */
        int ok = 1;
        uint32_t la = fdv_rd_count32(in, &p, len, &ok);
        if (!ok || p + la > len) { free(syms); return -1; }
        /* A key frame primes from nothing, exactly as the encoder did. */
        uint32_t flat[256] = {0};
        fdv_adec ad; fdv_adec_init(&ad, in + p, la);
        for (int k = 0; k < IS_N; ++k) {
            fdv_amodel m; fdv_amodel_init(&m, flat);
            for (size_t i = 0; i < sn[k]; ++i)
                syms[off[k] + i] = (uint8_t)fdv_adec_sym(&ad, &m);
        }
    } else if (imode == 0) {                     /* one shared model over all */
        uint16_t freq[256];
        int ok = 1;
        p = fdv_rans_read_freqs_bounded(in, p, len, freq, &ok);
        if (!ok) { free(syms); return -1; }
        uint32_t l1 = fdv_rd_count32(in, &p, len, &ok);
        if (!ok || p + l1 > len) { free(syms); return -1; }
        fdv_rans_table d1;
        fdv_rans_dec_table_from_freq(freq, &d1);
        fdv_rans_decode_bounded(in + p, l1, total, &d1, syms);
    } else {                                     /* a model per stream */
        for (int k = 0; k < IS_N; ++k) {
            if (sn[k] == 0) continue;
            uint16_t freq[256];
            int ok = 1;
            p = fdv_rans_read_freqs_bounded(in, p, len, freq, &ok);
            if (!ok) { free(syms); return -1; }
            uint32_t lk = fdv_rd_count32(in, &p, len, &ok);
            if (!ok || p + lk > len) { free(syms); return -1; }
            fdv_rans_table dk;
            fdv_rans_dec_table_from_freq(freq, &dk);
            fdv_rans_decode_bounded(in + p, lk, sn[k], &dk, syms + off[k]);
            p += lk;
        }
    }

    /* One bounded cursor per stream. A malformed stream flips ok=0 and is
     * rejected rather than over-read; the recursion itself is driven by the
     * plane's geometry, so no stream content can make it run away. */
    size_t cur[IS_N], end[IS_N];
    for (int k = 0; k < IS_N; ++k) { cur[k] = off[k]; end[k] = off[k] + sn[k]; }
    /* The intra path codes one plane per call, so it never uses the chroma
     * streams and leaves those cursors empty. */
    fdv_cr r = { .b = syms,
                 .np  = cur[IS_CNT],  .nend  = end[IS_CNT],
                 .lp  = cur[IS_LVL],  .lend  = end[IS_LVL],
                 .np8 = cur[IS_CNT8], .nend8 = end[IS_CNT8],
                 .lp8 = cur[IS_LVL8], .lend8 = end[IS_LVL8],
                 .flp = cur[IS_FLAG], .flend = end[IS_FLAG] };

    int mmw = w / 4;
    uint8_t *mmap = calloc((size_t)mmw * (h / 4), 1);
    if (!mmap) { free(syms); return -1; }

    int ok = 1;
    for (int by = 0; by < h && ok; by += FDV_CTU)
        for (int bx = 0; bx < w && ok; bx += FDV_CTU)
            intra_node_dec(syms, cur, end, &ok, dst, stride, w, h,
                           bx, by, FDV_CTU, qp, mmap, mmw, &r);

    free(syms);
    free(mmap);
    if (!ok) return -1;
    fdv_deblock_plane(dst, w, h, stride, qp);   /* same filter as the encoder */
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
    return 0;
}

/* ===========================================================================
 * 11. VIDEO
 * P-frame video codec, full YUV 4:2:0.
 * ======================================================================== */


#define MB 16          /* luma macroblock size for motion */
#define CB (MB / 2)    /* chroma block size (4:2:0) */
#ifndef FDV_ME_RANGE
#define FDV_ME_RANGE 16    /* integer motion search radius (pels) */
#endif

/* Motion compensation reads FDV_ME_RANGE pixels beyond the visible plane, plus
 * three more for the 6-tap sub-pel filter's right-hand taps. The border has to
 * cover both. Widening the search without widening the border reads past the
 * allocation -- silently, and identically in encoder and decoder, so it decodes
 * "successfully" into a wrecked picture: raising the range to 32 cost 24 dB and
 * nothing complained. Fail the build instead. */
_Static_assert(FDV_BORDER >= FDV_ME_RANGE + 4,
               "FDV_BORDER must cover the motion search range plus the sub-pel filter taps");


/* Copy a reconstructed I420 frame into the bordered reference and extend. */
static void load_ref(fdv_frame *ref, const uint8_t *y, const uint8_t *u,
                     const uint8_t *v, int w, int h) {
    int cw = w / 2, ch = h / 2;
    for (int r = 0; r < h; ++r)
        memcpy(fdv_plane_at(&ref->planes[0], 0, r), y + (size_t)r * w, w);
    for (int r = 0; r < ch; ++r) {
        memcpy(fdv_plane_at(&ref->planes[1], 0, r), u + (size_t)r * cw, cw);
        memcpy(fdv_plane_at(&ref->planes[2], 0, r), v + (size_t)r * cw, cw);
    }
    fdv_frame_extend_borders(ref);
}

/* Gather n top and n left neighbor samples of the block at (bx,by) from a
 * tightly packed plane with the given stride (the current frame's in-progress
 * reconstruction). */
static void gather_nb(const uint8_t *buf, int stride, int bx, int by, int n,
                      uint8_t *top, uint8_t *left, uint8_t *topleft,
                      int *ht, int *hl) {
    *ht = by > 0; *hl = bx > 0;
    for (int k = 0; k < n; ++k) { top[k] = 0; left[k] = 0; }
    if (*ht) for (int k = 0; k < n; ++k) top[k]  = buf[(by - 1) * stride + bx + k];
    if (*hl) for (int k = 0; k < n; ++k) left[k] = buf[(by + k) * stride + bx - 1];
    *topleft = (*ht && *hl) ? buf[(by - 1) * stride + bx - 1] : 128;
}

/* Code one 4x4 block (cur at (cx,cy) minus pred at (px,py)): quant + RDOQ,
 * reconstruct into rec at (rx,ry), EOB-emit, accumulate D (vs cur) and bits. */

static void code_block4(const uint8_t *cur, int cstride, int cx, int cy,
                          const uint8_t *pred, int pstride, int px, int py,
                          uint8_t *rec, int rstride, int rx, int ry,
                          int qp, double lambda, fdv_cw *w,
                          double *D, int *bits) {
    int16_t res[16], level[16];
    int32_t coeff[16], dcoeff[16];
    const uint8_t *cb = &cur[(size_t)cy * cstride + cx];
    const uint8_t *pb = &pred[(size_t)py * pstride + px];
#if defined(__ARM_NEON)
    uint8x8_t c01 = ld_2rows(cb, cstride), c23 = ld_2rows(cb + 2 * cstride, cstride);
    uint8x8_t p01 = ld_2rows(pb, pstride), p23 = ld_2rows(pb + 2 * pstride, pstride);
    vst1q_s16(res,     vreinterpretq_s16_u16(vsubl_u8(c01, p01)));
    vst1q_s16(res + 8, vreinterpretq_s16_u16(vsubl_u8(c23, p23)));
#else
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            res[i * 4 + j] = (int16_t)(cb[i * cstride + j] - pb[i * pstride + j]);
#endif
    fdv_fdct4x4(res, coeff);
    fdv_quant4x4(coeff, level, qp);
    fdv_rdoq4x4(level, res, qp, lambda);
    int16_t rr[16];
    fdv_dequant4x4(level, dcoeff, qp);
    fdv_idct4x4(dcoeff, rr);
    /* Sum of squared differences of 8-bit samples is an integer, and a small
     * one (16 terms of at most 255^2). Accumulating it in int and converting
     * once is exactly the value the per-pixel double accumulation produced --
     * every partial sum is representable -- at a fraction of the cost, and
     * this runs a few million times per 720p frame. */
    int32_t ssd = 0;
    uint8_t *rb = &rec[(size_t)ry * rstride + rx];
#if defined(__ARM_NEON)
    {
        uint8x8_t r01 = vqmovun_s16(vaddq_s16(
            vreinterpretq_s16_u16(vmovl_u8(p01)), vld1q_s16(rr)));
        uint8x8_t r23 = vqmovun_s16(vaddq_s16(
            vreinterpretq_s16_u16(vmovl_u8(p23)), vld1q_s16(rr + 8)));
        st_2rows(rb, rstride, r01);
        st_2rows(rb + 2 * rstride, rstride, r23);
        uint8x8_t d01 = vabd_u8(c01, r01), d23 = vabd_u8(c23, r23);
        uint16x8_t sq = vmull_u8(d01, d01);
        sq = vmlal_u8(sq, d23, d23);
        ssd = (int32_t)vaddlvq_u16(sq);
    }
#else
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            int r = clip255(pb[i * pstride + j] + rr[i * 4 + j]);
            rb[i * rstride + j] = (uint8_t)r;
            int d = cb[i * cstride + j] - r;
            ssd += d * d;
        }
#endif
    *D += (double)ssd;
    int last = -1;
    for (int k = 0; k < 16; ++k) if (level[fdv_zz4[k]] != 0) last = k;
    cw_count(w, (unsigned)(last + 1));
    *bits += fdv_bits_val((uint32_t)(last + 1));
    for (int k = 0; k <= last; ++k) {
        uint32_t z = fdv_zz_enc(level[fdv_zz4[k]]);
        cw_level(w, z);
        *bits += fdv_bits_val(z);
    }
}

static void decode_block4(fdv_cr *r, int *ok,
                          const uint8_t *pred, int pstride, int px, int py, int qp,
                          uint8_t *dst, int dstride, int dx, int dy) {
    int16_t level[16] = {0};
    int cnt = cr_count(r, 16, ok);
    for (int k = 0; k < cnt; ++k)
        level[fdv_zz4[k]] = (int16_t)cr_level(r, ok);
    int32_t dcoeff[16];
    int16_t rr[16];
    fdv_dequant4x4(level, dcoeff, qp);
    fdv_idct4x4(dcoeff, rr);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[(dy + i) * dstride + dx + j] =
                clip255(pred[(py + i) * pstride + px + j] + rr[i * 4 + j]);
}

/* Code an n x n residual. For each aligned 8x8 region (n>=8) the transform size
 * is chosen by RD between four 4x4 transforms and one 8x8 transform, signaled by
 * a 1-byte flag; n==4 stays a single flagless 4x4. */
static void fdv_code_residual_inner(const uint8_t *cur, int cstride, int ox, int oy,
                            const uint8_t *pred, int n, int qp, uint8_t *rec,
                            fdv_cw *w, double *ssd, int *bits) {
    double e = 0.0; int b = 0;
    double lambda = FDV_LAMBDA0 * pow(2.0, (qp - 12) / 3.0);

    if (n == 4) {
        code_block4(cur, cstride, ox, oy, pred, n, 0, 0, rec, n, 0, 0,
                    qp, lambda, w, &e, &b);
        *ssd += e; *bits += b;
        return;
    }

    for (int ry = 0; ry < n; ry += 8)
        for (int rx = 0; rx < n; rx += 8) {
            /* Option A: four 4x4 transforms, into a temp stream + local rec. */
            uint8_t nA[8], lA[4 * 16 * 5], recA[64];
            fdv_cw wA = { .n = nA, .l = lA };          /* 4x4 trial */
            double eA = 0.0; int bA = FDV_BITS_FLAG;      /* region flag */
            for (int sy = 0; sy < 8; sy += 4)
                for (int sx = 0; sx < 8; sx += 4)
                    code_block4(cur, cstride, ox + rx + sx, oy + ry + sy,
                                pred, n, rx + sx, ry + sy, recA, 8, sx, sy,
                                qp, lambda, &wA, &eA, &bA);

            /* Option B: one 8x8 transform. */
            int16_t res8[64], lev8[64];
            int32_t co8[64], dc8[64];
            int16_t rr8[64];
            const uint8_t *cB = &cur[(size_t)(oy + ry) * cstride + ox + rx];
            const uint8_t *pB = &pred[(size_t)ry * n + rx];
#if defined(__ARM_NEON)
            for (int i = 0; i < 8; ++i)
                vst1q_s16(&res8[i * 8], vreinterpretq_s16_u16(
                    vsubl_u8(vld1_u8(&cB[i * cstride]), vld1_u8(&pB[i * n]))));
#else
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j)
                    res8[i * 8 + j] = (int16_t)(cB[i * cstride + j] - pB[i * n + j]);
#endif
            fdv_fdct8x8(res8, co8);
            fdv_quant8x8(co8, lev8, qp);
            fdv_dequant8x8(lev8, dc8, qp);
            fdv_idct8x8(dc8, rr8);
            uint8_t recB[64];
            /* Integer accumulation: 64 terms of at most 255^2, exact in int32
             * and exactly what the per-pixel double sum produced. */
            int32_t eB_i = 0;
#if defined(__ARM_NEON)
            {
                uint32x4_t acc = vdupq_n_u32(0);
                for (int i = 0; i < 8; ++i) {
                    uint8x8_t pv = vld1_u8(&pB[i * n]);
                    uint8x8_t rv = vqmovun_s16(vaddq_s16(
                        vreinterpretq_s16_u16(vmovl_u8(pv)), vld1q_s16(&rr8[i * 8])));
                    vst1_u8(&recB[i * 8], rv);
                    uint8x8_t dv = vabd_u8(vld1_u8(&cB[i * cstride]), rv);
                    acc = vpadalq_u16(acc, vmull_u8(dv, dv));
                }
                eB_i = (int32_t)vaddvq_u32(acc);
            }
#else
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j) {
                    int r = clip255(pB[i * n + j] + rr8[i * 8 + j]);
                    recB[i * 8 + j] = (uint8_t)r;
                    int d = cB[i * cstride + j] - r;
                    eB_i += d * d;
                }
#endif
            double eB = (double)eB_i;
            uint8_t nB[2], lB[64 * 5];
            fdv_cw wB = { .n8 = nB, .l8 = lB };        /* 8x8 trial */
            fdv_coeff8_encode(lev8, &wB);
            int lev8_last1 = 0;                  /* coefficients actually sent */
            for (int k = 0; k < 64; ++k) if (lev8[fdv_zz8[k]] != 0) lev8_last1 = k + 1;
            int bB = FDV_BITS_FLAG + fdv_bits_val((uint32_t)lev8_last1);
            for (int k = 0; k < lev8_last1; ++k)
                bB += fdv_bits_val(fdv_zz_enc(lev8[fdv_zz8[k]]));

            /* A region with nothing in it is the common case once the quantizer
             * bites, and it used to cost a full set of end-of-block counts all
             * the same -- four for the 4x4 split, one for the 8x8. That is a
             * floor the quantizer cannot get under: at QP 45 on a panning clip
             * the counts were still 292 bytes a frame against 20 bytes of actual
             * coefficients. So the region flag carries a third value meaning
             * "nothing here", and then nothing follows it. */
            int emptyA = (wA.lp == 0), emptyB = (wB.lp8 == 0);
            if (emptyA) bA = FDV_BITS_FLAG;      /* just the flag */
            if (emptyB) bB = FDV_BITS_FLAG;

            if (eA + lambda * bA <= eB + lambda * bB) {
                if (emptyA) w->fl[w->flp++] = 0;
                else {
                    w->fl[w->flp++] = 1;
                    cw_merge4(w, nA, wA.np, lA, wA.lp);
                }
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 8; ++j)
                        rec[(ry + i) * n + rx + j] = recA[i * 8 + j];
                e += eA; b += bA;
            } else {
                if (emptyB) w->fl[w->flp++] = 0;
                else {
                    w->fl[w->flp++] = 2;
                    memcpy(w->n8 + w->np8, nB, wB.np8); w->np8 += wB.np8;
                    memcpy(w->l8 + w->lp8, lB, wB.lp8); w->lp8 += wB.lp8;
                }
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 8; ++j)
                        rec[(ry + i) * n + rx + j] = recB[i * 8 + j];
                e += eB; b += bB;
            }
        }
    *ssd += e; *bits += b;
}

/* Instrumented entry point; the work is in fdv_code_residual_inner. */
static void code_residual(const uint8_t *cur, int cstride, int ox, int oy,
                          const uint8_t *pred, int n, int qp, uint8_t *rec,
                          fdv_cw *w, double *ssd, int *bits) {
    FDV_ZB(FDV_Z_RESID4);
    fdv_code_residual_inner(cur, cstride, ox, oy, pred, n, qp, rec, w, ssd, bits);
    FDV_ZE(FDV_Z_RESID4);
}

/* Inverse of code_residual. Bounded by `end`; sets *ok=0 on malformed input. */
static void decode_residual(fdv_cr *r, int *ok,
                            const uint8_t *pred, int n, int qp,
                            uint8_t *dst, int dstride, int ox, int oy) {
    if (n == 4) {
        decode_block4(r, ok, pred, n, 0, 0, qp, dst, dstride, ox, oy);
        return;
    }

    for (int ry = 0; ry < n; ry += 8)
        for (int rx = 0; rx < n; rx += 8) {
            int flag = fdv_rd_byte(r->b, &r->flp, r->flend, ok);
            if (flag == 0) {                     /* nothing coded: keep the prediction */
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 8; ++j)
                        dst[(oy + ry + i) * dstride + ox + rx + j] =
                            pred[(ry + i) * n + rx + j];
            } else if (flag == 1) {
                for (int sy = 0; sy < 8; sy += 4)
                    for (int sx = 0; sx < 8; sx += 4)
                        decode_block4(r, ok, pred, n, rx + sx, ry + sy, qp,
                                      dst, dstride, ox + rx + sx, oy + ry + sy);
            } else if (flag == 2) {
                int16_t lev8[64] = {0};
                fdv_coeff8_decode(r, ok, lev8);
                int32_t dc8[64];
                int16_t rr8[64];
                fdv_dequant8x8(lev8, dc8, qp);
                fdv_idct8x8(dc8, rr8);
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 8; ++j)
                        dst[(oy + ry + i) * dstride + ox + rx + j] =
                            clip255(pred[(ry + i) * n + rx + j] + rr8[i * 8 + j]);
            } else { *ok = 0; return; }          /* unknown region flag */
        }
}

/* ---- I-frame: code each plane with the intra image codec ----------------- */

/* One plane of an intra frame, coded into its own buffer. */
typedef struct {
    const uint8_t *src;
    int      pw, ph, qp;
    uint8_t *rec;
    uint8_t *blob;
    size_t   blen, bcap;
    int      rc;
} fdv_iplane;

static void *iplane_fn(void *v) {
    fdv_iplane *p = v;
    p->blen = fdv_image_encode(p->src, p->pw, p->ph, p->pw, p->qp,
                           p->blob, p->bcap, p->rec);
    p->rc = p->blen ? 0 : -1;
    fdv_profile_flush();               /* fold this thread's zone counters in */
    return NULL;
}

static size_t iframe_encode(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                            int w, int h, int qp, uint8_t *out, size_t cap,
                            uint8_t *ry, uint8_t *ru, uint8_t *rv) {
    int cw = w / 2, ch = h / 2;

    /* The three planes share nothing -- separate sources, separate
     * reconstructions, separate sub-streams -- so they encode concurrently and
     * are concatenated in order afterwards. Intra prediction is serial *within*
     * a plane (every block reads its reconstructed neighbours), so this is the
     * only parallelism the single-stream intra path has. Luma is two thirds of
     * the work, which caps the gain near 1.5x; the tiled container is where
     * real intra parallelism lives. */
    fdv_iplane pl[3] = {
        {y,  w,  h,  qp, ry, NULL, 0, (size_t)w  * h  * 2 + 4096, 0},
        {u, cw, ch, qp, ru, NULL, 0, (size_t)cw * ch * 2 + 4096, 0},
        {v, cw, ch, qp, rv, NULL, 0, (size_t)cw * ch * 2 + 4096, 0},
    };
    for (int i = 0; i < 3; ++i) {
        pl[i].blob = malloc(pl[i].bcap);
        if (!pl[i].blob) {
            for (int k = 0; k <= i; ++k) free(pl[k].blob);
            return 0;
        }
    }

    /* The stats sink and the encoder's per-frame counters are per-thread or
     * unlocked; fdv_image_encode only touches the latter, which are thread-local. */
    pthread_t tid[3];
    int spawned[3] = {0, 0, 0};
    for (int i = 1; i < 3; ++i)
        spawned[i] = (pthread_create(&tid[i], NULL, iplane_fn, &pl[i]) == 0);
    iplane_fn(&pl[0]);                          /* luma on this thread */
    for (int i = 1; i < 3; ++i) {
        if (spawned[i]) pthread_join(tid[i], NULL);
        else            iplane_fn(&pl[i]);      /* spawn failed: run inline */
    }

    size_t o = 0;
    int rc = 0;
    for (int i = 0; i < 3; ++i) {
        if (pl[i].rc != 0 || o + 4 + pl[i].blen > cap) { rc = -1; break; }
        o = fdv_put32(out, o, (uint32_t)pl[i].blen);
        memcpy(out + o, pl[i].blob, pl[i].blen);
        o += pl[i].blen;
    }
    for (int i = 0; i < 3; ++i) free(pl[i].blob);
    return rc == 0 ? o : 0;
}

static int iframe_decode(const uint8_t *in, size_t len, uint8_t *y, uint8_t *u,
                         uint8_t *v, int w, int h) {
    int cw = w / 2, ch = h / 2;
    struct { uint8_t *dst; int stride, pw, ph; } pl[3] = {
        {y, w, w, h}, {u, cw, cw, ch}, {v, cw, cw, ch}
    };
    size_t p = 0;
    for (int i = 0; i < 3; ++i) {
        if (p + 4 > len) return -1;
        uint32_t blob = fdv_get32(in, &p);
        if (p + blob > len) return -1;
        /* fdv_image_decode writes per the dims in the blob's own header; a corrupt
         * header could declare a larger plane and overrun pl[i].dst. Peek those
         * dims (w,h are the first 4 bytes) and reject any mismatch before the
         * decode writes anything. */
        if (blob < 4) return -1;
        size_t hp = p;
        int bw = (int)fdv_get16(in, &hp);
        int bh = (int)fdv_get16(in, &hp);
        if (bw != pl[i].pw || bh != pl[i].ph) return -1;
        int dw, dh;
        if (fdv_image_decode(in + p, blob, pl[i].dst, pl[i].stride, &dw, &dh) != 0)
            return -1;
        p += blob;
    }
    return 0;
}

/* ---- P-frame ------------------------------------------------------------- */

/* Code one chroma block's residual (INTER) or just its prediction (SKIP) into
 * the reconstruction, appending coefficient symbols when not skipping. */
static void chroma_block_inner(const uint8_t *cur, uint8_t *rec, int cw,
                         const fdv_plane *ref, int cbx, int cby,
                         int cmvx, int cmvy, int qp, int skip, fdv_cw *w) {
    uint8_t pred[CB * CB];
    fdv_mc_chroma(ref, cbx, cby, CB, CB, cmvx, cmvy, pred, CB);

    if (skip) {
        for (int i = 0; i < CB; ++i)
            for (int j = 0; j < CB; ++j)
                rec[(cby + i) * cw + cbx + j] = pred[i * CB + j];
        return;
    }

    /* Quantize all four sub-blocks first, because whether *any* of them has a
     * coefficient decides whether anything at all needs sending. */
    int16_t lv[4][16];
    int last[4];
    int any = 0, bi = 0;
    for (int sy = 0; sy < CB; sy += 4)
        for (int sx = 0; sx < CB; sx += 4, ++bi) {
            int16_t res[16];
            int32_t coeff[16], dcoeff[16];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    res[i * 4 + j] = (int16_t)(cur[(cby + sy + i) * cw + cbx + sx + j] -
                                               pred[(sy + i) * CB + sx + j]);
            fdv_fdct4x4(res, coeff);
            fdv_quant4x4(coeff, lv[bi], qp);
            int16_t rres[16];
            fdv_dequant4x4(lv[bi], dcoeff, qp);
            fdv_idct4x4(dcoeff, rres);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    rec[(cby + sy + i) * cw + cbx + sx + j] =
                        clip255(pred[(sy + i) * CB + sx + j] + rres[i * 4 + j]);
            last[bi] = -1;
            for (int k = 0; k < 16; ++k) if (lv[bi][fdv_zz4[k]] != 0) last[bi] = k;
            if (last[bi] >= 0) any = 1;
        }

    /* One flag for the whole chroma block. Chroma quantizes to nothing long
     * before luma does, so on anything but the finest quantizers this is the
     * usual answer, and it replaces four end-of-block counts with one symbol. */
    if (!any) { w->fl[w->flp++] = 0; return; }
    w->fl[w->flp++] = 1;
    for (int b = 0; b < 4; ++b) {
        cw_count(w, (unsigned)(last[b] + 1));
        for (int k = 0; k <= last[b]; ++k)
            cw_level(w, fdv_zz_enc(lv[b][fdv_zz4[k]]));
    }
}

/* Chroma coefficients go to their own streams; the flag says so for the whole
 * call, including the 4x4 coders it reaches, and is cleared on the way out so
 * the next luma block is unaffected. */
static void chroma_block(const uint8_t *cur, uint8_t *rec, int cw,
                         const fdv_plane *ref, int cbx, int cby,
                         int cmvx, int cmvy, int qp, int skip, fdv_cw *w) {
    w->chroma = 1;
    chroma_block_inner(cur, rec, cw, ref, cbx, cby, cmvx, cmvy, qp, skip, w);
    w->chroma = 0;
}

/* A table can code a frame only if it has a nonzero frequency for every symbol
 * that frame uses. */
static int tab_covers(const uint16_t freq[256], const uint32_t cnt[256]) {
    for (int i = 0; i < 256; ++i) if (cnt[i] && !freq[i]) return 0;
    return 1;
}

/* The cached form of a table: the transmitted one with every symbol given at
 * least one slot out of 4096.
 *
 * Without this a single symbol the previous frame happened not to use
 * disqualifies the whole table -- measured, that ruled it out on 99% of frames
 * and table reuse never fired at all. The reserved slots are paid for out of
 * the largest bin, where the relative loss is smallest; the symbols actually
 * used give up a few hundredths of a bit each, against a table that costs 36%
 * of a cheap frame. Encoder and decoder derive this from the same transmitted
 * table by the same rule, so they cannot drift apart. */
/* Fold one frame's symbol counts into a stream's history. Halving past a bound
 * is what lets the prior follow a scene change rather than averaging over the
 * whole stream, and it keeps the model total small enough that the range coder
 * does not lose precision to its range/total division. Encoder and decoder run
 * this on the same counts in the same order, so they cannot drift apart. */
static void hist_add(uint32_t h[256], const uint32_t c[256]) {
    uint64_t s = 0;
    for (int i = 0; i < 256; ++i) { h[i] += c[i]; s += h[i]; }
    if (s > FDV_AD_HIST) for (int i = 0; i < 256; ++i) h[i] >>= 1;
}

static void tab_store(uint16_t dst[256], const uint16_t src[256]) {
    int add = 0, big = 0;
    for (int i = 0; i < 256; ++i) {
        if (!src[i]) ++add;
        if (src[i] > src[big]) big = i;
    }
    /* Nothing to spare: keep it exact and let it simply not be a candidate. */
    if (src[big] <= (uint16_t)add) { memcpy(dst, src, 256 * sizeof *dst); return; }
    for (int i = 0; i < 256; ++i) dst[i] = src[i] ? src[i] : 1;
    dst[big] = (uint16_t)(dst[big] - add);
}

/* One macroblock's motion, kept so later macroblocks can merge with it. */
typedef struct { int16_t x, y; uint8_t ref, inter; } fdv_mbmv;

/* HEVC's merge candidates, cut to what this codec can use.
 *
 * A SKIP macroblock used to have exactly one motion vector available to it --
 * the running predictor, which is the last vector coded in this row. That is a
 * good guess on uniform motion and a bad one at an object boundary, where the
 * macroblock above is right and the one to the left is wrong, and SKIP had no
 * way to say so: it either took the wrong vector or stopped being SKIP and paid
 * for a coded one.
 *
 * So SKIP now names which vector it is inheriting. The list is the running
 * predictor first -- index 0 reproduces exactly what SKIP did before, which is
 * what keeps this from being able to regress -- then the macroblock above and
 * the one above-right, deduplicated. Both sides build it from macroblocks
 * already coded, so only the index is transmitted, and only when there is more
 * than one candidate to choose between.
 *
 * A candidate carries its reference index too, which is how a SKIP can inherit
 * the further reference; it could only ever use the nearest one before.
 *
 * The other half of HEVC's merge -- an inherited vector *with* a coded
 * residual, filling the gap between SKIP and INTER16 -- was built and measured
 * at +0.18% mean, and reverted. It has little to remove: a vector delta against
 * a predictor this good is already cheap (a better predictor was measured at
 * -0.3% here), so the mode saves a handful of bits on the blocks that choose it
 * and costs a fifth symbol in the macroblock-mode alphabet on every block that
 * does not. SKIP is where the merge gain is in this codec. */
#ifndef FDV_MERGE_MAX
#define FDV_MERGE_MAX 3
#endif
static int merge_cands(const fdv_mbmv *mv, int mmw, int mbx, int mby,
                       int pmvx, int pmvy, fdv_mbmv *out) {
    int n = 0;
    out[n].x = (int16_t)pmvx; out[n].y = (int16_t)pmvy;
    out[n].ref = 0; out[n].inter = 1; ++n;

    const int src[2][2] = { { mbx, mby - 1 }, { mbx + 1, mby - 1 } };
    for (int k = 0; k < 2 && n < FDV_MERGE_MAX; ++k) {
        int cx = src[k][0], cy = src[k][1];
        if (cx < 0 || cy < 0 || cx >= mmw) continue;
        const fdv_mbmv *m = &mv[(size_t)cy * mmw + cx];
        if (!m->inter) continue;
        int dup = 0;
        for (int j = 0; j < n; ++j)
            if (out[j].x == m->x && out[j].y == m->y && out[j].ref == m->ref) dup = 1;
        if (!dup) out[n++] = *m;
    }
    return n;
}

/* Squared error of a 16x16 prediction against the source. This runs for every
 * merge candidate of every macroblock in the frame -- it is the one thing the
 * early-out cannot skip -- so it is worth doing 16 pixels at a time. */
static int32_t mb_ssd(const uint8_t *cy, int w, int bx, int by, const uint8_t *pred) {
    int32_t ssd = 0;
#if defined(__ARM_NEON)
    uint32x4_t acc = vdupq_n_u32(0);
    for (int i = 0; i < MB; ++i) {
        uint8x16_t a = vld1q_u8(&cy[(by + i) * w + bx]);
        uint8x16_t b = vld1q_u8(&pred[i * MB]);
        uint8x16_t d = vabdq_u8(a, b);        /* |a-b| fits in u8 */
        acc = vpadalq_u16(acc, vmull_u8(vget_low_u8(d),  vget_low_u8(d)));
        acc = vpadalq_u16(acc, vmull_u8(vget_high_u8(d), vget_high_u8(d)));
    }
    ssd = (int32_t)vaddvq_u32(acc);
#else
    for (int i = 0; i < MB; ++i)
        for (int j = 0; j < MB; ++j) {
            int d = cy[(by + i) * w + bx + j] - pred[i * MB + j];
            ssd += d * d;
        }
#endif
    return ssd;
}

static size_t pframe_encode(const uint8_t *cy, const uint8_t *cu, const uint8_t *cv,
                            int w, int h, const fdv_frame *const refs[], int navail, int qp,
                            uint8_t *out, size_t cap,
                            uint8_t *ry, uint8_t *ru, uint8_t *rv, int force_skip,
                            fdv_tabcache *tc) {
    int cw = w / 2;
    /* SKIP / 8x8 / INTRA use the nearest reference; 16x16 INTER may pick either. */
    const fdv_plane *rpY = &refs[0]->planes[0], *rpU = &refs[0]->planes[1], *rpV = &refs[0]->planes[2];

    size_t sym_cap = (size_t)(w / MB) * (h / MB) *
                     (1 + 4 * 2 * 5 + MB * MB * 5 + 2 * CB * CB * 5 + 256) + 64;
    /* One buffer per coded stream: the structure symbols, and the coefficient
     * streams split by transform size and by plane. */
    uint8_t *syms = malloc(sym_cap);
    uint8_t *sctx = malloc(sym_cap);      /* which model each structure byte uses */
    uint8_t *cn   = malloc(sym_cap);
    uint8_t *cl   = malloc(sym_cap);
    uint8_t *cn8  = malloc(sym_cap);
    uint8_t *cl8  = malloc(sym_cap);
    uint8_t *cfl  = malloc(sym_cap);
    uint8_t *cnc  = malloc(sym_cap);
    uint8_t *clc  = malloc(sym_cap);
    uint8_t *mmap = calloc((size_t)(w / MB) * (h / MB), 1);
    fdv_mbmv *mvmap = calloc((size_t)(w / MB) * (h / MB), sizeof(*mvmap));
    if (!syms || !sctx || !cn || !cl || !cn8 || !cl8 || !cfl || !cnc || !clc ||
        !mmap || !mvmap) {
        free(syms); free(sctx); free(cn); free(cl); free(cn8); free(cl8); free(cfl); free(cnc); free(clc);
        free(mmap); free(mvmap); return 0;
    }
    size_t sp = 0;
    int mmw = w / MB;
    fdv_cw W = { .n = cn, .l = cl, .n8 = cn8, .l8 = cl8, .fl = cfl,
                 .nc = cnc, .lc = clc };
    double lambda = FDV_LAMBDA0 * pow(2.0, (qp - 12) / 3.0);

    fdv_mb_tally[0] = fdv_mb_tally[1] = fdv_mb_tally[2] = fdv_mb_tally[3] = 0;

    for (int by = 0; by < h; by += MB) {
        int pmvx = 0, pmvy = 0;
        for (int bx = 0; bx < w; bx += MB) {
            /* A held frame -- the picture has not changed, so every macroblock
             * is SKIP with a zero vector and no residual. Writing that directly
             * instead of letting mode decision rediscover it is the whole
             * point: below about QP 16 the RD comparison stops choosing SKIP
             * even for a byte-identical frame, because lambda gets small enough
             * that coding the reference's own quantization noise scores better.
             * A repeat then costs 26 ms instead of 0.7, and a recorder holding
             * slots to keep time falls further behind for doing so. */
            if (force_skip) {
                uint8_t pf[MB * MB];
                int fcx = bx / 2, fcy = by / 2;
                fdv_mc_luma(rpY, bx, by, MB, MB, 0, 0, pf, MB);
                sctx[sp] = (uint8_t)mb_mode_ctx(mmap, mmw, bx / MB, by / MB);
                mmap[(by / MB) * mmw + bx / MB] = 0;
                syms[sp++] = 0;
                /* One candidate (the zero predictor), so no index is sent. */
                mvmap[(by / MB) * mmw + bx / MB] = (fdv_mbmv){0, 0, 0, 1};
                for (int i = 0; i < MB; ++i)
                    for (int j = 0; j < MB; ++j)
                        ry[(by + i) * w + bx + j] = pf[i * MB + j];
                chroma_block(NULL, ru, cw, rpU, fcx, fcy, 0, 0, qp, 1, &W);
                chroma_block(NULL, rv, cw, rpV, fcx, fcy, 0, 0, qp, 1, &W);
                fdv_mb_tally[0]++;
                continue;                       /* pmv stays (0,0) throughout */
            }

            /* Current-frame reconstructed neighbors for the intra candidate. */
            uint8_t ntop[MB], nleft[MB], ntl;
            int iht, ihl;
            gather_nb(ry, w, bx, by, MB, ntop, nleft, &ntl, &iht, &ihl);

            /* --- SKIP candidate: an inherited MV, no residual. -----------
             * Evaluated first because it is by far the cheapest to test (one
             * motion compensation and a sum of squares per candidate) and, on
             * real content, by far the most often chosen. */
            fdv_mbmv cand[FDV_MERGE_MAX];
            int ncand = merge_cands(mvmap, mmw, bx / MB, by / MB, pmvx, pmvy, cand);
            int midx_bits = ncand > 1 ? FDV_BITS_MODE : 0;
            uint8_t pred_s[MB * MB];
            int best_mi = 0;
            int32_t Ds_i = 0;
            for (int mi = 0; mi < ncand; ++mi) {
                uint8_t tp[MB * MB];
                fdv_mc_luma(&refs[cand[mi].ref]->planes[0], bx, by, MB, MB,
                            cand[mi].x, cand[mi].y, tp, MB);
                int32_t e = mb_ssd(cy, w, bx, by, tp);
                if (mi == 0 || e < Ds_i) {
                    Ds_i = e; best_mi = mi;
                    memcpy(pred_s, tp, sizeof pred_s);
                }
            }
            int smvx = cand[best_mi].x, smvy = cand[best_mi].y;
            int sref = cand[best_mi].ref;
            double Ds = (double)Ds_i;
            double Js = Ds + lambda * (FDV_BITS_MODE + midx_bits);

            /* Every other mode costs at least MIN_CODED_BITS of rate before any
             * distortion. So if SKIP's cost already sits at or below
             * lambda*MIN_CODED_BITS, no other mode can beat it and searching
             * them is provably wasted work.
             *
             * This is an exact test, not a heuristic -- the decision it skips is
             * the decision it would have made. Ties go to SKIP because the
             * comparison below is a strict <.
             *
             * Each mode's floor is the syntax it must emit even when every
             * coefficient quantizes to zero: a 16x16 residual is four 8x8
             * regions, each of which still costs its transform-size flag; an
             * 8x8 residual is one such region; and the cheapest motion vector
             * difference is the two one-bit values fdv_bits_val gives for zero.
             *
             * These are derived from the cost constants rather than written
             * out, because the exactness depends on them agreeing: a floor left
             * behind at the old eight-bits-a-byte scale would prune candidates
             * that can in fact win. */
            enum {
                FLOOR_RESID16 = 4 * FDV_BITS_FLAG,   /* four empty 8x8 regions */
                FLOOR_RESID8  = FDV_BITS_FLAG,       /* one                    */
                FLOOR_MVD     = 2,                   /* fdv_bits_val(0) twice  */
                FLOOR_INTRA   = 2 * FDV_BITS_MODE + FLOOR_RESID16,
                FLOOR_INTER16 = 2 * FDV_BITS_MODE + FLOOR_MVD + FLOOR_RESID16,
                FLOOR_INTER8  = FDV_BITS_MODE + 4 * FLOOR_MVD + 4 * FLOOR_RESID8,
            };
            enum { MIN_CODED_BITS = FLOOR_INTRA };   /* the cheapest of the three */
            _Static_assert(FLOOR_INTRA <= FLOOR_INTER16 && FLOOR_INTRA <= FLOOR_INTER8,
                           "MIN_CODED_BITS must be the smallest of the three floors");
            double Jbest = Js;
            int skip_wins = (Js <= lambda * MIN_CODED_BITS);

            /* --- INTER 16x16 candidate: best over the available references. --- */
            int mvx = 0, mvy = 0, best_ref = 0;
            uint8_t rec_i[MB * MB];
            uint8_t isS[1 + 2 * 5], isCn[64], isCl[MB * MB * 5 + 64];
            size_t is_slen = 0;
            uint8_t isCn8[64], isCl8[MB * MB * 5 + 64], isFl[64];
            fdv_cw isw = {0};
            double Ji = 1e30;
            for (int r = 0; r < navail && !skip_wins; ++r) {
                /* A further reference is still an INTER16 block, so it carries
                 * the same floor; if what we have already beats that, searching
                 * it cannot change the outcome. */
                if (r > 0 && Jbest <= lambda * FLOOR_INTER16) break;
                const fdv_plane *rp = &refs[r]->planes[0];
                int rmx, rmy;
                fdv_me_search(cy, w, rp, bx, by, MB, MB, FDV_ME_RANGE, pmvx, pmvy, &rmx, &rmy);
                uint8_t pr[MB * MB], rr[MB * MB];
                fdv_mc_luma(rp, bx, by, MB, MB, rmx, rmy, pr, MB);
                uint8_t tS[1 + 2 * 5], tCn[64], tCl[MB * MB * 5 + 64];
                uint8_t tCn8[64], tCl8[MB * MB * 5 + 64], tFl[64];
                fdv_cw tw = { .n = tCn, .l = tCl, .n8 = tCn8, .l8 = tCl8, .fl = tFl };
                size_t tsl = 0;
                tS[tsl++] = (uint8_t)r;                 /* reference index */
                uint32_t zx = fdv_zz_enc(rmx - pmvx), zy = fdv_zz_enc(rmy - pmvy);
                tsl = fdv_leb_put(tS, tsl, zx);
                tsl = fdv_leb_put(tS, tsl, zy);
                double dd = 0.0;
                int bb = FDV_BITS_MODE + FDV_BITS_MODE
                       + fdv_bits_val(zx) + fdv_bits_val(zy);  /* mode + refidx + mvd */
                code_residual(cy, w, bx, by, pr, MB, qp, rr, &tw, &dd, &bb);
                double J = dd + lambda * bb;
                if (J < Jbest) Jbest = J;
                if (J < Ji) {
                    Ji = J; best_ref = r; mvx = rmx; mvy = rmy;
                    memcpy(rec_i, rr, sizeof(rec_i));
                    memcpy(isS, tS, tsl); is_slen = tsl;
                    memcpy(isCn,  tCn,  tw.np);  memcpy(isCl,  tCl,  tw.lp);
                    memcpy(isCn8, tCn8, tw.np8); memcpy(isCl8, tCl8, tw.lp8);
                    memcpy(isFl, tFl, tw.flp);
                    isw = tw;
                }
            }

            /* --- INTRA candidate: best of DC/V/H from current-frame neighbors. --- */
            double Jintra = 1e30; int best_sub = FDV_INTRA_NN_DC;
            uint8_t rec_in[MB * MB], inCn[64], inCl[MB * MB * 5 + 64];
            uint8_t inCn8[64], inCl8[MB * MB * 5 + 64], inFl[64];
            fdv_cw inw = {0};
            int try_intra  = !skip_wins && (Jbest > lambda * FLOOR_INTRA);
            for (int sub = 0; sub < FDV_INTRA_NN_NMODES && try_intra; ++sub) {
                /* Luma and chroma share one sub-mode symbol, so a candidate is
                 * only usable where both planes can predict it. */
                if (!fdv_intra_nn_mode_ok(sub, MB, iht, ihl)) continue;
                uint8_t predI[MB * MB], trec[MB * MB], tCn[64], tCl[MB * MB * 5 + 64];
                uint8_t tCn8[64], tCl8[MB * MB * 5 + 64], tFl[64];
                fdv_cw tw = { .n = tCn, .l = tCl, .n8 = tCn8, .l8 = tCl8, .fl = tFl };
                fdv_intra_nxn(sub, ntop, nleft, ntl, MB, iht, ihl, predI);
                double Dn = 0.0;
                int bn = FDV_BITS_MODE + FDV_BITS_MODE;   /* mode + submode */
                code_residual(cy, w, bx, by, predI, MB, qp, trec, &tw, &Dn, &bn);
                double J = Dn + lambda * bn;
                if (J < Jbest) Jbest = J;
                if (J < Jintra) {
                    Jintra = J; best_sub = sub;
                    memcpy(rec_in, trec, sizeof(rec_in));
                    memcpy(inCn,  tCn,  tw.np);  memcpy(inCl,  tCl,  tw.lp);
                    memcpy(inCn8, tCn8, tw.np8); memcpy(inCl8, tCl8, tw.lp8);
                    memcpy(inFl, tFl, tw.flp);
                    inw = tw;
                }
            }

            /* --- 8x8 inter candidate: four independently-searched quadrants. --- */
            uint8_t p8S[4 * 2 * 5], p8Cn[64], p8Cl[4 * (8 * 8 * 5 + 8)], rec8[MB * MB];
            uint8_t p8Cn8[64], p8Cl8[4 * (8 * 8 * 5 + 8)], p8Fl[64];
            fdv_cw w8 = { .n = p8Cn, .l = p8Cl, .n8 = p8Cn8, .l8 = p8Cl8, .fl = p8Fl };
            size_t p8_slen = 0; double D8 = 0.0; int b8 = FDV_BITS_MODE;
            int qmv[4][2];
            int try_inter8 = !skip_wins && (Jbest > lambda * FLOOR_INTER8);
            if (try_inter8) {
                int qpx = pmvx, qpy = pmvy;
                for (int q = 0; q < 4; ++q) {
                    int qx = (q & 1) * 8, qy = (q >> 1) * 8, mx, my;
                    fdv_me_search(cy, w, rpY, bx + qx, by + qy, 8, 8, FDV_ME_RANGE, qpx, qpy, &mx, &my);
                    qmv[q][0] = mx; qmv[q][1] = my;
                    uint32_t zdx = fdv_zz_enc(mx - qpx), zdy = fdv_zz_enc(my - qpy);
                    p8_slen = fdv_leb_put(p8S, p8_slen, zdx);
                    p8_slen = fdv_leb_put(p8S, p8_slen, zdy);
                    b8 += fdv_bits_val(zdx) + fdv_bits_val(zdy);
                    qpx = mx; qpy = my;
                    uint8_t pred8[64], r8[64];
                    fdv_mc_luma(rpY, bx + qx, by + qy, 8, 8, mx, my, pred8, 8);
                    code_residual(cy, w, bx + qx, by + qy, pred8, 8, qp, r8, &w8, &D8, &b8);
                    for (int i = 0; i < 8; ++i)
                        for (int j = 0; j < 8; ++j)
                            rec8[(qy + i) * MB + qx + j] = r8[i * 8 + j];
                }
            }
            double J8 = try_inter8 ? D8 + lambda * b8 : 1e30;

            /* --- Commit the cheapest of SKIP / INTER / INTRA / INTER8x8. --- */
            int cbx = bx / 2, cby = by / 2;
            double Jmin = Js; int chosen = 0;
            if (Ji     < Jmin) { Jmin = Ji;     chosen = 1; }
            if (Jintra < Jmin) { Jmin = Jintra; chosen = 2; }
            if (J8     < Jmin) { Jmin = J8;     chosen = 3; }

            fdv_mb_tally[chosen]++;
            FDV_TRACE_LOG("mb", "(%3d,%3d) mode=%s J=%.1f mv=(%d,%d) ref=%d",
                          bx, by,
                          chosen == 0 ? "SKIP" : chosen == 1 ? "INTER16" :
                          chosen == 2 ? "INTRA" : "INTER8",
                          Jmin, mvx, mvy, best_ref);

            sctx[sp] = (uint8_t)mb_mode_ctx(mmap, mmw, bx / MB, by / MB);
            mmap[(by / MB) * mmw + bx / MB] = (uint8_t)chosen;
            if (chosen == 0) {                              /* SKIP */
                syms[sp++] = 0;
                if (ncand > 1) {                            /* which vector */
                    sctx[sp] = PS_MERGE;
                    syms[sp++] = (uint8_t)best_mi;
                }
                for (int i = 0; i < MB; ++i)
                    for (int j = 0; j < MB; ++j)
                        ry[(by + i) * w + bx + j] = pred_s[i * MB + j];
                const fdv_plane *sU = &refs[sref]->planes[1], *sV = &refs[sref]->planes[2];
                chroma_block(cu, ru, cw, sU, cbx, cby, smvx / 2, smvy / 2, qp, 1, &W);
                chroma_block(cv, rv, cw, sV, cbx, cby, smvx / 2, smvy / 2, qp, 1, &W);
                mvmap[(by / MB) * mmw + bx / MB] =
                    (fdv_mbmv){(int16_t)smvx, (int16_t)smvy, (uint8_t)sref, 1};
                pmvx = smvx; pmvy = smvy;
            } else if (chosen == 1) {                       /* INTER 16x16 */
                syms[sp++] = 1;
                {   /* [reference index][zigzag LEB dx][zigzag LEB dy] */
                    size_t q = 0; int part = 0;
                    sctx[sp] = PS_REF; q = 1;
                    while (q < is_slen) {
                        sctx[sp + q] = (uint8_t)(part ? PS_MVY : PS_MVX);
                        if (!(isS[q] & 0x80)) ++part;
                        ++q;
                    }
                }
                memcpy(syms + sp, isS, is_slen); sp += is_slen;
                memcpy(W.n  + W.np,  isCn,  isw.np);  W.np  += isw.np;
                memcpy(W.l  + W.lp,  isCl,  isw.lp);  W.lp  += isw.lp;
                memcpy(W.n8 + W.np8, isCn8, isw.np8); W.np8 += isw.np8;
                memcpy(W.l8 + W.lp8, isCl8, isw.lp8); W.lp8 += isw.lp8;
                memcpy(W.fl + W.flp, isFl,  isw.flp); W.flp += isw.flp;
                for (int i = 0; i < MB; ++i)
                    for (int j = 0; j < MB; ++j)
                        ry[(by + i) * w + bx + j] = rec_i[i * MB + j];
                pmvx = mvx; pmvy = mvy;
                mvmap[(by / MB) * mmw + bx / MB] =
                    (fdv_mbmv){(int16_t)mvx, (int16_t)mvy, (uint8_t)best_ref, 1};
                const fdv_plane *cU = &refs[best_ref]->planes[1], *cV = &refs[best_ref]->planes[2];
                chroma_block(cu, ru, cw, cU, cbx, cby, mvx / 2, mvy / 2, qp, 0, &W);
                chroma_block(cv, rv, cw, cV, cbx, cby, mvx / 2, mvy / 2, qp, 0, &W);
            } else if (chosen == 2) {                       /* INTRA */
                syms[sp++] = 2;
                sctx[sp] = PS_SUB;
                syms[sp++] = (uint8_t)best_sub;
                memcpy(W.n  + W.np,  inCn,  inw.np);  W.np  += inw.np;
                memcpy(W.l  + W.lp,  inCl,  inw.lp);  W.lp  += inw.lp;
                memcpy(W.n8 + W.np8, inCn8, inw.np8); W.np8 += inw.np8;
                memcpy(W.l8 + W.lp8, inCl8, inw.lp8); W.lp8 += inw.lp8;
                memcpy(W.fl + W.flp, inFl,  inw.flp); W.flp += inw.flp;
                for (int i = 0; i < MB; ++i)
                    for (int j = 0; j < MB; ++j)
                        ry[(by + i) * w + bx + j] = rec_in[i * MB + j];
                struct { const uint8_t *src; uint8_t *rec; } pl2[2] = {{cu, ru}, {cv, rv}};
                for (int pl = 0; pl < 2; ++pl) {
                    uint8_t ct[CB], cleft[CB], ctl; int cht, chl;
                    gather_nb(pl2[pl].rec, cw, cbx, cby, CB, ct, cleft, &ctl, &cht, &chl);
                    uint8_t cpred[CB * CB], crec[CB * CB];
                    fdv_intra_nxn(best_sub, ct, cleft, ctl, CB, cht, chl, cpred);
                    double dd = 0.0; int bb = 0;
                    W.chroma = 1;
                    code_residual(pl2[pl].src, cw, cbx, cby, cpred, CB, qp, crec, &W, &dd, &bb);
                    W.chroma = 0;
                    for (int i = 0; i < CB; ++i)
                        for (int j = 0; j < CB; ++j)
                            pl2[pl].rec[(cby + i) * cw + cbx + j] = crec[i * CB + j];
                }
                /* No motion, so nothing here for a later macroblock to merge
                 * with; the predictor is unchanged too. */
                mvmap[(by / MB) * mmw + bx / MB] = (fdv_mbmv){0, 0, 0, 0};
            } else {                                        /* INTER 8x8 */
                syms[sp++] = 3;
                {   /* four (dx, dy) pairs, LEB coded */
                    size_t q = 0; int part = 0;
                    while (q < p8_slen) {
                        sctx[sp + q] = (uint8_t)((part & 1) ? PS_MVY : PS_MVX);
                        if (!(p8S[q] & 0x80)) ++part;
                        ++q;
                    }
                }
                memcpy(syms + sp, p8S, p8_slen); sp += p8_slen;
                memcpy(W.n  + W.np,  p8Cn,  w8.np);  W.np  += w8.np;
                memcpy(W.l  + W.lp,  p8Cl,  w8.lp);  W.lp  += w8.lp;
                memcpy(W.n8 + W.np8, p8Cn8, w8.np8); W.np8 += w8.np8;
                memcpy(W.l8 + W.lp8, p8Cl8, w8.lp8); W.lp8 += w8.lp8;
                memcpy(W.fl + W.flp, p8Fl,  w8.flp); W.flp += w8.flp;
                for (int i = 0; i < MB; ++i)
                    for (int j = 0; j < MB; ++j)
                        ry[(by + i) * w + bx + j] = rec8[i * MB + j];
                pmvx = qmv[3][0]; pmvy = qmv[3][1];
                mvmap[(by / MB) * mmw + bx / MB] =
                    (fdv_mbmv){(int16_t)qmv[3][0], (int16_t)qmv[3][1], 0, 1};
                struct { const uint8_t *src; uint8_t *rec; const fdv_plane *ref; } pl2[2] =
                    {{cu, ru, rpU}, {cv, rv, rpV}};
                for (int pl = 0; pl < 2; ++pl)
                    for (int q = 0; q < 4; ++q) {
                        int qcx = (q & 1) * 4, qcy = (q >> 1) * 4;
                        uint8_t cpred[16], crec[16];
                        fdv_mc_chroma(pl2[pl].ref, cbx + qcx, cby + qcy, 4, 4,
                                  qmv[q][0] / 2, qmv[q][1] / 2, cpred, 4);
                        double dd = 0.0; int bb = 0;
                        W.chroma = 1;
                        code_residual(pl2[pl].src, cw, cbx + qcx, cby + qcy, cpred, 4, qp,
                                      crec, &W, &dd, &bb);
                        W.chroma = 0;
                        for (int i = 0; i < 4; ++i)
                            for (int j = 0; j < 4; ++j)
                                pl2[pl].rec[(cby + qcy + i) * cw + cbx + qcx + j] = crec[i * 4 + j];
                    }
            }
        }
    }

    /* Two entropy strategies, pick the smaller (a 1-byte mode flag): one shared
     * model over S++C, or separate models per stream. On small frames the second
     * table costs more than the modeling gain, so one-model wins; on large frames
     * the split wins. Adaptive selection never regresses. */

    /* Partition the structure stream into its models, raster order preserved
     * inside each. The context comes from neighbouring macroblocks, which the
     * decoder has already decoded, so nothing extra is transmitted. */
    size_t srun[PS_NSTRUCT];
    {   uint8_t *part = malloc(sp ? sp : 1);
        if (!part) { free(syms); free(sctx); free(cn); free(cl);
                     free(cn8); free(cl8); free(cfl); free(cnc); free(clc);
                     free(mmap); free(mvmap); return 0; }
        size_t a = 0;
        for (int c = 0; c < PS_NSTRUCT; ++c) {
            size_t start = a;
            for (size_t i = 0; i < sp; ++i) if (sctx[i] == c) part[a++] = syms[i];
            srun[c] = a - start;
        }
        memcpy(syms, part, sp);
        free(part);
    }

    /* Ten streams: one per macroblock-mode model, one for the rest of the
     * structure, and one each for the four coefficient streams -- counts and
     * levels, kept apart by transform size. */
    #define NS (PS_NSTRUCT + 7)
    const uint8_t *sv[NS];
    size_t sn[NS];
    {   size_t off = 0;
        for (int c = 0; c < PS_NSTRUCT; ++c) { sv[c] = syms + off; sn[c] = srun[c]; off += srun[c]; }
        sv[NS - 7] = cn;  sn[NS - 7] = W.np;
        sv[NS - 6] = cl;  sn[NS - 6] = W.lp;
        sv[NS - 5] = cn8; sn[NS - 5] = W.np8;
        sv[NS - 4] = cl8; sn[NS - 4] = W.lp8;
        sv[NS - 3] = cfl; sn[NS - 3] = W.flp;
        sv[NS - 2] = cnc; sn[NS - 2] = W.ncp;
        sv[NS - 1] = clc; sn[NS - 1] = W.lcp;
    }
    size_t total = sp;
    for (int k = PS_NSTRUCT; k < NS; ++k) total += sn[k];

    FDV_ZB(FDV_Z_ENTROPY);
    uint8_t *all = malloc(total ? total : 1);
    if (!all) { FDV_ZE(FDV_Z_ENTROPY); free(syms); free(sctx); free(cn); free(cl);
                free(cn8); free(cl8); free(cfl); free(cnc); free(clc);
                free(mmap); free(mvmap); return 0; }
    {   size_t o2 = 0;
        for (int k = 0; k < NS; ++k) { memcpy(all + o2, sv[k], sn[k]); o2 += sn[k]; }
    }

    uint32_t c1[256] = {0};
    for (size_t i = 0; i < total; ++i) c1[all[i]]++;
    fdv_rans_sym e1[256]; fdv_rans_table d1;
    fdv_rans_build_tables(c1, e1, &d1);
    uint8_t *p1 = malloc(total * 2 + 1024);

    uint32_t ck[NS][256];
    fdv_rans_sym enc[NS][256];
    fdv_rans_table dec[NS];
    uint8_t *pay[NS] = {0};
    size_t len[NS] = {0};
    int oom = (p1 == NULL);
    for (int k = 0; k < NS; ++k) {
        memset(ck[k], 0, sizeof ck[k]);
        for (size_t i = 0; i < sn[k]; ++i) ck[k][sv[k][i]]++;
        fdv_rans_build_tables(ck[k], enc[k], &dec[k]);
        pay[k] = malloc(sn[k] * 2 + 1024);
        if (!pay[k]) oom = 1;
    }
    if (oom) {
        FDV_ZE(FDV_Z_ENTROPY);
        free(syms); free(sctx); free(cn); free(cl); free(cn8); free(cl8); free(cfl); free(cnc); free(clc); free(mmap); free(mvmap); free(all); free(p1);
        for (int k = 0; k < NS; ++k) free(pay[k]);
        return 0;
    }
    size_t l1 = fdv_rans_encode(all, total, e1, p1, total * 2 + 1024);
    for (int k = 0; k < NS; ++k)
        len[k] = fdv_rans_encode(sv[k], sn[k], enc[k], pay[k], sn[k] * 2 + 1024);

    /* One symbol count per stream, varint-coded. Seven fixed 32-bit fields is
     * 28 bytes a frame, which is nothing on a busy frame and several percent of
     * a still one -- and still frames are where this codec is furthest behind. */
    size_t hdr = 0;
    for (int k = 0; k < NS; ++k) hdr += fdv_leb_size((uint32_t)sn[k]);
    size_t size_one = hdr + fdv_rans_freqs_size(d1.freq) + 4 + l1;
    size_t size_two = hdr;
    for (int k = 0; k < NS; ++k) size_two += fdv_rans_freqs_size(dec[k].freq) + 4 + len[k];

    /* And the same two coded with the tables the previous frame already sent.
     * A cached table is only a candidate if it can represent every symbol this
     * frame uses; otherwise there is nothing to compare. */
    size_t l1r = 0, lenr[NS] = {0};
    size_t size_one_r = (size_t)-1, size_two_r = (size_t)-1;
    uint8_t *p1r = NULL, *payr[NS] = {0};
    if (tc && tc->have) {
        fdv_rans_sym er[256];
        if (tab_covers(tc->f1, c1)) {
            p1r = malloc(total * 2 + 1024);
            if (p1r) {
                fdv_rans_enc_from_freq(tc->f1, er);
                l1r = fdv_rans_encode(all, total, er, p1r, total * 2 + 1024);
                if (l1r) size_one_r = hdr + 4 + l1r;
            }
        }
        int cover = 1;
        for (int k = 0; k < NS; ++k) if (!tab_covers(tc->fs[k], ck[k])) cover = 0;
        if (cover) {
            int ok2 = 1;
            for (int k = 0; k < NS; ++k) {
                payr[k] = malloc(sn[k] * 2 + 1024);
                if (!payr[k]) { ok2 = 0; break; }
                fdv_rans_enc_from_freq(tc->fs[k], er);
                lenr[k] = fdv_rans_encode(sv[k], sn[k], er, payr[k], sn[k] * 2 + 1024);
            }
            if (ok2) {
                size_two_r = hdr;
                for (int k = 0; k < NS; ++k) size_two_r += 4 + lenr[k];
            }
        }
    }
    /* Per stream, keep whichever is smaller: sending a fresh table, or reusing
     * the one the previous frame sent.
     *
     * This used to be one decision for all seven streams together, which threw
     * away most of what reuse is worth -- a single stream whose distribution
     * moved forced every other stream to resend a table it did not need. One
     * bit each instead of one bit for all costs seven bits and is worth 2% to
     * 12% of a frame, the most on exactly the cheap content where tables are a
     * sixth of the bitstream. */
    int have_reuse = (tc && tc->have && size_two_r != (size_t)-1);
    /* One reuse bit per stream. Sixteen bits because there are ten streams and
     * the count has grown twice already; a mask that silently truncates loses
     * the tables at the top of the list and the decoder reads past the blob
     * looking for them. */
    _Static_assert(NS <= 16, "the reuse mask holds sixteen streams");
    size_t size_mix = hdr + 2;
    unsigned mask = 0;
    for (int k = 0; k < NS; ++k) {
        /* An empty stream says everything it has to say in its symbol count.
         * Ten streams means several are routinely empty -- a frame with no
         * intra macroblocks sends no sub-modes -- and paying a frequency table
         * and a length for each of them costs more than a cheap frame's entire
         * residual. */
        if (sn[k] == 0) continue;
        size_t fresh  = fdv_rans_freqs_size(dec[k].freq) + fdv_leb_size((uint32_t)len[k]) + len[k];
        size_t reused = have_reuse ? fdv_leb_size((uint32_t)lenr[k]) + lenr[k] : (size_t)-1;
        if (reused < fresh) { size_mix += reused; mask |= 1u << k; }
        else                  size_mix += fresh;
    }
    (void)size_two;

    /* And the fourth candidate: every stream short enough to afford a serial
     * decode is coded adaptively against the history both sides already hold,
     * and sends no table at all. They share one range-coded blob so the coder's
     * four-byte flush is paid once a frame instead of once a stream -- ten
     * flushes would cost more than the whole gain on a cheap frame.
     *
     * Which streams qualify is a rule, not a transmitted mask: the decoder
     * knows every symbol count before it reads any payload, so it can derive
     * the set for free. */
    size_t size_ada = (size_t)-1, lada = 0;
    uint8_t *pada = NULL;
    unsigned amask = 0;
    if (tc && tc->hn) {
        size_t asyms = 0;
        for (int k = 0; k < NS; ++k)
            if (sn[k] && sn[k] <= FDV_AD_CAP) { amask |= 1u << k; asyms += sn[k]; }
        /* A symbol costs at most log2(FDV_AD_MAX) bits against the thinnest
         * weight the model allows, which is two bytes; the spare third is
         * margin, and a buffer that ran out only costs this candidate. */
        if (amask && (pada = malloc(asyms * 3 + 64)) != NULL) {
            fdv_aenc ae; fdv_aenc_init(&ae, pada, asyms * 3 + 64);
            for (int k = 0; k < NS; ++k) {
                if (!((amask >> k) & 1)) continue;
                fdv_amodel m; fdv_amodel_init(&m, tc->hist[k]);
                for (size_t i = 0; i < sn[k]; ++i) fdv_aenc_sym(&ae, &m, sv[k][i]);
            }
            lada = fdv_aenc_finish(&ae);      /* 0 if the buffer ran out */
            if (lada) {
                size_ada = hdr + 2 + fdv_leb_size((uint32_t)lada) + lada;
                for (int k = 0; k < NS; ++k) {
                    if (sn[k] == 0 || ((amask >> k) & 1)) continue;
                    size_t fresh  = fdv_rans_freqs_size(dec[k].freq)
                                  + fdv_leb_size((uint32_t)len[k]) + len[k];
                    size_t reused = have_reuse ? fdv_leb_size((uint32_t)lenr[k]) + lenr[k]
                                               : (size_t)-1;
                    size_ada += reused < fresh ? reused : fresh;
                }
            }
        }
    }

    /* emode 0: one shared model, fresh table.  2: one shared model, reused.
     * 1: a model per stream, each fresh or reused per the mask byte.
     * 3: as 1, but the short streams go to the adaptive coder instead. */
    int emode = 0; size_t best = size_one;
    if (size_mix   < best) { best = size_mix;   emode = 1; }
    if (size_one_r < best) { best = size_one_r; emode = 2; }
    if (size_ada   < best) { best = size_ada;   emode = 3; }
    FDV_ZE(FDV_Z_ENTROPY);

    fdv_note_entropy(emode, sp, total - sp);

    size_t o = 0;
    if (2 + best > cap) goto fail;
    out[o++] = (uint8_t)qp;
    out[o++] = (uint8_t)emode;
    for (int k = 0; k < NS; ++k) o = fdv_leb_put(out, o, (uint32_t)sn[k]);
    if (!(emode & 1)) {                           /* one shared model */
        if (!(emode & 2)) o = fdv_rans_write_freqs(out, o, d1.freq);
        size_t n = (emode & 2) ? l1r : l1;
        const uint8_t *q = (emode & 2) ? p1r : p1;
        o = fdv_put32(out, o, (uint32_t)n);
        memcpy(out + o, q, n); o += n;
    } else {                                      /* a model per stream */
        unsigned am = (emode == 3) ? amask : 0u;
        o = fdv_put16(out, o, mask & ~am);
        for (int k = 0; k < NS; ++k) {
            if (sn[k] == 0 || ((am >> k) & 1)) continue;   /* nothing to send */
            int reused = (mask >> k) & 1;
            if (!reused) o = fdv_rans_write_freqs(out, o, dec[k].freq);
            size_t n = reused ? lenr[k] : len[k];
            const uint8_t *q = reused ? payr[k] : pay[k];
            o = fdv_leb_put(out, o, (uint32_t)n);
            memcpy(out + o, q, n); o += n;
        }
        if (emode == 3) {
            o = fdv_leb_put(out, o, (uint32_t)lada);
            memcpy(out + o, pada, lada); o += lada;
        }
    }
    /* Remember what this frame was coded with. A stream that reused its table
     * leaves the cache alone -- it is already the table in force. */
    if (tc) {
        if (emode == 0) { tab_store(tc->f1, d1.freq); tc->have = 1; }
        else if (emode == 1 || emode == 3) {
            unsigned am = (emode == 3) ? amask : 0u;
            for (int k = 0; k < NS; ++k)
                if (sn[k] && !((am >> k) & 1) && !((mask >> k) & 1))
                    tab_store(tc->fs[k], dec[k].freq);
            tc->have = 1;
        }
        /* Every stream feeds the history, whichever way the frame was coded --
         * the decoder does the same from the symbols it just decoded. */
        for (int k = 0; k < NS; ++k) hist_add(tc->hist[k], ck[k]);
        if (tc->hn < (1 << 20)) ++tc->hn;
    }
    free(syms); free(sctx); free(cn); free(cl); free(cn8); free(cl8); free(cfl); free(cnc); free(clc); free(mmap); free(mvmap); free(all); free(p1); free(p1r); free(pada);
    for (int k = 0; k < NS; ++k) { free(pay[k]); free(payr[k]); }
    return o;
fail:
    free(syms); free(sctx); free(cn); free(cl); free(cn8); free(cl8); free(cfl); free(cnc); free(clc); free(mmap); free(mvmap); free(all); free(p1); free(p1r); free(pada);
    for (int k = 0; k < NS; ++k) { free(pay[k]); free(payr[k]); }
    return 0;
}

/* Decode one chroma block (mirrors chroma_block). */
static void chroma_block_dec_inner(uint8_t *dst, int cw, const fdv_plane *ref,
                             int cbx, int cby, int cmvx, int cmvy, int qp,
                             int skip, fdv_cr *r, int *ok) {
    uint8_t pred[CB * CB];
    fdv_mc_chroma(ref, cbx, cby, CB, CB, cmvx, cmvy, pred, CB);

    if (skip) {
        for (int i = 0; i < CB; ++i)
            for (int j = 0; j < CB; ++j)
                dst[(cby + i) * cw + cbx + j] = pred[i * CB + j];
        return;
    }

    int any = fdv_rd_byte(r->b, &r->flp, r->flend, ok);
    if (any == 0) {                              /* nothing coded: keep the prediction */
        for (int i = 0; i < CB; ++i)
            for (int j = 0; j < CB; ++j)
                dst[(cby + i) * cw + cbx + j] = pred[i * CB + j];
        return;
    }
    if (any != 1) { *ok = 0; return; }
    for (int sy = 0; sy < CB; sy += 4)
        for (int sx = 0; sx < CB; sx += 4) {
            int16_t level[16] = {0};
            int cnt = cr_count(r, 16, ok);
            for (int k = 0; k < cnt; ++k)
                level[fdv_zz4[k]] = (int16_t)cr_level(r, ok);
            int32_t dcoeff[16];
            int16_t rres[16];
            fdv_dequant4x4(level, dcoeff, qp);
            fdv_idct4x4(dcoeff, rres);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    dst[(cby + sy + i) * cw + cbx + sx + j] =
                        clip255(pred[(sy + i) * CB + sx + j] + rres[i * 4 + j]);
        }
}

static void chroma_block_dec(uint8_t *dst, int cw, const fdv_plane *ref,
                             int cbx, int cby, int cmvx, int cmvy, int qp,
                             int skip, fdv_cr *r, int *ok) {
    r->chroma = 1;
    chroma_block_dec_inner(dst, cw, ref, cbx, cby, cmvx, cmvy, qp, skip, r, ok);
    r->chroma = 0;
}

/* Reject a motion vector that would make mc_* read outside a plane's bordered
 * region. The quarter-pel position is x+(mvx>>2); the 6-tap luma filter reads
 * up to 3 samples beyond the block on each axis, well inside a 3-sample margin.
 * Valid streams stay within the border (the encoder reconstructs through the
 * same reads), so this only fires on corrupt input. */
static int mv_in_bounds(const fdv_plane *pl, int x, int y, int bw, int bh,
                        int mvx, int mvy) {
    int ix = x + (mvx >> 2), iy = y + (mvy >> 2);
    int b = pl->border;
    if (ix - 3 < -b || iy - 3 < -b) return 0;
    if (ix + bw + 3 > pl->width + b || iy + bh + 3 > pl->height + b) return 0;
    return 1;
}

static int pframe_decode(const uint8_t *in, size_t len, const fdv_frame *const refs[],
                         uint8_t *dy, uint8_t *du, uint8_t *dv,
                         int w, int h, fdv_tabcache *tc) {
    int cw = w / 2;
    const fdv_plane *rpY = &refs[0]->planes[0], *rpU = &refs[0]->planes[1], *rpV = &refs[0]->planes[2];

    if (len < 2) return -1;
    size_t p = 0;
    int qp = in[p++];                           /* this frame's quantizer */
    if (qp > 51) return -1;
    int emode = in[p++];                        /* entropy mode flag */
    size_t total = 0;
    uint8_t *syms = NULL;
    /* Upper bound on decoded symbols: per 16x16 macroblock, structure bytes
     * (mode + sub/ref + up to 8 MV components) and coefficients for luma plus
     * two chroma planes (384 samples, each <= a count + 5-byte level). Generous
     * but finite, so a malformed header cannot force an absurd malloc. */
    size_t nmb = (size_t)((w + 15) / 16) * (size_t)((h + 15) / 16);
    size_t maxsyms = nmb * (size_t)(16 + 384 * 6);

    if (emode > 3) return -1;
    int reuse = (emode == 2);
    if (reuse && (!tc || !tc->have)) return -1;   /* nothing to reuse from */

    #define NS (PS_NSTRUCT + 7)
    size_t sn[NS], base[NS];
    for (int k = 0; k < NS; ++k) {
        int rok = 1;
        uint32_t v = (uint32_t)fdv_rd_count32(in, &p, len, &rok);
        if (!rok || v > maxsyms) return -1;
        sn[k] = v;
        total += sn[k];
        if (total > maxsyms) return -1;
    }
    {   size_t off = 0;
        for (int k = 0; k < NS; ++k) { base[k] = off; off += sn[k]; }
    }
    syms = malloc(total ? total : 1);
    if (!syms) return -1;

    if (!(emode & 1)) {                           /* one shared model */
        uint16_t freq[256];
        int rok = 1;
        if (reuse) memcpy(freq, tc->f1, sizeof freq);
        else       p = fdv_rans_read_freqs_bounded(in, p, len, freq, &rok);
        if (!rok || p + 4 > len) { free(syms); return -1; }
        uint32_t l1 = fdv_get32(in, &p);
        if (p + l1 > len) { free(syms); return -1; }
        fdv_rans_table d1;
        fdv_rans_dec_table_from_freq(freq, &d1);
        fdv_rans_decode_bounded(in + p, l1, total, &d1, syms);
        if (!reuse && tc) { tab_store(tc->f1, freq); tc->have = 1; }
    } else {                                      /* a model per stream */
        if (p + 2 > len) { free(syms); return -1; }
        unsigned mask = fdv_get16(in, &p);
        /* Which streams the encoder sent adaptively is derived from the symbol
         * counts, which are already in hand -- the same rule it applied. */
        unsigned am = 0;
        if (emode == 3) {
            if (!tc || !tc->hn) { free(syms); return -1; }
            for (int k = 0; k < NS; ++k)
                if (sn[k] && sn[k] <= FDV_AD_CAP) am |= 1u << k;
        }
        if ((mask & ~am) && (!tc || !tc->have)) { free(syms); return -1; }
        uint16_t freq[NS][256];
        for (int k = 0; k < NS; ++k) {
            if (sn[k] == 0 || ((am >> k) & 1)) continue;   /* nothing was sent */
            int rok = 1, reused = (mask >> k) & 1;
            if (reused) memcpy(freq[k], tc->fs[k], sizeof freq[k]);
            else        p = fdv_rans_read_freqs_bounded(in, p, len, freq[k], &rok);
            if (!rok) { free(syms); return -1; }
            uint32_t n = fdv_rd_count32(in, &p, len, &rok);
            if (!rok || p + n > len) { free(syms); return -1; }
            fdv_rans_table d;
            fdv_rans_dec_table_from_freq(freq[k], &d);
            fdv_rans_decode_bounded(in + p, n, sn[k], &d, syms + base[k]);
            p += n;
            if (!reused && tc) tab_store(tc->fs[k], freq[k]);
        }
        if (am) {
            int rok = 1;
            uint32_t n = fdv_rd_count32(in, &p, len, &rok);
            if (!rok || p + n > len) { free(syms); return -1; }
            fdv_adec ad; fdv_adec_init(&ad, in + p, n);
            for (int k = 0; k < NS; ++k) {
                if (!((am >> k) & 1)) continue;
                fdv_amodel m; fdv_amodel_init(&m, tc->hist[k]);
                for (size_t i = 0; i < sn[k]; ++i)
                    syms[base[k] + i] = (uint8_t)fdv_adec_sym(&ad, &m);
            }
            p += n;
        }
        if (tc) tc->have = 1;
    }

    /* The history the next frame's adaptive models are primed from, built from
     * the symbols just decoded -- the encoder folds in the same counts. */
    if (tc) {
        for (int k = 0; k < NS; ++k) {
            uint32_t c[256] = {0};
            for (size_t i = 0; i < sn[k]; ++i) c[syms[base[k] + i]]++;
            hist_add(tc->hist[k], c);
        }
        if (tc->hn < (1 << 20)) ++tc->hn;
    }

    /* A cursor per stream. Each stays inside its own run, so a malformed
     * stream trips `ok` and is rejected rather than over-read. */
    size_t cur[NS], end[NS];
    for (int k = 0; k < NS; ++k) { cur[k] = base[k]; end[k] = base[k] + sn[k]; }
    fdv_cr R = { .b = syms,
                 .np  = cur[NS - 7], .nend  = end[NS - 7],
                 .lp  = cur[NS - 6], .lend  = end[NS - 6],
                 .np8 = cur[NS - 5], .nend8 = end[NS - 5],
                 .lp8 = cur[NS - 4], .lend8 = end[NS - 4],
                 .flp = cur[NS - 3], .flend = end[NS - 3],
                 .ncp = cur[NS - 2], .ncend = end[NS - 2],
                 .lcp = cur[NS - 1], .lcend = end[NS - 1] };
    int ok = 1;
    int mmw = w / MB;
    uint8_t *mmap = calloc((size_t)mmw * ((h + MB - 1) / MB), 1);
    fdv_mbmv *mvmap = calloc((size_t)mmw * ((h + MB - 1) / MB), sizeof(*mvmap));
    if (!mmap || !mvmap) { free(syms); free(mmap); free(mvmap); return -1; }
    for (int by = 0; by < h && ok; by += MB) {
        int pmvx = 0, pmvy = 0;
        for (int bx = 0; bx < w && ok; bx += MB) {
            int mbx = bx / MB, mby = by / MB;
            int mc = mb_mode_ctx(mmap, mmw, mbx, mby);
            int mode = fdv_rd_byte(syms, &cur[mc], end[mc], &ok);
            mmap[mby * mmw + mbx] = (uint8_t)mode;
            int cbx = bx / 2, cby = by / 2;

            if (mode == 2) {                            /* INTRA */
                int sub = fdv_rd_byte(syms, &cur[PS_SUB], end[PS_SUB], &ok);
                uint8_t ntop[MB], nleft[MB], ntl; int iht, ihl;
                gather_nb(dy, w, bx, by, MB, ntop, nleft, &ntl, &iht, &ihl);
                uint8_t pred[MB * MB];
                fdv_intra_nxn(sub, ntop, nleft, ntl, MB, iht, ihl, pred);
                decode_residual(&R, &ok, pred, MB, qp, dy, w, bx, by);

                uint8_t *pc[2] = {du, dv};
                for (int pl = 0; pl < 2; ++pl) {
                    uint8_t ct[CB], cl[CB], ctl; int cht, chl;
                    gather_nb(pc[pl], cw, cbx, cby, CB, ct, cl, &ctl, &cht, &chl);
                    uint8_t cpred[CB * CB];
                    fdv_intra_nxn(sub, ct, cl, ctl, CB, cht, chl, cpred);
                    R.chroma = 1;
                    decode_residual(&R, &ok, cpred, CB, qp, pc[pl], cw, cbx, cby);
                    R.chroma = 0;
                }
                mvmap[(size_t)mby * mmw + mbx] = (fdv_mbmv){0, 0, 0, 0};
                continue;                               /* MV predictor unchanged */
            }

            if (mode == 3) {                            /* INTER 8x8 */
                int qmv[4][2], qpx = pmvx, qpy = pmvy;
                for (int q = 0; q < 4; ++q) {
                    int qx = (q & 1) * 8, qy = (q >> 1) * 8;
                    int mx = qpx + fdv_rd_level(syms, &cur[PS_MVX], end[PS_MVX], &ok);
                    int my = qpy + fdv_rd_level(syms, &cur[PS_MVY], end[PS_MVY], &ok);
                    qpx = mx; qpy = my; qmv[q][0] = mx; qmv[q][1] = my;
                    if (!ok || !mv_in_bounds(rpY, bx + qx, by + qy, 8, 8, mx, my)) { ok = 0; break; }
                    uint8_t pred8[64];
                    fdv_mc_luma(rpY, bx + qx, by + qy, 8, 8, mx, my, pred8, 8);
                    decode_residual(&R, &ok, pred8, 8, qp, dy, w, bx + qx, by + qy);
                }
                if (!ok) break;
                pmvx = qmv[3][0]; pmvy = qmv[3][1];
                mvmap[(size_t)mby * mmw + mbx] =
                    (fdv_mbmv){(int16_t)qmv[3][0], (int16_t)qmv[3][1], 0, 1};
                uint8_t *pc[2] = {du, dv};
                const fdv_plane *rp[2] = {rpU, rpV};
                for (int pl = 0; pl < 2 && ok; ++pl)
                    for (int q = 0; q < 4 && ok; ++q) {
                        int qcx = (q & 1) * 4, qcy = (q >> 1) * 4;
                        if (!mv_in_bounds(rp[pl], cbx + qcx, cby + qcy, 4, 4,
                                          qmv[q][0] / 2, qmv[q][1] / 2)) { ok = 0; break; }
                        uint8_t cpred[16];
                        fdv_mc_chroma(rp[pl], cbx + qcx, cby + qcy, 4, 4,
                                  qmv[q][0] / 2, qmv[q][1] / 2, cpred, 4);
                        R.chroma = 1;
                        decode_residual(&R, &ok, cpred, 4, qp, pc[pl], cw, cbx + qcx, cby + qcy);
                        R.chroma = 0;
                    }
                if (!ok) break;
                continue;
            }

            if (mode != 0 && mode != 1) { ok = 0; break; }   /* unknown mode */
            int mvx, mvy, skip = (mode == 0), ridx = 0;
            if (skip) {
                /* The same list the encoder built, from macroblocks already
                 * decoded; only the index travels, and only when there was
                 * more than one thing to point at. */
                fdv_mbmv cand[FDV_MERGE_MAX];
                int ncand = merge_cands(mvmap, mmw, mbx, mby, pmvx, pmvy, cand);
                int mi = 0;
                if (ncand > 1) {
                    mi = fdv_rd_count(syms, &cur[PS_MERGE], end[PS_MERGE],
                                      ncand - 1, &ok);
                    if (!ok) break;
                }
                /* The candidates come from this decoder's own map, so their
                 * reference indices are ones it already validated. */
                mvx = cand[mi].x; mvy = cand[mi].y; ridx = cand[mi].ref;
                pmvx = mvx; pmvy = mvy;
            } else {
                ridx = fdv_rd_byte(syms, &cur[PS_REF], end[PS_REF], &ok);
                if (ridx > 1) { ok = 0; break; }            /* only two references */
                mvx = pmvx + fdv_rd_level(syms, &cur[PS_MVX], end[PS_MVX], &ok);
                mvy = pmvy + fdv_rd_level(syms, &cur[PS_MVY], end[PS_MVY], &ok);
                pmvx = mvx; pmvy = mvy;
            }
            mvmap[(size_t)mby * mmw + mbx] =
                (fdv_mbmv){(int16_t)mvx, (int16_t)mvy, (uint8_t)ridx, 1};
            const fdv_plane *lref = &refs[ridx]->planes[0];
            if (!ok || !mv_in_bounds(lref, bx, by, MB, MB, mvx, mvy)
                    || !mv_in_bounds(&refs[ridx]->planes[1], cbx, cby, CB, CB, mvx / 2, mvy / 2)) {
                ok = 0; break;
            }
            if (skip) {
                uint8_t pred[MB * MB];
                fdv_mc_luma(lref, bx, by, MB, MB, mvx, mvy, pred, MB);
                for (int i = 0; i < MB; ++i)
                    for (int j = 0; j < MB; ++j)
                        dy[(by + i) * w + bx + j] = pred[i * MB + j];
            } else {
                uint8_t pred[MB * MB];
                fdv_mc_luma(lref, bx, by, MB, MB, mvx, mvy, pred, MB);
                decode_residual(&R, &ok, pred, MB, qp, dy, w, bx, by);
            }

            chroma_block_dec(du, cw, &refs[ridx]->planes[1], cbx, cby, mvx / 2, mvy / 2, qp, skip, &R, &ok);
            chroma_block_dec(dv, cw, &refs[ridx]->planes[2], cbx, cby, mvx / 2, mvy / 2, qp, skip, &R, &ok);
        }
    }
    free(mmap);
    if (!ok) { free(syms); return -1; }
    free(syms);
    fdv_deblock_plane(dy, w, h, w, qp);
    fdv_deblock_plane(du, cw, h / 2, cw, qp);
    fdv_deblock_plane(dv, cw, h / 2, cw, qp);
    return 0;
}

/* ---- Sequence container -------------------------------------------------- */

size_t fdv_video_encode(const uint8_t *const *frames, int nframes,
                    int w, int h, int qp, int keyint, uint8_t *out, size_t cap) {
    if (nframes <= 0 || (w & 15) || (h & 15)) return 0;
    if (keyint < 0) keyint = 0;
    if (keyint > 255) keyint = 255;
    int cw = w / 2, ch = h / 2;

    /* Reference pool of two frames: `near` (previous) and `far` (two back). */
    fdv_frame r0, r1;
    if (fdv_frame_alloc(&r0, w, h) != 0) return 0;
    if (fdv_frame_alloc(&r1, w, h) != 0) { fdv_frame_free(&r0); return 0; }
    fdv_frame *near = &r0, *far = &r1;
    int nref = 0;

    size_t tmp_cap = (size_t)w * h * 3 + 8192;
    uint8_t *tmp = malloc(tmp_cap);
    uint8_t *ry = malloc((size_t)w * h);
    uint8_t *ru = malloc((size_t)cw * ch);
    uint8_t *rv = malloc((size_t)cw * ch);
    if (!tmp || !ry || !ru || !rv) {
        free(tmp); free(ry); free(ru); free(rv); fdv_frame_free(&r0); fdv_frame_free(&r1); return 0;
    }

    size_t o = 0;
    o = fdv_put16(out, o, (unsigned)nframes);
    o = fdv_put16(out, o, (unsigned)w);
    o = fdv_put16(out, o, (unsigned)h);
    out[o++] = (uint8_t)qp;
    out[o++] = (uint8_t)keyint;

    FDV_LOG(FDV_LOG_INFO, "encode",
            "begin %dx%d %d frame(s) qp=%d keyint=%d  (%d MB/frame, header %zu B)",
            w, h, nframes, qp, keyint, (w / 16) * (h / 16), o);

    fdv_tabcache tc; memset(&tc, 0, sizeof tc);
    for (int f = 0; f < nframes; ++f) {
        const uint8_t *cy = frames[f];
        const uint8_t *cu = cy + (size_t)w * h;
        const uint8_t *cv = cu + (size_t)cw * ch;
        int is_intra = (f == 0) || (keyint > 0 && f % keyint == 0);

        fdv_frame_counters_reset();
        double t_frame = fdv_now_ms();
        size_t blob;
        /* Entropy tables carry over between P-frames, and a key frame is where
         * the chain restarts -- so seeking to one still needs nothing before it. */
        if (is_intra) memset(&tc, 0, sizeof tc);   /* tables and history both restart */
        if (is_intra) {
            blob = iframe_encode(cy, cu, cv, w, h, qp, tmp, tmp_cap, ry, ru, rv);
        } else {
            const fdv_frame *refs[2] = {near, (nref >= 2) ? far : near};
            int navail = (nref >= 2) ? 2 : 1;
            blob = pframe_encode(cy, cu, cv, w, h, refs, navail, qp, tmp, tmp_cap, ry, ru, rv, 0, &tc);
            /* Deblock the P-frame reconstruction so it matches the decoder. */
            fdv_deblock_plane(ry, w, h, w, qp);
            fdv_deblock_plane(ru, cw, ch, cw, qp);
            fdv_deblock_plane(rv, cw, ch, cw, qp);
        }

        double frame_ms = fdv_now_ms() - t_frame;
        if (blob == 0 || o + 4 + blob > cap) {
            FDV_LOG(FDV_LOG_INFO, "encode", "frame %d failed (blob=%zu, cap=%zu)",
                    f, blob, cap);
            o = 0; break;
        }
        o = fdv_put32(out, o, (uint32_t)blob);
        memcpy(out + o, tmp, blob);
        o += blob;

        {
            int nmb = (w / 16) * (h / 16);
            fdv_frame_stat *st = stat_next(f, is_intra);
            if (st) {
                st->bytes = blob;
                st->ms = frame_ms;
                st->entropy_mode = fdv_entropy_mode;
                st->sym_struct = fdv_sym_struct;
                st->sym_coeff  = fdv_sym_coeff;
                if (is_intra) {
                    st->mb_intra = nmb;
                } else {
                    st->mb_skip    = fdv_mb_tally[0];
                    st->mb_inter16 = fdv_mb_tally[1];
                    st->mb_intra   = fdv_mb_tally[2];
                    st->mb_inter8  = fdv_mb_tally[3];
                }
            }
            if (is_intra)
                FDV_LOG(FDV_LOG_FRAME, "encode",
                        "frame %-4d I  %7zu B  %8.2f ms  %4d MB all-intra          "
                        "entropy=%-8s syms S=%zu C=%zu",
                        f, blob, frame_ms, nmb,
                        fdv_emode_name(fdv_entropy_mode),
                        fdv_sym_struct, fdv_sym_coeff);
            else
                FDV_LOG(FDV_LOG_FRAME, "encode",
                        "frame %-4d P  %7zu B  %8.2f ms  skip %-4d inter16 %-4d "
                        "inter8 %-4d intra %-4d entropy=%-8s syms S=%zu C=%zu",
                        f, blob, frame_ms, fdv_mb_tally[0], fdv_mb_tally[1],
                        fdv_mb_tally[3], fdv_mb_tally[2],
                        fdv_emode_name(fdv_entropy_mode),
                        fdv_sym_struct, fdv_sym_coeff);
        }

        /* New reconstruction becomes `near`; old `near` slides to `far`. A
         * keyframe resets the pool so later P-frames never reference across it. */
        load_ref(far, ry, ru, rv, w, h);
        fdv_frame *t = near; near = far; far = t;
        nref = is_intra ? 1 : (nref < 2 ? nref + 1 : 2);
    }

    free(tmp); free(ry); free(ru); free(rv); fdv_frame_free(&r0); fdv_frame_free(&r1);
    FDV_LOG(FDV_LOG_INFO, "encode", "done  %zu B total (%.1f B/frame, %.1fx vs raw)",
            o, nframes ? (double)o / nframes : 0.0,
            o ? (double)((size_t)w * h * 3 / 2 * (size_t)nframes) / (double)o : 0.0);
    return o;
}

int fdv_video_decode(const uint8_t *in, size_t len, uint8_t *out,
                 int *nframes, int *w, int *h) {
    if (len < 8) return -1;
    size_t p = 0;
    int nf = (int)fdv_get16(in, &p);
    int ww = (int)fdv_get16(in, &p);
    int hh = (int)fdv_get16(in, &p);
    int qp = in[p++];
    int keyint = in[p++];
    if (nf <= 0 || (ww & 15) || (hh & 15) || ww > 8192 || hh > 8192) return -1;
    if (qp < 0 || qp > 51) return -1;   /* qp indexes the quant tables (52 entries) */
    int cw = ww / 2, ch = hh / 2;
    size_t fsize = (size_t)ww * hh + 2 * (size_t)cw * ch;

    fdv_frame r0, r1;
    if (fdv_frame_alloc(&r0, ww, hh) != 0) return -1;
    if (fdv_frame_alloc(&r1, ww, hh) != 0) { fdv_frame_free(&r0); return -1; }
    fdv_frame *near = &r0, *far = &r1;
    int nref = 0;

    FDV_LOG(FDV_LOG_INFO, "decode",
            "begin %dx%d %d frame(s) qp=%d keyint=%d  (%zu B stream, %zu B/frame out)",
            ww, hh, nf, qp, keyint, len, fsize);

    fdv_tabcache tc; memset(&tc, 0, sizeof tc);
    for (int f = 0; f < nf; ++f) {
        if (p + 4 > len) { fdv_frame_free(&r0); fdv_frame_free(&r1); return -1; }
        uint32_t blob = fdv_get32(in, &p);
        if (p + blob > len) { fdv_frame_free(&r0); fdv_frame_free(&r1); return -1; }

        uint8_t *dy = out + (size_t)f * fsize;
        uint8_t *du = dy + (size_t)ww * hh;
        uint8_t *dv = du + (size_t)cw * ch;

        int is_intra = (f == 0) || (keyint > 0 && f % keyint == 0);
        double t_frame = fdv_now_ms();
        int rc;
        if (is_intra) memset(&tc, 0, sizeof tc);   /* tables and history both restart */
        if (is_intra) {
            rc = iframe_decode(in + p, blob, dy, du, dv, ww, hh);
        } else {
            const fdv_frame *refs[2] = {near, (nref >= 2) ? far : near};
            rc = pframe_decode(in + p, blob, refs, dy, du, dv, ww, hh, &tc);
        }
        if (rc != 0) {
            FDV_LOG(FDV_LOG_INFO, "decode", "frame %d rejected (%u B blob at offset %zu)",
                    f, blob, p);
            fdv_frame_free(&r0); fdv_frame_free(&r1); return -1;
        }
        p += blob;

        {
            double frame_ms = fdv_now_ms() - t_frame;
            fdv_frame_stat *st = stat_next(f, is_intra);
            if (st) { st->bytes = blob; st->ms = frame_ms; }
            FDV_LOG(FDV_LOG_FRAME, "decode",
                    "frame %-4d %c  %7u B consumed  %8.3f ms  -> %zu B planar  (%zu/%zu stream)",
                    f, is_intra ? 'I' : 'P', blob, frame_ms, fsize, p, len);
        }

        load_ref(far, dy, du, dv, ww, hh);
        fdv_frame *t = near; near = far; far = t;
        nref = is_intra ? 1 : (nref < 2 ? nref + 1 : 2);
    }

    fdv_frame_free(&r0); fdv_frame_free(&r1);
    FDV_LOG(FDV_LOG_INFO, "decode", "done  %d frame(s), %zu of %zu stream bytes consumed",
            nf, p, len);
    if (nframes) *nframes = nf;
    if (w) *w = ww;
    if (h) *h = hh;
    return 0;
}

/* ===========================================================================
 * 12. TILES
 * Independent still-image tiles; threaded decode.
 * ======================================================================== */




size_t fdv_tiled_encode(const uint8_t *src, int w, int h, int qp,
                    int tile_w, int tile_h, uint8_t *out, size_t cap) {
    if (w <= 0 || h <= 0 || (w & 3) || (h & 3) ||
        (tile_w & 3) || (tile_h & 3) || tile_w <= 0 || tile_h <= 0)
        return 0;

    int ntx = ceil_div(w, tile_w);
    int nty = ceil_div(h, tile_h);

    size_t tmp_cap = (size_t)tile_w * tile_h * 2 + 4096;
    uint8_t *tmp = malloc(tmp_cap);
    if (!tmp) return 0;

    size_t o = 0;
    o = fdv_put16(out, o, (unsigned)w);
    o = fdv_put16(out, o, (unsigned)h);
    out[o++] = (uint8_t)qp;
    o = fdv_put16(out, o, (unsigned)tile_w);
    o = fdv_put16(out, o, (unsigned)tile_h);
    o = fdv_put16(out, o, (unsigned)ntx);
    o = fdv_put16(out, o, (unsigned)nty);

    for (int ty = 0; ty < nty; ++ty) {
        for (int tx = 0; tx < ntx; ++tx) {
            int x = tx * tile_w, y = ty * tile_h;
            int tw = (x + tile_w <= w) ? tile_w : (w - x);
            int th = (y + tile_h <= h) ? tile_h : (h - y);

            size_t blob = fdv_image_encode(src + (size_t)y * w + x, tw, th, w, qp,
                                       tmp, tmp_cap, NULL);
            if (blob == 0 || o + 4 + blob > cap) { free(tmp); return 0; }
            o = fdv_put32(out, o, (uint32_t)blob);
            memcpy(out + o, tmp, blob);
            o += blob;
        }
    }

    free(tmp);
    return o;
}

/* A located tile substream: where its bytes are and where its pixels go. */
typedef struct {
    const uint8_t *blob;
    size_t         blob_len;
    uint8_t       *dst;       /* pointer to the tile's top-left in the frame */
    int            stride;
} fdv_tile_job;

/* Parse the container into a job list. Returns ntiles, or -1 on malformed
 * input. *jobs is malloc'd on success. */
static int parse_jobs(const uint8_t *in, size_t len, uint8_t *dst,
                      int *w_out, int *h_out, fdv_tile_job **jobs_out) {
    if (len < 11) return -1;
    size_t p = 0;
    int w  = (int)fdv_get16(in, &p);
    int h  = (int)fdv_get16(in, &p);
    p++;                              /* qp (each tile substream carries its own) */
    int tile_w = (int)fdv_get16(in, &p);
    int tile_h = (int)fdv_get16(in, &p);
    int ntx = (int)fdv_get16(in, &p);
    int nty = (int)fdv_get16(in, &p);
    if (w <= 0 || h <= 0 || tile_w <= 0 || tile_h <= 0) return -1;

    int n = ntx * nty;
    fdv_tile_job *jobs = malloc((size_t)n * sizeof(*jobs));
    if (!jobs) return -1;

    for (int ty = 0; ty < nty; ++ty) {
        for (int tx = 0; tx < ntx; ++tx) {
            if (p + 4 > len) { free(jobs); return -1; }
            uint32_t blob = fdv_get32(in, &p);
            if (p + blob > len) { free(jobs); return -1; }
            int idx = ty * ntx + tx;
            jobs[idx].blob     = in + p;
            jobs[idx].blob_len = blob;
            jobs[idx].dst      = dst + (size_t)(ty * tile_h) * w + tx * tile_w;
            jobs[idx].stride   = w;
            p += blob;
        }
    }
    *w_out = w;
    *h_out = h;
    *jobs_out = jobs;
    return n;
}

static int run_job(const fdv_tile_job *j) {
    int dw, dh;
    return fdv_image_decode(j->blob, j->blob_len, j->dst, j->stride, &dw, &dh);
}

int fdv_tiled_decode(const uint8_t *in, size_t len, uint8_t *dst,
                 int *w_out, int *h_out) {
    fdv_tile_job *jobs;
    int n = parse_jobs(in, len, dst, w_out, h_out, &jobs);
    if (n < 0) return -1;

    int rc = 0;
    for (int i = 0; i < n; ++i)
        if (run_job(&jobs[i]) != 0) rc = -1;

    free(jobs);
    return rc;
}

typedef struct {
    const fdv_tile_job *jobs;
    int   n;
    int   thread_id;
    int   nthreads;
    int   rc;
} fdv_worker_arg;

static void *worker(void *v) {
    fdv_worker_arg *a = v;
    a->rc = 0;
    for (int i = a->thread_id; i < a->n; i += a->nthreads)
        if (run_job(&a->jobs[i]) != 0) a->rc = -1;
    return NULL;
}

int fdv_tiled_decode_threaded(const uint8_t *in, size_t len, uint8_t *dst,
                          int *w_out, int *h_out, int nthreads) {
    if (nthreads <= 1) return fdv_tiled_decode(in, len, dst, w_out, h_out);

    fdv_tile_job *jobs;
    int n = parse_jobs(in, len, dst, w_out, h_out, &jobs);
    if (n < 0) return -1;
    if (nthreads > n) nthreads = n > 0 ? n : 1;

    pthread_t   *tids = malloc((size_t)nthreads * sizeof(*tids));
    fdv_worker_arg  *args = malloc((size_t)nthreads * sizeof(*args));
    if (!tids || !args) { free(tids); free(args); free(jobs); return -1; }

    for (int t = 0; t < nthreads; ++t) {
        args[t] = (fdv_worker_arg){jobs, n, t, nthreads, 0};
        if (pthread_create(&tids[t], NULL, worker, &args[t]) != 0) {
            /* On spawn failure, run the rest inline. */
            worker(&args[t]);
            tids[t] = 0;
        }
    }

    int rc = 0;
    for (int t = 0; t < nthreads; ++t) {
        if (tids[t]) pthread_join(tids[t], NULL);
        if (args[t].rc != 0) rc = -1;
    }

    free(tids); free(args); free(jobs);
    return rc;
}

/* ===========================================================================
 * 13. VTILE
 * Tile-parallel video: independent horizontal bands.
 * ======================================================================== */


/* Container: [w u16][h u16][nframes u16][qp u8][keyint u8][nbands u16]
 *            [band_mbrows u16]  then nbands x ([blob_len u32][blob]).
 * Band b covers luma rows [b*band_mbrows*16, ...); the layout is derived
 * deterministically on both sides so only the per-band blob sizes are stored. */


/* Code one band into its own freshly allocated buffer.  Everything it touches
 * is either read-only (the source frames) or thread-private, so bands can run
 * concurrently; the caller concatenates the results in band order afterwards,
 * which is what keeps the output byte-identical to a serial encode. */
static int encode_band(const uint8_t *const *frames, int nframes, int w, int h,
                       int qp, int keyint, int y0, int bh,
                       uint8_t **blob_out, size_t *blen_out) {
    int cw = w / 2, ch = h / 2, bch = bh / 2, cy0 = y0 / 2;
    size_t bfs = (size_t)w * bh + 2 * (size_t)cw * bch;
    size_t scap = bfs * 2 + 65536;

    uint8_t *sub = malloc(bfs * (size_t)nframes);
    const uint8_t **subp = malloc((size_t)nframes * sizeof(*subp));
    uint8_t *tmp = malloc(scap);
    if (!sub || !subp || !tmp) { free(sub); free(subp); free(tmp); return -1; }

    for (int f = 0; f < nframes; ++f) {
        uint8_t *d = sub + (size_t)f * bfs;
        const uint8_t *fy = frames[f];
        const uint8_t *fu = fy + (size_t)w * h;
        const uint8_t *fv = fu + (size_t)cw * ch;
        memcpy(d, fy + (size_t)y0 * w, (size_t)w * bh);
        memcpy(d + (size_t)w * bh, fu + (size_t)cy0 * cw, (size_t)cw * bch);
        memcpy(d + (size_t)w * bh + (size_t)cw * bch,
               fv + (size_t)cy0 * cw, (size_t)cw * bch);
        subp[f] = d;
    }

    size_t blob = fdv_video_encode(subp, nframes, w, bh, qp, keyint, tmp, scap);
    free(sub); free(subp);
    if (blob == 0) { free(tmp); return -1; }
    *blob_out = tmp;
    *blen_out = blob;
    return 0;
}

typedef struct {
    const uint8_t *const *frames;
    int nframes, w, h, qp, keyint, bh_full, nbands;
    int tid, nthreads, rc;
    uint8_t **blob;              /* [nbands], filled in by whichever thread owns it */
    size_t   *blen;
} fdv_vencworker;

static void *vencworker_fn(void *v) {
    fdv_vencworker *a = v;
    a->rc = 0;
    for (int b = a->tid; b < a->nbands; b += a->nthreads) {
        int y0 = b * a->bh_full;
        int bh = (y0 + a->bh_full <= a->h) ? a->bh_full : (a->h - y0);
        if (encode_band(a->frames, a->nframes, a->w, a->h, a->qp, a->keyint,
                        y0, bh, &a->blob[b], &a->blen[b]) != 0)
            a->rc = -1;
    }
    return NULL;
}

size_t fdv_vtile_encode(const uint8_t *const *frames, int nframes, int w, int h,
                    int qp, int keyint, int band_mbrows, int nthreads,
                    uint8_t *out, size_t cap) {
    if (nframes <= 0 || w <= 0 || h <= 0 || (w & 15) || (h & 15) || band_mbrows < 1)
        return 0;
    if (cap < 12) return 0;
    int bh_full = band_mbrows * 16;
    int nbands = ceil_div(h, bh_full);
    if (keyint < 0) keyint = 0;
    if (keyint > 255) keyint = 255;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > nbands) nthreads = nbands;

    size_t o = 0;
    o = fdv_put16(out, o, (unsigned)w);
    o = fdv_put16(out, o, (unsigned)h);
    o = fdv_put16(out, o, (unsigned)nframes);
    out[o++] = (uint8_t)qp;
    out[o++] = (uint8_t)keyint;
    o = fdv_put16(out, o, (unsigned)nbands);
    o = fdv_put16(out, o, (unsigned)band_mbrows);

    FDV_LOG(FDV_LOG_INFO, "vtile",
            "encode %dx%d %d frame(s) qp=%d keyint=%d  %d band(s) of %d MB row(s), %d thread(s)",
            w, h, nframes, qp, keyint, nbands, band_mbrows, nthreads);

    uint8_t **blob = calloc((size_t)nbands, sizeof(*blob));
    size_t   *blen = calloc((size_t)nbands, sizeof(*blen));
    if (!blob || !blen) { free(blob); free(blen); return 0; }

    int rc = 0;
    if (nthreads <= 1) {
        for (int b = 0; b < nbands && rc == 0; ++b) {
            int y0 = b * bh_full;
            int bh = (y0 + bh_full <= h) ? bh_full : (h - y0);
            if (encode_band(frames, nframes, w, h, qp, keyint, y0, bh,
                            &blob[b], &blen[b]) != 0) rc = -1;
        }
    } else {
        /* The stats sink is a caller-owned array with no locking, and every
         * band drives fdv_video_encode; letting the workers race on it would
         * interleave the records. Suspend it, as threaded decode does. */
        fdv_stats *saved_sink = fdv_stats_get();
        fdv_stats_set(NULL);
        pthread_t  *tids = malloc((size_t)nthreads * sizeof(*tids));
        fdv_vencworker *args = malloc((size_t)nthreads * sizeof(*args));
        if (!tids || !args) {
            fdv_stats_set(saved_sink);
            free(tids); free(args); free(blob); free(blen); return 0;
        }
        for (int t = 0; t < nthreads; ++t) {
            args[t] = (fdv_vencworker){frames, nframes, w, h, qp, keyint, bh_full,
                                   nbands, t, nthreads, 0, blob, blen};
            if (pthread_create(&tids[t], NULL, vencworker_fn, &args[t]) != 0) {
                vencworker_fn(&args[t]);      /* spawn failed: run inline */
                tids[t] = 0;
            }
        }
        for (int t = 0; t < nthreads; ++t) {
            if (tids[t]) pthread_join(tids[t], NULL);
            if (args[t].rc != 0) rc = -1;
        }
        free(tids); free(args);
        fdv_stats_set(saved_sink);
    }

    /* Concatenate in band order regardless of the order they finished in. */
    for (int b = 0; b < nbands && rc == 0; ++b) {
        if (!blob[b] || o + 4 + blen[b] > cap) { rc = -1; break; }
        FDV_LOG(FDV_LOG_FRAME, "vtile", "band %-3d rows %4d..%-4d  %7zu B",
                b, b * bh_full,
                (b * bh_full + bh_full <= h ? b * bh_full + bh_full : h) - 1, blen[b]);
        o = fdv_put32(out, o, (uint32_t)blen[b]);
        memcpy(out + o, blob[b], blen[b]);
        o += blen[b];
    }
    for (int b = 0; b < nbands; ++b) free(blob[b]);
    free(blob); free(blen);

    if (rc != 0) return 0;
    FDV_LOG(FDV_LOG_INFO, "vtile", "encode done  %zu B total across %d band(s)", o, nbands);
    return o;
}

/* A located band sub-stream: its bytes and its position in the full frame. */
typedef struct {
    const uint8_t *blob;
    size_t         blen;
    int            y0, bh;
} fdv_vband;

/* Decode one band into its own packed buffer, then scatter its nframes
 * sub-frames into the full output planes. `sub` must hold nframes packed
 * sub-frames of this band's height. */
static int decode_band(const fdv_vband *bd, uint8_t *out, int w, int h,
                       int nframes, uint8_t *sub) {
    int cw = w / 2, ch = h / 2;
    int bh = bd->bh, bch = bh / 2, cy0 = bd->y0 / 2;
    int dnf, dw, dh;
    if (fdv_video_decode(bd->blob, bd->blen, sub, &dnf, &dw, &dh) != 0) return -1;
    if (dnf != nframes || dw != w || dh != bh) return -1;
    size_t bfs = (size_t)w * bh + 2 * (size_t)cw * bch;
    size_t fsize = (size_t)w * h + 2 * (size_t)cw * ch;
    for (int f = 0; f < nframes; ++f) {
        const uint8_t *s = sub + (size_t)f * bfs;
        uint8_t *fy = out + (size_t)f * fsize;
        uint8_t *fu = fy + (size_t)w * h;
        uint8_t *fv = fu + (size_t)cw * ch;
        memcpy(fy + (size_t)bd->y0 * w, s, (size_t)w * bh);
        memcpy(fu + (size_t)cy0 * cw, s + (size_t)w * bh, (size_t)cw * bch);
        memcpy(fv + (size_t)cy0 * cw,
               s + (size_t)w * bh + (size_t)cw * bch, (size_t)cw * bch);
    }
    return 0;
}

typedef struct {
    const fdv_vband *bands;
    uint8_t *out;
    int   nbands, w, h, nframes;
    int   tid, nthreads, rc;
} fdv_vworker;

static void *vworker_fn(void *v) {
    fdv_vworker *a = v;
    a->rc = 0;
    int cw = a->w / 2;
    size_t maxbfs = 0;                 /* tallest band this thread will touch */
    for (int i = a->tid; i < a->nbands; i += a->nthreads) {
        int bh = a->bands[i].bh;
        size_t bfs = (size_t)a->w * bh + 2 * (size_t)cw * (bh / 2);
        if (bfs > maxbfs) maxbfs = bfs;
    }
    if (maxbfs == 0) return NULL;      /* no bands assigned */
    uint8_t *sub = malloc(maxbfs * (size_t)a->nframes);
    if (!sub) { a->rc = -1; return NULL; }
    for (int i = a->tid; i < a->nbands; i += a->nthreads)
        if (decode_band(&a->bands[i], a->out, a->w, a->h, a->nframes, sub) != 0)
            a->rc = -1;
    free(sub);
    return NULL;
}

int fdv_vtile_decode(const uint8_t *in, size_t len, uint8_t *out,
                 int *nframes, int *w, int *h, int nthreads) {
    if (len < 12) return -1;
    size_t p = 0;
    int ww = (int)fdv_get16(in, &p);
    int hh = (int)fdv_get16(in, &p);
    int nf = (int)fdv_get16(in, &p);
    int qp = in[p++]; (void)qp;
    int keyint = in[p++]; (void)keyint;
    int nbands = (int)fdv_get16(in, &p);
    int band_mbrows = (int)fdv_get16(in, &p);
    if (nf <= 0 || ww <= 0 || hh <= 0 || (ww & 15) || (hh & 15) ||
        ww > 8192 || hh > 8192 || band_mbrows < 1) return -1;
    int bh_full = band_mbrows * 16;
    if (nbands != ceil_div(hh, bh_full) || nbands < 1 || nbands > hh / 16) return -1;

    fdv_vband *bands = malloc((size_t)nbands * sizeof(*bands));
    if (!bands) return -1;
    for (int b = 0; b < nbands; ++b) {
        int y0 = b * bh_full;
        int bh = (y0 + bh_full <= hh) ? bh_full : (hh - y0);
        if (p + 4 > len) { free(bands); return -1; }
        uint32_t blob = fdv_get32(in, &p);
        if (p + blob > len) { free(bands); return -1; }
        bands[b].blob = in + p; bands[b].blen = blob; bands[b].y0 = y0; bands[b].bh = bh;
        p += blob;
    }

    FDV_LOG(FDV_LOG_INFO, "vtile",
            "decode %dx%d %d frame(s) qp=%d  %d band(s) of %d MB row(s), %d thread(s)",
            ww, hh, nf, qp, nbands, band_mbrows, nthreads < 1 ? 1 : nthreads);

    int rc = 0;
    if (nthreads <= 1 || nbands <= 1) {
        int cw = ww / 2;
        size_t maxbfs = (size_t)ww * bh_full + 2 * (size_t)cw * (bh_full / 2);
        uint8_t *sub = malloc(maxbfs * (size_t)nf);
        if (!sub) { free(bands); return -1; }
        for (int b = 0; b < nbands && rc == 0; ++b)
            if (decode_band(&bands[b], out, ww, hh, nf, sub) != 0) rc = -1;
        free(sub);
    } else {
        /* The per-frame stats sink is a plain global with no locking, and every
         * band runs the base decoder.  Suspend it for the duration rather than
         * let the workers race; the serial path above still records. */
        fdv_stats *saved_sink = fdv_stats_get();
        fdv_stats_set(NULL);
        if (nthreads > nbands) nthreads = nbands;
        pthread_t *tids = malloc((size_t)nthreads * sizeof(*tids));
        fdv_vworker   *args = malloc((size_t)nthreads * sizeof(*args));
        if (!tids || !args) {
            fdv_stats_set(saved_sink);
            free(tids); free(args); free(bands); return -1;
        }
        for (int t = 0; t < nthreads; ++t) {
            args[t] = (fdv_vworker){bands, out, nbands, ww, hh, nf, t, nthreads, 0};
            if (pthread_create(&tids[t], NULL, vworker_fn, &args[t]) != 0) {
                vworker_fn(&args[t]);   /* spawn failed: run inline */
                tids[t] = 0;
            }
        }
        for (int t = 0; t < nthreads; ++t) {
            if (tids[t]) pthread_join(tids[t], NULL);
            if (args[t].rc != 0) rc = -1;
        }
        free(tids); free(args);
        fdv_stats_set(saved_sink);
    }

    free(bands);
    FDV_LOG(FDV_LOG_INFO, "vtile", "decode %s (%d band(s))",
            rc == 0 ? "ok" : "failed", nbands);
    if (rc == 0) { if (nframes) *nframes = nf; if (w) *w = ww; if (h) *h = hh; }
    return rc;
}

/* ===========================================================================
 * 14. RC
 * Encoder-side rate control and frame-level RDO.
 * ======================================================================== */


#define FDV_QP_MIN 0
#define FDV_QP_MAX 51

/* Encode once at the given QP; report rate (bytes) and distortion (SSE of the
 * reconstruction vs. source). Returns 0 on success. */
static int measure(const uint8_t *src, int w, int h, int qp,
                   size_t *bytes, double *sse) {
    size_t cap = (size_t)w * h * 2 + 4096;
    uint8_t *tmp = malloc(cap);
    uint8_t *rec = malloc((size_t)w * h);
    if (!tmp || !rec) { free(tmp); free(rec); return -1; }

    size_t len = fdv_image_encode(src, w, h, w, qp, tmp, cap, rec);
    if (len == 0) { free(tmp); free(rec); return -1; }

    double e = 0.0;
    for (int i = 0; i < w * h; ++i) { double d = src[i] - rec[i]; e += d * d; }

    *bytes = len;
    *sse   = e;
    free(tmp); free(rec);
    return 0;
}

int fdv_rc_qp_for_size(const uint8_t *src, int w, int h,
                   size_t target_bytes, size_t *out_bytes) {
    /* Size falls as QP rises; walk from finest to coarsest and take the first
     * QP that fits the budget (best quality within target). */
    size_t bytes = 0;
    double sse;
    int chosen = FDV_QP_MAX;
    for (int qp = FDV_QP_MIN; qp <= FDV_QP_MAX; ++qp) {
        if (measure(src, w, h, qp, &bytes, &sse) != 0) continue;
        if (bytes <= target_bytes) { chosen = qp; break; }
        chosen = qp;   /* track coarsest tried in case nothing fits */
    }
    if (out_bytes) {
        /* Report the achieved size at the chosen QP. */
        double tmp_sse;
        measure(src, w, h, chosen, out_bytes, &tmp_sse);
    }
    return chosen;
}

int fdv_rc_qp_rdo(const uint8_t *src, int w, int h, double lambda) {
    int best_qp = FDV_QP_MIN;
    double best_cost = -1.0;
    for (int qp = FDV_QP_MIN; qp <= FDV_QP_MAX; ++qp) {
        size_t bytes;
        double sse;
        if (measure(src, w, h, qp, &bytes, &sse) != 0) continue;
        double cost = sse + lambda * (double)bytes;
        if (best_cost < 0.0 || cost < best_cost) { best_cost = cost; best_qp = qp; }
    }
    return best_qp;
}

/* ===========================================================================
 * 15. FDV CONTAINER
 * Self-describing file wrapper around a coded stream.
 * ======================================================================== */

size_t fdv_wrap(uint8_t *out, size_t cap, int kind, int fps,
                 const uint8_t *payload, size_t payload_len) {
    if (!out || payload_len > 0xffffffffu || cap < FDV_HEAD + payload_len) return 0;
    if (kind != FDV_VIDEO && kind != FDV_TILED) return 0;
    if (fps <= 0 || fps > 65535) fps = 25;

    /* memmove, not memcpy: writing in place (payload sitting at out+FDV_HEAD)
     * is the normal case for the CLI, and the regions overlap. */
    memmove(out + FDV_HEAD, payload, payload_len);
    memcpy(out, FDV_MAGIC, 4);
    out[4] = (uint8_t)FDV_VERSION;
    out[5] = (uint8_t)kind;
    size_t o = 6;
    o = fdv_put16(out, o, (unsigned)fps);
    o = fdv_put32(out, o, (uint32_t)payload_len);
    return FDV_HEAD + payload_len;
}

int fdv_read(const uint8_t *in, size_t len, fdv_info *info) {
    if (!in || !info || len < FDV_HEAD) return -1;
    if (memcmp(in, FDV_MAGIC, 4) != 0) return -1;

    memset(info, 0, sizeof *info);
    info->version = in[4];
    info->kind    = in[5];
    if (info->version != FDV_VERSION) return -1;
    if (info->kind != FDV_VIDEO && info->kind != FDV_TILED) return -1;

    size_t p = 6;
    info->fps = (int)fdv_get16(in, &p);
    uint32_t plen = fdv_get32(in, &p);
    if (plen > len - FDV_HEAD) return -1;
    info->payload     = in + FDV_HEAD;
    info->payload_len = plen;
    info->nbands      = 1;

    /* The two coded streams begin with different fields, which is exactly why
     * the kind has to be recorded rather than guessed. */
    size_t q = 0;
    if (info->kind == FDV_VIDEO) {
        if (plen < 8) return -1;
        info->nframes = (int)fdv_get16(info->payload, &q);
        info->w       = (int)fdv_get16(info->payload, &q);
        info->h       = (int)fdv_get16(info->payload, &q);
        info->qp      = info->payload[q++];
        info->keyint  = info->payload[q++];
    } else {
        if (plen < 12) return -1;
        info->w       = (int)fdv_get16(info->payload, &q);
        info->h       = (int)fdv_get16(info->payload, &q);
        info->nframes = (int)fdv_get16(info->payload, &q);
        info->qp      = info->payload[q++];
        info->keyint  = info->payload[q++];
        info->nbands  = (int)fdv_get16(info->payload, &q);
    }
    /* Same limits the decoders enforce, applied here so a caller can size
     * buffers from this struct without having decoded anything yet. */
    if (info->nframes <= 0 || info->w <= 0 || info->h <= 0) return -1;
    if ((info->w & 15) || (info->h & 15)) return -1;
    if (info->w > 8192 || info->h > 8192) return -1;
    if (info->qp < 0 || info->qp > 51) return -1;
    if (info->fps <= 0) info->fps = 25;
    return 0;
}

int fdv_decode(const fdv_info *info, uint8_t *out, int nthreads) {
    if (!info || !out) return -1;
    int nf = 0, w = 0, h = 0;
    int rc = info->kind == FDV_TILED
        ? fdv_vtile_decode(info->payload, info->payload_len, out, &nf, &w, &h, nthreads)
        : fdv_video_decode(info->payload, info->payload_len, out, &nf, &w, &h);
    if (rc != 0) return -1;
    /* The head and the coded stream must agree; if they do not, the file is
     * inconsistent and the caller's buffer was sized from the wrong numbers. */
    if (nf != info->nframes || w != info->w || h != info->h) return -1;
    return 0;
}

/* --- streaming decode ---------------------------------------------------- */

/* One coded stream being walked a frame at a time. A tiled file holds several
 * of these -- one per band -- and steps them together. */
typedef struct fdv_stream_dec {
    fdv_tabcache tc;    /* entropy tables carried between P-frames */
    const uint8_t *pay;
    size_t         paylen, pos;
    int            nframes, w, h, qp, keyint;
    size_t         fsize;
    int            next;                 /* frame index the position refers to */
    fdv_frame      r0, r1;
    fdv_frame     *near, *far;
    int            nref;
    int            opened;
} fdv_stream_dec;

struct fdv_decoder {
    int kind, w, h, nframes, fps, qp, keyint;
    size_t fsize;
    int next;

    fdv_stream_dec  one;                 /* single-stream */

    int             nbands;              /* tile-parallel */
    fdv_stream_dec *band;
    int            *band_y0, *band_h;
    uint8_t        *bandbuf;
    size_t          bandbuf_cap;

    uint8_t        *scratch;             /* for frames a seek passes over */
};

/* Parse a coded stream's own header and allocate its reference pool. */
static int stream_open(fdv_stream_dec *s, const uint8_t *pay, size_t len) {
    if (len < 8) return -1;
    size_t p = 0;
    s->nframes = (int)fdv_get16(pay, &p);
    s->w       = (int)fdv_get16(pay, &p);
    s->h       = (int)fdv_get16(pay, &p);
    s->qp      = pay[p++];
    s->keyint  = pay[p++];
    if (s->nframes <= 0 || (s->w & 15) || (s->h & 15) ||
        s->w > 8192 || s->h > 8192) return -1;
    if (s->qp < 0 || s->qp > 51) return -1;
    s->pay = pay; s->paylen = len; s->pos = p; s->next = 0; s->nref = 0;
    s->fsize = (size_t)s->w * s->h + 2 * (size_t)(s->w / 2) * (s->h / 2);
    if (fdv_frame_alloc(&s->r0, s->w, s->h) != 0) return -1;
    if (fdv_frame_alloc(&s->r1, s->w, s->h) != 0) { fdv_frame_free(&s->r0); return -1; }
    s->near = &s->r0; s->far = &s->r1;
    s->opened = 1;
    return 0;
}

static void stream_close(fdv_stream_dec *s) {
    if (!s->opened) return;
    fdv_frame_free(&s->r0);
    fdv_frame_free(&s->r1);
    s->opened = 0;
}

/* Back to the first frame without reallocating: the reference pool is simply
 * marked empty, which is what an intra frame would do anyway. */
static void stream_rewind(fdv_stream_dec *s) {
    s->pos  = 8;
    s->next = 0;
    s->nref = 0;
    s->near = &s->r0;
    s->far  = &s->r1;
}

/* Skip the next frame's payload without decoding it. Only valid before the
 * next key frame, where nothing that follows depends on it. */
static int stream_skip(fdv_stream_dec *s) {
    if (s->next >= s->nframes || s->pos + 4 > s->paylen) return -1;
    uint32_t blob = fdv_get32(s->pay, &s->pos);
    if (s->pos + blob > s->paylen) return -1;
    s->pos += blob;
    ++s->next;
    return 0;
}

/* Decode one frame. Mirrors fdv_video_decode's loop body exactly -- the only
 * difference is where the state between iterations lives. */
static int stream_next(fdv_stream_dec *s, uint8_t *out) {
    if (s->next >= s->nframes) return 0;
    if (s->pos + 4 > s->paylen) return -1;
    uint32_t blob = fdv_get32(s->pay, &s->pos);
    if (s->pos + blob > s->paylen) return -1;

    uint8_t *dy = out;
    uint8_t *du = dy + (size_t)s->w * s->h;
    uint8_t *dv = du + (size_t)(s->w / 2) * (s->h / 2);

    int f = s->next;
    int is_intra = (f == 0) || (s->keyint > 0 && f % s->keyint == 0);
    double t0 = fdv_now_ms();
    int rc;
    if (is_intra) memset(&s->tc, 0, sizeof s->tc);
    if (is_intra) {
        rc = iframe_decode(s->pay + s->pos, blob, dy, du, dv, s->w, s->h);
    } else {
        const fdv_frame *refs[2] = {s->near, (s->nref >= 2) ? s->far : s->near};
        rc = pframe_decode(s->pay + s->pos, blob, refs, dy, du, dv, s->w, s->h, &s->tc);
    }
    if (rc != 0) {
        FDV_LOG(FDV_LOG_INFO, "decode", "frame %d rejected (%u B blob)", f, blob);
        return -1;
    }
    s->pos += blob;

    double ms = fdv_now_ms() - t0;
    fdv_frame_stat *st = stat_next(f, is_intra);
    if (st) { st->bytes = blob; st->ms = ms; }
    FDV_LOG(FDV_LOG_FRAME, "decode",
            "frame %-4d %c  %7u B consumed  %8.3f ms  -> %zu B planar",
            f, is_intra ? 'I' : 'P', blob, ms, s->fsize);

    load_ref(s->far, dy, du, dv, s->w, s->h);
    fdv_frame *t = s->near; s->near = s->far; s->far = t;
    s->nref = is_intra ? 1 : (s->nref < 2 ? s->nref + 1 : 2);
    ++s->next;
    return 1;
}

fdv_decoder *fdv_dec_open(const fdv_info *info) {
    if (!info || !info->payload) return NULL;
    fdv_decoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->kind = info->kind;
    d->w = info->w; d->h = info->h; d->nframes = info->nframes;
    d->fps = info->fps; d->qp = info->qp; d->keyint = info->keyint;
    d->fsize = (size_t)d->w * d->h * 3 / 2;
    d->nbands = 1;

    if (d->kind == FDV_VIDEO) {
        if (stream_open(&d->one, info->payload, info->payload_len) != 0) goto fail;
    } else {
        /* A tiled file is a header plus one length-prefixed sub-stream per
         * band, each a complete video stream over a shorter frame. Opening one
         * decoder per band and stepping them together gives streaming for
         * free -- the bands were already independent. */
        size_t p = 0;
        int ww = (int)fdv_get16(info->payload, &p);
        int hh = (int)fdv_get16(info->payload, &p);
        p += 2;                                   /* nframes, already known */
        p += 2;                                   /* qp, keyint */
        int nbands = (int)fdv_get16(info->payload, &p);
        int band_mbrows = (int)fdv_get16(info->payload, &p);
        int bh_full = band_mbrows * 16;
        if (ww != d->w || hh != d->h || nbands < 1 || band_mbrows < 1) goto fail;
        if (nbands != ceil_div(hh, bh_full)) goto fail;

        d->nbands  = nbands;
        d->band    = calloc((size_t)nbands, sizeof(*d->band));
        d->band_y0 = calloc((size_t)nbands, sizeof(*d->band_y0));
        d->band_h  = calloc((size_t)nbands, sizeof(*d->band_h));
        if (!d->band || !d->band_y0 || !d->band_h) goto fail;

        size_t widest = 0;
        for (int b = 0; b < nbands; ++b) {
            int y0 = b * bh_full;
            int bh = (y0 + bh_full <= hh) ? bh_full : (hh - y0);
            if (p + 4 > info->payload_len) goto fail;
            uint32_t blob = fdv_get32(info->payload, &p);
            if (p + blob > info->payload_len) goto fail;
            if (stream_open(&d->band[b], info->payload + p, blob) != 0) goto fail;
            p += blob;
            d->band_y0[b] = y0;
            d->band_h[b]  = bh;
            if (d->band[b].fsize > widest) widest = d->band[b].fsize;
        }
        d->bandbuf_cap = widest;
        d->bandbuf = malloc(widest);
        if (!d->bandbuf) goto fail;
    }
    return d;
fail:
    fdv_dec_close(d);
    return NULL;
}

void fdv_dec_close(fdv_decoder *d) {
    if (!d) return;
    stream_close(&d->one);
    if (d->band)
        for (int b = 0; b < d->nbands; ++b) stream_close(&d->band[b]);
    free(d->band); free(d->band_y0); free(d->band_h);
    free(d->bandbuf); free(d->scratch);
    free(d);
}

int    fdv_dec_pos(const fdv_decoder *d)        { return d ? d->next : -1; }
size_t fdv_dec_frame_size(const fdv_decoder *d) { return d ? d->fsize : 0; }

int fdv_dec_next(fdv_decoder *d, uint8_t *out) {
    if (!d || !out) return -1;
    if (d->next >= d->nframes) return 0;

    if (d->kind == FDV_VIDEO) {
        int rc = stream_next(&d->one, out);
        if (rc == 1) ++d->next;
        return rc;
    }

    /* Bands are spatially disjoint, so each decodes into its own buffer and is
     * scattered into the right rows of the output frame. Serial here: at one
     * frame per display refresh there is nothing to gain from threads, and
     * fdv_decode remains the parallel whole-file path. */
    int cw = d->w / 2, ch = d->h / 2;
    for (int b = 0; b < d->nbands; ++b) {
        int rc = stream_next(&d->band[b], d->bandbuf);
        if (rc != 1) return rc;
        int y0 = d->band_y0[b], bh = d->band_h[b];
        int bch = bh / 2, cy0 = y0 / 2;
        const uint8_t *sy = d->bandbuf;
        const uint8_t *su = sy + (size_t)d->w * bh;
        const uint8_t *sv = su + (size_t)cw * bch;
        uint8_t *fy = out;
        uint8_t *fu = fy + (size_t)d->w * d->h;
        uint8_t *fv = fu + (size_t)cw * ch;
        memcpy(fy + (size_t)y0 * d->w, sy, (size_t)d->w * bh);
        memcpy(fu + (size_t)cy0 * cw, su, (size_t)cw * bch);
        memcpy(fv + (size_t)cy0 * cw, sv, (size_t)cw * bch);
    }
    ++d->next;
    return 1;
}

int fdv_dec_seek(fdv_decoder *d, int frame) {
    if (!d || frame < 0 || frame >= d->nframes) return -1;
    if (frame == d->next) return 0;

    /* The nearest key frame at or before the target. Everything before it can
     * be skipped outright; everything from it up to the target has to be
     * decoded, because that is what builds the reference pool. */
    int key = (d->keyint > 0) ? (frame / d->keyint) * d->keyint : 0;

    /* Already past the key frame and behind the target: roll forward from
     * where we are rather than replaying the whole GOP. */
    int from = (d->next > key && d->next <= frame) ? d->next : key;
    if (from == key) {
        if (d->kind == FDV_VIDEO) stream_rewind(&d->one);
        else for (int b = 0; b < d->nbands; ++b) stream_rewind(&d->band[b]);
        d->next = 0;
        while (d->next < key) {
            if (d->kind == FDV_VIDEO) {
                if (stream_skip(&d->one) != 0) return -1;
            } else {
                for (int b = 0; b < d->nbands; ++b)
                    if (stream_skip(&d->band[b]) != 0) return -1;
            }
            ++d->next;
        }
    }

    if (!d->scratch) {
        d->scratch = malloc(d->fsize);
        if (!d->scratch) return -1;
    }
    /* Decode the frames between here and the target purely for their effect on
     * the reference pool; their pixels are thrown away. */
    int saved = fdv_log_level;
    fdv_log_level = FDV_LOG_OFF;
    while (d->next < frame) {
        if (fdv_dec_next(d, d->scratch) != 1) { fdv_log_level = saved; return -1; }
    }
    fdv_log_level = saved;
    return 0;
}

/* --- streaming encode ---------------------------------------------------- */

struct fdv_encoder {
    int key_due;        /* a key frame fell on a held slot; owe one */
    fdv_tabcache tc;    /* entropy tables carried between P-frames */
    fdv_rc rc;          /* average-bitrate control, when it is switched on */
    int use_rc;
    int frame_qp;       /* >=0 when a band-parallel parent dictates the QP */
    /* Band-parallel mode. NULL band[] means this is a single-stream encoder;
     * otherwise this object owns one ordinary encoder per horizontal band and
     * only coordinates them. */
    int            nbands, band_mbrows, nthreads;
    fdv_encoder  **band;
    uint8_t      **bandbuf;
    int w, h, qp, keyint, n;
    size_t fsize;
    fdv_frame r0, r1;
    fdv_frame *near, *far;
    int nref;
    uint8_t *tmp, *ry, *ru, *rv;
    size_t tmp_cap;
    uint8_t *out;                /* FDV_HEAD reserved, then the coded stream */
    size_t   cap, len;
};

static int enc_push(fdv_encoder *e, const uint8_t *i420, int force_skip);

/* How hard the loop pulls. Constant QP is very nearly RD-optimal on stationary
 * content, so the controller's job is to hit an average while disturbing QP as
 * little as it can get away with: steer the bucket back over seconds, not
 * frames, and never move far in one step. A fast loop hits the bitrate just as
 * well and looks considerably worse doing it. */
#ifndef FDV_RC_DRAIN
#define FDV_RC_DRAIN  2.0      /* seconds over which to return to half full */
#endif
#ifndef FDV_RC_STEP
#define FDV_RC_STEP   1        /* largest QP move between consecutive P-frames */
#endif
/* How much finer a key frame may be coded than the P-frames around it, and how
 * fast that gap adapts. Key frames do not run a loop of their own: at a
 * two-second interval the P loop sees sixty samples for every one a key-frame
 * loop would, so an independent loop converges sixty times more slowly. On hard
 * content that showed plainly -- the first key frame landed at QP 16 and cost
 * an eighth of the whole stream, and six key frames later the loop had still
 * not caught up. Following the P operating point with an adapting offset gets
 * the benefit of every P-frame's evidence. */
#ifndef FDV_RC_IP_MAX
#define FDV_RC_IP_MAX 8.0
#endif
#ifndef FDV_RC_IBOOST
#define FDV_RC_IBOOST 10.0     /* a key frame's share, counted in P-frames */
#endif
/* Moving bits by a factor needs roughly 5 QP per doubling on this content, so
 * a gain of 2 applies about 40% of the correction each frame: it converges in a
 * handful of frames and cannot oscillate. */
#ifndef FDV_RC_GAIN
#define FDV_RC_GAIN   1.0      /* QP steps per doubling of the bit error */
#endif

void fdv_rc_init(fdv_rc *rc, int bitrate, int fps, int w, int h, int keyint,
                 double bufsecs, int qpmin, int qpmax) {
    memset(rc, 0, sizeof *rc);
    rc->bitrate = bitrate > 0 ? bitrate : 1000000;
    rc->fps     = fps > 0 ? fps : 30;
    rc->bufbits = rc->bitrate * (bufsecs > 0.0 ? bufsecs : 1.0);
    rc->qpmin   = qpmin > 0 ? qpmin : 8;
    rc->qpmax   = qpmax > 0 ? qpmax : 46;
    rc->fill    = rc->bufbits * 0.5;
    /* With no key frames after the first, treat the interval as ten seconds:
     * long enough that the one I-frame barely shifts the P allocation. */
    rc->keyint  = keyint > 1 ? keyint : rc->fps * 10;

    /* A starting point, so the first frames are in the right neighbourhood
     * rather than wherever a fixed QP happens to land. Bits per pixel per
     * frame, measured on 720p camera content: the loop corrects from here
     * within a few frames, so this only has to be close. */
    double bpp = (double)rc->bitrate / rc->fps / ((double)w * h);
    double qp  = 26.0 - 6.0 * log2(bpp / 0.03);
    if (qp < rc->qpmin) qp = rc->qpmin;
    if (qp > rc->qpmax) qp = rc->qpmax;
    rc->qp_p = qp;
    /* Start level with the P-frames and let the offset earn its way up. The
     * first key frame of a stream is the one the loop knows least about, and
     * guessing it fine is expensive in exactly the place a buffer has least
     * room. */
    rc->ip_off = 0.0;
}

/* What this frame is allowed to cost. */
static double rc_target(fdv_rc *rc, int is_intra) {
    double budget = (double)rc->bitrate / rc->fps;

    /* Allocate over a whole key-frame interval rather than per frame: its bits
     * are split between the one key frame and its P-frames, with the key frame
     * counted as FDV_RC_IBOOST of them. Doing it per frame instead makes the
     * key frame's allowance depend on how full the bucket happens to be when it
     * lands, which is how the first frame of a stream ended up at QP 41. */
    double n = (double)rc->keyint;
    double denom = FDV_RC_IBOOST + (n - 1.0);
    double target = budget * n * (is_intra ? FDV_RC_IBOOST : 1.0) / denom;

    /* Then steer the bucket back toward half full, slowly. Balancing the books
     * every frame instead pumps quality frame to frame, which is both more
     * visible than the drift it prevents and worse for compression: an
     * over-quantized frame goes into the reference pool and every frame after
     * it pays to repair it. */
    target -= (rc->fill - rc->bufbits * 0.5) / (rc->fps * FDV_RC_DRAIN);
    double lo = budget * (is_intra ? 1.0 : 0.4);
    double hi = budget * (is_intra ? n * FDV_RC_IBOOST / denom * 2.0 : 2.5);
    if (target < lo) target = lo;
    if (target > hi) target = hi;
    return target;
}

int fdv_rc_pick(fdv_rc *rc, int is_intra) {
    rc->last_target = rc_target(rc, is_intra);
    double q = is_intra ? rc->qp_p - rc->ip_off : rc->qp_p;
    int qp = (int)lround(q);
    if (qp < rc->qpmin) qp = rc->qpmin;
    if (qp > rc->qpmax) qp = rc->qpmax;
    rc->used_qp = qp;
    return qp;
}

void fdv_rc_update(fdv_rc *rc, int is_intra, size_t bytes) {
    double bits = (double)bytes * 8.0;

    /* Correct the operating point by how far this frame missed what it was
     * asked for. No model of bits against QP anywhere -- that relation is not a
     * fixed power law (measured between 1.0 and 1.34 over QP 16..32), and a
     * loop that assumes it is runs away whenever it guesses high: the estimate
     * rises with QP, which asks for more QP. Plain feedback with a gain below
     * what the relation needs cannot do that, whatever the content. */
    double err = log2(bits / (rc->last_target > 1.0 ? rc->last_target : 1.0));
    double dq  = FDV_RC_GAIN * err;

    if (is_intra) {
        /* A key frame moves the offset, not the operating point: it says how
         * much finer key frames can afford to be, and nothing about how hard
         * the P-frames are. Overspending closes the gap toward the P point. */
        if (dq >  2.0) dq =  2.0;         /* or key frames pulse visibly */
        if (dq < -2.0) dq = -2.0;
        rc->ip_off -= dq;
        if (rc->ip_off < 0.0) rc->ip_off = 0.0;
        if (rc->ip_off > FDV_RC_IP_MAX) rc->ip_off = FDV_RC_IP_MAX;
    } else {
        if (dq >  FDV_RC_STEP) dq =  FDV_RC_STEP;
        if (dq < -FDV_RC_STEP) dq = -FDV_RC_STEP;
        rc->qp_p += dq;
        if (rc->qp_p < rc->qpmin) rc->qp_p = rc->qpmin;
        if (rc->qp_p > rc->qpmax) rc->qp_p = rc->qpmax;
    }

    rc->fill += bits - (double)rc->bitrate / rc->fps;
    if (rc->fill < 0.0) rc->fill = 0.0;
    /* Let it run past nominal so sustained overshoot keeps pushing QP up, but
     * not without bound, or one hard scene would govern the rest of the clip. */
    if (rc->fill > rc->bufbits * 2.0) rc->fill = rc->bufbits * 2.0;
}

void fdv_rc_skip(fdv_rc *rc, size_t bytes) {
    rc->fill += (double)bytes * 8.0 - (double)rc->bitrate / rc->fps;
    if (rc->fill < 0.0) rc->fill = 0.0;
    if (rc->fill > rc->bufbits * 2.0) rc->fill = rc->bufbits * 2.0;
}

/* Copy one horizontal band out of an I420 frame, planes still contiguous. */
static void slice_band(const uint8_t *i420, int w, int h, int y0, int bh, uint8_t *dst) {
    int cw = w / 2, ch = h / 2, bch = bh / 2, cy0 = y0 / 2;
    const uint8_t *fy = i420, *fu = fy + (size_t)w * h, *fv = fu + (size_t)cw * ch;
    memcpy(dst, fy + (size_t)y0 * w, (size_t)w * bh);
    memcpy(dst + (size_t)w * bh, fu + (size_t)cy0 * cw, (size_t)cw * bch);
    memcpy(dst + (size_t)w * bh + (size_t)cw * bch,
           fv + (size_t)cy0 * cw, (size_t)cw * bch);
}

typedef struct {
    fdv_encoder *e;
    const uint8_t *i420;
    int first, stride, repeat, rc;
} fdv_bandpush;

static void *bandpush_fn(void *vp) {
    fdv_bandpush *a = (fdv_bandpush *)vp;
    fdv_encoder *e = a->e;
    for (int b = a->first; b < e->nbands; b += a->stride) {
        int y0 = b * e->band_mbrows * 16;
        int bh = e->band[b]->h;
        int rc;
        if (a->repeat) rc = enc_push(e->band[b], NULL, 1);
        else {
            slice_band(a->i420, e->w, e->h, y0, bh, e->bandbuf[b]);
            rc = enc_push(e->band[b], e->bandbuf[b], 0);
        }
        if (rc != 0) a->rc = -1;
    }
    return NULL;
}

/* Drive every band through one frame, in parallel. */
static int bands_push(fdv_encoder *e, const uint8_t *i420, int repeat) {
    /* Mirror the intra decision every band is about to make on its own, so
     * rate control sees the same frame type they do and the parent's key_due
     * stays in step with theirs. */
    int f = e->n;
    int is_intra = (f == 0) || e->key_due || (e->keyint > 0 && f % e->keyint == 0);
    if (repeat) { e->key_due = is_intra; is_intra = 0; }
    else e->key_due = 0;

    size_t before = 0;
    if (e->use_rc) {
        /* One QP for the whole picture. Letting each band run its own
         * controller would have them all chasing the full budget, and the seams
         * between them would be visible as a quality step. */
        int qp = repeat ? e->rc.used_qp : fdv_rc_pick(&e->rc, is_intra);
        for (int b = 0; b < e->nbands; ++b) e->band[b]->frame_qp = qp;
        before = fdv_enc_bytes(e);
    }

    int nt = e->nthreads;
    if (nt > e->nbands) nt = e->nbands;
    /* A held frame is all-SKIP and costs almost nothing, so threading one
     * would cost more in spawns than it saves. */
    if (repeat) nt = 1;

    int rc = 0;
    if (nt <= 1) {
        fdv_bandpush a = { e, i420, 0, 1, repeat, 0 };
        bandpush_fn(&a);
        rc = a.rc;
    } else {
        /* The stats sink is a plain global with no locking and every band drives
         * a full encode, so suspend it while the workers run -- as threaded
         * decode and fdv_vtile_encode both do. */
        fdv_stats *saved = fdv_stats_get();
        fdv_stats_set(NULL);
        pthread_t *tids = malloc((size_t)nt * sizeof(*tids));
        fdv_bandpush *args = malloc((size_t)nt * sizeof(*args));
        if (!tids || !args) { free(tids); free(args); fdv_stats_set(saved); return -1; }
        for (int t = 0; t < nt; ++t) {
            args[t] = (fdv_bandpush){ e, i420, t, nt, repeat, 0 };
            if (pthread_create(&tids[t], NULL, bandpush_fn, &args[t]) != 0) {
                bandpush_fn(&args[t]);             /* spawn failed: run it inline */
                tids[t] = 0;
            }
        }
        for (int t = 0; t < nt; ++t) {
            if (tids[t]) pthread_join(tids[t], NULL);
            if (args[t].rc != 0) rc = -1;
        }
        free(tids); free(args);
        fdv_stats_set(saved);
    }

    if (rc != 0) return rc;
    ++e->n;
    if (e->use_rc) {
        size_t used = fdv_enc_bytes(e) - before;
        if (repeat) fdv_rc_skip(&e->rc, used);
        else        fdv_rc_update(&e->rc, is_intra, used);
    }
    return 0;
}

fdv_encoder *fdv_enc_open(int w, int h, int qp, int keyint) {
    if (w <= 0 || h <= 0 || (w & 15) || (h & 15) || w > 8192 || h > 8192) return NULL;
    if (qp < 0 || qp > 51) return NULL;
    if (keyint < 0) keyint = 0;
    if (keyint > 255) keyint = 255;

    fdv_encoder *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    int cw = w / 2, ch = h / 2;
    e->w = w; e->h = h; e->qp = qp; e->keyint = keyint;
    e->fsize = (size_t)w * h + 2 * (size_t)cw * ch;
    e->tmp_cap = (size_t)w * h * 3 + 8192;
    e->tmp = malloc(e->tmp_cap);
    e->ry  = malloc((size_t)w * h);
    e->ru  = malloc((size_t)cw * ch);
    e->rv  = malloc((size_t)cw * ch);
    e->cap = FDV_HEAD + 8 + e->fsize;          /* room for the head and a frame */
    e->out = malloc(e->cap);
    if (!e->tmp || !e->ry || !e->ru || !e->rv || !e->out) goto fail;
    if (fdv_frame_alloc(&e->r0, w, h) != 0) goto fail;
    if (fdv_frame_alloc(&e->r1, w, h) != 0) { fdv_frame_free(&e->r0); goto fail; }
    e->near = &e->r0; e->far = &e->r1;
    e->frame_qp = -1;

    /* Payload header, with the frame count left at zero until finish knows it. */
    size_t o = FDV_HEAD;
    o = fdv_put16(e->out, o, 0);
    o = fdv_put16(e->out, o, (unsigned)w);
    o = fdv_put16(e->out, o, (unsigned)h);
    e->out[o++] = (uint8_t)qp;
    e->out[o++] = (uint8_t)keyint;
    e->len = o;

    FDV_LOG(FDV_LOG_INFO, "encode", "stream open %dx%d qp=%d keyint=%d (%d MB/frame)",
            w, h, qp, keyint, (w / 16) * (h / 16));
    return e;
fail:
    free(e->tmp); free(e->ry); free(e->ru); free(e->rv); free(e->out); free(e);
    return NULL;
}

fdv_encoder *fdv_enc_open_tiled(int w, int h, int qp, int keyint,
                                int band_mbrows, int nthreads) {
    if (w <= 0 || h <= 0 || (w & 15) || (h & 15) || w > 8192 || h > 8192) return NULL;
    if (qp < 0 || qp > 51) return NULL;
    if (band_mbrows < 1) return NULL;
    if (keyint < 0) keyint = 0;
    if (keyint > 255) keyint = 255;

    int bh_full = band_mbrows * 16;
    int nbands = ceil_div(h, bh_full);
    if (nbands > 65535) return NULL;
    if (nthreads < 1) nthreads = 1;

    fdv_encoder *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->w = w; e->h = h; e->qp = qp; e->keyint = keyint;
    e->fsize = (size_t)w * h + 2 * (size_t)(w / 2) * (h / 2);
    e->nbands = nbands; e->band_mbrows = band_mbrows; e->nthreads = nthreads;
    e->frame_qp = -1;
    e->band    = calloc((size_t)nbands, sizeof(*e->band));
    e->bandbuf = calloc((size_t)nbands, sizeof(*e->bandbuf));
    if (!e->band || !e->bandbuf) { fdv_enc_close(e); return NULL; }

    for (int b = 0; b < nbands; ++b) {
        int y0 = b * bh_full;
        int bh = (y0 + bh_full <= h) ? bh_full : (h - y0);
        e->band[b] = fdv_enc_open(w, bh, qp, keyint);
        e->bandbuf[b] = malloc((size_t)w * bh + 2 * (size_t)(w / 2) * (bh / 2));
        if (!e->band[b] || !e->bandbuf[b]) { fdv_enc_close(e); return NULL; }
    }
    FDV_LOG(FDV_LOG_INFO, "encode",
            "stream open %dx%d qp=%d keyint=%d  %d band(s) of %d MB row(s), %d thread(s)",
            w, h, qp, keyint, nbands, band_mbrows, nthreads);
    return e;
}

void fdv_enc_close(fdv_encoder *e) {
    if (!e) return;
    if (e->band) {
        for (int b = 0; b < e->nbands; ++b) {
            fdv_enc_close(e->band[b]);
            free(e->bandbuf[b]);
        }
        free(e->band); free(e->bandbuf);
    }
    fdv_frame_free(&e->r0);
    fdv_frame_free(&e->r1);
    free(e->tmp); free(e->ry); free(e->ru); free(e->rv); free(e->out);
    free(e);
}

int fdv_enc_count(const fdv_encoder *e) { return e ? e->n : 0; }

size_t fdv_enc_bytes(const fdv_encoder *e) {
    if (!e) return 0;
    if (!e->band) return e->len - FDV_HEAD;
    size_t t = 0;
    for (int b = 0; b < e->nbands; ++b) t += e->band[b]->len - FDV_HEAD;
    return t;
}

static int enc_push(fdv_encoder *e, const uint8_t *i420, int force_skip) {
    if (!e || (!i420 && !force_skip)) return -1;
    if (e->n >= 65535) return -1;              /* the header's frame count is u16 */

    int w = e->w, h = e->h, cw = w / 2, ch = h / 2;
    const uint8_t *cy = i420;
    const uint8_t *cu = cy ? cy + (size_t)w * h : NULL;
    const uint8_t *cv = cu ? cu + (size_t)cw * ch : NULL;
    int f = e->n;
    int is_intra = (f == 0) || e->key_due || (e->keyint > 0 && f % e->keyint == 0);
    /* A held frame has no source to code, so it cannot carry a key frame.
     * Rather than lose the seek point, remember one was due and let the next
     * real frame carry it. */
    if (force_skip) { e->key_due = is_intra; is_intra = 0; }
    else e->key_due = 0;
    /* The table chain and the adaptive coder's history both restart at a key
     * frame, so seeking to one needs nothing before it. */
    if (is_intra) memset(&e->tc, 0, sizeof e->tc);

    /* Where this frame's QP comes from: a band-parallel parent, the rate
     * controller, or the fixed value the encoder was opened with. */
    int fqp;
    if (e->frame_qp >= 0)   fqp = e->frame_qp;
    else if (e->use_rc)     fqp = force_skip ? e->rc.used_qp
                                             : fdv_rc_pick(&e->rc, is_intra);
    else                    fqp = e->qp;

    fdv_frame_counters_reset();
    double t0 = fdv_now_ms();
    size_t blob;
    if (is_intra) {
        blob = iframe_encode(cy, cu, cv, w, h, fqp, e->tmp, e->tmp_cap,
                             e->ry, e->ru, e->rv);
    } else {
        const fdv_frame *refs[2] = {e->near, (e->nref >= 2) ? e->far : e->near};
        int navail = (e->nref >= 2) ? 2 : 1;
        blob = pframe_encode(cy, cu, cv, w, h, refs, navail, fqp, e->tmp, e->tmp_cap,
                             e->ry, e->ru, e->rv, force_skip, &e->tc);
        fdv_deblock_plane(e->ry, w, h, w, fqp);
        fdv_deblock_plane(e->ru, cw, ch, cw, fqp);
        fdv_deblock_plane(e->rv, cw, ch, cw, fqp);
    }
    /* A key frame is the one frame the loop has no evidence for -- at the start
     * of a stream it is coded from a guess, and a guess on the fine side costs
     * an eighth of the stream in a single frame, in exactly the place the
     * buffer has least room. If it lands far past its allocation, code it once
     * more, coarser. One retry, key frames only, and only under rate control:
     * the reference pool has not been updated yet, so nothing else has to be
     * unwound. */
    if (is_intra && e->use_rc && blob > 0 && fqp < e->rc.qpmax) {
        double over = (double)blob * 8.0 / (e->rc.last_target > 1.0 ? e->rc.last_target : 1.0);
        if (over > 2.0) {
            int retry = fqp + (int)lround(6.0 * log2(over));
            if (retry > e->rc.qpmax) retry = e->rc.qpmax;
            FDV_LOG(FDV_LOG_FRAME, "rc", "key frame %.1fx over budget, requantizing %d -> %d",
                    over, fqp, retry);
            size_t again = iframe_encode(cy, cu, cv, w, h, retry, e->tmp, e->tmp_cap,
                                         e->ry, e->ru, e->rv);
            if (again > 0) { blob = again; fqp = retry; e->rc.used_qp = retry; }
        }
    }

    double ms = fdv_now_ms() - t0;
    if (blob == 0) return -1;

    /* Grow geometrically: a recording has no known length, so the buffer has to
     * be able to keep up without a realloc per frame. */
    if (e->len + 4 + blob > e->cap) {
        size_t want = e->cap * 2;
        while (want < e->len + 4 + blob) want *= 2;
        uint8_t *bigger = realloc(e->out, want);
        if (!bigger) return -1;
        e->out = bigger; e->cap = want;
    }
    e->len = fdv_put32(e->out, e->len, (uint32_t)blob);
    memcpy(e->out + e->len, e->tmp, blob);
    e->len += blob;

    int nmb = (w / 16) * (h / 16);
    fdv_frame_stat *st = stat_next(f, is_intra);
    if (st) {
        st->bytes = blob; st->ms = ms; st->qp = fqp;
        st->entropy_mode = fdv_entropy_mode;
        st->sym_struct = fdv_sym_struct; st->sym_coeff = fdv_sym_coeff;
        if (is_intra) st->mb_intra = nmb;
        else {
            st->mb_skip = fdv_mb_tally[0]; st->mb_inter16 = fdv_mb_tally[1];
            st->mb_intra = fdv_mb_tally[2]; st->mb_inter8 = fdv_mb_tally[3];
        }
    }
    FDV_LOG(FDV_LOG_FRAME, "encode", "frame %-4d %c  %7zu B  %8.2f ms",
            f, is_intra ? 'I' : 'P', blob, ms);

    if (e->use_rc) {
        if (force_skip) fdv_rc_skip(&e->rc, blob);
        else            fdv_rc_update(&e->rc, is_intra, blob);
    }

    load_ref(e->far, e->ry, e->ru, e->rv, w, h);
    fdv_frame *t = e->near; e->near = e->far; e->far = t;
    e->nref = is_intra ? 1 : (e->nref < 2 ? e->nref + 1 : 2);
    ++e->n;
    return 0;
}

int fdv_enc_frame(fdv_encoder *e, const uint8_t *i420) {
    if (!e || !i420) return -1;
    if (e->band) return e->n >= 65535 ? -1 : bands_push(e, i420, 0);
    return enc_push(e, i420, 0);
}

int fdv_enc_set_bitrate(fdv_encoder *e, int bits_per_second, int fps, double bufsecs) {
    if (!e || bits_per_second <= 0 || e->n != 0) return -1;
    fdv_rc_init(&e->rc, bits_per_second, fps, e->w, e->h, e->keyint, bufsecs, 0, 0);
    e->use_rc = 1;
    FDV_LOG(FDV_LOG_INFO, "encode", "abr %d bps @ %d fps, %.1fs buffer",
            bits_per_second, fps > 0 ? fps : 30, bufsecs > 0.0 ? bufsecs : 1.0);
    return 0;
}

/* Re-emit the frame already in the reference pool. Costs no analysis and a few
 * hundred bytes, which is what lets a recorder hold a slot to keep time. */
int fdv_enc_repeat(fdv_encoder *e) {
    if (!e || e->n == 0) return -1;             /* nothing to repeat yet */
    if (e->band) return e->n >= 65535 ? -1 : bands_push(e, NULL, 1);
    return enc_push(e, NULL, 1);
}

/* A band's payload, with its frame count filled in the way finish would. */
static const uint8_t *band_payload(fdv_encoder *b, size_t *len) {
    fdv_put16(b->out, FDV_HEAD, (unsigned)b->n);
    *len = b->len - FDV_HEAD;
    return b->out + FDV_HEAD;
}

uint8_t *fdv_enc_finish(fdv_encoder *e, int fps, size_t *len) {
    if (!e) return NULL;
    if (e->n == 0) { fdv_enc_close(e); return NULL; }

    if (e->band) {
        /* Same layout fdv_vtile_encode writes, so the same readers open it. */
        size_t total = 12;
        for (int b = 0; b < e->nbands; ++b) total += 4 + (e->band[b]->len - FDV_HEAD);
        size_t cap = FDV_HEAD + total + 64;
        uint8_t *buf = malloc(cap);
        if (!buf) { fdv_enc_close(e); return NULL; }
        size_t o = FDV_HEAD;
        o = fdv_put16(buf, o, (unsigned)e->w);
        o = fdv_put16(buf, o, (unsigned)e->h);
        o = fdv_put16(buf, o, (unsigned)e->n);
        buf[o++] = (uint8_t)e->qp;
        buf[o++] = (uint8_t)e->keyint;
        o = fdv_put16(buf, o, (unsigned)e->nbands);
        o = fdv_put16(buf, o, (unsigned)e->band_mbrows);
        for (int b = 0; b < e->nbands; ++b) {
            size_t bl; const uint8_t *bp = band_payload(e->band[b], &bl);
            o = fdv_put32(buf, o, (uint32_t)bl);
            memcpy(buf + o, bp, bl);
            o += bl;
        }
        size_t plen = o - FDV_HEAD;
        size_t flen = fdv_wrap(buf, cap, FDV_TILED, fps, buf + FDV_HEAD, plen);
        FDV_LOG(FDV_LOG_INFO, "encode",
                "stream done  %d frame(s), %zu B payload across %d band(s)",
                e->n, plen, e->nbands);
        fdv_enc_close(e);
        if (!flen) { free(buf); return NULL; }
        if (len) *len = flen;
        return buf;
    }

    /* The one thing a streaming writer cannot know at the start. */
    size_t o = FDV_HEAD;
    fdv_put16(e->out, o, (unsigned)e->n);

    size_t plen = e->len - FDV_HEAD;
    size_t flen = fdv_wrap(e->out, e->cap, FDV_VIDEO, fps, e->out + FDV_HEAD, plen);
    if (!flen) { fdv_enc_close(e); return NULL; }

    FDV_LOG(FDV_LOG_INFO, "encode", "stream done  %d frame(s), %zu B payload", e->n, plen);
    uint8_t *buf = e->out;
    e->out = NULL;                              /* ownership passes to the caller */
    if (len) *len = flen;
    fdv_enc_close(e);
    return buf;
}

#endif /* FDV_IMPLEMENTATION_INCLUDED */
#endif /* FDV_IMPLEMENTATION */
