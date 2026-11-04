#pragma once

#include "Dataflow/DemandAPA/ApaInstance.h"
#include "Dataflow/DemandAPA/Project.h"
#include "Dataflow/DemandAPA/Reader.h"
#include "Dataflow/DemandAPA/Support.h"

class IfdsReader : public reader {
public:
  vi D;
  vector<vector<vector<int>>> flow;

  IfdsReader(string pathToFile, string TWDpath, string TDDpath) {
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
      //			if (edgeListH[i].fs == edgeListH[i].sc)
      //				db(edgeListH[i]);
    }

    // tw decomposition
    readTWD(TWDpath);
    // td decomposition
    readTDD(TDDpath);
    assert(m_G == sz(unweightedEdgeListG));
    assert(m_H == sz(edgeListH));

    D.assign(n_H, 0);

    for (int i = 0; i < n_H; ++i)
      in >> D[i];

    int m_GExp = 0;
    flow.assign(m_G, vvi());

    for (int e = 0; e < m_G; ++e) {
      int u = unweightedEdgeListG[e].fs, v = unweightedEdgeListG[e].sc;
      assert(u < n_G && u < sz(procOf));
      assert(procOf[u] < n_H);
      flow[e].assign(D[procOf[u]] + 1, vi());

      for (int j = 0; j <= D[procOf[u]]; ++j) {
        int Sz;
        in >> Sz;

        for (int k = 0; k < Sz; ++k) {
          int jj;
          in >> jj;
          assert(jj >= 0 && jj <= D[procOf[v]]);
          flow[e][j].pb(jj);
          ++m_GExp;
        }
        sort(flow[e][j].begin(), flow[e][j].end());
      }
    }

    string s;
    in >> s;

    assert(s == "done");

    in.close();
  }

  ApaInstance<vi> getInstance() {

    Algebra<vi> *Ifdsalgebra = new Algebra<vi>(Ifdszero, Ifdsone, Ifdsplus,
                                               Ifdsdot, Ifdsstar, Ifdsproject);

    vector<pair<pii, vi>> edgeListG;

    for (int e = 0; e < m_G; ++e) {
      int u = unweightedEdgeListG[e].fs, v = unweightedEdgeListG[e].sc;
      vi flowVal;
      flowVal.pb((D[procOf[u]] << 16) | D[procOf[v]]);
      for (int d1 = 0; d1 <= D[procOf[u]]; ++d1)
        for (auto &d2 : flow[e][d1])
          flowVal.pb((d1 << 16) | d2);

      for (int i = 2; i < sz(flowVal); ++i)
        assert(flowVal[i] > flowVal[i - 1]);

      edgeListG.pb(mp(unweightedEdgeListG[e], flowVal));
    }

    ;
    {
      vi maxBagSz(n_H, 0);
      weightSameBag.assign(n_H, 0);
      weightCentroid.assign(n_H, 0);
      weightCallsG.assign(n_H, 0);

      double pwr1 = 1.2;
      double pwr2 = 1.2;
      double pwr3 = 1.2;
      //			db(pwr1, pwr2, pwr3);

      for (int p = 0; p < n_H; ++p) {
        for (auto &bag : TWD[p]) {
          maxBagSz[p] = max(maxBagSz[p], sz(bag));
          weightSameBag[p] +=
              sz(bag) * 1LL * sz(bag) * sz(bag) * pow(D[p], pwr1);
          weightCentroid[p] +=
              sz(bag) * 1LL * sz(bag) * sz(bag) * pow(D[p], pwr2);
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

          int p = procOf[c];
          weightCallsG[p] += 1LL * sz(revReach) * maxBagSz[p] * D[p];
        }
      }
    }

    //		db(weightCallsG);

    return ApaInstance<vi>(n_G, n_H, edgeListG, edgeListH, vertexTypeG, procOf,
                           TWD, TWD_root, TWD_par, TDD_root, par_tdH,
                           Ifdsalgebra, weightSameBag, weightCentroid,
                           weightCallsG);
  }
};

