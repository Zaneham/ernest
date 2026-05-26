#include "route.h"
#include "snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

/*
 * SABRE qubit routing.  Citation: Li, Ding, & Xie (2019).  Full
 * reference in REFERENCES.md.  The algorithm walks a DAG of multi-
 * qubit gates, executing any whose operands are physically
 * adjacent and inserting SWAPs whenever no executable gate is
 * available.  Each SWAP is scored against a heuristic combining
 * the current front layer with a lookahead over the next few
 * gates, and a small per-qubit decay term that prevents the
 * algorithm from oscillating between equivalent SWAPs.
 *
 * v1 runs forward only with the identity initial mapping.  At the
 * end of routing we append SWAPs to restore the identity mapping
 * so the final logical-to-physical correspondence matches the
 * input and the existing verifier can compare statevectors
 * directly.
 */

/* ---------------------------------------------------------------- */
/* Coupling graphs                                                  */
/* ---------------------------------------------------------------- */

/*
 * Build a coupling graph at static-initialiser time from an edge
 * list.  C99 designated initialisers handle this cleanly for the
 * small graphs we ship; the redundancy of declaring each edge
 * twice (a-b and b-a) is the price of using a symmetric adjacency
 * matrix and being able to ask "is i adjacent to j" with one
 * lookup later.
 */

/*
 * Build a coupling graph by listing its edges. The adjacency matrix
 * is large enough at ROUTE_MAX_QUBITS = 128 that writing it by hand
 * for anything bigger than a handful of qubits is impractical, so
 * we use small builder functions for the larger topologies.
 */
static void coupling_clear(coupling_graph_t *G, const char *name, uint32_t n)
{
    G->name = name;
    G->num_qubits = n;
    for (uint32_t i = 0u; i < ROUTE_MAX_QUBITS; i++) {
        for (uint32_t j = 0u; j < ROUTE_MAX_QUBITS; j++) {
            G->adj[i][j] = 0u;
        }
    }
}

static void coupling_add_edge(coupling_graph_t *G, uint32_t a, uint32_t b)
{
    if (a < ROUTE_MAX_QUBITS && b < ROUTE_MAX_QUBITS && a != b) {
        G->adj[a][b] = 1u;
        G->adj[b][a] = 1u;
    }
}

/*
 * Static instances of every built-in graph. Initialised lazily on
 * first lookup via build_X_once() helpers so we don't need to
 * write 128-by-128 designated initialisers by hand.
 */
static coupling_graph_t g_linear_5;
static coupling_graph_t g_linear_16;
static coupling_graph_t g_ring_5;
static coupling_graph_t g_ring_8;
static coupling_graph_t g_grid_4x4;
static coupling_graph_t g_heavy_hex_7;
static coupling_graph_t g_ibm_falcon_5_t;
static coupling_graph_t g_full_16;
static int g_initialised = 0;

static void build_all_topologies_once(void)
{
    if (g_initialised) {
        return;
    }
    g_initialised = 1;

    /* linear_5: 0-1-2-3-4 */
    coupling_clear(&g_linear_5, "linear_5", 5u);
    for (uint32_t i = 0u; i + 1u < 5u; i++) {
        coupling_add_edge(&g_linear_5, i, i + 1u);
    }

    /* linear_16: 0-1-2-...-15 */
    coupling_clear(&g_linear_16, "linear_16", 16u);
    for (uint32_t i = 0u; i + 1u < 16u; i++) {
        coupling_add_edge(&g_linear_16, i, i + 1u);
    }

    /* ring_5: 0-1-2-3-4-0 */
    coupling_clear(&g_ring_5, "ring_5", 5u);
    for (uint32_t i = 0u; i < 5u; i++) {
        coupling_add_edge(&g_ring_5, i, (i + 1u) % 5u);
    }

    /* ring_8: 0-1-2-...-7-0 */
    coupling_clear(&g_ring_8, "ring_8", 8u);
    for (uint32_t i = 0u; i < 8u; i++) {
        coupling_add_edge(&g_ring_8, i, (i + 1u) % 8u);
    }

    /* grid_4x4: 16 qubits in a square grid, indexed row-major.
     * Each qubit couples to its horizontal and vertical neighbours.
     *
     *   0  - 1  - 2  - 3
     *   |    |    |    |
     *   4  - 5  - 6  - 7
     *   |    |    |    |
     *   8  - 9  - 10 - 11
     *   |    |    |    |
     *   12 - 13 - 14 - 15
     */
    coupling_clear(&g_grid_4x4, "grid_4x4", 16u);
    for (uint32_t r = 0u; r < 4u; r++) {
        for (uint32_t c = 0u; c < 4u; c++) {
            uint32_t idx = r * 4u + c;
            if (c + 1u < 4u) coupling_add_edge(&g_grid_4x4, idx, idx + 1u);
            if (r + 1u < 4u) coupling_add_edge(&g_grid_4x4, idx, idx + 4u);
        }
    }

    /* heavy_hex_7: mirrors an IBM Falcon r4 sub-graph (ibm_lagos and
     * similar). Edges (0,1), (1,2), (2,3), (2,4), (3,5), (5,6).
     *
     *      0
     *      |
     *      1 - 2 - 3 - 5 - 6
     *          |
     *          4
     */
    coupling_clear(&g_heavy_hex_7, "heavy_hex_7", 7u);
    coupling_add_edge(&g_heavy_hex_7, 0u, 1u);
    coupling_add_edge(&g_heavy_hex_7, 1u, 2u);
    coupling_add_edge(&g_heavy_hex_7, 2u, 3u);
    coupling_add_edge(&g_heavy_hex_7, 2u, 4u);
    coupling_add_edge(&g_heavy_hex_7, 3u, 5u);
    coupling_add_edge(&g_heavy_hex_7, 5u, 6u);

    /* ibm_falcon_5_t: the T-shape used by retired Falcon r5 devices
     * like ibm_quito, ibm_belem, ibm_lima. Edges (0,1), (1,2), (1,3),
     * (3,4).
     *
     *   0 - 1 - 2
     *       |
     *       3
     *       |
     *       4
     */
    coupling_clear(&g_ibm_falcon_5_t, "ibm_falcon_5_t", 5u);
    coupling_add_edge(&g_ibm_falcon_5_t, 0u, 1u);
    coupling_add_edge(&g_ibm_falcon_5_t, 1u, 2u);
    coupling_add_edge(&g_ibm_falcon_5_t, 1u, 3u);
    coupling_add_edge(&g_ibm_falcon_5_t, 3u, 4u);

    /* full_16: every pair of qubits connected. Routing on this is
     * a no-op for any input; useful as a baseline. */
    coupling_clear(&g_full_16, "full_16", 16u);
    for (uint32_t i = 0u; i < 16u; i++) {
        for (uint32_t j = i + 1u; j < 16u; j++) {
            coupling_add_edge(&g_full_16, i, j);
        }
    }
}

/*
 * Case-insensitive comparison with hyphen and underscore folded.
 * Lets users type --coupling=heavy-hex-7 or --coupling=Heavy_Hex_7
 * and get the same result.
 */
static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca == '-') ca = '_';
        if (cb == '-') cb = '_';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

const coupling_graph_t *route_lookup_coupling(const char *name)
{
    if (name == NULL) return NULL;
    build_all_topologies_once();
    if (name_eq(name, "linear_5"))        return &g_linear_5;
    if (name_eq(name, "linear_16"))       return &g_linear_16;
    if (name_eq(name, "ring_5"))          return &g_ring_5;
    if (name_eq(name, "ring_8"))          return &g_ring_8;
    if (name_eq(name, "grid_4x4"))        return &g_grid_4x4;
    if (name_eq(name, "heavy_hex_7"))     return &g_heavy_hex_7;
    if (name_eq(name, "ibm_falcon_5_t"))  return &g_ibm_falcon_5_t;
    if (name_eq(name, "full_16"))         return &g_full_16;
    return NULL;
}

/* ---------------------------------------------------------------- */
/* File-based coupling loader                                       */
/* ---------------------------------------------------------------- */

static coupling_graph_t g_file_loaded;
static char             g_file_loaded_name[64];

const coupling_graph_t *route_load_coupling_from_file(const char *path)
{
    if (path == NULL) return NULL;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }

    /* Stash a short version of the path as the graph's name so the
     * job-step output has something meaningful to print. We take
     * the basename in case the path is long. */
    const char *basename = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') basename = p + 1;
    }
    uint32_t bn = 0u;
    while (basename[bn] != '\0' && bn + 1u < (uint32_t)sizeof g_file_loaded_name) {
        g_file_loaded_name[bn] = basename[bn];
        bn++;
    }
    g_file_loaded_name[bn] = '\0';

    coupling_clear(&g_file_loaded, g_file_loaded_name, 0u);

    uint32_t max_q = 0u;
    char line[256];
    while (fgets(line, (int)sizeof line, f) != NULL) {
        /* Skip blank lines and #-comments. */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;

        /* Parse "u v". */
        unsigned u, v;
        if (sscanf(p, "%u %u", &u, &v) != 2) continue;
        if (u >= ROUTE_MAX_QUBITS || v >= ROUTE_MAX_QUBITS) continue;
        coupling_add_edge(&g_file_loaded, u, v);
        if (u > max_q) max_q = u;
        if (v > max_q) max_q = v;
    }
    (void)fclose(f);

    g_file_loaded.num_qubits = max_q + 1u;
    return &g_file_loaded;
}

/* ---------------------------------------------------------------- */
/* Driver-side configuration                                        */
/* ---------------------------------------------------------------- */

static const coupling_graph_t *active_coupling = NULL;

void route_set_active_coupling(const coupling_graph_t *G)
{
    active_coupling = G;
}

const coupling_graph_t *route_get_active_coupling(void)
{
    return active_coupling;
}

/* ---------------------------------------------------------------- */
/* Distance matrix (Floyd-Warshall)                                 */
/* ---------------------------------------------------------------- */

/*
 * For small graphs (n <= 32) Floyd-Warshall is fine: O(n^3) is
 * 32768 operations, fast and obviously correct.  BFS per source
 * would be O(n * (n + e)) which is slightly faster but the
 * difference is meaningless at this size and the algorithm is
 * less obviously right.
 */
#define ROUTE_INF 0xFFu

static uint8_t route_dist[ROUTE_MAX_QUBITS][ROUTE_MAX_QUBITS];

static void compute_distances(const coupling_graph_t *G)
{
    uint32_t n = G->num_qubits;
    for (uint32_t i = 0u; i < ROUTE_MAX_QUBITS; i++) {
        for (uint32_t j = 0u; j < ROUTE_MAX_QUBITS; j++) {
            if (i == j)               route_dist[i][j] = 0u;
            else if (i < n && j < n && G->adj[i][j]) route_dist[i][j] = 1u;
            else                      route_dist[i][j] = ROUTE_INF;
        }
    }
    for (uint32_t k = 0u; k < n; k++) {
        for (uint32_t i = 0u; i < n; i++) {
            for (uint32_t j = 0u; j < n; j++) {
                uint32_t via = (uint32_t)route_dist[i][k] +
                               (uint32_t)route_dist[k][j];
                if (via < (uint32_t)route_dist[i][j]) {
                    if (via > 254u) via = ROUTE_INF;
                    route_dist[i][j] = (uint8_t)via;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* DAG construction                                                 */
/* ---------------------------------------------------------------- */

/*
 * A SABRE DAG node is one TIR instruction with information about
 * which qubits it touches and which earlier instructions it depends
 * on.  Predecessors are the most recent earlier instructions to
 * touch any of this instruction's qubits.  We cap predecessors at
 * 4, which covers every gate in our TIR (the widest is CCX with 3
 * qubits, so at most 3 predecessors).
 */
typedef struct {
    uint16_t inst_idx;       /* index into the input module's insts[] */
    uint8_t  num_qubits;
    uint8_t  num_preds;
    uint8_t  num_unmet_preds;
    uint8_t  done;           /* set to 1 once emitted */
    uint16_t qubits[4];      /* flat qubit indices */
    uint16_t preds[4];
} sabre_node_t;

#define SABRE_MAX_NODES TIR_MAX_INSTS
static sabre_node_t sabre_nodes[SABRE_MAX_NODES];
static uint32_t     sabre_num_nodes;

/* For each qubit, the most recent node that touched it. Used during
 * DAG build to wire up predecessors.  UINT16_MAX = "no previous". */
static uint16_t sabre_last_on[ROUTE_MAX_QUBITS];

/*
 * Decode a TIR operand list into a flat qubit list.  Returns the
 * number of qubit operands; for measurement and reset only the
 * qubit operand counts (the classical-bit operand is ignored from
 * a routing perspective because measurement doesn't constrain
 * coupling).
 */
static uint32_t decode_qubits(const tir_inst_t *I, uint16_t out[4])
{
    switch ((tir_op_t)I->op) {
    case TIR_QREG_DECL:
    case TIR_CREG_DECL:
        return 0u;
    case TIR_GATE_H:   case TIR_GATE_X:   case TIR_GATE_Y:   case TIR_GATE_Z:
    case TIR_GATE_S:   case TIR_GATE_T:   case TIR_GATE_SDG: case TIR_GATE_TDG:
    case TIR_GATE_SX:
    case TIR_GATE_RX:  case TIR_GATE_RY:  case TIR_GATE_RZ:
    case TIR_RESET:
        out[0] = (uint16_t)TIR_REF_IDX(I->operands[0]);
        return 1u;
    case TIR_GATE_CX:  case TIR_GATE_CZ:  case TIR_GATE_CH:
    case TIR_GATE_SWAP:
    case TIR_GATE_CP:
        out[0] = (uint16_t)TIR_REF_IDX(I->operands[0]);
        out[1] = (uint16_t)TIR_REF_IDX(I->operands[1]);
        return 2u;
    case TIR_GATE_CCX:
        out[0] = (uint16_t)TIR_REF_IDX(I->operands[0]);
        out[1] = (uint16_t)TIR_REF_IDX(I->operands[1]);
        out[2] = (uint16_t)TIR_REF_IDX(I->operands[2]);
        return 3u;
    case TIR_MEASURE:
        out[0] = (uint16_t)TIR_REF_IDX(I->operands[0]);
        return 1u;
    case TIR_OP_COUNT:
    default:
        return 0u;
    }
}

/*
 * Build the dependency DAG over the input module's instructions.
 * Register declarations are passed through later; we only build
 * nodes for actual gates.
 */
static void build_dag(const tir_module_t *M)
{
    sabre_num_nodes = 0u;
    for (uint32_t i = 0u; i < ROUTE_MAX_QUBITS; i++) {
        sabre_last_on[i] = (uint16_t)0xFFFFu;
    }

    uint32_t n = M->num_insts;
    for (uint32_t i = 0u; i < n; i++) {
        const tir_inst_t *I = &M->insts[i];
        uint16_t qs[4];
        uint32_t nq = decode_qubits(I, qs);
        if (nq == 0u) {
            continue;   /* declarations skip the DAG */
        }
        sabre_node_t *N = &sabre_nodes[sabre_num_nodes++];
        N->inst_idx = (uint16_t)i;
        N->num_qubits = (uint8_t)nq;
        N->num_preds = 0u;
        N->done = 0u;
        for (uint32_t k = 0u; k < nq; k++) {
            N->qubits[k] = qs[k];
            uint16_t prev = sabre_last_on[qs[k]];
            if (prev != (uint16_t)0xFFFFu) {
                /* Avoid double-adding the same predecessor when a
                 * gate touches two qubits both last-touched by the
                 * same node. */
                int already = 0;
                for (uint32_t p = 0u; p < N->num_preds; p++) {
                    if (N->preds[p] == prev) { already = 1; break; }
                }
                if (!already && N->num_preds < 4u) {
                    N->preds[N->num_preds++] = prev;
                }
            }
            sabre_last_on[qs[k]] = (uint16_t)(sabre_num_nodes - 1u);
        }
        N->num_unmet_preds = N->num_preds;
    }
}

/* ---------------------------------------------------------------- */
/* Mapping and output                                               */
/* ---------------------------------------------------------------- */

/* l2p[logical] = physical, p2l is the inverse. */
static uint32_t route_l2p[ROUTE_MAX_QUBITS];
static uint32_t route_p2l[ROUTE_MAX_QUBITS];
static double   route_decay[ROUTE_MAX_QUBITS];

/* Output scratch.  Same idea as opt_decompose_ibm: build into a
 * static buffer, copy back at the end. */
static tir_inst_t route_scratch[TIR_MAX_INSTS];
static uint32_t   route_scratch_count;

static uint32_t  route_swap_count;

static void init_identity_mapping(uint32_t n)
{
    for (uint32_t i = 0u; i < ROUTE_MAX_QUBITS; i++) {
        if (i < n) {
            route_l2p[i] = i;
            route_p2l[i] = i;
        } else {
            route_l2p[i] = (uint32_t)0xFFFFFFFFu;
            route_p2l[i] = (uint32_t)0xFFFFFFFFu;
        }
        route_decay[i] = 1.0;
    }
}

/*
 * Emit a copy of an input instruction into the scratch buffer with
 * its operand qubits remapped through the current logical-to-
 * physical mapping.  The output's qubit indices are physical
 * positions, which is the form hardware vendors expect.
 *
 * Register declarations pass through unchanged because they
 * declare the size of the qreg in physical-qubit space, which is
 * the same as logical-qubit space (we use one register per
 * compilation and the routing pass does not change the qreg's
 * size).
 */
static void emit_remapped(const tir_inst_t *src)
{
    assert(route_scratch_count < (uint32_t)TIR_MAX_INSTS);
    tir_inst_t *D = &route_scratch[route_scratch_count++];
    *D = *src;
    /* Walk operands; for any operand that's a qubit ref, remap. */
    uint16_t qs[4];
    uint32_t nq = decode_qubits(src, qs);
    if (nq > 0u && (tir_op_t)src->op != TIR_QREG_DECL
                && (tir_op_t)src->op != TIR_CREG_DECL) {
        /* Determine which TIR operand slots hold qubits.  For
         * MEASURE that's operand 0 only; for CP it's 0 and 1; etc.
         * The same opcode-driven dispatch as decode_qubits. */
        switch ((tir_op_t)src->op) {
        case TIR_MEASURE:
            D->operands[0] = (src->operands[0] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[0])];
            /* operand[1] is the classical bit; leave it alone. */
            break;
        case TIR_GATE_CCX:
            D->operands[0] = (src->operands[0] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[0])];
            D->operands[1] = (src->operands[1] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[1])];
            D->operands[2] = (src->operands[2] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[2])];
            break;
        case TIR_GATE_CP:
            D->operands[0] = (src->operands[0] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[0])];
            D->operands[1] = (src->operands[1] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[1])];
            /* operand[2] is the angle index; leave it. */
            break;
        case TIR_GATE_CX:  case TIR_GATE_CZ:  case TIR_GATE_CH:
        case TIR_GATE_SWAP:
            D->operands[0] = (src->operands[0] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[0])];
            D->operands[1] = (src->operands[1] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[1])];
            break;
        default:
            /* One-qubit gates and rotations: operand[0] is the
             * qubit, anything else (angle index) stays put. */
            D->operands[0] = (src->operands[0] & 0xFFFF0000u)
                           | route_l2p[TIR_REF_IDX(src->operands[0])];
            break;
        }
    }
}

/*
 * Emit a SWAP between two physical qubits.  Reuses the existing
 * SWAP opcode; the operands are physical-qubit refs.  Updates the
 * mapping arrays and the decay table to reflect the swap.  Uses
 * the qreg index from the first operand of the source TIR so the
 * emitted SWAP refers to the same register the rest of the module
 * does.
 */
static void emit_swap(uint32_t phys_a, uint32_t phys_b, uint32_t qreg_idx,
                      uint32_t src_line, uint32_t src_col)
{
    assert(route_scratch_count < (uint32_t)TIR_MAX_INSTS);
    tir_inst_t *D = &route_scratch[route_scratch_count++];
    D->op = (uint16_t)TIR_GATE_SWAP;
    D->num_operands = 2u;
    D->subop = 0u;
    D->operands[0] = TIR_REF(qreg_idx, phys_a);
    D->operands[1] = TIR_REF(qreg_idx, phys_b);
    D->operands[2] = 0u;
    D->operands[3] = 0u;
    D->src_line = src_line;
    D->src_col  = src_col;
    D->origin_pass = (uint16_t)TIR_ORIGIN_ROUTE;
    D->reserved = 0u;

    /* Update mapping: whichever logicals were at phys_a and phys_b
     * are now swapped. */
    uint32_t la = route_p2l[phys_a];
    uint32_t lb = route_p2l[phys_b];
    if (la != (uint32_t)0xFFFFFFFFu) route_l2p[la] = phys_b;
    if (lb != (uint32_t)0xFFFFFFFFu) route_l2p[lb] = phys_a;
    route_p2l[phys_a] = lb;
    route_p2l[phys_b] = la;

    route_swap_count++;
}

/* ---------------------------------------------------------------- */
/* SABRE main loop                                                  */
/* ---------------------------------------------------------------- */

/*
 * A 2q gate is "executable" if its two operand logical qubits are
 * physically adjacent.  1q gates and measurements have no coupling
 * constraint and are always executable.  CCX is treated as
 * non-executable here; the optimiser pipeline normally decomposes
 * CCX to 2q gates before this pass runs.
 */
static bool node_executable(const sabre_node_t *N,
                            const coupling_graph_t *G)
{
    if (N->num_qubits <= 1u) {
        return true;
    }
    if (N->num_qubits == 2u) {
        uint32_t pa = route_l2p[N->qubits[0]];
        uint32_t pb = route_l2p[N->qubits[1]];
        return G->adj[pa][pb] != 0u;
    }
    /* CCX or wider: cannot route here. */
    return false;
}

/*
 * Compute the heuristic cost of a layer of gates: the sum of
 * distances between operand pairs of two-qubit gates under the
 * current mapping.  Normalised by the number of contributing gates
 * so layers of different sizes are comparable.
 */
static double layer_cost(const sabre_node_t *nodes,
                         const uint32_t *layer, uint32_t layer_n)
{
    if (layer_n == 0u) return 0.0;
    double total = 0.0;
    uint32_t counted = 0u;
    for (uint32_t i = 0u; i < layer_n; i++) {
        const sabre_node_t *N = &nodes[layer[i]];
        if (N->num_qubits == 2u) {
            uint32_t pa = route_l2p[N->qubits[0]];
            uint32_t pb = route_l2p[N->qubits[1]];
            total += (double)route_dist[pa][pb];
            counted++;
        }
    }
    if (counted == 0u) return 0.0;
    return total / (double)counted;
}

/*
 * Compute the extended set E: a small look-ahead window of gates
 * whose dependencies are not all met yet but which are close in
 * the DAG.  SABRE uses this so the routing decisions account for
 * imminent rather than only current pressure.
 *
 * Implementation: walk successors of the front layer's nodes for a
 * few steps, accumulating up to E_MAX nodes.  Stops at the cap or
 * when no more reachable nodes remain.
 */
#define EXT_SET_MAX 20u

static uint32_t build_extended_set(const sabre_node_t *nodes,
                                   uint32_t num_nodes,
                                   const uint32_t *front, uint32_t front_n,
                                   uint32_t *out)
{
    /* Mark current front so we don't re-add. */
    static uint8_t in_set[SABRE_MAX_NODES];
    for (uint32_t i = 0u; i < num_nodes; i++) in_set[i] = 0u;
    for (uint32_t i = 0u; i < front_n; i++)   in_set[front[i]] = 1u;

    uint32_t count = 0u;
    /* For each node in the input module's order, beyond the front,
     * add to E if at least one of its predecessors is in the front
     * (or in E already). */
    for (uint32_t i = 0u; i < num_nodes && count < EXT_SET_MAX; i++) {
        if (nodes[i].done || in_set[i]) continue;
        const sabre_node_t *N = &nodes[i];
        int link = 0;
        for (uint32_t p = 0u; p < N->num_preds; p++) {
            if (in_set[N->preds[p]]) { link = 1; break; }
        }
        if (link) {
            out[count++] = i;
            in_set[i] = 1u;
        }
    }
    return count;
}

/*
 * Find all SWAP candidates relevant to the current front layer:
 * any (a, b) coupled edge where a or b currently holds a logical
 * qubit referenced by a front-layer 2q gate.  Returns the number
 * of candidates in out[][].
 */
#define MAX_SWAP_CANDIDATES 256u

static uint32_t find_swap_candidates(const sabre_node_t *nodes,
                                     const uint32_t *front, uint32_t front_n,
                                     const coupling_graph_t *G,
                                     uint32_t out_a[MAX_SWAP_CANDIDATES],
                                     uint32_t out_b[MAX_SWAP_CANDIDATES])
{
    /* Mark physical qubits that hold logicals referenced by the
     * front layer's 2q gates. */
    static uint8_t hot[ROUTE_MAX_QUBITS];
    for (uint32_t i = 0u; i < ROUTE_MAX_QUBITS; i++) hot[i] = 0u;
    for (uint32_t i = 0u; i < front_n; i++) {
        const sabre_node_t *N = &nodes[front[i]];
        if (N->num_qubits == 2u) {
            hot[route_l2p[N->qubits[0]]] = 1u;
            hot[route_l2p[N->qubits[1]]] = 1u;
        }
    }
    uint32_t count = 0u;
    uint32_t n = G->num_qubits;
    for (uint32_t a = 0u; a < n; a++) {
        if (!hot[a]) continue;
        for (uint32_t b = a + 1u; b < n; b++) {
            if (G->adj[a][b]) {
                if (count < MAX_SWAP_CANDIDATES) {
                    out_a[count] = a;
                    out_b[count] = b;
                    count++;
                }
            }
        }
    }
    return count;
}

/*
 * Score a candidate SWAP.  Lower is better.  Combines:
 *   - normalised front-layer distance after the hypothetical swap
 *   - weighted normalised extended-set distance after the swap
 *   - multiplied by the decay term for the swapped qubits
 */
#define SABRE_W       0.5
#define SABRE_DECAY   0.001

static double score_swap(const sabre_node_t *nodes,
                         const uint32_t *front, uint32_t front_n,
                         const uint32_t *ext, uint32_t ext_n,
                         uint32_t a, uint32_t b)
{
    /* Apply the swap to the mapping arrays. */
    uint32_t la = route_p2l[a];
    uint32_t lb = route_p2l[b];
    if (la != (uint32_t)0xFFFFFFFFu) route_l2p[la] = b;
    if (lb != (uint32_t)0xFFFFFFFFu) route_l2p[lb] = a;
    route_p2l[a] = lb;
    route_p2l[b] = la;

    double hf = layer_cost(nodes, front, front_n);
    double he = (ext_n > 0u) ? layer_cost(nodes, ext, ext_n) : 0.0;
    double score = hf + SABRE_W * he;

    /* Decay multiplier discourages re-using the same qubit. */
    double da = route_decay[a];
    double db = route_decay[b];
    double dm = (da > db) ? da : db;
    score *= dm;

    /* Undo the swap so the caller's state is preserved. */
    la = route_p2l[a];
    lb = route_p2l[b];
    if (la != (uint32_t)0xFFFFFFFFu) route_l2p[la] = b;
    if (lb != (uint32_t)0xFFFFFFFFu) route_l2p[lb] = a;
    route_p2l[a] = lb;
    route_p2l[b] = la;

    return score;
}

/*
 * Identity-restoring tail.  After the main loop finishes, the
 * mapping is some permutation of identity.  We append SWAPs to
 * bring it back to identity, so the routed circuit's final logical
 * state is in the same physical positions as the input expects.
 *
 * Strategy: for each logical qubit not currently at its identity
 * position, find what's at its identity position, and SWAP.
 * Repeat until every qubit is at its own position.  Worst case
 * O(n) extra SWAPs; usually fewer.
 */
static void restore_identity_mapping(uint32_t n, uint32_t qreg_idx,
                                     uint32_t src_line, uint32_t src_col)
{
    for (uint32_t i = 0u; i < n; i++) {
        if (route_l2p[i] != i) {
            uint32_t phys_here = i;
            uint32_t phys_want = route_l2p[i];
            emit_swap(phys_here, phys_want, qreg_idx, src_line, src_col);
        }
    }
}

/* ---------------------------------------------------------------- */
/* The pass entry point                                             */
/* ---------------------------------------------------------------- */

int opt_route(tir_module_t *M, opt_stats_t *stats, mnote_log_t *log)
{
    assert(M != NULL);
    assert(stats != NULL);
    (void)log;

    stats->insts_in     = M->num_insts;
    stats->iterations   = 1u;
    stats->insts_removed = 0u;

    /* No coupling means no routing.  We're a no-op. */
    if (active_coupling == NULL) {
        stats->insts_out = M->num_insts;
        return 0;
    }

    const coupling_graph_t *G = active_coupling;
    compute_distances(G);

    /* Find which qreg index this module uses.  v1 supports a single
     * qreg; we use the first one declared.  If the circuit has
     * more qubits than the coupling graph allows, we bail with a
     * non-zero RC so the user sees a clear error. */
    uint32_t total_q = tir_total_qubits(M);
    if (total_q > G->num_qubits) {
        /* Not enough physical qubits to route this circuit on this
         * device.  Pass through unchanged and let the caller see
         * the loud return code. */
        stats->insts_out = M->num_insts;
        return 12;
    }
    uint32_t qreg_idx = 0u;   /* assume single qreg */

    /* Bookkeeping reset. */
    init_identity_mapping(G->num_qubits);
    build_dag(M);
    route_scratch_count = 0u;
    route_swap_count    = 0u;

    /* Pass through declarations from the input.  They sit at the
     * top of M->insts and the DAG skipped them, so we copy them
     * across verbatim. */
    for (uint32_t i = 0u; i < M->num_insts; i++) {
        const tir_inst_t *I = &M->insts[i];
        if ((tir_op_t)I->op == TIR_QREG_DECL
            || (tir_op_t)I->op == TIR_CREG_DECL) {
            emit_remapped(I);
        }
    }

    /* Front layer: nodes whose unmet-pred counter is zero. */
    static uint32_t front[SABRE_MAX_NODES];
    uint32_t front_n = 0u;
    for (uint32_t i = 0u; i < sabre_num_nodes; i++) {
        if (sabre_nodes[i].num_unmet_preds == 0u) {
            front[front_n++] = i;
        }
    }

    /* Main loop. */
    uint32_t safety_iter = 0u;
    while (front_n > 0u
           && safety_iter < (uint32_t)TIR_MAX_INSTS * 4u) {
        safety_iter++;

        /* Try to execute any front-layer gate whose operands are
         * physically adjacent.  Loop until no more are executable
         * in this iteration. */
        int progress = 1;
        while (progress) {
            progress = 0;
            for (uint32_t i = 0u; i < front_n; ) {
                sabre_node_t *N = &sabre_nodes[front[i]];
                if (node_executable(N, G)) {
                    emit_remapped(&M->insts[N->inst_idx]);
                    N->done = 1u;
                    /* Decay reset for any qubit touched by an
                     * executed gate. */
                    for (uint32_t k = 0u; k < N->num_qubits; k++) {
                        route_decay[route_l2p[N->qubits[k]]] = 1.0;
                    }
                    /* Advance dependencies: any node whose preds
                     * include this one ticks down. */
                    for (uint32_t j = 0u; j < sabre_num_nodes; j++) {
                        sabre_node_t *S = &sabre_nodes[j];
                        if (S->done) continue;
                        for (uint32_t p = 0u; p < S->num_preds; p++) {
                            if (S->preds[p] == front[i]) {
                                if (S->num_unmet_preds > 0u) {
                                    S->num_unmet_preds--;
                                }
                            }
                        }
                    }
                    /* Remove from front; replace with last and
                     * shrink. */
                    front[i] = front[front_n - 1u];
                    front_n--;
                    progress = 1;
                } else {
                    i++;
                }
            }
            /* After removals, re-scan for newly-eligible nodes. */
            for (uint32_t j = 0u; j < sabre_num_nodes; j++) {
                if (sabre_nodes[j].done) continue;
                if (sabre_nodes[j].num_unmet_preds == 0u) {
                    /* Already in front? */
                    int in_front = 0;
                    for (uint32_t k = 0u; k < front_n; k++) {
                        if (front[k] == j) { in_front = 1; break; }
                    }
                    if (!in_front && front_n < SABRE_MAX_NODES) {
                        front[front_n++] = j;
                        progress = 1;
                    }
                }
            }
        }

        if (front_n == 0u) break;

        /* Nothing executable.  Pick a SWAP using the heuristic. */
        static uint32_t ext[EXT_SET_MAX];
        uint32_t ext_n = build_extended_set(sabre_nodes, sabre_num_nodes,
                                            front, front_n, ext);

        static uint32_t swap_a[MAX_SWAP_CANDIDATES];
        static uint32_t swap_b[MAX_SWAP_CANDIDATES];
        uint32_t cand_n = find_swap_candidates(sabre_nodes, front, front_n,
                                                G, swap_a, swap_b);
        if (cand_n == 0u) {
            /* No candidate SWAPs.  Either the circuit is impossible
             * to route on this graph or we've hit a corner case.
             * Bail. */
            (void)log;
            stats->insts_out = route_scratch_count;
            for (uint32_t i = 0u; i < route_scratch_count; i++) {
                M->insts[i] = route_scratch[i];
            }
            M->num_insts = route_scratch_count;
            return 14;
        }

        double best_score = 1e30;
        uint32_t best_i = 0u;
        for (uint32_t i = 0u; i < cand_n; i++) {
            double s = score_swap(sabre_nodes,
                                   front, front_n, ext, ext_n,
                                   swap_a[i], swap_b[i]);
            if (s < best_score) {
                best_score = s;
                best_i = i;
            }
        }

        /* Apply the chosen SWAP. */
        uint32_t a = swap_a[best_i];
        uint32_t b = swap_b[best_i];
        /* Source location: use the first front-layer node's, so
         * the SWAP is attributable to a real instruction in the
         * input. */
        uint32_t sl = 0u, sc = 0u;
        if (front_n > 0u) {
            const tir_inst_t *I0 =
                &M->insts[sabre_nodes[front[0]].inst_idx];
            sl = I0->src_line;
            sc = I0->src_col;
        }
        emit_swap(a, b, qreg_idx, sl, sc);
        route_decay[a] += SABRE_DECAY;
        route_decay[b] += SABRE_DECAY;
    }

    /* Append SWAPs to restore identity mapping. */
    restore_identity_mapping(G->num_qubits, qreg_idx, 0u, 0u);

    /* Copy scratch back into M. */
    for (uint32_t i = 0u; i < route_scratch_count; i++) {
        M->insts[i] = route_scratch[i];
    }
    M->num_insts = route_scratch_count;

    /* Publish the final mapping to the snap side channel so any
     * snapshot taken after routing carries it. We send the l2p
     * array sized to the device's qubit count, not ROUTE_MAX_QUBITS. */
    snap_set_mapping(route_l2p, G->num_qubits);

    stats->insts_out = route_scratch_count;
    stats->insts_removed = 0u;
    return 0;
}
