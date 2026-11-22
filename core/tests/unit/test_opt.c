/* test_opt.c — shared option parser (src/util/opt.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/opt.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", desc);
    if (!ok) failures++;
}

static const opt_long_t longs[] = {
    { "bare",   OPT_BARE,              'b' },
    { "joined", OPT_JOINED,            'j' },
    { "req",    OPT_REQ,               'r' },
    { "lax",    OPT_BARE | OPT_JOINED, 'l' },
    { NULL, 0, 0 }
};

/* Run the parser over WORDS and render every result into OUT, e.g.
 * "a b=x ?- !n ." -- option(=arg), ?unknown, !missing, +operand, '.' done
 * followed by the operand index. */
static void trace_ops(const char *spec, unsigned flags, char **words, int n,
                      char *out, size_t outsz, int show_ops)
{
    char *argv[16];
    argv[0] = (char *)"prog";
    for (int i = 0; i < n; i++)
        argv[i + 1] = words[i];
    opt_t o;
    opt_init(&o, n + 1, argv, spec, longs, flags);
    out[0] = '\0';
    size_t len = 0;
    int c;
    while ((c = opt_next(&o)) != OPT_DONE) {
        char item[64];
        if (c == OPT_UNKNOWN)
            snprintf(item, sizeof(item), "?%c ", o.opt);
        else if (c == OPT_MISSING)
            snprintf(item, sizeof(item), "!%c ", o.opt);
        else if (c == OPT_OPERAND)
            snprintf(item, sizeof(item), "+%s ", o.arg);
        else if (o.arg)
            snprintf(item, sizeof(item), "%c=%s ", c, o.arg);
        else
            snprintf(item, sizeof(item), "%c ", c);
        len += (size_t)snprintf(out + len, outsz - len, "%s", item);
        if (c == OPT_UNKNOWN || c == OPT_MISSING)
            break;
    }
    len += (size_t)snprintf(out + len, outsz - len, ".%d", o.ind);
    /* With show_ops, also list argv[o.ind..] -- what the applet will treat
     * as its operands once the loop ends. */
    if (show_ops)
        for (int i = o.ind; i < n + 1; i++)
            len += (size_t)snprintf(out + len, outsz - len, " %s", argv[i]);
}

static void trace(const char *spec, unsigned flags, char **words, int n,
                  char *out, size_t outsz)
{
    trace_ops(spec, flags, words, n, out, outsz, 0);
}

#define T(desc, spec, flags, expect, ...) do {                         \
        char *w_[] = { __VA_ARGS__ };                                   \
        char got_[256];                                                 \
        trace(spec, flags, w_, (int)(sizeof(w_) / sizeof(w_[0])),       \
              got_, sizeof(got_));                                      \
        int ok_ = strcmp(got_, expect) == 0;                            \
        check(desc, ok_);                                               \
        if (!ok_) printf("    got '%s', expected '%s'\n", got_, expect); \
    } while (0)

/* Like T, but the expectation also lists the operands left at argv[o.ind..]. */
#define TO(desc, spec, flags, expect, ...) do {                        \
        char *w_[] = { __VA_ARGS__ };                                   \
        char got_[256];                                                 \
        trace_ops(spec, flags, w_, (int)(sizeof(w_) / sizeof(w_[0])),   \
                  got_, sizeof(got_), 1);                               \
        int ok_ = strcmp(got_, expect) == 0;                            \
        check(desc, ok_);                                               \
        if (!ok_) printf("    got '%s', expected '%s'\n", got_, expect); \
    } while (0)

int main(void)
{
    /* The tests below must not depend on the caller's environment. */
    unsetenv("POSIXLY_CORRECT");

    T("cluster of flags",          "abc",   0, "a b c .3",       "-ab", "-c");
    T("first operand stops",       "ab",    0, "a .2",           "-a", "x", "-b");
    T("lone dash is an operand",   "a",     0, ".1",             "-", "-a");
    T("double dash ends options",  "a",     0, "a .3",           "-a", "--", "-a");
    T("attached argument",         "an:",   0, "a n=5 .2",       "-an5", "x");
    T("separate argument",         "n:",    0, "n=5 .3",         "-n", "5", "x");
    T("argument may be --",        "n:",    0, "n=-- .3",        "-n", "--", "x");
    T("missing argument",          "n:",    0, "!n .2",          "-n");
    T("optional attached only",    "i::a",  0, "i a .3",         "-i", "-a", "x");
    T("optional attached value",   "i::",   0, "i=bak .2",       "-ibak", "x");
    T("unknown short",             "a",     0, "a ?z .2",        "-az");
    T("colon is never an option",  "n:",    0, "?: .2",          "-:");
    T("dash inside a cluster",     "a",     0, "a ?- .2",        "-a-");
    T("long bare",                 "",      0, "b .2",           "--bare", "x");
    T("long bare rejects =",       "",      0, "?- .2",          "--bare=1");
    T("long joined",               "",      0, "j=v .2",         "--joined=v", "x");
    T("long joined rejects bare",  "",      0, "?- .2",          "--joined", "v");
    T("long req joined",           "",      0, "r=v .2",         "--req=v");
    T("long req separate",         "",      0, "r=v .3",         "--req", "v", "x");
    T("long req missing",          "",      0, "!- .2",          "--req");
    T("long lax both forms",       "",      0, "l l=1 .3",       "--lax", "--lax=1");
    T("long exact, no prefixes",   "",      0, "?- .2",          "--ba");
    T("long unknown",              "",      0, "?- .2",          "--nope");
    T("permute operands",          "a",     OPT_PERMUTE, "+x a +y .4", "x", "-a", "y");
    T("permute after --",          "a",     OPT_PERMUTE, "a +-a +- .5", "-a", "--", "-a", "-");

    /* OPT_GNU: options may follow operands; argv is reordered so the
     * operands, in their original order, end up at argv[o.ind..]. */
    TO("gnu: option after operand",  "a",   OPT_GNU, "a .2 x",        "x", "-a");
    TO("gnu: operands keep order",   "ab",  OPT_GNU, "a b .3 x y z w",
       "x", "-a", "y", "z", "-b", "w");
    TO("gnu: separate argument",     "n:",  OPT_GNU, "n=5 .3 f g",    "f", "-n", "5", "g");
    TO("gnu: argument looks like operand", "n:a", OPT_GNU, "a n=3 .3 x y",
       "x", "-an", "3", "y");
    TO("gnu: long option after operand", "", OPT_GNU, "r=v b .4 x",  "x", "--req", "v", "--bare");
    TO("gnu: options only",          "ab",  OPT_GNU, "a b .3",        "-a", "-b");
    TO("gnu: operands only",         "a",   OPT_GNU, ".1 x y",        "x", "y");
    TO("gnu: -- ends options",       "a",   OPT_GNU, "a .3 x -a y",   "x", "-a", "--", "-a", "y");
    TO("gnu: leading --",            "a",   OPT_GNU, ".2 -a x",       "--", "-a", "x");
    TO("gnu: trailing --",           "a",   OPT_GNU, "a .3 x",        "x", "-a", "--");
    TO("gnu: -- as option argument", "n:",  OPT_GNU, "n=-- .3 x y",   "x", "-n", "--", "y");
    TO("gnu: lone dash is operand",  "a",   OPT_GNU, "a .2 - x",      "-", "-a", "x");
    T("gnu: unknown after operand",  "a",   OPT_GNU, "?z .3",         "x", "-z");
    T("gnu: missing after operand",  "n:",  OPT_GNU, "!n .3",         "x", "-n");
    T("gnu: OPT_PERMUTE wins",       "a",   OPT_GNU | OPT_PERMUTE, "+x a .3", "x", "-a");
    TO("flags 0 still stops",        "a",   0,       ".1 x -a",       "x", "-a");

    /* POSIXLY_CORRECT: the first operand ends option parsing again. */
    setenv("POSIXLY_CORRECT", "1", 1);
    TO("gnu + POSIXLY_CORRECT stops", "a",  OPT_GNU, "a .2 x -a",     "-a", "x", "-a");
    TO("gnu + POSIXLY_CORRECT --",    "a",  OPT_GNU, "a .3 x",        "-a", "--", "x");
    unsetenv("POSIXLY_CORRECT");
    TO("gnu after unsetenv",          "a",  OPT_GNU, "a a .3 x",      "-a", "x", "-a");

    /* OPT_GNU opt_peek: operands in the way are stepped over, so the word
     * offered is the next one that could be an option. */
    {
        char *argv[] = { (char *)"date", (char *)"x", (char *)"+%F",
                         (char *)"--iso-8601", (char *)"-u", (char *)"y", NULL };
        opt_t o;
        opt_init(&o, 6, argv, "u", NULL, OPT_GNU);
        const char *p = opt_peek(&o);
        int ok = p && strcmp(p, "--iso-8601") == 0;
        ok = ok && opt_peek(&o) == p;          /* idempotent */
        opt_skip(&o);
        ok = ok && opt_next(&o) == 'u';
        ok = ok && opt_peek(&o) == NULL;
        ok = ok && opt_next(&o) == OPT_DONE && o.ind == 3;
        ok = ok && strcmp(argv[3], "x") == 0 && strcmp(argv[4], "+%F") == 0 &&
             strcmp(argv[5], "y") == 0;
        check("gnu: opt_peek steps over operands", ok);
    }

    /* opt_rest: claim the rest of a cluster (head -5 / -q5). */
    {
        char *argv[] = { (char *)"head", (char *)"-q12", (char *)"f", NULL };
        opt_t o;
        opt_init(&o, 3, argv, "q", NULL, 0);
        int c1 = opt_next(&o);
        int c2 = opt_next(&o);
        const char *r = (c2 == OPT_UNKNOWN) ? opt_rest(&o) : "";
        int c3 = opt_next(&o);
        check("opt_rest returns the cluster from the unknown char",
              c1 == 'q' && c2 == OPT_UNKNOWN && strcmp(r, "12") == 0 &&
              c3 == OPT_DONE && o.ind == 2);
    }

    /* opt_peek/opt_skip: claim a whole word before the parser does. */
    {
        char *argv[] = { (char *)"date", (char *)"+%F", (char *)"-u",
                         (char *)"x", NULL };
        opt_t o;
        opt_init(&o, 4, argv, "u", NULL, 0);
        const char *p = opt_peek(&o);
        int ok = p && strcmp(p, "+%F") == 0;
        opt_skip(&o);
        ok = ok && opt_next(&o) == 'u';
        ok = ok && opt_next(&o) == OPT_DONE && o.ind == 3;
        ok = ok && opt_peek(&o) == NULL;
        check("opt_peek/opt_skip claim words in order", ok);
    }

    /* Module hand-off needs the argv slot of the unknown flag. */
    {
        char *argv[] = { (char *)"cp", (char *)"-r", (char *)"-vZ",
                         (char *)"a", NULL };
        opt_t o;
        opt_init(&o, 4, argv, "rv", NULL, 0);
        while (opt_next(&o) > 0)
            ;
        check("cur_ind names the word the unknown flag came from",
              o.opt == 'Z' && o.cur_ind == 2 && !o.islong);
    }

    /* State is per-call: a second parse over new argv starts clean. */
    {
        char *a1[] = { (char *)"x", (char *)"-ab", NULL };
        char *a2[] = { (char *)"x", (char *)"-b", (char *)"y", NULL };
        opt_t o;
        opt_init(&o, 2, a1, "ab", NULL, 0);
        opt_next(&o);
        opt_init(&o, 3, a2, "ab", NULL, 0);
        int c1 = opt_next(&o);
        int c2 = opt_next(&o);
        check("re-initialisation discards a half-read cluster",
              c1 == 'b' && c2 == OPT_DONE && o.ind == 2);
    }

    if (failures) {
        printf("\nResults: %d failed\n", failures);
        return 1;
    }
    printf("\nResults: all passed\n");
    return 0;
}
