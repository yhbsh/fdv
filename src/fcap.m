/* fcap — record the camera straight into an .fdv file.  macOS only.
 *
 * Every platform-specific line in this project lives in this file. fdv.h is
 * plain C11 with no operating system in it at all, and that is worth keeping:
 * the codec should be portable even though capture cannot be. So the split is
 * strict — AVFoundation gets frames to I420, and from there it is the same
 * fdv_enc_* calls any other program would make.
 *
 * The encoder is the streaming one for a reason: a recording has no known
 * length, and buffering raw frames would cost 3.1 MB each at 1080p. Only the
 * coded stream accumulates, which is small enough to keep until the frame count
 * is known and can be written into the header.
 *
 * Layout:
 *   1. CONVERT   NV12 (what the camera gives) -> I420 (what the codec wants)
 *   2. SINK      the capture delegate: one frame in, one frame encoded
 *   3. MAIN      device selection, session setup, and the recording loop
 */

#define FDV_IMPLEMENTATION
#include "fdv.h"

#include <math.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* Same presentation as the fdv tool, so the two read alike. */
/* Defaults to no colour, so a path that forgets ui_init still prints
 * plain text rather than "(null)". */
static fdv_palette P = { "", "", "", "", "", "", "", "" };
static void ui_init(void) { fdv_palette_for(stdout, &P); }
static void ui_head(const char *verb, const char *what) {
    printf("%s%s%s %s%s%s\n", P.bold, verb, P.rst, P.cyn, what, P.rst);
}
static void ui_row(const char *label, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("  %s%-10s%s ", P.dim, label, P.rst);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ===========================================================================
 * 1. CONVERT
 * NV12 -> I420.
 * ======================================================================== */

/* The camera delivers 4:2:0 with the two chroma planes interleaved, one byte of
 * Cb then one of Cr (NV12). The codec wants them separate (I420). That is the
 * whole conversion: copy luma row by row, and de-interleave chroma.
 *
 * Rows are copied individually because a capture buffer's stride is whatever
 * the hardware found convenient and is routinely wider than the picture. */
static void nv12_to_i420(const uint8_t *y, size_t ystride,
                         const uint8_t *uv, size_t uvstride,
                         int w, int h, uint8_t *out) {
    int cw = w / 2, ch = h / 2;
    uint8_t *dy = out;
    uint8_t *du = dy + (size_t)w * h;
    uint8_t *dv = du + (size_t)cw * ch;

    for (int j = 0; j < h; ++j)
        memcpy(dy + (size_t)j * w, y + (size_t)j * ystride, (size_t)w);

    for (int j = 0; j < ch; ++j) {
        const uint8_t *row = uv + (size_t)j * uvstride;
        uint8_t *ru = du + (size_t)j * cw;
        uint8_t *rv = dv + (size_t)j * cw;
        int i = 0;
#if defined(__ARM_NEON)
        /* vld2q_u8 de-interleaves a byte pair per lane, which is exactly the
         * NV12 layout: 16 Cb and 16 Cr samples per load. */
        for (; i + 16 <= cw; i += 16) {
            uint8x16x2_t p = vld2q_u8(row + (size_t)i * 2);
            vst1q_u8(ru + i, p.val[0]);
            vst1q_u8(rv + i, p.val[1]);
        }
#endif
        for (; i < cw; ++i) {
            ru[i] = row[(size_t)i * 2];
            rv[i] = row[(size_t)i * 2 + 1];
        }
    }
}

/* ===========================================================================
 * 2. SINK
 * The capture delegate.
 * ======================================================================== */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

@interface FdvSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    /* Public ivars rather than properties: these are written on the capture
     * queue and read on the main thread, so they have to be real atomics that
     * both sides address directly. */
    atomic_int frames, dropped, failed, dupes;
    atomic_llong encode_ns;
}
@property (nonatomic, assign) fdv_encoder *enc;
@property (nonatomic, assign) uint8_t     *i420;
@property (nonatomic, assign) int          w, h;      /* 0 until the first frame */
@property (nonatomic, assign) int          qp, keyint;
@property (nonatomic, assign) int          maxframes;
@property (nonatomic, assign) int          threads;   /* bands, 1 = single stream */
@property (nonatomic, assign) long         bitrate;   /* 0 = fixed qp instead */
@property (nonatomic, assign) BOOL         live;      /* stdout is a terminal */
@property (nonatomic, assign) BOOL         verbose;
/* The capture time grid. Touched only from the serial delegate queue. */
@property (nonatomic, assign) double       fps;      /* slots per second */
@property (nonatomic, assign) double       t0;       /* PTS of slot 0     */
@property (nonatomic, assign) long long    gridSlot; /* last slot filled  */
@property (nonatomic, assign) BOOL         havet0;
@end

@implementation FdvSink

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection {
    (void)output; (void)connection;
    if (g_stop) return;
    if (self.maxframes > 0 && atomic_load(&self->frames) >= self.maxframes) return;

    CVImageBufferRef px = CMSampleBufferGetImageBuffer(sample);
    if (!px) return;
    CVPixelBufferLockBaseAddress(px, kCVPixelBufferLock_ReadOnly);

    const uint8_t *y  = CVPixelBufferGetBaseAddressOfPlane(px, 0);
    const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(px, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(px, 0);
    size_t us = CVPixelBufferGetBytesPerRowOfPlane(px, 1);

    /* Dimensions come from the buffer, not from the device: a session preset is
     * a request, and what the camera actually hands over is the only thing that
     * settles it. Opening the encoder here rather than up front is what makes
     * that possible. Cropped to a multiple of 16 because the block grid needs
     * it -- 1080 is not one -- and cropping keeps the pixels it does encode
     * honest, where scaling would not. */
    if (y && uv && !self.enc) {
        int bw = (int)CVPixelBufferGetWidth(px)  & ~15;
        int bh = (int)CVPixelBufferGetHeight(px) & ~15;
        if (bw > 0 && bh > 0) {
            self.w = bw; self.h = bh;
            self.i420 = malloc((size_t)bw * bh * 3 / 2);
            /* Band-parallel unless asked not to. A P-frame encode is otherwise
             * single-threaded, which is what caps real motion on a machine with
             * cores to spare -- the bands are the difference between roughly 12
             * and 40 fps of new pictures at 720p. */
            int mbrows = bh / 16, nb = self.threads;
            if (nb > mbrows) nb = mbrows;
            if (nb > 1) self.enc = fdv_enc_open_tiled(bw, bh, self.qp, self.keyint,
                                                      (mbrows + nb - 1) / nb, nb);
            else        self.enc = fdv_enc_open(bw, bh, self.qp, self.keyint);
            self.threads = nb;
            if (self.enc && self.bitrate > 0)
                fdv_enc_set_bitrate(self.enc, (int)self.bitrate, (int)self.fps, 1.0);
            if (!self.enc || !self.i420) {
                fprintf(stderr, "cannot start encoding %dx%d\n", bw, bh);
                g_stop = 1;
            } else {
                printf("  %s%-10s%s %dx%d", P.dim, "capture", P.rst, bw, bh);
                if (bw != (int)CVPixelBufferGetWidth(px) ||
                    bh != (int)CVPixelBufferGetHeight(px))
                    printf(" (camera gives %zux%zu, cropped to a multiple of 16)",
                           CVPixelBufferGetWidth(px), CVPixelBufferGetHeight(px));
                printf("\n");
                fflush(stdout);
            }
        }
    }
    if (y && uv && self.enc) {
        /* Put the frame where it belongs in time, rather than assuming that
         * whatever survived arrived evenly spaced.
         *
         * When the encoder cannot keep up, AVFoundation drops frames in bursts,
         * so the gap between two frames that *do* arrive is one camera period,
         * or two, or three. Encoding them back to back and writing a single
         * average frame rate is what makes playback lurch: most frames then
         * play slightly slow and the ones with a dropped neighbour snap forward
         * at two or three times speed. The presentation timestamp is the only
         * honest record of when a frame was actually taken, so it decides which
         * slot of a fixed grid the frame belongs to. */
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        double t = CMTimeGetSeconds(pts);
        BOOL take = YES;
        long long slot = 0;

        if (!self.havet0) { self.havet0 = YES; self.t0 = t; }
        else {
            slot = llround((t - self.t0) * self.fps);
            if (slot <= self.gridSlot) {
                take = NO;              /* camera runs ahead of the grid */
            } else {
                /* Fill the slots nothing arrived for by re-encoding the frame
                 * already in i420. A repeat codes as all-SKIP -- a few hundred
                 * bytes and a fraction of a fresh frame's time -- which is what
                 * makes holding the grid affordable. Beyond a second it is not
                 * a dropped frame any more but a stall, and emitting a second
                 * of stills would only put the encoder further behind, so the
                 * grid slides instead. */
                long long gap = slot - self.gridSlot - 1;
                if (gap > (long long)self.fps) {
                    self.t0 += (double)gap / self.fps;
                    slot -= gap;
                    gap = 0;
                }
                for (long long g = 0; g < gap; ++g)
                    [self hold];
            }
        }
        if (take) {
            self.gridSlot = slot;
            nv12_to_i420(y, ys, uv, us, self.w, self.h, self.i420);
            [self encode:self.i420 duplicate:NO];
        }
    }
    CVPixelBufferUnlockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
}

/* Hold the previous frame for one more slot. fdv_enc_repeat writes an all-SKIP
 * frame without re-running mode decision, so holding costs almost nothing even
 * at a low qp -- where letting the encoder rediscover SKIP costs nearly as much
 * as a fresh frame and puts the recording further behind for keeping time. */
- (void)hold {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = fdv_enc_repeat(self.enc);
    clock_gettime(CLOCK_MONOTONIC, &b);
    atomic_fetch_add(&self->encode_ns,
                     (long long)(b.tv_sec - a.tv_sec) * 1000000000LL
                     + (b.tv_nsec - a.tv_nsec));
    if (rc != 0) { atomic_fetch_add(&self->failed, 1); return; }
    atomic_fetch_add(&self->dupes, 1);
    atomic_fetch_add(&self->frames, 1);
}

/* One frame into the encoder, with its timing accounted for. */
- (void)encode:(const uint8_t *)i420 duplicate:(BOOL)dup {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = fdv_enc_frame(self.enc, i420);
    clock_gettime(CLOCK_MONOTONIC, &b);
    atomic_fetch_add(&self->encode_ns,
                     (long long)(b.tv_sec - a.tv_sec) * 1000000000LL
                     + (b.tv_nsec - a.tv_nsec));
    if (rc != 0) { atomic_fetch_add(&self->failed, 1); return; }
    if (dup) atomic_fetch_add(&self->dupes, 1);
    int n = atomic_fetch_add(&self->frames, 1) + 1;
    /* A live line while recording, on a terminal only -- a carriage return into
     * a pipe or a log file is just noise. */
    if (self.live && n % 10 == 0) {
        char rr[32], dd[32];
        double secs = n / (self.fps > 0 ? self.fps : 30);
        int held = atomic_load(&self->dupes);
        printf("\r  %s%-10s%s %-8s %5d frames   %-10s",
               P.dim, "recording", P.rst, fdv_dur(dd, sizeof dd, secs), n,
               fdv_rate(rr, sizeof rr, fdv_enc_bytes(self.enc) * 8.0 / secs));
        if (held) printf("  %s%d held%s", P.dim, held, P.rst);
        printf("   ");
        fflush(stdout);
    }
}

- (void)captureOutput:(AVCaptureOutput *)output
  didDropSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection {
    (void)output; (void)sample; (void)connection;
    atomic_fetch_add(&self->dropped, 1);
}
@end

/* ===========================================================================
 * 3. MAIN
 * Device selection, session setup, and the recording loop.
 * ======================================================================== */

static void usage(void) {
    fprintf(stderr,
        "fcap — record the camera to an .fdv file\n\n"
        "usage: fcap <out.fdv> [options]\n"
        "  -q QP        quantizer 0..51 (default 24)\n"
        "  -b RATE      target bitrate instead: 1200k, 2M (average, for streaming)\n"
        "  -k GOP       key frame every N frames, or Ns for seconds\n"
        "               (default 2s; 0 = only the first frame)\n"
        "  -t SECONDS   stop after this long (default: until Ctrl-C)\n"
        "  -n FRAMES    stop after this many frames\n"
        "  -d INDEX     capture device (default 0)\n"
        "  -s SIZE      capture size: 480p, 720p (default), 1080p\n"
        "  --fps N      frame rate to record at (default 30)\n"
        "  -j N         encode bands in parallel (default 4, 1 = off)\n"
        "  --list       list capture devices and exit\n"
        "  -v           per-frame progress\n\n"
        "The window of a recording is whatever the camera gives, cropped to a\n"
        "multiple of 16 in each direction — the codec's block grid needs that.\n\n"
        "Encoding is what usually limits a capture, so the frame is split into\n"
        "-j horizontal bands coded on separate cores. Bands cost a few percent\n"
        "of bitrate; -j 1 turns them off and writes a single-stream file.\n\n"
        "Frames are placed by their capture timestamp, so a frame the encoder\n"
        "was too slow to take is held rather than skipped and the motion keeps\n"
        "its real speed. If most frames end up held, record at a lower --fps.\n\n"
        "play it back with:  fpl out.fdv\n");
}

static NSArray<AVCaptureDevice *> *cameras(void) {
    AVCaptureDeviceDiscoverySession *ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera,
    /* Renamed in macOS 14; the old spelling is still what older systems know. */
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
                                          AVCaptureDeviceTypeExternal]
#else
                                          AVCaptureDeviceTypeExternalUnknown]
#endif
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];
    return ds.devices;
}

/* macOS gates the camera behind a consent prompt. A command-line tool inherits
 * the decision made for the terminal that launched it, so a refusal here is
 * something the user has to fix in System Settings -- say that rather than
 * failing with an empty device list. */
static BOOL ensure_permission(void) {
    AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (st == AVAuthorizationStatusAuthorized) return YES;
    if (st == AVAuthorizationStatusDenied || st == AVAuthorizationStatusRestricted) {
        fprintf(stderr, "camera access is denied for this terminal\n"
                        "  System Settings > Privacy & Security > Camera\n");
        return NO;
    }
    __block BOOL granted = NO;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL ok) {
        granted = ok;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!granted) fprintf(stderr, "camera access was not granted\n");
    return granted;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        int qp = 24, keyint = -1, devidx = 0, maxframes = 0, verbose = 0;
        const char *keyint_arg = NULL;
        long bitrate = 0;
        int want_fps = 30;
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        int want_j = ncpu >= 4 ? 4 : (ncpu > 0 ? (int)ncpu : 1);
        const char *preset = "720p";
        double seconds = 0.0;
        const char *out = NULL;

        for (int i = 1; i < argc; ++i) {
            int has = i + 1 < argc;
            if      (!strcmp(argv[i], "--list")) {
                if (!ensure_permission()) return 1;
                NSArray<AVCaptureDevice *> *ds = cameras();
                if (!ds.count) { printf("no capture devices\n"); return 1; }
                printf("capture devices:\n");
                for (NSUInteger k = 0; k < ds.count; ++k)
                    printf("  %lu  %s\n", (unsigned long)k,
                           ds[k].localizedName.UTF8String);
                return 0;
            }
            else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
            else if (!strcmp(argv[i], "-q") && has) qp = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-b") && has) {
                char *end = NULL; double v = strtod(argv[++i], &end);
                if (end && (*end == 'k' || *end == 'K')) v *= 1000.0;
                else if (end && (*end == 'm' || *end == 'M')) v *= 1000000.0;
                bitrate = (long)v;
            }
            else if (!strcmp(argv[i], "-k") && has) keyint_arg = argv[++i];
            else if (!strcmp(argv[i], "-t") && has) seconds = atof(argv[++i]);
            else if (!strcmp(argv[i], "-n") && has) maxframes = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-d") && has) devidx = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-s") && has) preset = argv[++i];
            else if (!strcmp(argv[i], "--fps") && has) want_fps = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-j") && has) want_j = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-v"))        verbose = 1;
            else if (argv[i][0] != '-' && !out)     out = argv[i];
            else { fprintf(stderr, "unknown option '%s'\n\n", argv[i]); usage(); return 1; }
        }
        if (!out) { usage(); return 1; }
        if (qp < 0 || qp > 51) { fprintf(stderr, "qp must be 0..51\n"); return 1; }
        if (want_fps < 1 || want_fps > 240) { fprintf(stderr, "--fps must be 1..240\n"); return 1; }
        if (want_j < 1 || want_j > 64) { fprintf(stderr, "-j must be 1..64\n"); return 1; }

        /* The key-frame interval is really a duration -- two seconds is the
         * streaming convention, and it is what bounds how long a viewer joining
         * mid-stream waits for a decodable frame. Written in frames it silently
         * halves when the frame rate doubles, so seconds are the default unit
         * and the frame count is available for anyone who wants it exactly. */
        if (keyint_arg) {
            char *end = NULL;
            double v = strtod(keyint_arg, &end);
            if (v < 0.0) { fprintf(stderr, "-k must not be negative\n"); return 1; }
            keyint = (end && (*end == 's' || *end == 'S'))
                   ? (int)lround(v * want_fps) : (int)v;
        } else {
            keyint = want_fps * 2;
        }
        if (keyint > 255) {
            fprintf(stderr, "note: key-frame interval capped at 255 frames "
                            "(%.1fs at %d fps)\n", 255.0 / want_fps, want_fps);
            keyint = 255;
        }
        if (!ensure_permission()) return 1;

        NSArray<AVCaptureDevice *> *devs = cameras();
        if (devidx < 0 || (NSUInteger)devidx >= devs.count) {
            fprintf(stderr, "no capture device %d (try: fcap --list)\n", devidx);
            return 1;
        }
        AVCaptureDevice *dev = devs[(NSUInteger)devidx];

        NSError *err = nil;
        AVCaptureDeviceInput *in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
        if (!in) {
            fprintf(stderr, "cannot open '%s': %s\n", dev.localizedName.UTF8String,
                    err.localizedDescription.UTF8String);
            return 1;
        }

        AVCaptureSession *session = [[AVCaptureSession alloc] init];
        /* 720p by default: at 1080p this encoder needs about 80 ms on real
         * camera content -- noisy, textured, everything moving -- so it would
         * cap the recording near 12 fps. Resolution is the knob that matters
         * most here, so it is exposed. */
        NSString *sp = AVCaptureSessionPreset1280x720;
        int want_w = 1280, want_h = 720;
        if      (!strcmp(preset, "480p"))  { sp = AVCaptureSessionPreset640x480;   want_w = 640;  want_h = 480; }
        else if (!strcmp(preset, "1080p")) { sp = AVCaptureSessionPreset1920x1080; want_w = 1920; want_h = 1080; }
        else if (strcmp(preset, "720p") != 0) {
            fprintf(stderr, "unknown size '%s' — use 480p, 720p or 1080p\n", preset);
            return 1;
        }
        if ([session canSetSessionPreset:sp]) session.sessionPreset = sp;
        else fprintf(stderr, "note: camera cannot do %s, using its default\n", preset);
        if (![session canAddInput:in]) { fprintf(stderr, "cannot add camera input\n"); return 1; }
        [session addInput:in];

        AVCaptureVideoDataOutput *vout = [[AVCaptureVideoDataOutput alloc] init];
        /* The size has to be asked for here as well as through the preset.
         * Setting videoSettings at all makes AVFoundation deliver the device's
         * native format and ignore the preset, so the two must agree. */
        vout.videoSettings = @{
            (id)kCVPixelBufferPixelFormatTypeKey:
                @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
            (id)kCVPixelBufferWidthKey:  @(want_w),
            (id)kCVPixelBufferHeightKey: @(want_h)
        };
        /* Drop rather than queue: if encoding falls behind the camera, a growing
         * backlog would drift further behind forever. A dropped frame is
         * reported and the recording stays in step with real time. */
        vout.alwaysDiscardsLateVideoFrames = YES;
        if (![session canAddOutput:vout]) { fprintf(stderr, "cannot add video output\n"); return 1; }
        [session addOutput:vout];

        /* Ask the camera for the grid rate too. Frames it never sends cost
         * nothing, where frames it sends and we discard cost a colour
         * conversion each. This is a request: a camera that cannot do the rate
         * keeps its own, and the grid above still sorts out the timing. */
        if ([dev lockForConfiguration:NULL]) {
            CMTime d = CMTimeMake(1, want_fps);
            BOOL ok = NO;
            for (AVFrameRateRange *r in dev.activeFormat.videoSupportedFrameRateRanges)
                if (want_fps >= r.minFrameRate - 0.5 && want_fps <= r.maxFrameRate + 0.5) ok = YES;
            if (ok) { dev.activeVideoMinFrameDuration = d; dev.activeVideoMaxFrameDuration = d; }
            else fprintf(stderr, "note: camera cannot run at %d fps, "
                                 "holding frames to reach it\n", want_fps);
            [dev unlockForConfiguration];
        }

        if (verbose) fdv_log_set(FDV_LOG_INFO, stderr);

        FdvSink *sink = [[FdvSink alloc] init];
        sink.qp = qp; sink.keyint = keyint;
        sink.maxframes = maxframes; sink.verbose = verbose;
        sink.fps = want_fps;
        sink.threads = want_j;
        sink.bitrate = bitrate;
        sink.live = isatty(fileno(stdout)) ? YES : NO;
        atomic_init(&sink->frames, 0);
        atomic_init(&sink->dropped, 0);
        atomic_init(&sink->failed, 0);
        atomic_init(&sink->dupes, 0);
        atomic_init(&sink->encode_ns, 0);

        /* A serial queue, so only one thread is ever inside the encoder. */
        dispatch_queue_t q = dispatch_queue_create("fdv.capture", DISPATCH_QUEUE_SERIAL);
        [vout setSampleBufferDelegate:sink queue:q];

        signal(SIGINT, on_sigint);
        char gop[64], rt[32];
        if (keyint > 0) snprintf(gop, sizeof gop, "key frame every %d frames (%.1f s)",
                                 keyint, (double)keyint / want_fps);
        else            snprintf(gop, sizeof gop, "one key frame only");
        ui_init();
        ui_head("record", dev.localizedName.UTF8String);
        ui_row("target", "%s @%d fps, %s", preset, want_fps, gop);
        if (bitrate > 0)
            ui_row("coding", "%s%s%s average", P.bold, fdv_rate(rt, sizeof rt, (double)bitrate), P.rst);
        else
            ui_row("coding", "qp %d %s(fixed -- bitrate will follow the scene)%s", qp, P.dim, P.rst);
        ui_row("output", "%s", out);
        printf("  %s%-10s%s %sCtrl-C to stop%s\n", P.dim, "", P.rst, P.dim, P.rst);
        fflush(stdout);

        [session startRunning];
        NSDate *start = [NSDate date];
        while (!g_stop) {
            [[NSRunLoop currentRunLoop] runUntilDate:
                [NSDate dateWithTimeIntervalSinceNow:0.05]];
            double elapsed = -[start timeIntervalSinceNow];
            if (seconds > 0.0 && elapsed >= seconds) break;
            if (maxframes > 0 && atomic_load(&sink->frames) >= maxframes) break;
        }
        [session stopRunning];
        /* Let anything already in the queue finish before the encoder is read. */
        dispatch_sync(q, ^{});

        double secs = -[start timeIntervalSinceNow];
        int n = atomic_load(&sink->frames);
        int dropped = atomic_load(&sink->dropped);
        int failed = atomic_load(&sink->failed);
        double encode_ms = atomic_load(&sink->encode_ns) / 1e6;
        if (verbose) fprintf(stderr, "\n");

        if (n == 0 || !sink.enc) {
            fprintf(stderr, "no frames captured\n");
            fdv_enc_close(sink.enc); free(sink.i420);
            return 1;
        }
        int w = sink.w, h = sink.h;

        /* The grid rate is the true rate now: every frame in the file occupies
         * exactly one slot of it, held frames included. */
        int fps = want_fps;
        int dupes = atomic_load(&sink->dupes);

        size_t len = 0;
        uint8_t *file = fdv_enc_finish(sink.enc, fps, &len);
        free(sink.i420);
        if (!file) { fprintf(stderr, "encode failed\n"); return 1; }

        FILE *f = fopen(out, "wb");
        if (!f || fwrite(file, 1, len, f) != len) {
            fprintf(stderr, "cannot write '%s'\n", out);
            if (f) fclose(f);
            free(file);
            return 1;
        }
        fclose(f);

        if (sink.live) { printf("\r%*s\r", 70, ""); fflush(stdout); }
        double bps = (double)len * 8 / (secs > 0.0 ? secs : 1.0);
        double raw = (double)w * h * 3 / 2 * 8 * fps;
        double live = (double)(n - dupes) / (secs > 0.0 ? secs : 1.0);
        char ra[32], rb[32], dd[32];
        printf("\n");
        ui_head("recorded", out);
        ui_row("video", "%dx%d, %d frames @%d fps  (%s)", w, h, n, fps,
               fdv_dur(dd, sizeof dd, secs));
        ui_row("stream", "%s%s%s   %s(%.0fx under raw %s)%s",
               P.bold, fdv_rate(ra, sizeof ra, bps), P.rst,
               P.dim, raw / (bps > 0 ? bps : 1), fdv_rate(rb, sizeof rb, raw), P.rst);
        if (bitrate > 0) {
            double miss = 100.0 * (bps - (double)bitrate) / (double)bitrate;
            const char *col = (miss > 10.0 || miss < -10.0) ? P.yel : P.grn;
            ui_row("target", "%s @ %s%+.1f%%%s", fdv_rate(rb, sizeof rb, (double)bitrate),
                   col, miss, P.rst);
        }
        if (sink.threads > 1)
            ui_row("encode", "%.1f ms/frame  %sacross %d bands%s",
                   encode_ms / n, P.dim, sink.threads, P.rst);
        else
            ui_row("encode", "%.1f ms/frame", encode_ms / n);
        if (dupes)
            ui_row("held", "%d of %d frames (%.0f%%)  %s-- %.0f fps of new pictures%s",
                   dupes, n, 100.0 * dupes / n, P.dim, live, P.rst);
        if (dropped || failed)
            ui_row("lost", "%s%d dropped, %d failed%s", P.yel, dropped, failed, P.rst);
        /* If encoding took most of the wall clock, the encoder set the rate --
         * worth saying plainly, because the fix is a smaller -s or a higher -q,
         * not anything about the camera. */
        if (encode_ms > secs * 1000.0 * 0.8)
            ui_row("note", "%sthe encoder was the limit (%.0f%% of wall clock)%s -- "
                   "try -s 480p, a higher -q, %s--fps %d",
                   P.yel, 100.0 * encode_ms / (secs * 1000.0), P.rst,
                   sink.threads < 8 ? "a larger -j, or " : "or ",
                   fps / 2 > 0 ? fps / 2 : 1);
        ui_row("play", "fpl %s", out);
        free(file);
        return 0;
    }
}
