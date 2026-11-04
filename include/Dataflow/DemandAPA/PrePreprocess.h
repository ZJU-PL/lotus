#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

template <class T> void Algorithm<T>::prePreprocess(ApaInstance<T> *inst) {
  Time = omp_get_wtime();

  copyData(inst);
  doAuxiliaryStuff();
  validateTWD();
  validateTDD();

  stopClock(Time, "Pre-preprocessing");
}

template <class T> void Algorithm<T>::copyData(ApaInstance<T> *inst) {
  n_G = inst->n_G;
  n_H = inst->n_H;
  vertexTypeG = inst->vertexTypeG;
  procOf = inst->procOf;
  TWD = inst->TWD;
  TWD_root = inst->TWD_root;
  TWD_par = inst->TWD_par;
  TDD_root = inst->TDD_root;
  TDD_par = inst->TDD_par;
  zeroA = inst->algebra->zeroA;
  oneA = inst->algebra->oneA;
  plusA = inst->algebra->plusA;
  dotA = inst->algebra->dotA;
  starA = inst->algebra->starA;
  projectA = inst->algebra->projectA;
  weightSameBag = inst->weightSameBag;
  weightCentroid = inst->weightCentroid;
  weightCallsG = inst->weightCallsG;

  edgeListGPerProc.assign(n_H, unordered_map<ll, T>());

  {
    vi szs(n_H);
    for (int i = 0; i < n_G; ++i)
      ++szs[procOf[i]];

    for (int p = 0; p < n_H; ++p) {
      int szSq = szs[p] * szs[p];
      for (int b = 1;; ++b) {
        int mask = (1 << b) - 1;
        if ((mask | szSq) == mask) {
          szSq = mask + 1;
          break;
        }
      }
      edgeListGPerProc[p].reserve(szSq);
      edgeListGPerProc[p].max_load_factor(0.25);
    }
  }

  for (auto &pr : inst->edgeListG) {
    int u = pr.fs.fs, v = pr.fs.sc;
    ll uv = ((ll(u)) << 32) | v;
    T val = pr.sc;

    if (vertexTypeG[u] == ERROR_VERTEX || vertexTypeG[v] == ERROR_VERTEX ||
        (vertexTypeG[u] == CALL_VERTEX && vertexTypeG[v] == START_VERTEX) ||
        (vertexTypeG[u] == EXIT_VERTEX &&
         vertexTypeG[v] == RETURN_SITE_VERTEX)) {
      edgeListGInter.insert(mp(uv, val));
    } else {
      assert(procOf[u] == procOf[v]);
      int p = procOf[u];
      edgeListGPerProc[p].insert(mp(uv, val));
    }
  }

  for (auto &pr : inst->edgeListH) {
    int u = pr.fs, v = pr.sc;
    ll uv = ((ll(u)) << 32) | v;
    edgeListH[uv] = zeroA;
  }
}

// all the book-keeping data-structures that constructed directly from
// instance's data
template <class T> void Algorithm<T>::doAuxiliaryStuff() {
  // computing callNodeInfo and callerReturnSitePairs
  {
    callNodeInfo.assign(n_G, mp(-1, -1));
    callerReturnSitePairs.assign(n_H, V<pair<pair<int, int>, int>>());

    vvi G(n_G);

    for (int p = 0; p < n_H; ++p) {
      for (auto &pr : edgeListGPerProc[p]) {
        G[pr.fs >> 32].pb(pr.fs & FIRST_32BIT);
      }
    }

    for (auto &pr : edgeListGInter) {
      G[pr.fs >> 32].pb(pr.fs & FIRST_32BIT);
    }

    for (int c = 0; c < n_G; ++c) {
      if (vertexTypeG[c] == CALL_VERTEX) {
        int r = -1;
        assert(sz(G[c]) == 2);
        for (auto &v : G[c]) {
          if (vertexTypeG[v] == RETURN_SITE_VERTEX)
            assert(r == -1), r = v;
        }
        assert(r != -1);
        assert(procOf[c] == procOf[r]);

        int p = procOf[c];

        int pp = -1; // called procedure
        for (auto &v : G[c]) {
          if (vertexTypeG[v] == START_VERTEX)
            assert(pp == -1), pp = procOf[v];
        }
        assert(pp != -1);

        callNodeInfo[c] = mp(r, pp);

        callerReturnSitePairs[p].pb(mp(mp(c, r), pp));
      }
    }
  }
  // computing TWD_adj and TDD_adj
  {
    TWD_adj.assign(n_H, vvi());

    for (int i = 0; i < n_H; ++i) {

      assert(sz(TWD[i]) == sz(TWD_par[i]));
      int n_T_i = sz(TWD[i]); // number of bags in treew. dec. of G_i
      TWD_adj[i].assign(n_T_i, vi());

      for (int j = 0; j < n_T_i; ++j)
        if (TWD_par[i][j] != -1) {
          TWD_adj[i][j].pb(TWD_par[i][j]);
          TWD_adj[i][TWD_par[i][j]].pb(j);
        }
    }

    TDD_adj.assign(n_H, vi());

    for (int i = 0; i < n_H; ++i)
      if (TDD_par[i] != -1)
        TDD_adj[i].pb(TDD_par[i]), TDD_adj[TDD_par[i]].pb(i);
  }

  // computing TWDLCA and TDDLCA
  {
    TWDLCA.assign(n_H, LCA());

    for (int i = 0; i < n_H; ++i)
      TWDLCA[i] = LCA(TWD_par[i]);

    TDDLCA = LCA(TDD_par);
  }

  // computing H and HRev
  {
    H.assign(n_H, vi());
    HRev.assign(n_H, vi());
    for (pair<ll, T> edge : edgeListH) {
      int u = edge.fs >> 32, v = edge.fs & FIRST_32BIT;
      H[u].pb(v), HRev[v].pb(u);
    }
  }

  // computing s and e
  {
    s.assign(n_H, -1);
    e.assign(n_H, -1);
    for (int u = 0; u < n_G; ++u) {
      if (vertexTypeG[u] == START_VERTEX || vertexTypeG[u] == ERROR_VERTEX) {
        assert(s[procOf[u]] == -1);
        s[procOf[u]] = u;
      }
      if (vertexTypeG[u] == EXIT_VERTEX || vertexTypeG[u] == ERROR_VERTEX) {
        assert(e[procOf[u]] == -1);
        e[procOf[u]] = u;
      }
    }
    for (int p = 0; p < n_H; ++p) {
      assert(s[p] != -1 && e[p] != -1);
    }
  }

  // computing nodesPerProc
  {
    nodesPerProc.assign(n_H, vi());
    for (int u = 0; u < n_G; ++u) {
      nodesPerProc[procOf[u]].pb(u);
    }
  }

  // computing calledFunction
  {
    calledFunction.assign(n_G, -1);
    for (auto &elem : edgeListGInter) {
      ll uv = elem.fs;
      int u = uv >> 32, v = uv & FIRST_32BIT;
      if (vertexTypeG[u] == CALL_VERTEX)
        assert(vertexTypeG[v] == START_VERTEX), calledFunction[u] = procOf[v];
    }
  }
  // computing dfsOrder, TWD_depth, delta, rb, and bagsContainingNode by
  // traversing the twd decomposition
  {

    dfsOrder.assign(n_H, vi());
    TWD_depth.assign(n_H, vi());
    rb.assign(n_G, -1);
    bagsContainingNode.assign(n_G, vi());

    // easy part, needn't be parallelized
    for (int p = 0; p < n_H; ++p) {
      TWD_depth[p].assign(sz(TWD[p]), 0);
      dfsTWD(TWD_adj[p], TWD_root[p], -1, p, 0);
    }
  }

  // computing ancInTDD, TDD_depth
  {
    ancInTDD.assign(n_H, unordered_set<int>());
    TDD_depth.assign(n_H, 0);
    dfsTDD(TDD_root, vi(), -1, 0);
  }
}

template <class T>
void Algorithm<T>::dfsTWD(vvi &adj, int u, int parent, int p, int dep) {

  for (auto &v : TWD[p][u]) {
    if (rb[v] == -1)
      rb[v] = u;
    bagsContainingNode[v].pb(u);
  }

  TWD_depth[p][u] = dep;
  for (int v : adj[u])
    if (v != parent)
      dfsTWD(adj, v, u, p, dep + 1);
  assert(p < sz(dfsOrder));
  dfsOrder[p].pb(u);
}

template <class T>
void Algorithm<T>::dfsTDD(int u, vector<int> ancestors, int parent, int dep) {
  ancestors.pb(u);
  for (int ancestor : ancestors)
    ancInTDD[u].insert(ancestor);

  TDD_depth[u] = dep;
  for (int v : TDD_adj[u])
    if (v != parent)
      dfsTDD(v, ancestors, u, dep + 1);
}

template <class T> void Algorithm<T>::validateTWD() {
  for (int u = 0; u < n_G; ++u) {
    sort(bagsContainingNode[u].begin(), bagsContainingNode[u].end(),
         [&](int b1, int b2) {
           return TWD_depth[procOf[u]][b1] < TWD_depth[procOf[u]][b2];
         });
  }

  // check that bagsContainingNode[u] is connected and non-empty

  for (int u = 0; u < n_G; ++u) {
    si prevBags;
    assert(!bagsContainingNode[u].empty());
    assert(rb[u] != -1);
    assert(bagsContainingNode[u][0] == rb[u]);
    prevBags.insert(rb[u]);
    int p = procOf[u];

    for (auto &b : bagsContainingNode[u])
      if (b != rb[u]) {
        assert(present(prevBags, TWD_par[p][b]));
        prevBags.insert(b);
      }
  }

  // check that each edge appears in a bag

  V<si> canHave(n_G); // canHave[u] = set of all neighbours u can have given
                      // that TWD is valid
  for (int u = 0; u < n_G; ++u) {
    int p = procOf[u];

    for (auto &b : bagsContainingNode[u]) {
      for (auto &v : TWD[p][b]) {
        canHave[u].insert(v);
      }
    }
  }

  for (int p = 0; p < n_H; ++p) {
    for (auto &pr : edgeListGPerProc[p]) {
      int u = pr.fs >> 32, v = pr.fs & FIRST_32BIT;
      assert(present(canHave[u], v));
    }
  }

  int mxBagSz = 0;

  for (int p = 0; p < n_H; ++p) {
    for (auto &elem : TWD[p]) {
      mxBagSz = max(mxBagSz, sz(elem));
    }
  }
  row.tw = mxBagSz - 1;
  cout << "TWD is valid! with max bag size = " << mxBagSz << endl;
}

template <class T>
void Algorithm<T>::validateTDD() { // checking that the constructed td decomp.
                                   // of H is valid
  int treedepth = 0;

  for (int p = 0; p < n_H; ++p)
    treedepth = max(treedepth, TDD_depth[p]);

  for (auto &pr : edgeListH) {
    ll uv = pr.fs;
    int u = uv >> 32, v = uv & FIRST_32BIT;
    assert(present(ancInTDD[u], v) || present(ancInTDD[v], u));
  }
  row.td = treedepth - 1;
  cout << "TDD is valid! with tredepth = " << treedepth << endl;
}

