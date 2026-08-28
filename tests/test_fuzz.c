/* Decoder robustness fuzz: feed truncations and byte-flipped copies of a valid
 * bitstream to fdv_video_decode/fdv_image_decode and assert they never crash or
 * over-read — they must return cleanly (0 or nonzero) for every input.
 *
 * The container header (dims/qp) is kept intact so the caller's output buffer
 * stays correctly sized per the contract; corruption targets the body, which is
 * exactly what the bounded symbol-stream parse must survive. A few header bytes
 * are also exercised separately to confirm the dimension sanity caps reject
 * absurd sizes rather than writing past the caller's buffer.
 *
 * Run under the normal build (a crash = test failure) and under `make fuzz`
 * (-fsanitize=address) where any out-of-bounds access aborts even without a
 * visible crash. */

#define FDV_IMPLEMENTATION
#include "fdv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* Deterministic xorshift — no Date/random in this environment, and a fixed seed
 * makes any failure reproducible. */
static uint32_t rng = 0x9e3779b9u;
static uint32_t nr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void) {
    /* --- Video stream --- */
    /* Eight frames at keyint 4, not four at keyint 2, so the corpus contains
     * every entropy mode. The adaptive coder needs two P-frames since the last
     * key frame before it has any history to prime from, so a stream that key-
     * frames every other frame never reaches emode 3 and leaves that decoder
     * path -- the one that walks untrusted input hardest -- unfuzzed. Asserted
     * below rather than assumed. */
    int w = 48, h = 32, nf = 8, qp = 22, keyint = 4;
    size_t fsize = (size_t)w * h * 3 / 2;
    uint8_t *seq = malloc(fsize * nf);
    CHECK(seq);
    for (int f = 0; f < nf; ++f)
        for (size_t i = 0; i < fsize; ++i)
            seq[f * fsize + i] = (uint8_t)((i * 7 + f * 31 + (i >> 4) * 13) ^ (f << 2));
    const uint8_t *fr[8];
    for (int f = 0; f < nf; ++f) fr[f] = seq + (size_t)f * fsize;

    size_t cap = fsize * nf * 2 + 65536;
    uint8_t *bs = malloc(cap);
    CHECK(bs);
    size_t blen = fdv_video_encode(fr, nf, w, h, qp, keyint, bs, cap);
    CHECK(blen > 8);

    uint8_t *vout = malloc(fsize * nf);
    CHECK(vout);
    int dnf, dw, dh;

    /* Valid stream decodes cleanly. */
    CHECK(fdv_video_decode(bs, blen, vout, &dnf, &dw, &dh) == 0);
    CHECK(dnf == nf && dw == w && dh == h);

    /* Every entropy mode is actually present, so the flips below reach all of
     * them. Frame layout is a 4-byte length then the blob, whose second byte is
     * the mode; key frames do not carry one. */
    {   int seen[4] = {0,0,0,0};
        size_t q = 8;                       /* nframes, w, h, qp, keyint */
        for (int f = 0; f < nf && q + 4 <= blen; ++f) {
            uint32_t n = fdv_get32(bs, &q);
            if (q + n > blen) break;
            int is_key = (f % keyint) == 0;
            if (!is_key && n >= 2 && bs[q+1] < 4) seen[bs[q+1]] = 1;
            q += n;
        }
        CHECK(seen[3]);                     /* the adaptive path is in here */
    }

    /* (a) Truncations at every length, including mid-header. Each is decoded
     *     from an EXACT-size buffer so any read past `len` is a real overflow
     *     ASan will flag. Anything that loses the header must return -1; the
     *     rest must not over-read. The header stays intact for t>=8, so vout is
     *     always correctly sized. */
    uint8_t *cp = malloc(blen);
    CHECK(cp);
    for (size_t t = 0; t <= blen; ++t) {
        memcpy(cp, bs, t);
        fdv_video_decode(cp, t, vout, &dnf, &dw, &dh);   /* must not crash/over-read */
    }

    /* (b) Byte-flipped copies of the body (header intact: bytes [8, blen) so
     *     the decoded dims — and thus the needed vout size — never change). */
    for (int it = 0; it < 40000; ++it) {
        memcpy(cp, bs, blen);
        int flips = 1 + (int)(nr() % 6);
        for (int k = 0; k < flips; ++k) {
            size_t pos = 8 + nr() % (blen - 8);
            cp[pos] ^= (uint8_t)(1u << (nr() & 7));
        }
        fdv_video_decode(cp, blen, vout, &dnf, &dw, &dh);
    }
    free(cp);

    /* Dimension sanity caps: a header declaring out-of-range dims must be
     * rejected outright (the decoder writes nothing past the caller buffer).
     * The caller's contract is to size `out` per the header, so we verify the
     * decoder's own guard rather than fuzzing dims against a fixed buffer. */
    {
        uint8_t hdr[8]; memcpy(hdr, bs, 8);
        uint8_t bad[8];
        memcpy(bad, hdr, 8); bad[2] = 0xff; bad[3] = 0xff;     /* w = 65535 */
        CHECK(fdv_video_decode(bad, blen, vout, &dnf, &dw, &dh) != 0);
        memcpy(bad, hdr, 8); bad[4] = 0xff; bad[5] = 0xff;     /* h = 65535 */
        CHECK(fdv_video_decode(bad, blen, vout, &dnf, &dw, &dh) != 0);
        memcpy(bad, hdr, 8); bad[2] = 1;                        /* w not mult of 16 */
        CHECK(fdv_video_decode(bad, blen, vout, &dnf, &dw, &dh) != 0);
        CHECK(fdv_video_decode(bs, 4, vout, &dnf, &dw, &dh) != 0);  /* header truncated */
    }

    /* --- Image stream (intra codec) --- */
    int iw = 64, ih = 48;
    uint8_t *img = malloc((size_t)iw * ih);
    CHECK(img);
    for (int i = 0; i < iw * ih; ++i) img[i] = (uint8_t)((i * 5 + (i / iw) * 11) & 0xff);
    size_t icap = (size_t)iw * ih * 2 + 4096;
    uint8_t *ibs = malloc(icap);
    CHECK(ibs);
    size_t ilen = fdv_image_encode(img, iw, ih, iw, 18, ibs, icap, NULL);
    CHECK(ilen > 5);

    uint8_t *iout = malloc((size_t)iw * ih);
    CHECK(iout);
    int iwo, iho;
    CHECK(fdv_image_decode(ibs, ilen, iout, iw, &iwo, &iho) == 0);
    CHECK(iwo == iw && iho == ih);
    /* The intra path picks its entropy mode per frame; byte 5 records which.
     * Mode 2 is the adaptive coder, whose decoder walks this buffer hardest.
     * Asserted so the flips below cannot quietly stop reaching it. */
    CHECK(ibs[5] == 2);

    uint8_t *icp = malloc(ilen);
    CHECK(icp);
    for (size_t t = 0; t <= ilen; ++t) {
        memcpy(icp, ibs, t);
        fdv_image_decode(icp, t, iout, iw, &iwo, &iho);
    }

    for (int it = 0; it < 40000; ++it) {
        memcpy(icp, ibs, ilen);
        int flips = 1 + (int)(nr() % 6);
        for (int k = 0; k < flips; ++k) {
            size_t pos = 5 + nr() % (ilen - 5);        /* header is 5 bytes (w,h,qp) */
            icp[pos] ^= (uint8_t)(1u << (nr() & 7));
        }
        fdv_image_decode(icp, ilen, iout, iw, &iwo, &iho);
    }
    /* Image dimension caps: out-of-range dims rejected before any write. */
    {
        uint8_t bad[5]; memcpy(bad, ibs, 5);
        bad[0] = 0xff; bad[1] = 0xff;                          /* w = 65535 */
        CHECK(fdv_image_decode(bad, ilen, iout, iw, &iwo, &iho) != 0);
        memcpy(bad, ibs, 5); bad[0] = 1;                        /* w not mult of 4 */
        CHECK(fdv_image_decode(bad, ilen, iout, iw, &iwo, &iho) != 0);
        CHECK(fdv_image_decode(ibs, 3, iout, iw, &iwo, &iho) != 0); /* header truncated */
    }
    free(icp);

    /* --- Tiled video container (vtile, threaded decode) --- */
    /* band_mbrows=1 → multiple 16-row bands, so threaded decode is exercised. */
    uint8_t *vtbs = malloc(cap);
    CHECK(vtbs);
    size_t vtlen = fdv_vtile_encode(fr, nf, w, h, qp, keyint, 1, 1, vtbs, cap);
    CHECK(vtlen > 12);
    CHECK(fdv_vtile_decode(vtbs, vtlen, vout, &dnf, &dw, &dh, 2) == 0);
    CHECK(dnf == nf && dw == w && dh == h);

    uint8_t *vtcp = malloc(vtlen);
    CHECK(vtcp);
    for (size_t t = 0; t <= vtlen; ++t) {
        memcpy(vtcp, vtbs, t);                       /* exact-size: ASan-meaningful */
        fdv_vtile_decode(vtcp, t, vout, &dnf, &dw, &dh, 2);
    }
    for (int it = 0; it < 40000; ++it) {
        memcpy(vtcp, vtbs, vtlen);
        int flips = 1 + (int)(nr() % 6);
        for (int k = 0; k < flips; ++k) {
            size_t pos = 12 + nr() % (vtlen - 12);   /* header (12 B) intact → vout sized right */
            vtcp[pos] ^= (uint8_t)(1u << (nr() & 7));
        }
        fdv_vtile_decode(vtcp, vtlen, vout, &dnf, &dw, &dh, 2);
    }
    /* Tiled-header dimension caps: absurd dims rejected before any write. */
    {
        uint8_t bad[12]; memcpy(bad, vtbs, 12);
        bad[0] = 0xff; bad[1] = 0xff;                          /* w = 65535 */
        CHECK(fdv_vtile_decode(bad, vtlen, vout, &dnf, &dw, &dh, 2) != 0);
        memcpy(bad, vtbs, 12); bad[2] = 0xff; bad[3] = 0xff;   /* h = 65535 */
        CHECK(fdv_vtile_decode(bad, vtlen, vout, &dnf, &dw, &dh, 2) != 0);
        CHECK(fdv_vtile_decode(vtbs, 6, vout, &dnf, &dw, &dh, 2) != 0); /* header truncated */
    }
    free(vtcp);

    free(seq); free(bs); free(vout);
    free(img); free(ibs); free(iout); free(vtbs);
    printf("fuzz: video (%zu-byte, all truncations + 40000 flips + header), "
           "image (%zu-byte, all truncations + 40000 flips + header), and "
           "vtile (%zu-byte tiled, all truncations + 40000 flips + header, threaded) survived\n",
           blen, ilen, vtlen);
    return 0;
}
