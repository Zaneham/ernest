"""The smoke test. Bell state on real quantum hardware via IBM Cloud.

So you wanna run your first thing on a real quantum computer.

This script is the simplest possible "does any of this work" test:
build a Bell state directly in Qiskit, submit it to whatever IBM
Quantum backend you have access to on the Open Plan, print the
histogram. Bypasses Ernest entirely on purpose. If you can't get
this to work, anything more elaborate is also not going to work,
and starting with the simpler thing is how you find out which part
is the problem.

Running this for the first time:

  1. Get an IBM Cloud API key from https://cloud.ibm.com/iam/apikeys.
     Click Create. The actual key value is shown ONCE, in a popup,
     hidden behind dots. Click Show, copy that. The "ApiKey-..." ID
     visible in the listing afterwards is NOT the key; it's a label.
     Mistaking the ID for the value is the most common cause of "API
     key could not be found" errors.

  2. Set the key in the SAVE-CREDENTIALS block below and uncomment
     it. Or, run save_account once in a Python REPL and leave this
     script alone.

  3. Run this script. If you see roughly 50/50 on |00> and |11>
     with a few percent of noise leaking into |01> and |10>, you
     have a Bell state on a real superconducting chip in DC.

  4. RE-COMMENT the SAVE-CREDENTIALS block so the key doesn't sit
     in your source tree. Better yet, never put the key in source
     at all and use the REPL pattern.

Step 4 is more important than people give it credit for.
"""

from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2 as Sampler
from qiskit import QuantumCircuit, transpile
import sys
import traceback


# ----- SAVE CREDENTIALS (one-off; re-comment after running) -----
#
# Replace the placeholders, run the script once to save the
# account, then re-comment so the values don't sit on disk. Or do
# the equivalent save_account call from a Python REPL and leave
# this script untouched.
#
# from qiskit_ibm_runtime import QiskitRuntimeService
# QiskitRuntimeService.save_account(
#     channel="ibm_cloud",
#     token="<your IBM Cloud API key here, the long random one, NOT the ApiKey- id>",
#     instance="<your CRN: crn:v1:bluemix:public:quantum-computing:...>",
#     overwrite=True,
# )


print("=== Connecting to IBM Cloud ===")
try:
    service = QiskitRuntimeService(channel="ibm_cloud")
    print("  ok")
except Exception as e:
    print(f"  FAILED: {type(e).__name__}: {e}")
    print()
    print("Most likely you haven't saved credentials yet. Uncomment the")
    print("SAVE CREDENTIALS block at the top of this script, paste in")
    print("your IBM Cloud API key (the long one from the Create popup,")
    print("not the ApiKey- ID from the listing), paste in your CRN, and")
    print("re-run.")
    traceback.print_exc()
    sys.exit(1)


print()
print("=== Available backends ===")
backends = service.backends()
if not backends:
    print("  Your account can't see any backends. Either your instance")
    print("  hasn't been provisioned, or the channel is wrong, or your")
    print("  quota is exhausted.")
    sys.exit(2)
for b in backends:
    try:
        st = b.status()
        op = "operational" if st.operational else "down"
        print(f"  {b.name}  {b.num_qubits}q  {op}  queue={st.pending_jobs}")
    except Exception as e:
        print(f"  {b.name}  (status query failed: {e})")


print()
print("=== Picking the least-busy backend ===")
backend = service.least_busy(simulator=False, operational=True)
print(f"  {backend.name} ({backend.num_qubits} qubits, "
      f"queue={backend.status().pending_jobs})")


print()
print("=== Building a Bell state ===")
qc = QuantumCircuit(2, 2)
qc.h(0)
qc.cx(0, 1)
qc.measure([0, 1], [0, 1])
qc_t = transpile(qc, backend, optimization_level=1)
print(f"  {qc_t.size()} native gates after IBM's transpiler")


print()
print("=== Submitting ===")
sampler = Sampler(backend)
job = sampler.run([qc_t], shots=1000)
print(f"  job_id: {job.job_id()}")
print(f"  watch at https://quantum.ibm.com/jobs/{job.job_id()}")


print()
print("=== Waiting ===")
import time
t0 = time.time()
result = job.result()
print(f"  done in {time.time() - t0:.1f}s")


print()
print("=== Histogram (real superconducting qubits, DC) ===")
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
print("If that looks like ~50/50 on |00> and |11> with a couple of")
print("percent of noise on |01> and |10>: congratulations, you can")
print("now move on to the more interesting scripts. If it doesn't,")
print("something between you and the chip is broken; comparing the")
print("error to the relevant section above should narrow it down.")
