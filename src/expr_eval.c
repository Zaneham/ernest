#include "expr_eval.h"
#include <string.h>
#include <math.h>

/*
 * Pratt-style angle expression parser. Small enough to fit in one
 * file and live entirely on the stack. No malloc, no recursion
 * deeper than the expression nests, which in any sane QASM file is
 * under ten levels and in any insane QASM file is the QASM file's
 * problem, not ours.
 *
 * The state struct carries the lexer pointer, the current token
 * index, the end-of-range, the source filename for diagnostics, and
 * the diagnostic log. Everything the recursive helpers need.
 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    const qasm_lexer_t *L;
    uint32_t            pos;
    uint32_t            end;
    const char         *source_file;
    mnote_log_t        *log;
    int                 had_error;
} eval_state_t;

/* ----- Token cursor ---------------------------------------------- */

static const qasm_tok_t *cur_tok(const eval_state_t *S)
{
    assert(S != NULL);
    if (S->pos >= S->end) {
        /* Past the end: return the EOF token the lexer always
         * leaves at the very end of its buffer. Safe because the
         * lexer guarantees one EOF terminator. */
        return &S->L->tokens[S->L->num_tokens - 1u];
    }
    return &S->L->tokens[S->pos];
}

static void advance_tok(eval_state_t *S)
{
    assert(S != NULL);
    if (S->pos < S->end) {
        S->pos++;
    }
}

static void error_at(eval_state_t *S, const qasm_tok_t *T,
                     const char *msg)
{
    assert(S != NULL);
    assert(T != NULL);
    assert(msg != NULL);
    if (S->log != NULL) {
        mnote_emit(S->log, MNOTE_ERROR, 20u,
                   S->source_file, T->line, T->col,
                   "ANGLE EXPRESSION: %s", msg);
    }
    S->had_error = 1;
}

/* ----- Recursive descent ----------------------------------------- */

/* Forward declaration for mutual recursion. */
static double parse_expr(eval_state_t *S);

/*
 * factor := NUMBER | 'pi' | '(' expr ')' | '-' factor
 *
 * The atoms of the expression grammar. Numbers are obvious. 'pi' is
 * the only named constant we recognise today. Parens trigger a
 * recursive call back into expr. Unary minus negates a factor.
 */
/*
 * parse_atom handles everything-except-unary-minus. parse_factor
 * collects any leading run of minus signs into a sign flip and
 * delegates the rest to parse_atom. This makes unary minus
 * iterative rather than recursive, which the static analyser much
 * prefers and which is also slightly faster.
 */
static double parse_atom(eval_state_t *S)
{
    assert(S != NULL);
    const qasm_tok_t *T = cur_tok(S);

    if (T->kind == TOK_INT) {
        advance_tok(S);
        return (double)T->val.ival;
    }
    if (T->kind == TOK_FLOAT) {
        advance_tok(S);
        return T->val.fval;
    }
    if (T->kind == TOK_IDENT) {
        /* Only 'pi' is recognised. Anything else is an error today;
         * the day we support variable angle parameters in custom
         * gate definitions, this turns into a small symbol-table
         * lookup. */
        const char *s = &S->L->source[T->src_off];
        uint32_t len = T->src_len;
        if (len == 2u && strncmp(s, "pi", 2) == 0) {
            advance_tok(S);
            return M_PI;
        }
        error_at(S, T, "UNKNOWN IDENTIFIER IN ANGLE EXPRESSION");
        advance_tok(S);
        return 0.0;
    }
    if (T->kind == TOK_LPAREN) {
        advance_tok(S);
        double v = parse_expr(S);
        const qasm_tok_t *closing = cur_tok(S);
        if (closing->kind != TOK_RPAREN) {
            error_at(S, closing, "EXPECTED ')' IN ANGLE EXPRESSION");
        } else {
            advance_tok(S);
        }
        return v;
    }

    error_at(S, T, "EXPECTED NUMBER, pi, '(' OR '-'");
    advance_tok(S);
    return 0.0;
}

static double parse_factor(eval_state_t *S)
{
    assert(S != NULL);

    /* Count leading minuses iteratively so an analyser doesn't
     * imagine the recursion is unbounded. Each pass over the loop
     * consumes one token, so it terminates in at most as many
     * iterations as there are tokens. */
    int neg = 0;
    while (cur_tok(S)->kind == TOK_MINUS) {
        advance_tok(S);
        neg = !neg;
    }
    double v = parse_atom(S);
    return neg ? -v : v;
}

/*
 * term := factor (('*' | '/') factor)*
 *
 * Standard left-associative product handling. Division by zero is
 * tolerated at evaluation time (returns infinity); the diagnostic
 * happens upstream when the angle gets used.
 */
static double parse_term(eval_state_t *S)
{
    assert(S != NULL);
    double lhs = parse_factor(S);
    for (;;) {
        const qasm_tok_t *T = cur_tok(S);
        if (T->kind == TOK_STAR) {
            advance_tok(S);
            lhs = lhs * parse_factor(S);
        } else if (T->kind == TOK_SLASH) {
            advance_tok(S);
            double rhs = parse_factor(S);
            lhs = lhs / rhs;
        } else {
            return lhs;
        }
    }
}

/*
 * expr := term (('+' | '-') term)*
 *
 * Same shape one level up. Plus and minus have the same precedence
 * as each other and lower precedence than times and divide, which
 * is what every sane person expects from a calculator.
 */
static double parse_expr(eval_state_t *S)
{
    assert(S != NULL);
    double lhs = parse_term(S);
    for (;;) {
        const qasm_tok_t *T = cur_tok(S);
        if (T->kind == TOK_PLUS) {
            advance_tok(S);
            lhs = lhs + parse_term(S);
        } else if (T->kind == TOK_MINUS) {
            advance_tok(S);
            lhs = lhs - parse_term(S);
        } else {
            return lhs;
        }
    }
}

/* ----- Public entry ---------------------------------------------- */

int expr_eval(const qasm_lexer_t *L,
              uint32_t start_idx, uint32_t end_idx,
              const char *source_file,
              mnote_log_t *log,
              double *out)
{
    assert(L != NULL);
    assert(log != NULL);
    assert(out != NULL);
    assert(start_idx <= end_idx);

    eval_state_t S;
    S.L           = L;
    S.pos         = start_idx;
    S.end         = end_idx;
    S.source_file = source_file;
    S.log         = log;
    S.had_error   = 0;

    double v = parse_expr(&S);

    if (S.had_error != 0) {
        return 1;
    }
    if (S.pos != end_idx) {
        const qasm_tok_t *T = cur_tok(&S);
        error_at(&S, T, "TRAILING TOKENS AFTER ANGLE EXPRESSION");
        return 2;
    }
    *out = v;
    return 0;
}
