/* thompson.c — Thompson NFA simulation with lazy DFA cache + BMH
 *
 * Two-list NFA simulation:
 *   - Maintain "current state set" and "next state set"
 *   - For each input char, advance all current states simultaneously
 *   - O(n * k) where n = input length, k = NFA size (usually small)
 *
 * Lazy DFA cache:
 *   - Encode NFA state set as a sorted array, hashed via FNV-1a
 *   - Cache {state_set → per-char next_state_set_id} to amortise repeated steps
 *   - Bounded at DFA_CACHE_MAX entries; evict LRU when full
 *   - Gives O(n) amortised for repeated patterns on similar text
 *
 * Boyer-Moore-Horspool (BMH) for fixed-string search:
 *   - Precompute bad-character skip table from needle
 *   - Average O(n/m) for typical text
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include "../section.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* ---- Boyer-Moore-Horspool ------------------------------------------------ */

void mb_bmh_build(const char *needle, size_t nlen, size_t skip[256])
{
    for (int i = 0; i < 256; i++)
        skip[i] = nlen ? nlen : 1;
    if (nlen < 2) return;  /* guard unsigned underflow for nlen-1 */
    for (size_t i = 0; i < nlen - 1; i++)
        skip[(unsigned char)needle[i]] = nlen - 1 - i;
}

HOT const char *mb_bmh_search(const char *haystack, size_t hlen,
                               const char *needle, size_t nlen,
                               const size_t skip[256])
{
    if (nlen == 0) return haystack;
    if (nlen > hlen) return NULL;

    size_t i = nlen - 1;
    while (i < hlen) {
        size_t j = nlen - 1;
        size_t k = i;
        while (j < nlen && haystack[k] == needle[j]) {
            if (j == 0) return haystack + k;
            j--; k--;
        }
        i += skip[(unsigned char)haystack[i]];
    }
    return NULL;
}

void mb_bmh_build_icase(const char *needle, size_t nlen, size_t skip[256])
{
    for (int i = 0; i < 256; i++)
        skip[i] = nlen ? nlen : 1;
    if (nlen < 2) return;  /* guard unsigned underflow */
    for (size_t i = 0; i < nlen - 1; i++) {
        unsigned char lo = (unsigned char)tolower((unsigned char)needle[i]);
        unsigned char up = (unsigned char)toupper((unsigned char)needle[i]);
        size_t s = nlen - 1 - i;
        skip[lo] = s;
        skip[up] = s;
    }
}

HOT const char *mb_bmh_search_icase(const char *haystack, size_t hlen,
                                     const char *needle, size_t nlen,
                                     const size_t skip[256])
{
    if (nlen == 0) return haystack;
    if (nlen > hlen) return NULL;

    size_t i = nlen - 1;
    while (i < hlen) {
        size_t j = nlen - 1;
        size_t k = i;
        while (j < nlen &&
               tolower((unsigned char)haystack[k]) ==
               tolower((unsigned char)needle[j])) {
            if (j == 0) return haystack + k;
            j--; k--;
        }
        i += skip[(unsigned char)haystack[i]];
    }
    return NULL;
}

/* ---- NFA state list ------------------------------------------------------- */

#define MAX_NFA_STATES MB_MAX_INSTRS

typedef struct {
    int  states[MAX_NFA_STATES];
    int  n;
    int  gen;   /* generation counter for deduplication */
} nfa_list;

/* Per-state "last generation added" for deduplication */
static int state_last[MAX_NFA_STATES];
static int global_gen = 0;

/*
 * Add state 's' to list 'l', following SPLIT and JUMP transitions recursively.
 * Uses global_gen to avoid adding the same state twice.
 * 'depth' guards against stack overflow from pathological epsilon-closure chains;
 * the cap is MB_MAX_INSTRS (4096) since no chain can be longer than the program.
 */
static void addstate(const mb_prog *prog, nfa_list *l, int s, int depth)
{
    if (s < 0 || s >= prog->len) return;
    if (depth >= MB_MAX_INSTRS) return;        /* recursion depth guard */
    if (state_last[s] == l->gen) return;       /* already in list */
    state_last[s] = l->gen;

    const mb_instr *in = &prog->instrs[s];
    if (in->op == I_SPLIT) {
        addstate(prog, l, in->x, depth + 1);
        addstate(prog, l, in->y, depth + 1);
        return;
    }
    if (in->op == I_JUMP) {
        addstate(prog, l, in->x, depth + 1);
        return;
    }
    /* Regular state: add to list */
    if (l->n < MAX_NFA_STATES)
        l->states[l->n++] = s;
}

/*
 * Compute the next state list from clist given input character c.
 */
HOT static void step(const mb_prog *prog, int flags,
                     const nfa_list *clist, unsigned char c,
                     nfa_list *nlist, int is_newline_prev)
{
    nlist->n   = 0;
    nlist->gen = ++global_gen;
    (void)is_newline_prev;

    int newline_mode = (flags & MB_REG_NEWLINE) != 0;

    for (int i = 0; i < clist->n; i++) {
        int s = clist->states[i];
        if (s < 0 || s >= prog->len) continue;
        const mb_instr *in = &prog->instrs[s];

        switch (in->op) {
        case I_CHAR:
            if ((unsigned char)in->arg.c == c)
                addstate(prog, nlist, in->x, 0);
            break;
        case I_ICHAR:
            if ((unsigned char)tolower(c) == (unsigned char)in->arg.c)
                addstate(prog, nlist, in->x, 0);
            break;
        case I_CLASS:
            if (in->arg.cc && mb_charclass_test(in->arg.cc, c))
                addstate(prog, nlist, in->x, 0);
            break;
        case I_ANY:
            if (!newline_mode || c != '\n')
                addstate(prog, nlist, in->x, 0);
            break;
        case I_BOL:
        case I_EOL:
            /* Zero-width assertions handled at start of addstate */
            break;
        case I_MATCH:
        case I_SPLIT:
        case I_JUMP:
        case I_SAVE:
            break;
        }
    }
}

/* Check if clist contains a MATCH state */
HOT static int is_match(const mb_prog *prog, const nfa_list *l)
{
    for (int i = 0; i < l->n; i++) {
        int s = l->states[i];
        if (s >= 0 && s < prog->len && prog->instrs[s].op == I_MATCH)
            return 1;
    }
    return 0;
}

/* Check if clist contains any MATCH-producing state (including checking
 * if any state is I_MATCH directly) */
HOT static int contains_match(const mb_prog *prog, const nfa_list *l)
{
    return is_match(prog, l);
}

/* ---- Zero-width assertion handling --------------------------------------- */

/*
 * Process zero-width assertions (BOL, EOL) in the current state list.
 * Called at the start of simulation and after each character.
 */
static void process_assertions(const mb_prog *prog, int flags,
                                nfa_list *l, int at_bol, int at_eol)
{
    int newline_mode = (flags & MB_REG_NEWLINE) != 0;
    (void)newline_mode;

    /* Expand BOL/EOL states */
    for (int i = 0; i < l->n; i++) {
        int s = l->states[i];
        if (s < 0 || s >= prog->len) continue;
        const mb_instr *in = &prog->instrs[s];
        if (in->op == I_BOL && at_bol) {
            addstate(prog, l, in->x, 0);
        } else if (in->op == I_EOL && at_eol) {
            addstate(prog, l, in->x, 0);
        }
    }
}

/* ---- Lazy DFA cache ------------------------------------------------------- */

/*
 * The DFA cache maps {sorted NFA state set} → {per-char next state set id}.
 * This turns O(n*k) NFA simulation into O(n) amortised.
 *
 * Implementation: simple open-addressing hash table of DFA states.
 * Each DFA state stores:
 *   - sorted array of NFA states
 *   - transition table: 256 ints (DFA state ID for each input byte)
 *
 * Bounded at DFA_CACHE_MAX. When full, clear all entries (simple eviction).
 */

#define DFA_CACHE_MAX 256

typedef struct {
    int nfa_states[MAX_NFA_STATES];
    int n_states;
    int next[256];   /* -1 = not computed yet */
    int is_match;    /* 1 if this DFA state contains a MATCH NFA state */
    int in_use;
} dfa_state_t;

/* DFA cache is per-call (on stack or static) to keep things thread-safe per use */
typedef struct {
    dfa_state_t states[DFA_CACHE_MAX];
    int         count;
} dfa_cache_t;

static uint32_t fnv1a(const int *data, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static int dfa_find_or_create(dfa_cache_t *cache, const mb_prog *prog,
                               const nfa_list *states)
{
    if (cache->count >= DFA_CACHE_MAX) {
        /* Cache full: clear and start over */
        memset(cache->states, 0, sizeof(cache->states));
        cache->count = 0;
    }

    /* Compute hash */
    uint32_t h = fnv1a(states->states, states->n) % DFA_CACHE_MAX;

    /* Linear probe */
    for (int tries = 0; tries < DFA_CACHE_MAX; tries++) {
        int slot = (int)((h + (uint32_t)tries) % DFA_CACHE_MAX);
        dfa_state_t *d = &cache->states[slot];

        if (!d->in_use) {
            /* Create new DFA state */
            d->n_states = states->n;
            for (int i = 0; i < states->n && i < MAX_NFA_STATES; i++)
                d->nfa_states[i] = states->states[i];
            memset(d->next, -1, sizeof(d->next));
            d->is_match = contains_match(prog, states);
            d->in_use   = 1;
            cache->count++;
            return slot;
        }

        /* Check if this slot matches */
        if (d->n_states == states->n) {
            int match = 1;
            for (int i = 0; i < states->n; i++) {
                if (d->nfa_states[i] != states->states[i]) {
                    match = 0; break;
                }
            }
            if (match) return slot;
        }
    }

    /* Hash table full (shouldn't happen with clear above) */
    return -1;
}

/* ---- Main search function ------------------------------------------------- */

HOT int mb_thompson_search(const mb_prog *prog, int flags,
                            const char *text, size_t n,
                            mb_match *out, int anchor_bol)
{
    if (!prog || prog->len == 0) return 0;

    int newline_mode = (flags & MB_REG_NEWLINE) != 0;

    /* State lists (two-list simulation) */
    nfa_list clist, nlist;

    /* DFA cache (stack-allocated for thread safety) */
    dfa_cache_t *dfa = malloc(sizeof(dfa_cache_t));
    if (!dfa) goto no_dfa;  /* fall back to pure NFA simulation */

    memset(dfa, 0, sizeof(dfa_cache_t));

    /* Try matching from each start position */
    for (size_t start = 0; start <= n; start++) {
        if (anchor_bol && start > 0) break;

        /* Initialize state list from pattern entry */
        clist.n   = 0;
        clist.gen = ++global_gen;
        memset(state_last, 0, (size_t)prog->len * sizeof(int));

        addstate(prog, &clist, 0, 0);  /* entry is always 0; depth starts at 0 */

        /* Handle BOL assertion at this position */
        int at_bol = (start == 0) || (newline_mode && start > 0 &&
                                       text[start - 1] == '\n');
        process_assertions(prog, flags, &clist, at_bol, 0);

        /* Check for immediate match (empty pattern or all-assertions) */
        if (contains_match(prog, &clist)) {
            if (out) { out->start = text + start; out->end = text + start; }
            free(dfa);
            return 1;
        }

        if (clist.n == 0) continue;

        /* Get DFA state for this initial state set */
        int dfa_id = dfa_find_or_create(dfa, prog, &clist);

        /* Run the NFA/DFA over text[start..] */
        for (size_t i = start; i < n; i++) {
            unsigned char c = (unsigned char)text[i];

            /* Use DFA cache if available */
            if (dfa_id >= 0) {
                int next_id = dfa->states[dfa_id].next[c];
                if (next_id >= 0) {
                    dfa_id = next_id;
                } else {
                    /* Compute next NFA state set */
                    step(prog, flags, &clist, c, &nlist, 0);

                    /* Handle EOL/BOL assertions in nlist */
                    int at_eol_next = (i + 1 >= n) ||
                                      (newline_mode && text[i + 1] == '\n');
                    int at_bol_next = newline_mode && c == '\n';
                    process_assertions(prog, flags, &nlist, at_bol_next, at_eol_next);

                    int new_id = dfa_find_or_create(dfa, prog, &nlist);
                    if (dfa_id >= 0 && new_id >= 0)
                        dfa->states[dfa_id].next[c] = new_id;
                    dfa_id = new_id;

                    /* Update clist for next iteration */
                    clist = nlist;
                }

                if (dfa_id < 0) break;

                if (dfa->states[dfa_id].is_match) {
                    if (out) {
                        out->start = text + start;
                        out->end   = text + i + 1;
                    }
                    free(dfa);
                    return 1;
                }

                /* Update clist from DFA state */
                if (dfa_id >= 0) {
                    clist.n   = dfa->states[dfa_id].n_states;
                    clist.gen = ++global_gen;
                    for (int j = 0; j < clist.n; j++) {
                        clist.states[j] = dfa->states[dfa_id].nfa_states[j];
                        state_last[clist.states[j]] = clist.gen;
                    }
                }
            } else {
                /* Pure NFA fallback */
                step(prog, flags, &clist, c, &nlist, 0);
                int at_eol_next = (i + 1 >= n) ||
                                  (newline_mode && text[i + 1] == '\n');
                int at_bol_next = newline_mode && c == '\n';
                process_assertions(prog, flags, &nlist, at_bol_next, at_eol_next);

                if (contains_match(prog, &nlist)) {
                    if (out) {
                        out->start = text + start;
                        out->end   = text + i + 1;
                    }
                    free(dfa);
                    return 1;
                }

                clist = nlist;
                if (clist.n == 0) break;
            }
        }

        /* Check EOL assertion at end of string */
        if (clist.n > 0) {
            int at_eol = 1;
            process_assertions(prog, flags, &clist, 0, at_eol);
            if (contains_match(prog, &clist)) {
                if (out) { out->start = text + start; out->end = text + n; }
                free(dfa);
                return 1;
            }
        }
    }

    free(dfa);
    return 0;

no_dfa:
    /* Pure NFA simulation without DFA cache */
    for (size_t start = 0; start <= n; start++) {
        if (anchor_bol && start > 0) break;

        clist.n   = 0;
        clist.gen = ++global_gen;
        memset(state_last, 0, (size_t)prog->len * sizeof(int));
        addstate(prog, &clist, 0, 0);

        int at_bol = (start == 0) || (newline_mode && start > 0 &&
                                       text[start - 1] == '\n');
        process_assertions(prog, flags, &clist, at_bol, 0);

        if (contains_match(prog, &clist)) {
            if (out) { out->start = text + start; out->end = text + start; }
            return 1;
        }

        for (size_t i = start; i < n; i++) {
            unsigned char c = (unsigned char)text[i];
            step(prog, flags, &clist, c, &nlist, 0);
            int at_eol_next = (i + 1 >= n) ||
                              (newline_mode && text[i + 1] == '\n');
            int at_bol_next = newline_mode && c == '\n';
            process_assertions(prog, flags, &nlist, at_bol_next, at_eol_next);

            if (contains_match(prog, &nlist)) {
                if (out) { out->start = text + start; out->end = text + i + 1; }
                return 1;
            }
            clist = nlist;
            if (clist.n == 0) break;
        }

        if (clist.n > 0) {
            process_assertions(prog, flags, &clist, 0, 1);
            if (contains_match(prog, &clist)) {
                if (out) { out->start = text + start; out->end = text + n; }
                return 1;
            }
        }
    }
    return 0;
}
