#include "CFL/CSIndex/DWGraphUtil.h"

// implement tarjan's algorithm to find Strongly Connected Component from a given start node
void DWGraphUtil::tarjan(DWGraph& g, int vid, int& index, map< int, pair<int,int> >& order, 
	vector<int>& sn, map<int, vector<int> >& sccmap, int& scc) {
	order[vid].first = index;
	order[vid].second = index;
	index++;
	sn.push_back(vid);
	g.vl[vid].visited = true;
	DWEdgeList el = g.out_edges(vid);
	DWEdgeList::iterator eit;
	for (eit = el.begin(); eit != el.end(); eit++) {
		if (!g.vl[g.target(*eit)].visited) {
			tarjan(g, g.target(*eit), index, order, sn, sccmap, scc);
			order[vid].second = min(order[g.target(*eit)].second, order[vid].second);
		}
		else if (find(sn.begin(), sn.end(), g.target(*eit)) != sn.end()) {	
			order[vid].second = min(order[g.target(*eit)].first, order[vid].second);
		}
	}

	vector<int>::reverse_iterator rit;
	if (order[vid].first == order[vid].second) {
		vector<int> vec;
		for (rit = sn.rbegin(); rit != sn.rend(); rit++) {
			if ((*rit) != vid) {
				vec.push_back(*rit);
				sn.pop_back();
			}
			else {
				vec.push_back(*rit);
				sn.pop_back();
				break;
			}
		}
		sccmap.insert(make_pair(scc, vec));
		scc++;
	}
}


void DWGraphUtil::findMaxBranching1(DWGraph& g, DWGraph& maxBranch) {
	DWVertexList::iterator vlit;
	DWVertexList vl = g.vertices();
	DWEdgeList el;
	DWEdgeList::iterator eit, eit1;

	// for test
//	cout << "read graph" << endl;
//	g.printGraph();
	static int branch_depth = 0;
	branch_depth++;

	int maxw = MIN_VAL;
	DWVertexProp maxe;
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		maxw = MIN_VAL;
		el = g.in_edges(vlit->first);
		for (eit = el.begin(); eit != el.end(); eit++) {
			if (g.edgeOpMap[*eit].weight > maxw) {
				maxw = g.edgeOpMap[*eit].weight;
				maxe.id = g.edgeOpMap[*eit].src;
				maxe.edgeid = *eit;
				maxe.weight = maxw;		
			}
		}
		if (maxw != MIN_VAL) {
			maxBranch.addEdge(maxe.id, vlit->first, maxw, maxe.edgeid);
		}
		else
			maxBranch.addVertex(vlit->first);
	}

	// for test
/*
	cout << "max branch" << endl;
	maxBranch.printGraph();
*/
	// find strongly connected component
	vector<int> list_s;
	int index = 0;
	vector<int> sn;
	vector<int>::iterator vit;
	vector<int>::reverse_iterator rvit;
	map< int, pair<int, int> > order;
	map<int, vector<int> > sccmap;
	int scc_num = 0;
	int vid;
	vl = maxBranch.vertices();
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		vid = vlit->first;
		if (maxBranch.vl[vid].visited) continue;
		tarjan(maxBranch, vid, index, order, sn, sccmap, scc_num);
	}	

//	cout << "scc_num = " << scc_num;

	if (scc_num == maxBranch.num_vertices())
		return;

	map<int, vector<int> >::iterator mit = sccmap.begin();
	int num_comp;
	for (mit = sccmap.begin(); mit != sccmap.end(); mit++) {
		num_comp = mit->first;
	
	//	cout << "scc " << num_comp << " size=" << mit->second.size() << endl;

		if (mit->second.size() == 1)
			continue;

		vector<int> sccvec = mit->second;
		// find min weight edge in sccvec
		int minw = MAX_VAL;
		int weight, edgeid, src, trg;
		for (rvit = sccvec.rbegin(); rvit != sccvec.rend(); rvit++) {
			if ((rvit+1) != sccvec.rend()) {
				weight = maxBranch.weight(*rvit, *(rvit+1));
				edgeid = maxBranch.edgeId(*rvit, *(rvit+1));
				src = *rvit;
				trg = *(rvit+1);
			}
			else {
				weight = maxBranch.weight(*rvit, sccvec.back());
				edgeid = maxBranch.edgeId(*rvit, sccvec.back());
				src = *rvit;
				trg = sccvec.back();
			}
			if (weight < minw) {
				minw = weight;
				maxBranch.removeEdgeWithID(src,trg,edgeid);
			}
		}
	}
}


/*
* Dec 15: avoid direct operation on edgeOpMap
*
*/
// Edmonds' Branching Algorithm
void DWGraphUtil::findMaxBranching(DWGraph& g, DWGraph& maxBranch) {
	DWVertexList::iterator vlit;
	DWVertexList vl = g.vertices();
	DWEdgeList el;
	DWEdgeList::iterator eit, eit1;

	static int branch_depth = 0;
	branch_depth++;

	int maxw = MIN_VAL;
	DWVertexProp maxe;
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		maxw = MIN_VAL;
		el = g.in_edges(vlit->first);
		for (eit = el.begin(); eit != el.end(); eit++) {
		/*
			if (g.edgeOpMap[*eit].weight > maxw) {
				maxw = g.edgeOpMap[*eit].weight;
				maxe.id = g.edgeOpMap[*eit].src;
				maxe.edgeid = *eit;
				maxe.weight = maxw;		
			}
		*/
			if (g.weight(*eit) > maxw) {
				maxw = g.weight(*eit);
				maxe.id = g.source(*eit);
				maxe.edgeid = *eit;
				maxe.weight = maxw;	
			}
		}
		if (maxw != MIN_VAL) {
			maxBranch.addEdge(maxe.id, vlit->first, maxw, maxe.edgeid);
		}
		else
			maxBranch.addVertex(vlit->first);
	}

	// for test
	/*
	cout << "max branch" << endl;
	maxBranch.printGraph();
	*/

	// find strongly connected component
	vector<int> list_s;
	int index = 0;
	vector<int> sn;
	vector<int>::iterator vit;
	vector<int>::reverse_iterator rvit;
	map< int, pair<int, int> > order;
	map<int, vector<int> > sccmap;
	int scc_num = 0;
	int vid;
	vl = maxBranch.vertices();
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		vid = vlit->first;
		if (maxBranch.vl[vid].visited) continue;
		tarjan(maxBranch, vid, index, order, sn, sccmap, scc_num);
	}	
	
	if (scc_num == maxBranch.num_vertices())
		return;

	DWGraph ng = g;
	
	map<int, vector<int> >::iterator mit = sccmap.begin();
	int num_comp;
	int maxid = ng.maxid();
	EdgeMap edgemap;
	map<int, vector<int> > newVertex;
	map<int, int> minWeight; 
	map<int, DWVertexProp> sccPropMap;	// keep edge info in strongly connceted component, <src, trgEdgeProp>
	DWEdgeList inedge, outedge;

	for (mit = sccmap.begin(); mit != sccmap.end(); mit++) {
		num_comp = mit->first;
	#ifdef DEBUG
		cout << "scc " << num_comp << " size=" << mit->second.size() << endl;
	#endif

		if (mit->second.size() == 1)
			continue;
			
			
		// for test
	#ifdef DEBUG
		cout << "Before next scc operation " << endl;
		ng.printGraph();
		char ch1;
	//	cin >> ch1;
	#endif

		// add new vertex
		maxid++;	
		ng.addVertex(maxid);
		list_s.push_back(maxid);
		newVertex.insert(make_pair(maxid, mit->second));
		vector<int> &sccvec = mit->second;

		// find min weight edge in sccvec
		int minw = MAX_VAL;
		int weight, edgeid;
		for (rvit = sccvec.rbegin(); rvit != sccvec.rend(); rvit++) {
			
			DWVertexProp prop;

			if ((rvit+1) != sccvec.rend()) {
				// Dec 15
				// get weight based on edgeid to keep consistent
				edgeid = maxBranch.edgeId(*rvit, *(rvit+1));
			//	weight = maxBranch.weight(*rvit, *(rvit+1));
				weight = maxBranch.weight(edgeid);
				prop.id = *(rvit+1);
				prop.weight = weight;
				prop.edgeid = edgeid;
				sccPropMap.insert(make_pair(*rvit, prop));
			}
			else {
			//	weight = maxBranch.weight(*rvit, sccvec.back());
				edgeid = maxBranch.edgeId(*rvit, sccvec.back());
				weight = maxBranch.weight(edgeid);
				prop.id = sccvec.back();
				prop.weight = weight;
				prop.edgeid = edgeid;
				sccPropMap.insert(std::make_pair(*rvit, prop));
			}
			if (weight < minw)
				minw = weight;
		}
		minWeight[maxid] = minw;
		
		// for test
	#ifdef DEBUG
		cout << endl;		
	#endif

		// Dec 15
		// update graph g to ng while keep edge mapping using grpah g
		for (vit = sccvec.begin(); vit != sccvec.end(); vit++) {
			// process out edges
			el = ng.out_edges(*vit);
			for (eit = el.begin(); eit != el.end(); eit++) {
			//	if (find(sccvec.begin(), sccvec.end(), g.target(*eit)) != sccvec.end()) {
				if (find(sccvec.begin(), sccvec.end(), ng.target(*eit)) != sccvec.end()) {
					// Dec 15
					// remove edges among scc
					ng.removeEdge(*eit);
					continue; 
				}
			//	ng.addEdge(maxid, g.target(*eit), g.edgeOpMap[*eit].weight, *eit);
			//	ng.addEdge(maxid, g.target(*eit), g.weight(*eit), *eit);
				ng.addEdge(maxid, ng.target(*eit), ng.weight(*eit), *eit);
				DWVertexProp newnode;
				
				// Dec 16
				// change g.target(*eit) to ng.target(*eit)
			//	newnode.id = g.target(*eit);
				newnode.id = ng.target(*eit);
			//	newnode.weight = g.edgeOpMap[*eit].weight;
			//	newnode.weight = g.weight(*eit);
				newnode.weight = ng.weight(*eit);
				newnode.edgeid = *eit;
				edgemap.insert(make_pair(make_pair(maxid, newnode), make_pair(*vit, newnode)));
			}
			
			// for test
		#ifdef DEBUG	
			cout << "after process outedges of " << *vit << endl;
			ng.printGraph();
			char ch;
		//	cin >> ch;
		#endif	
			
			// find edge whose target is vit
			inedge = maxBranch.in_edges(*vit);
		//	int ce = g.edgeOpMap[*(inedge.begin())].weight;
			int ce = g.weight(inedge.front());
			// process in edges;
			el = ng.in_edges(*vit);
			for (eit = el.begin(); eit != el.end(); eit++) {
			//	if (find(sccvec.begin(), sccvec.end(), g.source(*eit)) != sccvec.end()) {
				if (find(sccvec.begin(), sccvec.end(), ng.source(*eit)) != sccvec.end()) {
					// Dec 15
					// remove edges among scc
					ng.removeEdge(*eit);
					continue;
				}
				// update weight
			//	int y1 = ng.edgeOpMap[*eit].weight - ce + minw;
			//	ng.addEdge(ng.edgeOpMap[*eit].trg, maxid, y1, *eit);
				int y1 = ng.weight(*eit) - ce + minw;
				DWVertexProp src, trg;
				src.id = maxid;
				src.weight = y1;
				src.edgeid = *eit;
				trg.id = *vit;
			//	trg.weight = ng.edgeOpMap[*eit].weight;
				trg.weight = ng.weight(*eit);
				trg.edgeid = *eit;
				
				// Dec 15
				// change ng.target(*eit) to ng.source(*eit)
			//	edgemap.insert(make_pair(make_pair(ng.edgeOpMap[*eit].trg, src),make_pair(ng.edgeOpMap[*eit].trg, trg)));
			//	edgemap.insert(make_pair(make_pair(ng.target(*eit), src),make_pair(ng.target(*eit), trg)));
				// Dec 15
				// change ng.source(*eit) to g.source(*eit) such that mapping the original edges to new graph ng
			//	edgemap.insert(make_pair(make_pair(ng.source(*eit), src),make_pair(ng.source(*eit), trg)));
			
				// Dec 16
				// change g.source(*eit) to ng.source(*eit)
			//	edgemap.insert(make_pair(make_pair(g.source(*eit), src),make_pair(g.source(*eit), trg)));
				edgemap.insert(make_pair(make_pair(ng.source(*eit), src),make_pair(ng.source(*eit), trg)));
				
				// Dec 15
				// change ng.target(*eit) to ng.source(*eit); change the order of update edgemap and add edge operation
			//	ng.addEdge(ng.target(*eit), maxid, y1, *eit);
				ng.addEdge(ng.source(*eit),maxid,y1,*eit);
			}
			ng.removeVertexfromVL(*vit);
			
			// for test
		#ifdef DEBUG	
			cout << "after process inedges of " << *vit << endl;
			ng.printGraph();
		//	cin >> ch;
		#endif	
		}
	}
	
	//for test
	#ifdef DEBUG
	cout << "before return " << endl;
	ng.printGraph();
	#endif

	// find max branching recursively
	DWGraph newBranch;
	findMaxBranching(ng, newBranch);
	maxBranch = newBranch;

	// for test
	#ifdef DEBUG
	cout << "recursive " << endl;
	maxBranch.printGraph();
	#endif
	
	// cycle decomposition
	vector<int>::iterator vit1;
	vector<int>::reverse_iterator rvit1, rvit2;
	vector<int> vec1;
	int minsrc, mintrg;
	bool keep = false;
	for (rvit2 = list_s.rbegin(); rvit2 != list_s.rend(); rvit2++) {
		// for test
	#ifdef DEBUG
		cout << "open cycle " << *rvit2 << endl;
	#endif
	
		vec1 = newVertex[*rvit2];
		for (vit1 = vec1.begin(); vit1 != vec1.end(); vit1++) {
			maxBranch.addVertex(*vit1);	
			// for test
		#ifdef DEBUG
			cout << "add vertex " << *vit1 << endl;
		#endif
		}
		
		
		//update in edges and strongly connected component
		if (maxBranch.in_edges(*rvit2).size() == 0) {
			// for test
		#ifdef DEBUG
			cout << *rvit2 << " no inedges" << endl;
		#endif
		
			keep = false;
			for (rvit1 = vec1.rbegin(); rvit1 != vec1.rend(); rvit1++) {
				DWVertexProp dwep = sccPropMap[*rvit1];
				// for test
			#ifdef DEBUG
				cout << "edge " << *rvit1 << "->" << dwep.id << " edgeid " << dwep.edgeid << endl;
			#endif
				
				if (dwep.weight == minWeight[*rvit2] && !keep) {
					keep = true;
					continue;
				}
				
				// for test
			#ifdef DEBUG
				cout << "in add edge " << *rvit1 << "->" << dwep.id << " eid " << dwep.edgeid << endl;
			#endif
				maxBranch.addEdge(*rvit1, dwep.id, dwep.weight, dwep.edgeid);
			}
		}
		else {
			el = maxBranch.in_edges(*rvit2);
			// for test
		#ifdef DEBUG
			cout << "search edge " << maxBranch.source(el.front()) << "->" << *rvit2 << endl; 
		#endif
			
			// Dec 15
			// change g.target to maxBranch.source
		//	DWVertexProp dwep2 = maxBranch.edge(g.target(*(el.begin())), *rvit2);
		//	Edge edge = make_pair(g.target(*(el.begin())), dwep2);
			DWVertexProp dwep2 = maxBranch.edge(maxBranch.source(el.front()), *rvit2);
			Edge edge = make_pair(maxBranch.source(el.front()), dwep2);
			EdgeMap::iterator newedge = edgemap.find(edge);
			
			// for test
		#ifdef DEBUG
			if (newedge == edgemap.end()) {
				EdgeMap::iterator iter;
				cout << "edgemap list: " << endl;
				for (iter = edgemap.begin(); iter != edgemap.end(); iter++)
					cout << iter->first.first << "->" << iter->first.second.id << " eid " << iter->first.second.edgeid
						<< " mapping " << iter->second.first << "->" << iter->second.second.id << " eid " 
						<< iter->second.second.edgeid << endl;
				cout << "in search edge " << edge.first << "->" << edge.second.id << " eid " << edge.second.edgeid << endl;
				exit(0);
			}
		#endif
			
			DWVertexProp dwep1 = newedge->second.second;
			int src = newedge->second.first;
			maxBranch.addEdge(src, dwep1.id, dwep1.weight, dwep1.edgeid);
			keep = false;
			for (rvit1 = vec1.rbegin(); rvit1 != vec1.rend(); rvit1++) {
				DWVertexProp dwep = sccPropMap[*rvit1];
				
				// for test
			#ifdef DEBUG
				cout << "process edge " << *rvit1 << "->" << dwep.id << " eid " << dwep.edgeid << endl;
			#endif
				if (dwep.id == dwep1.id && !keep) { 
					keep = true;
					continue;
				}
				// for test
			#ifdef DEBUG
				cout << "add edge " << *rvit1 << "->" << dwep.id << " eid " << dwep.edgeid << endl;
			#endif
				maxBranch.addEdge(*rvit1, dwep.id, dwep.weight, dwep.edgeid);
			}
		}
	
		// update out edges 
		if (maxBranch.out_edges(*rvit2).size() > 0) {
			el = maxBranch.out_edges(*rvit2);
			for (eit = el.begin(); eit != el.end(); eit++) {
				DWVertexProp dwep;
			//	dwep.id = maxBranch.edgeOpMap[*eit].trg;
				dwep.id = maxBranch.target(*eit);
				dwep.edgeid = *eit;
			//	dwep.weight = maxBranch.edgeOpMap[*eit].weight;
				dwep.weight = maxBranch.weight(*eit);
				Edge edge = make_pair(*rvit2, dwep);	
				EdgeMap::iterator newedge = edgemap.find(edge);
				
				// for test
			#ifdef DEBUG
				if (newedge == edgemap.end()) {
					EdgeMap::iterator iter;
					cout << "edgemap list: " << endl;
					for (iter = edgemap.begin(); iter != edgemap.end(); iter++)
						cout << iter->first.first << "->" << iter->first.second.id << " eid " << iter->first.second.edgeid
							<< " mapping " << iter->second.first << "->" << iter->second.second.id << " eid " 
							<< iter->second.second.edgeid << endl;
					cout << "search edge " << edge.first << "->" << edge.second.id << " eid " << edge.second.edgeid << endl;
					exit(0);
				}
			#endif
				
				DWVertexProp dwep1 = newedge->second.second;
				int src = newedge->second.first;			
				maxBranch.addEdge(src, dwep1.id, dwep1.weight, dwep1.edgeid);
				
				// for test
			#ifdef DEBUG
				cout << "add out edge " << src << "->" << dwep1.id << " eid " << dwep1.edgeid << endl;
			#endif
			}
		}

		// Dec 15
		// change maxBranch.removeVertex to maxBranch.removeVertexfromVL
	//	maxBranch.removeVertex(*rvit2);
		maxBranch.removeVertexfromVL(*rvit2);
		
		// for test
	#ifdef DEBUG
		cout << "after open cycle " << *rvit2 << endl;
		maxBranch.printGraph();
	#endif
	}
	
	// for test
	#ifdef DEBUG
	cout << "after unfold scc" << endl;
	maxBranch.printGraph();
	char ch2;
//	cin >> ch2;
	#endif
}

bool DWGraphUtil::checkBranching(DWGraph& graph, DWGraph& branch) {
	findMaxBranching(graph, branch);
	int gsize = graph.num_vertices();
	DWVertexList bvl = branch.vertices();
	DWVertexList::iterator vlit;
	DWEdgeList::iterator eit;
	int weight = MAX_VAL;
	int src = -1, trg = -1;
	for (vlit = bvl.begin(); vlit != bvl.end(); vlit++) {
		if (vlit->first > gsize-1) {
			cout << "error: vertex " << vlit->first << " not existed!" << endl;
			return false;
		//	branch.removeVertex(vlit->first);
		}
		else if (branch.in_degree(vlit->first) >= 1) {
			src = -1;
			trg = -1;
			weight = MAX_VAL;
			for (eit = branch.in_edges(vlit->first).begin(); eit != branch.in_edges(vlit->first).end(); eit++) {
			/*
				if (branch.edgeOpMap[*eit].weight < weight) {
					src = branch.edgeOpMap[*eit].src;
					trg = branch.edgeOpMap[*eit].trg;
					weight = branch.edgeOpMap[*eit].weight;
				}
			*/
				if (graph.edgeOpMap.find(*eit) == graph.edgeOpMap.end() || graph.edgeOpMap[*eit].trg != vlit->first) {
					cout << "error: edge [" << branch.edgeOpMap[*eit].src << "-->>" << branch.edgeOpMap[*eit].trg << "] not existed!" << endl;
					return false;
				}
			}
			/*
			if (src != -1 && trg != -1)
				branch.removeEdge(src, trg);
			*/
		}
	}
	bool result = checkBranch(branch);
	if (!result) { cout << "error: More than one incoming edges!" << endl; return false; }
	int index = 0;
	vector<int> sn;
	vector<int>::iterator vit;
	vector<int>::reverse_iterator rvit;
	map< int, pair<int, int> > order;
	map<int, vector<int> > sccmap;
	int scc_num = 0;
	int vid;
	DWVertexList vl = branch.vertices();
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) 
		branch.vl[vlit->first].visited = false;
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		vid = vlit->first;
		if (branch.vl[vid].visited) continue;
		tarjan(branch, vid, index, order, sn, sccmap, scc_num);
	}
	
	int prev = -1;
	if (sccmap.size() != gsize) {
		cout << "scc_num = " << scc_num << endl;
		cout << "error: loop exists!" << endl;
		map<int, vector<int> >::iterator mit = sccmap.begin();
		int num_comp;
		for (mit = sccmap.begin(); mit != sccmap.end(); mit++) {
			num_comp = mit->first;
		
		//	cout << "scc " << num_comp << " size=" << mit->second.size() << endl;

			if (mit->second.size() == 1)
				continue;

			vector<int>& sccvec = mit->second;
		//	branch.removeEdge(sccvec[1],sccvec[0]);
			
			cout << "loop " << num_comp << ": ";
			for (rvit = sccvec.rbegin(); rvit != sccvec.rend(); rvit++) {
				cout << *rvit << ", ";
			}
			cout << endl;
			
		}
		return false;
	}
	return true;
}

bool DWGraphUtil::checkBranching1(DWGraph& graph, DWGraph& branch) {
	findMaxBranching1(graph, branch);
	int gsize = graph.num_vertices();
	DWVertexList bvl = branch.vertices();
	DWVertexList::iterator vlit;
	DWEdgeList::iterator eit;
	int weight = MAX_VAL;
	int src = -1, trg = -1;
	for (vlit = bvl.begin(); vlit != bvl.end(); vlit++) {
		if (vlit->first > gsize-1) {
			cout << "error: vertex " << vlit->first << " not existed!" << endl;
			return false;
		//	branch.removeVertex(vlit->first);
		}
		else if (branch.in_degree(vlit->first) >= 1) {
			src = -1;
			trg = -1;
			weight = MAX_VAL;
			for (eit = branch.in_edges(vlit->first).begin(); eit != branch.in_edges(vlit->first).end(); eit++) {
			/*
				if (branch.edgeOpMap[*eit].weight < weight) {
					src = branch.edgeOpMap[*eit].src;
					trg = branch.edgeOpMap[*eit].trg;
					weight = branch.edgeOpMap[*eit].weight;
				}
			*/
				if (graph.edgeOpMap.find(*eit) == graph.edgeOpMap.end() || graph.edgeOpMap[*eit].trg != vlit->first) {
					cout << "error: edge [" << branch.edgeOpMap[*eit].src << "-->>" << branch.edgeOpMap[*eit].trg << "] not existed!" << endl;
					return false;
				}
			}
			/*
			if (src != -1 && trg != -1)
				branch.removeEdge(src, trg);
			*/
		}
	}
	bool result = checkBranch(branch);
	if (!result) { cout << "error: More than one incoming edges!" << endl; return false; }
	int index = 0;
	vector<int> sn;
	vector<int>::iterator vit;
	vector<int>::reverse_iterator rvit;
	map< int, pair<int, int> > order;
	map<int, vector<int> > sccmap;
	int scc_num = 0;
	int vid;
	DWVertexList vl = branch.vertices();
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) 
		branch.vl[vlit->first].visited = false;
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		vid = vlit->first;
		if (branch.vl[vid].visited) continue;
		tarjan(branch, vid, index, order, sn, sccmap, scc_num);
	}
	
	int prev = -1;
	if (sccmap.size() != gsize) {
		cout << "scc_num = " << scc_num << endl;
		cout << "error: loop exists!" << endl;
		map<int, vector<int> >::iterator mit = sccmap.begin();
		int num_comp;
		for (mit = sccmap.begin(); mit != sccmap.end(); mit++) {
			num_comp = mit->first;
		
		//	cout << "scc " << num_comp << " size=" << mit->second.size() << endl;

			if (mit->second.size() == 1)
				continue;

			vector<int>& sccvec = mit->second;
		//	branch.removeEdge(sccvec[1],sccvec[0]);
			
			cout << "loop " << num_comp << ": ";
			for (rvit = sccvec.rbegin(); rvit != sccvec.rend(); rvit++) {
				cout << *rvit << ", ";
			}
			cout << endl;
			
		}
		return false;
	}
	return true;
}

bool DWGraphUtil::checkBranch(DWGraph branch) {
	DWVertexList vl = branch.vertices();
	DWVertexList::iterator vlit;
	DWEdgeList el;
	DWEdgeList::iterator eit;
	for (vlit = vl.begin(); vlit != vl.end(); vlit++) {
		if (branch.in_degree(vlit->first) > 1) {
			el = branch.in_edges(vlit->first);
			cout << "Max Branch Wrong" << endl;
			return false;
		}
	}
	return true;
}

// for test
void DWGraphUtil::genRandomGraph(int n, double c, const char* filename) {
	int threshold = (int)(c);
	DWGraph g;
	int i, j;
	int rand_num;
	for (i = 0; i < n; i++) 
		g.addVertex(i);

	map<pair<int,int>, int> checkEdge;
	int weight;
	int eid = 0;
	srand(time(NULL));
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			if (i != j) {
				rand_num = rand()%n;
				if (rand_num < threshold) {
					if (checkEdge.find(pair<int,int>(i,j)) == checkEdge.end()) {
						weight = rand()%n;
						g.addEdge(i,j,weight,eid);
						checkEdge.insert(make_pair(pair<int,int>(i,j), 0));
						eid++;
					}
				}
			}
		}
	}

	ofstream out(filename);
	g.writeGraph(out);
	out.close();

	// write in GDL format
/*
	string fname(filename);
	fname = fname.substr(0, fname.find_last_of("."));
	fname += ".gdl";
	ofstream og(fname.c_str());
	g.toGDL(og);
	og.close();
*/
}

