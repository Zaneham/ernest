#ifndef ERNEST_OPT_H
#define ERNEST_OPT_H

#include "tir.h"
#include "mnote.h"

/*
 * Optimisation. The bit where the compiler reads what the user
 * actually asked for, notices that some of the gates are politely
 * undoing one another behind the scenes, and quietly leaves the
 * undoers out of the output. Same circuit at the end of it, with
 * less of a queue at the door.
 *
 * Passes live in a static table inside opt.c. The driver iterates
 * the table, runs each pass that qualifies for the current
 * optimisation level, and writes a mainframe-style job step for the
 * record. Adding a new pass is appending one row to the table; no
 * other file in the compiler needs to be touched, which is what
 * "extensible" was supposed to mean before the word got tired.
 *
 * Opt levels follow the GCC convention closely enough that nobody
 * needs to be told twice. Level zero is the --no-opt path. Level one
 * runs the safe and obviously-winning passes, which is what most
 * users want most of the time. Higher levels are reserved for the
 * cleverer transforms that pay off on the right circuit shapes.
 */

/* ----- Levels ----------------------------------------------------- */
#define OPT_LEVEL_NONE   0u   /* the --no-opt setting           */
#define OPT_LEVEL_BASIC  1u   /* default; safe and fast passes  */
#define OPT_LEVEL_AGGR   2u   /* future home of the clever lot  */

/* ----- Targets ---------------------------------------------------- */
/*
 * The hardware vendor whose native gate set the output should speak.
 * OPT_TARGET_GENERIC leaves the gate set unrestricted and is the
 * default; the rest tell the decomposition pass to rewrite every
 * non-native gate into a sequence the named vendor can run.
 *
 *   IBM = { RZ, SX, X, CX, ID }
 *
 * IonQ, Quantinuum, Rigetti each have their own. Adding one is
 * adding a new decomposition pass and a new enum value.
 */
typedef enum {
    OPT_TARGET_GENERIC = 0,
    OPT_TARGET_IBM     = 1
} opt_target_t;

/* ----- Bounds ----------------------------------------------------- */
#define OPT_MAX_PASSES        16u
#define OPT_GCAN_MAX_ITER     64u
#define OPT_FUSE_MAX_ITER     64u
#define OPT_FUSE_ZERO_EPS     1.0e-12

/* ----- Per-pass statistics --------------------------------------- */
/*
 * What each pass tells the driver about its run. Filled in by the
 * pass, read by the driver for the job-step output. Generous enough
 * to hold most things a pass might want to report, narrow enough that
 * the per-pass cost stays small.
 */
typedef struct {
    uint32_t insts_in;
    uint32_t insts_out;
    uint32_t insts_removed;
    uint32_t iterations;
} opt_stats_t;

/* ----- Pass shape ------------------------------------------------- */
/*
 * A pass is a function that takes a module, modifies it in place,
 * fills in its stats, and returns a mainframe-style return code.
 * Return zero unless something went sideways. Severity-eight return
 * codes propagate to the driver, which propagates them to the user.
 */
typedef int (*opt_pass_fn_t)(tir_module_t *M, opt_stats_t *stats,
                             mnote_log_t *log);

typedef struct {
    const char    *name;          /* mainframe-style module name, e.g. "OPTGCAN" */
    const char    *desc;          /* human description for the job-step line     */
    uint32_t       min_level;     /* runs when opt_level >= this                 */
    opt_target_t   target_only;   /* OPT_TARGET_GENERIC = run for any target    */
    opt_pass_fn_t  run;
} opt_pass_t;

/* ----- Inspection ------------------------------------------------- */
/*
 * Return the pass table and its length. Used by the optimiser's own
 * help text and by anyone curious about what the optimiser will do
 * before they hit Enter.
 */
const opt_pass_t *opt_passes(uint32_t *count);

/* ----- Driver ----------------------------------------------------- */
/*
 * Run every pass whose min_level <= opt_level and whose
 * target_only is either OPT_TARGET_GENERIC or matches the supplied
 * target. Modifies M in place. Prints mainframe-style job-step
 * output to stdout. Returns the worst return code seen across the
 * pass set, or zero if everything came home clean. Logs are
 * appended to the provided mnote_log_t for the caller to print at
 * its leisure.
 *
 * At opt_level == OPT_LEVEL_NONE the function does nothing, prints
 * nothing, and returns zero. This is the --no-opt path.
 */
int opt_run(tir_module_t *M, uint32_t opt_level,
            opt_target_t target, mnote_log_t *log);

/* ----- Individual passes (also callable directly for tests) ------- */
/*
 * Gate cancellation: walk the instruction stream, drop adjacent
 * pairs that undo each other. The simplest optimisation in the
 * catalogue and the one with the highest ratio of demonstration
 * value to lines of code.
 */
int opt_gate_cancellation(tir_module_t *M, opt_stats_t *stats,
                          mnote_log_t *log);

/*
 * Rotation fusion: where two same-axis rotations sit next to each
 * other on the same qubit, fold them into one with the angles added.
 * Catches the cancellation cases too, where the combined angle
 * comes out to zero modulo 2*pi. Handles RX, RY, RZ, and CP. The
 * single most impactful pass on variational ansatzes, which tend to
 * generate long ladders of parameterised rotations.
 */
int opt_rotation_fusion(tir_module_t *M, opt_stats_t *stats,
                        mnote_log_t *log);

/*
 * IBM decomposition. Walks the instruction stream and rewrites
 * every non-IBM-native gate as a sequence drawn from the IBM
 * basis: RZ, SX, X, CX. Each TIR gate has a known expansion in
 * those primitives, sourced from textbook quantum-computing
 * decompositions and verified against the simulator. The pass
 * runs only when target == OPT_TARGET_IBM.
 */
int opt_decompose_ibm(tir_module_t *M, opt_stats_t *stats,
                      mnote_log_t *log);

/*
 * The QASM linter. Walks the instruction stream looking for
 * structural problems and emits MNOTE diagnostics for each one.
 * Does not modify the module. Findings include: qubits declared
 * but never operated on, qubits operated on but never measured,
 * gates applied to a qubit after measurement without a reset,
 * measurements repeated on the same qubit, classical bits written
 * more than once, gates whose operands are the same qubit, and
 * circuits with no measurements at all.
 */
int opt_qlint(tir_module_t *M, opt_stats_t *stats, mnote_log_t *log);

#endif /* ERNEST_OPT_H */
