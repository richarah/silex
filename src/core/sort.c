/* sort.c — sort builtin: sort lines of text files */

/* _GNU_SOURCE enables O_TMPFILE in <fcntl.h> (glibc) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- options --------------------------------------------------------------- */

typedef struct {
    int ignore_blanks;    /* -b */
    int dict_order;       /* -d */
    int fold_lower;       /* -f */
    int ignore_nonprint;  /* -i */
    int numeric;          /* -n */
    int general_numeric;  /* -g */
    int human_numeric;    /* -h */
    int version_sort;     /* -V */
    int reverse;          /* -r */
    int unique;           /* -u */
    int zero_term;        /* -z */
    char field_sep;       /* -t SEP, '\0' = whitespace */
    int have_sep;         /* 1 if -t was given */
    const char *outfile;  /* -o FILE */
} sort_opts_t;

/* ---- key definition ------------------------------------------------------- */

/* Per-key option overrides */
typedef struct {
    int field;       /* 1-based field index */
    int char_off;    /* 0-based char offset within field (0 = start) */
    /* flags overriding global */
    int b, d, f, i, n, g, h, V, r; /* -1 = not set, 0/1 = explicit */
} key_end_t;

typedef struct {
    key_end_t start;
    key_end_t end;   /* field 0 = "to end of line" */
    int has_end;
} key_def_t;

#define MAX_KEYS 64

static sort_opts_t g_opts;
static key_def_t   g_keys[MAX_KEYS];
static int         g_nkeys = 0;

/* ---- line storage ---------------------------------------------------------- */

typedef struct {
    char  *line;    /* NUL-terminated line text (includes \n or \0 term) */
    size_t len;     /* length not counting the terminator */
    size_t idx;     /* original index for stable sort */
    int    from_mmap; /* 1 if line points into a mmap'd region (do not free) */
} line_t;

/* mmap region tracking for cleanup */
#define MAX_MMAP_REGIONS 64
typedef struct { void *base; size_t size; } mmap_region_t;
static mmap_region_t g_mmaps[MAX_MMAP_REGIONS];
static int g_n_mmaps = 0;

/* ---- field splitting ------------------------------------------------------ */

/*
 * Find the start offset of field F (1-based) in line.
 * If have_sep: split on exact separator char.
 * Else:        split on runs of whitespace (and skip leading whitespace for f1).
 * Returns pointer to start of field, or end of string if not enough fields.
 */
static const char *field_start(const char *line, int field,
                                char sep, int have_sep)
{
    if (field <= 0) return line;

    const char *p = line;
    int cur = 1;

    if (!have_sep) {
        /* Skip leading whitespace for field 1 */
        while (*p && isspace((unsigned char)*p)) p++;
        if (cur == field) return p;
        /* Skip field 1 content */
        while (*p && !isspace((unsigned char)*p)) p++;
        cur++;
        while (cur <= field) {
            /* Skip inter-field whitespace */
            while (*p && isspace((unsigned char)*p)) p++;
            if (cur == field) return p;
            /* Skip field content */
            while (*p && !isspace((unsigned char)*p)) p++;
            cur++;
        }
        return p; /* past end */
    } else {
        /* Exact separator */
        while (*p) {
            if (cur == field) return p;
            if (*p == sep) cur++;
            p++;
        }
        /* If we ran off end but cur == field, field is empty at end */
        if (cur == field) return p;
        return p;
    }
}

/*
 * Find the end (exclusive) of field F (1-based) in line.
 * Points to separator or end-of-line/string.
 */
static const char *field_end(const char *start, char sep, int have_sep)
{
    const char *p = start;
    if (!have_sep) {
        while (*p && !isspace((unsigned char)*p)) p++;
    } else {
        while (*p && *p != sep) p++;
    }
    return p;
}

/*
 * Extract sort key from line into buf (at most bufsz-1 chars + NUL).
 * Returns number of chars placed (not counting NUL).
 */
static size_t extract_key(const char *line, const key_def_t *kd,
                           char sep, int have_sep,
                           char *buf, size_t bufsz)
{
    if (bufsz == 0) return 0;

    /* Start position */
    const char *fs = field_start(line, kd->start.field, sep, have_sep);
    /* Apply char offset */
    for (int c = 0; c < kd->start.char_off && *fs; c++) fs++;

    /* End position */
    const char *fe;
    if (!kd->has_end || kd->end.field == 0) {
        /* to end of line */
        fe = fs + strlen(fs);
        /* strip trailing newline */
        while (fe > fs && (*(fe-1) == '\n' || *(fe-1) == '\0'))
            fe--;
    } else {
        fe = field_start(line, kd->end.field, sep, have_sep);
        if (kd->end.char_off == 0) {
            /* end is end of specified field */
            fe = field_end(fe, sep, have_sep);
        } else {
            for (int c = 0; c < kd->end.char_off && *fe; c++) fe++;
        }
    }

    size_t len = (size_t)(fe - fs);
    if (len > bufsz - 1) len = bufsz - 1;
    memcpy(buf, fs, len);
    buf[len] = '\0';
    return len;
}

/* ---- comparison helpers --------------------------------------------------- */

/* Compare two strings as general floating-point numbers */
static int cmp_general_numeric(const char *a, const char *b)
{
    char *ea, *eb;
    double da = strtod(a, &ea);
    double db = strtod(b, &eb);
    int a_valid = (ea != a);
    int b_valid = (eb != b);
    if (!a_valid && !b_valid) return 0;
    if (!a_valid) return -1;
    if (!b_valid) return  1;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

/* Parse a human-readable size like "1K", "2.5M", "3G" etc. as a double */
static double parse_human(const char *s)
{
    if (!s || !*s) return 0.0;
    char *end;
    double v = strtod(s, &end);
    if (end == s) return 0.0;
    switch (*end) {
    case 'K': case 'k': v *= 1024.0;               break;
    case 'M':           v *= 1024.0 * 1024.0;       break;
    case 'G':           v *= 1024.0 * 1024.0 * 1024.0; break;
    case 'T':           v *= 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
    case 'P':           v *= 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
    default: break;
    }
    return v;
}

static int cmp_human_numeric(const char *a, const char *b)
{
    double da = parse_human(a);
    double db = parse_human(b);
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

/*
 * Version sort: tokenise into alternating numeric/non-numeric segments
 * and compare segment-by-segment.
 */
static int cmp_version(const char *a, const char *b)
{
    while (*a || *b) {
        /* Non-numeric segment */
        size_t an = 0, bn = 0;
        while (a[an] && !isdigit((unsigned char)a[an])) an++;
        while (b[bn] && !isdigit((unsigned char)b[bn])) bn++;
        size_t minnn = (an < bn) ? an : bn;
        int rc = strncmp(a, b, minnn);
        if (rc != 0) return rc;
        if (an != bn) return (an < bn) ? -1 : 1;
        a += an; b += bn;

        /* Numeric segment */
        size_t ad = 0, bd = 0;
        while (isdigit((unsigned char)a[ad])) ad++;
        while (isdigit((unsigned char)b[bd])) bd++;
        if (ad == 0 && bd == 0) break;
        /* Compare numerically by length first (skip leading zeros) */
        const char *ap = a, *bp = b;
        while (*ap == '0' && ad > 1) { ap++; ad--; }
        while (*bp == '0' && bd > 1) { bp++; bd--; }
        if (ad != bd) return (ad < bd) ? -1 : 1;
        rc = strncmp(ap, bp, ad);
        if (rc != 0) return rc;
        a += (size_t)(ap - a) + ad;
        b += (size_t)(bp - b) + bd;
    }
    return 0;
}

/*
 * Apply key-level filter flags to a string copy (blanks, fold, dict, nonprint).
 * Writes result into buf (bufsz bytes).
 * Returns pointer into buf.
 */
static const char *apply_key_flags(const char *s, int kb, int kd, int kf, int ki,
                                    char *buf, size_t bufsz)
{
    if (!kb && !kd && !kf && !ki) return s;

    const char *p = s;
    if (kb) while (*p && isspace((unsigned char)*p)) p++;

    size_t out = 0;
    while (*p && out < bufsz - 1) {
        char c = *p;
        if (kd && !isalnum((unsigned char)c) && !isspace((unsigned char)c) && c != '\0') {
            p++;
            continue;
        }
        if (ki && !isprint((unsigned char)c)) {
            p++;
            continue;
        }
        if (kf) c = (char)tolower((unsigned char)c);
        buf[out++] = c;
        p++;
    }
    buf[out] = '\0';
    return buf;
}

/* ---- global compare function (used by qsort wrapper) ---------------------- */

static const line_t *g_lines_ptr;   /* set before qsort call */

static int key_compare_strings(const char *a, const char *b,
                                int kn, int kg, int kh, int kV,
                                int kb, int kd, int kf, int ki, int kr)
{
    char abuf[4096], bbuf[4096];
    const char *as = apply_key_flags(a, kb, kd, kf, ki, abuf, sizeof(abuf));
    const char *bs = apply_key_flags(b, kb, kd, kf, ki, bbuf, sizeof(bbuf));

    int rc;
    if (kn) {
        /* Numeric: use strtoll */
        char *ea, *eb;
        errno = 0;
        long long ia = strtoll(as, &ea, 10);
        long long ib = strtoll(bs, &eb, 10);
        int av = (ea != as);
        int bv = (eb != bs);
        if (!av && !bv) rc = 0;
        else if (!av) rc = -1;
        else if (!bv) rc =  1;
        else rc = (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
    } else if (kg) {
        rc = cmp_general_numeric(as, bs);
    } else if (kh) {
        rc = cmp_human_numeric(as, bs);
    } else if (kV) {
        rc = cmp_version(as, bs);
    } else {
        rc = strcmp(as, bs);
    }

    return kr ? -rc : rc;
}

static int compare_lines(const line_t *la, const line_t *lb)
{
    char ka[4096], kb_buf[4096];

    if (g_nkeys == 0) {
        /* Whole-line comparison */
        int bn = g_opts.numeric;
        int bg = g_opts.general_numeric;
        int bh = g_opts.human_numeric;
        int bV = g_opts.version_sort;
        int bb = g_opts.ignore_blanks;
        int bd = g_opts.dict_order;
        int bf = g_opts.fold_lower;
        int bi = g_opts.ignore_nonprint;

        const char *as = la->line;
        const char *bs = lb->line;
        /* Strip trailing newline for comparison */
        size_t al = la->len, bl = lb->len;
        while (al > 0 && (as[al-1] == '\n' || as[al-1] == '\r')) al--;
        while (bl > 0 && (bs[bl-1] == '\n' || bs[bl-1] == '\r')) bl--;

        /* Temporary copies for whole-line compare */
        if (al >= sizeof(ka)) al = sizeof(ka) - 1;
        if (bl >= sizeof(kb_buf)) bl = sizeof(kb_buf) - 1;
        memcpy(ka, as, al); ka[al] = '\0';
        memcpy(kb_buf, bs, bl); kb_buf[bl] = '\0';

        int rc = key_compare_strings(ka, kb_buf, bn, bg, bh, bV, bb, bd, bf, bi, 0);
        if (rc != 0) return g_opts.reverse ? -rc : rc;
        /* Stable: use original index */
        return (la->idx < lb->idx) ? -1 : (la->idx > lb->idx) ? 1 : 0;
    }

    for (int k = 0; k < g_nkeys; k++) {
        const key_def_t *kd_ptr = &g_keys[k];
        char sep = g_opts.field_sep;
        int hsep = g_opts.have_sep;

        size_t kal = extract_key(la->line, kd_ptr, sep, hsep, ka, sizeof(ka));
        size_t kbl = extract_key(lb->line, kd_ptr, sep, hsep, kb_buf, sizeof(kb_buf));
        (void)kal; (void)kbl;

        /* Resolve per-key flags (fallback to global) */
        int kn = kd_ptr->start.n  >= 0 ? kd_ptr->start.n  : g_opts.numeric;
        int kg = kd_ptr->start.g  >= 0 ? kd_ptr->start.g  : g_opts.general_numeric;
        int kh = kd_ptr->start.h  >= 0 ? kd_ptr->start.h  : g_opts.human_numeric;
        int kV = kd_ptr->start.V  >= 0 ? kd_ptr->start.V  : g_opts.version_sort;
        int kb_f = kd_ptr->start.b  >= 0 ? kd_ptr->start.b : g_opts.ignore_blanks;
        int kd_f = kd_ptr->start.d  >= 0 ? kd_ptr->start.d : g_opts.dict_order;
        int kf_f = kd_ptr->start.f  >= 0 ? kd_ptr->start.f : g_opts.fold_lower;
        int ki   = kd_ptr->start.i  >= 0 ? kd_ptr->start.i : g_opts.ignore_nonprint;
        int kr   = kd_ptr->start.r  >= 0 ? kd_ptr->start.r : 0; /* per-key reverse */

        int rc = key_compare_strings(ka, kb_buf, kn, kg, kh, kV, kb_f, kd_f, kf_f, ki, kr);
        if (rc != 0) return g_opts.reverse ? -rc : rc;
    }

    /* All keys equal: stable sort by original index */
    return (la->idx < lb->idx) ? -1 : (la->idx > lb->idx) ? 1 : 0;
}

static int qsort_cmp(const void *pa, const void *pb)
{
    const line_t *la = (const line_t *)pa;
    const line_t *lb = (const line_t *)pb;
    return compare_lines(la, lb);
}

/* ---- key definition parser ------------------------------------------------ */

/*
 * Parse a KEYDEF string: F[.C][opts][,F[.C][opts]]
 * F is 1-based field number, C is 1-based char offset (0 = unset = start/end of field).
 */
static int parse_keydef(const char *s, key_def_t *kd)
{
    memset(kd, 0, sizeof(*kd));
    /* Initialize flag overrides to "not set" */
    kd->start.n = kd->start.g = kd->start.h = kd->start.V = -1;
    kd->start.b = kd->start.d = kd->start.f = kd->start.i = kd->start.r = -1;
    kd->end.n   = kd->end.g   = kd->end.h   = kd->end.V   = -1;
    kd->end.b   = kd->end.d   = kd->end.f   = kd->end.i   = kd->end.r   = -1;

    const char *p = s;

    /* Parse start field */
    if (*p < '1' || *p > '9') return -1;
    char *ep;
    kd->start.field = (int)strtol(p, &ep, 10);
    p = ep;

    /* Optional .C start char offset */
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        kd->start.char_off = (int)strtol(p, &ep, 10);
        if (kd->start.char_off > 0) kd->start.char_off--; /* convert to 0-based */
        p = ep;
    }

    /* Optional flags on start */
    while (*p && *p != ',') {
        switch (*p) {
        case 'b': kd->start.b = 1; break;
        case 'd': kd->start.d = 1; break;
        case 'f': kd->start.f = 1; break;
        case 'i': kd->start.i = 1; break;
        case 'n': kd->start.n = 1; break;
        case 'g': kd->start.g = 1; break;
        case 'h': kd->start.h = 1; break;
        case 'V': kd->start.V = 1; break;
        case 'r': kd->start.r = 1; break;
        default: return -1;
        }
        p++;
    }

    if (*p != ',') return 0; /* no end spec */
    kd->has_end = 1;
    p++;

    /* Parse end field */
    if (*p < '0' || *p > '9') return -1;
    kd->end.field = (int)strtol(p, &ep, 10);
    p = ep;

    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        kd->end.char_off = (int)strtol(p, &ep, 10);
        p = ep;
    }

    /* Optional flags on end */
    while (*p) {
        switch (*p) {
        case 'b': kd->end.b = 1; break;
        case 'd': kd->end.d = 1; break;
        case 'f': kd->end.f = 1; break;
        case 'i': kd->end.i = 1; break;
        case 'n': kd->end.n = 1; break;
        case 'g': kd->end.g = 1; break;
        case 'h': kd->end.h = 1; break;
        case 'V': kd->end.V = 1; break;
        case 'r': kd->end.r = 1; break;
        default: return -1;
        }
        p++;
    }

    return 0;
}

/* ---- I/O helpers ---------------------------------------------------------- */

/*
 * Read all lines from fp into *lines, growing array as needed.
 * *nlines: current count (updated).
 * *cap: current capacity (updated).
 * Returns 0 on success, 1 on error.
 */
static int read_lines(FILE *fp, line_t **lines, size_t *nlines, size_t *cap,
                      int zero_term)
{
    char *raw = NULL;
    size_t raw_cap = 0;
    ssize_t len;

    if (zero_term) {
        /* NUL-terminated: read character by character */
        size_t buf_used = 0;
        size_t buf_cap  = 256;
        char *buf = malloc(buf_cap);
        if (!buf) { err_msg("sort", "out of memory"); return 1; }

        int ch;
        while ((ch = getc(fp)) != EOF) {
            if (buf_used + 1 >= buf_cap) {
                size_t newcap = buf_cap * 2;
                char *tmp = realloc(buf, newcap);
                if (!tmp) { free(buf); err_msg("sort", "out of memory"); return 1; }
                buf = tmp;
                buf_cap = newcap;
            }
            buf[buf_used++] = (char)ch;
            if (ch == '\0') {
                /* Complete record */
                if (*nlines == *cap) {
                    size_t newcap = (*cap) ? (*cap) * 2 : 1024;
                    line_t *tmp = realloc(*lines, newcap * sizeof(line_t));
                    if (!tmp) { free(buf); err_msg("sort", "out of memory"); return 1; }
                    *lines = tmp;
                    *cap = newcap;
                }
                char *copy = malloc(buf_used);
                if (!copy) { free(buf); err_msg("sort", "out of memory"); return 1; }
                memcpy(copy, buf, buf_used);
                (*lines)[*nlines].line      = copy;
                (*lines)[*nlines].len       = buf_used - 1; /* not counting NUL */
                (*lines)[*nlines].idx       = *nlines;
                (*lines)[*nlines].from_mmap = 0;
                (*nlines)++;
                buf_used = 0;
            }
        }
        /* Partial last record (no trailing NUL) */
        if (buf_used > 0) {
            if (*nlines == *cap) {
                size_t newcap = (*cap) ? (*cap) * 2 : 1024;
                line_t *tmp = realloc(*lines, newcap * sizeof(line_t));
                if (!tmp) { free(buf); err_msg("sort", "out of memory"); return 1; }
                *lines = tmp;
                *cap = newcap;
            }
            buf[buf_used++] = '\0';
            char *copy = malloc(buf_used);
            if (!copy) { free(buf); err_msg("sort", "out of memory"); return 1; }
            memcpy(copy, buf, buf_used);
            (*lines)[*nlines].line      = copy;
            (*lines)[*nlines].len       = buf_used - 1;
            (*lines)[*nlines].idx       = *nlines;
            (*lines)[*nlines].from_mmap = 0;
            (*nlines)++;
        }
        free(buf);
        return ferror(fp) ? 1 : 0;
    }

    /* Normal newline-terminated mode.
     * Fast path: mmap regular files > 64KB to eliminate per-line malloc.
     * Lines point directly into the mapped region; no strdup needed.
     * Guard: only mmap when st_size <= SIZE_MAX/2 to avoid overflow. */
    {
        int fd = fileno(fp);
        struct stat st;
        if (fd >= 0 && !zero_term &&
            fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_size > 65536 &&
            (size_t)st.st_size <= ((size_t)-1) / 2 &&
            g_n_mmaps < MAX_MMAP_REGIONS)
        {
            size_t map_size = (size_t)st.st_size;
            void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE, fd, 0);
            if (map != MAP_FAILED) {
                madvise(map, map_size, MADV_SEQUENTIAL);

                /* Record region for munmap at cleanup */
                g_mmaps[g_n_mmaps].base = map;
                g_mmaps[g_n_mmaps].size = map_size;
                g_n_mmaps++;

                /* Scan lines from the mapped region */
                char *p   = (char *)map;
                char *end = p + map_size;
                while (p < end) {
                    char *nl = memchr(p, '\n', (size_t)(end - p));
                    char *line_end = nl ? nl : end;
                    size_t line_len = (size_t)(line_end - p);

                    /* NUL-terminate in the MAP_PRIVATE copy */
                    *line_end = '\0';

                    if (*nlines == *cap) {
                        size_t newcap = (*cap) ? (*cap) * 2 : 1024;
                        line_t *tmp = realloc(*lines, newcap * sizeof(line_t));
                        if (!tmp) {
                            err_msg("sort", "out of memory");
                            return 1;
                        }
                        *lines = tmp;
                        *cap = newcap;
                    }
                    (*lines)[*nlines].line      = p;
                    (*lines)[*nlines].len       = line_len;
                    (*lines)[*nlines].idx       = *nlines;
                    (*lines)[*nlines].from_mmap = 1;
                    (*nlines)++;

                    p = nl ? nl + 1 : end;
                }
                return ferror(fp) ? 1 : 0;
            }
            /* mmap failed: fall through to getline path */
        }
    }

    while ((len = getline(&raw, &raw_cap, fp)) != -1) {
        if (*nlines == *cap) {
            size_t newcap = (*cap) ? (*cap) * 2 : 1024;
            line_t *tmp = realloc(*lines, newcap * sizeof(line_t));
            if (!tmp) { free(raw); err_msg("sort", "out of memory"); return 1; }
            *lines = tmp;
            *cap = newcap;
        }
        char *copy = malloc((size_t)len + 1);
        if (!copy) { free(raw); err_msg("sort", "out of memory"); return 1; }
        memcpy(copy, raw, (size_t)len);
        copy[len] = '\0';
        (*lines)[*nlines].line      = copy;
        (*lines)[*nlines].len       = (size_t)len;
        (*lines)[*nlines].idx       = *nlines;
        (*lines)[*nlines].from_mmap = 0;
        (*nlines)++;
    }
    free(raw);
    return ferror(fp) ? 1 : 0;
}

/* ---- unique deduplication ------------------------------------------------- */

/*
 * After sort, remove consecutive duplicate lines (by comparison key equality).
 * Returns new count.
 */
static size_t dedup_lines(line_t *lines, size_t n)
{
    if (n == 0) return 0;
    size_t out = 1;
    for (size_t k = 1; k < n; k++) {
        /* compare_lines returns 0 for equal keys */
        int rc = compare_lines(&lines[k], &lines[out - 1]);
        if (rc != 0) {
            lines[out++] = lines[k];
        } else {
            if (!lines[k].from_mmap)
                free(lines[k].line);
        }
    }
    return out;
}

/* ---- output --------------------------------------------------------------- */

static int write_lines(FILE *fp, const line_t *lines, size_t n, int zero_term)
{
    for (size_t k = 0; k < n; k++) {
        size_t len = lines[k].len;
        if (fwrite(lines[k].line, 1, len, fp) != len) return 1;
        if (zero_term) {
            /* Lines already include or are followed by NUL in zero mode;
             * but since we stored them as C strings, output explicit NUL. */
            if (fputc('\0', fp) == EOF) return 1;
        } else {
            /* Ensure newline termination */
            if (len == 0 || lines[k].line[len - 1] != '\n') {
                if (fputc('\n', fp) == EOF) return 1;
            }
        }
    }
    return 0;
}

/* ---- applet entry point --------------------------------------------------- */

int applet_sort(int argc, char **argv)
{
    memset(&g_opts, 0, sizeof(g_opts));
    g_nkeys  = 0;
    g_n_mmaps = 0;
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        /* Long options */
        if (strcmp(arg, "--reverse") == 0)          { g_opts.reverse = 1; continue; }
        if (strcmp(arg, "--unique") == 0)            { g_opts.unique = 1; continue; }
        if (strcmp(arg, "--stable") == 0)            { continue; /* always stable */ }
        if (strcmp(arg, "--zero-terminated") == 0)   { g_opts.zero_term = 1; continue; }
        if (strcmp(arg, "--numeric-sort") == 0)      { g_opts.numeric = 1; continue; }
        if (strcmp(arg, "--general-numeric-sort") == 0) { g_opts.general_numeric = 1; continue; }
        if (strcmp(arg, "--human-numeric-sort") == 0)   { g_opts.human_numeric = 1; continue; }
        if (strcmp(arg, "--version-sort") == 0)      { g_opts.version_sort = 1; continue; }
        if (strcmp(arg, "--ignore-leading-blanks") == 0) { g_opts.ignore_blanks = 1; continue; }
        if (strcmp(arg, "--dictionary-order") == 0)  { g_opts.dict_order = 1; continue; }
        if (strcmp(arg, "--ignore-case") == 0)       { g_opts.fold_lower = 1; continue; }
        if (strcmp(arg, "--ignore-nonprinting") == 0) { g_opts.ignore_nonprint = 1; continue; }

        if (strncmp(arg, "--output=", 9) == 0) { g_opts.outfile = arg + 9; continue; }
        if (strncmp(arg, "--field-separator=", 18) == 0) {
            g_opts.field_sep = arg[18];
            g_opts.have_sep = 1;
            continue;
        }
        if (strncmp(arg, "--key=", 6) == 0) {
            if (g_nkeys >= MAX_KEYS) {
                err_msg("sort", "too many -k keys");
                return 1;
            }
            if (parse_keydef(arg + 6, &g_keys[g_nkeys]) != 0) {
                err_msg("sort", "invalid key: '%s'", arg + 6);
                return 1;
            }
            g_nkeys++;
            continue;
        }

        /* Short flags (clustered) */
        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'b': g_opts.ignore_blanks = 1; break;
            case 'd': g_opts.dict_order = 1; break;
            case 'f': g_opts.fold_lower = 1; break;
            case 'i': g_opts.ignore_nonprint = 1; break;
            case 'n': g_opts.numeric = 1; break;
            case 'g': g_opts.general_numeric = 1; break;
            case 'h': g_opts.human_numeric = 1; break;
            case 'V': g_opts.version_sort = 1; break;
            case 'r': g_opts.reverse = 1; break;
            case 'u': g_opts.unique = 1; break;
            case 's': break; /* stable — already our default */
            case 'z': g_opts.zero_term = 1; break;
            case 't': {
                const char *sep;
                if (p[1]) { sep = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("sort", "[-bdfginrsuvz] [-t SEP] [-k KEY] [-o FILE] [FILE...]");
                        return 1;
                    }
                    sep = argv[i];
                }
                g_opts.field_sep = sep[0];
                g_opts.have_sep = 1;
                stop = 1;
                break;
            }
            case 'k': {
                const char *keystr;
                if (p[1]) { keystr = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("sort", "[-bdfginrsuvz] [-t SEP] [-k KEY] [-o FILE] [FILE...]");
                        return 1;
                    }
                    keystr = argv[i];
                }
                if (g_nkeys >= MAX_KEYS) {
                    err_msg("sort", "too many -k keys");
                    return 1;
                }
                if (parse_keydef(keystr, &g_keys[g_nkeys]) != 0) {
                    err_msg("sort", "invalid key: '%s'", keystr);
                    return 1;
                }
                g_nkeys++;
                stop = 1;
                break;
            }
            case 'o': {
                const char *ofile;
                if (p[1]) { ofile = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("sort", "[-bdfginrsuvz] [-t SEP] [-k KEY] [-o FILE] [FILE...]");
                        return 1;
                    }
                    ofile = argv[i];
                }
                g_opts.outfile = ofile;
                stop = 1;
                break;
            }
            default:
                err_msg("sort", "unrecognized option '-%c'", *p);
                err_usage("sort", "[-bdfginrsuvz] [-t SEP] [-k KEY] [-o FILE] [FILE...]");
                return 1;
            }
            p++;
        }
    }

    /* Read all input lines */
    line_t *lines = NULL;
    size_t  nlines = 0;
    size_t  cap    = 0;

    int nfiles = argc - i;
    if (nfiles == 0) {
        if (read_lines(stdin, &lines, &nlines, &cap, g_opts.zero_term) != 0)
            ret = 1;
    } else {
        for (int j = i; j < argc; j++) {
            const char *fname = argv[j];
            FILE *fp;
            if (strcmp(fname, "-") == 0) {
                fp = stdin;
            } else {
                fp = fopen(fname, "r");
                if (!fp) {
                    err_sys("sort", "cannot open '%s'", fname);
                    ret = 1;
                    continue;
                }
                posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL);
                /* Large read buffer: reduces getline() syscall count ~30x
                 * (default glibc 4KB → 128KB, matching GNU sort behaviour).
                 * Must supply an explicit buffer; NULL still yields 4KB. */
                static char sort_iobuf[131072];
                setvbuf(fp, sort_iobuf, _IOFBF, sizeof(sort_iobuf));
            }
            /* Assign stable indices relative to total collected so far */
            size_t before = nlines;
            if (read_lines(fp, &lines, &nlines, &cap, g_opts.zero_term) != 0)
                ret = 1;
            /* Re-index new lines starting at global offset */
            for (size_t k = before; k < nlines; k++)
                lines[k].idx = k;
            if (fp != stdin) fclose(fp);
        }
    }

    if (nlines > 0) {
        g_lines_ptr = lines;
        qsort(lines, nlines, sizeof(line_t), qsort_cmp);

        if (g_opts.unique)
            nlines = dedup_lines(lines, nlines);
    }

    /* Explicit 128KB stdout buffer when not writing to terminal: reduces write()
     * syscall count for large sorted outputs (default glibc buf = 4KB). */
    if (!isatty(STDOUT_FILENO)) {
        static char sort_out_buf[131072];
        setvbuf(stdout, sort_out_buf, _IOFBF, sizeof(sort_out_buf));
    }

    /* Output: if -o, write to temp then rename */
    if (g_opts.outfile) {
        char tmppath[PATH_MAX];
        int  use_tmpfile = 0;
        int  tmpfd = -1;

        /* Extract directory containing outfile for O_TMPFILE */
        char outdir[PATH_MAX];
        {
            const char *sl = strrchr(g_opts.outfile, '/');
            if (sl && sl != g_opts.outfile) {
                size_t dlen = (size_t)(sl - g_opts.outfile);
                if (dlen < sizeof(outdir)) {
                    memcpy(outdir, g_opts.outfile, dlen);
                    outdir[dlen] = '\0';
                } else {
                    outdir[0] = '.'; outdir[1] = '\0';
                }
            } else if (sl == g_opts.outfile) {
                outdir[0] = '/'; outdir[1] = '\0';
            } else {
                outdir[0] = '.'; outdir[1] = '\0';
            }
        }

#ifdef O_TMPFILE
        /* Try O_TMPFILE: anonymous file; no orphan on crash */
        tmpfd = open(outdir, O_TMPFILE | O_RDWR, 0600);
        if (tmpfd >= 0) {
            int nr = snprintf(tmppath, sizeof(tmppath),
                              "%s.sort_%d", outdir, (int)getpid());
            if (nr < 0 || (size_t)nr >= sizeof(tmppath)) {
                close(tmpfd);
                tmpfd = -1;
            } else {
                use_tmpfile = 1;
            }
        }
#endif
        if (tmpfd < 0) {
            int r = snprintf(tmppath, sizeof(tmppath),
                             "%s.sort.XXXXXX", g_opts.outfile);
            if (r < 0 || (size_t)r >= sizeof(tmppath)) {
                err_msg("sort", "output path too long");
                ret = 1;
                goto cleanup;
            }
            tmpfd = mkstemp(tmppath);
            if (tmpfd < 0) {
                err_sys("sort", "cannot create temp file '%s'", tmppath);
                ret = 1;
                goto cleanup;
            }
        }
        FILE *tmpfp = fdopen(tmpfd, "w");
        if (!tmpfp) {
            err_sys("sort", "fdopen failed");
            close(tmpfd);
            if (!use_tmpfile) unlink(tmppath);
            ret = 1;
            goto cleanup;
        }
        if (write_lines(tmpfp, lines, nlines, g_opts.zero_term) != 0) {
            err_sys("sort", "write error");
            fclose(tmpfp);
            if (!use_tmpfile) unlink(tmppath);
            ret = 1;
            goto cleanup;
        }
#ifdef O_TMPFILE
        if (use_tmpfile) {
            /* Link anonymous file into filesystem before closing */
            if (fflush(tmpfp) != 0) {
                err_sys("sort", "fflush");
                fclose(tmpfp);
                ret = 1;
                goto cleanup;
            }
            char procpath[64];
            snprintf(procpath, sizeof(procpath),
                     "/proc/self/fd/%d", fileno(tmpfp));
            if (linkat(AT_FDCWD, procpath, AT_FDCWD, tmppath,
                       AT_SYMLINK_FOLLOW) != 0) {
                err_sys("sort", "linkat");
                fclose(tmpfp);
                ret = 1;
                goto cleanup;
            }
        }
#endif
        fclose(tmpfp);
        if (rename(tmppath, g_opts.outfile) != 0) {
            err_sys("sort", "cannot rename '%s' to '%s'", tmppath, g_opts.outfile);
            unlink(tmppath);
            ret = 1;
        }
    } else {
        if (write_lines(stdout, lines, nlines, g_opts.zero_term) != 0) {
            err_sys("sort", "write error");
            ret = 1;
        }
    }

cleanup:
    for (size_t k = 0; k < nlines; k++) {
        if (!lines[k].from_mmap)
            free(lines[k].line);
    }
    free(lines);
    for (int m = 0; m < g_n_mmaps; m++)
        munmap(g_mmaps[m].base, g_mmaps[m].size);
    g_n_mmaps = 0;
    return ret;
}
