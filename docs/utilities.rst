Utilities
==========

Lotus provides utility libraries and frameworks for program analysis.

Sparta Abstract Interpretation Framework
-----------------------------------------

Header-only library for building abstract interpreters.

**Location**: ``include/Analysis/sparta``

**Features**: Generic framework, fixpoint iteration, WTO, flat sets

**Usage**:
.. code-block:: cpp

   #include <Analysis/sparta/AbstractDomain.h>
   class MyDomain : public AbstractDomain<MyDomain> {
   public:
       MyDomain join(const MyDomain& other) const;
       bool leq(const MyDomain& other) const;
   };
   MonotonicFixpointIterator<MyDomain> iterator(cfg, domain);
   iterator.run();

cJSON
-----

Lightweight JSON parser/generator for C.

**Location**: ``include/Support/cJSON.h``

**Features**: Simple API, small footprint, easy to use

**Usage**:
.. code-block:: cpp

   #include <Support/cJSON.h>
   cJSON* json = cJSON_Parse(jsonString);
   cJSON* item = cJSON_GetObjectItem(json, "key");
   const char* value = cJSON_GetStringValue(item);
   cJSON_Delete(json);

Transform Utilities
-------------------

LLVM bitcode manipulation utilities.

**Location**: ``lib/Transform``

**Transforms**: LowerConstantExpr, MergeReturn, RemoveDeadBlock, etc.

**Usage**:
.. code-block:: cpp

   #include <Transform/LowerConstantExpr.h>
   LowerConstantExpr transform;
   bool changed = transform.runOnModule(module);

Support Libraries
-----------------

**ADT**: ``include/Support/ADT``
* BitVector, Interval, Worklist, Graph

**IO**: ``include/Support/IO``
* FileUtils, StringUtils, ProgressBar

**Debug**: ``include/Support``
* Debug.h, Log.h, Timer.h, Statistics.h

**Usage Examples**:
.. code-block:: cpp

   // BitVector
   BitVector bv(100);
   bv.set(10);
   bool has10 = bv.test(10);
   
   // Logging
   LOG_INFO("Analysis completed");
   
   // Timer
   Timer timer;
   timer.start();
   // ... analysis ...
   timer.stop();

Multi-threading Support
-----------------------

**ThreadPool**: ``include/Support/ThreadPool.h`

**Usage**:
.. code-block:: cpp

   ThreadPool pool(4);
   auto future1 = pool.submit([]() { return analysis1(); });
   auto result1 = future1.get();

Configuration Management
------------------------

**API Spec**: ``include/Support/APISpec.h``
* Specification file parsing, API behavior modeling

**Annotations**: ``include/Annotation``
* Pointer, Mod/ref, Taint annotations
