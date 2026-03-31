/* parser.c — shell parser: build AST from token stream */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "parser.h"
#include "lexer.h"
#include "../util/arena.h"

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
    p->lexer = l;
    p->arena = a;
    p->error = 0;
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

/* Peek at next token (skips nothing) */
static token_t peek(parser_t *p)
{
    return lexer_peek(p->lexer);
}

/* Consume next token */
static token_t consume(parser_t *p)
{
    return lexer_next(p->lexer);
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

    /* Target word */
    token_t tgt = consume(p);
    if (tgt.type != TOK_WORD && tgt.type != TOK_ASSIGN) {
        parser_error(p, "expected word after redirect operator");
        return NULL;
    }

    redir_t *r  = arena_alloc(p->arena, sizeof(redir_t));
    r->fd       = (io_fd >= 0) ? io_fd : default_fd_for_op(op_tok.type);
    r->op       = op_tok.type;
    r->target   = tgt.text;
    r->heredoc  = NULL;
    r->heredoc_no_expand = 0;
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
        hp->next       = p->lexer->heredocs;
        p->lexer->heredocs = hp;

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
    word_list_t words;
    wl_init(&words);
    int saw_in = 0;

    /* Optional: IN word-list */
    skip_newlines(p);
    if (peek(p).type == TOK_IN) {
        saw_in = 1;
        consume(p); /* IN */
        while (peek(p).type == TOK_WORD) {
            token_t w = consume(p);
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
    /* CASE already consumed */
    token_t word_tok = consume(p);
    if (word_tok.type != TOK_WORD && word_tok.type != TOK_ASSIGN) {
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

        /* At least one pattern word */
        if (peek(p).type != TOK_WORD && peek(p).type != TOK_ASSIGN) {
            parser_error(p, "expected pattern in case item");
            wl_free(&pats);
            return NULL;
        }

        for (;;) {
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
static node_t *parse_function_def(parser_t *p, char *func_name)
{
    /* '(' and ')' already consumed by caller */
    skip_newlines(p);
    node_t *body = parse_compound_command(p);
    if (!body || p->error) {
        parser_error(p, "expected compound command as function body");
        return NULL;
    }

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
        node_t *n      = alloc_node(p, N_FUNC);
        n->u.func.name = name_tok.text;
        n->u.func.body = body;
        return n;
    }

    /* Compound command */
    {
        node_t *compound = parse_compound_command(p);
        if (compound) {
            /* Collect optional trailing redirects */
            redir_t  *rhead = NULL;
            redir_t **rtail = &rhead;
            for (;;) {
                token_t t = peek(p);
                int fd = -1;
                if (is_redir_op(t.type)) {
                    /* direct redirect op: use default fd */
                } else if (t.type == TOK_WORD) {
                    /* might be io-number: consume and check next */
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
            if (rhead) {
                node_t *wrap              = alloc_node(p, N_REDIR);
                wrap->u.redir_node.body   = compound;
                wrap->u.redir_node.redirs = rhead;
                return wrap;
            }
            return compound;
        }
    }

    /* Simple command (handles function detection too) */
    t = peek(p);
    if (t.type == TOK_WORD || t.type == TOK_ASSIGN || is_redir_op(t.type)) {
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

            /* name_tok is the first word of the command */
            if (name_tok.type == TOK_ASSIGN) {
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

                break;
            }

            node_t *n        = alloc_node(p, N_CMD);
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
    if (peek(p).type == TOK_BANG) {
        consume(p);
        negate = 1;
    }

    node_t *left = parse_command(p);
    if (!left || p->error) return NULL;

    while (peek(p).type == TOK_PIPE) {
        consume(p); /* | */
        skip_newlines(p);
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
static node_t *parse_list(parser_t *p)
{
    node_t *left = parse_and_or(p);
    if (!left || p->error) return NULL;

    for (;;) {
        tok_type_t t = peek(p).type;

        if (t == TOK_AMP) {
            /* Async: wrap left in N_ASYNC */
            consume(p);
            node_t *async_node           = alloc_node(p, N_ASYNC);
            async_node->u.binary.left    = left;
            async_node->u.binary.right   = NULL;
            left = async_node;

            skip_newlines(p);
            /* More commands? */
            tok_type_t nxt = peek(p).type;
            if (nxt == TOK_EOF   || nxt == TOK_FI    || nxt == TOK_DONE  ||
                nxt == TOK_ESAC  || nxt == TOK_THEN   || nxt == TOK_ELSE  ||
                nxt == TOK_ELIF  || nxt == TOK_DO     || nxt == TOK_RPAREN ||
                nxt == TOK_RBRACE || nxt == TOK_DSEMI)
                break;

            node_t *right = parse_and_or(p);
            if (!right || p->error) break;

            node_t *seq       = alloc_node(p, N_SEQ);
            seq->u.binary.left  = left;
            seq->u.binary.right = right;
            left = seq;
            continue;
        }

        if (t == TOK_SEMI || t == TOK_NEWLINE) {
            consume(p);
            skip_newlines(p);

            /* Check for list terminators */
            tok_type_t nxt = peek(p).type;
            if (nxt == TOK_EOF   || nxt == TOK_FI    || nxt == TOK_DONE  ||
                nxt == TOK_ESAC  || nxt == TOK_THEN   || nxt == TOK_ELSE  ||
                nxt == TOK_ELIF  || nxt == TOK_DO     || nxt == TOK_RPAREN ||
                nxt == TOK_RBRACE || nxt == TOK_DSEMI)
                break;

            node_t *right = parse_and_or(p);
            if (!right || p->error) break;

            node_t *seq        = alloc_node(p, N_SEQ);
            seq->u.binary.left  = left;
            seq->u.binary.right = right;
            left = seq;
            continue;
        }

        break;
    }

    return left;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Parse a single complete command (one and_or with optional separator) */
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
    if (peek(p).type == TOK_EOF)
        return NULL;
    return parse_list(p);
}

/* Parse a complete program (list until EOF) */
node_t *parser_parse_list(parser_t *p)
{
    skip_newlines(p);
    if (peek(p).type == TOK_EOF)
        return NULL;
    return parse_list(p);
}
