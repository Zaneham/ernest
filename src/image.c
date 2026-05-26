#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

/*
 * Image module. Stamp DOOM, load PPM, load WAD TITLEPIC, render to
 * ASCII. All three input paths feed the same 256x256 grayscale
 * buffer that the QPIE encoder reads downstream.
 */

void ernest_image_clear(ernest_image_t *img)
{
    assert(img != NULL);
    for (uint32_t i = 0u; i < ERNEST_IMAGE_PIXELS; i++) {
        img->pixels[i] = 0u;
    }
}

/* ---------------------------------------------------------------- */
/* Programmatic DOOM stamper                                        */
/* ---------------------------------------------------------------- */

/*
 * Each character is defined on a 7x9 cell. The bitmap is read
 * left-to-right, top-to-bottom; a '1' is foreground, anything else
 * is background. We scale up by an integer factor when blitting
 * into the 256x256 buffer to get the chunky pixel-font look that
 * survives downsampling to ASCII.
 *
 * Only the four letters DOOM are defined; this is not a general
 * font, and the demo only ever writes those four. Adding more is
 * appending more strings.
 */
typedef struct {
    char glyph;
    const char *rows[9];
} pixel_letter_t;

static const pixel_letter_t LETTERS[] = {
    { 'D', {
        "1111100",
        "1000110",
        "1000011",
        "1000011",
        "1000011",
        "1000011",
        "1000011",
        "1000110",
        "1111100"
    }},
    { 'O', {
        "0111110",
        "1000001",
        "1000001",
        "1000001",
        "1000001",
        "1000001",
        "1000001",
        "1000001",
        "0111110"
    }},
    { 'M', {
        "1000001",
        "1100011",
        "1110111",
        "1011101",
        "1010001",
        "1000001",
        "1000001",
        "1000001",
        "1000001"
    }}
};
#define NUM_LETTERS (sizeof LETTERS / sizeof LETTERS[0])

static const pixel_letter_t *find_letter(char c)
{
    for (uint32_t i = 0u; i < (uint32_t)NUM_LETTERS; i++) {
        if (LETTERS[i].glyph == c) {
            return &LETTERS[i];
        }
    }
    return NULL;
}

/*
 * Blit one letter into the image at the given top-left pixel
 * position, scaled by `scale` in each axis. Foreground pixels get
 * the given brightness; background pixels are left untouched so
 * letters can be stacked over a background gradient.
 */
static void blit_letter(ernest_image_t *img, char c,
                        uint32_t x0, uint32_t y0,
                        uint32_t scale, uint8_t fg)
{
    const pixel_letter_t *L = find_letter(c);
    if (L == NULL) {
        return;
    }
    for (uint32_t row = 0u; row < 9u; row++) {
        const char *r = L->rows[row];
        for (uint32_t col = 0u; col < 7u; col++) {
            if (r[col] != '1') {
                continue;
            }
            uint32_t px0 = x0 + col * scale;
            uint32_t py0 = y0 + row * scale;
            for (uint32_t dy = 0u; dy < scale; dy++) {
                for (uint32_t dx = 0u; dx < scale; dx++) {
                    uint32_t px = px0 + dx;
                    uint32_t py = py0 + dy;
                    if (px < ERNEST_IMAGE_WIDTH && py < ERNEST_IMAGE_HEIGHT) {
                        img->pixels[py * ERNEST_IMAGE_WIDTH + px] = fg;
                    }
                }
            }
        }
    }
}

/*
 * Paint a soft top-to-bottom gradient into the buffer, brighter at
 * the top of the canvas and dimmer at the bottom. Gives the
 * reconstruction something to chew on besides four letters; the
 * gradient shows up clearly in the quantum-measurement histogram as
 * the column statistics fall off with row index.
 */
static void background_gradient(ernest_image_t *img)
{
    for (uint32_t y = 0u; y < ERNEST_IMAGE_HEIGHT; y++) {
        /* Top row about 40, bottom row 0. Linear ramp. */
        uint32_t brightness = 40u - (40u * y) / (ERNEST_IMAGE_HEIGHT - 1u);
        for (uint32_t x = 0u; x < ERNEST_IMAGE_WIDTH; x++) {
            img->pixels[y * ERNEST_IMAGE_WIDTH + x] = (uint8_t)brightness;
        }
    }
}

void ernest_image_stamp_doom(ernest_image_t *img)
{
    assert(img != NULL);

    background_gradient(img);

    /* DOOM = 4 letters, each 7 columns wide, plus 2-column gaps.
     * Total at scale 1: 4*7 + 3*2 = 34 columns.
     * At scale 6: 204 columns. Centre in a 256-wide canvas: x0 = 26.
     * Letter height at scale 6 is 54 pixels. Centre vertically: y0 = 101. */
    const uint32_t scale = 6u;
    const uint32_t letter_w = 7u * scale;
    const uint32_t gap = 2u * scale;
    const uint32_t total_w = 4u * letter_w + 3u * gap;
    const uint32_t x_start = (ERNEST_IMAGE_WIDTH  - total_w) / 2u;
    const uint32_t y_start = (ERNEST_IMAGE_HEIGHT - 9u * scale) / 2u;

    const uint8_t fg = 240u;
    const char *word = "DOOM";
    for (uint32_t i = 0u; i < 4u; i++) {
        uint32_t x0 = x_start + i * (letter_w + gap);
        blit_letter(img, word[i], x0, y_start, scale, fg);
    }
}

/* ---------------------------------------------------------------- */
/* NetPBM loader (P5 grayscale, P6 RGB->grayscale)                  */
/* ---------------------------------------------------------------- */

/* Read the next non-comment whitespace-separated integer from the
 * PPM header. PPM headers allow comments starting with '#' and any
 * whitespace separator. Returns 0 on success. */
static int read_pnm_int(FILE *f, uint32_t *out)
{
    int c;
    /* Skip whitespace and comments. */
    for (;;) {
        c = fgetc(f);
        if (c == EOF) return 1;
        if (isspace(c)) continue;
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') { }
            continue;
        }
        break;
    }
    uint32_t v = 0u;
    int any = 0;
    while (c != EOF && isdigit(c)) {
        v = v * 10u + (uint32_t)(c - '0');
        any = 1;
        c = fgetc(f);
    }
    if (!any) return 2;
    *out = v;
    return 0;
}

/*
 * Centre-fit a source brightness buffer of size sw x sh into the
 * 256x256 image with nearest-neighbour sampling. The source can be
 * any size; we scale uniformly so it fits inside the canvas and
 * black-pad whatever space remains.
 */
static void blit_fit(ernest_image_t *img,
                     const uint8_t *src, uint32_t sw, uint32_t sh)
{
    /* Compute integer scale that fits. */
    uint32_t scale_w = ERNEST_IMAGE_WIDTH  * 1000u / sw;
    uint32_t scale_h = ERNEST_IMAGE_HEIGHT * 1000u / sh;
    uint32_t scale = (scale_w < scale_h) ? scale_w : scale_h;
    if (scale == 0u) scale = 1u;

    uint32_t out_w = (sw * scale) / 1000u;
    uint32_t out_h = (sh * scale) / 1000u;
    if (out_w > ERNEST_IMAGE_WIDTH)  out_w = ERNEST_IMAGE_WIDTH;
    if (out_h > ERNEST_IMAGE_HEIGHT) out_h = ERNEST_IMAGE_HEIGHT;

    uint32_t off_x = (ERNEST_IMAGE_WIDTH  - out_w) / 2u;
    uint32_t off_y = (ERNEST_IMAGE_HEIGHT - out_h) / 2u;

    for (uint32_t y = 0u; y < out_h; y++) {
        uint32_t sy = (y * sh) / out_h;
        for (uint32_t x = 0u; x < out_w; x++) {
            uint32_t sx = (x * sw) / out_w;
            img->pixels[(off_y + y) * ERNEST_IMAGE_WIDTH + off_x + x] =
                src[sy * sw + sx];
        }
    }
}

int ernest_image_load_ppm(ernest_image_t *img, const char *path)
{
    assert(img != NULL);
    assert(path != NULL);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 1;
    }

    char magic[3] = {0, 0, 0};
    if (fread(magic, 1u, 2u, f) != 2u) { (void)fclose(f); return 2; }
    int is_p5 = (magic[0] == 'P' && magic[1] == '5');
    int is_p6 = (magic[0] == 'P' && magic[1] == '6');
    if (!is_p5 && !is_p6) { (void)fclose(f); return 3; }

    uint32_t sw, sh, maxval;
    if (read_pnm_int(f, &sw)     != 0) { (void)fclose(f); return 4; }
    if (read_pnm_int(f, &sh)     != 0) { (void)fclose(f); return 5; }
    if (read_pnm_int(f, &maxval) != 0) { (void)fclose(f); return 6; }
    if (maxval == 0u || maxval > 255u) { (void)fclose(f); return 7; }

    /* The header is followed by a single whitespace byte, then raw
     * pixel data. Consume it. */
    (void)fgetc(f);

    /* Read the source into a heap-free static buffer. We cap source
     * dimensions at 2048x2048 for sanity; anything larger gets
     * rejected. */
    if (sw > 2048u || sh > 2048u) { (void)fclose(f); return 8; }

    static uint8_t src[2048u * 2048u];
    uint32_t pixels = sw * sh;
    if (is_p5) {
        if (fread(src, 1u, pixels, f) != pixels) {
            (void)fclose(f);
            return 9;
        }
    } else {
        /* P6: read RGB triples, fold to grayscale. */
        static uint8_t rgb_buf[2048u * 2048u * 3u];
        if (fread(rgb_buf, 1u, pixels * 3u, f) != pixels * 3u) {
            (void)fclose(f);
            return 10;
        }
        for (uint32_t i = 0u; i < pixels; i++) {
            uint32_t r = rgb_buf[i * 3u + 0u];
            uint32_t g = rgb_buf[i * 3u + 1u];
            uint32_t b = rgb_buf[i * 3u + 2u];
            /* Standard luma. Integer arithmetic with /1000 scale. */
            uint32_t luma = (299u * r + 587u * g + 114u * b) / 1000u;
            if (luma > 255u) luma = 255u;
            src[i] = (uint8_t)luma;
        }
    }
    (void)fclose(f);

    ernest_image_clear(img);
    blit_fit(img, src, sw, sh);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Doom WAD TITLEPIC loader                                         */
/* ---------------------------------------------------------------- */

/*
 * WAD format reference: https://doomwiki.org/wiki/WAD
 * The header is 12 bytes:
 *   char id[4]         "IWAD" or "PWAD"
 *   uint32 numlumps    number of directory entries
 *   uint32 infotableofs  offset to the directory in bytes
 * Each directory entry is 16 bytes:
 *   uint32 filepos     offset to lump data in bytes
 *   uint32 size        size of lump in bytes
 *   char name[8]       lump name, NUL-padded
 *
 * Doom picture format reference: https://doomwiki.org/wiki/Picture_format
 * Picture header is 8 bytes:
 *   uint16 width
 *   uint16 height
 *   int16 leftoffset
 *   int16 topoffset
 * Followed by `width` uint32 column offsets pointing to per-column
 * post data. Each post is:
 *   uint8 topdelta   (Y offset, 0xFF = end of column)
 *   uint8 length     (number of pixel bytes in the post)
 *   uint8 unused     (padding before pixel data)
 *   uint8 pixels[length]  (palette indices)
 *   uint8 unused     (padding after pixel data)
 * The column terminator is a topdelta byte of 0xFF.
 */

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Compare an 8-byte WAD lump name against a string. Lump names are
 * NUL-padded, so we compare up to 8 bytes or the first NUL. */
static int lump_name_eq(const uint8_t *name, const char *target)
{
    for (uint32_t i = 0u; i < 8u; i++) {
        char a = (char)name[i];
        char b = target[i];
        /* Convert to uppercase for case-insensitive comparison. */
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
        if (b == '\0') {
            /* Target ended; remaining WAD name bytes must be NUL or end. */
            for (uint32_t j = i; j < 8u; j++) {
                if (name[j] != 0u) return 0;
            }
            return 1;
        }
    }
    return target[8] == '\0';
}

/* Read an entire file into a malloc-free static buffer. WADs are
 * up to about 30 MB for retail Doom, fine for a one-shot load.
 * We cap at 64 MB. */
#define WAD_MAX_BYTES (64u * 1024u * 1024u)
static uint8_t wad_buf[WAD_MAX_BYTES];

int ernest_image_load_wad_titlepic(ernest_image_t *img, const char *path)
{
    assert(img != NULL);
    assert(path != NULL);

    FILE *f = fopen(path, "rb");
    if (f == NULL) return 1;

    size_t n = fread(wad_buf, 1u, (size_t)WAD_MAX_BYTES, f);
    (void)fclose(f);
    if (n < 12u) return 2;

    /* Header. */
    if (!(wad_buf[0] == 'I' || wad_buf[0] == 'P')
        || wad_buf[1] != 'W'
        || wad_buf[2] != 'A'
        || wad_buf[3] != 'D') {
        return 3;
    }
    uint32_t numlumps     = read_u32_le(&wad_buf[4]);
    uint32_t infotableofs = read_u32_le(&wad_buf[8]);
    if (infotableofs + numlumps * 16u > (uint32_t)n) return 4;

    /* Find TITLEPIC and PLAYPAL lumps. */
    uint32_t title_pos = 0u, title_size = 0u;
    uint32_t pal_pos = 0u, pal_size = 0u;
    int found_title = 0, found_pal = 0;
    for (uint32_t i = 0u; i < numlumps; i++) {
        const uint8_t *e = &wad_buf[infotableofs + i * 16u];
        uint32_t filepos = read_u32_le(&e[0]);
        uint32_t size    = read_u32_le(&e[4]);
        const uint8_t *name = &e[8];
        if (!found_title && lump_name_eq(name, "TITLEPIC")) {
            title_pos = filepos;
            title_size = size;
            found_title = 1;
        }
        if (!found_pal && lump_name_eq(name, "PLAYPAL")) {
            pal_pos = filepos;
            pal_size = size;
            found_pal = 1;
        }
        if (found_title && found_pal) break;
    }
    if (!found_title) return 5;
    if (!found_pal)   return 6;
    if (pal_size < 768u) return 7;
    if (title_pos + title_size > (uint32_t)n) return 8;
    if (pal_pos + 768u > (uint32_t)n) return 9;

    /* Build a 256-entry palette: PLAYPAL is 14 palettes of 768 bytes
     * each; we use the first (palette 0, the normal lighting). */
    static uint8_t palette_luma[256];
    const uint8_t *pal = &wad_buf[pal_pos];
    for (uint32_t i = 0u; i < 256u; i++) {
        uint32_t r = pal[i * 3u + 0u];
        uint32_t g = pal[i * 3u + 1u];
        uint32_t b = pal[i * 3u + 2u];
        uint32_t luma = (299u * r + 587u * g + 114u * b) / 1000u;
        if (luma > 255u) luma = 255u;
        palette_luma[i] = (uint8_t)luma;
    }

    /* Decode the picture. */
    const uint8_t *pic = &wad_buf[title_pos];
    if (title_size < 8u) return 10;
    uint32_t pw = read_u16_le(&pic[0]);
    uint32_t ph = read_u16_le(&pic[2]);
    /* leftoffset and topoffset are signed 16-bit at +4 and +6 but
     * we don't use them for the title pic; the drawing position is
     * always (0,0) on the canvas. */
    if (pw == 0u || ph == 0u || pw > 2048u || ph > 2048u) return 11;
    if (title_size < 8u + pw * 4u) return 12;

    /* Allocate a static decoded buffer. Initialise to palette index
     * 0, which is usually black or close to it. */
    static uint8_t pic_buf[2048u * 2048u];
    uint32_t pic_pixels = pw * ph;
    for (uint32_t i = 0u; i < pic_pixels; i++) pic_buf[i] = 0u;

    /* For each column, walk the posts. */
    for (uint32_t col = 0u; col < pw; col++) {
        uint32_t col_off = read_u32_le(&pic[8u + col * 4u]);
        if (col_off >= title_size) return 13;
        uint32_t p = col_off;
        while (p < title_size) {
            uint8_t topdelta = pic[p];
            if (topdelta == 0xFFu) break;        /* end of column */
            if (p + 1u >= title_size) return 14;
            uint8_t length = pic[p + 1u];
            if (p + 3u + (uint32_t)length + 1u > title_size) return 15;
            /* Skip the unused padding byte at p+2, then read pixels
             * at p+3..p+3+length-1, then skip the unused byte after. */
            for (uint32_t k = 0u; k < length; k++) {
                uint32_t y = topdelta + k;
                if (y < ph) {
                    pic_buf[y * pw + col] = palette_luma[pic[p + 3u + k]];
                }
            }
            p += 4u + (uint32_t)length;
        }
    }

    ernest_image_clear(img);
    blit_fit(img, pic_buf, pw, ph);
    return 0;
}

/* ---------------------------------------------------------------- */
/* ASCII renderer                                                   */
/* ---------------------------------------------------------------- */

/*
 * Brightness ramp from dark to light. Ten characters chosen for
 * roughly uniform optical density steps in a monospace terminal.
 * Index 0 is darkest (space), index 9 is brightest (@).
 */
static const char ASCII_RAMP[] = " .,:;ox%#@";
#define ASCII_RAMP_LEN ((sizeof ASCII_RAMP) - 1u)

void ernest_image_render_ascii(const ernest_image_t *img,
                               FILE *out,
                               uint32_t out_cols, uint32_t out_rows)
{
    assert(img != NULL);
    assert(out != NULL);
    assert(out_cols > 0u && out_cols <= 256u);
    assert(out_rows > 0u && out_rows <= 256u);

    /* For each output cell, average the pixels in the corresponding
     * source block. Block sizes are integer division; we accept the
     * resulting slight bias for output sizes that don't evenly
     * divide the source. */
    uint32_t bw = ERNEST_IMAGE_WIDTH  / out_cols;
    uint32_t bh = ERNEST_IMAGE_HEIGHT / out_rows;
    if (bw == 0u) bw = 1u;
    if (bh == 0u) bh = 1u;

    for (uint32_t cy = 0u; cy < out_rows; cy++) {
        uint32_t y0 = cy * bh;
        for (uint32_t cx = 0u; cx < out_cols; cx++) {
            uint32_t x0 = cx * bw;
            uint32_t sum = 0u;
            uint32_t cnt = 0u;
            for (uint32_t dy = 0u; dy < bh; dy++) {
                if (y0 + dy >= ERNEST_IMAGE_HEIGHT) break;
                for (uint32_t dx = 0u; dx < bw; dx++) {
                    if (x0 + dx >= ERNEST_IMAGE_WIDTH) break;
                    sum += img->pixels[(y0 + dy) * ERNEST_IMAGE_WIDTH + x0 + dx];
                    cnt++;
                }
            }
            uint32_t avg = (cnt > 0u) ? (sum / cnt) : 0u;
            uint32_t idx = (avg * (ASCII_RAMP_LEN - 1u)) / 255u;
            if (idx >= ASCII_RAMP_LEN) idx = ASCII_RAMP_LEN - 1u;
            (void)fputc(ASCII_RAMP[idx], out);
        }
        (void)fputc('\n', out);
    }
}
