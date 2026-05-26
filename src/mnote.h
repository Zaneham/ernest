#ifndef ERNEST_MNOTE_H
#define ERNEST_MNOTE_H

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

/*
 * MNOTE is the way the HLASM assembler tells its operator something
 * has happened. It is also, by mild appropriation, the way Ernest
 * tells its operator the same. Every diagnostic the compiler raises,
 * from the lexer all the way down to the simulator, flows through
 * this one channel. The whole compiler speaks the same language to
 * its user, and that language has been spoken in machine rooms since
 * the Beatles were still touring.
 *
 * Severity follows the mainframe convention:
 *
 *   0   informational, no action needed
 *   4   warning, the work proceeded but may want a second look
 *   8   error, the work stopped at this step
 *   12  severe error, the work stopped and downstream is unsafe
 *   16  terminating error, the work was abandoned outright
 *
 * The maximum severity any module has seen so far is the return code
 * of the whole compiler when it exits. JES does the same thing on
 * z/OS; we are in good company.
 *
 * The diagnostic code is a small integer the module uses to identify
 * the specific complaint, separate from the human-readable message.
 * Useful for grep, useful for documentation, useful for the day
 * someone asks "what does ERNESTLX MNOTE 12 mean exactly".
 */

/* ----- Limits ----------------------------------------------------- */
#define MNOTE_MAX            (1u << 12)
#define MNOTE_MSG_LEN        256u
#define MNOTE_MODULE_LEN     16u
#define MNOTE_SRCFILE_LEN    128u

/* ----- Severity --------------------------------------------------- */
typedef enum {
    MNOTE_INFO   = 0,
    MNOTE_WARN   = 4,
    MNOTE_ERROR  = 8,
    MNOTE_SEVERE = 12,
    MNOTE_FATAL  = 16
} mnote_severity_t;

/* ----- One diagnostic --------------------------------------------- */
typedef struct {
    mnote_severity_t severity;
    uint32_t         code;
    uint32_t         line;
    uint32_t         col;
    char             source_file[MNOTE_SRCFILE_LEN];
    char             message[MNOTE_MSG_LEN];
} mnote_t;

/* ----- The log ---------------------------------------------------- */
/*
 * One log per compiler instance. Modules append to it; the driver
 * prints it. Fixed-size array, no malloc, fail loudly on overflow.
 */
typedef struct {
    char     module_name[MNOTE_MODULE_LEN];
    mnote_t  notes[MNOTE_MAX];
    uint32_t num_notes;
    uint32_t max_severity;
} mnote_log_t;

/* ----- API -------------------------------------------------------- */

/*
 * Initialise the log with the eight-character mainframe-style module
 * name. The name turns up in the printed diagnostics and gives the
 * reader a quick clue to which part of the compiler is talking.
 */
void mnote_init(mnote_log_t *L, const char *module_name);

/*
 * Append one diagnostic. Severity, numeric code, source location,
 * and a printf-style formatted message. The format string must
 * produce something shorter than MNOTE_MSG_LEN once expanded;
 * anything longer gets truncated with a trailing ellipsis.
 *
 * Pass source_file as NULL when the diagnostic isn't tied to a
 * specific input file (for example, internal compiler invariants).
 */
void mnote_emit(mnote_log_t *L, mnote_severity_t sev, uint32_t code,
                const char *source_file, uint32_t line, uint32_t col,
                const char *fmt, ...);

/*
 * Append an ABEND-style diagnostic. ABENDs are MNOTEs with a system
 * completion code attached, in the IBM convention: a four-character
 * code like S0C7 (system, data exception) or U0008 (user code 8).
 * Severity is always FATAL for ABENDs.
 */
void mnote_abend(mnote_log_t *L, const char *completion_code,
                 const char *source_file, uint32_t line, uint32_t col,
                 const char *fmt, ...);

/*
 * Print every diagnostic in the log to the given stream, mainframe-
 * style. The line shape is:
 *
 *   ERNESTLX MNOTE 8 [BELL.QASM:12:5] UNKNOWN CHARACTER '@'
 *
 * For ABENDs the leading word changes:
 *
 *   ERNESTSM ABEND S0C7 [BELL.QASM:18:9] DATA EXCEPTION - NaN AMPLITUDE
 */
void mnote_print(const mnote_log_t *L, FILE *out);

/*
 * Return the exit code the compiler should use, given the maximum
 * severity seen so far. Mainframe convention: pass the severity
 * straight through. RC 0, 4, 8, 12, or 16.
 */
uint32_t mnote_exit_code(const mnote_log_t *L);

/*
 * Convenience: did we see anything at error or worse? Useful for
 * COND= step-skipping in the compilation pipeline.
 */
int mnote_has_errors(const mnote_log_t *L);

#endif /* ERNEST_MNOTE_H */
