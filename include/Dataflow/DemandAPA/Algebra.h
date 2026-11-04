#pragma once

#include "Dataflow/DemandAPA/RegEx.h"
#include "Dataflow/DemandAPA/Support.h"
#include "bdd.h"

template <class T> class Algebra {
public:
  T zeroA;
  T oneA;
  T (*plusA)(T, T);
  T (*dotA)(T, T);
  T (*starA)(T);
  T (*projectA)(
      int,
      T); // first argument is interpretation of the corresponding (c, s) edge
  Algebra(T zero, T one, T (*plus)(T, T), T (*dot)(T, T), T (*star)(T),
          T (*project)(int, T))
      : zeroA(zero), oneA(one), plusA(plus), dotA(dot), starA(star),
        projectA(project) {}
};

// Encoding of elements: 0: empty vector, 1: vector containing {-1}
// Otherwise, for element f, let d_1 and d_2 be the sizes of the domains s.t.
// for all (x, y) in f, 0 <= x <= d1 and 0 <= y <= d2 Then f is represented with
// a vector containing pairs (d1, d2), (x1, y1), ... for all (x, y) in f. We
// represent pairs with ll. WHERE: (x1, y1) ... are sorted in increasing order
// of their corresponding ll

struct rollingHash {
  size_t operator()(const vi &v) const {
    size_t hashVal = 0;
    for (int i = 0; i < sz(v); ++i)
      hashVal = (hashVal + (v[i] + 1) * 998244353LL) % 1000000009;
    return hashVal;
  }
};
inline unordered_map<vi, vi, rollingHash> IfdsstarCache[MAX_N_THREADS];

inline V<int> Ifdszero;
inline V<int> Ifdsone = V<int>({-1});

inline V<int> Ifdsplus(V<int> a, V<int> b) {
  if (a.empty())
    return b;
  if (b.empty())
    return a;

  if (a[0] == -1 && b[0] == -1)
    return Ifdsone;

  for (int i = 2; i < sz(a); ++i)
    assert(a[i] > a[i - 1]);
  for (int i = 2; i < sz(b); ++i)
    assert(b[i] > b[i - 1]);

  V<int> c;
  set<int> edges;

  int d1_a = a[0] >> 16, d2_a = a[0] & FIRST_16BIT;
  int d1_b = b[0] >> 16, d2_b = b[0] & FIRST_16BIT;

  if (a[0] == -1) {
    assert(d1_b == d2_b);
    c.pb(b[0]);
    for (int i = 1; i < sz(b); ++i)
      edges.insert(b[i]);
    for (int i = 0; i <= d1_b; ++i)
      edges.insert((i << 16) | i);
  } else if (b[0] == -1) {
    assert(d1_a == d2_a);
    c.pb(a[0]);
    for (int i = 1; i < sz(a); ++i)
      edges.insert(a[i]);
    for (int i = 0; i <= d1_a; ++i)
      edges.insert((i << 16) | i);
  } else {
    assert(a[0] == b[0]);
    c.pb(a[0]);
    for (int i = 1; i < sz(a); ++i)
      edges.insert(a[i]);
    for (int i = 1; i < sz(b); ++i)
      edges.insert(b[i]);
  }

  for (auto &elem : edges)
    c.pb(elem);
  for (int i = 2; i < sz(c); ++i)
    assert(c[i] > c[i - 1]);
  return c;
}

inline V<int> Ifdsdot(V<int> a, V<int> b) {
  if (a.empty() || b.empty())
    return Ifdszero;
  if (a[0] == -1)
    return b;
  if (b[0] == -1)
    return a;

  for (int i = 2; i < sz(a); ++i)
    assert(a[i] > a[i - 1]);
  for (int i = 2; i < sz(b); ++i)
    assert(b[i] > b[i - 1]);

  int d1_a = a[0] >> 16, d2_a = a[0] & FIRST_16BIT;
  int d1_b = b[0] >> 16, d2_b = b[0] & FIRST_16BIT;
  assert(d2_a == d1_b);
  V<int> c;
  c.pb((d1_a << 16) | d2_b);
  vvi G_b(d1_b + 1);
  for (int i = 1; i < sz(b); ++i) {
    int u = b[i] >> 16, v = b[i] & FIRST_16BIT;
    assert(u >= 0 && u <= d1_b);
    assert(v >= 0 && v <= d2_b);
    G_b[u].pb(v);
  }
  si edges;
  for (int i = 1; i < sz(a); ++i) {
    int u = a[i] >> 16, v = a[i] & FIRST_16BIT;
    assert(u >= 0 && u <= d1_a);
    assert(v >= 0 && v <= d2_a);
    //		db(G_b[v]);
    for (auto &w : G_b[v])
      edges.insert((u << 16) | w);
  }
  for (auto &elem : edges)
    c.pb(elem);
  for (int i = 2; i < sz(c); ++i)
    assert(c[i] > c[i - 1]);
  return c;
}

inline V<int> Ifdsstar(V<int> a) {
  if (a.empty())
    return Ifdsone;
  if (a[0] == -1)
    return a;
  for (int i = 2; i < sz(a); ++i)
    assert(a[i] > a[i - 1]);
  //	db(sz(IfdsstarCache));
  int processor_id = omp_get_thread_num();
  //	db(processor_id);
  if (present(IfdsstarCache[processor_id], a)) {
    //		cout << "Cache hit!" << endl;
    return IfdsstarCache[processor_id][a];
  }
  int d1_a = a[0] >> 16, d2_a = a[0] & FIRST_16BIT;
  assert(d1_a == d2_a);
  pb_set<int> everything;
  for (int i = 0; i <= d1_a; ++i)
    everything.insert((i << 16) | i);

  V<int> aPowered = Ifdsone;
  int cnt = 0;
  //	db(sz(everything));
  while (true) {
    ++cnt;
    int Sz = sz(everything);
    aPowered = Ifdsdot(aPowered, a);
    for (int i = 1; i < sz(aPowered); ++i)
      everything.insert(aPowered[i]);
    //		db(sz(everything));
    if (Sz == sz(everything))
      break;
  }
  //	db(cnt);
  V<int> c;
  c.pb(a[0]);
  for (auto &elem : everything)
    c.pb(elem);

  for (int i = 2; i < sz(c); ++i)
    assert(c[i] > c[i - 1]);
  //	db(c);
  IfdsstarCache[processor_id][a] = c;
  //	db(IfdsstarCache);
  return c;
}

/*RegEx* REzero = new RegEx(RegEx::ZERO);
RegEx* REone = new RegEx(RegEx::ONE);


RegEx* REplus(RegEx* a, RegEx* b) {
        return new RegEx(RegEx::PLUS, a, b);
}
RegEx* REdot(RegEx* a, RegEx* b) {
        return new RegEx(RegEx::DOT, a, b);
}
RegEx* REstar(RegEx* a) {
        return new RegEx(RegEx::STAR, a, nullptr);
}

RegEx* REproject(RegEx* a, RegEx* b) {
        return REdot(a, b);
}*/

inline int SPzero = INF;
inline int SPone = 0;

inline int SPplus(int a, int b) { return min(a, b); }
inline int SPdot(int a, int b) {
  if (a == -INF || b == -INF)
    return -INF;
  if (a == INF || b == INF)
    return INF;
  return a + b;
}
inline int SPstar(int a) {
  if (a < 0)
    return -INF;
  return 0;
}

inline bdd PAzero = bddfalse;
inline bdd PAone = bddtrue; // will be redefined in BpReader
// these are global to be accessed from Algebra.h
inline int G; // G = |globals|
inline int L; // max_i(|locals[i]|), we'll assume all functions have L local
              // variables, even if not all of them are used
inline int GL; // G + L
inline int maxGL = 0;
inline string curStmtType;
inline int curProc, curNode;

inline void allsatHandler(char *varset, int size) {
  assert(size >= 4 * GL);
  V<string> rel(4);
  for (int v = 0; v < 4 * G; ++v) {
    rel[v % 4].pb(varset[v] < 0 ? 'X' : (char)('0' + varset[v]));
  }
  //	db(rel[0]);
  //	db(rel[1]);
  db(rel);
}

inline void print(bdd a) {
  bdd_allsat(a, allsatHandler);
  cout << "======================" << endl;
}

template <class T> void print(T a) {}

template <class T> void check(T a, T b) {}

inline void check(bdd a, bdd b) {
  assert(bdd_imp(a, b) == bddtrue);
  //	assert(a != b);
}

inline int var(int i) { return 4 * i; }
inline int varPrimed(int i) { return 4 * i + 1; }
inline int varDoublePrimed(int i) { return 4 * i + 2; }
inline int varTriplePrimed(int i) { return 4 * i + 3; }

inline string tabs = "";

inline void setPAone() {
  // setting the value of identity transformer
  PAone = bddtrue;
  for (int i = 0; i < GL; ++i)
    PAone &= bdd_biimp(bdd_ithvar(var(i)), bdd_ithvar(varPrimed(i)));
}

inline bdd PAplus(bdd a, bdd b) {
  //	cout << tabs << "Plus: start" << endl;
  bdd ans = a | b;
  //	cout << tabs << "Plus: finish" << endl;
  return ans;
}

inline bdd PAdot(bdd a, bdd b) {
  //	cout << tabs << "Dot: start" << endl;
  bdd ans;
  // IMP
  if (a == PAzero || b == PAzero)
    ans = PAzero;
  else if (a == PAone)
    ans = b;
  else if (b == PAone)
    ans = a;
  else {
    for (int i = 0; i < GL; ++i) {
      a = bdd_compose(a, bdd_ithvar(varDoublePrimed(i)), varPrimed(i));
      b = bdd_compose(b, bdd_ithvar(varDoublePrimed(i)), var(i));
    }
    ans = a & b;
    for (int i = 0; i < GL; ++i) {
      ans = bdd_exist(ans, bdd_ithvar(varDoublePrimed(i)));
      //			print(ans);
    }
  }
  //	cout << tabs << "Dot: finish" << endl;
  return ans;
}

inline bdd PAstar(bdd a) {
  //	cout << tabs << "Star: start" << endl;
  tabs += "\t";
  assert(sz(tabs) <= 1);
  bdd ans = PAone;
  int iterations = 0;
  while (true) {
    ++iterations;
    //		cout << tabs << "Star iterations: " << iterations << endl;
    bdd oldAns = ans;
    ans = PAdot(ans, a);
    ans = PAplus(oldAns, ans);
    //		print(ans);
    if (ans == oldAns)
      break;
  }
  tabs.pop_back();
  //	cout << tabs << "Star: finish" << endl;
  return ans;
}

// returns the formula x'_idx = x_idx
inline bdd setEq(int idx) {
  return bdd_biimp(bdd_ithvar(var(idx)), bdd_ithvar(varPrimed(idx)));
}

// returns the formula x'_idx != x_idx
inline bdd setNotEq(int idx) {
  return bdd_biimp(bdd_nithvar(var(idx)), bdd_ithvar(varPrimed(idx)));
}

// MUST match the one in BpReader
inline bdd setVar(int idx, string eType, bdd EOpL, bdd EOpR) {
  assert(idx >= 0 && idx < GL);
  if (eType == "choose") {
    bdd left = EOpL;
    bdd right = EOpR;
    return bdd_imp(left & (!right), bdd_ithvar(varPrimed(idx))) &
           bdd_imp(((!left) & right), bdd_nithvar(varPrimed(idx)));
  } else
    return bdd_biimp(bdd_ithvar(varPrimed(idx)), EOpL);
}

// deadIndices = {d_0 ... d_k} subset of {0, .. GL-1}
// returns the formula (x'_a_0 = x_a_0) & (x'_a_1 = x_a_2) ...
// where a_0 a_1 .. = {0, .. GL-1} \ deadIndices
inline bdd deadTF(si deadIndices) {
  for (auto &idx : deadIndices)
    assert(idx >= 0 && idx < GL);
  bdd ans = bddtrue;
  for (int i = 0; i < GL; ++i)
    if (!present(deadIndices, i))
      ans &= setEq(i);
  return ans;
}

inline void checkStats() {
  bddStat b;
  bdd_stats(&b);
  db(b.produced);
}
