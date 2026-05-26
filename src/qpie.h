#ifndef ERNEST_QPIE_H
#define ERNEST_QPIE_H

#include "image.h"
#include "sim.h"

/*
 * Quantum Probability Image Encoding.
 *
 * A small library of three functions that move pixels in and out of
 * the simulator's statevector. Encoding takes a brightness buffer
 * and writes amplitudes; sampling draws measurement outcomes from
 * the encoded distribution; reconstruction turns a measurement
 * histogram back into a brightness buffer.
 *
 * The encoding is the simplest one in the quantum-image-processing
 * literature: each pixel's intensity, divided by the total
 * intensity of the image and then square-rooted, becomes the
 * amplitude of one computational-basis state. All amplitudes are
 * real and non-negative. The probability of measuring basis state
 * |k> is intensity_k / total_intensity, which means a measurement
 * sample is a sample from the image's normalised brightness
 * distribution.
 *
 * Related encodings, briefly, so the reader knows what we are not
 * doing here. FRQI (Le, Dong, & Hirota, 2011) uses 2n+1 qubits for
 * a 2^n by 2^n image, with n+n for position and one ancilla
 * carrying colour as a rotation angle. NEQR (Zhang, Lu, Gao, &
 * Wang, 2013) keeps the position register but encodes 8-bit colour
 * into eight extra qubits in the computational basis. The encoding
 * here is the amplitude-only variant, also called QPIE in
 * Yao et al. (2017).
 *
 * APA-style references go in a project-level REFERENCES.md when we
 * do the citation pass.
 *
 * For a 256 by 256 image, n+n = 16 qubits, which is the simulator's
 * cap. The coincidence is the kind of thing a careful engineer
 * notices and a less careful one designs around. The numbers were
 * here first.
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
