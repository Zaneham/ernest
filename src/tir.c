#include "tir.h"
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

/*
 * The implementation of TIR. The interesting questions live next door,
 * in the simulator and the OpenQASM emitter. This file is the back of
 * the marae, where the unglamorous work of putting things in arrays
 * and fetching them out again happens. Without it, the front of the
 * marae has nothing to talk about.
 *
 * Every function checks its arguments before doing anything
 * irreversible. The cost of an assertion at the top of a function is
 * approximately none. The cost of the function silently doing the
 * wrong thing for six months until someone with a debugger notices is
 * approximately your weekend.
 */

/* ----- Module init ------------------------------------------------ */
/*
 * memset on a struct full of arrays is the C99 way of asking the
 * universe to please tidy up. The universe generally agrees, though
 * it does so with the air of someone who has been asked to tidy a
 * teenager's room before.
 */
void tir_module_init(tir_module_t *M, const char *name)
{
    assert(M != NULL);
    assert(name != NULL);

    memset(M, 0, sizeof *M);
    M->module_name = tir_add_string(M, name);
}

uint32_t tir_add_string(tir_module_t *M, const char *s)
{
    assert(M != NULL);
    assert(s != NULL);

    uint32_t off = M->string_len;
    size_t n = strlen(s);

    /* The string table is bounded. Running out of room in it
     * generally means the program has wandered into the bush
     * without checking the map. Better to stop and ask for
     * directions than to keep walking. */
    if (off + (uint32_t)n + 1u > (uint32_t)TIR_MAX_STRINGS) {
        (void)fprintf(stderr, "tir: string table overflow\n");
        exit(1);
    }

    /* memcpy with the bounds checked above, the string checked for
     * non-null, and the destination part of a struct that was
     * zeroed at init. The lights are green all the way down. */
    memcpy(&M->strings[off], s, n);
    M->strings[off + n] = '\0';
    M->string_len += (uint32_t)n + 1u;
    return off;
}

/* ----- Register declarations ------------------------------------- */
/*
 * Qubit registers and bit registers are the same shape inside, so
 * one helper handles both. The caller passes in which table to grow
 * and which opcode the declaration instruction should carry. Sweet
 * as.
 */
static uint32_t reg_decl(tir_module_t *M,
                         tir_reg_t *table,
                         uint32_t *count,
                         uint32_t max,
                         const char *name,
                         uint32_t width,
                         tir_op_t op)
{
    assert(M != NULL);
    assert(table != NULL);
    assert(count != NULL);
    assert(name != NULL);
    assert(width > 0u);

    if (*count >= max) {
        (void)fprintf(stderr, "tir: register table overflow\n");
        exit(1);
    }
    if (M->num_insts >= (uint32_t)TIR_MAX_INSTS) {
        (void)fprintf(stderr, "tir: instruction table overflow\n");
        exit(1);
    }

    uint32_t idx = (*count)++;
    table[idx].name = tir_add_string(M, name);
    table[idx].width = width;

    /* Emit a declaration instruction as well, so a printed dump of
     * the module reads in order from top to bottom. The register
     * tables already hold the canonical copy. The instruction is
     * there for the sake of anyone reading the IR out loud. */
    tir_inst_t *I = &M->insts[M->num_insts++];
    I->op = (uint16_t)op;
    I->num_operands = 2u;
    I->subop = 0u;
    I->operands[0] = idx;
    I->operands[1] = width;
    return idx;
}

uint32_t tir_qreg(tir_module_t *M, const char *name, uint32_t width)
{
    assert(M != NULL);
    assert(name != NULL);
    return reg_decl(M, M->qregs, &M->num_qregs, TIR_MAX_QREGS,
                    name, width, TIR_QREG_DECL);
}

uint32_t tir_creg(tir_module_t *M, const char *name, uint32_t width)
{
    assert(M != NULL);
    assert(name != NULL);
    return reg_decl(M, M->cregs, &M->num_cregs, TIR_MAX_CREGS,
                    name, width, TIR_CREG_DECL);
}

/* ----- Instruction emission --------------------------------------- */
/*
 * Two little helpers, one for one-operand gates and one for two.
 * Everything in v0.1 fits in one of those two shapes, which keeps
 * the moving parts to a minimum. Number 8 wire engineering, really.
 */
static void emit1(tir_module_t *M, tir_op_t op, uint32_t a)
{
    assert(M != NULL);
    assert((uint32_t)op < (uint32_t)TIR_OP_COUNT);

    if (M->num_insts >= (uint32_t)TIR_MAX_INSTS) {
        (void)fprintf(stderr, "tir: instruction table overflow\n");
        exit(1);
    }
    tir_inst_t *I = &M->insts[M->num_insts++];
    I->op = (uint16_t)op;
    I->num_operands = 1u;
    I->subop = 0u;
    I->operands[0] = a;
    I->src_line = 0u;
    I->src_col  = 0u;
    I->origin_pass = (uint16_t)TIR_ORIGIN_USER;
    I->reserved = 0u;
}

static void emit2(tir_module_t *M, tir_op_t op, uint32_t a, uint32_t b)
{
    assert(M != NULL);
    assert((uint32_t)op < (uint32_t)TIR_OP_COUNT);

    if (M->num_insts >= (uint32_t)TIR_MAX_INSTS) {
        (void)fprintf(stderr, "tir: instruction table overflow\n");
        exit(1);
    }
    tir_inst_t *I = &M->insts[M->num_insts++];
    I->op = (uint16_t)op;
    I->num_operands = 2u;
    I->subop = 0u;
    I->operands[0] = a;
    I->operands[1] = b;
    I->src_line = 0u;
    I->src_col  = 0u;
    I->origin_pass = (uint16_t)TIR_ORIGIN_USER;
    I->reserved = 0u;
}

/*
 * The public emit functions are one-liners that hand the work to the
 * private helpers. Their job is to give the caller proper named
 * functions, so the type checker can object if someone tries to give
 * CX one operand instead of two.
 */
void tir_emit_h   (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_H,   q); }
void tir_emit_x   (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_X,   q); }
void tir_emit_y   (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_Y,   q); }
void tir_emit_z   (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_Z,   q); }
void tir_emit_s   (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_S,   q); }
void tir_emit_t   (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_T,   q); }
void tir_emit_sdg (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_SDG, q); }
void tir_emit_tdg (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_TDG, q); }
void tir_emit_sx  (tir_module_t *M, uint32_t q) { assert(M != NULL); emit1(M, TIR_GATE_SX,  q); }

/* Intern an angle and return its index. Angles are stored once and
 * referenced by their slot; the same theta showing up many times in
 * a circuit shares a single slot. Saves space and keeps printing
 * tidy. */
uint32_t tir_add_angle(tir_module_t *M, double theta)
{
    assert(M != NULL);
    if (M->num_angles >= (uint32_t)TIR_MAX_ANGLES) {
        (void)fprintf(stderr, "tir: angle table overflow\n");
        exit(1);
    }
    uint32_t idx = M->num_angles++;
    M->angles[idx] = theta;
    return idx;
}

/* Rotation gate emission. The qubit reference goes in operands[0]
 * the way a single-qubit gate normally does, and the angle index
 * goes in operands[1]. Two-operand instruction, one of each kind. */
void tir_emit_rx(tir_module_t *M, uint32_t q, double theta)
{
    assert(M != NULL);
    emit2(M, TIR_GATE_RX, q, tir_add_angle(M, theta));
}
void tir_emit_ry(tir_module_t *M, uint32_t q, double theta)
{
    assert(M != NULL);
    emit2(M, TIR_GATE_RY, q, tir_add_angle(M, theta));
}
void tir_emit_rz(tir_module_t *M, uint32_t q, double theta)
{
    assert(M != NULL);
    emit2(M, TIR_GATE_RZ, q, tir_add_angle(M, theta));
}

void tir_emit_cx   (tir_module_t *M, uint32_t c, uint32_t t) { assert(M != NULL); emit2(M, TIR_GATE_CX,   c, t); }
void tir_emit_cz   (tir_module_t *M, uint32_t c, uint32_t t) { assert(M != NULL); emit2(M, TIR_GATE_CZ,   c, t); }
void tir_emit_ch   (tir_module_t *M, uint32_t c, uint32_t t) { assert(M != NULL); emit2(M, TIR_GATE_CH,   c, t); }
void tir_emit_swap (tir_module_t *M, uint32_t a, uint32_t b) { assert(M != NULL); emit2(M, TIR_GATE_SWAP, a, b); }

/* Controlled phase. Two qubits and one angle, like a two-qubit cousin
 * of the rotation family. The angle goes into the angle table and its
 * index lives in operands[2], leaving operands[0] and operands[1] for
 * the control and target. */
void tir_emit_cp(tir_module_t *M, uint32_t ctrl, uint32_t tgt, double theta)
{
    assert(M != NULL);
    if (M->num_insts >= (uint32_t)TIR_MAX_INSTS) {
        (void)fprintf(stderr, "tir: instruction table overflow\n");
        exit(1);
    }
    uint32_t angle_idx = tir_add_angle(M, theta);
    tir_inst_t *I = &M->insts[M->num_insts++];
    I->op = (uint16_t)TIR_GATE_CP;
    I->num_operands = 3u;
    I->subop = 0u;
    I->operands[0] = ctrl;
    I->operands[1] = tgt;
    I->operands[2] = angle_idx;
    I->src_line = 0u;
    I->src_col  = 0u;
    I->origin_pass = (uint16_t)TIR_ORIGIN_USER;
    I->reserved = 0u;
}

/* Toffoli. Three qubits, no angle, two of them controls and one the
 * target. Operand slots three is plenty; the inline operand array can
 * carry four. */
void tir_emit_ccx(tir_module_t *M, uint32_t c1, uint32_t c2, uint32_t tgt)
{
    assert(M != NULL);
    if (M->num_insts >= (uint32_t)TIR_MAX_INSTS) {
        (void)fprintf(stderr, "tir: instruction table overflow\n");
        exit(1);
    }
    tir_inst_t *I = &M->insts[M->num_insts++];
    I->op = (uint16_t)TIR_GATE_CCX;
    I->num_operands = 3u;
    I->subop = 0u;
    I->operands[0] = c1;
    I->operands[1] = c2;
    I->operands[2] = tgt;
    I->src_line = 0u;
    I->src_col  = 0u;
    I->origin_pass = (uint16_t)TIR_ORIGIN_USER;
    I->reserved = 0u;
}

void tir_emit_measure(tir_module_t *M, uint32_t qref, uint32_t cref)
{
    assert(M != NULL);
    emit2(M, TIR_MEASURE, qref, cref);
}

void tir_emit_reset(tir_module_t *M, uint32_t qref)
{
    assert(M != NULL);
    emit1(M, TIR_RESET, qref);
}

/*
 * Stamp the most-recently-emitted instruction with a source line and
 * column. The parser uses this to attach the gate-name token's
 * position to each emitted instruction; downstream readers can then
 * answer "which line of the input file did this gate come from?".
 * Also bumps origin_pass to PARSE because anything with a source
 * location plainly came from the parser.
 */
void tir_stamp_loc(tir_module_t *M, uint32_t line, uint32_t col)
{
    assert(M != NULL);
    if (M->num_insts == 0u) {
        return;
    }
    tir_inst_t *I = &M->insts[M->num_insts - 1u];
    I->src_line   = line;
    I->src_col    = col;
    I->origin_pass = (uint16_t)TIR_ORIGIN_PARSE;
}

/* Origin pass to a short, mainframe-friendly name. */
const char *tir_origin_name(tir_origin_t o)
{
    switch (o) {
    case TIR_ORIGIN_USER:    return "USER";
    case TIR_ORIGIN_PARSE:   return "PARSE";
    case TIR_ORIGIN_OPTGCAN: return "OPTGCAN";
    case TIR_ORIGIN_OPTFUSE: return "OPTFUSE";
    case TIR_ORIGIN_OPTDECP: return "OPTDECP";
    case TIR_ORIGIN_ROUTE:   return "OPTROUT";
    case TIR_ORIGIN_COUNT:
    default:                 return "?";
    }
}

/* ----- Inspection ------------------------------------------------- */
/*
 * The opcode-to-name table follows OpenQASM conventions because
 * that's what TIR emits, and using the same word for the same thing
 * in both places spares the reader from learning the language
 * twice.
 */
const char *tir_op_name(tir_op_t op)
{
    switch (op) {
    case TIR_QREG_DECL: return "qreg";
    case TIR_CREG_DECL: return "creg";
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
    case TIR_MEASURE:   return "measure";
    case TIR_RESET:     return "reset";
    default:            return "?";
    }
}

const char *tir_reg_name(const tir_module_t *M, uint32_t reg_idx, int is_qreg)
{
    assert(M != NULL);
    if (is_qreg != 0) {
        assert(reg_idx < M->num_qregs);
        return &M->strings[M->qregs[reg_idx].name];
    }
    assert(reg_idx < M->num_cregs);
    return &M->strings[M->cregs[reg_idx].name];
}

static void print_ref(const tir_module_t *M, FILE *out, uint32_t ref, int is_qreg)
{
    assert(M != NULL);
    assert(out != NULL);
    (void)fprintf(out, "%s[%u]",
                  tir_reg_name(M, TIR_REF_REG(ref), is_qreg),
                  (unsigned)TIR_REF_IDX(ref));
}

/*
 * Print the module in TIR's own text form. Humans first, tools
 * second. If you catch yourself writing a parser for this output,
 * stop and use OpenQASM. That's what OpenQASM is there for.
 */
void tir_print_module(const tir_module_t *M, FILE *out)
{
    assert(M != NULL);
    assert(out != NULL);

    (void)fprintf(out, "module %s {\n", &M->strings[M->module_name]);

    /* Bounded loop. num_insts can't exceed TIR_MAX_INSTS, the
     * assertion below makes that explicit, and the static analyser
     * goes back to its tea. */
    uint32_t n = M->num_insts;
    assert(n <= (uint32_t)TIR_MAX_INSTS);
    for (uint32_t i = 0u; i < n; i++) {
        const tir_inst_t *I = &M->insts[i];
        (void)fprintf(out, "  ");
        switch (I->op) {
        case TIR_QREG_DECL:
            (void)fprintf(out, "qreg %s[%u]\n",
                          tir_reg_name(M, I->operands[0], 1),
                          (unsigned)I->operands[1]);
            break;
        case TIR_CREG_DECL:
            (void)fprintf(out, "creg %s[%u]\n",
                          tir_reg_name(M, I->operands[0], 0),
                          (unsigned)I->operands[1]);
            break;
        case TIR_GATE_H:   case TIR_GATE_X:   case TIR_GATE_Y:   case TIR_GATE_Z:
        case TIR_GATE_S:   case TIR_GATE_T:   case TIR_GATE_SDG: case TIR_GATE_TDG:
        case TIR_GATE_SX:
        case TIR_RESET:
            (void)fprintf(out, "%s ", tir_op_name((tir_op_t)I->op));
            print_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, "\n");
            break;
        case TIR_GATE_RX: case TIR_GATE_RY: case TIR_GATE_RZ:
            assert(I->operands[1] < M->num_angles);
            (void)fprintf(out, "%s(%.6f) ", tir_op_name((tir_op_t)I->op),
                          M->angles[I->operands[1]]);
            print_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, "\n");
            break;
        case TIR_GATE_CX: case TIR_GATE_CZ: case TIR_GATE_CH: case TIR_GATE_SWAP:
            (void)fprintf(out, "%s ", tir_op_name((tir_op_t)I->op));
            print_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ", ");
            print_ref(M, out, I->operands[1], 1);
            (void)fprintf(out, "\n");
            break;
        case TIR_GATE_CP:
            assert(I->operands[2] < M->num_angles);
            (void)fprintf(out, "cp(%.6f) ", M->angles[I->operands[2]]);
            print_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ", ");
            print_ref(M, out, I->operands[1], 1);
            (void)fprintf(out, "\n");
            break;
        case TIR_GATE_CCX:
            (void)fprintf(out, "ccx ");
            print_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, ", ");
            print_ref(M, out, I->operands[1], 1);
            (void)fprintf(out, ", ");
            print_ref(M, out, I->operands[2], 1);
            (void)fprintf(out, "\n");
            break;
        case TIR_MEASURE:
            (void)fprintf(out, "measure ");
            print_ref(M, out, I->operands[0], 1);
            (void)fprintf(out, " -> ");
            print_ref(M, out, I->operands[1], 0);
            (void)fprintf(out, "\n");
            break;
        default:
            (void)fprintf(out, "<unknown op %u>\n", (unsigned)I->op);
            break;
        }
    }
    (void)fprintf(out, "}\n");
}

/* ----- Cross-reference listing ----------------------------------- */
/*
 * Print an instruction in a single-line form, similar to
 * tir_print_module but without the leading whitespace and trailing
 * newline. Used by the XREF listing where each row needs the rest
 * of its columns intact.
 */
static void xref_print_inst_text(const tir_module_t *M, FILE *out,
                                 const tir_inst_t *I)
{
    assert(M != NULL);
    assert(out != NULL);
    assert(I != NULL);

    switch (I->op) {
    case TIR_QREG_DECL:
        (void)fprintf(out, "qreg %s[%u]",
                      tir_reg_name(M, I->operands[0], 1),
                      (unsigned)I->operands[1]);
        break;
    case TIR_CREG_DECL:
        (void)fprintf(out, "creg %s[%u]",
                      tir_reg_name(M, I->operands[0], 0),
                      (unsigned)I->operands[1]);
        break;
    case TIR_GATE_H:   case TIR_GATE_X:   case TIR_GATE_Y:   case TIR_GATE_Z:
    case TIR_GATE_S:   case TIR_GATE_T:   case TIR_GATE_SDG: case TIR_GATE_TDG:
    case TIR_GATE_SX:
    case TIR_RESET:
        (void)fprintf(out, "%s ", tir_op_name((tir_op_t)I->op));
        print_ref(M, out, I->operands[0], 1);
        break;
    case TIR_GATE_RX: case TIR_GATE_RY: case TIR_GATE_RZ:
        assert(I->operands[1] < M->num_angles);
        (void)fprintf(out, "%s(%.6f) ", tir_op_name((tir_op_t)I->op),
                      M->angles[I->operands[1]]);
        print_ref(M, out, I->operands[0], 1);
        break;
    case TIR_GATE_CX: case TIR_GATE_CZ: case TIR_GATE_CH: case TIR_GATE_SWAP:
        (void)fprintf(out, "%s ", tir_op_name((tir_op_t)I->op));
        print_ref(M, out, I->operands[0], 1);
        (void)fprintf(out, ", ");
        print_ref(M, out, I->operands[1], 1);
        break;
    case TIR_GATE_CP:
        assert(I->operands[2] < M->num_angles);
        (void)fprintf(out, "cp(%.6f) ", M->angles[I->operands[2]]);
        print_ref(M, out, I->operands[0], 1);
        (void)fprintf(out, ", ");
        print_ref(M, out, I->operands[1], 1);
        break;
    case TIR_GATE_CCX:
        (void)fprintf(out, "ccx ");
        print_ref(M, out, I->operands[0], 1);
        (void)fprintf(out, ", ");
        print_ref(M, out, I->operands[1], 1);
        (void)fprintf(out, ", ");
        print_ref(M, out, I->operands[2], 1);
        break;
    case TIR_MEASURE:
        (void)fprintf(out, "measure ");
        print_ref(M, out, I->operands[0], 1);
        (void)fprintf(out, " -> ");
        print_ref(M, out, I->operands[1], 0);
        break;
    default:
        (void)fprintf(out, "<unknown op %u>", (unsigned)I->op);
        break;
    }
}

void tir_print_xref(const tir_module_t *M, FILE *out)
{
    assert(M != NULL);
    assert(out != NULL);

    (void)fprintf(out,
        "ERNESTXR CROSS-REFERENCE LISTING  module=%s  insts=%u\n",
        &M->strings[M->module_name], (unsigned)M->num_insts);
    (void)fprintf(out,
        "================================================================\n");
    (void)fprintf(out,
        "  IDX  ORIGIN   SRC        INST\n");
    (void)fprintf(out,
        "  ---  -------  ---------  ----------------------------------\n");

    uint32_t counts[TIR_ORIGIN_COUNT];
    for (uint32_t k = 0u; k < (uint32_t)TIR_ORIGIN_COUNT; k++) {
        counts[k] = 0u;
    }

    uint32_t n = M->num_insts;
    assert(n <= (uint32_t)TIR_MAX_INSTS);
    for (uint32_t i = 0u; i < n; i++) {
        const tir_inst_t *I = &M->insts[i];

        /* Source location: "--:--" when we have none, otherwise
         * line:col padded to a fixed width. */
        char src_buf[16];
        if (I->src_line == 0u && I->src_col == 0u) {
            (void)snprintf(src_buf, sizeof src_buf, "--:--");
        } else {
            (void)snprintf(src_buf, sizeof src_buf, "%u:%u",
                           (unsigned)I->src_line, (unsigned)I->src_col);
        }

        (void)fprintf(out, "  %3u  %-7s  %-9s  ",
                      (unsigned)i,
                      tir_origin_name((tir_origin_t)I->origin_pass),
                      src_buf);
        xref_print_inst_text(M, out, I);
        (void)fputc('\n', out);

        if ((uint32_t)I->origin_pass < (uint32_t)TIR_ORIGIN_COUNT) {
            counts[I->origin_pass]++;
        }
    }

    (void)fprintf(out,
        "================================================================\n");
    (void)fprintf(out, "SUMMARY");
    for (uint32_t k = 0u; k < (uint32_t)TIR_ORIGIN_COUNT; k++) {
        (void)fprintf(out, "  %s=%u",
                      tir_origin_name((tir_origin_t)k), (unsigned)counts[k]);
    }
    (void)fprintf(out, "  TOTAL=%u\n", (unsigned)n);
}

/* ----- Flat-index helpers ----------------------------------------- */
/*
 * The simulator wants one long list of qubits. TIR keeps them in
 * named registers because humans read names better than indices.
 * These two helpers do the small translation in between, which is
 * the kind of bureaucratic act that separates working code from
 * code that almost works.
 */
uint32_t tir_total_qubits(const tir_module_t *M)
{
    assert(M != NULL);
    assert(M->num_qregs <= (uint32_t)TIR_MAX_QREGS);

    uint32_t total = 0u;
    uint32_t n = M->num_qregs;
    for (uint32_t i = 0u; i < n; i++) {
        total += M->qregs[i].width;
    }
    return total;
}

uint32_t tir_flat_qubit_index(const tir_module_t *M, uint32_t qref)
{
    assert(M != NULL);
    uint32_t r = TIR_REF_REG(qref);
    uint32_t i = TIR_REF_IDX(qref);
    assert(r < M->num_qregs);
    assert(i < M->qregs[r].width);

    uint32_t base = 0u;
    for (uint32_t k = 0u; k < r; k++) {
        base += M->qregs[k].width;
    }
    return base + i;
}

uint32_t tir_total_bits(const tir_module_t *M)
{
    assert(M != NULL);
    assert(M->num_cregs <= (uint32_t)TIR_MAX_CREGS);

    uint32_t total = 0u;
    uint32_t n = M->num_cregs;
    for (uint32_t i = 0u; i < n; i++) {
        total += M->cregs[i].width;
    }
    return total;
}

uint32_t tir_flat_bit_index(const tir_module_t *M, uint32_t cref)
{
    assert(M != NULL);
    uint32_t r = TIR_REF_REG(cref);
    uint32_t i = TIR_REF_IDX(cref);
    assert(r < M->num_cregs);
    assert(i < M->cregs[r].width);

    uint32_t base = 0u;
    for (uint32_t k = 0u; k < r; k++) {
        base += M->cregs[k].width;
    }
    return base + i;
}

/* ----- Structural comparison ------------------------------------- */
/*
 * Module-level equivalence. We compare in a sensible order: first
 * the register tables (most obvious mismatch), then the instruction
 * stream (subtler mismatches), and finally angle values (the most
 * likely to drift through floating-point rounding, so checked last
 * with a tolerance).
 *
 * The reason buffer holds at most one short sentence on what went
 * wrong. Truncated if it would overflow; we don't need a novel,
 * just a clue.
 */
static void diff_say(char *reason, uint32_t cap, const char *fmt, ...)
{
    if (reason == NULL || cap == 0u) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(reason, (size_t)cap, fmt, ap);
    va_end(ap);
}

int tir_module_diff(const tir_module_t *A, const tir_module_t *B,
                    char *reason, uint32_t reason_cap)
{
    assert(A != NULL);
    assert(B != NULL);

    /* Register counts. */
    if (A->num_qregs != B->num_qregs) {
        diff_say(reason, reason_cap, "qreg count: A=%u B=%u",
                 (unsigned)A->num_qregs, (unsigned)B->num_qregs);
        return 1;
    }
    if (A->num_cregs != B->num_cregs) {
        diff_say(reason, reason_cap, "creg count: A=%u B=%u",
                 (unsigned)A->num_cregs, (unsigned)B->num_cregs);
        return 2;
    }

    /* Register names and widths. */
    for (uint32_t i = 0u; i < A->num_qregs; i++) {
        if (A->qregs[i].width != B->qregs[i].width) {
            diff_say(reason, reason_cap, "qreg %u width: A=%u B=%u",
                     (unsigned)i,
                     (unsigned)A->qregs[i].width,
                     (unsigned)B->qregs[i].width);
            return 3;
        }
        const char *an = &A->strings[A->qregs[i].name];
        const char *bn = &B->strings[B->qregs[i].name];
        if (strcmp(an, bn) != 0) {
            diff_say(reason, reason_cap, "qreg %u name: A=%s B=%s",
                     (unsigned)i, an, bn);
            return 4;
        }
    }
    for (uint32_t i = 0u; i < A->num_cregs; i++) {
        if (A->cregs[i].width != B->cregs[i].width) {
            diff_say(reason, reason_cap, "creg %u width: A=%u B=%u",
                     (unsigned)i,
                     (unsigned)A->cregs[i].width,
                     (unsigned)B->cregs[i].width);
            return 5;
        }
        const char *an = &A->strings[A->cregs[i].name];
        const char *bn = &B->strings[B->cregs[i].name];
        if (strcmp(an, bn) != 0) {
            diff_say(reason, reason_cap, "creg %u name: A=%s B=%s",
                     (unsigned)i, an, bn);
            return 6;
        }
    }

    /* Instruction stream. */
    if (A->num_insts != B->num_insts) {
        diff_say(reason, reason_cap, "inst count: A=%u B=%u",
                 (unsigned)A->num_insts, (unsigned)B->num_insts);
        return 7;
    }
    for (uint32_t i = 0u; i < A->num_insts; i++) {
        const tir_inst_t *IA = &A->insts[i];
        const tir_inst_t *IB = &B->insts[i];
        if (IA->op != IB->op) {
            diff_say(reason, reason_cap,
                     "inst %u op: A=%s B=%s",
                     (unsigned)i,
                     tir_op_name((tir_op_t)IA->op),
                     tir_op_name((tir_op_t)IB->op));
            return 8;
        }
        if (IA->num_operands != IB->num_operands) {
            diff_say(reason, reason_cap,
                     "inst %u operand count: A=%u B=%u",
                     (unsigned)i,
                     (unsigned)IA->num_operands,
                     (unsigned)IB->num_operands);
            return 9;
        }
        /* Parameterised gates carry an angle index in one of their
         * operand slots; compare the angle value through the angle
         * table rather than the index, because two equivalent
         * circuits can intern the same angle at different slots and
         * we'd rather not call that a difference. RX/RY/RZ keep the
         * angle at operands[1], CP keeps it at operands[2]. */
        int has_angle = (IA->op == TIR_GATE_RX ||
                         IA->op == TIR_GATE_RY ||
                         IA->op == TIR_GATE_RZ ||
                         IA->op == TIR_GATE_CP);
        uint32_t angle_op_idx = (IA->op == TIR_GATE_CP) ? 2u : 1u;
        uint32_t check_ops = IA->num_operands;
        if (has_angle && check_ops > angle_op_idx) {
            check_ops = angle_op_idx;  /* skip angle index direct compare */
        }
        for (uint32_t k = 0u; k < check_ops; k++) {
            if (IA->operands[k] != IB->operands[k]) {
                diff_say(reason, reason_cap,
                         "inst %u operand %u: A=0x%08X B=0x%08X",
                         (unsigned)i, (unsigned)k,
                         (unsigned)IA->operands[k],
                         (unsigned)IB->operands[k]);
                return 10;
            }
        }
        if (has_angle) {
            assert(IA->operands[angle_op_idx] < A->num_angles);
            assert(IB->operands[angle_op_idx] < B->num_angles);
            double da = A->angles[IA->operands[angle_op_idx]];
            double db = B->angles[IB->operands[angle_op_idx]];
            double diff = da - db;
            if (diff < 0.0) {
                diff = -diff;
            }
            if (diff > 1e-9) {
                diff_say(reason, reason_cap,
                         "inst %u angle: A=%.9f B=%.9f", (unsigned)i, da, db);
                return 11;
            }
        }
    }

    return 0;
}
