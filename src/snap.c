#include "snap.h"
#include "abend.h"

#include <math.h>
#include <string.h>
#include <assert.h>

/*
 * SNAP implementation. The capture path is meant to be cheap so
 * callers can sprinkle snap_take through the pipeline without
 * worrying about overhead. Heavy state is reduced to summaries:
 * the statevector contributes only its L2 norm and its largest
 * amplitude, the instruction stream contributes only counts and
 * the last few trace entries.
 */

/* ----- File-scope routing-mapping side channel ------------------ */
/*
 * The routing pass calls snap_set_mapping after each material
 * change so subsequent snaps include the current logical-to-
 * physical assignment. Stored in module-local state because the
 * snap struct is what gets archived, and stuffing the mapping
 * into every snap_take call site would be needlessly invasive.
 */

static uint32_t g_mapping[SNAP_MAPPING_LEN];
static uint32_t g_mapping_n;
static int      g_mapping_set;

void snap_set_mapping(const uint32_t *l2p, uint32_t n)
{
    if (l2p == NULL || n == 0u) {
        g_mapping_n = 0u;
        g_mapping_set = 0;
        return;
    }
    if (n > SNAP_MAPPING_LEN) n = SNAP_MAPPING_LEN;
    for (uint32_t i = 0u; i < n; i++) {
        g_mapping[i] = l2p[i];
    }
    g_mapping_n = n;
    g_mapping_set = 1;
}

/* ----- Helpers --------------------------------------------------- */

static void copy_truncated_snap(char *dst, uint32_t cap, const char *src)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    uint32_t n = (uint32_t)strlen(src);
    if (n + 1u > cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Compute statevector summary. Returns L2 norm and (via out
 * parameters) the largest |amp|^2 and the basis state where it
 * lives. Single pass, no allocations.
 */
static double sv_summary(const sim_state_t *S,
                         double *max_mag2_out, uint32_t *max_basis_out)
{
    assert(S != NULL);
    assert(max_mag2_out != NULL);
    assert(max_basis_out != NULL);

    double norm2 = 0.0;
    double best  = 0.0;
    uint32_t best_idx = 0u;
    uint32_t dim = 1u << S->num_qubits;
    for (uint32_t i = 0u; i < dim; i++) {
        double re = S->state[i].re;
        double im = S->state[i].im;
        double m2 = re * re + im * im;
        norm2 += m2;
        if (m2 > best) {
            best = m2;
            best_idx = i;
        }
    }
    *max_mag2_out = best;
    *max_basis_out = best_idx;
    return sqrt(norm2);
}

/* ----- API ------------------------------------------------------- */

void snap_log_init(snap_log_t *L)
{
    assert(L != NULL);
    memset(L, 0, sizeof *L);
    L->enabled = 0u;
}

void snap_log_enable(snap_log_t *L, int enabled)
{
    assert(L != NULL);
    L->enabled = enabled ? 1u : 0u;
}

void snap_take(snap_log_t *L, const char *label,
               const tir_module_t *M, const sim_state_t *S)
{
    if (L == NULL || L->enabled == 0u) {
        return;
    }
    if (L->num_entries >= SNAP_MAX_ENTRIES) {
        return;  /* log full; drop silently */
    }

    snap_t *N = &L->entries[L->num_entries++];
    memset(N, 0, sizeof *N);
    N->seq = L->seq_counter++;
    copy_truncated_snap(N->label, SNAP_LABEL_LEN, label);
    copy_truncated_snap(N->stage, SNAP_STAGE_LEN, abend_current_stage());
    copy_truncated_snap(N->sub,   SNAP_SUB_LEN,   abend_current_sub());

    if (M != NULL) {
        N->num_insts  = M->num_insts;
        N->num_qregs  = M->num_qregs;
        N->num_cregs  = M->num_cregs;
        N->num_angles = M->num_angles;
    }

    if (S != NULL && S->num_qubits > 0u) {
        N->has_sim = 1u;
        N->inst_idx = S->current_inst;
        N->sim_num_qubits = S->num_qubits;
        N->sim_num_bits   = S->num_bits;
        double max_mag2 = 0.0;
        uint32_t max_basis = 0u;
        N->sv_norm = sv_summary(S, &max_mag2, &max_basis);
        N->sv_max_mag2 = max_mag2;
        N->sv_max_basis = max_basis;

        /* Trace: last SNAP_TRACE_LEN entries from the simulator's
         * ring buffer. */
        uint32_t avail = (S->trace_count < (uint32_t)SNAP_TRACE_LEN)
                         ? S->trace_count
                         : (uint32_t)SNAP_TRACE_LEN;
        if (S->trace_count >= (uint32_t)ERNEST_SIM_TRACE_LEN) {
            /* Ring has wrapped; the latest entries are the ones
             * just before head, accounting for buffer length. */
            uint32_t head = S->trace_head;
            uint32_t buf_len = (uint32_t)ERNEST_SIM_TRACE_LEN;
            for (uint32_t k = 0u; k < avail; k++) {
                uint32_t pos = (head + buf_len - avail + k) % buf_len;
                N->trace[k] = S->trace_buf[pos];
            }
        } else {
            for (uint32_t k = 0u; k < avail; k++) {
                N->trace[k] = S->trace_buf[k];
            }
        }
        N->trace_n = avail;
    }

    if (g_mapping_set && g_mapping_n > 0u) {
        N->has_mapping = 1u;
        N->mapping_n = g_mapping_n;
        for (uint32_t i = 0u; i < g_mapping_n; i++) {
            N->mapping[i] = g_mapping[i];
        }
    }
}

/* ----- Output ---------------------------------------------------- */

static void dump_one(const snap_t *N, FILE *out)
{
    (void)fprintf(out,
        "ERNESTSN SNAP %u  %s\n",
        (unsigned)N->seq, N->label[0] ? N->label : "(no label)");

    if (N->stage[0] != '\0') {
        if (N->sub[0] != '\0') {
            (void)fprintf(out, "ERNESTSN   STAGE   %s / %s\n",
                          N->stage, N->sub);
        } else {
            (void)fprintf(out, "ERNESTSN   STAGE   %s\n", N->stage);
        }
    }

    (void)fprintf(out,
        "ERNESTSN   MODULE  insts=%u qregs=%u cregs=%u angles=%u\n",
        (unsigned)N->num_insts, (unsigned)N->num_qregs,
        (unsigned)N->num_cregs, (unsigned)N->num_angles);

    if (N->has_sim) {
        double drift = N->sv_norm - 1.0;
        if (drift < 0.0) drift = -drift;
        (void)fprintf(out,
            "ERNESTSN   SIM     qubits=%u bits=%u inst=%u  "
            "norm=%.9f drift=%.3e\n",
            (unsigned)N->sim_num_qubits, (unsigned)N->sim_num_bits,
            (unsigned)N->inst_idx, N->sv_norm, drift);
        (void)fprintf(out,
            "ERNESTSN   PEAK    |%u> mag2=%.6f\n",
            (unsigned)N->sv_max_basis, N->sv_max_mag2);
    }

    if (N->trace_n > 0u) {
        (void)fprintf(out, "ERNESTSN   TRACE   ");
        for (uint32_t k = 0u; k < N->trace_n; k++) {
            (void)fprintf(out, "%u%s", (unsigned)N->trace[k],
                          (k + 1u < N->trace_n) ? "," : "");
        }
        (void)fputc('\n', out);
    }

    if (N->has_mapping) {
        (void)fprintf(out, "ERNESTSN   MAP     ");
        for (uint32_t k = 0u; k < N->mapping_n; k++) {
            (void)fprintf(out, "%u:%u%s",
                          (unsigned)k, (unsigned)N->mapping[k],
                          (k + 1u < N->mapping_n) ? " " : "");
        }
        (void)fputc('\n', out);
    }
}

void snap_log_dump(const snap_log_t *L, FILE *out)
{
    assert(L != NULL);
    assert(out != NULL);

    (void)fprintf(out, "\n");
    (void)fprintf(out, "ERNESTSN SNAP DUMP -- %u entries\n",
                  (unsigned)L->num_entries);
    (void)fprintf(out,
        "================================================================\n");

    for (uint32_t i = 0u; i < L->num_entries; i++) {
        dump_one(&L->entries[i], out);
    }

    (void)fprintf(out,
        "================================================================\n");
    (void)fprintf(out, "ERNESTSN END  total=%u\n",
                  (unsigned)L->num_entries);
}

int snap_log_write_file(const snap_log_t *L, const char *path)
{
    assert(L != NULL);
    assert(path != NULL);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return 1;
    }
    snap_log_dump(L, f);
    (void)fclose(f);
    return 0;
}

/* ----- Active-log convenience ------------------------------------ */

static snap_log_t *g_active_log = NULL;

void snap_set_active_log(snap_log_t *L)
{
    g_active_log = L;
}

snap_log_t *snap_get_active_log(void)
{
    return g_active_log;
}

void snap_active(const char *label,
                 const tir_module_t *M, const sim_state_t *S)
{
    if (g_active_log != NULL) {
        snap_take(g_active_log, label, M, S);
    }
}
