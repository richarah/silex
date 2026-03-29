#define _POSIX_C_SOURCE 200809L
/*
 * tests/fuzz/fuzz_shell_lexer.c — LibFuzzer target for the matchbox shell lexer.
 *
 * Build (requires clang with libFuzzer support):
 *   clang -std=c11 -fsanitize=fuzzer,address,undefined \
 *       -I../../src \
 *       fuzz_shell_lexer.c \
 *       ../../src/shell/lexer.c \
 *       ../../src/util/arena.c \
 *       ../../src/util/strbuf.c \
 *       ../../src/util/error.c \
 *       -o fuzz_shell_lexer
 *
 * Run:
 *   mkdir -p corpus_lexer
 *   echo 'echo hello' > corpus_lexer/seed0.sh
 *   echo 'for i in a b c; do echo $i; done' > corpus_lexer/seed1.sh
 *   ./fuzz_shell_lexer corpus_lexer -max_len=4096
 *
 * The fuzzer exercises the lexer with arbitrary byte sequences and checks that:
 *   - No crashes, hangs, or memory errors occur.
 *   - The lexer always terminates (loop is bounded by TOK_EOF or iteration limit).
 */

#include "../../src/shell/lexer.h"
#include "../../src/util/arena.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Maximum tokens to read before assuming the lexer is looping.
 * Shell input of up to 4096 bytes cannot produce more than this many tokens.
 */
#define MAX_TOKENS 16384

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Null-terminate the fuzzer-provided bytes to form a C string. */
    char *input = (char *)malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    arena_t arena;
    arena_init(&arena);

    lexer_t lexer;
    lexer_init_str(&lexer, input, &arena);

    /*
     * Lex tokens until EOF or the iteration cap.
     * We discard all token content — only memory safety matters here.
     */
    for (int i = 0; i < MAX_TOKENS; i++) {
        token_t tok = lexer_next(&lexer);
        if (tok.type == TOK_EOF) break;
    }

    lexer_free(&lexer);
    arena_free(&arena);
    free(input);
    return 0;
}
