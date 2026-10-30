#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
using namespace std;

#define pb push_back
#define mp make_pair
#define f first
#define s second
#define pii pair<int, int>

int error_node;

// procedure info
int proc_num = 0;
unordered_map<string, int> proc;
unordered_map<int, int> entryn;
unordered_map<int, int> enforce_xp;
unordered_map<int, int> exitn;
unordered_map<int, int> is_exit;
unordered_map<int, vector<int>> parameters;
unordered_map<int, int> return_num;

// general node info
int node_num = 0;
unordered_map<int, int> node_to_proc;
unordered_map<int, int> node_to_next;
unordered_map<int, string> node_to_type;

// specific node info

// dead nodes
struct dead_node {
  vector<string> var_names;
};

unordered_map<int, dead_node> dead;

// call nodes
struct call_node {
  string name;
  // array of indexes of expressions
  vector<int> expressions;
};

unordered_map<int, call_node> call;

// assert nodes (and assume and constrain)
struct assert_node {
  int xp_id;
};
unordered_map<int, assert_node> assert;

// assign_call nodes
struct assign_call_node {
  vector<string> var_names;
  string name;
  vector<int> expressions;
};
unordered_map<int, assign_call_node> assign_call;

// parallel_assign nodes
struct parallel_assign_node {
  vector<string> var_names;
  vector<int> expressions;
  int optional_constraint;
};
unordered_map<int, parallel_assign_node> parallel_assign;

// return nodes
struct return_node {
  vector<int> expressions;
};
unordered_map<int, return_node> returnn;

// skip has no node

// goto nodes
struct goto_node {
  vector<string> labels;
};
unordered_map<int, goto_node> goton;

// while nodes
struct while_node {
  int xp_id;
  int fv;
};
unordered_map<int, while_node> whilen;

// if nodes
struct if_node {
  vector<int> expressions;
  vector<int> fv;
  vector<int> lv;
};
unordered_map<int, if_node> ifn;

// var info
int var_num = 0;
map<pair<int, string>, int> var;
unordered_map<int, int> var_to_proc;
map<pair<int, string>, int> name_to_var;

int exp_num = 0;
unordered_map<int, string> xp_rec;
// type, procedure, var name
unordered_map<int, pair<string, pair<int, string>>> xp_params;

// label info
int lab_num = 0;
map<pair<int, string>, int> name_to_lab;
unordered_map<int, int> lab_to_node;
unordered_map<int, int> lab_no_enforce;

void print_vars(int my_proc, vector<string> identifiers) {
  cout << identifiers.size() << " ";
  for (auto x : identifiers) {
    if (name_to_var.find(mp(my_proc, x)) != name_to_var.end()) {
      cout << name_to_var[mp(my_proc, x)] << " ";
    } else
      cout << name_to_var[mp(-1, x)] << " ";
  }
}

void print_labels(int my_proc, vector<string> labels) {
  cout << labels.size() << " ";
  for (auto x : labels) {
    cout << name_to_lab[mp(my_proc, x)] << " ";
  }
}

void print_expressions(int my_proc, vector<int> expressions) {
  cout << expressions.size() << " ";
  for (auto x : expressions) {
    cout << x << " ";
  }
}

void read_ints(vector<int> &ans) {
  int n;
  cin >> n;
  for (int i = 0; i < n; i++) {
    int x;
    cin >> x;
    ans.pb(x);
  }
}

void read_strings(vector<string> &ans) {
  int n;
  cin >> n;
  for (int i = 0; i < n; i++) {
    string x;
    cin >> x;
    ans.pb(x);
  }
}

map<int, vector<int>> succ;

int32_t main() {
  ios::sync_with_stdio(false);
  cin.tie(NULL);

  string qq;
  int my_id;
  string my_type;
  while (cin >> qq) {
    // cout<<"entering per "<<qq<<endl;
    if (qq == "procedure") {
      proc_num++;
      // exit node
      node_num++;

      int i;
      string s;
      int my_entry, my_exit, my_enforce;
      cin >> i >> s >> my_entry >> my_exit >> my_enforce;

      // map procedure name to index
      proc[s] = i;
      // entry node of procedure
      entryn[i] = my_entry;
      // exit node of procedure
      exitn[i] = my_exit;
      is_exit[my_exit] = 1;
      node_to_type[my_exit] = "exit";
      node_to_proc[my_exit] = i;
      // maps enforce
      enforce_xp[i] = my_enforce;

    } else if (qq == "node") {
      node_num++;

      int my_proc;
      int i;
      cin >> my_proc >> i;
      node_to_proc[i] = my_proc;
      string type;
      cin >> type;
      node_to_type[i] = type;
      // cout<<"found node "<<i<<" of type "<<type<<endl;
      if (type == "dead") {
        dead_node curr;
        read_strings(curr.var_names);
        dead[i] = curr;
      } else if (type == "call") {
        node_num++;
        call_node curr;
        cin >> curr.name;
        read_ints(curr.expressions);
        call[i] = curr;
        node_to_proc[i + 1] = my_proc;
        node_to_type[i + 1] = "skip";
      } else if (type == "assert" || type == "assume" || type == "constrain") {
        assert_node curr;
        cin >> curr.xp_id;
        assert[i] = curr;
      } else if (type == "assign_call") {
        node_num++;
        assign_call_node curr;
        read_strings(curr.var_names);
        cin >> curr.name;
        read_ints(curr.expressions);
        assign_call[i] = curr;
        node_to_proc[i + 1] = my_proc;
        node_to_type[i + 1] = "skip";
      } else if (type == "parallel_assign") {
        parallel_assign_node curr;
        read_strings(curr.var_names);
        read_ints(curr.expressions);
        cin >> curr.optional_constraint;
        parallel_assign[i] = curr;
      } else if (type == "return") {
        return_node curr;
        read_ints(curr.expressions);
        returnn[i] = curr;
      } else if (type == "skip") {
      } else if (type == "goto") {
        goto_node curr;
        read_strings(curr.labels);
        goton[i] = curr;
      } else if (type == "while") {
        while_node curr;
        cin >> curr.xp_id >> curr.fv;
        whilen[i] = curr;
      } else if (type == "if") {
        if_node curr;
        int num;
        cin >> num;
        for (int i = 0; i < num; i++) {
          int my_xp, my_fv, my_lv;
          cin >> my_xp >> my_fv >> my_lv;
          curr.expressions.pb(my_xp);
          curr.fv.pb(my_fv);
          curr.lv.pb(my_lv);
        }
        ifn[i] = curr;
      } else if (type == "error") {
        error_node = i;
        node_to_next[i] = -1;
      }
    } else if (qq == "var" || qq == "parameter") {
      var_num++;

      int my_proc;
      int i;
      string name;
      cin >> my_proc >> i >> name;
      var_to_proc[i] = my_proc;
      name_to_var[mp(my_proc, name)] = i;
      if (qq == "parameter") {
        parameters[my_proc].pb(i);
      }
    } else if (qq == "label") {
      lab_num++;
      int my_proc, i;
      string name;
      int my_node;
      cin >> my_proc >> i >> name >> my_node;
      lab_to_node[i] = my_node;
      name_to_lab[mp(my_proc, name)] = i;
      if (name.substr(0, 10) == "no_enforce") {
        lab_no_enforce[i] = 1;
      }
    } else if (qq == "next") {
      int my_proc, i, j;
      cin >> my_proc >> i >> j;
      node_to_next[i] = j;
    } else if (qq == "xp") {
      exp_num++;
      cin >> my_id >> my_type;
      if (my_type == "var" || my_type == "primed") {
        xp_params[my_id].first = my_type;
        cin >> xp_params[my_id].s.f >> xp_params[my_id].s.s;
      } else {
        int params = 2;
        if (my_type == "nondet" || my_type == "true" || my_type == "false")
          params = 0;
        else if (my_type == "not" || my_type == "int")
          params = 1;
        string val = my_type;
        for (int j = 0; j < params; j++) {
          val.pb(' ');
          string my_param;
          cin >> my_param;
          val += my_param;
        }
        xp_rec[my_id] = val;
      }
    } else if (qq == "return") {
      int my_proc, val;
      cin >> my_proc >> val;
      return_num[my_proc] = val;
    } else {
      assert(0);
    }
  }

  for (int i = node_num - 1; i >= 0; i--) {
    if (node_to_type[i] == "if") {
      for (auto x : ifn[i].lv) {
        node_to_next[x] = node_to_next[i];
      }
    }
  }

  for (int i = 0; i < node_num; i++) {
    if (node_to_type[i] == "call" || node_to_type[i] == "assign_call") {
      node_to_next[i + 1] = node_to_next[i];
      node_to_next[i] = i + 1;
    }
  }
  // cout<<"end preprocessing"<<endl;

  // A line containing four integers: "n_G n_H n_E n_V n_L"
  cout << node_num << " " << proc_num << " " << exp_num << " " << var_num << " "
       << lab_num << "\n";

  // A line containing one integer: the error node
  cout << error_node << "\n";

  // cout<<"procedures of nodes"<<endl;
  // A line of n_G integers denoting the procedure that node i lies in.
  for (int i = 0; i < node_num; i++) {
    cout << node_to_proc[i] << " ";
  }
  cout << "\n";

  // cout<<"procedures of variables"<<endl;
  //> A line of n_V integers the procedure in which i is a local variable.
  for (int i = 0; i < var_num; i++) {
    cout << var_to_proc[i] << " ";
  }
  cout << "\n";

  // cout<<"procs entry statement"<<endl;
  //> A line of n_H integers the first statement (node) in procedure i.
  for (int i = 0; i < proc_num; i++) {
    cout << entryn[i] << " ";
  }
  cout << "\n";

  // cout<<"procs exit statement"<<endl;
  //> A line of n_H integers the exit node in procedure i.
  for (int i = 0; i < proc_num; i++) {
    cout << exitn[i] << " ";
  }
  cout << "\n";

  // cout<<"procs return num"<<endl;
  //> A line of n_H integers the number of arguments that procedure i returns.
  for (int i = 0; i < proc_num; i++) {
    cout << return_num[i] << " ";
  }
  cout << "\n";

  // cout<<"parameters"<<endl;
  //> n_H lines the number of parameters that procedure i takes and those
  // parameters in order.
  for (int i = 0; i < proc_num; i++) {
    cout << parameters[i].size() << " ";
    for (auto x : parameters[i])
      cout << x << " ";
    cout << "\n";
  }

  // A line of n_H integers the enforce of procedure i
  // cout<<"enforces"<<endl;
  //> n_H lines the number of parameters that procedure i takes and those
  // parameters in order.
  for (int i = 0; i < proc_num; i++) {
    cout << enforce_xp[i] << " ";
  }
  cout << "\n";

  // cout<<"label destinations"<<endl;
  // A line of n_L integers the node that each label goes to
  for (int i = 0; i < lab_num; i++) {
    cout << lab_to_node[i] << " ";
  }
  cout << "\n";

  // cout<<"label no enforce"<<endl;
  // A line of n_L integers 1 if the i'th label begins with the prefix
  // "no_enforce"
  for (int i = 0; i < lab_num; i++) {
    cout << lab_no_enforce[i] << " ";
  }
  cout << "\n";

  // cout<<"next nodes"<<endl;
  // n_G lines, the i'th of them is Next(i)
  for (int i = 0; i < node_num; i++) {
    int nxt = node_to_next[i];
    if (node_to_type[i] == "error")
      cout << -1 << " ";
    else if (is_exit[i])
      cout << -1 << " ";
    else if (nxt == -1)
      cout << exitn[node_to_proc[i]] << " ";
    else
      cout << node_to_next[i] << " ";
  }
  cout << "\n";

  // cout<<"node info"<<endl;
  // n_G lines, the i'th of them is associated control information and
  // evaluation information
  for (int i = 0; i < node_num; i++) {
    string my_type = node_to_type[i];
    cout << my_type << " ";
    if (my_type == "exit") {
      cout << node_to_proc[i] << " ";
      // exit nodes have no edges
    } else if (my_type == "dead") {
      print_vars(node_to_proc[i], dead[i].var_names);
    } else if (my_type == "call") {
      cout << proc[call[i].name] << " ";
      print_expressions(node_to_proc[i], call[i].expressions);
    } else if (my_type == "assert" || my_type == "assume" ||
               my_type == "constrain") {
      cout << assert[i].xp_id << " ";
    } else if (my_type == "assign_call") {
      print_vars(node_to_proc[i], assign_call[i].var_names);
      cout << proc[assign_call[i].name] << " ";
      print_expressions(node_to_proc[i], assign_call[i].expressions);
    } else if (my_type == "parallel_assign") {
      print_vars(node_to_proc[i], parallel_assign[i].var_names);
      print_expressions(node_to_proc[i], parallel_assign[i].expressions);
      cout << parallel_assign[i].optional_constraint << " ";
    } else if (my_type == "return") {
      print_expressions(node_to_proc[i], returnn[i].expressions);
    } else if (my_type == "skip") {
    } else if (my_type == "goto") {
      print_labels(node_to_proc[i], goton[i].labels);
    } else if (my_type == "while") {
      cout << whilen[i].xp_id << " ";
      cout << whilen[i].fv << " ";
    } else if (my_type == "if") {
      cout << ifn[i].expressions.size() << " ";
      bool is_fi = 0;
      for (size_t j = 0; j < ifn[i].expressions.size(); j++) {
        cout << ifn[i].expressions[j] << " " << ifn[i].fv[j] << " ";
      }
    }
    cout << "\n";
  }

  // n_E lines describing how to access the value of expression i
  for (int i = 0; i < exp_num; i++) {
    if (xp_rec.find(i) != xp_rec.end())
      cout << xp_rec[i];
    else {
      cout << xp_params[i].first << " ";
      int my_proc = xp_params[i].second.first;
      string x = xp_params[i].second.second;
      if (name_to_var.find(mp(my_proc, x)) != name_to_var.end()) {
        cout << name_to_var[mp(my_proc, x)];
      } else
        cout << name_to_var[mp(-1, x)];
    }
    cout << " \n";
  }

  // output the cfg

  for (int i = 0; i < node_num; ++i) {
    if (node_to_type[i] == "dead") {
      succ[i].push_back(node_to_next[i]);
    } else if (node_to_type[i] == "call") {
      // call-to-return-site edge
      succ[i].push_back(node_to_next[i]);
      assert(node_to_type[node_to_next[i]] == "skip");
      // interprocedural edges
      // call-to-start edge
      succ[i].push_back(entryn[proc[call[i].name]]);
      // exit-to-return-site edge
      succ[exitn[proc[call[i].name]]].push_back(node_to_next[i]);
    } else if (node_to_type[i] == "assign_call") {
      // call-to-return-site edge
      succ[i].push_back(node_to_next[i]);
      assert(node_to_type[node_to_next[i]] == "skip");
      // interprocedural edges
      // call-to-start edge
      succ[i].push_back(entryn[proc[assign_call[i].name]]);
      // exit-to-return-site edge
      succ[exitn[proc[assign_call[i].name]]].push_back(node_to_next[i]);
    } else if (node_to_type[i] == "parallel_assign") {
      succ[i].push_back(node_to_next[i]);
    } else if (node_to_type[i] == "assert" || node_to_type[i] == "assume") {
      // if the assertion is true
      succ[i].push_back(node_to_next[i]);
      // if the assertion is false
      succ[i].push_back(error_node);
    } else if (node_to_type[i] == "constrain") {
      // if the constraint is true
      succ[i].push_back(node_to_next[i]);
      // if the constraint is false
      succ[i].push_back(error_node);
    } else if (node_to_type[i] == "return") {
      succ[i].push_back(exitn[node_to_proc[i]]);
    } else if (node_to_type[i] == "skip") {
      succ[i].push_back(node_to_next[i]);
    } else if (node_to_type[i] == "goto") {
      for (auto &curr_label : goton[i].labels) {
        int lab_id = name_to_lab[mp(node_to_proc[i], curr_label)];
        succ[i].push_back(lab_to_node[lab_id]);
      }
    } else if (node_to_type[i] == "while") {
      // if the loop condition is true
      succ[i].push_back(whilen[i].fv);
      // if the loop condition is false
      succ[i].push_back(node_to_next[i]);
    } else if (node_to_type[i] == "if") {
      for (auto &curr_node : ifn[i].fv) {
        succ[i].push_back(curr_node);
      }
    }
  }

  // only modify the rest of this to fix errors

  vector<pair<int, int>> CFGedges;
  vector<pair<int, int>> callGraphEdges;
  for (int u = 0; u < node_num; ++u)
    for (int v : succ[u]) {
      CFGedges.push_back(make_pair(u, v));
      if (node_to_proc[v] != -1)
        callGraphEdges.push_back(make_pair(node_to_proc[u], node_to_proc[v]));
    }

  // output at the end, I'll just use it for sanity checks
  cout << CFGedges.size() << '\n';
  for (auto pr : CFGedges)
    cout << pr.first << " " << pr.second << "\n";

  // output the CFGs for the PACE treewidth solver
  {
    // 1-based input to be fed to the treewidth PACE solver, make it 0-based in
    // our code

    ofstream out("treewidth_solver_input.txt");

    set<pii> treewidthEdges;

    for (auto &edge : CFGedges)
      if (node_to_proc[edge.first] == node_to_proc[edge.second])
        treewidthEdges.insert(make_pair(min(edge.first, edge.second),
                                        max(edge.first, edge.second)));

    for (int i = 0; i < node_num; ++i)
      treewidthEdges.insert(make_pair(i, node_num));

    // assert(diff == proc_num);

    out << "p tw " << node_num + 1 << ' ' << treewidthEdges.size() << '\n';

    // 1-based connected graph with no self loops of multiple edges to fit the
    // PACE solver

    for (auto pr : treewidthEdges) {
      out << pr.first + 1 << ' ' << pr.second + 1 << '\n';
    }
  }

  // output the call graph for the PACE treedepth solver
  {

    // 1-based input to be fed to the treedepth PACE solver, make it 0-based in
    // our code

    ofstream out("treedepth_solver_input.txt");

    set<pii> PACEedges;

    // add a dummy node to ensure the graph is connected, remove in our code
    for (int u = 0; u < proc_num; ++u) {
      PACEedges.insert(make_pair(u, proc_num));
    }
    for (auto edge : callGraphEdges) {
      int u = edge.first, v = edge.second;
      if (u != v)
        PACEedges.insert(make_pair(min(u, v), max(u, v)));
    }

    out << "p tdp " << proc_num + 1 << ' ' << (PACEedges.size()) << '\n';

    // 1-based connected graph with no self loops or multiple edges to fit the
    // PACE solver

    for (auto pr : PACEedges) {
      out << pr.first + 1 << ' ' << pr.second + 1 << '\n';
    }
  }

  return 0;
}
