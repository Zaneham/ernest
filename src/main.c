#include "tir.h"
#include "qasm.h"
#include "sim.h"
#include "abend.h"
#include "opt.h"
#include "route.h"
#include "aot.h"
#include "snap.h"
#include "mnote.h"
#include "../demos/demos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* run_lex_test is declared in demos/demos.h. Bypasses the normal
 * simulate-and-histogram path because the lexer test isn't a circuit
 * demo; it's a compiler demo. */

/*
 * The Ernest demo driver.
 *
 * Picks a circuit by subcommand, builds it, prints the TIR, prints
 * the OpenQASM, simulates the requested number of shots, and shows
 * a histogram of the outcomes.
 *
 *   ./ernest             defaults to the Bell state
 *   ./ernest bell        the Bell state
 *   ./ernest ghz         the GHZ state
 *   ./ernest deutsch     Deutsch's algorithm
 *   ./ernest grover      Grover's algorithm
 *   ./ernest <name> N    use N shots instead of the default 8192
 *
 * Adding a new demo: write demos/<name>.c with a
 * build_<name>_module function, declare it in demos/demos.h, and
 * add a case to the dispatcher below. Ernest grows by accretion.
 */

/* SNAP log dump handler. Registered via atexit so subcommands that
 * return through their own paths (verify, compile, doom, etc.)
 * still emit the snap log on the way out. */
static snap_log_t *snap_dump_log = NULL;
static const char *snap_dump_path = NULL;
static void snap_exit_handler(void)
{
    if (snap_dump_log == NULL || !snap_dump_log->enabled) {
        return;
    }
    if (snap_dump_path != NULL) {
        (void)snap_log_write_file(snap_dump_log, snap_dump_path);
        (void)fprintf(stderr, "ernest: snap log written to %s\n",
                      snap_dump_path);
    } else {
        snap_log_dump(snap_dump_log, stdout);
    }
}

/* Counts table sized for any circuit the simulator can handle.
 * Sixteen qubits gives 2^16 = 65536 possible outcomes, although in
 * practice circuits have far fewer classical bits than qubits. */
#define MAIN_MAX_OUTCOMES (1u << 16)
static uint32_t counts[MAIN_MAX_OUTCOMES];

/*
 * Print one histogram row. The label is the binary representation of
 * the outcome, big-endian for human readability (bit 0 on the right,
 * the way people normally write numbers). The bar is rounded to the
 * nearest column.
 */
static void print_row(uint32_t outcome, uint32_t count, uint32_t shots,
                      uint32_t num_bits)
{
    assert(shots > 0u);
    assert(num_bits > 0u);
    assert(num_bits <= 32u);

    const uint32_t BAR_WIDTH = 40u;
    double frac = (double)count / (double)shots;
    uint32_t bar_len = (uint32_t)(frac * (double)BAR_WIDTH + 0.5);

    (void)printf("  |");
    /* Print bits high-to-low so the most-significant bit shows first. */
    for (uint32_t b = num_bits; b > 0u; b--) {
        uint32_t bit = (outcome >> (b - 1u)) & 1u;
        (void)putchar(bit != 0u ? '1' : '0');
    }
    (void)printf(">  %6u  ", (unsigned)count);

    for (uint32_t b = 0u; b < bar_len; b++) {
        (void)putchar('#');
    }
    (void)printf("  %5.1f%%\n", frac * 100.0);
}

static void print_histogram(const uint32_t *table, uint32_t num_outcomes,
                            uint32_t shots, uint32_t num_bits)
{
    assert(table != NULL);

    (void)printf("\n  Histogram (%u shots)\n", (unsigned)shots);
    (void)printf("  ---------------------------\n");

    uint32_t end = num_outcomes;
    if (end > (uint32_t)MAIN_MAX_OUTCOMES) {
        end = (uint32_t)MAIN_MAX_OUTCOMES;
    }
    for (uint32_t v = 0u; v < end; v++) {
        if (table[v] == 0u) {
            continue;
        }
        print_row(v, table[v], shots, num_bits);
    }
}

/*
 * Dispatch the subcommand string to the matching circuit builder.
 * Returns 0 on success, 1 if the name was not recognised.
 */
static int build_module(const char *name, tir_module_t *M)
{
    assert(name != NULL);
    assert(M != NULL);

    if (strcmp(name, "bell") == 0) {
        build_bell_module(M);
        return 0;
    }
    if (strcmp(name, "ghz") == 0) {
        build_ghz_module(M);
        return 0;
    }
    if (strcmp(name, "deutsch") == 0) {
        build_deutsch_module(M);
        return 0;
    }
    if (strcmp(name, "grover") == 0) {
        build_grover_module(M);
        return 0;
    }
    if (strcmp(name, "qftlib") == 0) {
        build_qftlib_module(M);
        return 0;
    }
    if (strcmp(name, "groverlib") == 0) {
        build_groverlib_module(M);
        return 0;
    }
    return 1;
}

static void print_usage(void)
{
    (void)fprintf(stderr,
        "usage: ernest [flags] [subcommand|circuit] [args]\n"
        "  circuit    one of: bell, ghz, deutsch, grover, qftlib, groverlib\n"
        "             (default: bell; qftlib/groverlib use libqstd)\n"
        "  compile P  parse the OpenQASM file at P, run the pipeline\n"
        "  lex        run the ERNESTLX lexer on Bell state QASM\n"
        "  roundtrip  build each demo, emit, lex, parse, compare\n"
        "  abend      deliberately trigger an ABEND, print the dump\n"
        "  opt        run the ERNESTOP optimisation demo\n"
        "  corpus     parse the real Qiskit-generated QASM corpus\n"
        "  verify     religious correctness check across every demo\n"
        "  --no-opt   disable the optimiser (default: on, level 1)\n"
        "  --target N target vendor: generic (default) or ibm\n"
        "  --xref     print a cross-reference listing showing provenance\n"
        "  --aot      simulate via AOT-compiled native code (calls gcc)\n"
        "  --coupling NAME    qubit routing topology, one of:\n"
        "             linear_5, linear_16, ring_5, ring_8, grid_4x4,\n"
        "             heavy_hex_7, ibm_falcon_5_t, full_16\n"
        "  --coupling-file P  load a coupling map from a plain-text edge list\n"
        "             (one 'u v' edge per line, # comments). Use this for\n"
        "             real IBM device topologies dumped from Qiskit.\n"
        "  --qasm-out P       write the optimised OpenQASM 3 to a file,\n"
        "             for downstream submission to a vendor's API.\n"
        "  --snap     enable MVS-style snapshot collection at pass\n"
        "             boundaries; dump at exit (or to --snap-out)\n"
        "  --snap-out P       write the snap dump to a file at exit\n"
        "  doom [N]   QPIE-encode a DOOM image, QFT-roundtrip, sample N times\n"
        "  --wad P    path to a Doom IWAD to pull TITLEPIC from (for doom demo)\n"
        "  --image P  path to a PPM image to use instead (for doom demo)\n"
        "  shots      number of measurement shots (default: 8192)\n");
}

int main(int argc, char **argv)
{
    /* Defaults. Optimiser on at level one unless --no-opt says otherwise. */
    bool          no_opt    = false;
    bool          xref      = false;
    bool          aot       = false;
    opt_target_t  target    = OPT_TARGET_GENERIC;
    const char   *circuit   = "bell";
    uint32_t      shots     = 8192u;
    const char   *doom_wad  = NULL;
    const char   *doom_img  = NULL;
    const char   *qasm_out  = NULL;
    bool          snap_on   = false;
    const char   *snap_out  = NULL;

    /* Two-pass argv handling: pull out flags, then process positional
     * args in the order they were given. Keeps the flag set extensible
     * without rewriting the dispatcher every time a new switch turns
     * up. */
    const char *positional[8] = { NULL };
    uint32_t    num_positional = 0u;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[i], "--no-opt") == 0) {
            no_opt = true;
            continue;
        }
        if (strcmp(argv[i], "--xref") == 0) {
            xref = true;
            continue;
        }
        if (strcmp(argv[i], "--aot") == 0) {
            aot = true;
            continue;
        }
        if (strncmp(argv[i], "--wad=", 6) == 0) {
            doom_wad = argv[i] + 6;
            continue;
        }
        if (strcmp(argv[i], "--wad") == 0 && i + 1 < argc) {
            i++;
            doom_wad = argv[i];
            continue;
        }
        if (strncmp(argv[i], "--image=", 8) == 0) {
            doom_img = argv[i] + 8;
            continue;
        }
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            i++;
            doom_img = argv[i];
            continue;
        }
        if (strncmp(argv[i], "--coupling=", 11) == 0) {
            const coupling_graph_t *G = route_lookup_coupling(argv[i] + 11);
            if (G == NULL) {
                (void)fprintf(stderr,
                    "ernest: unknown --coupling '%s' "
                    "(try linear_5, ring_5, heavy_hex_7, full_16)\n",
                    argv[i] + 11);
            } else {
                route_set_active_coupling(G);
            }
            continue;
        }
        if (strcmp(argv[i], "--coupling") == 0 && i + 1 < argc) {
            i++;
            const coupling_graph_t *G = route_lookup_coupling(argv[i]);
            if (G == NULL) {
                (void)fprintf(stderr,
                    "ernest: unknown --coupling '%s' "
                    "(try linear_5, ring_5, heavy_hex_7, full_16)\n",
                    argv[i]);
            } else {
                route_set_active_coupling(G);
            }
            continue;
        }
        if (strncmp(argv[i], "--coupling-file=", 16) == 0) {
            const coupling_graph_t *G =
                route_load_coupling_from_file(argv[i] + 16);
            if (G == NULL) {
                (void)fprintf(stderr,
                    "ernest: failed to load coupling map from '%s'\n",
                    argv[i] + 16);
            } else {
                route_set_active_coupling(G);
            }
            continue;
        }
        if (strcmp(argv[i], "--coupling-file") == 0 && i + 1 < argc) {
            i++;
            const coupling_graph_t *G = route_load_coupling_from_file(argv[i]);
            if (G == NULL) {
                (void)fprintf(stderr,
                    "ernest: failed to load coupling map from '%s'\n",
                    argv[i]);
            } else {
                route_set_active_coupling(G);
            }
            continue;
        }
        if (strncmp(argv[i], "--qasm-out=", 11) == 0) {
            qasm_out = argv[i] + 11;
            continue;
        }
        if (strcmp(argv[i], "--qasm-out") == 0 && i + 1 < argc) {
            i++;
            qasm_out = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--snap") == 0) {
            snap_on = true;
            continue;
        }
        if (strncmp(argv[i], "--snap-out=", 11) == 0) {
            snap_out = argv[i] + 11;
            snap_on = true;
            continue;
        }
        if (strcmp(argv[i], "--snap-out") == 0 && i + 1 < argc) {
            i++;
            snap_out = argv[i];
            snap_on = true;
            continue;
        }
        /* --target=NAME or --target NAME, accepting "generic" and
         * "ibm" today. Anything else falls back to GENERIC with a
         * warning to stderr so the user knows their flag didn't
         * land. */
        if (strncmp(argv[i], "--target=", 9) == 0) {
            const char *t = argv[i] + 9;
            if (strcmp(t, "ibm") == 0)          { target = OPT_TARGET_IBM;     }
            else if (strcmp(t, "generic") == 0) { target = OPT_TARGET_GENERIC; }
            else { (void)fprintf(stderr, "ernest: unknown --target '%s'\n", t); }
            continue;
        }
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "ibm") == 0)          { target = OPT_TARGET_IBM;     }
            else if (strcmp(argv[i], "generic") == 0) { target = OPT_TARGET_GENERIC; }
            else { (void)fprintf(stderr, "ernest: unknown --target '%s'\n", argv[i]); }
            continue;
        }
        if (num_positional < 8u) {
            positional[num_positional++] = argv[i];
        }
    }

    uint32_t opt_level = no_opt ? OPT_LEVEL_NONE : OPT_LEVEL_BASIC;

    /* SNAP setup. The active log gets shared with every pipeline
     * stage via the snap module's file-scope pointer; the log
     * itself is dumped at exit (or written to a file) via the
     * atexit handler so subcommands that return early still emit. */
    static snap_log_t snap_log;
    if (snap_on) {
        snap_log_init(&snap_log);
        snap_log_enable(&snap_log, 1);
        snap_set_active_log(&snap_log);
        snap_dump_log = &snap_log;
        snap_dump_path = snap_out;
        (void)atexit(snap_exit_handler);
    }

    if (num_positional > 0u) {
        if (strcmp(positional[0], "lex") == 0) {
            return run_lex_test();
        }
        if (strcmp(positional[0], "roundtrip") == 0) {
            return run_roundtrip();
        }
        if (strcmp(positional[0], "abend") == 0) {
            return run_abend_test();
        }
        if (strcmp(positional[0], "opt") == 0) {
            return run_opt_test();
        }
        if (strcmp(positional[0], "corpus") == 0) {
            return run_corpus_test();
        }
        if (strcmp(positional[0], "verify") == 0) {
            return run_verify();
        }
        if (strcmp(positional[0], "doom") == 0) {
            uint32_t doom_shots = 0u;  /* 0 = let demo pick default */
            if (num_positional > 1u) {
                long s = strtol(positional[1], NULL, 10);
                if (s > 0 && s <= 100000000) {
                    doom_shots = (uint32_t)s;
                }
            }
            return run_doom_demo(doom_img, doom_wad, doom_shots);
        }
        if (strcmp(positional[0], "compile") == 0) {
            if (num_positional < 2u) {
                (void)fprintf(stderr,
                    "ernest: 'compile' needs a file path\n");
                return 1;
            }
            /* For the compile subcommand the layout is:
             *   compile  <path>  [shots]
             * so shots lives at positional[2], not positional[1]. */
            if (num_positional > 2u) {
                long s = strtol(positional[2], NULL, 10);
                if (s > 0 && s <= 100000000) {
                    shots = (uint32_t)s;
                }
            }
            return run_compile(positional[1],
                               xref ? 1 : 0,
                               no_opt ? 1 : 0,
                               (target == OPT_TARGET_IBM) ? 1 : 0,
                               aot ? 1 : 0,
                               shots);
        }
        circuit = positional[0];
    }
    if (num_positional > 1u) {
        long s = strtol(positional[1], NULL, 10);
        if (s > 0 && s <= 100000000) {
            shots = (uint32_t)s;
        }
    }

    /* Build the module. */
    static tir_module_t M;
    if (build_module(circuit, &M) != 0) {
        (void)fprintf(stderr, "ernest: unknown circuit '%s'\n", circuit);
        print_usage();
        return 1;
    }

    /* Optimise. The driver prints the job-step output itself; we
     * still keep an MNOTE log alongside for the diagnostics channel,
     * which any future pass with something to say will use. */
    if (opt_level != OPT_LEVEL_NONE) {
        static mnote_log_t opt_log;
        mnote_init(&opt_log, "ERNESTOP");
        (void)opt_run(&M, opt_level, target, &opt_log);
        mnote_print(&opt_log, stdout);
        (void)printf("\n");
    }

    /* Show the TIR. */
    (void)printf("-- TIR (Ernest Intermediate Representation) --\n");
    tir_print_module(&M, stdout);

    /* If the user asked for a cross-reference listing, print it
     * between the TIR and the QASM. Shows the provenance fields
     * (source location, origin pass) that don't fit in the normal
     * TIR dump. */
    if (xref) {
        (void)printf("\n");
        tir_print_xref(&M, stdout);
    }

    /* Show the OpenQASM. */
    (void)printf("\n-- OpenQASM 3.0 --\n");
    ernest_emit_qasm3(&M, stdout);

    /* If the user asked for a separate QASM file (typically because
     * they want to feed it to a submission script), write it now.
     * No diagnostic noise in the output file, just the QASM. */
    if (qasm_out != NULL) {
        FILE *qf = fopen(qasm_out, "w");
        if (qf == NULL) {
            (void)fprintf(stderr,
                "ernest: cannot open --qasm-out '%s' for write\n",
                qasm_out);
        } else {
            ernest_emit_qasm3(&M, qf);
            (void)fclose(qf);
            (void)printf("\n-- wrote optimised OpenQASM 3 to %s --\n",
                         qasm_out);
        }
    }

    uint32_t num_bits = tir_total_bits(&M);
    uint32_t num_outcomes = (num_bits == 0u) ? 1u : (1u << num_bits);
    assert(num_outcomes <= (uint32_t)MAIN_MAX_OUTCOMES);

    /* Zero the counts table. */
    for (uint32_t i = 0u; i < num_outcomes; i++) {
        counts[i] = 0u;
    }

    if (aot) {
        /* AOT path: hand the module to the codegen, get the
         * histogram back populated. */
        (void)printf("\n");
        clock_t t0 = clock();
        int rc = aot_compile_and_run(&M, shots, counts, num_outcomes, stdout);
        clock_t t1 = clock();
        if (rc != 0) {
            (void)fprintf(stderr, "ernest: AOT failed RC=%d\n", rc);
            return 8;
        }
        double secs = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
        (void)printf("ERNESTAO WALL     %.3fs total (codegen+gcc+exec)\n", secs);
    } else {
        /* Interpreted path: run shots through the in-process
         * simulator one by one. */
        static sim_state_t S;
        srand((unsigned int)time(NULL));

        for (uint32_t s = 0u; s < shots; s++) {
            sim_init(&S, &M);
            sim_run_shot(&S, &M);
            if (S.status != SIM_OK) {
                abend_t A;
                abend_init(&A, sim_status_code(S.status));
                abend_set_reason(&A, "%s", S.status_reason);
                abend_set_psw(&A, ABEND_PSW_RUNNING,
                              S.current_inst, S.status_qubit);
                abend_dump(&A, &S, &M, stdout);
                return 8;
            }
            uint32_t outcome = sim_creg_as_uint(&S);
            assert(outcome < num_outcomes);
            counts[outcome]++;
        }
    }

    /* Show the histogram. */
    print_histogram(counts, num_outcomes, shots,
                    (num_bits == 0u) ? 1u : num_bits);

    return 0;
}
