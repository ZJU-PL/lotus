#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

/*
 * Computing summaries of functions
 * The call to preprocessFunctionSummaries should be that all edges (c, r) in
 * edgeListGPerProc hold an algebra element which corresponds to the summary
 * making a call at c then returning at r. We assume that for each call we have
 * a call-to-start, exit-to-return-site, and call-to-return-site edges.
 * Depending on the example we might set the value of some of them to 0 or 1,
 * but we must always have them
 */

#include "Dataflow/DemandAPA/Tarjan.h"

template <class T> void Algorithm<T>::preprocessFunctionSummaries() {
  Time = omp_get_wtime();

  vector<RegEx *> RHSs = getCFGsRE();

  //	db("HAHAHA");

  /*	for (int p = 0; p < n_H; ++p) {
                  cout << *RHSs[p] << endl;
          }*/

  for (int p = 0; p < n_H; ++p)
    buildIncludedVars(RHSs[p]);

  for (auto &pr : includedVars) {
    RegEx *re = pr.fs;
    for (auto &p : pr.sc)
      REsWithVar[p].insert(re);
  }

  for (int p = 0; p < n_H; ++p) {
    unordered_set<int> HSet;
    for (auto &pp : H[p])
      HSet.insert(pp);

    /*		db(p, *RHSs[p]);
                    db(includedVars[RHSs[p]]);
                    db(HSet);*/

    for (auto &elem : includedVars[RHSs[p]])
      assert(present(HSet, elem));
  }

  unordered_map<RegEx *, T> cache;

  function<T(RegEx *, bool)> I = [&](RegEx *re, bool doCaching) {
    if (doCaching && present(cache, re))
      return cache[re];
    T ans;
    if (re->eId == RegEx::ZERO)
      ans = zeroA;
    else if (re->eId == RegEx::ONE)
      ans = oneA;
    else if (re->eId == RegEx::PLUS)
      ans = plusA(I(re->L, doCaching), I(re->R, doCaching));
    else if (re->eId == RegEx::DOT)
      ans = dotA(I(re->L, doCaching), I(re->R, doCaching));
    else if (re->eId == RegEx::PROJECT)
      ans = projectA(re->L->eId, I(re->R, doCaching));
    else if (re->eId == RegEx::STAR)
      ans = starA(I(re->L, doCaching));
    else if (re->eId >= 0) {
      ll uv = re->eId;
      int u = uv >> 32, v = uv & FIRST_32BIT;
      int t_u = vertexTypeG[u], t_v = vertexTypeG[v];
      if ((t_u == CALL_VERTEX && t_v == START_VERTEX) ||
          (t_u == EXIT_VERTEX && t_v == RETURN_SITE_VERTEX)) {
        assert(present(edgeListGInter, uv));
        ans = edgeListGInter[uv];
      } else {
        assert(present(edgeListGPerProc[procOf[u]], uv));
        ans = edgeListGPerProc[procOf[u]][uv];
      }
    } else {
      assert(re->eId <= -7);
      int p = -(re->eId + 7);
      assert(p >= 0 && p < n_H);
      assert(present(includedVars[re], p));
      assert(present(REsWithVar[p], re));
      //			db(p, summary[p]);
      ans = summary[p];
    }

    if (doCaching)
      return cache[re] = ans;
    else
      return ans;
  };

  vii edgeListHPlain;
  for (int p = 0; p < n_H; ++p)
    for (auto &pp : H[p])
      edgeListHPlain.pb(mp(p, pp));

  SCC scc(n_H, edgeListHPlain);

  int biggestCC = 0;

  for (int i = 0; i < sz(scc.CC); ++i) {
    biggestCC = max(biggestCC, sz(scc.CC[i]));
  }

  db(biggestCC);

  // worklist
  set<pair<int, int>> W;
  vi updCnt(n_H, 0);

  //	db("AAA");

  { // process functions higher in the topological order first
    for (int p = 0; p < n_H; ++p)
      W.insert(mp(-scc.CCID[p], p));

    summary.assign(n_H, zeroA);

    int iterations = 0;

    while (!W.empty()) {
      int p = W.begin()->sc;
      //			db(p);
      W.erase(W.begin());
      T y = I(RHSs[p], 1);
      //			print(y);
      //			bdd b = bddtrue;

      //			check(summary[p], y);
      if (y != summary[p]) {
        ++iterations;
        summary[p] = y;
        ++updCnt[p];
        //				db(p, updCnt[p]);
        for (auto &re : REsWithVar[p])
          if (present(cache, re))
            cache.erase(re);
        for (auto &pp : HRev[p])
          W.insert(mp(-scc.CCID[pp], pp));
      }
    }
    //		db(iterations);
  }

  //	db(updCnt);

  for (int p = 0; p < n_H; ++p)
    assert(summary[p] == I(RHSs[p], 0));

  int gain = 0;
  for (auto &pr : summariesAtCalls) {
    ll uv = pr.fs;
    int u = uv >> 32, v = uv & FIRST_32BIT;
    assert(present(edgeListGPerProc[procOf[u]], uv));
    //		db(edgeListGPerProc[procOf[u]][uv]);
    gain += edgeListGPerProc[procOf[u]][uv] != I(pr.sc, 0);
    edgeListGPerProc[procOf[u]][uv] = I(pr.sc, 0);
  }

  //	db("AAAAA");
  for (int p = 0; p < n_H; ++p)
    functionPETarjan[p]->clearPointers();
  //	db("BBBBB");

  stopClock(Time, "Computing function summaries");
}

// returns a vector ret of size n_H, ret[p] is a path expression from s[p] to
// e[p]
template <class T> vector<RegEx *> Algorithm<T>::getCFGsRE() {
  functionPETarjan.assign(n_H, nullptr);
  vector<RegEx *> ret(n_H);
  // handles error Proc correctly
  for (int p = 0; p < n_H; ++p) {

    vii edges;
    for (auto &pr : edgeListGPerProc[p]) {
      int u = pr.fs >> 32, v = pr.fs & FIRST_32BIT;
      edges.pb(mp(u, v));
    }

    int n_ = sz(nodesPerProc[p]), m_ = sz(edges);

    auto PR = relabel(edges, nodesPerProc[p], 1);
    vii labeledEdges = PR.fs;
    vi unlabel = PR.sc;

    int labeledS = -1, labeledE = -1;
    for (int i = 1; i <= n_; ++i) {
      if (unlabel[i] == s[p])
        labeledS = i;
      if (unlabel[i] == e[p])
        labeledE = i;
    }

    /*		for (auto& u : nodesPerProc[p]) {
                        db(vertexTypeG[u]);
                    }
                    db(p, edges, s[p], e[p]);*/

    assert(labeledS != -1);
    assert(labeledE != -1);

    functionPETarjan[p] = new Tarjan(n_, m_, labeledEdges);
    ret[p] = functionPETarjan[p]->query(labeledS, labeledE);
    /*
                    if (p == 176) {
                            db(*ret[p]);
                            int x;
                            cin >> x;
                    }
    */

    assert(sz(nodesPerProc[p]) + 1 == sz(unlabel));

    unlabelRegEx(ret[p], unlabel, labeledEdges);

    visRE.clear();
  }

  return ret;
}

template <class T>
void Algorithm<T>::unlabelRegEx(RegEx *re, vi &unlabel, vii labeledEdges) {
  set<pii> labeledEdgesSet;
  for (auto &e : labeledEdges)
    labeledEdgesSet.insert(e);

  vector<RegEx *> leaves;
  visRE.clear();
  getLeaves(re, leaves);

  unordered_set<RegEx *> leafSet;

  for (auto &leafRe : leaves)
    leafSet.insert(leafRe);

  assert(sz(leafSet) == sz(leaves));

  for (auto &leafRe : leafSet) {
    ll uv_labeled = leafRe->eId;
    int u_labeled = uv_labeled >> 32, v_labeled = uv_labeled & FIRST_32BIT;

    assert(present(labeledEdgesSet, mp(u_labeled, v_labeled)));
    assert(min(u_labeled, v_labeled) >= 1 &&
           max(u_labeled, v_labeled) < sz(unlabel));

    int u = unlabel[u_labeled], v = unlabel[v_labeled];
    ll uv = ((ll(u)) << 32) | v;
    assert(present(edgeListGPerProc[procOf[u]], uv));

    if (vertexTypeG[u] == CALL_VERTEX && vertexTypeG[v] == RETURN_SITE_VERTEX) {
      int pr = calledFunction[u];
      summariesAtCalls[uv] = leafRe;

      assert(pr >= 0 && pr < n_H);

      int st = s[pr], ex = e[pr];

      ll c_s = ((ll(u)) << 32) | st;
      assert(present(edgeListGInter, c_s));

      ll e_r = ((ll(ex)) << 32) | v;
      assert(present(edgeListGInter, e_r));

      leafRe->eId = RegEx::PLUS;
      leafRe->L = new RegEx(uv);
      leafRe->R = new RegEx(RegEx::PROJECT, new RegEx(u), new RegEx(-7 - pr));
    } else
      leafRe->eId = uv;
  }
}

// finds non-constant leaves of a regular expression (before adding the Project
// operator)
template <class T>
void Algorithm<T>::getLeaves(RegEx *re, vector<RegEx *> &ret) {
  if (present(visRE, re))
    return;
  visRE.insert(re);
  if (re->eId == RegEx::ZERO || re->eId == RegEx::ONE)
    return;
  if (re->eId == RegEx::PLUS || re->eId == RegEx::DOT)
    getLeaves(re->L, ret), getLeaves(re->R, ret);
  else if (re->eId == RegEx::STAR)
    getLeaves(re->L, ret);
  else
    ret.pb(re);
}

template <class T> void Algorithm<T>::buildIncludedVars(RegEx *re) {
  if (present(visRE, re))
    return;
  visRE.insert(re);
  if (re->eId == RegEx::ZERO || re->eId == RegEx::ONE)
    return;
  if (re->eId == RegEx::PLUS || re->eId == RegEx::DOT ||
      re->eId == RegEx::PROJECT) {
    buildIncludedVars(re->L), buildIncludedVars(re->R);

    if (present(includedVars, re->L))
      for (auto &var : includedVars[re->L])
        includedVars[re].insert(var);

    if (present(includedVars, re->R))
      for (auto &var : includedVars[re->R])
        includedVars[re].insert(var);

  } else if (re->eId == RegEx::STAR) {
    buildIncludedVars(re->L);

    if (present(includedVars, re->L))
      for (auto &var : includedVars[re->L])
        includedVars[re].insert(var);

  } else if (re->eId <= -7)
    includedVars[re].insert(-(re->eId + 7));
}

