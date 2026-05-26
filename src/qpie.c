#include "qpie.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * QPIE implementation. Three functions: encode, sample, reconstruct.
 * The maths is in the comments next to each one.
 */

void ernest_qpie_encode(const ernest_image_t *img, sim_state_t *S,
                        uint32_t num_qubits)
{
    assert(img != NULL);
    assert(S != NULL);
    assert(num_qubits > 0u);
    assert(num_qubits <= (uint32_t)ERNEST_SIM_MAX_QUBITS);

    uint32_t dim = 1u << num_qubits;
    uint32_t pixels = ERNEST_IMAGE_PIXELS;
    if (pixels > dim) pixels = dim;

    /* Compute the total brightness so we can normalise. The
     * statevector's L2 norm must equal one, which means each
     * amplitude is sqrt(intensity / total). Without the
     * normalisation the simulator's downstream sanity checks would
     * justifiably complain. */
    double total = 0.0;
    for (uint32_t i = 0u; i < pixels; i++) {
        total += (double)img->pixels[i];
    }

    /* Reset the rest of the simulator state. We do not call
     * sim_init because that hardwires |0...0>; we want our own
     * amplitudes. */
    memset(S, 0, sizeof *S);
    S->num_qubits = num_qubits;
    S->num_bits   = num_qubits;
    S->status     = SIM_OK;

    if (total <= 0.0) {
        /* Pathological case: image is entirely black. Encode |0> so
         * the simulator has a valid normalised state to work with.
         * Reconstruction will look uniformly black, which is what
         * the input was anyway. */
        S->state[0].re = 1.0;
        return;
    }

    double inv_sqrt_total = 1.0 / sqrt(total);
    for (uint32_t i = 0u; i < pixels; i++) {
        double v = (double)img->pixels[i];
        S->state[i].re = sqrt(v) * inv_sqrt_total;
        S->state[i].im = 0.0;
    }
    /* Any extra basis states beyond `pixels` are already zero from
     * the memset above. */
}

/*
 * Sample from the statevector. We compute a cumulative probability
 * vector once and then do `shots` binary searches into it. Each
 * search is O(log dim), each computation is O(1), total is
 * O(dim + shots * log dim).
 *
 * Why this is mathematically equivalent to physical measurement.
 * In the physical setup, after each measurement the state collapses
 * to a single basis state, and to measure again the experimenter
 * must re-prepare the state and re-run the unitary. Between shots,
 * the state is identical. The probability of outcome k on any
 * single shot is |amp_k|^2. Drawing N independent samples from that
 * distribution gives the same count statistics as N round-trips of
 * prepare, evolve, measure.
 */
void ernest_qpie_sample(const sim_state_t *S, uint32_t shots,
                        uint32_t *counts, uint32_t num_outcomes)
{
    assert(S != NULL);
    assert(counts != NULL);

    uint32_t dim = 1u << S->num_qubits;
    if (dim > num_outcomes) dim = num_outcomes;

    for (uint32_t i = 0u; i < num_outcomes; i++) {
        counts[i] = 0u;
    }

    /* Cumulative probability table. We allocate it on the stack via
     * a static buffer to stay within the no-malloc style of the
     * rest of the project. Max size is 2^16 entries. */
    static double cumulative[1u << ERNEST_SIM_MAX_QUBITS];
    double running = 0.0;
    for (uint32_t k = 0u; k < dim; k++) {
        double re = S->state[k].re;
        double im = S->state[k].im;
        running += re * re + im * im;
        cumulative[k] = running;
    }

    /* Normalise to exactly one in case of accumulated rounding. */
    if (running > 0.0) {
        double scale = 1.0 / running;
        for (uint32_t k = 0u; k < dim; k++) {
            cumulative[k] *= scale;
        }
    }

    for (uint32_t s = 0u; s < shots; s++) {
        double r = (double)rand() / (double)RAND_MAX;
        /* Binary search for the first cumulative value >= r. */
        uint32_t lo = 0u, hi = dim - 1u;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (cumulative[mid] < r) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        counts[lo]++;
    }
}

void ernest_qpie_reconstruct(const uint32_t *counts, uint32_t num_outcomes,
                             ernest_image_t *img)
{
    assert(counts != NULL);
    assert(img != NULL);

    uint32_t n = num_outcomes;
    if (n > ERNEST_IMAGE_PIXELS) n = ERNEST_IMAGE_PIXELS;

    /* Find the max count so we can scale the brightest pixel to
     * 255. This is the equivalent of an auto-exposure on a camera:
     * we don't know the absolute scale, but the relative scale is
     * what makes the image recognisable. */
    uint32_t max_count = 1u;
    for (uint32_t i = 0u; i < n; i++) {
        if (counts[i] > max_count) max_count = counts[i];
    }

    for (uint32_t i = 0u; i < n; i++) {
        uint32_t v = (counts[i] * 255u) / max_count;
        if (v > 255u) v = 255u;
        img->pixels[i] = (uint8_t)v;
    }
    /* Pixels beyond `n` left at their prior values, which would be
     * whatever was in the image buffer before. Caller is expected
     * to ernest_image_clear before reconstructing if they want a
     * clean black background. */
}
