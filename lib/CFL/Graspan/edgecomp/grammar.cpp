#include "CFL/Graspan/edgecomp/grammar.h"

Grammar::Grammar(){
	//0 is using another method instead of int
	map_info.push_back("");
}
Grammar::~Grammar(){}

bool Grammar::loadGrammar(string fname){
  std::ifstream fin;
	fin.open(fname);
	if(!fin) return false;	

	char str[512]; //for reading original data
	char *p; //token
	vector<string> tok; //for distinguishing rules
	string temp; //temporary storage

	while(fin.getline(str,sizeof(str))){
		p = strtok(str,"\t");
		while(p != NULL){
			temp = trimStr(p);
			trimStr(temp);
			tok.push_back(temp);
			//mapping process
			mapping(temp);
			p = strtok(NULL,"\t");
		}

		//e-rule
		if(tok.size() == 1)
			eRules.insert(getValue(tok.at(0)));
		//s-rule
		else if(tok.size() == 2)
			rules[changeShort(0,getValue(tok.at(1)))]=getValue(tok.at(0));
		//d-rule
		else if(tok.size() == 3)
			rules[changeShort(getValue(tok.at(1)),getValue(tok.at(2)))]=getValue(tok.at(0));
		
		//initialize
		tok.erase(tok.begin(),tok.end());
	}
	fin.close();
	return true;
}

void Grammar::mapping(string temp){
	if(map_info.size() == find(map_info.begin(),map_info.end(),temp) - map_info.begin())
		map_info.push_back(temp);

}

string Grammar::trimStr(string str){
	size_t first = str.find_first_not_of(' ');
	size_t last = str.find_last_not_of(' ');
	return str.substr(first,(last - first + 1));
}
/*
//ex) checkRules("a","");
//ex) checkRules("a","b");
char Grammar::checkRules(string srcVal, string dstVal){
	map<short, char>::iterator it;
	if(!dstVal.empty()){
		short key = changeShort(getValue(srcVal),getValue(dstVal));
		it = rules.find(key);
		return (it != rules.end()) ? (*it).second : char(-1);
	}	
	else{
		short key = changeShort(0,getValue(srcVal));	
		it = rules.find(key);
		return (it != rules.end()) ? (*it).second : char(-1);
	}

}*/

//ex) checkRules("a","0");
//ex) checkRules("a","b");
/*label_t Grammar::checkRules(label_t srcVal, label_t dstVal){
	map<short,char>::iterator it;
	if(dstVal!=0){
		short key = changeShort(srcVal,dstVal);
		it = rules.find(key);
		return (it != rules.end()) ? (*it).second : label_t(-1);
	}	
	else{
		short key = changeShort(0,srcVal);	
		it = rules.find(key);
		return (it != rules.end()) ? (*it).second : label_t(-1);
	}
  }*/

bool Grammar::print_all(){

  using std::cin; 

	if(!eRules.size() && !rules.size()) return false;
	
	set<char>::iterator it_e; //for eRules
	for(it_e=eRules.begin();it_e!=eRules.end();it_e++)
		cout << (short)*it_e << endl;
	
	map<short, char>::iterator it;
	for(it=rules.begin(); it!=rules.end(); it++){
		if((*it).first <=256)
			cout << ((*it).first) << "->" << (short)(*it).second << endl;
		else
			cout << ((*it).first >> 8) << "," << (short)(char)(*it).first << "->" << (short)(*it).second << endl;
	}
	return true;
}
