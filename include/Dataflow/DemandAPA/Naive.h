#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

template <class T> void Algorithm<T>::prepareNaive() {
  for (int p = 0; p < n_H; ++p)
    for (auto &pr : edgeListGPerProc[p]) {
      edgeListNaive.insert(pr);
      edgeListNaiveOneBased.pb(
          mp((pr.fs >> 32) + 1, (pr.fs & FIRST_32BIT) + 1));
    }

  for (auto &pr : edgeListGInter) {
    ll uv = pr.fs;
    int u = uv >> 32, v = uv & FIRST_32BIT;
    if ((vertexTypeG[u] == CALL_VERTEX && vertexTypeG[v] == START_VERTEX) ||
        vertexTypeG[v] == ERROR_VERTEX) {
      edgeListNaiveOneBased.pb(
          mp((pr.fs >> 32) + 1, (pr.fs & FIRST_32BIT) + 1));
      edgeListNaive.insert(pr);
    }
  }

  for (auto &pr : edgeListNaiveOneBased)
    assert(min(pr.fs, pr.sc) >= 1 && max(pr.fs, pr.sc) <= n_G);

  naiveTar = new Tarjan(n_G, sz(edgeListNaiveOneBased), edgeListNaiveOneBased);
}

template <class T> T Algorithm<T>::queryNaive(int u, int v) {
  ++u, ++v;
  RegEx *re = naiveTar->query(u, v);
  //	db(*re);
  T ans = Inaive(re, 1);
  naiveTar->clearPointers();
  cacheNaive.clear();
  return ans;
}

template <class T> T Algorithm<T>::Inaive(RegEx *re, bool doCaching) {
  if (doCaching && present(cacheNaive, re))
    return cacheNaive[re];
  T ans;
  if (re->eId == RegEx::ZERO)
    ans = zeroA;
  else if (re->eId == RegEx::ONE)
    ans = oneA;
  else if (re->eId == RegEx::PLUS)
    ans = plusA(Inaive(re->L, doCaching), Inaive(re->R, doCaching));
  else if (re->eId == RegEx::DOT)
    ans = dotA(Inaive(re->L, doCaching), Inaive(re->R, doCaching));
  else if (re->eId == RegEx::STAR)
    ans = starA(Inaive(re->L, doCaching));
  else {
    assert(re->eId >= 0);
    ll uv_1b = re->eId;
    int u_1b = uv_1b >> 32, v_1b = uv_1b & FIRST_32BIT;
    int u = u_1b - 1, v = v_1b - 1;
    ll uv = ((ll(u)) << 32) | v;
    assert(present(edgeListNaive, uv));
    ans = edgeListNaive[uv];
  }

  if (doCaching)
    return cacheNaive[re] = ans;
  else
    return ans;
}

