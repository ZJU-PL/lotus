#ifndef LOADEDVERTEXINTERVAL_H
#define LOADEDVERTEXINTERVAL_H

#include <string>
#include <sstream>

#include "CFL/Graspan/utilities/globalDefinitions.hpp"

class LoadedVertexInterval 
{
	int firstVertex;			// first and last vertex ID of current loaded partition
	int lastVertex;				// in the ComputationSet
	int indexStart;				// first and last index of current loaded partition into
	int indexEnd;				// the ComputationSet
	int partitionID;

	// If new edge added to partition in the current round
	bool newEdgeAdded;

	public:
	// Constructor
	LoadedVertexInterval(int partitionID);

	// Getters 
	inline int getFirstVertex() { return firstVertex; }
	inline int getLastVertex() { return lastVertex; }
	inline int getIndexStart() { return indexStart; }
	inline int getIndexEnd() { return indexEnd; }
	inline int getPartitionID() { return partitionID; }

	inline bool hasNewEdges() { return newEdgeAdded; }

	// Setters
	inline void setFirstVertex(int firstVertex) { this->firstVertex = firstVertex; }
	inline void setLastVertex(int lastVertex) { this->lastVertex = lastVertex; }
	inline void setIndexStart(int indexStart) { this->indexStart = indexStart; }
	inline void setIndexEnd(int indexEnd) { this->indexEnd = indexEnd; }
	inline void setPartitionID(int partitionID) { this->partitionID = partitionID; }

	inline void setNewEdgeAdded(bool newEdgeAdded) { this->newEdgeAdded = newEdgeAdded; }


	string toString();
};

#endif
