#include "mnote.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * The MNOTE implementation. Mostly bookkeeping: append a diagnostic
 * to the log, track the running maximum severity, format the printed
 * output the way a z/OS console would. The whole point is that
 * everywhere in the compiler talks to the operator through this one
 * channel, so the operator hears a single consistent voice rather
 * than the usual chorus of stderr.write.
 */

/* ----- Helpers ---------------------------------------------------- */

/*
 * Copy a string into a fixed-size field, null-terminated, truncating
 * with an ellipsis if the source overflows. Used for the module
 * name, the source filename, and the message body.
 */
static void copy_truncated(char *dst, uint32_t dst_size,
                           const char *src, uint32_t src_len)
{
    assert(dst != NULL);
    assert(dst_size >= 4u);
    assert(src != NULL);

    if (src_len + 1u <= dst_size) {
        memcpy(dst, src, src_len);
        dst[src_len] = '\0';
        return;
    }

    /* Source is too long. Copy what fits and append "..." inside
     * the buffer so we still have a sensible null terminator. */
    uint32_t cap = dst_size - 4u;
    memcpy(dst, src, cap);
    dst[cap]     = '.';
    dst[cap + 1] = '.';
    dst[cap + 2] = '.';
    dst[cap + 3] = '\0';
}

/* ----- Init ------------------------------------------------------- */

void mnote_init(mnote_log_t *L, const char *module_name)
{
    assert(L != NULL);
    assert(module_name != NULL);

    memset(L, 0, sizeof *L);
    L->max_severity = (uint32_t)MNOTE_INFO;

    uint32_t n = (uint32_t)strlen(module_name);
    copy_truncated(L->module_name, (uint32_t)MNOTE_MODULE_LEN,
                   module_name, n);
}

/* ----- Append a regular MNOTE ------------------------------------- */

void mnote_emit(mnote_log_t *L, mnote_severity_t sev, uint32_t code,
                const char *source_file, uint32_t line, uint32_t col,
                const char *fmt, ...)
{
    assert(L != NULL);
    assert(fmt != NULL);

    if (L->num_notes >= (uint32_t)MNOTE_MAX) {
        (void)fprintf(stderr, "mnote: log overflow, dropping diagnostic\n");
        return;
    }

    mnote_t *N = &L->notes[L->num_notes++];
    N->severity = sev;
    N->code     = code;
    N->line     = line;
    N->col      = col;

    if (source_file != NULL) {
        uint32_t n = (uint32_t)strlen(source_file);
        copy_truncated(N->source_file, (uint32_t)MNOTE_SRCFILE_LEN,
                       source_file, n);
    } else {
        N->source_file[0] = '\0';
    }

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(N->message, (size_t)MNOTE_MSG_LEN, fmt, ap);
    va_end(ap);
    if (written < 0) {
        N->message[0] = '\0';
    }

    if ((uint32_t)sev > L->max_severity) {
        L->max_severity = (uint32_t)sev;
    }
}

/* ----- Append an ABEND -------------------------------------------- */

void mnote_abend(mnote_log_t *L, const char *completion_code,
                 const char *source_file, uint32_t line, uint32_t col,
                 const char *fmt, ...)
{
    assert(L != NULL);
    assert(completion_code != NULL);
    assert(fmt != NULL);

    if (L->num_notes >= (uint32_t)MNOTE_MAX) {
        (void)fprintf(stderr, "mnote: log overflow, dropping ABEND\n");
        return;
    }

    mnote_t *N = &L->notes[L->num_notes++];
    N->severity = MNOTE_FATAL;

    /* Stash the completion code in the message buffer at the front,
     * prefixed with a marker so mnote_print can pick it out. */
    int prefix_len = snprintf(N->message, (size_t)MNOTE_MSG_LEN,
                              "ABEND %s ", completion_code);
    if (prefix_len < 0) {
        prefix_len = 0;
    }
    N->code = 0u;
    N->line = line;
    N->col  = col;

    if (source_file != NULL) {
        uint32_t n = (uint32_t)strlen(source_file);
        copy_truncated(N->source_file, (uint32_t)MNOTE_SRCFILE_LEN,
                       source_file, n);
    } else {
        N->source_file[0] = '\0';
    }

    /* Append the user-supplied message after the ABEND prefix. */
    if ((uint32_t)prefix_len < (uint32_t)MNOTE_MSG_LEN) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(N->message + prefix_len,
                        (size_t)((uint32_t)MNOTE_MSG_LEN - (uint32_t)prefix_len),
                        fmt, ap);
        va_end(ap);
    }

    L->max_severity = (uint32_t)MNOTE_FATAL;
}

/* ----- Print ------------------------------------------------------ */

/*
 * Tell the operator. The format mimics a z/OS console line. The
 * module name leads, then either MNOTE or ABEND, then the severity
 * or completion code, then the source location in square brackets,
 * then the message. The whole line reads left to right the way an
 * operator scans a job log: who is talking, what kind of thing it
 * is, how serious it is, where it happened, what went wrong.
 */
void mnote_print(const mnote_log_t *L, FILE *out)
{
    assert(L != NULL);
    assert(out != NULL);

    uint32_t n = L->num_notes;
    assert(n <= (uint32_t)MNOTE_MAX);
    for (uint32_t i = 0u; i < n; i++) {
        const mnote_t *N = &L->notes[i];

        /* ABEND messages already carry "ABEND CODE " at the front
         * of their message buffer. Regular MNOTEs print severity. */
        int is_abend = (N->severity == MNOTE_FATAL)
                       && (strncmp(N->message, "ABEND ", 6) == 0);

        (void)fprintf(out, "%-8s ", L->module_name);

        if (is_abend) {
            /* Message buffer starts "ABEND S0C7 ...". Print message
             * directly; it already has the right prefix. */
            if (N->source_file[0] != '\0') {
                (void)fprintf(out, "%s [%s:%u:%u]\n",
                              N->message,
                              N->source_file,
                              (unsigned)N->line,
                              (unsigned)N->col);
            } else {
                (void)fprintf(out, "%s\n", N->message);
            }
        } else {
            int has_file = (N->source_file[0] != '\0');
            int has_loc  = (N->line != 0u || N->col != 0u);
            if (has_file && has_loc) {
                (void)fprintf(out, "MNOTE %u [%s:%u:%u] %s\n",
                              (unsigned)N->severity,
                              N->source_file,
                              (unsigned)N->line,
                              (unsigned)N->col,
                              N->message);
            } else if (has_file) {
                (void)fprintf(out, "MNOTE %u [%s] %s\n",
                              (unsigned)N->severity,
                              N->source_file,
                              N->message);
            } else if (has_loc) {
                (void)fprintf(out, "MNOTE %u [%u:%u] %s\n",
                              (unsigned)N->severity,
                              (unsigned)N->line,
                              (unsigned)N->col,
                              N->message);
            } else {
                (void)fprintf(out, "MNOTE %u %s\n",
                              (unsigned)N->severity,
                              N->message);
            }
        }
    }
}

/* ----- Exit code -------------------------------------------------- */

uint32_t mnote_exit_code(const mnote_log_t *L)
{
    assert(L != NULL);
    return L->max_severity;
}

int mnote_has_errors(const mnote_log_t *L)
{
    assert(L != NULL);
    return (L->max_severity >= (uint32_t)MNOTE_ERROR) ? 1 : 0;
}
