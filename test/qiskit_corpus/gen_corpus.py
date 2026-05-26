"""
Generate a Qiskit-authored OpenQASM 3.0 corpus for Ernest's parser.

Each function below produces one circuit. The driver at the bottom
calls qiskit.qasm3.dumps on each and writes the result to samples/.
The set spans the easy stuff (Bell, GHZ), the rotation-heavy stuff
(QFT, VQE ansatz), the multi-register stuff (teleportation), and a
few things Ernest is unlikely to handle yet (Toffoli, classical
control). That's the point: we want to find the gaps, not avoid
them.
"""

import os
import numpy as np
from qiskit import QuantumCircuit, QuantumRegister, ClassicalRegister
from qiskit.qasm3 import dumps
from qiskit.circuit.library import QFT, EfficientSU2

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "samples")
os.makedirs(OUT_DIR, exist_ok=True)


def save(name, qc):
    path = os.path.join(OUT_DIR, name)
    qasm = dumps(qc)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(qasm)
    lines = qasm.count("\n") + 1
    print(f"  {name:32s}  {qc.num_qubits:>2d} qubits  {qc.size():>3d} gates  {lines:>3d} lines")


def gen_bell():
    qc = QuantumCircuit(2, 2)
    qc.h(0)
    qc.cx(0, 1)
    qc.measure([0, 1], [0, 1])
    return qc


def gen_ghz5():
    qc = QuantumCircuit(5, 5)
    qc.h(0)
    for i in range(4):
        qc.cx(i, i + 1)
    qc.measure(range(5), range(5))
    return qc


def gen_deutsch():
    qc = QuantumCircuit(2, 1)
    qc.x(1)
    qc.h([0, 1])
    qc.cx(0, 1)  # oracle for balanced f(x) = x
    qc.h(0)
    qc.measure(0, 0)
    return qc


def gen_grover_2q():
    qc = QuantumCircuit(2, 2)
    qc.h([0, 1])
    qc.cz(0, 1)  # oracle marks |11>
    qc.h([0, 1])
    qc.x([0, 1])
    qc.cz(0, 1)
    qc.x([0, 1])
    qc.h([0, 1])
    qc.measure([0, 1], [0, 1])
    return qc


def gen_qft4():
    qft = QFT(4, do_swaps=True).decompose()
    qc = QuantumCircuit(qft.num_qubits, qft.num_qubits)
    qc.compose(qft, inplace=True)
    qc.measure(range(qft.num_qubits), range(qft.num_qubits))
    return qc


def gen_vqe_ansatz():
    # Hardware-efficient ansatz, three qubits, two repetitions.
    ansatz = EfficientSU2(num_qubits=3, reps=2, entanglement="linear").decompose()
    np.random.seed(42)
    params = np.random.uniform(0.0, 2.0 * np.pi, ansatz.num_parameters)
    bound = ansatz.assign_parameters(params)
    return bound


def gen_toffoli():
    qc = QuantumCircuit(3, 3)
    qc.h(0)
    qc.h(1)
    qc.ccx(0, 1, 2)
    qc.measure([0, 1, 2], [0, 1, 2])
    return qc


def gen_bernstein_vazirani():
    secret = "1011"
    n = len(secret)
    qc = QuantumCircuit(n + 1, n)
    qc.x(n)
    qc.h(range(n + 1))
    for i, b in enumerate(reversed(secret)):
        if b == "1":
            qc.cx(i, n)
    qc.h(range(n))
    qc.measure(range(n), range(n))
    return qc


def gen_w_state():
    qc = QuantumCircuit(3, 3)
    theta = 2.0 * np.arccos(1.0 / np.sqrt(3.0))
    qc.ry(theta, 0)
    qc.ch(0, 1)
    qc.cx(1, 2)
    qc.cx(0, 1)
    qc.x(0)
    qc.measure([0, 1, 2], [0, 1, 2])
    return qc


def gen_clifford_small():
    # Hand-picked small Clifford circuit. Avoids the qiskit
    # random_clifford helper because the decomposition produces
    # extremely long output and we want compact corpus files.
    qc = QuantumCircuit(4, 4)
    qc.h(0); qc.s(1); qc.cx(0, 1)
    qc.h(2); qc.cx(2, 3)
    qc.cx(1, 2); qc.s(3); qc.h(0)
    qc.measure(range(4), range(4))
    return qc


def gen_rotations_only():
    # All three rotation axes on three qubits, with measurement.
    qc = QuantumCircuit(3, 3)
    qc.rx(np.pi / 4.0, 0)
    qc.ry(np.pi / 3.0, 1)
    qc.rz(np.pi / 2.0, 2)
    qc.rx(-np.pi / 6.0, 0)
    qc.ry(-np.pi / 8.0, 1)
    qc.rz(np.pi, 2)
    qc.measure([0, 1, 2], [0, 1, 2])
    return qc


def gen_multireg():
    # Two quantum registers, two classical registers, with names
    # that don't collide with the default q/c.
    qr_a = QuantumRegister(2, "qa")
    qr_b = QuantumRegister(2, "qb")
    cr_a = ClassicalRegister(2, "ca")
    cr_b = ClassicalRegister(2, "cb")
    qc = QuantumCircuit(qr_a, qr_b, cr_a, cr_b)
    qc.h(qr_a[0])
    qc.cx(qr_a[0], qr_a[1])
    qc.h(qr_b[0])
    qc.cx(qr_b[0], qr_b[1])
    qc.cx(qr_a[0], qr_b[0])
    qc.measure(qr_a, cr_a)
    qc.measure(qr_b, cr_b)
    return qc


CORPUS = [
    ("bell.qasm",                gen_bell),
    ("ghz_5.qasm",               gen_ghz5),
    ("deutsch.qasm",             gen_deutsch),
    ("grover_2q.qasm",           gen_grover_2q),
    ("qft_4.qasm",               gen_qft4),
    ("vqe_ansatz.qasm",          gen_vqe_ansatz),
    ("toffoli.qasm",             gen_toffoli),
    ("bernstein_vazirani.qasm",  gen_bernstein_vazirani),
    ("w_state.qasm",             gen_w_state),
    ("clifford_small.qasm",      gen_clifford_small),
    ("rotations_only.qasm",      gen_rotations_only),
    ("multireg.qasm",            gen_multireg),
]


def main():
    import qiskit
    print(f"Qiskit {qiskit.__version__}")
    print(f"Writing corpus to {OUT_DIR}")
    print()
    for name, builder in CORPUS:
        qc = builder()
        save(name, qc)
    print()
    print(f"DONE: {len(CORPUS)} samples generated")


if __name__ == "__main__":
    main()
