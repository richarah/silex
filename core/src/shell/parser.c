/* parser.c — shell parser: build AST from token stream */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "parser.h"
#include "lexer.h"
#include "../util/arena.h"
#include "../util/charclass.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

static node_t *parse_list(parser_t *p);
static node_t *parse_and_or(parser_t *p);
static node_t *parse_pipeline(parser_t *p);
static node_t *parse_command(parser_t *p);
static node_t *parse_simple_command(parser_t *p);
static node_t *parse_compound_command(parser_t *p);
static node_t *parse_if_cmd(parser_t *p);
static node_t *parse_while_cmd(parser_t *p);
static node_t *parse_until_cmd(parser_t *p);
static node_t *parse_for_cmd(parser_t *p);
static node_t *parse_case_cmd(parser_t *p);
static node_t *parse_subshell(parser_t *p);
static node_t *parse_brace_group(parser_t *p);
static node_t *parse_compound_list(parser_t *p);
static redir_t *parse_redirect(parser_t *p, int io_fd);
static void skip_newlines(parser_t *p);

/* -------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

void parser_init(parser_t *p, lexer_t *l, arena_t *a)
{
    p->lexer        = l;
    p->arena        = a;
    p->error        = 0;
    p->alias_lookup = NULL;
    p->alias_ctx    = NULL;
    p->pend         = NULL;
    p->pend_head    = 0;
    p->pend_count   = 0;
    p->pend_cap     = 0;
}

void parser_set_aliases(parser_t *p,
                        const char *(*lookup)(void *ctx, const char *name),
                        void *ctx)
{
    p->alias_lookup = lookup;
    p->alias_ctx    = ctx;
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ------------------------------------------------------------------------- */

void parser_error(parser_t *p, const char *msg)
{
    token_t t = lexer_peek(p->lexer);
    fprintf(stderr, "silex: parse error at line %d: %s\n", t.lineno, msg);
    p->error = 1;
}

/* -------------------------------------------------------------------------
 * Node allocation helpers
 * ------------------------------------------------------------------------- */

static node_t *alloc_node(parser_t *p, node_type_t type)
{
    node_t *n = arena_alloc(p->arena, sizeof(node_t));
    memset(n, 0, sizeof(*n));
    n->type  = type;
    n->arena = p->arena;
    return n;
}

/* -------------------------------------------------------------------------
 * Deep node copy
 *
 * Transient input (an `eval` string, a trap action, a sourced line) is parsed
 * into a scratch arena that the runner frees as soon as the command finishes,
 * so its parse tree does not pile up in the shell's persistent parse_arena.
 * The one thing that must outlive that scratch arena is a function BODY: `f() {
 * ...; }` defined inside an eval has to keep working after the eval returns.
 * add_function() therefore deep-copies the body into the persistent arena via
 * node_dup(), severing every pointer back into the soon-to-be-freed scratch.
 *
 * node->arena is set to dst for tidiness; nothing in exec reads it.
 * ------------------------------------------------------------------------- */

static char **strv_dup(arena_t *dst, char *const *v)
{
    if (!v) return NULL;
    size_t n = 0;
    while (v[n]) n++;
    char **out = arena_alloc(dst, (n + 1) * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        out[i] = arena_strdup(dst, v[i]);
    out[n] = NULL;
    return out;
}

static redir_t *redirs_dup(arena_t *dst, const redir_t *r)
{
    redir_t *head = NULL, *tail = NULL;
    for (; r; r = r->next) {
        redir_t *nr = arena_alloc(dst, sizeof(redir_t));
        nr->fd                = r->fd;
        nr->op                = r->op;
        nr->target            = r->target  ? arena_strdup(dst, r->target)  : NULL;
        nr->heredoc           = r->heredoc ? arena_strdup(dst, r->heredoc) : NULL;
        nr->heredoc_no_expand = r->heredoc_no_expand;
        nr->expanded_target   = NULL; /* a fresh copy holds the raw, unexpanded target */
        nr->next              = NULL;
        if (tail) tail->next = nr; else head = nr;
        tail = nr;
    }
    return head;
}

node_t *node_dup(arena_t *dst, const node_t *src)
{
    if (!src) return NULL;
    node_t *n = arena_alloc(dst, sizeof(node_t));
    memset(n, 0, sizeof(*n));
    n->type   = src->type;
    n->arena  = dst;
    n->lineno = src->lineno;

    switch (src->type) {
    case N_CMD:
        n->u.cmd.words   = strv_dup(dst, src->u.cmd.words);
        n->u.cmd.assigns = strv_dup(dst, src->u.cmd.assigns);
        n->u.cmd.redirs  = redirs_dup(dst, src->u.cmd.redirs);
        break;
    case N_PIPE: case N_AND: case N_OR: case N_SEQ:
    case N_NOT:  case N_ASYNC:
        n->u.binary.left  = node_dup(dst, src->u.binary.left);
        n->u.binary.right = node_dup(dst, src->u.binary.right);
        break;
    case N_SUBSHELL: case N_BRACE: case N_REDIR:
        n->u.redir_node.body   = node_dup(dst, src->u.redir_node.body);
        n->u.redir_node.redirs = redirs_dup(dst, src->u.redir_node.redirs);
        break;
    case N_IF:
        n->u.if_node.cond       = node_dup(dst, src->u.if_node.cond);
        n->u.if_node.then_b     = node_dup(dst, src->u.if_node.then_b);
        n->u.if_node.elif_chain = node_dup(dst, src->u.if_node.elif_chain);
        n->u.if_node.else_b     = node_dup(dst, src->u.if_node.else_b);
        break;
    case N_WHILE: case N_UNTIL:
        n->u.loop.cond = node_dup(dst, src->u.loop.cond);
        n->u.loop.body = node_dup(dst, src->u.loop.body);
        break;
    case N_FOR:
        n->u.for_node.var   = src->u.for_node.var ? arena_strdup(dst, src->u.for_node.var) : NULL;
        n->u.for_node.words = strv_dup(dst, src->u.for_node.words);
        n->u.for_node.body  = node_dup(dst, src->u.for_node.body);
        break;
    case N_CASE: {
        n->u.case_node.word = src->u.case_node.word ? arena_strdup(dst, src->u.case_node.word) : NULL;
        case_item_t *head = NULL, *tail = NULL;
        for (const case_item_t *it = src->u.case_node.items; it; it = it->next) {
            case_item_t *ni = arena_alloc(dst, sizeof(case_item_t));
            ni->patterns = strv_dup(dst, it->patterns);
            ni->body     = node_dup(dst, it->body);
            ni->next     = NULL;
            if (tail) tail->next = ni; else head = ni;
            tail = ni;
        }
        n->u.case_node.items = head;
        break;
    }
    case N_FUNC:
        n->u.func.name = src->u.func.name ? arena_strdup(dst, src->u.func.name) : NULL;
        n->u.func.body = node_dup(dst, src->u.func.body);
        break;
    }
    return n;
}

/* -------------------------------------------------------------------------
 * Word-array building helpers
 *
 * We maintain a dynamically-sized list of char* while parsing, then
 * copy it into the arena as a NULL-terminated array.
 * ------------------------------------------------------------------------- */

typedef struct {
    char  **items;
    size_t  count;
    size_t  cap;
} word_list_t;

static void wl_init(word_list_t *wl)
{
    wl->items = NULL;
    wl->count = 0;
    wl->cap   = 0;
}

static void wl_push(word_list_t *wl, char *w)
{
    if (wl->count + 1 >= wl->cap) {
        size_t newcap = wl->cap ? wl->cap * 2 : 8;
        char **newbuf = realloc(wl->items, newcap * sizeof(char *));
        if (!newbuf) {
            perror("silex: parser word_list realloc");
            abort();
        }
        wl->items = newbuf;
        wl->cap   = newcap;
    }
    wl->items[wl->count++] = w;
}

/* Copy word list into arena as a NULL-terminated char** */
static char **wl_to_arena(parser_t *p, word_list_t *wl)
{
    size_t   n    = wl->count;
    char   **arr  = arena_alloc(p->arena, (n + 1) * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        arr[i] = wl->items[i];
    arr[n] = NULL;
    return arr;
}

static void wl_free(word_list_t *wl)
{
    free(wl->items);
    wl->items = NULL;
    wl->count = 0;
    wl->cap   = 0;
}

/* -------------------------------------------------------------------------
 * Token-peeking helpers
 * ------------------------------------------------------------------------- */

/* Peek at next token (skips nothing). Tokens injected by alias expansion in
 * `pend` are served before the lexer's. */
static token_t peek(parser_t *p)
{
    if (p->pend_head < p->pend_count)
        return p->pend[p->pend_head];
    return lexer_peek(p->lexer);
}

/* Consume next token */
static token_t consume(parser_t *p)
{
    if (p->pend_head < p->pend_count)
        return p->pend[p->pend_head++];
    return lexer_next(p->lexer);
}

/* Prepend `n` tokens ahead of the remaining pending/lexer input. */
static void pend_prepend(parser_t *p, const token_t *toks, int n)
{
    int rem  = p->pend_count - p->pend_head;
    int need = n + rem;
    token_t *nt = arena_alloc(p->arena, (size_t)need * sizeof(token_t));
    for (int i = 0; i < n; i++)   nt[i]     = toks[i];
    for (int i = 0; i < rem; i++) nt[n + i] = p->pend[p->pend_head + i];
    p->pend       = nt;
    p->pend_count = need;
    p->pend_head  = 0;
    p->pend_cap   = need;
}

/* POSIX parse-time alias substitution at a command-word position. While the next
 * token is a plain word that names an alias (and is not already being expanded,
 * which stops `alias ls='ls -la'` and mutual loops), replace it with the tokens
 * of its value. The first token of the value becomes the new command word, so an
 * alias chain expands and an alias to a keyword/operator (`not='! '` -> TOK_BANG)
 * reshapes the parse. Call only where a command word is expected. */
/* Returns 1 when at least one alias name was consumed (so the caller can
 * recognize a command that vanished entirely, e.g. `alias empty=''; empty`). */
/* Sentinel queued after the tokens of an alias value that ended in a blank.
 * POSIX: when an alias value ends in a <blank>, the word FOLLOWING it is
 * itself checked for alias substitution. Recognised by POINTER identity, so
 * no real word token can be mistaken for it. */
static char alias_blank_marker[] = "";

static int expand_command_aliases(parser_t *p)
{
    int consumed = 0;
    if (!p->alias_lookup)
        return 0;
    const char *active[64];
    int nactive = 0;
    /* Words already scanned past (the tokens of an expanded value, and any
     * word that turned out not to be an alias). They are pushed back in
     * front of the stream when we finish. */
    token_t prefix[512];
    int np      = 0;
    int markers = 0;   /* blank-continuation sentinels still ahead */
    int eligible = 1;  /* is the token at the head in an alias-checked position? */
    int guard   = 0;

    while (guard++ < 1000) {
        token_t t = peek(p);

        /* End of a value that ended in a blank: the next word becomes
         * eligible, and the recursion guard resets -- `echo-x echo-x` must
         * expand BOTH words, which a command-wide guard would prevent. */
        if (t.type == TOK_WORD && t.text == alias_blank_marker) {
            consume(p);
            markers--;
            nactive  = 0;
            eligible = 1;
            continue;
        }

        const char *val = NULL;
        if (eligible && t.type == TOK_WORD && t.text) {
            int seen = 0;
            for (int i = 0; i < nactive; i++)
                if (strcmp(active[i], t.text) == 0) { seen = 1; break; }
            if (!seen)
                val = p->alias_lookup(p->alias_ctx, t.text);
        }

        if (!val) {
            /* Not expandable here. Step over it only to reach a pending
             * sentinel; with none ahead there is nothing left to do. Words
             * stepped over are NOT eligible, so an alias name sitting inside
             * a value (`alias e_='echo one '` with `one` also an alias) stays
             * literal, as POSIX requires. */
            if (markers > 0 && t.type != TOK_EOF && np < 512) {
                prefix[np++] = consume(p);
                eligible = 0;
                continue;
            }
            break;
        }

        if (nactive < 64)
            active[nactive++] = t.text;   /* arena-owned; outlives the parse */
        consume(p);                       /* drop the alias name */
        if (np == 0 && markers == 0)
            consumed = 1;                 /* an actual command word vanished */

        size_t vlen = strlen(val);
        int ends_blank = (vlen > 0 &&
                          (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'));

        /* Re-lex the alias value; its token text is arena-allocated (p->arena),
         * so it survives lexer_free. */
        lexer_t sub;
        lexer_init_str(&sub, val, p->arena);
        token_t buf[257];
        int n = 0;
        for (;;) {
            token_t tk = lexer_next(&sub);
            if (tk.type == TOK_EOF)
                break;
            if (n < 256)
                buf[n++] = tk;
        }
        lexer_free(&sub);

        if (ends_blank) {
            token_t m;
            memset(&m, 0, sizeof(m));
            m.type = TOK_WORD;
            m.text = alias_blank_marker;
            buf[n++] = m;
            markers++;
        }
        if (n > 0)
            pend_prepend(p, buf, n);
        /* The value's own first token is still a command word, so it stays
         * eligible: `alias hi='e_ hello'` with `alias e_='echo __'` chains. */
        eligible = 1;
    }

    /* A sentinel must never reach the parser (it would look like an empty
     * word). Only reachable if the guard above ran out. */
    while (markers > 0) {
        token_t t = peek(p);
        if (t.type == TOK_EOF)
            break;
        token_t got = consume(p);
        if (got.type == TOK_WORD && got.text == alias_blank_marker) {
            markers--;
            continue;
        }
        if (np < 512)
            prefix[np++] = got;
        else
            break;
    }

    if (np > 0)
        pend_prepend(p, prefix, np);
    return consumed;
}

int parser_at_eof(parser_t *p)
{
    skip_newlines(p);
    return peek(p).type == TOK_EOF;
}

/* Consume token of expected type or set error and return it anyway */
static token_t expect(parser_t *p, tok_type_t type, const char *errmsg)
{
    token_t t = consume(p);
    if (t.type != type) {
        parser_error(p, errmsg);
    }
    return t;
}

/* Skip over optional newlines */
static void skip_newlines(parser_t *p)
{
    while (peek(p).type == TOK_NEWLINE)
        consume(p);
}

/* -------------------------------------------------------------------------
 * Redirect default file descriptors
 * ------------------------------------------------------------------------- */

static int default_fd_for_op(tok_type_t op)
{
    switch (op) {
    case TOK_LESS:
    case TOK_DLESS:
    case TOK_DLESSDASH:
    case TOK_LESSAND:
    case TOK_LESSGREAT:
        return 0;
    case TOK_GREAT:
    case TOK_DGREAT:
    case TOK_GREATAND:
    case TOK_CLOBBER:
        return 1;
    default:
        return 1;
    }
}

/* Returns 1 if the token type is a redirect operator */
static int is_redir_op(tok_type_t t)
{
    switch (t) {
    case TOK_LESS:
    case TOK_GREAT:
    case TOK_DLESS:
    case TOK_DGREAT:
    case TOK_LESSAND:
    case TOK_GREATAND:
    case TOK_LESSGREAT:
    case TOK_DLESSDASH:
    case TOK_CLOBBER:
        return 1;
    default:
        return 0;
    }
}

/* Parse a redirect: [IO_NUMBER] redirect_op WORD
 * io_fd is the fd already parsed as a digit word (-1 if not present).
 */
static redir_t *parse_redirect(parser_t *p, int io_fd)
{
    token_t op_tok = peek(p);
    if (!is_redir_op(op_tok.type))
        return NULL;
    consume(p); /* consume the operator */

    /* Target word. Reserved words are only special in command position, so
     * `echo >in` or `cat <done` must treat them as ordinary filenames -- the
     * lexer already classified them, but their .text is intact. */
    token_t tgt = consume(p);
    if (tgt.type != TOK_WORD && tgt.type != TOK_ASSIGN &&
        !(tgt.text && tgt.text[0] &&
          tgt.type != TOK_NEWLINE && tgt.type != TOK_EOF &&
          !is_redir_op(tgt.type) &&
          tgt.type != TOK_SEMI && tgt.type != TOK_AMP &&
          tgt.type != TOK_PIPE && tgt.type != TOK_AND_AND &&
          tgt.type != TOK_OR_OR && tgt.type != TOK_LPAREN &&
          tgt.type != TOK_RPAREN)) {
        parser_error(p, "expected word after redirect operator");
        return NULL;
    }

    redir_t *r  = arena_alloc(p->arena, sizeof(redir_t));
    r->fd       = (io_fd >= 0) ? io_fd : default_fd_for_op(op_tok.type);
    r->op       = op_tok.type;
    r->target   = tgt.text;
    r->heredoc  = NULL;
    r->heredoc_no_expand = 0;
    r->expanded_target = NULL;
    r->next     = NULL;

    /* For heredocs, register the pending heredoc in the lexer.
     * body_out points to r->heredoc so the lexer can fill it in when
     * it reads the heredoc body after the next newline.
     *
     * If the delimiter is quoted ('EOF', "EOF", or \E\O\F), variable
     * expansion is suppressed in the heredoc body (POSIX). Strip the
     * quotes to get the actual delimiter string used for matching. */
    if (op_tok.type == TOK_DLESS || op_tok.type == TOK_DLESSDASH) {
        const char *raw = tgt.text;
        char *actual_delim = tgt.text;
        int no_expand = 0;

        if (raw[0] == '\'') {
            /* Single-quoted: 'EOF' — strip outer single quotes */
            size_t len = strlen(raw);
            if (len >= 2) {
                actual_delim = arena_alloc(p->arena, len - 1);
                memcpy(actual_delim, raw + 1, len - 2);
                actual_delim[len - 2] = '\0';
            }
            no_expand = 1;
        } else if (raw[0] == '"') {
            /* Double-quoted: "EOF" — strip outer double quotes */
            size_t len = strlen(raw);
            if (len >= 2) {
                actual_delim = arena_alloc(p->arena, len - 1);
                memcpy(actual_delim, raw + 1, len - 2);
                actual_delim[len - 2] = '\0';
            }
            no_expand = 1;
        } else if (raw[0] == '\\') {
            /* Backslash-escaped: \EOF — remove backslashes */
            size_t len = strlen(raw);
            actual_delim = arena_alloc(p->arena, len + 1);
            const char *src = raw;
            char *dst = actual_delim;
            while (*src) {
                if (*src == '\\' && *(src + 1) != '\0')
                    src++;
                *dst++ = *src++;
            }
            *dst = '\0';
            no_expand = 1;
        }

        heredoc_pending_t *hp = arena_alloc(p->arena, sizeof(heredoc_pending_t));
        hp->delim      = actual_delim;
        hp->strip_tabs = (op_tok.type == TOK_DLESSDASH) ? 1 : 0;
        hp->no_expand  = no_expand;
        hp->body_out   = &r->heredoc;
        /* Append at the TAIL: bodies follow the command line in the order
         * their << operators appeared, so a LIFO queue read them in reverse
         * and mismatched the delimiters -- `cat <<EOF1 <<EOF2` scanned for
         * EOF2 first and swallowed EOF1's body and terminator with it. */
        hp->next = NULL;
        if (!p->lexer->heredocs) {
            p->lexer->heredocs = hp;
        } else {
            heredoc_pending_t *tail = p->lexer->heredocs;
            while (tail->next)
                tail = tail->next;
            tail->next = hp;
        }

        r->heredoc_no_expand = no_expand;
    }

    return r;
}

/* -------------------------------------------------------------------------
 * is_io_number: detect pure-digit word that precedes a redirect operator
 * e.g., "2>" → fd=2, ">&"
 * Returns the fd value or -1.
 * ------------------------------------------------------------------------- */
static int try_io_number(const char *text, tok_type_t next_op)
{
    if (!is_redir_op(next_op))
        return -1;
    if (!text)
        return -1;
    /* All chars must be digits */
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }
    return atoi(text);
}

/* -------------------------------------------------------------------------
 * Simple command parsing
 *
 * A simple command is a sequence of:
 *   - ASSIGN tokens   (NAME=VALUE, before first non-assign word)
 *   - WORD tokens     (command and its arguments)
 *   - Redirect specs  ([IO_NUMBER] redirect_op WORD)
 *
 * We allow assigns, words and redirects interleaved as POSIX permits.
 * ------------------------------------------------------------------------- */
static node_t *parse_simple_command(parser_t *p)
{
    int start_lineno = peek(p).lineno;
    word_list_t assigns, words;
    wl_init(&assigns);
    wl_init(&words);

    redir_t  *redir_head = NULL;
    redir_t **redir_tail = &redir_head;

    int seen_cmd_word = 0; /* have we seen a non-assign WORD yet */

    for (;;) {
        token_t t = peek(p);

        if (t.type == TOK_ASSIGN) {
            consume(p);
            if (!seen_cmd_word) {
                wl_push(&assigns, t.text);
            } else {
                /* After cmd word (e.g. `local x=5`, `export X=val`): treat as arg */
                wl_push(&words, t.text);
            }
            continue;
        }

        if (t.type == TOK_WORD) {
            /* Check if it could be an IO number followed by a redirect */
            token_t saved = consume(p);  /* consume the word */
            token_t nxt   = peek(p);
            int fd = try_io_number(saved.text, nxt.type);
            if (fd >= 0) {
                redir_t *r = parse_redirect(p, fd);
                if (r) {
                    *redir_tail = r;
                    redir_tail  = &r->next;
                    continue;
                }
            }
            /* Not an IO number: it's a regular word */
            seen_cmd_word = 1;
            wl_push(&words, saved.text);
            continue;
        }

        if (is_redir_op(t.type)) {
            redir_t *r = parse_redirect(p, -1);
            if (r && !p->error) {
                *redir_tail = r;
                redir_tail  = &r->next;
            }
            continue;
        }

        /* POSIX: reserved words are only keywords at command-name position.
         * After the command name is established, treat keyword tokens as
         * plain word arguments (e.g. 'echo done', 'echo fi', 'echo then'). */
        if (seen_cmd_word && t.text != NULL) {
            consume(p);
            wl_push(&words, t.text);
            continue;
        }

        /* Nothing else belongs to simple command */
        break;
    }

    if (assigns.count == 0 && words.count == 0 && redir_head == NULL) {
        wl_free(&assigns);
        wl_free(&words);
        return NULL;
    }

    node_t *n       = alloc_node(p, N_CMD);
    n->lineno       = start_lineno;
    n->u.cmd.words   = wl_to_arena(p, &words);
    n->u.cmd.assigns = wl_to_arena(p, &assigns);
    n->u.cmd.redirs  = redir_head;

    wl_free(&assigns);
    wl_free(&words);
    return n;
}

/* -------------------------------------------------------------------------
 * Compound list: newline* list newline*
 * ------------------------------------------------------------------------- */
static node_t *parse_compound_list(parser_t *p)
{
    skip_newlines(p);
    node_t *n = parse_list(p);
    skip_newlines(p);
    return n;
}

/* -------------------------------------------------------------------------
 * if command:
 *   IF compound_list THEN compound_list
 *   (ELIF compound_list THEN compound_list)*
 *   (ELSE compound_list)?
 *   FI
 * ------------------------------------------------------------------------- */
static node_t *parse_if_cmd(parser_t *p)
{
    /* IF already consumed by caller */
    node_t *cond = parse_compound_list(p);
    /* Empty condition with no error means `if` ran into EOF -- a syntax error,
     * so `eval 'if'` fails rather than silently returning 0. See parse_subshell. */
    if (!cond && !p->error)
        parser_error(p, "expected condition after 'if'");
    if (!cond || p->error) return NULL;

    expect(p, TOK_THEN, "expected 'then'");
    if (p->error) return NULL;

    node_t *then_b = parse_compound_list(p);
    if (!then_b || p->error) return NULL;

    node_t *elif_chain = NULL;
    node_t *else_b     = NULL;

    /* Chain of ELIF ... THEN ... */
    while (peek(p).type == TOK_ELIF) {
        consume(p); /* ELIF */
        node_t *elif_cond = parse_compound_list(p);
        if (!elif_cond || p->error) return NULL;

        expect(p, TOK_THEN, "expected 'then' after elif condition");
        if (p->error) return NULL;

        node_t *elif_then = parse_compound_list(p);
        if (!elif_then || p->error) return NULL;

        /* Build an if node for this elif branch */
        node_t *elif_node       = alloc_node(p, N_IF);
        elif_node->u.if_node.cond       = elif_cond;
        elif_node->u.if_node.then_b     = elif_then;
        elif_node->u.if_node.elif_chain = NULL;
        elif_node->u.if_node.else_b     = NULL;

        /* Attach as elif_chain; nest further elifs inside this node's else */
        if (!elif_chain) {
            elif_chain = elif_node;
        } else {
            /* Find the last node in the chain and append */
            node_t *cur = elif_chain;
            while (cur->u.if_node.elif_chain)
                cur = cur->u.if_node.elif_chain;
            cur->u.if_node.elif_chain = elif_node;
        }
    }

    if (peek(p).type == TOK_ELSE) {
        consume(p); /* ELSE */
        else_b = parse_compound_list(p);
        if (!else_b || p->error) return NULL;
    }

    /* Attach else_b to the last node in elif_chain (not the root if-node) */
    if (elif_chain && else_b) {
        node_t *cur = elif_chain;
        while (cur->u.if_node.elif_chain)
            cur = cur->u.if_node.elif_chain;
        cur->u.if_node.else_b = else_b;
        else_b = NULL;  /* root node gets NULL */
    }

    expect(p, TOK_FI, "expected 'fi'");
    if (p->error) return NULL;

    node_t *n               = alloc_node(p, N_IF);
    n->u.if_node.cond       = cond;
    n->u.if_node.then_b     = then_b;
    n->u.if_node.elif_chain = elif_chain;
    n->u.if_node.else_b     = else_b;
    return n;
}

/* -------------------------------------------------------------------------
 * while command: WHILE compound_list DO compound_list DONE
 * ------------------------------------------------------------------------- */
static node_t *parse_while_cmd(parser_t *p)
{
    /* WHILE already consumed */
    node_t *cond = parse_compound_list(p);
    if (!cond && !p->error)
        parser_error(p, "expected condition after 'while'");
    if (!cond || p->error) return NULL;

    expect(p, TOK_DO, "expected 'do' in while");
    if (p->error) return NULL;

    node_t *body = parse_compound_list(p);
    if (!body || p->error) return NULL;

    expect(p, TOK_DONE, "expected 'done'");
    if (p->error) return NULL;

    node_t *n         = alloc_node(p, N_WHILE);
    n->u.loop.cond    = cond;
    n->u.loop.body    = body;
    return n;
}

/* -------------------------------------------------------------------------
 * until command: UNTIL compound_list DO compound_list DONE
 * ------------------------------------------------------------------------- */
static node_t *parse_until_cmd(parser_t *p)
{
    /* UNTIL already consumed */
    node_t *cond = parse_compound_list(p);
    if (!cond && !p->error)
        parser_error(p, "expected condition after 'until'");
    if (!cond || p->error) return NULL;

    expect(p, TOK_DO, "expected 'do' in until");
    if (p->error) return NULL;

    node_t *body = parse_compound_list(p);
    if (!body || p->error) return NULL;

    expect(p, TOK_DONE, "expected 'done'");
    if (p->error) return NULL;

    node_t *n         = alloc_node(p, N_UNTIL);
    n->u.loop.cond    = cond;
    n->u.loop.body    = body;
    return n;
}

/* -------------------------------------------------------------------------
 * for command: FOR WORD (IN WORD*)? SEMI? DO compound_list DONE
 * ------------------------------------------------------------------------- */
static node_t *parse_for_cmd(parser_t *p)
{
    /* FOR already consumed */
    token_t var_tok = expect(p, TOK_WORD, "expected variable name in for");
    if (p->error) return NULL;

    char       *var   = var_tok.text;
    /* The loop variable must be a NAME (POSIX grammar): `for i.j in ...` is a
     * syntax error, not a loop over a variable called "i.j". Without the check
     * the loop ran and the assignment silently went nowhere. */
    if (var) {
        const char *q = var;
        int ok = is_alpha_underscore((unsigned char)*q);
        for (; ok && *q; q++)
            if (!is_name_char((unsigned char)*q)) ok = 0;
        if (!ok) {
            parser_error(p, "bad for loop variable");
            return NULL;
        }
    }
    word_list_t words;
    wl_init(&words);
    int saw_in = 0;

    /* Optional: IN word-list */
    skip_newlines(p);
    if (peek(p).type == TOK_IN) {
        saw_in = 1;
        consume(p); /* IN */
        /* The word list may hold tokens that lex as reserved words or `{`/`}`/`!`
         * -- `for v in ! { } case do done ... while; do` (modernish builtin.t)
         * lists every reserved word as data. Reserved words are only special in
         * command position, so accept any text-carrying token here and stop only
         * at a real list terminator. The `;`/newline that precedes the loop's
         * `do` is such a terminator, so the body's `do` is never swallowed. */
        for (;;) {
            tok_type_t t = peek(p).type;
            if (t == TOK_SEMI  || t == TOK_NEWLINE || t == TOK_DSEMI ||
                t == TOK_AMP   || t == TOK_PIPE    || t == TOK_AND_AND ||
                t == TOK_OR_OR || t == TOK_LPAREN  || t == TOK_RPAREN ||
                t == TOK_EOF)
                break;
            token_t w = consume(p);
            if (w.text == NULL) break;
            wl_push(&words, w.text);
        }
    }

    /* Optional semicolon or newline before DO */
    if (peek(p).type == TOK_SEMI || peek(p).type == TOK_NEWLINE)
        consume(p);
    skip_newlines(p);

    expect(p, TOK_DO, "expected 'do' in for");
    if (p->error) { wl_free(&words); return NULL; }

    node_t *body = parse_compound_list(p);
    if (!body || p->error) { wl_free(&words); return NULL; }

    expect(p, TOK_DONE, "expected 'done'");
    if (p->error) { wl_free(&words); return NULL; }

    node_t *n           = alloc_node(p, N_FOR);
    n->u.for_node.var   = var;
    /* NULL words means "no in clause" (iterate $@); non-NULL means explicit list */
    n->u.for_node.words = saw_in ? wl_to_arena(p, &words) : NULL;
    n->u.for_node.body  = body;
    wl_free(&words);
    return n;
}

/* -------------------------------------------------------------------------
 * case command: CASE WORD IN (pattern ')' compound_list ';;')* ESAC
 * ------------------------------------------------------------------------- */
static node_t *parse_case_cmd(parser_t *p)
{
    /* CASE already consumed. The subject word may lex as a reserved word
     * (`case in in ...`, `case do in ...`): reserved words are only special in
     * command position, so accept any token that carries text. */
    token_t word_tok = consume(p);
    if (word_tok.text == NULL) {
        parser_error(p, "expected word in case");
        return NULL;
    }

    skip_newlines(p);
    expect(p, TOK_IN, "expected 'in' in case");
    if (p->error) return NULL;

    skip_newlines(p);

    case_item_t  *item_head = NULL;
    case_item_t **item_tail = &item_head;

    while (peek(p).type != TOK_ESAC && peek(p).type != TOK_EOF) {
        /* Collect pattern(s) separated by '|' until ')' */
        word_list_t pats;
        wl_init(&pats);

        /* Optional leading '(' */
        if (peek(p).type == TOK_LPAREN)
            consume(p);

        /* One or more '|'-separated patterns. A pattern alternative may lex as a
         * reserved word or operator token -- `( in )`, `( ! )`, `( { )`,
         * `( -* | \( | ! )` (modernish find.mm) -- so accept any token that
         * carries text; only structural tokens ')' '|' ';;' etc. have none. */
        for (;;) {
            if (peek(p).text == NULL) {
                parser_error(p, "expected pattern in case item");
                wl_free(&pats);
                return NULL;
            }
            token_t pat = consume(p);
            wl_push(&pats, pat.text);
            if (peek(p).type == TOK_PIPE) {
                consume(p); /* | */
            } else {
                break;
            }
        }

        expect(p, TOK_RPAREN, "expected ')' after case pattern");
        if (p->error) { wl_free(&pats); return NULL; }

        skip_newlines(p);

        /* Body: compound_list until ;; or esac */
        node_t *body = NULL;
        if (peek(p).type != TOK_DSEMI && peek(p).type != TOK_ESAC) {
            body = parse_compound_list(p);
            if (p->error) { wl_free(&pats); return NULL; }
        }

        if (peek(p).type == TOK_DSEMI)
            consume(p);

        skip_newlines(p);

        case_item_t *item  = arena_alloc(p->arena, sizeof(case_item_t));
        item->patterns     = wl_to_arena(p, &pats);
        item->body         = body;
        item->next         = NULL;
        *item_tail         = item;
        item_tail          = &item->next;
        wl_free(&pats);
    }

    expect(p, TOK_ESAC, "expected 'esac'");
    if (p->error) return NULL;

    node_t *n             = alloc_node(p, N_CASE);
    n->u.case_node.word   = word_tok.text;
    n->u.case_node.items  = item_head;
    return n;
}

/* -------------------------------------------------------------------------
 * subshell: '(' compound_list ')'
 * ------------------------------------------------------------------------- */
static node_t *parse_subshell(parser_t *p)
{
    /* LPAREN already consumed */
    node_t *body = parse_compound_list(p);
    /* A NULL body with no error means the list was empty -- `(` hit EOF (or `)`)
     * with nothing inside. That is a syntax error, not a clean parse. Without
     * flagging it, `eval '('` parsed to nothing and returned 0, so modernish's
     * FTL_EVALERR check (eval must fail on a syntax error) wrongly triggered. */
    if (!body && !p->error)
        parser_error(p, "expected command inside subshell");
    if (!body || p->error) return NULL;

    expect(p, TOK_RPAREN, "expected ')' to close subshell");
    if (p->error) return NULL;

    node_t *n               = alloc_node(p, N_SUBSHELL);
    n->u.redir_node.body    = body;
    n->u.redir_node.redirs  = NULL;
    return n;
}

/* -------------------------------------------------------------------------
 * brace group: '{' compound_list '}'
 * ------------------------------------------------------------------------- */
static node_t *parse_brace_group(parser_t *p)
{
    /* LBRACE already consumed */
    node_t *body = parse_compound_list(p);
    if (!body && !p->error)
        parser_error(p, "expected command inside brace group");
    if (!body || p->error) return NULL;

    expect(p, TOK_RBRACE, "expected '}' to close brace group");
    if (p->error) return NULL;

    node_t *n               = alloc_node(p, N_BRACE);
    n->u.redir_node.body    = body;
    n->u.redir_node.redirs  = NULL;
    return n;
}

/* -------------------------------------------------------------------------
 * Compound command dispatcher
 * ------------------------------------------------------------------------- */
static node_t *parse_compound_command(parser_t *p)
{
    token_t t = peek(p);
    switch (t.type) {
    case TOK_IF:
        consume(p);
        return parse_if_cmd(p);
    case TOK_WHILE:
        consume(p);
        return parse_while_cmd(p);
    case TOK_UNTIL:
        consume(p);
        return parse_until_cmd(p);
    case TOK_FOR:
        consume(p);
        return parse_for_cmd(p);
    case TOK_CASE:
        consume(p);
        return parse_case_cmd(p);
    case TOK_LPAREN:
        consume(p);
        return parse_subshell(p);
    case TOK_LBRACE:
        consume(p);
        return parse_brace_group(p);
    default:
        return NULL;
    }
}

/* -------------------------------------------------------------------------
 * Function definition: WORD '(' ')' compound_command
 * Detected from parse_command when we see WORD LPAREN RPAREN.
 * The WORD token is passed in as func_name.
 * ------------------------------------------------------------------------- */
/* Collect a run of trailing redirects (e.g. `>f 2>&1`) into a redir_t list.
 * Returns NULL if there are none. Shared by compound commands and function
 * definitions. */
static redir_t *collect_trailing_redirects(parser_t *p)
{
    redir_t  *rhead = NULL;
    redir_t **rtail = &rhead;
    for (;;) {
        token_t rt = peek(p);
        int fd = -1;
        if (is_redir_op(rt.type)) {
            /* direct redirect op: use default fd */
        } else if (rt.type == TOK_WORD) {
            token_t w = consume(p);
            fd = try_io_number(w.text, peek(p).type);
            if (fd < 0)
                break; /* not an io number; stop collecting */
        } else {
            break;
        }
        redir_t *r = parse_redirect(p, fd);
        if (r && !p->error) {
            *rtail = r;
            rtail  = &r->next;
        }
        if (p->error) break;
    }
    return rhead;
}

/* Wrap `body` in an N_REDIR node if any trailing redirects follow. For a
 * function definition, POSIX says these redirects apply on every call, which is
 * exactly what wrapping the body achieves: the N_REDIR re-applies them each time
 * the body executes. Without this, `fn() { ...; } >f` silently dropped the
 * redirect (modernish FTL_FNREDIR). */
static node_t *wrap_trailing_redirects(parser_t *p, node_t *body)
{
    redir_t *rhead = collect_trailing_redirects(p);
    if (p->error) return NULL;
    if (rhead) {
        node_t *wrap              = alloc_node(p, N_REDIR);
        wrap->u.redir_node.body   = body;
        wrap->u.redir_node.redirs = rhead;
        return wrap;
    }
    return body;
}

static node_t *parse_function_def(parser_t *p, char *func_name)
{
    /* '(' and ')' already consumed by caller */
    skip_newlines(p);
    node_t *body = parse_compound_command(p);
    if (!body || p->error) {
        parser_error(p, "expected compound command as function body");
        return NULL;
    }
    body = wrap_trailing_redirects(p, body);
    if (!body) return NULL;

    node_t *n         = alloc_node(p, N_FUNC);
    n->u.func.name    = func_name;
    n->u.func.body    = body;
    return n;
}

/* -------------------------------------------------------------------------
 * Command: simple_command | compound_command | function_def
 * Also handles trailing redirects on compound commands.
 * ------------------------------------------------------------------------- */
static node_t *parse_command(parser_t *p)
{
    token_t t = peek(p);

    /* Handle 'function NAME compound_command' syntax */
    if (t.type == TOK_FUNCTION) {
        consume(p); /* FUNCTION */
        token_t name_tok = expect(p, TOK_WORD, "expected function name");
        if (p->error) return NULL;

        /* Optional '()' after function name */
        if (peek(p).type == TOK_LPAREN) {
            consume(p); /* ( */
            expect(p, TOK_RPAREN, "expected ')' after function name");
            if (p->error) return NULL;
        }

        skip_newlines(p);
        node_t *body = parse_compound_command(p);
        if (!body || p->error) {
            parser_error(p, "expected compound command as function body");
            return NULL;
        }
        body = wrap_trailing_redirects(p, body);
        if (!body) return NULL;
        node_t *n      = alloc_node(p, N_FUNC);
        n->u.func.name = name_tok.text;
        n->u.func.body = body;
        return n;
    }

    /* Compound command */
    {
        node_t *compound = parse_compound_command(p);
        if (compound)
            return wrap_trailing_redirects(p, compound);
    }

    /* Simple command (handles function detection too) */
    t = peek(p);
    if (t.type == TOK_WORD || t.type == TOK_ASSIGN || is_redir_op(t.type)) {
        int cmd_lineno = t.lineno;   /* $LINENO for this command */
        /* Look ahead for WORD '(' ')' function definition */
        if (t.type == TOK_WORD) {
            /* We need two more tokens of lookahead.
             * Strategy: consume the word, then check for '(' ')'. */
            token_t name_tok = consume(p);
            token_t next1    = peek(p);

            if (next1.type == TOK_LPAREN) {
                consume(p); /* ( */
                token_t next2 = peek(p);
                if (next2.type == TOK_RPAREN) {
                    consume(p); /* ) */
                    /* This is a function definition */
                    return parse_function_def(p, name_tok.text);
                }
                /* Not a function def; push back the '(' and treat as simple cmd */
                /* We can't truly push back a token, so build a synthetic simple
                 * command starting with name_tok, treating '(' as error or word.
                 * In practice, 'name ( args )' isn't valid POSIX — treat '(' as
                 * start of a subshell that is part of a pipeline; put name back
                 * by constructing the simple command manually with what we have. */
                parser_error(p, "expected ')' in function definition");
                return NULL;
            }

            /* Not a function def: build simple command manually, having already
             * consumed name_tok. */
            word_list_t assigns, words;
            wl_init(&assigns);
            wl_init(&words);
            redir_t  *redir_head = NULL;
            redir_t **redir_tail = &redir_head;

            /* name_tok is the first word of the command -- unless it is an
             * IO number introducing a redirect. This branch consumed the word
             * up front (to look for `name ( )`), so without this check a
             * command that is ONLY redirects, like a bare `2>&1` or
             * `2>/dev/null`, ran a command named "2". */
            int first_fd = (name_tok.type == TOK_WORD)
                         ? try_io_number(name_tok.text, peek(p).type) : -1;
            if (first_fd >= 0) {
                redir_t *r = parse_redirect(p, first_fd);
                if (r && !p->error) {
                    *redir_tail = r;
                    redir_tail  = &r->next;
                }
            } else if (name_tok.type == TOK_ASSIGN) {
                wl_push(&assigns, name_tok.text);
            } else {
                wl_push(&words, name_tok.text);
            }

            /* Continue collecting remaining parts of the simple command */
            for (;;) {
                token_t cur = peek(p);

                if (cur.type == TOK_ASSIGN) {
                    consume(p);
                    if (words.count == 0) {
                        wl_push(&assigns, cur.text);
                    } else {
                        wl_push(&words, cur.text);
                    }
                    continue;
                }

                if (cur.type == TOK_WORD) {
                    token_t saved = consume(p);
                    token_t nxt   = peek(p);
                    int fd = try_io_number(saved.text, nxt.type);
                    if (fd >= 0) {
                        redir_t *r = parse_redirect(p, fd);
                        if (r) {
                            *redir_tail = r;
                            redir_tail  = &r->next;
                        }
                    } else {
                        wl_push(&words, saved.text);
                    }
                    continue;
                }

                if (is_redir_op(cur.type)) {
                    redir_t *r = parse_redirect(p, -1);
                    if (r && !p->error) {
                        *redir_tail = r;
                        redir_tail  = &r->next;
                    }
                    continue;
                }

                /* POSIX: keywords in argument position treated as words */
                if (words.count > 0 && cur.text != NULL) {
                    consume(p);
                    wl_push(&words, cur.text);
                    continue;
                }

                /* `(` cannot follow a command word: `echo a(b)` is a syntax
                 * error, not `echo a` followed by the subshell `(b)`. Only a
                 * function definition's `name()` may put a paren there, and
                 * that was handled before this loop. */
                if (cur.type == TOK_LPAREN && (words.count > 0 || assigns.count > 0)) {
                    parser_error(p, "unexpected '('");
                    wl_free(&assigns);
                    wl_free(&words);
                    return NULL;
                }

                break;
            }

            node_t *n        = alloc_node(p, N_CMD);
            n->lineno        = cmd_lineno;
            n->u.cmd.words   = wl_to_arena(p, &words);
            n->u.cmd.assigns = wl_to_arena(p, &assigns);
            n->u.cmd.redirs  = redir_head;
            wl_free(&assigns);
            wl_free(&words);
            return n;
        }

        /* starts with ASSIGN or redirect */
        return parse_simple_command(p);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * pipeline: '!'? command ('|' command)*
 * ------------------------------------------------------------------------- */
static node_t *parse_pipeline(parser_t *p)
{
    int negate = 0;
    /* Command-word position: expand aliases first, so `not`->`! ` is seen as the
     * TOK_BANG below rather than a command named "not". */
    int alias_used = expand_command_aliases(p);
    if (peek(p).type == TOK_BANG) {
        consume(p);
        negate = 1;
        alias_used |= expand_command_aliases(p);   /* the negated command's word */
    }

    /* An alias that expanded to nothing at all (`alias empty=''; empty`) is a
     * valid empty command: it runs nothing and succeeds. Without this the
     * parser demanded a command word and errored on the newline. Only applies
     * when an alias really was consumed and only terminators remain --
     * `empty > file` still parses as a redirect-only command below. */
    tok_type_t after = peek(p).type;
    if (alias_used &&
        (after == TOK_NEWLINE || after == TOK_EOF || after == TOK_SEMI ||
         after == TOK_AMP || after == TOK_AND_AND || after == TOK_OR_OR)) {
        node_t *empty = alloc_node(p, N_CMD);
        empty->u.cmd.words   = arena_alloc(p->arena, sizeof(char *));
        empty->u.cmd.words[0] = NULL;
        empty->u.cmd.assigns = NULL;
        empty->u.cmd.redirs  = NULL;
        return empty;
    }

    node_t *left = parse_command(p);
    if (!left || p->error) return NULL;

    while (peek(p).type == TOK_PIPE) {
        consume(p); /* | */
        skip_newlines(p);
        expand_command_aliases(p);   /* the next pipeline stage's word */
        node_t *right = parse_command(p);
        if (!right || p->error) {
            parser_error(p, "expected command after '|'");
            return NULL;
        }
        node_t *pipe      = alloc_node(p, N_PIPE);
        pipe->u.binary.left  = left;
        pipe->u.binary.right = right;
        left = pipe;
    }

    if (negate) {
        node_t *not_node         = alloc_node(p, N_NOT);
        not_node->u.binary.left  = left;
        not_node->u.binary.right = NULL;
        return not_node;
    }

    return left;
}

/* -------------------------------------------------------------------------
 * and_or: pipeline (('&&' | '||') pipeline)*
 * ------------------------------------------------------------------------- */
static node_t *parse_and_or(parser_t *p)
{
    node_t *left = parse_pipeline(p);
    if (!left || p->error) return NULL;

    for (;;) {
        tok_type_t t = peek(p).type;
        if (t != TOK_AND_AND && t != TOK_OR_OR)
            break;

        consume(p);
        skip_newlines(p);

        node_t *right = parse_pipeline(p);
        if (!right || p->error) {
            parser_error(p, "expected pipeline after '&&' or '||'");
            return NULL;
        }

        node_type_t ntype = (t == TOK_AND_AND) ? N_AND : N_OR;
        node_t *n          = alloc_node(p, ntype);
        n->u.binary.left   = left;
        n->u.binary.right  = right;
        left = n;
    }

    return left;
}

/* -------------------------------------------------------------------------
 * list: and_or (separator and_or)*
 * separator: ';' | NEWLINE | '&'
 * ------------------------------------------------------------------------- */
/* list : and_or ( ( ';' | '&' ) and_or )* ( ';' | '&' )?
 *
 * A '&' makes ONLY the and_or it immediately follows asynchronous, then that
 * unit is folded into the accumulated left-associative sequence. The previous
 * version wrapped the whole accumulated list in N_ASYNC on each '&', so
 * `sleep 1 & sleep 2 &` became one async of a sequence -- both ran, but the job
 * table saw a single "(compound command)" job instead of two. */
static node_t *parse_list(parser_t *p)
{
    node_t *unit = parse_and_or(p);
    if (!unit || p->error) return NULL;

    node_t *result = NULL;   /* accumulated sequence; NULL until we have >1 unit */

    for (;;) {
        tok_type_t t = peek(p).type;

        if (t == TOK_AMP) {
            consume(p);
            node_t *a         = alloc_node(p, N_ASYNC);
            a->u.binary.left  = unit;
            a->u.binary.right = NULL;
            unit = a;
        } else if (t == TOK_SEMI || t == TOK_NEWLINE) {
            consume(p);
        } else {
            break;   /* no separator: `unit` is the final unit of the list */
        }

        /* Fold this (possibly async-wrapped) unit into the sequence. */
        if (!result) {
            result = unit;
        } else {
            node_t *seq         = alloc_node(p, N_SEQ);
            seq->u.binary.left  = result;
            seq->u.binary.right = unit;
            result = seq;
        }
        unit = NULL;

        skip_newlines(p);
        tok_type_t nxt = peek(p).type;
        if (nxt == TOK_EOF   || nxt == TOK_FI    || nxt == TOK_DONE  ||
            nxt == TOK_ESAC  || nxt == TOK_THEN   || nxt == TOK_ELSE  ||
            nxt == TOK_ELIF  || nxt == TOK_DO     || nxt == TOK_RPAREN ||
            nxt == TOK_RBRACE || nxt == TOK_DSEMI)
            break;

        unit = parse_and_or(p);
        if (!unit || p->error) break;
    }

    /* A trailing unit with no separator after it. */
    if (unit) {
        if (!result) {
            result = unit;
        } else {
            node_t *seq         = alloc_node(p, N_SEQ);
            seq->u.binary.left  = result;
            seq->u.binary.right = unit;
            result = seq;
        }
    }

    return result;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Parse one logical input line: a `;`/`&`-separated list of and_ors, ending at
 * an unquoted top-level NEWLINE (or EOF).
 *
 * This is like parse_list EXCEPT a newline TERMINATES the list instead of being
 * a separator that continues it. That distinction is what lets the shell driver
 * execute a line before the NEXT line is parsed — POSIX alias semantics. An
 * alias, `use`, or function DEFINED on one line must be visible on the next
 * (modernish's `use var/loop` then a later `LOOP ...; DONE` block, or its
 * per-line `alias not='! '`). Batch-parsing the whole file up front defined
 * those too late. Matching dash, a `;`-separated alias on the SAME line is still
 * not yet in effect (`alias z=cmd; z` — z is parsed before the alias runs).
 *
 * A line still spans multiple PHYSICAL lines whenever a construct is
 * unterminated: a compound command (if/for/while/case/{...}/subshell) consumes
 * newlines through its own parse_compound_list, and an alias like LOOP that
 * opens a brace group a later line's DONE closes keeps parse_and_or reading
 * across newlines until the braces balance. Only a newline reached at the TOP
 * level ends the line. */
static node_t *parse_line(parser_t *p)
{
    node_t *unit = parse_and_or(p);
    if (!unit || p->error) return NULL;

    node_t *result = NULL;   /* accumulated sequence; NULL until we have >1 unit */

    for (;;) {
        tok_type_t t = peek(p).type;

        if (t == TOK_AMP) {
            consume(p);
            node_t *a         = alloc_node(p, N_ASYNC);
            a->u.binary.left  = unit;
            a->u.binary.right = NULL;
            unit = a;
        } else if (t == TOK_SEMI) {
            consume(p);
        } else {
            break;   /* NEWLINE / EOF / stray terminator: the line ends here */
        }

        /* Fold this (possibly async-wrapped) unit into the sequence. */
        if (!result) {
            result = unit;
        } else {
            node_t *seq         = alloc_node(p, N_SEQ);
            seq->u.binary.left  = result;
            seq->u.binary.right = unit;
            result = seq;
        }
        unit = NULL;

        /* A separator followed by a line/scope terminator ends the line. We do
         * NOT skip_newlines here: a bare newline must stop the line (that is the
         * whole point), and the leftover terminators below can only legally
         * follow when this list is the body of a compound command — in which
         * case that caller, not this top-level driver, is what parses us. */
        tok_type_t nxt = peek(p).type;
        if (nxt == TOK_NEWLINE || nxt == TOK_EOF   || nxt == TOK_FI    ||
            nxt == TOK_DONE    || nxt == TOK_ESAC  || nxt == TOK_THEN  ||
            nxt == TOK_ELSE    || nxt == TOK_ELIF  || nxt == TOK_DO    ||
            nxt == TOK_RPAREN  || nxt == TOK_RBRACE || nxt == TOK_DSEMI)
            break;

        unit = parse_and_or(p);
        if (!unit || p->error) break;
    }

    /* A trailing unit with no separator after it. */
    if (unit) {
        if (!result) {
            result = unit;
        } else {
            node_t *seq         = alloc_node(p, N_SEQ);
            seq->u.binary.left  = result;
            seq->u.binary.right = unit;
            result = seq;
        }
    }

    return result;
}

/* Parse one logical line (see parse_line). Returns NULL at EOF. */
node_t *parser_parse(parser_t *p)
{
    /* Skip leading newlines and semicolons between top-level commands.
     * This allows the shell loop to call parser_parse repeatedly on input
     * like "X=hello; echo $X" — after X=hello is parsed the ';' must be
     * consumed before trying to parse the next command. */
    for (;;) {
        tok_type_t t = peek(p).type;
        if (t == TOK_NEWLINE || t == TOK_SEMI)
            consume(p);
        else
            break;
    }
    token_t t = peek(p);
    if (t.type == TOK_EOF)
        return NULL;

    node_t *result = parse_line(p);

    /* If parse_line returned NULL but we're not at EOF, it's a parse error */
    if (!result && !p->error && peek(p).type != TOK_EOF) {
        parser_error(p, "unexpected token");
    }

    return result;
}

/* Parse a complete program (list until EOF) */
node_t *parser_parse_list(parser_t *p)
{
    skip_newlines(p);
    token_t t = peek(p);
    if (t.type == TOK_EOF)
        return NULL;

    node_t *result = parse_list(p);

    /* If parse_list returned NULL but we're not at EOF, it's a parse error */
    if (!result && !p->error && peek(p).type != TOK_EOF) {
        parser_error(p, "unexpected token");
    }

    return result;
}
