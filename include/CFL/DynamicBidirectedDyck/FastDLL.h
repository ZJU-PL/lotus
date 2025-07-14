#ifndef FASTDLL_H
#define FASTDLL_H

#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

using namespace std;

// typedef pair<string, unsigned> dllitem;

/*
class FastDLL{
public:
  FastDLL(){};
  bool empty() const{
    return dll.empty();
  }
  void add(string s, unsigned degree){
    dllitem item(s, degree);
    list< dllitem >::iterator it;

    dll.push_back(item);
    it = dll.end();
    it--;
    stringmap[s] = it;
  }
  void remove(string s){
    list< dllitem >::iterator it = stringmap[s];

    dll.erase(it);
    stringmap.erase(s);
    //stringmap[s] = dll.end();

  }

  int isInFDLL(string s){
    if(stringmap.find(s) == stringmap.end())
      return 0;
    //if(stringmap[s] == dll.end() )
    //return 0;
    else
      return 1;
  }
  unsigned getDegree(string s){
    list< dllitem >::iterator it = stringmap[s];
    return it->second;

  }
  dllitem front(){

    return dll.front();
  }
  void pop_front(){

    dllitem dit = dll.front();
    stringmap.erase(dit.first);
    dll.pop_front();

  }




private:
  list< dllitem > dll;
  unordered_map<string, list< dllitem >::iterator> stringmap;






};

*/

template <typename T1>

class In_FastDLL {
public:
  In_FastDLL() { dll_size = 0; }
  bool empty() const { return dll.empty(); }
  void add(T1 node) {

    typename list<T1>::iterator it;

    dll.push_back(node);
    it = dll.end();
    it--;
    nodemap[node] = it;
    dll_size++;
  }
  void remove(T1 node) {
    typename list<T1>::iterator it = nodemap[node];

    dll.erase(it);
    nodemap.erase(node);
    dll_size--;

    // stringmap[s] = dll.end();
  }

  int isInFDLL(T1 node) {
    if (nodemap.find(node) == nodemap.end())
      return 0;
    // if(stringmap[s] == dll.end() )
    // return 0;
    else
      return 1;
  }

  T1 front() { return dll.front(); }

  T1 front2() {
    typename list<T1>::iterator iter = dll.begin();
    iter++;
    return *iter;
  }
  void pop_front() {

    T1 dit = dll.front();
    nodemap.erase(dit);
    dll.pop_front();
    dll_size--;
  }
  unsigned size() { return dll_size; }

  void printlist() {

    cout << "==begin print list" << endl;
    for (typename list<T1>::iterator iter = dll.begin(); iter != dll.end();
         ++iter) {
      cout << *iter << endl;
    }
    cout << "==end" << endl;
  }

private:
  list<T1> dll;
  unordered_map<T1, typename list<T1>::iterator> nodemap;
  unsigned dll_size;
};

#endif