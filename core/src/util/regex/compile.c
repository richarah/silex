/* compile.c — NFA program assembly helpers */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <string.h>

void mb_prog_init(mb_prog *p)
{
    p->instrs = NULL;
    p->len    = 0;
    p->cap    = 0;
}

void mb_prog_free(mb_prog *p)
{
    if (!p) return;
    /* Free any character class bitmaps */
    for (int i = 0; i < p->len; i++) {
        if ((p->instrs[i].op == I_CLASS) && p->instrs[i].arg.cc) {
            free(p->instrs[i].arg.cc);
            p->instrs[i].arg.cc = NULL;
        }
    }
    free(p->instrs);
    p->instrs = NULL;
    p->len    = 0;
    p->cap    = 0;
}

int mb_prog_emit(mb_prog *p, mb_instr instr)
{
    if (p->len >= SX_MAX_INSTRS)
        return -1;

    if (p->len >= p->cap) {
        int newcap = p->cap ? p->cap * 2 : 64;
        if (newcap > SX_MAX_INSTRS) newcap = SX_MAX_INSTRS;
        mb_instr *tmp = realloc(p->instrs, (size_t)newcap * sizeof(mb_instr));
        if (!tmp)
            return -1;
        p->instrs = tmp;
        p->cap    = newcap;
    }

    p->instrs[p->len] = instr;
    return p->len++;
}

int mb_prog_emit_class(mb_prog *p, mb_charclass *cc, instr_type_t op)
{
    /* Allocate a copy of the charclass */
    mb_charclass *copy = malloc(sizeof(mb_charclass));
    if (!copy)
        return -1;
    memcpy(copy, cc, sizeof(mb_charclass));

    mb_instr instr;
    memset(&instr, 0, sizeof(instr));
    instr.op     = op;
    instr.arg.cc = copy;

    /* mb_prog_emit returns the INSTRUCTION INDEX on success (see
     * regex_internal.h: "returns instr index or -1"), so success is >= 0, not
     * == 0. Testing `rc != 0` freed a charclass the program had just taken
     * ownership of for every instruction at a non-zero index -- a double free.
     * Only a negative return means emit failed and nothing owns `copy`. */
    int rc = mb_prog_emit(p, instr);
    if (rc < 0)
        free(copy);
    return rc;
}
