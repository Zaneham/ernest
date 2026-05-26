#ifndef ERNEST_QSTD_H
#define ERNEST_QSTD_H

#include "tir.h"

/*
 * libqstd. Ernest's quantum standard library.
 *
 * Think of this the way you'd think of libc for C, or std::algorithm
 * for C++. Each function in here emits a sequence of TIR instructions
 * that implements one of the well-known quantum primitives. Use the
 * functions the way you'd use printf or qsort: as named operations
 * that compose into larger programs, so you spend your time thinking
 * about the problem and not about how to wire up a controlled-phase
 * ladder one gate at a time.
 *
 * Each primitive in this file carries a citation in the comments so
 * the curious reader can chase down the original construction. The
 * implementations are conservative: they emit known-correct gate
 * sequences without doing anything clever about ancilla allocation
 * or depth optimisation. The optimiser running over the output is
 * welcome to find improvements; libqstd's job is just to be right.
 *
 * Naming convention: qstd_<verb> for the public API. Functions that
 * take a qreg index and a width assume the caller has declared a
 * register of at least that width.
 */

/*
 * Quantum Fourier Transform on the first n qubits of qreg.
 *
 * Emits H followed by a ladder of controlled-phase gates, then the
 * canonical reversing SWAPs at the end so the output bit order
 * matches the conventional "big-endian" form. For n qubits, the
 * gate count is n Hadamards + n(n-1)/2 controlled phases + floor(n/2)
 * SWAPs.
 *
 * Citation: Nielsen and Chuang, "Quantum Computation and Quantum
 * Information" (2010), Section 5.1.
 */
void qstd_qft(tir_module_t *M, uint32_t qreg, uint32_t n);

/*
 * Inverse Quantum Fourier Transform on the first n qubits of qreg.
 *
 * Same shape as qstd_qft but applied in reverse order with negated
 * phase angles. QFT followed by IQFT (or IQFT followed by QFT) is
 * the identity, which makes this a useful round-trip test for the
 * whole compilation pipeline.
 *
 * Citation: same as qstd_qft.
 */
void qstd_iqft(tir_module_t *M, uint32_t qreg, uint32_t n);

/*
 * Grover's diffusion operator on the first n qubits of qreg.
 *
 * Reflects the amplitude vector about the uniform-superposition
 * mean. Used inside the Grover iteration after an oracle has phase-
 * flipped the marked state. v1 supports n = 2 and n = 3; larger
 * widths need an ancilla-aware multi-controlled Z, which is filed
 * for a later edition.
 *
 * Citation: Grover, "A fast quantum mechanical algorithm for
 * database search" (1996), arXiv:quant-ph/9605043.
 */
void qstd_grover_diffusion(tir_module_t *M, uint32_t qreg, uint32_t n);

/*
 * Initialise the first n qubits of qreg to a uniform superposition
 * over all 2^n basis states. Trivial primitive: one H per qubit.
 * Included for readability of higher-level code that starts most
 * of its life with a uniform mixture.
 */
void qstd_uniform_superposition(tir_module_t *M, uint32_t qreg, uint32_t n);

/*
 * Bit-string oracle. Phase-flips the basis state whose bit pattern
 * matches the supplied mask (interpreting bit k of mask as the
 * desired value of qubit k). Implemented with X gates wrapping a
 * CZ-or-CCZ at the centre. n = 2 supported in v1; n = 3 uses CCZ
 * which we synthesise from CCX with Hadamards. Useful as a starter
 * oracle for Grover demonstrations on a known marked state.
 */
void qstd_phase_oracle(tir_module_t *M, uint32_t qreg, uint32_t n,
                       uint32_t marked_state);

#endif /* ERNEST_QSTD_H */
