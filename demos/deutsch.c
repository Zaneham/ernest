#include "demos.h"

/*
 * Deutsch's algorithm. The original "quantum is genuinely different"
 * trick, published by David Deutsch in 1985.
 *
 * Setup: imagine someone gives you a black-box function f from one
 * bit to one bit. It is either constant (always returns the same
 * answer) or balanced (returns 0 for one input and 1 for the other).
 * The classical machine needs two queries to tell which is which.
 * The quantum machine needs one.
 *
 * The circuit:
 *
 *   q[0] starts in |0>           the input register
 *   q[1] starts in |1>           the output register
 *   H on both                    superposition
 *   apply the oracle             one call to f
 *   H on q[0]                    interfere the alternatives
 *   measure q[0]                 result: 0 if constant, 1 if balanced
 *
 * This demo wires up a balanced oracle (f(x) = x, implemented as
 * a CX from q[0] to q[1]). The measurement on q[0] should come
 * back as 1 every single time.
 */
void build_deutsch_module(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "deutsch_balanced");

    uint32_t q = tir_qreg(M, "q", 2u);
    uint32_t c = tir_creg(M, "c", 1u);

    /* Prepare q[1] in |1> by flipping it from |0>. */
    tir_emit_x(M, TIR_REF(q, 1u));

    /* Put both qubits into superposition. */
    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_h(M, TIR_REF(q, 1u));

    /* Oracle for the balanced function f(x) = x. A single CX from
     * the input register to the output register implements this. */
    tir_emit_cx(M, TIR_REF(q, 0u), TIR_REF(q, 1u));

    /* Bring the interference home. */
    tir_emit_h(M, TIR_REF(q, 0u));

    /* Measure only the input register. The output register has done
     * its job and is not needed. */
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
}
