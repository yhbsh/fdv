/* fdv_scene.h -- text scene description -> synthetic I420 video.
 *
 * Single header, in the same style as fdv.h: include it anywhere for the
 * declarations, and in exactly one translation unit define
 *
 *     #define FDV_SCENE_IMPLEMENTATION
 *
 * before including it, to get the code.
 *
 * This lives apart from the codec on purpose. fdv.h is an encoder, a decoder
 * and the instrumentation for both; it opens no files and knows nothing about
 * where pixels come from. Scene rendering is a *test fixture* -- it invents
 * content, parses text and writes Y4M -- and none of that belongs in a library
 * whose job is to turn frames into bits. Keeping the two apart also means the
 * codec can be built for somewhere with no filesystem at all.
 *
 * The renderer is deterministic: the same description gives the same bytes on
 * any machine, which is what makes it usable as a regression fixture as well as
 * a source of test footage.
 *
 * Switches:
 *   FDV_SCENE_IMPLEMENTATION   emit the code (exactly one TU)
 *   FDV_SCENE_DIR              directory searched for named .scn files
 */

#ifndef FDV_SCENE_H
#define FDV_SCENE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ===========================================================================
 * 16. SCENE
 * Text scene description -> synthetic I420 video.
 * ======================================================================== */

/* Scene description -> synthetic I420 video.
 *
 * The codec needs content to chew on, and real .yuv clips are awkward to come
 * by.  This turns a short text description of *what should be on screen* into a
 * deterministic YUV 4:2:0 sequence, so a test clip is a six-line file rather
 * than a download.
 *
 * It is also the codec's exercise machine: the built-in presets are chosen to
 * hit specific paths in the encoder — static content for SKIP, uniform pans for
 * the motion predictor, opposed motion inside one macroblock for the 8x8 split,
 * hard cuts for the intra fallback, noise for the entropy coder.
 *
 * Rendering is a pure function of (scene, frame index): the same scene always
 * yields the same bytes, on any machine, so PSNR and bitrate numbers are
 * comparable across runs.
 *
 * ---------------------------------------------------------------- language --
 *
 *   # comments run to end of line
 *   size 320 192          # luma dimensions, must be multiples of 16
 *   frames 60
 *   fps 30
 *   qp 22                 # suggested QP for the pipeline (optional)
 *   keyint 0              # suggested key-frame interval (optional)
 *   seed 12345            # PRNG seed for noise and cut jumps
 *
 *   bg luma=32 cb=128 cr=128
 *
 * Then any number of draw directives, painted in declaration order:
 *
 *   grad    dir=v from=24 to=90
 *   rect    x=20 y=24 w=64 h=48 luma=200 cb=90 cr=170 vx=2 vy=1 bounce=1
 *   circle  x=160 y=96 r=28 luma=120 cb=200 cr=60 vx=-1.5 vy=0.7 bounce=1
 *   checker x=0 y=0 w=96 h=64 cell=8 luma=235 luma2=16
 *   noise   sigma=3
 *   stamp   x=4 y=4 scale=3        # burn the frame number into the picture
 *   cut     frame=30               # hard scene change: everything jumps
 *
 * Rectangles and circles make good diagnostics and a poor landscape. Six more
 * directives build one. Each places itself in *world* coordinates and is moved
 * past a single camera by its own `depth`, so one pan gives every layer a
 * different velocity -- which is what a real camera move looks like to an
 * encoder, and what sliding rectangles can never produce.
 *
 *   camera  vx=2.6                 # one pan, for the whole scene
 *   ridge   y=410 amp=132 freq=2.0 oct=6 depth=0.14 fade=0.16 luma2=240
 *   clouds  y=0 h=300 freq=2.1 depth=0.03 speed=0.0005 thresh=142 alpha=118
 *   water   y=496 h=132 freq=3.2 amp=20 depth=0.60 speed=0.021 alpha=92
 *   scatter x=-640 y=606 w=2560 h=30 r=0 iw=3 ih=12 count=3000 depth=0.86
 *   grain   freq=300 amp=8 oct=2 depth=0.92
 *
 * And three that are actually three-dimensional -- a heightfield marched one
 * ray per screen column, and billboards standing on it:
 *
 *   camera3 x=0 y=0 z=-260 yaw=0.15 pitch=-0.05 fov=1.05 vz=2.4 agl=125
 *   terrain freq=0.0021 amp=300 oct=9 far=2100 y=34 thresh=200 luma2=124
 *   props   density=118 cell=11 r=2.7 ih=8 iw=0.5 luma=58 luma2=44
 *
 * `agl` holds the camera that far above the ground, so a flight follows the
 * landscape. Props stand on whichever terrain was declared above them. These
 * are in world units and `fdv_scene_resize` leaves them alone, because a
 * perspective projection is already resolution-independent.
 *
 * depth 0 pins a layer to the camera, 1 moves it a pixel per pixel of pan, and
 * more than 1 outruns it. `freq` is in cycles across the frame width and
 * `depth` is a ratio, so both survive a resize; `amp` and instance sizes are
 * pixel measurements and scale with the picture. `seed=` pins a layer's noise
 * so two layers can share one shape -- a trunk under its own canopy.
 *
 * Attributes are `key=value`, order-free, and every one has a default, so
 * `rect x=8 y=8 w=32 h=32` is a complete directive.  Positions and sizes are in
 * luma pixels; chroma is derived by halving.  `vx`/`vy` are pixels per frame.
 */

typedef struct scene fdv_scene;

/* Parse a scene.  On failure returns NULL and writes a one-line reason
 * (including the line number) into err, if err is non-NULL. */
fdv_scene *fdv_scene_parse_text(const char *text, char *err, size_t errcap);
fdv_scene *fdv_scene_parse_file(const char *path, char *err, size_t errcap);

/* Resolve `name` to a scene file and parse it. This is what the CLI takes.
 *
 * There are no built-in scenes: every scene is a file, so anything that names
 * one names something you can open and edit. A bare name like "motion" is
 * resolved as a path, then as "motion.scn", then inside the scene directory --
 * see fdv_scene_find. */
fdv_scene *fdv_scene_open(const char *name, char *err, size_t errcap);

/* Where bare names are looked up: $FDV_SCENES, or "scenes" if unset. */
const char *fdv_scene_dir(void);

/* Resolve a name to a readable path without parsing it. Returns 0 and fills
 * `out`, or -1 if nothing matched. */
int fdv_scene_find(const char *name, char *out, size_t outcap);

/* Read a scene file's description: its first comment line. Keeping the blurb
 * in the file means it travels with the scene. Returns 0 on success. */
int fdv_scene_file_blurb(const char *path, char *out, size_t outcap);

void fdv_scene_free(fdv_scene *s);

/* Re-target a scene at a different frame size.
 *
 * Not just a new `size` line: every object's position, extent and velocity is
 * scaled with the frame, so a fdv_preset written at 320x192 becomes the same
 * picture at 1920x1088 rather than a small picture marooned in a large frame.
 * Colours, gradient endpoints and noise amplitude are intensity values and stay
 * as they are. Returns 0, or -1 if the dimensions are not positive multiples
 * of 16. */
int fdv_scene_resize(fdv_scene *s, int w, int h);

/* Parse a resolution argument: "1920x1088", or one of the shorthands
 * "360p" / "480p" / "720p" / "1080p" / "1440p" / "4k". Returns 0 and sets
 * w/h, or -1. Heights are rounded up to a multiple of 16 the way real encoders
 * do (1080 is not codable, so 1080p means 1920x1088). */
int fdv_scene_parse_size(const char *arg, int *w, int *h);

int fdv_scene_width(const fdv_scene *s);
int fdv_scene_height(const fdv_scene *s);
int fdv_scene_frames(const fdv_scene *s);
int fdv_scene_fps(const fdv_scene *s);
int fdv_scene_qp(const fdv_scene *s);       /* suggested QP, or -1 if the scene set none */
int fdv_scene_keyint(const fdv_scene *s);   /* suggested keyint, or -1 */
int fdv_scene_objects(const fdv_scene *s);
const char *fdv_scene_name(const fdv_scene *s);

/* Bytes in one rendered frame: w*h*3/2. */
size_t fdv_scene_frame_size(const fdv_scene *s);

/* Render frame `f` (0-based; values past the end keep extrapolating) into
 * `i420`, which must hold fdv_scene_frame_size() bytes. */
void fdv_scene_render(const fdv_scene *s, int f, uint8_t *i420);

/* Render the whole clip into `dst` (frames * fdv_scene_frame_size() bytes). */
void fdv_scene_render_all(const fdv_scene *s, uint8_t *dst);

/* One-line-per-fact dump of what was parsed, for logs. */
void fdv_scene_describe(const fdv_scene *s, FILE *out, const char *tag);

/* Cheap per-frame content statistics, for logging what was actually rendered.
 * `prev` may be NULL for the first frame, in which case mad is 0. */
typedef struct {
    double mean;    /* mean luma */
    double sd;      /* luma standard deviation (texture proxy)   */
    double mad;     /* mean |luma - prev luma| (motion proxy)    */
} fdv_scene_info;

fdv_scene_info fdv_scene_measure(const uint8_t *i420, const uint8_t *prev,
                               int w, int h);

/* --------------------------------------------------------------- file I/O --
 * Y4M is the useful interchange form here: ffplay/mpv open it directly, so a
 * rendered or decoded clip can be eyeballed without guessing dimensions. */

int fdv_y4m_write(const char *path, const uint8_t *seq, int w, int h,
              int nframes, int fps);
int fdv_yuv_write(const char *path, const uint8_t *seq, size_t len);


#ifdef FDV_SCENE_IMPLEMENTATION

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * 16. SCENE
 * Text scene description -> synthetic I420 video.
 * ======================================================================== */

/* Scene description -> synthetic I420 video.  See scene.h for the language.
 *
 * Layout mirrors the codec's convention: sections in dependency order, nothing
 * in a section depending on one below it.
 *
 *   1. PRNG      deterministic noise and cut jumps
 *   2. RASTER    plane-level drawing primitives (subsample-aware)
 *   3. FONT      3x5 digits, for burning frame numbers into the picture
 *   4. MODEL     the scene struct and its accessors
 *   5. PARSE     the text format
 *   6. RENDER    scene + frame index -> pixels
 *   7. RESOLVE   turning a name into a scene file
 *   8. FILEIO    raw I420 and Y4M output
 */



/* ===========================================================================
 * 1. PRNG
 * Deterministic noise and cut jumps.
 * ======================================================================== */

/* A scene must render identically on every machine and every run, so nothing
 * here touches rand(); everything derives from an explicit seed.  This is a
 * 32-bit mix (splitmix-style finalizer) used as a hash: value depends only on
 * its inputs, so a pixel's noise can be computed independently of its
 * neighbours and a frame can be rendered without rendering the ones before it.
 */
static uint32_t mix32(uint32_t x) {
    x += 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x85ebca6bu;
    x = (x ^ (x >> 13)) * 0xc2b2ae35u;
    return x ^ (x >> 16);
}

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    return mix32(mix32(mix32(a) ^ b) ^ c);
}

/* Roughly-Gaussian signed value scaled by `sigma`, from summing four uniforms
 * (central-limit; plenty for a texture generator). */
static int noise_at(uint32_t seed, uint32_t frame, uint32_t idx, int sigma) {
    if (sigma <= 0) return 0;
    uint32_t h = hash3(seed, frame, idx);
    int acc = (int)((h >> 0) & 0xff) + (int)((h >> 8) & 0xff)
            + (int)((h >> 16) & 0xff) + (int)((h >> 24) & 0xff);
    /* acc is in [0,1020], mean 510, sd ~147.  Normalize to unit sd. */
    return (int)lround((acc - 510.0) * (sigma / 147.0));
}

/* --- Coherent noise ------------------------------------------------------
 *
 * `noise_at` above is white: every pixel independent. That is the right thing
 * for film grain and the wrong thing for a landscape, where what makes a
 * mountain look like a mountain is that neighbouring points are *correlated*.
 *
 * Value noise gives that: hash the integer lattice, smoothly interpolate
 * between corners. Summing octaves at doubling frequency (fBm) produces the
 * self-similar detail real terrain has, and folding each octave about its
 * midpoint (`ridge2`) turns rounded hills into the creased ridgelines of a
 * mountain range.
 *
 * Still a pure function of its arguments -- no tables, no state -- so a frame
 * can be rendered on its own and two runs agree bit for bit. */
static double lat2(uint32_t seed, int xi, int yi) {
    return (double)(hash3(seed, (uint32_t)xi, (uint32_t)yi) >> 8) * (1.0 / 16777216.0);
}

static double vnoise2(uint32_t seed, double x, double y) {
    double fx0 = floor(x), fy0 = floor(y);
    int xi = (int)fx0, yi = (int)fy0;
    double fx = x - fx0, fy = y - fy0;
    double u = fx * fx * (3.0 - 2.0 * fx);        /* smoothstep: C1 continuous */
    double v = fy * fy * (3.0 - 2.0 * fy);
    double a = lat2(seed, xi,     yi);
    double b = lat2(seed, xi + 1, yi);
    double c = lat2(seed, xi,     yi + 1);
    double d = lat2(seed, xi + 1, yi + 1);
    double top = a + (b - a) * u, bot = c + (d - c) * u;
    return top + (bot - top) * v;
}

/* Fractional Brownian motion: octaves at doubling frequency, halving weight. */
static double fbm2(uint32_t seed, double x, double y, int oct) {
    if (oct < 1) oct = 1;
    if (oct > 8) oct = 8;
    double sum = 0.0, amp = 1.0, norm = 0.0;
    for (int i = 0; i < oct; ++i) {
        sum  += amp * vnoise2(seed + (uint32_t)i * 7919u, x, y);
        norm += amp;
        amp  *= 0.5;
        x *= 2.0; y *= 2.0;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

/* The same, folded about the midpoint so each octave peaks in a crease. */
static double ridge2(uint32_t seed, double x, double y, int oct) {
    if (oct < 1) oct = 1;
    if (oct > 8) oct = 8;
    double sum = 0.0, amp = 1.0, norm = 0.0;
    for (int i = 0; i < oct; ++i) {
        double n = vnoise2(seed + (uint32_t)i * 26699u, x, y);
        n = 1.0 - fabs(2.0 * n - 1.0);
        sum  += amp * n * n;                      /* squared: sharper crests */
        norm += amp;
        amp  *= 0.5;
        x *= 2.0; y *= 2.0;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

/* Hashing a world coordinate.
 *
 * The double goes through a signed integer first because converting a negative
 * double straight to an unsigned type is undefined behaviour, and world
 * coordinates are negative half the time -- the camera starts at the origin and
 * the terrain extends both ways. Signed-to-unsigned conversion afterwards is
 * well defined and wraps, which is all a hash needs. UBSan found this; the
 * output happened to be identical either way, which is exactly why it needed a
 * sanitizer rather than an eye. */
static uint32_t wkey(double v) { return (uint32_t)(int32_t)floor(v); }

static double sclamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* A perspective camera, for the layers that are actually three-dimensional.
 *
 * Kept separate from the 2D pan rather than folded into it: `camera vx=` means
 * pixels of screen shift per frame and `camera3 vz=` means world units of
 * travel, and quietly making one word mean both would be a trap. A scene can
 * use either or both -- 2D layers read the 3D camera's yaw as a pan, which is
 * what a distant sky should do when you turn. */
typedef struct {
    double x, y, z;        /* position, world units */
    double yaw, pitch;     /* radians */
    double fov;            /* horizontal field of view, radians */
} fdv_cam3;

/* ===========================================================================
 * 2. RASTER
 * Plane-level drawing primitives.
 * ======================================================================== */

/* Every primitive takes a plane plus a subsample shift, so one call site draws
 * the same shape into full-resolution luma (sh=0) and half-resolution chroma
 * (sh=1) without a second code path.  Coordinates arrive in luma pixels. */
typedef struct {
    uint8_t *p;
    int      w, h;   /* this plane's own dimensions */
    int      sh;     /* subsample shift: 0 for luma, 1 for 4:2:0 chroma */
} fdv_splane;

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int scn_min(int a, int b) { return a < b ? a : b; }
static int scn_max(int a, int b) { return a > b ? a : b; }

static void ras_fill(fdv_splane *s, int value) {
    memset(s->p, clamp255(value), (size_t)s->w * s->h);
}

static void ras_rect(fdv_splane *s, double lx, double ly, double lw, double lh, int value) {
    int sh = s->sh;
    int x0 = scn_max(0, (int)floor(lx) >> sh);
    int y0 = scn_max(0, (int)floor(ly) >> sh);
    int x1 = scn_min(s->w, (int)ceil(lx + lw) >> sh);
    int y1 = scn_min(s->h, (int)ceil(ly + lh) >> sh);
    uint8_t v = (uint8_t)clamp255(value);
    for (int y = y0; y < y1; ++y)
        memset(s->p + (size_t)y * s->w + x0, v, (size_t)scn_max(0, x1 - x0));
}

static void ras_circle(fdv_splane *s, double lcx, double lcy, double lr, int value) {
    int sh = s->sh;
    double cx = lcx / (1 << sh), cy = lcy / (1 << sh), r = lr / (1 << sh);
    int x0 = scn_max(0, (int)floor(cx - r)), x1 = scn_min(s->w, (int)ceil(cx + r) + 1);
    int y0 = scn_max(0, (int)floor(cy - r)), y1 = scn_min(s->h, (int)ceil(cy + r) + 1);
    uint8_t v = (uint8_t)clamp255(value);
    double r2 = r * r;
    for (int y = y0; y < y1; ++y) {
        double dy = y + 0.5 - cy;
        for (int x = x0; x < x1; ++x) {
            double dx = x + 0.5 - cx;
            if (dx * dx + dy * dy <= r2) s->p[(size_t)y * s->w + x] = v;
        }
    }
}

/* Linear ramp across the whole plane; dir 0 = horizontal, 1 = vertical. */
/* Composite one sample. alpha 255 replaces, 0 leaves the pixel alone; anything
 * between mixes, which is what puts colours in a frame that no object actually
 * contains -- the case a codec has to face on real footage and never sees in a
 * scene made of flat fills. */
static void px_blend(uint8_t *p, int v, int alpha) {
    if (alpha >= 255) { *p = (uint8_t)clamp255(v); return; }
    if (alpha <= 0) return;
    *p = (uint8_t)clamp255((*p * (255 - alpha) + clamp255(v) * alpha) / 255);
}

/* A disc that can be composited. Rectangles have had alpha since translucency
 * went in; circles not having it was an oversight, and a soft round glow is
 * exactly what a low sun over a horizon needs. */
static void ras_circle_a(fdv_splane *s, double lcx, double lcy, double lr,
                         int value, int alpha) {
    int sh = s->sh;
    double cx = lcx / (1 << sh), cy = lcy / (1 << sh), r = lr / (1 << sh);
    int x0 = scn_max(0, (int)floor(cx - r)), x1 = scn_min(s->w, (int)ceil(cx + r) + 1);
    int y0 = scn_max(0, (int)floor(cy - r)), y1 = scn_min(s->h, (int)ceil(cy + r) + 1);
    double r2 = r * r;
    for (int y = y0; y < y1; ++y) {
        double dy = y + 0.5 - cy;
        for (int x = x0; x < x1; ++x) {
            double dx = x + 0.5 - cx;
            double d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            /* Feather the edge by the remaining radius, so a glow falls off
             * instead of ending in a hard circle. */
            int a = alpha;
            if (alpha < 255) {
                double t = 1.0 - sqrt(d2 / (r2 > 0.0 ? r2 : 1.0));
                a = (int)lround(alpha * t * t);
            }
            px_blend(&s->p[(size_t)y * s->w + x], value, a);
        }
    }
}

/* A rectangle that can be rotated, blended and textured.
 *
 * Rotation matters because every motion vector in this codec is a translation:
 * a spinning edge cannot be predicted by shifting the previous frame, so the
 * encoder has to fall back on coding residual or going intra, which is exactly
 * the path that stays untested when everything slides in straight lines. */
static void ras_rect_rot(fdv_splane *s, double lcx, double lcy, double lw, double lh,
                         double ang, int value, int alpha, int tex,
                         uint32_t seed, uint32_t frame) {
    int sh = s->sh;
    double ca = cos(-ang), sa = sin(-ang);
    double diag = 0.5 * sqrt(lw * lw + lh * lh) + 2.0;
    int x0 = scn_max(0, (int)floor(lcx - diag) >> sh);
    int y0 = scn_max(0, (int)floor(lcy - diag) >> sh);
    int x1 = scn_min(s->w, ((int)ceil(lcx + diag) >> sh) + 1);
    int y1 = scn_min(s->h, ((int)ceil(lcy + diag) >> sh) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            double px = ((double)(x << sh) + (sh ? 0.5 : 0.0)) - lcx;
            double py = ((double)(y << sh) + (sh ? 0.5 : 0.0)) - lcy;
            double rx = px * ca - py * sa, ry = px * sa + py * ca;
            if (fabs(rx) > lw * 0.5 || fabs(ry) > lh * 0.5) continue;
            int v = value;
            if (tex) {
                uint32_t k = seed ^ (frame * 2654435761u)
                           ^ ((uint32_t)x * 374761393u) ^ ((uint32_t)y * 668265263u);
                k ^= k >> 13; k *= 1274126177u; k ^= k >> 16;
                v += (int)(k % (uint32_t)(2 * tex + 1)) - tex;
            }
            px_blend(&s->p[(size_t)y * s->w + x], v, alpha);
        }
}

/* A gradient across the plane: linear in x or y, or radial from the centre.
 * `from`/`to` are this plane's own endpoints, so chroma can ramp independently
 * of luma -- a frame whose colour changes across it, rather than one flat
 * colour with a brightness ramp. */
static void ras_grad2(fdv_splane *s, int dir, int from, int to) {
    for (int y = 0; y < s->h; ++y)
        for (int x = 0; x < s->w; ++x) {
            int v;
            if (dir == 2) {                        /* radial */
                double dx = (x + 0.5) / s->w - 0.5, dy = (y + 0.5) / s->h - 0.5;
                double t = sqrt(dx * dx + dy * dy) * 2.0;
                if (t > 1.0) t = 1.0;
                v = from + (int)lround((to - from) * t);
            } else {
                /* Integer, truncating -- exactly what this did before radial and
                 * chroma endpoints were added, so every scene written against
                 * the old renderer still produces the identical bytes and old
                 * measurements stay comparable. */
                int n = dir ? s->h : s->w;
                int i = dir ? y : x;
                v = from + (n > 1 ? (to - from) * i / (n - 1) : 0);
            }
            s->p[(size_t)y * s->w + x] = (uint8_t)clamp255(v);
        }
}


static void ras_checker(fdv_splane *s, double lx, double ly, double lw, double lh,
                        int cell, int a, int b) {
    int sh = s->sh;
    int c = scn_max(1, cell >> sh);
    int x0 = scn_max(0, (int)floor(lx) >> sh), y0 = scn_max(0, (int)floor(ly) >> sh);
    int x1 = scn_min(s->w, (int)ceil(lx + lw) >> sh);
    int y1 = scn_min(s->h, (int)ceil(ly + lh) >> sh);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            int on = (((x - x0) / c) + ((y - y0) / c)) & 1;
            s->p[(size_t)y * s->w + x] = (uint8_t)clamp255(on ? a : b);
        }
}

static void ras_noise(fdv_splane *s, uint32_t seed, uint32_t frame, int sigma) {
    for (int y = 0; y < s->h; ++y)
        for (int x = 0; x < s->w; ++x) {
            size_t i = (size_t)y * s->w + x;
            s->p[i] = (uint8_t)clamp255(s->p[i] + noise_at(seed, frame, (uint32_t)i, sigma));
        }
}


/* --- World layers ---------------------------------------------------------
 *
 * These five draw a landscape rather than shapes: a horizon, weather, water,
 * scattered vegetation and surface grain. Each is placed in *world* coordinates
 * and shifted past the camera by its own `depth`, so a single pan produces a
 * different velocity in every layer at once -- a motion field with a dozen
 * distinct translations in one frame, which is what a real camera move looks
 * like to an encoder and what a scene of sliding rectangles never produces.
 *
 * All of them are functions of world position and frame number only, so the
 * renderer stays a pure function and frames can be produced out of order. */

/* A ridgeline. Per column, fold fBm into a crest; fill below it with a ramp
 * from `far` at the crest to `near` at the bottom, which is what aerial
 * perspective does to a mountainside. */
static void ras_ridge(fdv_splane *s, uint32_t seed, double camx,
                      double basey, double amp, double freq, int oct,
                      int near_v, int far_v, double fade, int tex,
                      int fullw, int fullh) {
    int sh = s->sh;
    for (int x = 0; x < s->w; ++x) {
        double lx = (double)(x << sh);
        double u  = (lx + camx) / (double)fullw * freq;
        double hy = basey - amp * ridge2(seed, u, 0.37, oct);
        int y0 = (int)floor(hy) >> sh;
        if (y0 < 0) y0 = 0;
        /* The ramp runs over the range's own relief, not down to the bottom of
         * the frame. Tied to the frame, a distant range -- which is a thin
         * strip near its own crest -- showed only its crest colour and read as
         * a flat ribbon of sky rather than as mountains. */
        double span = amp * 2.0;
        if (span < 1.0) span = 1.0;
        (void)fullh;
        for (int y = y0; y < s->h; ++y) {
            double ly = (double)(y << sh);
            double t = sclamp((ly - hy) / span, 0.0, 1.0);
            if (fade > 0.0 && fade < 1.0) t = sclamp(t / fade, 0.0, 1.0);
            int v = far_v + (int)lround((near_v - far_v) * t);
            if (tex) {
                /* Hashed in world space, so the rock face translates with the
                 * layer instead of crawling underneath it. */
                uint32_t k = hash3(seed ^ 0x5bd1u, wkey((lx + camx) * 2.0),
                                   wkey(ly * 2.0));
                v += (int)(k % (uint32_t)(2 * tex + 1)) - tex;
            }
            s->p[(size_t)y * s->w + x] = (uint8_t)clamp255(v);
        }
    }
}

/* Cloud or mist: fBm thresholded into coverage, feathered by how far past the
 * threshold it got, drifting on its own on top of the camera's parallax. */
static void ras_clouds(fdv_splane *s, uint32_t seed, double camx, double drift,
                       double y0, double hgt, double freq, int oct,
                       int thresh, int maxa, int value, int fullw) {
    int sh = s->sh;
    int ys = scn_max(0, (int)floor(y0) >> sh);
    int ye = scn_min(s->h, ((int)ceil(y0 + hgt) >> sh) + 1);
    double t0 = thresh / 255.0;
    if (t0 >= 0.999) return;
    for (int y = ys; y < ye; ++y) {
        double ly = (double)(y << sh);
        double vv = ly / (double)fullw * freq;
        for (int x = 0; x < s->w; ++x) {
            double lx = (double)(x << sh);
            double d = fbm2(seed, (lx + camx + drift) / (double)fullw * freq, vv, oct);
            if (d <= t0) continue;
            int a = (int)lround((d - t0) / (1.0 - t0) * maxa);
            px_blend(&s->p[(size_t)y * s->w + x], value, a);
        }
    }
}

/* Water: mirror what is already on the plane about the waterline, displace the
 * lookup by a moving fBm, and darken.
 *
 * Reflection is the interesting part for a codec. The surface translates with
 * the camera, but what it reflects moves the *opposite* way vertically and is
 * warped by ripples that are not a translation at all, so no single motion
 * vector predicts a water block. It reads only rows above the waterline, which
 * are drawn and never written here. */
static void ras_water(fdv_splane *s, uint32_t seed, double camx, double phase,
                      double y0, double hgt, double freq, double amp, int oct,
                      int tint, int tinta, int lumap, int fullw) {
    int sh = s->sh;
    int ys = scn_max(0, (int)floor(y0) >> sh);
    int ye = scn_min(s->h, ((int)ceil(y0 + hgt) >> sh) + 1);
    int wl = (int)floor(y0) >> sh;
    double hh = hgt > 1.0 ? hgt : 1.0;
    for (int y = ys; y < ye; ++y) {
        double ly = (double)(y << sh);
        double near = sclamp((ly - y0) / hh, 0.0, 1.0);
        for (int x = 0; x < s->w; ++x) {
            double lx = (double)(x << sh);
            double n = fbm2(seed, (lx + camx) / (double)fullw * freq,
                            ly / (double)fullw * freq * 3.0 + phase, oct) - 0.5;
            int src = wl - (y - wl) + (int)lround(n * amp * (0.35 + near));
            if (src < 0) src = 0;
            if (src >= s->h) src = s->h - 1;
            int val = s->p[(size_t)src * s->w + x];
            if (lumap) val = (int)(val * 0.80) + 10;   /* a reflection is dimmer */
            uint8_t *pp = &s->p[(size_t)y * s->w + x];
            *pp = (uint8_t)clamp255(val);
            if (tinta) px_blend(pp, tint, tinta);
        }
    }
}

/* Scattered instances -- trees, rocks, grass. Placed by hash across a world
 * band wider than the frame and wrapped, so the camera scrolls new ones in
 * forever without the scene declaring them one by one. */
static void ras_scatter(fdv_splane *s, uint32_t seed, double camx,
                        double x0, double y0, double bw, double bh,
                        double sw, double shh, double rr, int count,
                        double jitter, int value, int alpha) {
    int sh = s->sh;
    if (bw < 1.0) bw = 1.0;
    for (int i = 0; i < count; ++i) {
        double hx = (double)(hash3(seed, (uint32_t)i, 1u) >> 8) * (1.0 / 16777216.0);
        double hy = (double)(hash3(seed, (uint32_t)i, 2u) >> 8) * (1.0 / 16777216.0);
        double hs = (double)(hash3(seed, (uint32_t)i, 3u) >> 8) * (1.0 / 16777216.0);
        double hc = (double)(hash3(seed, (uint32_t)i, 4u) >> 8) * (1.0 / 16777216.0);
        double sx = fmod(x0 + hx * bw - camx - x0, bw);
        if (sx < 0.0) sx += bw;
        sx += x0;
        double sy = y0 + hy * bh;
        double k  = 1.0 + jitter * (2.0 * hs - 1.0);
        int v = value + (int)lround((2.0 * hc - 1.0) * jitter * (sh ? 12.0 : 40.0));
        if (rr > 0.0) {
            double r = rr * k / (1 << sh);
            double cx = sx / (1 << sh), cy = sy / (1 << sh);
            int ax0 = scn_max(0, (int)floor(cx - r)), ax1 = scn_min(s->w, (int)ceil(cx + r) + 1);
            int ay0 = scn_max(0, (int)floor(cy - r)), ay1 = scn_min(s->h, (int)ceil(cy + r) + 1);
            double r2 = r * r;
            for (int y = ay0; y < ay1; ++y) {
                double dy = y + 0.5 - cy;
                for (int x = ax0; x < ax1; ++x) {
                    double dx = x + 0.5 - cx;
                    if (dx * dx + dy * dy <= r2)
                        px_blend(&s->p[(size_t)y * s->w + x], v, alpha);
                }
            }
        } else {
            double ww = sw * k, hgt2 = shh * k;
            int ax0 = scn_max(0, (int)floor(sx) >> sh);
            int ay0 = scn_max(0, (int)floor(sy) >> sh);
            int ax1 = scn_min(s->w, ((int)ceil(sx + ww) >> sh) + 1);
            int ay1 = scn_min(s->h, ((int)ceil(sy + hgt2) >> sh) + 1);
            for (int y = ay0; y < ay1; ++y)
                for (int x = ax0; x < ax1; ++x)
                    px_blend(&s->p[(size_t)y * s->w + x], v, alpha);
        }
    }
}

/* Correlated surface grain, in world coordinates so it moves with the shot.
 *
 * `noise` is white and reseeded every frame: pure entropy, unpredictable by
 * construction. This is the opposite and the harder case -- fine detail that
 * *is* predictable, but only if motion compensation gets the vector exactly
 * right, because a quarter-pixel error decorrelates it completely. */
static void ras_grain(fdv_splane *s, uint32_t seed, double camx, double phase,
                      double freq, double amp, int oct, int fullw) {
    int sh = s->sh;
    if (amp <= 0.0) return;
    for (int y = 0; y < s->h; ++y) {
        double ly = (double)(y << sh);
        double vv = ly / (double)fullw * freq + phase;
        for (int x = 0; x < s->w; ++x) {
            double lx = (double)(x << sh);
            double n = fbm2(seed, (lx + camx) / (double)fullw * freq, vv, oct) - 0.5;
            uint8_t *pp = &s->p[(size_t)y * s->w + x];
            *pp = (uint8_t)clamp255(*pp + (int)lround(n * 2.0 * amp));
        }
    }
}


/* --- 3D: a heightfield through a perspective camera ------------------------
 *
 * Everything above fakes depth: layers slide at different rates and the eye
 * fills in the rest. This derives it. A ray per screen column is marched out
 * across a noise heightfield, front to back, painting each column down from a
 * watermark so nearer ground occludes farther ground for free and nothing is
 * drawn twice.
 *
 * The reason to have it in a codec's test set is the motion field. Sliding
 * layers give a handful of discrete velocities; a perspective camera gives a
 * velocity that varies *continuously* with depth, and one flying forward gives
 * vectors that diverge from a focus of expansion -- every block moving in a
 * different direction by a different amount, none of it a global translation.
 * That is the case block matching is worst at and the one real footage is full
 * of. */

typedef struct {
    uint32_t seed;
    double   freq, amp, fardist, water, ripple, snow;
    int      oct, tex;
    int      lo[3], hi[3], wat[3], haze[3];
} fdv_terr;

/* Ridged fBm, squared: sharp crests over flat valleys, which is roughly what
 * erosion leaves behind and reads as mountains rather than as dunes. */
static double terr_h(uint32_t seed, double wx, double wz,
                     double freq, double amp, int oct) {
    double r = ridge2(seed, wx * freq, wz * freq, oct);
    return amp * r * r;
}

/* Octaves worth sampling at distance t: past a point the detail is subpixel and
 * the samples are the entire cost of the march. */
static int terr_oct(int oct, double t) {
    int o = oct - (int)(log(1.0 + t / 24.0) * 1.4427);
    return o < 2 ? 2 : o;
}

/* Screen row of the surface at distance t along a ray, or a large number if the
 * ray is still in open sky there. Shared by the terrain march and the prop
 * occlusion test so the two cannot disagree about where a hill is. */
static double terr_row(const fdv_terr *T, const fdv_cam3 *c,
                       double dx, double dz, double t,
                       double horizon, double focal, double *hout, int *isw) {
    double wx = c->x + dx * t, wz = c->z + dz * t;
    double hgt = terr_h(T->seed, wx, wz, T->freq, T->amp, terr_oct(T->oct, t));
    int water = hgt < T->water;
    if (hout) *hout = hgt;
    if (isw)  *isw  = water;
    return horizon - ((water ? T->water : hgt) - c->y) * focal / t;
}

static void ras_terrain(fdv_splane *Y, fdv_splane *U, fdv_splane *V,
                        const fdv_cam3 *c, const fdv_terr *T) {
    int w = Y->w, h = Y->h;
    double focal   = (w * 0.5) / tan(c->fov * 0.5);
    double horizon = h * 0.5 + c->pitch * focal;
    double fx = sin(c->yaw), fz = cos(c->yaw);        /* forward */
    double rx = cos(c->yaw), rz = -sin(c->yaw);       /* right   */
    /* One fixed sun. This is a landscape, not a lighting rig. */
    const double sx_ = 0.55, sy_ = 0.66, sz_ = -0.51;
    static const int SNOW[3] = {238, 130, 130};

    for (int col = 0; col < w; ++col) {
        double a  = ((double)col - w * 0.5) / focal;
        double dx = fx + rx * a, dz = fz + rz * a;    /* unnormalized: t is depth */
        int ybuf = h;
        for (double t = 2.0; t < T->fardist && ybuf > 0; ) {
            /* Step proportional to depth, which keeps the sample spacing
             * roughly constant in screen space. Too coarse and the surface
             * terraces visibly, because each step paints a solid run of rows. */
            double step = t * 0.0055;
            if (step < 0.35) step = 0.35;
            int oct = terr_oct(T->oct, t);
            double wx = c->x + dx * t, wz = c->z + dz * t;
            double hgt = terr_h(T->seed, wx, wz, T->freq, T->amp, oct);
            int water  = hgt < T->water;
            double surf = water ? T->water : hgt;
            int sy = (int)(horizon - (surf - c->y) * focal / t);
            if (sy < ybuf) {
                int cv[3];
                if (water) {
                    /* No true reflection here -- a second ray per pixel is not
                     * worth it. The ripple modulates brightness instead, which
                     * still moves in a way no translation predicts. */
                    double rp = fbm2(T->seed ^ 0x9e37u, wx * 0.05,
                                     wz * 0.05 + T->ripple, 3) - 0.5;
                    cv[0] = T->wat[0] + (int)lround(rp * 46.0);
                    cv[1] = T->wat[1]; cv[2] = T->wat[2];
                } else {
                    /* Slope from two extra taps, lit by the sun. This is what
                     * makes the relief read as three-dimensional rather than as
                     * a height-coloured map. */
                    double e = step + 1.0;
                    double hxp = terr_h(T->seed, wx + e, wz, T->freq, T->amp, oct);
                    double hzp = terr_h(T->seed, wx, wz + e, T->freq, T->amp, oct);
                    double nx = (hgt - hxp) / e, nz = (hgt - hzp) / e;
                    double nl = sqrt(nx * nx + 1.0 + nz * nz);
                    double lam = (nx * sx_ + sy_ + nz * sz_) / nl;
                    double lit = 0.42 + 0.78 * (lam < 0.0 ? 0.0 : lam);

                    double hf = T->amp > 0.0 ? hgt / T->amp : 0.0;
                    double m  = sclamp((hf - 0.22) / 0.42, 0.0, 1.0);
                    for (int k = 0; k < 3; ++k)
                        cv[k] = T->lo[k] + (int)lround((T->hi[k] - T->lo[k]) * m);
                    if (hf > T->snow) {
                        double sm = sclamp((hf - T->snow) / 0.10, 0.0, 1.0);
                        for (int k = 0; k < 3; ++k)
                            cv[k] += (int)lround((SNOW[k] - cv[k]) * sm);
                    }
                    cv[0] = (int)lround(cv[0] * lit);
                    if (T->tex) {
                        uint32_t k2 = hash3(T->seed ^ 0x51edu, wkey(wx * 3.0),
                                            wkey(wz * 3.0));
                        cv[0] += (int)(k2 % (uint32_t)(2 * T->tex + 1)) - T->tex;
                    }
                }
                /* Aerial perspective. Without it the far ranges read as near
                 * ones drawn small, which is exactly how a flat scene looks. */
                double fog = 1.0 - exp(-t / (T->fardist * 0.80));
                for (int k = 0; k < 3; ++k)
                    cv[k] += (int)lround((T->haze[k] - cv[k]) * fog);

                int y0 = sy < 0 ? 0 : sy;
                for (int y = y0; y < ybuf; ++y)
                    Y->p[(size_t)y * Y->w + col] = (uint8_t)clamp255(cv[0]);
                int cc = col >> 1;
                if (cc < U->w)
                    for (int y = y0 >> 1; y < (ybuf + 1) >> 1 && y < U->h; ++y) {
                        U->p[(size_t)y * U->w + cc] = (uint8_t)clamp255(cv[1]);
                        V->p[(size_t)y * V->w + cc] = (uint8_t)clamp255(cv[2]);
                    }
                ybuf = y0;
            }
            t += step;
        }
    }
}

/* Things standing on the terrain: billboarded trees, placed on a world grid
 * around the camera so flying forward scrolls new ones in forever rather than
 * running out of a fixed list. */
typedef struct { double t, sx, sybase, sytop, r, fog; int col[3]; } fdv_prop;

static void ras_props(fdv_splane *Y, fdv_splane *U, fdv_splane *V,
                      const fdv_cam3 *c, const fdv_terr *T, uint32_t seed,
                      double cell, int density, double rad, double hgt,
                      double tw, double jitter, const int col[3],
                      const int tcol[3]) {
    int w = Y->w, h = Y->h;
    double focal   = (w * 0.5) / tan(c->fov * 0.5);
    double horizon = h * 0.5 + c->pitch * focal;
    double fx = sin(c->yaw), fz = cos(c->yaw);
    double rx = cos(c->yaw), rz = -sin(c->yaw);

    enum { MAXP = 3000 };
    fdv_prop *ps = malloc(MAXP * sizeof *ps);
    if (!ps) return;
    int np = 0;

    int reach = (int)(T->fardist / cell) + 1;
    if (reach > 64) reach = 64;
    long cx0 = (long)floor(c->x / cell), cz0 = (long)floor(c->z / cell);
    for (int i = -reach; i <= reach && np < MAXP; ++i)
        for (int j = -reach; j <= reach && np < MAXP; ++j) {
            long gx = cx0 + i, gz = cz0 + j;
            uint32_t hh = hash3(seed, (uint32_t)gx, (uint32_t)gz);
            if ((int)(hh & 255u) >= density) continue;      /* thinned out */
            double ox = (double)((hh >> 8)  & 1023u) / 1024.0;
            double oz = (double)((hh >> 18) & 1023u) / 1024.0;
            double px = ((double)gx + ox) * cell, pz = ((double)gz + oz) * cell;
            double ex = px - c->x, ez = pz - c->z;
            double t  = ex * fx + ez * fz;                   /* depth */
            if (t < 2.0 || t > T->fardist) continue;
            double lat = ex * rx + ez * rz;
            double sxp = w * 0.5 + lat * focal / t;
            double rr  = rad * focal / t;
            if (sxp + rr < 0 || sxp - rr >= w) continue;
            double gy = terr_h(T->seed, px, pz, T->freq, T->amp, T->oct);
            if (gy < T->water + 1.0) continue;               /* not in the lake */
            uint32_t h2 = hash3(seed ^ 0x2ba3u, (uint32_t)gx, (uint32_t)gz);
            double k = 1.0 + jitter * ((double)(h2 & 1023u) / 512.0 - 1.0);
            double top = gy + hgt * k;
            fdv_prop *q = &ps[np++];
            q->t = t; q->sx = sxp; q->r = rr * k;
            q->sybase = horizon - (gy  - c->y) * focal / t;
            q->sytop  = horizon - (top - c->y) * focal / t;
            double shade = 0.80 + 0.40 * ((double)((h2 >> 12) & 255u) / 255.0);
            double fog = 1.0 - exp(-t / (T->fardist * 0.80));
            q->fog = fog;
            for (int m = 0; m < 3; ++m) {
                int base = m == 0 ? (int)lround(col[0] * shade) : col[m];
                q->col[m] = base + (int)lround((T->haze[m] - base) * fog);
            }
        }

    /* Back to front, so a near tree covers a far one. */
    for (int i = 1; i < np; ++i)
        for (int j = i; j > 0 && ps[j].t > ps[j - 1].t; --j) {
            fdv_prop tmp = ps[j]; ps[j] = ps[j - 1]; ps[j - 1] = tmp;
        }

    for (int i = 0; i < np; ++i) {
        fdv_prop *q = &ps[i];
        /* What nearer ground hides. March the tree's own ray up to its depth
         * and keep the highest terrain row seen: everything below that is
         * behind a hill. */
        double a  = (q->sx - w * 0.5) / focal;
        double dx = fx + rx * a, dz = fz + rz * a;
        double clip = (double)h;
        for (double t = 2.0; t < q->t; t += (t * 0.02 < 1.0 ? 1.0 : t * 0.02)) {
            double r = terr_row(T, c, dx, dz, t, horizon, focal, NULL, NULL);
            if (r < clip) clip = r;
        }
        int ybot = (int)(q->sybase < clip ? q->sybase : clip);
        if (ybot > h) ybot = h;

        double cy = q->sytop + q->r;                 /* canopy centre */
        int x0 = scn_max(0, (int)(q->sx - q->r)), x1 = scn_min(w, (int)(q->sx + q->r) + 1);
        double r2 = q->r * q->r;
        for (int y = scn_max(0, (int)(cy - q->r)); y < scn_min(ybot, (int)(cy + q->r) + 1); ++y)
            for (int x = x0; x < x1; ++x) {
                double ddx = x + 0.5 - q->sx, ddy = y + 0.5 - cy;
                if (ddx * ddx + ddy * ddy > r2) continue;
                Y->p[(size_t)y * Y->w + x] = (uint8_t)clamp255(q->col[0]);
                if ((x >> 1) < U->w && (y >> 1) < U->h) {
                    U->p[(size_t)(y >> 1) * U->w + (x >> 1)] = (uint8_t)clamp255(q->col[1]);
                    V->p[(size_t)(y >> 1) * V->w + (x >> 1)] = (uint8_t)clamp255(q->col[2]);
                }
            }
        double half = tw * 0.5 * focal / q->t;
        if (half < 0.5) half = 0.5;
        int tx0 = scn_max(0, (int)(q->sx - half)), tx1 = scn_min(w, (int)(q->sx + half) + 1);
        for (int y = scn_max(0, (int)(cy + q->r * 0.2)); y < ybot; ++y)
            for (int x = tx0; x < tx1; ++x) {
                for (int m = 0; m < 3; ++m) {
                    int v = tcol[m] + (int)lround((T->haze[m] - tcol[m]) * q->fog);
                    if (m == 0) Y->p[(size_t)y * Y->w + x] = (uint8_t)clamp255(v);
                    else if ((x >> 1) < U->w && (y >> 1) < U->h)
                        (m == 1 ? U : V)->p[(size_t)(y >> 1) * U->w + (x >> 1)]
                            = (uint8_t)clamp255(v);
                }
            }
    }
    free(ps);
}

/* ===========================================================================
 * 3. FONT
 * 3x5 digits, for burning frame numbers into the picture.
 * ======================================================================== */

/* Each digit is 15 bits, row-major, 3 wide by 5 tall, MSB = top-left.  A frame
 * counter drawn into the picture is the fastest way to tell whether a decoded
 * clip is correct, in order, and complete — worth the 12 lines. */
static const uint16_t FONT35[10] = {
    0x7B6F, /* 0: 111 101 101 101 111 */
    0x2C97, /* 1: 010 110 010 010 111 */
    0x73E7, /* 2: 111 001 111 100 111 */
    0x73CF, /* 3: 111 001 111 001 111 */
    0x5BC9, /* 4: 101 101 111 001 001 */
    0x79CF, /* 5: 111 100 111 001 111 */
    0x79EF, /* 6: 111 100 111 101 111 */
    0x7249, /* 7: 111 001 001 001 001 */
    0x7BEF, /* 8: 111 101 111 101 111 */
    0x7BCF, /* 9: 111 101 111 001 111 */
};

/* Draw `n` at (x,y) in luma pixels, each font pixel scaled to `scale` square,
 * with a one-pixel dark surround so it stays readable over any background. */
static void draw_number(fdv_splane *s, int n, int x, int y, int scale, int ink, int paper) {
    char buf[16];
    int len = snprintf(buf, sizeof buf, "%d", n < 0 ? 0 : n);
    if (scale < 1) scale = 1;
    int gw = 4 * scale;                       /* 3 columns + 1 spacing */
    ras_rect(s, x - scale, y - scale, (double)len * gw + scale, 5.0 * scale + 2 * scale, paper);
    for (int d = 0; d < len; ++d) {
        uint16_t bits = FONT35[buf[d] - '0'];
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if (bits & (1u << (14 - (row * 3 + col))))
                    ras_rect(s, x + d * gw + col * scale, y + row * scale,
                             scale, scale, ink);
    }
}

/* ===========================================================================
 * 4. MODEL
 * The scene struct and its accessors.
 * ======================================================================== */

typedef enum {
    OBJ_GRAD, OBJ_RECT, OBJ_CIRCLE, OBJ_CHECKER, OBJ_NOISE, OBJ_STAMP,
    /* World layers: everything below is placed in world coordinates and moved
     * past the camera by its own `depth`, so one camera pan produces a
     * different velocity in every layer. That is the thing a flat scene cannot
     * express and the thing a real shot always has. */
    OBJ_RIDGE, OBJ_CLOUDS, OBJ_WATER, OBJ_SCATTER, OBJ_GRAIN,
    /* Genuine 3D: a heightfield and the things standing on it, seen through a
     * perspective camera that can be flown anywhere. Everything above fakes
     * depth by sliding layers at different rates; these two derive it. */
    OBJ_TERRAIN, OBJ_PROPS
} fdv_obj_kind;

typedef struct {
    fdv_obj_kind kind;
    double   x, y, w, h, r;      /* geometry, luma pixels */
    double   vx, vy;             /* pixels per frame */
    int      bounce;             /* reflect off the frame edges */
    int      luma, cb, cr;       /* colour */
    int      luma2;              /* checker's second colour */
    int      cell;               /* checker cell size */
    int      dir, from, to;      /* gradient, luma endpoints */
    int      cb_from, cb_to;     /* ... and its chroma endpoints, so colour can
                                  * ramp independently of brightness */
    int      cr_from, cr_to;
    int      sigma;              /* noise */
    int      scale;              /* stamp */
    int      alpha;              /* 255 opaque (the default), less composites */
    int      tex;                /* per-object grain, +/- this many levels */
    double   spin;               /* degrees per frame, rect and checker */
    double   dluma, dcb, dcr;    /* colour drift per frame */

    /* --- world layers -------------------------------------------------- */
    double   depth;      /* parallax: screen shift per unit of camera pan.
                          * 0 pins a layer to the camera (the far sky), 1 moves
                          * it a pixel per pixel (the ground at your feet). */
    double   freq;       /* base noise frequency, in cycles across the frame
                          * width -- a ratio, so it survives a resize */
    double   amp;        /* ridge height / water displacement / grain depth */
    double   speed;      /* animation rate, cycles per frame */
    int      oct;        /* fBm octaves */
    int      count;      /* scatter instances */
    double   jitter;     /* scatter size and colour spread, 0..1 */
    int      thresh;     /* clouds: coverage cut, 0..255 */
    int      cb2, cr2;   /* far/second chroma, to pair with luma2 */
    double   iw, ih;     /* scatter: one instance's size (r>0 draws discs) */
    double   far;        /* terrain: draw distance, in world units */
    int      wl, wcb, wcr;   /* terrain: the water surface */
    int      hl, hcb, hcr;   /* terrain: what distance fades toward */
    double   fade;       /* ridge: how much of the fill the far colour holds,
                          * as a fraction of the band. Small values put snow on
                          * the crests instead of hazing the whole face. */
    int      oseed;      /* nonzero pins this layer's noise, so two layers can
                          * share one shape -- a canopy and its trunk, a rock
                          * face and the snow on it */
} fdv_scene_obj;

struct scene {
    int w, h, frames, fps, qp, keyint;
    uint32_t seed;
    int bg_y, bg_u, bg_v;
    double cam_vx, cam_vy;       /* camera pan, world units per frame */
    int    has3;                 /* a camera3 directive was given */
    fdv_cam3 cam;                /* its state at frame 0 */
    double c3vx, c3vy, c3vz;     /* travel per frame */
    double c3dyaw, c3dpitch;     /* turn per frame */
    double c3agl;                /* >0: fly this far above the ground instead
                                  * of at a fixed height, which is what makes a
                                  * flythrough follow the landscape */
    fdv_scene_obj *obj;
    int nobj, objcap;
    int *cut;
    int ncut, cutcap;
    char name[64];
};

int fdv_scene_width  (const fdv_scene *s) { return s ? s->w : 0; }
int fdv_scene_height (const fdv_scene *s) { return s ? s->h : 0; }
int fdv_scene_frames (const fdv_scene *s) { return s ? s->frames : 0; }
int fdv_scene_fps    (const fdv_scene *s) { return s ? s->fps : 0; }
int fdv_scene_qp     (const fdv_scene *s) { return s ? s->qp : -1; }
int fdv_scene_keyint (const fdv_scene *s) { return s ? s->keyint : -1; }
int fdv_scene_objects(const fdv_scene *s) { return s ? s->nobj : 0; }
const char *fdv_scene_name(const fdv_scene *s) { return s ? s->name : ""; }

size_t fdv_scene_frame_size(const fdv_scene *s) {
    if (!s) return 0;
    return (size_t)s->w * s->h + 2 * (size_t)(s->w / 2) * (s->h / 2);
}

void fdv_scene_free(fdv_scene *s) {
    if (!s) return;
    free(s->obj);
    free(s->cut);
    free(s);
}

static fdv_scene_obj *fdv_scene_add(fdv_scene *s, fdv_obj_kind k) {
    if (s->nobj == s->objcap) {
        int cap = s->objcap ? s->objcap * 2 : 8;
        fdv_scene_obj *n = realloc(s->obj, (size_t)cap * sizeof(*n));
        if (!n) return NULL;
        s->obj = n; s->objcap = cap;
    }
    fdv_scene_obj *o = &s->obj[s->nobj++];
    memset(o, 0, sizeof(*o));
    o->kind = k;
    /* Defaults chosen so every attribute is optional. */
    o->luma = 200; o->cb = 128; o->cr = 128; o->luma2 = 16;
    o->w = 32; o->h = 32; o->r = 16; o->cell = 8;
    o->from = 16; o->to = 235; o->sigma = 3; o->scale = 3;
    o->cb_from = o->cb_to = o->cr_from = o->cr_to = 128;   /* colourless unless asked */
    o->alpha = 255;
    o->depth = 1.0; o->freq = 4.0; o->amp = 24.0; o->oct = 5;
    o->count = 200; o->jitter = 0.5; o->thresh = 128;
    o->cb2 = o->cr2 = 128; o->iw = 3.0; o->ih = 8.0;
    o->fade = 1.0; o->far = 900.0;
    o->wl = 96;  o->wcb = 150; o->wcr = 118;
    o->hl = 208; o->hcb = 130; o->hcr = 134;
    return o;
}

static int fdv_scene_add_cut(fdv_scene *s, int frame) {
    if (s->ncut == s->cutcap) {
        int cap = s->cutcap ? s->cutcap * 2 : 4;
        int *n = realloc(s->cut, (size_t)cap * sizeof(*n));
        if (!n) return -1;
        s->cut = n; s->cutcap = cap;
    }
    s->cut[s->ncut++] = frame;
    return 0;
}

int fdv_scene_resize(fdv_scene *s, int w, int h) {
    if (!s || w <= 0 || h <= 0 || (w & 15) || (h & 15)) return -1;
    if (w == s->w && h == s->h) return 0;

    double sx = (double)w / s->w, sy = (double)h / s->h;
    /* Round shapes and glyphs have one size, not two; scale them by the mean so
     * a circle stays a circle when the aspect ratio changes. */
    double sm = (sx + sy) * 0.5;

    for (int i = 0; i < s->nobj; ++i) {
        fdv_scene_obj *o = &s->obj[i];
        /* The 3D layers are already resolution-independent: they are measured
         * in world units and projected through a focal length derived from the
         * frame width. Scaling them would move the camera, not the picture. */
        if (o->kind == OBJ_TERRAIN || o->kind == OBJ_PROPS) continue;
        o->x  *= sx;  o->w  *= sx;  o->vx *= sx;
        o->y  *= sy;  o->h  *= sy;  o->vy *= sy;
        o->r  *= sm;
        /* Checker cells and the frame-number glyph scale with the picture, so
         * texture stays the same size relative to the frame rather than
         * becoming fine detail at high resolution. */
        o->cell  = (int)(o->cell * sm + 0.5);
        o->scale = (int)(o->scale * sy + 0.5);
        if (o->cell < 1) o->cell = 1;
        if (o->scale < 1) o->scale = 1;
        /* World layers: `amp` and the instance size are pixel measurements and
         * scale. `freq` is deliberately in cycles across the frame width and
         * `depth` is a ratio, so both are already resolution-independent. */
        o->amp *= sy;
        o->iw  *= sx;
        o->ih  *= sy;
    }
    s->cam_vx *= sx;
    s->cam_vy *= sy;
    s->w = w;
    s->h = h;
    return 0;
}

int fdv_scene_parse_size(const char *arg, int *w, int *h) {
    if (!arg || !w || !h) return -1;
    /* Already rounded to legal dimensions: this codec needs multiples of 16, so
     * the nominal sizes that are not (1080, and 480p's 854 width) are given as
     * the nearest codable ones -- the same padding real encoders carry and crop
     * on display. */
    static const struct { const char *name; int w, h; } SHORT[] = {
        {"360p",   640,  368},   /* 640x360   -> 368 */
        {"480p",   848,  480},   /* 854x480   -> 848 */
        {"720p",  1280,  720},
        {"1080p", 1920, 1088},   /* 1920x1080 -> 1088 */
        {"1440p", 2560, 1440},
        {"2160p", 3840, 2160},
        {"4k",    3840, 2160},
    };
    for (size_t i = 0; i < sizeof SHORT / sizeof *SHORT; ++i)
        if (!strcmp(arg, SHORT[i].name)) {
            *w = SHORT[i].w;
            *h = SHORT[i].h;
            return 0;
        }

    char *end;
    long a = strtol(arg, &end, 10);
    if (end == arg || (*end != 'x' && *end != 'X')) return -1;
    long b = strtol(end + 1, &end, 10);
    if (*end != '\0' || a <= 0 || b <= 0 || a > 8192 || b > 8192) return -1;
    if ((a & 15) || (b & 15)) return -1;
    *w = (int)a;
    *h = (int)b;
    return 0;
}

/* ===========================================================================
 * 5. PARSE
 * The text format.
 * ======================================================================== */

static void err_set(char *err, size_t cap, int line, const char *fmt, ...) {
    if (!err || cap == 0) return;
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (line > 0) snprintf(err, cap, "line %d: %s", line, body);
    else          snprintf(err, cap, "%s", body);
}

/* Split a line into whitespace-separated tokens, in place, dropping comments. */
static int tokenize(char *line, char **tok, int maxtok) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    int n = 0;
    char *p = line;
    while (*p && n < maxtok) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* Look up `key` among `key=value` tokens; returns 1 and writes *out if found.
 * An unparseable value is a hard error, reported through *bad. */
static int attr(char **tok, int ntok, const char *key, double *out, const char **bad) {
    size_t klen = strlen(key);
    for (int i = 1; i < ntok; ++i) {
        if (strncmp(tok[i], key, klen) != 0 || tok[i][klen] != '=') continue;
        const char *v = tok[i] + klen + 1;
        char *end;
        double d = strtod(v, &end);
        if (end == v || *end != '\0') { *bad = tok[i]; return 0; }
        *out = d;
        return 1;
    }
    return 0;
}

static int attri(char **tok, int ntok, const char *key, int *out, const char **bad) {
    double d;
    if (!attr(tok, ntok, key, &d, bad)) return 0;
    *out = (int)lround(d);
    return 1;
}

/* Reject attributes that no directive on this line understands, so a typo is a
 * parse error rather than a silently ignored instruction. */
static int check_attrs(char **tok, int ntok, const char *const *allowed,
                       char *err, size_t errcap, int line) {
    for (int i = 1; i < ntok; ++i) {
        const char *eq = strchr(tok[i], '=');
        if (!eq) {
            err_set(err, errcap, line, "expected key=value, got '%s'", tok[i]);
            return -1;
        }
        size_t klen = (size_t)(eq - tok[i]);
        int ok = 0;
        for (int a = 0; allowed[a]; ++a)
            if (strlen(allowed[a]) == klen && !strncmp(allowed[a], tok[i], klen)) { ok = 1; break; }
        if (!ok) {
            err_set(err, errcap, line, "'%s' has no attribute '%.*s'",
                    tok[0], (int)klen, tok[i]);
            return -1;
        }
    }
    return 0;
}

fdv_scene *fdv_scene_parse_text(const char *text, char *err, size_t errcap) {
    static const char *A_MOVE[]    = {"x","y","w","h","r","luma","cb","cr","vx","vy","bounce",
                                      "alpha","tex","spin","dluma","dcb","dcr",NULL};
    static const char *A_CHECKER[] = {"x","y","w","h","cell","luma","luma2","cb","cr",
                                      "vx","vy","bounce","alpha","dluma","dcb","dcr",NULL};
    static const char *A_GRAD[]    = {"dir","from","to",
                                      "cb_from","cb_to","cr_from","cr_to",NULL};
    static const char *A_NOISE[]   = {"sigma",NULL};
    static const char *A_STAMP[]   = {"x","y","scale","luma","cb","cr",NULL};
    static const char *A_CUT[]     = {"frame",NULL};
    static const char *A_BG[]      = {"luma","cb","cr",NULL};
    static const char *A_CAMERA[]  = {"vx","vy",NULL};
    /* The 3D camera and the two layers that use it. World units throughout --
     * `fdv_scene_resize` deliberately leaves these alone, because a perspective
     * projection is already resolution-independent. */
    static const char *A_CAMERA3[] = {"x","y","z","yaw","pitch","fov",
                                      "vx","vy","vz","dyaw","dpitch","agl",NULL};
    static const char *A_TERRAIN[] = {"freq","amp","oct","far","y","thresh",
                                      "speed","tex","seed","luma","cb","cr",
                                      "luma2","cb2","cr2","wluma","wcb","wcr",
                                      "haze","hcb","hcr",NULL};
    static const char *A_PROPS[]   = {"density","cell","r","ih","iw","jitter",
                                      "seed","luma","cb","cr",
                                      "luma2","cb2","cr2",NULL};
    /* World layers. `depth` is the parallax factor and `freq` is in cycles
     * across the frame width, so both survive a resize unchanged. */
    static const char *A_RIDGE[]   = {"y","amp","freq","oct","depth","tex",
                                      "luma","luma2","cb","cr","cb2","cr2",
                                      "fade","seed",NULL};
    static const char *A_CLOUDS[]  = {"y","h","freq","oct","depth","speed",
                                      "thresh","alpha","luma","cb","cr","seed",NULL};
    static const char *A_WATER[]   = {"y","h","freq","amp","oct","depth","speed",
                                      "alpha","cb","cr",NULL};
    static const char *A_SCATTER[] = {"x","y","w","h","iw","ih","r","count",
                                      "jitter","depth","alpha","luma","cb","cr",
                                      "vx","vy","tex","seed",NULL};
    static const char *A_GRAIN[]   = {"freq","amp","oct","depth","speed","seed",NULL};

    if (!text) { err_set(err, errcap, 0, "no scene text"); return NULL; }

    fdv_scene *s = calloc(1, sizeof(*s));
    if (!s) { err_set(err, errcap, 0, "out of memory"); return NULL; }
    s->w = 160; s->h = 96; s->frames = 16; s->fps = 25;
    s->qp = -1; s->keyint = -1; s->seed = 12345u;
    s->bg_y = 32; s->bg_u = 128; s->bg_v = 128;
    snprintf(s->name, sizeof s->name, "scene");

    char *copy = strdup(text);
    if (!copy) { free(s); err_set(err, errcap, 0, "out of memory"); return NULL; }

    int line = 0, failed = 0;
    char *save = NULL;
    for (char *ln = strtok_r(copy, "\n", &save); ln && !failed;
         ln = strtok_r(NULL, "\n", &save)) {
        ++line;
        char *tok[32];
        int ntok = tokenize(ln, tok, 32);
        if (ntok == 0) continue;

        const char *bad = NULL;
        const char *cmd = tok[0];

        /* --- header directives: positional, `key v...` --- */
        if (!strcmp(cmd, "size")) {
            if (ntok != 3) { err_set(err, errcap, line, "size needs W and H"); failed = 1; break; }
            s->w = atoi(tok[1]); s->h = atoi(tok[2]);
            if (s->w <= 0 || s->h <= 0 || (s->w & 15) || (s->h & 15)) {
                err_set(err, errcap, line,
                        "size %dx%d invalid: both must be positive multiples of 16", s->w, s->h);
                failed = 1; break;
            }
            continue;
        }
        if (!strcmp(cmd, "frames")) {
            if (ntok != 2 || (s->frames = atoi(tok[1])) <= 0) {
                err_set(err, errcap, line, "frames needs a positive count"); failed = 1; break;
            }
            continue;
        }
        if (!strcmp(cmd, "fps"))    { if (ntok == 2) s->fps    = atoi(tok[1]); continue; }
        if (!strcmp(cmd, "qp"))     { if (ntok == 2) s->qp     = atoi(tok[1]); continue; }
        if (!strcmp(cmd, "keyint")) { if (ntok == 2) s->keyint = atoi(tok[1]); continue; }
        if (!strcmp(cmd, "seed"))   { if (ntok == 2) s->seed   = (uint32_t)strtoul(tok[1], NULL, 0); continue; }
        if (!strcmp(cmd, "name"))   { if (ntok == 2) snprintf(s->name, sizeof s->name, "%s", tok[1]); continue; }

        if (!strcmp(cmd, "bg")) {
            if (check_attrs(tok, ntok, A_BG, err, errcap, line) != 0) { failed = 1; break; }
            attri(tok, ntok, "luma", &s->bg_y, &bad);
            attri(tok, ntok, "cb",   &s->bg_u, &bad);
            attri(tok, ntok, "cr",   &s->bg_v, &bad);
            if (bad) { err_set(err, errcap, line, "bad value in '%s'", bad); failed = 1; break; }
            continue;
        }
        if (!strcmp(cmd, "camera")) {
            if (check_attrs(tok, ntok, A_CAMERA, err, errcap, line) != 0) { failed = 1; break; }
            attr(tok, ntok, "vx", &s->cam_vx, &bad);
            attr(tok, ntok, "vy", &s->cam_vy, &bad);
            if (bad) { err_set(err, errcap, line, "bad value in '%s'", bad); failed = 1; break; }
            continue;
        }
        if (!strcmp(cmd, "camera3")) {
            if (check_attrs(tok, ntok, A_CAMERA3, err, errcap, line) != 0) { failed = 1; break; }
            s->has3 = 1;
            if (s->cam.fov <= 0.0) s->cam.fov = 1.05;
            attr(tok, ntok, "x",      &s->cam.x,     &bad);
            attr(tok, ntok, "y",      &s->cam.y,     &bad);
            attr(tok, ntok, "z",      &s->cam.z,     &bad);
            attr(tok, ntok, "yaw",    &s->cam.yaw,   &bad);
            attr(tok, ntok, "pitch",  &s->cam.pitch, &bad);
            attr(tok, ntok, "fov",    &s->cam.fov,   &bad);
            attr(tok, ntok, "vx",     &s->c3vx,      &bad);
            attr(tok, ntok, "vy",     &s->c3vy,      &bad);
            attr(tok, ntok, "vz",     &s->c3vz,      &bad);
            attr(tok, ntok, "dyaw",   &s->c3dyaw,    &bad);
            attr(tok, ntok, "dpitch", &s->c3dpitch,  &bad);
            attr(tok, ntok, "agl",    &s->c3agl,     &bad);
            if (bad) { err_set(err, errcap, line, "bad value in '%s'", bad); failed = 1; break; }
            continue;
        }
        if (!strcmp(cmd, "cut")) {
            if (check_attrs(tok, ntok, A_CUT, err, errcap, line) != 0) { failed = 1; break; }
            int f = 0;
            if (!attri(tok, ntok, "frame", &f, &bad) || bad) {
                err_set(err, errcap, line, "cut needs frame=N"); failed = 1; break;
            }
            if (fdv_scene_add_cut(s, f) != 0) { err_set(err, errcap, line, "out of memory"); failed = 1; break; }
            continue;
        }

        /* --- draw directives --- */
        fdv_obj_kind kind;
        const char *const *allowed;
        if      (!strcmp(cmd, "rect"))    { kind = OBJ_RECT;    allowed = A_MOVE; }
        else if (!strcmp(cmd, "circle"))  { kind = OBJ_CIRCLE;  allowed = A_MOVE; }
        else if (!strcmp(cmd, "checker")) { kind = OBJ_CHECKER; allowed = A_CHECKER; }
        else if (!strcmp(cmd, "grad"))    { kind = OBJ_GRAD;    allowed = A_GRAD; }
        else if (!strcmp(cmd, "noise"))   { kind = OBJ_NOISE;   allowed = A_NOISE; }
        else if (!strcmp(cmd, "stamp"))   { kind = OBJ_STAMP;   allowed = A_STAMP; }
        else if (!strcmp(cmd, "ridge"))   { kind = OBJ_RIDGE;   allowed = A_RIDGE; }
        else if (!strcmp(cmd, "clouds"))  { kind = OBJ_CLOUDS;  allowed = A_CLOUDS; }
        else if (!strcmp(cmd, "water"))   { kind = OBJ_WATER;   allowed = A_WATER; }
        else if (!strcmp(cmd, "scatter")) { kind = OBJ_SCATTER; allowed = A_SCATTER; }
        else if (!strcmp(cmd, "grain"))   { kind = OBJ_GRAIN;   allowed = A_GRAIN; }
        else if (!strcmp(cmd, "terrain")) { kind = OBJ_TERRAIN; allowed = A_TERRAIN; }
        else if (!strcmp(cmd, "props"))   { kind = OBJ_PROPS;   allowed = A_PROPS; }
        else {
            err_set(err, errcap, line, "unknown directive '%s'", cmd);
            failed = 1; break;
        }

        if (check_attrs(tok, ntok, allowed, err, errcap, line) != 0) { failed = 1; break; }
        fdv_scene_obj *o = fdv_scene_add(s, kind);
        if (!o) { err_set(err, errcap, line, "out of memory"); failed = 1; break; }

        attr (tok, ntok, "x",      &o->x,      &bad);
        attr (tok, ntok, "y",      &o->y,      &bad);
        attr (tok, ntok, "w",      &o->w,      &bad);
        attr (tok, ntok, "h",      &o->h,      &bad);
        attr (tok, ntok, "r",      &o->r,      &bad);
        attr (tok, ntok, "vx",     &o->vx,     &bad);
        attr (tok, ntok, "vy",     &o->vy,     &bad);
        attri(tok, ntok, "bounce", &o->bounce, &bad);
        attri(tok, ntok, "luma",   &o->luma,   &bad);
        attri(tok, ntok, "luma2",  &o->luma2,  &bad);
        attri(tok, ntok, "cb",     &o->cb,     &bad);
        attri(tok, ntok, "cr",     &o->cr,     &bad);
        attri(tok, ntok, "cell",   &o->cell,   &bad);
        attri(tok, ntok, "from",   &o->from,   &bad);
        attri(tok, ntok, "to",     &o->to,     &bad);
        attri(tok, ntok, "sigma",  &o->sigma,  &bad);
        attri(tok, ntok, "scale",  &o->scale,  &bad);
        attri(tok, ntok, "cb_from",&o->cb_from,&bad);
        attri(tok, ntok, "cb_to",  &o->cb_to,  &bad);
        attri(tok, ntok, "cr_from",&o->cr_from,&bad);
        attri(tok, ntok, "cr_to",  &o->cr_to,  &bad);
        attri(tok, ntok, "alpha",  &o->alpha,  &bad);
        attri(tok, ntok, "tex",    &o->tex,    &bad);
        attr (tok, ntok, "spin",   &o->spin,   &bad);
        attr (tok, ntok, "depth",  &o->depth,  &bad);
        attr (tok, ntok, "freq",   &o->freq,   &bad);
        attr (tok, ntok, "amp",    &o->amp,    &bad);
        attr (tok, ntok, "speed",  &o->speed,  &bad);
        attr (tok, ntok, "jitter", &o->jitter, &bad);
        attr (tok, ntok, "iw",     &o->iw,     &bad);
        attr (tok, ntok, "ih",     &o->ih,     &bad);
        attr (tok, ntok, "fade",   &o->fade,   &bad);
        attri(tok, ntok, "seed",   &o->oseed,  &bad);
        attr (tok, ntok, "far",    &o->far,    &bad);
        attri(tok, ntok, "density",&o->count,  &bad);
        attri(tok, ntok, "wluma",  &o->wl,     &bad);
        attri(tok, ntok, "wcb",    &o->wcb,    &bad);
        attri(tok, ntok, "wcr",    &o->wcr,    &bad);
        attri(tok, ntok, "haze",   &o->hl,     &bad);
        attri(tok, ntok, "hcb",    &o->hcb,    &bad);
        attri(tok, ntok, "hcr",    &o->hcr,    &bad);
        attri(tok, ntok, "oct",    &o->oct,    &bad);
        attri(tok, ntok, "count",  &o->count,  &bad);
        attri(tok, ntok, "thresh", &o->thresh, &bad);
        attri(tok, ntok, "cb2",    &o->cb2,    &bad);
        attri(tok, ntok, "cr2",    &o->cr2,    &bad);
        attr (tok, ntok, "dluma",  &o->dluma,  &bad);
        attr (tok, ntok, "dcb",    &o->dcb,    &bad);
        attr (tok, ntok, "dcr",    &o->dcr,    &bad);
        {
            double d;
            const char *dirbad = NULL;
            if (attr(tok, ntok, "dir", &d, &dirbad)) o->dir = (int)lround(d) != 0;
            else if (dirbad) {                       /* dir=h / dir=v */
                for (int i = 1; i < ntok; ++i)
                    if (!strncmp(tok[i], "dir=", 4)) {
                        char c = tok[i][4];
                        if      (c == 'h' || c == 'H') o->dir = 0;
                        else if (c == 'v' || c == 'V') o->dir = 1;
                        else if (c == 'r' || c == 'R') o->dir = 2;
                        else { bad = tok[i]; }
                    }
            }
        }
        if (bad) { err_set(err, errcap, line, "bad value in '%s'", bad); failed = 1; break; }
    }

    free(copy);
    if (failed) { fdv_scene_free(s); return NULL; }
    if (s->cut) {                                  /* keep cuts in frame order */
        for (int i = 1; i < s->ncut; ++i)
            for (int j = i; j > 0 && s->cut[j] < s->cut[j - 1]; --j) {
                int t = s->cut[j]; s->cut[j] = s->cut[j - 1]; s->cut[j - 1] = t;
            }
    }
    return s;
}

fdv_scene *fdv_scene_parse_file(const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    if (!f) { err_set(err, errcap, 0, "cannot open '%s'", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); err_set(err, errcap, 0, "cannot size '%s'", path); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); err_set(err, errcap, 0, "out of memory"); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); err_set(err, errcap, 0, "short read on '%s'", path); return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    fdv_scene *s = fdv_scene_parse_text(buf, err, errcap);
    if (s) {
        const char *base = strrchr(path, '/');
        snprintf(s->name, sizeof s->name, "%s", base ? base + 1 : path);
    }
    free(buf);
    return s;
}

/* ===========================================================================
 * 6. RENDER
 * Scene + frame index -> pixels.
 * ======================================================================== */

/* Fold v into [lo,hi] by reflection, so a bouncing object never leaves frame.
 * A plain clamp would make objects pile up on the edges and stop moving, which
 * is exactly the content that stops exercising the motion search. */
static double reflect(double v, double lo, double hi) {
    if (hi <= lo) return lo;
    double span = hi - lo, two = 2.0 * span;
    double t = fmod(v - lo, two);
    if (t < 0) t += two;
    return lo + (t <= span ? t : two - t);
}

/* How many cuts have happened by frame f, and when the last one was.  A cut is
 * a hard scene change: object positions jump, so the encoder has to fall back
 * to intra for that frame. */
static void cut_state(const fdv_scene *s, int f, int *epoch, int *since) {
    int e = 0, last = 0;
    for (int i = 0; i < s->ncut; ++i)
        if (s->cut[i] <= f) { ++e; last = s->cut[i]; }
    *epoch = e;
    *since = f - last;
}

/* Where object `i` sits at frame `f`. */
static void obj_pos(const fdv_scene *s, int i, int f, double *ox, double *oy) {
    const fdv_scene_obj *o = &s->obj[i];
    int epoch, t;
    cut_state(s, f, &epoch, &t);

    double x = o->x, y = o->y;
    if (epoch > 0) {                    /* jump to a new deterministic spot */
        uint32_t hx = hash3(s->seed, (uint32_t)epoch, (uint32_t)(i * 2 + 0));
        uint32_t hy = hash3(s->seed, (uint32_t)epoch, (uint32_t)(i * 2 + 1));
        x = (double)(hx % (uint32_t)scn_max(1, s->w));
        y = (double)(hy % (uint32_t)scn_max(1, s->h));
    }
    x += o->vx * t;
    y += o->vy * t;

    if (o->bounce) {
        double ew = (o->kind == OBJ_CIRCLE) ? o->r * 2 : o->w;
        double eh = (o->kind == OBJ_CIRCLE) ? o->r * 2 : o->h;
        x = reflect(x, 0.0, scn_max(0, s->w - (int)ew));
        y = reflect(y, 0.0, scn_max(0, s->h - (int)eh));
    }
    *ox = x; *oy = y;
}

void fdv_scene_render(const fdv_scene *s, int f, uint8_t *i420) {
    if (!s || !i420) return;
    int w = s->w, h = s->h, cw = w / 2, ch = h / 2;
    fdv_splane Y = {i420, w, h, 0};
    fdv_splane U = {i420 + (size_t)w * h, cw, ch, 1};
    fdv_splane V = {i420 + (size_t)w * h + (size_t)cw * ch, cw, ch, 1};
    fdv_splane *pl[3] = {&Y, &U, &V};

    ras_fill(&Y, s->bg_y);
    ras_fill(&U, s->bg_u);
    ras_fill(&V, s->bg_v);

    /* The 3D camera at this frame, and the terrain it is flying over. Props
     * stand on whichever terrain was declared above them, so the renderer
     * carries the last one forward -- the same painter's order the rest of the
     * language uses, applied to a fact about the world rather than to pixels. */
    fdv_cam3 cam = s->cam;
    cam.x += s->c3vx * f; cam.y += s->c3vy * f; cam.z += s->c3vz * f;
    cam.yaw += s->c3dyaw * f; cam.pitch += s->c3dpitch * f;
    if (cam.fov <= 0.0) cam.fov = 1.05;
    fdv_terr lastT; int haveT = 0;
    memset(&lastT, 0, sizeof lastT);
    if (s->has3 && s->c3agl > 0.0) {
        /* Follow the ground rather than flying a straight line through it. */
        for (int i = 0; i < s->nobj; ++i) {
            if (s->obj[i].kind != OBJ_TERRAIN) continue;
            const fdv_scene_obj *o = &s->obj[i];
            uint32_t sd = s->seed + (o->oseed ? (uint32_t)o->oseed * 2654435761u
                                              : (uint32_t)i * 2246822519u);
            double g = terr_h(sd, cam.x, cam.z, o->freq, o->amp, o->oct);
            if (g < o->y) g = o->y;
            cam.y = g + s->c3agl;
            break;
        }
    }

    for (int i = 0; i < s->nobj; ++i) {
        const fdv_scene_obj *o = &s->obj[i];
        /* Colour may drift with time, so a scene can change hue as well as
         * move -- chroma that varies is the case flat fills never produce. */
        int colour[3] = {
            clamp255(o->luma + (int)lround(o->dluma * f)),
            clamp255(o->cb   + (int)lround(o->dcb   * f)),
            clamp255(o->cr   + (int)lround(o->dcr   * f))
        };
        double x, y;
        obj_pos(s, i, f, &x, &y);
        double ang = o->spin * f * (3.14159265358979323846 / 180.0);
        /* Where this layer sits after the camera has moved. One pan, a
         * different shift per layer -- that is the whole point of `depth`. */
        /* A 3D camera's yaw is a pan to anything far enough away to be drawn
         * as a flat layer, which is exactly what the sky is. */
        double yawpan = s->has3 ? cam.yaw / cam.fov * w : 0.0;
        double camx = (s->cam_vx * f + yawpan) * o->depth;
        double camy = s->cam_vy * f * o->depth;
        /* A layer with an explicit seed gets the same noise as any other layer
         * carrying that seed, which is how a trunk lands under its own canopy. */
        uint32_t osd = s->seed + (o->oseed ? (uint32_t)o->oseed * 2654435761u
                                           : (uint32_t)i * 2246822519u);

        switch (o->kind) {
        case OBJ_RECT:
            if (o->spin != 0.0 || o->alpha < 255 || o->tex)
                for (int p = 0; p < 3; ++p)
                    ras_rect_rot(pl[p], x + o->w * 0.5, y + o->h * 0.5, o->w, o->h,
                                 ang, colour[p], o->alpha, p ? 0 : o->tex,
                                 s->seed + (uint32_t)i, (uint32_t)f);
            else
                for (int p = 0; p < 3; ++p) ras_rect(pl[p], x, y, o->w, o->h, colour[p]);
            break;
        case OBJ_CIRCLE:
            if (o->alpha < 255)
                for (int p = 0; p < 3; ++p)
                    ras_circle_a(pl[p], x + o->r, y + o->r, o->r, colour[p], o->alpha);
            else
                for (int p = 0; p < 3; ++p) ras_circle(pl[p], x + o->r, y + o->r, o->r, colour[p]);
            break;
        case OBJ_CHECKER:
            ras_checker(&Y, x, y, o->w, o->h, o->cell, o->luma, o->luma2);
            ras_rect(&U, x, y, o->w, o->h, colour[1]);
            ras_rect(&V, x, y, o->w, o->h, colour[2]);
            break;
        case OBJ_GRAD:
            ras_grad2(&Y, o->dir, o->from, o->to);
            if (o->cb_from != 128 || o->cb_to != 128) ras_grad2(&U, o->dir, o->cb_from, o->cb_to);
            if (o->cr_from != 128 || o->cr_to != 128) ras_grad2(&V, o->dir, o->cr_from, o->cr_to);
            break;
        case OBJ_NOISE:
            /* Luma only: chroma noise at half resolution reads as colour
             * fringing rather than film grain, and luma is what the transform
             * and entropy coder are being stressed on. */
            ras_noise(&Y, s->seed, (uint32_t)f, o->sigma);
            break;
        case OBJ_STAMP:
            draw_number(&Y, f, (int)x, (int)y, o->scale, o->luma, 16);
            break;
        case OBJ_RIDGE: {
            int farv[3] = {o->luma2, o->cb2, o->cr2};
            for (int q = 0; q < 3; ++q)
                ras_ridge(pl[q], osd, camx, o->y + camy, o->amp, o->freq, o->oct,
                          colour[q], farv[q], o->fade, q ? 0 : o->tex, w, h);
            break; }
        case OBJ_CLOUDS:
            for (int q = 0; q < 3; ++q)
                ras_clouds(pl[q], osd, camx,
                           o->speed * f * w, o->y + camy, o->h, o->freq, o->oct,
                           o->thresh, o->alpha, colour[q], w);
            break;
        case OBJ_WATER:
            for (int q = 0; q < 3; ++q)
                ras_water(pl[q], osd, camx, o->speed * f,
                          o->y + camy, o->h, o->freq, o->amp, o->oct,
                          colour[q], q ? o->alpha : 0, q == 0, w);
            break;
        case OBJ_SCATTER:
            for (int q = 0; q < 3; ++q)
                ras_scatter(pl[q], osd, camx,
                            x, y + camy, o->w, o->h, o->iw, o->ih, o->r,
                            o->count, o->jitter, colour[q], o->alpha);
            break;
        case OBJ_TERRAIN: {
            if (!s->has3) break;
            fdv_terr T;
            T.seed = osd; T.freq = o->freq; T.amp = o->amp; T.oct = o->oct;
            T.fardist = o->far > 1.0 ? o->far : 1.0;
            T.water = o->y; T.ripple = o->speed * f;
            T.snow = o->thresh / 255.0; T.tex = o->tex;
            T.lo[0] = colour[0]; T.lo[1] = colour[1]; T.lo[2] = colour[2];
            T.hi[0] = o->luma2;  T.hi[1] = o->cb2;    T.hi[2] = o->cr2;
            T.wat[0] = o->wl;    T.wat[1] = o->wcb;   T.wat[2] = o->wcr;
            T.haze[0] = o->hl;   T.haze[1] = o->hcb;  T.haze[2] = o->hcr;
            ras_terrain(&Y, &U, &V, &cam, &T);
            lastT = T; haveT = 1;
            break; }
        case OBJ_PROPS: {
            if (!s->has3 || !haveT) break;
            int cc[3] = {colour[0], colour[1], colour[2]};
            int tc[3] = {o->luma2, o->cb2, o->cr2};
            ras_props(&Y, &U, &V, &cam, &lastT, osd,
                      o->cell > 0 ? (double)o->cell : 24.0,
                      o->count, o->r, o->ih, o->iw, o->jitter, cc, tc);
            break; }
        case OBJ_GRAIN:
            /* Luma only, for the reason `noise` is: chroma detail at half
             * resolution reads as fringing, not as texture. */
            ras_grain(&Y, osd, camx, o->speed * f,
                      o->freq, o->amp, o->oct, w);
            break;
        }
    }
}

void fdv_scene_render_all(const fdv_scene *s, uint8_t *dst) {
    if (!s || !dst) return;
    size_t fs = fdv_scene_frame_size(s);
    for (int f = 0; f < s->frames; ++f) fdv_scene_render(s, f, dst + (size_t)f * fs);
}

fdv_scene_info fdv_scene_measure(const uint8_t *i420, const uint8_t *prev, int w, int h) {
    fdv_scene_info fi = {0.0, 0.0, 0.0};
    size_t n = (size_t)w * h;
    if (n == 0) return fi;
    double sum = 0.0, sum2 = 0.0, mad = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = i420[i];
        sum += v; sum2 += v * v;
        if (prev) mad += fabs(v - (double)prev[i]);
    }
    fi.mean = sum / (double)n;
    double var = sum2 / (double)n - fi.mean * fi.mean;
    fi.sd  = var > 0.0 ? sqrt(var) : 0.0;
    fi.mad = prev ? mad / (double)n : 0.0;
    return fi;
}

void fdv_scene_describe(const fdv_scene *s, FILE *out, const char *tag) {
    if (!s || !out) return;
    static const char *KIND[] = {"grad","rect","circle","checker","noise","stamp",
                                 "ridge","clouds","water","scatter","grain",
                                 "terrain","props"};
    fprintf(out, "[%-6s] scene '%s': %dx%d, %d frame(s) @%d fps, %d object(s)",
            tag, s->name, s->w, s->h, s->frames, s->fps, s->nobj);
    if (s->qp     >= 0) fprintf(out, ", qp %d", s->qp);
    if (s->keyint >= 0) fprintf(out, ", keyint %d", s->keyint);
    fprintf(out, ", seed %u\n", s->seed);
    fprintf(out, "[%-6s]   bg luma=%d cb=%d cr=%d\n", tag, s->bg_y, s->bg_u, s->bg_v);
    if (s->cam_vx != 0.0 || s->cam_vy != 0.0)
        fprintf(out, "[%-6s]   camera pan (%+.2f,%+.2f) per frame\n",
                tag, s->cam_vx, s->cam_vy);
    if (s->has3)
        fprintf(out, "[%-6s]   camera3 at (%.0f,%.0f,%.0f) yaw %.2f pitch %.2f "
                     "fov %.2f, per frame (%+.2f,%+.2f,%+.2f) dyaw %+.4f%s\n",
                tag, s->cam.x, s->cam.y, s->cam.z, s->cam.yaw, s->cam.pitch,
                s->cam.fov, s->c3vx, s->c3vy, s->c3vz, s->c3dyaw,
                s->c3agl > 0.0 ? ", ground-following" : "");
    for (int i = 0; i < s->nobj; ++i) {
        const fdv_scene_obj *o = &s->obj[i];
        fprintf(out, "[%-6s]   %d. %-7s", tag, i, KIND[o->kind]);
        switch (o->kind) {
        case OBJ_RECT:
            fprintf(out, " at (%.0f,%.0f) %.0fx%.0f luma=%d", o->x, o->y, o->w, o->h, o->luma); break;
        case OBJ_CIRCLE:
            fprintf(out, " at (%.0f,%.0f) r=%.0f luma=%d", o->x, o->y, o->r, o->luma); break;
        case OBJ_CHECKER:
            fprintf(out, " at (%.0f,%.0f) %.0fx%.0f cell=%d", o->x, o->y, o->w, o->h, o->cell); break;
        case OBJ_GRAD:
            fprintf(out, " dir=%c %d..%d", o->dir ? 'v' : 'h', o->from, o->to); break;
        case OBJ_NOISE:
            fprintf(out, " sigma=%d", o->sigma); break;
        case OBJ_STAMP:
            fprintf(out, " at (%.0f,%.0f) scale=%d", o->x, o->y, o->scale); break;
        case OBJ_RIDGE:
            fprintf(out, " horizon y=%.0f amp=%.0f freq=%.1f oct=%d depth=%.2f",
                    o->y, o->amp, o->freq, o->oct, o->depth); break;
        case OBJ_CLOUDS:
            fprintf(out, " band y=%.0f h=%.0f freq=%.1f thresh=%d depth=%.2f",
                    o->y, o->h, o->freq, o->thresh, o->depth); break;
        case OBJ_WATER:
            fprintf(out, " surface y=%.0f h=%.0f amp=%.1f depth=%.2f",
                    o->y, o->h, o->amp, o->depth); break;
        case OBJ_SCATTER:
            fprintf(out, " %d instance(s) over %.0fx%.0f depth=%.2f",
                    o->count, o->w, o->h, o->depth); break;
        case OBJ_GRAIN:
            fprintf(out, " freq=%.0f amp=%.1f oct=%d depth=%.2f",
                    o->freq, o->amp, o->oct, o->depth); break;
        case OBJ_TERRAIN:
            fprintf(out, " freq=%.4f amp=%.0f oct=%d far=%.0f water=%.0f snow=%.2f",
                    o->freq, o->amp, o->oct, o->far, o->y, o->thresh / 255.0); break;
        case OBJ_PROPS:
            fprintf(out, " density %d/256 per %d units, r=%.1f h=%.1f",
                    o->count, o->cell, o->r, o->ih); break;
        }
        if (o->vx != 0.0 || o->vy != 0.0)
            fprintf(out, "  vel (%+.2f,%+.2f)%s", o->vx, o->vy, o->bounce ? " bouncing" : "");
        fputc('\n', out);
    }
    for (int i = 0; i < s->ncut; ++i)
        fprintf(out, "[%-6s]   cut at frame %d\n", tag, s->cut[i]);
}

/* ===========================================================================
 * 7. RESOLVE
 * Turning a name on the command line into a scene file.
 * ======================================================================== */

/* Where bare names are looked up. Three places, in order:
 *
 *   $FDV_SCENES        an explicit choice always wins
 *   ./scenes            working in the source tree
 *   FDV_SCENE_DIR      where `make install` puts them, baked in at build time
 *
 * The installed directory matters: without it, `vela encode motion out.vela`
 * would only work from inside the repository, which is a poor thing for an
 * installed tool to require. */
#ifndef FDV_SCENE_DIR
#define FDV_SCENE_DIR ""
#endif

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Fills `dirs` with the search path and returns how many entries are usable. */
static int fdv_scene_dirs(const char *dirs[3]) {
    int n = 0;
    const char *env = getenv("FDV_SCENES");
    if (env && *env) dirs[n++] = env;
    dirs[n++] = "scenes";
    if (FDV_SCENE_DIR[0]) dirs[n++] = FDV_SCENE_DIR;
    return n;
}

/* The directory to list and to name in messages: the first one that exists,
 * falling back to the first candidate so an error still says where it looked. */
const char *fdv_scene_dir(void) {
    const char *dirs[3];
    int n = fdv_scene_dirs(dirs);
    for (int i = 0; i < n; ++i) {
        char probe[1024];
        snprintf(probe, sizeof probe, "%s/.", dirs[i]);
        FILE *f = fopen(probe, "rb");
        if (f) { fclose(f); return dirs[i]; }
        /* fopen on a directory succeeds on some systems and not others; fall
         * back to asking whether anything inside it can be opened. */
        snprintf(probe, sizeof probe, "%s/motion.scn", dirs[i]);
        if (file_exists(probe)) return dirs[i];
    }
    return dirs[0];
}

/* Resolve `name` to a readable scene file.
 *
 * A path that exists is taken as given. Otherwise the name is treated as a
 * short one -- "motion" rather than "scenes/motion.scn" -- and the .scn
 * extension and the scene directory are filled in. Nothing is built in: every
 * scene is a file, so a name that resolves is a file you can open and edit. */
int fdv_scene_find(const char *name, char *out, size_t outcap) {
    if (!name || !*name || !out) return -1;
    if (file_exists(name)) {
        snprintf(out, outcap, "%s", name);
        return 0;
    }
    if (strchr(name, '/')) return -1;          /* an explicit path that is not there */

    snprintf(out, outcap, "%s.scn", name);
    if (file_exists(out)) return 0;

    const char *dirs[3];
    int n = fdv_scene_dirs(dirs);
    for (int i = 0; i < n; ++i) {
        snprintf(out, outcap, "%s/%s.scn", dirs[i], name);
        if (file_exists(out)) return 0;
        snprintf(out, outcap, "%s/%s", dirs[i], name);
        if (file_exists(out)) return 0;
    }
    return -1;
}

fdv_scene *fdv_scene_open(const char *name, char *err, size_t errcap) {
    char path[1024];
    if (fdv_scene_find(name, path, sizeof path) != 0) {
        err_set(err, errcap, 0,
                "no scene '%s' — looked for it as a path, as %s.scn, "
                "and in %s/ (set FDV_SCENES to look elsewhere)",
                name, name, fdv_scene_dir());
        return NULL;
    }
    return fdv_scene_parse_file(path, err, errcap);
}

/* The first comment line of a scene file, which is where its description
 * lives now that there is no table of built-in blurbs. Returns 0 on success. */
int fdv_scene_file_blurb(const char *path, char *out, size_t outcap) {
    if (!out || outcap == 0) return -1;
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '#') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        size_t n = strlen(p);
        while (n && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = '\0';
        if (!n) continue;
        snprintf(out, outcap, "%s", p);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* ===========================================================================
 * 8. FILEIO
 * Raw I420 and Y4M output.
 * ======================================================================== */

int fdv_yuv_write(const char *path, const uint8_t *seq, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(seq, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

int fdv_y4m_write(const char *path, const uint8_t *seq, int w, int h,
              int nframes, int fps) {
    if (w <= 0 || h <= 0 || nframes <= 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t fs = (size_t)w * h + 2 * (size_t)(w / 2) * (h / 2);
    int ok = fprintf(f, "YUV4MPEG2 W%d H%d F%d:1 Ip A1:1 C420\n",
                     w, h, fps > 0 ? fps : 25) > 0;
    for (int i = 0; i < nframes && ok; ++i) {
        ok = fprintf(f, "FRAME\n") > 0
          && fwrite(seq + (size_t)i * fs, 1, fs, f) == fs;
    }
    fclose(f);
    return ok ? 0 : -1;
}
#endif /* FDV_SCENE_IMPLEMENTATION */

#endif /* FDV_SCENE_H */
