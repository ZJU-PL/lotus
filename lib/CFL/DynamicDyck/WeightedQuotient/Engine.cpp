// Direct port of upstream dynamic/DyckReach.cpp; see README.md.
#include "CFL/DynamicDyck/WeightedQuotient/Engine.h"

#include "CFL/DynamicDyck/WeightedQuotient/DotParser.h"

#include <cassert>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::dynamic_dyck::weighted_quotient {
void Engine::inc_weight(
    unsigned i, unsigned j, unsigned eid, CFLHashMap &merged_cm,
    unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes) {
  unsigned iresp = resp->find(i);
  unsigned jresp = resp->find(j);
  string wei;
  stringstream weiconvert;
  weiconvert << "_" << iresp << "_" << jresp << "_" << eid;
  wei = weiconvert.str();
  if (weight.find(wei) == weight.end()) {
    weight[wei] = 0;
    // if(debug && merged_cm.HasEdgeBetween(iresp, jresp, eid)){
    //     cout << "Never Happens: weight inconsistent when inc.\n";
    // }
    merged_cm.InsertEdge(iresp, jresp, eid);
    string s;
    stringstream convert;
    convert << jresp << "_" << eid;
    s = convert.str();
    if (!ColorInNodes[s].isInFDLL(iresp)) {
      ColorInNodes[s].add(iresp);
    }
  }
  weight[wei]++;
  node2weight[iresp].insert(wei);
  node2weight[jresp].insert(wei);
}

list<unsigned> Engine::rmlistitemsin(list<unsigned> &lst,
                                     list<unsigned> &toremove) {
  unordered_set<unsigned> items;
  for (list<unsigned>::iterator it = toremove.begin(); it != toremove.end();
       it++) {
    items.insert(*it);
  }
  list<unsigned> res;
  for (list<unsigned>::iterator it = lst.begin(); it != lst.end(); it++) {
    if (items.find(*it) == items.end()) {
      // should keep
      res.push_back(*it);
    }
  }
  return res;
}

void Engine::dec_weight(
    unsigned i, unsigned j, unsigned eid, CFLHashMap &merged_cm,
    unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes) {
  unsigned iresp = resp->find(i);
  unsigned jresp = resp->find(j);
  string wei;
  stringstream weiconvert;
  weiconvert << "_" << iresp << "_" << jresp << "_" << eid;
  wei = weiconvert.str();
  if (weight.find(wei) == weight.end()) {
    // if(debug)
    //     cout << "Never Happens no weight when dec.\n";
    weight[wei] = 1;
  }
  weight[wei]--;
  if (weight[wei] == 0) {
    weight.erase(wei);
    node2weight[iresp].erase(wei);
    node2weight[jresp].erase(wei);
    // if(debug && (!merged_cm.HasEdgeBetween(iresp, jresp, eid))){
    //     cout << "no edge to delete when dec weight 0, skip seq.\n";
    //     return;
    // }
    merged_cm.DeleteEdge(iresp, jresp, eid);
    string s;
    stringstream convert;
    convert << jresp << "_" << eid;
    s = convert.str();
    if (ColorInNodes[s].isInFDLL(iresp))
      ColorInNodes[s].remove(iresp);
  }
}

void Engine::mainproc(In_FastDLL<string> &fdll,
                      unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                      CFLHashMap &cm,
                      unordered_map<unsigned, list<unsigned>> &s_sets,
                      unsigned NodeNum) {
  if (resp == NULL) {
    resp.reset(new DisjointSet(NodeNum));
  }
  while (!fdll.empty()) {

    const string z_string = fdll.front();
    fdll.pop_front();
    // cout<<(fdllitem.first)<<" and "<<(fdllitem.second)<<endl;

    // string s = fdllitem.first;

    unsigned n1 = ColorInNodes[z_string].front();
    unsigned n2 = ColorInNodes[z_string].front2();
    unsigned x, y;

    if (cm.GetNodeDegree(n1) >= cm.GetNodeDegree(n2)) {
      x = n1;
      y = n2;
    } else {
      x = n2;
      y = n1;
    }

    resp->join(x, y);
    ++merge_count;
    s_sets[x].splice(s_sets[x].end(), s_sets[y]);
    s_sets.erase(y);

    // s_set[y]
    // if(cm.GetNodeDegree(y) == 0)
    // cout<<"error!"<<endl;

    if (cm.HasEdgeBetween(y, y)) {

      unordered_map<unsigned, char> color;
      cm.CheckInColor(y, y, color);
      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        string sy;
        stringstream convert;
        convert << y << "_" << (c->first);
        sy = convert.str();

        if (!cm.HasEdgeBetween(x, x, c->first)) {
          cm.InsertEdge(x, x, c->first);
          // add to color in nodes
          string s;
          stringstream convert;
          convert << x << "_" << (c->first);
          s = convert.str();
          if (!ColorInNodes[s].isInFDLL(x)) {
            ColorInNodes[s].add(x);
            // add x to dll if D[x] > 1
          }

          if ((ColorInNodes[s]).size() > 1) {
            if (!fdll.isInFDLL(s)) {
              fdll.add(s);
            }
          }
        }
        cm.DeleteEdge(y, y, c->first);
        string wei;
        stringstream weiconvert;
        weiconvert << "_" << y << "_" << y << "_" << c->first;
        wei = weiconvert.str();
        if (weight.find(wei) == weight.end()) {
          // if(debug)
          //     cout << "Never Happens 1.\n";
          weight[wei] = 1;
        }
        int weightnum = weight[wei];
        weight[wei] = 0;
        // if(debug)
        //     total_weight -= weightnum;
        weight.erase(wei);
        node2weight[y].erase(wei);
        string wei2;
        stringstream weiconvert2;
        weiconvert2 << "_" << x << "_" << x << "_" << c->first;
        wei2 = weiconvert2.str();
        if (weight.find(wei2) == weight.end()) {
          weight[wei2] = 0;
        }
        weight[wei2] += weightnum;
        node2weight[x].insert(wei2);
        // if(debug){
        //     total_weight += weightnum;
        // }
        if (ColorInNodes[sy].isInFDLL(y)) {
          ColorInNodes[sy].remove(y);
        }
        if ((ColorInNodes[sy]).size() < 2) {
          if (fdll.isInFDLL(sy)) {
            fdll.remove(sy);
          }
        }
      }
    }

    // for y 's neighbor

    unordered_map<unsigned, Matrix1> innodes;

    // colorcountmap colorcount = NodeColorCount[i];
    cm.CheckInEdges(y, innodes);

    for (unordered_map<unsigned, Matrix1>::iterator itw = innodes.begin();
         itw != innodes.end(); ++itw) {
      unsigned w = (itw->first);
      unordered_map<unsigned, char> color = (itw->second).colors;
      // cm.CheckInColor(w, y, color);
      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        if (!cm.HasEdgeBetween(w, x, c->first)) {
          cm.InsertEdge(w, x, c->first);

          // add to color in nodes
          string s;
          stringstream convert;
          convert << x << "_" << (c->first);
          s = convert.str();
          if (!ColorInNodes[s].isInFDLL(w)) {

            ColorInNodes[s].add(w);
          }
          if ((ColorInNodes[s]).size() > 1) {
            if (!fdll.isInFDLL(s)) {
              fdll.add(s);
            }
          }
        }

        string sy;
        stringstream convert;
        convert << y << "_" << (c->first);
        sy = convert.str();
        if (ColorInNodes[sy].isInFDLL(w)) {
          ColorInNodes[sy].remove(w);
        }

        // remove w to dll if D[w] <2 1

        if ((ColorInNodes[sy]).size() < 2) {
          if (fdll.isInFDLL(sy)) {
            fdll.remove(sy);
          }
        }
        cm.DeleteEdge(w, y, c->first);
        string wei;
        stringstream weiconvert;
        weiconvert << "_" << w << "_" << y << "_" << c->first;
        wei = weiconvert.str();
        if (weight.find(wei) == weight.end()) {
          // if(debug)
          //     cout << "Never Happens 2.\n";
          weight[wei] = 1;
        }
        int weightnum = weight[wei];
        weight[wei] = 0;
        weight.erase(wei);
        node2weight[w].erase(wei);
        node2weight[y].erase(wei);
        // if(debug)
        //     total_weight -= weightnum;
        string wei2;
        stringstream weiconvert2;
        weiconvert2 << "_" << w << "_" << x << "_" << c->first;
        wei2 = weiconvert2.str();
        if (weight.find(wei2) == weight.end()) {
          weight[wei2] = 0;
        }
        weight[wei2] += weightnum;
        node2weight[w].insert(wei2);
        node2weight[x].insert(wei2);
        // if(debug)
        //     total_weight += weightnum;
      }
    }

    unordered_map<unsigned, Matrix1> outnodes;

    // colorcountmap colorcount = NodeColorCount[i];
    cm.CheckOutEdges(y, outnodes);

    for (unordered_map<unsigned, Matrix1>::iterator itw = outnodes.begin();
         itw != outnodes.end(); ++itw) {
      unsigned w = (itw->first);
      unordered_map<unsigned, char> color = (itw->second).colors;
      // cm.CheckOutColor(y, w, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        string s;
        stringstream convert;
        convert << w << "_" << (c->first);
        s = convert.str();

        // compute weight
        if (!no_weight) {
          string wei2;
          stringstream weiconvert2;
          weiconvert2 << "_" << y << "_" << w << "_" << c->first;
          wei2 = weiconvert2.str();
          if (weight.find(wei2) == weight.end()) {
            // if(debug)
            //     cout << "Never Happens 3.\n";
            weight[wei2] = 1;
          }
          int weightnum = weight[wei2];
          weight[wei2] = 0;
          weight.erase(wei2);
          node2weight[y].erase(wei2);
          node2weight[w].erase(wei2);
          // if(debug){
          //     total_weight -= weightnum;
          // }
          string wei;
          stringstream weiconvert;
          weiconvert << "_" << x << "_" << w << "_" << c->first;
          wei = weiconvert.str();
          if (weight.find(wei) == weight.end()) {
            weight[wei] = 0;
          }
          weight[wei] += weightnum;
          node2weight[x].insert(wei);
          node2weight[w].insert(wei);
          // if(debug){
          //     total_weight += weightnum;
          // }
        }
        if (!cm.HasEdgeBetween(x, w, c->first)) {
          cm.InsertEdge(x, w, c->first);

          // add to color in nodes

          if (!ColorInNodes[s].isInFDLL(x)) {
            ColorInNodes[s].add(x);
          }
        }

        if (ColorInNodes[s].isInFDLL(y)) {
          (ColorInNodes[s]).remove(y);
        }

        if ((ColorInNodes[s]).size() < 2) {
          if (fdll.isInFDLL(s)) {
            fdll.remove(s);
          }
        }
        cm.DeleteEdge(y, w, c->first);
      }
    }
    if ((ColorInNodes[z_string]).size() > 1) {
      if (!fdll.isInFDLL(z_string)) {
        fdll.add(z_string);
      }
    }
  }

  unsigned s_set_size = 0;
  for (unordered_map<unsigned, list<unsigned>>::iterator sit = s_sets.begin();
       sit != s_sets.end(); ++sit) {
    // cout<<"set "<<sit->first<<endl;;
    unsigned tmps = sit->second.size();
    s_set_size = s_set_size + tmps * tmps;
  }
  // cout<<"nodes "<<NodeNum<<endl;
  // cout<<"s edges "<<s_set_size<<endl;
  // MEM_USAGE();
}

/***
 *
 *
 */
void Engine::insert(CFLHashMap &cm, unsigned node_a, unsigned node_b,
                    unsigned eid, CFLHashMap &merged_cm,
                    unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                    unordered_map<unsigned, list<unsigned>> &s_sets) {
  gettimeofday(&begin_t, NULL);
  if (!cm.HasEdgeBetween(node_a, node_b, eid)) {
    cm.InsertEdge(node_a, node_b, eid);
    ++insertion_count;
  } else {
    return;
  }
  if (!merged_cm.HasEdgeBetween(resp->find(node_a), resp->find(node_b), eid)) {
    merged_cm.InsertEdge(resp->find(node_a), resp->find(node_b), eid);
  }

  string s;
  stringstream convert;
  convert << resp->find(node_b) << "_" << eid;
  s = convert.str();
  string wei;
  stringstream weiconvert;
  weiconvert << "_" << resp->find(node_a) << "_" << resp->find(node_b) << "_"
             << eid;
  wei = weiconvert.str();
  if (weight.find(wei) == weight.end()) {
    weight[wei] = 0;
  }
  weight[wei]++;
  node2weight[resp->find(node_a)].insert(wei);
  node2weight[resp->find(node_b)].insert(wei);
  // if(debug)
  //     total_weight ++;
  if (!ColorInNodes[s].isInFDLL(resp->find(node_a)))
    ColorInNodes[s].add(resp->find(node_a));
  In_FastDLL<string> fdll;
  if (ColorInNodes[s].size() > 1) {
    fdll.add(s);
  }
  mainproc(fdll, ColorInNodes, merged_cm, s_sets, merged_cm.GetVtxNum());
  gettimeofday(&end_t, NULL);
  elapsed += ((end_t.tv_sec - begin_t.tv_sec) +
              ((end_t.tv_usec - begin_t.tv_usec) / 1000000.0));
}

// should update s_sets, resp, edges (in G_m), ColorInNodes and weight
// correctly. need to guarantee the items in nodes are unique, the node in nodes
// are in the same merged node return the representative node for the original
// node set after the split
int Engine::split(list<unsigned> &nodes,
                  unordered_map<unsigned, list<unsigned>> &s_sets,
                  CFLHashMap &cm, CFLHashMap &merged_cm,
                  unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes) {
  unsigned firnode = nodes.front();
  unsigned origresp = resp->find(firnode);
  if (nodes.size() == s_sets[origresp].size()) {
    // split all the nodes means do nothing
    return -1;
  }
  // get the two set of nodes, one list of nodes is nodes, the other is resnodes
  list<unsigned> restnodes;
  restnodes.assign(s_sets[origresp].begin(), s_sets[origresp].end());
  restnodes = rmlistitemsin(restnodes, nodes);
  // update the s_sets, resp and weights of edges in nodes and restnodes
  unsigned nodesresp = firnode;
  unsigned restresp = restnodes.front();
  for (list<unsigned>::iterator it = nodes.begin(); it != nodes.end(); it++) {
    if (*it == origresp) {
      nodesresp = origresp;
    }
  }
  if (nodesresp != origresp) {
    restresp = origresp;
  }
  //      clean the previous s_sets
  s_sets.erase(origresp);
  ++split_count;
  //      set the new s_sets
  s_sets[nodesresp] = nodes;
  s_sets[restresp] = restnodes;
  //      clean the previous resp
  //          no need to clean
  //      set the new resp
  for (list<unsigned>::iterator it = nodes.begin(); it != nodes.end(); it++) {
    resp->arr[*it] = nodesresp;
  }
  for (list<unsigned>::iterator it = restnodes.begin(); it != restnodes.end();
       it++) {
    resp->arr[*it] = restresp;
  }
  //      clean the previous weight and set the new weight, edges are also
  //      updated accordingly
  list<unsigned> *workingnodes;
  if (nodesresp != origresp) {
    workingnodes = &nodes;
  } else {
    workingnodes = &restnodes;
  }
  //          working nodes are the nodes without weights
  for (list<unsigned>::iterator it = workingnodes->begin();
       it != workingnodes->end(); it++) {
    unsigned j = *it;
    unordered_map<unsigned, Matrix1> innodes;
    cm.CheckInEdges(j, innodes);
    for (unordered_map<unsigned, Matrix1>::iterator itw = innodes.begin();
         itw != innodes.end(); ++itw) {
      unsigned i = itw->first;
      unordered_map<unsigned, char> color = (itw->second).colors;
      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        unsigned eid = c->first;
        inc_weight(resp->find(i), resp->find(j), eid, merged_cm, ColorInNodes);
        dec_weight(resp->find(i), origresp, eid, merged_cm, ColorInNodes);
      }
    }
    unsigned i = *it;
    unordered_map<unsigned, Matrix1> outnodes;
    cm.CheckOutEdges(i, outnodes);
    for (unordered_map<unsigned, Matrix1>::iterator itw = outnodes.begin();
         itw != outnodes.end(); ++itw) {
      unsigned j = itw->first;
      if (j == i) {
        // already considered
        continue;
      }
      unordered_map<unsigned, char> color = (itw->second).colors;
      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        unsigned eid = c->first;
        inc_weight(resp->find(i), resp->find(j), eid, merged_cm, ColorInNodes);
        dec_weight(origresp, resp->find(j), eid, merged_cm, ColorInNodes);
      }
    }
  }
  return restresp;
  // return the respnode for the other set
  // if(res_u != u){
  //    for(list<unsigned>::iterator sit = s_sets[res_u].begin(); sit !=
  //    s_sets[res_u].end(); sit++){
  //        resp->find(*sit);
  //    }
  //    s_sets[res_u].remove(u);
  //    resp->arr[u] = u;
  //    s_sets[u].push_back(u);
  //    result = res_u;
  //}else if(s_sets[res_u].size() > 1){
  //    unsigned other = u;
  //    for(list<unsigned>::iterator sit = s_sets[res_u].begin(); sit !=
  //    s_sets[res_u].end(); sit++){
  //        if(*sit != u){
  //            other = *sit;
  //            break;
  //        }
  //    }
  //    if(other == u){
  //        if(debug)
  //            cout << "Never Happens 4.\n";
  //    }
  //    s_sets[res_u].remove(res_u);
  //    for(list<unsigned>::iterator sit = s_sets[res_u].begin(); sit !=
  //    s_sets[res_u].end(); sit++){
  //        if(*sit != other){
  //            resp->arr[*sit] = other;
  //        }
  //    }
  //    s_sets[other].splice(s_sets[other].end(), s_sets[res_u]);
  //    s_sets.erase(res_u);
  //    s_sets[res_u].push_back(res_u);
  //    resp->arr[u] = u;
  //    result = other;
  //}
  // return result;
}

bool Engine::weight_split_condition(unsigned noderesp) {
  // if there exists no weight 2 edges, return false
  // otherwise return true
  unordered_set<string> node_weights = node2weight[noderesp];
  for (unordered_set<string>::iterator it = node_weights.begin();
       it != node_weights.end(); it++) {
    string wei = *it;
    string pat = "_" + to_string(noderesp) + "_";
    if (wei.find(pat) != 0) {
      // not an outgoing edge weight
      continue;
    }
    if (weight[wei] > 1) {
      return true;
    }
  }
  return false;
}

void Engine::split_further(
    CFLHashMap &cm, list<unsigned> tosplit, CFLHashMap &merged_cm,
    unordered_map<unsigned, list<unsigned>> &s_sets,
    unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
    unordered_set<string> &checked) {
  // if(debug)
  //     cout << "splitting node " << u << endl;

  int other_resp = split(tosplit, s_sets, cm, merged_cm, ColorInNodes);
  if (other_resp == -1) {
    // didn't split
    return;
  }

  //// check whether to split predecessors
  //// get predecessors
  // unordered_set<unsigned> pred_resps;
  ////      pred for new resp
  // unsigned new_resp = resp->find(tosplit.front());
  // unordered_map<unsigned, Matrix1> innodes_newresp;
  // merged_cm.CheckInEdges(new_resp, innodes_newresp);
  // for(unordered_map<unsigned, Matrix1>::iterator it =
  // innodes_newresp.begin(); it != innodes_newresp.end(); it++){
  //     unsigned pred_resp = it->first;
  //     if(pred_resp == new_resp)
  //         continue;
  //     pred_resps.insert(pred_resp);
  // }
  ////      pred for orig resp
  // unordered_map<unsigned, Matrix1> innodes_otherresp;
  // merged_cm.CheckInEdges(other_resp, innodes_otherresp);
  // for(unordered_map<unsigned, Matrix1>::iterator it =
  // innodes_otherresp.begin(); it != innodes_otherresp.end(); it++){
  //     unsigned pred_resp = it->first;
  //     if(pred_resp == other_resp)
  //         continue;
  //     pred_resps.insert(pred_resp);
  // }

  // for(unordered_set<unsigned>::iterator it = pred_resps.begin(); it !=
  // pred_resps.end(); it++){
  //     unsigned pred_resp = *it;
  //     if(!weight_split_condition(pred_resp)){
  //         // first split into two set
  //         split_further(cm, s_sets[pred_resp], merged_cm, s_sets,
  //         ColorInNodes, checked);
  //     }
  // }

  // check weight condition
  // check further condition
  //// in some cases, the resp(u) = u, and u becomes a new nodes, we still need
  /// to query the old weight.
  // unordered_set<unsigned> updatedin;
  // unordered_map<unsigned, Matrix1> innodes;
  // cm.CheckInEdges(u, innodes);
  // for( unordered_map<unsigned, Matrix1>::iterator itw = innodes.begin(); itw
  // != innodes.end(); ++itw){
  //     unsigned w = (itw->first);
  //     unordered_map<unsigned, char> color = (itw->second).colors;
  //    for (unordered_map<unsigned, char>::iterator c = color.begin(); c !=
  //    color.end(); ++c){
  //        string wei;
  //        stringstream weiconvert;
  //        if(updatedin.find(resp->find(w)) == updatedin.end()){
  //            weiconvert << "_" << resp->find(w) << "_" << orig_resp_for_wei
  //            << "_" << c->first;
  //        }else{
  //            weiconvert << "_" << resp->find(w) << "_" << orig_resp << "_" <<
  //            c->first;
  //        }
  //        wei = weiconvert.str();
  //        if(weight.find(wei) == weight.end()){
  //            if(debug)
  //                cout << "Never Happens 5.\n";
  //            weight[wei] = 1;
  //        }
  //        weight[wei]--;
  //        if(debug)
  //            total_weight --;
  //        if(updatedin.find(resp->find(w)) == updatedin.end() &&
  //        orig_resp_for_wei != orig_resp){
  //            string newwei;
  //            stringstream newweiconvert;
  //            newweiconvert << "_" << resp->find(w) << "_" << orig_resp << "_"
  //            << c->first; newwei = newweiconvert.str(); weight[newwei] =
  //            weight[wei]; node2weight[resp->find(w)].insert(newwei);
  //            node2weight[orig_resp].insert(newwei);
  //            updatedin.insert(resp->find(w));
  //            weight.erase(wei);
  //            node2weight[resp->find(w)].erase(wei);
  //            node2weight[orig_resp_for_wei].erase(wei);
  //            wei = newwei;
  //        }
  //        if(weight[wei] == 0){
  //            weight.erase(wei);
  //            merged_cm.DeleteEdge(resp->find(w), orig_resp, c->first);
  //            string s;
  //            stringstream convert;
  //            convert<<orig_resp<<"_"<<c->first;
  //            s= convert.str();
  //            if(ColorInNodes[s].isInFDLL(resp->find(w)))
  //                ColorInNodes[s].remove(resp->find(w));
  //        }
  //        string wei2;
  //        stringstream weiconvert2;
  //        weiconvert2 << "_" << resp->find(w) << "_" << resp->find(u) << "_"
  //        << c->first; wei2 = weiconvert2.str();
  //        if(!merged_cm.HasEdgeBetween(resp->find(w), resp->find(u),
  //        c->first)){
  //            merged_cm.InsertEdge(resp->find(w), resp->find(u), c->first);
  //            string s;
  //            stringstream convert;
  //            convert << resp->find(u) << "_" << c->first;
  //            s = convert.str();
  //            if(!ColorInNodes[s].isInFDLL(resp->find(w))){
  //                ColorInNodes[s].add(resp->find(w));
  //            }
  //            weight[wei2] = 0;
  //        }
  //        weight[wei2]++;
  //        if(debug)
  //            total_weight ++;
  //    }
  //}
  //
  // unordered_set<unsigned> updatedout;
  // unordered_map<unsigned, Matrix1> outnodes;
  // cm.CheckOutEdges(u, outnodes);
  // for( unordered_map<unsigned, Matrix1>::iterator itw = outnodes.begin(); itw
  // != outnodes.end(); ++itw){
  //    unsigned w = (itw->first);
  //    unordered_map<unsigned, char> color = (itw->second).colors;
  //    for (unordered_map<unsigned, char>::iterator c = color.begin(); c !=
  //    color.end(); ++c){
  //        string wei;
  //        stringstream weiconvert;
  //        if(updatedout.find(resp->find(w)) == updatedout.end()){
  //            weiconvert << "_" << orig_resp_for_wei << "_" << resp->find(w)
  //            << "_" << c->first;
  //        }else{
  //            weiconvert << "_" << orig_resp << "_" << resp->find(w) << "_" <<
  //            c->first;
  //        }
  //        wei = weiconvert.str();
  //        if(weight.find(wei) == weight.end()){
  //            if(debug)
  //                cout << "Never Happens 6.\n";
  //            weight[wei] = 1;
  //        }
  //        weight[wei]--;
  //        if(debug)
  //            total_weight --;
  //        if(updatedout.find(resp->find(w)) == updatedout.end() &&
  //        orig_resp_for_wei != orig_resp){
  //            string newwei;
  //            stringstream newweiconvert;
  //            newweiconvert << "_" << resp->find(w) << "_" << orig_resp << "_"
  //            << c->first; newwei = newweiconvert.str(); weight[newwei] =
  //            weight[wei]; weight.erase(wei);
  //            updatedout.insert(resp->find(w));
  //            wei = newwei;
  //        }
  //        if(weight[wei] == 0){
  //            weight.erase(wei);
  //            merged_cm.DeleteEdge(orig_resp, resp->find(w), c->first);
  //            string s;
  //            stringstream convert;
  //            convert<<resp->find(w)<<"_"<<c->first;
  //            s= convert.str();
  //            if(ColorInNodes[s].isInFDLL(orig_resp))
  //                ColorInNodes[s].remove(orig_resp);
  //        }
  //        string wei2;
  //        stringstream weiconvert2;
  //        weiconvert2 << "_" << resp->find(u) << "_" << resp->find(w) << "_"
  //        << c->first; wei2 = weiconvert2.str();
  //        if(!merged_cm.HasEdgeBetween(resp->find(u), resp->find(w),
  //        c->first)){
  //            merged_cm.InsertEdge(resp->find(u), resp->find(w), c->first);
  //            string s;
  //            stringstream convert;
  //            convert << resp->find(w) << "_" << c->first;
  //            s = convert.str();
  //            if(!ColorInNodes[s].isInFDLL(resp->find(u))){
  //                ColorInNodes[s].add(resp->find(u));
  //            }
  //            weight[wei2] = 0;
  //        }
  //        weight[wei2]++;
  //        if(debug)
  //            total_weight ++;

  //    }
  //}

  // unordered_set<unsigned> preds;
  // for ( list<unsigned>::iterator sit = s_sets[orig_resp].begin(); sit!=
  // s_sets[orig_resp].end(); ++sit){
  //     unsigned orig_node = *sit;
  //     unordered_map<unsigned, Matrix1> pred_innodes;
  //     cm.CheckInEdges(orig_node, pred_innodes);
  //     for( unordered_map<unsigned, Matrix1>::iterator itw =
  //     pred_innodes.begin(); itw != pred_innodes.end(); ++itw){
  //         preds.insert(itw->first);
  //     }
  // }
  // unordered_map<unsigned, Matrix1> pred_innodes;
  // cm.CheckInEdges(u, pred_innodes);
  // for( unordered_map<unsigned, Matrix1>::iterator itw = pred_innodes.begin();
  // itw != pred_innodes.end(); ++itw){
  //     preds.insert(itw->first);
  // }
  // for(unordered_set<unsigned>::iterator it = preds.begin(); it !=
  // preds.end(); it++){
  //     unsigned pred_node = *it;
  //     unordered_map<unsigned, Matrix1> pred_outnodes;
  //     cm.CheckOutEdges(pred_node, pred_outnodes);
  //     bool need_split = true;
  //     for( unordered_map<unsigned, Matrix1>::iterator itw =
  //     pred_innodes.begin(); itw != pred_innodes.end(); ++itw){
  //         unsigned nextnode = itw->first;
  //         unordered_map<unsigned, char> color = (itw->second).colors;
  //        for (unordered_map<unsigned, char>::iterator c = color.begin(); c !=
  //        color.end(); ++c){
  //            string wei;
  //            stringstream weiconvert;
  //            weiconvert << "_" << resp->find(pred_node) << "_" <<
  //            resp->find(nextnode) << "_" << c->first; wei = weiconvert.str();
  //            if(weight.find(wei) != weight.end() && weight[wei] > 2){
  //                need_split = false;
  //            }

  //        }

  //
  //    }
  //    if(need_split){
  //        string ss;
  //        stringstream ssconvert;
  //        ssconvert << resp->find(pred_node) << "_" << resp->find(u);
  //        ss = ssconvert.str();
  //        if(checked.find(ss) != checked.end()){
  //            checked.insert(ss);
  //            split_further(cm, pred_node, merged_cm, s_sets, ColorInNodes,
  //            checked);
  //        }

  //    }

  //}
}

void Engine::split_further(
    CFLHashMap &cm, unsigned tosplit, CFLHashMap &merged_cm,
    unordered_map<unsigned, list<unsigned>> &s_sets,
    unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
    unordered_set<string> &checked) {
  // get predecessors for recursive split
  unordered_set<unsigned> pred_resps;
  unsigned cur_resp = resp->find(tosplit);
  unordered_map<unsigned, Matrix1> innodes_curresp;
  merged_cm.CheckInEdges(cur_resp, innodes_curresp);
  for (unordered_map<unsigned, Matrix1>::iterator it = innodes_curresp.begin();
       it != innodes_curresp.end(); it++) {
    unsigned pred_resp = it->first;
    if (pred_resp == cur_resp)
      continue;
    pred_resps.insert(pred_resp);
  }

  // divide the nodes represented by tosplit into groups
  //      first, every one as its own group
  list<unsigned> contained_nodes;
  contained_nodes.assign(s_sets[tosplit].begin(), s_sets[tosplit].end());
  if (contained_nodes.size() < 2) {
    return;
  }
  unordered_map<unsigned, unsigned> n2id;
  unordered_map<unsigned, unsigned> id2n;
  unsigned newid = 0;
  for (list<unsigned>::iterator it = contained_nodes.begin();
       it != contained_nodes.end(); it++) {
    unsigned nd = *it;
    n2id[nd] = newid;
    id2n[newid] = nd;
    newid++;
  }
  DisjointSet n2groups(contained_nodes.size());

  // find the groups
  //      a map takes the endnode, eid to a node in the merged_node
  unordered_map<unsigned, unordered_map<unsigned, unsigned>>
      endnode_eid_to_node;
  // according to the out edges, merge nodes into groups
  for (list<unsigned>::iterator it = contained_nodes.begin();
       it != contained_nodes.end(); it++) {
    unsigned nd = *it;
    unordered_map<unsigned, Matrix1> outnodes;
    cm.CheckOutEdges(nd, outnodes);
    for (unordered_map<unsigned, Matrix1>::iterator itw = outnodes.begin();
         itw != outnodes.end(); ++itw) {
      unsigned endnode = itw->first;
      if (n2id.find(endnode) == n2id.end()) {
        endnode = resp->find(endnode);
      }
      unordered_map<unsigned, char> color = (itw->second).colors;
      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        unsigned eid = c->first;
        unordered_map<unsigned, unsigned> *endnode_map =
            &(endnode_eid_to_node[endnode]);
        // if there is no info, add the info into the map
        if (endnode_map->find(eid) == endnode_map->end()) {
          (*endnode_map)[eid] = nd;
        } else {
          // there is already info, just merge the nodes
          n2groups.join(n2id[nd], n2id[(*endnode_map)[eid]]);
        }
      }
    }
  }
  // get the groups
  unordered_map<unsigned, list<unsigned>> resp2groups;
  for (size_t i = 0; i < n2groups.arr.size(); i++) {
    unsigned groupid = n2groups.find(i);
    resp2groups[groupid].push_back(id2n[i]);
  }
  if (resp2groups.size() < 2) {
    return;
  }
  // split each group
  for (unordered_map<unsigned, list<unsigned>>::iterator it =
           resp2groups.begin();
       it != resp2groups.end(); it++) {
    list<unsigned> groupnodes = it->second;
    split_further(cm, groupnodes, merged_cm, s_sets, ColorInNodes, checked);
  }

  // recursively split predecessors
  for (unordered_set<unsigned>::iterator it = pred_resps.begin();
       it != pred_resps.end(); it++) {
    split_further(cm, *it, merged_cm, s_sets, ColorInNodes, checked);
  }
}

// void insert(CFLHashMap& cm, unsigned node_a, unsigned node_b, unsigned eid,
// CFLHashMap& merged_cm, unordered_map<string, In_FastDLL<unsigned> >&
// ColorInNodes, unordered_map<unsigned, list<unsigned> >& s_sets){
void Engine::deleteE(CFLHashMap &cm, unsigned u, unsigned v, unsigned l,
                     CFLHashMap &merged_cm,
                     unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
                     unordered_map<unsigned, list<unsigned>> &s_sets) {
  // if(debug){
  //     int s_pairs_num = 0;
  //     for(unordered_map<unsigned, list<unsigned> >::iterator ssetit =
  //     s_sets.begin(); ssetit != s_sets.end(); ssetit++){
  //         s_pairs_num += (ssetit->second.size() * ssetit->second.size());
  //     }
  //     cout << "-----------\ns pairs: " << s_pairs_num << endl;
  //     cout << "total weight " << total_weight << endl;
  // }
  gettimeofday(&begin_t, NULL);
  // if(debug)
  //     cout << "deleting edge " << u << "->" << v << " " << l << ".\n";
  if (cm.HasEdgeBetween(u, v, l)) {
    ++deletion_count;
    // Detect cycles before deletion, which may itself remove a cycle edge.
    if (hasAffectedCycle(merged_cm, resp->find(u), resp->find(v))) {
      cm.DeleteEdge(u, v, l);
      rebuild(cm, merged_cm, ColorInNodes, s_sets);
      gettimeofday(&end_t, NULL);
      elapsed += ((end_t.tv_sec - begin_t.tv_sec) +
                  ((end_t.tv_usec - begin_t.tv_usec) / 1000000.0));
      return;
    }
    cm.DeleteEdge(u, v, l);
  } else {
    return;
  }
  dec_weight(u, v, l, merged_cm, ColorInNodes);
  unsigned u_prime = resp->find(u);
  unsigned v_prime = resp->find(v);
  unordered_set<string> checked1;
  split_further(cm, u_prime, merged_cm, s_sets, ColorInNodes, checked1);
  unordered_set<string> checked2;
  split_further(cm, v_prime, merged_cm, s_sets, ColorInNodes, checked2);
  // if(!merged_cm.HasEdgeBetween(u_prime, v_prime, l)){
  //     if(debug)
  //         cout << "Never happens 7";
  // }
  // string wei;
  // stringstream weiconvert;
  // weiconvert << "_" << u_prime << "_" << v_prime << "_" << l;
  // wei = weiconvert.str();
  // if(weight.find(wei) == weight.end()){
  //     if(debug)
  //         cout << "Never Happens 8.\n";
  //     weight[wei] = 1;
  // }
  // weight[wei]--;
  // if(debug)
  //     total_weight --;
  // if(weight[wei] == 0){
  //     weight.erase(wei);
  //     node2weight[u_prime].erase(wei);
  //     node2weight[v_prime].erase(wei);
  //     merged_cm.DeleteEdge(u_prime, v_prime, l);
  //     string s;
  //     stringstream convert;
  //     convert<<v_prime<<"_"<<l;
  //     s= convert.str();
  //     if(ColorInNodes[s].isInFDLL(u_prime))
  //         ColorInNodes[s].remove(u_prime);
  //}
  gettimeofday(&end_t, NULL);
  elapsed += ((end_t.tv_sec - begin_t.tv_sec) +
              ((end_t.tv_usec - begin_t.tv_usec) / 1000000.0));
}

void Engine::arrayreach(
    CFLHashMap &cm, unordered_map<string, In_FastDLL<unsigned>> &ColorInNodes,
    unordered_map<unsigned, list<unsigned>> &s_sets) {

  const unsigned NodeNum = cm.GetVtxNum();
  resp.reset(new DisjointSet(NodeNum));
  weight.clear();
  node2weight.clear();
  ColorInNodes.clear();
  s_sets.clear();
  // unsigned sedges = 0;
  In_FastDLL<string> fdll;
  // typedef unordered_map<unsigned, unsigned> colorcountmap;

  // unordered_map<string, In_FastDLL<unsigned> > ColorInNodes;

  // unordered_map<unsigned, list<unsigned> > s_sets;

  // colorcountmap colorcount;
  // vector<colorcountmap> NodeColorCount(NodeNum);
  //     unsigned edgelabels=9;

  // cout<<cm.GetEdgNum()<<endl;

  // gettimeofday(&begin_t, NULL);

  // preprocessing all nodes

  for (unsigned i = 0; i < NodeNum; i++) {
    // cout<<"doing"<<endl;
    // cout<<"degree "<<cm.GetNodeDegree(i)<<endl;

    s_sets[i].push_back(i);

    unordered_map<unsigned, Matrix1> innodes;
    // colorcountmap colorcount = NodeColorCount[i];
    cm.CheckInEdges(i, innodes);

    for (unordered_map<unsigned, Matrix1>::iterator itj = innodes.begin();
         itj != innodes.end(); ++itj) {
      unsigned j = (itj->first);
      unordered_map<unsigned, char> color = (itj->second).colors;
      // cm.CheckInColor(j, i, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {

        // add to color in nodes
        string s;
        stringstream convert;
        convert << i << "_" << (c->first);
        s = convert.str();
        if (!ColorInNodes[s].isInFDLL(j))
          ColorInNodes[s].add(j);

        // compute weight
        string wei;
        stringstream weiconvert;
        weiconvert << "_" << j << "_" << i << "_" << c->first;
        wei = weiconvert.str();
        if (weight.find(wei) == weight.end()) {
          weight[wei] = 0;
        }
        weight[wei]++;
        node2weight[j].insert(wei);
        node2weight[i].insert(wei);
        // if(debug)
        //     total_weight ++;
      }
    }
  }

  // insert node if indegree > 1

  for (unordered_map<string, In_FastDLL<unsigned>>::iterator it =
           ColorInNodes.begin();
       it != ColorInNodes.end(); ++it) {
    if ((it->second).size() > 1) {
      fdll.add(it->first);
    }
  }

  // if(debug){
  //     int s_pairs_num = 0;
  //     for(unordered_map<unsigned, list<unsigned> >::iterator ssetit =
  //     s_sets.begin(); ssetit != s_sets.end(); ssetit++){
  //         s_pairs_num += (ssetit->second.size() * ssetit->second.size());
  //     }
  //     cout << "-----------\nbefore merge\ns pairs: " << s_pairs_num << endl;
  //     cout << "total weight " << total_weight << endl;
  // }
  // main procedure
  mainproc(fdll, ColorInNodes, cm, s_sets, NodeNum);

  // gettimeofday(&end_t, NULL);
  // elapsed += ((end_t.tv_sec - begin_t.tv_sec) + ((end_t.tv_usec -
  // begin_t.tv_usec) / 1000000.0)); if(debug){
  //     int s_pairs_num = 0;
  //     for(unordered_map<unsigned, list<unsigned> >::iterator ssetit =
  //     s_sets.begin(); ssetit != s_sets.end(); ssetit++){
  //         s_pairs_num += (ssetit->second.size() * ssetit->second.size());
  //     }
  //     cout << "s pairs: " << s_pairs_num << endl;
  //     //cout << "total weight " << total_weight << endl;
  // }
}

int Engine::arrayversion(bool inc_version, string init_dotfile,
                         string input_seqfile) {
  // if(debug)
  //     cout<<version<<endl;
  // cout << "working on " << input_seqfile << endl;
  gettimeofday(&begin_t, NULL);
  unordered_map<string, unsigned> NodeID;
  unordered_map<string, unsigned> EdgeID;
  queue<string> input_queue;
  int nid_count = 0;
  int eid_count = 0;

  string from_id, to_id;
  string operator_str, eid_str;

  ifstream inf2(init_dotfile);
  if (!inf2)
    throw runtime_error("cannot open initial graph: " + init_dotfile);
  string linestr;
  while (getline(inf2, linestr)) {
    SimpleDotParser parser;
    if (!parser.IsEdge(linestr))
      continue;
    eid_str = parser.GetEdgeLabel(linestr);
    if (eid_str.find("op") != 0 && eid_str.find("cp") != 0)
      throw invalid_argument("expected op/cp artifact label");
    auto endpoints = parser.ReturnNodePair(linestr, "->");
    from_id = endpoints.first;
    to_id = endpoints.second;
    // cout << "find in initial graph: " << from_id << " " << to_id << " " <<
    // eid_str << endl;
    if (NodeID.find(from_id) == NodeID.end()) {
      NodeID[from_id] = nid_count;
      // cout << "new node ID: " << from_id << ".\n";
      nid_count++;
    }
    if (NodeID.find(to_id) == NodeID.end()) {
      NodeID[to_id] = nid_count;
      // cout << "new node ID: " << to_id << ".\n";
      nid_count++;
    }
    if (EdgeID.find(eid_str.substr(1)) == EdgeID.end()) {
      EdgeID[eid_str.substr(1)] = eid_count;
      eid_count++;
    }
  }

  ifstream inf(input_seqfile);
  if (!inf)
    throw runtime_error("cannot open update sequence: " + input_seqfile);
  while (getline(inf, linestr)) {
    istringstream record(linestr);
    if (!(record >> operator_str))
      continue;
    if (operator_str.front() == '#' || operator_str.find("//") == 0)
      continue;
    string extra;
    if (!(record >> from_id >> to_id >> eid_str) || (record >> extra) ||
        (operator_str != "A" && operator_str != "D") ||
        (eid_str.find("op") != 0 && eid_str.find("cp") != 0))
      throw invalid_argument("invalid artifact update record");
    input_queue.push(operator_str + " " + from_id + " " + to_id + " " +
                     eid_str);
    if (NodeID.find(from_id) == NodeID.end()) {
      NodeID[from_id] = nid_count;
      // cout << "new node ID: " << from_id << ".\n";
      nid_count++;
    }
    if (NodeID.find(to_id) == NodeID.end()) {
      NodeID[to_id] = nid_count;
      // cout << "new node ID: " << to_id << ".\n";
      nid_count++;
    }
    if (EdgeID.find(eid_str.substr(1)) == EdgeID.end()) {
      EdgeID[eid_str.substr(1)] = eid_count;
      eid_count++;
    }
  }

  unsigned NodeNum = NodeID.size();
  node_names.resize(NodeNum);
  for (const auto &entry : NodeID)
    node_names[entry.second] = entry.first;
  // if(debug)
  //     cout << "All sequence node number: " << NodeNum << endl;
  SimpleDotParser dotparser;
  CFLHashMap cm1(NodeNum);
  dotparser.BuildMyHashTable(init_dotfile, NodeID, EdgeID, cm1);
  CFLHashMap orig_cm(cm1, NodeNum);
  unordered_map<string, In_FastDLL<unsigned>> ColorInNodes;
  unordered_map<unsigned, list<unsigned>> s_sets;
  arrayreach(cm1, ColorInNodes, s_sets);
  gettimeofday(&end_t, NULL);
  // elapsed += ((end_t.tv_sec - begin_t.tv_sec) + ((end_t.tv_usec -
  // begin_t.tv_usec) / 1000000.0)); if(!debug){
  //     cout << "Initial time: " << elapsed << endl;
  // }
  elapsed = 0;

  // int seqid = 0;
  // int queuesize_byten = input_queue.size() / 10;
  // if(queuesize_byten == 0){
  //     queuesize_byten++;
  // }
  // cout << "input queue size: " << queuesize_byten << endl;
  while (!input_queue.empty()) {
    ++update_count;
    // seqid++;
    // if((!debug) && seqid % queuesize_byten == 0){
    //     cout << "----------------\nseq id: " << seqid << endl;
    //     cout<<"Runtime: "<<elapsed<<endl;
    // }
    // if(debug){
    //     cout << "----------------\nseq id: " << seqid << endl;
    // }
    stringstream ss(input_queue.front());
    input_queue.pop();
    ss >> operator_str >> from_id >> to_id >> eid_str;
    // cout << operator_str << "---" << from_id << "---" << to_id << "---" <<
    // eid_str << endl;
    unsigned startnid;
    unsigned endnid;
    unsigned edgeid = EdgeID[eid_str.substr(1)];
    if (eid_str.find("op") == 0) {
      startnid = NodeID[from_id];
      endnid = NodeID[to_id];
      // cm.InsertEdge(NodeID[from_id], NodeID[to_id],
      // EdgeID[eid_str.substr(1)]);
    } else if (eid_str.find("cp") == 0) {
      startnid = NodeID[to_id];
      endnid = NodeID[from_id];
      // cm.InsertEdge(NodeID[to_id], NodeID[from_id],
      // EdgeID[eid_str.substr(1)]);
    }
    bool isInsert = (operator_str.find("A") != string::npos);
    if (inc_version) {
      if (isInsert) {
        insert(orig_cm, startnid, endnid, edgeid, cm1, ColorInNodes, s_sets);
      } else {
        deleteE(orig_cm, startnid, endnid, edgeid, cm1, ColorInNodes, s_sets);
      }
      // if(debug){
      //     int s_pairs_num = 0;
      //     for(unordered_map<unsigned, list<unsigned> >::iterator ssetit =
      //     s_sets.begin(); ssetit != s_sets.end(); ssetit++){
      //         s_pairs_num += (ssetit->second.size() * ssetit->second.size());
      //     }
      //     cout << "s pairs: " << s_pairs_num << endl;
      //     //cout << "total weight " << total_weight << endl;
      // }
    } else {
      if (isInsert) {
        if (!orig_cm.HasEdgeBetween(startnid, endnid, edgeid))
          ++insertion_count;
        orig_cm.InsertEdge(startnid, endnid, edgeid);
      } else {
        if (orig_cm.HasEdgeBetween(startnid, endnid, edgeid))
          ++deletion_count;
        orig_cm.DeleteEdge(startnid, endnid, edgeid);
      }
      CFLHashMap tmpcm(orig_cm, NodeNum);
      unordered_map<string, In_FastDLL<unsigned>> tmpColorInNodes;
      unordered_map<unsigned, list<unsigned>> tmps_sets;

      gettimeofday(&begin_t, NULL);
      arrayreach(tmpcm, tmpColorInNodes, tmps_sets);
      gettimeofday(&end_t, NULL);
      elapsed += ((end_t.tv_sec - begin_t.tv_sec) +
                  ((end_t.tv_usec - begin_t.tv_usec) / 1000000.0));
    }
  }
  final_edge_count = orig_cm.GetEdgNum();
  return 0;
}

} // namespace lotus::cfl::dynamic_dyck::weighted_quotient
