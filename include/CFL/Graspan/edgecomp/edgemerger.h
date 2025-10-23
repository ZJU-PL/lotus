#ifndef EDGEMERGER_H
#define EDGEMERGER_H

#include <iostream>
#include <algorithm>
#include <ctime>
#include <limits>
#include <unordered_set>
#include <queue>

#include "CFL/Graspan/datastructures/minset.h"
#include "CFL/Graspan/utilities/timer.h"
#include "CFL/Graspan/utilities/globalDefinitions.hpp"


class EdgeMerger
{
	int deltaPtr;		// index into the vector of new edges
	int oUnUdPtr;		// index into the vector of ALL edges
	int currID;			// the current vertex ID (increasing order)

	vector<char> currEvals;		// the current edge values (to check for duplicates)
	std::priority_queue<MinSet, vector<MinSet>, compare> minEdges;		// finds the minset with the minimum current vertex ID


	// FUNCTIONS
	void fillPriorityQueue(vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
			vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals, vector<int> &srcoUnUdEdges, vector<char> &srcoUnUdVals,
			int srcID);

	bool find_val(vector<char> &evals, char val);

	void removeExtraSpace(vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals, vector<int> &srcoUnUdEdges, vector<char> &srcoUnUdVals);

	void updateMinSet(MinSet &minset, vector<int> &edges, vector<char> &vals);

	void processMinSets(MinSet &srcMS, MinSet &tgtMS, vector<int> &srcEdgesToMerge,
		vector<char> &srcValsToMerge, vector<int> &tgtEdgesToMerge,
		vector<char> &tgtValsToMerge, vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals,
		vector<int> &srcoUnUdEdges, vector<char> &srcoUnUdVals);

	void updateVector(int vid, char val, vector<int> &edges, vector<char> &vals, int &ptr);

	public:
	// constructor
	EdgeMerger();

	// getters
	inline int getNumNewEdges() { return deltaPtr + 1; }

	// given all new edges and their values, merge with the source creating a vector of old U new edges
	// and only new edges
	void mergeVectors(vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
		vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals, vector<int> &srcoUnUdEdges,
		vector<char> &srcoUnUdVals, int srcID);
};

#endif
