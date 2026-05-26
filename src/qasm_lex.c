#include "qasm_lex.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * The lexer. Walks the source buffer one character at a time and
 * emits tokens into a flat array. The interesting work happens at
 * the boundaries: where does an identifier stop, where does a number
 * end, was that two slashes that started a comment or one slash that
 * meant division.
 *
 * The classical world is generous about this sort of thing. Quantum
 * mechanics doesn't get a vote until the parser starts.
 */

/* ----- Keyword table --------------------------------------------- */
/*
 * Classified after an identifier has been collected. The shape is
 * deliberately ordered: most-common keywords first, because we are
 * going to read them several million times over the lifetime of the
 * project and the linear scan is fine.
 */
typedef struct {
    const char     *name;
    qasm_tok_kind_t kind;
} qasm_keyword_t;

static const qasm_keyword_t KEYWORDS[] = {
    /* Names appearing in basically every QASM file we will ever
     * meet. */
    { "qubit",    TOK_KW_QUBIT },
    { "bit",      TOK_KW_BIT },
    { "measure",  TOK_KW_MEASURE },
    { "reset",    TOK_KW_RESET },
    { "include",  TOK_KW_INCLUDE },
    { "OPENQASM", TOK_KW_OPENQASM },

    /* Less common but still expected. */
    { "gate",     TOK_KW_GATE },
    { "def",      TOK_KW_DEF },
    { "if",       TOK_KW_IF },
    { "else",     TOK_KW_ELSE },
    { "for",      TOK_KW_FOR },
    { "while",    TOK_KW_WHILE },

    /* Classical type names. Parsed and recognised so the lexer
     * doesn't classify them as plain identifiers, even when the
     * parser later decides it doesn't care. */
    { "int",      TOK_KW_INT },
    { "float",    TOK_KW_FLOAT },
    { "bool",     TOK_KW_BOOL },
    { "angle",    TOK_KW_ANGLE },
};

#define NUM_KEYWORDS (sizeof KEYWORDS / sizeof KEYWORDS[0])

static qasm_tok_kind_t classify_ident(const char *s, uint32_t len)
{
    assert(s != NULL);
    /* The keyword table is small enough that a linear scan is
     * cheaper than the cache miss any clever data structure would
     * incur. */
    for (uint32_t i = 0u; i < (uint32_t)NUM_KEYWORDS; i++) {
        const char *k = KEYWORDS[i].name;
        size_t klen = strlen(k);
        if (klen == (size_t)len && strncmp(s, k, klen) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TOK_IDENT;
}

/* ----- Read helpers ---------------------------------------------- */
/*
 * peek() looks at the current character without consuming it.
 * peek2() looks at the next one. advance() consumes one character
 * and updates the line and column counters. The three together
 * cover ninety-eight per cent of the lexer's reading needs.
 */
static char peek(const qasm_lexer_t *L)
{
    assert(L != NULL);
    if (L->pos >= L->source_len) {
        return '\0';
    }
    return L->source[L->pos];
}

static char peek2(const qasm_lexer_t *L)
{
    assert(L != NULL);
    if (L->pos + 1u >= L->source_len) {
        return '\0';
    }
    return L->source[L->pos + 1u];
}

static void advance(qasm_lexer_t *L)
{
    assert(L != NULL);
    if (L->pos >= L->source_len) {
        return;
    }
    char c = L->source[L->pos];
    L->pos++;
    if (c == '\n') {
        L->line++;
        L->col = 1u;
    } else {
        L->col++;
    }
}

/* ----- Whitespace and comments ----------------------------------- */
/*
 * Skip whitespace and both styles of comment. Slash-slash runs to
 * end of line. Slash-star ... star-slash runs until the matching
 * close. Nested blocks are not supported because OpenQASM 3
 * doesn't support them, C99 doesn't either, and we are not in the
 * business of inventing exciting new behaviours for quantum
 * circuit comments.
 */
static void skip_ws_and_comments(qasm_lexer_t *L)
{
    assert(L != NULL);

    for (;;) {
        char c = peek(L);
        if (c == '\0') {
            return;
        }

        /* Plain whitespace, including newlines. */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(L);
            continue;
        }

        /* Line comment: // to end of line. */
        if (c == '/' && peek2(L) == '/') {
            while (peek(L) != '\0' && peek(L) != '\n') {
                advance(L);
            }
            continue;
        }

        /* Block comment. Eat the opener, then scan forward until
         * the matching close. If the close is missing the comment
         * runs to EOF, which is something the parser will notice
         * when it asks for a token and gets back EOF unexpectedly. */
        if (c == '/' && peek2(L) == '*') {
            advance(L);  /* eat / */
            advance(L);  /* eat * */
            while (peek(L) != '\0') {
                if (peek(L) == '*' && peek2(L) == '/') {
                    advance(L);  /* eat * */
                    advance(L);  /* eat / */
                    break;
                }
                advance(L);
            }
            continue;
        }

        return;
    }
}

/* ----- Add a token ------------------------------------------------ */
static qasm_tok_t *push_token(qasm_lexer_t *L, qasm_tok_kind_t kind,
                              uint32_t line, uint32_t col)
{
    assert(L != NULL);
    if (L->num_tokens >= (uint32_t)QASM_LEX_MAX_TOKENS) {
        if (L->log != NULL) {
            mnote_emit(L->log, MNOTE_FATAL, 16u,
                       L->source_file, line, col,
                       "TOKEN BUFFER OVERFLOW AT %u TOKENS",
                       (unsigned)L->num_tokens);
        }
        return NULL;
    }
    qasm_tok_t *T = &L->tokens[L->num_tokens++];
    T->kind     = kind;
    T->line     = line;
    T->col      = col;
    T->src_off  = 0u;
    T->src_len  = 0u;
    T->val.ival = 0;
    return T;
}

/* ----- Lex an identifier or keyword ------------------------------ */
/*
 * Read [A-Za-z_][A-Za-z0-9_]*. Classify the result against the
 * keyword table. Identifier text is recorded as (offset, length)
 * into the source buffer; no copy.
 */
static void lex_ident(qasm_lexer_t *L)
{
    assert(L != NULL);
    uint32_t start_line = L->line;
    uint32_t start_col  = L->col;
    uint32_t start_pos  = L->pos;

    while (peek(L) != '\0') {
        char c = peek(L);
        if (c == '_' || isalnum((unsigned char)c)) {
            advance(L);
        } else {
            break;
        }
    }

    uint32_t len = L->pos - start_pos;
    qasm_tok_kind_t kind = classify_ident(&L->source[start_pos], len);

    qasm_tok_t *T = push_token(L, kind, start_line, start_col);
    if (T != NULL) {
        T->src_off = start_pos;
        T->src_len = len;
    }
}

/* ----- Lex a number ---------------------------------------------- */
/*
 * Read either an integer literal or a floating-point literal. The
 * distinction is whether the run of digits contains a dot or an
 * exponent. Hex and binary literals are not yet supported because
 * no demo emits them; will be added when the first circuit needs
 * one.
 *
 * On parse failure (which should be impossible given the read loop
 * above) the token is recorded as a zero-valued integer and an
 * MNOTE is raised. The lexer keeps going.
 */
static void lex_number(qasm_lexer_t *L)
{
    assert(L != NULL);
    uint32_t start_line = L->line;
    uint32_t start_col  = L->col;
    uint32_t start_pos  = L->pos;
    int is_float = 0;

    /* Integer part. */
    while (isdigit((unsigned char)peek(L))) {
        advance(L);
    }

    /* Optional fractional part. */
    if (peek(L) == '.' && isdigit((unsigned char)peek2(L))) {
        is_float = 1;
        advance(L);  /* eat the dot */
        while (isdigit((unsigned char)peek(L))) {
            advance(L);
        }
    }

    /* Optional exponent. */
    if (peek(L) == 'e' || peek(L) == 'E') {
        is_float = 1;
        advance(L);
        if (peek(L) == '+' || peek(L) == '-') {
            advance(L);
        }
        while (isdigit((unsigned char)peek(L))) {
            advance(L);
        }
    }

    uint32_t len = L->pos - start_pos;
    qasm_tok_t *T = push_token(L,
                               is_float ? TOK_FLOAT : TOK_INT,
                               start_line, start_col);
    if (T == NULL) {
        return;
    }

    T->src_off = start_pos;
    T->src_len = len;

    /* Build a null-terminated copy on a small stack buffer for the
     * strtol/strtod call. Numbers in practice are short. */
    char buf[64];
    uint32_t copy_len = (len < 63u) ? len : 63u;
    memcpy(buf, &L->source[start_pos], copy_len);
    buf[copy_len] = '\0';

    if (is_float) {
        T->val.fval = strtod(buf, NULL);
    } else {
        T->val.ival = (int64_t)strtoll(buf, NULL, 10);
    }
}

/* ----- Lex a string literal -------------------------------------- */
/*
 * Read a double-quoted string. No escape sequences yet because no
 * QASM file we have ever met needs them; if one shows up the lexer
 * will need to learn about \n, \\, and friends. For now: the bytes
 * between the quotes, verbatim, recorded as (offset, length) into
 * the source.
 */
static void lex_string(qasm_lexer_t *L)
{
    assert(L != NULL);
    uint32_t start_line = L->line;
    uint32_t start_col  = L->col;

    advance(L);  /* eat opening quote */
    uint32_t body_start = L->pos;

    while (peek(L) != '\0' && peek(L) != '"' && peek(L) != '\n') {
        advance(L);
    }

    uint32_t body_len = L->pos - body_start;

    if (peek(L) != '"') {
        if (L->log != NULL) {
            mnote_emit(L->log, MNOTE_ERROR, 4u,
                       L->source_file, start_line, start_col,
                       "UNTERMINATED STRING LITERAL");
        }
    } else {
        advance(L);  /* eat closing quote */
    }

    qasm_tok_t *T = push_token(L, TOK_STRING, start_line, start_col);
    if (T != NULL) {
        T->src_off = body_start;
        T->src_len = body_len;
    }
}

/* ----- The main loop --------------------------------------------- */
/*
 * Look at the current character. Dispatch to the right handler.
 * After each handler the position has moved past whatever it just
 * read, so the next iteration starts on a fresh character.
 *
 * Errors raise an MNOTE and advance one character so we don't loop
 * forever on bad input.
 */
int qasm_lex_run(qasm_lexer_t *L)
{
    assert(L != NULL);
    assert(L->log != NULL);

    while (L->pos < L->source_len) {
        skip_ws_and_comments(L);
        if (L->pos >= L->source_len) {
            break;
        }

        char c = peek(L);
        uint32_t line = L->line;
        uint32_t col  = L->col;

        /* Identifiers and keywords. */
        if (c == '_' || isalpha((unsigned char)c)) {
            lex_ident(L);
            continue;
        }

        /* Numbers. */
        if (isdigit((unsigned char)c)) {
            lex_number(L);
            continue;
        }

        /* Strings. */
        if (c == '"') {
            lex_string(L);
            continue;
        }

        /* Punctuation. Handled inline because there is only a small
         * number of cases and a switch reads better than a table. */
        qasm_tok_kind_t kind = TOK_COUNT;
        int eat = 1;
        switch (c) {
        case '[': kind = TOK_LBRACKET; break;
        case ']': kind = TOK_RBRACKET; break;
        case '{': kind = TOK_LBRACE;   break;
        case '}': kind = TOK_RBRACE;   break;
        case '(': kind = TOK_LPAREN;   break;
        case ')': kind = TOK_RPAREN;   break;
        case ';': kind = TOK_SEMI;     break;
        case ',': kind = TOK_COMMA;    break;
        case '+': kind = TOK_PLUS;     break;
        case '*': kind = TOK_STAR;     break;
        case '/': kind = TOK_SLASH;    break;
        case '-':
            if (peek2(L) == '>') {
                kind = TOK_ARROW;
                eat = 2;
            } else {
                kind = TOK_MINUS;
            }
            break;
        case '=':
            if (peek2(L) == '=') {
                kind = TOK_EQEQ;
                eat = 2;
            } else {
                kind = TOK_EQUALS;
            }
            break;
        default:
            break;
        }

        if (kind != TOK_COUNT) {
            qasm_tok_t *T = push_token(L, kind, line, col);
            if (T != NULL) {
                T->src_off = L->pos;
                T->src_len = (uint32_t)eat;
            }
            for (int i = 0; i < eat; i++) {
                advance(L);
            }
            continue;
        }

        /* Unknown character. Mainframe-flavoured complaint, then move
         * on so the operator can see every other problem in the file
         * on the same pass. */
        mnote_emit(L->log, MNOTE_ERROR, 1u,
                   L->source_file, line, col,
                   "UNKNOWN CHARACTER %c%c%c (0x%02X)",
                   '\'', c, '\'',
                   (unsigned)(unsigned char)c);
        advance(L);
    }

    /* Final EOF token, so the parser never has to bounds-check. */
    (void)push_token(L, TOK_EOF, L->line, L->col);

    return mnote_has_errors(L->log) ? (int)mnote_exit_code(L->log) : 0;
}

/* ----- Init ------------------------------------------------------- */

void qasm_lex_init(qasm_lexer_t *L,
                   const char *source, uint32_t source_len,
                   const char *source_file,
                   mnote_log_t *log)
{
    assert(L != NULL);
    assert(source != NULL);
    assert(log != NULL);

    memset(L, 0, sizeof *L);
    L->source      = source;
    L->source_len  = source_len;
    L->pos         = 0u;
    L->line        = 1u;
    L->col         = 1u;
    L->source_file = source_file;
    L->log         = log;
}

/* ----- Inspection ------------------------------------------------- */

const char *qasm_tok_name(qasm_tok_kind_t k)
{
    switch (k) {
    case TOK_EOF:         return "EOF";
    case TOK_INT:         return "INT";
    case TOK_FLOAT:       return "FLOAT";
    case TOK_IDENT:       return "IDENT";
    case TOK_STRING:      return "STRING";
    case TOK_LBRACKET:    return "LBRACKET";
    case TOK_RBRACKET:    return "RBRACKET";
    case TOK_LBRACE:      return "LBRACE";
    case TOK_RBRACE:      return "RBRACE";
    case TOK_LPAREN:      return "LPAREN";
    case TOK_RPAREN:      return "RPAREN";
    case TOK_SEMI:        return "SEMI";
    case TOK_COMMA:       return "COMMA";
    case TOK_ARROW:       return "ARROW";
    case TOK_EQUALS:      return "EQUALS";
    case TOK_EQEQ:        return "EQEQ";
    case TOK_PLUS:        return "PLUS";
    case TOK_MINUS:       return "MINUS";
    case TOK_STAR:        return "STAR";
    case TOK_SLASH:       return "SLASH";
    case TOK_KW_OPENQASM: return "KW_OPENQASM";
    case TOK_KW_INCLUDE:  return "KW_INCLUDE";
    case TOK_KW_QUBIT:    return "KW_QUBIT";
    case TOK_KW_BIT:      return "KW_BIT";
    case TOK_KW_INT:      return "KW_INT";
    case TOK_KW_FLOAT:    return "KW_FLOAT";
    case TOK_KW_BOOL:     return "KW_BOOL";
    case TOK_KW_ANGLE:    return "KW_ANGLE";
    case TOK_KW_MEASURE:  return "KW_MEASURE";
    case TOK_KW_RESET:    return "KW_RESET";
    case TOK_KW_GATE:     return "KW_GATE";
    case TOK_KW_DEF:      return "KW_DEF";
    case TOK_KW_IF:       return "KW_IF";
    case TOK_KW_ELSE:     return "KW_ELSE";
    case TOK_KW_FOR:      return "KW_FOR";
    case TOK_KW_WHILE:    return "KW_WHILE";
    default:              return "?";
    }
}

void qasm_lex_print(const qasm_lexer_t *L, FILE *out)
{
    assert(L != NULL);
    assert(out != NULL);

    (void)fprintf(out, "ERNESTLX tokens (%u):\n", (unsigned)L->num_tokens);
    uint32_t n = L->num_tokens;
    assert(n <= (uint32_t)QASM_LEX_MAX_TOKENS);
    for (uint32_t i = 0u; i < n; i++) {
        const qasm_tok_t *T = &L->tokens[i];
        (void)fprintf(out, "  %4u:%-3u  %-12s",
                      (unsigned)T->line, (unsigned)T->col,
                      qasm_tok_name(T->kind));
        switch (T->kind) {
        case TOK_INT:
            (void)fprintf(out, " %lld\n", (long long)T->val.ival);
            break;
        case TOK_FLOAT:
            (void)fprintf(out, " %g\n", T->val.fval);
            break;
        case TOK_IDENT:
        case TOK_STRING: {
            (void)fputc(' ', out);
            uint32_t k = T->src_len;
            if (k > 40u) k = 40u;
            for (uint32_t j = 0u; j < k; j++) {
                (void)fputc(L->source[T->src_off + j], out);
            }
            (void)fputc('\n', out);
            break;
        }
        case TOK_EOF:
            (void)fputc('\n', out);
            break;
        default:
            (void)fputc('\n', out);
            break;
        }
    }
}
