#include "../src/tir.h"
#include "../src/qasm.h"
#include "../src/sim.h"
#include "../src/abend.h"
#include "demos.h"

#include <stdio.h>

/*
 * A deliberate ABEND. Builds a circuit that asks for more qubits
 * than the simulator can possibly handle, runs it, and lets the
 * ABEND machinery write the dump.
 *
 * The point of this demo is not to do useful quantum computing.
 * The point is to show what a failure looks like, end to end:
 * mainframe-style header, completion code, PSW, recent
 * instructions trace, register map. If you want to see the dump
 * in action without actually breaking your real circuit, run
 * this.
 */

void build_abend_module(tir_module_t *M);

void build_abend_module(tir_module_t *M)
{
    assert(M != NULL);

    /* ERNEST_SIM_MAX_QUBITS is 16, so 20 qubits is guaranteed to
     * trip the U0008 ABEND inside sim_init. Pick a number that
     * comfortably exceeds the limit. */
    tir_module_init(M, "abend_demo");

    uint32_t q = tir_qreg(M, "q", 20u);
    uint32_t c = tir_creg(M, "c", 20u);

    /* A handful of gates so the trace and module pretty-printer
     * have something to show. None of these actually run; sim_init
     * fails before the dispatch loop starts. */
    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_cx(M, TIR_REF(q, 0u), TIR_REF(q, 1u));
    tir_emit_cx(M, TIR_REF(q, 1u), TIR_REF(q, 2u));
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
    tir_emit_measure(M, TIR_REF(q, 1u), TIR_REF(c, 1u));
    tir_emit_measure(M, TIR_REF(q, 2u), TIR_REF(c, 2u));
}

int run_abend_test(void)
{
    static tir_module_t M;
    build_abend_module(&M);

    (void)printf("ERNESTJB ABND   BUILD  ENDED   RC=0  INSTS=%u  QUBITS=20\n",
                 (unsigned)M.num_insts);
    (void)printf("ERNESTJB ABND   ERNESTSM START\n");

    static sim_state_t S;
    sim_init(&S, &M);

    if (S.status == SIM_OK) {
        /* Unlikely but possible if ERNEST_SIM_MAX_QUBITS ever grew
         * to accommodate twenty qubits. In that case the demo no
         * longer demonstrates anything; warn and exit clean. */
        (void)printf("ERNESTJB ABND   ERNESTSM ENDED   RC=0  (no ABEND triggered)\n");
        return 0;
    }

    /* Construct the ABEND block from the status the simulator
     * left for us. */
    abend_t A;
    abend_init(&A, sim_status_code(S.status));
    abend_set_reason(&A, "%s", S.status_reason);
    abend_set_psw(&A, ABEND_PSW_RUNNING, S.current_inst, S.status_qubit);

    (void)printf("ERNESTJB ABND   ERNESTSM ENDED   RC=8  (%s)\n",
                 sim_status_code(S.status));

    /* Drop the dump. */
    abend_dump(&A, &S, &M, stdout);

    (void)printf("ERNESTJB ABND   EOJ    RC=8\n");
    return 8;
}
