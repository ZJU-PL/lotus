#ifndef _GRAPH_H
#define _GRAPH_H

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <list>
#include <deque>
#include <algorithm>
#include <utility>
#include <cmath>
#include <string>
#include <cassert>
#include <unordered_map>

#include "BitVector.h"

using namespace std;

#ifndef MAX_VAL
#define MAX_VAL 100000000
#endif
#ifndef MIN_VAL
#define MIN_VAL -100000000
#endif

enum NodeType {
    NORMAL = 0,
    INPUT,
    ARG,
    RET,
    OUTPUT
};

struct Vertex {
    int id;
    bool visited;
    int min_parent_level;
    bool fat;    // fat node
    int topo_id;    // topological order
    int top_level;    // topological level
    int path_id;    // path id
    int dfs_order;
    int pre_order;
    int post_order;
    int first_visit; // for test
    int kind = NodeType::NORMAL;
    int func_id = -1;
    int o_vid = -1;
    bool removed = false;

    double tcs;
    int mingap;
    vector<int> *pre;
    vector<int> *post;
    vector<int> *middle;

    Vertex(int ID) : id(ID) {
        top_level = -1;
        visited = false;
    }

    Vertex() {
        top_level = -1;
        visited = false;
    };

};

typedef vector<int> EdgeList;    // edge list represented by vertex id list
typedef vector<Vertex> VertexList;    // vertices list (store real vertex property) indexing by id

struct In_OutList {
    EdgeList inList;
    EdgeList outList;
};
typedef vector<In_OutList> GRA;    // index graph

struct pair_hash {
   std::size_t operator() (const std::pair<int, int> &p) const {
       long p1 = p.first;
       long p2 = p.second;
       if (p1 < 0 || p2 < 0) {
             cerr << "key error " << p1 << " " << p2 << "\n";
             exit(10); 
       }
       long x = (p1 << 32) | p2;
       return std::hash<long>{}(x);
   }
};

class Graph {
protected:
    VertexList vl;
    GRA graph;
    int n_vertices = 0;
    int n_edges = 0;

    std::unordered_map<std::pair<int, int>, int, pair_hash> pos_label_map;
    std::unordered_map<std::pair<int, int>, int, pair_hash> neg_label_map;
    std::unordered_map<int, std::set<int>> summary_edges; // out <- in, a reversed map

public:
    Graph();

    explicit Graph(int);

    explicit Graph(istream &);

    Graph(GRA &, VertexList &);

    ~Graph();

    void readGraph(istream &);

    void writeGraph(ostream &);

    void printGraph();

    void addVertex(int);

    virtual void remove_vertex(int);

    void addEdge(int, int);

    void addEdge(int, int, int);

    int num_vertices();

    int num_edges();

    VertexList &vertices();

    EdgeList &out_edges(int);

    EdgeList &in_edges(int);

    int out_degree(int);

    int in_degree(int);

    vector<int> getRoots();

    bool hasEdge(int, int);

    Graph &operator=(const Graph &);

    Vertex &operator[](int);

    Vertex &at(int);

    void clear();

    void strTrimRight(string &str);

    Graph(unordered_map<int, vector<int> > &inlist, unordered_map<int, vector<int> > &outlist);

    void extract(unordered_map<int, vector<int> > &inlist, unordered_map<int, vector<int> > &outlist);

    void printMap(unordered_map<int, vector<int> > &inlist, unordered_map<int, vector<int> > &outlist);

    void print_edges();

    double tcs(int);

    void sortEdges();

    static vector<string> split(const string &s, char delim);

    static vector<string> &split(const string &s, char delim, vector<string> &elems);

    void build_summary_edges();

    size_t summary_edge_size();

    void to_indexing_graph();

    void removeEdge(int s, int t);

    void check();

    int label(int s, int t);

    void add_summary_edges();
};

#endif
