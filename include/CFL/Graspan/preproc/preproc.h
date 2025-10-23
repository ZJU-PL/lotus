#ifndef PREPROC_H
#define PREPROC_H

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

#include "../utilities/globalDefinitions.hpp"
#include "../datastructures/vit.h"
#include "../datastructures/DDM.h"
#include "../datastructures/context.h"
#include "../datastructures/partition.h"



class Preproc {
private:
	int count;
	int vitSize;
	int dataSize;
	int numVertex;

	int *dataCheck;
	set<string> eRules;
	vector<string> mapInfo;
	vector<pair<int, string>> *data;

	//Partition *p;


public:
	Preproc(Context &context);

	void countNum(Context &context);
	void saveData(Context &context);
	void makeVIT(Context &context);
	void makePart(Context &context);
	void makeBinaryPart(Context &context);
	void makeDDM(Context &context);

	void setMapInfo(vector<string> mapInfo, set<char> eRules);

	int getNumOfPartitions();
	~Preproc();	
};

#endif
