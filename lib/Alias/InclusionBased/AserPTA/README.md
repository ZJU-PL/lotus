# AserPTA - LLVM Pointer Analysis Framework


## Publications

- ICSE 22: PUS: A Fast and Highly Efficient Solver for Inclusion-based Pointer Analysis.  Peiming Liu, Yanze Li, Brad Swain and Jeff Huang.
https://peimingliu.github.io/asset/pic/PUS.pdf
- PLDI 21: When Threads Meet Events: Efficient and Precise Static Race Detection with Origins. Bozhen Liu, Peiming Liu, Yanze Li, Chia-Che Tsai, Dilma Da Silva and Jeff Huang.


## Features

- **Multiple Context Sensitivities**:
  - Context-insensitive (`NoCtx`)
  - K-call-site sensitivity (`KCallSite<K>`)
  - K-origin sensitivity (`KOrigin<K>`)
 
- **Memory Models**:
  - **Field-Insensitive (FI)**: Treats entire objects as single entities
  - **Field-Sensitive (FS)**: Models individual struct fields separately

- **Solver Algorithms**:
  - **Andersen**: Basic worklist-based algorithm
  - **WavePropagation**: SCC detection with differential propagation
  - **DeepPropagation**: Enhanced cycle detection for improved performance
  - **PartialUpdateSolver**: Hybrid solver with incremental updates

- **Constraint-Based Analysis**: Five constraint types:
  - `addr_of`: Address-of operations (`p = &obj`)
  - `copy`: Assignments (`p = q`)
  - `load`: Dereference loads (`p = *q`)
  - `store`: Dereference stores (`*p = q`)
  - `offset`: Field access via GEP (`p = &obj->field`)

- **Optional Points-to Cache**: `lotus-alias-aser-aa` can persist the solved
  AserPTA graph through `--write-ander=<file>` and load it with
  `--read-ander=<file>`. The cache records the exact node count, a semantic
  node-layout digest, and every points-to set. A load either installs a fully
  matching cache, explicitly falls back to constraint solving to reconstruct a
  changed graph, or fails when the final node layout still differs. Loading
  rebuilds indirect-call edges from the restored function-pointer facts.

## Architecture

### Core Components

1. **Context Models** (`PointerAnalysis/Context/`)
   - Context evolution strategies for inter-procedural analysis

2. **Constraint Graph** (`PointerAnalysis/Graph/`)
   - Graph-based representation of pointer constraints
   - Nodes: `CGPtrNode` (pointers), `CGObjNode` (objects), `CGSuperNode` (collapsed SCCs)

3. **Memory Models** (`PointerAnalysis/Models/MemoryModel/`)
   - Field-sensitive and field-insensitive object representations
   - Memory layout analysis for complex types

4. **Language Models** (`PointerAnalysis/Models/LanguageModel/`)
   - C/C++ semantics handling
   - Heap allocation modeling
   - Thread creation interception (`pthread_create`)

5. **Solvers** (`PointerAnalysis/Solver/`)
   - Multiple propagation strategies
   - BitVector-based points-to set representation

6. **Preprocessing Passes** (`PreProcessing/Passes/`)
   - IR canonicalization for field-sensitive analysis
   - Heap API normalization
   - Exception handler removal
