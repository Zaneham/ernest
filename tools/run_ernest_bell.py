"""Ernest's compiled Bell QASM on real quantum hardware.

Same shape as run_bell_now.py but the circuit goes through Ernest
first. Where run_bell_now builds a fresh QuantumCircuit in Qiskit
and submits it directly, this one reads bell_ernest_ibm.qasm
(which Ernest wrote) and submits that.

You generate the QASM file by running, from the project root:

    ./ernest --target=ibm --qasm-out=bell_ernest_ibm.qasm bell 0

If the histogram coming back looks like real Bell-state data, your
C99 quantum compiler talked to a superconducting chip on the other
side of the Pacific. Take a moment.

Requires saved IBM Cloud credentials; see run_bell_now.py for how
to do that the first time.
"""
from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2 as Sampler
from qiskit import qasm3, transpile
import sys
import time


print("=== Load Ernest's compiled QASM ===")
try:
    with open("bell_ernest_ibm.qasm", "r") as f:
        qasm_src = f.read()
except FileNotFoundError:
    print("  bell_ernest_ibm.qasm not found. Generate it first:")
    print("    ./ernest --target=ibm --qasm-out=bell_ernest_ibm.qasm bell 0")
    sys.exit(1)
print(qasm_src)

qc = qasm3.loads(qasm_src)
print(f"  parsed: {qc.num_qubits} qubits, {qc.size()} gates")


print()
print("=== Connect to IBM ===")
service = QiskitRuntimeService(channel="ibm_cloud")
backend = service.least_busy(simulator=False, operational=True)
print(f"  backend: {backend.name} ({backend.num_qubits} qubits, "
      f"queue={backend.status().pending_jobs})")


print()
print("=== Submit ===")
qc_t = transpile(qc, backend, optimization_level=1)
print(f"  transpiled to {qc_t.size()} gates on hardware basis")
sampler = Sampler(backend)
job = sampler.run([qc_t], shots=1000)
print(f"  job_id: {job.job_id()}")


print()
print("=== Wait ===")
t0 = time.time()
result = job.result()
print(f"  done in {time.time() - t0:.1f}s")


print()
print("=== Histogram (Ernest -> IBM hardware) ===")
pub = result[0]
if hasattr(pub.data, 'c'):
    counts = pub.data.c.get_counts()
elif hasattr(pub.data, 'meas'):
    counts = pub.data.meas.get_counts()
else:
    for attr in dir(pub.data):
        if not attr.startswith('_'):
            fld = getattr(pub.data, attr)
            if hasattr(fld, 'get_counts'):
                counts = fld.get_counts()
                break

total = sum(counts.values())
print(f"  {total} shots")
print()
for o in sorted(counts.keys()):
    c = counts[o]
    pct = c * 100.0 / total
    bar = '#' * int(c * 40 / total + 0.5)
    print(f"  |{o}>  {c:>4}  {bar:40s}  {pct:5.1f}%")
print()
print("Ernest -> IBM -> screenshot. Done.")
