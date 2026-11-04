#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

template <class T> void Algorithm<T>::preprocessInterprocedural() {
  Time = omp_get_wtime();

  computeHEdgeValues();
  //	stopClock(Time, "Interprocedural preprocessing: finding H's values");
  Time = omp_get_wtime();
  buildCallsG();
  //	stopClock(Time, "Interprocedural preprocessing: building callsG");
  Time = omp_get_wtime();
  buildUpAndDown();
  //	stopClock(Time, "Interprocedural preprocessing: building up and down");

  stopClock(Time, "Inter-procedural preprocessing done");
  //	testCGQCorrectness();
}

template <class T> void Algorithm<T>::computeHEdgeValues() {
  for (int p = 0; p < n_H; ++p) {
    for (auto &pr : callerReturnSitePairs[p]) {
      int c = pr.fs.fs, r = pr.fs.sc, f_j = pr.sc;
      //			db(c, r);
      assert(procOf[c] == procOf[r]);
      int f_i = procOf[c];
      assert(p == f_i);
      //			db(f_i, f_j);

      int s_fi = s[f_i], s_fj = s[f_j];

      ll fi_fj = ((ll(f_i)) << 32) | f_j;
      assert(present(edgeListH, fi_fj));

      ll c_sj = ((ll(c)) << 32) | s_fj;
      assert(present(edgeListGInter, c_sj));

      T ans = dotA(SCQ(s_fi, c), edgeListGInter[c_sj]);

      edgeListH[fi_fj] = plusA(edgeListH[fi_fj], ans);
      //			if (f_i == f_j)
      //				edgeListH[fi_fj] =
      // plusA(edgeListH[fi_fj], oneA);
    }
  }
  //	db(edgeListH);
}

template <class T> void Algorithm<T>::buildCallsG() {

  callsG.assign(n_G, vector<pair<int, T>>());

  visCallsG.assign(n_G, -1);
  GRev.assign(n_G, vi());
  for (auto &pr : edgeListNaive) {
    int u = pr.fs >> 32, v = pr.fs & FIRST_32BIT;
    if (!(vertexTypeG[u] == CALL_VERTEX && vertexTypeG[v] == START_VERTEX) &&
        vertexTypeG[v] != ERROR_VERTEX)
      assert(procOf[u] == procOf[v]), GRev[v].pb(u);
  }

  /*	for (int p = 0; p < n_H; ++p)
                  buildCallsGPerProc(p);*/

  // parallel version
  {
    vvi parts = getNiceSlitting(weightCallsG, threadNum);
    omp_set_num_threads(threadNum);
#pragma omp parallel
    {
      int nthreads = omp_get_num_threads();
      assert(threadNum == nthreads);
      int processor_id = omp_get_thread_num();
      cout << 0 << flush;
      double t = omp_get_wtime();
      for (int p : parts[processor_id])
        buildCallsGPerProc(p);
      /*db(processor_id, p, sz(nodesPerProc[p]), weightSameBag[p], H[p],
       * HRev[p], sz(callerReturnSitePairs[p])), */

      cout << processor_id << ": " << (omp_get_wtime() - t) << " s" << endl;
    }
    cout << endl;
  }
  int cnt = 0;
  for (int u = 0; u < n_G; ++u) {
    if (!callsG[u].empty())
      cnt += sz(callsG[u]);
  }
  db(cnt);
}

template <class T> void Algorithm<T>::buildCallsGPerProc(int p) {
  for (auto &pr : callerReturnSitePairs[p]) {
    int c = pr.fs.fs, f_j = pr.sc;
    assert(vertexTypeG[c] == CALL_VERTEX);
    vi revReach;

    function<void(int)> dfs = [&](int u) {
      revReach.pb(u);
      visCallsG[u] = c;
      for (auto &v : GRev[u])
        if (visCallsG[v] != c)
          dfs(v);
    };

    int endVertex = -1;
    for (auto &u : nodesPerProc[procOf[c]]) {
      if (vertexTypeG[u] == EXIT_VERTEX)
        endVertex = u;
    }

    assert(endVertex != -1);

    dfs(c);

    for (auto &u : revReach) {
      T scq = SCQ(u, c);
      if (scq != zeroA) {
        int s_fj = s[f_j];
        ll c_sj = ((ll(c)) << 32) | s_fj;
        assert(present(edgeListGInter, c_sj));
        T val = dotA(scq, edgeListGInter[c_sj]);
        callsG[u].pb(mp(s_fj, val));
      }
    }
  }
}

template <class T> void Algorithm<T>::buildUpAndDown() {

  {
    vii unweightedEdgesH;
    for (auto &pr : edgeListH) {
      unweightedEdgesH.pb(mp(pr.fs >> 32, pr.fs & FIRST_32BIT));
    }
    SCC scc(n_H, unweightedEdgesH);
    vi sizes;
    for (auto &cc : scc.CC) {
      sizes.pb(sz(cc));
    }
    sort(sizes.rbegin(), sizes.rend());
  }

  up.assign(n_H, V<T>());
  down.assign(n_H, V<T>());

  for (int p = 0; p < n_H; ++p) {
    up[p].assign(sz(ancInTDD[p]), zeroA);
    down[p].assign(sz(ancInTDD[p]), zeroA);
    up[p][TDD_depth[p]] = down[p][TDD_depth[p]] = oneA;
  }

  for (int p = 0; p < n_H; ++p) {
    for (int rev = 0; rev < 2; ++rev) {
      // if rev = 0 (= 1) edges holds edges in H (HRev) that are reachable from
      // p in the TDD
      vii edges;
      unordered_set<int> vis;
      vi nodes;

      function<void(int)> dfs = [&](int u) {
        vis.insert(u);
        nodes.pb(u);
        vi N = (!rev ? H[u] : HRev[u]);
        for (auto &v : N)
          if (TDD_depth[v] >= TDD_depth[p]) {
            edges.pb(mp(u, v));
            if (!present(vis, v))
              dfs(v);
          }
      };

      dfs(p);

      if (edges.empty())
        continue;

      auto PR = relabel(edges, nodes);
      vii labeledEdges = PR.fs;
      vi unlabel = PR.sc;
      int n_ = sz(unlabel);
      int labeledRoot = -1;
      for (int i = 0; i < sz(unlabel); ++i)
        if (unlabel[i] == p)
          labeledRoot = i;
      assert(labeledRoot != -1);

      vector<pair<RegEx *, pii>> pathSequence;

      // find path sequence on labeledEdges
      {
        SCC scc(n_, labeledEdges);

        int n_scc = sz(scc.CC);

        V<bool> vis(n_, 0);
        for (int ccid = 0; ccid < n_scc; ++ccid) {
          vii E_in, E_out;
          vi V_in;
          function<void(int)> dfsSCC = [&](int u) {
            V_in.pb(u);
            vis[u] = true;
            for (auto &v : scc.adj[u]) {
              assert(scc.CCID[v] >= ccid);
              if (scc.CCID[v] == ccid) {
                E_in.pb(mp(u, v));
                if (!vis[v])
                  dfsSCC(v);
              } else
                E_out.pb(mp(u, v));
            }
          };
          dfsSCC(scc.CC[ccid][0]);

          // find path sequence over E_in, and append it to pathSequence
          if (!E_in.empty()) {
            auto _PR = relabel(E_in, V_in);
            vii labeledE_in = _PR.fs;
            vi unlabelE_in = _PR.sc;
            int sccSz = sz(unlabelE_in);
            assert(sccSz == sz(scc.CC[ccid]));

            // a path sequence over _labeledE_in
            vector<pair<RegEx *, pii>> SCCpathSequence;

            // this corresponds to ELIMINATE
            {
              VV<RegEx *> P(sccSz, V<RegEx *>(sccSz));
              for (int i = 0; i < sccSz; ++i)
                for (int j = 0; j < sccSz; ++j)
                  P[i][j] = new RegEx(RegEx::ZERO);

              for (auto &pr : labeledE_in) {
                int u = pr.fs, v = pr.sc;
                assert(max(u, v) <= sccSz);
                int u_orig = unlabel[unlabelE_in[u]],
                    v_orig = unlabel[unlabelE_in[v]];
                ll uv_orig = ((ll(u_orig)) << 32) | v_orig;
                P[u][v] = new RegEx(RegEx::PLUS, P[u][v], new RegEx(uv_orig));
              }
              for (int v = 0; v < sccSz; ++v) {
                P[v][v] = new RegEx(RegEx::STAR, P[v][v], nullptr);
                for (int u = v + 1; u < sccSz; ++u)
                  if (P[u][v]->eId != RegEx::ZERO) {
                    P[u][v] = new RegEx(RegEx::DOT, P[u][v], P[v][v]);
                    for (int w = v + 1; w < sccSz; ++w)
                      if (P[v][w]->eId != RegEx::ZERO)
                        P[u][w] =
                            new RegEx(RegEx::PLUS, P[u][w],
                                      new RegEx(RegEx::DOT, P[u][v], P[v][w]));
                  }
              }
              for (int u = 0; u < sccSz; ++u)
                for (int w = u; w < sccSz; ++w)
                  if (P[u][w]->eId != RegEx::ZERO && P[u][w]->eId != RegEx::ONE)
                    SCCpathSequence.pb(mp(P[u][w], mp(u, w)));
              for (int u = sccSz - 1; u >= 0; --u)
                for (int w = u - 1; w >= 0; --w)
                  if (P[u][w]->eId != RegEx::ZERO && P[u][w]->eId != RegEx::ONE)
                    SCCpathSequence.pb(mp(P[u][w], mp(u, w)));
            }

            for (int i = 0; i < sz(SCCpathSequence); ++i) {
              RegEx *P_i = SCCpathSequence[i].fs;
              int v_i = SCCpathSequence[i].sc.fs;
              int w_i = SCCpathSequence[i].sc.sc;
              v_i = unlabelE_in[v_i];
              w_i = unlabelE_in[w_i];
              pathSequence.pb(mp(P_i, mp(v_i, w_i)));
            }
          }

          for (auto &pr : E_out) {
            int u = pr.fs, v = pr.sc;
            int u_orig = unlabel[u], v_orig = unlabel[v];
            ll uv_org = ((ll(u_orig)) << 32) | v_orig;
            pathSequence.pb(mp(new RegEx(uv_org), mp(u, v)));
          }
        }
      }
      // for i in [0, n_) ans[i] is a PE over labeledEdges from labeledRoot to i
      vector<RegEx *> ans(n_);

      // this corresponds to SOLVE
      {
        for (int i = 0; i < n_; ++i) {
          if (i == labeledRoot)
            ans[i] = new RegEx(RegEx::ONE);
          else
            ans[i] = new RegEx(RegEx::ZERO);
        }

        for (int i = 0; i < sz(pathSequence); ++i) {
          RegEx *P_i = pathSequence[i].fs;
          int v_i = pathSequence[i].sc.fs;
          int w_i = pathSequence[i].sc.sc;
          //					db(i, *P_i, v_i, w_i);
          if (v_i == w_i)
            ans[v_i] = new RegEx(RegEx::DOT, ans[v_i], P_i);
          else
            ans[w_i] = new RegEx(RegEx::PLUS, ans[w_i],
                                 new RegEx(RegEx::DOT, ans[v_i], P_i));
        }
      }

      // Interpret appropriately, and use caching to save answers for RegEx's
      // that are interpreted before

      unordered_map<RegEx *, T> cache;

      function<T(RegEx *)> I = [&](RegEx *re) {
        if (present(cache, re))
          return cache[re];
        T ans;
        if (re->eId == RegEx::ZERO)
          ans = zeroA;
        else if (re->eId == RegEx::ONE)
          ans = oneA;
        else if (re->eId == RegEx::PLUS)
          ans = plusA(I(re->L), I(re->R));
        else if (re->eId == RegEx::DOT) {
          if (!rev)
            ans = dotA(I(re->L), I(re->R));
          else
            ans = dotA(I(re->R), I(re->L));
        } else if (re->eId == RegEx::STAR)
          ans = starA(I(re->L));
        else {
          ll uv = re->eId;
          int u = uv >> 32, v = uv & FIRST_32BIT;
          if (!rev) {
            assert(present(edgeListH, uv));
            ans = edgeListH[uv];
          } else {
            ll vu = ((ll(v)) << 32) | u;
            assert(present(edgeListH, vu));
            ans = edgeListH[vu];
          }
        }
        return cache[re] = ans;
      };

      for (int i = 0; i < n_; ++i) {
        int pp = unlabel[i];
        if (!rev)
          down[pp][TDD_depth[p]] = I(ans[i]);
        else
          up[pp][TDD_depth[p]] = I(ans[i]);
      }
    }
    //		db(up[p], down[p]);
  }
}

template <class T> void Algorithm<T>::testCGQCorrectness() {

  unordered_map<ll, T> correct;

  // only applies to SP
  /*
          for (int u = 0; u < n_H; ++u) {
                  vi dist(n_H, zeroA);
                  dist[u] = 0;
                  priority_queue<pii, vii, greater<pii>> pq;
                  pq.push(mp(0, u));

                  while (!pq.empty()) {
                          pii front = pq.top();
                          pq.pop();
                          int d = front.fs, u = front.sc;
                          if (d > dist[u])
                                  continue;
                          for (auto& v : H[u]) {
                                  ll uv = ((ll(u)) << 32) | v;
                                  assert(present(edgeListH, uv));
                                  if (d + edgeListH[uv] < dist[v]) {
                                          dist[v] = d + edgeListH[uv];
                                          pq.push(mp(dist[v], v));
                                  }
                          }
                  }
                  for (int v = 0; v < n_H; ++v) {
                          ll uv = ((ll(u)) << 32) | v;
                          correct[uv] = dist[v];
                  }
          }
  */

  // Applies to any algebra
  /*	for (int u = 0; u < n_H; ++u)
                  for (int v = 0; v < n_H; ++v) {
                          T val;
                          ll uv = ((ll(u)) << 32) | v;

                          if (present(edgeListH, uv))
                                  val = edgeListH[uv];
                          else if (u == v)
                                  val = oneA;
                          else
                                  val = zeroA;
                          correct[uv] = val;
                  }

          for (int w = 0; w < n_H; ++w) {
                  for (int u = 0; u < n_H; ++u) {
                          ll uw = ((ll(u)) << 32) | w;
                          for (int v = 0; v < n_H; ++v) {
                                  ll uv = ((ll(u)) << 32) | v;
                                  ll wv = ((ll(w)) << 32) | v;
                                  ll ww = ((ll(w)) << 32) | w;
                                  correct[uv] = plusA(correct[uv],
     dotA(correct[uw], dotA(starA(correct[ww]), correct[wv])));
                          }
                  }
          }*/

  /*
   * Applies to any algebra, but done using Tarjan
   */
  // CGedgesOneBased[i] = (u+1, v+1) where (u, v) is an edge in the call graph

  function<T(RegEx *)> I_CGTest = [&](RegEx *re) {
    T ans;
    if (re->eId == RegEx::ZERO)
      ans = zeroA;
    else if (re->eId == RegEx::ONE)
      ans = oneA;
    else if (re->eId == RegEx::PLUS)
      ans = plusA(I_CGTest(re->L), I_CGTest(re->R));
    else if (re->eId == RegEx::DOT)
      ans = dotA(I_CGTest(re->L), I_CGTest(re->R));
    else if (re->eId == RegEx::PROJECT)
      ans = projectA(re->L->eId, I_CGTest(re->R));
    else if (re->eId == RegEx::STAR)
      ans = starA(I_CGTest(re->L));
    else {
      assert(re->eId >= 0);
      ll uv = re->eId;
      int u = uv >> 32, v = uv & FIRST_32BIT;
      --u, --v;
      uv = ((ll(u)) << 32) | v;
      assert(min(u, v) >= 0 && max(u, v) < n_H);
      assert(present(edgeListH, uv));
      ans = edgeListH[uv];
    }
    return ans;
  };

  vii CGedgesOneBased;
  for (int u = 0; u < n_H; ++u)
    for (int v = 0; v < n_H; ++v) {
      T val;
      ll uv = ((ll(u)) << 32) | v;

      if (present(edgeListH, uv))
        CGedgesOneBased.pb(mp(u + 1, v + 1));
    }
  Tarjan *CGTar = new Tarjan(n_H, sz(CGedgesOneBased), CGedgesOneBased);
  for (int u = 0; u < n_H; ++u)
    for (int v = 0; v < n_H; ++v) {
      ll uv = ((ll(u)) << 32) | v;
      RegEx *pe = CGTar->query(u + 1, v + 1);
      correct[uv] = I_CGTest(pe);
    }

  //	int n_test = 1000;
  //	for (int i = 0; i < n_test; ++i) {
  //		int u = rand() % n_H, v = rand() % n_H;
  //		ll uv = ((ll(u)) << 32) | v;
  ////		db(u, v, correct[uv], CGQ(u, v));
  //		assert(correct[uv] == CGQ(u, v));
  //	}

  for (int u = 0; u < n_H; ++u) {
    for (int v = 0; v < n_H; ++v) {
      ll uv = ((ll(u)) << 32) | v;
      //			if (correct[uv] != CGQ(u, v))
      //				db(u, v, correct[uv], CGQ(u, v));
      assert(correct[uv] == CGQ(u, v));
      if (correct[uv] != zeroA) {
        db(u, v, correct[uv], CGQ(u, v));
      }
    }
  }

  cout << "CGQ seems correct!!" << endl;
  {
    int cnt = 0;
    for (int p = 0; p < n_H; ++p) {
      if (CGQ(p, p) == oneA)
        ++cnt;
    }
    db(cnt, n_H);
    //		while (true) {}
  }
}

template <class T> T Algorithm<T>::CGQ(int f_u, int f_v) {
  int f_w = TDDLCA.query(f_u, f_v);
  T ans = zeroA;
  for (int i = 0; i <= TDD_depth[f_w]; ++i)
    ans = plusA(ans, dotA(up[f_u][i], down[f_v][i]));
  return ans;
}

template <class T> T Algorithm<T>::EQ(int u, int v) {
  assert(u == s[procOf[u]] && v == s[procOf[v]]);
  return CGQ(procOf[u], procOf[v]);
}

template <class T> T Algorithm<T>::MQ(int u, int v) {
  assert(u == s[procOf[u]]);
  return dotA(EQ(s[procOf[u]], s[procOf[v]]), SCQ(s[procOf[v]], v));
}

// general query
template <class T> T Algorithm<T>::query(int u, int v) {
  int f_u = procOf[u], f_v = procOf[v];
  T sfv_v = SCQ(s[f_v], v);
  T ans = zeroA;
  //	db(ans);
  for (auto &pr : callsG[u]) {
    int s_fwi = pr.fs;
    //		db(s_fwi);
    ans = plusA(ans, dotA(pr.sc, EQ(s_fwi, s[f_v])));
  }
  if (!callsG[u].empty())
    ans = dotA(ans, sfv_v);
  //	db(ans, sfv_v);
  //	db(queryNaive(s[f_v], v));
  //	db(queryNaive(u, s[f_v]));
  if (f_u == f_v)
    ans = plusA(ans, SCQ(u, v));
  return ans;
}

