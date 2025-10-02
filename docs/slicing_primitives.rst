PDG Slicing Primitives
======================

Slicing primitives for Program Dependence Graph (PDG) analysis.

Overview
--------

Three main functionalities:

1. **Forward Slicing**: Find nodes reachable from slicing criteria
2. **Backward Slicing**: Find nodes that reach slicing criteria  
3. **Program Chopping**: Find nodes on paths between source and sink

API Reference
-------------

ForwardSlicing
~~~~~~~~~~~~~

.. code-block:: cpp

   ForwardSlicing(GenericGraph &pdg)
   NodeSet computeSlice(Node &start_node, const std::set<EdgeType> &edge_types = {})
   NodeSet computeSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types = {})
   NodeSet computeSliceWithDepth(Node &start_node, size_t max_depth, const std::set<EdgeType> &edge_types = {})

BackwardSlicing
~~~~~~~~~~~~~~~

.. code-block:: cpp

   BackwardSlicing(GenericGraph &pdg)
   NodeSet computeSlice(Node &end_node, const std::set<EdgeType> &edge_types = {})
   NodeSet computeSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types = {})
   NodeSet computeSliceWithDepth(Node &end_node, size_t max_depth, const std::set<EdgeType> &edge_types = {})

ProgramChopping
~~~~~~~~~~~~~~~

.. code-block:: cpp

   ProgramChopping(GenericGraph &pdg)
   NodeSet computeChop(const NodeSet &source_nodes, const NodeSet &sink_nodes, const std::set<EdgeType> &edge_types = {})
   NodeSet computeChop(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {})
   bool hasPath(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {})
   std::vector<std::vector<Node *>> findAllPaths(Node &source_node, Node &sink_node, size_t max_paths = 0, const std::set<EdgeType> &edge_types = {})

SlicingUtils
~~~~~~~~~~~~

.. code-block:: cpp

   std::set<EdgeType> getDataDependencyEdges()
   std::set<EdgeType> getControlDependencyEdges()
   std::set<EdgeType> getParameterDependencyEdges()
   void printSlice(const NodeSet &slice, const std::string &slice_name)
   std::unordered_map<std::string, size_t> getSliceStatistics(const NodeSet &slice)

Usage Examples
--------------

.. code-block:: cpp

   #include "IR/PDG/Slicing.h"
   
   ProgramGraph &PDG = ProgramGraph::getInstance();
   Node *startNode = PDG.getNode(*someValue);
   
   // Forward slicing
   ForwardSlicing forwardSlicer(PDG);
   auto slice = forwardSlicer.computeSlice(*startNode);
   
   // Edge type filtering
   auto dataEdges = SlicingUtils::getDataDependencyEdges();
   auto dataSlice = forwardSlicer.computeSlice(*startNode, dataEdges);
   
   // Depth-limited slicing
   auto limitedSlice = forwardSlicer.computeSliceWithDepth(*startNode, 3);
   
   // Backward slicing
   BackwardSlicing backwardSlicer(PDG);
   auto backwardSlice = backwardSlicer.computeSlice(*endNode);
   
   // Program chopping
   ProgramChopping chopper(PDG);
   if (chopper.hasPath(*sourceNode, *sinkNode)) {
       auto chop = chopper.computeChop(*sourceNode, *sinkNode);
       auto paths = chopper.findAllPaths(*sourceNode, *sinkNode, 10);
   }

Edge Types
----------

**Data Dependencies**: ``DATA_DEF_USE``, ``DATA_RAW``, ``DATA_READ``, ``DATA_ALIAS``, ``DATA_RET``, ``PARAMETER_IN``, ``PARAMETER_OUT``, ``PARAMETER_FIELD``, ``VAL_DEP``

**Control Dependencies**: ``CONTROLDEP_CALLINV``, ``CONTROLDEP_CALLRET``, ``CONTROLDEP_ENTRY``, ``CONTROLDEP_BR``, ``CONTROLDEP_IND_BR``

**Other Dependencies**: ``IND_CALL``, ``GLOBAL_DEP``, ``CLS_MTH``, ``ANNO_VAR``, ``ANNO_GLOBAL``, ``ANNO_OTHER``

Testing
-------

.. code-block:: bash

   cd build && make slicing_test && ./tests/unit/slicing_test
