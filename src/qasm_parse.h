#ifndef ERNEST_QASM_PARSE_H
#define ERNEST_QASM_PARSE_H

#include <stdint.h>
#include <assert.h>

#include "qasm_lex.h"
#include "mnote.h"
#include "tir.h"

/*
 * ERNESTPR. The OpenQASM 3 parser.
 *
 * Walks the token stream that ERNESTLX produced and assembles a
 * tir_module_t from it. Recursive descent, one function per
 * non-terminal, no surprises.
 *
 * v0.1 scope. Handles every construct the four built-in demos
 * emit, plus rotation gates with angle expressions, plus custom
 * gate definitions:
 *
 *     OPENQASM 3.0;
 *     include "stdgates.inc";
 *     qubit[N] name;
 *     bit[N] name;
 *     h q[i]; x q[i]; ... etc the named Cliffords
 *     rx(angle) q[i]; ry(angle) q[i]; rz(angle) q[i];
 *     cx q[i], q[j]; cz ...; swap ...;
 *     c[i] = measure q[i];
 *     measure q[i] -> c[i];
 *     reset q[i];
 *     gate name(params) qubits { body }
 *
 * Out of scope for v0.1 (turns up later):
 *     classical types beyond bit (int, float, bool, angle declarations)
 *     classical control flow (if/else, while, for)
 *     subroutines (def)
 *     timing and calibration (delay, box, defcal, barrier)
 *     complex numbers, arrays
 *
 * Errors flow through the MNOTE channel. The parser tries to keep
 * going past one error so the operator can see the whole picture
 * of what went wrong, the way HLASM does. Recovery is to the next
 * semicolon.
 */

/*
 * Parse the token stream in L into the module M. M must already be
 * initialised (via tir_module_init or equivalent) with a reasonable
 * name; the parser will not overwrite it. Diagnostics land in log.
 *
 * Returns the mainframe-style RC: 0 if no diagnostics rose above
 * INFO, 4 on warnings only, 8+ on errors. Pass through to the
 * compiler driver's exit code.
 */
int qasm_parse(const qasm_lexer_t *L,
               tir_module_t *M,
               mnote_log_t *log);

#endif /* ERNEST_QASM_PARSE_H */
