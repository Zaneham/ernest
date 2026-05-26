#include "demos.h"

/*
 * The GHZ state. Three qubits in (|000> + |111>) / sqrt(2). The
 * honest extension of the Bell state: the three qubits all agree
 * with each other when measured, even though none of them knew
 * in advance which answer to give.
 *
 *   H  on q[0]       Superposition on q[0].
 *   CX q[0] -> q[1]  Entangle q[1] with q[0].
 *   CX q[1] -> q[2]  Entangle q[2] with the pair.
 *   measure all      The three qubits sing in unison.
 *
 * Greenberger, Horne, and Zeilinger published this in 1989 and it
 * has been useful for talking about non-locality ever since. It's
 * also the easiest three-qubit thing to build, which doesn't hurt.
 */
void build_ghz_module(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "ghz_state");

    uint32_t q = tir_qreg(M, "q", 3u);
    uint32_t c = tir_creg(M, "c", 3u);

    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_cx(M, TIR_REF(q, 0u), TIR_REF(q, 1u));
    tir_emit_cx(M, TIR_REF(q, 1u), TIR_REF(q, 2u));
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
    tir_emit_measure(M, TIR_REF(q, 1u), TIR_REF(c, 1u));
    tir_emit_measure(M, TIR_REF(q, 2u), TIR_REF(c, 2u));
}
