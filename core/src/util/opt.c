/* opt.c — shared command-line option parser for applets (see opt.h) */

#include "opt.h"
#include "error.h"

#include <stddef.h>
#include <string.h>

void opt_init(opt_t *o, int argc, char **argv, const char *shorts,
              const opt_long_t *longs, unsigned flags)
{
    o->argc    = argc;
    o->argv    = argv;
    o->shorts  = shorts ? shorts : "";
    o->longs   = longs;
    o->flags   = flags;
    o->ind     = 1;
    o->cur_ind = 0;
    o->cur     = NULL;
    o->arg     = NULL;
    o->opt     = 0;
    o->lopt    = NULL;
    o->islong  = 0;
    o->next    = NULL;
    o->pos     = NULL;
    o->ddash   = 0;
    o->done    = 0;
}

static int opt_long(opt_t *o, const char *word)
{
    const char *name = word + 2;
    const char *eq   = strchr(name, '=');
    size_t      nlen = eq ? (size_t)(eq - name) : strlen(name);

    o->opt    = '-';
    o->islong = 1;

    if (o->longs) {
        for (const opt_long_t *e = o->longs; e->name; e++) {
            if (strncmp(e->name, name, nlen) != 0 || e->name[nlen] != '\0')
                continue;
            o->lopt = e;
            if (eq) {
                if (!(e->flags & OPT_JOINED))
                    break;
                o->arg = eq + 1;
                return e->val;
            }
            if (e->flags & OPT_BARE)
                return e->val;
            if (e->flags & OPT_NEXT) {
                if (o->ind >= o->argc)
                    return OPT_MISSING;
                o->arg = o->argv[o->ind++];
                return e->val;
            }
            break;
        }
    }
    o->lopt = NULL;
    return OPT_UNKNOWN;
}

static int opt_short(opt_t *o)
{
    int c = (unsigned char)*o->next;
    o->pos = o->next++;
    o->opt = c;

    const char *s = (c != ':') ? strchr(o->shorts, c) : NULL;
    if (!s)
        return OPT_UNKNOWN;

    if (s[1] == ':') {
        if (*o->next) {
            o->arg = o->next;
        } else if (s[2] == ':') {
            o->arg = NULL;           /* optional: attached form only */
        } else if (o->ind < o->argc) {
            o->arg = o->argv[o->ind++];
        } else {
            o->next = NULL;
            return OPT_MISSING;
        }
        o->next = NULL;
    }
    return c;
}

int opt_next(opt_t *o)
{
    o->arg    = NULL;
    o->lopt   = NULL;
    o->islong = 0;

    if (o->next && *o->next)
        return opt_short(o);
    o->next = NULL;

    if (o->done)
        return OPT_DONE;

    for (;;) {
        if (o->ind >= o->argc)
            return OPT_DONE;

        const char *w = o->argv[o->ind];

        if (o->ddash || w[0] != '-' || w[1] == '\0') {
            if (!(o->flags & OPT_PERMUTE)) {
                o->done = 1;
                return OPT_DONE;
            }
            o->cur_ind = o->ind++;
            o->cur = w;
            o->arg = w;
            return OPT_OPERAND;
        }

        if (w[1] == '-' && w[2] == '\0') {
            o->ind++;
            if (!(o->flags & OPT_PERMUTE)) {
                o->done = 1;
                return OPT_DONE;
            }
            o->ddash = 1;
            continue;
        }

        o->cur_ind = o->ind++;
        o->cur = w;
        if (w[1] == '-') {
            o->pos = w + 1;
            return opt_long(o, w);
        }
        o->next = w + 1;
        return opt_short(o);
    }
}

const char *opt_peek(const opt_t *o)
{
    if ((o->next && *o->next) || o->ddash || o->done || o->ind >= o->argc)
        return NULL;
    return o->argv[o->ind];
}

void opt_skip(opt_t *o)
{
    o->cur_ind = o->ind;
    o->cur = o->argv[o->ind++];
    o->next = NULL;
}

const char *opt_rest(opt_t *o)
{
    const char *r = o->pos;
    o->next = NULL;
    return r;
}

void opt_err_unknown(const char *name, const opt_t *o)
{
    err_msg(name, "unrecognized option '-%c'", o->opt);
}
