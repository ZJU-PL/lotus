#include "CFL/Graspan/edgecomp/compute.h"
#include "CFL/Graspan/utilities/timer.h"



// FUNCTION DEFINITIONS
long updateEdges(int vertInd, ComputationSet compsets[], LoadedVertexInterval intervals[],
		Context &context);

void getEdgesToMerge(ComputationSet *compSet, ComputationSet compsets[],
		LoadedVertexInterval intervals[], bool oldEdgesEmpty, bool newEdgesEmpty,
		vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
		int &rowMergeID, Context &context);

void genS_RuleEdges(vector<int> &newEdges, vector<char> &newVals,
		vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
		int &rowMergeID, Context &context);

void genD_RuleEdges(ComputationSet compsets[], LoadedVertexInterval intervals[], vector<int> &edges,
		vector<char> &vals, vector< vector<int> > &edgeVecsToMerge,
		vector< vector<char> > &valVecsTomerge, int &rowMergeID, Context &context, char flag);

void checkEdges(int dstInd, char dstVal, ComputationSet compsets[],
		vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge, int &rowMergeID,
		Context &context, char flag);

// PRINT EDGES FOR DEBUGGING
void print_edges(vector<vector<int>> &edgeVecsToMerge, vector<vector<char>> &valVecsToMerge)
{
	for (int i = 0; i < edgeVecsToMerge.size(); i++)
	{
		if (edgeVecsToMerge[i].size() == 0) cout << "VEC" << i << ": empty..." << endl;
		else {
			cout << "VEC" << i << ": ";
			for (int j = 0; j < edgeVecsToMerge[i].size(); j++)
				cout << "(" << edgeVecsToMerge[i][j] << ", " << (short)valVecsToMerge[i][j] << ")  ";

			cout << endl;
		}
	}
	cout << endl;
}


/**
 * given an index into the compsets, find all valid edges and merge them into a new list
 *
 * @param vertInd			-index of source vertex in compsets[]
 * @param compsets[]		-ComputationSet list for all in-memory vertices
 * @param intervals			-list (size=2) with information about the partitions
 * @param context			-Context object for checking grammar
 */
long updateEdges(int vertInd, ComputationSet compsets[], LoadedVertexInterval intervals[], Context &context)
{
	ComputationSet *compSet = &compsets[vertInd];

	bool oldEdgesEmpty = (compSet->getOldEdges().empty()) ? true : false;
	bool newEdgesEmpty = (compSet->getNewEdges().empty()) ? true : false;

	if (oldEdgesEmpty && newEdgesEmpty) return 0;

	// ESTIMATE the number of vectors we will need to merge
	vector< vector<double> > &ddm = context.ddm.getDdmMap();
	int numRowsToMerge = 8;		// place holder because no better heuristic
	vector< vector<int> > edgeVecsToMerge(numRowsToMerge);
	vector< vector<char> > valVecsToMerge(numRowsToMerge);

	// set edgeVecsToMerge[0] equal to the current vertex's outgoing edges
	int rowMergeID = 0;
	edgeVecsToMerge[rowMergeID] = compSet->getoldUnewEdges();
	valVecsToMerge[rowMergeID++] = compSet->getoldUnewVals();

	// find new edges
	getEdgesToMerge(compSet, compsets, intervals, oldEdgesEmpty, newEdgesEmpty, edgeVecsToMerge, valVecsToMerge, rowMergeID, context);

	EdgeMerger em;

	vector<int> &srcDeltaEdges = compSet->getDeltaEdges();
	vector<char> &srcDeltaVals = compSet->getDeltaVals();

	vector<int> &srcoUnUdEdges = compSet->getoUnUdEdges();
	vector<char> &srcoUnUdVals = compSet->getoUnUdVals();

	em.mergeVectors(edgeVecsToMerge, valVecsToMerge, srcDeltaEdges, srcDeltaVals, srcoUnUdEdges, srcoUnUdVals, 0);

	return em.getNumNewEdges();
}

/**
 * @param CompSet			-ComputationSet of source vertex
 * @param compsets[] 		-ComputationSet list of in-mem vertices
 * @param intervals[] 		-information about partition intervals
 * @param oldEdgesEmpty		-old edge list is empty
 * @param newEdgesEmpty		-new edge list is empty
 * @param edgeVecsToMerge	-2D vector of sorted edges to merge
 * @param valVecsToMerge	-2D vector of vals associated with edges
 * @param rowMergeID		-current row to add edges to
 * @param context			-grammar checker
 */
void getEdgesToMerge(ComputationSet *compSet, ComputationSet compsets[],
		LoadedVertexInterval intervals[], bool oldEdgesEmpty, bool newEdgesEmpty,
		vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
		int &rowMergeID, Context &context)
{
	vector<int> &oldEdges = compSet->getOldEdges();
	vector<char> &oldVals = compSet->getOldVals();

	vector<int> &newEdges = compSet->getNewEdges();
	vector<char> &newVals = compSet->getNewVals();

	// look through new edges of current vertex to see if any S-Rules
	if (!newEdgesEmpty)
		genS_RuleEdges(newEdges, newVals, edgeVecsToMerge, valVecsToMerge, rowMergeID, context);

	// look through old edges of current vertex to check if any are in a partition that is currently in memory
	if (!oldEdgesEmpty)
		genD_RuleEdges(compsets, intervals, oldEdges, oldVals, edgeVecsToMerge, valVecsToMerge, rowMergeID, context, 'o');

	// look through new edges of current vertex to check if any are in a partition that is currenlty in memory
	if (!newEdgesEmpty)
		genD_RuleEdges(compsets, intervals, newEdges, newVals, edgeVecsToMerge, valVecsToMerge, rowMergeID, context, 'n');
}

/**
 * For each edge in the outgoing edges list, check if the contextmar for a "S Rules" (single
 * char) rules applies. If so, add it to the new edge list to be merged
 *
 * @param newEdges				-vector of only most recently added edges
 * @param newVals				-vector of values corresponding to new edges
 * @param edgeVecsToMerge		-vector of vectors of edges to merge with source
 * @param valVecsToMerge		-vector of vecotrs of vals to merge with source
 * @param rowMergeID			-current row of vector to add
 * @param context					-grammar checker
 */
void genS_RuleEdges(vector<int> &newEdges, vector<char> &newVals,
		vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge,
		int &rowMergeID, Context &context)
{
	char newEdgeVal;
	bool added = false;
	for (int i = 0; i < newEdges.size(); i++) {
		newEdgeVal = context.grammar.checkRules(newVals[i], 0);
		if (newEdgeVal != (char)-1) {
			edgeVecsToMerge[rowMergeID].push_back(newEdges[i]);
			valVecsToMerge[rowMergeID].push_back(newEdgeVal);
			added = true;
		}
	}
	if (added) rowMergeID++;
}

/**
 * For each outgoing edge of the current source vertex, check if it points to a destination vertex that
 * is currently loaded into memory. If so, check the DESTINATION VERTEX's outgoing edges for D-Rules
 */
void genD_RuleEdges(ComputationSet compsets[], LoadedVertexInterval intervals[], vector<int> &edges,
		vector<char> &vals, vector< vector<int> > &edgeVecsToMerge,
		vector< vector<char> > &valVecsToMerge, int &rowMergeID, Context &context, char flag)
{
	int dstInd;
	for (int i = 0; i < edges.size(); i++)
	{
		for (int j = 0; j < 2; j++)
		{
			if (edges[i] >= intervals[j].getFirstVertex() && edges[i] <= intervals[j].getLastVertex()) {	// if loaded into memory
				dstInd = intervals[j].getIndexStart() + (edges[i] - intervals[j].getFirstVertex());		// calculate offset into ComputationSet
				if (rowMergeID == edgeVecsToMerge.size()) {				// if current row == size, add a new row
					edgeVecsToMerge.push_back( vector<int>() );
					valVecsToMerge.push_back( vector<char>() );
				}
				if (flag == 'o' && compsets[dstInd].getNewEdges().size() > 0)
					checkEdges(dstInd, vals[i], compsets, edgeVecsToMerge, valVecsToMerge, rowMergeID, context, flag);
				else if (flag == 'n' && compsets[dstInd].getoldUnewEdges().size() > 0)
					checkEdges(dstInd, vals[i], compsets, edgeVecsToMerge, valVecsToMerge, rowMergeID, context, flag);
			}
		}
	}
}

/**
 * given an index into the ComputationSet list, and the edge value of that vertex, check if any of
 * the outgoing edges fit the context grammar and if so add them to the vector to be merged with the source
 */
void checkEdges(int dstInd, char dstVal, ComputationSet compsets[],
		vector< vector<int> > &edgeVecsToMerge, vector< vector<char> > &valVecsToMerge, int &rowMergeID,
		Context &context, char flag)
{
	vector<int> &edges = (flag == 'o') ? compsets[dstInd].getNewEdges() : compsets[dstInd].getoldUnewEdges();
	vector<char> &vals = (flag == 'o') ? compsets[dstInd].getNewVals() : compsets[dstInd].getoldUnewVals();

	char newVal;
	bool added = false;
	for (int i = 0; i < edges.size(); i++)
	{
		newVal = context.grammar.checkRules(dstVal, vals[i]);   // check if  ( dstVal  ->  vals[i] ) == newVal
		if (newVal != (char)-1) {
			edgeVecsToMerge[rowMergeID].push_back(edges[i]);
			valVecsToMerge[rowMergeID].push_back(newVal);
			added = true;
		}
	}
	if (added) rowMergeID++;
}
