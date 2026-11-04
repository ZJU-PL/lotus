#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

template <class T> void Algorithm<T>::sameBagPreprocessing() {
  Time = omp_get_wtime();

  // parallel version
  {
    vvi parts = getNiceSlitting(weightSameBag, threadNum);
    omp_set_num_threads(sz(parts));
#pragma omp parallel
    {
      int nthreads = omp_get_num_threads();
      assert(threadNum == nthreads);
      int processor_id = omp_get_thread_num();
      cout << 0 << flush;
      double t = omp_get_wtime();
      for (int p : parts[processor_id]) {
        //				db(p);
        solveSameBag(p, 0);
      }

      cout << processor_id << ": " << (omp_get_wtime() - t) << " s" << endl;
    }
    cout << endl;
  }

  //	stopClock(Time, "Same-bag preprocessing");
}

template <class T> void Algorithm<T>::solveSameBag(int p, int idx) {

  updateBag(p, idx);

  if (idx + 1 < sz(dfsOrder[p])) {
    solveSameBag(p, idx + 1);
    updateBag(p, idx);
  }
}

template <class T> void Algorithm<T>::updateBag(int p, int idx) {
  int bagLabel = dfsOrder[p][idx];
  vi bag = TWD[p][bagLabel];
  int bagSz = sz(bag);

  int e = 0;

  for (int i = 0; i < bagSz; ++i)
    for (int j = 0; j < bagSz; ++j) {
      int u = bag[i], v = bag[j];
      ll uv = ((ll(u)) << 32) | v;
      int p = procOf[u];
      assert(procOf[u] == procOf[v]);
      if (!present(edgeListGPerProc[p], uv)) {
        if (u == v)
          edgeListGPerProc[p][uv] = oneA;
        else
          edgeListGPerProc[p][uv] = zeroA;
      }
    }

  for (int k = 0; k < bagSz; ++k) {
    int w = bag[k];
    for (int i = 0; i < bagSz; ++i) {
      int u = bag[i];
      ll uw = ((ll(u)) << 32) | w;
      for (int j = 0; j < bagSz; ++j) {
        int v = bag[j];
        ll uv = ((ll(u)) << 32) | v;
        ll wv = ((ll(w)) << 32) | v;
        ll ww = ((ll(w)) << 32) | w;
        edgeListGPerProc[p][uv] = plusA(
            edgeListGPerProc[p][uv],
            dotA(edgeListGPerProc[p][uw], dotA(starA(edgeListGPerProc[p][ww]),
                                               edgeListGPerProc[p][wv])));
      }
    }
  }
}

template <class T> void Algorithm<T>::checkSameBagCorrectness() {
  for (auto &pr : naivePrecomp) {
    int p = pr.fs;
    auto answers = pr.sc;
    for (auto &prpr : edgeListGPerProc[p]) {
      ll uv = prpr.fs;
      T myAns = prpr.sc;
      T correctAns = answers[uv];
      int u = uv >> 32, v = uv & FIRST_32BIT;
      assert(myAns == correctAns);
    }
  }
  int nonSameBagPairs = 0;
  for (auto &pr : naivePrecomp) {
    int p = pr.fs;
    auto answers = pr.sc;
    for (auto &prpr : answers) {
      ll uv = prpr.fs;
      T correctAns = prpr.sc;
      if (!present(edgeListGPerProc[p], uv)) {
        ++nonSameBagPairs;
        //					db(correctAns);
      }
    }
  }
  cout << "Same-bag algorithm seems correct!!\n";
}

