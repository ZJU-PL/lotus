#pragma once

#include "Dataflow/DemandAPA/Support.h"

class TreeDec {
public:
  TreeDec() {
    n_G = 0;
    treewidth = n_bag = 0;
  }

  TreeDec(int _n_bag, int _width, int _n) {
    treewidth = _width;
    n_bag = _n_bag;
    n_G = _n;
    LOG = int(log2(n_bag)) + 1;

    all_bags.resize(n_bag);
    tree.resize(n_bag);
    ctd.resize(n_bag);
    // lvl.resize(n_bag);
    vis.resize(n_bag);
    sub.resize(n_bag);
    P.resize(n_bag);
    all_descendants.resize(n_bag);
    belonging_bag.clear();
    // post_order.clear();
    for (int i = 0; i < n_bag; i++) {
      all_bags[i].clear(), tree[i].clear();
      ctd[i].clear(), all_descendants[i].clear();
      vis[i] = 0;
    }
  }

  void add_vertex_to_bag(int bag, int v) {
    assert(0 <= v and v < n_G);
    assert(0 <= bag and bag < n_bag);
    all_bags[bag].pb(v);
    belonging_bag[v] = bag;
  }

  void add_edge(int u, int v) {
    assert(0 <= u and u < n_bag);
    assert(0 <= v and v < n_bag);
    assert(u != v);
    tree[u].pb(v);
  }

  void show_decomposition() {
    puts("---------------------------------");
    printf("Number of possible vertices = %d\n", n_G);
    printf("Treewidth = %d\n", treewidth);
    printf("Number of bags = %d\n", n_bag);
    for (int i = 0; i < n_bag; i++) {
      printf("Bag %d: ", i);
      for (auto to : all_bags[i])
        printf("%d ", to);
      puts("");
    }
    printf("Tree Decomposition\n");
    for (int i = 0; i < n_bag; i++) {
      for (auto to : tree[i])
        if (i < to)
          printf("%d %d\n", i, to);
    }
    puts("---------------------------------");
  }

  void show_ctd() {
    puts("---------------------------------");
    printf("Centroid Tree Decomposition\n");
    for (int i = 0; i < n_bag; i++) {
      for (auto to : ctd[i])
        printf("%d %d\n", i, to);
    }
    puts("---------------------------------");
  }

  void prep(int nd, int pr) {
    sub[nd] = 1;
    for (auto to : tree[nd])
      if (to != pr and !vis[to]) {
        prep(to, nd);
        sub[nd] += sub[to];
      }
  }

  int find_centroid(int nd, int pr, int sz) {
    for (auto to : tree[nd])
      if (to != pr and !vis[to] and sub[to] > sz)
        return find_centroid(to, nd, sz);
    return nd;
  }

  void save_descendats(int nd, int pr, vector<int> &descendants) {
    if (!vis[nd])
      descendants.pb(nd);
    for (auto to : tree[nd])
      if (to != pr and !vis[to])
        save_descendats(to, nd, descendants);
  }

  void dfs(int nd, int par) {
    prep(nd, -1);
    int centr = find_centroid(nd, -1, sub[nd] >> 1);
    if (~par)
      ctd[par].pb(centr);
    else
      root_centr = centr;
    P[centr] = par;
    vis[centr] = 1;
    save_descendats(centr, -1, all_descendants[centr]);
    for (auto to : tree[centr])
      if (!vis[to])
        dfs(to, centr);
  }

  void compute_centroid() {
    for (int i = 0; i < n_bag; i++)
      if (!vis[i])
        dfs(i, -1);
    for (int i = 0; i < n_bag; i++)
      vis[i] = 0;
  }
  // void build_lca(){
  // 	for (int j = 1; j < LOG; j++)
  // 		for (int i = 0; i < n_bag; i++)
  // 			if (~P[i][j-1])
  // 				P[i][j] = P[P[i][j-1]][j-1];
  // }
  // int LCA(int x, int y){
  // 	if (lvl[x] < lvl[y])
  // 		swap(x, y);
  // 	for (int i = LOG - 1; i >= 0; i--)
  // 		if (~P[x][i] and lvl[P[x][i]] >= lvl[y])
  // 			x = P[x][i];
  // 	if (x == y)
  // 		return x;
  // 	for (int i = LOG - 1; i >= 0; i--)
  // 		if (~P[x][i] and P[x][i] != P[y][i])
  // 			x = P[x][i], y = P[y][i];
  // 	return P[x][0];
  // }

  // Query
  vector<int> get_descendats(int bag) { return all_descendants[bag]; }

  vector<int> get_bag(int bag) { return all_bags[bag]; }

  vector<int> get_parents() { return P; }

  // vector<int> get_parent(int bag){
  // 	return tree[bag];
  // }
  int get_belonging_bag(int v) {
    assert(belonging_bag.find(v) != belonging_bag.end());
    return belonging_bag[v];
  }

  int vertices() { return n_G; }

  int bags() { return n_bag; }

  int width() { return treewidth; }

  int get_centr() { return root_centr; }

private:
  int treewidth, n_bag, LOG, root_centr, n_G;
  vector<vector<int>> all_bags, tree, ctd, all_descendants;
  vector<int> vis, sub, lvl, post_order, P;
  unordered_map<int, int> belonging_bag;
};

