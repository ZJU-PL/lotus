#pragma once

#include "Dataflow/DemandAPA/Algebra.h"

template <class T> class ApaInstance {
public:
  /*
   * Each instance corresponds to a program (CFGs + decompositions) and an
   * associated algebra
   *
   * An instance consists of:
   *      - Directed graph G of n_G nodes labeled with [0..n_G) and m_G edges
   * labeled with [0..m_G)
   *      - G describes the CFGs of all n_H procedures, which are labeled with
   * [0, n_H)
   *      - Denote the CFG of procedure i with G_i
   *      - G also has interprocedural edges corresponding to function calls and
   * returns
   *      - Further, every edge in G has an associate algebra element
   * corresponding to the interpretation of that edges
   *      - Each procedure has:
   *              - Unique start vertex marked with vertexTypeG[u] =
   * START_VERTEX
   *              - Unique exit vertex marked with vertexTypeG[u] = EXIT_VERTEX
   *      - For inter-procedural calls, each call is represented by:
   *              - Unique call vertex marked with vertexTypeG[u] = CALL_VERTEX
   *              - Unique return-site vertex marked with vertexTypeG[u] =
   * RETURN_SITE_VERTEX
   *              - The call vertex has exactly two outgoing edges:
   * call-to-return-site and call-to-start
   *              - The return-site vertex has exactly two incoming edges:
   * call-to-return-site and exit-to-return-site
   *
   *      - Directed graph H of n_H nodes labeled with [0..n_H) and m_H edges
   * labeled with [0..m_G) H corresponds to the call graph: there is an edge (i,
   * j) if somewhere in G_i we have a call to G_j
   *
   *      - For G and H, we are given their edge list, and an edge's label is
   * its index in the list.
   *
   *      - Treewidth decomposition of n_H CFGs G_i:
   *              - TWD[i] for i in [0, n_H) is an array of vectors of length
   * n_{TW_i}
   *              - Bags of G_i are labeled with [0, n_{TW_i}), TWD[i][j] j'th
   * bag in the tw decomposition of G_i
   *              - TWD_root[i] for i in [0, n_H) is in [0, n_{TW_i}): the root
   * of tw decomposition of G_i
   *              - TWD_par[i][j] for i in [0, n_H) and j in [0, n_{TW_j}) is
   * parent of j'th bag of twd of G_i and is -1 only in one root node.
   *
   *
   *      - Treedepth decomposition of H: an array TDD_par[i] for i in [0, n_H)
   * where TDD_par[i] = -1 if i is a root (there is one unique root, the dec. is
   * connected), otherwise TDD_par[i] is the parent of i in the td dec.
   *
   */

  int n_G, n_H;
  vector<pair<pair<int, int>, T>> edgeListG;
  vector<pair<int, int>> edgeListH;
  vector<int> vertexTypeG, procOf;
  vector<vector<vector<int>>> TWD;
  vector<int> TWD_root;
  vector<vector<int>> TWD_par;
  vector<int> TDD_par;
  int TDD_root;
  Algebra<T> *algebra;
  // for every procedure, determine how much time it takes to run a part of the
  // algorithm on it, used for load-balancing the threads when parallelizing the
  // code
  vector<ll> weightSameBag, weightCentroid, weightCallsG;

  ApaInstance() {}

  ApaInstance(int _n_G, int _n_H, vector<pair<pair<int, int>, T>> _edgeListG,
              vector<pair<int, int>> &_edgeListH, vector<int> &_vertexTypeG,
              vector<int> &_procOf, vector<vector<vector<int>>> &_TWD,
              vector<int> &_TWD_root, vector<vector<int>> &_TWD_par,
              int _TDD_root, vector<int> &_TDD_par, Algebra<T> *_algebra,
              vector<ll> _weightSameBag, vector<ll> _weightCentroid,
              vector<ll> _weightCallsG)
      : n_G(_n_G), n_H(_n_H), edgeListG(_edgeListG), edgeListH(_edgeListH),
        vertexTypeG(_vertexTypeG), procOf(_procOf), TWD(_TWD),
        TWD_root(_TWD_root), TWD_par(_TWD_par), TDD_par(_TDD_par),
        TDD_root(_TDD_root), algebra(_algebra), weightSameBag(_weightSameBag),
        weightCentroid(_weightCentroid), weightCallsG(_weightCallsG) {}
};

