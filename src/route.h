#ifndef ERNEST_ROUTE_H
#define ERNEST_ROUTE_H

#include "tir.h"
#include "opt.h"

/*
 * Qubit routing.
 *
 * Real quantum hardware has a coupling map: a graph that says which
 * pairs of physical qubits can do a two-qubit gate directly. IBM's
 * heavy-hex topology, for instance, looks nothing like a complete
 * graph. If a circuit says cx q[0], q[5] but the device only couples
 * q[0]<->q[1]<->q[2]<->...<->q[5], the compiler has to insert SWAPs
 * along the path to bring those two logical qubits next to each
 * other in physical space, do the CX, and live with the consequences
 * for the logical-to-physical mapping going forward.
 *
 * That is what this pass does. The algorithm is SABRE (Li, Ding, &
 * Xie, 2019): build a DAG of multi-qubit gates from the input
 * circuit, maintain a front layer of gates whose dependencies are
 * met, and at each step either execute any front-layer gates whose
 * operands are physically adjacent under the current mapping, or
 * pick the next SWAP that minimises a heuristic combining the cost
 * of the current front layer and a weighted look-ahead over the
 * next few gates. A small decay term per recently-swapped qubit
 * keeps the pass from oscillating between equivalent SWAPs.
 *
 * For v1 we run SABRE in a single forward pass with identity
 * initial mapping. After routing, we append SWAPs to restore the
 * identity mapping at the end of the circuit, so the routed
 * module's final statevector matches the original's exactly and
 * the verifier can compare them without permutation. The cost is
 * a few extra SWAPs at the tail; the benefit is that hardware
 * submission and the verifier read from the same canonical form.
 */

/* ----- Coupling graphs -------------------------------------------- */
/*
 * A coupling graph is an adjacency matrix on a fixed-size table.
 * We bound the matrix at one hundred and twenty-eight qubits, which
 * comfortably fits any IBM Eagle device (127 qubits) and most
 * smaller machines. Heron and larger devices need a bigger bound;
 * that's a one-line edit when the time comes. The table cost at
 * 128 is sixteen kilobytes per coupling graph, which is fine.
 */
#define ROUTE_MAX_QUBITS 128u

typedef struct {
    const char *name;
    uint32_t    num_qubits;
    uint8_t     adj[ROUTE_MAX_QUBITS][ROUTE_MAX_QUBITS];
} coupling_graph_t;

/*
 * Look up a coupling graph by name. Returns NULL if the name does
 * not match any of the built-in topologies. Names are matched
 * case-insensitively and underscores and hyphens are
 * interchangeable to be friendly to people typing them at a shell.
 *
 * Built-in topologies (more added as they become useful):
 *
 *   linear_5         5-qubit chain
 *   linear_16        16-qubit chain, matches the simulator's cap
 *   ring_5           5-qubit ring
 *   ring_8           8-qubit ring
 *   grid_4x4         16-qubit 4-by-4 grid, IBM Falcon-style
 *   heavy_hex_7      7-qubit heavy-hex sub-graph (IBM Falcon r4)
 *   ibm_falcon_5_t   5-qubit T-shape, matches retired Falcon r5
 *   full_16          all-pairs connected, routing is a no-op
 *
 * For real IBM devices, use route_load_coupling_from_file with a
 * coupling map dumped from Qiskit (see tools/dump_coupling.py).
 */
const coupling_graph_t *route_lookup_coupling(const char *name);

/*
 * Load a coupling map from a plain-text file. The format is one
 * edge per line, "u v" where u and v are non-negative integers,
 * with #-prefixed comment lines and blank lines allowed anywhere.
 * The number of qubits is inferred as one plus the largest index
 * mentioned. Returns a pointer to a static internal buffer on
 * success and NULL on any I/O or parse error.
 *
 * The buffer is single-instance: a subsequent call overwrites the
 * previous load. This is fine for the typical use case of one
 * device per compilation. Threaded callers would need to take a
 * copy.
 */
const coupling_graph_t *route_load_coupling_from_file(const char *path);

/* ----- Driver-side configuration --------------------------------- */
/*
 * The routing pass needs the coupling graph but the opt_pass_fn_t
 * signature doesn't carry extra parameters. The driver sets the
 * active coupling here before invoking the pass; opt_route reads
 * it. A NULL active coupling means routing is a no-op, which is
 * also the default and matches the "no hardware target chosen" case.
 */
void route_set_active_coupling(const coupling_graph_t *G);
const coupling_graph_t *route_get_active_coupling(void);

/* ----- The pass --------------------------------------------------- */
/*
 * Walk the instruction stream, route every two-qubit gate to use
 * physically-adjacent operands, and insert SWAPs along the way as
 * needed. Modifies M in place. Returns 0 on a clean run. If no
 * coupling graph has been set, the pass is a no-op.
 */
int opt_route(tir_module_t *M, opt_stats_t *stats, mnote_log_t *log);

#endif /* ERNEST_ROUTE_H */
