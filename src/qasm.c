#include "qasm.h"
#include <assert.h>

/*
 * The emitter walks the TIR module and writes OpenQASM 3 to the
 * output. The hard part is agreeing with TIR on what each gate means
 * and agreeing with OpenQASM on what each gate is called. The two
 * vocabularies mostly line up, which is a kindness, and kindnesses
 * are thin on the ground in compiler work.
 */

/*
 * Map a TIR opcode to its OpenQASM 3 mnemonic. Returns NULL for the
 * opcodes that have their own syntax instead of a gate name, like
 * register declarations and measurement, which get handled by the
 * main loop.
 */
static const char *qasm_gate_name(tir_op_t op)
{
    switch (op) {
    case TIR_GATE_H:    return "h";
    case TIR_GATE_X:    return "x";
    case TIR_GATE_Y:    return "y";
    case TIR_GATE_Z:    return "z";
    case TIR_GATE_S:    return "s";
    case TIR_GATE_T:    return "t";
    case TIR_GATE_SDG:  return "sdg";
    case TIR_GATE_TDG:  return "tdg";
    case TIR_GATE_SX:   return "sx";
    case TIR_GATE_RX:   return "rx";
    case TIR_GATE_RY:   return "ry";
    case TIR_GATE_RZ:   return "rz";
    case TIR_GATE_CX:   return "cx";
    case TIR_GATE_CZ:   return "cz";
    case TIR_GATE_CH:   return "ch";
    case TIR_GATE_CP:   return "cp";
    case TIR_GATE_SWAP: return "swap";
    case TIR_GATE_CCX:  return "ccx";
    default:            return NULL;
    }
}

/*
 * Print one register reference as name[index]. OpenQASM's syntax
 * happens to match TIR's printed form, on account of TIR having
 * copied OpenQASM, on account of the world having more important
 * problems than inventing a fourth notation for the same thing.
 */
static void emit_ref(const tir_module_t *M, FILE *out, uint32_t ref, int is_qreg)
{
    assert(M != NULL);
    assert(out != NULL);
    (void)fprintf(out, "%s[%u]",
                  tir_reg_name(M, TIR_REF_REG(ref), is_qreg),
                  (unsigned)TIR_REF_IDX(ref));
}

void ernest_emit_qasm3(const tir_module_t *M, FILE *out)
{
    assert(M != NULL);
    assert(out != NULL);

    /* Preamble. Version line and the stdgates include go first; that
     * is where h, x, cx and the gang are actually defined. Skip it and
     * you are hand-writing a 2x2 unitary for every gate. Which: no. */
    (void)fprintf(out, "OPENQASM 3.0;\n");
    (void)fprintf(out, "include \"stdgates.inc\";\n");

    /* Register declarations come first in OpenQASM 3. TIR carries
     * the declarations both in dedicated tables and as instructions,
     * so we emit them from the tables here and ignore the matching
     * instructions in the main loop below. */
    uint32_t nq = M->num_qregs;
    assert(nq <= (uint32_t)TIR_MAX_QREGS);
    for (uint32_t i = 0u; i < nq; i++) {
        (void)fprintf(out, "qubit[%u] %s;\n",
                      (unsigned)M->qregs[i].width,
                      &M->strings[M->qregs[i].name]);
    }

    uint32_t nc = M->num_cregs;
    assert(nc <= (uint32_t)TIR_MAX_CREGS);
    for (uint32_t i = 0u; i < nc; i++) {
        (void)fprintf(out, "bit[%u] %s;\n",
                      (unsigned)M->cregs[i].width,
                      &M->strings[M->cregs[i].name]);
    }

    /* The body. One TIR instruction translates to one OpenQASM
     * statement, with the exception of the declarations which were
     * handled above. The loop bound is M->num_insts which is
     * bounded by TIR_MAX_INSTS. */
    uint32_t ni = M->num_insts;
    assert(ni <= (uint32_t)TIR_MAX_INSTS);
    for (uint32_t i = 0u; i < ni; i++) {
        const tir_inst_t *I = &M->insts[i];
        const char *g = NULL;

        switch (I->op) {
        case TIR_QREG_DECL:
        case TIR_CREG_DECL:
            /* Already emitted from the register tables. */
            break;

        case TIR_GATE_H:   case TIR_GATE_X:   case TIR_GATE_Y:   case TIR_GATE_Z:
        case TIR_GATE_S:   case TIR_GATE_T:   case TIR_GATE_SDG: case TIR_GATE_TDG:
        case TIR_GATE_SX:
            g = qasm_gate_name((tir_op_t)I->op);
            assert(g != NULL);
            (void)fprintf(out, "%s ", g);
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ";\n");
            break;

        case TIR_GATE_RX: case TIR_GATE_RY: case TIR_GATE_RZ:
            g = qasm_gate_name((tir_op_t)I->op);
            assert(g != NULL);
            assert(I->operands[1] < M->num_angles);
            (void)fprintf(out, "%s(%.6f) ", g, M->angles[I->operands[1]]);
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ";\n");
            break;

        case TIR_GATE_CX: case TIR_GATE_CZ: case TIR_GATE_CH: case TIR_GATE_SWAP:
            g = qasm_gate_name((tir_op_t)I->op);
            assert(g != NULL);
            (void)fprintf(out, "%s ", g);
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ", ");
            emit_ref(M, out, I->operands[1], 1);
            (void)fprintf(out, ";\n");
            break;

        case TIR_GATE_CP:
            assert(I->operands[2] < M->num_angles);
            (void)fprintf(out, "cp(%.6f) ", M->angles[I->operands[2]]);
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ", ");
            emit_ref(M, out, I->operands[1], 1);
            (void)fprintf(out, ";\n");
            break;

        case TIR_GATE_CCX:
            (void)fprintf(out, "ccx ");
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ", ");
            emit_ref(M, out, I->operands[1], 1);
            (void)fprintf(out, ", ");
            emit_ref(M, out, I->operands[2], 1);
            (void)fprintf(out, ";\n");
            break;

        case TIR_MEASURE:
            /* OpenQASM 3's measurement syntax reads backwards from
             * what you'd guess. Classical bit on the left of the
             * equals, the measure keyword in the middle, qubit on
             * the right. The classical register goes first because
             * the classical register is the part anyone is ever
             * going to read. Fair, when you think about it. */
            emit_ref(M, out, I->operands[1], 0);
            (void)fprintf(out, " = measure ");
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ";\n");
            break;

        case TIR_RESET:
            (void)fprintf(out, "reset ");
            emit_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ";\n");
            break;

        default:
            (void)fprintf(out, "// unknown op %u\n", (unsigned)I->op);
            break;
        }
    }
}
