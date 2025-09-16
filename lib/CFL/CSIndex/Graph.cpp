#include "CFL/CSIndex/Graph.h"
#include "CFL/CSIndex/CSProgressBar.h"
#include <algorithm>
#include <cstring>

Graph::Graph() {
    graph = GRA();
    vl = VertexList();
}

Graph::Graph(int size) {
    n_vertices = size;
    vl = VertexList(size);
    graph = GRA(size, In_OutList());
}

Graph::Graph(GRA &g, VertexList &vlist) {
    n_vertices = vlist.size();
    graph = g;
    vl = vlist;
}

Graph::Graph(istream &in) {
    readGraph(in);
}

Graph::~Graph() = default;

void Graph::printGraph() {
    writeGraph(cout);
}

void Graph::clear() {
    n_vertices = 0;
    graph.clear();
    vl.clear();
}

void Graph::strTrimRight(string &str) {
    string whitespaces(" \t");
    int index = str.find_last_not_of(whitespaces);
    if (index != string::npos)
        str.erase(index + 1);
    else
        str.clear();
}

void Graph::readGraph(istream &in) {
    string buf;
    getline(in, buf);

    strTrimRight(buf);
    if (buf.length() < strlen("graph_for_greach")) {
        cerr << "BAD FILE FORMAT!" << endl;
        exit(1);
    }

    string tag = buf.substr(0, strlen("graph_for_greach"));
    if (tag != "graph_for_greach") {
        cerr << "BAD FILE FORMAT!" << endl;
        exit(2);
    }

    int n;
    getline(in, buf);
    istringstream(buf) >> n;
    // initialize
    n_vertices = n;
    vl = VertexList(n);
    graph = GRA(n, In_OutList());

    for (int i = 0; i < n; i++)
        addVertex(i);

    string sub;
    int idx;
    int sid = 0;
    int tid = 0;
    while (getline(in, buf)) {
        strTrimRight(buf);
        idx = buf.find(':');
        buf.erase(0, idx + 2);

        auto startIdx = 0;
        auto emptyIdx = buf.find(' ');
        while (emptyIdx != string::npos) {
            sub = buf.substr(startIdx, emptyIdx - startIdx);

            auto cs_idx = sub.find('.');
            if (cs_idx == string::npos) {
                istringstream(sub) >> tid;
                addEdge(sid, tid);
            } else {
                istringstream(sub.substr(0, cs_idx)) >> tid;
                int cs_id;
                istringstream(sub.substr(cs_idx + 1)) >> cs_id;
                addEdge(sid, tid, cs_id);
            }

            startIdx = emptyIdx + 1;
            emptyIdx = buf.find(' ', startIdx);
        }
        sub = buf.substr(startIdx);
        assert(sub[0] == '#');
        istringstream(sub.substr(1)) >> this->at(sid).func_id;

        ++sid;
    }
}


void Graph::writeGraph(ostream &out) {
    cout << "Graph size = " << graph.size() << endl;
    out << "graph_for_greach" << endl;
    out << vl.size() << endl;

    GRA::iterator git;
    EdgeList el;
    EdgeList::iterator eit;
    for (int i = 0; i < vl.size(); i++) {
        out << i << ": ";
        el = graph[i].outList;
        for (eit = el.begin(); eit != el.end(); eit++)
            out << (*eit) << " ";
        out << "#" << endl;
    }
}

void Graph::addVertex(int vid) {
    if (vid >= vl.size()) {
        int size = vl.size();
        for (int i = 0; i < (vid - size + 1); i++) {
            graph.push_back(In_OutList());
            vl.push_back(Vertex(vid + i));
        }
        n_vertices = vl.size();
    }

    Vertex v;
    v.id = vid;
    v.top_level = -1;
    v.visited = false;
    vl[vid] = v;

    EdgeList il = EdgeList();
    EdgeList ol = EdgeList();
    In_OutList oil = In_OutList();
    oil.inList = il;
    oil.outList = ol;
    graph[vid] = oil;
}

void Graph::remove_vertex(int vid) {
    cout << vid << endl;
    EdgeList preds = graph[vid].inList;
    cout << vid << endl;
    for (const auto &pred_it : preds) {
        cout << pred_it << endl;
        auto pred = graph[pred_it].outList;
        auto f_it = find(pred.begin(), pred.end(), vid);
        assert(f_it != pred.end());
        pred.erase(f_it);
    }
    EdgeList succs = graph[vid].outList;
    for (const auto &succ_it : succs) {
        auto succ = graph[succ_it].inList;
        auto f_it = find(succ.begin(), succ.end(), vid);
        assert(f_it != succ.end());
        succ.erase(f_it);
    }
    graph[vid].inList.clear();
    graph[vid].outList.clear();
    n_vertices--;
}

void Graph::addEdge(int sid, int tid) {
    if (sid == tid) {
        return;
    }

    if (sid >= vl.size())
        addVertex(sid);
    if (tid >= vl.size())
        addVertex(tid);
    // update edge list
    graph[tid].inList.push_back(sid);
    graph[sid].outList.push_back(tid);
    n_edges++;
}

void Graph::addEdge(int sid, int tid, int label) {
    if (sid >= vl.size())
        addVertex(sid);
    if (tid >= vl.size())
        addVertex(tid);
    // update edge list
    graph[tid].inList.push_back(sid);
    graph[sid].outList.push_back(tid);
    n_edges++;

    assert(label);
    if (label > 0) {
        pos_label_map[std::make_pair(sid, tid)] = label;
    } else {
        neg_label_map[std::make_pair(sid, tid)] = label;
    }
}

int Graph::num_vertices() {
    return vl.size();
}

int Graph::num_edges() {
    EdgeList el;
    GRA::iterator git;
    int num = 0;
    for (git = graph.begin(); git != graph.end(); git++) {
        el = git->outList;
        num += el.size();
    }
    return num;
}

// return out edges of specified vertex
EdgeList &Graph::out_edges(int src) {
    return graph[src].outList;
}

// return in edges of specified vertex
EdgeList &Graph::in_edges(int trg) {
    return graph[trg].inList;
}

int Graph::out_degree(int src) {
    return graph[src].outList.size();
}

int Graph::in_degree(int trg) {
    return graph[trg].inList.size();
}

// get roots of graph (root is zero in_degree vertex)
vector<int> Graph::getRoots() {
    vector<int> roots;
    GRA::iterator git;
    int i = 0;
    for (git = graph.begin(); git != graph.end(); git++, i++) {
        if (git->inList.empty())
            roots.push_back(i);
    }

    return std::move(roots);
}

// check whether the edge from src to trg is in the graph
bool Graph::hasEdge(int src, int trg) {
    EdgeList &el = graph[src].outList;
    return std::any_of(el.begin(), el.end(), [trg](int ei){ return ei == trg; });
}

// return vertex list of graph
VertexList &Graph::vertices() {
    return vl;
}

Graph &Graph::operator=(const Graph &g) {
    if (this != &g) {
        graph = g.graph;
        vl = g.vl;
        n_vertices = g.n_vertices;
    }
    return *this;
}

// get a specified vertex property
Vertex &Graph::operator[](int vid) {
    return vl[vid];
}

Vertex &Graph::at(int vid) {
    return vl.at(vid);
}

Graph::Graph(unordered_map<int, vector<int> > &inlist, unordered_map<int, vector<int> > &outlist) {
    n_vertices = inlist.size();
    cout << "inlist size: " << inlist.size() << endl;
    cout << "outlist size: " << outlist.size() << endl;
    vl = VertexList(n_vertices);
    graph = GRA(n_vertices, In_OutList());
    for (int i = 0; i < n_vertices; i++)
        addVertex(i);
    cout << "inlist size: " << inlist.size() << endl;
    cout << "outlist size: " << outlist.size() << endl;
    unordered_map<int, vector<int> >::iterator hit, hit1;
    unordered_map<int, int> indexmap;
    vector<int> vec;
    vector<int>::iterator vit;
    int k;
    for (hit = inlist.begin(), k = 0; hit != inlist.end(); hit++, k++) {
        indexmap[hit->first] = k;
    }
    cout << "k: " << k << endl;
    for (hit = inlist.begin(), hit1 = outlist.begin(), k = 0; hit != inlist.end(); hit++, hit1++, k++) {
        vec = hit->second;
        for (vit = vec.begin(); vit != vec.end(); vit++)
            graph[k].inList.push_back(indexmap[*vit]);
        vec = hit1->second;
        for (vit = vec.begin(); vit != vec.end(); vit++)
            graph[k].outList.push_back(indexmap[*vit]);
    }
}

void Graph::extract(unordered_map<int, vector<int> > &inlist, unordered_map<int, vector<int> > &outlist) {
    for (int i = 0; i < vl.size(); i++) {
        inlist[i] = graph[i].inList;
        outlist[i] = graph[i].outList;
    }
//	printMap(inlist,outlist);
}

// for test
void Graph::printMap(unordered_map<int, vector<int> > &inlist, unordered_map<int, vector<int> > &outlist) {
    cout << "==============================================" << endl;
    unordered_map<int, vector<int> >::iterator hit;
    vector<int>::iterator vit;
    for (hit = outlist.begin(); hit != outlist.end(); hit++) {
        cout << hit->first << ": ";
        vector<int> vec = hit->second;
        for (vit = vec.begin(); vit != vec.end(); vit++)
            cout << *vit << " ";
        cout << "#" << endl;
    }
    cout << "In List for graph" << endl;
    for (hit = inlist.begin(); hit != inlist.end(); hit++) {
        cout << hit->first << ": ";
        vector<int> vec = hit->second;
        for (vit = vec.begin(); vit != vec.end(); vit++)
            cout << *vit << " ";
        cout << "#" << endl;
    }
    cout << "================================================" << endl;
}

void Graph::print_edges() {
    cout << "----Current Edge sets: ----" << endl;
    EdgeList el;
    for (int i = 0; i < num_vertices(); i++) {
        el = graph[i].outList;
        for (const auto &e_it:el) {
            cout << i << "->" << e_it << endl;
        }
    }
    cout << "---------------------------" << endl;
}

double Graph::tcs(const int vid) {
    return vl[vid].tcs;
}

void Graph::sortEdges() {
    GRA::iterator git;
    for (git = graph.begin(); git != graph.end(); git++) {
        sort(git->inList.begin(), git->inList.end());
        sort(git->outList.begin(), git->outList.end());
    }
}

vector<string> &Graph::split(const string &s, char delim, vector<string> &elems) {
    stringstream ss(s);
    string item;
    while (getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

vector<string> Graph::split(const string &s, char delim) {
    vector<string> elems;
    return split(s, delim, elems);
}

void Graph::build_summary_edges() {
    std::set<std::pair<int, int>> WorkList;
    std::map<int, std::set<int>> PathEdge;
    auto propagate = [&PathEdge, &WorkList](int s, int t) {
        if (!PathEdge[s].count(t)) {
            PathEdge[s].insert(t);
            WorkList.emplace(s, t);
        }
    };

    std::set<int> actualOut, formalin, formalout;
    for (auto &it : neg_label_map) {
        auto &e = it.first;
        PathEdge[e.first].insert(e.first);
        WorkList.emplace(e.first, e.first);
        actualOut.insert(e.second);
        formalout.insert(e.first);
    }
    for (auto &it : pos_label_map) {
        auto &e = it.first;
        formalin.insert(e.second);
    }

    while (!WorkList.empty()) {
        auto it = WorkList.begin();
        auto e = *it;// v->w
        WorkList.erase(it);
        int v = e.first;
        int w = e.second;

        if (actualOut.count(v)) {
            for (auto x : summary_edges[v]) {
                propagate(x, w);
            }

            for (auto x : graph[v].inList) {
                if (!pos_label_map.count({x, v}) && !neg_label_map.count({x, v})) {
                    propagate(x, w);
                }
            }
        } else if (formalin.count(v)) {
            if (formalout.count(w)) {
                for (int vin : graph[v].inList) {
                    auto xit = pos_label_map.find({vin, v});
                    if (xit == pos_label_map.end())
                        continue;
                    int x = xit->first.first;
                    int label = xit->second;
                    for (int wout : graph[w].outList) {
                        auto yit = neg_label_map.find({w, wout});
                        if (yit == neg_label_map.end())
                            continue;
                        int y = yit->first.second;
                        int label2 = yit->second;
                        if (label > 0 && label + label2 == 0) {
                            if (!hasEdge(x, y)) {
                                summary_edges[y].insert(x);
                            }
                            for (auto a : PathEdge[y]) {
                                propagate(x, a);
                            }
                        }
                    }
                }
            }
        } else {
            // default
            for (auto x : graph[v].inList) {
                if (!pos_label_map.count({x, v}) && !neg_label_map.count({x, v})) {
                    propagate(x, w);
                }
            }
        }
    }
}

void Graph::to_indexing_graph() {
    // add all summary edges to the graph
    add_summary_edges();

    // copy
    n_vertices = n_vertices * 2;
    vl.resize(n_vertices);
    graph.resize(n_vertices);

    for (int i = num_vertices() / 2; i < num_vertices(); ++i) {
        addVertex(i);
    }
    for (int i = num_vertices() / 2; i < num_vertices(); ++i) {
        int orig_i = i - n_vertices / 2;
        for (int t : out_edges(orig_i)) {
            addEdge(i, t + n_vertices / 2);
        }
        addEdge(orig_i, i);
    }

    for (auto &it : pos_label_map) {
        auto &pos_e = it.first;
        removeEdge(pos_e.first, pos_e.second);
    }
    for (auto &it : neg_label_map) {
        auto &neg_e = it.first;
        removeEdge(neg_e.first + n_vertices / 2, neg_e.second + n_vertices / 2);
    }

    // remove all self-cycles
    for (int i = 0; i < n_vertices; ++i) {
        removeEdge(i, i);
    }
}

void Graph::removeEdge(int s, int t) {
    // update edge list
    auto &inList = graph[t].inList;
    for (int i = 0; i < inList.size(); ++i) {
        if (inList[i] == s) {
            inList[i] = inList.back();
            inList.pop_back();
            --i;
        }
    }

    auto &outList = graph[s].outList;
    for (int i = 0; i < outList.size(); ++i) {
        if (outList[i] == t) {
            outList[i] = outList.back();
            outList.pop_back();
            --i;
            --n_edges;
        }
    }
}

void Graph::check() {
    cout << "Checking correctness of the input graph..." << endl;
    CSProgressBar bar(n_vertices);
    for (int i = 0; i < n_vertices; ++i) {
        Vertex& v = at(i);
        auto &inList = graph[i].inList;
        for (auto t : inList) {
            Vertex& u = at(t);
            if (label(t , i) == 0 && v.func_id != u.func_id) {
                cerr << "invalid graph where a labeled edge is cross funcs" << endl;
                cerr << t << " -> " << i << "\n";
                exit(8);
            }
        }

        auto &outList = graph[i].outList;
        for (auto t : outList) {
            Vertex& u = at(t);
            if (label(i , t) == 0 && v.func_id != u.func_id) {
                cerr << "invalid graph where a labeled edge is cross funcs" << endl;
                cerr << i << " -> " << t << "\n";
                exit(18);
            }
        }
        bar.update();
    }
    cout << endl;

    std::map<int, std::set<int>> func_arg_map;
    CSProgressBar bar2(pos_label_map.size());
    for (auto pit : pos_label_map) {
        int formalin = pit.first.second;
        auto& vertex = this->at(formalin);
        func_arg_map[vertex.func_id].insert(formalin);

        auto &inList = graph[formalin].inList;
        for (auto actualin : inList) {
            if (!pos_label_map.count(std::make_pair(actualin, formalin))) {
                cerr << "actualin -> formalin does not have a positive label" << endl;
                cerr << actualin << " -> " << formalin << "\n";
                exit(28);
            }
        }
        bar2.update();
    }

    int max_arg = -1;
    int max_arg_func = -1;
    for (auto &it : func_arg_map) {
        int arg_size = it.second.size();
        if (arg_size > max_arg) {
            max_arg_func = it.first;
            max_arg = it.second.size();
        }
    }
    cout << endl;
    cout << "# max arg " << max_arg << " in func " << max_arg_func << endl;
    cout << "Checking done!" << endl;
}

size_t Graph::summary_edge_size() {
    size_t ret = 0;
    for (auto &it : summary_edges) {
        ret += it.second.size();
    }
    return ret;
}

int Graph::label(int s, int t) {
    auto it = pos_label_map.find({s, t});
    if (it != pos_label_map.end()) {
        return it->second;
    }
    it = neg_label_map.find({s, t});
    if (it != neg_label_map.end()) {
        return it->second;
    }
    return 0;
}

void Graph::add_summary_edges() {
    for (auto &sit : summary_edges) {
        auto t = sit.first;
        for (auto s : sit.second) {
            addEdge(s, t);
        }
    }
}
