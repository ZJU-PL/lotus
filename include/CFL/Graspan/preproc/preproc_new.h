#ifndef PREPROC_NEW_H
#define PREPROC_NEW_H

#include <iostream>
#include <fstream>
#include <vector>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <algorithm>
#include <stdio.h>
#include <time.h>
#include <set>
#include <unistd.h>

#include "CFL/Graspan/utilities/globalDefinitions.hpp"
#include "CFL/Graspan/datastructures/vit.h"
#include "CFL/Graspan/datastructures/DDM.h"
#include "CFL/Graspan/datastructures/context.h"
#include "CFL/Graspan/datastructures/partition.h"



class Preproc_new {
private:
	int count;
	int vitSize;
	int dataSize;
	int numVertex;
	long numEdges;
	int *dataCount;
	int *numPartBuf;
	set<char> eRules;
	vector<string> mapInfo;
	Vertex **vertices;
	Vertex *partBuf;
	
	//Partition *p;


public:
	Preproc_new(Context &context);

	void countNum(Context &context);
	void setVIT(Context &context);

	void saveData(Context &context);
	void savePartChunk(Context &context, int partNum);
	
	void mergePart(Context &context);
	void loadPartChunk(Context &context, int pID);
	void addErules(Context &context, int pID);
	void checkPart(Context &context, int pID);
	void savePart(Context &context, int pID);

	void setMapInfo(vector<string> mapInfo, set<char> eRules);

	~Preproc_new();
};

#endif
