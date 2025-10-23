#include "CFL/Graspan/edgecomp/edgemerger.h"

// PUBLIC
EdgeMerger::EdgeMerger()
{
	deltaPtr = -1;
	oUnUdPtr = -1;
	currID = -1;
}

/**
 * given the vectors and vals to merge, as well as the ID of the source edges,
 * merge edges into a new list
 */
void EdgeMerger::mergeVectors(vector< vector<int> > &edgeVecsToMerge,
		vector< vector<char> > &valVecsToMerge,  vector<int> &srcDeltaEdges,
		vector<char> &srcDeltaVals, vector<int> &srcoUnUdEdges,
		vector<char> &srcoUnUdVals, int srcID)
{
	MinSet srcMS;				// initialize a MinSet for the source vertex
	srcMS.setMinSetID(0);
	updateMinSet(srcMS, edgeVecsToMerge[srcID], valVecsToMerge[srcID]);		// initialize the info of the source MinSet
	
	fillPriorityQueue(edgeVecsToMerge, valVecsToMerge, srcDeltaEdges, srcDeltaVals, srcoUnUdEdges, srcoUnUdVals, srcID);
	
    MinSet tgt;
    int max = std::numeric_limits<int>::max();
    while (1)
    {
        if (!minEdges.empty()) {
            tgt = minEdges.top();		// get reference to the minset with the smallest vertex value
            minEdges.pop();
        }

        if (srcMS.getCurrVID() == max && tgt.getCurrVID() == max) {
            break;
        }

        int indOfTgt = tgt.getMinSetID();
        processMinSets(srcMS, tgt, edgeVecsToMerge[srcID], valVecsToMerge[srcID], edgeVecsToMerge[indOfTgt], valVecsToMerge[indOfTgt], srcDeltaEdges, srcDeltaVals, srcoUnUdEdges, srcoUnUdVals);
    }

    removeExtraSpace(srcDeltaEdges, srcDeltaVals, srcoUnUdEdges, srcoUnUdVals);
}


// PRIVATE
/**
 * fill the minEdges priority_queue with the lowest value in each vector. Find the total number of new edges and reserve
 * space in the vectors accordingly
 */
void EdgeMerger::fillPriorityQueue(vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
		vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals, vector<int> &srcoUnUdEdges, vector<char> &srcoUnUdVals,
		int srcID)
{
	MinSet newminset;
	int totTgtRowSize = 0;
    for (int i = 1; i < edgeVecsToMerge.size(); i++)
    {
		newminset.setMinSetID(i);
		updateMinSet(newminset, edgeVecsToMerge[i], valVecsToMerge[i]);

		totTgtRowSize += edgeVecsToMerge[i].size();
		minEdges.push(newminset);
		newminset.resetPtr();
    }

    srcDeltaEdges.reserve(totTgtRowSize);
    srcDeltaVals.reserve(totTgtRowSize);

    srcoUnUdEdges.reserve(edgeVecsToMerge[srcID].size() + totTgtRowSize);
    srcoUnUdVals.reserve(edgeVecsToMerge[srcID].size() + totTgtRowSize);
}

bool EdgeMerger::find_val(vector<char> &evals, char val)
{
	return std::find(evals.begin(), evals.end(), val) == evals.end();
}

/**
 * remove the excess space created when duplicate values were removed
 */
void EdgeMerger::removeExtraSpace(vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals, vector<int> &srcoUnUdEdges, vector<char> &srcoUnUdVals)
{
	// TODO: this happens no matter what right now. Use a conditional statement to stop this from
	// happening every time and avoid excessive copying!
	srcoUnUdEdges = vector<int>(srcoUnUdEdges.begin(), srcoUnUdEdges.begin() + oUnUdPtr + 1);
	srcoUnUdVals = vector<char>(srcoUnUdVals.begin(), srcoUnUdVals.begin() + oUnUdPtr + 1);

	srcDeltaEdges = vector<int>(srcDeltaEdges.begin(), srcDeltaEdges.begin() + deltaPtr + 1);
	srcDeltaVals = vector<char>(srcDeltaVals.begin(), srcDeltaVals.begin() + deltaPtr + 1);
}

/**
 * for the given minset, find the next minimum value and any corresponding values
 */
void EdgeMerger::updateMinSet(MinSet &minset, vector<int> &edges, vector<char> &vals)
{
	minset.setCurrVID(std::numeric_limits<int>::max());		// set the VID as the max so all values will be smaller
	minset.clearEvalSet();		// because it is a new vertex, get rid of any values in the edge value set

	for (int i = minset.getPtr(); i < edges.size() && edges[i] <= minset.getCurrVID(); i++)		// find all edge values
	{																							// associated with a 
		minset.setCurrVID(edges[i]);															// specific edge value
		minset.addEval(vals[i]);																// and store them in the
		minset.incPtr();																		// minset
	}
}

/**
 * compare the next smallest target minset with the source minset to determine which vector to update
 */
void EdgeMerger::processMinSets(MinSet &srcMS, MinSet &tgtMS, vector<int> &srcEdgesToMerge,
		vector<char> &srcValsToMerge, vector<int> &tgtEdgesToMerge,
		vector<char> &tgtValsToMerge, vector<int> &srcDeltaEdges, vector<char> &srcDeltaVals,
		vector<int> &srcoUnUdEdges, vector<char> &srcoUnUdVals)
{
	// case 1 - the target vertexID is smaller than the source vertexID
	if (srcMS.getCurrVID() > tgtMS.getCurrVID()) {
		if (currID != tgtMS.getCurrVID()) {
			currID = tgtMS.getCurrVID();
			currEvals.clear();
		}

        vector<char> &tgtVals = tgtMS.getEvals();
        for (vector<char>::iterator iter = tgtVals.begin(); iter != tgtVals.end(); iter++)
		{
            if (find_val(currEvals, *iter)) {
				updateVector(tgtMS.getCurrVID(), *iter, srcoUnUdEdges, srcoUnUdVals, oUnUdPtr);		// if not add to both oUnUd
				updateVector(tgtMS.getCurrVID(), *iter, srcDeltaEdges, srcDeltaVals, deltaPtr);		// and Delta
				currEvals.push_back(*iter);				// insert edge value into currEvals to mark as already used
            }
        }
        updateMinSet(tgtMS, tgtEdgesToMerge, tgtValsToMerge);
        minEdges.push(tgtMS);

        return;
    }

	// case 2 - target vertexID and source vertexID are equal
	if (srcMS.getCurrVID() == tgtMS.getCurrVID()) {
		if (currID != tgtMS.getCurrVID()) {
			currID = tgtMS.getCurrVID();
			currEvals.clear();
		}

        vector<char> &srcVals = srcMS.getEvals();
        vector<char> &tgtVals = tgtMS.getEvals();
        for (vector<char>::iterator iter = tgtVals.begin(); iter != tgtVals.end(); iter++)
		{
            if (find_val(srcVals, *iter)) {
                if (find_val(currEvals, *iter)) {
					updateVector(tgtMS.getCurrVID(), *iter, srcoUnUdEdges, srcoUnUdVals, oUnUdPtr);
					updateVector(tgtMS.getCurrVID(), *iter, srcDeltaEdges, srcDeltaVals, deltaPtr);
					currEvals.push_back(*iter);
                }
            }
        }
        updateMinSet(tgtMS, tgtEdgesToMerge, tgtValsToMerge);
        minEdges.push(tgtMS);

        return;
    }

	// case 3 - source vertexID is less than target vertexID
	if (srcMS.getCurrVID() < tgtMS.getCurrVID()) {
		if (currID != srcMS.getCurrVID()) {
			currID = srcMS.getCurrVID();
			currEvals.clear();
		}

        vector<char> &srcVals = srcMS.getEvals();
        for (vector<char>::iterator iter = srcVals.begin(); iter != srcVals.end(); iter++)
		{
            if (find_val(currEvals, *iter)) {
				updateVector(srcMS.getCurrVID(), *iter, srcoUnUdEdges, srcoUnUdVals, oUnUdPtr);		// only add to oUnUd
				currEvals.push_back(*iter);															// because not new
            }
        }
        updateMinSet(srcMS, srcEdgesToMerge, srcValsToMerge);
        minEdges.push(tgtMS);

        return;
    }
}

/**
 * increment the pointer, then add the vertexID and the edge value into the vector
 */
void EdgeMerger::updateVector(int vid, char val, vector<int> &edges, vector<char> &vals, int &ptr)
{
	ptr++;
	edges[ptr] = vid;
	vals[ptr] = val;
}
