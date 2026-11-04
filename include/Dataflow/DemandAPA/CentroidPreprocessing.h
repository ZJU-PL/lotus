#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

template <class T> void Algorithm<T>::centroidPreprocessing() {
  Time = omp_get_wtime();

  Tr.assign(n_H, TreeDec());
  Centroid_LCA.assign(n_H, LCA());
  computed.assign(n_G, false);

  // parallel version
  {
    vvi parts = getNiceSlitting(weightCentroid, threadNum);
    omp_set_num_threads(sz(parts));
#pragma omp parallel
    {
      int nthreads = omp_get_num_threads();
      assert(threadNum == nthreads);
      int processor_id = omp_get_thread_num();
      cout << 0 << flush;
      double t = omp_get_wtime();
      for (int p : parts[processor_id])
        solveCentroid(p);
      /*db(processor_id, p, sz(nodesPerProc[p]), weightSameBag[p], H[p],
       * HRev[p], sz(callerReturnSitePairs[p])), */
      cout << processor_id << ": " << (omp_get_wtime() - t) << " s" << endl;
    }
    cout << endl;
  }

  //	stopClock(Time, "Centroid preprocessing");
}

template <class T> void Algorithm<T>::read_twd(int p) {
  int n_bag = sz(TWD[p]), treewidth = 0;

  for (int i = 0; i < n_bag; i++)
    treewidth = max(treewidth, sz(TWD[p][i]) + 1);

  Tr[p] = TreeDec(n_bag, treewidth, n_G);

  for (int i = 0; i < n_bag; i++) {
    for (int j = 0; j < sz(TWD[p][i]); j++)
      Tr[p].add_vertex_to_bag(i, TWD[p][i][j]);

    for (int j = 0; j < sz(TWD_adj[p][i]); j++)
      Tr[p].add_edge(i, TWD_adj[p][i][j]);
  }
}

template <class T> void Algorithm<T>::compute(int p, int u, int w, int v) {
  ll uv = ((ll(u)) << 32) | v;
  ll uw = ((ll(u)) << 32) | w;
  ll wv = ((ll(w)) << 32) | v;

  if (present(edgeListGPerProc[p], uw) and present(edgeListGPerProc[p], wv)) {
    if (edgeListGPerProc[p].find(uv) == edgeListGPerProc[p].end())
      edgeListGPerProc[p][uv] = zeroA;
    edgeListGPerProc[p][uv] =
        plusA(edgeListGPerProc[p][uv],
              dotA(edgeListGPerProc[p][uw], edgeListGPerProc[p][wv]));
  }
}

template <class T> void Algorithm<T>::solveCentroid(int p) {
  // here, only focusing on nodes lying in procedure p (i.e. a single CFG)
  // TWD_adj[p], TWD[p], TWD_par[p], edgeListGPerProc[p]
  // plusA(x, y), dotA(x, y), starA(x), inst->f(u, v)
  // x and y are of type T.
  read_twd(p);
  Tr[p].compute_centroid();
  Centroid_LCA[p] = LCA(Tr[p].get_parents());
  stack<int> garbage; // temporary stack
  // Tr[p].build_lca();
  // Tr[p].show_decomposition();
  // Tr[p].show_ctd();

  // Assume that Local preprocessing is done and saved to edgeListGPerProc

  for (int i = 0; i < Tr[p].bags(); i++) {
    for (auto v_bag : Tr[p].get_descendats(i)) {
      vector<int> v_elements = Tr[p].get_bag(v_bag);
      for (auto v : v_elements) {
        if (computed[v])
          continue;
        computed[v] = true;
        garbage.push(v);
        for (auto u : Tr[p].get_bag(i))
          for (auto w : v_elements) {
            compute(p, u, w, v);
            compute(p, v, w, u);
          }
      }
    }
    while (!garbage.empty()) {
      computed[garbage.top()] = false;
      garbage.pop();
    }
  }
}

template <class T> T Algorithm<T>::SCQ(int u, int v) {
  // u and v are CFG nodes, in the same procedure
  assert(procOf[u] == procOf[v]);
  int p = procOf[u];
  int u_b = Tr[p].get_belonging_bag(u);
  int v_b = Tr[p].get_belonging_bag(v);
  // int l_b = Tr[p].LCA(u_b, v_b);
  int l_b = Centroid_LCA[p].query(u_b, v_b);
  T answer = zeroA;
  auto trav = Tr[p].get_bag(l_b);
  for (auto w : trav) {
    //		ll uv = ((ll(u)) << 32) | v;
    ll uw = ((ll(u)) << 32) | w;
    ll wv = ((ll(w)) << 32) | v;
    if (present(edgeListGPerProc[p], uw) and present(edgeListGPerProc[p], wv))
      answer =
          plusA(answer, dotA(edgeListGPerProc[p][uw], edgeListGPerProc[p][wv]));
  }
  return answer;
}

template <class T> void Algorithm<T>::checkIntraCorectness() {

  int correct = 0, incorrect = 0, tot = 0;

  for (auto &pr : naivePrecomp) {
    int p = pr.fs;
    auto answers = pr.sc;

    for (auto &prpr : answers) {
      ll uv = prpr.fs;
      int u = uv >> 32, v = uv & FIRST_32BIT;
      T myAns = SCQ(u, v);
      T correctAns = prpr.sc;
      bool isSameBag = present(edgeListGPerProc[p], ((ll(u)) << 32) | v);

      assert(myAns == correctAns);
    }
  }
  //		db(correct, incorrect, tot);
  cout << "Centroid algorithm seems correct!!\n";
}

