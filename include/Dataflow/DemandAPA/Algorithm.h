#pragma once

#include "Dataflow/DemandAPA/ApaInstance.h"
#include "Dataflow/DemandAPA/LCA.h"
#include "Dataflow/DemandAPA/SCC.h"
#include "Dataflow/DemandAPA/Support.h"
#include "Dataflow/DemandAPA/Tarjan.h"
#include "Dataflow/DemandAPA/TreeDecomposition.h"

template <class T> class Algorithm {
public:
  double Time;
  RowInExcelSheet row;
  int threadNum;
  int baselineTimeout = 20;

  /*
   * Pre-preprocessing
   */

  // data structures that are directly inherited from the ApaInstance

  int n_G, n_H;
  // edgeListGPerProc[p] stores all edges between CFG nodes in procedure p; (u,
  // v) = ((ll(u)) << 32) | v
  vector<unordered_map<ll, T>> edgeListGPerProc;
  // stores all edges (c, s) nodes; (u, v) = ((ll(u)) << 32) | v
  unordered_map<ll, T> edgeListGInter;
  // edges of call graph, the value is initially zeroA. After interprocedural
  // preprocessing,
  unordered_map<ll, T> edgeListH; // it'll hold g(f_i, f_j) values (see draft)
  vector<int> vertexTypeG, procOf;
  vector<vector<vector<int>>> TWD;
  vector<int> TWD_root;
  vector<vector<int>> TWD_par;
  int TDD_root;
  vector<int> TDD_par;
  T zeroA, oneA;
  T (*plusA)(T, T);
  T (*dotA)(T, T);
  T (*starA)(T);
  T (*projectA)(int, T);
  vector<ll> weightSameBag, weightCentroid, weightCallsG;

  // data structures created in pre-preprocessing

  vii callNodeInfo; // for call node c, callNodeInfo = (r, p) where r is the
                    // return-site node, and p is the called procedure
  // for procedure p, callerReturnSitePairs[p] is a list of elements ((c, r),
  // p') containing all the call-return-site pairs lying inside p and the
  // function p' that they call
  V<V<pair<pair<int, int>, int>>> callerReturnSitePairs;
  vector<vector<vector<int>>> TWD_adj;
  vector<vector<int>> TDD_adj;
  vector<LCA> TWDLCA;
  LCA TDDLCA;
  vvi H, HRev;
  vector<int>
      s; // for i in [0, n_H), s[i] in [0, n_G) is the start node of proc_i in G
  vector<int>
      e; // for i in [0, n_H), e[i] in [0, n_G) is the exit node of proc_i in G
  vvi nodesPerProc;
  // for i in [0, n_G), if vertexTypeG[i] == CALL_VERTEX, calledFunction[i] (in
  // [0, n_H)) is the function called by i. Otherwise calledFunction[i] = -1
  vi calledFunction;

  // information obtained by traversing the twd decomposition. Some of it are
  // used to validate twd decompositions, other are used by intraprocedural
  // algorithm

  vector<vector<int>> dfsOrder; // for procedure, dfsOrder[p] contains the bags
                                // of TWD[p] in DFS bottom-up order
  // rb[u] for u in [0, n_G) is a bag label in [0, |TWD[procOf[u]]|), which is,
  vector<int> rb; // the root bag of u. i.e. the lowest-depth bag that has u.
  // bagsContainingNode[u] is all bags in [0, |TWD[procOf[u]]|) that contain u.
  vector<vector<int>> TWD_depth; // TWD_depth[p][j] = depth of bag j (j in [0,
                                 // |TWD[p]|)) in treedecomp. of p.
  vector<vector<int>> bagsContainingNode;

  // for procedure p, ancInTDD[p] is the set of its ancestors in TDD, INCLUDING
  // p
  vector<unordered_set<int>> ancInTDD;
  vector<int> TDD_depth; // for procedure p, TDD_depth[p] = depth of p in the td
                         // decomposition

  // pre-preprocessing functions
  void prePreprocess(ApaInstance<T> *);
  void copyData(ApaInstance<T> *);
  void doAuxiliaryStuff();
  void dfsTWD(vvi &, int, int, int, int);
  void dfsTDD(int, vi, int, int);
  void validateTWD();
  void validateTDD();

  /*
   * Function summary preprocessing
   */

  // For every RE (which can be a subexpression of some RHS RE), includedVars is
  // a list of procedures p where X_p appears in the RE.
  unordered_map<RegEx *, unordered_set<int>> includedVars;
  // for a procedure p, REsWithVar[p] is the set of all (sub)expressions
  // containing X_p
  unordered_map<int, unordered_set<RegEx *>> REsWithVar;
  unordered_set<RegEx *> visRE; // a visited array to traverse the RE DAG
  vector<T> summary; // summary[p] is the value of the summary of function p
  // Function summary data structures go here
  // for every (c, r), where c calls f, it is mapped to a pointer to a RegEx (c,
  // r) + Project_(c, s_f)(summary[f] (e_f, r)) After computing function
  // summaries, its interpretation will correspond to the summary of the valid
  // paths from c to r we simply replace edgeListGPerProc[procOf[c]][(c, r)]
  // with I(summariesAtCalls[(c, r)]) Note that we might encounter REs with
  // different pointers, but all of them contain the same RE, we just store one
  // of them.
  unordered_map<ull, RegEx *> summariesAtCalls;

  // Function summary methods go here
  void preprocessFunctionSummaries();
  vector<RegEx *> getCFGsRE();
  void unlabelRegEx(RegEx *, vi &, vii); // third parameter is just for testing
  void getLeaves(RegEx *, vector<RegEx *> &);
  void buildIncludedVars(RegEx *);

  /*
   * Naive algorithm
   */
  unordered_map<ll, T> edgeListNaive;
  vii edgeListNaiveOneBased;
  unordered_map<RegEx *, T> cacheNaive;
  Tarjan *naiveTar;

  // computes function summaries and builds the "summarized" CFGs where (c, r)
  // holds the summary of the call and we maintain (c, s) edges but no (e, r)
  // edges, also initializes an instance of Tarjan on this graph
  void prepareNaive();
  T queryNaive(int, int);
  T Inaive(RegEx *, bool);
  vector<Tarjan *> functionPETarjan;

  /*
   * Intraprocedural preprocessing
   */

  map<int, unordered_map<ll, T>> naivePrecomp; // for testing
  // same-bag preprocessing data structures

  // (none)

  // centroid preprocessing data structures

  vector<TreeDec> Tr; // Data structure to keep tree decompositions
  // IMP: If this is a vector<bool>, will give incorrect when parallelizing
  // without locks!
  vector<int> computed;     // To ensure operating each vertex at most one time
  vector<LCA> Centroid_LCA; // Keeps LCA data structure for each procedure

  // intraprocedural preprocessing functions
  void preprocessIntraprocedural();
  unordered_map<ll, T> getAllPairsAns(int);
  void prepareNaiveAnswers();

  // same-bag preprocessing functions
  void sameBagPreprocessing();
  void solveSameBag(int, int);
  void updateBag(int, int);
  void checkSameBagCorrectness();

  // centroid preprocessing functions
  void centroidPreprocessing();
  void read_twd(int);
  void compute(int, int, int, int);
  void solveCentroid(int);
  T SCQ(int, int);
  void checkIntraCorectness();

  /*
   * Interprocedural preprocessing
   */
  // interprocedural preprocessing data structures
  // auxiliary data used to build callsG
  vi visCallsG;
  vvi GRev;
  // for every CFG node u, callsG[u] has a list of all pairs (s, val), where
  // each pair corresponds to  a call node c in procOf[u] calling a function
  // whose start node is s, and SCQ(u, c) != zeroA val holds dotA(SCQ(u, c),
  // f(c, s))
  vector<vector<pair<int, T>>> callsG;
  // for procedure p, up[p][i] is an interpretation of a PE (in the call graph)
  // from p to it's ancestor at depth i in the TDD, down[p][i] is the same but
  // the PE is from the ancestor to p.
  vector<vector<T>> up, down;

  // interprocedural preprocessing functions
  void preprocessInterprocedural();
  void computeHEdgeValues();
  void buildCallsG();
  void buildCallsGPerProc(int);
  void buildUpAndDown();
  void testCGQCorrectness();
  T CGQ(int, int);
  T EQ(int, int);
  T MQ(int, int);
  T query(int, int);

  /*
   * Comparison between the two algorithms, including query generation
   */

  void doComparison();

  Algorithm() {}

  void work(ApaInstance<T> *inst, RowInExcelSheet _row, int _threadNum,
            int timeout = 20) {
    baselineTimeout = timeout;
    threadNum = _threadNum;
#ifndef _OPENMP
    threadNum = 1;
#endif
    row = _row;
    double t_preprocessing = omp_get_wtime();

    prePreprocess(inst);

    db(n_G, n_H);

    for (int i = 0; i < MAX_N_THREADS; ++i)
      IfdsstarCache[i].clear();

    double t_functionsSummary = omp_get_wtime();
    preprocessFunctionSummaries();
    row.functionSummaryTime = omp_get_wtime() - t_functionsSummary;

    prepareNaive();

    preprocessIntraprocedural();
    preprocessInterprocedural();
    row.Proc = omp_get_wtime() - t_preprocessing;

    stopClock(t_preprocessing, "Total preprocessing time");
    doComparison();

    row.printRow();
  }

  pair<vector<pair<int, int>>, vi> relabel(vii edges, vi nodes,
                                           int oneBased = 0) {
    unordered_map<int, int> label;
    vi unlabel;
    int cnt = 0;

    if (oneBased)
      ++cnt, unlabel.pb(-1);

    function<void(int)> add = [&](int x) {
      if (!present(label, x))
        label[x] = cnt++, unlabel.pb(x);
    };

    for (auto &u : nodes)
      add(u);

    for (auto &pr : edges) {
      assert(present(label, pr.fs));
      assert(present(label, pr.sc));
      pr = mp(label[pr.fs], label[pr.sc]);
    }

    for (auto &edge : edges)
      assert(min(edge.fs, edge.sc) >= 0 && max(edge.fs, edge.sc) < sz(unlabel));

    return mp(edges, unlabel);
  }

  // returns a fail splitting for the procedures with the given weights as
  // indicator of the cost of each function
  vvi getNiceSlitting(V<ll> weights, int threadNum) {
    // Can be tweaked
    db(threadNum);
    assert(threadNum <= MAX_N_THREADS);
    vvi parts(threadNum);

    vi procedures(n_H);

    for (int p = 0; p < n_H; ++p)
      procedures[p] = p;

    sort(procedures.begin(), procedures.end(),
         [&](int i, int j) { return weights[i] < weights[j]; });

    for (int p = 0; p < n_H; ++p)
      parts[p % threadNum].pb(procedures[p]);

    return parts;
  }
};

// Template definitions belong to Algorithm's interface. Clients can include
// Algorithm.h alone; definition headers also include this declaration and are
// protected against recursive inclusion by their own header guards.
#include "Dataflow/DemandAPA/CentroidPreprocessing.h"
#include "Dataflow/DemandAPA/Comparison.h"
#include "Dataflow/DemandAPA/FunctionSummaries.h"
#include "Dataflow/DemandAPA/Interprocedural.h"
#include "Dataflow/DemandAPA/Intraprocedural.h"
#include "Dataflow/DemandAPA/Naive.h"
#include "Dataflow/DemandAPA/PrePreprocess.h"
#include "Dataflow/DemandAPA/SameBagPreprocessing.h"
