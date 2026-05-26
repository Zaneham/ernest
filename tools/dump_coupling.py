#!/usr/bin/env python
"""Ask IBM how its qubits are wired together. Write that down.

Every IBM Quantum chip has a coupling map: a graph that says which
physical qubits can do a two-qubit gate directly. The maps are
public information but the only convenient way to ask is through
qiskit-ibm-runtime in Python (because IBM decided that's where life
is now). This script does the asking and writes the answer in the
plain-text edge-list format Ernest understands.

Output format: one '#' comment block at the top with metadata, then
one 'u v' edge per line. Ernest reads this directly through
--coupling-file=path. Use it on the Ernest side when compiling a
circuit for that specific device:

    ./ernest --target=ibm --coupling-file=brisbane.cmap \\
             --qasm-out=mine.qasm mycircuit.qasm

Usage:
    python tools/dump_coupling.py --list                # list backends
    python tools/dump_coupling.py BACKEND_NAME > x.cmap # dump one

Requires saved IBM Cloud credentials. The first time, save them
with the channel and CRN of your instance:

    from qiskit_ibm_runtime import QiskitRuntimeService
    QiskitRuntimeService.save_account(
        channel="ibm_cloud",
        token="<your IBM Cloud API key, the long one, not the ApiKey- id>",
        instance="<your full CRN>",
        overwrite=True,
    )

After that this script reads the saved-default account.
"""

import argparse
import sys


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("backend", nargs="?",
                        help="Backend name (e.g. ibm_brisbane, ibm_kyoto). "
                             "Omit when using --list.")
    parser.add_argument("--channel", default="ibm_cloud",
                        help="QiskitRuntimeService channel (default: ibm_cloud)")
    parser.add_argument("--instance", default=None,
                        help="CRN of the IBM Cloud instance (overrides saved)")
    parser.add_argument("--list", action="store_true",
                        help="List available backends instead of dumping")
    args = parser.parse_args()

    try:
        from qiskit_ibm_runtime import QiskitRuntimeService
    except ImportError:
        print(
            "qiskit-ibm-runtime not installed. Install with:\n"
            "    pip install qiskit-ibm-runtime",
            file=sys.stderr,
        )
        sys.exit(2)

    service_kwargs = {"channel": args.channel}
    if args.instance is not None:
        service_kwargs["instance"] = args.instance
    service = QiskitRuntimeService(**service_kwargs)

    if args.list:
        print(f"Available backends on channel={args.channel}:", file=sys.stderr)
        for b in service.backends():
            status = "operational" if b.status().operational else "down"
            print(f"  {b.name}  {b.num_qubits}q  {status}", file=sys.stderr)
        return

    if args.backend is None:
        print("ernest: backend name required (or use --list)",
              file=sys.stderr)
        sys.exit(1)
    backend = service.backend(args.backend)
    coupling = backend.coupling_map
    edges = list(coupling.get_edges())

    print(f"# Coupling map for {backend.name}")
    print(f"# {backend.num_qubits} qubits, {len(edges)} directed edges")
    print(f"# Dumped from qiskit-ibm-runtime by tools/dump_coupling.py")
    print(f"# Channel: {args.channel}")
    print(f"#")
    print(f"# Each line below is a coupled pair 'u v'. IBM treats")
    print(f"# directions separately so edges appear twice; Ernest")
    print(f"# treats the coupling as symmetric so duplicates are")
    print(f"# harmless.")
    print(f"")
    for u, v in edges:
        print(f"{u} {v}")


if __name__ == "__main__":
    main()
