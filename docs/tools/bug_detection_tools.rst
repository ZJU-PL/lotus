Bug Detection Tools
=====================

Static analysis tools for detecting various types of bugs and vulnerabilities.

Lotus Kint Static Bug Finder
-----------------------------

**Binary**: ``build/bin/lotus-kint``

Static bug finder for integer-related and taint-style bugs.

**Usage**:
.. code-block:: bash

   ./build/bin/lotus-kint [options] <input IR file>

**Bug Checkers**:
* ``-check-all``: Enable all checkers
* ``-check-int-overflow``: Integer overflow detection
* ``-check-div-by-zero``: Division by zero detection
* ``-check-bad-shift``: Invalid shift detection
* ``-check-array-oob``: Array out of bounds detection
* ``-check-dead-branch``: Dead branch detection

**Performance Options**:
* ``-function-timeout=<seconds>``: Timeout per function (default: 10)
* ``-log-level=<level>``: ``debug``, ``info``, ``warning``, ``error``, ``none``
* ``-quiet``: Suppress most output
* ``-log-to-file=<filename>``: Redirect logs to file

**Examples**:
.. code-block:: bash

   ./build/bin/lotus-kint -check-all example.ll
   ./build/bin/lotus-kint -check-int-overflow -check-div-by-zero example.ll
   ./build/bin/lotus-kint -function-timeout=30 -log-level=debug example.ll



Buffer Overflow Detection
-------------------------

Integrated into Lotus Kint through array out of bounds checker.

**Usage**:
.. code-block:: bash

   ./build/bin/lotus-kint -check-array-oob example.ll

**Detection**: Array bounds, buffer overruns, string vulnerabilities

Memory Safety Analysis
----------------------

**Tools**:
* Null Pointer, Use-After-Free, etc.: ``lotus-gvfa``
* Buffer Overflow: ``lotus-kint -check-array-oob``

**Comprehensive Analysis**:
.. code-block:: bash

   ./build/bin/lotus-kint -check-array-oob example.l   
   ./build/bin/lotus-gvfa -vuln-type=nullpointer example.bc

Bug Detection Workflow
----------------------

1. **Compile to LLVM IR**:
   .. code-block:: bash

      clang -emit-llvm -c example.c -o example.bc
      clang -emit-llvm -S example.c -o example.ll

2. **Run Analysis**:
   .. code-block:: bash

      ./build/bin/lotus-kint -check-all example.ll
      ./build/bin/lotus-taint example.bc

3. **Review Results**: Check vulnerabilities, validate findings, prioritize fixes

Output Examples
---------------

**Integer Overflow**:
.. code-block:: text

   Bug Report:
   Function: main
   Location: example.c:15:5
   Type: Integer Overflow
   Severity: HIGH
   Code: result = a + b;

**Null Pointer**:
.. code-block:: text

   Null Pointer Vulnerability:
   Function: process_data
   Location: example.c:25:10
   Type: Null Pointer Dereference
   Severity: CRITICAL
   Code: *ptr = value;

Performance
-----------

* Small programs (< 1K functions): < 5 seconds
* Medium programs (1K-10K functions): 5-30 seconds
* Large programs (> 10K functions): 30+ seconds

**Tips**: Use timeouts, appropriate logging, precision trade-offs

Tool Selection
--------------

| Bug Type | Tool | Options |
|----------|------|---------|
| Integer Bugs | Lotus Kint | ``-check-int-overflow``, ``-check-div-by-zero`` |
| Memory Bugs | lotus-gvfa, lotus-kint | ``-check-array-oob`` |
| Taint Bugs | lotus-taint | Various options |