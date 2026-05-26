"""Generate the simulator-only DOOM title screen reconstruction PNG.

Replays Ernest's `./ernest doom --wad ...` demo as a Python
script so we can save the result as an image. The mathematics is
identical: QPIE encoding (Yao et al. 2017), unitary (identity, or
QFT-then-IQFT which is also identity), measurement, reconstruct.
The only difference from Ernest's C version is that this one
writes a PNG instead of ASCII to a terminal.

Output: docs/screenshots/doom_titlepic_simulator.png
"""

import struct
import os
import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


WAD_PATH = ("C:/Program Files (x86)/Steam/steamapps/common/"
            "Ultimate Doom/base/DOOM.WAD")
SHOTS = 1_000_000   # matches the LinkedIn-post wording exactly
OUT_PATH = "docs/screenshots/doom_titlepic_simulator.png"


def read_wad_titlepic(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] not in (b"IWAD", b"PWAD"):
        raise ValueError(f"not a WAD: {data[:4]!r}")
    numlumps = struct.unpack("<I", data[4:8])[0]
    infotableofs = struct.unpack("<I", data[8:12])[0]

    title_pos = title_size = None
    pal_pos = None
    for i in range(numlumps):
        entry = data[infotableofs + i * 16: infotableofs + (i + 1) * 16]
        filepos, size = struct.unpack("<II", entry[:8])
        name = entry[8:16].rstrip(b'\x00')
        if name == b"TITLEPIC":
            title_pos, title_size = filepos, size
        elif name == b"PLAYPAL" and pal_pos is None:
            pal_pos = filepos
    if title_pos is None or pal_pos is None:
        raise ValueError("WAD missing TITLEPIC or PLAYPAL")

    pal_data = data[pal_pos:pal_pos + 768]
    palette_luma = np.zeros(256, dtype=np.uint8)
    for i in range(256):
        r, g, b = pal_data[i * 3], pal_data[i * 3 + 1], pal_data[i * 3 + 2]
        palette_luma[i] = min(255, (299 * r + 587 * g + 114 * b) // 1000)

    pic = data[title_pos:title_pos + title_size]
    pw, ph = struct.unpack("<HH", pic[:4])
    col_offsets = struct.unpack(f"<{pw}I", pic[8:8 + pw * 4])
    pic_buf = np.zeros((ph, pw), dtype=np.uint8)
    for col in range(pw):
        p = col_offsets[col]
        while p < len(pic):
            topdelta = pic[p]
            if topdelta == 0xFF:
                break
            length = pic[p + 1]
            for k in range(length):
                y = topdelta + k
                if y < ph:
                    pic_buf[y, col] = palette_luma[pic[p + 3 + k]]
            p += 4 + length
    return pw, ph, pic_buf


def center_fit_256(pw, ph, src):
    """Center-fit source into a 256x256 canvas; matches Ernest's blit_fit."""
    scale_w = 256_000 // pw
    scale_h = 256_000 // ph
    scale = max(1, min(scale_w, scale_h))
    out_w = min(256, (pw * scale) // 1000)
    out_h = min(256, (ph * scale) // 1000)
    off_x = (256 - out_w) // 2
    off_y = (256 - out_h) // 2
    canvas = np.zeros((256, 256), dtype=np.uint8)
    for y in range(out_h):
        sy = (y * ph) // out_h
        for x in range(out_w):
            sx = (x * pw) // out_w
            canvas[off_y + y, off_x + x] = src[sy, sx]
    return canvas


def main():
    print(f"Reading DOOM.WAD ...")
    pw, ph, pic = read_wad_titlepic(WAD_PATH)
    print(f"  TITLEPIC: {pw}x{ph}")

    print("Center-fitting to 256x256 ...")
    img = center_fit_256(pw, ph, pic)

    # QPIE: amplitude[k] = sqrt(brightness[k] / total)
    # |amplitude[k]|^2 = brightness[k] / total = probability of measuring k
    pixels = img.flatten().astype(np.float64)
    total = pixels.sum()
    if total <= 0:
        raise RuntimeError("image is all black")
    probabilities = pixels / total

    print(f"Sampling {SHOTS:,} measurement shots ...")
    rng = np.random.default_rng(0xDEAD)   # deterministic, for the screenshot
    samples = rng.choice(65536, size=SHOTS, p=probabilities)
    counts = np.bincount(samples, minlength=65536)
    recon = counts.reshape(256, 256).astype(np.float64)

    print("Rendering ...")
    fig, axes = plt.subplots(1, 2, figsize=(13, 7.2))

    axes[0].imshow(img, cmap="inferno", interpolation="nearest")
    axes[0].set_title("Input\nDOOM TITLEPIC, fit to 256x256",
                      fontsize=12, pad=10)
    axes[0].set_xticks([])
    axes[0].set_yticks([])

    axes[1].imshow(recon, cmap="inferno", interpolation="nearest")
    axes[1].set_title(f"Reconstructed from {SHOTS:,} quantum measurement shots\n"
                      f"16 qubits, 65,536 amplitudes (Ernest simulator)",
                      fontsize=12, pad=10)
    axes[1].set_xticks([])
    axes[1].set_yticks([])

    fig.suptitle("DOOM through quantum amplitude encoding (QPIE)",
                 fontsize=14, y=0.98)
    fig.tight_layout(rect=(0.0, 0.0, 1.0, 0.94))

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    fig.savefig(OUT_PATH, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
