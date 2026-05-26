#include "sim.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * Status-code table. Each simulator status maps to the mainframe
 * completion code text used in the eventual ABEND dump.
 */
const char *sim_status_code(sim_status_t s)
{
    switch (s) {
    case SIM_OK:           return "????";
    case SIM_ABEND_S0C1:   return "S0C1";
    case SIM_ABEND_S0C4:   return "S0C4";
    case SIM_ABEND_S0C7:   return "S0C7";
    case SIM_ABEND_S0CB:   return "S0CB";
    case SIM_ABEND_U0008:  return "U0008";
    case SIM_ABEND_U0009:  return "U0009";
    default:               return "????";
    }
}

/*
 * Record a status on the simulator state. Once set, no further
 * instructions execute on this shot; the dispatcher checks
 * S->status at the top of its loop.
 */
static void sim_set_status(sim_state_t *S, sim_status_t code,
                           uint32_t qubit, const char *fmt, ...)
{
    assert(S != NULL);
    assert(fmt != NULL);
    if (S->status != SIM_OK) {
        return;  /* first abend wins, the rest are noise */
    }
    S->status = code;
    S->status_qubit = qubit;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(S->status_reason, sizeof S->status_reason, fmt, ap);
    va_end(ap);
}

/*
 * After-gate sanity check. If any amplitude has gone NaN or Inf
 * the statevector is dead and we should ABEND rather than carry
 * on producing garbage. S0C7 is the mainframe code for "data
 * exception", which is exactly what this is: arithmetic on a
 * value that won't behave.
 */
static void check_amplitudes_finite(sim_state_t *S)
{
    assert(S != NULL);
    if (S->status != SIM_OK) {
        return;
    }
    uint32_t dim = 1u << S->num_qubits;
    for (uint32_t i = 0u; i < dim; i++) {
        double re = S->state[i].re;
        double im = S->state[i].im;
        if (!isfinite(re) || !isfinite(im)) {
            sim_set_status(S, SIM_ABEND_S0C7, 0u,
                "NON-FINITE AMPLITUDE AT BASIS STATE %u (re=%.3e im=%.3e)",
                (unsigned)i, re, im);
            return;
        }
    }
}

/*
 * The statevector simulator.
 *
 * A qubit, viewed sideways, is a pair of complex numbers. Two qubits
 * together are four. Three are eight. The pattern continues and
 * gets out of hand alarmingly fast, which is the reason the universe
 * gets to do quantum mechanics and we mostly get to watch from the
 * sidelines.
 *
 * Each amplitude is a complex number written a + bi. The probability
 * of observing a given computational basis state is a^2 + b^2, the
 * squared magnitude of its amplitude. Amplitudes can interfere with
 * each other. This is the trick the whole field rests on, and the
 * thing classical computing finds vaguely unsporting. Interference
 * is what makes a Hadamard interesting. Without it the Hadamard
 * would just be an expensive coin flip with extra steps.
 *
 * Every gate is applied as a unitary transformation to the entire
 * statevector at once. The cost is 2^N amplitudes per shot. Sixteen
 * qubits, sixty-five thousand amplitudes, runs on any laptop you
 * own. Thirty-two qubits, four billion amplitudes, runs on machines
 * you do not own.
 */

/*
 * The reciprocal of root two. Turns up everywhere on the Bloch
 * sphere where you might want a forty-five degree turn: Hadamard,
 * T, T-dagger, and friends. Pinned to a compile-time constant once
 * so the cycles add up the right way over a few million shots.
 */
static const double INV_SQRT2 = 0.70710678118654752440;

/*
 * Initialise to |0...0>. Amplitude zero is one, every other
 * amplitude is zero, the classical register is also zero. Quiet
 * water before anyone has thrown a rock in.
 */
void sim_init(sim_state_t *S, const tir_module_t *M)
{
    assert(S != NULL);
    assert(M != NULL);

    /* Zero everything so trace state, status flags, and the
     * classical register start fresh for each shot. The statevector
     * itself is overwritten below; zeroing it here is harmless
     * either way. About a megabyte per call, which is fast enough
     * not to notice. */
    memset(S, 0, sizeof *S);

    uint32_t nq = tir_total_qubits(M);
    uint32_t nb = tir_total_bits(M);

    if (nq > (uint32_t)ERNEST_SIM_MAX_QUBITS) {
        sim_set_status(S, SIM_ABEND_U0008, 0u,
            "CIRCUIT NEEDS %u QUBITS, SIMULATOR LIMIT IS %u",
            (unsigned)nq, (unsigned)ERNEST_SIM_MAX_QUBITS);
        return;
    }
    if (nb > (uint32_t)ERNEST_SIM_MAX_BITS) {
        sim_set_status(S, SIM_ABEND_U0009, 0u,
            "CIRCUIT NEEDS %u CLASSICAL BITS, SIMULATOR LIMIT IS %u",
            (unsigned)nb, (unsigned)ERNEST_SIM_MAX_BITS);
        return;
    }

    S->num_qubits = nq;
    S->num_bits = nb;

    /* Zero the statevector. Total length is 2^nq, capped statically
     * at 2^ERNEST_SIM_MAX_QUBITS. The compiler can see the ceiling
     * from across the room. */
    uint32_t dim = 1u << nq;
    assert(dim <= (uint32_t)(1u << ERNEST_SIM_MAX_QUBITS));
    for (uint32_t i = 0u; i < dim; i++) {
        S->state[i].re = 0.0;
        S->state[i].im = 0.0;
    }
    S->state[0].re = 1.0;

    /* Zero the classical register. */
    for (uint32_t i = 0u; i < (uint32_t)ERNEST_SIM_MAX_BITS; i++) {
        S->bits[i] = 0u;
    }
}

/*
 * Apply a single-qubit gate as a 2x2 complex matrix. The four
 * entries arrive in row-major order. The gate acts on the qubit at
 * position q in the flat qubit list.
 *
 * The trick: a single-qubit gate divides the basis states into
 * pairs. Each pair has one state with qubit q equal to zero and
 * one with q equal to one, with all other qubits matching exactly.
 * The gate matrix mixes the two amplitudes of each pair. Every pair
 * minds its own business, which means a future version could thread
 * this very nicely if it ever needed to.
 */
static void apply_1q(sim_state_t *S, uint32_t q,
                     double m00r, double m00i,
                     double m01r, double m01i,
                     double m10r, double m10i,
                     double m11r, double m11i)
{
    assert(S != NULL);
    assert(q < S->num_qubits);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t mask = 1u << q;

    /* Walk every basis state. When we hit one with qubit q set to
     * zero, find its partner (same bits everywhere else, q set to
     * one) and mix the two amplitudes through the matrix. The
     * partner always has a higher index than the current state, so
     * the in-place update never tries to visit the same pair twice
     * and never tramples its own working memory. */
    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & mask) != 0u) {
            continue;
        }
        uint32_t j = i | mask;

        double a_re = S->state[i].re;
        double a_im = S->state[i].im;
        double b_re = S->state[j].re;
        double b_im = S->state[j].im;

        /* state[i] = m00 * a + m01 * b */
        S->state[i].re = m00r * a_re - m00i * a_im + m01r * b_re - m01i * b_im;
        S->state[i].im = m00r * a_im + m00i * a_re + m01r * b_im + m01i * b_re;
        /* state[j] = m10 * a + m11 * b */
        S->state[j].re = m10r * a_re - m10i * a_im + m11r * b_re - m11i * b_im;
        S->state[j].im = m10r * a_im + m10i * a_re + m11r * b_im + m11i * b_re;
    }
}

/*
 * Hadamard. The gate that takes a qubit which knows what it is and
 * gives it room to consider the alternatives. After a Hadamard the
 * qubit measures zero or one with equal probability, but the
 * probabilities are far from independent. Run two qubits through
 * Hadamards and bind them with a CX, and you get an entangled pair
 * that neither qubit could have managed alone. Two solitary
 * tramping huts get a track between them and become a route.
 * Matrix: (1/sqrt(2)) * { {1, 1}, {1, -1} }.
 */
static void gate_h(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q,
             INV_SQRT2, 0.0,   INV_SQRT2, 0.0,
             INV_SQRT2, 0.0,  -INV_SQRT2, 0.0);
}

/* Pauli X. The bit flip. The one gate where classical and quantum
 * agree without much fuss. */
static void gate_x(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 0.0, 0.0,  1.0, 0.0,
                   1.0, 0.0,  0.0, 0.0);
}

/* Pauli Y. X with an imaginary tan, for rotations that need to land
 * somewhere off the obvious axes. */
static void gate_y(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 0.0,  0.0,   0.0, -1.0,
                   0.0,  1.0,   0.0,  0.0);
}

/* Pauli Z flips the phase of |1> and leaves |0> alone. Invisible in
 * the standard basis. Becomes loud the moment another Hadamard
 * walks past. */
static void gate_z(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 1.0, 0.0,  0.0, 0.0,
                   0.0, 0.0, -1.0, 0.0);
}

/* S is a quarter turn around Z. The phase of |1> picks up a factor
 * of i. Lets you talk about phase without dragging Greek letters
 * into the conversation. */
static void gate_s(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 1.0, 0.0,  0.0, 0.0,
                   0.0, 0.0,  0.0, 1.0);
}

/* T is half of S, an eighth turn around Z. Clifford gates plus T
 * span every useful unitary, so once you have T you have all the
 * single-qubit gates you'll ever need. */
static void gate_t(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 1.0, 0.0,        0.0, 0.0,
                   0.0, 0.0,        INV_SQRT2, INV_SQRT2);
}

/* S-dagger is S walking backwards. Same quarter turn, other way. */
static void gate_sdg(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 1.0, 0.0,  0.0,  0.0,
                   0.0, 0.0,  0.0, -1.0);
}

/* T-dagger is T walking backwards. The dagger comes from physics,
 * where it means conjugate transpose, which for a unitary is its
 * inverse. The mathematicians have signed off on this and gone
 * home. */
static void gate_tdg(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q, 1.0, 0.0,        0.0,  0.0,
                   0.0, 0.0,        INV_SQRT2, -INV_SQRT2);
}

/* SX is the square root of X. SX^2 = X, which is the property that
 * earned the gate its job description. IBM's hardware can do this
 * one directly, and most of the rest of the gate zoo gets routed
 * through it when targeting an IBM machine.
 * Matrix: ((1+i)/2) * [[1, -i], [-i, 1]]
 *       = [[(1+i)/2, (1-i)/2], [(1-i)/2, (1+i)/2]] */
static void gate_sx(sim_state_t *S, uint32_t q)
{
    apply_1q(S, q,
             0.5,  0.5,   0.5, -0.5,
             0.5, -0.5,   0.5,  0.5);
}

/* RX(theta) rotates around the X axis of the Bloch sphere.
 *   RX(theta) = [[ cos(theta/2), -i*sin(theta/2) ],
 *                [ -i*sin(theta/2),  cos(theta/2) ]]
 * Pick theta = pi and you get X up to a global phase. Pick smaller
 * thetas and you get gentler rotations, which is what every
 * variational algorithm needs to do its job. */
static void gate_rx(sim_state_t *S, uint32_t q, double theta)
{
    double c = cos(theta * 0.5);
    double s = sin(theta * 0.5);
    apply_1q(S, q,
             c,  0.0,   0.0, -s,
             0.0, -s,   c,   0.0);
}

/* RY(theta) rotates around the Y axis.
 *   RY(theta) = [[ cos(theta/2), -sin(theta/2) ],
 *                [ sin(theta/2),  cos(theta/2) ]]
 * Real-valued matrix, which means RY rotations keep things real
 * if they started real, which is a small kindness in a field that
 * mostly trades in complex numbers. */
static void gate_ry(sim_state_t *S, uint32_t q, double theta)
{
    double c = cos(theta * 0.5);
    double s = sin(theta * 0.5);
    apply_1q(S, q,
             c,   0.0,  -s,  0.0,
             s,   0.0,   c,  0.0);
}

/* RZ(theta) rotates around the Z axis.
 *   RZ(theta) = [[ exp(-i*theta/2), 0 ],
 *                [ 0,                exp(i*theta/2) ]]
 * Diagonal, so it only changes phases. Invisible if you measure
 * straight away in the computational basis. Becomes loud the moment
 * another Hadamard turns up. */
static void gate_rz(sim_state_t *S, uint32_t q, double theta)
{
    double c = cos(theta * 0.5);
    double s = sin(theta * 0.5);
    apply_1q(S, q,
             c, -s,    0.0, 0.0,
             0.0, 0.0,  c,  s);
}

/*
 * CX, the CNOT. The first operand is the control, the second the
 * target. If the control is |0> nothing happens. If the control is
 * |1> the target flips. The honest description is that CX
 * correlates two qubits in a way that survives measurement, which
 * is the trick the whole field rests on.
 *
 * The implementation walks every basis state. When the control bit
 * is set, swap the amplitude at this state with the one at the
 * state-with-target-flipped. The "j > i" guard makes sure each
 * pair is visited once and only once.
 */
static void gate_cx(sim_state_t *S, uint32_t ctrl, uint32_t tgt)
{
    assert(S != NULL);
    assert(ctrl < S->num_qubits);
    assert(tgt < S->num_qubits);
    assert(ctrl != tgt);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t cm = 1u << ctrl;
    uint32_t tm = 1u << tgt;

    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & cm) == 0u) {
            continue;
        }
        uint32_t j = i ^ tm;
        if (j > i) {
            sim_complex_t tmp = S->state[i];
            S->state[i] = S->state[j];
            S->state[j] = tmp;
        }
    }
}

/*
 * CZ flips the phase of |11> and leaves the rest of the universe
 * alone. The polite cousin of CX. Both entangle, but CZ does so
 * without shuffling any amplitudes around, which is occasionally
 * the more elegant move.
 */
static void gate_cz(sim_state_t *S, uint32_t ctrl, uint32_t tgt)
{
    assert(S != NULL);
    assert(ctrl < S->num_qubits);
    assert(tgt < S->num_qubits);
    assert(ctrl != tgt);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t mask = (1u << ctrl) | (1u << tgt);

    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & mask) == mask) {
            S->state[i].re = -S->state[i].re;
            S->state[i].im = -S->state[i].im;
        }
    }
}

/*
 * CH applies the Hadamard to the target only when the control is
 * |1>, leaving the rest of the universe alone. Useful for building
 * states like the three-qubit W in one line that would otherwise
 * want three. The implementation walks every pair of basis states
 * that differ only in the target bit and have the control bit set,
 * mixing the two amplitudes through the Hadamard matrix.
 */
static void gate_ch(sim_state_t *S, uint32_t ctrl, uint32_t tgt)
{
    assert(S != NULL);
    assert(ctrl < S->num_qubits);
    assert(tgt < S->num_qubits);
    assert(ctrl != tgt);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t cm = 1u << ctrl;
    uint32_t tm = 1u << tgt;

    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & cm) == 0u) {
            continue;             /* control off, identity */
        }
        if ((i & tm) != 0u) {
            continue;             /* visit each pair once, from the t=0 side */
        }
        uint32_t j = i | tm;
        double a_re = S->state[i].re;
        double a_im = S->state[i].im;
        double b_re = S->state[j].re;
        double b_im = S->state[j].im;

        S->state[i].re = INV_SQRT2 * (a_re + b_re);
        S->state[i].im = INV_SQRT2 * (a_im + b_im);
        S->state[j].re = INV_SQRT2 * (a_re - b_re);
        S->state[j].im = INV_SQRT2 * (a_im - b_im);
    }
}

/*
 * CP applies a phase exp(i*theta) to the |11> component of a qubit
 * pair and leaves the rest of the four-state space alone. The QFT
 * is essentially a ladder of CP gates with the angles halving as you
 * go down the register, which is the reason this gate gets called
 * almost as much as CX in real circuits.
 */
static void gate_cp(sim_state_t *S, uint32_t ctrl, uint32_t tgt, double theta)
{
    assert(S != NULL);
    assert(ctrl < S->num_qubits);
    assert(tgt < S->num_qubits);
    assert(ctrl != tgt);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t mask = (1u << ctrl) | (1u << tgt);

    double c = cos(theta);
    double s = sin(theta);

    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & mask) != mask) {
            continue;
        }
        double re = S->state[i].re;
        double im = S->state[i].im;
        /* multiply by exp(i*theta) = c + i*s */
        S->state[i].re = re * c - im * s;
        S->state[i].im = re * s + im * c;
    }
}

/*
 * CCX, the Toffoli. Flips the target bit if both controls are |1>.
 * Universal for classical reversible computation by itself, useful
 * for everything from arithmetic to oracle construction. The
 * implementation is CX with a second guard on the second control:
 * walk basis states, when both controls are set, swap with the
 * target-flipped partner. The j > i guard keeps each pair visited
 * once.
 */
static void gate_ccx(sim_state_t *S, uint32_t c1, uint32_t c2, uint32_t tgt)
{
    assert(S != NULL);
    assert(c1  < S->num_qubits);
    assert(c2  < S->num_qubits);
    assert(tgt < S->num_qubits);
    assert(c1 != c2);
    assert(c1 != tgt);
    assert(c2 != tgt);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t ctrl_mask = (1u << c1) | (1u << c2);
    uint32_t tm = 1u << tgt;

    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & ctrl_mask) != ctrl_mask) {
            continue;
        }
        uint32_t j = i ^ tm;
        if (j > i) {
            sim_complex_t tmp = S->state[i];
            S->state[i] = S->state[j];
            S->state[j] = tmp;
        }
    }
}

/*
 * SWAP exchanges the contents of two qubits. Earns its keep on
 * superconducting hardware, where the coupling map is fixed and
 * the qubits you want to interact are often not the qubits that
 * happen to be next to each other.
 */
static void gate_swap(sim_state_t *S, uint32_t a, uint32_t b)
{
    assert(S != NULL);
    assert(a < S->num_qubits);
    assert(b < S->num_qubits);
    assert(a != b);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t ma = 1u << a;
    uint32_t mb = 1u << b;

    for (uint32_t i = 0u; i < dim; i++) {
        uint32_t bit_a = (i & ma) != 0u;
        uint32_t bit_b = (i & mb) != 0u;
        if (bit_a == bit_b) {
            continue;
        }
        uint32_t j = i ^ ma ^ mb;
        if (j > i) {
            sim_complex_t tmp = S->state[i];
            S->state[i] = S->state[j];
            S->state[j] = tmp;
        }
    }
}

/*
 * Measure a single qubit. The qubit, having been in a superposition
 * for as long as nobody asked, now has to commit. It picks zero or
 * one weighted by the squared magnitude of each amplitude, and once
 * it has picked, it sticks. The statevector is then renormalised,
 * because the universe is fastidious about its accounting and likes
 * the books to balance to one.
 *
 * Returns the outcome and writes it into the classical bit at
 * flat_bit_idx. The result is now part of the classical world.
 * Welcome back.
 */
static uint8_t measure(sim_state_t *S, uint32_t q, uint32_t flat_bit_idx)
{
    assert(S != NULL);
    assert(q < S->num_qubits);
    assert(flat_bit_idx < S->num_bits);

    uint32_t dim = 1u << S->num_qubits;
    uint32_t mask = 1u << q;

    /* Sum the probability of the qubit being one. */
    double p1 = 0.0;
    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & mask) != 0u) {
            double re = S->state[i].re;
            double im = S->state[i].im;
            p1 += re * re + im * im;
        }
    }

    /* Sample. rand() returns an integer in [0, RAND_MAX], dividing
     * gives a double in [0, 1], compare against p1. A more
     * principled simulator would use a better RNG. This one is
     * for the demo, not for cryptography. */
    double r = (double)rand() / (double)RAND_MAX;
    uint8_t outcome = (r < p1) ? 1u : 0u;

    /* Project the statevector. If the outcome was one, zero out
     * every amplitude where qubit q is zero, and vice versa. Then
     * renormalise so the surviving amplitudes still add to one. */
    double norm = (outcome == 1u) ? p1 : (1.0 - p1);
    if (norm <= 0.0) {
        /* The outcome we drew has probability zero, which should
         * only happen if rand() landed exactly on the boundary. Fall
         * back to the other outcome. */
        outcome = (outcome == 1u) ? 0u : 1u;
        norm = (outcome == 1u) ? p1 : (1.0 - p1);
    }
    double inv_norm = 1.0 / sqrt(norm);

    for (uint32_t i = 0u; i < dim; i++) {
        uint32_t bit = (i & mask) != 0u;
        if (bit != outcome) {
            S->state[i].re = 0.0;
            S->state[i].im = 0.0;
        } else {
            S->state[i].re *= inv_norm;
            S->state[i].im *= inv_norm;
        }
    }

    S->bits[flat_bit_idx] = outcome;
    return outcome;
}

/*
 * Reset is measurement followed by an X if the measurement came out
 * one. The qubit lands in |0>, wherever it started. Useful for
 * giving a qubit a fresh start between sub-circuits, the way you
 * dust off your boots between rooms.
 */
static void reset_qubit(sim_state_t *S, uint32_t q)
{
    assert(S != NULL);
    assert(q < S->num_qubits);

    /* Same maths as measurement, except we don't record the
     * outcome anywhere classical. We just sample, project, and
     * conditionally apply X to land in |0>. */
    uint32_t dim = 1u << S->num_qubits;
    uint32_t mask = 1u << q;

    double p1 = 0.0;
    for (uint32_t i = 0u; i < dim; i++) {
        if ((i & mask) != 0u) {
            double re = S->state[i].re;
            double im = S->state[i].im;
            p1 += re * re + im * im;
        }
    }
    double r = (double)rand() / (double)RAND_MAX;
    uint8_t outcome = (r < p1) ? 1u : 0u;

    double norm = (outcome == 1u) ? p1 : (1.0 - p1);
    if (norm <= 0.0) {
        outcome = (outcome == 1u) ? 0u : 1u;
        norm = (outcome == 1u) ? p1 : (1.0 - p1);
    }
    double inv_norm = 1.0 / sqrt(norm);

    for (uint32_t i = 0u; i < dim; i++) {
        uint32_t bit = (i & mask) != 0u;
        if (bit != outcome) {
            S->state[i].re = 0.0;
            S->state[i].im = 0.0;
        } else {
            S->state[i].re *= inv_norm;
            S->state[i].im *= inv_norm;
        }
    }
    if (outcome == 1u) {
        gate_x(S, q);
    }
}

/*
 * The main loop. Walk the TIR instructions in order and apply each
 * one to the statevector. Declarations are no-ops here, since the
 * simulator already learned its dimensions in sim_init.
 *
 * Does not touch the RNG state. The caller seeds rand() once at
 * startup and the simulator just reads from it. Re-seeding mid-run
 * is a famous way to introduce bias on rand() implementations
 * where consecutive seeds produce correlated first samples.
 */
void sim_run_shot(sim_state_t *S, const tir_module_t *M)
{
    assert(S != NULL);
    assert(M != NULL);

    uint32_t n = M->num_insts;
    assert(n <= (uint32_t)TIR_MAX_INSTS);
    for (uint32_t i = 0u; i < n; i++) {
        /* If a previous instruction left the simulator with an
         * abnormal status, stop the shot cold. The caller will
         * pick up S->status and print the dump. */
        if (S->status != SIM_OK) {
            return;
        }

        const tir_inst_t *I = &M->insts[i];
        uint32_t q0;
        uint32_t q1;
        uint32_t b0;

        /* Stamp the instruction into the trace ring buffer, then
         * record it as the current one. If we end up taking an
         * ABEND below, the dump will pick this up and show the
         * exact offender at the top of the recent-instructions
         * listing. */
        S->trace_buf[S->trace_head] = i;
        S->trace_head = (S->trace_head + 1u) % (uint32_t)ERNEST_SIM_TRACE_LEN;
        S->trace_count++;
        S->current_inst = i;

        switch (I->op) {
        case TIR_QREG_DECL:
        case TIR_CREG_DECL:
            break;

        case TIR_GATE_H:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_h(S, q0);
            break;
        case TIR_GATE_X:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_x(S, q0);
            break;
        case TIR_GATE_Y:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_y(S, q0);
            break;
        case TIR_GATE_Z:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_z(S, q0);
            break;
        case TIR_GATE_S:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_s(S, q0);
            break;
        case TIR_GATE_T:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_t(S, q0);
            break;
        case TIR_GATE_SDG:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_sdg(S, q0);
            break;
        case TIR_GATE_TDG:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_tdg(S, q0);
            break;
        case TIR_GATE_SX:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_sx(S, q0);
            break;

        case TIR_GATE_RX:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            assert(I->operands[1] < M->num_angles);
            gate_rx(S, q0, M->angles[I->operands[1]]);
            break;
        case TIR_GATE_RY:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            assert(I->operands[1] < M->num_angles);
            gate_ry(S, q0, M->angles[I->operands[1]]);
            break;
        case TIR_GATE_RZ:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            assert(I->operands[1] < M->num_angles);
            gate_rz(S, q0, M->angles[I->operands[1]]);
            break;

        case TIR_GATE_CX:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_cx(S, q0, q1);
            break;
        case TIR_GATE_CZ:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_cz(S, q0, q1);
            break;
        case TIR_GATE_CH:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_ch(S, q0, q1);
            break;
        case TIR_GATE_CP:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            assert(I->operands[2] < M->num_angles);
            gate_cp(S, q0, q1, M->angles[I->operands[2]]);
            break;
        case TIR_GATE_SWAP:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_swap(S, q0, q1);
            break;
        case TIR_GATE_CCX: {
            uint32_t q2 = tir_flat_qubit_index(M, I->operands[2]);
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_ccx(S, q0, q1, q2);
            break;
        }

        case TIR_MEASURE:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            b0 = tir_flat_bit_index(M, I->operands[1]);
            (void)measure(S, q0, b0);
            break;

        case TIR_RESET:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            reset_qubit(S, q0);
            break;

        default:
            sim_set_status(S, SIM_ABEND_S0C1, 0u,
                "UNKNOWN GATE OPCODE %u AT INSTRUCTION %u",
                (unsigned)I->op, (unsigned)i);
            return;
        }

        /* Sanity check after every instruction. Cheap relative to
         * the gate application above for small qubit counts, and
         * catches data exceptions the moment they happen rather
         * than letting them propagate through subsequent gates
         * until the final histogram looks weird. */
        check_amplitudes_finite(S);
    }
}

/*
 * Run the unitary portion of the circuit. Same dispatch as
 * sim_run_shot but measurement and reset are no-ops, leaving the
 * statevector in its pre-measurement form. Useful for verification:
 * two circuits that implement the same overall unitary produce
 * statevectors that are equal up to a global phase, and we can
 * check that without depending on the RNG.
 */
void sim_run_unitary_only(sim_state_t *S, const tir_module_t *M)
{
    assert(S != NULL);
    assert(M != NULL);

    uint32_t n = M->num_insts;
    assert(n <= (uint32_t)TIR_MAX_INSTS);
    for (uint32_t i = 0u; i < n; i++) {
        if (S->status != SIM_OK) {
            return;
        }

        const tir_inst_t *I = &M->insts[i];
        uint32_t q0;
        uint32_t q1;

        S->trace_buf[S->trace_head] = i;
        S->trace_head = (S->trace_head + 1u) % (uint32_t)ERNEST_SIM_TRACE_LEN;
        S->trace_count++;
        S->current_inst = i;

        switch (I->op) {
        case TIR_QREG_DECL:
        case TIR_CREG_DECL:
        case TIR_MEASURE:
        case TIR_RESET:
            /* No-ops in verification mode: declarations contribute
             * to sim_init, measurement and reset are deliberately
             * skipped so we keep the pre-measurement statevector. */
            break;

        case TIR_GATE_H:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_h(S, q0);
            break;
        case TIR_GATE_X:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_x(S, q0);
            break;
        case TIR_GATE_Y:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_y(S, q0);
            break;
        case TIR_GATE_Z:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_z(S, q0);
            break;
        case TIR_GATE_S:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_s(S, q0);
            break;
        case TIR_GATE_T:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_t(S, q0);
            break;
        case TIR_GATE_SDG:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_sdg(S, q0);
            break;
        case TIR_GATE_TDG:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_tdg(S, q0);
            break;
        case TIR_GATE_SX:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            gate_sx(S, q0);
            break;
        case TIR_GATE_RX:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            assert(I->operands[1] < M->num_angles);
            gate_rx(S, q0, M->angles[I->operands[1]]);
            break;
        case TIR_GATE_RY:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            assert(I->operands[1] < M->num_angles);
            gate_ry(S, q0, M->angles[I->operands[1]]);
            break;
        case TIR_GATE_RZ:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            assert(I->operands[1] < M->num_angles);
            gate_rz(S, q0, M->angles[I->operands[1]]);
            break;
        case TIR_GATE_CX:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_cx(S, q0, q1);
            break;
        case TIR_GATE_CZ:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_cz(S, q0, q1);
            break;
        case TIR_GATE_CH:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_ch(S, q0, q1);
            break;
        case TIR_GATE_CP:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            assert(I->operands[2] < M->num_angles);
            gate_cp(S, q0, q1, M->angles[I->operands[2]]);
            break;
        case TIR_GATE_SWAP:
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_swap(S, q0, q1);
            break;
        case TIR_GATE_CCX: {
            uint32_t q2 = tir_flat_qubit_index(M, I->operands[2]);
            q0 = tir_flat_qubit_index(M, I->operands[0]);
            q1 = tir_flat_qubit_index(M, I->operands[1]);
            gate_ccx(S, q0, q1, q2);
            break;
        }

        case TIR_OP_COUNT:
        default:
            sim_set_status(S, SIM_ABEND_S0C1, 0u,
                "UNKNOWN GATE OPCODE %u AT INSTRUCTION %u",
                (unsigned)I->op, (unsigned)i);
            return;
        }

        check_amplitudes_finite(S);
    }
}

/*
 * Pack the classical register into a single unsigned integer, bit
 * 0 being the first classical bit. Each shot becomes one key in
 * the histogram. The caller does the counting.
 */
uint32_t sim_creg_as_uint(const sim_state_t *S)
{
    assert(S != NULL);
    assert(S->num_bits <= (uint32_t)ERNEST_SIM_MAX_BITS);

    uint32_t out = 0u;
    uint32_t n = S->num_bits;
    for (uint32_t i = 0u; i < n; i++) {
        if (S->bits[i] != 0u) {
            out |= (1u << i);
        }
    }
    return out;
}
