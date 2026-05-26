#!/usr/bin/env python
"""Throw an OpenQASM 3 file at IBM hardware and tell us what came back.

This is the general-purpose run script. It takes any QASM file
Ernest produced, sends it across the internet to a chilled-to-near-
absolute-zero superconducting chip in Washington DC, waits for the
result, prints the histogram. The waiting is the long part. The IBM
queue is the IBM queue.

Usage:
    python tools/submit_ibm.py CIRCUIT.qasm [--backend NAME] [--shots N]
    python tools/submit_ibm.py --list anything            # list backends

Use --dry-run to load and validate without spending QPU time. Useful
when you suspect the QASM might reference a qubit the backend
doesn't have, or use a gate the backend doesn't speak.

The script does a final transpile(..., optimization_level=0) pass
against the backend to validate the circuit. If Ernest produced
correctly-routed IBM-native QASM, that transpile is essentially a
no-op pass that just confirms the device accepts it. If it isn't,
the transpile will either fix it up (with some optimisation) or
complain loudly.

Requires:
    pip install qiskit qiskit-ibm-runtime qiskit-qasm3-import

And saved credentials. See run_bell_now.py header for the one-off
setup.
"""

import argparse
import sys
import time


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("qasm_file", nargs="?",
                        help="OpenQASM 3 file produced by Ernest. "
                             "Omit when using --list.")
    parser.add_argument("--backend", default=None,
                        help="IBM Quantum backend name "
                             "(if omitted, picks the least-busy operational backend)")
    parser.add_argument("--channel", default="ibm_cloud",
                        help="QiskitRuntimeService channel (default: ibm_cloud)")
    parser.add_argument("--instance", default=None,
                        help="CRN of the IBM Cloud instance (overrides saved)")
    parser.add_argument("--shots", type=int, default=1000,
                        help="Number of measurement shots (default: 1000)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Load and validate, do not actually submit")
    parser.add_argument("--list", action="store_true",
                        help="List available backends and exit")
    args = parser.parse_args()

    try:
        from qiskit import qasm3, transpile
        from qiskit_ibm_runtime import (
            QiskitRuntimeService,
            SamplerV2 as Sampler,
        )
    except ImportError as e:
        print(
            f"Required package not installed: {e}\n"
            "Install with:\n"
            "    pip install qiskit qiskit-ibm-runtime",
            file=sys.stderr,
        )
        sys.exit(2)

    service_kwargs = {"channel": args.channel}
    if args.instance is not None:
        service_kwargs["instance"] = args.instance
    service = QiskitRuntimeService(**service_kwargs)

    if args.list:
        print(f"Available backends on channel={args.channel}:")
        for b in service.backends():
            status_obj = b.status()
            operational = status_obj.operational
            pending = status_obj.pending_jobs
            print(f"  {b.name}  {b.num_qubits}q  "
                  f"{'operational' if operational else 'down'}  "
                  f"queue={pending}")
        return

    print(f"ERNESTIB START   file={args.qasm_file}")
    print(f"  channel : {args.channel}")
    print(f"  shots   : {args.shots}")

    with open(args.qasm_file, "r") as f:
        qasm = f.read()
    print(f"  loaded  : {len(qasm)} bytes of QASM")

    try:
        qc = qasm3.loads(qasm)
    except Exception as e:
        print(f"ERNESTIB QASM3 LOAD FAILED: {e}", file=sys.stderr)
        sys.exit(3)
    print(f"  parsed  : {qc.num_qubits} qubits, {qc.size()} gates")

    # Pick backend
    if args.backend is None:
        backend = service.least_busy(simulator=False, operational=True)
        print(f"  backend : {backend.name} (auto-selected: least busy)")
    else:
        backend = service.backend(args.backend)
        print(f"  backend : {backend.name}")
    print(f"            {backend.num_qubits} qubits  "
          f"queue={backend.status().pending_jobs}")

    # Final transpile against the backend. Ernest should have done
    # all the substantive routing and decomposition; this pass
    # mostly validates the circuit against the device.
    qc_t = transpile(qc, backend, optimization_level=0)
    print(f"  ibm xpile : {qc_t.size()} gates after IBM transpiler")

    if args.dry_run:
        print("ERNESTIB DRY-RUN  no submission")
        return

    print(f"ERNESTIB SUBMIT  queuing job...")
    sampler = Sampler(backend)
    t0 = time.time()
    job = sampler.run([qc_t], shots=args.shots)
    job_id = job.job_id()
    print(f"ERNESTIB SUBMIT  job_id={job_id}")
    print(f"  watch at: https://quantum.ibm.com/jobs/{job_id}")
    print(f"           (or in the IBM Cloud dashboard)")

    print(f"ERNESTIB WAIT    waiting for hardware result...")
    result = job.result()
    t1 = time.time()
    print(f"ERNESTIB DONE    {t1 - t0:.1f}s wall clock")

    # Extract counts. SamplerV2 returns per-pub data; we run one pub.
    pub_result = result[0]
    if hasattr(pub_result.data, 'c'):
        counts = pub_result.data.c.get_counts()
    elif hasattr(pub_result.data, 'meas'):
        counts = pub_result.data.meas.get_counts()
    else:
        # Fall back to whatever bit-string field exists
        for attr in dir(pub_result.data):
            if not attr.startswith('_'):
                field = getattr(pub_result.data, attr)
                if hasattr(field, 'get_counts'):
                    counts = field.get_counts()
                    break
        else:
            print("ERNESTIB no counts field found in result", file=sys.stderr)
            sys.exit(4)

    total = sum(counts.values())
    print()
    print(f"  Histogram ({total} shots, real hardware)")
    print(f"  ---------------------------")
    for outcome in sorted(counts.keys()):
        count = counts[outcome]
        frac = count / total
        bar_len = int(frac * 40 + 0.5)
        bar = '#' * bar_len
        print(f"  |{outcome}>  {count:>6}  {bar:40s}  {frac * 100:5.1f}%")

    print(f"\nERNESTIB ENDED   RC=0")


if __name__ == "__main__":
    main()
