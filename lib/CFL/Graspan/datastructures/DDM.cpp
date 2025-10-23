#include "CFL/Graspan/datastructures/DDM.h"

DDM::DDM() {
  // the intial vector should be reserved quite big to accomondate for repartitioning
  // This should be configurable by user thru command line, not this hard-coded value
 // cout << "input the init size : ";
 // std::cin >>  max_size;
  max_size = 25;
  ddmMap.assign(max_size,vector<double>(max_size,0));
  d_ddmMap.assign(max_size, vector<double>(max_size, 0));
}

DDM::~DDM(){}

int DDM::getNumPartition()
{
	return numPartition;
}

//the meaning of adjust p is to set teminate_map[p][] and teminate_map[][p] into UNMARK
void DDM::adjust(int p, int q) {
	if (d_ddmMap[p][q] > 0 && ddmMap[p][q] < 1)
		ddmMap[p][q] += 1;
	if (d_ddmMap[q][p] > 0 && ddmMap[q][p] < 1)
		ddmMap[q][p] += 1;
	if (d_ddmMap[p][q] == 0 && ddmMap[p][q] > 1)
		ddmMap[p][q] -= 1;
	if (d_ddmMap[q][p] == 0 && ddmMap[q][p] > 1)
		ddmMap[q][p] -= 1;
}

void DDM::adjustRow(int pId)
{
	for (int i = 0; i < ddmMap[pId].size(); i++) {
		if (ddmMap[pId][i] < 1 && ddmMap[pId][i] > 0)
			ddmMap[pId][i] += 1;
		else if (ddmMap[pId][i] > 1)
			ddmMap[pId][i] -= 1;
	}
}


//return false if the whole system should terminate
bool DDM::nextPartitionPair(partitionid_t &p, partitionid_t &q) {

  //TODO: we want to favor the current loaded partition in memory: how to express that? (i.e., add bonus to the score)
  double max = 0.0;

  for (int i = 0; i < numPartition; ++i) {
	  for (int j = i+1; j < numPartition; ++j) {
		  if (i == j || (d_ddmMap[i][j] == 0 && d_ddmMap[j][i] == 0))
			  continue;
		  if (max < ddmMap[i][j] + ddmMap[j][i]) {
			  max = ddmMap[i][j] + ddmMap[j][i];
			  p = i;
			  q = j;
		  }
	  }
  }

  if (max == 0.0) { // should terminate since every cell is either 0 or negative -- we assume a pair is done when its partitionRate is negative
    return false;
  }
  
  return true;
}

// I have a feeling this is not quite right
void DDM::reSize() {
	ddmMap.resize(numPartition);
	d_ddmMap.resize(numPartition);
	for (int i = 0; i < numPartition; ++i) {
		ddmMap[i].resize(numPartition, 0);
		d_ddmMap[i].resize(numPartition, 0);
	}
}

void DDM::copy(int p, int q) {
	std::copy(ddmMap[p].begin(), ddmMap[p].end(), ddmMap[q].begin());
}

bool DDM::load_DDM(string fname){
	std::ifstream fin;
	fin.open(fname);
	if(!fin){
		cout << "can't read file" << endl;
		return false;
	}

	fin >> numPartition;
	reSize();
	for(int i=0;i<numPartition;i++){
		for(int j=0;j<numPartition;j++){
			fin >> ddmMap[i][j];
		}
	}
	for (int i = 0; i<numPartition; i++) {
		for (int j = 0; j<numPartition; j++) {
			fin >> d_ddmMap[i][j];
		}
	}
	fin.close();
	
	return true;
}

bool DDM::save_DDM(string fname){
	std::ofstream fout;
	fout.open(fname); 
	if(!fout){
		cout << "can't make file" << endl;
		return false;
	}

	//store numPartition
	fout << numPartition << endl;
	for(int i=0;i<numPartition;i++){
		for(int j=0;j<numPartition;j++){
		
			fout << ddmMap[i][j] << " ";
		}
		fout << endl;
	}
	fout << endl;
	for (int i = 0; i<numPartition; i++) {
		for (int j = 0; j<numPartition; j++) {
			fout << d_ddmMap[i][j] << " ";
		}
		fout << endl;
	}
	fout.close();

	return true;
}

//I would recommend another representation, why don't you try to print this out in matrix form instead? It's easier to see
string DDM::toString() {
	std::stringstream output;

	for (int i = 0; i < numPartition; ++i) {
		for (int j = 0; j < numPartition; ++j) {
			if (i != j)
				output << "Partition p: " << i << "  Partition q: " << j << "  rate: " << ddmMap[i][j] << endl;
		}
	}
	return output.str();
}
