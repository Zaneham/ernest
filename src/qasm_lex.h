#ifndef ERNEST_QASM_LEX_H
#define ERNEST_QASM_LEX_H

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "mnote.h"

/*
 * ERNESTLX. The OpenQASM 3 lexer.
 *
 * Reads a source buffer of OpenQASM 3 text and produces a flat array
 * of tokens for the parser to walk later. The job is straightforward,
 * which is to say it would be straightforward if quantum physics were
 * easy and computers were honest, neither of which entirely apply,
 * but the lexer at least operates in the classical world where bytes
 * are bytes and the alphabet has a finite size.
 *
 * Fixed-size token buffer. No malloc. Comments stripped. Line and
 * column tracked at all times so the diagnostic system can point at
 * the exact character that misbehaved.
 *
 * Diagnostics flow through the MNOTE channel. The lexer is the first
 * compiler stage that has to talk to the operator about what it saw,
 * and what it has to say is mostly "I found a thing I didn't
 * recognise", which is severity 8 in the mainframe scheme of things.
 */

/* ----- Limits ----------------------------------------------------- */
#define QASM_LEX_MAX_TOKENS  (1u << 16)

/* ----- Token kinds ------------------------------------------------ */
typedef enum {
    TOK_EOF = 0,

    /* Literals */
    TOK_INT,         /* 42, 0, 100                                   */
    TOK_FLOAT,       /* 0.5, 1.0e-3, 3.14159                         */
    TOK_IDENT,       /* foo, q, my_gate                              */
    TOK_STRING,      /* "stdgates.inc"                               */

    /* Punctuation */
    TOK_LBRACKET,    /* [                                            */
    TOK_RBRACKET,    /* ]                                            */
    TOK_LBRACE,      /* {                                            */
    TOK_RBRACE,      /* }                                            */
    TOK_LPAREN,      /* (                                            */
    TOK_RPAREN,      /* )                                            */
    TOK_SEMI,        /* ;                                            */
    TOK_COMMA,       /* ,                                            */
    TOK_ARROW,       /* ->                                           */
    TOK_EQUALS,      /* =                                            */
    TOK_EQEQ,        /* ==                                           */

    /* Operators (for angle expressions like pi/2, theta + pi)       */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,

    /* Keywords (recognised after identifier match)                  */
    TOK_KW_OPENQASM,
    TOK_KW_INCLUDE,
    TOK_KW_QUBIT,
    TOK_KW_BIT,
    TOK_KW_INT,
    TOK_KW_FLOAT,
    TOK_KW_BOOL,
    TOK_KW_ANGLE,
    TOK_KW_MEASURE,
    TOK_KW_RESET,
    TOK_KW_GATE,
    TOK_KW_DEF,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_FOR,
    TOK_KW_WHILE,

    TOK_COUNT
} qasm_tok_kind_t;

/* ----- One token -------------------------------------------------- */
/*
 * Tokens carry just enough information for the parser to do its job.
 * Line and column for diagnostics. A pointer-style (offset, length)
 * into the source buffer for identifiers and strings. A value union
 * for numeric literals.
 */
typedef struct {
    qasm_tok_kind_t kind;
    uint32_t        line;
    uint32_t        col;
    uint32_t        src_off;
    uint32_t        src_len;
    union {
        int64_t ival;
        double  fval;
    } val;
} qasm_tok_t;

/* ----- Lexer state ------------------------------------------------ */
/*
 * Holds the source pointer, the current read position, the line and
 * column counters, and the output token buffer. The diagnostic log
 * is passed in by the caller so the lexer can share it with the
 * parser and the rest of the pipeline.
 */
typedef struct {
    const char  *source;
    uint32_t     source_len;
    uint32_t     pos;
    uint32_t     line;
    uint32_t     col;

    qasm_tok_t   tokens[QASM_LEX_MAX_TOKENS];
    uint32_t     num_tokens;

    /* Optional source filename for MNOTE diagnostics. */
    const char  *source_file;

    /* Diagnostic log (owned by caller). */
    mnote_log_t *log;
} qasm_lexer_t;

/* ----- API -------------------------------------------------------- */

/*
 * Initialise a lexer over a source buffer. The source pointer must
 * stay valid for the lifetime of the lexer, since tokens carry
 * offsets into it rather than copies.
 */
void qasm_lex_init(qasm_lexer_t *L,
                   const char *source, uint32_t source_len,
                   const char *source_file,
                   mnote_log_t *log);

/*
 * Run the lexer to completion. Returns 0 if no errors were emitted
 * (severity less than 8), non-zero otherwise. Tokens land in the
 * lexer's tokens[] buffer; diagnostics land in the MNOTE log.
 *
 * The lexer keeps going past most errors, on the principle that one
 * unknown character shouldn't stop the operator from seeing every
 * other unknown character in the same file. Genuinely fatal
 * problems (buffer overflow) stop the lexer cold.
 */
int qasm_lex_run(qasm_lexer_t *L);

/* ----- Inspection ------------------------------------------------- */

/*
 * Token-kind name for diagnostics and debug printing.
 */
const char *qasm_tok_name(qasm_tok_kind_t k);

/*
 * Print the token stream to a stream, one token per line, with line
 * and column. Useful for debugging the lexer and for seeing what the
 * parser is about to be handed.
 */
void qasm_lex_print(const qasm_lexer_t *L, FILE *out);

#endif /* ERNEST_QASM_LEX_H */
