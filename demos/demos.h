#ifndef ERNEST_DEMOS_H
#define ERNEST_DEMOS_H

#include "../src/tir.h"

/*
 * Demo circuits. Each function fills a module with a small canonical
 * quantum circuit and returns. The dispatcher in main.c picks one
 * based on the subcommand the user typed.
 *
 * Adding a new demo: write a build_<name>_module(tir_module_t *M)
 * function in demos/<name>.c, declare it here, and add a case in
 * main.c's dispatcher. Three lines, plus the actual circuit.
 */

/* The Bell state. Two qubits, entangled. Hello world for quantum
 * computing. Histogram: ~50% |00>, ~50% |11>. */
void build_bell_module(tir_module_t *M);

/* GHZ state. Three qubits, all entangled. The honest extension of
 * the Bell state. Histogram: ~50% |000>, ~50% |111>. */
void build_ghz_module(tir_module_t *M);

/* Deutsch's algorithm. The smallest circuit that does something a
 * classical machine can't do in one query: decides whether a
 * one-bit function is constant or balanced. Two qubits, one query,
 * deterministic answer. Histogram: 100% on a specific outcome. */
void build_deutsch_module(tir_module_t *M);

/* Grover's algorithm on three qubits. Searches an eight-element
 * space for a marked element. With the right number of iterations
 * (two for n=3) the marked state arrives with very high
 * probability. Histogram: dominant peak on the marked state. */
void build_grover_module(tir_module_t *M);

/* libqstd QFT round-trip on four qubits. Prepares |0001>, applies
 * QFT then inverse QFT, measures. Histogram: 100% on |0001> because
 * QFT and inverse QFT cancel exactly. Tests every libqstd primitive
 * that goes into the QFT pair. */
void build_qftlib_module(tir_module_t *M);

/* libqstd Grover on two qubits via the standard-library primitives.
 * Uniform superposition, phase oracle for |11>, diffusion, measure.
 * Histogram: 100% on |11>. Demonstrates the shape of compositional
 * algorithm building that libqstd makes possible. */
void build_groverlib_module(tir_module_t *M);

/* Religious-correctness verifier. For each demo circuit, simulates
 * the abstract and the IBM-decomposed versions on the interpreted
 * simulator and compares their statevectors up to a global phase.
 * Then runs the IBM-decomposed module through both the interpreted
 * simulator and the AOT pipeline and computes total-variation
 * distance between the two histograms. Returns 0 only if every
 * circuit passed every check. Designed to be the gate you ship
 * through before burning real-hardware API time. */
int run_verify(void);

/* The DOOM demo. Encodes an image into a 16-qubit statevector via
 * QPIE, applies QFT then inverse QFT, samples a million measurement
 * outcomes, reconstructs the image from the histogram, and renders
 * both input and reconstruction as ASCII art. Image source is the
 * programmatic stamper by default, optionally a PPM file the user
 * supplies, optionally a Doom IWAD's TITLEPIC lump. */
int run_doom_demo(const char *image_path, const char *wad_path,
                  uint32_t shots);

/* The lexer test. Doesn't fit the build-a-module shape because it
 * exercises the compiler rather than a circuit. Builds the Bell
 * state internally, emits its QASM, runs the lexer over it, prints
 * the token stream. Returns the lexer's exit code (mainframe RC). */
int run_lex_test(void);

/* The round-trip harness. For each demo, builds a module, emits
 * QASM, lexes, parses, compares the round-tripped module against
 * the original. Returns 0 on a clean sweep, 8 on any mismatch. */
int run_roundtrip(void);

/* Deliberate ABEND. Builds a too-large circuit so the simulator
 * trips U0008 in sim_init, then prints the full mainframe-style
 * dump. Returns 8. Useful for confirming the ABEND path looks
 * sensible without breaking a real circuit. */
int run_abend_test(void);

/* Optimisation demo. Builds an obfuscated Bell state full of pairs
 * that cancel each other, runs the optimiser, shows before-and-after
 * TIR and QASM, simulates both circuits, demonstrates the Bell
 * distribution survives the transformation. Returns 0 on a clean
 * run, 8 on a simulator ABEND interfering with the comparison. */
int run_opt_test(void);

/* Qiskit corpus test. Reads each .qasm file in
 * test/qiskit_corpus/samples through Ernest's lexer and parser,
 * reports per-file PASS / parser-fail / lex-fail / io-fail, and
 * gives a tally at the end. Returns 0 if every file came through
 * clean, 4 otherwise. Used to track parser coverage against real
 * Qiskit-emitted input. */
int run_corpus_test(void);

/* Compile a user-supplied OpenQASM 3 file. Lex, parse, optimise,
 * print the TIR, optionally print a cross-reference listing, then
 * print the canonical OpenQASM 3 of the result. If do_aot is set,
 * also AOT-compiles the resulting circuit and runs the requested
 * number of shots, printing a histogram. Returns 0 on a clean run,
 * the relevant phase's RC on failure. */
int run_compile(const char *path, int do_xref,
                int do_no_opt, int target_is_ibm,
                int do_aot, uint32_t shots);

#endif /* ERNEST_DEMOS_H */
