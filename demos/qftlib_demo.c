#include "../src/tir.h"
#include "../src/qstd.h"
#include "demos.h"

/*
 * The libqstd QFT round-trip demo. Build a four-qubit circuit that
 * prepares |0001> via an X on qubit zero, applies the quantum
 * Fourier transform, applies the inverse QFT, and measures.
 *
 * Because QFT and inverse QFT are exact inverses of one another,
 * the final state is identical to the prepared state. The histogram
 * should show 100% on |0001> and nothing anywhere else. If that
 * happens, the libqstd qft and iqft primitives are honest, the
 * controlled-phase implementations are honest, the SWAP order is
 * right, and the simulator did its sums correctly. Five things at
 * once with one demo.
 */
void build_qftlib_module(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "qftlib_roundtrip");
    uint32_t q = tir_qreg(M, "q", 4u);
    uint32_t c = tir_creg(M, "c", 4u);

    /* Prepare |0001>. */
    tir_emit_x(M, TIR_REF(q, 0u));

    /* Apply QFT then its inverse. */
    qstd_qft (M, q, 4u);
    qstd_iqft(M, q, 4u);

    /* Measure into the classical register. Output should match the
     * prepared state because QFT then inverse QFT is identity. */
    for (uint32_t i = 0u; i < 4u; i++) {
        tir_emit_measure(M, TIR_REF(q, i), TIR_REF(c, i));
    }
}
