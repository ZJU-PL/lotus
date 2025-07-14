#ifndef _DWGRAPH_H
#define _DWGRAPH_H

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <list>
#include <algorithm>
#include <utility>
#include <cmath>
#include <unordered_map>

using namespace std;

#ifndef MAX_VAL
#define MAX_VAL 15000000
#endif
#ifndef MIN_VAL
#define MIN_VAL -15000000
#endif

struct DWVertex {
	int id;
	bool visited;
	int pre_order;	// for path tree labeling
	int post_order;
};

struct DWEdgeProp {
	int src;
	int trg;
	int weight;	// weight of edge
};

struct DWVertexProp {
	int id;
	int weight;
	int edgeid;
};

typedef list<int> DWEdgeList;	// edge list represented by edge id list
typedef unordered_map<int, DWVertex> DWVertexList;	// vertices list (store real vertex property) indexing by id
typedef unordered_map<int, DWEdgeProp> DWEdgeOpMap;
typedef pair<int, DWVertexProp> Edge;
struct ltEdge {
	bool operator()(const Edge& e1, const Edge& e2) const {
		if (e1.first < e2.first)
			return true;
		if (e1.first == e2.first && e1.second.id < e2.second.id)
			return true;
		if (e1.first == e2.first && e1.second.id == e2.second.id 
			&& e1.second.weight < e2.second.weight)
			return true;
		if (e1.first == e2.first && e1.second.id == e2.second.id 
			&& e1.second.weight == e2.second.weight
			&& e1.second.edgeid < e2.second.edgeid)
			return true;
		return false;
	}
};
typedef map<Edge, Edge, ltEdge> EdgeMap;

struct DWIn_OutList {
	DWEdgeList inList;
	DWEdgeList outList;
};
typedef map<int, DWIn_OutList> DWGRA;	// index graph

class DWGraph {
	public:
		DWGRA graph;
		DWVertexList vl;
		DWEdgeOpMap edgeOpMap;

		int maxEdgeId;

		void readGraph(istream&);
		void readGraph1(istream&);
		void writeGraph(ostream&);
		
	public:
		DWGraph();
		DWGraph(DWGRA&, DWVertexList&);
		DWGraph(istream&);
		~DWGraph();
		void printGraph();
		void addVertex(int);
		void removeVertex(int);
		void removeEdge(int, int);
		void removeEdgeWithID(int, int, int);
		void removeEdgeWithWeight(int, int, int);
		void addEdge(int, int, int, int);
		void addEdgeWithWeight(int, int, int);
		void updateEdge(int,int,int);
		int num_vertices();
		int num_edges();
		DWVertexList& vertices();
		DWEdgeList& out_edges(int);
		DWEdgeList& in_edges(int);
		int out_degree(int);
		int in_degree(int);
		int weight(int, int);	// for edmonds' algorithm
		int edgeId(int, int);	// for edmonds' algorithm
		DWVertexProp edge(int, int);	// for edmonds' algorithm
		void removeVertexfromVL(int);	// for edmonds' algorithm
		int maxid();
		set<int> getRoots();
		bool hasVertex(int);
		bool hasEdge(int, int);
		bool hasEdgeWithID(int, int, int);	
		DWGraph& operator=(const DWGraph&);
		DWVertex& operator[](const int);
		
		void clear();
		void strTrim(string& str);
		void toGDL(ostream& out);
		
		// Dec 15
		// avoid direct operation on edgeOpMap
		int weight(int eid);
		int source(int eid);
		int target(int eid);
		
		// remove edges with id
		void removeEdge(int eid);
};	

#endif
