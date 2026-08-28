/* Command-line front-end for the codec: encode/decode raw planar I420 files,
 * plus an in-memory self-test. Dependency-free (stdio only). */

#define FDV_IMPLEMENTATION
#include "fdv.h"
#define FDV_SCENE_IMPLEMENTATION
#include "fdv_scene.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

/* Milliseconds of wall clock, for the per-stage timings the pipeline reports. */
/* Everything printed goes through these, so the tools read alike: a dim label
 * in a fixed column, the value plain, and the number that matters in bold. */
/* Defaults to no colour, so a path that forgets ui_init still prints
 * plain text rather than "(null)". */
static fdv_palette P = { "", "", "", "", "", "", "", "" };
static void ui_init(void) { fdv_palette_for(stdout, &P); }
static void ui_head(const char *verb, const char *what) {
    printf("%s%s%s %s%s%s\n", P.bold, verb, P.rst, P.cyn, what, P.rst);
}
/* "key frame every 60 frames (2.0 s)", or that there is only the one. */
static const char *gop_str(char *b, size_t n, int keyint, double fps) {
    if (keyint <= 0) snprintf(b, n, "one key frame only");
    else if (fps > 0) snprintf(b, n, "key frame every %d frames (%.1f s)", keyint, keyint / fps);
    else snprintf(b, n, "key frame every %d frames", keyint);
    return b;
}

static void ui_row(const char *label, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("  %s%-10s%s ", P.dim, label, P.rst);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* "60" is a frame count; "2s" is a duration at the clip's frame rate. The
 * interval that matters is a duration -- it bounds how long a viewer joining a
 * stream waits for a decodable frame -- and a frame count silently halves it
 * when the frame rate doubles. */
static int parse_gop(const char *s, int fps) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (v < 0.0) return -1;
    int k = (end && (*end == 's' || *end == 'S')) ? (int)lround(v * fps) : (int)v;
    return k > 255 ? 255 : k;
}

/* "1500000", "1500k" or "1.5M" -- all bits per second. */
static long parse_bitrate(const char *s) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (end && (*end == 'k' || *end == 'K')) v *= 1000.0;
    else if (end && (*end == 'm' || *end == 'M')) v *= 1000000.0;
    return (long)v;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

static int write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n) {
    double sse = 0.0;
    for (size_t i = 0; i < n; ++i) { double d = (double)a[i] - b[i]; sse += d * d; }
    if (sse == 0.0) return 1e9;
    return 10.0 * log10(255.0 * 255.0 / (sse / (double)n));
}

/* Parse a Y4M header line ("YUV4MPEG2 Wxx Hyy ..."\n). Returns 0 and sets w/h
 * and *end (offset just past the newline), or -1. Only 4:2:0 is supported; the
 * default colorspace (no C token) is treated as C420. */
static int parse_y4m_header(const uint8_t *buf, size_t len, int *w, int *h, size_t *end) {
    if (len < 10 || memcmp(buf, "YUV4MPEG2", 9) != 0) return -1;
    size_t nl = 0;
    while (nl < len && buf[nl] != 0x0A) ++nl;
    if (nl >= len) return -1;
    *w = *h = 0;
    int c420 = 1;                                /* default is 4:2:0 */
    for (size_t i = 9; i < nl; ) {
        while (i < nl && buf[i] == ' ') ++i;
        if (i >= nl) break;
        char t = buf[i];
        if (t == 'W') *w = atoi((const char *)buf + i + 1);
        else if (t == 'H') *h = atoi((const char *)buf + i + 1);
        else if (t == 'C') c420 = (i + 4 <= nl && memcmp(buf + i + 1, "420", 3) == 0);
        while (i < nl && buf[i] != ' ') ++i;
    }
    *end = nl + 1;
    if (*w <= 0 || *h <= 0 || !c420) return -1;
    return 0;
}

static int do_ency4m(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: codec ency4m <in.y4m> <qp> <out.bin>\n"); return 1; }
    int qp = atoi(argv[3]);
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }

    int w, h; size_t pos;
    if (parse_y4m_header(in, len, &w, &h, &pos) != 0) {
        fprintf(stderr, "not a 4:2:0 Y4M file\n"); free(in); return 1;
    }
    if ((w & 15) || (h & 15)) {
        fprintf(stderr, "dimensions %dx%d must be multiples of 16\n", w, h); free(in); return 1;
    }
    size_t fsize = (size_t)w * h * 3 / 2;
    const uint8_t **frames = malloc((len / (fsize ? fsize : 1) + 1) * sizeof(*frames));
    int nf = 0;
    while (pos + 5 <= len && memcmp(in + pos, "FRAME", 5) == 0) {
        size_t nl = pos;
        while (nl < len && in[nl] != 0x0A) ++nl;          /* end of FRAME line */
        if (nl >= len) break;
        pos = nl + 1;
        if (pos + fsize > len) break;
        frames[nf++] = in + pos;
        pos += fsize;
    }
    if (nf == 0) { fprintf(stderr, "no frames found\n"); free(in); free(frames); return 1; }

    size_t cap = fsize * (size_t)nf * 2 + 65536;
    uint8_t *out = malloc(cap);
    size_t outlen = fdv_video_encode((const uint8_t *const *)frames, nf, w, h, qp, 0, out, cap);
    int rc = 0;
    if (!outlen) { fprintf(stderr, "encode failed\n"); rc = 1; }
    else if (write_file(argv[4], out, outlen) != 0) { fprintf(stderr, "cannot write %s\n", argv[4]); rc = 1; }
    else printf("encoded %d Y4M frame(s) %dx%d qp%d: %zu -> %zu bytes (%.1fx)\n",
                nf, w, h, qp, fsize * (size_t)nf, outlen, (double)(fsize * (size_t)nf) / outlen);
    free(in); free(frames); free(out);
    return rc;
}

static int do_decy4m(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: codec decy4m <in.bin> <out.y4m>\n"); return 1; }
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (len < 7) { fprintf(stderr, "bitstream too small\n"); free(in); return 1; }
    int nf = in[0] | (in[1] << 8), w = in[2] | (in[3] << 8), h = in[4] | (in[5] << 8);
    if (nf <= 0 || w <= 0 || h <= 0) { fprintf(stderr, "bad header\n"); free(in); return 1; }
    size_t fsize = (size_t)w * h * 3 / 2;
    uint8_t *out = malloc(fsize * (size_t)nf);
    int dnf, dw, dh;
    if (fdv_video_decode(in, len, out, &dnf, &dw, &dh) != 0) {
        fprintf(stderr, "decode failed\n"); free(in); free(out); return 1;
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[3]); free(in); free(out); return 1; }
    fprintf(f, "YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420\n", dw, dh);
    for (int i = 0; i < dnf; ++i) {
        fprintf(f, "FRAME\n");
        fwrite(out + (size_t)i * fsize, 1, fsize, f);
    }
    fclose(f);
    printf("decoded %d frame(s) %dx%d -> %s\n", dnf, dw, dh, argv[3]);
    free(in); free(out);
    return 0;
}

static int do_enc(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: fdv enc <in.yuv> <w> <h> <nframes> <qp> <out.fdv> [keyint] [fps]\n");
        return 1;
    }
    int w = atoi(argv[3]), h = atoi(argv[4]), nf = atoi(argv[5]), qp = atoi(argv[6]);
    int keyint = argc > 8 ? atoi(argv[8]) : 0;
    /* Raw I420 carries no frame rate, so the container needs one from the
     * caller; 30 is the least surprising default for a test clip. */
    int fps = argc > 9 ? atoi(argv[9]) : 30;
    if (w <= 0 || h <= 0 || nf <= 0 || (w & 15) || (h & 15)) {
        fprintf(stderr, "bad dimensions (w,h must be positive multiples of 16)\n");
        return 1;
    }
    size_t fsize = (size_t)w * h * 3 / 2;
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (len < fsize * (size_t)nf) {
        fprintf(stderr, "input too small: %zu < %zu bytes\n", len, fsize * (size_t)nf);
        free(in); return 1;
    }
    const uint8_t **frames = malloc((size_t)nf * sizeof(*frames));
    for (int f = 0; f < nf; ++f) frames[f] = in + (size_t)f * fsize;

    size_t cap = fsize * (size_t)nf * 2 + 65536;
    uint8_t *out = malloc(cap);
    size_t plen = fdv_video_encode((const uint8_t *const *)frames, nf, w, h, qp, keyint,
                               out + FDV_HEAD, cap - FDV_HEAD);
    size_t outlen = plen ? fdv_wrap(out, cap, FDV_VIDEO, fps, out + FDV_HEAD, plen) : 0;

    int rc = 0;
    if (!outlen) { fprintf(stderr, "encode failed\n"); rc = 1; }
    else if (write_file(argv[7], out, outlen) != 0) { fprintf(stderr, "cannot write %s\n", argv[7]); rc = 1; }
    else printf("encoded %d frame(s) %dx%d qp%d @%d fps: %zu -> %zu bytes (%.1fx)\n",
                nf, w, h, qp, fps, fsize * (size_t)nf, outlen,
                (double)(fsize * (size_t)nf) / outlen);
    free(in); free(frames); free(out);
    return rc;
}

/* Tile-parallel video: encode into the vtile container (per-band independent
 * sub-streams). Mirrors do_enc but takes a band_mbrows argument (MB rows per
 * band). */
static int do_enctiled(int argc, char **argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: codec enctiled <in.yuv> <w> <h> <nframes> <qp> <band_mbrows> <out.bin> [keyint]\n");
        return 1;
    }
    int w = atoi(argv[3]), h = atoi(argv[4]), nf = atoi(argv[5]), qp = atoi(argv[6]);
    int bmr = atoi(argv[7]);
    int keyint  = argc > 9  ? atoi(argv[9])  : 0;
    int threads = argc > 10 ? atoi(argv[10]) : 4;   /* bands are independent */
    if (w <= 0 || h <= 0 || nf <= 0 || (w & 15) || (h & 15) || bmr <= 0) {
        fprintf(stderr, "bad arguments (w,h positive multiples of 16; band_mbrows > 0)\n");
        return 1;
    }
    size_t fsize = (size_t)w * h * 3 / 2;
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (len < fsize * (size_t)nf) {
        fprintf(stderr, "input too small: %zu < %zu bytes\n", len, fsize * (size_t)nf);
        free(in); return 1;
    }
    const uint8_t **frames = malloc((size_t)nf * sizeof(*frames));
    for (int f = 0; f < nf; ++f) frames[f] = in + (size_t)f * fsize;

    size_t cap = fsize * (size_t)nf * 2 + 65536;
    uint8_t *out = malloc(cap);
    size_t plen = fdv_vtile_encode((const uint8_t *const *)frames, nf, w, h, qp, keyint,
                               bmr, threads, out + FDV_HEAD, cap - FDV_HEAD);
    size_t outlen = plen ? fdv_wrap(out, cap, FDV_TILED, 30, out + FDV_HEAD, plen) : 0;

    int rc = 0;
    int nbands = (h + bmr * 16 - 1) / (bmr * 16);
    if (!outlen) { fprintf(stderr, "encode failed\n"); rc = 1; }
    else if (write_file(argv[8], out, outlen) != 0) { fprintf(stderr, "cannot write %s\n", argv[8]); rc = 1; }
    else printf("encoded %d frame(s) %dx%d qp%d, %d band(s): %zu -> %zu bytes (%.1fx)\n",
                nf, w, h, qp, nbands, fsize * (size_t)nf, outlen, (double)(fsize * (size_t)nf) / outlen);
    free(in); free(frames); free(out);
    return rc;
}

/* Tile-parallel video decode with up to <threads> worker threads (default 4). */
static int do_dectiled(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: fdv dectiled <in.fdv> <out.yuv> [threads]\n"); return 1; }
    int threads = argc > 4 ? atoi(argv[4]) : 4;
    if (threads < 1) threads = 1;
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (len < 12) { fprintf(stderr, "bitstream too small\n"); free(in); return 1; }

    /* A .fdv file, or a bare tiled stream from an older build. */
    fdv_info vi;
    const uint8_t *pay = in;
    size_t paylen = len;
    int w, h, nf;
    if (fdv_read(in, len, &vi) == 0) {
        pay = vi.payload; paylen = vi.payload_len;
        w = vi.w; h = vi.h; nf = vi.nframes;
        if (vi.kind != FDV_TILED) {
            fprintf(stderr, "'%s' is a single-stream file — use dec\n", argv[2]);
            free(in); return 1;
        }
    } else {
        w = in[0] | (in[1] << 8); h = in[2] | (in[3] << 8); nf = in[4] | (in[5] << 8);
    }
    if (nf <= 0 || w <= 0 || h <= 0) { fprintf(stderr, "bad header\n"); free(in); return 1; }
    size_t fsize = (size_t)w * h * 3 / 2;
    uint8_t *out = malloc(fsize * (size_t)nf);

    int dnf, dw, dh;
    if (fdv_vtile_decode(pay, paylen, out, &dnf, &dw, &dh, threads) != 0) {
        fprintf(stderr, "decode failed\n"); free(in); free(out); return 1;
    }
    int rc = write_file(argv[3], out, fsize * (size_t)dnf);
    if (rc != 0) fprintf(stderr, "cannot write %s\n", argv[3]);
    else printf("decoded %d frame(s) %dx%d (%d thread%s) -> %zu bytes\n",
                dnf, dw, dh, threads, threads == 1 ? "" : "s", fsize * (size_t)dnf);
    free(in); free(out);
    return rc ? 1 : 0;
}

static int do_dec(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: fdv dec <in.fdv> <out.yuv>\n"); return 1; }
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (len < 7) { fprintf(stderr, "bitstream too small\n"); free(in); return 1; }

    /* A .fdv file, or a bare coded stream from an older build. The magic check
     * makes telling them apart exact rather than a guess. */
    fdv_info vi;
    const uint8_t *pay = in;
    size_t paylen = len;
    int nf, w, h;
    if (fdv_read(in, len, &vi) == 0) {
        pay = vi.payload; paylen = vi.payload_len;
        nf = vi.nframes; w = vi.w; h = vi.h;
        if (vi.kind != FDV_VIDEO) {
            fprintf(stderr, "'%s' is a tile-parallel stream — use dectiled\n", argv[2]);
            free(in); return 1;
        }
    } else {
        nf = in[0] | (in[1] << 8); w = in[2] | (in[3] << 8); h = in[4] | (in[5] << 8);
    }
    if (nf <= 0 || w <= 0 || h <= 0) { fprintf(stderr, "bad header\n"); free(in); return 1; }
    size_t fsize = (size_t)w * h * 3 / 2;
    uint8_t *out = malloc(fsize * (size_t)nf);

    int dnf, dw, dh;
    if (fdv_video_decode(pay, paylen, out, &dnf, &dw, &dh) != 0) {
        fprintf(stderr, "decode failed\n"); free(in); free(out); return 1;
    }
    int rc = write_file(argv[3], out, fsize * (size_t)dnf);
    if (rc != 0) fprintf(stderr, "cannot write %s\n", argv[3]);
    else printf("decoded %d frame(s) %dx%d -> %zu bytes\n", dnf, dw, dh, fsize * (size_t)dnf);
    free(in); free(out);
    return rc ? 1 : 0;
}

/* Synthesize a panning textured I420 sequence (used by selftest and bench). */
static void synth_seq(uint8_t *seq, int w, int h, int nf) {
    size_t ys = (size_t)w * h, cs = (size_t)(w / 2) * (h / 2), fsize = ys + 2 * cs;
    for (int f = 0; f < nf; ++f) {
        uint8_t *Y = seq + (size_t)f * fsize, *U = Y + ys, *V = U + cs;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                Y[y * w + x] = (uint8_t)((x + 2 * f) * 3 + (y + f) * 5 +
                                         40 * ((((x + 2 * f) / 8) + ((y + f) / 8)) & 1));
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x) {
                U[y * (w / 2) + x] = (uint8_t)(110 + x + f);
                V[y * (w / 2) + x] = (uint8_t)(140 - x + f);
            }
    }
}

static int do_bench(int argc, char **argv) {
    int w     = argc > 2 ? atoi(argv[2]) : 320;
    int h     = argc > 3 ? atoi(argv[3]) : 192;
    int nf    = argc > 4 ? atoi(argv[4]) : 8;
    int qp    = argc > 5 ? atoi(argv[5]) : 20;
    int iters = argc > 6 ? atoi(argv[6]) : 20;
    if (w <= 0 || h <= 0 || nf <= 0 || (w & 15) || (h & 15) || iters <= 0) {
        fprintf(stderr, "bad arguments (w,h multiples of 16; nf,iters > 0)\n");
        return 1;
    }
    size_t fsize = (size_t)w * h * 3 / 2;
    uint8_t *seq = malloc(fsize * (size_t)nf);
    synth_seq(seq, w, h, nf);
    const uint8_t **frames = malloc((size_t)nf * sizeof(*frames));
    for (int f = 0; f < nf; ++f) frames[f] = seq + (size_t)f * fsize;

    size_t cap = fsize * (size_t)nf * 2 + 65536;
    uint8_t *bs = malloc(cap), *dec = malloc(fsize * (size_t)nf);
    size_t blen = fdv_video_encode((const uint8_t *const *)frames, nf, w, h, qp, 0, bs, cap);
    if (!seq || !frames || !bs || !dec || !blen) {
        fprintf(stderr, "setup failed\n"); free(seq); free(frames); free(bs); free(dec); return 1;
    }

    clock_t t0 = clock();
    for (int it = 0; it < iters; ++it)
        fdv_video_encode((const uint8_t *const *)frames, nf, w, h, qp, 0, bs, cap);
    double enc_s = (double)(clock() - t0) / CLOCKS_PER_SEC / iters;

    int dnf, dw, dh;
    t0 = clock();
    for (int it = 0; it < iters; ++it) fdv_video_decode(bs, blen, dec, &dnf, &dw, &dh);
    double dec_s = (double)(clock() - t0) / CLOCKS_PER_SEC / iters;

    double mpix = (double)w * h * nf / 1e6;
    printf("bench %dx%d %d frame(s) qp%d, %d iters:\n", w, h, nf, qp, iters);
    printf("  encode: %7.2f ms/seq  %7.1f Mpix/s  %6.1f fps\n",
           enc_s * 1e3, enc_s > 0 ? mpix / enc_s : 0, enc_s > 0 ? nf / enc_s : 0);
    printf("  decode: %7.2f ms/seq  %7.1f Mpix/s  %6.1f fps\n",
           dec_s * 1e3, dec_s > 0 ? mpix / dec_s : 0, dec_s > 0 ? nf / dec_s : 0);
    printf("  bitstream %zu bytes (%.0f B/frame, %.1fx)\n",
           blen, (double)blen / nf, (double)(fsize * (size_t)nf) / blen);
    free(seq); free(frames); free(bs); free(dec);
    return 0;
}

static int do_compare(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: codec compare <a.yuv> <b.yuv> <w> <h> <nframes>\n");
        return 1;
    }
    int w = atoi(argv[4]), h = atoi(argv[5]), nf = atoi(argv[6]);
    if (w <= 0 || h <= 0 || nf <= 0 || (w & 1) || (h & 1)) {
        fprintf(stderr, "bad dimensions (w,h positive and even for 4:2:0)\n");
        return 1;
    }
    size_t ys = (size_t)w * h, cs = (size_t)(w / 2) * (h / 2), fsize = ys + 2 * cs;
    size_t la = 0, lb = 0;
    uint8_t *a = read_file(argv[2], &la), *b = read_file(argv[3], &lb);
    if (!a || !b) { fprintf(stderr, "cannot read inputs\n"); free(a); free(b); return 1; }
    if (la < fsize * (size_t)nf || lb < fsize * (size_t)nf) {
        fprintf(stderr, "input(s) too small for %d %dx%d frames\n", nf, w, h);
        free(a); free(b); return 1;
    }
    double yp = 0, up = 0, vp = 0;
    for (int f = 0; f < nf; ++f) {
        const uint8_t *pa = a + (size_t)f * fsize, *pb = b + (size_t)f * fsize;
        yp += psnr(pa, pb, ys);
        up += psnr(pa + ys, pb + ys, cs);
        vp += psnr(pa + ys + cs, pb + ys + cs, cs);
    }
    printf("compare %dx%d %d frame(s): PSNR Y %.2f  U %.2f  V %.2f dB\n",
           w, h, nf, yp / nf, up / nf, vp / nf);
    free(a); free(b);
    return 0;
}

static int do_enctarget(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: codec enctarget <in.yuv> <w> <h> <nframes> <target_bytes> <out.bin>\n");
        return 1;
    }
    int w = atoi(argv[3]), h = atoi(argv[4]), nf = atoi(argv[5]);
    long target = atol(argv[6]);
    if (w <= 0 || h <= 0 || nf <= 0 || (w & 15) || (h & 15) || target <= 0) {
        fprintf(stderr, "bad arguments (w,h multiples of 16; target_bytes > 0)\n");
        return 1;
    }
    size_t fsize = (size_t)w * h * 3 / 2, ys = (size_t)w * h, cs = (size_t)(w / 2) * (h / 2);
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (len < fsize * (size_t)nf) {
        fprintf(stderr, "input too small: %zu < %zu bytes\n", len, fsize * (size_t)nf);
        free(in); return 1;
    }
    const uint8_t **frames = malloc((size_t)nf * sizeof(*frames));
    for (int f = 0; f < nf; ++f) frames[f] = in + (size_t)f * fsize;

    size_t cap = fsize * (size_t)nf * 2 + 65536;
    uint8_t *out = malloc(cap), *best = malloc(cap);
    if (!frames || !out || !best) { free(in); free(frames); free(out); free(best); return 1; }

    /* Size falls as QP rises; take the lowest (best-quality) QP within budget,
     * else the coarsest QP if even QP 51 overshoots. */
    int bestqp = -1; size_t bestlen = 0;
    for (int qp = 0; qp <= 51; ++qp) {
        size_t l = fdv_video_encode((const uint8_t *const *)frames, nf, w, h, qp, 0, out, cap);
        if (l == 0) continue;
        if (l <= (size_t)target) { bestqp = qp; bestlen = l; memcpy(best, out, l); break; }
        if (qp == 51 && bestqp < 0) { bestqp = 51; bestlen = l; memcpy(best, out, l); }
    }
    int rc = 0;
    if (bestqp < 0) { fprintf(stderr, "encode failed at all QPs\n"); rc = 1; goto done; }
    if (write_file(argv[7], best, bestlen) != 0) { fprintf(stderr, "cannot write %s\n", argv[7]); rc = 1; goto done; }

    {
        uint8_t *dec = malloc(fsize * (size_t)nf);
        int dnf, dw, dh;
        double yp = 0, up = 0, vp = 0;
        if (dec && fdv_video_decode(best, bestlen, dec, &dnf, &dw, &dh) == 0) {
            for (int f = 0; f < nf; ++f) {
                const uint8_t *o = in + (size_t)f * fsize, *d = dec + (size_t)f * fsize;
                yp += psnr(o, d, ys); up += psnr(o + ys, d + ys, cs); vp += psnr(o + ys + cs, d + ys + cs, cs);
            }
        }
        printf("target %ld bytes: chose QP %d -> %zu bytes (%s budget)  PSNR Y %.1f U %.1f V %.1f dB\n",
               target, bestqp, bestlen, bestlen <= (size_t)target ? "within" : "over",
               yp / nf, up / nf, vp / nf);
        free(dec);
    }
done:
    free(in); free(frames); free(out); free(best);
    return rc;
}

static int do_selftest(int argc, char **argv) {
    int w  = argc > 2 ? atoi(argv[2]) : 64;
    int h  = argc > 3 ? atoi(argv[3]) : 64;
    int nf = argc > 4 ? atoi(argv[4]) : 4;
    int qp = argc > 5 ? atoi(argv[5]) : 18;
    if (w <= 0 || h <= 0 || nf <= 0 || (w & 15) || (h & 15)) {
        fprintf(stderr, "bad dimensions (w,h must be positive multiples of 16)\n");
        return 1;
    }
    size_t ys = (size_t)w * h, cs = (size_t)(w / 2) * (h / 2), fsize = ys + 2 * cs;

    uint8_t *seq = malloc(fsize * (size_t)nf);
    synth_seq(seq, w, h, nf);
    const uint8_t **frames = malloc((size_t)nf * sizeof(*frames));
    for (int f = 0; f < nf; ++f) frames[f] = seq + (size_t)f * fsize;

    size_t cap = fsize * (size_t)nf * 2 + 65536;
    uint8_t *bs = malloc(cap);
    size_t blen = fdv_video_encode((const uint8_t *const *)frames, nf, w, h, qp, 0, bs, cap);
    if (!blen) { fprintf(stderr, "encode failed\n"); free(seq); free(frames); free(bs); return 1; }

    uint8_t *dec = malloc(fsize * (size_t)nf);
    int dnf, dw, dh;
    if (fdv_video_decode(bs, blen, dec, &dnf, &dw, &dh) != 0) {
        fprintf(stderr, "decode failed\n"); free(seq); free(frames); free(bs); free(dec); return 1;
    }

    double yp = 0, up = 0, vp = 0;
    for (int f = 0; f < nf; ++f) {
        const uint8_t *o = seq + (size_t)f * fsize, *d = dec + (size_t)f * fsize;
        yp += psnr(o, d, ys);
        up += psnr(o + ys, d + ys, cs);
        vp += psnr(o + ys + cs, d + ys + cs, cs);
    }
    printf("selftest %dx%d %d frame(s) qp%d: %zu bytes (%.0f B/frame, %.1fx)  "
           "PSNR Y %.1f  U %.1f  V %.1f dB\n",
           w, h, nf, qp, blen, (double)blen / nf, (double)(fsize * (size_t)nf) / blen,
           yp / nf, up / nf, vp / nf);
    free(seq); free(frames); free(bs); free(dec);
    return 0;
}

/* Apply a -s/--size argument to a scene. Returns 0, or prints why and fails. */
static int apply_size(fdv_scene *s, const char *arg) {
    int w, h;
    if (fdv_scene_parse_size(arg, &w, &h) != 0) {
        fprintf(stderr,
            "bad size '%s' — use WxH (multiples of 16) or "
            "360p/480p/720p/1080p/1440p/4k\n", arg);
        return -1;
    }
    if (fdv_scene_resize(s, w, h) != 0) {
        fprintf(stderr, "cannot resize to %dx%d\n", w, h);
        return -1;
    }
    return 0;
}

/* Render a scene and encode it straight to a .fdv file -- the one step that
 * turns a description into something the player can open. */
/* Encode a .y4m through the streaming encoder, so rate control sees frames one
 * at a time exactly as a live source delivers them. */
/* -v / -vv / -vvv, shared by every subcommand that codes something.
 *
 * The levels are the codec's own: INFO is a line per stage, FRAME is a line per
 * coded frame -- size, timing, macroblock mode mix, which entropy mode won --
 * and BLOCK is per macroblock, which only exists in the tracing build. */
static int verbosity_opt(const char *a, int *verbose) {
    if      (!strcmp(a, "-v"))   { *verbose = 1; return 1; }
    else if (!strcmp(a, "-vv"))  { *verbose = 2; return 1; }
    else if (!strcmp(a, "-vvv")) { *verbose = 3; return 1; }
    return 0;
}

static void verbosity_set(int verbose) {
    fdv_log_set(verbose == 0 ? FDV_LOG_OFF
              : verbose == 1 ? FDV_LOG_INFO
              : verbose == 2 ? FDV_LOG_FRAME : FDV_LOG_BLOCK, stdout);
#if !defined(FDV_TRACE)
    if (verbose >= 3)
        fprintf(stderr, "note: -vvv needs the tracing build; run `make trace` "
                        "and use build/fdv-trace\n");
#endif
}

static int encode_y4m(int argc, char **argv) {
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot read '%s'\n", argv[2]); return 1; }
    char hdr[512]; int hl = 0, c;
    while ((c = fgetc(f)) != '\n' && c != EOF) if (hl < 511) hdr[hl++] = (char)c;
    hdr[hl] = 0;
    int sw = 0, sh = 0, fps = 30;
    char *t;
    if ((t = strstr(hdr, "W")))  sw  = atoi(t + 1);
    if ((t = strstr(hdr, " H"))) sh  = atoi(t + 2);
    if ((t = strstr(hdr, " F"))) fps = atoi(t + 2);
    if (fps <= 0) fps = 30;
    if (sw <= 0 || sh <= 0) { fprintf(stderr, "'%s': bad Y4M header\n", argv[2]); fclose(f); return 1; }

    int qp = 20, keyint = fps * 2, band = 0, threads = 8, verbose = 0;
    long bitrate = 0;
    for (int i = 4; i < argc; ++i) {
        int has = i + 1 < argc;
        if      (!strcmp(argv[i], "-q") && has) qp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && has) bitrate = parse_bitrate(argv[++i]);
        else if (!strcmp(argv[i], "-k") && has) keyint = parse_gop(argv[++i], fps);
        else if (!strcmp(argv[i], "-t") && has) band = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-j") && has) threads = atoi(argv[++i]);
        else if (verbosity_opt(argv[i], &verbose)) { }
        else { fprintf(stderr, "unknown option '%s'\n", argv[i]); fclose(f); return 1; }
    }
    verbosity_set(verbose);
    /* The block grid needs a multiple of 16; crop rather than scale, so every
     * pixel coded is the source's own. */
    int w = sw & ~15, h = sh & ~15;
    if (w <= 0 || h <= 0) { fprintf(stderr, "'%s': smaller than a macroblock\n", argv[2]); fclose(f); return 1; }

    fdv_encoder *e = band > 0 ? fdv_enc_open_tiled(w, h, qp, keyint, band, threads)
                              : fdv_enc_open(w, h, qp, keyint);
    if (!e) { fprintf(stderr, "cannot open encoder\n"); fclose(f); return 1; }
    if (bitrate > 0) fdv_enc_set_bitrate(e, (int)bitrate, fps, 1.0);

    size_t sfs = (size_t)sw * sh * 3 / 2, fs = (size_t)w * h * 3 / 2;
    uint8_t *src = malloc(sfs), *cur = malloc(fs);
    if (!src || !cur) { fprintf(stderr, "out of memory\n"); free(src); free(cur);
                        fdv_enc_close(e); fclose(f); return 1; }
    int n = 0;
    double t0 = now_ms();
    for (;;) {
        while ((c = fgetc(f)) != '\n' && c != EOF) { }
        if (c == EOF) break;
        if (fread(src, 1, sfs, f) != sfs) break;
        if (w == sw && h == sh) memcpy(cur, src, fs);
        else {
            for (int y = 0; y < h; ++y) memcpy(cur + (size_t)y * w, src + (size_t)y * sw, (size_t)w);
            uint8_t *du = cur + (size_t)w * h, *dv = du + (size_t)(w / 2) * (h / 2);
            const uint8_t *su = src + (size_t)sw * sh, *sv = su + (size_t)(sw / 2) * (sh / 2);
            for (int y = 0; y < h / 2; ++y) {
                memcpy(du + (size_t)y * (w / 2), su + (size_t)y * (sw / 2), (size_t)w / 2);
                memcpy(dv + (size_t)y * (w / 2), sv + (size_t)y * (sw / 2), (size_t)w / 2);
            }
        }
        if (fdv_enc_frame(e, cur) != 0) break;
        ++n;
    }
    fclose(f); free(src); free(cur);
    double ms = now_ms() - t0;

    size_t len = 0;
    uint8_t *out = fdv_enc_finish(e, fps, &len);
    if (!out || n == 0) { fprintf(stderr, "encode failed\n"); free(out); return 1; }
    if (write_file(argv[3], out, len) != 0) {
        fprintf(stderr, "cannot write '%s'\n", argv[3]); free(out); return 1;
    }
    char r1[32], r2[32], d1[32], g[64], crop[48] = "";
    double secs = n / (double)fps;
    double bps = (double)len * 8 / secs;
    double raw = (double)sw * sh * 3 / 2 * 8 * fps;
    if (w != sw || h != sh) snprintf(crop, sizeof crop, "  (cropped from %dx%d)", sw, sh);
    ui_init();
    ui_head("encode", argv[3]);
    ui_row("source", "%s, %dx%d%s, %d frames @%d fps  (%s)",
           argv[2], w, h, crop, n, fps, fdv_dur(d1, sizeof d1, secs));
    ui_row("coding", bitrate > 0 ? "%s target, %s%s" : "qp %s, %s%s",
           bitrate > 0 ? fdv_rate(r2, sizeof r2, (double)bitrate) : (snprintf(r2, sizeof r2, "%d", qp), r2),
           gop_str(g, sizeof g, keyint, fps), band > 0 ? ", tile-parallel" : "");
    ui_row("stream", "%s%s%s   %s(%.0fx under raw %s)%s",
           P.bold, fdv_rate(r1, sizeof r1, bps), P.rst,
           P.dim, raw / bps, fdv_rate(r2, sizeof r2, raw), P.rst);
    ui_row("speed", "%.1f ms/frame  %s(%.0f fps)%s", ms / n, P.dim, 1000.0 * n / ms, P.rst);
    free(out);
    return 0;
}

static int do_encode(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: fdv encode <scene|in.y4m> <out.fdv> [options]\n"
            "  -q QP        quantizer (default: the scene's, else 20)\n"
            "  -b RATE      target bitrate instead: 1200k, 2M\n"
            "  -k GOP       key-frame interval: N frames, or Ns for seconds\n"
            "  -n FRAMES    override the scene's frame count\n"
            "  -s SIZE      render at another resolution: WxH, or 720p/1080p/4k\n"
            "  -t MBROWS    tile-parallel container, MB rows per band\n"
            "  -j THREADS   encode threads for -t (default 8)\n"
            "  -v, -vv      per-stage, then per-frame logging\n");
        return 1;
    }
    char err[256];
    /* A .y4m is real video rather than a scene description: read its header for
     * the geometry and stream its frames straight through. Everything past that
     * is identical, which is the point -- the encoder should not care where the
     * frames came from. */
    size_t nlen = strlen(argv[2]);
    if (nlen > 4 && strcmp(argv[2] + nlen - 4, ".y4m") == 0) return encode_y4m(argc, argv);

    fdv_scene *s = fdv_scene_open(argv[2], err, sizeof err);
    if (!s) { fprintf(stderr, "scene: %s\n", err); return 1; }

    const char *out = argv[3];
    int qp = fdv_scene_qp(s) >= 0 ? fdv_scene_qp(s) : 20;
    int keyint = fdv_scene_keyint(s) >= 0 ? fdv_scene_keyint(s) : 0;
    int nf = fdv_scene_frames(s), band = 0, threads = 8, verbose = 0;
    long bitrate = 0;
    for (int i = 4; i < argc; ++i) {
        int has = i + 1 < argc;
        if      (!strcmp(argv[i], "-q") && has) qp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && has) keyint = parse_gop(argv[++i], fdv_scene_fps(s));
        else if (!strcmp(argv[i], "-n") && has) nf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && has) {
            if (apply_size(s, argv[++i]) != 0) { fdv_scene_free(s); return 1; }
        }
        else if (!strcmp(argv[i], "-t") && has) band = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-j") && has) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && has) bitrate = parse_bitrate(argv[++i]);
        else if (verbosity_opt(argv[i], &verbose)) { }
        else { fprintf(stderr, "unknown option '%s'\n", argv[i]); fdv_scene_free(s); return 1; }
    }
    if (nf <= 0) nf = fdv_scene_frames(s);
    if (qp < 0 || qp > 51) { fprintf(stderr, "qp must be 0..51\n"); fdv_scene_free(s); return 1; }
    verbosity_set(verbose);
    if (verbose) fdv_scene_describe(s, stdout, "scene");

    int w = fdv_scene_width(s), h = fdv_scene_height(s);
    size_t fs = fdv_scene_frame_size(s), total = fs * (size_t)nf;
    uint8_t *src = malloc(total);
    const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
    size_t cap = total + 65536;
    uint8_t *buf = malloc(cap);                 /* head + payload, one buffer */
    if (!src || !fp || !buf) {
        fprintf(stderr, "out of memory\n");
        free(src); free(fp); free(buf); fdv_scene_free(s); return 1;
    }
    for (int f = 0; f < nf; ++f) {
        fdv_scene_render(s, f, src + (size_t)f * fs);
        fp[f] = src + (size_t)f * fs;
    }

    double t0 = now_ms();
    uint8_t *payload = buf + FDV_HEAD;
    size_t plen = 0, flen = 0;
    uint8_t *abrbuf = NULL;
    if (bitrate > 0) {
        /* Rate control needs the frames one at a time, which is the streaming
         * encoder's shape rather than the whole-array one. */
        fdv_encoder *e = band > 0
            ? fdv_enc_open_tiled(w, h, qp, keyint, band, threads)
            : fdv_enc_open(w, h, qp, keyint);
        if (e && fdv_enc_set_bitrate(e, (int)bitrate, fdv_scene_fps(s), 1.0) == 0) {
            for (int f = 0; f < nf; ++f) fdv_enc_frame(e, src + (size_t)f * fs);
            abrbuf = fdv_enc_finish(e, fdv_scene_fps(s), &flen);
            plen = flen ? flen - FDV_HEAD : 0;
        } else if (e) fdv_enc_close(e);
    } else {
        plen = band > 0
            ? fdv_vtile_encode(fp, nf, w, h, qp, keyint, band, threads, payload, cap - FDV_HEAD)
            : fdv_video_encode(fp, nf, w, h, qp, keyint, payload, cap - FDV_HEAD);
    }
    double t_enc = now_ms() - t0;

    int rc = 1;
    if (abrbuf) {
        if (write_file(out, abrbuf, flen) != 0) fprintf(stderr, "cannot write '%s'\n", out);
        else {
            {   char dd[64], gg[64];
                double sf = fdv_scene_fps(s) > 0 ? fdv_scene_fps(s) : 30;
                ui_init();
                ui_head("encode", out);
                ui_row("source", "%s, %dx%d, %d frames @%g fps  (%s)",
                       fdv_scene_name(s), w, h, nf, sf,
                       fdv_dur(dd, sizeof dd, nf / sf));
                ui_row("coding", "%s%s", gop_str(gg, sizeof gg, keyint, sf),
                       band > 0 ? ", tile-parallel" : "");
            }
            {   char ra[32], rb[32];
                double sfps = fdv_scene_fps(s) > 0 ? fdv_scene_fps(s) : 30;
                double got = (double)flen * 8 / (nf / sfps);
                double miss = 100.0 * (got - (double)bitrate) / (double)bitrate;
                const char *col = (miss > 10.0 || miss < -10.0) ? P.yel : P.grn;
                ui_row("stream", "%s%s%s   %starget %s%s @ %s%+.1f%%%s",
                       P.bold, fdv_rate(ra, sizeof ra, got), P.rst,
                       P.dim, fdv_rate(rb, sizeof rb, (double)bitrate), P.rst,
                       col, miss, P.rst);
                ui_row("speed", "%.1f ms/frame  %s(%.0f fps)%s",
                       t_enc / nf, P.dim, 1000.0 * nf / t_enc, P.rst);
            }
            rc = 0;
        }
        free(abrbuf);
    } else if (!plen) {
        fprintf(stderr, "encode failed\n");
    } else {
        flen = fdv_wrap(buf, cap, band > 0 ? FDV_TILED : FDV_VIDEO,
                        fdv_scene_fps(s), payload, plen);
        if (!flen || write_file(out, buf, flen) != 0) {
            fprintf(stderr, "cannot write '%s'\n", out);
        } else {
            char r1[32], r2[32], d1[32];
            double fps = fdv_scene_fps(s);
            double secs = nf / (fps > 0 ? fps : 30);
            double bps  = (double)flen * 8 / secs;
            double raw  = (double)total * 8 / secs;
            ui_init();
            ui_head("encode", out);
            ui_row("source", "%s, %dx%d, %d frames @%g fps  (%s)",
                   fdv_scene_name(s), w, h, nf, fps, fdv_dur(d1, sizeof d1, secs));
            char g[64];
            ui_row("coding", "qp %d, %s%s", qp, gop_str(g, sizeof g, keyint, fps),
                   band > 0 ? ", tile-parallel" : "");
            ui_row("stream", "%s%s%s   %s(%.0fx under raw %s)%s",
                   P.bold, fdv_rate(r1, sizeof r1, bps), P.rst,
                   P.dim, raw / bps, fdv_rate(r2, sizeof r2, raw), P.rst);
            ui_row("speed", "%.1f ms/frame  %s(%.0f fps)%s",
                   t_enc / nf, P.dim, 1000.0 * nf / t_enc, P.rst);
            rc = 0;
        }
    }
    free(src); free(fp); free(buf);
    fdv_scene_free(s);
    return rc;
}

/* Dump what is actually in a .fdv file: the head, then each coded unit and how
 * big it is. Useful when a stream misbehaves and you need to know whether the
 * problem is in the file or in the decoder. */
/* Walk a video payload's frame blobs, collecting the QP each P-frame carries. */
static void scan_frame_qps(const uint8_t *pay, size_t paylen, int nframes,
                           int keyint, int *qlo, int *qhi) {
    size_t q = 8;                                /* past nframes, w, h, qp, keyint */
    for (int f = 0; f < nframes && q + 4 <= paylen; ++f) {
        uint32_t blob = (uint32_t)pay[q] | ((uint32_t)pay[q+1] << 8)
                      | ((uint32_t)pay[q+2] << 16) | ((uint32_t)pay[q+3] << 24);
        q += 4;
        if (q + blob > paylen) break;
        int intra = (f == 0) || (keyint > 0 && f % keyint == 0);
        if (!intra && blob >= 1) {               /* P-frame: its QP leads the blob */
            int fq = pay[q];
            if (fq < *qlo) *qlo = fq;
            if (fq > *qhi) *qhi = fq;
        }
        q += blob;
    }
}

static int do_info(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fdv info <in.fdv> [-v]\n"); return 1; }
    int verbose = 0;
    for (int i = 3; i < argc; ++i)
        if (!strcmp(argv[i], "-v")) verbose = 1;

    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read '%s'\n", argv[2]); return 1; }

    fdv_info vi;
    if (fdv_read(in, len, &vi) != 0) {
        fprintf(stderr, "'%s' is not a fdv stream this build understands\n", argv[2]);
        if (len >= 4)
            fprintf(stderr, "  (first bytes: %02x %02x %02x %02x)\n",
                    in[0], in[1], in[2], in[3]);
        free(in);
        return 1;
    }

    size_t fs = (size_t)vi.w * vi.h * 3 / 2;
    double secs = (double)vi.nframes / (vi.fps > 0 ? vi.fps : 25);
    char rb[32], rr[32], db[32], gg[64];
    double bps = (double)len * 8 / (secs > 0 ? secs : 1);
    double raw = (double)fs * 8 * (vi.fps > 0 ? vi.fps : 30);
    ui_init();
    ui_head("stream", argv[2]);
    ui_row("container", "FDV v%d, %s%s%s", vi.version,
           vi.kind == FDV_TILED ? P.yel : "",
           vi.kind == FDV_TILED ? "tile-parallel" : "single-stream",
           vi.kind == FDV_TILED ? P.rst : "");
    ui_row("video", "%dx%d, %d frames @%d fps  (%s)",
           vi.w, vi.h, vi.nframes, vi.fps, fdv_dur(db, sizeof db, secs));
    /* The header QP is only what the encoder opened with. Under rate control
     * every P-frame carries its own, so read them back rather than reporting a
     * number the stream may not have used anywhere. */
    int qlo = 99, qhi = -1;
    {
        /* A tiled payload is a short header then one ordinary video payload per
         * band; band 0 carries the same per-frame QPs as the rest, since rate
         * control hands every band the same one. */
        const uint8_t *pay = vi.payload;
        size_t paylen = vi.payload_len;
        if (vi.kind == FDV_TILED) {
            if (paylen < 16) paylen = 0;
            else {
                size_t b0 = 12;                  /* past w,h,nframes,qp,keyint,nbands,mbrows */
                uint32_t blen = (uint32_t)pay[b0] | ((uint32_t)pay[b0+1] << 8)
                              | ((uint32_t)pay[b0+2] << 16) | ((uint32_t)pay[b0+3] << 24);
                if (b0 + 4 + blen > paylen) paylen = 0;
                else { pay += b0 + 4; paylen = blen; }
            }
        }
        if (paylen > 8) scan_frame_qps(pay, paylen, vi.nframes, vi.keyint, &qlo, &qhi);
    }
    char gop[64];
    if (vi.keyint > 0 && vi.fps > 0)
        snprintf(gop, sizeof gop, "keyint %d (%.1fs)", vi.keyint,
                 (double)vi.keyint / vi.fps);
    else if (vi.keyint > 0) snprintf(gop, sizeof gop, "keyint %d", vi.keyint);
    else snprintf(gop, sizeof gop, "keyint 0 (only the first frame is intra)");
    (void)gop;
    if (qhi >= 0 && qlo != qhi)
        ui_row("coding", "qp %d..%d %s(rate controlled)%s, %s", qlo, qhi, P.dim, P.rst,
               gop_str(gg, sizeof gg, vi.keyint, vi.fps));
    else
        ui_row("coding", "qp %d, %s", qhi >= 0 ? qhi : vi.qp,
               gop_str(gg, sizeof gg, vi.keyint, vi.fps));
    if (vi.kind == FDV_TILED) ui_row("bands", "%d", vi.nbands);
    ui_row("stream", "%s%s%s   %s(%.0fx under raw %s)%s",
           P.bold, fdv_rate(rb, sizeof rb, bps), P.rst,
           P.dim, raw / (bps > 0 ? bps : 1), fdv_rate(rr, sizeof rr, raw), P.rst);

    /* Walk the coded units. Both kinds are a sequence of length-prefixed
     * blobs; only the header ahead of them differs. */
    size_t p = vi.kind == FDV_TILED ? 12 : 8;
    int n = vi.kind == FDV_TILED ? vi.nbands : vi.nframes;
    const char *what = vi.kind == FDV_TILED ? "band" : "frame";
    size_t sum = 0, big = 0, small = (size_t)-1;
    for (int i = 0; i < n; ++i) {
        if (p + 4 > vi.payload_len) { printf("  TRUNCATED at %s %d\n", what, i); break; }
        uint32_t blob = fdv_get32(vi.payload, &p);
        if (p + blob > vi.payload_len) { printf("  TRUNCATED inside %s %d\n", what, i); break; }
        int intra = vi.kind == FDV_TILED
                  ? 0 : (i == 0 || (vi.keyint > 0 && i % vi.keyint == 0));
        if (verbose) {
            char ru[32];
            double k = vi.kind == FDV_TILED ? 8.0 / (secs > 0 ? secs : 1)
                                            : 8.0 * (vi.fps > 0 ? vi.fps : 30);
            printf("  %s%-5s %-4d%s %s%s%s %11s  %sat %zu%s\n", P.dim, what, i, P.rst,
                   intra ? P.yel : "", vi.kind == FDV_TILED ? " " : (intra ? "I" : "P"),
                   intra ? P.rst : "", fdv_rate(ru, sizeof ru, (double)blob * k),
                   P.dim, p, P.rst);
        }
        sum += blob;
        if (blob > big) big = blob;
        if (blob < small) small = blob;
        p += blob;
    }
    if (!verbose) {
        /* Per-unit sizes as the rate they would imply if sustained -- what the
         * link has to absorb at the peak, which is the question a buffer asks. */
        char lo[32], hi[32], av[32];
        /* A frame's size is worth seeing as the rate it would imply if it were
         * sustained -- that is the peak the link has to absorb, which is the
         * question a buffer asks. A band is a whole sub-stream over the same
         * duration, so its rate is simply its share of the total. */
        double k = vi.kind == FDV_TILED ? 8.0 / (secs > 0 ? secs : 1)
                                        : 8.0 * (vi.fps > 0 ? vi.fps : 30);
        ui_row(vi.kind == FDV_TILED ? "per band" : "per frame",
               "%s..%s  %s(mean %s)%s  %s[-v to list]%s",
               fdv_rate(lo, sizeof lo, (double)small * k), fdv_rate(hi, sizeof hi, (double)big * k),
               P.dim, fdv_rate(av, sizeof av, n ? (double)sum / n * k : 0), P.rst,
               P.dim, P.rst);
    }
    if (p != vi.payload_len)
        printf("  note        %zu trailing byte(s) after the last %s\n",
               vi.payload_len - p, what);
    free(in);
    return 0;
}

/* Decode a .fdv file back to raw I420 (or Y4M, by extension). */
static int do_play_decode(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: fdv decode <in.fdv> <out.yuv|out.y4m> [threads] [-v|-vv]\n");
        return 1;
    }
    size_t len = 0;
    uint8_t *in = read_file(argv[2], &len);
    if (!in) { fprintf(stderr, "cannot read '%s'\n", argv[2]); return 1; }
    fdv_info vi;
    if (fdv_read(in, len, &vi) != 0) {
        fprintf(stderr, "'%s' is not a fdv stream\n", argv[2]); free(in); return 1;
    }
    int threads = 4, verbose = 0;
    for (int i = 4; i < argc; ++i) {
        if (verbosity_opt(argv[i], &verbose)) continue;
        threads = atoi(argv[i]);
    }
    verbosity_set(verbose);
    size_t fs = (size_t)vi.w * vi.h * 3 / 2;
    uint8_t *out = malloc(fs * (size_t)vi.nframes);
    if (!out) { fprintf(stderr, "out of memory\n"); free(in); return 1; }

    double t0 = now_ms();
    int rc = fdv_decode(&vi, out, threads);
    double t = now_ms() - t0;
    if (rc != 0) { fprintf(stderr, "decode failed\n"); free(in); free(out); return 1; }

    size_t n = strlen(argv[3]);
    int y4m = n > 4 && !strcmp(argv[3] + n - 4, ".y4m");
    rc = y4m ? fdv_y4m_write(argv[3], out, vi.w, vi.h, vi.nframes, vi.fps)
             : fdv_yuv_write(argv[3], out, fs * (size_t)vi.nframes);
    if (rc != 0) fprintf(stderr, "cannot write '%s'\n", argv[3]);
    else printf("decoded %d frame(s) %dx%d in %.2f ms (%.2f ms/frame) -> %s\n",
                vi.nframes, vi.w, vi.h, t, t / vi.nframes, argv[3]);
    free(in); free(out);
    return rc == 0 ? 0 : 1;
}

static void usage(void) {
    fprintf(stderr,
      "fdv — a clean-sheet, fast-decode video codec\n\n"
      "streams (.fdv files — self-describing, what the player opens):\n"
      "  encode   <scene|in.y4m> <out.fdv> [-q QP] [-b RATE] [-k GOP] [-n N]\n"
      "                                     [-s SIZE] [-t MBROWS] [-j THREADS]\n"
      "                                     [-v|-vv]\n"
      "  info     <in.fdv> [-v]                  dump a stream's structure\n"
      "  decode   <in.fdv> <out.yuv|.y4m> [threads] [-v|-vv]\n\n"
      "content (scenes are text files; see scenes/ or $FDV_SCENES):\n"
      "  scenes                                   list the scene files\n"
      "  scene    <name>                          print a scene, to copy and edit\n"
      "  gen      <scene> <out.yuv|.y4m> [frames] [-s SIZE] [-v]  render a clip\n"
      "  pipeline <scene> [-q QP] [-k KEYINT] [-n N] [-t MBROWS]\n"
      "                          [-j THREADS] [-o DIR] [-v|-vv|-vvv]\n"
      "                                           render, encode, decode, verify\n\n"
      "raw-file tools:\n"
      "  enc       <in.yuv> <w> <h> <nframes> <qp> <out.fdv> [keyint] [fps]\n"
      "  dec       <in.fdv> <out.yuv>\n"
      "  enctiled  <in.yuv> <w> <h> <nframes> <qp> <band_mbrows> <out.fdv> [keyint] [threads]\n"
      "  dectiled  <in.fdv> <out.yuv> [threads]\n"
      "  enctarget <in.yuv> <w> <h> <nframes> <bytes> <out.bin>\n"
      "  ency4m    <in.y4m> <qp> <out.bin>\n"
      "  decy4m    <in.bin> <out.y4m>\n"
      "  compare   <a.yuv> <b.yuv> <w> <h> <nframes>\n"
      "  bench     [w] [h] [nframes] [qp] [iters]\n"
      "  selftest  [w] [h] [nframes] [qp]\n");
}

/* ---------------------------------------------------------------------------
 * Scene-driven commands: render synthetic clips and run the full pipeline.
 * ------------------------------------------------------------------------ */

static int mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0777);
        *p = '/';
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* List the scene files that are available. There are no built-in scenes, so
 * this is a directory listing -- and each one's description is its first
 * comment line, which keeps the blurb with the scene rather than in a table
 * that drifts away from it. */
static int do_scenes(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *dir = fdv_scene_dir();
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "no scene directory '%s'\n", dir);
        fprintf(stderr, "  set FDV_SCENES, or run from a tree that has one\n");
        return 1;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".scn") != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **t = realloc(names, (size_t)cap * sizeof(*t));
            if (!t) break;
            names = t;
        }
        names[n] = strdup(e->d_name);
        if (!names[n]) break;
        ++n;
    }
    closedir(d);
    if (n == 0) {
        fprintf(stderr, "no .scn files in '%s'\n", dir);
        free(names);
        return 1;
    }
    qsort(names, (size_t)n, sizeof(*names), name_cmp);

    printf("scenes in %s/\n\n", dir);
    for (int i = 0; i < n; ++i) {
        char path[1024], blurb[256] = "";
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        fdv_scene_file_blurb(path, blurb, sizeof blurb);
        char err[256];
        fdv_scene *sc = fdv_scene_parse_file(path, err, sizeof err);
        char stem[128];
        snprintf(stem, sizeof stem, "%.*s", (int)(strlen(names[i]) - 4), names[i]);
        if (sc) {
            printf("  %-10s %4dx%-4d %3d frames  %s\n", stem,
                   fdv_scene_width(sc), fdv_scene_height(sc), fdv_scene_frames(sc), blurb);
            fdv_scene_free(sc);
        } else {
            printf("  %-10s (will not parse: %s)\n", stem, err);
        }
        free(names[i]);
    }
    free(names);
    printf("\n  fdv encode <name> out.fdv [-s 1080p]   encode one\n");
    printf("  fdv scene  <name>                       print it, to copy and edit\n");
    return 0;
}

/* Print a scene file. Scenes are the only source of content now, so the way to
 * make a new one is to start from an existing one. */
static int do_scene(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fdv scene <name|file.scn>\n"); return 1; }
    char path[1024];
    if (fdv_scene_find(argv[2], path, sizeof path) != 0) {
        fprintf(stderr, "no scene '%s' (try: fdv scenes)\n", argv[2]);
        return 1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot read '%s'\n", path); return 1; }
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, got, stdout);
    fclose(f);
    return 0;
}

/* Render a scene to .y4m (if the name ends that way) or raw .yuv. */
static int do_gen(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: fdv gen <scene> <out.yuv|.y4m> [frames] [-s SIZE] [-v]\n");
        return 1;
    }
    char err[256];
    fdv_scene *s = fdv_scene_open(argv[2], err, sizeof err);
    if (!s) { fprintf(stderr, "scene: %s\n", err); return 1; }

    const char *out = argv[3];
    int nf = 0, verbose = 0;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            if (apply_size(s, argv[++i]) != 0) { fdv_scene_free(s); return 1; }
        } else if (verbosity_opt(argv[i], &verbose)) { }
        else nf = atoi(argv[i]);
    }
    if (nf <= 0) nf = fdv_scene_frames(s);
    verbosity_set(verbose);
    if (verbose) fdv_scene_describe(s, stdout, "scene");

    size_t fs = fdv_scene_frame_size(s);
    uint8_t *seq = malloc(fs * (size_t)nf);
    if (!seq) { fprintf(stderr, "out of memory\n"); fdv_scene_free(s); return 1; }
    for (int f = 0; f < nf; ++f) fdv_scene_render(s, f, seq + (size_t)f * fs);

    size_t len = strlen(out);
    int y4m = len > 4 && !strcmp(out + len - 4, ".y4m");
    int rc = y4m ? fdv_y4m_write(out, seq, fdv_scene_width(s), fdv_scene_height(s), nf, fdv_scene_fps(s))
                 : fdv_yuv_write(out, seq, fs * (size_t)nf);
    if (rc != 0) {
        fprintf(stderr, "cannot write '%s'\n", out);
    } else {
        struct stat sb;                       /* report what actually landed on disk */
        long long bytes = stat(out, &sb) == 0 ? (long long)sb.st_size
                                              : (long long)(fs * (size_t)nf);
        printf("rendered %s: %dx%d %d frame(s) @%d fps -> %s (%lld bytes, %s)\n",
               fdv_scene_name(s), fdv_scene_width(s), fdv_scene_height(s), nf, fdv_scene_fps(s),
               out, bytes, y4m ? "Y4M" : "raw I420");
    }
    free(seq);
    fdv_scene_free(s);
    return rc == 0 ? 0 : 1;
}

/* The full pipeline: render -> encode -> decode -> verify, logging each stage.
 *
 * Every intermediate is written to the output directory, so a failure can be
 * inspected rather than just reported: source.yuv/.y4m, stream.bin,
 * decoded.yuv/.y4m. */
static int do_pipeline(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: codec pipeline <scene> [options]\n"
            "  -q QP        quantizer (default: the scene's, else 20)\n"
            "  -k KEYINT    key-frame interval (default: the scene's, else 0)\n"
            "  -n FRAMES    override the scene's frame count\n"
            "  -s SIZE      render at another resolution: WxH, or 720p/1080p/4k\n"
            "  -t MBROWS    use the tile-parallel path, MBROWS per band\n"
            "  -j THREADS   decode threads for -t (default 4)\n"
            "  -o DIR       artifact directory (default build/pipeline)\n"
            "  -v/-vv/-vvv  per-stage / per-frame / per-macroblock logging\n"
"               (-vvv needs the `make trace` build)\n");
        return 1;
    }

    char err[256];
    fdv_scene *s = fdv_scene_open(argv[2], err, sizeof err);
    if (!s) { fprintf(stderr, "scene: %s\n", err); return 1; }

    int qp = fdv_scene_qp(s) >= 0 ? fdv_scene_qp(s) : 20;
    int keyint = fdv_scene_keyint(s) >= 0 ? fdv_scene_keyint(s) : 0;
    int nf = fdv_scene_frames(s);
    int band = 0, threads = 4, verbose = 0;
    const char *dir = "build/pipeline";

    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        int has_val = i + 1 < argc;
        if      (!strcmp(a, "-q") && has_val) qp = atoi(argv[++i]);
        else if (!strcmp(a, "-k") && has_val) keyint = atoi(argv[++i]);
        else if (!strcmp(a, "-n") && has_val) nf = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && has_val) {
            if (apply_size(s, argv[++i]) != 0) { fdv_scene_free(s); return 1; }
        }
        else if (!strcmp(a, "-t") && has_val) band = atoi(argv[++i]);
        else if (!strcmp(a, "-j") && has_val) threads = atoi(argv[++i]);
        else if (!strcmp(a, "-o") && has_val) dir = argv[++i];
        else if (!strcmp(a, "-v"))   verbose = 1;
        else if (!strcmp(a, "-vv"))  verbose = 2;
        else if (!strcmp(a, "-vvv")) verbose = 3;
        else { fprintf(stderr, "unknown option '%s'\n", a); fdv_scene_free(s); return 1; }
    }
    if (nf <= 0) nf = fdv_scene_frames(s);
    if (qp < 0 || qp > 51) { fprintf(stderr, "qp must be 0..51\n"); fdv_scene_free(s); return 1; }

    fdv_log_set(verbose == 0 ? FDV_LOG_OFF
              : verbose == 1 ? FDV_LOG_INFO
              : verbose == 2 ? FDV_LOG_FRAME : FDV_LOG_BLOCK, stdout);
    if (verbose >= 3) {
#if !defined(FDV_TRACE)
        fprintf(stderr, "note: -vvv needs the tracing build; run "
                        "`make trace` and use build/codec-trace\n");
#endif
    }

    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "cannot create '%s'\n", dir);
        fdv_scene_free(s); return 1;
    }

    int w = fdv_scene_width(s), h = fdv_scene_height(s);
    size_t fs = fdv_scene_frame_size(s);
    size_t ys = (size_t)w * h, cs = fs > ys ? (fs - ys) / 2 : 0;
    size_t total = fs * (size_t)nf;

    /* ---- stage 1: scene ---- */
    printf("== 1. scene =====================================================\n");
    fdv_scene_describe(s, stdout, "scene");
    printf("[scene ] source '%s' -> %d frame(s) of %dx%d I420 (%zu B/frame, %zu B total)\n",
           argv[2], nf, w, h, fs, total);

    uint8_t *src = malloc(total);
    uint8_t *dec = malloc(total);
    size_t   cap = total * 2 + 65536;
    uint8_t *bs  = malloc(cap);
    const uint8_t **fp = malloc((size_t)nf * sizeof(*fp));
    fdv_frame_stat *est = calloc((size_t)nf, sizeof(*est));
    fdv_frame_stat *dst = calloc((size_t)nf, sizeof(*dst));
    if (!src || !dec || !bs || !fp || !est || !dst) {
        fprintf(stderr, "out of memory\n");
        free(src); free(dec); free(bs); free(fp); free(est); free(dst);
        fdv_scene_free(s); return 1;
    }

    /* ---- stage 2: render ---- */
    printf("\n== 2. render ====================================================\n");
    double t0 = now_ms();
    for (int f = 0; f < nf; ++f) {
        uint8_t *cur = src + (size_t)f * fs;
        fdv_scene_render(s, f, cur);
        fp[f] = cur;
    }
    double t_render = now_ms() - t0;
    for (int f = 0; f < nf; ++f) {
        const uint8_t *cur = src + (size_t)f * fs;
        fdv_scene_info fi = fdv_scene_measure(cur, f ? cur - fs : NULL, w, h);
        printf("[render] frame %-4d  mean Y %6.2f  sd %6.2f  motion(mad) %6.2f\n",
               f, fi.mean, fi.sd, fi.mad);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/source.yuv", dir);
    fdv_yuv_write(path, src, total);
    snprintf(path, sizeof path, "%s/source.y4m", dir);
    fdv_y4m_write(path, src, w, h, nf, fdv_scene_fps(s));
    printf("[render] %d frame(s) in %.2f ms -> %s/source.yuv + source.y4m\n",
           nf, t_render, dir);

    /* ---- stage 3: encode ---- */
    printf("\n== 3. encode ====================================================\n");
    printf("[encode] qp=%d keyint=%d path=%s\n", qp, keyint,
           band > 0 ? "tile-parallel (vtile)" : "single-stream (video)");
    fdv_stats es = {est, nf, 0};
    fdv_stats_set(&es);
    t0 = now_ms();
    size_t blen = band > 0
        ? fdv_vtile_encode(fp, nf, w, h, qp, keyint, band, threads, bs, cap)
        : fdv_video_encode(fp, nf, w, h, qp, keyint, bs, cap);
    double t_enc = now_ms() - t0;
    fdv_stats_set(NULL);

    if (blen == 0) {
        fprintf(stderr, "[encode] FAILED\n");
        free(src); free(dec); free(bs); free(fp); free(est); free(dst);
        fdv_scene_free(s); return 1;
    }
    if (band == 0) {
        printf("[encode] %-5s %9s %9s  %-42s %s\n",
               "frame", "bytes", "ms", "macroblock modes", "entropy / symbols");
        for (int i = 0; i < es.n; ++i) {
            const fdv_frame_stat *e = &est[i];
            char modes[64];
            if (e->is_intra)
                snprintf(modes, sizeof modes, "%d intra (I-frame)", e->mb_intra);
            else
                snprintf(modes, sizeof modes, "skip %-5d inter16 %-4d inter8 %-4d intra %-4d",
                         e->mb_skip, e->mb_inter16, e->mb_inter8, e->mb_intra);
            printf("[encode] %-5d %9zu %9.2f  %-42s %-8s S=%zu C=%zu\n",
                   e->index, e->bytes, e->ms, modes,
                   fdv_emode_name(e->entropy_mode), e->sym_struct, e->sym_coeff);
        }
    }
    snprintf(path, sizeof path, "%s/stream.bin", dir);
    write_file(path, bs, blen);
    double mpix = (double)ys * nf / 1e6;
    printf("[encode] %zu B total (%.1f B/frame, %.1fx vs raw) in %.2f ms (%.1f Mpix/s)\n",
           blen, (double)blen / nf, (double)total / (double)blen, t_enc,
           t_enc > 0 ? mpix / (t_enc / 1e3) : 0.0);
    printf("[encode] -> %s/stream.bin\n", dir);

    /* ---- stage 4: decode ---- */
    printf("\n== 4. decode ====================================================\n");
    fdv_stats ds = {dst, nf, 0};
    fdv_stats_set(&ds);
    int dnf = 0, dw = 0, dh = 0;
    t0 = now_ms();
    int rc = band > 0
        ? fdv_vtile_decode(bs, blen, dec, &dnf, &dw, &dh, threads)
        : fdv_video_decode(bs, blen, dec, &dnf, &dw, &dh);
    double t_dec = now_ms() - t0;
    fdv_stats_set(NULL);

    if (rc != 0) {
        fprintf(stderr, "[decode] FAILED (rc=%d)\n", rc);
        free(src); free(dec); free(bs); free(fp); free(est); free(dst);
        fdv_scene_free(s); return 1;
    }
    printf("[decode] geometry %dx%d %d frame(s) %s (expected %dx%d %d)\n",
           dw, dh, dnf, (dw == w && dh == h && dnf == nf) ? "OK" : "MISMATCH", w, h, nf);
    for (int i = 0; i < ds.n; ++i)
        printf("[decode] frame %-4d %c  %9zu B consumed  %8.3f ms  (%.1f Mpix/s)\n",
               dst[i].index, dst[i].is_intra ? 'I' : 'P', dst[i].bytes, dst[i].ms,
               dst[i].ms > 0 ? (double)ys / 1e6 / (dst[i].ms / 1e3) : 0.0);
    snprintf(path, sizeof path, "%s/decoded.yuv", dir);
    fdv_yuv_write(path, dec, total);
    snprintf(path, sizeof path, "%s/decoded.y4m", dir);
    fdv_y4m_write(path, dec, w, h, nf, fdv_scene_fps(s));
    printf("[decode] %.2f ms (%.1f Mpix/s, %.0f fps) -> %s/decoded.yuv + decoded.y4m\n",
           t_dec, t_dec > 0 ? mpix / (t_dec / 1e3) : 0.0,
           t_dec > 0 ? nf / (t_dec / 1e3) : 0.0, dir);

    /* ---- stage 5: verify ---- */
    printf("\n== 5. verify ====================================================\n");
    double sy = 0, su = 0, sv = 0, miny = 1e18;
    int worst = 0;
    for (int f = 0; f < nf; ++f) {
        const uint8_t *a = src + (size_t)f * fs, *b = dec + (size_t)f * fs;
        double py = psnr(a, b, ys);
        double pu = psnr(a + ys, b + ys, cs);
        double pv = psnr(a + ys + cs, b + ys + cs, cs);
        sy += py; su += pu; sv += pv;
        if (py < miny) { miny = py; worst = f; }
        printf("[verify] frame %-4d  Y %6.2f  U %6.2f  V %6.2f dB   %8zu B\n",
               f, py, pu, pv, est[f].bytes);
    }
    printf("[verify] mean  Y %6.2f  U %6.2f  V %6.2f dB\n", sy / nf, su / nf, sv / nf);
    printf("[verify] worst Y %6.2f dB at frame %d\n", miny, worst);

    ui_init();
    printf("\n%s== summary %s%s\n", P.bold,
           "=====================================================", P.rst);
    printf("  scene      %s (%dx%d, %d frames @%d fps)\n",
           fdv_scene_name(s), w, h, nf, fdv_scene_fps(s));
    printf("  settings   qp %d, keyint %d, %s\n", qp, keyint,
           band > 0 ? "tile-parallel" : "single-stream");
    {   char rs[32], rr[32];
        double sfps = fdv_scene_fps(s) > 0 ? fdv_scene_fps(s) : 30;
        double bps = (double)blen * 8 * sfps / nf;
        double raw = (double)fs * 8 * sfps;
        printf("  bitstream  %s%s%s  (%.0fx under raw %s)\n",
               P.bold, fdv_rate(rs, sizeof rs, bps), P.rst,
               raw / (bps > 0 ? bps : 1), fdv_rate(rr, sizeof rr, raw));
    }
    printf("  quality    Y %.2f dB mean, %.2f dB worst\n", sy / nf, miny);
    printf("  speed      encode %.1f ms total, decode %.2f ms total, %.0fx asymmetry\n",
           t_enc, t_dec, t_dec > 0 ? t_enc / t_dec : 0.0);
    if (band > 0) {
        /* On the tile-parallel path the sink does not describe frames: the
         * encoder runs one fdv_video_encode per band (so each band overwrites the
         * previous band's records), and fdv_vtile_decode suspends the sink while
         * its worker threads run.  Report the honest derived averages instead
         * of per-band numbers mislabelled as per-frame. */
        printf("  per frame  derived from totals: encode %.1f ms, decode %.3f ms"
               "  (%d band(s), %d thread(s); per-frame stats are single-stream only)\n",
               t_enc / nf, t_dec / nf, band > 0 ? (h + band * 16 - 1) / (band * 16) : 1,
               threads);
    } else {
        /* Per-frame spread, split by frame type: an I-frame costs very
         * different amounts from a P-frame, and a single mean hides that. */
        struct { const char *what; const fdv_frame_stat *r; int n; }
            side[2] = {{"encode", est, es.n}, {"decode", dst, ds.n}};
        for (int k = 0; k < 2; ++k) {
            double lo = 1e18, hi = 0, sum = 0, iframe = 0, psum = 0;
            int np = 0;
            for (int i = 0; i < side[k].n; ++i) {
                double m = side[k].r[i].ms;
                if (m < lo) lo = m;
                if (m > hi) hi = m;
                sum += m;
                if (side[k].r[i].is_intra) iframe = m;
                else { psum += m; ++np; }
            }
            if (side[k].n == 0) continue;
            printf("  per frame  %s  mean %8.3f ms  min %8.3f  max %8.3f"
                   "   (I %.3f, P mean %.3f)  = %.1f fps\n",
                   side[k].what, sum / side[k].n, lo, hi, iframe,
                   np ? psum / np : 0.0,
                   sum > 0 ? side[k].n / (sum / 1e3) : 0.0);
        }
    }
    printf("  artifacts  %s/{source,decoded}.{yuv,y4m}, %s/stream.bin\n", dir, dir);
    {   int ok = (dw == w && dh == h && dnf == nf);
        printf("  result     %s%s%s\n", ok ? P.grn : P.red,
               ok ? "PASS" : "FAIL (geometry)", P.rst);
    }

    free(src); free(dec); free(bs); free(fp); free(est); free(dst);
    fdv_scene_free(s);
    fdv_log_set(FDV_LOG_OFF, NULL);
    return (dw == w && dh == h && dnf == nf) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    /* --profile is global rather than per-subcommand: pull it out of argv so
     * every mode can be profiled without each one parsing the flag. */
    int profile = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--profile") != 0) continue;
        profile = 1;
        for (int k = i; k < argc - 1; ++k) argv[k] = argv[k + 1];
        --argc; --i;
    }
    if (profile && !fdv_profile_available()) {
        fprintf(stderr, "note: this build has no profiler; "
                        "run `make profile` and use build/fdv-profile\n");
    }
    if (profile) fdv_profile_reset();
    double t_profile0 = now_ms();
    int rc = 1;
    if (0) { }
    else if (!strcmp(argv[1], "enc"))       rc = do_enc(argc, argv);
    else if (!strcmp(argv[1], "dec"))       rc = do_dec(argc, argv);
    else if (!strcmp(argv[1], "enctiled"))  rc = do_enctiled(argc, argv);
    else if (!strcmp(argv[1], "dectiled"))  rc = do_dectiled(argc, argv);
    else if (!strcmp(argv[1], "enctarget")) rc = do_enctarget(argc, argv);
    else if (!strcmp(argv[1], "ency4m"))    rc = do_ency4m(argc, argv);
    else if (!strcmp(argv[1], "decy4m"))    rc = do_decy4m(argc, argv);
    else if (!strcmp(argv[1], "compare"))   rc = do_compare(argc, argv);
    else if (!strcmp(argv[1], "bench"))     rc = do_bench(argc, argv);
    else if (!strcmp(argv[1], "selftest"))  rc = do_selftest(argc, argv);
    else if (!strcmp(argv[1], "scenes"))    rc = do_scenes(argc, argv);
    else if (!strcmp(argv[1], "scene"))     rc = do_scene(argc, argv);
    else if (!strcmp(argv[1], "gen"))       rc = do_gen(argc, argv);
    else if (!strcmp(argv[1], "pipeline"))  rc = do_pipeline(argc, argv);
    else if (!strcmp(argv[1], "encode"))    rc = do_encode(argc, argv);
    else if (!strcmp(argv[1], "info"))      rc = do_info(argc, argv);
    else if (!strcmp(argv[1], "decode"))    rc = do_play_decode(argc, argv);
    else {
        fprintf(stderr, "unknown mode '%s'\n", argv[1]);
        usage();
        return 1;
    }

    if (profile) fdv_profile_report(stdout, argv[1], now_ms() - t_profile0);
    return rc;
}
