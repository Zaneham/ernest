# `tools/`

The Python you can't avoid.

Ernest is a quantum compiler. Ernest emits OpenQASM 3. IBM's hardware
lives behind an HTTPS API that only really likes being addressed in
Python through the `qiskit-ibm-runtime` client. So here we are.
These scripts are the boundary between Ernest's C99 world and IBM's
Pythonic one. Each of them is small. None of them is doing anything
clever. They are the bit of plumbing that turns "Ernest emitted some
text" into "a chip in Virginia is running your circuit."

## What you need before any of this works

A real IBM Cloud quantum-computing instance. The Open Plan is free and
gives you about ten minutes of QPU time per month. Ten minutes is
enough to take several satisfying screenshots and zero minutes is enough
to take none.

```
pip install qiskit qiskit-ibm-runtime qiskit-qasm3-import
```

An IBM Cloud API key, from https://cloud.ibm.com/iam/apikeys. Click
Create, name it anything. The actual key value appears ONCE in a popup
labelled "Successfully created", hidden behind dots; click Show or the
eye icon to reveal it and copy it down. That's the one you need.

The string starting `ApiKey-` you'll see in the listing afterwards is
NOT the key. It's a display ID. Mistaking it for the key is the most
common cause of "API key could not be found" errors. It is genuinely
the IBM Cloud UI's fault, not yours, but knowing that doesn't help you
authenticate.

Save the credentials once with the CRN of your quantum instance:

```python
from qiskit_ibm_runtime import QiskitRuntimeService
QiskitRuntimeService.save_account(
    channel="ibm_cloud",
    token="<the long random string from the Create popup>",
    instance="<your CRN: crn:v1:bluemix:public:quantum-computing:...>",
    overwrite=True,
)
```

You can run that block from a Python REPL once. After that every
script here reads the saved-default account and you never have to
touch the key again.

## The scripts

`run_bell_now.py` — Smoke test. Builds a Bell state in Qiskit
directly, submits, prints the histogram. Bypasses Ernest entirely on
purpose so you can confirm IBM auth is working before involving any
other layer. If this one passes, the other layers can fail
individually with informative errors. If this one fails, the others
won't even start.

`run_ernest_bell.py` — Same shape, but the circuit goes through
Ernest first. Reads `bell_ernest_ibm.qasm`. Generate that file with
`./ernest --target=ibm --qasm-out=bell_ernest_ibm.qasm bell 0`. If
the histogram looks like real Bell-state data, your C99 quantum
compiler is now in conversation with superconducting silicon.

`run_ernest_doom_hw.py` — Mini-DOOM. A 4x4 hand-drawn 'D'
encoded as a four-qubit amplitude pattern, sent through Ernest's IBM
pipeline, reconstructed from real hardware measurements. Hand-drawn
because at sixteen pixels there's nothing recognisable in the
actual WAD anyway. Outputs a timestamped PNG with input, reconstruction,
and histogram side-by-side.

`run_ernest_wad_hw.py` — Real-WAD DOOM. Reads the actual TITLEPIC
lump from your own DOOM.WAD, downsamples the top strip to 4x8
pixels, runs the resulting five-qubit circuit. Bring your own
legally-purchased Doom; the script defaults to the Steam install path
for Ultimate Doom on Windows. Edit `WAD_PATH` at the top if you keep
it elsewhere. FREEDOOM works too if you don't own Doom; the WAD
format is identical across vendors.

`dump_coupling.py` — Pull a backend's coupling map and write it in
Ernest's edge-list format. Required if you want SABRE routing in
Ernest to know what shape your specific device is. Use `--list`
to see what backends your account has access to before picking one
to dump.

`submit_ibm.py` — General-purpose. Take any OpenQASM 3 file Ernest
emitted, submit it to IBM, print the histogram. The script you'd reach
for once you're past Bell and Doom and want to run your own circuit.

## The intended workflow once you're set up

```bash
# Pick a backend and pull its coupling map (one-off per device)
python tools/dump_coupling.py --list
python tools/dump_coupling.py ibm_brisbane > brisbane.cmap

# Confirm Ernest's verifier still passes
./ernest verify

# Compile your circuit through Ernest with the device's topology
./ernest --target=ibm --coupling-file=brisbane.cmap \
         --qasm-out=mine.qasm bell 0

# Dry-run the submission to validate against the backend without
# spending QPU time
python tools/submit_ibm.py mine.qasm --backend ibm_brisbane --dry-run

# Submit
python tools/submit_ibm.py mine.qasm --backend ibm_brisbane --shots 1000
```

The dry-run step is the load-bearing one. Ten free minutes a month is
ten free minutes; you don't want to spend any of them finding out the
QASM referred to a qubit the device doesn't have, or used a gate the
backend doesn't speak, or any of the other failures that take less
than half a second to detect with a dry run and however-long-the-queue
is to detect without one.

## A note about the WAD scripts

The Doom scripts read the TITLEPIC lump directly from your own copy of
DOOM.WAD or DOOM2.WAD or FREEDOOM. We do not ship Bethesda's content
in this repo and never will; you bring your own. The path is
hardcoded as a Windows Steam install location for convenience. If
that's not where your Doom lives, edit the `WAD_PATH` constant at
the top of `run_ernest_wad_hw.py`. The format is the same across
every Doom-engine vendor since 1993, so any IWAD works.

If you don't own Doom, FREEDOOM (https://freedoom.github.io/) is a
BSD/freeart-licensed drop-in replacement.

## A note about channels

Saved credentials use `channel="ibm_cloud"`, which is IBM's newer
Cloud-based quantum service. Older IBM Quantum accounts on
quantum.ibm.com used `channel="ibm_quantum"`. The legacy channel
was sunset in 2025. If you're seeing weird authentication errors and
this is a copy of the project from before the migration, that's
likely the cause. Re-save the account with `channel="ibm_cloud"`
and the CRN of your IBM Cloud instance.

## A note about the API key in your shell history

If you typed your API key onto the command line, it's in your shell
history. Check `~/.bash_history` or PowerShell equivalent. Roll the
key. Future scripts should always read from saved credentials, not
from environment variables or command-line arguments. Saved
credentials live in `~/.qiskit/qiskit-ibm.json` which is plain text
and world-readable on Linux by default; consider `chmod 600` if
that bothers you (and on a shared machine it should).
