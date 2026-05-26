#include "../src/tir.h"
#include "../src/qasm.h"
#include "../src/qasm_lex.h"
#include "../src/qasm_parse.h"
#include "../src/opt.h"
#include "../src/aot.h"
#include "../src/mnote.h"
#include "demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compile a user-supplied OpenQASM 3 file through the full Ernest
 * pipeline and print the result. Demonstrates Ernest as a CLI tool
 * for actual users: lex, parse, optimise, print the TIR, print the
 * QASM, optionally print a cross-reference listing showing where
 * each output gate came from.
 *
 * Doesn't simulate; that path stays gated on the C-builder demos
 * for now. The point here is to exercise the compiler on real
 * input and show the diagnostics flowing.
 */

#define COMPILE_BUF_MAX (1u << 16)

static char         compile_buf[COMPILE_BUF_MAX];
static qasm_lexer_t compile_lex;
static tir_module_t compile_M;
static mnote_log_t  compile_log_lex;
static mnote_log_t  compile_log_parse;
static mnote_log_t  compile_log_opt;

#define COMPILE_MAX_OUTCOMES (1u << 16)
static uint32_t compile_counts[COMPILE_MAX_OUTCOMES];

int run_compile(const char *path, int do_xref,
                int do_no_opt, int target_is_ibm,
                int do_aot, uint32_t shots)
{
    assert(path != NULL);

    /* Open and slurp the file. */
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        (void)fprintf(stderr, "ernest: cannot open '%s'\n", path);
        return 1;
    }
    size_t n = fread(compile_buf, 1u, (size_t)COMPILE_BUF_MAX - 1u, f);
    (void)fclose(f);
    compile_buf[n] = '\0';

    /* Job-step header. */
    (void)printf("ERNESTJB COMPILE START  file=%s  bytes=%u\n",
                 path, (unsigned)n);

    /* Lex. */
    mnote_init(&compile_log_lex, "ERNESTLX");
    qasm_lex_init(&compile_lex, compile_buf, (uint32_t)n,
                  path, &compile_log_lex);
    int lex_rc = qasm_lex_run(&compile_lex);
    mnote_print(&compile_log_lex, stdout);
    (void)printf("ERNESTJB COMPILE        ERNESTLX  RC=%d  TOKENS=%u\n",
                 lex_rc, (unsigned)compile_lex.num_tokens);
    if (lex_rc >= 8) {
        return lex_rc;
    }

    /* Parse. */
    tir_module_init(&compile_M, path);
    mnote_init(&compile_log_parse, "ERNESTPR");
    int parse_rc = qasm_parse(&compile_lex, &compile_M, &compile_log_parse);
    mnote_print(&compile_log_parse, stdout);
    (void)printf("ERNESTJB COMPILE        ERNESTPR  RC=%d  INSTS=%u\n",
                 parse_rc, (unsigned)compile_M.num_insts);
    if (parse_rc >= 8) {
        return parse_rc;
    }

    /* Optimise (unless --no-opt). */
    uint32_t opt_level = do_no_opt ? OPT_LEVEL_NONE : OPT_LEVEL_BASIC;
    opt_target_t tgt   = target_is_ibm ? OPT_TARGET_IBM : OPT_TARGET_GENERIC;
    if (opt_level != OPT_LEVEL_NONE) {
        mnote_init(&compile_log_opt, "ERNESTOP");
        (void)opt_run(&compile_M, opt_level, tgt, &compile_log_opt);
        mnote_print(&compile_log_opt, stdout);
        (void)printf("\n");
    }

    /* Print results. */
    (void)printf("-- TIR --\n");
    tir_print_module(&compile_M, stdout);

    if (do_xref) {
        (void)printf("\n");
        tir_print_xref(&compile_M, stdout);
    }

    (void)printf("\n-- OpenQASM 3.0 --\n");
    ernest_emit_qasm3(&compile_M, stdout);

    /* If the user asked for AOT execution, hand the optimised
     * module to the codegen and print the resulting histogram. */
    if (do_aot) {
        uint32_t num_bits = tir_total_bits(&compile_M);
        uint32_t num_outcomes = (num_bits == 0u) ? 1u : (1u << num_bits);
        if (num_outcomes > COMPILE_MAX_OUTCOMES) {
            num_outcomes = COMPILE_MAX_OUTCOMES;
        }
        (void)printf("\n");
        int rc = aot_compile_and_run(&compile_M, shots,
                                     compile_counts, num_outcomes,
                                     stdout);
        if (rc != 0) {
            (void)fprintf(stderr, "ernest: AOT failed RC=%d\n", rc);
            return 8;
        }
        (void)printf("\n  Histogram (%u shots)\n", (unsigned)shots);
        (void)printf("  ---------------------------\n");
        for (uint32_t v = 0u; v < num_outcomes; v++) {
            if (compile_counts[v] == 0u) continue;
            double frac = (double)compile_counts[v] / (double)shots;
            uint32_t bar = (uint32_t)(frac * 30.0 + 0.5);
            (void)printf("  |");
            uint32_t nb_disp = (num_bits == 0u) ? 1u : num_bits;
            for (uint32_t b = nb_disp; b > 0u; b--) {
                (void)putchar(((v >> (b - 1u)) & 1u) ? '1' : '0');
            }
            (void)printf(">  %6u  ", (unsigned)compile_counts[v]);
            for (uint32_t b = 0u; b < bar; b++) (void)putchar('#');
            (void)printf("  %5.1f%%\n", frac * 100.0);
        }
    }

    (void)printf("\nERNESTJB COMPILE ENDED  RC=0\n");
    return 0;
}
