/* opt.h — shared command-line option parser for applets
 *
 * One iterator, used by every applet that takes ordinary flags. It walks argv
 * exactly once, left to right, and never allocates: all state lives in an
 * opt_t on the caller's stack, so an applet running in-process as a shell
 * builtin costs no heap traffic for its flags and leaves no global state behind
 * for the next invocation (unlike getopt()'s optind/optarg).
 *
 * The parser only decides WHAT an argument is. Every message is printed by the
 * applet, because the applets do not agree on wording (usage-only vs.
 * "unrecognized option", GNU-style "--foo" vs. legacy "'--'", module lookup on
 * unknown flags, ...), and that wording is part of their interface.
 *
 * Grammar:
 *   "--"            ends option parsing (permute mode: everything after it is
 *                   an operand)
 *   "-" or "x..."   an operand; ends option parsing unless OPT_PERMUTE
 *   "--name[=val]"  a long option, matched EXACTLY (no abbreviations) against
 *                   the table; the forms each entry accepts are set by its
 *                   flags. A form an entry does not accept is OPT_UNKNOWN.
 *   "-abc"          a cluster of short options, getopt-style spec:
 *                   "x" flag, "x:" required argument (rest of the cluster or
 *                   the next word), "x::" optional argument (rest of cluster
 *                   only).
 *
 * Typical use:
 *
 *     opt_t o;
 *     int c;
 *     opt_init(&o, argc, argv, "ab:", longs, 0);
 *     while ((c = opt_next(&o)) != OPT_DONE) {
 *         switch (c) {
 *         case 'a': ...; break;
 *         case 'b': use(o.arg); break;
 *         case OPT_MISSING: ...; return 1;
 *         default:  opt_err_unknown("name", &o); ...; return 1;
 *         }
 *     }
 *     operands are argv[o.ind] .. argv[argc - 1]
 */

#ifndef SILEX_OPT_H
#define SILEX_OPT_H

/* opt_next() results other than an option's own value. All negative, so they
 * cannot collide with a short option character or a long option's val. */
enum {
    OPT_DONE    = -1,  /* no more options; operands start at o->ind        */
    OPT_UNKNOWN = -2,  /* unrecognised option; o->opt is the character, or
                        * '-' for a long option (o->cur is the whole word)  */
    OPT_MISSING = -3,  /* o->opt (or long o->lopt) needs an argument and
                        * none was left                                     */
    OPT_OPERAND = -4   /* OPT_PERMUTE only: o->arg is an operand            */
};

/* Long option forms. An entry accepts any combination. */
#define OPT_BARE    0x1  /* "--name"            (o->arg = NULL)            */
#define OPT_JOINED  0x2  /* "--name=VAL"        (o->arg = VAL)             */
#define OPT_NEXT    0x4  /* "--name VAL"        (o->arg = next word); only
                          * consulted when OPT_BARE is not set             */
#define OPT_REQ     (OPT_JOINED | OPT_NEXT)  /* getopt_long required_argument */

typedef struct {
    const char *name;    /* without the leading "--"; NULL ends the table */
    int         flags;   /* OPT_BARE / OPT_JOINED / OPT_NEXT              */
    int         val;     /* returned by opt_next() on a match             */
} opt_long_t;

/* opt_init() flags */
#define OPT_PERMUTE 0x1  /* operands may precede options; opt_next() returns
                          * them one at a time as OPT_OPERAND               */

typedef struct {
    int                argc;
    char             **argv;
    const char        *shorts;
    const opt_long_t  *longs;
    unsigned           flags;

    int                ind;     /* next argv index to examine                */
    int                cur_ind; /* argv index the last result came from      */
    const char        *cur;     /* argv[cur_ind]                             */
    const char        *arg;     /* option argument, or operand               */
    int                opt;     /* short option char; '-' for a long option  */
    const opt_long_t  *lopt;    /* matched long option entry, or NULL        */
    int                islong;  /* the last result came from a "--" word     */

    const char        *next;    /* unread rest of the current short cluster  */
    const char        *pos;     /* the character o->opt was read from        */
    int                ddash;   /* OPT_PERMUTE: "--" has been seen           */
    int                done;    /* OPT_DONE has been returned                */
} opt_t;

/* Start parsing argv[1..argc-1]. longs may be NULL. */
void opt_init(opt_t *o, int argc, char **argv, const char *shorts,
              const opt_long_t *longs, unsigned flags);

/* Advance to the next option. See the OPT_* results above. */
int opt_next(opt_t *o);

/* The next whole word, if opt_next() would start on a fresh one that could be
 * an option (not inside a cluster, not after "--" in permute mode, not past
 * the end); NULL otherwise. Lets an applet claim words with a syntax of its
 * own (date's "+FORMAT", chmod's "-w") before the parser sees them. */
const char *opt_peek(const opt_t *o);

/* Consume the word opt_peek() returned. */
void opt_skip(opt_t *o);

/* After a short option result (including OPT_UNKNOWN for a short option),
 * take the rest of the cluster starting AT that option's character -- e.g.
 * the digits of head's "-5" -- so parsing resumes with the next word. */
const char *opt_rest(opt_t *o);

/* "silex: NAME: unrecognized option '-C'", where C is o->opt; for a long
 * option that is '-', matching what the applets have always printed. */
void opt_err_unknown(const char *name, const opt_t *o);

#endif /* SILEX_OPT_H */
