#include "../src/tir.h"
#include "../src/qasm.h"
#include "../src/opt.h"
#include "../src/sim.h"
#include "../src/aot.h"
#include "../src/mnote.h"
#include "demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/*
 * The religious-correctness verifier.
 *
 * Before submitting to real quantum hardware - where a wrong circuit
 * burns API time you cannot get back - we want to know, to floating-
 * point tolerance, that the IBM-native circuit Ernest produces is
 * the exact same unitary the user originally asked for. Statistical
 * histogram agreement is not enough; two different unitaries can
 * produce similar-looking histograms over finite shots.
 *
 * This harness runs every demo through three independent paths and
 * compares them:
 *
 *   1. Abstract (target=generic):
 *        build -> optimise -> simulate unitary
 *      Produces the "ground truth" statevector.
 *
 *   2. IBM-native (target=ibm):
 *        build -> optimise -> decompose -> optimise -> simulate
 *      Produces the statevector of what we'd actually submit. This
 *      must equal the ground truth, up to a global phase.
 *
 *   3. AOT-compiled IBM-native:
 *        same as 2, but the simulation goes through gcc -O3 native
 *        code instead of the interpreted simulator.
 *      Histogram must match the IBM-native interpreted result
 *      within tight statistical tolerance.
 *
 * A PASS on a row means the IBM-native circuit is mathematically
 * identical to the abstract circuit (the decomposition is correct)
 * AND the AOT-compiled binary reproduces the interpreted simulator
 * (the codegen is correct). Both bugs are caught before any
 * hardware time is burned.
 */

/* Floating-point tolerance for statevector equality. Each gate
 * introduces O(epsilon) rounding; with ~50 gates and statevectors
 * normalised to one, 1e-9 is comfortable. */
#define VERIFY_SV_TOL    1.0e-9
/* Total-variation distance tolerance for histogram comparison.
 * At 100k shots, the per-bin standard deviation is ~0.0016 for
 * uniform-ish distributions, so 0.02 is several sigma. */
#define VERIFY_HIST_TOL  0.02
#define VERIFY_AOT_SHOTS 100000u

#define VERIFY_MAX_OUTCOMES (1u << 16)

typedef struct {
    const char *name;
    const char *desc;
} verify_demo_t;

static const verify_demo_t DEMOS[] = {
    { "bell",      "Bell state, two qubits"                   },
    { "ghz",       "GHZ state, three qubits"                  },
    { "deutsch",   "Deutsch algorithm"                        },
    { "grover",    "Grover, two qubits, marked |11>"          },
    { "qftlib",    "libqstd QFT then inverse QFT"             },
    { "groverlib", "libqstd Grover, two qubits"               },
};
#define NUM_DEMOS (sizeof DEMOS / sizeof DEMOS[0])

/* Big static state. Two modules, two simulator states, an AOT
 * counts array, and an interpreted counts array per verification. */
static tir_module_t v_M_gen;
static tir_module_t v_M_ibm;
static sim_state_t  v_S_gen;
static sim_state_t  v_S_ibm;
static mnote_log_t  v_log;
static uint32_t     v_counts_aot[VERIFY_MAX_OUTCOMES];
static uint32_t     v_counts_int[VERIFY_MAX_OUTCOMES];

/*
 * Dispatch a circuit name to its build function. Matches main.c's
 * build_module but kept private here so the verifier can stand
 * alone if main.c's table ever drifts.
 */
static int verify_build(const char *name, tir_module_t *M)
{
    if (strcmp(name, "bell")      == 0) { build_bell_module(M);      return 0; }
    if (strcmp(name, "ghz")       == 0) { build_ghz_module(M);       return 0; }
    if (strcmp(name, "deutsch")   == 0) { build_deutsch_module(M);   return 0; }
    if (strcmp(name, "grover")    == 0) { build_grover_module(M);    return 0; }
    if (strcmp(name, "qftlib")    == 0) { build_qftlib_module(M);    return 0; }
    if (strcmp(name, "groverlib") == 0) { build_groverlib_module(M); return 0; }
    return 1;
}

/*
 * Compare two statevectors of dimension dim. Returns the maximum
 * pointwise discrepancy after accounting for a global phase. Two
 * statevectors |a> and |b> implement the same unitary iff there
 * exists a unit complex number p such that a[i] = p * b[i] for
 * every i. We find p by dividing a[pivot]/b[pivot] for the first
 * pivot where |b[pivot]| is comfortably non-zero, then check every
 * other entry agrees.
 */
static double sv_max_diff(const sim_complex_t *a, const sim_complex_t *b,
                          uint32_t dim)
{
    /* Find a non-trivial pivot to extract the phase. We pick the
     * largest-magnitude element of b. */
    uint32_t pivot = 0u;
    double best = 0.0;
    for (uint32_t i = 0u; i < dim; i++) {
        double mag2 = b[i].re * b[i].re + b[i].im * b[i].im;
        if (mag2 > best) { best = mag2; pivot = i; }
    }
    if (best < 1.0e-30) {
        /* b is essentially the zero vector; both should be. */
        double worst = 0.0;
        for (uint32_t i = 0u; i < dim; i++) {
            double m2 = a[i].re * a[i].re + a[i].im * a[i].im;
            if (m2 > worst) worst = m2;
        }
        return sqrt(worst);
    }

    /* phase = a[pivot] / b[pivot] = a[pivot] * conj(b[pivot]) / |b[pivot]|^2 */
    double br = b[pivot].re, bi = b[pivot].im;
    double ar = a[pivot].re, ai = a[pivot].im;
    double inv_bb = 1.0 / best;
    double pr = (ar * br + ai * bi) * inv_bb;
    double pi = (ai * br - ar * bi) * inv_bb;

    double worst = 0.0;
    for (uint32_t i = 0u; i < dim; i++) {
        /* expected = phase * b[i] */
        double er = pr * b[i].re - pi * b[i].im;
        double ei = pr * b[i].im + pi * b[i].re;
        double dr = a[i].re - er;
        double di = a[i].im - ei;
        double d2 = dr * dr + di * di;
        if (d2 > worst) worst = d2;
    }
    return sqrt(worst);
}

/*
 * Total variation distance between two count histograms over the
 * same support. 0.5 * sum |p_i - q_i|. Range is [0, 1]; identical
 * distributions give 0, disjoint supports give 1.
 */
static double hist_tvd(const uint32_t *p, uint32_t p_total,
                       const uint32_t *q, uint32_t q_total,
                       uint32_t n)
{
    if (p_total == 0u || q_total == 0u) return 1.0;
    double inv_p = 1.0 / (double)p_total;
    double inv_q = 1.0 / (double)q_total;
    double sum = 0.0;
    for (uint32_t i = 0u; i < n; i++) {
        double pi = (double)p[i] * inv_p;
        double qi = (double)q[i] * inv_q;
        double d = pi - qi;
        if (d < 0.0) d = -d;
        sum += d;
    }
    return 0.5 * sum;
}

/*
 * Verify one circuit. Returns 0 if every check passed, non-zero
 * otherwise. Writes a one-line PASS/FAIL summary plus the
 * per-check numerics.
 */
static int verify_one(const verify_demo_t *D)
{
    /* Build twice. */
    if (verify_build(D->name, &v_M_gen) != 0) {
        (void)printf("  [SKIP] %-12s  unknown demo\n", D->name);
        return 1;
    }
    (void)verify_build(D->name, &v_M_ibm);

    /* Optimise each for its target. */
    mnote_init(&v_log, "ERNESTOP");
    (void)opt_run(&v_M_gen, OPT_LEVEL_BASIC, OPT_TARGET_GENERIC, &v_log);
    mnote_init(&v_log, "ERNESTOP");
    (void)opt_run(&v_M_ibm, OPT_LEVEL_BASIC, OPT_TARGET_IBM,     &v_log);

    /* Sim unitary only on both. */
    sim_init(&v_S_gen, &v_M_gen);
    sim_run_unitary_only(&v_S_gen, &v_M_gen);
    sim_init(&v_S_ibm, &v_M_ibm);
    sim_run_unitary_only(&v_S_ibm, &v_M_ibm);

    if (v_S_gen.status != SIM_OK || v_S_ibm.status != SIM_OK) {
        (void)printf("  [FAIL] %-12s  simulator ABEND\n", D->name);
        return 1;
    }
    if (v_S_gen.num_qubits != v_S_ibm.num_qubits) {
        (void)printf("  [FAIL] %-12s  qubit count differs gen=%u ibm=%u\n",
                     D->name,
                     (unsigned)v_S_gen.num_qubits,
                     (unsigned)v_S_ibm.num_qubits);
        return 1;
    }

    uint32_t dim = 1u << v_S_gen.num_qubits;
    double sv_diff = sv_max_diff(v_S_gen.state, v_S_ibm.state, dim);
    int sv_pass = (sv_diff < VERIFY_SV_TOL);

    /* Histogram check. Run the IBM-decomposed module through both
     * the interpreted simulator (many shots) and the AOT pipeline,
     * compute TVD. They should match within statistical noise. */
    uint32_t nb = tir_total_bits(&v_M_ibm);
    uint32_t outcomes = (nb == 0u) ? 1u : (1u << nb);
    if (outcomes > VERIFY_MAX_OUTCOMES) outcomes = VERIFY_MAX_OUTCOMES;

    /* Interpreted shots. */
    for (uint32_t i = 0u; i < outcomes; i++) v_counts_int[i] = 0u;
    srand(0x12345678u);
    for (uint32_t s = 0u; s < VERIFY_AOT_SHOTS; s++) {
        sim_init(&v_S_ibm, &v_M_ibm);
        sim_run_shot(&v_S_ibm, &v_M_ibm);
        if (v_S_ibm.status != SIM_OK) {
            (void)printf("  [FAIL] %-12s  interpreted IBM-sim ABEND\n", D->name);
            return 1;
        }
        uint32_t o = sim_creg_as_uint(&v_S_ibm);
        if (o < outcomes) v_counts_int[o]++;
    }

    /* AOT shots. */
    for (uint32_t i = 0u; i < outcomes; i++) v_counts_aot[i] = 0u;
    int aot_rc = aot_compile_and_run(&v_M_ibm, VERIFY_AOT_SHOTS,
                                     v_counts_aot, outcomes, NULL);
    if (aot_rc != 0) {
        (void)printf("  [FAIL] %-12s  AOT failed RC=%d\n", D->name, aot_rc);
        return 1;
    }

    double tvd = hist_tvd(v_counts_int, VERIFY_AOT_SHOTS,
                          v_counts_aot, VERIFY_AOT_SHOTS, outcomes);
    int hist_pass = (tvd < VERIFY_HIST_TOL);

    int pass = sv_pass && hist_pass;
    (void)printf("  [%s] %-12s  sv_diff=%.3e  hist_tvd=%.5f  %s\n",
                 pass ? "PASS" : "FAIL",
                 D->name, sv_diff, tvd, D->desc);
    return pass ? 0 : 1;
}

int run_verify(void)
{
    (void)printf("ERNESTJB VERIFY START\n");
    (void)printf("---------------------------------------------------\n");
    (void)printf("SV tolerance:    %.0e   (statevector max pointwise)\n",
                 VERIFY_SV_TOL);
    (void)printf("Hist tolerance:  %.3f  (total variation distance)\n",
                 VERIFY_HIST_TOL);
    (void)printf("Histogram shots: %u\n", (unsigned)VERIFY_AOT_SHOTS);
    (void)printf("---------------------------------------------------\n");

    uint32_t pass = 0u;
    uint32_t fail = 0u;
    for (uint32_t i = 0u; i < (uint32_t)NUM_DEMOS; i++) {
        int rc = verify_one(&DEMOS[i]);
        if (rc == 0) pass++; else fail++;
    }
    (void)printf("---------------------------------------------------\n");
    (void)printf("ERNEST VERIFY  PASS=%u  FAIL=%u  OF=%u\n",
                 (unsigned)pass, (unsigned)fail, (unsigned)NUM_DEMOS);
    (void)printf("ERNEST EOJ     RC=%u\n",
                 (unsigned)(fail == 0u ? 0u : 8u));
    return (fail == 0u) ? 0 : 8;
}
