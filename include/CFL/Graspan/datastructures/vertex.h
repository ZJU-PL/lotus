#ifndef VERTEX_H
#define VERTEX_H

#include <sstream>
#include "CFL/Graspan/utilities/globalDefinitions.hpp"

class Vertex
{
	vertexid_t id;
	int numOutEdges;		// degree

	vector<vertexid_t> outEdges;	
	vector<label_t> outEdgeValues;
	

	public:
	// Constructors
	Vertex() { numOutEdges = 0; }
	Vertex(int id, int outDegree);
	Vertex(int id, vector<int> outEdges, vector<char> outEdgeValues);

	// Getters
	inline int getVertexID() { return id; }
	inline int getNumOutEdges() { return numOutEdges; }
	inline vector<int> &getOutEdges() { return outEdges; }
	inline vector<char> &getOutEdgeValues() { return outEdgeValues; }
	inline int getOutEdge(int ind) { return outEdges[ind]; }
	inline char getOutEdgeValue(int ind) { return outEdgeValues[ind]; }

	// Setters
	inline void setNumOutEdges(int numEdges) { this->numOutEdges = numEdges; }
	inline void setOutEdges(vector<int> &outEdges) { this->outEdges = outEdges; }
	inline void setOutEdgeValues(vector<char> &outEdgeValues) { this->outEdgeValues = outEdgeValues; }

	// clean up memory from vertices
	inline void clearVector() { 
		outEdges.clear();
		outEdgeValues.clear();
		
		vector<vertexid_t>().swap(outEdges);
		vector<label_t>().swap(outEdgeValues);
		numOutEdges = 0;
	};
	
	string toString();
};


#endif
