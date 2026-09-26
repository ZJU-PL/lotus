Type Hierarchy
==============

``TypeHierarchy`` provides class-hierarchy recovery for C++-style programs.

**Headers**: ``include/Analysis/TypeHierarchy/``

**Implementation**: ``lib/Analysis/TypeHierarchy/``

Overview
--------

This subsystem reconstructs inheritance and virtual-function-table structure
from LLVM IR and debug information. It is useful for analyses that need dynamic
type information or devirtualization-style reasoning.

Main components
---------------

- ``TypeHierarchy`` defines the generic hierarchy-query interface.
- ``DIBasedTypeHierarchy`` and ``DIBasedTypeHierarchyData`` recover hierarchy
  information from debug metadata.
- ``LLVMVFTable`` and ``LLVMVFTableData`` model virtual-function tables.
- ``TypeHierarchyAnalysis`` packages the functionality as an analysis pass.

Call Graph Construction Algorithms
----------------------------------

In addition to base hierarchy recovery, the module provides classic algorithms for 
call graph construction and devirtualization:

- **CHA (Class Hierarchy Analysis)**: Statically resolves virtual calls by traversing the class inheritance tree without analyzing program control flow.
- **RTA (Rapid Type Analysis)**: Refines CHA by tracking which classes are instantiated (i.e., identifying `new` allocations).
- **VTA (Variable Type Analysis)**: A flow-insensitive algorithm that computes type constraints for variables to further refine the set of possible targets for a virtual call.
- **OTF (On-The-Fly Call Graph Construction)**: Constructs the call graph on-the-fly simultaneously with points-to analysis for the highest precision among the hierarchy-based approaches.

Typical use cases
-----------------

- Build class-hierarchy facts for indirect-call resolution.
- Recover subtype relationships for object-oriented analyses.
- Provide a basis for vtable- and dynamic-dispatch reasoning.

Limitations
-----------

Recovery depends on the type and debug metadata present in the module.  A
hierarchy query is therefore evidence for resolving a dynamic dispatch, not a
guarantee that every runtime type has been recovered.  Clients should preserve
an unknown or conservative target case when metadata is missing, incomplete,
or inconsistent across linked modules.

