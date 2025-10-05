

#include "Support/UnionFind.h"

using namespace std;

UnionFind::UnionFind(unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    id.push_back(i);
  }
}

unsigned UnionFind::mk() {
  unsigned n = id.size();
  id.push_back(n);
  return n;
}

unsigned UnionFind::find(unsigned i) {
  unsigned root = i;
  while (root != id[root])
    root = id[root];
  id[i] = root;
  return root;
}

unsigned UnionFind::merge(unsigned p, unsigned q) {
  unsigned i = find(p);
  unsigned j = find(q);
  id[i] = j;
  return j;
}
