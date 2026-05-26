"""So you wanna run DOOM on your supercomputer, the real version.

The other DOOM script (run_ernest_doom_hw.py) uses a hand-drawn 'D'
because at 16 pixels there is nothing recognisably Doom in the
actual WAD anyway. This script makes the opposite trade. It reads
the real TITLEPIC lump out of your own DOOM.WAD, crops to the top
strip where the logo lives, downsamples to 4x8 = 32 pixels which
fits comfortably on five qubits, and runs THAT through Ernest's
IBM compilation pipeline onto a real quantum computer. The pixels
are authentic. The reconstruction is noisier. You can choose your
fighter.

The script defaults to the Steam install path for Ultimate Doom on
Windows. Edit WAD_PATH below if you have it somewhere else, or
point it at DOOM2.WAD or a FREEDOOM IWAD; the format is the same
across vendors. We don't ship Bethesda's content. You bring your
own WAD.

The output PNG has four panels: the source TITLEPIC strip (in which
you can read 'THE ULTIMATE DOOM' with your own eyes), the 4x8
downsampled input to the circuit, the histogram reconstruction
after passing through real silicon, and the raw measurement
distribution. The bottom half of the reconstruction tends to be
suppressed by the chip's readout asymmetry, which biases the high-
weight qubit toward |0> at measurement time. That's not Ernest's
fault and it's not the algorithm's fault, it's the limits of the
hardware showing through. Today's superconducting qubits don't owe
you anything; tomorrow's might.
"""

import struct
import numpy as np
import subprocess
import sys
import time
import datetime

from qiskit import QuantumCircuit, qasm3, transpile
from qiskit.circuit.library import QFT
from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2 as Sampler

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


WAD_PATH = ("C:/Program Files (x86)/Steam/steamapps/common/"
            "Ultimate Doom/base/DOOM.WAD")


# ----- Tiny inline WAD reader --------------------------------------

def read_wad_titlepic(path):
    """Returns the TITLEPIC as a (width, height, grayscale ndarray)."""
    with open(path, "rb") as f:
        data = f.read()
    magic = data[:4]
    if magic not in (b"IWAD", b"PWAD"):
        raise ValueError(f"not a WAD: {magic!r}")
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

    # Palette: first 256 RGB triples, fold to luma.
    pal_data = data[pal_pos:pal_pos + 768]
    palette_luma = np.zeros(256, dtype=np.uint8)
    for i in range(256):
        r = pal_data[i * 3]
        g = pal_data[i * 3 + 1]
        b = pal_data[i * 3 + 2]
        palette_luma[i] = min(255, (299 * r + 587 * g + 114 * b) // 1000)

    # Picture: column-major with vertical posts.
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
            # post layout: topdelta, length, unused, pixels[length], unused
            for k in range(length):
                y = topdelta + k
                if y < ph:
                    pic_buf[y, col] = palette_luma[pic[p + 3 + k]]
            p += 4 + length

    return pw, ph, pic_buf


# ----- Load and downsample ----------------------------------------

print("=== Reading DOOM.WAD ===")
pw, ph, pic = read_wad_titlepic(WAD_PATH)
print(f"  TITLEPIC: {pw}x{ph}, brightness {pic.min()}..{pic.max()}")

# Crop to roughly where the DOOM logo lives in TITLEPIC.
# Empirically that's the top ~25% of the picture vertically.
crop_top = 0
crop_bot = int(ph * 0.35)
strip = pic[crop_top:crop_bot, :].astype(float)
print(f"  Cropped to logo strip: {strip.shape}")

# Downsample to 4 tall x 8 wide = 32 cells = 5 qubits.
TARGET_H, TARGET_W = 4, 8
strip_h, strip_w = strip.shape
target = np.zeros((TARGET_H, TARGET_W), dtype=float)
for ty in range(TARGET_H):
    for tx in range(TARGET_W):
        y0 = ty * strip_h // TARGET_H
        y1 = (ty + 1) * strip_h // TARGET_H
        x0 = tx * strip_w // TARGET_W
        x1 = (tx + 1) * strip_w // TARGET_W
        target[ty, tx] = strip[y0:y1, x0:x1].mean()

print(f"  Downsampled to {TARGET_H}x{TARGET_W}, "
      f"range {target.min():.1f}..{target.max():.1f}")

# Add a small baseline so all amplitudes are positive (state prep
# wants non-zero entries; lifts dark pixels above the floor).
target_flat = target.flatten() + 1.0
amplitudes = np.sqrt(target_flat / target_flat.sum())

# Show input pattern as ASCII so the user can sanity-check before
# spending QPU time.
target_norm = target / target.max() if target.max() > 0 else target
print()
print("=== Input pattern (4x8 from actual DOOM TITLEPIC) ===")
for row in range(TARGET_H):
    line = "  "
    for col in range(TARGET_W):
        v = target_norm[row, col]
        line += "##" if v > 0.75 else "%%" if v > 0.5 else "::" if v > 0.25 else ".."
        line += " "
    print(line)


# ----- Build circuit ----------------------------------------------

print()
print("=== Building circuit ===")
N_QUBITS = 5
qc = QuantumCircuit(N_QUBITS, N_QUBITS)
qc.prepare_state(amplitudes, range(N_QUBITS))
qc.append(QFT(N_QUBITS, do_swaps=True), range(N_QUBITS))
qc.append(QFT(N_QUBITS, do_swaps=True).inverse(), range(N_QUBITS))
qc.measure(range(N_QUBITS), range(N_QUBITS))

qc_basis = transpile(qc, basis_gates=['rx', 'ry', 'rz', 'cx', 'h'],
                     optimization_level=1)
print(f"  Qiskit decomposed: {qc_basis.size()} gates")

qasm_in = qasm3.dumps(qc_basis)
with open('wad_doom.qasm', 'w') as f:
    f.write(qasm_in)


# ----- Push through Ernest ----------------------------------------

print()
print("=== Ernest pipeline ===")
result = subprocess.run(
    ['./ernest.exe', '--target=ibm', 'compile', 'wad_doom.qasm'],
    capture_output=True, text=True
)
if result.returncode != 0:
    print("Ernest failed:")
    print(result.stdout[-3000:])
    print(result.stderr)
    sys.exit(1)

out = result.stdout
start_idx = out.find('-- OpenQASM 3.0 --')
end_idx = out.find('ERNESTJB COMPILE ENDED', start_idx if start_idx >= 0 else 0)
if start_idx < 0 or end_idx < 0:
    print("Could not locate QASM in Ernest's output")
    print(result.stdout[-2000:])
    sys.exit(1)
ernest_qasm = out[start_idx + len('-- OpenQASM 3.0 --'):end_idx].strip() + "\n"
with open('wad_doom_ibm.qasm', 'w') as f:
    f.write(ernest_qasm)

gate_lines = [l for l in ernest_qasm.split('\n')
              if l.strip() and not l.startswith(('OPENQASM', 'include',
                                                  'qubit', 'bit', '//'))]
print(f"  Ernest IBM-native: {len(gate_lines)} gate lines")


# ----- Submit -----------------------------------------------------

print()
print("=== Submitting to IBM ===")
qc_ernest = qasm3.loads(ernest_qasm)
service = QiskitRuntimeService(channel="ibm_cloud")
backend = service.least_busy(simulator=False, operational=True)
print(f"  Backend: {backend.name} "
      f"({backend.num_qubits}q, queue={backend.status().pending_jobs})")

qc_final = transpile(qc_ernest, backend, optimization_level=1)
print(f"  After IBM transpile: {qc_final.size()} gates, depth {qc_final.depth()}")

SHOTS = 16000
sampler = Sampler(backend)
job = sampler.run([qc_final], shots=SHOTS)
print(f"  Job id: {job.job_id()}")

print()
print("=== Waiting ===")
t0 = time.time()
result = job.result()
print(f"  Done in {time.time() - t0:.1f}s")


# ----- Reconstruct ------------------------------------------------

pub = result[0]
if hasattr(pub.data, 'c'):
    counts = pub.data.c.get_counts()
else:
    for attr in dir(pub.data):
        if not attr.startswith('_'):
            fld = getattr(pub.data, attr)
            if hasattr(fld, 'get_counts'):
                counts = fld.get_counts()
                break

total = sum(counts.values())
recon = np.zeros(32, dtype=float)
for outcome_str, c in counts.items():
    idx = int(outcome_str.replace(' ', ''), 2)
    if idx < 32:
        recon[idx] = float(c)

recon_grid = recon.reshape(TARGET_H, TARGET_W)
recon_norm = recon_grid / recon_grid.max() if recon_grid.max() > 0 else recon_grid

print()
print(f"=== Reconstructed ({total} shots from real hardware) ===")
for row in range(TARGET_H):
    line = "  "
    for col in range(TARGET_W):
        v = recon_norm[row, col]
        line += "##" if v > 0.75 else "%%" if v > 0.5 else "::" if v > 0.25 else ".."
        line += " "
    print(line)


# ----- Save image -------------------------------------------------

stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
png_path = f"doom_wad_hw_{stamp}.png"

fig, axes = plt.subplots(1, 4, figsize=(16, 4.2),
                         gridspec_kw={'width_ratios': [1.5, 1, 1, 1.2]})

# Far left: the full original TITLEPIC strip we cropped from
axes[0].imshow(strip, cmap="inferno", interpolation="nearest")
axes[0].set_title(f"Source: DOOM.WAD\nTITLEPIC top strip\n({strip.shape[1]}x{strip.shape[0]} px)",
                  fontsize=10)
axes[0].set_xticks([]); axes[0].set_yticks([])

# Mid-left: downsampled input
axes[1].imshow(target_norm, cmap="inferno", interpolation="nearest", vmin=0, vmax=1)
axes[1].set_title("Downsampled to 4x8\n(input to the circuit)", fontsize=10)
axes[1].set_xticks([]); axes[1].set_yticks([])

# Mid-right: hardware reconstruction
axes[2].imshow(recon_norm, cmap="inferno", interpolation="nearest", vmin=0, vmax=1)
axes[2].set_title(f"Reconstructed from\n{total} shots on {backend.name}",
                  fontsize=10)
axes[2].set_xticks([]); axes[2].set_yticks([])

# Far right: measurement distribution
outcomes = [f"{i:05b}" for i in range(32)]
axes[3].barh(range(32), recon / total, color="firebrick")
axes[3].set_yticks(range(32))
axes[3].set_yticklabels(outcomes, fontsize=6, family="monospace")
axes[3].invert_yaxis()
axes[3].set_xlabel("Probability")
axes[3].set_title("Histogram", fontsize=10)
axes[3].grid(axis="x", alpha=0.3)

fig.suptitle(
    f"WAD pixels on {backend.name} - "
    f"{qc_final.size()} native gates, depth {qc_final.depth()}, "
    f"{total} shots",
    fontsize=11
)
fig.tight_layout()
fig.savefig(png_path, dpi=150, bbox_inches="tight")
plt.close(fig)

print()
print(f"Wrote {png_path}")
print()
print("Bethesda's title pic. Through a quantum computer in DC.")
