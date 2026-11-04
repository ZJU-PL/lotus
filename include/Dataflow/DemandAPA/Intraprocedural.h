#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"
#include "Dataflow/DemandAPA/CentroidPreprocessing.h"
#include "Dataflow/DemandAPA/SameBagPreprocessing.h"

template <class T> void Algorithm<T>::preprocessIntraprocedural() {

  //	prepareNaiveAnswers();

  /*
   *  Same-bag preprocessing
   */

  sameBagPreprocessing();
  //	checkSameBagCorrectness();

  /*
   * Centroid preprocessing
   */

  centroidPreprocessing();

  stopClock(Time, "Intra-procedural preprocessing done");
  //	checkIntraCorectness();
}

template <class T> void Algorithm<T>::prepareNaiveAnswers() {
  Time = omp_get_wtime();
  /*
   * Before running intra-procedure algorithms, pre-compute answers naively for
   * some functions to test the correctness of our algorithms
   */

  // pick n_rand random functions to test
  // IMP
  int n_rand = min(n_H, 10);

  int i = 0;
  while (sz(naivePrecomp) < n_rand) {
    // IMP
    int p = rand() % n_H;
    // can add a condition to test only small procedures
    //		int Dp = edgeListGPerProc[p].begin()->sc.size();
    if (sz(nodesPerProc[p]) > 500)
      continue;
    naivePrecomp[p] = getAllPairsAns(p);
    db(sz(naivePrecomp));
  }

  stopClock(Time, "Precomputing all-pairs intraprecedural queries naively");
}

// For procedure p, runs Floyd's algorithm on G[nodes in p] to find all-pairs
// (interpretation of) path-expressions returns list of pair of ((u, v), val)
// for all u, v in procedure p and val is the corresponding interpretation (uses
// the graph at the moment of function call, so this must be called before any
// of our intra algorithm are run)
template <class T> unordered_map<ll, T> Algorithm<T>::getAllPairsAns(int p) {

  int procSz = sz(nodesPerProc[p]);

  ll iterations = procSz * 1LL * procSz * 1LL * procSz;
  //	db(p, procSz, iterations);

  unordered_map<ll, T> ret;

  for (int i = 0; i < procSz; ++i)
    for (int j = 0; j < procSz; ++j) {
      int u = nodesPerProc[p][i], v = nodesPerProc[p][j];
      T val;
      ll uv = ((ll(u)) << 32) | v;

      if (present(edgeListGPerProc[p], uv))
        val = edgeListGPerProc[p][uv];
      else if (u == v)
        val = oneA;
      else
        val = zeroA;

      ret[uv] = val;
    }

  //	db(ret);
  //	db(nodesPerProc[p]);
  //	db(edgeListGPerProc[p]);

  for (int k = 0; k < procSz; ++k) {
    int w = nodesPerProc[p][k];
    for (int i = 0; i < procSz; ++i) {
      int u = nodesPerProc[p][i];
      ll uw = ((ll(u)) << 32) | w;
      for (int j = 0; j < procSz; ++j) {
        int v = nodesPerProc[p][j];
        ll uv = ((ll(u)) << 32) | v;
        ll wv = ((ll(w)) << 32) | v;
        ll ww = ((ll(w)) << 32) | w;
        //				db(uv);
        //				db(ret[ww]);
        //				for (auto& elem : ret[ww])
        //				    cout << mp(elem >> 16, elem &
        // FIRST_16BIT) << ' ';
        //
        //				cout << endl;
        T C = starA(ret[ww]);
        //				db(C, ret[wv]);
        T B = dotA(C, ret[wv]);
        //				db(B, ret[uw]);
        T A = dotA(ret[uw], B);
        //				db(ret[uv], A);
        ret[uv] = plusA(ret[uv], A);
      }
    }
  }

  function<void(void)> print = [&]() {
    for (auto &u : nodesPerProc[p]) {
      for (auto &v : nodesPerProc[p]) {
        ll uv = ((ll(u)) << 32) | v;
        cout << ret[uv] << " ";
      }
      ndl;
    }
    cout << "----------------------------------------------\n";
  };

  //	print();

  return ret;
}

