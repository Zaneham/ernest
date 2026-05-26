#include "../src/tir.h"
#include "../src/qasm.h"
#include "../src/opt.h"
#include "../src/sim.h"
#include "../src/mnote.h"
#include "demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The optimisation demo.
 *
 * Builds a deliberately obfuscated Bell state, the kind of circuit
 * someone produces when they are still finding their footing in
 * OpenQASM: a Hadamard followed by another Hadamard for no especially
 * good reason, an X on the partner qubit followed immediately by its
 * own undoing, an S-and-then-SDG that goes out the door wearing the
 * same coat it came in with, and a chain of rotations whose angles
 * add up to exactly zero. Then it runs the optimiser, shows the much
 * shorter result, simulates both circuits, and demonstrates the Bell
 * distribution survives the operation.
 *
 * Two passes get a workout here. OPTGCAN handles the self-inverse
 * pairs (H, X, S/SDG, T/TDG). OPTFUSE handles the rotation chains,
 * adding adjacent same-axis rotations and dropping the pair when the
 * combined angle is zero modulo two pi.
 *
 * If you want to confirm the optimiser preserves semantics, this is
 * the demo to run.
 */

#define OPT_DEMO_SHOTS         8192u
#define OPT_DEMO_MAX_OUTCOMES  (1u << 8)

/*
 * The obfuscated Bell builder. Each comment names the redundant pair
 * the optimiser is expected to remove. After OPTGCAN has had its turn,
 * what remains is the canonical Bell state: H, CX, two measurements.
 */
static void build_opt_demo(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "opt_demo");
    uint32_t q = tir_qreg(M, "q", 2u);
    uint32_t c = tir_creg(M, "c", 2u);

    /* H followed by H. Cancels. */
    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_h(M, TIR_REF(q, 0u));

    /* The real Hadamard. Survives. */
    tir_emit_h(M, TIR_REF(q, 0u));

    /* X followed by X. Cancels. */
    tir_emit_x(M, TIR_REF(q, 1u));
    tir_emit_x(M, TIR_REF(q, 1u));

    /* The real CX, doing the actual entangling. Survives. */
    tir_emit_cx(M, TIR_REF(q, 0u), TIR_REF(q, 1u));

    /* S followed by SDG. Inverse pair, cancels. */
    tir_emit_s  (M, TIR_REF(q, 0u));
    tir_emit_sdg(M, TIR_REF(q, 0u));

    /* T followed by TDG. Inverse pair, cancels via OPTGCAN. */
    tir_emit_t  (M, TIR_REF(q, 1u));
    tir_emit_tdg(M, TIR_REF(q, 1u));

    /* RX pair on q[0] whose angles sum to zero. OPTFUSE adds them,
     * sees zero, drops both. */
    tir_emit_rx(M, TIR_REF(q, 0u),  0.7853981633974483); /* +pi/4 */
    tir_emit_rx(M, TIR_REF(q, 0u), -0.7853981633974483); /* -pi/4 */

    /* RY triple on q[1] whose angles sum to zero across three steps.
     * First pass fuses ry(0.4)+ry(0.6) into ry(1.0). Second pass
     * fuses ry(1.0)+ry(-1.0) into ry(0), which is identity, and the
     * gates both leave. Two OPTFUSE iterations, three instructions
     * gone. */
    tir_emit_ry(M, TIR_REF(q, 1u),  0.4);
    tir_emit_ry(M, TIR_REF(q, 1u),  0.6);
    tir_emit_ry(M, TIR_REF(q, 1u), -1.0);

    /* Measurements. Survive (and ought to). */
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
    tir_emit_measure(M, TIR_REF(q, 1u), TIR_REF(c, 1u));
}

/*
 * Simulate M for `shots` shots, accumulating per-outcome counts.
 * Returns zero on a clean run, one on an ABEND.
 */
static int simulate(tir_module_t *M, uint32_t *counts, uint32_t shots)
{
    assert(M != NULL);
    assert(counts != NULL);

    static sim_state_t S;

    uint32_t num_bits     = tir_total_bits(M);
    uint32_t num_outcomes = (num_bits == 0u) ? 1u : (1u << num_bits);

    for (uint32_t i = 0u; i < num_outcomes; i++) {
        counts[i] = 0u;
    }

    for (uint32_t s = 0u; s < shots; s++) {
        sim_init(&S, M);
        sim_run_shot(&S, M);
        if (S.status != SIM_OK) {
            return 1;
        }
        uint32_t outcome = sim_creg_as_uint(&S);
        if (outcome < num_outcomes) {
            counts[outcome]++;
        }
    }
    return 0;
}

/*
 * One histogram, mainframe-flavoured. Skips outcomes with zero counts
 * so the dump stays readable on small classical registers.
 */
static void print_histogram(const uint32_t *counts, uint32_t shots,
                            uint32_t num_bits, const char *title)
{
    assert(counts != NULL);
    assert(title  != NULL);

    const uint32_t BAR_WIDTH = 30u;
    uint32_t end = (num_bits == 0u) ? 1u : (1u << num_bits);

    (void)printf("\n  %s (%u shots)\n", title, (unsigned)shots);
    (void)printf("  ---------------------------\n");
    for (uint32_t v = 0u; v < end; v++) {
        if (counts[v] == 0u) {
            continue;
        }
        double frac    = (double)counts[v] / (double)shots;
        uint32_t bar   = (uint32_t)(frac * (double)BAR_WIDTH + 0.5);
        (void)printf("  |");
        for (uint32_t b = num_bits; b > 0u; b--) {
            (void)putchar(((v >> (b - 1u)) & 1u) != 0u ? '1' : '0');
        }
        (void)printf(">  %5u  ", (unsigned)counts[v]);
        for (uint32_t b = 0u; b < bar; b++) {
            (void)putchar('#');
        }
        (void)printf("  %5.1f%%\n", frac * 100.0);
    }
}

/* Big static state, in keeping with the rest of the codebase. */
static uint32_t      counts_before[OPT_DEMO_MAX_OUTCOMES];
static uint32_t      counts_after [OPT_DEMO_MAX_OUTCOMES];
static tir_module_t  M_before;
static tir_module_t  M_after;
static mnote_log_t   opt_log;

int run_opt_test(void)
{
    srand((unsigned int)time(NULL));

    (void)printf("ERNESTJB OPTD START ---------------------------\n");

    /* Build the obfuscated circuit twice: one copy keeps its
     * redundant gates for comparison, the other goes through the
     * optimiser. Two modules is cheaper than copying one. */
    build_opt_demo(&M_before);
    build_opt_demo(&M_after);

    (void)printf("\n-- BEFORE OPT (TIR) --\n");
    tir_print_module(&M_before, stdout);
    (void)printf("\n-- BEFORE OPT (OpenQASM 3.0) --\n");
    ernest_emit_qasm3(&M_before, stdout);

    (void)printf("\n");
    mnote_init(&opt_log, "ERNESTOP");
    int rc = opt_run(&M_after, OPT_LEVEL_BASIC, OPT_TARGET_GENERIC, &opt_log);
    mnote_print(&opt_log, stdout);

    (void)printf("\n-- AFTER OPT (TIR) --\n");
    tir_print_module(&M_after, stdout);
    (void)printf("\n-- AFTER OPT (OpenQASM 3.0) --\n");
    ernest_emit_qasm3(&M_after, stdout);

    /* Simulate both. If the optimiser is honest the two histograms
     * are statistically indistinguishable: both should show the
     * familiar Bell state pattern, roughly half |00>, roughly half
     * |11>, no other peaks worth speaking of. */
    uint32_t num_bits = tir_total_bits(&M_before);
    if (simulate(&M_before, counts_before, OPT_DEMO_SHOTS) != 0) {
        (void)printf("OPTD: simulate(before) ABEND\n");
        return 8;
    }
    if (simulate(&M_after, counts_after, OPT_DEMO_SHOTS) != 0) {
        (void)printf("OPTD: simulate(after) ABEND\n");
        return 8;
    }

    print_histogram(counts_before, OPT_DEMO_SHOTS, num_bits,
                    "BEFORE OPT histogram");
    print_histogram(counts_after,  OPT_DEMO_SHOTS, num_bits,
                    "AFTER  OPT histogram");

    (void)printf("\nERNESTJB OPTD ENDED  RC=%d\n", rc);
    (void)printf("ERNEST EOJ        RC=%d\n", rc);
    return rc;
}
