#ifndef DDM_H
#define DDM_H

#include "CFL/Graspan/utilities/globalDefinitions.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
// There should be only one DDM object in the whole execution
// ==> implement Singleton pattern

class DDM{

 private:

  //TODO: implement scheduling policy described in the paper
  int max_size;
  vector<vector<double> > ddmMap; //array for storing ddmInfo
  vector<vector<double> > d_ddmMap; //array for terminate ddminfo
	int numPartition;

 public:
  //default constructor, the numPartition should be obtained thru the Context object
  DDM(); 
	~DDM();
  
  inline vector<vector<double> >& getDdmMap(){
    return ddmMap;
  };

  inline vector<vector<double> >& getD_ddmMap() {
	  return d_ddmMap;
  };

  //small methods should be inlined for efficiency
  inline void set(partitionid_t p, partitionid_t q, double rate) {
    ddmMap[p][q] = rate;
	ddmMap[q][p] = rate;
  }
  int getNumPartition();

  void adjust(int p, int q);
  void adjustRow(int pId);

  inline void markTerminate(partitionid_t p, partitionid_t q, bool newEdgesInP, bool newEdgesInQ) {
    cout << "DDM Terminate" << endl;
	if (!newEdgesInP && d_ddmMap[p][q] > 0) d_ddmMap[p][q] = 0;
 	if (!newEdgesInQ && d_ddmMap[q][p] > 0) d_ddmMap[q][p] = 0;
  }

  // the indices p and q tell which partitionRate[p][q] is max: i.e., this is our scheduler
  // the bool return value indicate whether or not we should terminate the whole system
  bool nextPartitionPair(partitionid_t& p, partitionid_t &q);
  
  inline void setNumPartition(int numPartition){
    this->numPartition = numPartition;
  };
  
  // if repartitioning happens, 1 or 2 new partition can happen, that means DDM needs to be enlarged
  // you should figure out how to implement this resizing logic efficiently
  void reSize();
  void copy(int p, int q);


  bool load_DDM(string fname);
  bool save_DDM(string fname); //assist computation

  string toString();
  
};

#endif
