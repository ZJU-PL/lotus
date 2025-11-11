/*
 * lossy_Solvers/FPSolve/Polynomials/NonCommutativePolynomial.h
 *
 *  Created on: 22.04.2015
 *      Author: schlund
 */

#ifndef FPSOLVE_LOSSY_NON_COMMUTATIVE_POLYNOMIAL_H
#define FPSOLVE_LOSSY_NON_COMMUTATIVE_POLYNOMIAL_H

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <map>
#include <string>
#include <queue>
#include <climits>
#include <iostream>


#include "Solvers/FPSolve/Polynomials/NonCommutativeMonomial.h"
#include "Solvers/FPSolve/Polynomials/NonCommutativePolynomial.h"
#include "Solvers/FPSolve/Polynomials/LossyNonCommutativeMonomial.h"

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include "Solvers/FPSolve/Semirings/LossyFiniteAutomaton.h"

#include "Solvers/FPSolve/DataStructs/Equations.h"

#include "Solvers/FPSolve/Utils/StringUtil.h"
#include "Solvers/FPSolve/Solvers/SolverUtils.h"

class VarId;

template <>
class NonCommutativePolynomial<LossyFiniteAutomaton> : public NonCommutativePolynomialBase<LossyFiniteAutomaton> {

public:

  typedef NonCommutativeMonomial<LossyFiniteAutomaton> lossyMon;

  using NonCommutativePolynomialBase<LossyFiniteAutomaton>::NonCommutativePolynomialBase;

  NonCommutativePolynomial() = default;


  // constructor for implicit conversions
  NonCommutativePolynomial(const NonCommutativePolynomialBase<LossyFiniteAutomaton>& p) {
    this->monomials_ = p.monomials_;
  }


  /*
   * Extracts the terminal letters used in this polynomial; only recognizes alphanumeric characters.
   *
   * Use this as little as possible; cache it if you need it.
   */
   std::set<unsigned char> get_terminals() const {
     std::set<unsigned char> terminals;

     for(const auto &monomial: this->monomials_) {
       auto monomial_terminals = static_cast<lossyMon>(monomial.first).get_terminals();
       terminals.insert(monomial_terminals.begin(), monomial_terminals.end());
     }

     return terminals;
   }

   /*
    * As with get_variables(), make sure you cache this if you use it.
    *
    * Only considers monomials with exactly two variables; used in generating the lossy quadratic normal form
    * of a system, see LossyFiniteAutomaton::downwardClosureDerivationTrees(..).
    */
   std::set<VarId> get_variables_quadratic_monomials() const {
     std::set<VarId> vars;

     for(auto const &monomial : this->monomials_) {
       if(monomial.first.get_degree() == 2) {
         auto tmp = monomial.first.get_variables();
         vars.insert(tmp.begin(), tmp.end());
       }
     }

     return vars;
   }

   /*
    * Finds all the constants appearing in nonterminal monomials of this polynomial and appends each new one to the vector.
    *
    * Only checks linear monomials if checkLinearTerms is true.
    */
   void findConstantsInNonterminalMonomials(std::set<LossyFiniteAutomaton> &constants, bool checkLinearTerms) const {

     // delegate to the monomials
     for(auto &monomial: this->monomials_) {
       static_cast<lossyMon>(monomial.first).findConstantsInNonterminalMonomials(constants, checkLinearTerms);
     }
   }


   LossyFiniteAutomaton sumOfConstantMonomials() const {
     LossyFiniteAutomaton sum = LossyFiniteAutomaton::null();

     for(auto &monomial: this->monomials_) {
       if(monomial.first.get_degree() == 0) {
         sum = sum + monomial.first.getLeadingSR();
       }
     }

     return sum;
   }

   /*
    * Replaces all constants in the nonterminal monomials of the polynomial with their respective VarId mapping.
    * Used in transformation to Chomsky Normal Form.
    *
    * Only replaces constants in linear monomials if replaceInLinearMonomials is true.
    */
   NonCommutativePolynomialBase<LossyFiniteAutomaton> replaceConstants(std::map<LossyFiniteAutomaton, VarId> &constantsToVariables,
       bool replaceInLinearMonomials) const {
     NonCommutativePolynomialBase<LossyFiniteAutomaton> temp = null();

     // delegate to the monomials
     for(auto &monomial: this->monomials_) {

       // we only replace constants in nonterminal monomials, all terminal monomials are added to the polynomial as is
       // only replace constants in linear monomials if replaceInLinearMonomials is true
       if((monomial.first.get_degree() != 0)
           && (replaceInLinearMonomials || monomial.first.get_degree() != 1)) {
         temp += static_cast<lossyMon>(monomial.first).replaceConstants(constantsToVariables);
       } else {
         InsertMonomial(temp.monomials_, monomial.first, monomial.second);
       }
     }

     return temp;
   }

   /*
    * Removes all epsilon monomials from a polynmoial.
    * An epsilon monomial is a monomial consisting only of epsilon.
    */
   NonCommutativePolynomialBase<LossyFiniteAutomaton> removeEpsilonMonomials() const {
     NonCommutativePolynomialBase<LossyFiniteAutomaton> temp = null();

     for(auto monomial: this->monomials_) {

       // if the monomial has at least one variable or it doesn't have a variable but isn't
       // equal to epsilon, then it's not an epsilon monomial
       if(!monomial.first.isEpsilonMonomial()) {
         temp += monomial.first * monomial.second;
       }
     }

     return temp;
   }

   /*
    * Removes all epsilon productions from the system.
    */
   static void removeEpsilonProductions(NCEquationsBase<LossyFiniteAutomaton> &equations) {
     for(auto equation : equations) {
       equation.second = static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).removeEpsilonMonomials();
     }
   }

   /*
    * Cleans the polynomial system, i.e. removes variables that are unproductive or unreachable
    * from the set of nonterminals given in the worklist; the worklist needs to contain the
    * variables we want to start derivations from, i.e. the set of initial nonterminals.
    */
   static NCEquationsBase<LossyFiniteAutomaton> cleanSystem
   (const NCEquationsBase<LossyFiniteAutomaton> &equations,
       std::queue<VarId> &worklist) {

     std::queue<VarId> temp; // to store the variables that still need checking
     std::unordered_map<VarId, int> encounteredVariables; // to remember whether a variable was encountered and check for
     // for being productive yet;
     // 0 means "never encountered",
     // 1 means "encountered but not checked",
     // 2 means "checked at least once"
     std::unordered_map<VarId, bool> productiveVariables; // to map variables to "is this variable known to be productive?"
     std::unordered_map<VarId, NonCommutativePolynomialBase<LossyFiniteAutomaton>> productions; // to map variables to their productions

     VarId var;
     std::set<VarId> vars;
     // build the data structures that will store the info about which variables still
     // need checking and which variables are known to be productive
     for(auto &equation: equations) {
       vars = equation.second.get_variables();

       // just to make sure we catch all variables in case someone inputs a grammar with a terminal
       // that only appears in a lefthand side of a production
       for(auto &prodVar: vars) {
         encounteredVariables.insert(std::make_pair(prodVar, 0));
         productiveVariables.insert(std::make_pair(prodVar, false));
       }

       encounteredVariables.insert(std::make_pair(equation.first, 0));
       productiveVariables.insert(std::make_pair(equation.first, false));
       productions.insert(std::make_pair(equation.first, equation.second));

       // prepare the info for the initial variables
       while(!worklist.empty()) {
         var = worklist.front();
         worklist.pop();
         encounteredVariables[var] = 1;
         temp.push(var);
       }
       worklist.swap(temp);
     }

     // keep checking the variables that are not yet known to be productive
     // until no further variable becomes known to be productive; we do this
     // using BFS so unreachable variables will be removed as well since they
     // can never be found to be productive this way
     bool update;
     do {
       update = false;

       // terminates once there is nothing more to do; specifically, this terminates at the latest once
       // all reachable variables are known to be productive because then they won't be put in the
       // worklist again; it terminates at the earliest once all reachable variables have been checked
       // without finding a productive one since the worklist is being filled with all reachable variables
       // before the end of the first iteration
       while(!worklist.empty()) {
         var = worklist.front();
         worklist.pop();

         // if we have not yet checked this variable, remember that we now have and put all
         // the unencountered variables in its productions into the worklist; this way, we only
         // put reachable variables into the worklist once
         if(encounteredVariables[var] == 1) {
           encounteredVariables[var] = 2;

           for(auto &monomial: productions[var].monomials_) {
             for(auto &reachableVar: monomial.first.get_variables()) {

               // only put variables into the worklist that have not been seen before
               if(encounteredVariables[reachableVar] == 0) {
                 worklist.push(reachableVar);
                 encounteredVariables[reachableVar] = 1;
               }
             }
           }
         }

         // if the variable was found to be productive, remember the update and change the
         // flag for the variable; we know a variable is productive once one of the productions
         // associated with it becomes known to be productive; since there is no need to recheck a
         // productive variable, don't put it in the worklist again
         if(static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(productions[var]).isProductive(productiveVariables)) {
           productiveVariables[var] = true;
           update = true;
         } else { // if the variable isn't known to be productive yet, put it in the worklist
           // for the next iteration
           temp.push(var);
         }
       }

       // don't do work we don't need
       if(update) {
         worklist.swap(temp);
       }
     } while(update);

     // build the clean system: only use variables that are both productive and reachable;
     // remove unproductive monomials
     NCEquationsBase<LossyFiniteAutomaton> cleanEquations;
     for(auto &equation: equations) {
       if(productiveVariables[equation.first]) {

         // at this point we know that the polynomial we are associating with the
         // variable is not the null polynomial since at least one of its monomials was
         // found to be productive
         cleanEquations.push_back(std::make_pair(equation.first,
             static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).removeUnproductiveMonomials(productiveVariables)));
       }
     }

     return cleanEquations;
   }

   /*
    * Equations can contain productions that are sentential forms, and variablefiedEquations will afterwards hold
    * productions that are either terminal or only contain nonterminals (i.e. the monomials in each polynomial
    * are either semiring elements or only contain variables).
    *
    * Linear terms (i.e. such with exactly one variable in them) will have their terminals replaced by variables
    * iff eliminateInLinearTerms is true. This way, we can produce a quadratic normal form without  blowing up
    * linear terms, if desired.
    */
   static NCEquationsBase<LossyFiniteAutomaton> eliminateTerminalsInNonterminalProductions
   (const NCEquationsBase<LossyFiniteAutomaton> &equations,
       ValuationMap<LossyFiniteAutomaton> &variablesToConstants, bool eliminateInLinearTerms) {

     std::set<LossyFiniteAutomaton> constants; // will hold all terminals appearing in nonterminal productions

     // find all constants in nonterminal productions in the system
     for(auto &equation: equations) {
       static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).findConstantsInNonterminalMonomials(constants, eliminateInLinearTerms);
     }

     // return value
     NCEquationsBase<LossyFiniteAutomaton> variablefiedEquations;

     // introduce a new variable for each constant found, add the respective equation to the system
     VarId var;
     std::map<LossyFiniteAutomaton, VarId> constantsToVariables; // will hold a mapping from terminals to variables that produce them
     for(auto &constant: constants) {
       var = Var::GetVarId();
       variablefiedEquations.push_back(std::make_pair(var, NonCommutativePolynomialBase<LossyFiniteAutomaton>(constant)));
       constantsToVariables.insert(std::make_pair(constant, var));
       variablesToConstants.insert(std::make_pair(var,constant));
     }

     // replace the constants in each nonterminal production with the new constant variables
     NonCommutativePolynomialBase<LossyFiniteAutomaton> allVariablesPoly;
     for(auto &equation: equations) {
       allVariablesPoly = static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).replaceConstants(constantsToVariables, eliminateInLinearTerms);
       variablefiedEquations.push_back(std::make_pair(equation.first, allVariablesPoly));
     }

     return variablefiedEquations;
   }

   /*
    * Binarizes nonterminal productions the way it is done when constructing a CNF of a grammar.
    */
   static NCEquationsBase<LossyFiniteAutomaton> binarizeProductions
   (const NCEquationsBase<LossyFiniteAutomaton> &equations) {

     // stores mappings between suffixes of monomials and the variables that are introduced
     // during binarization to produce those suffixes
     std::unordered_map<std::string, VarId> binarizationVariables;

     // stores the productions (i.e. equations) that are introduced while producing the
     // Chomsky Normal Form
     NCEquationsBase<LossyFiniteAutomaton> binarizationVariablesEquations;

     // to store the result
     NCEquationsBase<LossyFiniteAutomaton> binarizedEquations;

     // determine the necessary new variables to give all non-terminal productions that need binarizing
     // (i.e. monomials of degree > 2) the form "X = YZ"
     NonCommutativePolynomialBase<LossyFiniteAutomaton> binarizedPoly;
     for(auto &equation: equations) {
       binarizedPoly =
           static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).binarize(binarizationVariables, binarizationVariablesEquations);
       binarizedEquations.push_back(std::make_pair(equation.first, binarizedPoly));
     }

     // finally, add the productions to the system that were introduced during the last step
     for(auto &equation: binarizationVariablesEquations) {
       binarizedEquations.push_back(std::make_pair(equation.first, equation.second));
     }

     return binarizedEquations;
   }

   /*
    * Checks whether this polynomial is productive, depending on the set of variables
    * that are already known to be productive.
    */
   bool isProductive(const std::unordered_map<VarId, bool> &productiveVariables) const {

     // check if any monomial in this polynomial is productive; that will be enough
     // for the polynomial to be productive
     for(auto &monomial: this->monomials_) {
       if(static_cast<lossyMon>(monomial.first).isProductive(productiveVariables)) {
         return true;
       }
     }

     // if there is no productive monomial, then the polynomial isn't known to be productive so far
     return false;
   }

   /*
    * Decides whether the variables in a strongly connected component of a polynomial system can
    * duplicate themselves via this polynomial. In order to decide that, the map "varToComponent"
    * needs to hold the information in which component each variable lies, while "component" is
    * the component we are interested in.
    *
    * Used in the algorithm by Courcelle, see LossyFiniteAutomaton::downwardClosureCourcelle.
    */
   bool componentIsSquarable(std::unordered_map<VarId, int> &varToComponent, int component) const {
     bool squarable = false;

     for(auto &monomial: this->monomials_) {

       // skip monomials that don't have at least two variables
       if(monomial.first.get_degree() >= 2) {
         squarable |= static_cast<lossyMon>(monomial.first).componentIsSquarable(varToComponent, component);

         // don't do work we don't need
         if(squarable) {
           break;
         }
       }
     }

     return squarable;
   }

   /*
    * Used in the algorithm by Courcelle, see LossyFiniteAutomaton::downwardClosureCourcelle.
    *
    * Assumes that all productions have been binarized.
    */
   void mapQuadraticLHStoRHS(std::unordered_map<int, std::unordered_map<int, std::set<int>>> &quadraticLHStoRHS,
       std::unordered_map<VarId, int> &varToComponent, int component) const {
     for(auto &monomial: this->monomials_) {
       static_cast<lossyMon>(monomial.first).mapQuadraticLHStoRHS(quadraticLHStoRHS, varToComponent, component);
     }
   }

   /*
    * Used in the algorithm by Courcelle, see LossyFiniteAutomaton::downwardClosureCourcelle.
    *
    * Assumes that all productions have been binarized.
    */
   void calculateLowerComponentVariables(
       std::unordered_map<int, std::set<int>> &lhsLowerComponentVariables,
       std::unordered_map<int, std::set<int>> &rhsLowerComponentVariables,
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::unordered_map<VarId, int> &varToComponent,
       int component) {

     for(auto &monomial: this->monomials_) {
       static_cast<lossyMon>(monomial.first).calculateLowerComponentVariables(lhsLowerComponentVariables, rhsLowerComponentVariables,
           components, varToComponent, component);
     }
   }

   /*
    * Used in the algorithm by Courcelle, see LossyFiniteAutomaton::downwardClosureCourcelle.
    *
    * Assumes grammar is in quadratic normal form.
    *
    */
   void calculateSameComponentLetters(
       std::unordered_map<int, std::set<unsigned char>> &lhsSameComponentLetters,
       std::unordered_map<int, std::set<unsigned char>> &rhsSameComponentLetters,
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::unordered_map<VarId, int> &varToComponent,
       int component) {

     for(auto &monomial: this->monomials_) {
       static_cast<lossyMon>(monomial.first).calculateSameComponentLetters(lhsSameComponentLetters, rhsSameComponentLetters,
           components, varToComponent, component);
     }
   }

   /*
    * Used during the generation of the intersection between a CFG and a FiniteAutomaton.
    * Generates all productions for a triple in states x states x nonterminals, represented
    * by their indices in the vectors "states" and "oldGrammar",
    * where "oldGrammar[nonterminalIndex].first" is the nonterminal we care about.
    *
    * See intersectionWithCFG(..) for details.
    *
    * WARNING: if you use an SR where the string representations of elements are NOT strings over
    * the English alphabet (e.g. they may contain 1s to represent epsilons, or they may use a
    * different character set), then this function will possibly not do what you want it to do.
    * The problem we run into there is that we have to process all productions of the form
    * A -> X_1 X_2 X_3 ... X_m where the (X_i)s are all elements of either the terminal or the
    * nonterminal alphabet, so having any symbols in the grammar that don't belong to either
    * will give you trouble
    *
    * WARNING: will currently break if used with anything but NonCommutativePolynomialBase<LossyFiniteAutomaton>
    */
   NonCommutativePolynomialBase<LossyFiniteAutomaton> intersectionPolynomial(std::vector<unsigned long> &states,
       std::unordered_map<unsigned long, std::unordered_map<unsigned char, std::forward_list<unsigned long>>> &transitionTable,
       unsigned long &startState,
       unsigned long &targetState,
       std::unordered_map<unsigned long, unsigned long> &statesToIndices,
       std::unordered_map<VarId, unsigned long> &oldVariablesToIndices,
       std::vector<std::vector<std::vector<VarId>>> &newVariables) const {

     NonCommutativePolynomialBase<LossyFiniteAutomaton> result = NonCommutativePolynomialBase<LossyFiniteAutomaton>::null();
     bool epsilonAdded = false;

     // delegate to the monomials
     for(auto &monomial: this->monomials_) {

       // if the nonterminal can produce epsilon, then the new grammar can produce epsilon only without
       // the FA changing state (since the FA does not have epsilon transitions)
       if(monomial.first.isEpsilonMonomial()) {
         if(startState == targetState && !epsilonAdded) {
           result += NonCommutativePolynomialBase<LossyFiniteAutomaton>::one();
           epsilonAdded = true;
         }
       } else { // if this is a non-epsilon production, we calculate the new productions that derive from it
         result += static_cast<lossyMon>(monomial.first).intersectionPolynomial
             (states, transitionTable, startState, targetState,
                 statesToIndices, oldVariablesToIndices, newVariables);
       }
     }

     return result;
   }

   /*
    * Finds a shortest word in the language described by the grammar starting from "startSymbol" and having
    * productions "productions". Will be null if the language is empty. Undefined behavior if startSymbol is
    * not productive in the grammar defined by productions - "productions" must define a clean grammar starting
    * from "startSymbol".
    */
   static LossyFiniteAutomaton shortestWord(NCEquationsBase<LossyFiniteAutomaton> &productions, VarId &startSymbol) {

     if(productions.size() == 0) {
       return LossyFiniteAutomaton::null();
     }

     // check that the grammar contains the start symbol
     bool startSymbolIsProductive = false;
     for(auto &equation: productions) {
       if(equation.first == startSymbol) {
         startSymbolIsProductive = true;
       }
     }

     if(!startSymbolIsProductive) {
       return LossyFiniteAutomaton::null();
     }

     std::unordered_map<VarId, unsigned long> lengthOfShortestWords;
     std::unordered_map<VarId, NonCommutativeMonomial<LossyFiniteAutomaton>> productionsForShortestWords;

     for(auto &production: productions) {
       lengthOfShortestWords.insert(std::make_pair(production.first, ULONG_MAX));
     }

     // while some shorter derivable word was found, check whether we can now find some other shorter
     // derivable word; terminates once we know a shortest derivable word for every nonterminal
     bool update;
     do {
       update = false;

       for(auto &equation: productions) {
         for(auto &monomial: equation.second.monomials_) {
           update |= static_cast<lossyMon>(monomial.first).findLengthOfDerivableStrings(lengthOfShortestWords, productionsForShortestWords, equation.first);
         }
       }
     } while(update);

     assert(productionsForShortestWords.find(startSymbol) != productionsForShortestWords.end());
     return productionsForShortestWords[startSymbol].shortestDerivableTerminal(productionsForShortestWords);
   }

   /*
    * Finds all terminal symbols that can be the first letter of some word in the language of the grammar.
    */
   static std::set<char> getDerivableFirstLettes(const NCEquationsBase<LossyFiniteAutomaton> &productions, const VarId &startSymbol) {
     std::unordered_map<VarId, std::set<char>> varToDerivableFirstLetters;
     std::set<VarId> varsWithEpsilon;
     if(productions.size() != 0) {

       // while we find new derivable first letters, continue updating
       bool update;
       do {
         update = false;

         for(auto &equation: productions) {
           for(auto &monomial: equation.second.monomials_) {
             update |= static_cast<lossyMon>(monomial.first).getDerivableFirstLettes(varToDerivableFirstLetters, varsWithEpsilon, equation.first);
           }
         }
       } while(update);
     }

     return varToDerivableFirstLetters[startSymbol];
   }

   /*
    * Returns a cleaned version of this polynomial, i.e. a version that had all monomials with unproductive
    * variables eliminated.
    */
   NonCommutativePolynomialBase<LossyFiniteAutomaton> removeUnproductiveMonomials(const std::unordered_map<VarId, bool> &productiveVariables) const {
     NonCommutativePolynomialBase<LossyFiniteAutomaton> cleanPoly = null();

     // add all monomials to the clean polynomial that only contain productive variables
     for(auto &monomial: this->monomials_) {
       if(static_cast<lossyMon>(monomial.first).isProductive(productiveVariables)) {
         InsertMonomial(cleanPoly.monomials_, monomial.first, monomial.second);
       }
     }

     return cleanPoly;
   }


   /*
    * Calculates the intersection of the CFG "equations" with start symbol "oldS" and the
    * regular language represented by a finite automaton. The result is a CFG given by
    * its set of productions (return value of this method) and its start symbol "S".
    *
    * The algorithm is that of Bar-Hillel et al., "On formal properties of simple phrase structure
    * grammars.", 1961; a neat description of it can be found in Nederhof & Satta, "Probabilistic Parsing", 2008
    * which is openly accessible on Nederhof's website as of the writing of this code (spring 2014).
    *
    * libfa doesn't model epsilon transitions (see add_epsilon_trans(state *from, state *to) in fa.c of the
    * source code of libfa for details), instead replacing them by transitions on nonempty symbols and
    * adjusting the set of final states; thus the simplifying assumption of "Probabilistc Parsing" that
    * there are no epsilon transitions in the FA holds, and we only have to deal with the fact that the FA
    * may have multiple final states.
    *
    * WARNING: do not allow productions of the form "X -> empty set" in your oldGrammar, or this may break.
    */

   static NCEquationsBase<LossyFiniteAutomaton> intersectionWithFA
       (const FiniteAutomaton &fa, VarId &newS, const VarId &oldS,
               const NCEquationsBase<LossyFiniteAutomaton> &oldGrammar) {

       // do it with a DFA, something went wrong with NFAs
       FiniteAutomaton minDfa = fa.minimize();

       // for the new grammar
       NCEquationsBase<LossyFiniteAutomaton> resultGrammar;

       // we don't want to start out with useless stuff because the intersection grammar
       // will blow up anyway
       std::queue<VarId> worklist;
       worklist.push(oldS);
       auto workGrammar = cleanSystem(oldGrammar, worklist);

       // change the grammar to one where monomials have degree at most 2 and those monomials with degree 2
       // have the form XY
       ValuationMap<LossyFiniteAutomaton> variablesToConstants;
       workGrammar = eliminateTerminalsInNonterminalProductions
               (workGrammar, variablesToConstants, false);
       workGrammar = binarizeProductions(workGrammar);

       // if the grammar doesn't produce anything, there is no need to do anything else; return an empty grammar
       // the same applies if the automaton represents the empty language
       if(workGrammar.size() == 0 || fa.empty()) {
           return resultGrammar;
       }

       /*
        * assuming anyone at all will have to change something here in the future, I expect they
        * won't want to go through the hassle of sifting through the undocumented implementation
        * of libfa, so I'm gonna go ahead and build the transition table of the FA and hope it won't
        * completely destroy our memory limitations; all of this will be represented using number
        * types only, so there's no funny business or trickery to watch out for once the table is built
        */

       // to map the hash of a state "q" to a map that maps each transition symbol "c" occurring at "q"
       // to the set of hashes of target states "t", i.e. t is element of Delta(q,c);
       std::unordered_map<unsigned long, std::unordered_map<unsigned char, std::forward_list<unsigned long>>> transitionTable;

       // will hold the hashes of all states in the FA; we don't want to deal with with the internals of libfa
       // so all this stuff is represented by simple numbers - hooray for numbers
       std::vector<unsigned long> states;

       // hashes of all final states of the FA
       std::set<unsigned long> finalStates;

       // initial state of the FA
       unsigned long initialState = 0;

       minDfa.extractTransitionTable(transitionTable, states, finalStates, initialState);

       // we will need a three-dimensional lookup table for the new variables so we don't mess up
       // the assignment of polynomials to nonterminals while constructing the grammar
       std::vector<std::vector<std::vector<VarId>>> newVariables;

       // the lookup table will represent states_FA x states_FA x nonterminals_grammar;
       // initialize its dimensions accordingly
       auto numberOfStates = states.size();
       newVariables.resize(numberOfStates);
       for(unsigned long i = 0 ; i < numberOfStates ; ++i) {
           newVariables[i].resize(numberOfStates);

           for(unsigned long j = 0; j < numberOfStates; j++) {
               newVariables[i][j].resize(workGrammar.size());
           }
       }

       // generate the variables of the new grammar, name them <q,X,r> where q and r are states of the DFA and X is
       // a nonterminal of the grammar
       for(unsigned long i = 0; i < numberOfStates; i++) {
           for(unsigned long j = 0; j < numberOfStates; j++) {
               for(unsigned long k = 0; k < workGrammar.size(); k++) {
                   std::stringstream ss;
                   ss << "<" << i << "," << Var::GetVar(workGrammar[k].first).string() << "," << j << ">";
                   newVariables[i][j][k] = Var::GetVarId(ss.str());
               }
           }
       }

       // build a map from variables to indices, where the index of a variable is the line in oldGrammar
       // that contains the productions of the variable
       std::unordered_map<VarId, unsigned long> oldVariablesToIndices;
       for(unsigned long i = 0; i < workGrammar.size(); i++) {
           oldVariablesToIndices.insert(std::make_pair(workGrammar[i].first, i));
       }

       // map states to indices; this is just to avoid having to look through the states vector in linear time
       // to find out at which index it lies
       std::unordered_map<unsigned long, unsigned long> statesToIndices;
       for(unsigned long i = 0; i < states.size(); i++) {
           statesToIndices.insert(std::make_pair(states[i], i));
       }

       /*
        * now we generate the productions of the new grammar
        */

       // the indices k, i, j are intended this way; we first choose some nonterminal of the grammar
       // and then generate all productions derived from that nonterminal; this mirrors the way
       // in which the algorithm is described in the paper referred to above
       NonCommutativePolynomialBase<LossyFiniteAutomaton> poly;
       for(unsigned long i = 0; i < numberOfStates; i++) {
           for(unsigned long j = 0; j < numberOfStates; j++) {
               for(unsigned long k = 0; k < workGrammar.size(); k++) {
                   poly = static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(workGrammar[k].second).intersectionPolynomial
                           (states, transitionTable, states[i], states[j], statesToIndices, oldVariablesToIndices, newVariables);
                   resultGrammar.push_back(std::make_pair(newVariables[i][j][k], poly));
               }
           }
       }

       // finally, use a new variable as initial variable and generate the productions; they are of the form
       // newS -> <q_0, oldS, q_f> where q_0 is the initial state of the FA and q_f is some final state of the FA
       NonCommutativePolynomialBase<LossyFiniteAutomaton> startPolynomial = NonCommutativePolynomialBase<LossyFiniteAutomaton>::null();

       for(auto target: finalStates) {
           startPolynomial += newVariables[statesToIndices[initialState]][statesToIndices[target]][oldVariablesToIndices[oldS]];
       }

       newS = Var::GetVarId();
       resultGrammar.push_back(std::make_pair(newS, startPolynomial));

       auto startProduction = resultGrammar[resultGrammar.size() - 1];
       resultGrammar[resultGrammar.size() - 1] = resultGrammar[0];
       resultGrammar[0] = startProduction;
       return resultGrammar;
   }

   /*
    * Takes two grammars and refines them by intersecting them with the prefix languages of their shared downward closure.
    */
   static LossyFiniteAutomaton refineCourcelle(const NCEquationsBase<LossyFiniteAutomaton> &equations_1,
           const VarId &S_1, const NCEquationsBase<LossyFiniteAutomaton> &equations_2,
           const VarId &S_2, int maxLengthOfPrefixes) {

       LossyFiniteAutomaton approx_1 = downwardClosureCourcelle(equations_1, S_1);
       LossyFiniteAutomaton approx_2 = downwardClosureCourcelle(equations_2, S_2);

       bool A1_subset_A2 = approx_2.contains(approx_1);
       bool A2_subset_A1 = approx_1.contains(approx_2);

       if(A1_subset_A2 && A2_subset_A1) {
           LossyFiniteAutomaton difference = LossyFiniteAutomaton::null();

           if(!(approx_1 == LossyFiniteAutomaton::null()) && !(approx_1 == LossyFiniteAutomaton::one())) {

               // build the automaton Sigma*
               std::string regexAlphabetStar = "[";
               for (auto c: approx_1.alphabet()) {
                   regexAlphabetStar.append(1, c);
               }
               regexAlphabetStar += "]*";

               // build the map of (length, prefixes of that length)
               std::set<char> derivableFirstLetters = getDerivableFirstLettes(equations_1, S_1);
               std::set<char> derivableFirstLetters2 = getDerivableFirstLettes(equations_2, S_2);

               for(char letter: derivableFirstLetters2) {
                   derivableFirstLetters.insert(letter);
               }

               std::unordered_map<int, std::set<std::string>> prefixesPerLength = approx_1.prefixesToMaxLength(maxLengthOfPrefixes, derivableFirstLetters);

               // iterate over the prefixes
               bool noDifference = true;
               std::queue<VarId> worklist;
               for(int i = 1; (i <= maxLengthOfPrefixes) && noDifference; i++) {
                   std::cout << "refining " << i << "..." << std::endl;

                   for(auto &prefix: prefixesPerLength[i]) {
                       std::string prefixAlphaStar = std::string(prefix) + regexAlphabetStar;
                       LossyFiniteAutomaton prefixAutomaton(prefixAlphaStar);

                       VarId S_1_partition, S_2_partition;

                       // generate the subset of language 1
                       auto equations_1_Partition = intersectionWithFA(prefixAutomaton.getFA(), S_1_partition, S_1, equations_1);

                       while(!worklist.empty()) {
                           worklist.pop();
                       }

                       worklist.push(S_1_partition);
                       equations_1_Partition = cleanSystem(equations_1_Partition, worklist);

                       // generate the subset of language 2
                       auto equations_2_Partition = intersectionWithFA(prefixAutomaton.getFA(), S_2_partition, S_2, equations_2);

                       while(!worklist.empty()) {
                           worklist.pop();
                       }

                       worklist.push(S_2_partition);
                       equations_2_Partition = cleanSystem(equations_2_Partition, worklist);

                       // approximate the grammars
                       LossyFiniteAutomaton approx_1_part = downwardClosureCourcelle(equations_1_Partition, S_1_partition);
                       LossyFiniteAutomaton approx_2_part = downwardClosureCourcelle(equations_2_Partition, S_2_partition);

                       LossyFiniteAutomaton tempDiff = compareClosures(equations_1_Partition, S_1_partition, equations_2_Partition, S_2_partition, approx_1_part, approx_2_part);

                       if(!(tempDiff == LossyFiniteAutomaton::null())) {
                           difference = tempDiff;
                           noDifference = false;
                           std::cout << "different " << i << std::endl;
                           break;
                       }
                   }
               }
           } else { // both languages empty
               std::cout << "equal 0" << std::endl;
               return difference;
           }

           if(difference == LossyFiniteAutomaton::null()) {
               std::cout << "maybe_equal " << maxLengthOfPrefixes << std::endl;
           }

           return difference;
       } else {
           LossyFiniteAutomaton difference = compareClosures(equations_1, S_1, equations_2, S_2, approx_1, approx_2);
           std::cout << "different 0" << std::endl;

           return difference;
       }
   }


   /*
    * Calculates the downward closure of the grammar defined by "equations" starting at "S"; the algorithm
    * derives from the "On Constructing Obstruction Sets of Words", B. Courcelle, Bulletin of EATCS 1991.
    *
    * WARNING: This function will break if you use a LossyFiniteAutomaton that does not have a constructor that takes POSIX
    * regex string or if there are any terminals in the grammar that contain non-alphanumeric letters,
    * see Solvers/FPSolve/Polynomials/NonCommutativeMonomial.get_terminals().
    */
   static LossyFiniteAutomaton downwardClosureCourcelle(const NCEquationsBase<LossyFiniteAutomaton> &equations, const VarId &S) {

       std::queue<VarId> worklist;
       worklist.push(S);

       auto cleanEquations = cleanSystem(equations, worklist);

       // if the start symbol is unproductive, then it doesn't generate anything;
       // the downward closure of the empty set is the empty set
       if(!variableHasProduction(cleanEquations, S)) {
           return LossyFiniteAutomaton::null();
       }

       NCEquationsBase<LossyFiniteAutomaton> cleanQNF = quadraticNormalForm(cleanEquations, true);
       std::vector<NCEquationsBase<LossyFiniteAutomaton> > components = group_by_scc(cleanQNF, false);

       // will hold the downward closure of all components
       std::unordered_map<int, LossyFiniteAutomaton> componentToClosure;

       // map variables to their components
       std::unordered_map<VarId, int> varToComponent = mapVariablesToComponents(components);

       // these components will have the star of the letters reachable from the variables
       // of the respective component as their closure
       std::set<int> squarableComponents = findSquarableComponents(components, varToComponent);

       // we need this map for the case where we can duplicate a variable
       std::unordered_map<int, std::set<unsigned char>> componentToReachableLetters =
               findReachableLetters(components, varToComponent);

       // we can already calculate the downward closures of squarable components
       calculateClosuresOfSquarableComponents(componentToReachableLetters, squarableComponents, componentToClosure);

       // for each component, this map holds all pairs AB where A and B are in a lower component;
       // we use the downward closure for those pairs and concatenate accordingly to construct the
       // "middle" part of the downward closure the way Courcelle calculates it
       std::unordered_map<int, std::unordered_map<int, std::set<int>>> quadraticLHStoRHS = mapQuadraticLHStoRHS(components, varToComponent, squarableComponents);

       // get the components of variables Y_l and Y_r such that there is a monomial in the component of any nonsquarable B
       // that has the form Y_l*B or B*Y_r;
       // their closures will be part of the lefthand/righthand terms of the calculation by Courcelle
       std::unordered_map<int, std::set<int>> lhsLowerComponentVariables;
       std::unordered_map<int, std::set<int>> rhsLowerComponentVariables;
       calculateLowerComponentVariables(lhsLowerComponentVariables, rhsLowerComponentVariables,
               components, varToComponent, squarableComponents);

       // for the middle part of each component, we also need the sum of the closures of all constant monomials
       // appearing in that component
       std::unordered_map<int, LossyFiniteAutomaton> closuresOfConstantMonomials;
       calculateClosuresOfConstantMonomials(closuresOfConstantMonomials, components, squarableComponents);

       // we have everything we can prepare beforehand; everything else will need to be dealt with while
       // putting the closures together
       calculateClosuresOfNonsquarableComponents(componentToClosure, components,
               squarableComponents, quadraticLHStoRHS,
               lhsLowerComponentVariables, rhsLowerComponentVariables,
               closuresOfConstantMonomials, varToComponent, componentToReachableLetters);

       return componentToClosure[varToComponent[S]];
   }

private:

   /*
    * Binarizes this polynomial, as one would when calculating a CNF of a grammar. Make sure all monomials
    * either have no terminals (i.e. semiring elements) or no nonterminals (i.e. variables) as factors.
    *
    * If a monomial consists of exactly one variable, it will not be changed (chain productions are not eliminated).
    *
    * Make sure there are no productions with both terminals and nonterminals as factors; those will be eliminated if
    * they have at least 3 variables as factors. Call eliminateTerminalsInNonterminalProductions(..) first to avoid this.
    *
    * The variables and productions that are introduced during that process will be stored in binarizationVariables
    * and binarizationVariablesEquations, respectively.
    */
   NonCommutativePolynomialBase<LossyFiniteAutomaton> binarize(std::unordered_map<std::string, VarId> &binarizationVariables,
       NCEquationsBase<LossyFiniteAutomaton> &binarizationVariablesEquations) const {
     NonCommutativePolynomialBase<LossyFiniteAutomaton> binarizedPoly = null();

     // delegate to the monomials
     for(auto &monomial: this->monomials_) {
       if(monomial.first.get_degree() > 2) { // only binarize productions that have at least 3 variables in them
         binarizedPoly += static_cast<lossyMon>(monomial.first).binarize(binarizationVariables, binarizationVariablesEquations);
       } else {
         InsertMonomial(binarizedPoly.monomials_, monomial.first, monomial.second);
       }
     }

     return binarizedPoly;
   }

   /*
    * Brings a system into quadratic normal form.
    */
   static NCEquationsBase<LossyFiniteAutomaton> quadraticNormalForm
   (const NCEquationsBase<LossyFiniteAutomaton> &equations,
       bool eliminateTerminalsInLinearProductions) {
     NCEquationsBase<LossyFiniteAutomaton> systemBeingProcessed;

     // for all nonterminal productions, introduce new variables for the terminal factors that
     // appear in them as one would when calculating the CNF of a CFG; do not do this for linear terms since
     // QNF allows for linear terms
     ValuationMap<LossyFiniteAutomaton> variablesToConstants;
     systemBeingProcessed = eliminateTerminalsInNonterminalProductions(equations, variablesToConstants, eliminateTerminalsInLinearProductions);

     // binarize all productions, i.e. nonterminal productions of length at least 3 will be split up until
     // there are only productions of length <= 2 left
     systemBeingProcessed = binarizeProductions(systemBeingProcessed);

     return systemBeingProcessed;
   }


   /*
    * Lossifies a given QNF; this means that every variable occurring in a polynomial is added to it and 1 is added
    */
   static void lossifyQNF(NCEquationsBase<LossyFiniteAutomaton> &equations) {

     // add the monomials of degree 1; since we are in an idempotent semiring, we have a+a = a and so
     // we don't need to worry about the number of occurrences of each variable
     std::set<VarId> vars;
     NonCommutativePolynomialBase<LossyFiniteAutomaton> monomialsOfDegreeOne;
     for(auto &equation: equations) {
       vars = static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).get_variables_quadratic_monomials();
       monomialsOfDegreeOne = NonCommutativePolynomialBase<LossyFiniteAutomaton>::null();

       for(VarId var: vars) {
         monomialsOfDegreeOne += NonCommutativePolynomialBase<LossyFiniteAutomaton>(var);
       }

       equation.second = equation.second + monomialsOfDegreeOne;
     }

     // add 1 to each equation
     for(auto &equation: equations) {
       equation.second = equation.second + NonCommutativePolynomialBase<LossyFiniteAutomaton>::one();
     }
   }


   /*
    * Checks if a given variable is productive in a given clean system.
    */
   static bool variableHasProduction(const NCEquationsBase<LossyFiniteAutomaton> &cleanEquations, const VarId &S) {
     bool ShasProduction = false;

     for(auto &equation: cleanEquations) {
       if(equation.first == S) {
         ShasProduction = true;
       }
     }

     return ShasProduction;
   }

   static std::unordered_map<VarId, int> mapVariablesToComponents(
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components) {

     std::unordered_map<VarId, int> varToComponent;

     for(int i = 0; i < components.size(); i++) {
       for(auto &equation: components[i]) {
         varToComponent.insert(std::make_pair(equation.first, i));
       }
     }

     return varToComponent;
   }

   /*
    * Finds components where we can duplicate literals, i.e. where we have derivations
    * of the form A ->* _A_A_; in the paper by Courcelle, those are the components where A <_2 A.
    */
   static std::set<int> findSquarableComponents(std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
                                                std::unordered_map<VarId, int> &varToComponent) {

     std::set<int> squarableComponents;

     for(int i = 0; i < components.size(); i++) {
       bool squarable = false;

       for(auto &equation: components[i]) {
         squarable |= static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).componentIsSquarable(varToComponent, i);

         if(squarable) {
           squarableComponents.insert(i);
           break;
         }
       }
     }

     return squarableComponents;
   }

   /*
    * Find out which linear monomials in this polynomial lead to a lower component and insert them into
    * lowerLinearTerms.
    *
    * Used in the algorithm by Courcelle, see downwardClosureCourcelle.
    * This is only here because we need access to monomials_.
    */
   void findLowerLinearTerms(std::set<int> &lowerLinearTerms, std::unordered_map<VarId, int> &varToComponent, int component) const {
       for(auto &monomial: this->monomials_) {
           static_cast<lossyMon>(monomial.first).findLowerLinearTerms(lowerLinearTerms, varToComponent, component);
       }
   }

   /*
    * Find for each component the components which are reachable from it.
    */
   static std::unordered_map<int, std::set<int>> findReachableComponents(std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
                                                               std::unordered_map<VarId, int> &varToComponent) {
     std::unordered_map<int, std::set<int>> reachabilityMap;

     // iterate over all components
     for(int i = 0; i < components.size(); i++) {
       std::set<int> reachableComponents;
       reachableComponents.insert(i);

       // build the set of reachable components: for each variable in the component,
       // check which other variables it can produce and add their components to the set of
       // reachable components
       for(auto &equation: components[i]) {
         auto tmp = equation.second.get_variables();

         for(auto var: tmp) {

           // don't re-add the stuff from within the current component
           if(varToComponent[var] < i) {
             auto tmpReachable = reachabilityMap[varToComponent[var]];
             reachableComponents.insert(tmpReachable.begin(), tmpReachable.end());
           }
         }
       }

       reachabilityMap.insert(std::make_pair(i, reachableComponents));
     }

     return reachabilityMap;
   }


   /*
    * Find the letters reachable from each component
    *
    * The function assumes that "components" is sorted in reverse topological order, i.e. the component
    * that depends on no other component comes first.
    */
   static std::unordered_map<int, std::set<unsigned char>> findReachableLetters(std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
                                                                      std::unordered_map<VarId, int> &varToComponent) {

     std::unordered_map<int, std::set<unsigned char>> componentToReachableLetters;
     std::unordered_map<int, std::set<int>> reachableComponents = findReachableComponents(components, varToComponent);

     for (int i = 0; i < components.size(); i++) {
       std::set<unsigned char> reachableLetters;

       // iterate over all components reachable from component i
       for(auto reachableComp: reachableComponents[i]) {

         // for every lower component, we have already calculated the set of reachable letters;
         // just add it to the letters reachable from i
         if(reachableComp != i) {
           auto letters = componentToReachableLetters[reachableComp];
           reachableLetters.insert(letters.begin(), letters.end());
         } else { // add the letters appearing in some production that stays within i itself
           for(auto &equation: components[i]) {
             std::set<unsigned char> letters = static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).get_terminals();

             reachableLetters.insert(letters.begin(), letters.end());
           }
         }
       }

       // once we're done, remember the set of reachable letters
       componentToReachableLetters.insert(std::make_pair(i, reachableLetters));
     }

     return componentToReachableLetters;
   }


   static void calculateClosuresOfConstantMonomials(
       std::unordered_map<int, LossyFiniteAutomaton> &closuresOfConstantMonomials,
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::set<int> &squarableComponents) {

     for(int i = 0; i < components.size(); i++) {
       if(squarableComponents.count(i) == 0) {
         LossyFiniteAutomaton closure = LossyFiniteAutomaton::null();

         for(auto &equation: components[i]) {
           closure = closure + static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).sumOfConstantMonomials();
         }

         closuresOfConstantMonomials[i] = closure.lossify().minimize();
       }
     }
   }

   static void calculateClosuresOfNonsquarableComponents(
       std::unordered_map<int, LossyFiniteAutomaton> &componentToClosure,
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::set<int> &squarableComponents,
       std::unordered_map<int, std::unordered_map<int, std::set<int>>> &quadraticLHStoRHS,
       std::unordered_map<int, std::set<int>> &lhsLowerComponentVariables,
       std::unordered_map<int, std::set<int>> &rhsLowerComponentVariables,
       std::unordered_map<int, LossyFiniteAutomaton> &closuresOfConstantMonomials,
       std::unordered_map<VarId, int> &varToComponent,
       std::unordered_map<int, std::set<unsigned char>> &componentToReachableLetters) {

     for(int i = 0; i < components.size(); i++) {

       if(squarableComponents.count(i) == 0){

         // find the reachable letters
         std::set<unsigned char> lefthandReachableLetters;

         for(auto variable: lhsLowerComponentVariables[i]) {
           lefthandReachableLetters.insert(componentToReachableLetters[variable].begin(),
               componentToReachableLetters[variable].end());
         }

         std::set<unsigned char> righthandReachableLetters;

         for(auto variable: rhsLowerComponentVariables[i]) {
           righthandReachableLetters.insert(componentToReachableLetters[variable].begin(),
               componentToReachableLetters[variable].end());
         }

         // build the automata
         LossyFiniteAutomaton lefthandAlphabetStar = LossyFiniteAutomaton::one();
         if(lefthandReachableLetters.size() != 0) {
           std::stringstream ssLHS;
           ssLHS << "[";

           for(auto letter: lefthandReachableLetters) {
             if(isalnum(letter)) {
               ssLHS << letter;
             }
           }

           ssLHS << "]*";
           lefthandAlphabetStar = LossyFiniteAutomaton(ssLHS.str());
         }

         LossyFiniteAutomaton righthandAlphabetStar = LossyFiniteAutomaton::one();
         if(righthandReachableLetters.size() != 0) {
           std::stringstream ssRHS;
           ssRHS << "[";

           for(auto letter: righthandReachableLetters) {
             if(isalnum(letter)) {
               ssRHS << letter;
             }
           }

           ssRHS << "]*";
           righthandAlphabetStar = LossyFiniteAutomaton(ssRHS.str());
         }

         /*
          * closures of elements in the middle
          */

         // find which linear productions appear in this component that lead to a lower component
         std::set<int> lowerLinearTerms;
         for(auto &equation: components[i]) {
           static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).findLowerLinearTerms(lowerLinearTerms, varToComponent, i);
         }

         LossyFiniteAutomaton closure = LossyFiniteAutomaton::courcelle_construct(lefthandAlphabetStar,righthandAlphabetStar,
             quadraticLHStoRHS[i], closuresOfConstantMonomials[i], lowerLinearTerms, componentToClosure, i).lossify().minimize();

         componentToClosure[i] = closure;
       }
     }
   }

   static void calculateClosuresOfSquarableComponents(
       std::unordered_map<int, std::set<unsigned char>> &componentToReachableLetters,
       std::set<int> &squarableComponents,
       std::unordered_map<int, LossyFiniteAutomaton> &componentToClosure) {

     // for squarable components, we only need the set of letters that appear in the strings derivable from that component
     // and star that set; the set is always nonempty since otherwise, the variable would be unproductive and would have
     // been eliminated while cleaning the system
     for(auto i: squarableComponents) {
       std::set<unsigned char> reachableLetters = componentToReachableLetters[i];

       if(reachableLetters.empty()) {
         componentToClosure.insert(std::make_pair(i, LossyFiniteAutomaton::one()));
       } else {
         std::stringstream ss;
         ss << "[";

         for(unsigned char letter: reachableLetters) {
           ss << letter;
         }

         ss << "]*";
         componentToClosure.insert(std::make_pair(i, LossyFiniteAutomaton(ss.str())));
       }
     }
   }

   /*
    * Finds monomials of degree two where both variables are in a different and therefore lower
    * scc than the axiom of the production.
    *
    * Assumes that "components" is sorted in reverse topological order.
    */
   static std::unordered_map<int, std::unordered_map<int, std::set<int>>>  mapQuadraticLHStoRHS(
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::unordered_map<VarId, int> &varToComponent,
       std::set<int> &squarableComponents) {

     std::unordered_map<int, std::unordered_map<int, std::set<int>>> quadraticLHStoRHS;

     for(int i = 0; i < components.size(); i++) {
       if(squarableComponents.count(i) == 0) {
         for(auto &equation: components[i]) {
           static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).mapQuadraticLHStoRHS(quadraticLHStoRHS, varToComponent, i);
         }
       }
     }

     return quadraticLHStoRHS;
   }

   static void calculateLowerComponentVariables(
       std::unordered_map<int, std::set<int>> &lhsLowerComponentVariables,
       std::unordered_map<int, std::set<int>> &rhsLowerComponentVariables,
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::unordered_map<VarId, int> &varToComponent,
       std::set<int> &squarableComponents) {

     for(int i = 0; i < components.size(); i++) {
       if(squarableComponents.count(i) == 0) {
         for(auto &equation: components[i]) {
           static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).calculateLowerComponentVariables(lhsLowerComponentVariables, rhsLowerComponentVariables,
               components, varToComponent, i);
         }
       }
     }
   }

   static void calculateSameComponentLetters(
       std::unordered_map<int, std::set<unsigned char>> &lhsSameComponentLetters,
       std::unordered_map<int, std::set<unsigned char>> &rhsSameComponentLetters,
       std::vector<NCEquationsBase<LossyFiniteAutomaton>> &components,
       std::unordered_map<VarId, int> &varToComponent,
       std::set<int> &squarableComponents) {

     for(int i = 0; i < components.size(); i++) {
       if(squarableComponents.count(i) == 0) {
         for(auto &equation: components[i]) {
           static_cast<NonCommutativePolynomial<LossyFiniteAutomaton> >(equation.second).calculateSameComponentLetters(lhsSameComponentLetters, rhsSameComponentLetters,
               components, varToComponent, i);
         }
       }
     }
   }

   static LossyFiniteAutomaton compareClosures(const NCEquationsBase<LossyFiniteAutomaton> &equations_1,
       const VarId &S_1, const NCEquationsBase<LossyFiniteAutomaton> &equations_2,
       const VarId &S_2, const LossyFiniteAutomaton& approx_1, const LossyFiniteAutomaton& approx_2) {

     bool A1_subset_A2 = approx_2.contains(approx_1);
     bool A2_subset_A1 = approx_1.contains(approx_2);

     if(A1_subset_A2 && A2_subset_A1) {
       return LossyFiniteAutomaton::null();
     } else {
       LossyFiniteAutomaton L1_intersect_A2c = LossyFiniteAutomaton::null();
       bool L1_intersect_A2c_changed = false;
       LossyFiniteAutomaton L2_intersect_A1c = LossyFiniteAutomaton::null();
       bool L2_intersect_A1c_changed = false;

       if(!A1_subset_A2) {
         VarId startSymbol_1_2;
         auto A2c = approx_1.minus(approx_2);
         auto intersectionGrammar = intersectionWithFA(A2c.getFA(), startSymbol_1_2, S_1, equations_1);

         std::queue<VarId> worklist;
         worklist.push(startSymbol_1_2);
         intersectionGrammar = cleanSystem(intersectionGrammar, worklist);

         L1_intersect_A2c = shortestWord(intersectionGrammar, startSymbol_1_2);
         L1_intersect_A2c_changed = true;
       }

       if(!A2_subset_A1) {
         VarId startSymbol_2_1;
         auto A1c = approx_2.minus(approx_1);
         auto intersectionGrammar_2 = intersectionWithFA(A1c.getFA(), startSymbol_2_1, S_2, equations_2);

         std::queue<VarId> worklist;
         worklist.push(startSymbol_2_1);
         intersectionGrammar_2 = cleanSystem(intersectionGrammar_2, worklist);

         L2_intersect_A1c = shortestWord(intersectionGrammar_2, startSymbol_2_1);
         L2_intersect_A1c_changed = true;
       }

       if(L1_intersect_A2c_changed && L2_intersect_A1c_changed) {
         if(L1_intersect_A2c.size() <= L2_intersect_A1c.size() && L1_intersect_A2c != LossyFiniteAutomaton::null()) {
           return L1_intersect_A2c;
         } else {
           return L2_intersect_A1c;
         }
       } else {
         if(L1_intersect_A2c_changed) {
           return L1_intersect_A2c;
         } else {
           return L2_intersect_A1c;
         }
       }
     }
   }

};



#endif /* FPSOLVE_LOSSY_NON_COMMUTATIVE_POLYNOMIAL_H */
