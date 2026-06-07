#ifndef ERNEST_QPIE_H
#define ERNEST_QPIE_H

#include "image.h"
#include "sim.h"

/*
 * Quantum Probability Image Encoding (QPIE).
 *
 * Three functions that move pixels in and out of the simulator's
 * statevector. Encode writes a brightness buffer in as amplitudes;
 * sample draws measurement outcomes from the encoded distribution;
 * reconstruct turns a histogram of outcomes back into a picture.
 *
 * The encoding is the simplest one going. Take each pixel's
 * intensity, divide by the image's total intensity, square-root it,
 * and that is the amplitude of one basis state. All real, all
 * non-negative. Measuring |k> then comes up with probability
 * intensity_k / total, so a measurement is just a draw from the
 * image's own brightness distribution. The picture becomes a
 * probability cloud, and you read it back by looking at it.
 *
 * The fancier cousins, for context: FRQI (Le, Dong & Hirota, 2011)
 * spends 2n+1 qubits and carries colour as a rotation angle; NEQR
 * (Zhang et al., 2013) packs 8-bit colour into eight more qubits in
 * the computational basis. This is the amplitude-only variant, QPIE
 * in Yao et al. (2017). Full citations live in REFERENCES.md.
 *
 * For a 256x256 image, n+n = 16 qubits, which is exactly the
 * simulator's cap. A careful engineer notices a coincidence like
 * that; a less careful one designs around it. The numbers were here
 * first.
 */

/*
 * Encode an image into the simulator state. Sets num_qubits to the
 * requested value, zeroes the trace and the classical register,
 * normalises the image's brightness vector to unit norm, and writes
 * each amplitude into the corresponding basis-state slot. Pixels
 * beyond the addressable range (2^num_qubits) are silently dropped;
 * the function expects ERNEST_IMAGE_PIXELS to equal 2^num_qubits
 * for clean encoding.
 *
 * After this call S is a valid statevector representation of the
 * image. The caller can apply unitary transformations through the
 * usual simulator entry points and then either sample with
 * ernest_qpie_sample or measure with the standard simulator
 * measurement.
 */
void ernest_qpie_encode(const ernest_image_t *img, sim_state_t *S,
                        uint32_t num_qubits);

/*
 * Sample `shots` outcomes from the statevector's probability
 * distribution and accumulate them into the counts array, which
 * must be at least num_outcomes long. Does not collapse the state;
 * the function is a faster equivalent of repeatedly re-encoding,
 * running the unitary, and measuring once. Caller is responsible
 * for seeding rand() before calling.
 *
 * The mathematics is identical to the physical measurement-and-
 * re-prepare cycle because all measurements are in the
 * computational basis and the state is unchanged between physical
 * shots. For a 16-qubit, million-shot demo this saves about a
 * minute of wall-clock time over the naive approach.
 */
void ernest_qpie_sample(const sim_state_t *S, uint32_t shots,
                        uint32_t *counts, uint32_t num_outcomes);

/*
 * Reconstruct an image from a measurement count histogram. The
 * brightest pixel in the reconstruction is scaled to 255; every
 * other pixel scales linearly with its count. Pixels that received
 * zero counts come back as zero brightness, which is the visually
 * sensible thing on a black background.
 */
void ernest_qpie_reconstruct(const uint32_t *counts, uint32_t num_outcomes,
                             ernest_image_t *img);

#endif /* ERNEST_QPIE_H */
