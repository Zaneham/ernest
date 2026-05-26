"""ERNESTJC. The JCL processor for quantum batch jobs.

Reads a JCL-style job file, compiles each step's circuit through
Ernest, optionally submits the whole lot as a single batched
SamplerV2 job to IBM Quantum, prints a mainframe-style job log.

Why JCL. Because submitting batch work to a job queue is the same
problem mainframes solved in 1964, and the syntax they invented
for it is still better than every modern alternative. One file
describes the whole run. Each step is named and addressable in
the log. Return codes propagate. You can read top to bottom and
see exactly what work is going to happen, in what order, with
what parameters. It is genuinely better than maintaining a Bash
script full of subprocess calls.

Why this matters for quantum. IBM's SamplerV2 API takes a list of
circuits per call. You queue once, you pay queue overhead once,
you get N results back. For an Open Plan user on ten free minutes
a month, batching is the difference between five demonstrations
and twenty.

Syntax. Each line is either a comment (//*), a job card
(//NAME JOB params), a step card (//NAME EXEC params), or //END.
Parameters are KEY=VALUE,KEY=VALUE,KEY=VALUE. The eight characters
that follow // are the step name; that name appears in the log
exactly the way a JES2 job log labels its steps.

Recognised parameters (job and step both accept these):
    CIRCUIT=name     one of bell, ghz, deutsch, grover, qftlib, groverlib
    QASM=path        alternative to CIRCUIT, use a QASM file as the step
    TARGET=name      generic or ibm (default: generic)
    COUPLING=name    linear_5, heavy_hex_7, full_16, etc.
    COUPLING_FILE=p  path to a dumped coupling map
    SHOTS=N          measurement shots per step (default: 1000)
    NOOPT=YES        skip Ernest's optimiser

Job-card values are defaults; step-card values override.

Usage:
    python tools/run_jcl.py myjob.jcl              # dry-run (compile only)
    python tools/run_jcl.py myjob.jcl --submit     # really submit
    python tools/run_jcl.py myjob.jcl --backend ibm_brisbane --submit
"""

import argparse
import subprocess
import sys
import time
import os
import re


# ----- JCL parser -------------------------------------------------

def parse_params(s):
    """Parse 'KEY=VAL,KEY=VAL' into a dict. Case-insensitive keys."""
    out = {}
    if not s:
        return out
    # Allow spaces around = and , for readability
    for part in re.split(r'\s*,\s*', s.strip()):
        if '=' in part:
            k, v = part.split('=', 1)
            out[k.strip().upper()] = v.strip()
    return out


def parse_jcl(path):
    """Parse a JCL file into (job_card, [step_cards]).

    Each card is a dict with NAME, VERB, and PARAMS.
    """
    with open(path, 'r') as f:
        lines = f.readlines()

    job = None
    steps = []
    for raw in lines:
        line = raw.rstrip()
        if not line.strip():
            continue
        # Comment
        if line.startswith('//*'):
            continue
        # End marker
        if line.strip() == '//END' or line.strip() == '//':
            break
        # Card: starts with //
        if line.startswith('//'):
            # Split: //NAME VERB PARAMS
            body = line[2:]
            # Name is the first word, then verb, then rest is params
            m = re.match(r'(\S+)\s+(\S+)(?:\s+(.*))?$', body)
            if not m:
                continue
            name = m.group(1)
            verb = m.group(2).upper()
            params = parse_params(m.group(3) or '')
            card = {'NAME': name, 'VERB': verb, 'PARAMS': params}
            if verb == 'JOB':
                job = card
            elif verb == 'EXEC':
                steps.append(card)

    return job, steps


# ----- Step driver: compile through Ernest ------------------------

def merged_params(job, step):
    """Combine job-card defaults with step-card overrides."""
    p = dict(job['PARAMS']) if job else {}
    p.update(step['PARAMS'])
    return p


def build_ernest_command(params, qasm_out_path):
    """Build the ernest.exe command line for this step."""
    cmd = ['./ernest.exe']
    if params.get('TARGET', '').lower() == 'ibm':
        cmd.append('--target=ibm')
    if 'COUPLING' in params:
        cmd.append(f"--coupling={params['COUPLING']}")
    if 'COUPLING_FILE' in params:
        cmd.append(f"--coupling-file={params['COUPLING_FILE']}")
    if params.get('NOOPT', '').upper() == 'YES':
        cmd.append('--no-opt')
    # Dispatch
    if 'CIRCUIT' in params:
        cmd.extend([params['CIRCUIT'].lower(), '0'])
    elif 'QASM' in params:
        cmd.extend(['compile', params['QASM']])
    else:
        raise ValueError("Step has neither CIRCUIT= nor QASM=")
    return cmd


def compile_step(job, step, scratch_dir):
    """Compile one step. Returns (qasm_text, rc, log_tail)."""
    params = merged_params(job, step)
    qasm_out_path = os.path.join(scratch_dir, f"{step['NAME']}.qasm")
    cmd = build_ernest_command(params, qasm_out_path)

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.returncode, result.stdout[-1500:] + result.stderr[-500:]

    # Extract the QASM section from Ernest's stdout
    out = result.stdout
    if 'CIRCUIT' in params:
        marker_start = '\n-- OpenQASM 3.0 --\n'
    else:
        marker_start = '-- OpenQASM 3.0 --'
    start = out.find(marker_start)
    if start < 0:
        return None, 8, "QASM section not found in Ernest output"
    qasm_start = start + len(marker_start)
    # End: either ERNESTJB COMPILE ENDED or end of stream
    end_markers = ['ERNESTJB COMPILE ENDED', 'ERNESTAO START',
                   'ERNEST EOJ', '\n  Histogram (']
    end = len(out)
    for m in end_markers:
        idx = out.find(m, qasm_start)
        if idx > 0 and idx < end:
            end = idx
    qasm_text = out[qasm_start:end].strip() + "\n"

    with open(qasm_out_path, 'w') as f:
        f.write(qasm_text)

    return qasm_text, 0, None


# ----- Job log formatting -----------------------------------------

def fmt_rc(rc):
    return f"{rc:04d}"


def print_job_log_header(job, steps):
    name = job['NAME'] if job else 'ANON    '
    print()
    print(f"$HASP373 {name:<8} STARTED - INIT 1 - CLASS=Q")
    print(f"//{name:<8} {'JOB':<6} {format_params(job['PARAMS'] if job else {})}")
    for s in steps:
        print(f"//{s['NAME']:<8} {'EXEC':<6} {format_params(s['PARAMS'])}")
    print(f"//END")
    print()


def format_params(p):
    return ','.join(f"{k}={v}" for k, v in p.items())


def print_step_start(step):
    print(f"$HASP373 {step['NAME']:<8} STARTED")


def print_step_end(step, rc, msg=None):
    print(f"$HASP395 {step['NAME']:<8} ENDED   RC={fmt_rc(rc)}"
          + (f"  {msg}" if msg else ""))


def print_job_end(job_name, max_rc):
    print(f"$HASP395 {job_name:<8} ENDED   RC={fmt_rc(max_rc)}")


# ----- Main -------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('jcl_file', help="JCL-style job description")
    parser.add_argument('--submit', action='store_true',
                        help="Submit the batch to IBM (default: dry-run)")
    parser.add_argument('--backend', default=None,
                        help="IBM backend (default: least-busy)")
    parser.add_argument('--scratch', default='build/jcl_scratch',
                        help="Directory for per-step QASM (default: build/jcl_scratch)")
    args = parser.parse_args()

    job, steps = parse_jcl(args.jcl_file)
    if not steps:
        print("ERNESTJC: no EXEC steps found in JCL deck", file=sys.stderr)
        sys.exit(8)

    os.makedirs(args.scratch, exist_ok=True)
    job_name = (job['NAME'] if job else 'ANON').ljust(8)

    print_job_log_header(job, steps)

    # Compile each step
    compiled = []  # list of (step, qasm_text, params)
    max_rc = 0
    for step in steps:
        print_step_start(step)
        params = merged_params(job, step)
        qasm_text, rc, errmsg = compile_step(job, step, args.scratch)
        if rc != 0:
            print_step_end(step, rc, f"COMPILE FAILED: {errmsg}")
            max_rc = max(max_rc, rc)
            continue
        circuit_name = params.get('CIRCUIT', params.get('QASM', '?'))
        gate_count = sum(1 for line in qasm_text.split('\n')
                         if line.strip() and
                         not line.startswith(('OPENQASM', 'include', 'qubit', 'bit', '//')))
        print_step_end(step, 0, f"COMPILED {circuit_name}  {gate_count} GATE LINES")
        compiled.append((step, qasm_text, params))

    if not args.submit:
        print()
        print("$HASP310 ERNESTJC DRY-RUN  use --submit to actually run")
        print_job_end(job_name, max_rc)
        return max_rc

    # Submit batch to IBM
    if not compiled:
        print()
        print(f"$HASP395 {job_name:<8} ENDED   RC={fmt_rc(max_rc)}  "
              f"NO STEPS COMPILED")
        return max_rc

    try:
        from qiskit import qasm3, transpile
        from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2 as Sampler
    except ImportError as e:
        print(f"$HASP395 {job_name:<8} ENDED   RC=0012  "
              f"IMPORT FAILED: {e}")
        return 12

    print()
    print(f"$HASP100 {job_name:<8} CONTACTING IBM CLOUD")
    service = QiskitRuntimeService(channel="ibm_cloud")
    if args.backend:
        backend = service.backend(args.backend)
    else:
        backend = service.least_busy(simulator=False, operational=True)
    print(f"$HASP100 {job_name:<8} BACKEND={backend.name}  "
          f"QUEUE={backend.status().pending_jobs}")

    # Build the circuit list
    circuits = []
    shots_list = []
    for (step, qasm_text, params) in compiled:
        qc = qasm3.loads(qasm_text)
        qc_t = transpile(qc, backend, optimization_level=1)
        circuits.append(qc_t)
        shots_list.append(int(params.get('SHOTS', '1000')))

    # SamplerV2 takes (circuit, parameter_values, shots) tuples per pub
    # so each step can ask for its own shot count.
    pubs = [(circuits[i], None, shots_list[i]) for i in range(len(circuits))]
    print(f"$HASP100 {job_name:<8} SUBMITTING {len(circuits)} CIRCUITS  "
          f"SHOTS={shots_list}")

    sampler = Sampler(backend)
    t0 = time.time()
    job_obj = sampler.run(pubs)
    job_id = job_obj.job_id()
    print(f"$HASP100 {job_name:<8} JOB_ID={job_id}")

    result = job_obj.result()
    wall = time.time() - t0
    print(f"$HASP100 {job_name:<8} HARDWARE  WALL={wall:.1f}s")

    # Print per-step histogram
    for i, (step, _, params) in enumerate(compiled):
        pub = result[i]
        try:
            if hasattr(pub.data, 'c'):
                counts = pub.data.c.get_counts()
            else:
                for attr in dir(pub.data):
                    if not attr.startswith('_'):
                        fld = getattr(pub.data, attr)
                        if hasattr(fld, 'get_counts'):
                            counts = fld.get_counts()
                            break
                else:
                    counts = {}
        except Exception as e:
            print(f"$HASP395 {step['NAME']:<8} ENDED   RC=0008  "
                  f"RESULT PARSE FAILED: {e}")
            max_rc = max(max_rc, 8)
            continue

        total = sum(counts.values())
        print()
        print(f"$HASP373 {step['NAME']:<8} HISTOGRAM  SHOTS={total}")
        for outcome in sorted(counts.keys()):
            c = counts[outcome]
            pct = c * 100.0 / total if total else 0
            bar = '#' * int(c * 40 / total + 0.5) if total else ''
            print(f"           |{outcome:<8}>  {c:>5}  {bar:40s}  {pct:5.1f}%")
        print(f"$HASP395 {step['NAME']:<8} ENDED   RC=0000")

    print()
    print_job_end(job_name, max_rc)
    return max_rc


if __name__ == '__main__':
    sys.exit(main())
