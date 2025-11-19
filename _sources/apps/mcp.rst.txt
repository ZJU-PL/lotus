MCP (Model Context Protocol) Server
====================================

MCP provides call graph analysis and querying capabilities for LLVM bitcode, enabling programmatic access to call graph information.

**Location**: ``lib/Apps/MCP/``

**Build Target**: ``tools/mcp/mcp``

Overview
--------

The MCP server extracts and analyzes call graphs from LLVM IR, providing:

- Direct call graph extraction (callers/callees)
- Transitive closure computation (reachability analysis)
- Export to JSON and DOT formats
- Query API for call graph information

Components
----------

**MCPServer** – Core call graph analysis engine:

* ``MCPServer.h`` – API for call graph operations
* ``MCPServer.cpp`` – Implementation of call graph extraction and analysis

**CallGraphData** – Data structure for call graph representation:

* ``callees`` – Map from function to its direct callees
* ``callers`` – Map from function to its direct callers
* ``allFunctions`` – Set of all functions in the module
* ``indirectCalls`` – List of indirect call sites

API
---

**Loading and Building**:

.. code-block:: cpp

   #include "Apps/MCP/MCPServer.h"
   
   MCPServer server;
   server.loadModule("program.bc");
   server.buildCallGraph();

**Query Operations**:

.. code-block:: cpp

   // Get direct callees of a function
   auto callees = server.getCallees("main");
   
   // Get direct callers of a function
   auto callers = server.getCallers("foo");
   
   // Get all functions in the module
   auto allFuncs = server.getAllFunctions();
   
   // Get all reachable functions (transitive closure)
   auto reachable = server.getReachableFunctions("main");
   
   // Check if one function can reach another
   bool canReach = server.canReach("main", "exit");

**Export Formats**:

.. code-block:: cpp

   // Export as JSON
   std::string json = server.exportAsJSON();
   
   // Export as DOT (Graphviz)
   std::string dot = server.exportAsDOT();

Command-Line Tool
-----------------

The ``mcp`` tool provides a command-line interface for call graph queries:

**Usage**:

.. code-block:: bash

   mcp <bitcode> <command> [args]

**Commands**:

* ``list`` – List all functions in the module
* ``callees <func>`` – Get direct callees of a function
* ``callers <func>`` – Get direct callers of a function
* ``reachable <func>`` – Get all reachable functions (transitive)
* ``can-reach <from> <to>`` – Check if one function can reach another
* ``export-json`` – Export call graph as JSON
* ``export-dot`` – Export call graph as DOT format

**Examples**:

.. code-block:: bash

   # List all functions
   ./build/bin/mcp program.bc list
   
   # Get callees of main
   ./build/bin/mcp program.bc callees main
   
   # Check reachability
   ./build/bin/mcp program.bc can-reach main exit
   
   # Export as DOT for visualization
   ./build/bin/mcp program.bc export-dot > callgraph.dot
   dot -Tpdf callgraph.dot -o callgraph.pdf

Limitations
-----------

* **Indirect Calls**: Indirect calls are detected but not resolved. The call graph only includes direct function calls.
* **Context-Insensitive**: The call graph is context-insensitive (no call-site distinction).
* **No Pointer Analysis**: Indirect calls through function pointers are not resolved.

Use Cases
---------

* **Impact Analysis**: Determine which functions are affected by changes
* **Dead Code Detection**: Find unreachable functions
* **Dependency Analysis**: Understand function call relationships
* **Visualization**: Generate call graph diagrams for documentation

Integration
-----------

MCP can be integrated into other Lotus analyses that need call graph information:

.. code-block:: cpp

   #include "Apps/MCP/MCPServer.h"
   
   MCPServer server;
   server.loadModule(bitcodeFile);
   server.buildCallGraph();
   
   // Use in analysis
   if (server.canReach("main", "vulnerable_function")) {
       // Analyze vulnerable path
   }

See Also
--------

- :doc:`../tools/mcp/index` – MCP tool documentation
- :doc:`../ir/index` – Intermediate representation documentation
- :doc:`../alias/index` – Alias analysis for resolving indirect calls

