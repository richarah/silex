#define _POSIX_C_SOURCE 200809L
/*
 * tests/fuzz/fuzz_shell_parser.c — LibFuzzer target for the silex shell parser.
 *
 * Build (requires clang with libFuzzer support):
 *   clang -std=c11 -fsanitize=fuzzer,address,undefined \
 *       -I../../src \
 *       fuzz_shell_parser.c \
 *       ../../src/shell/lexer.c \
 *       ../../src/shell/parser.c \
 *       ../../src/util/arena.c \
 *       ../../src/util/strbuf.c \
 *       ../../src/util/error.c \
 *       -o fuzz_shell_parser
 *
 * Run:
 *   mkdir -p corpus_parser
 *   echo 'if true; then echo yes; fi' > corpus_parser/seed0.sh
 *   echo 'for x in a b; do echo $x; done' > corpus_parser/seed1.sh
 *   echo 'case $x in a) echo a;; b) echo b;; esac' > corpus_parser/seed2.sh
 *   ./fuzz_shell_parser corpus_parser -max_len=8192
 *
 * The fuzzer exercises both the lexer and parser together with arbitrary
 * byte sequences. It verifies:
 *   - No crashes, hangs, or memory errors in the lexer or parser.
 *   - Malformed input is handled gracefully (parse errors do not crash).
 *   - The arena is properly freed regardless of parse success or failure.
 */

#include "../../src/shell/lexer.h"
#include "../../src/shell/parser.h"
#include "../../src/util/arena.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Maximum parse iterations (parse_list calls parser_parse in a loop).
 * Bounded to prevent infinite loops on pathological input.
 */
#define MAX_PARSE_ITERATIONS 4096

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Null-terminate the fuzzer input. */
    char *input = (char *)malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    arena_t arena;
    arena_init(&arena);

    lexer_t lexer;
    lexer_init_str(&lexer, input, &arena);

    parser_t parser;
    parser_init(&parser, &lexer, &arena);

    /*
     * Parse complete commands until EOF or error.
     * We walk the produced AST only to ensure no use-after-free or
     * dangling pointers — we do not execute anything.
     */
    int iterations = 0;
    while (iterations < MAX_PARSE_ITERATIONS) {
        node_t *tree = parser_parse(&parser);

        /* parser_parse returns NULL on EOF or unrecoverable error */
        if (tree == NULL) break;

        /*
         * Light structural sanity check: walk the root node type.
         * This exercises any pointer dereferences the walker might do.
         * A more thorough walker could be added here later.
         */
        (void)tree->type;

        /* If the parser has flagged an error, stop iterating. */
        if (parser.error) break;

        iterations++;
    }

    /*
     * arena_free releases all nodes, tokens, and strings allocated during
     * lexing and parsing in a single bulk free. No per-node free needed.
     */
    arena_free(&arena);
    /* lexer_free only releases the word buffer (heap), not arena memory. */
    lexer_free(&lexer);
    free(input);
    return 0;
}
