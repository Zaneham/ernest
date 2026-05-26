#include "demos.h"

/*
 * The Bell state. Two qubits put into the superposition
 * (|00> + |11>) / sqrt(2). The qubits always agree with each other
 * when measured, which is the part Einstein found upsetting and the
 * rest of us call entanglement.
 *
 *   H  on q[0]       Put q[0] into a superposition.
 *   CX q[0] -> q[1]  Tie q[1] to q[0].
 *   measure both     Ask the universe what it decided.
 */
void build_bell_module(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "bell_state");

    uint32_t q = tir_qreg(M, "q", 2u);
    uint32_t c = tir_creg(M, "c", 2u);

    tir_emit_h(M, TIR_REF(q, 0u));
    tir_emit_cx(M, TIR_REF(q, 0u), TIR_REF(q, 1u));
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
    tir_emit_measure(M, TIR_REF(q, 1u), TIR_REF(c, 1u));
}
