#include "../src/tir.h"
#include "../src/qstd.h"
#include "demos.h"

/*
 * The libqstd Grover demo, built from the standard-library
 * primitives instead of laid out gate by gate. Two qubits, search
 * space of size four, marked state |11>. After one Grover iteration
 * the algorithm is supposed to land on the marked state with
 * probability one for n = 2; nothing about Grover gets fancier than
 * that until you go to larger search spaces.
 *
 * The demo composes three libqstd functions: uniform superposition,
 * a phase oracle marking the chosen state, and the diffusion
 * operator. Three function calls, one quantum algorithm. The shape
 * libqstd is supposed to enable.
 */
void build_groverlib_module(tir_module_t *M)
{
    assert(M != NULL);

    tir_module_init(M, "grover_libqstd");
    uint32_t q = tir_qreg(M, "q", 2u);
    uint32_t c = tir_creg(M, "c", 2u);

    /* Start in equal superposition over all four basis states. */
    qstd_uniform_superposition(M, q, 2u);

    /* Phase oracle flips the amplitude of |11>. */
    qstd_phase_oracle(M, q, 2u, 0x3u);

    /* Diffusion amplifies the marked amplitude, suppresses the
     * others. After this single iteration the |11> amplitude is
     * (mathematically) exactly one and everything else is zero. */
    qstd_grover_diffusion(M, q, 2u);

    /* Measure into the classical register. */
    tir_emit_measure(M, TIR_REF(q, 0u), TIR_REF(c, 0u));
    tir_emit_measure(M, TIR_REF(q, 1u), TIR_REF(c, 1u));
}
