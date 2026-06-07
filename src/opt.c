#include "opt.h"
#include "route.h"
#include "abend.h"
#include "snap.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

/* Local M_PI, because MSVC's math.h won't hand it over without a
 * feature macro, and one constant is not worth a feature macro. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define OPT_TWO_PI (2.0 * M_PI)

/*
 * The cancellation predicate. Two adjacent instructions cancel when
 * removing both leaves the circuit's observable behaviour unchanged.
 * For this v1 pass that means self-inverse gates on the same operand
 * (H, X, Y, Z, CX, CZ, SWAP) and the named inverse pairs (S with SDG,
 * T with TDG). SWAP also cancels when its operands are flipped on
 * the second instance, because SWAP doesn't have a preferred end.
 *
 * Two gates that would cancel after a commutation reordering are
 * not handled here. Commutation analysis is a different pass with
 * different bookkeeping and will turn up later when someone has a
 * long Sunday afternoon and a fresh cup of tea.
 */
static bool insts_cancel(const tir_inst_t *a, const tir_inst_t *b)
{
    assert(a != NULL);
    assert(b != NULL);

    /* Inverse pairs on opposite opcodes. */
    if (a->op != b->op) {
        bool s_pair = (a->op == TIR_GATE_S   && b->op == TIR_GATE_SDG)
                   || (a->op == TIR_GATE_SDG && b->op == TIR_GATE_S);
        bool t_pair = (a->op == TIR_GATE_T   && b->op == TIR_GATE_TDG)
                   || (a->op == TIR_GATE_TDG && b->op == TIR_GATE_T);
        if (s_pair || t_pair) {
            return a->num_operands == 1u
                && b->num_operands == 1u
                && a->operands[0] == b->operands[0];
        }
        return false;
    }

    /* Self-inverse, same opcode on both sides. */
    switch ((tir_op_t)a->op) {
    case TIR_GATE_H:
    case TIR_GATE_X:
    case TIR_GATE_Y:
    case TIR_GATE_Z:
        return a->num_operands == 1u
            && b->num_operands == 1u
            && a->operands[0] == b->operands[0];

    case TIR_GATE_CX:
    case TIR_GATE_CZ:
    case TIR_GATE_CH:
        return a->num_operands == 2u
            && b->num_operands == 2u
            && a->operands[0] == b->operands[0]
            && a->operands[1] == b->operands[1];

    case TIR_GATE_SWAP:
        /* SWAP is symmetric on its operands, so it cancels either way
         * round. CX, CZ, and CH have a control and a target and do
         * not get this courtesy. */
        return a->num_operands == 2u
            && b->num_operands == 2u
            && ((a->operands[0] == b->operands[0]
                 && a->operands[1] == b->operands[1])
             || (a->operands[0] == b->operands[1]
                 && a->operands[1] == b->operands[0]));

    case TIR_GATE_CCX:
        /* Toffoli is symmetric on its two controls (operands[0] and
         * operands[1]); the target is operands[2]. Two adjacent
         * Toffolis cancel when targets match and the two controls
         * agree as a set, in either order. */
        if (a->num_operands != 3u || b->num_operands != 3u) {
            return false;
        }
        if (a->operands[2] != b->operands[2]) {
            return false;
        }
        return (a->operands[0] == b->operands[0]
                && a->operands[1] == b->operands[1])
            || (a->operands[0] == b->operands[1]
                && a->operands[1] == b->operands[0]);

    case TIR_QREG_DECL:
    case TIR_CREG_DECL:
    case TIR_GATE_S:
    case TIR_GATE_T:
    case TIR_GATE_SDG:
    case TIR_GATE_TDG:
    case TIR_GATE_RX:
    case TIR_GATE_RY:
    case TIR_GATE_RZ:
    case TIR_GATE_CP:
    case TIR_MEASURE:
    case TIR_RESET:
    case TIR_OP_COUNT:
    default:
        return false;
    }
}

/*
 * Gate cancellation. Walk the instruction stream; wherever two
 * adjacent instructions cancel, leave them out of the rebuilt stream.
 * Repeat the walk until no further pairs are found, because each
 * removal can make a new cancelling adjacency out of the gates that
 * used to sit either side of the deleted pair.
 *
 * The outer loop is bounded by OPT_GCAN_MAX_ITER for the analyser's
 * peace of mind. In practice a circuit settles within a handful of
 * passes; the bound is generous because we can afford it to be.
 */
int opt_gate_cancellation(tir_module_t *M, opt_stats_t *stats,
                          mnote_log_t *log)
{
    assert(M != NULL);
    assert(stats != NULL);
    (void)log;

    stats->insts_in   = M->num_insts;
    stats->iterations = 0u;

    bool changed = true;
    while (changed && stats->iterations < OPT_GCAN_MAX_ITER) {
        changed = false;
        stats->iterations++;

        uint32_t write = 0u;
        uint32_t read  = 0u;
        while (read < M->num_insts) {
            if (read + 1u < M->num_insts
                && insts_cancel(&M->insts[read], &M->insts[read + 1u])) {
                read += 2u;
                changed = true;
                continue;
            }
            if (write != read) {
                M->insts[write] = M->insts[read];
            }
            write++;
            read++;
        }
        M->num_insts = write;
    }

    stats->insts_out     = M->num_insts;
    stats->insts_removed = stats->insts_in - stats->insts_out;

    return 0;
}

/*
 * Two adjacent instructions count as "the same parametric gate on
 * the same operand(s)" when they are the same opcode, the same
 * rotation axis, and applied to the same qubit (or qubit pair, for
 * CP). The angle operand is intentionally not compared: that's the
 * point, we're about to add the two angles together.
 */
static bool same_parametric(const tir_inst_t *a, const tir_inst_t *b)
{
    assert(a != NULL);
    assert(b != NULL);

    if (a->op != b->op) {
        return false;
    }
    switch ((tir_op_t)a->op) {
    case TIR_GATE_RX:
    case TIR_GATE_RY:
    case TIR_GATE_RZ:
        return a->num_operands >= 1u
            && b->num_operands >= 1u
            && a->operands[0] == b->operands[0];
    case TIR_GATE_CP:
        return a->num_operands >= 2u
            && b->num_operands >= 2u
            && a->operands[0] == b->operands[0]
            && a->operands[1] == b->operands[1];
    default:
        return false;
    }
}

/*
 * Which operand slot holds the angle index for a given parametric
 * gate. Single-qubit rotations use slot 1; CP uses slot 2 because
 * slots 0 and 1 are the qubit pair.
 */
static uint32_t angle_slot(uint16_t op)
{
    switch ((tir_op_t)op) {
    case TIR_GATE_RX:
    case TIR_GATE_RY:
    case TIR_GATE_RZ:
        return 1u;
    case TIR_GATE_CP:
        return 2u;
    default:
        return 0u;
    }
}

/*
 * Reduce an angle to the canonical range (-pi, pi]. Saves chasing
 * accumulating-drift bugs when a circuit gets repeatedly fused over
 * many iterations and the raw sum strays beyond the friendly range.
 */
static double canonical_angle(double theta)
{
    double t = fmod(theta, OPT_TWO_PI);
    if (t > M_PI) {
        t -= OPT_TWO_PI;
    } else if (t < -M_PI) {
        t += OPT_TWO_PI;
    }
    return t;
}

/*
 * Rotation fusion. Walk the instruction stream; wherever two
 * adjacent parametric gates target the same qubit(s) with the same
 * axis, replace them with one gate whose angle is the sum, reduced
 * to the canonical range. If the combined angle comes out to zero
 * the gate is identity, and both instructions leave together.
 *
 * Iterates until settled, bounded by OPT_FUSE_MAX_ITER. A single
 * fusion pass can create new adjacencies for the next iteration to
 * find: RZ a; RZ b; RZ c becomes RZ(a+b); RZ c after the first
 * walk, which then becomes RZ(a+b+c) on the second.
 */
int opt_rotation_fusion(tir_module_t *M, opt_stats_t *stats,
                        mnote_log_t *log)
{
    assert(M != NULL);
    assert(stats != NULL);
    (void)log;

    stats->insts_in   = M->num_insts;
    stats->iterations = 0u;

    bool changed = true;
    while (changed && stats->iterations < OPT_FUSE_MAX_ITER) {
        changed = false;
        stats->iterations++;

        uint32_t write = 0u;
        uint32_t read  = 0u;
        while (read < M->num_insts) {
            bool fused = false;
            if (read + 1u < M->num_insts
                && same_parametric(&M->insts[read], &M->insts[read + 1u])) {
                const tir_inst_t *a = &M->insts[read];
                const tir_inst_t *b = &M->insts[read + 1u];
                uint32_t slot = angle_slot(a->op);
                assert(a->operands[slot] < M->num_angles);
                assert(b->operands[slot] < M->num_angles);

                double combined = canonical_angle(
                    M->angles[a->operands[slot]] +
                    M->angles[b->operands[slot]]);

                if (fabs(combined) < OPT_FUSE_ZERO_EPS) {
                    /* Two halves of an identity. Both gates go. */
                    read += 2u;
                    changed = true;
                    fused = true;
                } else if (M->num_angles < (uint32_t)TIR_MAX_ANGLES) {
                    /* Append a new angle slot for the fused value
                     * and rewrite the first instruction to point at
                     * it. The second instruction is skipped. The
                     * surviving instruction keeps the first source
                     * location and picks up an OPTFUSE origin
                     * stamp, so a downstream reader can trace it
                     * back to the original rotation pair. */
                    uint32_t new_idx = tir_add_angle(M, combined);
                    M->insts[write] = *a;
                    M->insts[write].operands[slot] = new_idx;
                    M->insts[write].origin_pass = (uint16_t)TIR_ORIGIN_OPTFUSE;
                    write++;
                    read += 2u;
                    changed = true;
                    fused = true;
                }
                /* If the angle table was full, fused stays false and
                 * we fall through to the copy-as-is path; the
                 * unfortunate user gets warned by the angle table
                 * overflow message either way. */
            }
            if (fused) {
                continue;
            }
            if (write != read) {
                M->insts[write] = M->insts[read];
            }
            write++;
            read++;
        }
        M->num_insts = write;
    }

    stats->insts_out     = M->num_insts;
    stats->insts_removed = stats->insts_in - stats->insts_out;
    return 0;
}

/* ---------------------------------------------------------------- */
/* qlint: structural static analysis                                */
/* ---------------------------------------------------------------- */

/*
 * The QASM linter. A read-only pass that walks the instruction list
 * once, tracking what's happened to each qubit and each classical
 * bit, and emits an MNOTE for every rule violation it finds. Does
 * not modify the module. Findings flow through a dedicated
 * ERNESTQL log so the operator can pick the linter's voice out of
 * the optimisation chatter.
 *
 * The state arrays are static and capped, which keeps the pass
 * fast and free of allocation. Modules with more qubits or bits
 * than the cap get partial coverage; we never want a linter to be
 * the thing that stops a compilation.
 *
 * MNOTE codes:
 *   1  qubit declared but never operated on (INFO)
 *   2  qubit used but never measured        (INFO)
 *   3  gate on qubit after measurement      (WARN)
 *   4  qubit measured more than once        (WARN)
 *   5  classical bit written more than once (WARN)
 *   6  circuit has gates but no measurement (WARN)
 *   7  gate operands include the same qubit twice (ERROR)
 *   8  circuit has no gates                 (INFO)
 */

#define QLINT_MAX_QUBITS 1024u
#define QLINT_MAX_BITS   1024u

static uint8_t  ql_qubit_used     [QLINT_MAX_QUBITS];
static uint8_t  ql_qubit_measured [QLINT_MAX_QUBITS];
static uint32_t ql_qubit_decl_line[QLINT_MAX_QUBITS];
static uint32_t ql_qubit_decl_col [QLINT_MAX_QUBITS];
static uint8_t  ql_bit_written    [QLINT_MAX_BITS];

static mnote_log_t  qlint_log;
static const char  *qlint_src_name = NULL;

static void qlint_use_qubit(mnote_log_t *log,
                            const tir_inst_t *I,
                            uint32_t fq)
{
    if (fq >= QLINT_MAX_QUBITS) {
        return;
    }
    if (ql_qubit_measured[fq] && (tir_op_t)I->op != TIR_RESET) {
        mnote_emit(log, MNOTE_WARN, 3u, qlint_src_name,
                   I->src_line, I->src_col,
                   "gate %s on qubit %u after measurement (no reset)",
                   tir_op_name((tir_op_t)I->op), (unsigned)fq);
    }
    ql_qubit_used[fq] = 1u;
}

int opt_qlint(tir_module_t *M, opt_stats_t *stats, mnote_log_t *log)
{
    assert(M != NULL);
    assert(stats != NULL);
    (void)log;  /* qlint uses its own log so its voice is recognisable */

    stats->insts_in     = M->num_insts;
    stats->insts_out    = M->num_insts;
    stats->insts_removed = 0u;
    stats->iterations   = 1u;

    mnote_init(&qlint_log, "ERNESTQL");

    /* The module's name is whatever tir_module_init was given; for
     * parsed files that's the source path, which is exactly what we
     * want each diagnostic to point at. File-scope static so the
     * per-instruction helper can read it without an extra
     * parameter. */
    qlint_src_name = &M->strings[M->module_name];

    uint32_t total_q = tir_total_qubits(M);
    uint32_t total_b = tir_total_bits(M);
    if (total_q > QLINT_MAX_QUBITS) total_q = QLINT_MAX_QUBITS;
    if (total_b > QLINT_MAX_BITS)   total_b = QLINT_MAX_BITS;

    /* Zero the state arrays for this run. */
    for (uint32_t i = 0u; i < QLINT_MAX_QUBITS; i++) {
        ql_qubit_used[i]      = 0u;
        ql_qubit_measured[i]  = 0u;
        ql_qubit_decl_line[i] = 0u;
        ql_qubit_decl_col[i]  = 0u;
    }
    for (uint32_t i = 0u; i < QLINT_MAX_BITS; i++) {
        ql_bit_written[i] = 0u;
    }

    uint32_t total_gates    = 0u;
    uint32_t total_measures = 0u;

    /* First sweep: walk the instruction stream, mark state, emit
     * the rules that fire on individual instructions. */
    for (uint32_t i = 0u; i < M->num_insts; i++) {
        const tir_inst_t *I = &M->insts[i];

        switch ((tir_op_t)I->op) {
        case TIR_QREG_DECL: {
            /* Remember where each qubit was declared so a later
             * "unused qubit" MNOTE can point back to it. */
            uint32_t reg   = I->operands[0];
            uint32_t width = I->operands[1];
            uint32_t base  = 0u;
            for (uint32_t k = 0u; k < reg && k < M->num_qregs; k++) {
                base += M->qregs[k].width;
            }
            for (uint32_t j = 0u; j < width && (base + j) < QLINT_MAX_QUBITS; j++) {
                ql_qubit_decl_line[base + j] = I->src_line;
                ql_qubit_decl_col[base + j]  = I->src_col;
            }
            break;
        }
        case TIR_CREG_DECL:
            break;

        case TIR_GATE_H:   case TIR_GATE_X:   case TIR_GATE_Y:   case TIR_GATE_Z:
        case TIR_GATE_S:   case TIR_GATE_T:   case TIR_GATE_SDG: case TIR_GATE_TDG:
        case TIR_GATE_SX:
        case TIR_GATE_RX:  case TIR_GATE_RY:  case TIR_GATE_RZ: {
            uint32_t fq = tir_flat_qubit_index(M, I->operands[0]);
            qlint_use_qubit(&qlint_log, I, fq);
            total_gates++;
            break;
        }

        case TIR_GATE_CX:  case TIR_GATE_CZ:  case TIR_GATE_CH:
        case TIR_GATE_SWAP: case TIR_GATE_CP: {
            if (I->operands[0] == I->operands[1]) {
                mnote_emit(&qlint_log, MNOTE_ERROR, 7u, qlint_src_name,
                           I->src_line, I->src_col,
                           "gate %s has both operands on the same qubit",
                           tir_op_name((tir_op_t)I->op));
            }
            uint32_t fq0 = tir_flat_qubit_index(M, I->operands[0]);
            uint32_t fq1 = tir_flat_qubit_index(M, I->operands[1]);
            qlint_use_qubit(&qlint_log, I, fq0);
            qlint_use_qubit(&qlint_log, I, fq1);
            total_gates++;
            break;
        }

        case TIR_GATE_CCX: {
            int dup = (I->operands[0] == I->operands[1])
                   || (I->operands[0] == I->operands[2])
                   || (I->operands[1] == I->operands[2]);
            if (dup) {
                mnote_emit(&qlint_log, MNOTE_ERROR, 7u, qlint_src_name,
                           I->src_line, I->src_col,
                           "gate ccx has duplicate qubit operands");
            }
            uint32_t fq0 = tir_flat_qubit_index(M, I->operands[0]);
            uint32_t fq1 = tir_flat_qubit_index(M, I->operands[1]);
            uint32_t fq2 = tir_flat_qubit_index(M, I->operands[2]);
            qlint_use_qubit(&qlint_log, I, fq0);
            qlint_use_qubit(&qlint_log, I, fq1);
            qlint_use_qubit(&qlint_log, I, fq2);
            total_gates++;
            break;
        }

        case TIR_MEASURE: {
            uint32_t fq = tir_flat_qubit_index(M, I->operands[0]);
            uint32_t fb = tir_flat_bit_index  (M, I->operands[1]);
            if (fq < QLINT_MAX_QUBITS) {
                if (ql_qubit_measured[fq]) {
                    mnote_emit(&qlint_log, MNOTE_WARN, 4u, qlint_src_name,
                               I->src_line, I->src_col,
                               "qubit %u measured more than once without reset",
                               (unsigned)fq);
                }
                ql_qubit_used[fq]     = 1u;
                ql_qubit_measured[fq] = 1u;
            }
            if (fb < QLINT_MAX_BITS) {
                if (ql_bit_written[fb]) {
                    mnote_emit(&qlint_log, MNOTE_WARN, 5u, qlint_src_name,
                               I->src_line, I->src_col,
                               "classical bit %u written more than once",
                               (unsigned)fb);
                }
                ql_bit_written[fb] = 1u;
            }
            total_measures++;
            break;
        }

        case TIR_RESET: {
            uint32_t fq = tir_flat_qubit_index(M, I->operands[0]);
            if (fq < QLINT_MAX_QUBITS) {
                ql_qubit_measured[fq] = 0u;
                ql_qubit_used[fq]     = 1u;
            }
            break;
        }

        case TIR_OP_COUNT:
        default:
            break;
        }
    }

    /* Second sweep: per-qubit and circuit-wide findings. */
    for (uint32_t i = 0u; i < total_q; i++) {
        if (!ql_qubit_used[i]) {
            mnote_emit(&qlint_log, MNOTE_INFO, 1u, qlint_src_name,
                       ql_qubit_decl_line[i], ql_qubit_decl_col[i],
                       "qubit %u declared but never operated on",
                       (unsigned)i);
        } else if (!ql_qubit_measured[i]) {
            mnote_emit(&qlint_log, MNOTE_INFO, 2u, qlint_src_name, 0u, 0u,
                       "qubit %u used but never measured",
                       (unsigned)i);
        }
    }

    if (total_gates > 0u && total_measures == 0u) {
        mnote_emit(&qlint_log, MNOTE_WARN, 6u, qlint_src_name, 0u, 0u,
                   "circuit has %u gates but no measurements",
                   (unsigned)total_gates);
    }
    if (total_gates == 0u) {
        mnote_emit(&qlint_log, MNOTE_INFO, 8u, qlint_src_name, 0u, 0u,
                   "circuit has no gates");
    }

    /* Print qlint's findings under its own module name so a reader
     * can tell the linter's voice from the optimiser's. */
    if (qlint_log.num_notes > 0u) {
        mnote_print(&qlint_log, stdout);
    }

    return (int)mnote_exit_code(&qlint_log);
}

/* ---------------------------------------------------------------- */
/* IBM native-gate decomposition                                    */
/* ---------------------------------------------------------------- */

/*
 * Scratch buffer for decomposition. The pass walks the input
 * instruction list once, writing the expanded form into this buffer,
 * then copies it back over M->insts at the end. Static so we don't
 * pay heap-allocation costs and don't add a new failure mode.
 */
static tir_inst_t decomp_scratch[TIR_MAX_INSTS];

/*
 * Provenance under decomposition. Each switch case in
 * opt_decompose_ibm sets these from the source instruction's
 * src_line / src_col before calling the decomp helpers, so every
 * native gate emitted by the helpers inherits the original
 * abstract-gate's source location. The origin_pass stamp is
 * OPTDECP on every emitted instruction.
 */
static uint32_t decomp_src_line = 0u;
static uint32_t decomp_src_col  = 0u;

static void stamp_decomp_prov(tir_inst_t *I)
{
    I->src_line   = decomp_src_line;
    I->src_col    = decomp_src_col;
    I->origin_pass = (uint16_t)TIR_ORIGIN_OPTDECP;
    I->reserved   = 0u;
}

static void emit_inst_1q(uint32_t *w, tir_op_t op, uint32_t q)
{
    assert(*w < (uint32_t)TIR_MAX_INSTS);
    tir_inst_t *I = &decomp_scratch[(*w)++];
    I->op = (uint16_t)op;
    I->num_operands = 1u;
    I->subop = 0u;
    I->operands[0] = q;
    stamp_decomp_prov(I);
}

static void emit_inst_2q(uint32_t *w, tir_op_t op, uint32_t a, uint32_t b)
{
    assert(*w < (uint32_t)TIR_MAX_INSTS);
    tir_inst_t *I = &decomp_scratch[(*w)++];
    I->op = (uint16_t)op;
    I->num_operands = 2u;
    I->subop = 0u;
    I->operands[0] = a;
    I->operands[1] = b;
    stamp_decomp_prov(I);
}

static void emit_rz_scratch(uint32_t *w, tir_module_t *M, uint32_t q, double theta)
{
    assert(*w < (uint32_t)TIR_MAX_INSTS);
    uint32_t aidx = tir_add_angle(M, theta);
    tir_inst_t *I = &decomp_scratch[(*w)++];
    I->op = (uint16_t)TIR_GATE_RZ;
    I->num_operands = 2u;
    I->subop = 0u;
    I->operands[0] = q;
    I->operands[1] = aidx;
    stamp_decomp_prov(I);
}

/* Decomposition templates. Each writes its native expansion into
 * the scratch buffer starting at *w. The expansions are textbook
 * forms; an OPTGCAN / OPTFUSE pass after this one will collapse
 * the adjacent RZ chains they tend to produce. */

static void decomp_h(uint32_t *w, tir_module_t *M, uint32_t q)
{
    /* H = RZ(pi/2) SX RZ(pi/2)  (up to global phase) */
    emit_rz_scratch(w, M, q, M_PI / 2.0);
    emit_inst_1q   (w, TIR_GATE_SX, q);
    emit_rz_scratch(w, M, q, M_PI / 2.0);
}

static void decomp_y(uint32_t *w, tir_module_t *M, uint32_t q)
{
    /* Y = RZ(pi) X  (up to global phase) */
    emit_rz_scratch(w, M, q, M_PI);
    emit_inst_1q   (w, TIR_GATE_X, q);
}

static void decomp_rx(uint32_t *w, tir_module_t *M, uint32_t q, double theta)
{
    /* RX(t) = H RZ(t) H, expanded and the middle RZ chain collapsed:
     *   RZ(pi/2) SX RZ(pi + t) SX RZ(pi/2) */
    emit_rz_scratch(w, M, q, M_PI / 2.0);
    emit_inst_1q   (w, TIR_GATE_SX, q);
    emit_rz_scratch(w, M, q, M_PI + theta);
    emit_inst_1q   (w, TIR_GATE_SX, q);
    emit_rz_scratch(w, M, q, M_PI / 2.0);
}

static void decomp_ry(uint32_t *w, tir_module_t *M, uint32_t q, double theta)
{
    /* RY(t) = SXDG RZ(t) SX, with SXDG = SX^3 = SX SX SX.
     * Circuit order: SX RZ(t) SX SX SX. */
    emit_inst_1q   (w, TIR_GATE_SX, q);
    emit_rz_scratch(w, M, q, theta);
    emit_inst_1q   (w, TIR_GATE_SX, q);
    emit_inst_1q   (w, TIR_GATE_SX, q);
    emit_inst_1q   (w, TIR_GATE_SX, q);
}

static void decomp_cz(uint32_t *w, tir_module_t *M, uint32_t c, uint32_t t)
{
    /* CZ = (I tensor H) CX (I tensor H)
     * Circuit: H tgt; CX ctrl, tgt; H tgt. */
    decomp_h(w, M, t);
    emit_inst_2q(w, TIR_GATE_CX, c, t);
    decomp_h(w, M, t);
}

static void decomp_cp(uint32_t *w, tir_module_t *M,
                      uint32_t c, uint32_t t, double theta)
{
    /* Standard CP decomposition into RZ + CX:
     *   RZ(t/2) ctrl;
     *   CX ctrl, tgt;
     *   RZ(-t/2) tgt;
     *   CX ctrl, tgt;
     *   RZ(t/2) tgt; */
    emit_rz_scratch(w, M, c, theta * 0.5);
    emit_inst_2q   (w, TIR_GATE_CX, c, t);
    emit_rz_scratch(w, M, t, -theta * 0.5);
    emit_inst_2q   (w, TIR_GATE_CX, c, t);
    emit_rz_scratch(w, M, t, theta * 0.5);
}

static void decomp_swap(uint32_t *w, uint32_t a, uint32_t b)
{
    /* SWAP = CX(a,b) CX(b,a) CX(a,b). */
    emit_inst_2q(w, TIR_GATE_CX, a, b);
    emit_inst_2q(w, TIR_GATE_CX, b, a);
    emit_inst_2q(w, TIR_GATE_CX, a, b);
}

static void decomp_ch(uint32_t *w, tir_module_t *M, uint32_t c, uint32_t t)
{
    /* CH via the textbook S H T CX TDG H SDG sequence on target. */
    emit_rz_scratch(w, M, t, M_PI / 2.0);          /* S */
    decomp_h(w, M, t);                              /* H */
    emit_rz_scratch(w, M, t, M_PI / 4.0);          /* T */
    emit_inst_2q   (w, TIR_GATE_CX, c, t);          /* CX */
    emit_rz_scratch(w, M, t, -M_PI / 4.0);         /* TDG */
    decomp_h(w, M, t);                              /* H */
    emit_rz_scratch(w, M, t, -M_PI / 2.0);         /* SDG */
}

static void decomp_ccx(uint32_t *w, tir_module_t *M,
                       uint32_t c1, uint32_t c2, uint32_t t)
{
    /* Standard Toffoli expansion into 6 CX + 9 single-qubit
     * abstract gates (2H, 4T, 3TDG). Each H expands to 3 native
     * gates, each T/TDG to 1 RZ. The output is ~19 native gates;
     * OPTFUSE on a later run collapses some of the adjacent RZ
     * chains. */
    decomp_h(w, M, t);                              /* H t */
    emit_inst_2q   (w, TIR_GATE_CX, c2, t);
    emit_rz_scratch(w, M, t, -M_PI / 4.0);         /* TDG t */
    emit_inst_2q   (w, TIR_GATE_CX, c1, t);
    emit_rz_scratch(w, M, t, M_PI / 4.0);          /* T t */
    emit_inst_2q   (w, TIR_GATE_CX, c2, t);
    emit_rz_scratch(w, M, t, -M_PI / 4.0);         /* TDG t */
    emit_inst_2q   (w, TIR_GATE_CX, c1, t);
    emit_rz_scratch(w, M, c2, M_PI / 4.0);         /* T c2 */
    emit_rz_scratch(w, M, t,  M_PI / 4.0);         /* T t */
    decomp_h(w, M, t);                              /* H t */
    emit_inst_2q   (w, TIR_GATE_CX, c1, c2);
    emit_rz_scratch(w, M, c1, M_PI / 4.0);         /* T c1 */
    emit_rz_scratch(w, M, c2, -M_PI / 4.0);        /* TDG c2 */
    emit_inst_2q   (w, TIR_GATE_CX, c1, c2);
}

int opt_decompose_ibm(tir_module_t *M, opt_stats_t *stats, mnote_log_t *log)
{
    assert(M != NULL);
    assert(stats != NULL);
    (void)log;

    stats->insts_in   = M->num_insts;
    stats->iterations = 1u;

    uint32_t w = 0u;
    for (uint32_t r = 0u; r < M->num_insts; r++) {
        const tir_inst_t *I = &M->insts[r];

        /* Stash the source instruction's provenance for the decomp
         * helpers to pick up. Anything they emit will inherit this
         * line/column so a cross-reference listing of the output
         * can point each native gate back to the abstract gate that
         * produced it. */
        decomp_src_line = I->src_line;
        decomp_src_col  = I->src_col;

        switch ((tir_op_t)I->op) {
        /* Pass through unchanged: register decls, native gates, and
         * measurement/reset. Their provenance is whatever the parser
         * or earlier pass already stamped, which is exactly what we
         * want to preserve. */
        case TIR_QREG_DECL:
        case TIR_CREG_DECL:
        case TIR_GATE_X:
        case TIR_GATE_SX:
        case TIR_GATE_RZ:
        case TIR_GATE_CX:
        case TIR_MEASURE:
        case TIR_RESET:
            decomp_scratch[w++] = *I;
            break;

        /* Single-qubit abstract gates that bottom out in RZ. */
        case TIR_GATE_Z:
            emit_rz_scratch(&w, M, I->operands[0], M_PI);
            break;
        case TIR_GATE_S:
            emit_rz_scratch(&w, M, I->operands[0], M_PI / 2.0);
            break;
        case TIR_GATE_T:
            emit_rz_scratch(&w, M, I->operands[0], M_PI / 4.0);
            break;
        case TIR_GATE_SDG:
            emit_rz_scratch(&w, M, I->operands[0], -M_PI / 2.0);
            break;
        case TIR_GATE_TDG:
            emit_rz_scratch(&w, M, I->operands[0], -M_PI / 4.0);
            break;

        /* Single-qubit gates that need an SX or two as well. */
        case TIR_GATE_H:
            decomp_h(&w, M, I->operands[0]);
            break;
        case TIR_GATE_Y:
            decomp_y(&w, M, I->operands[0]);
            break;
        case TIR_GATE_RX:
            assert(I->operands[1] < M->num_angles);
            decomp_rx(&w, M, I->operands[0], M->angles[I->operands[1]]);
            break;
        case TIR_GATE_RY:
            assert(I->operands[1] < M->num_angles);
            decomp_ry(&w, M, I->operands[0], M->angles[I->operands[1]]);
            break;

        /* Two-qubit decompositions. */
        case TIR_GATE_CZ:
            decomp_cz(&w, M, I->operands[0], I->operands[1]);
            break;
        case TIR_GATE_CH:
            decomp_ch(&w, M, I->operands[0], I->operands[1]);
            break;
        case TIR_GATE_CP:
            assert(I->operands[2] < M->num_angles);
            decomp_cp(&w, M, I->operands[0], I->operands[1],
                      M->angles[I->operands[2]]);
            break;
        case TIR_GATE_SWAP:
            decomp_swap(&w, I->operands[0], I->operands[1]);
            break;

        /* Three-qubit decomposition. */
        case TIR_GATE_CCX:
            decomp_ccx(&w, M, I->operands[0], I->operands[1], I->operands[2]);
            break;

        case TIR_OP_COUNT:
        default:
            /* Anything we don't know how to decompose passes
             * through unchanged. The simulator will object later if
             * it sees a non-native gate, which is the right place
             * to complain. */
            decomp_scratch[w++] = *I;
            break;
        }
    }

    /* Copy the scratch back into M. The angle table's growth is
     * already absorbed by tir_add_angle along the way. Dead angle
     * slots (referenced only by old, now-overwritten instructions)
     * stay in the table as harmless ballast. */
    for (uint32_t i = 0u; i < w; i++) {
        M->insts[i] = decomp_scratch[i];
    }
    M->num_insts = w;

    stats->insts_out = w;
    stats->insts_removed = (w < stats->insts_in)
                         ? (stats->insts_in - w) : 0u;
    return 0;
}

/*
 * The pass registry. Order matters: passes run top-to-bottom, and a
 * later pass sees the output of an earlier one. Add a pass by
 * appending here; nothing else in the file needs editing.
 *
 * Running rotation fusion first sometimes lets gate cancellation
 * pick up pairs the fusion missed; running cancellation first is
 * cheaper because it leaves less work for the (more expensive)
 * fusion pass. We order cancellation first for speed and let fusion
 * mop up afterwards. Decomposition comes last, on the IBM target,
 * because it expands gates and we'd rather optimise the smaller
 * abstract circuit before exploding it into native form.
 */
static const opt_pass_t PASSES[] = {
    /* QLINT runs first so its findings are about what the user
     * actually wrote, not the post-optimisation residue. It never
     * modifies the module; it just observes and reports. */
    {
        .name        = "ERNESTQL",
        .desc        = "QASM LINT",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_GENERIC,
        .run         = opt_qlint
    },
    {
        .name        = "OPTGCAN",
        .desc        = "GATE CANCELLATION",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_GENERIC,  /* runs for any target */
        .run         = opt_gate_cancellation
    },
    {
        .name        = "OPTFUSE",
        .desc        = "ROTATION FUSION",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_GENERIC,
        .run         = opt_rotation_fusion
    },
    /* Qubit routing. Runs before decomposition so the SWAPs it
     * inserts get decomposed to 3 CXs by OPTDECP along with the
     * abstract SWAPs already in the circuit. Target-agnostic; the
     * pass is a no-op when no coupling graph has been set. */
    {
        .name        = "OPTROUT",
        .desc        = "SABRE QUBIT ROUTING",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_GENERIC,
        .run         = opt_route
    },
    {
        .name        = "OPTDECP",
        .desc        = "IBM DECOMPOSITION",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_IBM,
        .run         = opt_decompose_ibm
    },
    /* Post-decomposition cleanup. Decomposition leaves long chains
     * of adjacent RZ that the fusion pass can collapse and a few
     * self-inverse pairs that the cancellation pass can drop. These
     * two entries run only on the IBM target, after the expansion,
     * to tidy up. */
    {
        .name        = "OPTFUSE2",
        .desc        = "ROTATION FUSION (POST-DECOMP)",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_IBM,
        .run         = opt_rotation_fusion
    },
    {
        .name        = "OPTGCAN2",
        .desc        = "GATE CANCELLATION (POST-DECOMP)",
        .min_level   = OPT_LEVEL_BASIC,
        .target_only = OPT_TARGET_IBM,
        .run         = opt_gate_cancellation
    }
};

#define NUM_PASSES (sizeof PASSES / sizeof PASSES[0])

const opt_pass_t *opt_passes(uint32_t *count)
{
    if (count != NULL) {
        *count = (uint32_t)NUM_PASSES;
    }
    return PASSES;
}

/* Short label for the target, for the job-step header. */
static const char *target_name(opt_target_t t)
{
    switch (t) {
    case OPT_TARGET_GENERIC: return "GENERIC";
    case OPT_TARGET_IBM:     return "IBM";
    default:                 return "?";
    }
}

int opt_run(tir_module_t *M, uint32_t opt_level,
            opt_target_t target, mnote_log_t *log)
{
    assert(M != NULL);

    if (opt_level == OPT_LEVEL_NONE) {
        return 0;
    }

    /* Pipeline stage. If an ABEND fires inside a pass, the dump
     * will tell the operator the optimiser was the thing running. */
    abend_set_stage("OPT", "(driver)");

    uint32_t pass_count = (uint32_t)NUM_PASSES;
    uint32_t total_in   = M->num_insts;
    int      max_rc     = 0;

    (void)printf("ERNESTOP START   OPT=%u  TARGET=%s  PASSES=%u  INSTS=%u\n",
                 (unsigned)opt_level, target_name(target),
                 (unsigned)pass_count, (unsigned)total_in);

    snap_active("opt-start", M, NULL);

    for (uint32_t i = 0u; i < pass_count; i++) {
        const opt_pass_t *P = &PASSES[i];
        if (opt_level < P->min_level) {
            (void)printf("  STEP %-8s  SKIPPED  NEEDS OPT>=%u\n",
                         P->name, (unsigned)P->min_level);
            continue;
        }
        if (P->target_only != OPT_TARGET_GENERIC
            && P->target_only != target) {
            (void)printf("  STEP %-8s  SKIPPED  NEEDS TARGET=%s\n",
                         P->name, target_name(P->target_only));
            continue;
        }
        opt_stats_t stats;
        memset(&stats, 0, sizeof stats);

        /* Sub-stage so a fault during this pass dumps with the
         * pass name visible. Snap before and after so the operator
         * can see the pass's effect on instruction count, etc. */
        abend_set_stage("OPT", P->name);
        char label_before[64];
        (void)snprintf(label_before, sizeof label_before,
                       "pre-%s", P->name);
        snap_active(label_before, M, NULL);

        int rc = P->run(M, &stats, log);

        char label_after[64];
        (void)snprintf(label_after, sizeof label_after,
                       "post-%s", P->name);
        snap_active(label_after, M, NULL);

        /* Report either REMOVED or ADDED depending on which way
         * the count moved. Decomposition passes expand; cleanup
         * passes shrink; either is news worth telling. */
        if (stats.insts_out > stats.insts_in) {
            (void)printf("  STEP %-8s  IN=%u  OUT=%u  ADDED=%u  ITER=%u  RC=%d\n",
                         P->name,
                         (unsigned)stats.insts_in,
                         (unsigned)stats.insts_out,
                         (unsigned)(stats.insts_out - stats.insts_in),
                         (unsigned)stats.iterations,
                         rc);
        } else {
            (void)printf("  STEP %-8s  IN=%u  OUT=%u  REMOVED=%u  ITER=%u  RC=%d\n",
                         P->name,
                         (unsigned)stats.insts_in,
                         (unsigned)stats.insts_out,
                         (unsigned)stats.insts_removed,
                         (unsigned)stats.iterations,
                         rc);
        }
        if (rc > max_rc) {
            max_rc = rc;
        }
    }

    (void)printf("ERNESTOP ENDED   IN=%u  OUT=%u  RC=%d\n",
                 (unsigned)total_in,
                 (unsigned)M->num_insts,
                 max_rc);

    snap_active("opt-end", M, NULL);
    abend_clear_stage();
    return max_rc;
}
