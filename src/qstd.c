#include "qstd.h"

#include <math.h>
#include <assert.h>

/* Local M_PI, because MSVC's math.h doesn't grant it without a
 * feature macro and we'd rather not introduce one for one constant. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * QFT. For each qubit i, apply H, then a ladder of controlled-phase
 * gates with angles pi / 2^k for k = 1..(n-i-1). The angle ladder
 * grows shallower as you move down the register, which is the part
 * of the construction that earns the algorithm its O(n^2) gate count.
 * Final SWAPs reverse the bit order so the output reads in the
 * conventional direction.
 */
void qstd_qft(tir_module_t *M, uint32_t qreg, uint32_t n)
{
    assert(M != NULL);
    assert(n > 0u);

    for (uint32_t i = 0u; i < n; i++) {
        tir_emit_h(M, TIR_REF(qreg, i));
        for (uint32_t j = i + 1u; j < n; j++) {
            uint32_t k = j - i;
            double angle = M_PI / (double)(1u << k);
            /* CP is symmetric in its two qubits, so the textbook
             * "controlled-Rk between qubit j (control) and qubit i
             * (target)" maps onto our cp(angle) ctrl, tgt directly. */
            tir_emit_cp(M, TIR_REF(qreg, j), TIR_REF(qreg, i), angle);
        }
    }

    /* Reverse the bit order with SWAPs. */
    for (uint32_t i = 0u; i < n / 2u; i++) {
        tir_emit_swap(M, TIR_REF(qreg, i), TIR_REF(qreg, n - 1u - i));
    }
}

/*
 * Inverse QFT. Same shape, run backwards, angles negated. The SWAPs
 * come first this time (mirror image of QFT's trailing SWAPs), then
 * the controlled-phase ladders, then the Hadamards. H is self-inverse
 * so the Hadamard column is unchanged; the rotations get negative
 * angles.
 */
void qstd_iqft(tir_module_t *M, uint32_t qreg, uint32_t n)
{
    assert(M != NULL);
    assert(n > 0u);

    /* SWAPs first. */
    for (uint32_t i = 0u; i < n / 2u; i++) {
        tir_emit_swap(M, TIR_REF(qreg, i), TIR_REF(qreg, n - 1u - i));
    }

    /* Hadamards and inverse controlled-phase ladder, in reverse
     * order. The loop runs i from n-1 down to 0. */
    for (uint32_t ii = n; ii > 0u; ii--) {
        uint32_t i = ii - 1u;
        for (uint32_t jj = n; jj > i + 1u; jj--) {
            uint32_t j = jj - 1u;
            uint32_t k = j - i;
            double angle = -M_PI / (double)(1u << k);
            tir_emit_cp(M, TIR_REF(qreg, j), TIR_REF(qreg, i), angle);
        }
        tir_emit_h(M, TIR_REF(qreg, i));
    }
}

/*
 * Uniform superposition. H on each qubit takes |0..0> to
 * (1/sqrt(2^n)) * sum over all basis states. Trivial primitive,
 * included so higher-level code reads cleanly.
 */
void qstd_uniform_superposition(tir_module_t *M, uint32_t qreg, uint32_t n)
{
    assert(M != NULL);
    assert(n > 0u);
    for (uint32_t i = 0u; i < n; i++) {
        tir_emit_h(M, TIR_REF(qreg, i));
    }
}

/*
 * Grover diffusion operator on n qubits. The canonical decomposition
 * is H^n X^n (multi-controlled Z) X^n H^n. For n = 2 the controlled
 * Z is a plain CZ; for n = 3 we synthesise a CCZ from H * CCX * H
 * on the target qubit. Larger n needs an ancilla-aware MCZ which
 * we'll add when someone has a genuine use for it.
 */
void qstd_grover_diffusion(tir_module_t *M, uint32_t qreg, uint32_t n)
{
    assert(M != NULL);
    assert(n == 2u || n == 3u);

    /* Wrap in H^n. */
    for (uint32_t i = 0u; i < n; i++) tir_emit_h(M, TIR_REF(qreg, i));
    /* Wrap in X^n. */
    for (uint32_t i = 0u; i < n; i++) tir_emit_x(M, TIR_REF(qreg, i));

    /* The multi-controlled Z at the centre. */
    if (n == 2u) {
        tir_emit_cz(M, TIR_REF(qreg, 0), TIR_REF(qreg, 1));
    } else {
        /* n == 3: CCZ via H * CCX * H on target. */
        tir_emit_h  (M, TIR_REF(qreg, 2));
        tir_emit_ccx(M, TIR_REF(qreg, 0), TIR_REF(qreg, 1), TIR_REF(qreg, 2));
        tir_emit_h  (M, TIR_REF(qreg, 2));
    }

    /* Unwrap. */
    for (uint32_t i = 0u; i < n; i++) tir_emit_x(M, TIR_REF(qreg, i));
    for (uint32_t i = 0u; i < n; i++) tir_emit_h(M, TIR_REF(qreg, i));
}

/*
 * Phase oracle for a known bit-string. Flips the phase of the basis
 * state whose qubit pattern equals marked_state, leaves every other
 * basis state alone. The standard construction wraps X gates around
 * a multi-controlled Z; the X gates on qubit k are present whenever
 * bit k of marked_state is zero, so that the controls all see |1>
 * exactly when the input matches.
 */
void qstd_phase_oracle(tir_module_t *M, uint32_t qreg, uint32_t n,
                       uint32_t marked_state)
{
    assert(M != NULL);
    assert(n == 2u || n == 3u);

    /* X on each qubit whose marked bit is zero. */
    for (uint32_t i = 0u; i < n; i++) {
        if (((marked_state >> i) & 1u) == 0u) {
            tir_emit_x(M, TIR_REF(qreg, i));
        }
    }

    /* Multi-controlled Z at the centre. */
    if (n == 2u) {
        tir_emit_cz(M, TIR_REF(qreg, 0), TIR_REF(qreg, 1));
    } else {
        tir_emit_h  (M, TIR_REF(qreg, 2));
        tir_emit_ccx(M, TIR_REF(qreg, 0), TIR_REF(qreg, 1), TIR_REF(qreg, 2));
        tir_emit_h  (M, TIR_REF(qreg, 2));
    }

    /* Undo the X gates. */
    for (uint32_t i = 0u; i < n; i++) {
        if (((marked_state >> i) & 1u) == 0u) {
            tir_emit_x(M, TIR_REF(qreg, i));
        }
    }
}
