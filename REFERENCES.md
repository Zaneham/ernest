# References

A bibliography of the prior art Ernest stands on. Citations are
APA 7th edition. Where a single source covers a range of Ernest's
modules, a short note follows the citation to point at the relevant
code.

This file is the canonical reference list; source-file comments
should cite by author and year and trust that the full entry lives
here. Anything missing or wrong is a pull request.

---

## Quantum computing fundamentals

Nielsen, M. A., & Chuang, I. L. (2010). *Quantum computation and
    quantum information* (10th anniversary ed.). Cambridge
    University Press.
> The general reference for everything in `src/sim.c`, `src/qstd.c`,
> and most of the gate-matrix mathematics scattered through the
> rest. The QFT construction in `qstd_qft` follows Section 5.1.

Barenco, A., Bennett, C. H., Cleve, R., DiVincenzo, D. P., Margolus,
    N., Shor, P., Sleator, T., Smolin, J. A., & Weinfurter, H.
    (1995). Elementary gates for quantum computation. *Physical
    Review A*, *52*(5), 3457–3467.
    https://doi.org/10.1103/PhysRevA.52.3457
> The source for the Toffoli (CCX) decomposition used in
> `src/opt.c` `decomp_ccx`. Their construction is the canonical
> 6-CX, 9-single-qubit-gate form.

---

## Quantum algorithms implemented by Ernest

### Bell states and non-locality

Einstein, A., Podolsky, B., & Rosen, N. (1935). Can quantum-
    mechanical description of physical reality be considered
    complete? *Physical Review*, *47*(10), 777–780.
    https://doi.org/10.1103/PhysRev.47.777

Bell, J. S. (1964). On the Einstein Podolsky Rosen paradox.
    *Physics Physique Физика*, *1*(3), 195–200.
    https://doi.org/10.1103/PhysicsPhysiqueFizika.1.195
> The Bell state in `demos/bell.c` and its journal-quality
> derivation. Mostly here for the historical record.

### GHZ state

Greenberger, D. M., Horne, M. A., & Zeilinger, A. (1989). Going
    beyond Bell's theorem. In M. Kafatos (Ed.), *Bell's theorem,
    quantum theory, and conceptions of the universe* (pp. 69–72).
    Kluwer.
> The GHZ construction in `demos/ghz.c`. Three qubits in an equal
> superposition of |000> and |111>.

### Deutsch algorithm

Deutsch, D. (1985). Quantum theory, the Church–Turing principle and
    the universal quantum computer. *Proceedings of the Royal
    Society of London. A. Mathematical and Physical Sciences*,
    *400*(1818), 97–117. https://doi.org/10.1098/rspa.1985.0070
> The original Deutsch paper. The two-qubit version `demos/deutsch.c`
> implements is the bare-minimum demonstration.

### Bernstein–Vazirani algorithm

Bernstein, E., & Vazirani, U. (1997). Quantum complexity theory.
    *SIAM Journal on Computing*, *26*(5), 1411–1473.
    https://doi.org/10.1137/S0097539796300921
> The BV algorithm appears in the Qiskit corpus
> (`test/qiskit_corpus/samples/bernstein_vazirani.qasm`).

### Grover's algorithm

Grover, L. K. (1996). A fast quantum mechanical algorithm for
    database search. *Proceedings of the Twenty-Eighth Annual ACM
    Symposium on Theory of Computing*, 212–219.
    https://doi.org/10.1145/237814.237866
> The diffusion operator in `qstd_grover_diffusion` and the
> two-qubit Grover demo in `demos/grover.c`.

### Quantum Fourier Transform

Coppersmith, D. (2002). An approximate Fourier transform useful in
    quantum factoring (arXiv:quant-ph/0201067) [Preprint]. arXiv.
    https://arxiv.org/abs/quant-ph/0201067
> The original 1994 IBM Research Report, posted to arXiv years
> later. The QFT decomposition into Hadamards and controlled-phase
> gates that `qstd_qft` and `qstd_iqft` implement is from this
> paper.

Shor, P. W. (1994). Algorithms for quantum computation: Discrete
    logarithms and factoring. *Proceedings 35th Annual Symposium on
    Foundations of Computer Science*, 124–134.
    https://doi.org/10.1109/SFCS.1994.365700
> Where Shor uses the QFT to break factoring. Context for why the
> QFT matters at all.

---

## Quantum compilation

### OpenQASM 3

Cross, A. W., Javadi-Abhari, A., Alexander, T., De Beaudrap, N.,
    Bishop, L. S., Heidel, S., Ryan, C. A., Sivarajah, P., Smolin,
    J., Gambetta, J. M., & Johnson, B. R. (2022). OpenQASM 3: A
    broader and deeper quantum assembly language. *ACM Transactions
    on Quantum Computing*, *3*(3), Article 12, 1–50.
    https://doi.org/10.1145/3505636
> The specification of the language Ernest reads and writes. The
> lexer (`qasm_lex.c`), parser (`qasm_parse.c`), and emitter
> (`qasm.c`) all target this spec.

### Qubit routing (SABRE)

Li, G., Ding, Y., & Xie, Y. (2019). Tackling the qubit mapping
    problem for NISQ-era quantum devices. *Proceedings of the
    Twenty-Fourth International Conference on Architectural Support
    for Programming Languages and Operating Systems*, 1001–1014.
    https://doi.org/10.1145/3297858.3304023
> The SABRE algorithm. The routing pass in `src/route.c` (Ernest
> v0.x onward) follows the front-layer / lookahead / SWAP-scoring
> structure from this paper.

### Native gate sets

International Business Machines Corporation. (2024). *IBM Quantum
    documentation: Native gates of IBM Quantum devices*. IBM.
    Retrieved from
    https://docs.quantum.ibm.com/guides/native-gates
> The IBM native basis {RZ, SX, X, CX, ID} used as the target of
> `opt_decompose_ibm` in `src/opt.c`.

---

## Quantum image processing

### Foundational encodings

Le, P. Q., Dong, F., & Hirota, K. (2011). A flexible representation
    of quantum images for polynomial preparation, image compression,
    and processing operations. *Quantum Information Processing*,
    *10*(1), 63–84. https://doi.org/10.1007/s11128-010-0177-y
> FRQI, the original quantum image representation. Uses 2n+1 qubits
> for a 2^n by 2^n image, with one ancilla carrying colour as a
> rotation angle. Ernest does not use FRQI but cites it as the
> source of the field. The header comment on `src/qpie.h` refers to
> this paper.

Zhang, Y., Lu, K., Gao, Y., & Wang, M. (2013). NEQR: A novel
    enhanced quantum representation of digital images. *Quantum
    Information Processing*, *12*(8), 2833–2860.
    https://doi.org/10.1007/s11128-013-0567-z
> NEQR. Keeps the FRQI position register but encodes colour into
> the computational basis with one extra qubit per colour bit.
> Mentioned in the `src/qpie.h` comment as a related encoding.

### The encoding Ernest actually uses

Yao, X.-W., Wang, H., Liao, Z., Chen, M.-C., Pan, J., Li, J.,
    Zhang, X., Lin, X., Wang, Y., Liu, Z., Chen, W., Xu, J., Wang,
    C., Yang, X., Cao, Q., Lu, J., Wei, J., Suter, D., Du, J., …
    Yang, T. (2017). Quantum image processing and its application
    to edge detection: Theory and experiment. *Physical Review X*,
    *7*(3), 031041. https://doi.org/10.1103/PhysRevX.7.031041
> QPIE (Quantum Probability Image Encoding). The amplitude-only
> encoding implemented by `ernest_qpie_encode` in `src/qpie.c`.
> Each pixel's intensity becomes the squared amplitude of one
> basis state.

### Surveys

Yan, F., Iliyasu, A. M., & Venegas-Andraca, S. E. (2016). A survey
    of quantum image representations. *Quantum Information
    Processing*, *15*(1), 1–35.
    https://doi.org/10.1007/s11128-015-1195-6
> Useful overview of FRQI, NEQR, GQIR, and other representations.
> Good starting point for readers who want the field rather than
> just the one encoding Ernest uses.

### GQIR

The Generalised Quantum Image Representation extends NEQR to
arbitrary image sizes (not just 2^n by 2^n). The authoritative
citation is on the to-verify list; multiple papers in the
2014–2015 window propose related representations under similar
names. To be confirmed before the next citation pass touches this
section.

---

## Doom

id Software. (1993). *Doom* [Video game]. id Software / GT
    Interactive.
> The 1993 first-person shooter. Ernest's `demos/doom_demo.c`
> renders a Doom-themed image through the quantum simulator. This
> project does not distribute Bethesda WAD files; users supplying
> their own WAD via the `--wad` flag is the path through the
> WAD-loader code in `src/image.c`.

DoomWiki contributors. (n.d.). *WAD*. DoomWiki.
    https://doomwiki.org/wiki/WAD
> Reference for the IWAD/PWAD file format used by
> `ernest_image_load_wad_titlepic`. Header is 12 bytes, directory
> entries are 16 bytes each, lump names are eight characters
> NUL-padded.

DoomWiki contributors. (n.d.). *Picture format*. DoomWiki.
    https://doomwiki.org/wiki/Picture_format
> Reference for the column-major run-length format used inside
> picture lumps like TITLEPIC.

Freedoom contributors. (n.d.). *Freedoom*.
    https://freedoom.github.io/
> A free, BSD-and-freeart-licensed Doom-compatible IWAD. Drop-in
> replacement for Bethesda's WADs for users who don't own Doom.

---

## How to cite Ernest

When Ernest becomes a citable artifact (paper, release, archived
DOI), the entry will go here. Until then, this file is the closest
thing to a citation surface.

---

## Verification status

Most entries above have been cross-checked against the original
publication or the publisher's website. The single known
uncertainty is the GQIR attribution; the placeholder is deliberate
and the next citation pass should pin it.

If you spot anything missing, mis-attributed, or out of APA 7th
form, a pull request fixing it is the best thing you can do for
the project this week.
