#ifndef ERNEST_IMAGE_HEADER
#define ERNEST_IMAGE_HEADER

#include <stdint.h>
#include <stdio.h>

/*
 * A tiny image type for the quantum-rendering demo. Yes, this is the
 * part where Doom's title screen gets encoded into a 16-qubit
 * statevector. It runs on everything else, so it was only a matter of
 * time.
 *
 * 256x256 = 65536 pixels, which is exactly 2^16, so each pixel's
 * brightness becomes the amplitude on one basis state of a 16-qubit
 * register. The simulator caps at 16 qubits; the Doom title screen is
 * 320x200, so it gets downsampled to fit. That Doom's pixel count and
 * a 16-qubit register's basis count land this close is a coincidence
 * flimsy enough to justify an entire project, which is exactly what
 * happened here.
 *
 * Brightness is 8-bit grayscale. Zero is black. The encoding
 * normalises to a unit statevector so the absolute scale doesn't
 * matter; what matters is the ratio between pixel brightnesses,
 * which becomes the ratio of probabilities at measurement time.
 *
 * Three sources of pixels in v1: a programmatic stamper that writes
 * "DOOM" into the buffer in chunky pixels (no IP concerns, demo
 * runs out of the box), a NetPBM .ppm loader for anyone who wants
 * to feed it their own image, and a Doom WAD loader that pulls the
 * TITLEPIC lump from a legally-acquired IWAD on the user's disk.
 * The three feed the same downstream pipeline.
 */

#define ERNEST_IMAGE_WIDTH   256u
#define ERNEST_IMAGE_HEIGHT  256u
#define ERNEST_IMAGE_PIXELS  (ERNEST_IMAGE_WIDTH * ERNEST_IMAGE_HEIGHT)

typedef struct {
    uint8_t pixels[ERNEST_IMAGE_PIXELS];   /* row-major, brightness 0..255 */
} ernest_image_t;

/* Zero the buffer. After this call every pixel is black. */
void ernest_image_clear(ernest_image_t *img);

/*
 * Stamp the word DOOM into the image in a chunky pixel font. The
 * letters are defined as bitmap patterns and blitted at a size that
 * survives the round-trip through quantum encoding and ASCII
 * downsampling. Includes a faint background gradient and a couple
 * of period-appropriate decorative pixels around the edges so the
 * reconstruction has something to look like besides four letters
 * floating in space.
 */
void ernest_image_stamp_doom(ernest_image_t *img);

/*
 * Load a NetPBM image from disk. Supports P5 (binary grayscale)
 * and P6 (binary RGB, converted to grayscale via standard luma
 * weights). The file's image is centre-fitted into the 256x256
 * buffer with nearest-neighbour sampling. Anything not covered is
 * left at its prior value (call ernest_image_clear first if you
 * want a black background). Returns 0 on success, non-zero on
 * any I/O or parse error.
 */
int ernest_image_load_ppm(ernest_image_t *img, const char *path);

/*
 * Load the TITLEPIC lump from a Doom IWAD. Parses the WAD header
 * and directory, locates the lump named TITLEPIC, decodes the Doom
 * picture format (column-major with vertical posts), looks up
 * palette colours from the included PLAYPAL lump, and converts to
 * grayscale. The 320x200 original is centre-fitted into the 256x256
 * buffer with black padding. Returns 0 on success, non-zero if the
 * file is not a valid IWAD or PWAD, lacks a TITLEPIC, or lacks a
 * usable PLAYPAL.
 *
 * Bring your own WAD. Ernest does not ship id's content with a
 * quantum compiler, for reasons that should not need spelling out.
 * Point this at a legally-acquired DOOM.WAD, DOOM1.WAD, DOOM2.WAD, or
 * a FREEDOOM IWAD. The WAD format is identical between vendors, so
 * all of them work.
 */
int ernest_image_load_wad_titlepic(ernest_image_t *img, const char *path);

/*
 * Render the image to a stream as ASCII art. Downsamples by block
 * averaging from 256x256 to the requested character grid (typically
 * 80x40 for a comfortable terminal width). Each averaged brightness
 * value selects from a character ramp from dark to light. Useful
 * for showing the input image, showing the reconstructed image, or
 * showing two side by side via two calls and a divider.
 */
void ernest_image_render_ascii(const ernest_image_t *img,
                               FILE *out,
                               uint32_t out_cols, uint32_t out_rows);

#endif /* ERNEST_IMAGE_HEADER */
