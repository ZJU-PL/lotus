IKOS
====

[![License](https://img.shields.io/badge/license-NOSA%201.3-blue.svg)](LICENSE.pdf)
[![Release](https://img.shields.io/badge/release-v3.5-orange.svg)](https://github.com/NASA-SW-VnV/ikos/releases/tag/v3.5)
[![Linux Build](https://github.com/NASA-SW-VnV/ikos/actions/workflows/build-linux.yml/badge.svg)](https://github.com/NASA-SW-VnV/ikos/actions/workflows/build-linux.yml)
[![MacOS Build](https://github.com/NASA-SW-VnV/ikos/actions/workflows/build-macos.yml/badge.svg)](https://github.com/NASA-SW-VnV/ikos/actions/workflows/build-macos.yml)

IKOS (Inference Kernel for Open Static Analyzers) is a static analyzer for C/C++ based on the theory of Abstract Interpretation.

Introduction
------------

IKOS started as a C++ library designed to facilitate the development of sound static analyzers based on [Abstract Interpretation](https://www.di.ens.fr/~cousot/AI/IntroAbsInt.html). Specialization of a static analyzer for an application or family of applications is critical for achieving both precision and scalability. Developing such an analyzer is arduous and requires significant expertise in Abstract Interpretation.

IKOS provides a generic and efficient implementation of state-of-the-art Abstract Interpretation data structures and algorithms, such as control-flow graphs, fixpoint iterators, numerical abstract domains, etc. IKOS is independent of a particular programming language.

IKOS also provides a C and C++ static analyzer based on [LLVM](https://llvm.org). It implements scalable analyses for detecting and proving the absence of runtime errors in C and C++ programs.

License
-------

IKOS has been released under the NASA Open Source Agreement version 1.3, see [LICENSE.pdf](LICENSE.pdf)

Contact
-------

ikos@lists.nasa.gov

Release notes
-------------

See [Releases](https://github.com/NASA-SW-VnV/ikos/releases).

Troubleshooting
---------------

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md)

Installation
------------

To install IKOS on **Linux** or **macOS**, we recommend to use **[Homebrew](https://brew.sh/)**.

First, install **Homebrew** by following [these instructions](https://docs.brew.sh/Installation).

Then, simply run:
```
$ brew install nasa-sw-vnv/core/ikos
```

For Windows, consider using [Windows Subsystem for Linux](https://learn.microsoft.com/en-us/windows/wsl/install).

How to run IKOS
---------------

Suppose we want to analyze the following C program in a file, called *loop.c*:

```c
 1: #include <stdio.h>
 2: int a[10];
 3: int main(int argc, char *argv[]) {
 4:     size_t i = 0;
 5:     for (;i < 10; i++) {
 6:         a[i] = i;
 7:     }
 8:     a[i] = i;
 9:     printf("%i", a[i]);
10: }
```

To analyze this program with IKOS, simply run:

```
$ ikos loop.c
```

You shall see the following output. IKOS reports two occurrences of buffer overflow at line 8 and 9.

```
[*] Compiling loop.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Analyzing entry point 'main'
[*] Checking properties for entry point 'main'

# Time stats:
clang        : 0.037 sec
ikos-analyzer: 0.023 sec
ikos-pp      : 0.007 sec

# Summary:
Total number of checks                : 7
Total number of unreachable checks    : 0
Total number of safe checks           : 5
Total number of definite unsafe checks: 2
Total number of warnings              : 0

The program is definitely UNSAFE

# Results
loop.c: In function 'main':
loop.c:8:10: error: buffer overflow, trying to access index 10 of global variable 'a' of 10 elements
    a[i] = i;
         ^
loop.c: In function 'main':
loop.c:9:18: error: buffer overflow, trying to access index 10 of global variable 'a' of 10 elements
    printf("%i", a[i]);
                 ^
```

The `ikos` command takes a source file (`.c`, `.cpp`) or a LLVM bitcode file (`.bc`) as input, analyzes it to find runtime errors (also called undefined behaviors), creates a result database `output.db` in the current working directory and prints a report.

In the report, each line has one of the following status:

* **safe**: the statement is proven safe;
* **error**: the statement always results into an error (or is unreachable);
* **unreachable**: the statement is never executed;
* **warning** may mean three things:
   1. the statement results into an error for some executions, or
   2. the static analyzer did not have enough information to conclude (check dependent on an external input, for instance), or
   3. the static analyzer was not powerful enough to prove the absence of errors;

By default, ikos shows warnings and errors directly in your terminal, like a compiler would do.

If the analysis report is too big, you shall use:
* `ikos-report output.db` to examine the report in your terminal
* `ikos-view output.db` to examine the report in a web interface

### Analyze a program with unmodeled external functions

When the analyzed program calls a library function that IKOS does not model, the
analyzer executes the call conservatively (`exec_unknown_extern_call`): the return
value and the written buffers become fully arbitrary, and the soundness checker
reports a *"call to an unknown extern function"* warning at every such call site.
This is sound, but imprecise. The API spec pipeline generates analysis stubs for
those functions *before* running the analysis, and then runs the original analysis
on the stubbed module:

```
# what is missing?
$ ikos-stub check prog.bc

# 0 LLM: install stubs already validated in the stub database
$ python3 tool/api-spec/ikos-api-spec.py prefetch -o stubs prog.bc

# generate + validate the rest with a large language model
$ python3 tool/api-spec/ikos-api-spec.py generate -o stubs --provider deepseek prog.bc

# compile + link the stub library, with signature verification
$ ikos-stub link --stub-dir stubs -o prog.stubbed.bc prog.bc

# the original analysis, run on the stubbed module
$ ikos prog.stubbed.bc
```

* `ikos-stub check` lists every external function of the bitcode with its
  disposition (`missing` = no builtin model and no stub file yet);
* `prefetch` installs, with zero LLM calls, the stub bodies that earlier runs
  already validated and recorded in the stub database
  (`~/.cache/ikos-api-spec/stubdb`); functions the database does not know are
  reported as `missing`;
* `generate` asks a large language model for the remaining missing functions.
  The C signature stays deterministic (rendered from the IR `declare` line), and
  every candidate body is compiled, signature-checked and differentially
  validated against the real library before it is installed; validated bodies
  are recorded in the stub database, so later programs reuse them for free.
  `--domain <name>` takes the same token as the analysis run's `-d` flag: the
  differential judge then admits stub claims by that domain's join semantics
  (by default the stub is validated under both the interval and the congruence
  join families, so it is sound under every backend); relational backends
  (dbm, apron octagon / polyhedra / linear equalities) additionally get stub
  bodies relating the return to the parameters (two-variable `__ikos_assume`),
  judged input by input so they stay sound under every backend while paying
  precision under theirs;
* `ikos-stub link` compiles the stub library, links it with the program bitcode
  and verifies every signature (llvm-link alone does not check signatures);
* `ikos prog.stubbed.bc` is the original analysis, run on the stubbed module:
  the *"call to an unknown extern function"* warnings are gone, and the
  analyzer now sees the stub semantics statement by statement.

The same flow applies to a whole project: run it on the bitcode files produced by
your build (see [Analyze a whole project with ikos-scan](analyzer/README.md#analyze-a-whole-project-with-ikos-scan)).

For comparison experiments, use the unmodified pipeline as the baseline: analyze
the original bitcode directly (`ikos prog.bc`, where unmodeled externals keep the
conservative behavior) against the stub pipeline (`ikos prog.stubbed.bc`). Both
arms run the same `ikos` binary with the same flags, so the presence of the stubs
is the only variable; `ikos-report` on the two result databases gives the
warning/status deltas.

Further information:
* [Analyze a whole project with ikos-scan](analyzer/README.md#analyze-a-whole-project-with-ikos-scan)
* [Examine a report with ikos-view](analyzer/README.md#examine-a-report-with-ikos-view)
* [Analysis Options](analyzer/README.md#analysis-options)
  - [Checks](analyzer/README.md#checks)
  - [Numerical abstract domains](analyzer/README.md#numerical-abstract-domains)
  - [Entry points](analyzer/README.md#entry-points)
  - [Multi-threading](analyzer/README.md#multi-threading)
  - [Optimization level](analyzer/README.md#optimization-level)
  - [Inter-procedural vs Intra-procedural](analyzer/README.md#inter-procedural-vs-intra-procedural)
  - [Fixpoint engine parameters](analyzer/README.md#fixpoint-engine-parameters)
  - [Partitioning](analyzer/README.md#partitioning)
  - [Hardware addresses](analyzer/README.md#hardware-addresses)
  - [Other analysis options](analyzer/README.md#other-analysis-options)
* [Report Options](analyzer/README.md#report-options)
  - [Format](analyzer/README.md#format)
  - [File](analyzer/README.md#file)
  - [Status Filter](analyzer/README.md#status-filter)
  - [Analysis Filter](analyzer/README.md#analysis-filter)
  - [Verbosity](analyzer/README.md#verbosity)
  - [Other report options](analyzer/README.md#other-report-options)
* [APRON Support](analyzer/README.md#apron-support)
* [Analysis Assumptions](analyzer/README.md#analysis-assumptions)
* [Analyze an embedded software requiring a cross-compiler](analyzer/README.md#analyze-an-embedded-software-requiring-a-cross-compiler)
* [Model library functions to reduce warnings](analyzer/README.md#model-library-functions-to-reduce-warnings)
* [Stub unmodeled external functions with LLM-generated specs](#analyze-a-program-with-unmodeled-external-functions)

Build from source
-----------------

Below are instructions to build IKOS from source.
This is only for advanced users that want to either package IKOS for an operating system or to experiment with the codebase. Otherwise, please follow the instructions [above](#installation).

### Dependencies

To build and run the analyzer, you will need the following dependencies:

* A C++ compiler that supports C++14 (gcc >= 4.9.2 or clang >= 3.4)
* CMake >= 3.4.3
* GMP >= 4.3.1
* Boost >= 1.55
* Python >= 3.3
* SQLite >= 3.6.20
* TBB >= 2
* LLVM and Clang 14.0.x
* (Optional) APRON >= 0.9.10

Most of them can be installed using your package manager.

Note: If you build LLVM from source, you need to enable run-time type information (RTTI).

### Build and Install

Now that you have all the dependencies on your system, you can build and install IKOS.

As you open the IKOS distribution, you shall see the following directory structure:

```
.
├── CMakeLists.txt
├── LICENSE.pdf
├── README.md
├── RELEASE_NOTES.md
├── TROUBLESHOOTING.md
├── analyzer
├── ar
├── cmake
├── core
├── doc
├── frontend
├── script
└── test
```

IKOS uses the CMake build system. You will need to specify an installation directory that will contain all the binaries, libraries and headers after installation. If you do not specify this directory, CMake will install everything under `install` in the root directory of the distribution. In the following steps, we will install IKOS under `/path/to/ikos-install-directory`.

Here are the steps to build and install IKOS:

```
$ mkdir build
$ cd build
$ cmake -DCMAKE_INSTALL_PREFIX=/path/to/ikos-install-directory ..
$ make
$ make install
```

Then, add IKOS in your PATH (consider adding this in your .bashrc):

```
$ PATH="/path/to/ikos-install-directory/bin:$PATH"
```

### Tests

To build and run the tests, simply type:

```
$ make check
```

Contributors
------------

See [CONTRIBUTORS.md](CONTRIBUTORS.md)

Publications
------------

* Sung Kook Kim, Arnaud J. Venet, Aditya V. Thakur. **Deterministic Parallel Fixpoint Computation.** In _Principles of Programming Languages (POPL 2020)_, New Orleans, Louisiana ([PDF](https://arxiv.org/pdf/1909.05951.pdf)).

* Guillaume Brat, Jorge Navas, Nija Shi and Arnaud Venet. **IKOS: a Framework for Static Analysis based on Abstract Interpretation.** In _Proceedings of the International Conference on Software Engineering and Formal Methods (SEFM 2014)_, Grenoble, France ([PDF](http://ti.arc.nasa.gov/publications/16610/download/)).

* Arnaud Venet. **The Gauge Domain: Scalable Analysis of Linear Inequality Invariants.** In _Proceedings of Computer Aided Verification (CAV 2012)_, Berkeley, California, USA 2012. Lecture Notes in Computer Science, pages 139-154, volume 7358, Springer 2012 ([PDF](http://ti.arc.nasa.gov/publications/4767/download/)).

Coding Standards
----------------

See [doc/CODING_STANDARDS.md](doc/CODING_STANDARDS.md)

Overview of the source code
---------------------------

See [doc/OVERVIEW.md](doc/OVERVIEW.md)

API Census
----------

See [doc/API_CENSUS.md](doc/API_CENSUS.md)

Stub Tool
---------

See [doc/STUB_TOOL.md](doc/STUB_TOOL.md)

API Spec (LLM stub generation)
------------------------------

See [tool/api-spec/README.md](tool/api-spec/README.md)
