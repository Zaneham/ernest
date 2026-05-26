#include "../src/tir.h"
#include "../src/sim.h"
#include "../src/qstd.h"
#include "../src/image.h"
#include "../src/qpie.h"
#include "demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The DOOM demo. Loads an image (a programmatic DOOM stamp by
 * default, optionally a PPM the user supplied, optionally TITLEPIC
 * from a Doom WAD the user owns), encodes it into a sixteen-qubit
 * statevector using QPIE, applies a quantum Fourier transform
 * followed by its inverse on those sixteen qubits, samples a few
 * hundred thousand measurement outcomes from the result, and
 * reconstructs the image from the sample histogram.
 *
 * Two facts get demonstrated by one demo. First, that the QPIE
 * encoding faithfully round-trips through a real quantum
 * transformation. Second, that the QFT and IQFT primitives in
 * libqstd compose to exactly the identity on a non-trivial input,
 * up to the floating-point noise floor and the statistical noise
 * of finite measurement.
 *
 * The output is two ASCII renderings side by side: input and
 * reconstruction. If the reconstruction is recognisable as the
 * input then everything from the parser through to the simulator
 * to the QPIE sampler is doing its job. If it's not, the verifier
 * will tell us which of those parts is responsible.
 *
 * For the IBM-hardware target, the route to actually running this
 * involves SABRE qubit routing (different session), submission via
 * qiskit-ibm-runtime (small Python wrapper), and the IBM machine
 * doing its part. The Doom demo is the input shape that exercises
 * the whole pipeline.
 */

#define DOOM_NUM_QUBITS  16u
#define DOOM_DEFAULT_SHOTS 1000000u
#define DOOM_ASCII_COLS   80u
#define DOOM_ASCII_ROWS   40u

/* Static state, in the style of the rest of the project. */
static ernest_image_t doom_img_in;
static ernest_image_t doom_img_out;
static sim_state_t    doom_sim;
static tir_module_t   doom_module;
static uint32_t       doom_counts[ERNEST_IMAGE_PIXELS];

int run_doom_demo(const char *image_path, const char *wad_path,
                  uint32_t shots)
{
    if (shots == 0u) {
        shots = DOOM_DEFAULT_SHOTS;
    }

    /* Decide where the input comes from. WAD wins over PPM wins
     * over the programmatic stamp; we report the source for the
     * record. */
    ernest_image_clear(&doom_img_in);
    const char *source;
    if (wad_path != NULL) {
        int rc = ernest_image_load_wad_titlepic(&doom_img_in, wad_path);
        if (rc != 0) {
            (void)fprintf(stderr,
                "ernest: failed to load TITLEPIC from %s (rc=%d)\n",
                wad_path, rc);
            return 1;
        }
        source = wad_path;
    } else if (image_path != NULL) {
        int rc = ernest_image_load_ppm(&doom_img_in, image_path);
        if (rc != 0) {
            (void)fprintf(stderr,
                "ernest: failed to load PPM %s (rc=%d)\n",
                image_path, rc);
            return 1;
        }
        source = image_path;
    } else {
        ernest_image_stamp_doom(&doom_img_in);
        source = "programmatic stamp (no WAD or PPM supplied)";
    }

    (void)printf("ERNESTJB DOOM   START\n");
    (void)printf("  source : %s\n", source);
    (void)printf("  qubits : %u  (2^16 = 65536 basis states for "
                 "%u x %u image)\n",
                 (unsigned)DOOM_NUM_QUBITS,
                 (unsigned)ERNEST_IMAGE_WIDTH,
                 (unsigned)ERNEST_IMAGE_HEIGHT);
    (void)printf("  shots  : %u\n", (unsigned)shots);

    /* Input rendering. */
    (void)printf("\n-- INPUT --\n");
    ernest_image_render_ascii(&doom_img_in, stdout,
                              DOOM_ASCII_COLS, DOOM_ASCII_ROWS);

    /* Encode into the statevector. After this call, every pixel's
     * brightness is the squared amplitude of one basis state. */
    (void)printf("\nERNESTJB DOOM   QPIE encoding\n");
    ernest_qpie_encode(&doom_img_in, &doom_sim, DOOM_NUM_QUBITS);

    /* Build a TIR module that holds QFT followed by IQFT on the
     * sixteen-qubit register. The composition is exactly identity,
     * so applying it to our encoded state should leave the state
     * unchanged up to floating-point rounding. */
    tir_module_init(&doom_module, "doom_qft_iqft");
    uint32_t q = tir_qreg(&doom_module, "q", DOOM_NUM_QUBITS);
    qstd_qft (&doom_module, q, DOOM_NUM_QUBITS);
    qstd_iqft(&doom_module, q, DOOM_NUM_QUBITS);

    (void)printf("ERNESTJB DOOM   QFT . IQFT  insts=%u\n",
                 (unsigned)doom_module.num_insts);

    /* Apply the unitary. Uses sim_run_unitary_only so the state
     * does not collapse and we can sample from it after. */
    clock_t t_unitary_0 = clock();
    sim_run_unitary_only(&doom_sim, &doom_module);
    clock_t t_unitary_1 = clock();
    if (doom_sim.status != SIM_OK) {
        (void)fprintf(stderr,
            "ernest: simulator ABEND during QFT.IQFT: %s\n",
            doom_sim.status_reason);
        return 8;
    }
    double unitary_secs =
        (double)(t_unitary_1 - t_unitary_0) / (double)CLOCKS_PER_SEC;
    (void)printf("ERNESTJB DOOM   unitary applied  %.3fs\n", unitary_secs);

    /* Sample. We seed with a fixed value so the demo is
     * reproducible; pass any byte through srand if you'd prefer
     * fresh randomness. */
    srand((unsigned)0x44ull);
    clock_t t_sample_0 = clock();
    ernest_qpie_sample(&doom_sim, shots, doom_counts,
                       ERNEST_IMAGE_PIXELS);
    clock_t t_sample_1 = clock();
    double sample_secs =
        (double)(t_sample_1 - t_sample_0) / (double)CLOCKS_PER_SEC;
    (void)printf("ERNESTJB DOOM   sampling  %.3fs\n", sample_secs);

    /* Reconstruct. The brightest measured pixel becomes 255 in the
     * output. */
    ernest_image_clear(&doom_img_out);
    ernest_qpie_reconstruct(doom_counts, ERNEST_IMAGE_PIXELS,
                            &doom_img_out);

    /* Reconstruction rendering. */
    (void)printf("\n-- RECONSTRUCTED  (after QFT then IQFT, "
                 "%u measurement shots) --\n", (unsigned)shots);
    ernest_image_render_ascii(&doom_img_out, stdout,
                              DOOM_ASCII_COLS, DOOM_ASCII_ROWS);

    (void)printf("\nERNESTJB DOOM   ENDED  RC=0\n");
    return 0;
}
