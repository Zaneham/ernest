#ifndef ERNEST_QASM_H
#define ERNEST_QASM_H

#include "tir.h"

/*
 * The OpenQASM 3 emitter.
 *
 * OpenQASM is the closest the quantum world has to a portable
 * assembly language, in the same sort of way that English is the
 * closest the airline world has to a portable language. Several
 * vendors will read it, several more will run what their compilers
 * produce from it, and the result is that an OpenQASM file is the
 * universal traveller's cheque of quantum circuits.
 *
 * The emitter folds across the TIR instruction list and writes one
 * statement of OpenQASM per TIR operation. It does this in one pass,
 * keeps no state between calls, and never looks back. The classical
 * world is allowed to be that reassuring, occasionally.
 *
 * v0.1 emits the friendly bits: register declarations, named gates
 * from stdgates.inc, measurement, reset. Parameterised gates and
 * classical control turn up later, when there is somewhere in the
 * IR to put them.
 */
void ernest_emit_qasm3(const tir_module_t *M, FILE *out);

#endif /* ERNEST_QASM_H */
