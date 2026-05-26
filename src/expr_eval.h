#ifndef ERNEST_EXPR_EVAL_H
#define ERNEST_EXPR_EVAL_H

#include <stdint.h>
#include <assert.h>

#include "mnote.h"
#include "qasm_lex.h"

/*
 * The angle expression evaluator.
 *
 * OpenQASM 3 lets rotation angles be written as expressions:
 *
 *     rx(pi/2)         q[0];
 *     ry(2*pi/4)       q[1];
 *     rz(-pi/3 + 0.1)  q[2];
 *
 * Ernest stores rotation angles as doubles. Somewhere between "stream
 * of tokens" and "stored angle" the expression has to be evaluated.
 * This module does the evaluation.
 *
 * It is a small Pratt-style parser. Reads from a sub-range of the
 * lexer's token buffer (start_idx inclusive to end_idx exclusive),
 * folds the expression down to a single double, and returns it via
 * the out parameter. Errors flow through the MNOTE channel.
 *
 * Supported grammar:
 *
 *     expr     := term (('+' | '-') term)*
 *     term     := factor (('*' | '/') factor)*
 *     factor   := number
 *               | 'pi'
 *               | '(' expr ')'
 *               | '-' factor
 *
 * Identifiers other than 'pi' are not yet supported because no demo
 * needs them. When the first custom-gate definition with parametric
 * variables shows up, this gets a tiny symbol-table extension.
 */

/*
 * Evaluate an expression from tokens[start_idx..end_idx). Returns
 * the resulting double in *out. Returns 0 on success and non-zero
 * on error (in which case an MNOTE has been emitted and *out is
 * left undefined).
 *
 * end_idx is the index of the token that ends the expression (a
 * comma, close-paren, or semicolon). The evaluator does not
 * consume that terminator.
 */
int expr_eval(const qasm_lexer_t *L,
              uint32_t start_idx, uint32_t end_idx,
              const char *source_file,
              mnote_log_t *log,
              double *out);

#endif /* ERNEST_EXPR_EVAL_H */
