#ifndef VIT_H
#define VIT_H

#include <iostream>
#include <sstream>
#include <stdlib.h>

#include "CFL/Graspan/utilities/globalDefinitions.hpp"

class VIT
{
private:

	// each entry is pair of [low, high] of vertices in the partition
	vector<pair<vertexid_t, vertexid_t>> vit;
	vector<int> degree; // what's this? number of edges per partition?

public:
	//Constructors
	VIT() {};
	

	//Getters
	inline vector<pair<vertexid_t, vertexid_t>> &getVIT() { return vit; };
	inline int getStart(int pid) { return vit[pid].first; };
	inline int getEnd(int pid) { return vit[pid].second; };
	inline vector<int> &getDegree() { return degree; };
	inline int getDegree(int pid) { return degree[pid]; };
	inline int getNumPartition() { return vit.size(); };

	//Setters
	inline void setVIT(vector<pair<int, int>> vit) { this->vit = vit; };
	void setDegree(int numPartition);

	//change the vit[id] values
	inline void setVITID(int id, vertexid_t start, vertexid_t end) {
		vit[id].first = start;
		vit[id].second = end;
	};

	// given a vertex id, return true if this vertex is in partition pid
	inline bool is_in_partition(vertexid_t vid, partitionid_t pid) {
		if (vid >= vit[pid].first || vid <= vit[pid].second)
			return true;
		else
			return false;
	};

	// return the partition id which vetex vid is in
	int partition(vertexid_t vid);

  string toString();

	// TODO:when partition whose id is partition_id is repartitioned, its VIT entry needs to be changed
	// also VIT adds new entry for the newly-created partition
	//void modify(partitionid_t pid); 

	bool writeToFile(string fname);

	bool loadFromFile(string fname);
	
};


#endif
