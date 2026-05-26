#ifndef ERNEST_SIM_H
#define ERNEST_SIM_H

#include "tir.h"

/*
 * The statevector simulator.
 *
 * Behind every quantum computer there is a worried piece of classical
 * machinery imagining what the quantum computer might be doing. This
 * file is that machinery. It walks a TIR circuit one instruction at
 * a time and keeps track of how the universe would have evolved if
 * it had been paying attention.
 *
 * The model is exact. Each gate becomes a unitary matrix applied to
 * a vector of complex amplitudes. Each measurement collapses that
 * vector down to a single outcome, with the probabilities that
 * quantum mechanics insists on. The number of amplitudes grows like
 * 2^N in the number of qubits, which gets out of hand alarmingly
 * quickly:
 *
 *   sixteen qubits is about a megabyte
 *   twenty qubits is sixteen megabytes
 *   thirty qubits is sixteen gigabytes
 *   forty qubits is more memory than any single computer ought to own
 *
 * Which is one of the better arguments for actually building
 * quantum hardware: the universe is much faster at this than we
 * are, and it has been since 1925.
 *
 * The statevector lives in a static array sized for the maximum
 * supported qubit count. Beyond that, the simulator stops and says
 * so. Better to halt cleanly than to pretend.
 */

/*
 * Sixteen qubits, 65,536 amplitudes, about a megabyte. Comfortable
 * on any laptop made this century. Bigger numbers are available
 * for the price of patience and a stick of memory.
 */
#define ERNEST_SIM_MAX_QUBITS 16
#define ERNEST_SIM_MAX_BITS   32

/*
 * Instruction trace ring buffer length. The ABEND dump reaches in
 * here for the "last N of M instructions executed" listing when
 * the simulator falls over. Sixteen is plenty: long enough to see
 * the run-up to a fault, short enough not to clutter the dump.
 */
#define ERNEST_SIM_TRACE_LEN  16

/*
 * Simulator status codes. SIM_OK means the shot completed normally;
 * anything else means the simulator stopped mid-shot for the reason
 * indicated. The corresponding mainframe completion code is in the
 * comment. The caller turns the status into an ABEND dump.
 */
typedef enum {
    SIM_OK         = 0,
    SIM_ABEND_S0C1 = 1,    /* unknown gate opcode                  */
    SIM_ABEND_S0C4 = 2,    /* qubit index out of range             */
    SIM_ABEND_S0C7 = 3,    /* NaN or Inf in statevector            */
    SIM_ABEND_S0CB = 4,    /* normalisation divide-by-zero         */
    SIM_ABEND_U0008 = 5,   /* statevector dimension exceeded       */
    SIM_ABEND_U0009 = 6    /* classical register dimension exceeded */
} sim_status_t;

/* Translate a sim_status_t to the four-character abend code text.
 * Returns "????" for SIM_OK. */
const char *sim_status_code(sim_status_t s);

typedef struct {
    double re;
    double im;
} sim_complex_t;

/*
 * The simulator state. The statevector is a flat array of complex
 * amplitudes indexed by computational basis state, little-endian:
 * amplitude index n is the amplitude of the state in which qubit 0
 * is the lowest bit of n. The classical register is a flat array
 * of zeros and ones that measurement fills in.
 */
typedef struct {
    uint32_t      num_qubits;
    uint32_t      num_bits;
    sim_complex_t state[1u << ERNEST_SIM_MAX_QUBITS];
    uint8_t       bits[ERNEST_SIM_MAX_BITS];

    /*
     * Instruction trace. As the simulator dispatches each TIR
     * instruction it stamps the index into this ring buffer. When
     * something goes wrong the ABEND dump walks the buffer in
     * order, oldest first, ending at the instruction that fired
     * the fault. Mainframe trace tables work the same way.
     */
    uint32_t      trace_buf[ERNEST_SIM_TRACE_LEN];
    uint32_t      trace_head;     /* next write position           */
    uint32_t      trace_count;    /* total instructions executed  */

    /*
     * The instruction the simulator is presently executing. Held
     * separately so the ABEND dump can point at the exact offender
     * even before the trace stamps it.
     */
    uint32_t      current_inst;

    /*
     * Status. SIM_OK while everything is going well, something
     * else when the simulator has stopped mid-shot. The reason
     * field carries a short explanation; the offending qubit (if
     * any) goes in status_qubit. The caller picks these up and
     * builds an ABEND dump from them.
     */
    sim_status_t  status;
    uint32_t      status_qubit;
    char          status_reason[160];
} sim_state_t;

/*
 * Initialise the simulator for a given module. Sets the statevector
 * to |0...0>, which is where every quantum circuit starts. The
 * convention is universal and only mildly arbitrary.
 */
void sim_init(sim_state_t *S, const tir_module_t *M);

/*
 * Run the circuit once. After the dust settles, the classical bits
 * hold the outcomes of any measurements. Randomness comes from the
 * standard library rand(), which is fine for demonstrations and
 * absolutely the wrong choice for cryptography. The caller is
 * responsible for seeding the RNG once at startup. Re-seeding
 * between shots is the kind of well-meaning idea that gives you a
 * histogram with a noticeable lean.
 */
void sim_run_shot(sim_state_t *S, const tir_module_t *M);

/*
 * Run the circuit's unitary portion, skipping measurements and
 * resets. The simulator state at the end carries the exact pre-
 * measurement statevector, which is what you want for mathematical
 * verification of a transformation: simulate the abstract and the
 * decomposed circuit, compare the two statevectors, and you know
 * whether the transformation was a true equivalence or only a
 * statistical near-miss. Deterministic; uses no RNG.
 */
void sim_run_unitary_only(sim_state_t *S, const tir_module_t *M);

/*
 * Pack the classical register into an unsigned integer, bit 0 being
 * the first classical bit. Handy for building histograms: each shot
 * becomes one key in a counts table.
 */
uint32_t sim_creg_as_uint(const sim_state_t *S);

#endif /* ERNEST_SIM_H */
