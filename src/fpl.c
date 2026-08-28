/* fpl — a small player for .fdv streams.
 *
 * Opens a coded file, decodes it, and shows it at the right size and rate.
 * The point is to watch the codec's output rather than trust a PSNR number:
 * blocking, drift, a band boundary or a dropped frame are obvious on screen
 * and invisible in a summary line.
 *
 * Frames are decoded on demand, not all at once. Holding a whole clip costs
 * nframes * w * h * 3/2 — 3.1 MB per frame at 1080p, so ten minutes would want
 * tens of gigabytes. Instead a streaming decoder pulls one frame at a time and
 * a bounded cache keeps the recent ones, so memory is set by the cache budget
 * rather than by the length of the clip. Short clips still end up entirely
 * cached, which is the old behaviour arrived at honestly.
 *
 * Frames arrive through fdv_dec_*, so nothing here assumes the clip fits in
 * memory. Bands of a tile-parallel stream are decoded in order rather than in
 * parallel: at one frame per display refresh there is nothing to gain, and the
 * threaded path is still there in fdv_decode for whole-file work.
 *
 * Layout:
 *   1. COLOR     YUV 4:2:0 -> RGB
 *   2. FIT       window and letterbox geometry
 *   3. HUD       the overlay
 *   4. MAIN      load, decode, and the playback loop
 */

#define FDV_IMPLEMENTATION
#include "fdv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raylib.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* ===========================================================================
 * 1. COLOR
 * YUV 4:2:0 -> RGB.
 * ======================================================================== */

/* Two conventions, because a file cannot say which it is.
 *
 * "full" maps Y 0..255 straight to luminance (what this codec's own scene
 * renderer produces, and what JPEG uses). "tv" is the BT.601 studio range,
 * where 16 is black and 235 is white -- the usual interpretation of I420 from
 * a camera or another encoder. Showing full-range content with the tv matrix
 * crushes the blacks and clips the highlights, so it is worth a flag rather
 * than a guess. */
typedef enum { RANGE_FULL = 0, RANGE_TV = 1 } fdv_color_range;

/* Fixed-point BT.601 coefficients, 8-bit fractions.
 *   R = Y + 1.402  (V-128)
 *   G = Y - 0.3441 (U-128) - 0.7141 (V-128)
 *   B = Y + 1.772  (U-128)                                */
enum { CR_V = 359, CG_U = 88, CG_V = 183, CB_U = 454 };

/* Named apart from the codec's own clamp: play.c defines
 * FDV_IMPLEMENTATION, so both live in this translation unit. */
static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Convert one I420 frame to packed RGB.
 *
 * Chroma is upsampled by repetition: each 2x2 luma quad shares one chroma
 * sample. A player could interpolate, but repetition is what the decoder's own
 * reconstruction assumed, so this shows the frame the codec actually produced
 * rather than a prettier version of it. */
static void yuv420_to_rgb(const uint8_t *yuv, int w, int h,
                          uint8_t *rgb, fdv_color_range range) {
    int cw = w / 2, ch = h / 2;
    const uint8_t *Y = yuv;
    const uint8_t *U = yuv + (size_t)w * h;
    const uint8_t *V = U + (size_t)cw * ch;

    /* Studio range stretches 16..235 to 0..255 (a gain of 255/219). */
    int y_off  = (range == RANGE_TV) ? 16 : 0;
    int y_gain = (range == RANGE_TV) ? 298 : 256;      /* 1.164 and 1.0, /256 */

    for (int j = 0; j < h; ++j) {
        const uint8_t *yr = Y + (size_t)j * w;
        const uint8_t *ur = U + (size_t)(j >> 1) * cw;
        const uint8_t *vr = V + (size_t)(j >> 1) * cw;
        uint8_t *out = rgb + (size_t)j * w * 3;
        for (int i = 0; i < w; ++i) {
            int c = ((yr[i] - y_off) * y_gain) >> 8;
            int d = ur[i >> 1] - 128;
            int e = vr[i >> 1] - 128;
            *out++ = (uint8_t)clamp8(c + ((CR_V * e) >> 8));
            *out++ = (uint8_t)clamp8(c - ((CG_U * d + CG_V * e) >> 8));
            *out++ = (uint8_t)clamp8(c + ((CB_U * d) >> 8));
        }
    }
}

/* ===========================================================================
 * 2. FIT
 * Window and letterbox geometry.
 * ======================================================================== */

/* Pick a window size: fill a good part of the display without changing shape.
 *
 * Small clips are scaled *up*. The built-in scenes are 320x192, and opening
 * those at native size gives a window smaller than this paragraph -- which
 * looks like a broken player rather than a small video. Upscaling uses an
 * integer factor so each source pixel stays a square block of screen pixels;
 * combined with nearest-neighbour filtering that shows the codec's actual
 * output, blocking and all, instead of a smoothed version of it.
 *
 * Large clips are scaled down by whatever fraction fits, where smoothing is
 * what you want. Returns the factor so the caller can pick the filter. */
static double fit_window(int vw, int vh, int mw, int mh, int *ww, int *wh) {
    double budget = 0.85;                       /* leave room for the menu bar */
    double sx = (mw * budget) / vw, sy = (mh * budget) / vh;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) {
        s = floor(s);                           /* integer upscale, square pixels */
        if (s > 8.0) s = 8.0;
    } else if (s <= 0.0) {
        s = 1.0;
    }
    *ww = (int)(vw * s + 0.5);
    *wh = (int)(vh * s + 0.5);
    if (*ww < 160) *ww = 160;
    if (*wh < 120) *wh = 120;
    return s;
}

/* Largest centred rectangle of the video's aspect that fits the window. */
static Rectangle letterbox(int vw, int vh, int ww, int wh) {
    double s = (double)ww / vw;
    double sy = (double)wh / vh;
    if (sy < s) s = sy;
    float dw = (float)(vw * s), dh = (float)(vh * s);
    return (Rectangle){ (ww - dw) * 0.5f, (wh - dh) * 0.5f, dw, dh };
}

/* ===========================================================================
 * 3. HUD
 * The overlay.
 * ======================================================================== */

typedef struct {
    const char *path;
    fdv_info   info;
    size_t      file_len;
    double      first_ms;        /* cost of the first frame, measured at open */
    int         ncache;
} clip_meta;

static void draw_hud(const clip_meta *m, int frame, int playing, int loop,
                     double speed, double shown_fps) {
    char l1[256], l2[256], l3[256];
    const char *base = strrchr(m->path, '/');
    base = base ? base + 1 : m->path;
    double secs = (double)m->info.nframes / m->info.fps;

    snprintf(l1, sizeof l1, "%s   %dx%d  %s  qp %d",
             base, m->info.w, m->info.h,
             m->info.kind == FDV_TILED ? "tile-parallel" : "single-stream",
             m->info.qp);
    snprintf(l2, sizeof l2, "frame %d/%d   %.2fs / %.2fs   %s  x%.2g   %s",
             frame + 1, m->info.nframes,
             (double)frame / m->info.fps, secs,
             playing ? "playing" : "paused", speed, loop ? "loop" : "once");
    {   char rr[32];
        snprintf(l3, sizeof l3, "%s   first frame %.1f ms   cache %d   %.0f fps",
                 fdv_rate(rr, sizeof rr, (double)m->file_len * 8 / secs),
                 m->first_ms, m->ncache, shown_fps);
    }

    int pad = 10, lh = 20;
    int wmax = MeasureText(l1, 18);
    if (MeasureText(l2, 18) > wmax) wmax = MeasureText(l2, 18);
    if (MeasureText(l3, 18) > wmax) wmax = MeasureText(l3, 18);
    DrawRectangle(0, 0, wmax + 2 * pad, 3 * lh + 2 * pad, Fade(BLACK, 0.65f));
    DrawText(l1, pad, pad,          18, RAYWHITE);
    DrawText(l2, pad, pad + lh,     18, (Color){170, 210, 255, 255});
    DrawText(l3, pad, pad + 2 * lh, 18, (Color){160, 160, 160, 255});

    /* Progress bar along the bottom, with a tick at each key frame so the GOP
     * structure is visible while scrubbing. */
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int bar = 6;
    DrawRectangle(0, sh - bar, sw, bar, Fade(BLACK, 0.6f));
    DrawRectangle(0, sh - bar, (int)((double)(frame + 1) / m->info.nframes * sw), bar,
                  (Color){120, 190, 255, 255});
    if (m->info.keyint > 0)
        for (int f = 0; f < m->info.nframes; f += m->info.keyint)
            DrawRectangle((int)((double)f / m->info.nframes * sw), sh - bar, 2, bar,
                          (Color){255, 220, 120, 255});
}

static void draw_help(void) {
    static const char *keys[] = {
        "space  play / pause",
        ".  ,   step one frame",
        "-> <-  seek one second",
        "0      restart",
        "l      loop",
        "f      fullscreen",
        "h      hide this / the overlay",
        "r      colour range (full / tv)",
        "esc    quit",
    };
    int n = (int)(sizeof keys / sizeof *keys);
    int lh = 20, pad = 10, wmax = 0;
    for (int i = 0; i < n; ++i) {
        int t = MeasureText(keys[i], 18);
        if (t > wmax) wmax = t;
    }
    int x = GetScreenWidth() - wmax - 2 * pad;
    int y = GetScreenHeight() - n * lh - 2 * pad - 12;
    DrawRectangle(x, y, wmax + 2 * pad, n * lh + 2 * pad, Fade(BLACK, 0.65f));
    for (int i = 0; i < n; ++i)
        DrawText(keys[i], x + pad, y + pad + i * lh, 18, (Color){200, 200, 200, 255});
}

/* ===========================================================================
 * 4. MAIN
 * Load, decode, and the playback loop.
 * ======================================================================== */

/* Wall clock, not clock(): a tile-parallel decode runs on several threads, and
 * CPU time would report the sum of them rather than how long it took. */
static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static uint8_t *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

/* Write one converted frame as a binary PPM and exit without opening a window.
 * Being able to ask "what exactly did frame N decode to" without a display is
 * what makes the colour path checkable rather than merely watchable. */
static int dump_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    int ok = fwrite(rgb, 1, (size_t)w * h * 3, f) == (size_t)w * h * 3;
    fclose(f);
    return ok ? 0 : -1;
}

/* Hand back frame `f`, decoding it if the cache does not already hold it.
 *
 * The cache is direct-mapped on frame index, which suits how a player actually
 * moves: forward one at a time, occasionally back a little. A miss forward by
 * one is a single decode; a miss backwards costs a seek, which replays from the
 * preceding key frame -- the price of inter-frame prediction, not of this cache.
 * Returns NULL only if the stream itself failed. */
static uint8_t *frame_at(fdv_decoder *dec, uint8_t *cache, int *cache_idx,
                         int ncache, size_t fsize, int f) {
    int slot = f % ncache;
    if (cache_idx[slot] == f) return cache + (size_t)slot * fsize;

    if (fdv_dec_pos(dec) != f && fdv_dec_seek(dec, f) != 0) return NULL;
    if (fdv_dec_next(dec, cache + (size_t)slot * fsize) != 1) return NULL;
    cache_idx[slot] = f;
    return cache + (size_t)slot * fsize;
}

static void usage(void) {
    fprintf(stderr,
        "fpl — play an .fdv stream\n\n"
        "usage: fpl <file.fdv> [options]\n"
        "  --cache MB       frame cache budget (default 64)\n"
        "  --tv             treat the video as BT.601 studio range (16..235)\n"
        "  --scale N        window scale factor (default: fit the display)\n"
        "  --dump N OUT.ppm write frame N as a PPM and exit (no window)\n"
        "  --grab N OUT.ppm draw frame N, read it back off the GPU, exit\n"
        "  --log-frames     report each displayed frame on stderr\n"
        "  -v -vv -vvv      codec logging: per-stage / per-frame / per-macroblock\n"
        "                   (-vvv needs the `make trace` build)\n\n"
        "make one with:  fdv encode motion clip.fdv\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    const char *path = argv[1];
    double scale = 0.0;                          /* 0 means fit to the display */
    fdv_color_range range = RANGE_FULL;
    int dump_frame = -1, grab_frame = -1, log_frames = 0, verbose = 0;
    int cache_mb = 64;
    const char *dump_path = NULL, *grab_path = NULL;
    for (int i = 2; i < argc; ++i) {
        int has = i + 1 < argc;
        if      (!strcmp(argv[i], "--scale") && has) scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tv"))           range = RANGE_TV;
        else if (!strcmp(argv[i], "--log-frames"))   log_frames = 1;
        else if (!strcmp(argv[i], "-v"))             verbose = 1;
        else if (!strcmp(argv[i], "-vv"))            verbose = 2;
        else if (!strcmp(argv[i], "-vvv"))           verbose = 3;
        else if (!strcmp(argv[i], "--cache") && has) cache_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) {
            dump_frame = atoi(argv[++i]);
            dump_path  = argv[++i];
        }
        else if (!strcmp(argv[i], "--grab") && i + 2 < argc) {
            grab_frame = atoi(argv[++i]);
            grab_path  = argv[++i];
        }
        else { fprintf(stderr, "unknown option '%s'\n\n", argv[i]); usage(); return 1; }
    }

    /* ---- load and decode, before any window exists ---- */
    clip_meta m = {0};
    m.path = path;
    uint8_t *file = read_whole(path, &m.file_len);
    if (!file) { fprintf(stderr, "cannot read '%s'\n", path); return 1; }
    if (fdv_read(file, m.file_len, &m.info) != 0) {
        fprintf(stderr, "'%s' is not an fdv stream (try: fdv info %s)\n", path, path);
        free(file);
        return 1;
    }

    int vw = m.info.w, vh = m.info.h, nf = m.info.nframes;
    size_t fsize = (size_t)vw * vh * 3 / 2;

    /* Cache enough frames to fit the budget, capped at the clip length. Two is
     * the floor: stepping backwards one frame should not force a reseek. */
    int ncache = (int)((size_t)cache_mb * 1024 * 1024 / fsize);
    if (ncache > nf) ncache = nf;
    if (ncache < 2)  ncache = 2;

    uint8_t *cache = malloc(fsize * (size_t)ncache);
    int *cache_idx = malloc((size_t)ncache * sizeof(*cache_idx));
    uint8_t *rgb = malloc((size_t)vw * vh * 3);
    if (!cache || !cache_idx || !rgb) {
        fprintf(stderr, "out of memory for a %d-frame cache of %dx%d\n", ncache, vw, vh);
        free(file); free(cache); free(cache_idx); free(rgb);
        return 1;
    }
    for (int i = 0; i < ncache; ++i) cache_idx[i] = -1;

    /* Turn the codec's own logging on before decoding, not after: everything it
     * has to say happens inside this one call. Without this the player is
     * silent about the decode it just did, which is the opposite of the point.
     *
     * The stats sink gives the same information in structured form, so the
     * per-frame sizes and times are available afterwards for the overlay. */
    fdv_log_set(verbose == 0 ? FDV_LOG_OFF
              : verbose == 1 ? FDV_LOG_INFO
              : verbose == 2 ? FDV_LOG_FRAME : FDV_LOG_BLOCK, stdout);
#if !defined(FDV_TRACE)
    if (verbose >= 3)
        fprintf(stderr, "note: -vvv needs the tracing build; run "
                        "`make trace` and use build/fdv-trace, or rebuild fpl "
                        "with -DFDV_TRACE\n");
#endif
    fdv_frame_stat *stat = calloc((size_t)nf, sizeof(*stat));
    fdv_stats sink = {stat, nf, 0};
    if (stat) fdv_stats_set(&sink);

    fdv_decoder *dec = fdv_dec_open(&m.info);
    if (!dec) {
        fprintf(stderr, "decode failed — the stream is corrupt or truncated\n");
        free(file); free(cache); free(cache_idx); free(rgb);
        return 1;
    }

    /* Decode the first frame now: it proves the stream is good and gives the
     * per-frame cost to report, before a window exists to hide a failure. */
    double t0 = wall_ms();
    if (fdv_dec_next(dec, cache) != 1) {
        fprintf(stderr, "decode failed on the first frame\n");
        fdv_dec_close(dec);
        free(file); free(cache); free(cache_idx); free(rgb);
        return 1;
    }
    m.first_ms = wall_ms() - t0;
    cache_idx[0] = 0;
    fdv_stats_set(NULL);
    m.ncache = ncache;
    {   fdv_palette P; fdv_palette_for(stdout, &P);
        char rr[32], dd[32];
        double secs0 = (double)nf / (m.info.fps > 0 ? m.info.fps : 30);
        printf("%splay%s %s%s%s\n", P.bold, P.rst, P.cyn, path, P.rst);
        printf("  %s%-10s%s %dx%d, %d frames @%d fps  (%s)\n", P.dim, "video", P.rst,
               vw, vh, nf, m.info.fps, fdv_dur(dd, sizeof dd, secs0));
        printf("  %s%-10s%s %s%s%s, qp %d\n", P.dim, "coding", P.rst,
               m.info.kind == FDV_TILED ? P.yel : "",
               m.info.kind == FDV_TILED ? "tile-parallel" : "single-stream",
               m.info.kind == FDV_TILED ? P.rst : "", m.info.qp);
        printf("  %s%-10s%s %s%s%s\n", P.dim, "stream", P.rst, P.bold,
               fdv_rate(rr, sizeof rr, (double)m.file_len * 8 / secs0), P.rst);
        printf("  %s%-10s%s first frame %.2f ms, %d of %d frames cached\n",
               P.dim, "streaming", P.rst, m.first_ms, ncache, nf);
    }
    if (verbose && stat && sink.n) {
        /* The tiled path suspends the sink across its worker threads, so this
         * is empty there by design rather than by accident. */
        size_t small = (size_t)-1, big = 0, sum = 0;
        int intra = 0;
        double slowest = 0.0;
        for (int i = 0; i < sink.n; ++i) {
            if (stat[i].bytes < small) small = stat[i].bytes;
            if (stat[i].bytes > big)   big   = stat[i].bytes;
            if (stat[i].ms > slowest)  slowest = stat[i].ms;
            sum += stat[i].bytes;
            intra += stat[i].is_intra;
        }
        {   char lo[32], hi[32], av[32];
            double k = 8.0 * (m.info.fps > 0 ? m.info.fps : 30);
            printf("  decoded so far: %d intra + %d inter, %s..%s (mean %s), "
                   "slowest %.2f ms\n",
                   intra, sink.n - intra,
                   fdv_rate(lo, sizeof lo, (double)small * k),
                   fdv_rate(hi, sizeof hi, (double)big * k),
                   fdv_rate(av, sizeof av, (double)sum / sink.n * k), slowest);
        }
    }
    free(stat);
    fflush(stdout);

    if (dump_path) {
        if (dump_frame < 0 || dump_frame >= nf) {
            fprintf(stderr, "frame %d is outside 0..%d\n", dump_frame, nf - 1);
            fdv_dec_close(dec); free(file); free(cache); free(cache_idx); free(rgb);
            return 1;
        }
        uint8_t *fr = frame_at(dec, cache, cache_idx, ncache, fsize, dump_frame);
        if (!fr) { fprintf(stderr, "cannot decode frame %d\n", dump_frame); return 1; }
        yuv420_to_rgb(fr, vw, vh, rgb, range);
        int rc = dump_ppm(dump_path, rgb, vw, vh);
        if (rc != 0) fprintf(stderr, "cannot write '%s'\n", dump_path);
        else printf("frame %d -> %s (%dx%d RGB, %s range)\n",
                    dump_frame, dump_path, vw, vh,
                    range == RANGE_TV ? "tv" : "full");
        fdv_dec_close(dec); free(file); free(cache); free(cache_idx); free(rgb);
        return rc == 0 ? 0 : 1;
    }

    /* ---- window ----
     *
     * The final size is not known until the monitor can be measured, and the
     * monitor cannot be measured until a window exists -- so the window is
     * created, resized, and then explicitly re-centred. The last step is the
     * one that matters: a resize keeps the top-left corner where it was and
     * grows down and right from there, so without it the window ends up
     * somewhere arbitrary, and partly off-screen for a large clip.
     *
     * Creating it hidden and revealing it after would avoid the brief resize,
     * but raylib centres the window inside InitWindow and that path computes a
     * garbage origin when the window is hidden -- AppKit then rejects the frame
     * and the process dies before main() gets control again. Not worth it. */
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT;
    /* Render at the display's real pixel density, so an upscaled clip is crisp
     * rather than a blurry stretch of a low-resolution framebuffer. Left off
     * for --grab, where the readback has to match the video's own size. */
    if (!grab_path) flags |= FLAG_WINDOW_HIGHDPI;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(vw, vh, "fpl");
    int mon = GetCurrentMonitor();
    int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
    int ww, wh;
    double zoom;
    if (grab_path) {
        /* Readback wants the window at the video's own size, decided here
         * rather than resized later: a resize is asynchronous, and grabbing
         * before it settles captures the old framebuffer. */
        zoom = 1.0; ww = vw; wh = vh;
    } else if (scale > 0.0) {
        zoom = scale;
        ww = (int)(vw * scale + 0.5);
        wh = (int)(vh * scale + 0.5);
    } else {
        zoom = fit_window(vw, vh, mw, mh, &ww, &wh);
    }
    SetWindowSize(ww, wh);
    SetWindowMinSize(160, 120);

    /* Centre on the monitor the window landed on. GetMonitorPosition matters
     * on a multi-display desktop, where a monitor's origin is not (0,0). */
    Vector2 mpos = GetMonitorPosition(mon);
    int wx = (int)mpos.x + (mw - ww) / 2;
    int wy = (int)mpos.y + (mh - wh) / 2;
    if (wy < (int)mpos.y + 40) wy = (int)mpos.y + 40;   /* clear the menu bar */
    SetWindowPosition(wx, wy);

    /* Report where the window actually landed, not where it was asked to go:
     * the window manager gets the last word. */
    Vector2 got = GetWindowPosition();
    printf("  display %dx%d, window %dx%d (%.3gx) at %d,%d\n",
           mw, mh, ww, wh, zoom, (int)got.x, (int)got.y);
    fflush(stdout);
    {
        char title[256];
        const char *base = strrchr(path, '/');
        snprintf(title, sizeof title, "fpl — %s (%dx%d)",
                 base ? base + 1 : path, vw, vh);
        SetWindowTitle(title);
    }

    Image img = { .data = rgb, .width = vw, .height = vh,
                  .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8 };
    Texture2D tex = LoadTextureFromImage(img);
    /* Nearest at whole-number zoom keeps the coded pixels visible; bilinear
     * when shrinking, where smoothing is what you want. */
    SetTextureFilter(tex, zoom >= 1.0 ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);

    /* Read one frame back off the GPU after drawing it. --dump exercises the
     * colour conversion only; this exercises the whole path -- texture format,
     * upload, and the draw -- which is the part a headless check would
     * otherwise have to take on trust. */
    if (grab_path) {
        if (grab_frame < 0 || grab_frame >= nf) grab_frame = 0;
        SetTextureFilter(tex, TEXTURE_FILTER_POINT);   /* no resampling */
        uint8_t *gf = frame_at(dec, cache, cache_idx, ncache, fsize, grab_frame);
        if (!gf) { fprintf(stderr, "cannot decode frame %d\n", grab_frame); return 1; }
        yuv420_to_rgb(gf, vw, vh, rgb, range);
        UpdateTexture(tex, rgb);
        for (int warm = 0; warm < 8; ++warm) {         /* let the window settle */
            BeginDrawing();
            ClearBackground(BLACK);
            DrawTexturePro(tex, (Rectangle){0, 0, (float)vw, (float)vh},
                           (Rectangle){0, 0, (float)vw, (float)vh},
                           (Vector2){0, 0}, 0.0f, WHITE);
            EndDrawing();
        }
        Image shot = LoadImageFromScreen();
        ImageFormat(&shot, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        int rc = dump_ppm(grab_path, (const uint8_t *)shot.data, shot.width, shot.height);
        printf("grabbed frame %d from the screen: %dx%d -> %s\n",
               grab_frame, shot.width, shot.height, grab_path);
        UnloadImage(shot);
        UnloadTexture(tex);
        CloseWindow();
        fdv_dec_close(dec); free(file); free(cache); free(cache_idx); free(rgb);
        return rc == 0 ? 0 : 1;
    }

    /* ---- playback ---- */
    int frame = 0, shown = -1, playing = 1, loop = 1, hud = 1;
    double speed = 1.0, acc = 0.0;
    double t_start = wall_ms();
    while (!WindowShouldClose()) {
        /* input */
        if (IsKeyPressed(KEY_SPACE)) playing = !playing;
        if (IsKeyPressed(KEY_L)) loop = !loop;
        if (IsKeyPressed(KEY_H)) hud = !hud;
        if (IsKeyPressed(KEY_F)) ToggleFullscreen();
        if (IsKeyPressed(KEY_R)) range = range == RANGE_FULL ? RANGE_TV : RANGE_FULL,
                                 shown = -1;
        if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_HOME)) { frame = 0; acc = 0; }
        if (IsKeyPressed(KEY_PERIOD)) { frame = frame + 1 < nf ? frame + 1 : frame; playing = 0; }
        if (IsKeyPressed(KEY_COMMA))  { frame = frame > 0 ? frame - 1 : 0;          playing = 0; }
        if (IsKeyPressed(KEY_RIGHT))  { frame += m.info.fps; if (frame >= nf) frame = nf - 1; }
        if (IsKeyPressed(KEY_LEFT))   { frame -= m.info.fps; if (frame < 0) frame = 0; }
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))      speed *= 2.0;
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) speed *= 0.5;
        if (speed > 8.0) speed = 8.0;
        if (speed < 0.125) speed = 0.125;

        /* Scrub by clicking the progress bar. */
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 mp = GetMousePosition();
            if (mp.y >= GetScreenHeight() - 14) {
                double t = mp.x / (double)GetScreenWidth();
                frame = (int)(t * nf);
                if (frame < 0) frame = 0;
                if (frame >= nf) frame = nf - 1;
                acc = 0;
            }
        }

        /* advance */
        if (playing) {
            acc += GetFrameTime() * speed;
            double step = 1.0 / m.info.fps;
            while (acc >= step) {
                acc -= step;
                if (++frame >= nf) {
                    if (loop) frame = 0;
                    else { frame = nf - 1; playing = 0; acc = 0; break; }
                }
            }
        }

        if (frame != shown) {
            uint8_t *fr = frame_at(dec, cache, cache_idx, ncache, fsize, frame);
            if (!fr) { fprintf(stderr, "decode failed at frame %d\n", frame); break; }
            yuv420_to_rgb(fr, vw, vh, rgb, range);
            UpdateTexture(tex, rgb);
            if (log_frames)
                fprintf(stderr, "t=%.4f frame=%d\n", (wall_ms() - t_start) / 1000.0, frame);
            shown = frame;
        }

        BeginDrawing();
        ClearBackground((Color){18, 18, 20, 255});
        Rectangle dst = letterbox(vw, vh, GetScreenWidth(), GetScreenHeight());
        DrawTexturePro(tex, (Rectangle){0, 0, (float)vw, (float)vh}, dst,
                       (Vector2){0, 0}, 0.0f, WHITE);
        if (hud) {
            draw_hud(&m, frame, playing, loop, speed, (double)GetFPS());
            draw_help();
        }
        EndDrawing();
    }

    UnloadTexture(tex);
    CloseWindow();
    fdv_dec_close(dec); free(file); free(cache); free(cache_idx); free(rgb);
    return 0;
}
