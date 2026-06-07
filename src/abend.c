#include "abend.h"
#include "sim.h"
#include "tir.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

/*
 * The ABEND dump formatter. Mainframe console aesthetic, quantum
 * substance: it reads like a z/OS operator's job log, except the
 * things going wrong are amplitudes, not account balances.
 *
 * Nothing here allocates. Strings get formatted into stack buffers,
 * walked through the dump, and thrown away. The simulator state and
 * the TIR module are read but never touched, on the principle that
 * the program is already having the worst day of its short life and
 * the postmortem should not pile on.
 */

/* ----- Pipeline stage tracking ---------------------------------- */
/*
 * File-scope state. Whichever pipeline stage is running calls
 * abend_set_stage on entry and abend_clear_stage on exit. If a
 * fault fires while a stage is active, the dump (or any snap
 * captured nearby) names the stage instead of leaving the operator
 * to guess.
 */
#define ABEND_STAGE_LEN  16u
#define ABEND_SUB_LEN    32u

static char abend_stage[ABEND_STAGE_LEN] = "";
static char abend_sub  [ABEND_SUB_LEN]   = "";

static void copy_truncated_abend(char *dst, uint32_t cap, const char *src)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    uint32_t n = (uint32_t)strlen(src);
    if (n + 1u > cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void abend_set_stage(const char *stage, const char *sub)
{
    copy_truncated_abend(abend_stage, ABEND_STAGE_LEN, stage);
    copy_truncated_abend(abend_sub,   ABEND_SUB_LEN,   sub);
}

void abend_clear_stage(void)
{
    abend_stage[0] = '\0';
    abend_sub  [0] = '\0';
}

const char *abend_current_stage(void)
{
    return abend_stage;
}

const char *abend_current_sub(void)
{
    return abend_sub;
}

/* ----- Construction ---------------------------------------------- */

void abend_init(abend_t *A, const char *completion_code)
{
    assert(A != NULL);
    assert(completion_code != NULL);
    memset(A, 0, sizeof *A);
    /* Codes are short enough that strncpy works fine if we leave
     * room for the terminator. */
    size_t n = strlen(completion_code);
    if (n >= sizeof A->completion_code) {
        n = sizeof A->completion_code - 1u;
    }
    memcpy(A->completion_code, completion_code, n);
    A->completion_code[n] = '\0';
}

void abend_set_reason(abend_t *A, const char *fmt, ...)
{
    assert(A != NULL);
    assert(fmt != NULL);
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(A->reason, sizeof A->reason, fmt, ap);
    va_end(ap);
}

void abend_set_psw(abend_t *A, uint32_t flags,
                   uint32_t inst_idx, uint32_t qubit_ctx)
{
    assert(A != NULL);
    uint64_t f = (uint64_t)(flags & 0xFFFFu);
    uint64_t i = (uint64_t)(inst_idx & 0xFFFFu);
    uint64_t q = (uint64_t)(qubit_ctx & 0xFFFFu);
    A->psw = (f << 48) | (i << 32) | (q << 16);
    A->inst_idx  = inst_idx;
    A->qubit_ctx = qubit_ctx;
}

/* ----- Helpers --------------------------------------------------- */

/*
 * Format the PSW as four sixteen-bit groups separated by
 * underscores. Reads the same way a real PSW does in a JES2
 * console line.
 */
static void format_psw(uint64_t psw, char *out, size_t out_size)
{
    assert(out != NULL);
    assert(out_size >= 24u);
    (void)snprintf(out, out_size, "%04X_%04X_%04X_%04X",
                   (unsigned)((psw >> 48) & 0xFFFFu),
                   (unsigned)((psw >> 32) & 0xFFFFu),
                   (unsigned)((psw >> 16) & 0xFFFFu),
                   (unsigned)(psw & 0xFFFFu));
}

/*
 * One-line summary of a TIR instruction. Used in the recent-
 * instructions trace and in the "fault at" line. Buffer-bounded
 * so it always returns.
 */
static void format_inst(const tir_module_t *M, uint32_t idx,
                        char *out, size_t out_size)
{
    assert(out != NULL);
    assert(out_size >= 16u);

    if (M == NULL || idx >= M->num_insts) {
        (void)snprintf(out, out_size, "<no instruction available>");
        return;
    }

    const tir_inst_t *I = &M->insts[idx];
    const char *opn = tir_op_name((tir_op_t)I->op);

    switch (I->op) {
    case TIR_QREG_DECL:
        (void)snprintf(out, out_size, "qreg decl (idx %u w=%u)",
                       (unsigned)I->operands[0],
                       (unsigned)I->operands[1]);
        return;
    case TIR_CREG_DECL:
        (void)snprintf(out, out_size, "creg decl (idx %u w=%u)",
                       (unsigned)I->operands[0],
                       (unsigned)I->operands[1]);
        return;
    case TIR_GATE_H:   case TIR_GATE_X:   case TIR_GATE_Y:   case TIR_GATE_Z:
    case TIR_GATE_S:   case TIR_GATE_T:   case TIR_GATE_SDG: case TIR_GATE_TDG:
    case TIR_RESET: {
        uint32_t r = TIR_REF_REG(I->operands[0]);
        uint32_t i = TIR_REF_IDX(I->operands[0]);
        const char *rn = (r < M->num_qregs) ? &M->strings[M->qregs[r].name] : "?";
        (void)snprintf(out, out_size, "%s %s[%u]", opn, rn, (unsigned)i);
        return;
    }
    case TIR_GATE_RX: case TIR_GATE_RY: case TIR_GATE_RZ: {
        uint32_t r = TIR_REF_REG(I->operands[0]);
        uint32_t i = TIR_REF_IDX(I->operands[0]);
        const char *rn = (r < M->num_qregs) ? &M->strings[M->qregs[r].name] : "?";
        double theta = (I->operands[1] < M->num_angles)
                       ? M->angles[I->operands[1]] : 0.0;
        (void)snprintf(out, out_size, "%s(%.6f) %s[%u]",
                       opn, theta, rn, (unsigned)i);
        return;
    }
    case TIR_GATE_CX: case TIR_GATE_CZ: case TIR_GATE_SWAP: {
        uint32_t ra = TIR_REF_REG(I->operands[0]);
        uint32_t ia = TIR_REF_IDX(I->operands[0]);
        uint32_t rb = TIR_REF_REG(I->operands[1]);
        uint32_t ib = TIR_REF_IDX(I->operands[1]);
        const char *na = (ra < M->num_qregs) ? &M->strings[M->qregs[ra].name] : "?";
        const char *nb = (rb < M->num_qregs) ? &M->strings[M->qregs[rb].name] : "?";
        (void)snprintf(out, out_size, "%s %s[%u], %s[%u]",
                       opn, na, (unsigned)ia, nb, (unsigned)ib);
        return;
    }
    case TIR_MEASURE: {
        uint32_t rq = TIR_REF_REG(I->operands[0]);
        uint32_t iq = TIR_REF_IDX(I->operands[0]);
        uint32_t rc = TIR_REF_REG(I->operands[1]);
        uint32_t ic = TIR_REF_IDX(I->operands[1]);
        const char *nq = (rq < M->num_qregs) ? &M->strings[M->qregs[rq].name] : "?";
        const char *nc = (rc < M->num_cregs) ? &M->strings[M->cregs[rc].name] : "?";
        (void)snprintf(out, out_size, "measure %s[%u] -> %s[%u]",
                       nq, (unsigned)iq, nc, (unsigned)ic);
        return;
    }
    default:
        (void)snprintf(out, out_size, "<op %u>", (unsigned)I->op);
        return;
    }
}

/*
 * Compute the L2 norm of the statevector. Should be 1 in a happy
 * universe. Drift away from 1 is itself a useful diagnostic; the
 * dump shows both the norm and how far it is off.
 */
static double compute_norm(const sim_state_t *S)
{
    assert(S != NULL);
    double s = 0.0;
    uint32_t dim = 1u << S->num_qubits;
    for (uint32_t i = 0u; i < dim; i++) {
        double re = S->state[i].re;
        double im = S->state[i].im;
        s += re * re + im * im;
    }
    return sqrt(s);
}

/* ----- The dump -------------------------------------------------- */

void abend_dump(const abend_t *A,
                const sim_state_t *S,
                const tir_module_t *M,
                FILE *out)
{
    assert(A != NULL);
    assert(out != NULL);

    char psw_buf[32];
    format_psw(A->psw, psw_buf, sizeof psw_buf);

    /* Header: who is talking, what code, what reason. */
    (void)fprintf(out, "\n");
    (void)fprintf(out, "ERNESTDM ABEND %s\n", A->completion_code);
    (void)fprintf(out, "ERNESTDM REASON  %s\n",
                  A->reason[0] != '\0' ? A->reason : "(no reason supplied)");
    /* Pipeline stage context, if set. */
    if (abend_stage[0] != '\0') {
        if (abend_sub[0] != '\0') {
            (void)fprintf(out, "ERNESTDM STAGE   %s / %s\n",
                          abend_stage, abend_sub);
        } else {
            (void)fprintf(out, "ERNESTDM STAGE   %s\n", abend_stage);
        }
    }
    (void)fprintf(out, "ERNESTDM PSW     %s\n", psw_buf);
    (void)fprintf(out, "ERNESTDM         "
                       "FFFF=flags+cc  IIII=inst  QQQQ=qubit  RRRR=reserved\n");

    /* Decoded PSW. */
    uint32_t flags = (uint32_t)((A->psw >> 48) & 0xFFFFu);
    (void)fprintf(out, "ERNESTDM FLAGS   %s%s%s  CC=0x%02X\n",
                  (flags & ABEND_PSW_RUNNING)    ? "RUNNING "    : "",
                  (flags & ABEND_PSW_TRACE_FULL) ? "TRACE_FULL " : "",
                  (flags & ABEND_PSW_MEASURING)  ? "MEASURING "  : "",
                  (unsigned)(flags & 0xFFu));

    /* The faulting instruction, if we know it. */
    if (M != NULL) {
        char fault_buf[160];
        format_inst(M, A->inst_idx, fault_buf, sizeof fault_buf);
        (void)fprintf(out, "ERNESTDM FAULT   inst %u: %s\n",
                      (unsigned)A->inst_idx, fault_buf);
    }

    /* Simulator state summary. */
    if (S != NULL) {
        double norm = compute_norm(S);
        double drift = norm - 1.0;
        if (drift < 0.0) drift = -drift;
        uint32_t dim = 1u << S->num_qubits;

        (void)fprintf(out, "ERNESTDM SIM     qubits=%u  amplitudes=%u  "
                           "norm=%.12f  drift=%.3e\n",
                      (unsigned)S->num_qubits,
                      (unsigned)dim,
                      norm, drift);

        /* Classical register, big-endian for readability. */
        if (S->num_bits > 0u) {
            (void)fprintf(out, "ERNESTDM CREG    ");
            assert(S->num_bits <= (uint32_t)ERNEST_SIM_MAX_BITS);
            for (uint32_t b = S->num_bits; b > 0u; b--) {
                (void)fputc(S->bits[b - 1u] != 0u ? '1' : '0', out);
            }
            (void)fputc('\n', out);
        }
    }

    /* Recent instructions trace. */
    if (S != NULL && M != NULL && S->trace_count > 0u) {
        uint32_t avail = (S->trace_count < (uint32_t)ERNEST_SIM_TRACE_LEN)
                         ? S->trace_count
                         : (uint32_t)ERNEST_SIM_TRACE_LEN;

        (void)fprintf(out, "ERNESTDM TRACE   recent %u of %u instructions:\n",
                      (unsigned)avail, (unsigned)S->trace_count);

        /* The ring buffer ordering: head points at the next slot to
         * write, so the oldest entry is at head (if the buffer has
         * wrapped) or at index 0 (if it has not). */
        uint32_t start;
        if (S->trace_count >= (uint32_t)ERNEST_SIM_TRACE_LEN) {
            start = S->trace_head;
        } else {
            start = 0u;
        }
        for (uint32_t k = 0u; k < avail; k++) {
            uint32_t pos = (start + k) % (uint32_t)ERNEST_SIM_TRACE_LEN;
            uint32_t inst = S->trace_buf[pos];
            char inst_buf[160];
            format_inst(M, inst, inst_buf, sizeof inst_buf);
            int is_fault = (inst == A->inst_idx);
            (void)fprintf(out, "ERNESTDM           %5u: %s%s\n",
                          (unsigned)inst, inst_buf,
                          is_fault ? "   <-- FAULT" : "");
        }
    }

    /* Register map. */
    if (M != NULL && (M->num_qregs > 0u || M->num_cregs > 0u)) {
        (void)fprintf(out, "ERNESTDM REGS    register map:\n");
        uint32_t base = 0u;
        for (uint32_t r = 0u; r < M->num_qregs; r++) {
            uint32_t w = M->qregs[r].width;
            const char *n = &M->strings[M->qregs[r].name];
            (void)fprintf(out, "ERNESTDM           qreg %-8s flat %u..%u  (width %u)\n",
                          n, (unsigned)base, (unsigned)(base + w - 1u), (unsigned)w);
            base += w;
        }
        base = 0u;
        for (uint32_t r = 0u; r < M->num_cregs; r++) {
            uint32_t w = M->cregs[r].width;
            const char *n = &M->strings[M->cregs[r].name];
            (void)fprintf(out, "ERNESTDM           creg %-8s flat %u..%u  (width %u)\n",
                          n, (unsigned)base, (unsigned)(base + w - 1u), (unsigned)w);
            base += w;
        }
    }

    (void)fprintf(out, "ERNESTDM END-OF-DUMP\n\n");
}

/* ----- MNOTE bridge ---------------------------------------------- */

void abend_emit(const abend_t *A, mnote_log_t *log,
                const char *source_file,
                uint32_t line, uint32_t col)
{
    assert(A != NULL);
    assert(log != NULL);
    mnote_abend(log, A->completion_code, source_file, line, col,
                "%s", A->reason);
}
