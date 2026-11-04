#pragma once

#include "Dataflow/DemandAPA/DemandOmp.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;
template <class T> using pb_set = set<T>;

#define ndl cout << '\n'
#define sz(v) int(v.size())
#define pb push_back
#define mp make_pair
#define fs first
#define sc second
#define present(a, x) (a.find(x) != a.end())
#define __FILENAME__                                                           \
  (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#ifdef LOCAL
#define db(...)                                                                \
  ({                                                                           \
    cout << "> " << __FILENAME__ << " " << __LINE__ << ": ";                   \
    _db(#__VA_ARGS__, __VA_ARGS__);                                            \
  })
#define RNG() rng()
#else
#define db(...) true
#define RNG() true
#endif
#define ff first
#define ss second
#define PII pair<int, int>
#define all(x) x.begin(), x.end()

template <class S, class T>
inline ostream &operator<<(ostream &o, const map<S, T> &v);
template <template <class, class...> class S, class T, class... L>
inline ostream &operator<<(ostream &o, const S<T, L...> &v);
template <class S, class T>
inline ostream &operator<<(ostream &o, const pair<S, T> &pr);
inline ostream &operator<<(ostream &o, const string &s);

template <class T> inline void _db(const char *dbStr, T e) {
  cout << dbStr << " = " << e << endl;
}
template <class T, class... L> inline void _db(const char *dbStr, T e, L... r) {
  while (*dbStr != ',')
    cout << *dbStr++;
  cout << " = " << e << ',';
  _db(dbStr + 1, r...);
}
template <class S, class T>
inline ostream &operator<<(ostream &o, const map<S, T> &v) {
  o << "[";
  int i = 0;
  for (const pair<S, T> &pr : v)
    o << (!i++ ? "" : ", ") << "{" << pr.fs << " : " << pr.sc << "}";
  return o << "]";
}
template <template <class, class...> class S, class T, class... L>
inline ostream &operator<<(ostream &o, const S<T, L...> &v) {
  o << "[";
  int i = 0;
  for (const auto &e : v)
    o << (!i++ ? "" : ", ") << e;
  return o << "]";
}
template <class S, class T>
inline ostream &operator<<(ostream &o, const pair<S, T> &pr) {
  return o << "(" << pr.fs << ", " << pr.sc << ")";
}
inline ostream &operator<<(ostream &o, const string &s) {
  for (const char &c : s)
    o << c;
  return o;
}
template <class T> using V = vector<T>;
template <class T> using VV = V<V<T>>;
template <class T> using VVV = VV<V<T>>;
using ll = long long;
using ull = unsigned long long;
using pii = pair<int, int>;
using vi = V<int>;
using vii = V<pii>;
using vvi = VV<int>;
using mii = map<int, int>;
using umii = unordered_map<int, int>;
using si = set<int>;
using usi = unordered_set<int>;

inline constexpr int INF = 1e9;
inline constexpr ll FIRST_32BIT = (1ULL << 32) - 1;
inline constexpr int FIRST_16BIT = (1 << 16) - 1;
inline constexpr int START_VERTEX = 0;
inline constexpr int EXIT_VERTEX = 1;
inline constexpr int CALL_VERTEX = 2;
inline constexpr int RETURN_SITE_VERTEX = 3;
inline constexpr int ERROR_VERTEX = 4;

inline constexpr int MAX_ALLOWED_GL = 80;
inline constexpr int MAX_N_THREADS = 40;

inline void stopClock(double Time, string s = "") {

  cout << "[" << s << "]" << " time taken = " << (omp_get_wtime() - Time)
       << " s" << endl;
}

class RowInExcelSheet {
public:
  RowInExcelSheet() {}
  string excelFilePath;
  string analysis;
  string program;

  int nG; // number of CFG nodes

  int mG; // number of edges or the "summarized" CFGs (they include (c, s) edges
          // but not (e, r))

  int nCallGraph, mCallGraph; // number of nodes and edges in the call graph

  int tw, td;

  long double
      Proc; // preprocessing time of our algorithm (includes function summaries)

  long double functionSummaryTime;

  long double totQueryTime; // our total uery time

  // our average query time, takes preprocessing into account, = (totQueryTime +
  // Proc) / (# queries answered before timeout)
  long double OAR;

  long double totNaiveQueryTime; // total query time of naive algorithm

  // average query time of the naive algorithm, = totNaiveQueryTime / (# queries
  // answered before timeout)
  long double BAR;

  // ratio of the queries answered by our algorithm before timeout (almost
  // surely to be either 0 (we timeout in preprocessing) or 1)
  long double ratio;

  long double ratioNaive; // ratio of the queries answered by the naive
                          // algorithm before timeout

  int qNaiveAnswered;
  int numQueries;

  long double performanceRatio;

  RowInExcelSheet(string _excelFilePath) : excelFilePath(_excelFilePath) {}

  void printRow() {
    ofstream fOut;
    db(excelFilePath);
    fOut.open(excelFilePath, ios_base::app);
    fOut << fixed;
    fOut.precision(20);
    fOut << analysis << "," << program << "," << nG << "," << mG << ","
         << nCallGraph << "," << mCallGraph << "," << tw << "," << td << ","
         << Proc << "," << functionSummaryTime << "," << totQueryTime << ","
         << OAR << "," << totNaiveQueryTime << "," << BAR << "," << ratio << ","
         << ratioNaive << "," << qNaiveAnswered << "," << numQueries << ","
         << performanceRatio << "\n";

    fOut.close();
  }
};
