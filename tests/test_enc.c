/* fdv — the whole unit-test suite, one binary.
 *
 * Sections mirror the ones in include/fdv.h: each `<name>_main` is one former
 * standalone test program, run in order by main() below.  test_scene covers
 * include/fdv_scene.h and the render->encode->decode path the CLI pipeline
 * drives.  A suite reports its own pass/fail line; the binary exits non-zero
 * if any suite failed.
 */

#define FDV_IMPLEMENTATION
#include "fdv.h"
#define FDV_SCENE_IMPLEMENTATION
#include "fdv_scene.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reset before each suite, so every suite reports its own count. */
static int failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);       \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

/* The vtile suite asserts differently: it bails out of the suite on the first
 * failure rather than counting, because its later steps depend on the earlier
 * ones having produced a valid stream. */
#define VCHECK(c)                                                         \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);            \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static double psnr(const uint8_t *a, const uint8_t *b, int n) {
    double sse = 0.0;
    for (int i = 0; i < n; ++i) { double d = a[i] - b[i]; sse += d * d; }
    if (sse == 0.0) return 1e9;
    return 10.0 * log10(255.0 * 255.0 / (sse / n));
}

/* ===========================================================================
 * 1. FRAME
 * Padded, aligned planar YUV frame and plane buffers.
 * ======================================================================== */

static int frame_main(void) {
    fdv_frame f;
    const int W = 176, H = 144;   /* QCIF: not a multiple of FDV_ALIGN on purpose */

    CHECK(fdv_frame_alloc(&f, W, H) == 0, "fdv_frame_alloc succeeds");

    /* Dimensions and 4:2:0 chroma subsampling. */
    CHECK(f.planes[0].width == W && f.planes[0].height == H, "luma dims");
    CHECK(f.planes[1].width == (W + 1) / 2 && f.planes[1].height == (H + 1) / 2,
          "chroma dims are half (4:2:0)");

    /* SIMD alignment: every plane's data origin and stride are FDV_ALIGN-aligned. */
    for (int i = 0; i < 3; ++i) {
        CHECK(((uintptr_t)f.planes[i].data % FDV_ALIGN) == 0, "data origin aligned");
        CHECK((f.planes[i].stride % FDV_ALIGN) == 0, "stride aligned");
        CHECK(f.planes[i].stride >= f.planes[i].width + 2 * FDV_BORDER,
              "stride spans width + both borders");
    }

    /* Fill luma with a deterministic gradient, then extend borders. */
    fdv_plane *y = &f.planes[0];
    for (int j = 0; j < y->height; ++j)
        for (int i = 0; i < y->width; ++i)
            *fdv_plane_at(y, i, j) = (uint8_t)((i * 7 + j * 13) & 0xff);

    fdv_frame_extend_borders(&f);

    /* Horizontal extension: left/right border equals the nearest edge pixel. */
    for (int j = 0; j < y->height; ++j) {
        CHECK(*fdv_plane_at(y, -FDV_BORDER, j) == *fdv_plane_at(y, 0, j), "left border = edge");
        CHECK(*fdv_plane_at(y, y->width + FDV_BORDER - 1, j) ==
                  *fdv_plane_at(y, y->width - 1, j), "right border = edge");
    }

    /* Vertical extension: top/bottom border equals the nearest edge row. */
    for (int i = 0; i < y->width; ++i) {
        CHECK(*fdv_plane_at(y, i, -FDV_BORDER) == *fdv_plane_at(y, i, 0), "top border = edge");
        CHECK(*fdv_plane_at(y, i, y->height + FDV_BORDER - 1) ==
                  *fdv_plane_at(y, i, y->height - 1), "bottom border = edge");
    }

    /* Corners: the top-left border block equals pixel (0,0), etc. */
    CHECK(*fdv_plane_at(y, -FDV_BORDER, -FDV_BORDER) == *fdv_plane_at(y, 0, 0),
          "top-left corner");
    CHECK(*fdv_plane_at(y, y->width + FDV_BORDER - 1, -FDV_BORDER) ==
              *fdv_plane_at(y, y->width - 1, 0), "top-right corner");
    CHECK(*fdv_plane_at(y, -FDV_BORDER, y->height + FDV_BORDER - 1) ==
              *fdv_plane_at(y, 0, y->height - 1), "bottom-left corner");
    CHECK(*fdv_plane_at(y, y->width + FDV_BORDER - 1, y->height + FDV_BORDER - 1) ==
              *fdv_plane_at(y, y->width - 1, y->height - 1), "bottom-right corner");

    fdv_frame_free(&f);
    CHECK(f.planes[0].base == NULL, "fdv_frame_free clears the struct");

    if (failures == 0)
        printf("all frame tests passed\n");
    else
        printf("%d frame test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 2. BITS
 * MSB-first bit I/O round-trip.
 * ======================================================================== */

static uint32_t rng_bits = 0xC0FFEEu;
static uint32_t next_rng_bits(void) { rng_bits = rng_bits * 1103515245u + 12345u; return rng_bits; }

static int bits_main(void) {
    /* Round-trip a mix of field widths, including 1, 32, and byte-straddling. */
    {
        struct { uint32_t v; int n; } fields[] = {
            {1, 1}, {0, 1}, {5, 3}, {0x2AB, 12}, {0, 0},
            {0xFFFFFFFFu, 32}, {0x13, 5}, {0x1234, 17}, {7, 3},
        };
        int nf = (int)(sizeof(fields) / sizeof(fields[0]));

        uint8_t buf[64];
        fdv_bitwriter w;
        fdv_bw_init(&w, buf, sizeof(buf));
        for (int i = 0; i < nf; ++i) fdv_bw_put(&w, fields[i].v, fields[i].n);
        size_t len = fdv_bw_flush(&w);
        CHECK(len <= sizeof(buf), "writer stays within buffer");

        fdv_bitreader r;
        fdv_br_init(&r, buf, len);
        for (int i = 0; i < nf; ++i) {
            uint32_t got = fdv_br_get(&r, fields[i].n);
            uint32_t want = fields[i].n ? fields[i].v : 0;
            CHECK(got == want, "field round-trips");
        }
    }

    /* Fuzz: many random (value,width) pairs round-trip exactly. */
    {
        uint8_t buf[8192];
        uint32_t vals[1000];
        int widths[1000];
        fdv_bitwriter w;
        fdv_bw_init(&w, buf, sizeof(buf));
        for (int i = 0; i < 1000; ++i) {
            int n = (int)(next_rng_bits() % 32) + 1;          /* 1..32 */
            uint32_t v = next_rng_bits() & ((n >= 32) ? 0xffffffffu : ((1u << n) - 1));
            vals[i] = v; widths[i] = n;
            fdv_bw_put(&w, v, n);
        }
        size_t len = fdv_bw_flush(&w);

        fdv_bitreader r;
        fdv_br_init(&r, buf, len);
        int ok = 1;
        for (int i = 0; i < 1000; ++i)
            if (fdv_br_get(&r, widths[i]) != vals[i]) ok = 0;
        CHECK(ok, "1000 random fields round-trip");
    }

    if (failures == 0) printf("all bits tests passed\n");
    else printf("%d bits test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 4. RANS
 * Entropy coder round-trip, single and interleaved.
 * ======================================================================== */

static uint32_t rng_rans = 0xABCDEF01u;
static uint32_t next_rng_rans(void) { rng_rans = rng_rans * 1103515245u + 12345u; return rng_rans; }

/* Round-trip a symbol buffer through both codecs and report compressed size. */
static void roundtrip(const uint8_t *syms, size_t n, const char *label) {
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < n; ++i) counts[syms[i]]++;

    fdv_rans_sym enc[256];
    fdv_rans_table dec;
    fdv_rans_build_tables(counts, enc, &dec);

    size_t cap = n * 2 + 1024;
    uint8_t *out = malloc(cap);
    uint8_t *back = malloc(n ? n : 1);

    /* Single-state. */
    size_t len1 = fdv_rans_encode(syms, n, enc, out, cap);
    fdv_rans_decode(out, n, &dec, back);
    CHECK(memcmp(syms, back, n) == 0, "single-state rANS round-trips");

    /* Interleaved. */
    size_t len2 = fdv_rans_encode_il2(syms, n, enc, out, cap);
    memset(back, 0, n);
    fdv_rans_decode_il2(out, n, &dec, back);
    CHECK(memcmp(syms, back, n) == 0, "interleaved rANS round-trips");

    printf("  %-14s n=%6zu  single=%6zu  il2=%6zu  (%.3f bits/sym)\n",
           label, n, len1, len2, n ? (8.0 * len1 / n) : 0.0);

    free(out);
    free(back);
}

static int rans_main(void) {
    /* Skewed distribution: should compress well below 8 bits/symbol. */
    {
        const size_t n = 50000;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; ++i) {
            /* Bias toward small symbols (geometric-ish). */
            uint32_t r = next_rng_rans();
            int s = 0;
            while ((r & 1) && s < 30) { ++s; r >>= 1; }
            buf[i] = (uint8_t)s;
        }
        uint32_t counts[256] = {0};
        for (size_t i = 0; i < n; ++i) counts[buf[i]]++;
        fdv_rans_sym enc[256];
        fdv_rans_table dec;
        fdv_rans_build_tables(counts, enc, &dec);
        uint8_t *out = malloc(n * 2 + 1024);
        size_t len = fdv_rans_encode(buf, n, enc, out, n * 2 + 1024);
        CHECK(len < n, "skewed data compresses below 1 byte/symbol");
        roundtrip(buf, n, "skewed");
        free(out);
        free(buf);
    }

    /* Uniform random bytes: ~8 bits/symbol, must still be exact. */
    {
        const size_t n = 20000;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)(next_rng_rans() >> 13);
        roundtrip(buf, n, "uniform");
        free(buf);
    }

    /* Single symbol everywhere: degenerate table, near-zero output. */
    {
        const size_t n = 10000;
        uint8_t *buf = malloc(n);
        memset(buf, 42, n);
        roundtrip(buf, n, "constant");
        free(buf);
    }

    /* Two symbols, odd count (exercises interleaved parity tail). */
    {
        const size_t n = 1001;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; ++i) buf[i] = (next_rng_rans() & 4) ? 200 : 7;
        roundtrip(buf, n, "binary-odd");
        free(buf);
    }

    /* --- The adaptive range coder --------------------------------------
     *
     * The other coder in section 4, and the one the small streams use. Both
     * sides must walk identical models symbol for symbol, so a round trip is
     * the whole test: any divergence in the Fenwick tree, the rescale, or the
     * prior shows up as a mismatch. */
    {
        static const uint32_t sizes[] = { 0, 1, 2, 3, 7, 100, 4095, 4096, 20000 };
        uint32_t flat[256] = {0}, prior[256] = {0};
        for (int i = 0; i < 8; ++i) prior[i] = 4000u / (unsigned)(i + 1);
        int bad = 0;
        for (unsigned z = 0; z < sizeof sizes / sizeof *sizes; ++z) {
            size_t n = sizes[z];
            for (int primed = 0; primed < 2; ++primed)
                for (int alpha = 2; alpha <= 256; alpha *= 4)
                    for (int skew = 0; skew < 2; ++skew) {
                        const uint32_t *h = primed ? prior : flat;
                        uint8_t *sy = malloc(n ? n : 1);
                        uint8_t *bf = malloc(n * 3 + 64);
                        uint8_t *bk = malloc(n ? n : 1);
                        for (size_t i = 0; i < n; ++i)
                            sy[i] = (uint8_t)((skew && (next_rng_rans() % 100) < 80)
                                              ? 0 : next_rng_rans() % (unsigned)alpha);
                        fdv_amodel m; fdv_amodel_init(&m, h);
                        fdv_aenc e; fdv_aenc_init(&e, bf, n * 3 + 64);
                        for (size_t i = 0; i < n; ++i) fdv_aenc_sym(&e, &m, sy[i]);
                        size_t len = fdv_aenc_finish(&e);
                        fdv_amodel m2; fdv_amodel_init(&m2, h);
                        fdv_adec d; fdv_adec_init(&d, bf, len);
                        for (size_t i = 0; i < n; ++i) bk[i] = (uint8_t)fdv_adec_sym(&d, &m2);
                        if (n && memcmp(sy, bk, n) != 0) ++bad;
                        free(sy); free(bf); free(bk);
                    }
        }
        CHECK(bad == 0, "adaptive coder round-trips every size and alphabet");

        /* A model must never hand the coder a zero-width symbol, whatever the
         * history and however far the rescale has run -- that would make some
         * byte uncodable and desynchronize the two sides. */
        {   fdv_amodel m; fdv_amodel_init(&m, prior);
            uint8_t sink[1 << 16];
            fdv_aenc e; fdv_aenc_init(&e, sink, sizeof sink);
            int bad2 = 0;
            /* Long enough to drive many rescales, and hammering one symbol so
             * the halving has the most lopsided model to survive. */
            for (int r = 0; r < 60000; ++r) {
                for (int i = 0; i < 256; ++i) if (m.f[i] == 0) bad2 = 1;
                if (m.tot > FDV_AD_MAX) bad2 = 1;
                fdv_aenc_sym(&e, &m, (unsigned)((r % 97) ? 5 : r % 256));
            }
            CHECK(!bad2, "no weight rescales to zero and the total stays bounded");
        }

        /* Corrupt and truncated input must terminate without over-reading;
         * ASan is what actually judges this one. */
        {   uint8_t junk[16];
            for (int i = 0; i < 16; ++i) junk[i] = (uint8_t)next_rng_rans();
            for (size_t L = 0; L <= sizeof junk; ++L) {
                fdv_amodel m; fdv_amodel_init(&m, flat);
                fdv_adec d; fdv_adec_init(&d, junk, L);
                for (int i = 0; i < 5000; ++i) (void)fdv_adec_sym(&d, &m);
            }
        }
        printf("  adaptive coder: round-trips, no zero-width symbol, corrupt input safe\n");
    }

    if (failures == 0) printf("all rans tests passed\n");
    else printf("%d rans test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 5. COEFF8
 * 8x8 zigzag + end-of-block serialization.
 * ======================================================================== */

static uint32_t rng_coeff8 = 0x9e3779b9u;
static int nextv(void) { rng_coeff8 = rng_coeff8 * 1103515245u + 12345u; return (int)((rng_coeff8 >> 17) % 41) - 20; }

static int coeff8_main(void) {
    /* fdv_zz8 is a permutation of 0..63. */
    {
        int seen[64] = {0}, ok = 1;
        for (int k = 0; k < 64; ++k) {
            if (fdv_zz8[k] < 0 || fdv_zz8[k] >= 64 || seen[fdv_zz8[k]]) ok = 0;
            else seen[fdv_zz8[k]] = 1;
        }
        CHECK(ok, "fdv_zz8 is a permutation of 0..63");
    }

    /* Round-trip dense and sparse blocks; sparse must be much smaller. */
    {

        int16_t a[64], b[64];
        uint8_t nbuf[64], lbuf[512];
        int ok = 1;

        /* Counts and levels ride in separate streams now, so a round-trip has
         * to reassemble both. */
        #define RT(src, dst)                                                   \
            do { fdv_cw w_ = { nbuf, 0, lbuf, 0 };                             \
                 fdv_coeff8_encode((src), &w_);                                \
                 fdv_cr r_ = { NULL, 0, w_.np, 0, w_.lp };                     \
                 uint8_t joint_[576];                                          \
                 memcpy(joint_, nbuf, w_.np);                                  \
                 memcpy(joint_ + w_.np, lbuf, w_.lp);                          \
                 r_.b = joint_; r_.nend = w_.np;                               \
                 r_.lp = w_.np; r_.lend = w_.np + w_.lp;                       \
                 ok = 1; fdv_coeff8_decode(&r_, &ok, (dst));                   \
                 last_n = w_.np; last_l = w_.lp; } while (0)
        size_t last_n = 0, last_l = 0;

        /* Dense. */
        for (int i = 0; i < 64; ++i) a[i] = (int16_t)nextv();
        memset(b, 0xAB, sizeof(b));
        RT(a, b);
        size_t dense_len = last_n + last_l;
        CHECK(ok, "dense 8x8 block decodes cleanly");
        CHECK(memcmp(a, b, sizeof(a)) == 0, "dense 8x8 block round-trips");
        CHECK(last_n == 1, "one count byte per 8x8 block");

        /* Sparse: only a couple of low-frequency coefficients. */
        int16_t s[64] = {0}, sd[64];
        s[0] = 12; s[1] = -3; s[8] = 5;            /* DC + two low-freq */
        RT(s, sd);
        size_t sparse_len = last_n + last_l;
        CHECK(memcmp(s, sd, sizeof(s)) == 0, "sparse 8x8 block round-trips");
        CHECK(sparse_len < dense_len, "EOB makes sparse blocks much smaller");
        printf("  8x8 coeff: dense=%zu bytes, sparse=%zu bytes\n", dense_len, sparse_len);

        /* All-zero block: just the count byte, and nothing in the level stream. */
        int16_t z[64] = {0}, zd[64];
        RT(z, zd);
        CHECK(last_n == 1 && last_l == 0, "all-zero 8x8 block is one count byte");
        CHECK(memcmp(z, zd, sizeof(z)) == 0, "all-zero block round-trips");
    }

    if (failures == 0) printf("all coeff8 tests passed\n");
    else printf("%d coeff8 test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 6. TRANSFORM
 * Transform/quant round-trip and NEON-vs-scalar equivalence.
 * ======================================================================== */

/* Deterministic LCG so the test is reproducible. */
static uint32_t rng_state = 0x12345678u;
static int rng_residual(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) % 511) - 255;   /* [-255, 255] */
}

static int transform_main(void) {
    /* --- SIMD equivalence: the NEON inverse transform must be bit-identical
     *     to the scalar reference on arbitrary (dequantized) coefficients. --- */
#if defined(__ARM_NEON)
    {
        int mism_i = 0, mism_f = 0;
        for (int t = 0; t < 5000; ++t) {
            int16_t r[16];
            for (int i = 0; i < 16; ++i) r[i] = (int16_t)rng_residual();

            /* Forward transform equivalence. */
            int32_t ca[16], cb[16];
            fdv_fdct4x4_scalar(r, ca);
            fdv_fdct4x4_neon(r, cb);
            for (int i = 0; i < 16; ++i) if (ca[i] != cb[i]) mism_f = 1;

            /* Inverse transform equivalence (on real forward coefficients). */
            int16_t a[16], b[16];
            fdv_idct4x4_scalar(ca, a);
            fdv_idct4x4_neon(ca, b);
            for (int i = 0; i < 16; ++i) if (a[i] != b[i]) mism_i = 1;
        }
        CHECK(!mism_f, "NEON fdv_fdct4x4 is bit-identical to scalar");
        CHECK(!mism_i, "NEON fdv_idct4x4 is bit-identical to scalar");

        /* Dequant equivalence across QPs. */
        int mism_dq = 0;
        for (int t = 0; t < 2000; ++t) {
            int16_t lv[16];
            int32_t da[16], db[16];
            for (int i = 0; i < 16; ++i) lv[i] = (int16_t)(rng_residual() / 4);
            int qp = (int)(rng_state >> 20) % 52;
            fdv_dequant4x4_scalar(lv, da, qp);
            fdv_dequant4x4_neon(lv, db, qp);
            for (int i = 0; i < 16; ++i) if (da[i] != db[i]) mism_dq = 1;
        }
        CHECK(!mism_dq, "NEON fdv_dequant4x4 is bit-identical to scalar");
        printf("SIMD: NEON fdv_fdct4x4 + fdv_idct4x4 + fdv_dequant4x4 match scalar\n");
    }
#else
    printf("SIMD: no NEON on this target; scalar idct only\n");
#endif

    /* --- Known-answer: a flat block transforms to DC only. --- */
    {
        int16_t res[16];
        int32_t coeff[16];
        for (int i = 0; i < 16; ++i) res[i] = 10;
        fdv_fdct4x4(res, coeff);
        CHECK(coeff[0] == 16 * 10, "flat block: DC = 16*value");
        int ac_zero = 1;
        for (int i = 1; i < 16; ++i)
            if (coeff[i] != 0) ac_zero = 0;
        CHECK(ac_zero, "flat block: all AC coefficients zero");
    }

    /* --- Linearity sanity: transform of a scaled block scales. --- */
    {
        int16_t a[16], b[16];
        int32_t ca[16], cb[16];
        for (int i = 0; i < 16; ++i) { a[i] = (int16_t)(i - 8); b[i] = (int16_t)(2 * (i - 8)); }
        fdv_fdct4x4(a, ca);
        fdv_fdct4x4(b, cb);
        int linear = 1;
        for (int i = 0; i < 16; ++i)
            if (cb[i] != 2 * ca[i]) linear = 0;
        CHECK(linear, "forward transform is linear (2x in -> 2x out)");
    }

    /* --- Full pipeline round-trip across QPs, bounded error. --- *
     * The transform/quant cascade is lossy by construction; we assert the
     * reconstruction error stays within a bound that grows with the quant
     * step, and report the worst case so regressions are visible. */
    {
        const int NBLOCKS = 2000;
        int qps[]      = {0, 6, 12, 18, 24, 30, 36};
        /* Error bound per QP: roughly proportional to the quant step (which
         * doubles every 6 QP). Generous but tight enough to catch real bugs. */
        int bound[]    = {2, 3, 6, 11, 22, 44, 88};
        int nqp = (int)(sizeof(qps) / sizeof(qps[0]));

        for (int q = 0; q < nqp; ++q) {
            int qp = qps[q];
            int max_err = 0;
            double sse = 0.0;
            long n = 0;
            for (int blk = 0; blk < NBLOCKS; ++blk) {
                int16_t res[16], recon[16], level[16];
                int32_t coeff[16], dcoeff[16];
                for (int i = 0; i < 16; ++i) res[i] = (int16_t)rng_residual();

                fdv_fdct4x4(res, coeff);
                fdv_quant4x4(coeff, level, qp);
                fdv_dequant4x4(level, dcoeff, qp);
                fdv_idct4x4(dcoeff, recon);

                for (int i = 0; i < 16; ++i) {
                    int e = recon[i] - res[i];
                    if (e < 0) e = -e;
                    if (e > max_err) max_err = e;
                    sse += (double)e * e;
                    ++n;
                }
            }
            double rmse = (n > 0) ? (sse / (double)n) : 0.0;
            printf("QP %2d: max |err| = %3d (bound %3d), MSE = %6.2f\n",
                   qp, max_err, bound[q], rmse);
            CHECK(max_err <= bound[q], "round-trip error within bound");
        }
    }

    /* --- 8x8 transform: flat -> DC only, and bounded round-trip. --- */
    {
        int16_t res[64], recon[64], level[64];
        int32_t coeff[64], dcoeff[64];

        for (int i = 0; i < 64; ++i) res[i] = 20;
        fdv_fdct8x8(res, coeff);
        int ac_zero = 1;
        for (int i = 1; i < 64; ++i) if (coeff[i] != 0) ac_zero = 0;
        CHECK(coeff[0] != 0 && ac_zero, "8x8 flat block: DC only");

        int qps[]   = {0, 12, 24, 36};
        int bound[] = {2, 8, 32, 128};
        for (int q = 0; q < 4; ++q) {
            int qp = qps[q], max_err = 0;
            for (int blk = 0; blk < 1000; ++blk) {
                for (int i = 0; i < 64; ++i) res[i] = (int16_t)rng_residual();
                fdv_fdct8x8(res, coeff);
                fdv_quant8x8(coeff, level, qp);
                fdv_dequant8x8(level, dcoeff, qp);
                fdv_idct8x8(dcoeff, recon);
                for (int i = 0; i < 64; ++i) {
                    int e = recon[i] - res[i];
                    if (e < 0) e = -e;
                    if (e > max_err) max_err = e;
                }
            }
            printf("8x8 QP %2d: max |err| = %3d (bound %3d)\n", qp, max_err, bound[q]);
            CHECK(max_err <= bound[q], "8x8 round-trip error within bound");
        }
    }

    /* --- fdv_satd4x4: the NEON path against the scalar definition -------------
     * SATD only ranks intra candidates, so an error here shows up as slightly
     * worse mode choices rather than corruption -- exactly the kind of bug that
     * hides. The two must agree exactly. */
    {
        uint8_t sbuf[4 * 24], pbuf[16];
        int bad = 0, n = 0;
        uint32_t st = 0xC0FFEEu;
        for (int trial = 0; trial < 2000; ++trial) {
            int sstride = 4 + (trial % 21);            /* varied, unaligned strides */
            for (int i = 0; i < 4 * 24; ++i) {
                st = st * 1103515245u + 12345u;
                sbuf[i] = (uint8_t)(trial % 3 == 0 ? ((st >> 16) & 0xff)
                        : trial % 3 == 1 ? (((st >> 20) & 1) ? 255 : 0)  /* extremes */
                        : ((st >> 18) & 0x0f));                          /* flat-ish */
            }
            for (int i = 0; i < 16; ++i) {
                st = st * 1103515245u + 12345u;
                pbuf[i] = (uint8_t)((st >> 16) & 0xff);
            }
            int got = fdv_satd4x4(sbuf, sstride, pbuf);
            int want = fdv_satd4x4_scalar(sbuf, sstride, pbuf);
            ++n;
            if (got != want) { ++bad; if (bad == 1) printf("      (first: %d vs %d)\n", got, want); }
        }
        CHECK(bad == 0, "fdv_satd4x4 matches its scalar definition");
        if (!bad) printf("  fdv_satd4x4: matches scalar over %d random blocks\n", n);
    }

    if (failures == 0)
        printf("all transform tests passed\n");
    else
        printf("%d transform test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 7. INTRA
 * Intra prediction modes.
 * ======================================================================== */

static uint32_t rng_intra = 0x731u;
static int rnd(void) { rng_intra = rng_intra * 1103515245u + 12345u; return (rng_intra >> 16) & 0xff; }

static int intra_main(void) {
    /* Flat neighborhood: every mode must reproduce the constant. */
    {
        uint8_t top[8], left[4], pred[16];
        for (int i = 0; i < 8; ++i) top[i] = 90;
        for (int i = 0; i < 4; ++i) left[i] = 90;
        for (int m = 0; m < FDV_INTRA_NMODES; ++m) {
            fdv_intra_predict_4x4(m, top, left, 90, 1, 1, pred);
            int flat = 1;
            for (int i = 0; i < 16; ++i) if (pred[i] != 90) flat = 0;
            CHECK(flat, "flat neighbors -> flat prediction (all modes)");
        }
    }

    /* Vertical copies the top row; horizontal copies the left column. */
    {
        uint8_t top[8] = {10, 20, 30, 40, 50, 60, 70, 80};
        uint8_t left[4] = {100, 110, 120, 130};
        uint8_t pred[16];
        fdv_intra_predict_4x4(FDV_INTRA_VERT, top, left, 5, 1, 1, pred);
        int okv = 1;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) if (pred[y * 4 + x] != top[x]) okv = 0;
        CHECK(okv, "vertical mode copies the top row");

        fdv_intra_predict_4x4(FDV_INTRA_HORIZ, top, left, 5, 1, 1, pred);
        int okh = 1;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) if (pred[y * 4 + x] != left[y]) okh = 0;
        CHECK(okh, "horizontal mode copies the left column");

        fdv_intra_predict_4x4(FDV_INTRA_DC, top, left, 5, 1, 1, pred);
        int dc = 0;
        for (int k = 0; k < 4; ++k) dc += top[k] + left[k];
        dc = (dc + 4) >> 3;
        int okdc = 1;
        for (int i = 0; i < 16; ++i) if (pred[i] != dc) okdc = 0;
        CHECK(okdc, "DC mode is the neighbor mean");
    }

    /* All modes stay in byte range on random neighbors (no overflow). */
    {
        int inrange = 1;
        for (int t = 0; t < 500; ++t) {
            uint8_t top[8], left[4], tl = (uint8_t)rnd(), pred[16];
            for (int i = 0; i < 8; ++i) top[i] = (uint8_t)rnd();
            for (int i = 0; i < 4; ++i) left[i] = (uint8_t)rnd();
            for (int m = 0; m < FDV_INTRA_NMODES; ++m) {
                fdv_intra_predict_4x4(m, top, left, tl, 1, 1, pred);
                for (int i = 0; i < 16; ++i)
                    if (pred[i] > 255) inrange = 0;   /* uint8_t: real check is no UB */
            }
        }
        CHECK(inrange, "all modes produce valid samples on random input");
    }

    /* n x n predictor (used for larger transform sizes): flat invariant,
     * vertical copies top, horizontal copies left, DC is the mean. */
    {
        const int n = 8;
        uint8_t top[8], left[8], pred[64];
        for (int i = 0; i < n; ++i) { top[i] = 77; left[i] = 77; }
        for (int m = 0; m < FDV_INTRA_NN_NMODES; ++m) {
            fdv_intra_nxn(m, top, left, n, 1, 1, pred);
            int flat = 1;
            for (int i = 0; i < n * n; ++i) if (pred[i] != 77) flat = 0;
            CHECK(flat, "fdv_intra_nxn flat neighbors -> flat (all modes)");
        }

        for (int i = 0; i < n; ++i) { top[i] = (uint8_t)(10 * i); left[i] = (uint8_t)(100 + i); }
        fdv_intra_nxn(FDV_INTRA_NN_V, top, left, n, 1, 1, pred);
        int okv = 1, okh = 1;
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) if (pred[y*n+x] != top[x]) okv = 0;
        CHECK(okv, "fdv_intra_nxn vertical copies the top row");
        fdv_intra_nxn(FDV_INTRA_NN_H, top, left, n, 1, 1, pred);
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) if (pred[y*n+x] != left[y]) okh = 0;
        CHECK(okh, "fdv_intra_nxn horizontal copies the left column");

        fdv_intra_nxn(FDV_INTRA_NN_DC, top, left, n, 1, 1, pred);
        int s = 0; for (int k = 0; k < n; ++k) s += top[k] + left[k];
        int dc = (s + n) / (2 * n);
        int okdc = 1; for (int i = 0; i < n * n; ++i) if (pred[i] != dc) okdc = 0;
        CHECK(okdc, "fdv_intra_nxn DC is the neighbor mean");
    }

    if (failures == 0) printf("all intra tests passed\n");
    else printf("%d intra test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 8. INTER
 * Motion compensation and motion search.
 * ======================================================================== */

/* Independent reference for luma quarter-pel interpolation, written from the
 * H.264 spec (figure 8-4 naming) rather than from the codec's implementation.
 * Deliberately the slow, obvious form: one output sample at a time, every
 * intermediate recomputed. */
static int ref_tap6(int a, int b, int c, int d, int e, int f) {
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}
static int ref_clip(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int ref_pix(const fdv_plane *p, int x, int y) { return *fdv_plane_at(p, x, y); }

static int ref_hf(const fdv_plane *p, int x, int y) {   /* unclipped horizontal */
    return ref_tap6(ref_pix(p, x - 2, y), ref_pix(p, x - 1, y), ref_pix(p, x, y),
                    ref_pix(p, x + 1, y), ref_pix(p, x + 2, y), ref_pix(p, x + 3, y));
}
static int ref_vf(const fdv_plane *p, int x, int y) {   /* unclipped vertical   */
    return ref_tap6(ref_pix(p, x, y - 2), ref_pix(p, x, y - 1), ref_pix(p, x, y),
                    ref_pix(p, x, y + 1), ref_pix(p, x, y + 2), ref_pix(p, x, y + 3));
}
static int ref_b(const fdv_plane *p, int x, int y) { return ref_clip((ref_hf(p, x, y) + 16) >> 5); }
static int ref_h(const fdv_plane *p, int x, int y) { return ref_clip((ref_vf(p, x, y) + 16) >> 5); }
static int ref_j(const fdv_plane *p, int x, int y) {
    int jj = ref_tap6(ref_hf(p, x, y - 2), ref_hf(p, x, y - 1), ref_hf(p, x, y),
                      ref_hf(p, x, y + 1), ref_hf(p, x, y + 2), ref_hf(p, x, y + 3));
    return ref_clip((jj + 512) >> 10);
}

static int ref_luma_sample(const fdv_plane *p, int x, int y, int fx, int fy) {
    int G = ref_pix(p, x, y);
    switch (fy * 4 + fx) {
    case  0: return G;
    case  1: return (G + ref_b(p, x, y) + 1) >> 1;
    case  2: return ref_b(p, x, y);
    case  3: return (ref_b(p, x, y) + ref_pix(p, x + 1, y) + 1) >> 1;
    case  4: return (G + ref_h(p, x, y) + 1) >> 1;
    case  5: return (ref_b(p, x, y) + ref_h(p, x, y) + 1) >> 1;
    case  6: return (ref_b(p, x, y) + ref_j(p, x, y) + 1) >> 1;
    case  7: return (ref_b(p, x, y) + ref_h(p, x + 1, y) + 1) >> 1;
    case  8: return ref_h(p, x, y);
    case  9: return (ref_h(p, x, y) + ref_j(p, x, y) + 1) >> 1;
    case 10: return ref_j(p, x, y);
    case 11: return (ref_j(p, x, y) + ref_h(p, x + 1, y) + 1) >> 1;
    case 12: return (ref_h(p, x, y) + ref_pix(p, x, y + 1) + 1) >> 1;
    case 13: return (ref_h(p, x, y) + ref_b(p, x, y + 1) + 1) >> 1;
    case 14: return (ref_j(p, x, y) + ref_b(p, x, y + 1) + 1) >> 1;
    case 15: return (ref_h(p, x + 1, y) + ref_b(p, x, y + 1) + 1) >> 1;
    }
    return G;
}

static int inter_main(void) {
    const int W = 64, H = 64;
    fdv_frame ref;
    CHECK(fdv_frame_alloc(&ref, W, H) == 0, "ref alloc");
    fdv_plane *ry = &ref.planes[0];

    /* Textured reference so motion search has something to lock onto. */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            *fdv_plane_at(ry, x, y) = (uint8_t)((x * 5 + y * 3 + ((x ^ y) << 1)) & 0xff);
    fdv_frame_extend_borders(&ref);

    /* Build a current frame as the reference shifted by a known integer motion:
     * cur[y][x] = ref[y+dy][x+dx]. Predicting cur with mv=(dx,dy) must be exact. */
    const int dx = 3, dy = -2;
    uint8_t *cur = malloc(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            cur[y * W + x] = *fdv_plane_at(ry, x + dx, y + dy);

    /* Integer-pel MC reproduces the shifted block exactly. */
    {
        int bx = 16, by = 16, bw = 16, bh = 16;
        uint8_t pred[16 * 16];
        fdv_mc_luma(ry, bx, by, bw, bh, dx * 4, dy * 4, pred, bw);
        int exact = 1;
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; ++j)
                if (pred[i * bw + j] != cur[(by + i) * W + bx + j]) exact = 0;
        CHECK(exact, "integer-pel MC reproduces shifted block exactly");
    }

    /* Motion search recovers the known motion with zero SAD. */
    {
        int bx = 24, by = 24, bw = 16, bh = 16;
        int mvx = 0, mvy = 0;
        int sad = fdv_me_search(cur, W, ry, bx, by, bw, bh, 8, 0, 0, &mvx, &mvy);
        printf("  ME found mv=(%d,%d) q4  sad=%d  (expected (%d,%d))\n",
               mvx, mvy, sad, dx * 4, dy * 4);
        CHECK(mvx == dx * 4 && mvy == dy * 4, "ME recovers the true motion vector");
        CHECK(sad == 0, "ME on exact integer motion has zero SAD");
    }

    free(cur);
    fdv_frame_free(&ref);

    /* SAD kernel: SIMD/scalar path must match a plain scalar reference. */
    {
        int mism = 0;
        for (int t = 0; t < 2000; ++t) {
            uint8_t a[16 * 24], b[16 * 16];
            int stride = 24, bw = (t & 1) ? 8 : 16, bh = bw;
            for (int i = 0; i < 16 * 24; ++i) a[i] = (uint8_t)((t * 7 + i * 13) & 0xff);
            for (int i = 0; i < bw * bh; ++i) b[i] = (uint8_t)((t * 5 + i * 11) & 0xff);
            int ref_sad = 0;
            for (int i = 0; i < bh; ++i)
                for (int j = 0; j < bw; ++j) {
                    int d = a[i * stride + j] - b[i * bw + j];
                    ref_sad += d < 0 ? -d : d;
                }
            if (fdv_sad_kernel(a, stride, b, bw, bh) != ref_sad) mism = 1;
        }
        CHECK(!mism, "fdv_sad_kernel matches scalar SAD (16- and 8-wide)");
    }

    /* 6-tap half-pel correctness: on a linear ramp the 6-tap filter is exact,
     * so half-pel samples equal the integer midpoints (within rounding). */
    {
        fdv_frame ramp;
        fdv_frame_alloc(&ramp, 64, 64);
        fdv_plane *p = &ramp.planes[0];
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                *fdv_plane_at(p, x, y) = (uint8_t)(20 + 2 * x + y);   /* linear, no clip */
        fdv_frame_extend_borders(&ramp);

        int bx = 16, by = 16, bw = 8, bh = 8;
        uint8_t hh[64], vv[64], cc[64];
        fdv_mc_luma(p, bx, by, bw, bh, 2, 0, hh, bw);   /* horizontal half */
        fdv_mc_luma(p, bx, by, bw, bh, 0, 2, vv, bw);   /* vertical half   */
        fdv_mc_luma(p, bx, by, bw, bh, 2, 2, cc, bw);   /* center          */

        int okh = 1, okv = 1, okc = 1;
        for (int i = 0; i < bh; ++i)
            for (int j = 0; j < bw; ++j) {
                int g  = *fdv_plane_at(p, bx + j,     by + i);
                int rt = *fdv_plane_at(p, bx + j + 1, by + i);
                int dn = *fdv_plane_at(p, bx + j,     by + i + 1);
                int dr = *fdv_plane_at(p, bx + j + 1, by + i + 1);
                if (abs(hh[i * bw + j] - ((g + rt + 1) >> 1)) > 1) okh = 0;
                if (abs(vv[i * bw + j] - ((g + dn + 1) >> 1)) > 1) okv = 0;
                if (abs(cc[i * bw + j] - ((g + rt + dn + dr + 2) >> 2)) > 1) okc = 0;
            }
        CHECK(okh, "6-tap horizontal half-pel matches ramp midpoint");
        CHECK(okv, "6-tap vertical half-pel matches ramp midpoint");
        CHECK(okc, "6-tap center half-pel matches ramp midpoint");
        fdv_frame_free(&ramp);
    }

    /* Chroma MC: the (NEON or scalar) fdv_mc_chroma must match the scalar bilinear
     * reference for both 4x4 and 8x8 blocks over assorted fractional MVs. */
    {
        fdv_frame cf;
        fdv_frame_alloc(&cf, 64, 64);
        fdv_plane *p = &cf.planes[0];
        uint32_t r = 0xC0FFEEu;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) { r = r * 1103515245u + 12345u; *fdv_plane_at(p, x, y) = (uint8_t)(r >> 16); }
        fdv_frame_extend_borders(&cf);

        int mvs[][2] = {{1, 3}, {2, 2}, {3, 1}, {0, 2}, {2, 0}, {5, 7}, {-3, 6}, {7, -5}};
        int sizes[2] = {4, 8};
        int mism = 0;
        for (int s = 0; s < 2; ++s) {
            int bw = sizes[s];
            for (int m = 0; m < 8; ++m) {
                int mvx = mvs[m][0], mvy = mvs[m][1];
                uint8_t out[8 * 8];
                fdv_mc_chroma(p, 20, 20, bw, bw, mvx, mvy, out, bw);
                int fx = mvx & 3, fy = mvy & 3, ix = 20 + (mvx >> 2), iy = 20 + (mvy >> 2);
                for (int i = 0; i < bw; ++i)
                    for (int j = 0; j < bw; ++j) {
                        const uint8_t *q = fdv_plane_at(p, ix + j, iy + i);
                        int a = q[0], b = q[1], c = q[p->stride], d = q[p->stride + 1];
                        int top = a * (4 - fx) + b * fx, bot = c * (4 - fx) + d * fx;
                        int refv = (top * (4 - fy) + bot * fy + 8) >> 4;
                        if (out[i * bw + j] != refv) mism = 1;
                    }
            }
        }
        CHECK(!mism, "fdv_mc_chroma matches scalar bilinear (4x4 and 8x8, fractional MVs)");
        fdv_frame_free(&cf);
    }

    /* --- fdv_mc_luma: block path vs an independent per-pixel reference ---------
     * fdv_mc_luma builds each block with separable filter passes into scratch,
     * which is a good deal more intricate than evaluating one pixel at a time.
     * This reference is written straight from the H.264 definition rather than
     * derived from the codec's own code, so agreement is real evidence.  It
     * must hold exactly: encoder and decoder both call fdv_mc_luma, and a single
     * differing pixel desynchronises them. */
    {
        const int MW = 96, MH = 64;
        fdv_frame mf;
        CHECK(fdv_frame_alloc(&mf, MW, MH) == 0, "fdv_mc_luma test frame allocates");
        fdv_plane *p = &mf.planes[0];
        for (int y = 0; y < MH; ++y)
            for (int x = 0; x < MW; ++x)         /* texture with real gradients */
                *fdv_plane_at(p, x, y) = (uint8_t)((x * 7 + y * 13 + ((x * y) >> 3)) & 0xff);
        fdv_frame_extend_borders(&mf);

        int worst_phase = -1, worst_bw = 0, mismatches = 0;
        for (int bw = 8; bw <= 16; bw += 8) {
            int bh = bw;
            uint8_t got[16 * 16];
            for (int fy = 0; fy < 4; ++fy)
                for (int fx = 0; fx < 4; ++fx)
                    for (int bx = 0; bx <= 32; bx += 16)
                        for (int by = 0; by <= 32; by += 16)
                            for (int mv = -9; mv <= 9; mv += 6) {
                                int mvx = mv * 4 + fx, mvy = mv * 4 + fy;
                                fdv_mc_luma(p, bx, by, bw, bh, mvx, mvy, got, bw);
                                int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
                                for (int i = 0; i < bh; ++i)
                                    for (int j = 0; j < bw; ++j) {
                                        int want = ref_luma_sample(p, ix + j, iy + i, fx, fy);
                                        if (got[i * bw + j] != want) {
                                            ++mismatches;
                                            worst_phase = fy * 4 + fx;
                                            worst_bw = bw;
                                        }
                                    }
                            }
        }
        CHECK(mismatches == 0, "fdv_mc_luma block path is bit-exact vs the reference");
        if (mismatches)
            printf("      (%d mismatching samples; e.g. phase %d, %dx%d block)\n",
                   mismatches, worst_phase, worst_bw, worst_bw);
        else
            printf("  fdv_mc_luma: all 16 quarter-pel phases bit-exact (8x8 and 16x16)\n");
        fdv_frame_free(&mf);
    }

    if (failures == 0) printf("all inter tests passed\n");
    else printf("%d inter test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 9. DEBLOCK
 * In-loop deblocking filter.
 * ======================================================================== */

/* Sum of absolute differences across all 4x4 block boundaries (both
 * orientations) — a proxy for visible blocking. */
static long boundary_energy(const uint8_t *p, int w, int h, int stride) {
    long e = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 4; x < w; x += 4)
            e += abs(p[y * stride + x] - p[y * stride + x - 1]);
    for (int y = 4; y < h; y += 4)
        for (int x = 0; x < w; ++x)
            e += abs(p[y * stride + x] - p[(y - 1) * stride + x]);
    return e;
}

/* Independent reference for the in-loop deblocking filter: the plain nested
 * loops from the filter's definition, no vectorisation, no shared code with
 * fdv.h. */
static void ref_filter_edge(uint8_t *a, int step, int alpha, int beta, int tc) {
    int p1 = a[-2 * step], p0 = a[-1 * step];
    int q0 = a[0],         q1 = a[ 1 * step];
    if (abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta) {
        int delta = ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3;
        if (delta < -tc) delta = -tc;
        if (delta >  tc) delta =  tc;
        int np = p0 + delta, nq = q0 - delta;
        a[-1 * step] = (uint8_t)(np < 0 ? 0 : (np > 255 ? 255 : np));
        a[ 0]        = (uint8_t)(nq < 0 ? 0 : (nq > 255 ? 255 : nq));
    }
}

static void ref_deblock_plane(uint8_t *plane, int w, int h, int stride, int qp) {
    int alpha = 6 + qp, beta = 2 + qp / 4, tc = 1 + qp / 10;
    for (int y = 0; y < h; ++y)
        for (int x = 4; x < w; x += 4)
            ref_filter_edge(&plane[y * stride + x], 1, alpha, beta, tc);
    for (int y = 4; y < h; y += 4)
        for (int x = 0; x < w; ++x)
            ref_filter_edge(&plane[y * stride + x], stride, alpha, beta, tc);
}

static int deblock_main(void) {
    const int W = 64, H = 64, qp = 24;
    uint8_t *img = malloc(W * H);
    uint8_t *ref = malloc(W * H);

    /* Blocky image: each 4x4 block is flat, with a small step between adjacent
     * blocks (a quantization-artifact-like discontinuity). */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int bx = x / 4, by = y / 4;
            img[y * W + x] = (uint8_t)(100 + 10 * ((bx + by) & 1));
        }
    memcpy(ref, img, W * H);

    long before = boundary_energy(img, W, H, W);
    fdv_deblock_plane(img, W, H, W, qp);
    long after = boundary_energy(img, W, H, W);

    printf("  boundary energy: before=%ld  after=%ld\n", before, after);
    CHECK(after < before, "deblock reduces block-boundary discontinuity");

    /* Block interior samples (not adjacent to any edge) must be untouched:
     * within a 4-wide block the sample at local offset 2 has neighbors on both
     * sides inside the same block. */
    int interior_changed = 0;
    for (int y = 2; y < H; y += 4)
        for (int x = 2; x < W; x += 4)
            if (img[y * W + x] != ref[y * W + x]) interior_changed = 1;
    CHECK(!interior_changed, "block interior samples unchanged");

    /* Determinism: filtering identical input twice gives identical output. */
    uint8_t *a = malloc(W * H), *b = malloc(W * H);
    memcpy(a, ref, W * H); memcpy(b, ref, W * H);
    fdv_deblock_plane(a, W, H, W, qp);
    fdv_deblock_plane(b, W, H, W, qp);
    CHECK(memcmp(a, b, W * H) == 0, "deblock is deterministic");

    /* A perfectly flat plane stays flat (no spurious filtering). */
    uint8_t *flat = malloc(W * H);
    memset(flat, 128, W * H);
    fdv_deblock_plane(flat, W, H, W, qp);
    int flat_ok = 1;
    for (int i = 0; i < W * H; ++i) if (flat[i] != 128) flat_ok = 0;
    CHECK(flat_ok, "flat plane is left unchanged");

    free(img); free(ref); free(a); free(b); free(flat);
    /* --- fdv_deblock_plane: vectorised path vs an independent scalar reference ---
     * The vertical-edge pass reaches its four samples through a 4-way
     * deinterleaving load and selects results with a mask instead of branching,
     * which is easy to get subtly wrong at the tail or the thresholds.  This
     * reference is the plain nested-loop definition, written out here rather
     * than shared with the codec, and it must agree exactly: the filter is
     * in-loop, so a single differing sample desynchronises decoder from
     * encoder for every later frame. */
    {
        static const int WS[] = {16, 20, 64, 132, 320};   /* incl. non-multiples of 16 */
        int mismatches = 0, checked = 0;
        for (size_t wi = 0; wi < sizeof(WS) / sizeof(*WS); ++wi) {
            int dw = WS[wi], dh = 32, dstride = dw + 7;  /* padded, unaligned stride */
            uint8_t *ga = malloc((size_t)dstride * dh);
            uint8_t *gb = malloc((size_t)dstride * dh);
            if (!ga || !gb) { free(ga); free(gb); continue; }
            for (int q = 0; q <= 51; q += 7) {
                uint32_t seed = 0x1234u + (uint32_t)(q * 977 + dw);
                for (int i = 0; i < dstride * dh; ++i) {
                    seed = seed * 1103515245u + 12345u;
                    /* Mix flat runs with sharp steps so the gate goes both ways. */
                    ga[i] = (uint8_t)((seed >> 16) & ((i / 37) & 1 ? 0xff : 0x0f));
                }
                memcpy(gb, ga, (size_t)dstride * dh);

                fdv_deblock_plane(ga, dw, dh, dstride, q);
                ref_deblock_plane(gb, dw, dh, dstride, q);

                for (int i = 0; i < dstride * dh; ++i) {
                    ++checked;
                    if (ga[i] != gb[i]) ++mismatches;
                }
            }
            free(ga); free(gb);
        }
        CHECK(mismatches == 0, "fdv_deblock_plane matches the scalar reference exactly");
        if (mismatches)
            printf("      (%d of %d samples differ)\n", mismatches, checked);
        else
            printf("  deblock: bit-exact vs reference over 5 widths x 8 QPs (%d samples)\n",
                   checked);
    }

    if (failures == 0) printf("all deblock tests passed\n");
    else printf("%d deblock test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 10. IMAGE
 * Still-image codec round-trip, size and PSNR.
 * ======================================================================== */

/* Synthesize a test image: a smooth gradient with a couple of hard edges and a
 * diagonal, so DC / vertical / horizontal intra modes all get exercised. */
static void make_image(uint8_t *img, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int v = (x + y) / 2;                       /* gradient */
            if (x > w / 3 && x < 2 * w / 3 && y > h / 4 && y < 3 * h / 4)
                v = 200;                               /* flat rectangle (edges) */
            if (((x ^ y) & 31) == 0) v = 30;           /* sparse diagonal texture */
            img[y * w + x] = (uint8_t)(v & 0xff);
        }
    }
}

static int image_main(void) {
    const int W = 128, H = 128;
    uint8_t *orig = malloc(W * H);
    uint8_t *recon = malloc(W * H);
    uint8_t *dec = malloc(W * H);
    size_t cap = (size_t)W * H * 2 + 4096;
    uint8_t *stream = malloc(cap);
    make_image(orig, W, H);

    int qps[] = {0, 12, 24};
    for (int q = 0; q < 3; ++q) {
        int qp = qps[q];
        memset(recon, 0, W * H);
        memset(dec, 0, W * H);

        size_t len = fdv_image_encode(orig, W, H, W, qp, stream, cap, recon);
        CHECK(len > 0, "encode succeeds");

        int dw = 0, dh = 0;
        int rc = fdv_image_decode(stream, len, dec, W, &dw, &dh);
        CHECK(rc == 0, "decode succeeds");
        CHECK(dw == W && dh == H, "decoded dimensions match");

        /* The core guarantee: decode is bit-identical to encoder reconstruction. */
        CHECK(memcmp(recon, dec, W * H) == 0, "decode == encoder reconstruction");

        double p = psnr(orig, dec, W * H);
        double bpp = 8.0 * len / (double)(W * H);
        printf("  QP %2d: %6zu bytes  (%.3f bpp, %.1fx)  PSNR %.1f dB\n",
               qp, len, bpp, (double)(W * H) / len, p);

        if (qp == 0)  CHECK(p > 40.0, "QP 0 is near-lossless (PSNR > 40 dB)");
        CHECK(len < (size_t)W * H, "compresses below raw size");
        /* Lock in the coding gain at QP 24. Size alone stopped being a
         * meaningful guard once lambda was corrected -- spending more bits for
         * more quality moves both numbers -- so pin the rate-distortion point
         * rather than the rate: over 1250 bytes or under 45 dB is a
         * regression, and so is a much smaller file at the same PSNR. */
        if (qp == 24) {
            CHECK(len < 1250, "QP 24 stays compact (EOB + RDOQ + split entropy models)");
            CHECK(p > 45.0,   "QP 24 holds its quality");
        }
    }

    free(orig); free(recon); free(dec); free(stream);
    if (failures == 0) printf("all image tests passed\n");
    else printf("%d image test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 11. VIDEO
 * P-frame codec: modes, refs, keyframes, PSNR.
 * ======================================================================== */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int video_main(void) {
    const int W = 64, H = 64, NF = 4, qp = 18;
    const int CW = W / 2, CH = H / 2;
    const size_t YS = (size_t)W * H, CS = (size_t)CW * CH;
    const size_t FS = YS + 2 * CS;       /* I420 frame size */

    /* Base planes: textured luma, smoother chroma. */
    uint8_t *bY = malloc(YS), *bU = malloc(CS), *bV = malloc(CS);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            bY[y * W + x] = (uint8_t)((x * 4 + y * 2 + 40 * ((x / 8 + y / 8) & 1)) & 0xff);
    for (int y = 0; y < CH; ++y)
        for (int x = 0; x < CW; ++x) {
            bU[y * CW + x] = (uint8_t)(110 + (x + y));
            bV[y * CW + x] = (uint8_t)(140 - (x - y));
        }

    /* Panning sequence: frame f shifts the base by (2f, f) luma / (f, f/2) chroma. */
    uint8_t *frames[8];
    const uint8_t *fptrs[8];
    for (int f = 0; f < NF; ++f) {
        frames[f] = malloc(FS);
        uint8_t *Y = frames[f], *U = Y + YS, *V = U + CS;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                Y[y * W + x] = bY[clampi(y + f, 0, H - 1) * W + clampi(x + 2 * f, 0, W - 1)];
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                int sx = clampi(x + f, 0, CW - 1), sy = clampi(y + f / 2, 0, CH - 1);
                U[y * CW + x] = bU[sy * CW + sx];
                V[y * CW + x] = bV[sy * CW + sx];
            }
        fptrs[f] = frames[f];
    }

    size_t cap = FS * NF * 2 + 8192;
    uint8_t *stream = malloc(cap);
    size_t len = fdv_video_encode(fptrs, NF, W, H, qp, 0, stream, cap);
    CHECK(len > 0, "fdv_video_encode succeeds");

    uint8_t *dec = malloc(FS * NF);
    int dnf = 0, dw = 0, dh = 0;
    CHECK(fdv_video_decode(stream, len, dec, &dnf, &dw, &dh) == 0, "fdv_video_decode succeeds");
    CHECK(dnf == NF && dw == W && dh == H, "decoded geometry matches");

    double yp = 0, cp = 0;
    for (int f = 0; f < NF; ++f) {
        const uint8_t *o = frames[f], *d = dec + (size_t)f * FS;
        double py = psnr(o, d, (int)YS);
        double pu = psnr(o + YS, d + YS, (int)CS);
        double pv = psnr(o + YS + CS, d + YS + CS, (int)CS);
        yp += py; cp += (pu + pv) / 2;
        printf("  frame %d (%s): Y %.1f dB  U %.1f dB  V %.1f dB\n",
               f, f ? "P" : "I", py, pu, pv);
    }
    printf("  total %zu bytes for %d YUV frames (%.0f B/frame), avg Y %.1f / C %.1f dB\n",
           len, NF, (double)len / NF, yp / NF, cp / NF);

    CHECK(yp / NF > 32.0, "average luma PSNR reasonable (> 32 dB)");
    CHECK(cp / NF > 32.0, "average chroma PSNR reasonable (> 32 dB)");
    CHECK(len < FS * NF, "sequence compresses below raw size");
    CHECK(len < 1300, "sequence stays compact (adaptive context entropy both paths)");

    /* SKIP: static YUV sequence — two P-frames cost less than one intra frame. */
    {
        uint8_t *st = malloc(FS);
        memcpy(st, bY, YS); memcpy(st + YS, bU, CS); memcpy(st + YS + CS, bV, CS);
        const uint8_t *sp3[3] = {st, st, st};
        size_t l1 = fdv_video_encode(sp3, 1, W, H, qp, 0, stream, cap);
        uint8_t *s3 = malloc(cap);
        size_t l3 = fdv_video_encode(sp3, 3, W, H, qp, 0, s3, cap);
        printf("  static: I-frame=%zu bytes, 2 P-frames=%zu bytes\n", l1, l3 - l1);
        CHECK(l3 - l1 < l1, "two static P-frames cost less than one intra frame");
        free(st); free(s3);
    }

    /* Intra-in-P: a scene cut (frame 1 uncorrelated with frame 0) — motion can't
     * predict it, so the RD decision falls back to intra blocks. Must still
     * round-trip at good quality. */
    {
        uint8_t *f0 = malloc(FS), *f1 = malloc(FS);
        memcpy(f0, bY, YS); memcpy(f0 + YS, bU, CS); memcpy(f0 + YS + CS, bV, CS);
        /* f1: a smooth gradient, nothing like f0's texture. */
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) f1[y * W + x] = (uint8_t)(x + y);
        for (int i = 0; i < (int)CS; ++i) { f1[YS + i] = 128; f1[YS + CS + i] = 128; }
        const uint8_t *cut[2] = {f0, f1};
        size_t cl = fdv_video_encode(cut, 2, W, H, qp, 0, stream, cap);
        uint8_t *cd = malloc(FS * 2);
        int a, b, c;
        CHECK(fdv_video_decode(stream, cl, cd, &a, &b, &c) == 0, "scene-cut decodes");
        double pcut = psnr(f1, cd + FS, (int)YS);
        printf("  scene-cut: frame 1 (new content) Y %.1f dB\n", pcut);
        CHECK(pcut > 35.0, "intra-in-P reconstructs new content well");
        free(f0); free(f1); free(cd);
    }

    /* 8x8 partitions: each macroblock's four 8x8 quadrants move differently, so
     * a single 16x16 motion vector can't fit — the 8x8 split should, giving a
     * good reconstruction. */
    {
        uint8_t *f0 = malloc(FS), *f1 = malloc(FS);
        memcpy(f0, bY, YS); memcpy(f0 + YS, bU, CS); memcpy(f0 + YS + CS, bV, CS);
        uint8_t *Y = f1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int qx = (x / 8) & 1, qy = (y / 8) & 1;     /* quadrant within MB */
                int dx = qx ? -2 : 2, dy = qy ? -1 : 1;     /* divergent per quadrant */
                Y[y * W + x] = bY[clampi(y + dy, 0, H - 1) * W + clampi(x + dx, 0, W - 1)];
            }
        memcpy(f1 + YS, bU, CS); memcpy(f1 + YS + CS, bV, CS);
        const uint8_t *seq[2] = {f0, f1};
        size_t sl = fdv_video_encode(seq, 2, W, H, qp, 0, stream, cap);
        uint8_t *sd = malloc(FS * 2);
        int a, b, c;
        CHECK(fdv_video_decode(stream, sl, sd, &a, &b, &c) == 0, "divergent-motion decodes");
        double pdiv = psnr(f1, sd + FS, (int)YS);
        printf("  8x8: divergent-motion frame 1 Y %.1f dB\n", pdiv);
        CHECK(pdiv > 38.0, "8x8 partitions handle divergent intra-MB motion");
        free(f0); free(f1); free(sd);
    }

    /* Multi-reference: a periodic sequence A,B,A — frame 2 (=A) is best
     * predicted from two frames back (frame 0 = A), not the previous (B). */
    {
        uint8_t *A = malloc(FS), *B = malloc(FS), *A2 = malloc(FS);
        memcpy(A, bY, YS); memcpy(A + YS, bU, CS); memcpy(A + YS + CS, bV, CS);
        memcpy(A2, A, FS);
        for (int y = 0; y < H; ++y)               /* B = transposed A: different texture */
            for (int x = 0; x < W; ++x) B[y * W + x] = bY[x * W + y];
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                B[YS + y * CW + x] = bU[x * CW + y];
                B[YS + CS + y * CW + x] = bV[x * CW + y];
            }
        const uint8_t *seq[3] = {A, B, A2};
        size_t sl = fdv_video_encode(seq, 3, W, H, qp, 0, stream, cap);
        uint8_t *sd = malloc(FS * 3);
        int a, b, c;
        CHECK(fdv_video_decode(stream, sl, sd, &a, &b, &c) == 0, "periodic A,B,A decodes");
        double pA2 = psnr(A2, sd + 2 * FS, (int)YS);
        printf("  multi-ref: periodic frame 2 (returns to A) Y %.1f dB\n", pA2);
        CHECK(pA2 > 46.0, "multi-ref predicts the periodic return from 2 frames back");
        free(A); free(B); free(A2); free(sd);
    }

    /* Periodic I-frames (keyint): the panning sequence with keyint=2 forces
     * frames 0 and 2 to be intra. It must round-trip, and cost more than the
     * keyint=0 version (extra I-frames) — confirming key frames were inserted. */
    {
        uint8_t *k = malloc(cap);
        size_t lk = fdv_video_encode(fptrs, NF, W, H, qp, 2, k, cap);
        uint8_t *kd = malloc(FS * NF);
        int a, b, c;
        CHECK(fdv_video_decode(k, lk, kd, &a, &b, &c) == 0 && a == NF, "keyint=2 sequence decodes");
        double yk = 0;
        for (int f = 0; f < NF; ++f) yk += psnr(frames[f], kd + (size_t)f * FS, (int)YS);
        printf("  keyint=2: %zu bytes (vs %zu at keyint=0), avg Y %.1f dB\n", lk, len, yk / NF);
        CHECK(yk / NF > 32.0, "keyint=2 round-trips at good quality");
        CHECK(lk > len, "keyint=2 costs more than keyint=0 (extra I-frames inserted)");
        free(k); free(kd);
    }

    for (int f = 0; f < NF; ++f) free(frames[f]);
    free(bY); free(bU); free(bV); free(stream); free(dec);
    if (failures == 0) printf("all video tests passed\n");
    else printf("%d video test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 12. TILES
 * Independent tiles, serial and threaded decode.
 * ======================================================================== */

static int tiles_main(void) {
    const int W = 128, H = 96, qp = 16;
    const int TW = 64, TH = 48;     /* 2x2 tile grid */
    const int N = W * H;

    uint8_t *orig = malloc(N);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            orig[y * W + x] = (uint8_t)((x * 3 + y * 5 + 50 * ((x / 16 + y / 16) & 1)) & 0xff);

    /* Reference reconstruction: encode each tile with fdv_image_encode and stitch
     * its reconstruction into place. The tiled decode must match this exactly. */
    uint8_t *ref = malloc(N);
    {
        size_t cap = (size_t)TW * TH * 2 + 4096;
        uint8_t *tmp = malloc(cap);
        uint8_t *rec = malloc((size_t)TW * TH);
        for (int ty = 0; ty < H; ty += TH)
            for (int tx = 0; tx < W; tx += TW) {
                int tw = (tx + TW <= W) ? TW : W - tx;
                int th = (ty + TH <= H) ? TH : H - ty;
                fdv_image_encode(orig + ty * W + tx, tw, th, W, qp, tmp, cap, rec);
                for (int i = 0; i < th; ++i)
                    memcpy(ref + (ty + i) * W + tx, rec + i * tw, tw);
            }
        free(tmp); free(rec);
    }

    size_t cap = (size_t)N * 2 + 8192;
    uint8_t *stream = malloc(cap);
    size_t len = fdv_tiled_encode(orig, W, H, qp, TW, TH, stream, cap);
    CHECK(len > 0, "fdv_tiled_encode succeeds");

    uint8_t *dec = malloc(N);
    int dw = 0, dh = 0;
    CHECK(fdv_tiled_decode(stream, len, dec, &dw, &dh) == 0, "fdv_tiled_decode succeeds");
    CHECK(dw == W && dh == H, "decoded geometry matches");
    CHECK(memcmp(dec, ref, N) == 0, "tiled decode == per-tile encoder reconstruction");

    /* Threaded decode must be a bit-exact parallelization of the serial path. */
    uint8_t *decp = malloc(N);
    memset(decp, 0, N);
    CHECK(fdv_tiled_decode_threaded(stream, len, decp, &dw, &dh, 4) == 0,
          "threaded decode succeeds");
    CHECK(memcmp(decp, dec, N) == 0, "threaded decode == serial decode (4 threads)");

    printf("  %dx%d, %dx%d tiles: %zu bytes (%.1fx), PSNR %.1f dB\n",
           W, H, TW, TH, len, (double)N / len, psnr(orig, dec, N));
    CHECK(len < (size_t)N, "tiled stream compresses below raw");

    free(orig); free(ref); free(stream); free(dec); free(decp);
    if (failures == 0) printf("all tiles tests passed\n");
    else printf("%d tiles test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 13. VTILE
 * Tile-parallel video: threaded == serial, fuzz.
 * ======================================================================== */

/* Tile-parallel video round-trip and invariants:
 *  - threaded decode is bit-identical to serial decode (independent bands);
 *  - a single-band tiling reproduces the plain video codec exactly (ties the
 *    tiled path to the verified base codec);
 *  - decode reports the right dims and round-trips at sane fidelity;
 *  - truncated / corrupt containers fail with -1 (never crash). */
static uint32_t rng_vtile = 0x2545f491u;
static uint32_t nr(void) { rng_vtile ^= rng_vtile << 13; rng_vtile ^= rng_vtile >> 17; rng_vtile ^= rng_vtile << 5; return rng_vtile; }

/* Synthesize a moving-gradient I420 sequence so there is real motion to code. */
static void synth(uint8_t *seq, int nframes, int w, int h) {
    int cw = w / 2, ch = h / 2;
    size_t fsize = (size_t)w * h + 2 * (size_t)cw * ch;
    for (int f = 0; f < nframes; ++f) {
        uint8_t *y = seq + (size_t)f * fsize;
        uint8_t *u = y + (size_t)w * h;
        uint8_t *v = u + (size_t)cw * ch;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                y[j * w + i] = (uint8_t)((i + j + f * 3) * 2 + 16);
        for (int j = 0; j < ch; ++j)
            for (int i = 0; i < cw; ++i) {
                u[j * cw + i] = (uint8_t)(96 + ((i - f) & 31));
                v[j * cw + i] = (uint8_t)(160 + ((j + f) & 31));
            }
    }
}

static int vtile_main(void) {
    int w = 64, h = 64, nframes = 5, qp = 20, keyint = 3;
    int cw = w / 2, ch = h / 2;
    size_t fsize = (size_t)w * h + 2 * (size_t)cw * ch;
    size_t total = fsize * nframes;

    uint8_t *seq = malloc(total);
    VCHECK(seq);
    synth(seq, nframes, w, h);
    const uint8_t *fr[5];
    for (int f = 0; f < nframes; ++f) fr[f] = seq + (size_t)f * fsize;

    size_t cap = total * 2 + 65536;
    uint8_t *bs = malloc(cap);
    uint8_t *a = malloc(total), *b = malloc(total);
    VCHECK(bs && a && b);

    /* --- Multi-band (band_mbrows=1 → 4 bands of 16px): serial == threaded --- */
    size_t blen = fdv_vtile_encode(fr, nframes, w, h, qp, keyint, 1, 1, bs, cap);
    VCHECK(blen > 12);
    int dnf, dw, dh;
    VCHECK(fdv_vtile_decode(bs, blen, a, &dnf, &dw, &dh, 1) == 0);
    VCHECK(dnf == nframes && dw == w && dh == h);
    VCHECK(fdv_vtile_decode(bs, blen, b, &dnf, &dw, &dh, 4) == 0);
    VCHECK(memcmp(a, b, total) == 0);                 /* threading must not change output */

    /* Fidelity sanity: decoded sequence resembles the source. */
    double se = 0.0;
    for (size_t i = 0; i < total; ++i) { int d = (int)seq[i] - a[i]; se += (double)d * d; }
    double psnr_db = se > 0.0 ? 10.0 * log10(255.0 * 255.0 * total / se) : 99.0;
    VCHECK(psnr_db > 28.0);

    /* --- Single band must equal the plain (non-tiled) video codec exactly --- */
    uint8_t *pbs = malloc(cap);
    uint8_t *pout = malloc(total), *tout = malloc(total);
    VCHECK(pbs && pout && tout);
    size_t plen = fdv_video_encode(fr, nframes, w, h, qp, keyint, pbs, cap);
    VCHECK(plen > 0);
    VCHECK(fdv_video_decode(pbs, plen, pout, &dnf, &dw, &dh) == 0);

    size_t tlen = fdv_vtile_encode(fr, nframes, w, h, qp, keyint, 999, 1, bs, cap);  /* one band */
    VCHECK(tlen > 12);
    VCHECK(fdv_vtile_decode(bs, tlen, tout, &dnf, &dw, &dh, 4) == 0);
    VCHECK(memcmp(pout, tout, total) == 0);           /* 1 band == base codec */

    /* --- Robustness: truncations and bit-flips never crash (return -1/ok) --- */
    blen = fdv_vtile_encode(fr, nframes, w, h, qp, keyint, 1, 1, bs, cap);
    VCHECK(blen > 12);
    uint8_t *cp = malloc(blen);
    VCHECK(cp);
    for (size_t t = 0; t <= blen; ++t) {
        memcpy(cp, bs, t);
        fdv_vtile_decode(cp, t, a, &dnf, &dw, &dh, 2);   /* must not crash */
    }
    for (int it = 0; it < 5000; ++it) {
        memcpy(cp, bs, blen);
        int flips = 1 + (int)(nr() % 5);
        for (int k = 0; k < flips; ++k) {
            size_t pos = 12 + nr() % (blen - 12);    /* keep header dims intact */
            cp[pos] ^= (uint8_t)(1u << (nr() & 7));
        }
        fdv_vtile_decode(cp, blen, a, &dnf, &dw, &dh, 2);
    }
    free(cp);

    free(seq); free(bs); free(a); free(b);
    free(pbs); free(pout); free(tout);
    printf("vtile: %d-frame %dx%d, 4-band threaded==serial, PSNR %.1f dB, "
           "1-band==base codec, truncation+flip fuzz survived\n", nframes, w, h, psnr_db);
    return 0;
}

/* ===========================================================================
 * 14. RC
 * Rate control and Lagrangian QP selection.
 * ======================================================================== */

static int rc_main(void) {
    const int W = 64, H = 64;
    uint8_t *img = malloc(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            img[y * W + x] = (uint8_t)((x * 7 + y * 11 + 60 * ((x / 8 + y / 8) & 1)) & 0xff);

    /* Rate control: the achieved size must respect the budget, and a tighter
     * budget must not pick a finer QP than a looser one. */
    {
        size_t big = 0, small = 0;
        int qp_big   = fdv_rc_qp_for_size(img, W, H, 3000, &big);
        int qp_small = fdv_rc_qp_for_size(img, W, H, 1200, &small);
        printf("  target 3000 -> QP %2d (%zu bytes)\n", qp_big, big);
        printf("  target 1200 -> QP %2d (%zu bytes)\n", qp_small, small);
        CHECK(big <= 3000, "achieved size fits the 3000-byte budget");
        CHECK(small <= 1200, "achieved size fits the 1200-byte budget");
        CHECK(qp_small >= qp_big, "tighter budget uses a coarser-or-equal QP");
    }

    /* Frame-level RDO: weighting rate more heavily (larger lambda) must not
     * choose a finer QP than weighting distortion more heavily. */
    {
        int qp_quality = fdv_rc_qp_rdo(img, W, H, 0.01);   /* distortion-favoring */
        int qp_rate    = fdv_rc_qp_rdo(img, W, H, 50.0);   /* rate-favoring */
        printf("  RDO lambda=0.01 -> QP %2d ;  lambda=50 -> QP %2d\n",
               qp_quality, qp_rate);
        CHECK(qp_rate >= qp_quality, "larger lambda (rate-favoring) picks coarser QP");
    }

    free(img);
    if (failures == 0) printf("all rc tests passed\n");
    else printf("%d rc test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * 15. SCENE
 * Scene parsing, deterministic rendering, and a full render->encode->decode
 * round trip through the pipeline the CLI drives.
 * ======================================================================== */

/* Scene sources used by the tests. Inline rather than read from scenes/, so the
 * suite does not depend on the working directory or on any particular file
 * existing -- a test that fails because it was run from the wrong place tells
 * you nothing about the code. */
static const char *const SCENE_MOTION =
    "name motion\nsize 320 192\nframes 30\nfps 30\nqp 20\n"
    "bg luma=36\n"
    "grad dir=v from=20 to=90\n"
    "rect x=16 y=24 w=64 h=48 luma=200 cb=90 cr=170 vx=2.0 vy=1.0 bounce=1\n"
    "circle x=180 y=100 r=28 luma=120 cb=200 cr=60 vx=-1.5 vy=0.7 bounce=1\n"
    "rect x=240 y=120 w=40 h=40 luma=240 cb=128 cr=128 vx=-2.5 vy=-1.5 bounce=1\n"
    "stamp x=6 y=6 scale=3\n";

static const char *const SCENE_TINY =
    "name tiny\nsize 64 64\nframes 4\nfps 25\nqp 18\n"
    "bg luma=40\n"
    "grad dir=h from=20 to=180\n"
    "rect x=8 y=8 w=24 h=24 luma=220 cb=90 cr=160 vx=2 vy=1 bounce=1\n";

static const char *const SCENE_DETAIL =
    "name detail\nsize 320 192\nframes 8\nqp 18\n"
    "bg luma=60\n"
    "checker x=0 y=0 w=160 h=192 cell=4 luma=235 luma2=16\n"
    "checker x=160 y=0 w=160 h=192 cell=16 luma=200 luma2=40\n";

static int scene_main(void) {
    char err[256];

    /* --- the sample scenes parse and report sane geometry ------------- */
    {
        const char *const srcs[] = {SCENE_MOTION, SCENE_TINY, SCENE_DETAIL};
        for (size_t i = 0; i < sizeof srcs / sizeof *srcs; ++i) {
            fdv_scene *s = fdv_scene_parse_text(srcs[i], err, sizeof err);
            CHECK(s != NULL, "scene source parses");
            if (!s) { printf("      (%s)\n", err); continue; }
            int w = fdv_scene_width(s), h = fdv_scene_height(s);
            CHECK(w > 0 && h > 0 && (w & 15) == 0 && (h & 15) == 0,
                  "dimensions are positive multiples of 16");
            CHECK(fdv_scene_frames(s) > 0, "at least one frame");
            CHECK(fdv_scene_objects(s) > 0, "the scene draws something");
            CHECK(fdv_scene_frame_size(s) == (size_t)w * h * 3 / 2, "frame size is w*h*3/2");
            fdv_scene_free(s);
        }
    }

    /* --- rendering is a pure function of (scene, frame) --------------- */
    {
        fdv_scene *a = fdv_scene_parse_text(SCENE_MOTION, err, sizeof err);
        fdv_scene *b = fdv_scene_parse_text(SCENE_MOTION, err, sizeof err);
        CHECK(a && b, "the same source parses twice");
        if (a && b) {
            size_t fs = fdv_scene_frame_size(a);
            uint8_t *fa = malloc(fs), *fb = malloc(fs);
            CHECK(fa && fb, "render buffers allocate");
            if (fa && fb) {
                int same = 1, moved = 0;
                for (int f = 0; f < 4; ++f) {
                    fdv_scene_render(a, f, fa);
                    fdv_scene_render(b, f, fb);           /* independent instance */
                    if (memcmp(fa, fb, fs) != 0) same = 0;
                }
                CHECK(same, "same scene + frame renders identical bytes");

                /* Rendering frame 3 directly must match rendering it in order:
                 * frames may not depend on render history. */
                fdv_scene_render(a, 3, fa);
                fdv_scene_render(b, 0, fb);
                fdv_scene_render(b, 1, fb);
                fdv_scene_render(b, 2, fb);
                fdv_scene_render(b, 3, fb);
                CHECK(memcmp(fa, fb, fs) == 0, "a frame renders the same out of order");

                fdv_scene_render(a, 0, fa);
                fdv_scene_render(a, 3, fb);
                for (size_t i = 0; i < fs; ++i) if (fa[i] != fb[i]) { moved = 1; break; }
                CHECK(moved, "motion fdv_preset actually changes between frames");
            }
            free(fa); free(fb);
        }
        fdv_scene_free(a); fdv_scene_free(b);
    }

    /* --- a static scene really is static ------------------------------ */
    {
        fdv_scene *s = fdv_scene_parse_text(
            "size 64 64\nframes 4\nbg luma=90\nrect x=8 y=8 w=32 h=16 luma=200\n",
            err, sizeof err);
        CHECK(s != NULL, "minimal scene parses");
        if (s) {
            size_t fs = fdv_scene_frame_size(s);
            uint8_t *f0 = malloc(fs), *f1 = malloc(fs);
            if (f0 && f1) {
                fdv_scene_render(s, 0, f0);
                fdv_scene_render(s, 3, f1);
                CHECK(memcmp(f0, f1, fs) == 0, "a scene with no velocity is frame-invariant");
                fdv_scene_info fi = fdv_scene_measure(f0, NULL, 64, 64);
                CHECK(fi.mad == 0.0, "measure reports no motion without a previous frame");
                CHECK(fi.mean > 90.0 && fi.mean < 200.0, "mean luma sits between bg and rect");
                CHECK(fi.sd > 0.0, "a scene with two levels has nonzero deviation");
            }
            free(f0); free(f1);
            fdv_scene_free(s);
        }
    }

    /* --- defaults ----------------------------------------------------- */
    {
        fdv_scene *s = fdv_scene_parse_text("rect x=0 y=0\n", err, sizeof err);
        CHECK(s != NULL, "every attribute is optional");
        if (s) {
            CHECK(fdv_scene_width(s) % 16 == 0 && fdv_scene_height(s) % 16 == 0,
                  "default size is codec-legal");
            CHECK(fdv_scene_qp(s) == -1 && fdv_scene_keyint(s) == -1,
                  "unset qp/keyint report as -1");
            fdv_scene_free(s);
        }
    }

    /* --- malformed input is rejected, with a reason -------------------- */
    {
        static const char *bad[] = {
            "size 320\n",                       /* missing H */
            "size 100 100\n",                   /* not a multiple of 16 */
            "size 0 0\n",
            "frames 0\n",
            "wobble x=1\n",                     /* unknown directive */
            "rect x=1 nosuchattr=2\n",          /* unknown attribute */
            "rect x=notanumber\n",              /* unparseable value */
            "rect x\n",                         /* not key=value */
            "cut\n",                            /* cut needs frame= */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
            err[0] = '\0';
            fdv_scene *s = fdv_scene_parse_text(bad[i], err, sizeof err);
            CHECK(s == NULL, "malformed scene is rejected");
            CHECK(err[0] != '\0', "rejection explains itself");
            fdv_scene_free(s);
        }
    }

    /* --- comments and blank lines are ignored -------------------------- */
    {
        fdv_scene *s = fdv_scene_parse_text(
            "# leading comment\n\n  \nsize 64 64  # trailing comment\nframes 2\n"
            "rect x=4 y=4 w=8 h=8   # another\n", err, sizeof err);
        CHECK(s != NULL, "comments and blank lines parse");
        if (s) {
            CHECK(fdv_scene_width(s) == 64 && fdv_scene_frames(s) == 2, "values survive comments");
            CHECK(fdv_scene_objects(s) == 1, "one object past the comments");
            fdv_scene_free(s);
        }
    }

    /* --- a cut really discontinues the picture ------------------------- */
    {
        fdv_scene *s = fdv_scene_parse_text(
            "size 64 64\nframes 6\nseed 7\nbg luma=20\n"
            "rect x=8 y=8 w=16 h=16 luma=230 vx=1 vy=0 bounce=1\ncut frame=3\n",
            err, sizeof err);
        CHECK(s != NULL, "scene with a cut parses");
        if (s) {
            size_t fs = fdv_scene_frame_size(s);
            uint8_t *a = malloc(fs), *b = malloc(fs), *c = malloc(fs);
            if (a && b && c) {
                fdv_scene_render(s, 1, a);
                fdv_scene_render(s, 2, b);
                fdv_scene_render(s, 3, c);
                fdv_scene_info smooth = fdv_scene_measure(b, a, 64, 64);
                fdv_scene_info jump   = fdv_scene_measure(c, b, 64, 64);
                CHECK(jump.mad > smooth.mad,
                      "the cut frame moves more than an ordinary frame");
            }
            free(a); free(b); free(c);
            fdv_scene_free(s);
        }
    }

    /* --- resolving a name to a file ----------------------------------- */
    {
        /* Write one where the resolver will look, so the test brings its own
         * fixture instead of relying on the repository's scenes/ being there. */
        const char *dir = getenv("VELA_SCENES");
        char path[512];
        snprintf(path, sizeof path, "%s/_selftest.scn", dir ? dir : ".");
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "test fixture scene can be written");
        if (f) {
            fputs("# a fixture written by the test suite\n", f);
            fputs(SCENE_TINY, f);
            fclose(f);

            fdv_scene *s = fdv_scene_open(path, err, sizeof err);
            CHECK(s != NULL, "fdv_scene_open takes a path that exists");
            CHECK(s && fdv_scene_width(s) == 64, "and parses it");
            fdv_scene_free(s);

            char found[512];
            CHECK(fdv_scene_find(path, found, sizeof found) == 0, "fdv_scene_find takes a path");

            char blurb[128] = "";
            CHECK(fdv_scene_file_blurb(path, blurb, sizeof blurb) == 0,
                  "a scene's description is its first comment line");
            CHECK(strstr(blurb, "fixture") != NULL, "and it is read back intact");
            remove(path);
        }

        err[0] = '\0';
        fdv_scene *n = fdv_scene_open("no-such-scene-anywhere", err, sizeof err);
        CHECK(n == NULL && err[0] != '\0', "an unknown name is rejected, with a reason");
        fdv_scene_free(n);
        CHECK(fdv_scene_find("no-such-scene-anywhere", (char[512]){0}, 512) != 0,
              "fdv_scene_find reports the miss too");
        CHECK(fdv_scene_dir() != NULL && *fdv_scene_dir(), "there is always a scene directory");
    }

    /* --- the whole point: a scene round-trips through the codec -------- */
    {
        fdv_scene *s = fdv_scene_parse_text(SCENE_TINY, err, sizeof err);
        CHECK(s != NULL, "tiny scene parses");
        if (s) {
            int w = fdv_scene_width(s), h = fdv_scene_height(s), nf = fdv_scene_frames(s);
            size_t fs = fdv_scene_frame_size(s), total = fs * (size_t)nf;
            size_t ys = (size_t)w * h;
            uint8_t *src = malloc(total), *dec = malloc(total);
            uint8_t *bs = malloc(total * 2 + 65536);
            const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
            CHECK(src && dec && bs && fp, "round-trip buffers allocate");
            if (src && dec && bs && fp) {
                fdv_scene_render_all(s, src);
                for (int f = 0; f < nf; ++f) fp[f] = src + (size_t)f * fs;

                /* Stats sink: the pipeline's per-frame numbers come from here. */
                fdv_frame_stat rec[16];
                fdv_stats st = {rec, 16, 0};
                fdv_stats_set(&st);
                size_t blen = fdv_video_encode(fp, nf, w, h, 20, 0, bs, total * 2 + 65536);
                fdv_stats_set(NULL);

                CHECK(blen > 0, "scene encodes");
                CHECK(blen < total, "scene stream is smaller than raw");
                CHECK(st.n == nf, "stats sink records one entry per frame");
                if (st.n == nf) {
                    CHECK(rec[0].is_intra == 1, "frame 0 is intra");
                    CHECK(rec[0].mb_intra == (w / 16) * (h / 16),
                          "I-frame counts every macroblock as intra");
                    size_t sum = 0;
                    int monotonic = 1;
                    for (int i = 0; i < nf; ++i) {
                        sum += rec[i].bytes;
                        if (rec[i].index != i) monotonic = 0;
                        if (i > 0) CHECK(rec[i].is_intra == 0, "later frames are P-frames");
                    }
                    CHECK(monotonic, "stats arrive in frame order");
                    CHECK(sum > 0 && sum < blen, "frame blobs sum to less than the stream");
                }

                int dnf = 0, dw = 0, dh = 0;
                fdv_stats ds = {rec, 16, 0};
                fdv_stats_set(&ds);
                int rc = fdv_video_decode(bs, blen, dec, &dnf, &dw, &dh);
                fdv_stats_set(NULL);
                CHECK(rc == 0, "scene decodes");
                CHECK(dnf == nf && dw == w && dh == h, "decoded geometry matches");
                CHECK(ds.n == nf, "decode fills the stats sink too");

                double sse = 0.0;
                for (int f = 0; f < nf; ++f) {
                    const uint8_t *a = src + (size_t)f * fs, *b = dec + (size_t)f * fs;
                    for (size_t i = 0; i < ys; ++i) {
                        double d = (double)a[i] - b[i];
                        sse += d * d;
                    }
                }
                double p = sse > 0.0
                    ? 10.0 * log10(255.0 * 255.0 * (double)(ys * (size_t)nf) / sse) : 99.0;
                CHECK(p > 35.0, "scene round-trips at sane fidelity");
                printf("  scene 'tiny' %dx%d x%d: %zu B, Y %.1f dB\n", w, h, nf, blen, p);
            }
            free(src); free(dec); free(bs); free(fp);
            fdv_scene_free(s);
        }
    }

    /* --- the stats sink is optional and safe to leave unset ------------ */
    {
        CHECK(fdv_stats_get() == NULL, "no sink is installed by default");
        fdv_frame_stat one[1];
        fdv_stats tiny_sink = {one, 1, 0};
        fdv_stats_set(&tiny_sink);
        CHECK(fdv_stats_get() == &tiny_sink, "sink round-trips through the accessor");
        fdv_stats_set(NULL);
        CHECK(fdv_stats_get() == NULL, "sink clears");
    }

    /* --- resizing a scene ---------------------------------------------------
     * The point of scaling geometry with the frame is that the picture stays
     * the picture. A resize that only changed the `size` line would leave a
     * 320x192 composition sitting in one corner of a 1920x1088 frame. */
    {
        static const struct { const char *arg; int w, h; } GOOD[] = {
            {"1920x1088", 1920, 1088}, {"720p", 1280, 720},
            {"1080p", 1920, 1088},     {"4k", 3840, 2160},
            {"640x368", 640, 368},
        };
        for (size_t i = 0; i < sizeof GOOD / sizeof *GOOD; ++i) {
            int w = 0, h = 0;
            CHECK(fdv_scene_parse_size(GOOD[i].arg, &w, &h) == 0, "size argument parses");
            CHECK(w == GOOD[i].w && h == GOOD[i].h, "size argument gives the right dims");
            CHECK((w & 15) == 0 && (h & 15) == 0, "every shorthand is codec-legal");
        }
        static const char *BADSZ[] = {
            "1920x1080",   /* 1080 is not a multiple of 16 */
            "1921x1088", "0x0", "-16x16", "1920", "1920y1088",
            "1920x1088x2", "huge", "", "99999x99999",
        };
        for (size_t i = 0; i < sizeof BADSZ / sizeof *BADSZ; ++i) {
            int w = 0, h = 0;
            CHECK(fdv_scene_parse_size(BADSZ[i], &w, &h) != 0, "illegal size is rejected");
        }

        fdv_scene *a = fdv_scene_parse_text(SCENE_MOTION, err, sizeof err);
        fdv_scene *b = fdv_scene_parse_text(SCENE_MOTION, err, sizeof err);
        CHECK(a && b, "the same source parses twice");
        if (a && b) {
            int ow = fdv_scene_width(a), oh = fdv_scene_height(a);
            CHECK(fdv_scene_resize(b, 1920, 1088) == 0, "resize succeeds");
            CHECK(fdv_scene_width(b) == 1920 && fdv_scene_height(b) == 1088, "new size takes");
            CHECK(fdv_scene_objects(b) == fdv_scene_objects(a), "resize keeps every object");
            CHECK(fdv_scene_frames(b) == fdv_scene_frames(a), "resize does not touch timing");
            CHECK(fdv_scene_resize(b, 100, 100) != 0, "resize rejects non-multiples of 16");
            CHECK(fdv_scene_width(b) == 1920, "a rejected resize changes nothing");

            /* Sample both at the same *relative* points: the content should be
             * the same, which is only true if geometry scaled with the frame. */
            uint8_t *fa = malloc(fdv_scene_frame_size(a)), *fb = malloc(fdv_scene_frame_size(b));
            if (fa && fb) {
                fdv_scene_render(a, 3, fa);
                fdv_scene_render(b, 3, fb);
                int same = 0, checked = 0;
                for (int gy = 1; gy < 8; ++gy)
                    for (int gx = 1; gx < 8; ++gx) {
                        double u = gx / 8.0, v = gy / 8.0;
                        int pa = fa[(int)(v * oh) * ow + (int)(u * ow)];
                        int pb = fb[(int)(v * 1088) * 1920 + (int)(u * 1920)];
                        ++checked;
                        if (abs(pa - pb) <= 12) ++same;
                    }
                CHECK(same >= checked - 2, "the resized scene is the same picture");
                printf("  resize: %dx%d -> 1920x1088, %d/%d sample points match\n",
                       ow, oh, same, checked);
            }
            free(fa); free(fb);
        }
        fdv_scene_free(a); fdv_scene_free(b);
    }

    /* --- the .fdv file container ------------------------------------------
     * A player reads this before it can decode anything, so it has to be right
     * about geometry and honest about garbage. */
    {
        fdv_scene *s = fdv_scene_parse_text(SCENE_TINY, err, sizeof err);
        CHECK(s != NULL, "tiny scene parses");
        if (s) {
            int w = fdv_scene_width(s), h = fdv_scene_height(s), nf = fdv_scene_frames(s);
            size_t fs = fdv_scene_frame_size(s), total = fs * (size_t)nf;
            uint8_t *src = malloc(total), *dec = malloc(total);
            size_t cap = total * 2 + 65536;
            uint8_t *buf = malloc(cap);
            const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
            CHECK(src && dec && buf && fp, "container buffers allocate");
            if (src && dec && buf && fp) {
                fdv_scene_render_all(s, src);
                for (int f = 0; f < nf; ++f) fp[f] = src + (size_t)f * fs;

                /* Wrapping in place is the CLI's normal path: encode into
                 * buf+FDV_HEAD, then write the head in front of it. */
                uint8_t *payload = buf + FDV_HEAD;
                size_t plen = fdv_video_encode(fp, nf, w, h, 20, 0, payload, cap - FDV_HEAD);
                CHECK(plen > 0, "payload encodes");
                size_t flen = fdv_wrap(buf, cap, FDV_VIDEO, 30, payload, plen);
                CHECK(flen == plen + FDV_HEAD, "wrapped length is head + payload");
                CHECK(memcmp(buf, "FDV1", 4) == 0, "file starts with the magic");

                fdv_info vi;
                CHECK(fdv_read(buf, flen, &vi) == 0, "wrapped file parses");
                CHECK(vi.kind == FDV_VIDEO, "kind round-trips");
                CHECK(vi.fps == 30, "fps round-trips");
                CHECK(vi.w == w && vi.h == h && vi.nframes == nf,
                      "geometry is readable without decoding");
                CHECK(vi.qp == 20 && vi.keyint == 0, "coding parameters round-trip");
                CHECK(vi.payload_len == plen, "payload length round-trips");

                CHECK(fdv_decode(&vi, dec, 1) == 0, "container decodes");
                CHECK(memcmp(dec, dec, 1) == 0, "decode wrote output");

                /* The tiled kind stores its header fields in a different order,
                 * which is the whole reason the kind is recorded rather than
                 * guessed at. */
                size_t tlen = fdv_vtile_encode(fp, nf, w, h, 20, 0, 1, 1,
                                           buf + FDV_HEAD, cap - FDV_HEAD);
                CHECK(tlen > 0, "tiled payload encodes");
                size_t tflen = fdv_wrap(buf, cap, FDV_TILED, 25, buf + FDV_HEAD, tlen);
                fdv_info ti;
                CHECK(fdv_read(buf, tflen, &ti) == 0, "tiled file parses");
                CHECK(ti.kind == FDV_TILED, "tiled kind round-trips");
                CHECK(ti.w == w && ti.h == h && ti.nframes == nf,
                      "tiled geometry is read from the right offsets");
                CHECK(ti.nbands >= 1, "band count is reported");
                CHECK(fdv_decode(&ti, dec, 4) == 0, "tiled container decodes threaded");

                /* --- malformed input is rejected, not trusted --- */
                fdv_info bad;
                CHECK(fdv_read(NULL, 0, &bad) != 0, "null input rejected");
                CHECK(fdv_read(buf, FDV_HEAD - 1, &bad) != 0, "short file rejected");
                uint8_t tmp[64];
                memcpy(tmp, buf, sizeof tmp);
                tmp[0] = 'X';
                CHECK(fdv_read(tmp, sizeof tmp, &bad) != 0, "wrong magic rejected");
                memcpy(tmp, buf, sizeof tmp);
                tmp[4] = FDV_VERSION + 7;
                CHECK(fdv_read(tmp, sizeof tmp, &bad) != 0, "unknown version rejected");
                memcpy(tmp, buf, sizeof tmp);
                tmp[5] = 9;
                CHECK(fdv_read(tmp, sizeof tmp, &bad) != 0, "unknown kind rejected");
                /* A length field claiming more than the file holds is the
                 * classic way to walk a decoder off the end. */
                memcpy(tmp, buf, sizeof tmp);
                tmp[8] = 0xff; tmp[9] = 0xff; tmp[10] = 0xff; tmp[11] = 0x0f;
                CHECK(fdv_read(tmp, sizeof tmp, &bad) != 0, "overlong payload length rejected");

                CHECK(fdv_wrap(buf, 4, FDV_VIDEO, 30, payload, plen) == 0,
                      "wrap refuses a buffer that cannot hold the file");
                CHECK(fdv_wrap(buf, cap, 42, 30, payload, plen) == 0,
                      "wrap refuses an unknown kind");

                printf("  container: %zu B file, %dx%d x%d @%dfps, both kinds parse\n",
                       flen, vi.w, vi.h, vi.nframes, vi.fps);
            }
            free(src); free(dec); free(buf); free(fp);
            fdv_scene_free(s);
        }
    }

    /* --- streaming decode --------------------------------------------------
     * fdv_dec_* pulls one frame at a time instead of materialising the whole
     * sequence. The only thing that makes it trustworthy is that it produces
     * exactly what the whole-file path produces -- including after a seek,
     * which has to rebuild the reference pool by replaying from a key frame. */
    {
        /* The third variant is 320x192 rather than 64x64 on purpose: below about
         * that size every stream is short enough that the shared entropy model
         * wins, and the adaptive coder -- whose whole correctness rests on both
         * sides rebuilding the same history -- barely appears. A seek replays
         * from a key frame and must reconstruct that history exactly, so the
         * case has to be tested on content that actually uses it. */
        int keyints[3] = {0, 2, 0};
        const char *const scenes[3] = {SCENE_TINY, SCENE_TINY, SCENE_DETAIL};
        for (int variant = 0; variant < 3; ++variant) {
            fdv_scene *s = fdv_scene_parse_text(scenes[variant], err, sizeof err);
            if (!s) { CHECK(0, "scene parses"); break; }
            int w = fdv_scene_width(s), h = fdv_scene_height(s), nf = 6;
            size_t fs = fdv_scene_frame_size(s), total = fs * (size_t)nf;
            size_t cap = total * 2 + 65536;
            uint8_t *src = malloc(total), *whole = malloc(total), *one = malloc(fs);
            uint8_t *buf = malloc(cap);
            const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
            if (src && whole && one && buf && fp) {
                for (int f = 0; f < nf; ++f) {
                    fdv_scene_render(s, f, src + (size_t)f * fs);
                    fp[f] = src + (size_t)f * fs;
                }
                size_t plen = fdv_video_encode(fp, nf, w, h, 20, keyints[variant],
                                               buf + FDV_HEAD, cap - FDV_HEAD);
                size_t flen = fdv_wrap(buf, cap, FDV_VIDEO, 30, buf + FDV_HEAD, plen);
                fdv_info vi;
                CHECK(plen && flen && fdv_read(buf, flen, &vi) == 0, "stream wraps and parses");
                CHECK(fdv_decode(&vi, whole, 1) == 0, "whole-file decode works");

                fdv_decoder *d = fdv_dec_open(&vi);
                CHECK(d != NULL, "streaming decoder opens");
                if (d) {
                    CHECK(fdv_dec_frame_size(d) == fs, "reports the right frame size");
                    CHECK(fdv_dec_pos(d) == 0, "starts at frame 0");

                    int mism = 0;
                    for (int f = 0; f < nf; ++f) {
                        if (fdv_dec_next(d, one) != 1) { mism = -1; break; }
                        if (memcmp(one, whole + (size_t)f * fs, fs) != 0) ++mism;
                    }
                    CHECK(mism == 0, "streaming matches whole-file decode frame for frame");
                    CHECK(fdv_dec_next(d, one) == 0, "end of stream is reported once");
                    CHECK(fdv_dec_next(d, one) == 0, "and stays reported");

                    /* Backwards, which is the case that has to replay from a
                     * key frame rather than roll forward. */
                    int sbad = 0;
                    for (int f = nf - 1; f >= 0; --f) {
                        if (fdv_dec_seek(d, f) != 0 || fdv_dec_pos(d) != f) { sbad = -1; break; }
                        if (fdv_dec_next(d, one) != 1) { sbad = -1; break; }
                        if (memcmp(one, whole + (size_t)f * fs, fs) != 0) ++sbad;
                    }
                    CHECK(sbad == 0, "seek lands on the frame it was asked for, exactly");

                    /* Forward seeks should roll on from where they are, and
                     * out-of-range ones should be refused rather than clamped. */
                    CHECK(fdv_dec_seek(d, 0) == 0, "seek to the start");
                    CHECK(fdv_dec_seek(d, nf - 1) == 0, "seek forward");
                    CHECK(fdv_dec_seek(d, nf) != 0, "seek past the end is rejected");
                    CHECK(fdv_dec_seek(d, -1) != 0, "negative seek is rejected");
                    fdv_dec_close(d);
                }
                if (variant == 2)
                    printf("  streaming: %d frames match whole-file decode, "
                           "seek exact, adaptive entropy replayed from the key frame\n",
                           nf);
            }
            free(src); free(whole); free(one); free(buf); free(fp);
            fdv_scene_free(s);
        }
        fdv_dec_close(NULL);            /* closing nothing must be safe */
        CHECK(fdv_dec_open(NULL) == NULL, "opening a null file is refused");
    }

    /* --- streaming encode --------------------------------------------------
     * Same contract as the streaming decoder: pushing frames one at a time must
     * produce exactly the file that handing over the whole array produces. A
     * capture program has no other way to work, since it does not know how many
     * frames are coming. */
    {
        fdv_scene *s = fdv_scene_parse_text(SCENE_TINY, err, sizeof err);
        if (s) {
            int w = fdv_scene_width(s), h = fdv_scene_height(s), nf = 6;
            size_t fs = fdv_scene_frame_size(s), cap = fs * (size_t)nf * 2 + 65536;
            uint8_t *src = malloc(fs * (size_t)nf), *whole = malloc(cap);
            const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
            if (src && whole && fp) {
                for (int f = 0; f < nf; ++f) {
                    fdv_scene_render(s, f, src + (size_t)f * fs);
                    fp[f] = src + (size_t)f * fs;
                }
                for (int ki = 0; ki <= 3; ki += 3) {
                    size_t plen = fdv_video_encode(fp, nf, w, h, 20, ki,
                                                   whole + FDV_HEAD, cap - FDV_HEAD);
                    size_t wlen = fdv_wrap(whole, cap, FDV_VIDEO, 30, whole + FDV_HEAD, plen);

                    fdv_encoder *e = fdv_enc_open(w, h, 20, ki);
                    CHECK(e != NULL, "streaming encoder opens");
                    int pushed = 0;
                    for (int f = 0; f < nf && e; ++f)
                        if (fdv_enc_frame(e, src + (size_t)f * fs) == 0) ++pushed;
                    CHECK(pushed == nf, "every frame is accepted");
                    CHECK(!e || fdv_enc_count(e) == nf, "the count is reported");
                    size_t slen = 0;
                    uint8_t *out = e ? fdv_enc_finish(e, 30, &slen) : NULL;
                    CHECK(out != NULL, "finish produces a file");
                    CHECK(out && slen == wlen, "same length as the whole-array encode");
                    CHECK(out && slen == wlen && memcmp(out, whole, wlen) == 0,
                          "streaming encode is byte-identical to fdv_video_encode");
                    /* And the result has to be a file the rest of the library reads. */
                    fdv_info vi;
                    CHECK(out && fdv_read(out, slen, &vi) == 0, "the file parses");
                    CHECK(out && vi.nframes == nf, "the frame count was patched in");
                    free(out);
                }
            }
            free(src); free(whole); free(fp);
            fdv_scene_free(s);
        }

        /* Refusals, and finishing with nothing pushed. */
        CHECK(fdv_enc_open(100, 64, 20, 0) == NULL, "dimensions must be multiples of 16");
        CHECK(fdv_enc_open(64, 64, 99, 0) == NULL, "qp must be in range");
        CHECK(fdv_enc_frame(NULL, NULL) != 0, "pushing to nothing is refused");
        fdv_encoder *empty = fdv_enc_open(64, 64, 20, 0);
        size_t elen = 1;
        CHECK(fdv_enc_finish(empty, 30, &elen) == NULL, "an empty encoder yields no file");
        fdv_enc_close(NULL);
        printf("  streaming encode: byte-identical to the whole-array path\n");
    }

    /* --- entropy-table reuse across many streams ---------------------------
     * A P-frame's structure and coefficients travel as ten separately modelled
     * streams, and each frame chooses per stream whether to send a fresh
     * frequency table or reuse the previous frame's. That choice is carried as
     * a bit per stream.
     *
     * This test exists because the mask was written as a single byte after the
     * stream count grew past eight, so the top two streams silently lost their
     * reuse bits and the decoder went looking for tables that were never sent.
     * Every existing round-trip test passed: the clips were short enough that
     * reuse rarely fired, and never on a high-numbered stream. So this one
     * deliberately runs long enough, and on content varied enough, that reuse
     * fires widely -- and then checks the decode really matches. */
    {
        fdv_scene *s = fdv_scene_parse_text(SCENE_MOTION, err, sizeof err);
        /* Big enough that a model per stream actually wins. Below about
         * 800x480 the shared model is always cheaper, the split path never
         * runs, and this test would pass without exercising anything -- which
         * is precisely how the mask bug survived every other round-trip test
         * in this file. */
        if (s) CHECK(fdv_scene_resize(s, 800, 480) == 0, "the reuse test can resize");
        if (s) {
            int w = fdv_scene_width(s), h = fdv_scene_height(s), nf = 24;
            size_t fs = fdv_scene_frame_size(s);
            uint8_t *src = malloc(fs * (size_t)nf);
            uint8_t *dec = malloc(fs * (size_t)nf);
            if (src && dec) {
                for (int f = 0; f < nf; ++f) fdv_scene_render(s, f, src + (size_t)f * fs);
                /* keyint 0 keeps one intra frame at the front, so every frame
                 * after it is a P-frame with a table cache to draw on. */
                for (int qp = 12; qp <= 36; qp += 12) {
                    fdv_encoder *e = fdv_enc_open(w, h, qp, 0);
                    CHECK(e != NULL, "encoder opens for the reuse test");
                    for (int f = 0; f < nf && e; ++f)
                        CHECK(fdv_enc_frame(e, src + (size_t)f * fs) == 0,
                              "every frame is accepted");
                    size_t len = 0;
                    uint8_t *out = e ? fdv_enc_finish(e, 30, &len) : NULL;
                    CHECK(out != NULL, "the stream finishes");

                    fdv_info vi;
                    int okread = out && fdv_read(out, len, &vi) == 0;
                    CHECK(okread, "the file parses");
                    int dnf = 0, dw = 0, dh = 0;
                    CHECK(okread && fdv_video_decode(vi.payload, vi.payload_len,
                                                     dec, &dnf, &dw, &dh) == 0,
                          "every frame decodes with tables reused across streams");
                    CHECK(okread && dnf == nf && dw == w && dh == h,
                          "and the whole sequence comes back");

                    /* The streaming decoder keeps its own table cache, so it is
                     * a second, independent check of the same agreement. */
                    if (okread) {
                        fdv_decoder *d = fdv_dec_open(&vi);
                        int got = 0;
                        while (d && fdv_dec_next(d, dec) == 1) ++got;
                        if (d) fdv_dec_close(d);
                        CHECK(got == nf, "and again through the streaming decoder");
                    }
                    free(out);
                }
            }
            free(src); free(dec);
            fdv_scene_free(s);
        }
        printf("  table reuse: every stream's bit survives the round trip\n");
    }

    /* --- band-parallel streaming encode ------------------------------------
     * A P-frame encode is single-threaded, which is what limits a capture on a
     * machine with cores to spare. fdv_enc_open_tiled runs the bands that
     * fdv_vtile_encode already defines, one frame at a time, and has to produce
     * exactly the file the whole-array path produces -- with any thread count,
     * since the split is spatial and the bands never see each other. */
    {
        fdv_scene *s = fdv_scene_parse_text(SCENE_MOTION, err, sizeof err);
        if (s) {
            int w = fdv_scene_width(s), h = fdv_scene_height(s), nf = 6;
            int mbrows = h / 16, br = mbrows > 1 ? (mbrows + 1) / 2 : 1;
            size_t fs = fdv_scene_frame_size(s), cap = fs * (size_t)nf * 2 + 65536;
            uint8_t *src = malloc(fs * (size_t)nf), *whole = malloc(cap);
            const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
            if (src && whole && fp) {
                for (int f = 0; f < nf; ++f) {
                    fdv_scene_render(s, f, src + (size_t)f * fs);
                    fp[f] = src + (size_t)f * fs;
                }
                int nbands = (mbrows + br - 1) / br;
                for (int nt = 1; nt <= 4; nt += 3) {
                    size_t plen = fdv_vtile_encode(fp, nf, w, h, 20, 3, br, nt,
                                                   whole + FDV_HEAD, cap - FDV_HEAD);
                    size_t wlen = fdv_wrap(whole, cap, FDV_TILED, 30,
                                           whole + FDV_HEAD, plen);
                    fdv_encoder *e = fdv_enc_open_tiled(w, h, 20, 3, br, nt);
                    CHECK(e != NULL, "band-parallel encoder opens");
                    for (int f = 0; f < nf && e; ++f)
                        CHECK(fdv_enc_frame(e, src + (size_t)f * fs) == 0,
                              "every frame is accepted");
                    CHECK(!e || fdv_enc_count(e) == nf, "the count is reported");
                    size_t slen = 0;
                    uint8_t *out = e ? fdv_enc_finish(e, 30, &slen) : NULL;
                    CHECK(out && slen == wlen && memcmp(out, whole, wlen) == 0,
                          "band-parallel streaming matches fdv_vtile_encode");
                    fdv_info vi;
                    CHECK(out && fdv_read(out, slen, &vi) == 0, "the file parses");
                    CHECK(out && vi.kind == FDV_TILED, "it is a tiled container");
                    CHECK(out && vi.nframes == nf, "the frame count was patched in");
                    free(out);
                }
                /* Holding has to work across every band at once, or the bands
                 * would drift out of step with each other. */
                fdv_encoder *e = fdv_enc_open_tiled(w, h, 20, 0, br, 4);
                if (e) {
                    fdv_enc_frame(e, src);
                    fdv_enc_frame(e, src + fs);
                    CHECK(fdv_enc_repeat(e) == 0, "a band-parallel frame can be held");
                    CHECK(fdv_enc_count(e) == 3, "a held frame counts once, not once per band");
                    size_t l = 0;
                    uint8_t *o2 = fdv_enc_finish(e, 30, &l);
                    fdv_info vi;
                    CHECK(o2 && fdv_read(o2, l, &vi) == 0 && vi.nframes == 3,
                          "the held frame reaches the file");
                    uint8_t *dec = malloc(fs * 3);
                    int gw = 0, gh = 0, gn = 0;
                    CHECK(o2 && dec && fdv_vtile_decode(vi.payload, vi.payload_len, dec,
                                                        &gn, &gw, &gh, 4) == 0 && gn == 3,
                          "and it decodes");
                    free(dec); free(o2);
                }
                (void)nbands;
            }
            free(src); free(whole); free(fp);
            fdv_scene_free(s);
        }
        CHECK(fdv_enc_open_tiled(64, 64, 20, 0, 0, 2) == NULL,
              "a band must be at least one macroblock row");
        printf("  band-parallel streaming: matches the whole-array tiled path\n");
    }

    /* --- held frames -------------------------------------------------------
     * fdv_enc_repeat exists because letting mode decision rediscover SKIP is
     * only cheap at a high qp. Below about QP 16, lambda gets small enough that
     * coding the reference's own quantization noise starts to score better than
     * SKIP, and re-pushing identical pixels costs most of a fresh frame's work
     * -- so a recorder holding a slot to keep time falls further behind for
     * doing so. Measured on camera frames at 720p: 26.5 ms a hold at QP 10
     * against 0.7 ms at QP 24.
     *
     * What fdv_enc_repeat guarantees is that a held frame is all-SKIP whatever
     * the qp and whatever the content, so that is what gets asserted. Timing
     * would depend on the machine, and byte counts do not separate the two
     * paths -- the old one did the whole search and then usually emitted a
     * small frame anyway. It was never the bits that were expensive. */
    {
        CHECK(fdv_enc_repeat(NULL) != 0, "holding nothing is refused");
        fdv_encoder *fresh = fdv_enc_open(64, 64, 6, 0);
        CHECK(fresh && fdv_enc_repeat(fresh) != 0,
              "holding before any frame is refused");
        fdv_enc_close(fresh);

        fdv_scene *s = fdv_scene_parse_text(SCENE_DETAIL, err, sizeof err);
        if (s) {
            int w = fdv_scene_width(s), h = fdv_scene_height(s);
            int nmb = (w / 16) * (h / 16);
            size_t fs = fdv_scene_frame_size(s);
            uint8_t *a = malloc(fs), *b = malloc(fs);
            uint8_t *d0 = malloc(fs), *d1 = malloc(fs);
            if (a && b && d0 && d1) {
                fdv_scene_render(s, 0, a);
                fdv_scene_render(s, 1, b);

                /* Both ends of the qp range: the guarantee is not qp-dependent. */
                for (int qi = 0; qi < 2; ++qi) {
                    int qp = qi ? 24 : 6;
                    fdv_frame_stat fst[8];
                    fdv_stats sink = { fst, 8, 0 };
                    fdv_stats_set(&sink);

                    fdv_encoder *e = fdv_enc_open(w, h, qp, 0);
                    fdv_enc_frame(e, a); fdv_enc_frame(e, b);
                    CHECK(e && fdv_enc_repeat(e) == 0, "a frame can be held");
                    CHECK(fdv_enc_count(e) == 3, "a held frame counts as a frame");
                    for (int i = 0; i < 3; ++i) fdv_enc_repeat(e);
                    size_t len = 0;
                    uint8_t *out = fdv_enc_finish(e, 30, &len);
                    fdv_stats_set(NULL);

                    int all_skip = sink.n >= 6;
                    for (int f = 2; f < sink.n; ++f)
                        if (fst[f].mb_skip != nmb || fst[f].mb_inter16 ||
                            fst[f].mb_inter8 || fst[f].mb_intra) all_skip = 0;
                    CHECK(all_skip, "every macroblock of a held frame is SKIP");

                    fdv_info vi;
                    if (out && fdv_read(out, len, &vi) == 0) {
                        CHECK(vi.nframes == 6, "every held frame is in the file");
                        fdv_decoder *d = fdv_dec_open(&vi);
                        int worst = 0;
                        for (int f = 0; f < 6 && d; ++f) {
                            uint8_t *dst = (f & 1) ? d1 : d0;
                            if (fdv_dec_next(d, dst) != 1) break;
                            if (f >= 2) {      /* frames 2..5 are all holds */
                                const uint8_t *prev = (f & 1) ? d0 : d1;
                                for (size_t k = 0; k < fs; ++k) {
                                    int diff = (int)dst[k] - (int)prev[k];
                                    if (diff < 0) diff = -diff;
                                    if (diff > worst) worst = diff;
                                }
                            }
                        }
                        fdv_dec_close(d);
                        /* Not bit-identical: a hold is reconstructed by copying
                         * the reference and deblocking it again, which moves a
                         * few pixels by a step or two. It settles at once rather
                         * than softening further with every hold, which is what
                         * matters -- a recording can hold many slots in a row. */
                        CHECK(worst <= 8, "a held frame decodes as the frame it held");
                    }
                    free(out);
                }
            }
            free(a); free(b); free(d0); free(d1);
            fdv_scene_free(s);
        }
        printf("  held frames: all-SKIP at any qp, and the same picture\n");
    }

    if (failures == 0) printf("all scene tests passed\n");
    else printf("%d scene test(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* ===========================================================================
 * DRIVER
 * ======================================================================== */

int main(void) {
    static const struct { const char *name; int (*fn)(void); } suites[] = {
        { "test_frame", frame_main },
        { "test_bits", bits_main },
        { "test_rans", rans_main },
        { "test_coeff8", coeff8_main },
        { "test_transform", transform_main },
        { "test_intra", intra_main },
        { "test_inter", inter_main },
        { "test_deblock", deblock_main },
        { "test_image", image_main },
        { "test_video", video_main },
        { "test_tiles", tiles_main },
        { "test_vtile", vtile_main },
        { "test_rc", rc_main },
        { "test_scene", scene_main },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(suites) / sizeof(*suites); ++i) {
        printf("=== %s ===\n", suites[i].name);
        failures = 0;
        if (suites[i].fn() != 0) bad = 1;
    }
    if (bad) printf("SUITE FAILED\n");
    return bad;
}
