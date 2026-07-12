/* sha256sum.c — sha256sum builtin: compute and verify SHA-256 checksums */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * SHA-256 implementation (~150 lines, self-contained)
 * ---------------------------------------------------------------------------*/

typedef struct {
    unsigned int  state[8];
    unsigned long count[2];  /* bit count, low/high */
    unsigned char buf[64];
} sha256_ctx_t;

static const unsigned int K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIG0(x) (ROR32(x, 2)  ^ ROR32(x, 13) ^ ROR32(x, 22))
#define SIG1(x) (ROR32(x, 6)  ^ ROR32(x, 11) ^ ROR32(x, 25))
#define sig0(x) (ROR32(x, 7)  ^ ROR32(x, 18) ^ ((x) >> 3))
#define sig1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx_t *ctx, const unsigned char *data)
{
    unsigned int W[64], a, b, c, d, e, f, g, h, T1, T2;

    for (int i = 0; i < 16; i++) {
        W[i] = ((unsigned int)data[i*4  ] << 24) |
               ((unsigned int)data[i*4+1] << 16) |
               ((unsigned int)data[i*4+2] <<  8) |
               ((unsigned int)data[i*4+3]);
    }
    for (int i = 16; i < 64; i++)
        W[i] = sig1(W[i-2]) + W[i-7] + sig0(W[i-15]) + W[i-16];

    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        T1 = h + SIG1(e) + CH(e,f,g) + K[i] + W[i];
        T2 = SIG0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+T1;
        d=c; c=b; b=a; a=T1+T2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count[0] = ctx->count[1] = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const unsigned char *data, size_t len)
{
    size_t i    = 0;
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    ctx->count[0] += (unsigned long)(len << 3);
    if (ctx->count[0] < (unsigned long)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (unsigned long)(len >> 29);

    size_t partlen = 64 - index;
    if (len >= partlen) {
        memcpy(ctx->buf + index, data, partlen);
        sha256_transform(ctx, ctx->buf);
        for (i = partlen; i + 63 < len; i += 64)
            sha256_transform(ctx, data + i);
        index = 0;
    }
    memcpy(ctx->buf + index, data + i, len - i);
}

static void sha256_final(sha256_ctx_t *ctx, unsigned char digest[32])
{
    unsigned char bits[8];
    for (int i = 0; i < 4; i++) {
        bits[i]   = (unsigned char)((ctx->count[1] >> (24 - i*8)) & 0xff);
        bits[i+4] = (unsigned char)((ctx->count[0] >> (24 - i*8)) & 0xff);
    }
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    size_t padlen = (index < 56) ? (56 - index) : (120 - index);
    unsigned char padding[128] = { 0x80, 0 };
    sha256_update(ctx, padding, padlen);
    sha256_update(ctx, bits, 8);
    for (int i = 0; i < 8; i++) {
        digest[i*4  ] = (unsigned char)((ctx->state[i] >> 24) & 0xff);
        digest[i*4+1] = (unsigned char)((ctx->state[i] >> 16) & 0xff);
        digest[i*4+2] = (unsigned char)((ctx->state[i] >>  8) & 0xff);
        digest[i*4+3] = (unsigned char)( ctx->state[i]        & 0xff);
    }
}

/* ---------------------------------------------------------------------------
 * Hash a file and print "HASH  FILENAME"
 * ---------------------------------------------------------------------------*/
#define HASH_BUFSZ 65536

static int hash_file(FILE *fp, unsigned char digest[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    unsigned char *buf = malloc(HASH_BUFSZ);
    if (!buf) return -1;
    size_t n;
    while ((n = fread(buf, 1, HASH_BUFSZ, fp)) > 0)
        sha256_update(&ctx, buf, n);
    free(buf);
    if (ferror(fp)) return -1;
    sha256_final(&ctx, digest);
    return 0;
}

static void print_digest(const unsigned char digest[32], const char *name)
{
    for (int i = 0; i < 32; i++)
        printf("%02x", digest[i]);
    printf("  %s\n", name);
}

static int check_file(const char *checkfile, int opt_status, int opt_warn)
{
    FILE *fp = fopen(checkfile, "r");
    if (!fp) {
        err_msg("sha256sum", "cannot open '%s'", checkfile);
        return 1;
    }

    int ok = 0, fail = 0, bad_format = 0;
    char line[PATH_MAX + 80];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
            line[--llen] = '\0';
        if (llen == 0 || line[0] == '#') continue;

        /* Expect: 64-hex-chars '  ' filename */
        if (llen < 66 || line[64] != ' ' || line[65] != ' ') {
            bad_format++;
            continue;
        }
        char expected_hex[65];
        memcpy(expected_hex, line, 64);
        expected_hex[64] = '\0';
        const char *fname = line + 66;

        FILE *f2 = fopen(fname, "rb");
        if (!f2) {
            if (!opt_status) err_msg("sha256sum", "%s: FAILED open or read", fname);
            fail++;
            continue;
        }
        unsigned char digest[32];
        if (hash_file(f2, digest) < 0) {
            fclose(f2);
            if (!opt_status) err_msg("sha256sum", "%s: FAILED open or read", fname);
            fail++;
            continue;
        }
        fclose(f2);

        /* Compare */
        char got_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(got_hex + i*2, 3, "%02x", digest[i]);
        got_hex[64] = '\0';

        if (strcmp(expected_hex, got_hex) == 0) {
            if (!opt_status) printf("%s: OK\n", fname);
            ok++;
        } else {
            if (!opt_status) printf("%s: FAILED\n", fname);
            fail++;
        }
    }
    fclose(fp);

    if (opt_warn && bad_format > 0)
        err_msg("sha256sum", "WARNING: %d line(s) improperly formatted", bad_format);

    return (fail > 0 || (bad_format > 0 && !ok)) ? 1 : 0;
}

int applet_sha256sum(int argc, char **argv)
{
    int opt_c      = 0;   /* -c: check mode */
    int opt_status = 0;   /* --status: no output in check mode */
    int opt_warn   = 1;   /* --warn / --quiet */
    int opt_b      = 0;   /* -b: binary mode (ignore on Linux) */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        if (strcmp(arg, "--check") == 0)  { opt_c = 1; continue; }
        if (strcmp(arg, "--status") == 0) { opt_status = 1; continue; }
        if (strcmp(arg, "--quiet") == 0)  { opt_warn = 0; continue; }
        if (strcmp(arg, "--warn") == 0)   { opt_warn = 1; continue; }

        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'c': opt_c = 1; break;
            case 'b': opt_b = 1; break;
            case 't': opt_b = 0; break;
            default:
                err_msg("sha256sum", "unrecognized option '-%c'", *p);
                err_usage("sha256sum", "[-c] [FILE]...");
                return 1;
            }
            p++;
        }
    }
    (void)opt_b;

    /* Check mode */
    if (opt_c) {
        if (i >= argc) {
            /* Read from stdin */
            err_usage("sha256sum", "-c CHECKFILE");
            return 1;
        }
        int ret = 0;
        for (; i < argc; i++)
            if (check_file(argv[i], opt_status, opt_warn)) ret = 1;
        return ret;
    }

    /* Hash mode */
    int ret = 0;
    if (i >= argc) {
        /* stdin */
        unsigned char digest[32];
        if (hash_file(stdin, digest) < 0) {
            err_msg("sha256sum", "read error");
            return 1;
        }
        print_digest(digest, "-");
        return 0;
    }

    for (; i < argc; i++) {
        FILE *fp;
        if (strcmp(argv[i], "-") == 0) {
            fp = stdin;
        } else {
            fp = fopen(argv[i], "rb");
            if (!fp) {
                err_msg("sha256sum", "cannot open '%s'", argv[i]);
                ret = 1;
                continue;
            }
        }
        unsigned char digest[32];
        if (hash_file(fp, digest) < 0) {
            err_msg("sha256sum", "read error: '%s'", argv[i]);
            ret = 1;
        } else {
            print_digest(digest, argv[i]);
        }
        if (fp != stdin) fclose(fp);
    }

    return ret;
}
