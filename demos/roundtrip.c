#include "../src/tir.h"
#include "../src/qasm.h"
#include "../src/qasm_lex.h"
#include "../src/qasm_parse.h"
#include "../src/mnote.h"
#include "demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The round-trip harness.
 *
 * For each demo circuit:
 *
 *   1. Build the circuit through the C builder API, producing M1.
 *   2. Emit M1 as OpenQASM 3 into an in-memory buffer.
 *   3. Lex the buffer.
 *   4. Parse the lexer output into a fresh module M2.
 *   5. Run tir_module_diff(M1, M2) and report.
 *
 * If all four demos round-trip cleanly the parser handles every
 * piece of the emitter's vocabulary, the lexer reads everything the
 * emitter writes, and the IR comparator confirms the loop closes.
 * That's our v1 milestone.
 *
 * Diagnostics flow through the mainframe-style MNOTE channel. The
 * job-step style output makes each circuit's compile look like a
 * z/OS console line, which is the aesthetic we live in now.
 */

#define RT_BUFFER_MAX (1u << 14)

/*
 * Function-pointer type to call any of the demo builders by handle.
 * Keeps the per-demo loop tidy.
 */
typedef void (*build_fn_t)(tir_module_t *);

typedef struct {
    const char  *txid;        /* four-character transaction ID    */
    const char  *name;        /* internal demo name               */
    build_fn_t   build;
} rt_demo_t;

static const rt_demo_t DEMOS[] = {
    { "BELL", "bell",    build_bell_module    },
    { "GHZS", "ghz",     build_ghz_module     },
    { "DTCH", "deutsch", build_deutsch_module },
    { "GRVR", "grover",  build_grover_module  },
};

#define NUM_DEMOS (sizeof DEMOS / sizeof DEMOS[0])

/*
 * Static state. Big buffers and big structs avoid the stack and
 * avoid the heap. main runs once; static storage is fine.
 */
static char         qasm_buf[RT_BUFFER_MAX];
static tir_module_t M_origin;
static tir_module_t M_replay;
static mnote_log_t  log_lex;
static mnote_log_t  log_parse;
static qasm_lexer_t lex;

/*
 * Round-trip one demo. Returns 0 if it passed, non-zero otherwise.
 * Prints job-step style output as it goes.
 */
static int roundtrip_one(const rt_demo_t *D)
{
    assert(D != NULL);

    (void)printf("\n----- ERNESTJB %s -----------------------------\n", D->txid);

    /* Step 1: build via C API. */
    (void)printf("ERNESTJB %s   BUILD  START\n", D->txid);
    D->build(&M_origin);
    (void)printf("ERNESTJB %s   BUILD  ENDED   RC=0  INSTS=%u\n",
                 D->txid, (unsigned)M_origin.num_insts);

    /* Step 2: emit QASM into an in-memory buffer via tmpfile. */
    (void)printf("ERNESTJB %s   ERNESTQA START\n", D->txid);
    FILE *tmp = tmpfile();
    if (tmp == NULL) {
        (void)printf("ERNESTJB %s   ERNESTQA ENDED   RC=16 (tmpfile failed)\n",
                     D->txid);
        return 1;
    }
    ernest_emit_qasm3(&M_origin, tmp);
    (void)fflush(tmp);
    rewind(tmp);
    size_t n = fread(qasm_buf, 1u, (size_t)RT_BUFFER_MAX - 1u, tmp);
    (void)fclose(tmp);
    qasm_buf[n] = '\0';
    (void)printf("ERNESTJB %s   ERNESTQA ENDED   RC=0  BYTES=%u\n",
                 D->txid, (unsigned)n);

    /* Step 3: lex. */
    (void)printf("ERNESTJB %s   ERNESTLX START\n", D->txid);
    mnote_init(&log_lex, "ERNESTLX");
    qasm_lex_init(&lex, qasm_buf, (uint32_t)n, "DEMO.QASM", &log_lex);
    int lex_rc = qasm_lex_run(&lex);
    mnote_print(&log_lex, stdout);
    (void)printf("ERNESTJB %s   ERNESTLX ENDED   RC=%u  TOKENS=%u\n",
                 D->txid, (unsigned)lex_rc, (unsigned)lex.num_tokens);
    if (lex_rc >= 8) {
        return 1;
    }

    /* Step 4: parse. */
    (void)printf("ERNESTJB %s   ERNESTPR START\n", D->txid);
    tir_module_init(&M_replay, D->name);
    mnote_init(&log_parse, "ERNESTPR");
    int parse_rc = qasm_parse(&lex, &M_replay, &log_parse);
    mnote_print(&log_parse, stdout);
    (void)printf("ERNESTJB %s   ERNESTPR ENDED   RC=%u  INSTS=%u\n",
                 D->txid, (unsigned)parse_rc, (unsigned)M_replay.num_insts);
    if (parse_rc >= 8) {
        return 1;
    }

    /* Step 5: diff. */
    char reason[160];
    reason[0] = '\0';
    int diff = tir_module_diff(&M_origin, &M_replay, reason, (uint32_t)sizeof reason);
    if (diff == 0) {
        (void)printf("ERNESTJB %s   DIFF     ENDED   RC=0  MATCH\n", D->txid);
        return 0;
    }
    (void)printf("ERNESTJB %s   DIFF     ENDED   RC=8  MISMATCH: %s\n",
                 D->txid, reason);
    return 1;
}

/*
 * Run all the demos and tally results. Exit code is 0 if everything
 * matched, 8 otherwise (mainframe-style: a failure that completed
 * its run is an 8, not a 16).
 */
int run_roundtrip(void)
{
    uint32_t pass = 0u;
    uint32_t fail = 0u;

    for (uint32_t i = 0u; i < (uint32_t)NUM_DEMOS; i++) {
        int r = roundtrip_one(&DEMOS[i]);
        if (r == 0) {
            pass++;
        } else {
            fail++;
        }
    }

    (void)printf("\n========================================\n");
    (void)printf("ERNEST ROUNDTRIP   PASS=%u  FAIL=%u  OF=%u\n",
                 (unsigned)pass, (unsigned)fail, (unsigned)NUM_DEMOS);
    (void)printf("ERNEST EOJ        RC=%u\n", (unsigned)(fail == 0u ? 0u : 8u));

    return (fail == 0u) ? 0 : 8;
}
