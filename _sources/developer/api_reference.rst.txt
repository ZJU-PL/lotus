API Reference
=============

This document provides API references and usage examples for Lotus components.

Alias Analysis APIs
-------------------

DyckAA API
~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
   #include "Alias/UnificationBased/DyckAA/DyckGraph.h"

Basic usage:

.. code-block:: cpp

   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IRReader/IRReader.h"
   #include "llvm/Support/SourceMgr.h"
   #include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
   
   using namespace llvm;
   
   int main(int argc, char **argv) {
       LLVMContext context;
       SMDiagnostic error;
       
       std::unique_ptr<Module> module = parseIRFile(argv[1], error, context);
       if (!module) {
           error.print(argv[0], errs());
           return 1;
       }
       
       DyckAliasAnalysis DAA;
       DAA.runOnModule(*module);
       
       return 0;
   }

Query alias information:

.. code-block:: cpp

   // Query if two values may alias
   bool mayAlias = DAA.mayAlias(val1, val2);

   // Check if a pointer may be null
   bool mayNull = DAA.mayNull(val1);

   // Retrieve the alias set for a pointer
   const std::set<Value *> *aliasSet = DAA.getAliasSet(val1);
   if (aliasSet) {
       for (Value *aliased : *aliasSet) {
           errs() << "Aliased: " << *aliased << "\n";
       }
   }

Access Dyck graph:

.. code-block:: cpp

   #include "Alias/UnificationBased/DyckAA/DyckGraph.h"
   
   DyckGraph *graph = DAA.getDyckGraph();
   
   // Retrieve vertex for a value
   DyckGraphNode *node = graph->retrieveDyckVertex(val1);
   if (node) {
       std::set<void *> *equivSet = node->getEquivalentSet();
       if (equivSet) {
           errs() << "Equivalence set size: " << equivSet->size() << "\n";
       }
   }

AserPTA API
~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/InclusionBased/AserPTA/PTADriver.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KCallSite.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/WavePropagation.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"

Configure and run analysis:

.. code-block:: cpp

   using namespace aser;

   // Configure language model, memory model, and points-to representation
   using Model = DefaultLangModel<KCallSite<1>, FSMemModel<KCallSite<1>>, BitVectorPTS>;
   using Solver = WavePropagation<Model>;

   // Run analysis with preprocessing passes
   runAnalysis<Solver>(*module);

LotusAA API
~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"

Use as LLVM pass:

.. code-block:: cpp

   legacy::PassManager PM;
   PM.add(new LotusAA());
   PM.run(*module);

Query results:

.. code-block:: cpp

   LotusAA *lotus = /* get pass */;
   IntraLotusAA *intraPTG = lotus->getPtGraph(F);
   CallTargetSet *callees = lotus->getCallees(F, callSite);

Intermediate Representation APIs
--------------------------------

Program Dependence Graph API
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "IR/PDG/Core/ProgramDependencyGraph.h"
   #include "IR/PDG/Core/Graph.h"
   #include "IR/PDG/Core/PDGNode.h"
   #include "IR/PDG/Core/PDGEdge.h"

Build PDG:

.. code-block:: cpp

   using namespace pdg;
   
   legacy::PassManager PM;
   PM.add(new DataDependencyGraph());
   PM.add(new ControlDependencyGraph());
   PM.add(new ProgramDependencyGraph());
   PM.run(*module);
   
   ProgramGraph *pdg = &ProgramGraph::getInstance();

Traverse PDG:

.. code-block:: cpp

   for (auto *node : *pdg) {
       for (auto *edge : node->getOutEdgeSet()) {
           Node *target = edge->getDstNode();
           EdgeType type = edge->getEdgeType();
       }
   }

Query function nodes:

.. code-block:: cpp

   Function *F = /* ... */;
   FunctionWrapper *fw = pdg->getFuncWrapper(*F);
   if (fw) {
       Node *entryNode = fw->getEntryNode();
       Tree *formalInTree = fw->getFormalInTree();
       Tree *formalOutTree = fw->getFormalOutTree();
   }

Check reachability:

.. code-block:: cpp

   bool hasPath = pdg->canReach(*srcNode, *dstNode);
   
   std::set<EdgeType> exclude = {EdgeType::CONTROLDEP_OTHER};
   bool hasDataPath = pdg->canReach(*srcNode, *dstNode, exclude);

PDG Cypher Query API
~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "IR/PDG/QueryLanguage/Cypher.h"
   #include "IR/PDG/Core/ProgramDependencyGraph.h"

Parse and execute Cypher queries:

.. code-block:: cpp

   using namespace pdg;
   
   ProgramGraph *pdg = &ProgramGraph::getInstance();
   CypherQueryExecutor executor(*pdg);
   
   CypherParser parser;
   std::string queryStr = "MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n";
   std::unique_ptr<CypherQuery> query = parser.parse(queryStr);
   
   if (!parser.hasError()) {
       std::unique_ptr<CypherResult> result = executor.execute(*query);
       if (result->getType() == CypherResult::ResultType::NODES) {
           for (auto *node : result->getNodes()) {
               errs() << "Result node: " << node->getNodeType() << "\n";
           }
       }
   }

Call Graph API
~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "IR/PDG/Core/PDGCallGraph.h"

Build and query call graph:

.. code-block:: cpp

   using namespace pdg;
   
   PDGCallGraph &cg = PDGCallGraph::getInstance();
   cg.build(*module);
   
   // Check reachability between call-graph nodes
   bool reachable = cg.canReach(*srcNode, *sinkNode);
   
   // Compute paths between functions
   auto paths = cg.computePaths(*srcNode, *sinkNode);
   
   // Identify indirect call targets
   auto candidates = cg.getIndirectCallCandidates(*callInst, *module);

Data Flow Analysis APIs
-----------------------

IFDS Taint Analysis API
~~~~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
   #include "Dataflow/IFDS/Solver/IFDSSolver.h"

Configure and run:

.. code-block:: cpp

   using namespace ifds;

   TaintAnalysis analysis;
   analysis.add_source_function("scanf");
   analysis.add_sink_function("system");

   IFDSSolver<TaintAnalysis> solver(analysis);
   solver.solve(*module);

   auto results = solver.get_all_results();

Bug Reporting APIs
------------------

BugReport and BugReportMgr API
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Checker/Framework/BugReport.h"
   #include "Checker/Framework/BugReportMgr.h"

Report bugs:

.. code-block:: cpp

   BugReportMgr &mgr = BugReportMgr::get_instance();
   int typeId = mgr.register_bug_type(
       "NullDeref", BugDescription::BI_HIGH, BugDescription::BC_SECURITY,
       "Null pointer dereference");

   auto *report = new BugReport(typeId);
   report->append_step(inst, "Dereference of null pointer");
   mgr.insert_report(typeId, report, /*deduplicate_by_trace=*/true);

Example: Complete Analysis Tool
--------------------------------

Here is an example building an analysis tool that integrates alias analysis, the PDG, and IFDS taint analysis:

.. code-block:: cpp

   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IRReader/IRReader.h"
   #include "llvm/Support/SourceMgr.h"
   #include "llvm/IR/LegacyPassManager.h"
   #include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
   #include "IR/PDG/Core/ProgramDependencyGraph.h"
   #include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
   #include "Dataflow/IFDS/Solver/IFDSSolver.h"
   
   using namespace llvm;
   
   int main(int argc, char **argv) {
       if (argc < 2) {
           errs() << "Usage: " << argv[0] << " <input.bc>\n";
           return 1;
       }
       
       LLVMContext context;
       SMDiagnostic error;
       std::unique_ptr<Module> module = parseIRFile(argv[1], error, context);
       
       if (!module) {
           error.print(argv[0], errs());
           return 1;
       }
       
       // 1. Alias analysis
       DyckAliasAnalysis DAA;
       DAA.runOnModule(*module);
       
       // 2. Build PDG
       legacy::PassManager PM;
       PM.add(new pdg::DataDependencyGraph());
       PM.add(new pdg::ControlDependencyGraph());
       PM.add(new pdg::ProgramDependencyGraph());
       PM.run(*module);
       
       // 3. IFDS taint analysis
       ifds::TaintAnalysis taint;
       taint.add_source_function("scanf");
       taint.add_sink_function("system");
       
       ifds::IFDSSolver<ifds::TaintAnalysis> solver(taint);
       solver.solve(*module);
       
       auto results = solver.get_all_results();
       errs() << "Analysis completed with " << results.size() << " result entries\n";
       
       return 0;
   }

Compile and link:

.. code-block:: bash

   clang++ -o my_analyzer my_analyzer.cpp \
       $(llvm-config --cxxflags --ldflags --libs) \
       -L/path/to/lotus/build/lib \
       -lCanaryDyckAA -lCanaryPDG -lIFDS

See Also
--------

- :doc:`../user_guide/architecture` - Understanding the framework architecture
- :doc:`../user_guide/tutorials` - Practical usage examples
- :doc:`developer_guide` - Extending Lotus
