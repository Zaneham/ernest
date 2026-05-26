"""So you wanna run DOOM on your supercomputer.

You can't. Not really. Not the way you think. The full TITLEPIC
from your DOOM.WAD is 320 by 200 pixels, which is 65 thousand
amplitudes, which is sixteen qubits, which is fine. The trouble is
arranging those amplitudes on the chip. Loading an arbitrary state
into sixteen qubits takes roughly 2^16 gates of state-preparation
circuitry, and today's superconducting hardware loses coherence in
something like five hundred gates if you're being optimistic.

So we shrink. This script encodes a hand-drawn 4x4 'D' as a four-
qubit amplitude pattern, sends the whole package - state prep plus
a QFT/inverse-QFT pair plus measurement - through Ernest's IBM
pipeline, submits to whatever IBM Quantum backend is least busy,
and reconstructs the image from the measurement histogram. The 'D'
is not from the WAD because at sixteen pixels there's nothing in
the WAD that survives to recognisable form anyway. It's a chunky
hand-rendered letter so the picture coming back from real silicon
is unambiguous when it does come back.

For actual WAD pixels on real hardware see run_ernest_wad_hw.py.
That one cheats less and looks worse.

This script writes a timestamped PNG with three panels: the ideal
input, the hardware reconstruction, and the measurement histogram.
"""

import numpy as np
import subprocess
import sys
import time
import datetime

from qiskit import QuantumCircuit, qasm3, transpile
from qiskit.circuit.library import QFT
from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2 as Sampler

import matplotlib
matplotlib.use("Agg")   # write to file, no GUI required
import matplotlib.pyplot as plt


# ----- Input: a 4x4 'D' brightness pattern --------------------------
# 9 bright pixels in a D shape, 7 dark. Background brightness 0.1
# keeps the dark pixels above the noise floor so the reconstruction
# can tell "dark" from "absent".
pattern = np.array([
    1, 1, 1, 0,
    1, 0, 0, 1,
    1, 0, 0, 1,
    1, 1, 1, 0,
], dtype=float)
pattern = pattern * 0.9 + 0.1
amplitudes = np.sqrt(pattern / pattern.sum())

print("=== Input pattern (4x4 'D') ===")
for row in range(4):
    line = "  "
    for col in range(4):
        line += "##" if pattern[row * 4 + col] > 0.5 else ".."
        line += " "
    print(line)
print()


# ----- Build the circuit -------------------------------------------
print("=== Building circuit ===")
qc = QuantumCircuit(4, 4)
qc.prepare_state(amplitudes, range(4))
qc.append(QFT(4, do_swaps=True), range(4))
qc.append(QFT(4, do_swaps=True).inverse(), range(4))
qc.measure(range(4), range(4))

# Decompose to a basis Ernest can parse end-to-end.
qc_basis = transpile(qc, basis_gates=['rx', 'ry', 'rz', 'cx', 'h'],
                     optimization_level=1)
print(f"  Qiskit decomposed to: {qc_basis.size()} gates "
      f"in basis {{rx, ry, rz, cx, h}}")

# Write QASM for Ernest to consume.
qasm_in = qasm3.dumps(qc_basis)
with open('mini_doom.qasm', 'w') as f:
    f.write(qasm_in)
print(f"  Wrote mini_doom.qasm ({len(qasm_in)} bytes)")


# ----- Push through Ernest pipeline --------------------------------
print()
print("=== Ernest pipeline ===")
result = subprocess.run(
    ['./ernest.exe', '--target=ibm',
     'compile', 'mini_doom.qasm'],
    capture_output=True, text=True
)
if result.returncode != 0:
    print("Ernest failed:")
    print(result.stdout)
    print(result.stderr)
    sys.exit(1)

# Extract the OpenQASM 3 section from Ernest's stdout.
out = result.stdout
start_marker = '-- OpenQASM 3.0 --'
end_marker = 'ERNESTJB COMPILE ENDED'
start_idx = out.find(start_marker)
end_idx = out.find(end_marker, start_idx if start_idx >= 0 else 0)
if start_idx < 0 or end_idx < 0:
    print("Could not locate QASM in Ernest's output")
    print(result.stdout[-2000:])
    sys.exit(1)
ernest_qasm = out[start_idx + len(start_marker):end_idx].strip() + "\n"
with open('mini_doom_ibm.qasm', 'w') as f:
    f.write(ernest_qasm)
ernest_gate_lines = [line for line in ernest_qasm.split('\n')
                     if line.strip() and
                     not line.startswith(('OPENQASM', 'include', 'qubit', 'bit', '//'))]
print(f"  Ernest IBM-native output: {len(ernest_gate_lines)} gate lines")


# ----- Submit ------------------------------------------------------
print()
print("=== Submitting to IBM ===")
qc_ernest = qasm3.loads(ernest_qasm)
service = QiskitRuntimeService(channel="ibm_cloud")
backend = service.least_busy(simulator=False, operational=True)
print(f"  Backend: {backend.name} "
      f"({backend.num_qubits} qubits, queue={backend.status().pending_jobs})")

qc_final = transpile(qc_ernest, backend, optimization_level=1)
print(f"  After IBM transpile: {qc_final.size()} gates, "
      f"depth {qc_final.depth()}")

SHOTS = 16000
sampler = Sampler(backend)
job = sampler.run([qc_final], shots=SHOTS)
print(f"  Job id: {job.job_id()}")

print()
print("=== Waiting on real hardware ===")
t0 = time.time()
result = job.result()
print(f"  Done in {time.time() - t0:.1f}s")


# ----- Reconstruct -------------------------------------------------
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
recon = np.zeros(16, dtype=float)
for outcome_str, c in counts.items():
    # Strip any whitespace and parse as binary
    idx = int(outcome_str.replace(' ', ''), 2)
    if idx < 16:
        recon[idx] = float(c)

print()
print(f"=== Result: {total} shots from real hardware ===")
print()

# Scale to [0, 1]
recon_max = recon.max() if recon.max() > 0 else 1.0
recon_norm = recon / recon_max

print("Reconstructed image (from quantum measurements):")
for row in range(4):
    line = "  "
    for col in range(4):
        val = recon_norm[row * 4 + col]
        if val > 0.66:
            line += "##"
        elif val > 0.33:
            line += "%%"
        elif val > 0.15:
            line += "::"
        else:
            line += ".."
        line += " "
    print(line)

print()
print("Raw histogram (1 bar = 1% of shots):")
for outcome in sorted(counts.keys()):
    c = counts[outcome]
    bar_len = int(c * 50 / total + 0.5)
    bar = '#' * bar_len
    pct = c * 100.0 / total
    print(f"  |{outcome.replace(' ', '')}>  {c:>4}  {bar:50s}  {pct:5.1f}%")


# ----- Save the screenshot ----------------------------------------
print()
print("=== Saving image ===")

stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
png_path = f"doom_hw_{stamp}.png"

fig, axes = plt.subplots(1, 3, figsize=(12, 4.5))

# Left: ideal input
ideal = pattern.reshape(4, 4)
axes[0].imshow(ideal, cmap="inferno", interpolation="nearest",
               vmin=0, vmax=1)
axes[0].set_title("Input pattern\n(4x4 'D')", fontsize=11)
axes[0].set_xticks([])
axes[0].set_yticks([])

# Middle: reconstructed image
recon_img = recon_norm.reshape(4, 4)
axes[1].imshow(recon_img, cmap="inferno", interpolation="nearest",
               vmin=0, vmax=1)
axes[1].set_title(f"Reconstructed from {total} shots\non {backend.name} ({backend.num_qubits}q)",
                  fontsize=11)
axes[1].set_xticks([])
axes[1].set_yticks([])

# Right: histogram bar chart
outcomes = [f"{i:04b}" for i in range(16)]
fractions = recon / total
axes[2].barh(range(16), fractions, color="firebrick")
axes[2].set_yticks(range(16))
axes[2].set_yticklabels(outcomes, fontsize=8, family="monospace")
axes[2].invert_yaxis()
axes[2].set_xlabel("Probability")
axes[2].set_title("Measurement distribution", fontsize=11)
axes[2].grid(axis="x", alpha=0.3)

fig.suptitle(
    f"Mini-DOOM on {backend.name} - "
    f"{qc_final.size()} native gates, depth {qc_final.depth()}, "
    f"{total} shots",
    fontsize=12
)
fig.tight_layout()
fig.savefig(png_path, dpi=150, bbox_inches="tight")
plt.close(fig)
print(f"  Wrote {png_path}")
print()
print("DOOM. On a real quantum computer. In Washington DC.")
