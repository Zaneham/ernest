#include "../src/qasm_lex.h"
#include "../src/qasm.h"
#include "../src/mnote.h"
#include "demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The ERNESTLX hello world.
 *
 * Builds the Bell state circuit through the existing C builder API,
 * doesn't run it, instead asks the QASM emitter for the text form,
 * then hands that text to the lexer. Prints the resulting token
 * stream and any MNOTEs the lexer cared to raise.
 *
 * This is the first time anything in Ernest has read what Ernest
 * itself wrote. Round-trip is one parser away.
 */

#define LEX_BUFFER_MAX (1u << 14)
static char qasm_buffer[LEX_BUFFER_MAX];

/*
 * The lexer test. Run from main.c when the user selects the "lex"
 * subcommand. Builds the Bell state, emits QASM into a memory
 * buffer via a tmpfile round-trip, runs the lexer, prints what it
 * found.
 */
int run_lex_test(void)
{
    /* Build the Bell state circuit. */
    static tir_module_t M;
    build_bell_module(&M);

    /* Emit QASM into a memory buffer. Standard C99 way to do this
     * portably is via a temporary file: tmpfile() returns a stream
     * backed by an automatically-deleted file. Write, rewind, read
     * back into our buffer. Not pretty, but portable. */
    FILE *tmp = tmpfile();
    if (tmp == NULL) {
        (void)fprintf(stderr, "lex_test: tmpfile failed\n");
        return 1;
    }
    ernest_emit_qasm3(&M, tmp);
    (void)fflush(tmp);
    rewind(tmp);

    size_t n = fread(qasm_buffer, 1u, (size_t)LEX_BUFFER_MAX - 1u, tmp);
    (void)fclose(tmp);
    qasm_buffer[n] = '\0';

    (void)printf("-- ERNESTQA output (input to ERNESTLX) --\n");
    (void)fputs(qasm_buffer, stdout);
    (void)printf("\n");

    /* Job-step start. */
    (void)printf("ERNESTLX START USING ERNEST 0.1.0\n");

    /* Run the lexer. */
    static mnote_log_t log;
    mnote_init(&log, "ERNESTLX");

    static qasm_lexer_t L;
    qasm_lex_init(&L, qasm_buffer, (uint32_t)n, "BELL.QASM", &log);
    int rc = qasm_lex_run(&L);

    /* Print any MNOTEs the lexer emitted. */
    mnote_print(&log, stdout);

    /* Job-step end. */
    (void)printf("ERNESTLX ENDED   RC=%u  TOKENS=%u\n",
                 (unsigned)mnote_exit_code(&log),
                 (unsigned)L.num_tokens);

    /* Show the tokens. */
    (void)printf("\n");
    qasm_lex_print(&L, stdout);

    return rc;
}
