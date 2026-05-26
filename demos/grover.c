#include "demos.h"

/*
 * Grover's algorithm on two qubits, looking for the marked element
 * |11> in a four-element search space.
 *
 * The full story takes a textbook chapter, but the punchline is
 * this: classical search through N items takes O(N) queries on
 * average; Grover does it in O(sqrt(N)) queries. For N=4 the
 * classical machine needs about 2 queries, Grover needs 1, and the
 * result is deterministic (the marked state arrives with probability
 * 1). For larger N the speedup is real and the result is
 * probabilistic but very likely.
 *
 * The two-qubit version is the cleanest demo because every step
 * uses gates we already have:
 *
 *   H, H                    uniform superposition over |00>..|11>
 *   CZ on (q[0], q[1])      oracle: flip phase of |11>
 *   diffusion operator      reflect amplitudes about the mean:
 *     H, H                     -- transform basis
 *     X, X                     -- flip about |00>
 *     CZ                       -- the actual reflection
 *     X, X                     -- undo basis flip
 *     H, H                     -- back to computational basis
 *   measure both            should read |11> every time
 *
 * Run this with eight thousand shots and the histogram should
 * show one tall column on |11> and nothing else. Quantum search
 * working as advertised.
 */
void build_grover_module(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "grover_2q");

    uint32_t q = tir_qreg(M, "q", 2u);
    uint32_t c = tir_creg(M, "c", 2u);

    /* Uniform superposition over all four basis states. */
    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_h(M, TIR_REF(q, 1u));

    /* Oracle: flip the phase of the marked state |11>. CZ does
     * exactly this in one gate, which is why two-qubit Grover is
     * such a pleasant demo. */
    tir_emit_cz(M, TIR_REF(q, 0u), TIR_REF(q, 1u));

    /* Diffusion operator. The pretty name for "reflect amplitudes
     * about their mean". After this, the marked state's amplitude
     * is amplified and everyone else's is suppressed. */
    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_h(M, TIR_REF(q, 1u));
    tir_emit_x(M, TIR_REF(q, 0u));
    tir_emit_x(M, TIR_REF(q, 1u));
    tir_emit_cz(M, TIR_REF(q, 0u), TIR_REF(q, 1u));
    tir_emit_x(M, TIR_REF(q, 0u));
    tir_emit_x(M, TIR_REF(q, 1u));
    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_h(M, TIR_REF(q, 1u));

    /* Measure. If the algorithm worked, we get |11>. */
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
    tir_emit_measure(M, TIR_REF(q, 1u), TIR_REF(c, 1u));
}
