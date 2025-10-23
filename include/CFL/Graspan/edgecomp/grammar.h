#ifndef GRAMMAR_H
#define GRAMMAR_H

#include "CFL/Graspan/utilities/globalDefinitions.hpp"
#include <algorithm>
#include <fstream>
#include <cstring>

// TODO: just like input graph, what if we can reuse the result from previous run
// even though the grammar is small, reusing might be required for consistency
// 

class Grammar{
  
 private:

  vector<raw_label_t> map_info; //for storing map info

  set<label_t> eRules; //encoded form
  
  map<short, label_t> rules;
	
  void mapping(raw_label_t temp);
  
  string trimStr(string str);

  //add 1 for delete 0
  //0 is using another method instead of int
  inline label_t getValue(raw_label_t str){
    return (find(map_info.begin(),map_info.end(),str) - map_info.begin());
  }

  inline short changeShort(label_t a, label_t b){
    return (short)a << 8 | b;
  }
  
 public:
	
  Grammar();
  ~Grammar();

  bool loadGrammar(string fname);
	
  bool print_all();

  inline vector<raw_label_t> getMapInfo() {
    return map_info;
  }
	
  inline set<label_t> getErules() {
    return eRules;
  }

  //label_t checkRules(string srcVal, string dstVal);
  inline label_t checkRules(label_t edgeVal) {
    map<short,char>::iterator it;
    short key = (short)edgeVal;
    it = rules.find(key);
    return (it != rules.end()) ? (*it).second : label_t(-1);
  }

  inline label_t checkRules(label_t srcVal, label_t dstVal) {
    map<short,char>::iterator it;
    short key;

    if(dstVal!=0){
      key = changeShort(srcVal,dstVal);
    }  
    else{
      key = changeShort(0,srcVal);
    }

    it = rules.find(key);
    return (it != rules.end()) ? (*it).second : label_t(-1);
  
  }

};


#endif
