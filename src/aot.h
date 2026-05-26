#ifndef ERNEST_AOT_H
#define ERNEST_AOT_H

#include "tir.h"
#include <stdio.h>
#include <stdint.h>

/*
 * Ahead-of-time compilation of a TIR module to native code.
 *
 * The interpreted simulator in sim.c walks the instruction list at
 * runtime and dispatches each gate through a switch. The AOT path
 * does the dispatch once, at compile time: it writes a self-
 * contained C program that contains the gate sequence inlined as
 * straight-line code, hands the program to gcc with the
 * optimisations turned up, then runs the resulting binary and
 * captures the histogram. The simulator's per-instruction cost goes
 * away. Whatever gcc can vectorise, gcc does.
 *
 * For circuits up to about sixteen qubits, where the statevector
 * fits comfortably in cache, this beats the interpreted path by a
 * meaningful margin. Comparisons against larger third-party
 * simulators are a benchmark conversation; the point of this
 * pipeline is to demonstrate that Ernest's C99 / no-dependencies /
 * mainframe-style architecture can produce competitively fast code
 * by leaning on the C compiler rather than against it.
 */

/*
 * Compile and run. M is the TIR module to simulate; shots is the
 * number of measurement shots to draw; counts is a pre-allocated
 * array of size num_outcomes that will be filled with per-outcome
 * shot counts. The progress stream gets a few mainframe-style
 * job-step lines and is allowed to be NULL if the caller does not
 * want them.
 *
 * Returns 0 on success, non-zero if the codegen, compile, or
 * execution step failed. Failure leaves the counts array in an
 * undefined state; the caller should not rely on partial data.
 */
int aot_compile_and_run(const tir_module_t *M, uint32_t shots,
                        uint32_t *counts, uint32_t num_outcomes,
                        FILE *progress);

#endif /* ERNEST_AOT_H */
