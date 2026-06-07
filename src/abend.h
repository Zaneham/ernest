#ifndef ERNEST_ABEND_H
#define ERNEST_ABEND_H

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "mnote.h"
#include "sim.h"
#include "tir.h"

/*
 * ERNESTDM. The ABEND dump formatter.
 *
 * Yes. A 1960s IBM mainframe abend dump, bolted onto a quantum
 * compiler. The oldest diagnostic format in computing, reporting on
 * the newest thing in it. The wheel turns, and somewhere a z/OS
 * operator from 1974 feels a disturbance he cannot name.
 *
 * When the simulator (or any other stage) walks off the edge of its
 * design envelope, this is what comes out instead of a bare crash:
 * a completion code, a Program Status Word, a recent-instruction
 * trace, a register summary, and one blunt sentence at the top
 * saying what blew up.
 *
 * The codes follow z/Architecture, repurposed to mean things that
 * can actually go wrong inside a statevector simulator:
 *
 *   S0C1   operation exception          unknown gate at runtime
 *   S0C4   protection exception         qubit index out of range
 *   S0C5   addressing exception         register not allocated
 *   S0C6   specification exception      malformed operand
 *   S0C7   data exception               NaN or Inf amplitude
 *   S0CB   fixed-point divide by zero   normalisation gone bad
 *   U0008  user code                    statevector dimension exceeded
 *   U0009  user code                    classical reg dimension exceeded
 *
 * Mainframe-shaped codes, quantum-shaped failures, and a dump that
 * reads exactly the way a z/OS operator expects, right up until the
 * detail that the thing going wrong is a probability amplitude.
 */

/* ----- The abend block ------------------------------------------- */

typedef struct {
    char     completion_code[8];   /* e.g. "S0C7" or "U0008" */
    char     reason[256];          /* one short sentence on what blew up */

    /*
     * PSW. A real Program Status Word, the way the 360 did it, because
     * if you are going to cosplay as a mainframe you commit to the bit.
     * Sixty-four bits packed into four sixteen-bit fields:
     *
     *   FFFF_IIII_QQQQ_RRRR
     *
     * FFFF: flags + condition code
     *   bit 15  RUNNING        executing when the abend fired
     *   bit 14  TRACE_FULL     trace ring buffer wrapped
     *   bit 13  MEASURING      mid-measurement when the abend fired
     *   bits 7-0  condition code (per-abend specific)
     *
     * IIII: the TIR instruction we were executing.
     * QQQQ: the qubit (or operand) context, if relevant.
     * RRRR: reserved, always zero.
     */
    uint64_t psw;

    uint32_t inst_idx;             /* duplicate of PSW's IIII, for convenience */
    uint32_t qubit_ctx;            /* duplicate of PSW's QQQQ */
} abend_t;

/* PSW flag bits. */
#define ABEND_PSW_RUNNING      (1u << 15)
#define ABEND_PSW_TRACE_FULL   (1u << 14)
#define ABEND_PSW_MEASURING    (1u << 13)

/* ----- Construction ----------------------------------------------- */

/*
 * Start a fresh abend block. completion_code is one of the codes in
 * the table above; anything else is fine but won't read as cleanly
 * in the dump.
 */
void abend_init(abend_t *A, const char *completion_code);

/*
 * Fill the reason field with a printf-style message. Truncated if
 * it would overflow.
 */
void abend_set_reason(abend_t *A, const char *fmt, ...);

/*
 * Pack flags, instruction index, and qubit context into the PSW.
 * Each field is masked into its sixteen-bit slot before being
 * folded into the sixty-four-bit word.
 */
void abend_set_psw(abend_t *A, uint32_t flags,
                   uint32_t inst_idx, uint32_t qubit_ctx);

/* ----- Output ----------------------------------------------------- */

/*
 * Print a complete dump to `out`. If S is non-NULL, the dump
 * includes the simulator's instruction trace and statevector
 * summary. If M is non-NULL, the trace and current-instruction
 * lines are pretty-printed using TIR's gate names. Either may be
 * NULL when no such information is available (e.g. an abend during
 * compilation rather than simulation).
 *
 * The dump format reads top-to-bottom the way a z/OS operator
 * expects: who is talking, what code, the PSW, the reason, then
 * the supporting evidence.
 */
void abend_dump(const abend_t *A,
                const sim_state_t *S,
                const tir_module_t *M,
                FILE *out);

/*
 * Emit a corresponding MNOTE so the abend also shows up in the
 * compiler's diagnostic log. The MNOTE carries the completion
 * code, the reason, and the source location (if any). The full
 * dump goes to its own stream via abend_dump.
 */
void abend_emit(const abend_t *A, mnote_log_t *log,
                const char *source_file,
                uint32_t line, uint32_t col);

/* ----- Pipeline stage tracking ----------------------------------- */
/*
 * Which part of the compiler is currently running. Pushed on entry
 * to a stage, popped on exit. abend_dump reads these so a fault in
 * (say) SABRE routing produces a dump that mentions routing rather
 * than leaving the operator to guess.
 *
 * The stage names are short tags meant to read cleanly in a dump:
 * "PARSE", "OPT", "ROUTE", "SIM", "AOT", "VERIFY". The sub-name is
 * a finer-grained context, typically a pass name like "OPTFUSE".
 * Both are limited to a small fixed length; longer strings are
 * truncated. Either may be NULL to clear.
 *
 * These are file-scope state. Single-threaded use only.
 */
void        abend_set_stage(const char *stage, const char *sub);
void        abend_clear_stage(void);
const char *abend_current_stage(void);
const char *abend_current_sub(void);

#endif /* ERNEST_ABEND_H */
