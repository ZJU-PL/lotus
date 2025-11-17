MCP – Model Checking Platform
==============================

Model checking / verification front-end built on top of SMT solving.

**Binary**: ``mcp``  
**Location**: ``tools/mcp/mcp.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/mcp [options] file.smt2

Typical workflow:

1. Encode your verification problem in SMT-LIB2 format
2. Run ``mcp`` on the SMT2 file
3. Inspect SAT/UNSAT result and model (if any)

