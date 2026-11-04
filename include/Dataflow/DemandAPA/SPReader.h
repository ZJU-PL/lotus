#pragma once

#include "Dataflow/DemandAPA/ApaInstance.h"
#include "Dataflow/DemandAPA/Project.h"
#include "Dataflow/DemandAPA/Reader.h"
#include "Dataflow/DemandAPA/Support.h"

class SPReader : public reader {
public:
  SPReader(string pathToFile, string TWDpath, string TDDpath) {
    ifstream in(pathToFile);

    // taking general input: CFGs and decompositions

    in >> n_G >> m_G >> n_H;

    procOf.assign(n_G, 0);

    for (int i = 0; i < n_G; ++i)
      in >> procOf[i];

    vertexTypeG.assign(n_G, 0);

    for (int i = 0; i < n_G; ++i)
      in >> vertexTypeG[i];

    unweightedEdgeListG.assign(m_G, mp(-1, -1));

    for (int i = 0; i < m_G; ++i) {
      in >> unweightedEdgeListG[i].fs >> unweightedEdgeListG[i].sc;
      assert(unweightedEdgeListG[i].fs >= 0);
      assert(unweightedEdgeListG[i].fs < n_G);
      assert(unweightedEdgeListG[i].sc >= 0);
      assert(unweightedEdgeListG[i].sc < n_G);
    }

    in >> m_H;

    edgeListH.assign(m_H, mp(-1, -1));

    for (int i = 0; i < m_H; ++i) {
      in >> edgeListH[i].fs >> edgeListH[i].sc;
      assert(edgeListH[i].fs >= 0);
      assert(edgeListH[i].fs < n_H);
      assert(edgeListH[i].sc >= 0);
      assert(edgeListH[i].sc < n_H);
    }

    // tw decomposition
    readTWD(TWDpath);
    // td decomposition
    readTDD(TDDpath);
    assert(m_G == sz(unweightedEdgeListG));
    assert(m_H == sz(edgeListH));

    in.close();
  }

  ApaInstance<int> getInstance() {
    Algebra<int> *SPalgebra =
        new Algebra<int>(SPzero, SPone, SPplus, SPdot, SPstar, SPproject);

    vector<pair<pii, int>> edgeListG;

    for (auto &edge : unweightedEdgeListG) {
      int t_u = vertexTypeG[edge.fs], t_v = vertexTypeG[edge.sc];
      if (t_u == CALL_VERTEX && t_v == RETURN_SITE_VERTEX)
        assert(procOf[edge.fs] == procOf[edge.sc]),
            edgeListG.pb(mp(edge, SPzero));
      else
        edgeListG.pb(mp(edge, 1 /*+ (rand() % 1000)*/));
    }

    {
      vi maxBagSz(n_H, 0);
      weightSameBag.assign(n_H, 0);
      weightCentroid.assign(n_H, 0);
      weightCallsG.assign(n_H, 0);

      for (int p = 0; p < n_H; ++p) {
        for (auto &bag : TWD[p]) {
          maxBagSz[p] = max(maxBagSz[p], sz(bag));
          weightSameBag[p] += sz(bag) * 1LL * sz(bag) * sz(bag);
          weightCentroid[p] += sz(bag) * 1LL * sz(bag) * sz(bag);
        }
        weightCentroid[p] *= log2(sz(TWD[p]));
      }

      vvi GRev(n_G);
      vi vis(n_G, -1);

      for (auto &pr : edgeListG) {
        int u = pr.fs.fs, v = pr.fs.sc;
        if (!(vertexTypeG[u] == CALL_VERTEX &&
              vertexTypeG[v] == START_VERTEX) &&
            !(vertexTypeG[u] == EXIT_VERTEX &&
              vertexTypeG[v] == RETURN_SITE_VERTEX))
          assert(procOf[u] == procOf[v]), GRev[v].pb(u);
      }

      for (int c = 0; c < n_G; ++c) {
        if (vertexTypeG[c] == CALL_VERTEX) {
          vi revReach;

          function<void(int)> dfs = [&](int u) {
            revReach.pb(u);
            vis[u] = c;
            for (auto &v : GRev[u])
              if (vis[v] != c)
                dfs(v);
          };

          dfs(c);

          weightCallsG[procOf[c]] += 1LL * sz(revReach) * maxBagSz[procOf[c]];
        }
      }
    }

    //		db(weightCallsG);

    return ApaInstance<int>(n_G, n_H, edgeListG, edgeListH, vertexTypeG, procOf,
                            TWD, TWD_root, TWD_par, TDD_root, par_tdH,
                            SPalgebra, weightSameBag, weightCentroid,
                            weightCallsG);
  }
};

