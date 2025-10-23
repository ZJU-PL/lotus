#include "CFL/Graspan/datastructures/loadedvertexinterval.h"

// Constructor
LoadedVertexInterval::LoadedVertexInterval(int pID)
{
	partitionID = pID;
	newEdgeAdded = false;
}


// string representation
string LoadedVertexInterval::toString()
{
	std::stringstream output;

	output << endl << "(PartID: " << partitionID << ", VRange: " << firstVertex << " - " << lastVertex << ", IDRange: " << indexStart << " - " << indexEnd << ")";

	return output.str();
}
