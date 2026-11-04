#pragma once

#include "Dataflow/DemandAPA/Algorithm.h"

template <class T> void Algorithm<T>::doComparison() {
  int maxQueries = 1000000;
  if (const char *value = getenv("LOTUS_DEMAND_APA_MAX_QUERIES"))
    maxQueries = std::max(1, stoi(value));

  row.nG = n_G;
  row.mG = sz(edgeListNaiveOneBased);
  row.nCallGraph = n_H;
  row.mCallGraph = sz(edgeListH);

  vvi reachH = [&]() {
    vvi ret(n_H);
    vi vis(n_H, 0);
    for (int p = 0; p < n_H; ++p) {
      function<void(int)> dfs = [&](int u) {
        vis[u] = p + 1;
        ret[p].pb(u);
        for (auto &v : H[u]) {
          if (vis[v] != p + 1)
            dfs(v);
        }
      };
      dfs(p);
    }
    return ret;
  }();

  V<ll> possibleVs(n_H, 0);
  for (int p = 0; p < n_H; ++p) {
    for (auto &pp : reachH[p]) {
      possibleVs[p] += sz(nodesPerProc[pp]);
    }
  }

  ll maxNumQueries = 0;

  vi querySrcs;
  for (int u = 0; u < n_G; ++u)
    if (!callsG[u].empty())
      querySrcs.pb(u), maxNumQueries += possibleVs[procOf[u]];

  db(maxNumQueries);

  default_random_engine e(0);
  vii queries;
  if (maxNumQueries < 500000000) {
    db("AAAAAAAAAAAA");
    for (auto &u : querySrcs) {
      for (auto &pp : reachH[procOf[u]]) {
        for (auto &v : nodesPerProc[pp]) {
          queries.pb(mp(u, v));
        }
      }
    }

    shuffle(queries.begin(), queries.end(), e);

    while (sz(queries) > maxQueries)
      queries.pop_back();

    db(sz(queries));
  } else {
    pb_set<pii> querySet;
    while (sz(querySet) < maxQueries) {
      int u = querySrcs[rand() % sz(querySrcs)];
      int v;

      int randomReachablePr = reachH[procOf[u]][rand() % sz(reachH[procOf[u]])];
      v = nodesPerProc[randomReachablePr]
                      [rand() % sz(nodesPerProc[randomReachablePr])];

      querySet.insert(mp(u, v));
    }
    for (auto &pr : querySet)
      queries.pb(pr);
    shuffle(queries.begin(), queries.end(), e);
  }

  db(sz(queries));

  row.numQueries = sz(queries);

  double totQueryTime = 0;
  double totNaiveQueryTime = 0;
  double t;

  int qNaiveAnswered = 0;
  V<T> ourAns(sz(queries));
  V<T> naiveAns(sz(queries));

  t = omp_get_wtime();
  int hmm = 0;
  for (int i = 0; i < sz(queries); ++i) {
    int u = queries[i].fs, v = queries[i].sc;
    ourAns[i] = query(u, v);
  }
  totQueryTime = omp_get_wtime() - t;

  row.totQueryTime = totQueryTime;
  row.OAR = (totQueryTime + row.Proc) / sz(queries);
  if (row.Proc > baselineTimeout)
    row.ratio = 0;
  else
    row.ratio = 1;

  for (int i = 0; i < sz(queries); ++i) {
    int u = queries[i].fs, v = queries[i].sc;
    t = omp_get_wtime();
    naiveAns[i] = queryNaive(u, v);
    totNaiveQueryTime += omp_get_wtime() - t;
    qNaiveAnswered++;
    db(qNaiveAnswered, totNaiveQueryTime);
    if (totNaiveQueryTime > baselineTimeout) {
      break;
    }
  }

  row.totNaiveQueryTime = totNaiveQueryTime;
  row.BAR = totNaiveQueryTime / qNaiveAnswered;
  row.ratioNaive = double(qNaiveAnswered) / sz(queries);
  row.qNaiveAnswered = qNaiveAnswered;
  row.performanceRatio = row.BAR / row.OAR;
}

