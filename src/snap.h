#ifndef ERNEST_SNAP_H
#define ERNEST_SNAP_H

#include "tir.h"
#include "sim.h"
#include <stdio.h>
#include <stdint.h>

/*
 * MVS-style SNAP. Non-fatal diagnostic snapshots taken at chosen
 * points during compilation or simulation. Unlike ABEND, a SNAP
 * does not terminate anything; it captures state into a log so the
 * operator can read what the compiler thought was happening at the
 * time.
 *
 * BarraCUDA's ab_snag() informs the shape here. Snaps are
 * lightweight: a small fixed-size struct per snapshot, with the
 * heavy state (statevector, instruction stream) reduced to
 * summaries before being recorded. A typical run might take dozens
 * of snaps; the log buffer is sized accordingly.
 *
 * Each snap inherits its stage/sub context from the abend module's
 * current stage tracking, so a snap taken during the OPTFUSE pass
 * naturally records "OPT / OPTFUSE" without the caller having to
 * pass either string explicitly.
 *
 * Conventional places to take snaps in Ernest:
 *   - Before and after each pass in opt_run
 *   - After SABRE routing settles
 *   - Mid-simulation at user-chosen instruction indices
 *   - On non-fatal failures (parse RC >= 8, lex RC >= 8, etc.)
 *
 * The log is dumped at the end of the run via snap_log_dump or
 * snap_log_write_file. The operator sees a chronological journal
 * of what the compiler was doing.
 */

#define SNAP_LABEL_LEN     48u
#define SNAP_STAGE_LEN     16u
#define SNAP_SUB_LEN       32u
#define SNAP_MAX_ENTRIES   4096u
#define SNAP_TRACE_LEN     8u
#define SNAP_MAPPING_LEN   32u

/* ----- A single snapshot ----------------------------------------- */

typedef struct {
    uint32_t seq;                          /* monotonic sequence number */
    char     label[SNAP_LABEL_LEN];
    char     stage[SNAP_STAGE_LEN];
    char     sub  [SNAP_SUB_LEN];

    /* Module summary. */
    uint32_t num_insts;
    uint32_t num_qregs;
    uint32_t num_cregs;
    uint32_t num_angles;

    /* Sim summary. Zero when no sim state was supplied. */
    uint32_t inst_idx;                     /* current/last executed inst */
    uint32_t sim_num_qubits;
    uint32_t sim_num_bits;
    double   sv_norm;
    double   sv_max_mag2;                  /* max |amp|^2 */
    uint32_t sv_max_basis;                 /* basis state with max |amp|^2 */
    uint8_t  has_sim;

    /* Recent instruction trace (small). */
    uint32_t trace[SNAP_TRACE_LEN];
    uint32_t trace_n;

    /* Logical-to-physical mapping snapshot (routing). Zero when no
     * routing context is set. */
    uint32_t mapping[SNAP_MAPPING_LEN];
    uint32_t mapping_n;
    uint8_t  has_mapping;
} snap_t;

/* ----- A log of snapshots --------------------------------------- */

typedef struct {
    snap_t   entries[SNAP_MAX_ENTRIES];
    uint32_t num_entries;
    uint32_t seq_counter;
    uint8_t  enabled;
} snap_log_t;

/* ----- API ------------------------------------------------------- */

/*
 * Initialise the log. Sets enabled to zero by default so an
 * uninitialised log is also a disabled log. Callers turn it on with
 * snap_log_enable.
 */
void snap_log_init(snap_log_t *L);

/*
 * Enable or disable the log. When disabled, snap_take is a no-op,
 * which lets the rest of the compiler call snap_take freely from
 * the pipeline without worrying about the overhead in production
 * runs.
 */
void snap_log_enable(snap_log_t *L, int enabled);

/*
 * Set a logical-to-physical mapping that snap_take should record
 * with subsequent snapshots. Used by the routing pass to make
 * its mapping changes visible in the snap log. Pass NULL or n=0 to
 * clear.
 */
void snap_set_mapping(const uint32_t *l2p, uint32_t n);

/*
 * Take a snapshot. Reads the abend module's current stage/sub for
 * context, snapshots the module summary, and (if S is non-NULL)
 * snapshots a summary of the simulator state. The label is
 * appended to the log along with all the captured fields.
 *
 * Safe to call when the log is disabled; the function is a no-op
 * in that case and consequently safe to call from anywhere in the
 * pipeline.
 */
void snap_take(snap_log_t *L, const char *label,
               const tir_module_t *M, const sim_state_t *S);

/*
 * Print the full log to a stream in MVS-style format. Each
 * snapshot occupies a small block of lines with header, module
 * summary, simulator summary (if present), trace, and mapping
 * (if present).
 */
void snap_log_dump(const snap_log_t *L, FILE *out);

/*
 * Convenience wrapper that opens a file, writes the dump, closes
 * the file. Returns 0 on success, non-zero on I/O error.
 */
int snap_log_write_file(const snap_log_t *L, const char *path);

/* ----- Active-log convenience ------------------------------------ */
/*
 * Pipeline code (opt_run, routing pass, simulator, etc.) wants to
 * take snaps without threading a snap_log_t pointer through every
 * function. We provide a file-scope "active log" that snap_active
 * uses. The top-level driver sets this once at startup; deeper
 * code calls snap_active with just a label and the available
 * module/sim state.
 *
 * If no active log is set, snap_active is a no-op, which makes it
 * safe to scatter through performance-sensitive paths.
 */
void        snap_set_active_log(snap_log_t *L);
snap_log_t *snap_get_active_log(void);
void        snap_active(const char *label,
                        const tir_module_t *M, const sim_state_t *S);

#endif /* ERNEST_SNAP_H */
