#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

// The imported project callbacks share these analysis instances. Define them
// once across translation units rather than requiring declarations in a driver.
inline Algorithm<int> algSP;
inline Algorithm<vi> algIfds;
inline Algorithm<bdd> algBp;
inline vvi indicesOflocalsNotInLHS;

// The project operators for every algebra are defined here because they need to
// access internal data

inline vector<int> Ifdsproject(int c, vector<int> b) {
  assert(algIfds.vertexTypeG[c] == CALL_VERTEX);
  int r = algIfds.callNodeInfo[c].fs;
  int pp = algIfds.callNodeInfo[c].sc;
  ll c_s = ((ll(c)) << 32) | algIfds.s[pp];
  ll e_r = ((ll(algIfds.e[pp])) << 32) | r;
  assert(present(algIfds.edgeListGInter, c_s));
  assert(present(algIfds.edgeListGInter, e_r));
  return Ifdsdot(algIfds.edgeListGInter[c_s],
                 Ifdsdot(b, algIfds.edgeListGInter[e_r]));
}

inline int SPproject(int c, int b) {
  assert(algSP.vertexTypeG[c] == CALL_VERTEX);
  ll c_s = ((ll(c)) << 32) | algSP.s[algSP.callNodeInfo[c].sc];
  ll e_r = ((ll(algSP.e[algSP.callNodeInfo[c].sc])) << 32) |
           algSP.callNodeInfo[c].fs;
  assert(present(algSP.edgeListGInter, c_s));
  assert(present(algSP.edgeListGInter, e_r));
  return algSP.edgeListGInter[c_s] + b + algSP.edgeListGInter[e_r];
}

inline bdd PAproject(int c, bdd Sigma) {
  //	cout << tabs << "Project: start" << endl;
  bdd ans;
  if (Sigma == PAzero)
    ans = PAzero;
  else {
    assert(algBp.vertexTypeG[c] == CALL_VERTEX);
    ll c_s = ((ll(c)) << 32) | algBp.s[algBp.callNodeInfo[c].sc];
    ll e_r = ((ll(algBp.e[algBp.callNodeInfo[c].sc])) << 32) |
             algBp.callNodeInfo[c].fs;
    assert(present(algBp.edgeListGInter, c_s));
    assert(present(algBp.edgeListGInter, e_r));
    bdd f_cs = algBp.edgeListGInter[c_s];
    bdd f_er = algBp.edgeListGInter[e_r];

    // Sigma(X, X') => Sigma(X'', X''')
    // f(c, s)(X, X') => f(c, s)(X, X'')
    // f(e, r)(X, X') => f(e, r)(X''', X')
    for (int i = 0; i < GL; ++i) {
      Sigma = bdd_compose(Sigma, bdd_ithvar(varDoublePrimed(i)), var(i));
      Sigma = bdd_compose(Sigma, bdd_ithvar(varTriplePrimed(i)), varPrimed(i));

      f_cs = bdd_compose(f_cs, bdd_ithvar(varDoublePrimed(i)), varPrimed(i));

      f_er = bdd_compose(f_er, bdd_ithvar(varTriplePrimed(i)), var(i));
    }

    ans = Sigma & f_cs & f_er;

    for (auto &idx : indicesOflocalsNotInLHS[c])
      ans &= bdd_biimp(bdd_ithvar(var(idx)), bdd_ithvar(varPrimed(idx)));

    for (int i = 0; i < GL; ++i) {
      ans = bdd_exist(ans, bdd_ithvar(varDoublePrimed(i)));
      ans = bdd_exist(ans, bdd_ithvar(varTriplePrimed(i)));
    }
  }

  //	cout << tabs << "Project: finish" << endl;
  return ans;
}
