#pragma once

#include "Dataflow/DemandAPA/Support.h"

class reader {
public:
  int n_G, n_H, m_G, m_H;
  vector<pair<int, int>> unweightedEdgeListG, edgeListH;
  vector<int> vertexTypeG, procOf;
  vector<vector<vector<int>>> TWD;
  vector<int> TWD_root;
  vector<vector<int>> TWD_par;
  int TDD_root;
  vector<int> par_tdH;
  vector<ll> weightSameBag, weightCentroid, weightCallsG;

  void readTDD(string TDDpath) {
    int treedepth;

    ifstream in(TDDpath);
    in >> treedepth;

    par_tdH.assign(n_H, 0);
    for (int i = 0; i < n_H; ++i) {
      if (par_tdH[i])
        assert(par_tdH[i] >= 1 && par_tdH[i] <= n_H);
      in >> par_tdH[i];
      --par_tdH[i];
    }
    vi roots;
    for (int i = 0; i < n_H; ++i) {
      if (par_tdH[i] == -1)
        roots.pb(i);
    }
    assert(sz(roots) == 1);
    TDD_root = roots[0];

    //		db(td);
  }

  void readTWD(string TWDpath) {
    //		db(TWDpath);
    // take TWD from pace solver
    TWD.assign(n_H, vvi());
    TWD_par.assign(n_H, vi());
    TWD_root.assign(n_H, -1);
    ifstream in(TWDpath);
    string s, dummy;

    int n_BB, treewidth;
    vvi bags;
    V<si> adjTw;
    while (getline(in, s)) {
      if (s[0] == 'c')
        continue;
      stringstream ss(s);
      if (s[0] == 's') {
        ss >> dummy >> dummy;
        int nnn;
        ss >> n_BB >> treewidth >> nnn;
        --treewidth;
        //				db(tw);
        assert(nnn == n_G + 1);
        adjTw.assign(n_BB, si());
      } else if (s[0] == 'b') {
        bags.pb(vi());
        ss >> dummy;
        int id;
        ss >> id;
        assert(id == sz(bags));
        int u;
        while (ss >> u) {
          --u;
          if (u != n_G) {
            assert(u >= 0);
            assert(u < n_G);
            bags.back().pb(u);
          }
        }
        //				assert(!bags.back().empty());
      } else {
        int u, v;
        ss >> u >> v;
        --u, --v;
        assert(u >= 0 && v < n_BB);
        if (u == v)
          continue;
        adjTw[u].insert(v);
        adjTw[v].insert(u);
      }
    }

    {
      int cnt = 0;
      function<void(int, int)> dfs = [&](int u, int par) {
        ++cnt;
        for (auto &v : adjTw[u])
          if (v != par) {
            dfs(v, u);
          }
      };

      dfs(0, -1);

      assert(cnt == n_BB);
      //			db("adjTw is a tree!");
    }

    V<map<int, int>> bagsMapping(n_H);

    for (int i = 0; i < n_BB; ++i) {
      map<int, vi> splitting;
      for (auto &u : bags[i]) {
        assert(u >= 0 && u < n_G);
        splitting[procOf[u]].pb(u);
      }
      for (auto &pr : splitting) {
        int p = pr.fs;
        int id = sz(TWD[p]);
        bagsMapping[p][i] = id;
        assert(!pr.sc.empty());
        TWD[p].pb(pr.sc);
      }
    }

    //		db(errNode, TWD[n_H - 1]);

    V<V<si>> adjTwPerProc(n_H);

    for (int p = 0; p < n_H; ++p) {
      adjTwPerProc[p].assign(sz(TWD[p]), si());
    }

    for (int i = 0; i < n_BB; ++i) {
      for (auto &j : adjTw[i]) {
        for (auto &u : bags[i]) {
          for (auto &v : bags[j]) {
            if (procOf[u] == procOf[v]) {
              int p = procOf[u];
              assert(i != j);
              assert(present(bagsMapping[p], i));
              assert(present(bagsMapping[p], j));
              assert(bagsMapping[p][i] != bagsMapping[p][j]);
              assert(bagsMapping[p][i] >= 0 && bagsMapping[p][i] < sz(TWD[p]));
              assert(bagsMapping[p][j] >= 0 && bagsMapping[p][j] < sz(TWD[p]));
              adjTwPerProc[p][bagsMapping[p][i]].insert(bagsMapping[p][j]);
              adjTwPerProc[p][bagsMapping[p][j]].insert(bagsMapping[p][i]);
            }
          }
        }
      }
    }

    //		db(tw);

    for (int p = 0; p < n_H; ++p) {
      TWD_par[p].assign(sz(TWD[p]), -1);

      function<void(int, int)> dfs = [&](int u, int par) {
        assert(u != par);
        TWD_par[p][u] = par;
        for (auto &v : adjTwPerProc[p][u])
          if (v != par) {
            assert(v != u);
            assert(v != par);
            dfs(v, u);
          }
      };

      //			db(p, n_H);

      for (int b = 0; b < sz(TWD[p]); ++b) {
        if (TWD_par[p][b] == -1)
          dfs(b, -1);
      }

      int minusOnes = 0;

      for (int i = 0; i < sz(TWD[p]); ++i) {
        if (TWD_par[p][i] == -1) {
          minusOnes++;
          TWD_root[p] = i;
        }
      }
      for (int i = 0; i < sz(TWD[p]); ++i) {
        if (TWD_par[p][i] == -1 && i != TWD_root[p]) {
          TWD_par[p][i] = TWD_root[p];
        }
      }

      assert(minusOnes >= 1);
    }

    //		db("got the PACE tw-decomp");
  }
};

