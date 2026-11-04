#pragma once

#include "Dataflow/DemandAPA/Algebra.h"
#include "Dataflow/DemandAPA/ApaInstance.h"
#include "Dataflow/DemandAPA/Project.h"
#include "Dataflow/DemandAPA/Support.h"
#include "bdd.h" // from buddy?

// won't inherit because this one is too complicated
class BpReader {
public:
  /*
   * Stuff from input
   */
  int n_G, n_H, n_E, n_V, n_L, errNode, errProc;
  vi procOfNode, procOfVar, entryOf, exitOf, nextNode, enforce, labelDest,
      numReturns, noEnforceLabel;
  V<string> stmtType, exprType;
  vvi params, exprOperands;
  // Information specific to each statement
  vvi deadVars; // if i in [0, n_G) is a not a dead statement, deadVars is
                // empty, otherwise it's the list of dead vars

  // if i in [0, n_G) is not a call/assign_call, calls[i] = -1, callsLHS[i] =
  // callsArgs[i] = {}, otherwise: calls[i] = fun_id (in [0, n_H)), the id of
  // called function callsArgs[i] is the list of expression id's for the
  // arguments of the function call
  //  callsLHS[i] is empty if i is not an assign_call statement, otherwise it's
  //  a list id's of the variable on the LHS (in [0, n_V))
  vi calls;
  vvi callsLHS;
  vvi callsArgs;

  // if i is a parallel assignment node (and the LHS is not a function call,
  // because this is handled by assign_call), then parallelAssigns[i] = {LHS,
  // RHS} where LHS is a list of variables (in [0, n_V)) and RHS is a list of
  // expressions (in [0, n_E)) otherwise, parallelAssigns[i] = {{}, {}}
  V<pair<vi, vi>> parallelAssigns;
  vi oc; // id of expression in the optional constrain, currently can only exist
         // for parallel assigns

  vi assumeOrAssertOrConstrain; // if i is an assume/assert node,
                                // assumeOrAssertOrConstrain[i] contains
                                // expression id of the argument
  vvi returnExprs; // if i is a return node, returnExprs[i] is a list of
                   // expressions returned in i, o.w. returnExprs is empty
  vvi gotoLabels;  // if i is a goto statement, gotoLabels[i] is the list of
                   // labels that i goes to
  vi whileDecider; // if i is a while node, whileDecider[i] is an expression id
                   // of its decider, o.w. -1
  vi firstNodeInWhileBody; // if i is a while node, firstNodeInWhileBody[i] is
                           // the id of the first statement in its body, o.w. -1
  V<V<pii>> ifBranches; // if i in an if node, ifBranches[i] is a list of pairs,
                        // the i'th of them is (exp_id_i, first_id_i) where
                        // exp_id_i is the expression id of the condition in
                        // i'th branch, first_id_i is the first statement in
                        // i'th branch. Last branch may have exp_id_i = -1
                        // indicating that it's an else (not else-if)
  /*
   * New stuff
   */

  /*
   * Variable representation: the bdd has 4 sets of variables, each having GL
   * boolean variables, the variable set is {x_0, .. x_{GL-1}, x'_0, ..
   * x'_{GL-1}, x''_0, .. x''_{GL-1}, x'''_0, .. x'''_{GL-1}} Given an index i:
   *  - We obtain a formula x_i by calling bdd_ithvar(var(i))
   *  - We obtain a formula x'_i by calling bdd_ithvar(varPrimed(i))
   *  - We obtain a formula x''_i by calling bdd_ithvar(varDoublePrimed(i))
   *  - We obtain a formula x'''_i by calling bdd_ithvar(varTriplePrimed(i))
   *
   * A transfer function is a formula/function of non-primed and primed
   * variables that is associated with  an edge in the CFGs. The way a BDD
   * variable is interpreted is based on which function the endpoints of the
   * transfer function lies in. For a transfer function on edge (u, v):
   *  - x_0 ... x_{GL-1} correspond to the pre-state of the global variables and
   * local variables in procOfNode[u]
   *  - x'_0 ... x'_{GL-1} correspond to the post-state of the global variables
   * and local variables in procOfNode[v] The mapping from variables in the
   * program to variables in the bdd is as follows: (the mapping is actually
   * important since good mappings can give more efficient BDDs but this is the
   * same variable ordering as NPA-TP, so should be good) Every variable v in
   * our program has a corresponding index i := mapVarToBDDIdx[v], then we
   * obtain a formula of that variable or its primed version as shown above. for
   * global variables, globals[j] is mapped to index j for a procedure p,
   * locals[p][j] is mapped to index G + j If we are dealing with a transfer
   * function f(omega_1, omega_2) on edge (u, v) then
   *  - for 0 <= i < G, bdd_ithvar(var(i)) and bdd_ithvar(varPrimed(i))
   * correspond to the global variable globals[i] and its primed version.
   *  - for G <= i < GL, bdd_ithvar(var(i)) correspond to pre-state local
   * variable locals[procOfNode[u]][i - G] in procOfNode[u]
   *  - for G <= i < GL, bdd_ithvar(varPrimed(i)) correspond to post-state local
   * variable locals[procOfNode[v]][i - G] in procOfNode[v]
   *  - Other variables are reserved for internal use
   */

  /*
   * How calls and returns are handled:
   * Call-to-return-site edge has the transformer false
   * Call-to-start edge has a transformer of f(omega_1, omega_2) that:
   * 1. sets the parameters in the called function to the value of the
   * expressions in the supplied arguments
   * 2. maintains the values for all global variables
   * 3. doesn't add any constraints on other local variables in the called
   * function, to mimic the nondeterminism of their initial values
   *
   * Return-to-exit edge (not that a function might have multiple returns, all
   * of them go to the exit node of the function) has a transformer f(omega_1,
   * omega_2) that:
   * 1. sets the local variables with indices G..G+numReturns[currProc]-1 to be
   * the expressions passed to the return statement. (We make sure that L >= max
   * numReturns[p], so there'll always be enough local vars)
   * 2. Leaves other variables unchanged
   *
   * Exit-to-return-site edge has a transformer of f(omega_1, omega_2) that:
   * 1. sets the LHS variables to the local variables in the called-function
   * with indices G..G+numReturns[calledProc]-1
   * 2. maintains the values for all global variables (that don't appear in LHS
   * in previous case)
   * 3. doesn't add any constraints on other local variables in the caller
   * function, and this non-determinism is resolved by the Project operator
   */

  vi noEnforceStmt; // boolean, for each statement, it tells whether or not it
                    // has a no-enforce label
  vi globals;       // list of global variables
  vvi locals; // for every procedure p, locals[p] is the set of local variables
              // in p (params[p] is a subset of it)
  V<bdd> enforceFormula; // for every procedures p, enforceFormula[p] holds a
                         // formula for the enforce expression
  V<V<pair<int, bdd>>>
      succ; // for a statement u, succ[u] is the lest of neighbours of u in the
            // CFG, as well the the edge weight (a boolean formula)
  vi mapVarToBDDIdx; // for variable with id v, mapVarToBDDIdx[v] is an integer
                     // in [0, GL) (it's in [0, G) if v is global) and otherwise
                     // it corresponds to some local variable in the current
                     // procedure

  // stuff related to the instance we'll ultimately return
  vector<pair<int, int>> edgeListH;
  vector<pair<pair<int, int>, bdd>> edgeListG;
  vector<int> vertexTypeG, procOf;
  vector<vector<vector<int>>> TWD;
  vector<int> TWD_root;
  vector<vector<int>> TWD_par;
  int TDD_root;
  vector<int> par_tdH;
  vector<ll> weightSameBag, weightCentroid, weightCallsG;

  BpReader(string pathToFile, string TWDpath, string TDDpath) {
    db(pathToFile, TWDpath, TDDpath);
    ifstream in(pathToFile);

    in >> n_G >> n_H >> n_E >> n_V >> n_L;

    db(n_G, n_H);

    in >> errNode;

    inputFirstFewLines(in);

    //		cout << "Read the first few lines" << endl;

    inputStmtsDesc(in);

    //		cout << "Read the statements description" << endl;

    inputExprsDesc(in);

    //		cout << "Read the expressions description" << endl;

    addErrorNodeProc();

    //		cout << "Added error node" << endl;

    buildCFGs(in);

    //		cout << "Read boolean program and verified some stuff!!" <<
    // endl;

    prepareStuff();

    readTWD(TWDpath);

    //		cout << "Read TWD" << endl;

    readTDD(TDDpath);

    //		cout << "Read TDD" << endl;

    setWeights();
  }

  void inputFirstFewLines(ifstream &in) {

    procOfNode.assign(n_G, -1);

    for (int i = 0; i < n_G; ++i) {
      in >> procOfNode[i];
      //			db(i, n_G, n_H, errNode, procOfNode[i]);
      assert((i == errNode && procOfNode[i] == -1) ||
             (i != errNode && procOfNode[i] >= 0 && procOfNode[i] < n_H));
    }

    procOfVar.assign(n_V, -2);

    for (int i = 0; i < n_V; ++i) {
      in >> procOfVar[i];
      assert(procOfVar[i] >= -1 && procOfVar[i] < n_H);
    }

    entryOf.assign(n_H, -1);

    for (int i = 0; i < n_H; ++i) {
      in >> entryOf[i];
      assert(procOfNode[entryOf[i]] == i);
      assert(entryOf[i] != errNode);
      assert(entryOf[i] >= 0 && entryOf[i] < n_G);
    }

    exitOf.assign(n_H, -1);

    for (int i = 0; i < n_H; ++i) {
      in >> exitOf[i];
      assert(procOfNode[exitOf[i]] == i);
      assert(exitOf[i] != errNode);
      assert(exitOf[i] >= 0 && exitOf[i] < n_G);
    }

    numReturns.assign(n_H, -1);

    for (int i = 0; i < n_H; ++i) {
      in >> numReturns[i];
      assert(numReturns[i] >= 0);
    }

    params.assign(n_H, vi());

    for (int p = 0; p < n_H; ++p) {
      int Sz = 0;
      in >> Sz;
      params[p].assign(Sz, 0);
      for (int i = 0; i < Sz; ++i) {
        in >> params[p][i];
        assert(params[p][i] >= 0 && params[p][i] < n_V);
        assert(procOfVar[params[p][i]] == p);
      }
      //			db(params[p]);
    }

    enforce.assign(n_H, 0);

    for (int p = 0; p < n_H; ++p) {
      in >> enforce[p];
      assert(enforce[p] == -1 || (enforce[p] >= 0 && enforce[p] < n_E));
    }

    labelDest.assign(n_L, -1);

    for (int l = 0; l < n_L; ++l) {
      in >> labelDest[l];
      assert(labelDest[l] >= 0 && labelDest[l] < n_G);
    }

    noEnforceLabel.assign(n_L, 0);

    for (int i = 0; i < n_L; ++i) {
      in >> noEnforceLabel[i];
      assert(noEnforceLabel[i] == 0 || noEnforceLabel[i] == 1);
    }

    nextNode.assign(n_G, -1);

    for (int i = 0; i < n_G; ++i) {
      in >> nextNode[i];
      if (i == errNode || i == exitOf[procOfNode[i]])
        assert(nextNode[i] == -1);
      else
        assert(nextNode[i] >= 0 && nextNode[i] < n_G &&
               procOfNode[i] == procOfNode[nextNode[i]]);
    }
  }

  void inputStmtsDesc(ifstream &in) {
    stmtType.assign(n_G, "");
    deadVars.assign(n_G, vi());
    calls.assign(n_G, -1);
    callsArgs.assign(n_G, vi());
    callsLHS.assign(n_G, vi());
    parallelAssigns.assign(n_G, mp(vi(), vi()));
    oc.assign(n_G, -1);
    assumeOrAssertOrConstrain.assign(n_G, -1);
    returnExprs.assign(n_G, vi());
    gotoLabels.assign(n_G, vi());
    whileDecider.assign(n_G, -1);
    firstNodeInWhileBody.assign(n_G, -1);
    ifBranches.assign(n_G, vii());
    for (int stmtId = 0; stmtId < n_G; ++stmtId) {
      in >> stmtType[stmtId];
      //			db(stmtType[stmtId]);
      if (stmtType[stmtId] == "exit") {
        assert(stmtId == exitOf[procOfNode[stmtId]]);
        int p;
        in >> p;
        assert(p == procOfNode[stmtId]);
      } else if (stmtType[stmtId] == "dead") {
        int nDead;
        in >> nDead;
        deadVars[stmtId].assign(nDead, 0);
        for (int j = 0; j < nDead; ++j) {
          in >> deadVars[stmtId][j];
          checkVar(procOfNode[stmtId], deadVars[stmtId][j]);
        }
      } else if (stmtType[stmtId] == "call" ||
                 stmtType[stmtId] == "assign_call") {
        if (stmtType[stmtId] == "assign_call") {
          int nVarsLHS;
          in >> nVarsLHS;
          //					db(nVarsLHS);
          callsLHS[stmtId].assign(nVarsLHS, -1);
          for (int j = 0; j < nVarsLHS; ++j) {
            in >> callsLHS[stmtId][j];
            checkVar(procOfNode[stmtId], callsLHS[stmtId][j]);
          }
        }
        in >> calls[stmtId];
        assert(calls[stmtId] >= 0 && calls[stmtId] < n_H);
        assert(sz(callsLHS[stmtId]) == numReturns[calls[stmtId]]);
        int nArgs;
        in >> nArgs;
        //				db(nArgs);
        //				db(nArgs, calls[stmtId],
        // params[calls[stmtId]]);
        assert(nArgs == sz(params[calls[stmtId]]));
        callsArgs[stmtId].assign(nArgs, -1);
        for (int j = 0; j < nArgs; ++j) {
          in >> callsArgs[stmtId][j];
          assert(callsArgs[stmtId][j] >= 0 && callsArgs[stmtId][j] < n_E);
        }
      } else if (stmtType[stmtId] == "parallel_assign") {
        int nVars;
        in >> nVars;
        //				db(nVars);
        parallelAssigns[stmtId].fs.assign(nVars, -1);
        for (int j = 0; j < nVars; ++j) {
          in >> parallelAssigns[stmtId].fs[j];
          //					db(stmtId, procOfNode[stmtId],
          // parallelAssigns[stmtId].fs[j],
          // procOfVar[parallelAssigns[stmtId].fs[j]]);
          checkVar(procOfNode[stmtId], parallelAssigns[stmtId].fs[j]);
        }
        int nExp;
        in >> nExp;
        assert(nVars == nExp);
        parallelAssigns[stmtId].sc.assign(nExp, -1);
        for (int j = 0; j < nExp; ++j) {
          in >> parallelAssigns[stmtId].sc[j];
          assert(parallelAssigns[stmtId].sc[j] >= 0 &&
                 parallelAssigns[stmtId].sc[j] < n_E);
        }
        in >> oc[stmtId];
        assert(oc[stmtId] == -1 || (oc[stmtId] >= 0 && oc[stmtId] < n_E));
      } else if (stmtType[stmtId] == "assert" || stmtType[stmtId] == "assume" ||
                 stmtType[stmtId] == "constrain") {
        in >> assumeOrAssertOrConstrain[stmtId];
        assert(assumeOrAssertOrConstrain[stmtId] >= 0 &&
               assumeOrAssertOrConstrain[stmtId] < n_E);
      } else if (stmtType[stmtId] == "return") {
        int nExpr;
        in >> nExpr;
        assert(nExpr == numReturns[procOfNode[stmtId]]);
        returnExprs[stmtId].assign(nExpr, -1);
        for (int j = 0; j < nExpr; ++j) {
          in >> returnExprs[stmtId][j];
          assert(returnExprs[stmtId][j] >= 0 && returnExprs[stmtId][j] < n_E);
        }
      } else if (stmtType[stmtId] == "skip") {
        // do nothing :)
      } else if (stmtType[stmtId] == "goto") {
        int nLabels;
        in >> nLabels;
        gotoLabels[stmtId].assign(nLabels, -1);
        for (int j = 0; j < nLabels; ++j) {
          in >> gotoLabels[stmtId][j];
          assert(gotoLabels[stmtId][j] >= 0 && gotoLabels[stmtId][j] < n_L);
          assert(procOfNode[labelDest[gotoLabels[stmtId][j]]] ==
                 procOfNode[stmtId]);
        }
      } else if (stmtType[stmtId] == "while") {
        in >> whileDecider[stmtId];
        assert(whileDecider[stmtId] >= 0 && whileDecider[stmtId] < n_E);

        in >> firstNodeInWhileBody[stmtId];
        assert(firstNodeInWhileBody[stmtId] >= 0 &&
               firstNodeInWhileBody[stmtId] < n_G);
        assert(procOfNode[firstNodeInWhileBody[stmtId]] == procOfNode[stmtId]);
      } else if (stmtType[stmtId] == "if") {
        int nBranches;
        in >> nBranches;
        ifBranches[stmtId].assign(nBranches, mp(-1, -1));
        for (int j = 0; j < nBranches; ++j) {
          in >> ifBranches[stmtId][j].fs >> ifBranches[stmtId][j].sc;
          assert((ifBranches[stmtId][j].fs >= 0 &&
                  ifBranches[stmtId][j].fs < n_E) ||
                 (j == nBranches - 1 && ifBranches[stmtId][j].fs == -1));

          assert(ifBranches[stmtId][j].sc >= 0 &&
                 ifBranches[stmtId][j].sc < n_G);
          assert(procOfNode[ifBranches[stmtId][j].sc] == procOfNode[stmtId]);
        }
      } else if (stmtType[stmtId] == "error") {

      } else
        assert(false);
    }
  }

  void inputExprsDesc(ifstream &in) {
    exprType.assign(n_E, "");
    exprOperands.assign(n_E, vi());
    for (int e = 0; e < n_E; ++e) {
      in >> exprType[e];
      // nullary operators
      if (exprType[e] == "nondet" || exprType[e] == "true" ||
          exprType[e] == "false") {

      } // unary operators
      else if (exprType[e] == "not" || exprType[e] == "var" ||
               exprType[e] == "primed" || exprType[e] == "int") {
        int operand;
        in >> operand;
        if (exprType[e] == "not")
          assert(operand >= 0 && operand < n_E);
        if (exprType[e] == "int")
          assert(operand == 0 || operand == 1);
        if (exprType[e] == "var" || exprType[e] == "primed")
          assert(operand >= 0 && operand < n_V);
        exprOperands[e].pb(operand);
      } // binary operators
      else if (exprType[e] == "choose" || exprType[e] == "implies" ||
               exprType[e] == "or" || exprType[e] == "xor" ||
               exprType[e] == "and" || exprType[e] == "eq" ||
               exprType[e] == "neq") {
        int operand1, operand2;
        in >> operand1 >> operand2;
        //				db(exprType[e], operand1, operand2,
        // n_E);
        assert(operand1 >= 0 && operand1 < n_E);
        assert(operand2 >= 0 && operand2 < n_E);
        exprOperands[e].pb(operand1);
        exprOperands[e].pb(operand2);
      } else
        assert(0);
    }
  }

  void addErrorNodeProc() {
    errProc = n_H++;
    procOfNode[errNode] = n_H - 1;
    entryOf.pb(errNode);
    exitOf.pb(errNode);
    enforce.pb(-1);
    numReturns.pb(0);
    params.pb(vi());
  }

  void checkVar(int p, int v) {
    assert(v >= 0 && v < n_V);
    assert(procOfVar[v] == -1 || procOfVar[v] == p);
  }

  void buildCFGs(ifstream &in) {

    locals.assign(n_H, vi());
    for (int v = 0; v < n_V; ++v) {
      if (procOfVar[v] == -1)
        globals.pb(v);
      else
        locals[procOfVar[v]].pb(v);
    }

    G = sz(globals);
    L = 0;

    for (int i = 0; i < n_H; ++i)
      L = max(L, max(sz(locals[i]), numReturns[i]));

    GL = G + L;

    db(G, L, GL);

    if (GL > maxGL)
      maxGL = GL;

    setPAone();

    mapVarToBDDIdx.assign(n_V, -1);

    for (int i = 0; i < G; ++i)
      mapVarToBDDIdx[globals[i]] = i;

    for (int p = 0; p < n_H; ++p) {
      for (int j = 0; j < sz(locals[p]); ++j) {
        mapVarToBDDIdx[locals[p][j]] = j + G;
      }
    }

    //		db("hmmmm4");

    noEnforceStmt.assign(n_G, 0);
    for (int l = 0; l < n_L; ++l)
      if (noEnforceLabel[l])
        noEnforceStmt[labelDest[l]] = 1;

    // for every edge on which the enforce is applied, we replace its weight
    // with PAdot(PAone & enforceFormula[p], oldWeight)
    enforceFormula.assign(n_H, bddtrue);
    for (int p = 0; p < n_H; ++p) {
      if (enforce[p] != -1)
        curProc = p, curStmtType = "enforce",
        enforceFormula[p] = getExprAsBDD(enforce[p]);
    }

    //		db("hmmmm5");

    indicesOflocalsNotInLHS.assign(n_G, vi());

    for (int stmtId = 0; stmtId < n_G; ++stmtId) {
      if (stmtType[stmtId] == "call" || stmtType[stmtId] == "assign_call") {
        si varsIndicesLHS;
        for (int i = 0; i < sz(callsLHS[stmtId]); ++i) {
          int v = callsLHS[stmtId][i];
          int idxLHS = mapVarToBDDIdx[v];
          varsIndicesLHS.insert(idxLHS);
        }
        for (auto &local : locals[procOfNode[stmtId]]) {
          int localIdx = mapVarToBDDIdx[local];
          if (!present(varsIndicesLHS, localIdx))
            indicesOflocalsNotInLHS[stmtId].pb(localIdx);
        }
      }
    }

    //		db("hmmmm5");

    succ.assign(n_G, V<pair<int, bdd>>());
    bdd figureOut = bddtrue;

    /*		getExprAsBDD()

                    return;*/

    for (int stmtId = 0; stmtId < n_G; ++stmtId) {

      curStmtType = stmtType[stmtId];
      curProc = procOfNode[stmtId];
      curNode = stmtId;

      if (stmtType[stmtId] == "dead") {
        si varsIndices;
        for (auto &v : deadVars[stmtId]) {
          varsIndices.insert(mapVarToBDDIdx[v]);
        }
        addEdge(stmtId, nextNode[stmtId], deadTF(varsIndices));
      } else if (stmtType[stmtId] == "call" ||
                 stmtType[stmtId] == "assign_call") {

        int pp = calls[stmtId];
        // call-to-return-site edge
        {
          succ[stmtId].pb(mp(nextNode[stmtId], PAzero));
          assert(stmtType[nextNode[stmtId]] == "skip");
        }
        // interprocedural edges
        // call-to-start edge
        {
          bdd res = bddtrue;
          for (int j = 0; j < G; ++j)
            res &= setEq(j);

          assert(sz(callsArgs[stmtId]) == sz(params[calls[stmtId]]));

          for (int j = 0; j < sz(params[pp]); ++j) {
            int v = params[pp][j];
            assert(procOfVar[v] == pp);
            res &= setVar(mapVarToBDDIdx[v], callsArgs[stmtId][j]);
          }

          addEdge(stmtId, entryOf[pp], res);
        }

        // exit-to-return-site edge
        {
          bdd res = bddtrue;
          assert(sz(callsLHS[stmtId]) == numReturns[pp]);
          si varsIndicesLHS;
          for (int idxRHS = G; idxRHS < G + numReturns[pp]; ++idxRHS) {
            // idxRHS correspond to the value returned by the call
            int v = callsLHS[stmtId][idxRHS - G];
            int idxLHS = mapVarToBDDIdx[v];
            varsIndicesLHS.insert(idxLHS);
            res &= bdd_biimp(bdd_ithvar(varPrimed(idxLHS)),
                             bdd_ithvar(var(idxRHS)));
          }

          for (int idx = 0; idx < G; ++idx)
            if (!present(varsIndicesLHS, idx))
              res &= setEq(idx);

          addEdge(exitOf[pp], nextNode[stmtId], res);
        }
      } else if (stmtType[stmtId] == "return") {
        bdd res = bddtrue;
        for (int idx = 0; idx < GL; ++idx) {
          if (idx >= G && idx < G + sz(returnExprs[stmtId])) {
            int e = returnExprs[stmtId][idx - G];
            assert(exprType[e] != "nondet");
            res &= setVar(idx, e);
          } else
            res &= setEq(idx);
        }
        addEdge(stmtId, exitOf[procOfNode[stmtId]], res);
      } else if (stmtType[stmtId] == "parallel_assign") {
        bdd res = bddtrue;
        si LHSIndices;
        for (auto &var : parallelAssigns[stmtId].fs)
          LHSIndices.insert(mapVarToBDDIdx[var]);

        // for variables that do not appear in LHS, they maintain their value
        for (int j = 0; j < GL; ++j)
          if (!present(LHSIndices, j))
            res &= setEq(j);

        for (int j = 0; j < sz(parallelAssigns[stmtId].fs); ++j) {
          int v = parallelAssigns[stmtId].fs[j],
              e = parallelAssigns[stmtId].sc[j];
          int varLHSIdx = mapVarToBDDIdx[v];
          assert(exprType[e] != "nondet");
          res &= setVar(varLHSIdx, e);
        }
        addEdge(stmtId, nextNode[stmtId], res);
      } else if (stmtType[stmtId] == "assert" || stmtType[stmtId] == "assume" ||
                 stmtType[stmtId] == "constrain" ||
                 stmtType[stmtId] == "while") {
        bdd trueTF = PAone, falseTF = PAone;
        int deciderExpr =
            (stmtType[stmtId] == "while" ? whileDecider[stmtId]
                                         : assumeOrAssertOrConstrain[stmtId]);
        if (exprType[deciderExpr] == "nondet")
          ;
        else {
          bdd exprFormula = getExprAsBDD(deciderExpr);
          trueTF &= exprFormula;
          falseTF &= !exprFormula;
        }

        if (stmtType[stmtId] == "while") {
          // if the loop condition is true
          addEdge(stmtId, firstNodeInWhileBody[stmtId], trueTF);
          // if the loop condition is false
          addEdge(stmtId, nextNode[stmtId], falseTF);
        } else {
          // if the assertion is true
          addEdge(stmtId, nextNode[stmtId], trueTF);
          // if the assertion is false
          // IMP: uncomment
          addEdge(stmtId, errNode, falseTF);
          // imp
          addEdge(errNode, stmtId, bddfalse);
        }
      } else if (stmtType[stmtId] == "skip") {
        addEdge(stmtId, nextNode[stmtId], PAone);
      } else if (stmtType[stmtId] == "goto") {
        for (auto &label : gotoLabels[stmtId])
          addEdge(stmtId, labelDest[label], PAone);

      } else if (stmtType[stmtId] == "if") {
        // holds the or of all the conditions that have been processed before.
        // We only enter a branch if !provConds & curCond. When going through
        // conditions with non-deterministic condition, prevConds is not
        // affected.
        bdd prevConds = bddfalse;
        for (auto &pr : ifBranches[stmtId]) {
          int e = pr.fs;
          int firstNodeInBranch = pr.sc;
          bdd res = PAone & !prevConds;
          if (e == -1 || exprType[e] == "nondet")
            ;
          else {
            bdd exprBdd = getExprAsBDD(e);
            res &= exprBdd;
            prevConds |= exprBdd;
          }
          addEdge(stmtId, firstNodeInBranch, res);
        }
      } else if (stmtType[stmtId] == "error" || stmtType[stmtId] == "exit") {
        // error has no outgoing edges, exit's outgoing edges will be added by
        // the corresponding callers imp upd: error's will have false outgoing
        // edges to model errorNode having its own function
      } else
        assert(false);
    }

    set<pii> edges;
    set<pii> otheredges;

    for (int u = 0; u < n_G; ++u) {
      for (auto &pr : succ[u]) {
        edges.insert(mp(min(u, pr.fs), max(u, pr.fs)));
      }
    }

    int m_G;

    in >> m_G;

    for (int i = 0; i < m_G; ++i) {
      int u, v;
      in >> u >> v;
      otheredges.insert(mp(min(u, v), max(u, v)));
    }

    //		assert(m_G == sz(edges));
    //		assert(edges == otheredges);
    checkStats();
  }

  void addEdge(int u, int v, bdd weight) {
    int p = procOfNode[u];
    if (enforceFormula[p] != bddtrue && !noEnforceStmt[p])
      weight = PAdot(PAone & enforceFormula[p], weight);
    succ[u].pb(mp(v, weight));
  }

  // Given an index idx, return (effectively) the formula x'_idx = e where e is
  // an expression id over variables x_0 .. x_{GL-1} e might be a choose[e, f]
  // expr, in which case we return ((e && !f) => x_idx') && ((f && !e) =>not
  // x_idx')
  bdd setVar(int idx, int e) {
    assert(idx >= 0 && idx < GL);
    assert(e >= 0 && idx < n_E);
    if (exprType[e] == "choose") {
      bdd left = getExprAsBDD(exprOperands[e][0]);
      bdd right = getExprAsBDD(exprOperands[e][1]);
      return bdd_imp(left & (!right), bdd_ithvar(varPrimed(idx))) &
             bdd_imp(((!left) & right), bdd_nithvar(varPrimed(idx)));
    } else
      return bdd_biimp(bdd_ithvar(varPrimed(idx)), getExprAsBDD(e));
  }

  bdd getExprAsBDD(int e) {
    //		db(e, exprType[e]);
    assert(exprType[e] != "nondet" && exprType[e] != "choose");
    if (exprType[e] == "true")
      return bddtrue;
    if (exprType[e] == "false")
      return bddfalse;

    if (exprType[e] == "int")
      return (exprOperands[e][0] == 0 ? bddfalse : bddtrue);

    if (exprType[e] == "var") {
      int v = exprOperands[e][0];
      //			db(v);
      assert(v >= 0 && v < n_V);

      //			db(e, v, errNode);
      //			db(curProc, curStmtType, procOfVar[v]);
      assert(procOfVar[v] == -1 || procOfVar[v] == curProc);
      int vIdx = mapVarToBDDIdx[v];
      assert(vIdx >= 0 && vIdx < GL);
      return bdd_ithvar(var(vIdx));
    }
    if (exprType[e] == "primed") {
      db(curStmtType);
      assert(curStmtType == "parallel_assign" && oc[curNode] != -1);
      int v = exprOperands[e][0];
      assert(v >= 0 && v < n_V);
      assert(procOfVar[v] == -1 || procOfVar[v] == curProc);
      int vIdx = mapVarToBDDIdx[v];
      assert(vIdx >= 0 && vIdx < GL);
      return bdd_ithvar(varPrimed(vIdx));
    }
    if (exprType[e] == "not")
      return !(getExprAsBDD(exprOperands[e][0]));

    if (exprType[e] == "implies")
      return bdd_imp(getExprAsBDD(exprOperands[e][0]),
                     getExprAsBDD(exprOperands[e][1]));

    if (exprType[e] == "or")
      return getExprAsBDD(exprOperands[e][0]) |
             getExprAsBDD(exprOperands[e][1]);

    if (exprType[e] == "and")
      return getExprAsBDD(exprOperands[e][0]) &
             getExprAsBDD(exprOperands[e][1]);

    if (exprType[e] == "eq")
      return bdd_biimp(getExprAsBDD(exprOperands[e][0]),
                       getExprAsBDD(exprOperands[e][1]));

    if (exprType[e] == "neq")
      return !bdd_biimp(getExprAsBDD(exprOperands[e][0]),
                        getExprAsBDD(exprOperands[e][1]));

    if (exprType[e] == "xor")
      return bdd_xor(getExprAsBDD(exprOperands[e][0]),
                     getExprAsBDD(exprOperands[e][1]));

    throw std::invalid_argument("unsupported Boolean-program expression");
  }

  void prepareStuff() {
    for (int u = 0; u < n_G; ++u) {
      for (auto &pr : succ[u]) {
        int v = pr.fs;
        bdd val = pr.sc;
        edgeListG.pb(mp(mp(u, v), val));
      }
    }
    //		db(sz(edgeListG));

    vertexTypeG.assign(n_G, -1);

    for (int p = 0; p < n_H - 1; ++p) {
      assert(vertexTypeG[entryOf[p]] == -1);
      vertexTypeG[entryOf[p]] = START_VERTEX;
      assert(vertexTypeG[exitOf[p]] == -1);
      vertexTypeG[exitOf[p]] = EXIT_VERTEX;
    }

    vertexTypeG[errNode] = ERROR_VERTEX;

    for (int i = 0; i < n_G; ++i) {
      if (stmtType[i] == "call" || stmtType[i] == "assign_call") {
        assert(vertexTypeG[i] == -1);
        vertexTypeG[i] = CALL_VERTEX;
        assert(vertexTypeG[nextNode[i]] == -1);
        vertexTypeG[nextNode[i]] = RETURN_SITE_VERTEX;
      }
    }

    assert(vertexTypeG[errNode] == ERROR_VERTEX);
    procOf = procOfNode;

    set<pii> edgeListHSet;
    for (int u = 0; u < n_G; ++u) {
      for (auto &pr : succ[u]) {
        int v = pr.fs;
        bdd val = pr.sc;
        if (vertexTypeG[u] == CALL_VERTEX && vertexTypeG[v] == START_VERTEX)
          edgeListHSet.insert(mp(procOf[u], procOf[v]));
      }
    }

    for (auto &pr : edgeListHSet)
      edgeListH.pb(pr);
  }

  void readTWD(string TWDpath) {
    //		db(TWDpath);
    // take TWD from pace solver
    TWD.assign(n_H, vvi());
    TWD_par.assign(n_H, vi());
    TWD_root.assign(n_H, -1);
    ifstream in(TWDpath);
    string s, dummy;

    int n_BB, treewidth;
    vvi bags;
    V<si> adjTw;
    while (getline(in, s)) {
      if (s[0] == 'c')
        continue;
      stringstream ss(s);
      if (s[0] == 's') {
        ss >> dummy >> dummy;
        int nnn;
        ss >> n_BB >> treewidth >> nnn;
        --treewidth;
        //				db(tw);
        assert(nnn == n_G + 1);
        adjTw.assign(n_BB, si());
      } else if (s[0] == 'b') {
        bags.pb(vi());
        ss >> dummy;
        int id;
        ss >> id;
        assert(id == sz(bags));
        int u;
        while (ss >> u) {
          --u;
          if (u != n_G) {
            assert(u >= 0);
            assert(u < n_G);
            bags.back().pb(u);
          }
        }
        //				assert(!bags.back().empty());
      } else {
        int u, v;
        ss >> u >> v;
        --u, --v;
        assert(u >= 0 && v < n_BB);
        if (u == v)
          continue;
        adjTw[u].insert(v);
        adjTw[v].insert(u);
      }
    }

    {
      int cnt = 0;
      function<void(int, int)> dfs = [&](int u, int par) {
        ++cnt;
        for (auto &v : adjTw[u])
          if (v != par) {
            dfs(v, u);
          }
      };

      dfs(0, -1);

      assert(cnt == n_BB);
      //			db("adjTw is a tree!");
    }

    V<map<int, int>> bagsMapping(n_H);

    for (int i = 0; i < n_BB; ++i) {
      map<int, vi> splitting;
      for (auto &u : bags[i]) {
        assert(u >= 0 && u < n_G);
        splitting[procOf[u]].pb(u);
      }
      for (auto &pr : splitting) {
        int p = pr.fs;
        int id = sz(TWD[p]);
        bagsMapping[p][i] = id;
        assert(!pr.sc.empty());
        TWD[p].pb(pr.sc);
      }
    }

    //		db(errNode, TWD[n_H - 1]);

    V<V<si>> adjTwPerProc(n_H);

    for (int p = 0; p < n_H; ++p) {
      adjTwPerProc[p].assign(sz(TWD[p]), si());
    }

    for (int i = 0; i < n_BB; ++i) {
      for (auto &j : adjTw[i]) {
        for (auto &u : bags[i]) {
          for (auto &v : bags[j]) {
            if (procOf[u] == procOf[v]) {
              int p = procOf[u];
              assert(i != j);
              assert(present(bagsMapping[p], i));
              assert(present(bagsMapping[p], j));
              assert(bagsMapping[p][i] != bagsMapping[p][j]);
              assert(bagsMapping[p][i] >= 0 && bagsMapping[p][i] < sz(TWD[p]));
              assert(bagsMapping[p][j] >= 0 && bagsMapping[p][j] < sz(TWD[p]));
              adjTwPerProc[p][bagsMapping[p][i]].insert(bagsMapping[p][j]);
              adjTwPerProc[p][bagsMapping[p][j]].insert(bagsMapping[p][i]);
            }
          }
        }
      }
    }

    //		db(tw);

    for (int p = 0; p < n_H; ++p) {
      TWD_par[p].assign(sz(TWD[p]), -1);

      function<void(int, int)> dfs = [&](int u, int par) {
        assert(u != par);
        TWD_par[p][u] = par;
        for (auto &v : adjTwPerProc[p][u])
          if (v != par) {
            assert(v != u);
            assert(v != par);
            dfs(v, u);
          }
      };

      //			db(p, n_H);

      for (int b = 0; b < sz(TWD[p]); ++b) {
        if (TWD_par[p][b] == -1)
          dfs(b, -1);
      }

      int minusOnes = 0;

      for (int i = 0; i < sz(TWD[p]); ++i) {
        if (TWD_par[p][i] == -1) {
          minusOnes++;
          TWD_root[p] = i;
        }
      }
      for (int i = 0; i < sz(TWD[p]); ++i) {
        if (TWD_par[p][i] == -1 && i != TWD_root[p]) {
          TWD_par[p][i] = TWD_root[p];
        }
      }

      assert(minusOnes >= 1);
    }

    //		db("got the PACE tw-decomp");
  }
  // imp: we'll use the dummy node added in the BP processor as the errProc's
  // representation in the call graph
  void readTDD(string TDDpath) {
    int treedepth;
    db(errProc);

    ifstream in(TDDpath);
    in >> treedepth;

    par_tdH.assign(n_H, 0);
    for (int i = 0; i < n_H; ++i) {
      in >> par_tdH[i];
      if (par_tdH[i])
        assert(par_tdH[i] >= 1 && par_tdH[i] <= n_H);
      //			db(i, par_tdH[i]);
      --par_tdH[i];
    }
    vi roots;
    for (int i = 0; i < n_H; ++i) {
      if (par_tdH[i] == -1)
        roots.pb(i);
    }
    assert(sz(roots) == 1);
    TDD_root = roots[0];
    //		db(td);
  }

  void setWeights() {
    vi maxBagSz(n_H, 0);
    weightSameBag.assign(n_H, 0);
    weightCentroid.assign(n_H, 0);
    weightCallsG.assign(n_H, 0);

    double pwr1 = 1.2;
    double pwr2 = 1.2;
    double pwr3 = 1.2;
    //		db(pwr1, pwr2, pwr3);

    for (int p = 0; p < n_H; ++p) {
      for (auto &bag : TWD[p]) {
        maxBagSz[p] = max(maxBagSz[p], sz(bag));
        weightSameBag[p] +=
            sz(bag) * 1LL * sz(bag) * sz(bag) * pow(sz(locals[p]), pwr1);
        weightCentroid[p] +=
            sz(bag) * 1LL * sz(bag) * sz(bag) * pow(sz(locals[p]), pwr2);
      }
      weightCentroid[p] *= log2(sz(TWD[p]));
    }

    vvi GRev(n_G);
    vi vis(n_G, -1);

    for (auto &pr : edgeListG) {
      int u = pr.fs.fs, v = pr.fs.sc;
      //			db(vertexTypeG[u], vertexTypeG[v]);
      if (vertexTypeG[u] != ERROR_VERTEX && vertexTypeG[v] != ERROR_VERTEX &&
          !(vertexTypeG[u] == CALL_VERTEX && vertexTypeG[v] == START_VERTEX) &&
          !(vertexTypeG[u] == EXIT_VERTEX &&
            vertexTypeG[v] == RETURN_SITE_VERTEX))
        assert(procOf[u] == procOf[v]), GRev[v].pb(u);
    }

    for (int c = 0; c < n_G; ++c) {
      if (vertexTypeG[c] == CALL_VERTEX) {
        vi revReach;

        function<void(int)> dfs = [&](int u) {
          revReach.pb(u);
          vis[u] = c;
          for (auto &v : GRev[u])
            if (vis[v] != c)
              dfs(v);
        };

        dfs(c);

        int p = procOf[c];
        weightCallsG[p] += 1LL * sz(revReach) * maxBagSz[p] * sz(locals[p]);
      }
    }
  }

  ApaInstance<bdd> getInstance() {

    /*		db(errNode, entryOf[176], exitOf[176]);
                    for (int u = 0; u < n_G; ++u) {
                            if (procOf[u] == 176) {
                                    db(u, stmtType[u], nextNode[u],
       procOf[nextNode[u]]); for (auto& pr : succ[u]) { cout << "\t";db(pr.fs);
                                    }
                            }
                    }
                    int x;
                    cin >> x;*/

    Algebra<bdd> *PAalgebra =
        new Algebra<bdd>(PAzero, PAone, PAplus, PAdot, PAstar, PAproject);
    return ApaInstance<bdd>(n_G, n_H, edgeListG, edgeListH, vertexTypeG, procOf,
                            TWD, TWD_root, TWD_par, TDD_root, par_tdH,
                            PAalgebra, weightSameBag, weightCentroid,
                            weightCallsG);
  }
};

