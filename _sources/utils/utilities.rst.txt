Utility Libraries
==================

Lotus provides comprehensive utility libraries organized into two main categories: general-purpose utilities and LLVM-specific helpers.

General Utilities
-----------------

General-purpose utilities located in ``lib/Utils/General/`` and ``include/Utils/General/``.

Abstract Data Types (ADT)
~~~~~~~~~~~~~~~~~~~~~~~~~

Data structures for efficient program analysis:

* **BDD.h** - Binary Decision Diagram implementation
* **DisjointSet.h** - Disjoint set data structure (Union-Find)
* **Hashing.h** - Hash utilities and helpers
* **ImmutableMap.h** / **ImmutableSet.h** / **ImmutableTree.h** - Immutable collections
* **IntervalsList.h** - Interval list data structure
* **MapOfSets.h** - Map of sets container
* **OrderedSet.h** - Ordered set container
* **PushPopCache.h** - Cache with push/pop semantics
* **SortedVector.h** - Sorted vector implementation
* **TreeStream.h** - Tree-structured stream output
* **VectorMap.h** / **VectorSet.h** - Vector-based map/set containers
* **anatree.h** - Tree analysis utilities

**Usage**:
.. code-block:: cpp

   #include <Utils/General/ADT/DisjointSet.h>
   DisjointSet<int> ds;
   ds.makeSet(0);
   ds.makeSet(1);
   ds.doUnion(0, 1);
   int root = ds.findSet(0);

Iterator Utilities
~~~~~~~~~~~~~~~~~~

Iterator adapters and utilities:

* **DereferenceIterator.h** - Iterator that dereferences values
* **filter_iterator.h** - Filtered iterator
* **InfixOutputIterator.h** - Output iterator with infix separators
* **IteratorAdaptor.h** / **IteratorFacade.h** - Iterator base classes
* **IteratorRange.h** - Range wrapper for iterators
* **IteratorTrait.h** - Iterator trait utilities
* **MapValueIterator.h** - Iterator over map values
* **UniquePtrIterator.h** - Iterator for unique pointers

Parser Combinator Framework (pcomb)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Parser combinator library for building parsers:

* **pcomb.h** - Main header
* **Parser/** - Base parser classes (Parser, ParseResult, StringParser, RegexParser, etc.)
* **Combinator/** - Combinators (AltParser, SeqParser, ManyParser, LazyParser, etc.)
* **InputStream/** - Input stream abstractions

System and Core Utilities
~~~~~~~~~~~~~~~~~~~~~~~~~

* **System.h** - System information (OS detection, endianness, time utilities)
* **Timer.h** - Timeout and timing utilities
* **RNG.h** - Random number generator (Mersenne Twister)
* **ProgressBar.h** - Progress bar display utilities
* **UnionFind.h** - Union-Find data structure (unsigned-based)
* **Offset.h** - Offset manipulation utilities
* **Optional.h** - Optional value wrapper
* **ScopeExit.h** - RAII-style scope exit handlers
* **range.h** - Range utilities

**Note**: There are two Union-Find implementations:
- ``UnionFind.h`` - Simple unsigned-based implementation with ``mk()``, ``find()``, ``merge()``
- ``ADT/DisjointSet.h`` - Template-based implementation with ``makeSet()``, ``findSet()``, ``doUnion()``

**Usage**:
.. code-block:: cpp

   #include <Utils/General/Timer.h>
   Timer timer(5.0, []() { /* timeout action */ });
   timer.check(); // Check for timeout
   
   #include <Utils/General/ProgressBar.h>
   ProgressBar bar("Processing", ProgressBar::PBS_CharacterStyle);
   bar.showProgress(0.5); // 50% complete

Configuration and I/O
~~~~~~~~~~~~~~~~~~~~~

* **APISpec.h** - Unified API specification loader (parses ptr.spec, modref.spec, taint.spec)
* **cJSON.h** - Lightweight JSON parser/generator (C library)
* **CLI11.h** - Command-line argument parsing

**Usage**:
.. code-block:: cpp

   #include <Utils/General/APISpec.h>
   lotus::APISpec spec;
   spec.loadFile("config/ptr.spec", errorMsg);
   bool ignored = spec.isIgnored("malloc");

Multi-threading
~~~~~~~~~~~~~~~

* **MultiThreading.h** - Multi-threading utilities and helpers

Miscellaneous
~~~~~~~~~~~~~

* **PdQsort.h** - Parallel quicksort implementation
* **egraphs.h** - E-graph data structure
* **TreeStream.h** - Tree-structured output streams

LLVM Utilities
--------------

LLVM-specific utilities located in ``lib/Utils/LLVM/`` and ``include/Utils/LLVM/``.

IR Manipulation and Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* **InstructionUtils.h** - LLVM instruction utilities (line numbers, names, JSON escaping)
* **API.h** - LLVM API helpers
* **Demangle.h** - Name demangling utilities
* **MetadataManager/** - LLVM metadata management
  * **MetadataManager.h** - Main metadata manager
  * **MetadataEntry.h** - Metadata entry representation
  * **LoopStructure.h** - Loop structure metadata

**Usage**:
.. code-block:: cpp

   #include <Utils/LLVM/InstructionUtils.h>
   unsigned lineNo = InstructionUtils::getLineNumber(*inst);
   std::string name = InstructionUtils::getInstructionName(inst);

I/O Utilities
~~~~~~~~~~~~~

* **FileUtils.h** - File system operations (create directory, read/write files)
* **IO/FileUtils.h** - Additional file utilities
* **IO/ReadFile.h** - File reading utilities
* **IO/ReadIR.h** - LLVM IR reading utilities
* **IO/WriteIR.h** - LLVM IR writing utilities
* **StringUtils.h** - String formatting and manipulation

**Usage**:
.. code-block:: cpp

   #include <Utils/LLVM/IO/FileUtils.h>
   FileUtil::createDirectory("output");
   FileUtil::writeToFile("output/result.txt", content);

Debugging and Profiling
~~~~~~~~~~~~~~~~~~~~~~~

* **Debug.h** - Debug output utilities
* **Log.h** - Logging infrastructure
* **Statistics.h** - Analysis statistics collection
* **RecursiveTimer.h** - Recursive timing utilities

**Usage**:
.. code-block:: cpp

   #include <Utils/LLVM/Log.h>
   LOG_INFO("Analysis completed");
   
   #include <Utils/LLVM/Statistics.h>
   Statistics::counter("functions_analyzed")++;

Work Lists
~~~~~~~~~~

Work list implementations for analysis algorithms:

* **FIFOWorkList.h** - First-in-first-out work list
* **PriorityWorkList.h** - Priority queue work list
* **TwoLevelWorkList.h** - Two-level work list

Graph Utilities
~~~~~~~~~~~~~~~

* **GenericGraph.h** - Generic graph data structure
* **GraphWriter.h** - Graph visualization utilities
* **LLVMBgl.h** - LLVM Boost Graph Library integration

Scheduler
~~~~~~~~~

Parallel execution scheduling:

* **Scheduler/ParallelSchedulerPass.h** - Parallel scheduler pass
* **Scheduler/PipelineScheduler.h** - Pipeline scheduler
* **Scheduler/Task.h** - Task representation

Multi-threading
~~~~~~~~~~~~~~~

* **ThreadPool.h** - Thread pool implementation for parallel execution

**Usage**:
.. code-block:: cpp

   #include <Utils/LLVM/ThreadPool.h>
   ThreadPool* pool = ThreadPool::get();
   auto future = pool->enqueue([]() { return compute(); });
   auto result = future.get();

System Headers
~~~~~~~~~~~~~~

* **SystemHeaders.h** - Consolidated LLVM and system header includes

Common Usage Patterns
---------------------

This section provides common usage patterns for frequently used utilities.

**Timer with Timeout**:
.. code-block:: cpp

   #include <Utils/General/Timer.h>
   Timer timer(5.0, []() { std::cout << "Timeout!"; });
   while (condition) {
     timer.check(); // Check timeout
     // ... work ...
   }

**Progress Reporting**:
.. code-block:: cpp

   #include <Utils/General/ProgressBar.h>
   ProgressBar bar("Analyzing", ProgressBar::PBS_CharacterStyle, 0.01);
   for (size_t i = 0; i < total; ++i) {
     bar.showProgress(float(i) / total);
     // ... work ...
   }

**Thread Pool**:
.. code-block:: cpp

   #include <Utils/LLVM/ThreadPool.h>
   ThreadPool* pool = ThreadPool::get();
   std::vector<std::future<Result>> futures;
   for (auto& item : items) {
     futures.push_back(pool->enqueue([&item]() { return process(item); }));
   }
   for (auto& f : futures) {
     results.push_back(f.get());
   }

**String Formatting**:
.. code-block:: cpp

   #include <Utils/LLVM/StringUtils.h>
   std::string msg = format_str("Function %s has %d instructions", 
                                funcName.c_str(), count);

**Logging**:
.. code-block:: cpp

   #include <Utils/LLVM/Log.h>
   LOG_INFO("Starting analysis");
   LOG_DEBUG("Processing function: " << funcName);
   LOG_ERROR("Error occurred: " << errorMsg);
