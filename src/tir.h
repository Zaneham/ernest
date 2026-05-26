#ifndef ERNEST_TIR_H
#define ERNEST_TIR_H

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

/*
 * TIR, the Ernest Intermediate Representation, is the form a quantum
 * circuit takes when it has stopped being a happy idea and started
 * being something a compiler can carry around in its pockets. It owes
 * its shape to BarraCUDA's BIR, which owes its shape to long evenings
 * of staring at LLVM IR and wondering whether quite so much was
 * strictly necessary.
 *
 * A module is a circuit. A circuit has quantum registers, full of
 * qubits, and classical registers, full of bits. Bits have been
 * doing the same job since about 1937 and they ask very little of
 * us. Qubits are a more recent hire and require a little more
 * supervision.
 *
 * Qubit references encode (register << 16) | offset. Both halves of
 * a thirty-two-bit integer earn their keep: one half says which
 * register, the other says which qubit inside it. This is the sort
 * of small economy that pays compound interest over a codebase, and
 * unlike most kinds of compound interest it is also free.
 *
 * You can't always get the qubit you want. But if you try sometimes,
 * with the right index, you'll find you get the qubit you need.
 */

/* ----- Limits ----------------------------------------------------- */
/*
 * Numbers chosen the way a sensible quartermaster chooses numbers:
 * generously enough that nothing in the foreseeable future will run
 * out, sparingly enough that the books still balance.
 */
#define TIR_MAX_INSTS    (1 << 16)
#define TIR_MAX_STRINGS  (1 << 16)
#define TIR_MAX_QREGS    32
#define TIR_MAX_CREGS    32
#define TIR_MAX_ANGLES   (1 << 12)

/* ----- Operations ------------------------------------------------- */
/*
 * The set of things a quantum circuit gets to do. Gates act on qubits.
 * Measurement and reset move information between the quantum world,
 * where things can be in two minds about themselves, and the
 * classical world, where they very much cannot.
 *
 * The list is short on purpose. Most quantum compilers eventually
 * grow an opcode table the length of an Edwardian novel. We are
 * starting small and adding what proves itself useful.
 */
typedef enum {
    /* Register declarations. */
    TIR_QREG_DECL,
    TIR_CREG_DECL,

    /* Single-qubit Clifford gates. The well-behaved cousins of the
     * gate family, mapping stabiliser states to stabiliser states.
     * Their effects on probability distributions are the kind of
     * thing you can work out with a pencil, given enough Tuesday. */
    TIR_GATE_H,
    TIR_GATE_X,
    TIR_GATE_Y,
    TIR_GATE_Z,
    TIR_GATE_S,
    TIR_GATE_T,
    TIR_GATE_SDG,
    TIR_GATE_TDG,

    /* The square root of X. SX^2 = X. Strange-looking on first
     * meeting, indispensable on second: it's one of the gates that
     * IBM's hardware can do directly, which means anyone targeting
     * an IBM quantum computer ends up routing every other rotation
     * through this one and through RZ. SXDG is SX walked backwards,
     * also known as SX^3. */
    TIR_GATE_SX,

    /* Parameterised single-qubit rotations. RX, RY, RZ rotate the
     * qubit around the named axis of the Bloch sphere by an angle
     * stored in the module's angle table. Without these you can do
     * Clifford circuits and nothing else, which is approximately
     * 0.0001% of the interesting things a quantum computer can be
     * asked to do. */
    TIR_GATE_RX,
    TIR_GATE_RY,
    TIR_GATE_RZ,

    /* Two-qubit Clifford gates. CX is the entangler. Before CX, your
     * qubits are introverts who happen to share a room. After CX,
     * they finish each other's sentences. */
    TIR_GATE_CX,
    TIR_GATE_CZ,
    TIR_GATE_CH,
    TIR_GATE_SWAP,

    /* Two-qubit parameterised gate. CP(theta) applies phase
     * exp(i*theta) to the |11> component of the pair and leaves the
     * other three components alone. The workhorse of QFT, where you
     * see a long ladder of pi/2, pi/4, pi/8... values walk down the
     * register one rung at a time. */
    TIR_GATE_CP,

    /* Three-qubit gate. CCX, also known as Toffoli, flips its target
     * qubit if and only if both control qubits are |1>. Universal
     * for classical reversible computation on its own, useful for
     * everything from arithmetic to oracle construction in
     * quantum-search workloads. */
    TIR_GATE_CCX,

    /* Measurement asks the qubit to commit. The qubit, having been
     * asked, picks a side: zero or one. Once it has picked, it does
     * not un-pick. Schrödinger's cat is fine. The cat has always
     * been fine. Whether you've opened the box is a different
     * question. */
    TIR_MEASURE,

    /* Reset is measurement with the courtesy of looking the other
     * way. The qubit ends up in |0> regardless of where it started,
     * and nobody is the wiser. */
    TIR_RESET,

    TIR_OP_COUNT
} tir_op_t;

/* ----- Registers -------------------------------------------------- */
/*
 * A register is a named slab of either qubits or bits. The width
 * counts how many. The name is a string table offset, because every
 * string in TIR is an offset into the same shared pool, and a shared
 * pool is what civilisation looks like at the bit level.
 */
typedef struct {
    uint32_t name;
    uint32_t width;
} tir_reg_t;

/* ----- Provenance ------------------------------------------------- */
/*
 * Which part of the toolchain stamped a particular instruction. The
 * parser stamps PARSE; direct C-builder API calls stamp USER; each
 * optimisation pass stamps itself when it produces a new instruction.
 * Carried per-instruction so a downstream reader can trace any gate
 * in the output back to a meaningful point of origin.
 */
typedef enum {
    TIR_ORIGIN_USER    = 0,  /* C builder API, no source location */
    TIR_ORIGIN_PARSE   = 1,  /* OpenQASM parser, tied to a source line */
    TIR_ORIGIN_OPTGCAN = 2,  /* survived gate cancellation */
    TIR_ORIGIN_OPTFUSE = 3,  /* produced by rotation fusion */
    TIR_ORIGIN_OPTDECP = 4,  /* produced by IBM decomposition */
    TIR_ORIGIN_ROUTE   = 5,  /* SWAP inserted by SABRE routing */
    TIR_ORIGIN_COUNT
} tir_origin_t;

const char *tir_origin_name(tir_origin_t o);

/* ----- Instruction ------------------------------------------------ */
/*
 * One operation. Four operand slots is plenty of room for every gate
 * we know about today, and a fair bit of room for the ones we will
 * find ourselves wanting tomorrow.
 *
 * Each instruction carries a small provenance tail: which line and
 * column of the source it came from (zero when there is no source,
 * for instructions built through the C API), and which pass last
 * touched it. The provenance survives every transformation so a
 * downstream reader can ask of any output gate "where did you come
 * from" and get a meaningful answer.
 */
#define TIR_OPERANDS_INLINE 4

typedef struct {
    uint16_t op;                            /* a tir_op_t */
    uint8_t  num_operands;
    uint8_t  subop;                         /* gate variant or unused */
    uint32_t operands[TIR_OPERANDS_INLINE]; /* qubit and bit refs */

    /* ----- Provenance ----- */
    uint32_t src_line;                      /* 1-based source line, 0 = none */
    uint32_t src_col;                       /* 1-based source column, 0 = none */
    uint16_t origin_pass;                   /* a tir_origin_t value */
    uint16_t reserved;
} tir_inst_t;

/*
 * Refs are (register << 16) | offset. Top half names the register,
 * bottom half points at the qubit or bit inside it. Both halves
 * carried in one integer means the operand table doesn't have to
 * develop strong opinions about which side it's looking at, which
 * keeps the operand table cheerful.
 */
#define TIR_REF(reg, idx)    (((uint32_t)(reg) << 16) | (uint32_t)(idx))
#define TIR_REF_REG(ref)     ((ref) >> 16)
#define TIR_REF_IDX(ref)     ((ref) & 0xFFFFu)

/* ----- Module ----------------------------------------------------- */
/*
 * The whole compilation unit in one place. The only handle the rest
 * of the codebase ever has to pass around, which keeps function
 * signatures short and reviewers awake.
 */
typedef struct {
    uint32_t   module_name;
    tir_reg_t  qregs[TIR_MAX_QREGS];
    uint32_t   num_qregs;
    tir_reg_t  cregs[TIR_MAX_CREGS];
    uint32_t   num_cregs;
    tir_inst_t insts[TIR_MAX_INSTS];
    uint32_t   num_insts;
    double     angles[TIR_MAX_ANGLES];
    uint32_t   num_angles;
    char       strings[TIR_MAX_STRINGS];
    uint32_t   string_len;
} tir_module_t;

/* ----- API -------------------------------------------------------- */
/*
 * Construction. The module is initialised once and grown by appending.
 * No removals, no edits. If you change your mind about a circuit,
 * make a new circuit. This is what version control is for.
 */
void     tir_module_init(tir_module_t *M, const char *name);
uint32_t tir_add_string (tir_module_t *M, const char *s);

uint32_t tir_qreg (tir_module_t *M, const char *name, uint32_t width);
uint32_t tir_creg (tir_module_t *M, const char *name, uint32_t width);

void tir_emit_h    (tir_module_t *M, uint32_t qref);
void tir_emit_x    (tir_module_t *M, uint32_t qref);
void tir_emit_y    (tir_module_t *M, uint32_t qref);
void tir_emit_z    (tir_module_t *M, uint32_t qref);
void tir_emit_s    (tir_module_t *M, uint32_t qref);
void tir_emit_t    (tir_module_t *M, uint32_t qref);
void tir_emit_sdg  (tir_module_t *M, uint32_t qref);
void tir_emit_tdg  (tir_module_t *M, uint32_t qref);
void tir_emit_sx   (tir_module_t *M, uint32_t qref);
void tir_emit_rx   (tir_module_t *M, uint32_t qref, double theta);
void tir_emit_ry   (tir_module_t *M, uint32_t qref, double theta);
void tir_emit_rz   (tir_module_t *M, uint32_t qref, double theta);
void tir_emit_cx   (tir_module_t *M, uint32_t ctrl, uint32_t tgt);
void tir_emit_cz   (tir_module_t *M, uint32_t ctrl, uint32_t tgt);
void tir_emit_ch   (tir_module_t *M, uint32_t ctrl, uint32_t tgt);
void tir_emit_cp   (tir_module_t *M, uint32_t ctrl, uint32_t tgt, double theta);
void tir_emit_swap (tir_module_t *M, uint32_t a, uint32_t b);
void tir_emit_ccx  (tir_module_t *M, uint32_t c1, uint32_t c2, uint32_t tgt);
void tir_emit_measure (tir_module_t *M, uint32_t qref, uint32_t cref);
void tir_emit_reset   (tir_module_t *M, uint32_t qref);

/* Interns an angle into the module's angle table and returns its
 * index. The rotation emitters use this internally; expose it for
 * callers who want to share an angle across multiple gates. */
uint32_t tir_add_angle(tir_module_t *M, double theta);

/*
 * Stamp the most-recently-emitted instruction with a source line
 * and column. Used by the parser and by anything else that has a
 * meaningful location to attribute. A no-op if the module has no
 * instructions yet, which keeps the call sites uniform.
 */
void tir_stamp_loc(tir_module_t *M, uint32_t line, uint32_t col);

/* Inspection. The module is a fishbowl. Everything visible. */
const char *tir_op_name  (tir_op_t op);
const char *tir_reg_name (const tir_module_t *M, uint32_t reg_idx, int is_qreg);
void        tir_print_module (const tir_module_t *M, FILE *out);

/*
 * Cross-reference listing. Same instructions as tir_print_module,
 * but with the provenance fields visible: which pass produced each
 * instruction, and which source line and column (where applicable).
 * A summary footer counts instructions by origin, the way a
 * mainframe XREF would.
 */
void tir_print_xref(const tir_module_t *M, FILE *out);

/*
 * Flat indices. Quantum registers in TIR are named and sized
 * separately, but the simulator wants one long flat list of qubits
 * with no internal divisions. These two functions handle the
 * translation, which is the sort of small bureaucratic act that
 * makes the difference between code that works and code that almost
 * works.
 */
uint32_t tir_total_qubits     (const tir_module_t *M);
uint32_t tir_flat_qubit_index (const tir_module_t *M, uint32_t qref);
uint32_t tir_total_bits       (const tir_module_t *M);
uint32_t tir_flat_bit_index   (const tir_module_t *M, uint32_t cref);

/*
 * Structural comparison. Two modules are considered equivalent if
 * they have the same registers (by name and width, in the same
 * order), the same instruction stream (op, operands, and angle
 * values where applicable), and the same angle table contents up
 * to floating-point tolerance.
 *
 * Returns 0 on match. On mismatch returns non-zero and, if reason
 * is non-NULL, writes a short human-readable explanation into it
 * (truncated at reason_cap-1 to leave room for the null
 * terminator). The reason tells you where the diff was, not how
 * to fix it; this is a comparator, not a counsellor.
 */
int tir_module_diff(const tir_module_t *A, const tir_module_t *B,
                    char *reason, uint32_t reason_cap);

#endif /* ERNEST_TIR_H */
