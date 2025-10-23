#ifndef COMPUTATIONSET_H
#define COMPUTATIONSET_H

#include "CFL/Graspan/utilities/globalDefinitions.hpp"

class ComputationSet
{
	vector<int> oldEdges;		// edges from last iter
	vector<char> oldVals;

	vector<int> newEdges;		// edges from most recent iter
	vector<char> newVals;

	vector<int> oldUnewEdges;	// all edges up to prev iter
	vector<char> oldUnewVals;

	// VECTORS WILL STORE NEW EDGES UNTIL END OF COMPUTATION
	vector<int> deltaEdges;		// new edges to be added this iter
	vector<char> deltaVals;

	vector<int> oUnUdEdges;		// all edges inlcuding new
	vector<char> oUnUdVals;


	public:
	// Constructor
	ComputationSet() {}

	// old getters and setters
	inline vector<int> &getOldEdges() { return oldEdges; }
	inline vector<char> &getOldVals() { return oldVals; }

	inline void setOldEdges(vector<int> &oldEdges) { this->oldEdges = oldEdges; }
	inline void setOldVals(vector<char> &oldVals) { this->oldVals = oldVals; }
	 
	 // new getters and setters
	inline vector<int> &getNewEdges() { return newEdges; }
	inline vector<char> &getNewVals() { return newVals; }

	inline void setNewEdges(vector<int> &newEdges) { this->newEdges = newEdges; }
	inline void setNewVals(vector<char> &newVals) { this->newVals = newVals; }

	// old U new getters and setters
	inline vector<int> &getoldUnewEdges() { return oldUnewEdges; }
	inline vector<char> &getoldUnewVals() { return oldUnewVals; }

	inline void setoldUnewEdges(vector<int> &oldUnewEdges) { this->oldUnewEdges = oldUnewEdges; }
	inline void setoldUnewVals(vector<char> &oldUnewVals) { this->oldUnewVals = oldUnewVals; }


	/*
		In order to ensure all edges are added, any edges added in the current iteration are
		stored in the delta vectros so the state of the graph does not change until all edges
		have been added for every vertex
	*/
	// delta getters and setters
	inline vector<int> &getDeltaEdges() { return deltaEdges; }
	inline vector<char> &getDeltaVals() { return deltaVals; }

	inline void setDeltaEdges(vector<int> &deltaEdges) { this->deltaEdges = deltaEdges; }
	inline void setDeltaVals(vector<char> &deltaVals) { this->deltaVals = deltaVals; }

	// old U new U delta getters and setters
	inline vector<int> &getoUnUdEdges() { return oUnUdEdges; }
	inline vector<char> &getoUnUdVals() { return oUnUdVals; }

	inline void setoUnUdEdges(vector<int> &oUnUdEdges) { this->oUnUdEdges = oUnUdEdges; }
	inline void setoUnUdVals(vector<char> &oUnUdVals) { this->oUnUdVals = oUnUdVals; }
};

#endif
