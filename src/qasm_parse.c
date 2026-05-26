#include "qasm_parse.h"
#include "expr_eval.h"
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

/*
 * ERNESTPR implementation. The parser is a recursive-descent walk
 * over the token stream. The state struct keeps the current cursor
 * position, the module being built, the diagnostic log, and a small
 * symbol table for custom gate definitions when they turn up.
 *
 * Most parsing functions follow the same shape: peek at the
 * current token, dispatch to the right sub-parser, advance past
 * the consumed tokens, return. The hard part is keeping the
 * diagnostics readable when the input is malformed.
 *
 * Recovery strategy: on a statement-level error, raise an MNOTE,
 * skip tokens until the next semicolon, and continue parsing from
 * there. The operator gets to see every misbehaviour in the file
 * rather than only the first one.
 */

/* ----- Limits ---------------------------------------------------- */
#define PARSE_MAX_GATE_DEFS  64u
#define PARSE_MAX_GATE_BODY  64u   /* statements in one gate body */

/* ----- Custom gate definitions ----------------------------------- */
/*
 * A custom gate def records the gate's name, its parameter names,
 * its qubit-argument names, and the body of statements. When a
 * call to the gate is encountered, the parser expands the body
 * inline, substituting the actual arguments for the formal
 * parameters.
 *
 * v0.1 stores the body as a tiny array of "templated calls": the
 * gate name to emit, indices into the parameter list for any
 * angle parameters, and indices into the qubit-argument list for
 * each qubit position. When expanding a call we walk the body and
 * synthesise real TIR instructions.
 */
typedef struct {
    /* Original gate name and its lookup info. */
    char    name[32];

    /* Parameter list (formal angle parameters). */
    uint32_t num_params;
    char     param_names[8][32];

    /* Qubit argument list (formal qubit positions). */
    uint32_t num_qargs;
    char     qarg_names[8][32];

    /* Body: a small array of templated calls. */
    uint32_t num_body_stmts;
    struct {
        /* Built-in gate name to emit for this statement. */
        char gate_name[32];

        /* Optional angle expression as a literal double. v0.1
         * supports literal-only angles in custom-gate bodies; full
         * expressions over parameter names would need a symbol-
         * table-aware evaluator and that's v0.2. */
        int    has_angle;
        double angle;

        /* Qubit slots: each entry is the index in qarg_names for
         * which formal qubit position this slot uses. */
        uint32_t num_qslots;
        uint32_t qslots[4];
    } body[PARSE_MAX_GATE_BODY];
} gate_def_t;

/* ----- Parser state --------------------------------------------- */
typedef struct {
    const qasm_lexer_t *L;
    uint32_t            pos;
    tir_module_t       *M;
    mnote_log_t        *log;

    /* Custom gate definitions registered so far. */
    gate_def_t          gate_defs[PARSE_MAX_GATE_DEFS];
    uint32_t            num_gate_defs;

    /* Source filename for diagnostics. */
    const char         *source_file;
} parser_t;

/* ----- Cursor helpers -------------------------------------------- */

static const qasm_tok_t *peek_tok(const parser_t *P)
{
    assert(P != NULL);
    /* The lexer always leaves an EOF terminator at the end. */
    if (P->pos >= P->L->num_tokens) {
        return &P->L->tokens[P->L->num_tokens - 1u];
    }
    return &P->L->tokens[P->pos];
}

static void advance_p(parser_t *P)
{
    assert(P != NULL);
    if (P->pos < P->L->num_tokens) {
        P->pos++;
    }
}

static void error_at(parser_t *P, const qasm_tok_t *T,
                     uint32_t code, const char *fmt, ...)
{
    assert(P != NULL);
    assert(T != NULL);
    assert(fmt != NULL);

    if (P->log == NULL) {
        return;
    }

    /* Format the message into a local buffer, then forward to
     * mnote_emit which has its own formatter. Two-step because
     * mnote_emit takes a format string and varargs; we want to
     * pass our own format and args through. Cheapest portable
     * approach is vsnprintf into a stack buffer. */
    char buf[MNOTE_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    mnote_emit(P->log, MNOTE_ERROR, code,
               P->source_file, T->line, T->col,
               "%s", buf);
}

static int expect(parser_t *P, qasm_tok_kind_t k, const char *what)
{
    assert(P != NULL);
    assert(what != NULL);
    const qasm_tok_t *T = peek_tok(P);
    if (T->kind == k) {
        advance_p(P);
        return 1;
    }
    error_at(P, T, 10u, "EXPECTED %s, GOT %s", what, qasm_tok_name(T->kind));
    return 0;
}

/*
 * Recover from a parse error by skipping to (and past) the next
 * semicolon. If we hit EOF first the parser stops cold; the caller
 * will notice on its next pass through the top-level loop.
 */
static void skip_to_semi(parser_t *P)
{
    assert(P != NULL);
    while (peek_tok(P)->kind != TOK_SEMI && peek_tok(P)->kind != TOK_EOF) {
        advance_p(P);
    }
    if (peek_tok(P)->kind == TOK_SEMI) {
        advance_p(P);
    }
}

/* ----- Identifier extraction ------------------------------------- */

static int copy_ident_text(const parser_t *P, const qasm_tok_t *T,
                           char *out, uint32_t out_cap)
{
    assert(P != NULL);
    assert(T != NULL);
    assert(out != NULL);

    if (T->kind != TOK_IDENT) {
        return 1;
    }
    uint32_t len = T->src_len;
    if (len + 1u > out_cap) {
        len = out_cap - 1u;
    }
    memcpy(out, &P->L->source[T->src_off], len);
    out[len] = '\0';
    return 0;
}

/* ----- Register lookup ------------------------------------------- */

static int find_qreg(const tir_module_t *M, const char *name)
{
    assert(M != NULL);
    assert(name != NULL);
    for (uint32_t i = 0u; i < M->num_qregs; i++) {
        if (strcmp(&M->strings[M->qregs[i].name], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_creg(const tir_module_t *M, const char *name)
{
    assert(M != NULL);
    assert(name != NULL);
    for (uint32_t i = 0u; i < M->num_cregs; i++) {
        if (strcmp(&M->strings[M->cregs[i].name], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ----- Qubit / bit reference ------------------------------------- */
/*
 * Parses IDENT '[' INT ']' and returns a TIR reference.
 * is_qubit selects which register table to look in.
 * On error the reference is zero and an MNOTE is emitted.
 */
static uint32_t parse_ref(parser_t *P, int is_qubit, int *ok)
{
    assert(P != NULL);
    assert(ok != NULL);
    *ok = 0;

    const qasm_tok_t *T = peek_tok(P);
    char name[32];
    if (copy_ident_text(P, T, name, sizeof name) != 0) {
        error_at(P, T, 11u, "EXPECTED REGISTER NAME");
        return 0u;
    }
    advance_p(P);

    int reg_idx;
    if (is_qubit) {
        reg_idx = find_qreg(P->M, name);
    } else {
        reg_idx = find_creg(P->M, name);
    }
    if (reg_idx < 0) {
        error_at(P, T, 12u, "%s '%s' NOT DECLARED",
                 is_qubit ? "QREG" : "CREG", name);
        return 0u;
    }

    if (!expect(P, TOK_LBRACKET, "'['")) {
        return 0u;
    }
    const qasm_tok_t *idxT = peek_tok(P);
    if (idxT->kind != TOK_INT) {
        error_at(P, idxT, 13u, "EXPECTED INTEGER INDEX");
        return 0u;
    }
    advance_p(P);
    uint32_t idx = (uint32_t)idxT->val.ival;
    if (!expect(P, TOK_RBRACKET, "']'")) {
        return 0u;
    }

    /* Bounds check. */
    uint32_t width;
    if (is_qubit) {
        width = P->M->qregs[reg_idx].width;
    } else {
        width = P->M->cregs[reg_idx].width;
    }
    if (idx >= width) {
        error_at(P, idxT, 14u,
                 "INDEX %u OUT OF RANGE FOR %s OF WIDTH %u",
                 (unsigned)idx, name, (unsigned)width);
        return 0u;
    }

    *ok = 1;
    return TIR_REF((uint32_t)reg_idx, idx);
}

/* ----- Top-level statements -------------------------------------- */

/* OPENQASM <number> ; */
static void parse_version(parser_t *P)
{
    assert(P != NULL);
    advance_p(P);  /* consume OPENQASM */
    const qasm_tok_t *T = peek_tok(P);
    if (T->kind != TOK_FLOAT && T->kind != TOK_INT) {
        error_at(P, T, 15u, "EXPECTED VERSION NUMBER AFTER OPENQASM");
        skip_to_semi(P);
        return;
    }
    /* We don't check the version value; we'll accept anything and
     * let the rest of the parser complain if it sees something it
     * doesn't understand. */
    advance_p(P);
    (void)expect(P, TOK_SEMI, "';'");
}

/* include "<string>" ; */
static void parse_include(parser_t *P)
{
    assert(P != NULL);
    advance_p(P);  /* consume include */
    const qasm_tok_t *T = peek_tok(P);
    if (T->kind != TOK_STRING) {
        error_at(P, T, 16u, "EXPECTED STRING AFTER include");
        skip_to_semi(P);
        return;
    }
    /* We only honour stdgates.inc by ignoring it; the standard
     * gate set is built into the parser's gate-name dispatch.
     * Other includes turn into a warning. */
    uint32_t len = T->src_len;
    int is_stdgates = (len == 12u) &&
        (strncmp(&P->L->source[T->src_off], "stdgates.inc", 12) == 0);
    if (!is_stdgates) {
        mnote_emit(P->log, MNOTE_WARN, 4u,
                   P->source_file, T->line, T->col,
                   "INCLUDE OTHER THAN stdgates.inc IGNORED");
    }
    advance_p(P);
    (void)expect(P, TOK_SEMI, "';'");
}

/* qubit '[' INT ']' IDENT ';' */
static void parse_qreg(parser_t *P)
{
    assert(P != NULL);
    const qasm_tok_t *kwT = peek_tok(P);
    advance_p(P);  /* consume qubit */

    if (!expect(P, TOK_LBRACKET, "'['")) { skip_to_semi(P); return; }
    const qasm_tok_t *widthT = peek_tok(P);
    if (widthT->kind != TOK_INT) {
        error_at(P, widthT, 17u, "EXPECTED INTEGER WIDTH");
        skip_to_semi(P);
        return;
    }
    uint32_t width = (uint32_t)widthT->val.ival;
    advance_p(P);
    if (!expect(P, TOK_RBRACKET, "']'")) { skip_to_semi(P); return; }

    const qasm_tok_t *nameT = peek_tok(P);
    char name[32];
    if (copy_ident_text(P, nameT, name, sizeof name) != 0) {
        error_at(P, nameT, 18u, "EXPECTED REGISTER NAME");
        skip_to_semi(P);
        return;
    }
    advance_p(P);

    (void)tir_qreg(P->M, name, width);
    tir_stamp_loc(P->M, kwT->line, kwT->col);

    (void)expect(P, TOK_SEMI, "';'");
}

/* bit '[' INT ']' IDENT ';' */
static void parse_creg(parser_t *P)
{
    assert(P != NULL);
    const qasm_tok_t *kwT = peek_tok(P);
    advance_p(P);  /* consume bit */

    if (!expect(P, TOK_LBRACKET, "'['")) { skip_to_semi(P); return; }
    const qasm_tok_t *widthT = peek_tok(P);
    if (widthT->kind != TOK_INT) {
        error_at(P, widthT, 19u, "EXPECTED INTEGER WIDTH");
        skip_to_semi(P);
        return;
    }
    uint32_t width = (uint32_t)widthT->val.ival;
    advance_p(P);
    if (!expect(P, TOK_RBRACKET, "']'")) { skip_to_semi(P); return; }

    const qasm_tok_t *nameT = peek_tok(P);
    char name[32];
    if (copy_ident_text(P, nameT, name, sizeof name) != 0) {
        error_at(P, nameT, 20u, "EXPECTED REGISTER NAME");
        skip_to_semi(P);
        return;
    }
    advance_p(P);

    (void)tir_creg(P->M, name, width);
    tir_stamp_loc(P->M, kwT->line, kwT->col);

    (void)expect(P, TOK_SEMI, "';'");
}

/* ----- Built-in gate dispatch ------------------------------------ */
/*
 * Map a gate name to a TIR emit function. Returns 1 if recognised,
 * 0 otherwise. The (num_qubits, num_angles) pair tells the dispatch
 * what shape of arguments to expect and which emitter to call.
 */
typedef struct {
    const char *name;
    uint8_t     num_qubits;   /* 1, 2, or 3                         */
    uint8_t     num_angles;   /* 0 or 1                             */
    tir_op_t    op;
} builtin_gate_t;

static const builtin_gate_t BUILTIN_GATES[] = {
    /* Single-qubit Cliffords */
    { "h",    1, 0, TIR_GATE_H   },
    { "x",    1, 0, TIR_GATE_X   },
    { "y",    1, 0, TIR_GATE_Y   },
    { "z",    1, 0, TIR_GATE_Z   },
    { "s",    1, 0, TIR_GATE_S   },
    { "t",    1, 0, TIR_GATE_T   },
    { "sdg",  1, 0, TIR_GATE_SDG },
    { "tdg",  1, 0, TIR_GATE_TDG },
    { "sx",   1, 0, TIR_GATE_SX  },
    /* Single-qubit rotations */
    { "rx",   1, 1, TIR_GATE_RX  },
    { "ry",   1, 1, TIR_GATE_RY  },
    { "rz",   1, 1, TIR_GATE_RZ  },
    /* Two-qubit Cliffords */
    { "cx",   2, 0, TIR_GATE_CX   },
    { "cz",   2, 0, TIR_GATE_CZ   },
    { "ch",   2, 0, TIR_GATE_CH   },
    { "swap", 2, 0, TIR_GATE_SWAP },
    /* Two-qubit parameterised */
    { "cp",   2, 1, TIR_GATE_CP   },
    /* Three-qubit */
    { "ccx",  3, 0, TIR_GATE_CCX  },
};
#define NUM_BUILTIN_GATES (sizeof BUILTIN_GATES / sizeof BUILTIN_GATES[0])

static const builtin_gate_t *find_builtin(const char *name)
{
    assert(name != NULL);
    for (uint32_t i = 0u; i < (uint32_t)NUM_BUILTIN_GATES; i++) {
        if (strcmp(name, BUILTIN_GATES[i].name) == 0) {
            return &BUILTIN_GATES[i];
        }
    }
    return NULL;
}

/* ----- Custom gate lookup ---------------------------------------- */

static const gate_def_t *find_gate_def(const parser_t *P, const char *name)
{
    assert(P != NULL);
    assert(name != NULL);
    for (uint32_t i = 0u; i < P->num_gate_defs; i++) {
        if (strcmp(P->gate_defs[i].name, name) == 0) {
            return &P->gate_defs[i];
        }
    }
    return NULL;
}

/* ----- Find end-of-expression token index ------------------------ */
/*
 * Used by the angle-expression evaluator: scan forward from the
 * current parser position until we hit a closing paren that matches
 * the open paren we just consumed. Returns the index of the close
 * paren (so the caller can slice [start, close) for evaluation).
 */
static uint32_t find_matching_rparen(const parser_t *P)
{
    assert(P != NULL);
    uint32_t depth = 1u;
    uint32_t i = P->pos;
    while (i < P->L->num_tokens) {
        qasm_tok_kind_t k = P->L->tokens[i].kind;
        if (k == TOK_LPAREN) depth++;
        else if (k == TOK_RPAREN) {
            depth--;
            if (depth == 0u) return i;
        } else if (k == TOK_EOF) {
            return i;
        }
        i++;
    }
    return i;
}

/* ----- Expand a custom gate call --------------------------------- */
/*
 * The caller has parsed: GATE_NAME '(' arg_list ')' qubit_list ';'
 * and recorded the actual angle values and qubit refs. Now we
 * synthesise TIR instructions per the gate's body, substituting
 * formal-to-actual.
 */
static void expand_custom_gate(parser_t *P, const gate_def_t *G,
                               const double *actual_angles, uint32_t num_angles,
                               const uint32_t *actual_qubits, uint32_t num_qubits)
{
    assert(P != NULL);
    assert(G != NULL);
    (void)actual_angles;
    (void)num_angles;
    /* v0.1 simplification: the gate body's angle slots are baked
     * literals from the body parser; parameter substitution into
     * angle expressions is v0.2 work. We use actual_angles only
     * when the body recorded a parameter index in place of a
     * literal, which v0.1 doesn't yet. */

    if (G->num_qargs != num_qubits) {
        return;  /* arity already checked by caller */
    }

    uint32_t n = G->num_body_stmts;
    for (uint32_t i = 0u; i < n; i++) {
        const builtin_gate_t *B = find_builtin(G->body[i].gate_name);
        if (B == NULL) {
            continue;  /* nested user gates handled in v0.2 */
        }
        /* Single-qubit rotation. */
        if (B->num_qubits == 1u && B->num_angles == 1u) {
            uint32_t q = actual_qubits[G->body[i].qslots[0]];
            if (B->op == TIR_GATE_RX) tir_emit_rx(P->M, q, G->body[i].angle);
            else if (B->op == TIR_GATE_RY) tir_emit_ry(P->M, q, G->body[i].angle);
            else if (B->op == TIR_GATE_RZ) tir_emit_rz(P->M, q, G->body[i].angle);
            continue;
        }
        /* Two-qubit controlled phase. */
        if (B->num_qubits == 2u && B->num_angles == 1u) {
            uint32_t a = actual_qubits[G->body[i].qslots[0]];
            uint32_t b = actual_qubits[G->body[i].qslots[1]];
            if (B->op == TIR_GATE_CP) tir_emit_cp(P->M, a, b, G->body[i].angle);
            continue;
        }
        /* Two-qubit Clifford. */
        if (B->num_qubits == 2u) {
            uint32_t a = actual_qubits[G->body[i].qslots[0]];
            uint32_t b = actual_qubits[G->body[i].qslots[1]];
            if (B->op == TIR_GATE_CX)        tir_emit_cx(P->M, a, b);
            else if (B->op == TIR_GATE_CZ)   tir_emit_cz(P->M, a, b);
            else if (B->op == TIR_GATE_CH)   tir_emit_ch(P->M, a, b);
            else if (B->op == TIR_GATE_SWAP) tir_emit_swap(P->M, a, b);
            continue;
        }
        /* Three-qubit Toffoli. */
        if (B->num_qubits == 3u) {
            uint32_t a = actual_qubits[G->body[i].qslots[0]];
            uint32_t b = actual_qubits[G->body[i].qslots[1]];
            uint32_t c = actual_qubits[G->body[i].qslots[2]];
            if (B->op == TIR_GATE_CCX) tir_emit_ccx(P->M, a, b, c);
            continue;
        }
        /* Single-qubit non-rotation. */
        uint32_t q = actual_qubits[G->body[i].qslots[0]];
        switch (B->op) {
        case TIR_GATE_H:   tir_emit_h(P->M, q);   break;
        case TIR_GATE_X:   tir_emit_x(P->M, q);   break;
        case TIR_GATE_Y:   tir_emit_y(P->M, q);   break;
        case TIR_GATE_Z:   tir_emit_z(P->M, q);   break;
        case TIR_GATE_S:   tir_emit_s(P->M, q);   break;
        case TIR_GATE_T:   tir_emit_t(P->M, q);   break;
        case TIR_GATE_SDG: tir_emit_sdg(P->M, q); break;
        case TIR_GATE_TDG: tir_emit_tdg(P->M, q); break;
        case TIR_GATE_SX:  tir_emit_sx(P->M,  q); break;
        default: break;
        }
    }
}

/* ----- Gate call ------------------------------------------------- */
/*
 * IDENT [ '(' angle_list ')' ] qubit_ref (',' qubit_ref)* ';'
 *
 * This is the workhorse. Identifies whether the gate is built-in
 * or user-defined, parses any angle parameters, parses the qubit
 * argument list, and emits TIR.
 */
static void parse_gate_call(parser_t *P)
{
    assert(P != NULL);
    const qasm_tok_t *nameT = peek_tok(P);
    char name[32];
    if (copy_ident_text(P, nameT, name, sizeof name) != 0) {
        error_at(P, nameT, 21u, "EXPECTED GATE NAME");
        skip_to_semi(P);
        return;
    }
    advance_p(P);

    /* Parse optional (angles). */
    double angles[8];
    uint32_t num_angles = 0u;
    if (peek_tok(P)->kind == TOK_LPAREN) {
        advance_p(P);
        /* Find the matching close paren. */
        uint32_t close_idx = find_matching_rparen(P);
        if (close_idx >= P->L->num_tokens ||
            P->L->tokens[close_idx].kind == TOK_EOF) {
            error_at(P, peek_tok(P), 22u, "UNTERMINATED ANGLE LIST");
            skip_to_semi(P);
            return;
        }
        /* Now split the range [pos, close_idx) at top-level commas
         * and evaluate each piece as an expression. */
        uint32_t arg_start = P->pos;
        uint32_t depth = 0u;
        for (uint32_t i = P->pos; i <= close_idx; i++) {
            qasm_tok_kind_t k = P->L->tokens[i].kind;
            if (k == TOK_LPAREN) depth++;
            else if (k == TOK_RPAREN) {
                if (depth > 0u) depth--;
                else {
                    /* Final argument. */
                    if (num_angles < 8u) {
                        double v = 0.0;
                        if (expr_eval(P->L, arg_start, i,
                                      P->source_file, P->log, &v) == 0) {
                            angles[num_angles++] = v;
                        }
                    }
                    arg_start = i + 1u;
                    break;
                }
            } else if (k == TOK_COMMA && depth == 0u) {
                if (num_angles < 8u) {
                    double v = 0.0;
                    if (expr_eval(P->L, arg_start, i,
                                  P->source_file, P->log, &v) == 0) {
                        angles[num_angles++] = v;
                    }
                }
                arg_start = i + 1u;
            }
        }
        /* Advance the parser cursor past the close paren. */
        P->pos = close_idx + 1u;
    }

    /* Parse the qubit argument list. */
    uint32_t qrefs[8];
    uint32_t num_qrefs = 0u;
    for (;;) {
        if (num_qrefs >= 8u) {
            error_at(P, peek_tok(P), 23u, "TOO MANY QUBIT ARGUMENTS");
            skip_to_semi(P);
            return;
        }
        int ok = 0;
        uint32_t r = parse_ref(P, 1, &ok);
        if (!ok) {
            skip_to_semi(P);
            return;
        }
        qrefs[num_qrefs++] = r;
        if (peek_tok(P)->kind == TOK_COMMA) {
            advance_p(P);
            continue;
        }
        break;
    }
    if (!expect(P, TOK_SEMI, "';'")) { return; }

    /* Dispatch. Built-in first, then custom. The (num_qubits,
     * num_angles) shape on the builtin entry tells us which arity
     * to expect; the opcode tells us which TIR emitter to call.
     *
     * After every successful emit we stamp the gate-name token's
     * source location onto the freshly-added instruction, so the
     * downstream cross-reference listing can point each output gate
     * back to a meaningful place in the input file. */
    const builtin_gate_t *B = find_builtin(name);
    if (B != NULL) {
        if ((uint32_t)B->num_qubits != num_qrefs) {
            error_at(P, nameT, 24u,
                     "GATE %s EXPECTS %u QUBITS, GOT %u",
                     name, (unsigned)B->num_qubits, (unsigned)num_qrefs);
            return;
        }
        if ((uint32_t)B->num_angles != num_angles) {
            error_at(P, nameT, 25u,
                     "GATE %s EXPECTS %u ANGLES, GOT %u",
                     name, (unsigned)B->num_angles, (unsigned)num_angles);
            return;
        }

        /* Single-qubit rotation. */
        if (B->num_qubits == 1u && B->num_angles == 1u) {
            if      (B->op == TIR_GATE_RX) tir_emit_rx(P->M, qrefs[0], angles[0]);
            else if (B->op == TIR_GATE_RY) tir_emit_ry(P->M, qrefs[0], angles[0]);
            else if (B->op == TIR_GATE_RZ) tir_emit_rz(P->M, qrefs[0], angles[0]);
            tir_stamp_loc(P->M, nameT->line, nameT->col);
            return;
        }
        /* Two-qubit controlled phase. */
        if (B->num_qubits == 2u && B->num_angles == 1u) {
            if (B->op == TIR_GATE_CP) tir_emit_cp(P->M, qrefs[0], qrefs[1], angles[0]);
            tir_stamp_loc(P->M, nameT->line, nameT->col);
            return;
        }
        /* Two-qubit Clifford. */
        if (B->num_qubits == 2u) {
            if      (B->op == TIR_GATE_CX)   tir_emit_cx(P->M, qrefs[0], qrefs[1]);
            else if (B->op == TIR_GATE_CZ)   tir_emit_cz(P->M, qrefs[0], qrefs[1]);
            else if (B->op == TIR_GATE_CH)   tir_emit_ch(P->M, qrefs[0], qrefs[1]);
            else if (B->op == TIR_GATE_SWAP) tir_emit_swap(P->M, qrefs[0], qrefs[1]);
            tir_stamp_loc(P->M, nameT->line, nameT->col);
            return;
        }
        /* Three-qubit Toffoli. */
        if (B->num_qubits == 3u) {
            if (B->op == TIR_GATE_CCX) tir_emit_ccx(P->M, qrefs[0], qrefs[1], qrefs[2]);
            tir_stamp_loc(P->M, nameT->line, nameT->col);
            return;
        }
        /* Single-qubit non-rotation. */
        switch (B->op) {
        case TIR_GATE_H:   tir_emit_h(P->M, qrefs[0]);   break;
        case TIR_GATE_X:   tir_emit_x(P->M, qrefs[0]);   break;
        case TIR_GATE_Y:   tir_emit_y(P->M, qrefs[0]);   break;
        case TIR_GATE_Z:   tir_emit_z(P->M, qrefs[0]);   break;
        case TIR_GATE_S:   tir_emit_s(P->M, qrefs[0]);   break;
        case TIR_GATE_T:   tir_emit_t(P->M, qrefs[0]);   break;
        case TIR_GATE_SDG: tir_emit_sdg(P->M, qrefs[0]); break;
        case TIR_GATE_TDG: tir_emit_tdg(P->M, qrefs[0]); break;
        case TIR_GATE_SX:  tir_emit_sx(P->M,  qrefs[0]); break;
        default:
            error_at(P, nameT, 28u, "INTERNAL: unhandled built-in gate");
            break;
        }
        tir_stamp_loc(P->M, nameT->line, nameT->col);
        return;
    }

    /* User-defined gate. */
    const gate_def_t *G = find_gate_def(P, name);
    if (G != NULL) {
        if (num_qrefs != G->num_qargs) {
            error_at(P, nameT, 29u,
                     "GATE %s EXPECTS %u QUBITS, GOT %u",
                     name, (unsigned)G->num_qargs, (unsigned)num_qrefs);
            return;
        }
        if (num_angles != G->num_params) {
            error_at(P, nameT, 30u,
                     "GATE %s EXPECTS %u ANGLES, GOT %u",
                     name, (unsigned)G->num_params, (unsigned)num_angles);
            return;
        }
        expand_custom_gate(P, G, angles, num_angles, qrefs, num_qrefs);
        return;
    }

    error_at(P, nameT, 31u, "UNKNOWN GATE '%s'", name);
}

/* ----- Measurement ----------------------------------------------- */
/*
 * Two forms:
 *   c[i] = measure q[j] ;            (assignment form)
 *   measure q[j] -> c[i] ;           (arrow form)
 *
 * The lexer-classified KW_MEASURE distinguishes them: assignment
 * form starts with IDENT, arrow form starts with KW_MEASURE. We
 * already know which we have by the time this is called.
 */
static void parse_measure_assign(parser_t *P)
{
    assert(P != NULL);
    /* Capture the starting token for provenance before we advance. */
    const qasm_tok_t *startT = peek_tok(P);
    int ok = 0;
    uint32_t cref = parse_ref(P, 0, &ok);
    if (!ok) { skip_to_semi(P); return; }
    if (!expect(P, TOK_EQUALS, "'='"))    { skip_to_semi(P); return; }
    if (!expect(P, TOK_KW_MEASURE, "'measure'")) { skip_to_semi(P); return; }
    uint32_t qref = parse_ref(P, 1, &ok);
    if (!ok) { skip_to_semi(P); return; }
    if (!expect(P, TOK_SEMI, "';'")) { return; }
    tir_emit_measure(P->M, qref, cref);
    tir_stamp_loc(P->M, startT->line, startT->col);
}

static void parse_measure_arrow(parser_t *P)
{
    assert(P != NULL);
    const qasm_tok_t *measureT = peek_tok(P);
    advance_p(P);  /* consume measure */
    int ok = 0;
    uint32_t qref = parse_ref(P, 1, &ok);
    if (!ok) { skip_to_semi(P); return; }
    if (!expect(P, TOK_ARROW, "'->'")) { skip_to_semi(P); return; }
    uint32_t cref = parse_ref(P, 0, &ok);
    if (!ok) { skip_to_semi(P); return; }
    if (!expect(P, TOK_SEMI, "';'")) { return; }
    tir_emit_measure(P->M, qref, cref);
    tir_stamp_loc(P->M, measureT->line, measureT->col);
}

/* reset q[i] ; */
static void parse_reset(parser_t *P)
{
    assert(P != NULL);
    const qasm_tok_t *resetT = peek_tok(P);
    advance_p(P);  /* consume reset */
    int ok = 0;
    uint32_t qref = parse_ref(P, 1, &ok);
    if (!ok) { skip_to_semi(P); return; }
    if (!expect(P, TOK_SEMI, "';'")) { return; }
    tir_emit_reset(P->M, qref);
    tir_stamp_loc(P->M, resetT->line, resetT->col);
}

/* ----- Custom gate definition ------------------------------------ */
/*
 * gate NAME [ '(' param_list ')' ] qubit_list '{' body '}'
 *
 * We record the formal parameter names, the formal qubit names,
 * and a small linearised body (each statement is "gate name plus
 * which qubit slots it uses"). When a call to NAME shows up later
 * the parser walks this body with the actual qubits substituted.
 */
static void parse_gate_def(parser_t *P)
{
    assert(P != NULL);
    advance_p(P);  /* consume gate */

    if (P->num_gate_defs >= (uint32_t)PARSE_MAX_GATE_DEFS) {
        error_at(P, peek_tok(P), 32u, "TOO MANY GATE DEFINITIONS");
        skip_to_semi(P);
        return;
    }

    gate_def_t *G = &P->gate_defs[P->num_gate_defs];
    memset(G, 0, sizeof *G);

    /* Gate name. */
    const qasm_tok_t *nameT = peek_tok(P);
    if (copy_ident_text(P, nameT, G->name, sizeof G->name) != 0) {
        error_at(P, nameT, 33u, "EXPECTED GATE NAME AFTER 'gate'");
        skip_to_semi(P);
        return;
    }
    advance_p(P);

    /* Optional parameter list. */
    if (peek_tok(P)->kind == TOK_LPAREN) {
        advance_p(P);
        while (peek_tok(P)->kind != TOK_RPAREN &&
               peek_tok(P)->kind != TOK_EOF) {
            const qasm_tok_t *pT = peek_tok(P);
            if (G->num_params >= 8u) {
                error_at(P, pT, 34u, "TOO MANY GATE PARAMETERS");
                break;
            }
            (void)copy_ident_text(P, pT,
                                  G->param_names[G->num_params],
                                  sizeof G->param_names[0]);
            G->num_params++;
            advance_p(P);
            if (peek_tok(P)->kind == TOK_COMMA) advance_p(P);
        }
        (void)expect(P, TOK_RPAREN, "')'");
    }

    /* Qubit argument list. */
    while (peek_tok(P)->kind != TOK_LBRACE &&
           peek_tok(P)->kind != TOK_EOF) {
        const qasm_tok_t *qT = peek_tok(P);
        if (G->num_qargs >= 8u) {
            error_at(P, qT, 35u, "TOO MANY GATE QUBIT ARGS");
            break;
        }
        (void)copy_ident_text(P, qT,
                              G->qarg_names[G->num_qargs],
                              sizeof G->qarg_names[0]);
        G->num_qargs++;
        advance_p(P);
        if (peek_tok(P)->kind == TOK_COMMA) advance_p(P);
    }

    if (!expect(P, TOK_LBRACE, "'{'")) { return; }

    /* Body. Each statement is GATE_NAME [(angle_literal)] QARG_NAME
     * (',' QARG_NAME)* ';'. v0.1 only supports literal-double
     * angles in custom gate bodies; full parameterised expressions
     * inside the body need a tiny symbol table and that's v0.2. */
    while (peek_tok(P)->kind != TOK_RBRACE &&
           peek_tok(P)->kind != TOK_EOF) {
        if (G->num_body_stmts >= (uint32_t)PARSE_MAX_GATE_BODY) {
            error_at(P, peek_tok(P), 36u, "GATE BODY TOO LARGE");
            skip_to_semi(P);
            continue;
        }
        const qasm_tok_t *gT = peek_tok(P);
        char gname[32];
        if (copy_ident_text(P, gT, gname, sizeof gname) != 0) {
            error_at(P, gT, 37u, "EXPECTED GATE NAME IN BODY");
            skip_to_semi(P);
            continue;
        }
        advance_p(P);

        /* Slot for this body statement. */
        uint32_t bi = G->num_body_stmts++;
        memcpy(G->body[bi].gate_name, gname, sizeof G->body[bi].gate_name);

        /* Optional literal angle. */
        if (peek_tok(P)->kind == TOK_LPAREN) {
            advance_p(P);
            uint32_t close_idx = find_matching_rparen(P);
            double v = 0.0;
            (void)expr_eval(P->L, P->pos, close_idx,
                            P->source_file, P->log, &v);
            P->pos = close_idx + 1u;
            G->body[bi].has_angle = 1;
            G->body[bi].angle = v;
        }

        /* Qubit slot list: look each name up in the formal arg
         * list and record the index. */
        while (peek_tok(P)->kind != TOK_SEMI &&
               peek_tok(P)->kind != TOK_EOF) {
            const qasm_tok_t *qT = peek_tok(P);
            char qname[32];
            (void)copy_ident_text(P, qT, qname, sizeof qname);
            advance_p(P);
            /* Find formal qubit by name. */
            int slot = -1;
            for (uint32_t k = 0u; k < G->num_qargs; k++) {
                if (strcmp(G->qarg_names[k], qname) == 0) {
                    slot = (int)k;
                    break;
                }
            }
            if (slot < 0) {
                error_at(P, qT, 38u,
                         "QUBIT '%s' NOT IN GATE PARAMETER LIST", qname);
            } else if (G->body[bi].num_qslots < 4u) {
                G->body[bi].qslots[G->body[bi].num_qslots++] = (uint32_t)slot;
            }
            if (peek_tok(P)->kind == TOK_COMMA) advance_p(P);
        }
        (void)expect(P, TOK_SEMI, "';'");
    }

    if (!expect(P, TOK_RBRACE, "'}'")) { return; }

    P->num_gate_defs++;
}

/* ----- Top-level dispatch ---------------------------------------- */

static void parse_top_level_stmt(parser_t *P)
{
    assert(P != NULL);
    const qasm_tok_t *T = peek_tok(P);

    switch (T->kind) {
    case TOK_KW_OPENQASM: parse_version(P);   return;
    case TOK_KW_INCLUDE:  parse_include(P);   return;
    case TOK_KW_QUBIT:    parse_qreg(P);      return;
    case TOK_KW_BIT:      parse_creg(P);      return;
    case TOK_KW_GATE:     parse_gate_def(P);  return;
    case TOK_KW_MEASURE:  parse_measure_arrow(P); return;
    case TOK_KW_RESET:    parse_reset(P);     return;
    default: break;
    }

    /* Two-IDENT-form measure-assign: starts with IDENT '[' ... '='. */
    if (T->kind == TOK_IDENT) {
        /* Look ahead far enough to tell measure-assign from
         * gate-call. Measure-assign has IDENT [INT] = KW_MEASURE.
         * Gate-call starts the same way up to '[' but doesn't have
         * '=' next. The cheap discriminator is: scan forward to the
         * next semi looking for KW_MEASURE; if we find it, it's a
         * measure-assign. Bounded by skip_to_semi. */
        uint32_t i = P->pos;
        int is_measure = 0;
        while (i < P->L->num_tokens &&
               P->L->tokens[i].kind != TOK_SEMI &&
               P->L->tokens[i].kind != TOK_EOF) {
            if (P->L->tokens[i].kind == TOK_KW_MEASURE) {
                is_measure = 1;
                break;
            }
            i++;
        }
        if (is_measure) {
            parse_measure_assign(P);
            return;
        }
        parse_gate_call(P);
        return;
    }

    /* Anything else is an unexpected token at the top level. */
    error_at(P, T, 40u, "UNEXPECTED TOKEN %s AT TOP LEVEL",
             qasm_tok_name(T->kind));
    advance_p(P);
}

/* ----- Public entry ---------------------------------------------- */

int qasm_parse(const qasm_lexer_t *L,
               tir_module_t *M,
               mnote_log_t *log)
{
    assert(L != NULL);
    assert(M != NULL);
    assert(log != NULL);

    parser_t P;
    memset(&P, 0, sizeof P);
    P.L           = L;
    P.pos         = 0u;
    P.M           = M;
    P.log         = log;
    P.source_file = L->source_file;

    while (peek_tok(&P)->kind != TOK_EOF) {
        parse_top_level_stmt(&P);
    }

    return (int)mnote_exit_code(log);
}
