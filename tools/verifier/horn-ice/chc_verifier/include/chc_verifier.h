/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_CHC_VERIFIER_H__
#define __CHCTEACHER_CHC_VERIFIER_H__

// C++ includes
#include <list>
#include <memory>
#include <stdexcept>
#include <vector>

// C includes
#include <cassert>

// Project includes
#include "conjecture.h"
#include "datapoint.h"
#include "horn_counterexample.h"
#include "seahorn_smtlib2_parser.h"

// Z3 includes
#include "z3++.h"


namespace chc_teacher
{

	class chc_verifier
	{

	public:

		/**
		 * Naively checks all CHC of a given problem 
		 */
		static std::unique_ptr<horn_counterexample> naive_check(z3::context & ctx, const problem & p, const std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> & conjectures)
		{
			
			//
			// First, do some sanity checks
			//
			for (const auto & r : p.relations)
			{
				assert (conjectures.count(r) > 0);
			}
			
			//
			// Check each CHC and return counterexample if detected
			//
			for (const auto & chc : p.chcs)
			{
				
				// Check CHC
				auto counterexample = check_chc(ctx, chc, conjectures);
				
				// If counterexample is detected, return it
				if (counterexample != nullptr)
				{
					return counterexample;
				}
				
			}

			//
			// Every CHC passed, return nullptr (indicating success)
			//
			return std::unique_ptr<horn_counterexample>(nullptr);
			
		}

		
		/**
		 * Checks all CHC of a given problem, skipping CHC that involve conjectures that
		 * have not changed (i.e., are equal in old_conjectures). (It is implicitely assumed
		 * that these CHC have been proven correct earlier.)
		 */
		static std::unique_ptr<horn_counterexample> check(z3::context & ctx, const problem & p, const std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> & new_conjectures, const std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> & old_conjectures)
		{
			
			//
			// First, do some sanity checks
			//
			for (const auto & r : p.relations)
			{
				assert (new_conjectures.count(r) > 0);
			}
			
			
			//
			// Check which conjectures have changed
			//
			std::unordered_map<z3::func_decl, bool, ASTHasher, ASTComparer> changed(new_conjectures.size());
			for (const auto & pair : new_conjectures)
			{
				
				// Get old conjecture for declaration (if exists)
				auto old_conjecture_it = old_conjectures.find(pair.first);
				
				// Store whether conjecture has changed
				changed[pair.first] = (old_conjecture_it == old_conjectures.end() || !(pair.second == old_conjecture_it->second));
				
			}
			// DEBUG
			// // // std::cout << "---------- Changes of conjectures ----------" << std::endl;
			//for (const auto & status : changed)
			{
				// // // std::cout << status.first << ": " << status.second << std::endl;
			}

			//
			// Check each CHC and return counterexample if detected
			//
			for (const auto & chc : p.chcs)
			{
				
				//
				// Check whether conjectures for relations occurring in the CHC have changed
				//
				bool any_conjecture_changed = false;
				for (const auto & decl : chc.uninterpreted_predicates)
				{
					if (changed.at(decl))
					{
						any_conjecture_changed = true;
						break;
					}
				}
				
				//
				// Check CHC only if at least one conjecture has changed
				//
				if (any_conjecture_changed)
				{
					
					auto counterexample = check_chc(ctx, chc, new_conjectures);
					
					// If counterexample is detected, return it
					if (counterexample != nullptr)
					{
						return counterexample;
					}

				}
					
			}

			//
			// Every CHC passed, return nullptr (indicating success)
			//
			return std::unique_ptr<horn_counterexample>(nullptr);
			
		}
		
		
		
		static datapoint extract_datapoint(const z3::expr & predicate_expr, const z3::model & model)
		{
			
			std::vector<z3::expr> values;
			values.reserve(predicate_expr.num_args());
			for (unsigned i = 0; i < predicate_expr.num_args(); ++i)
			{
				values.push_back(model.eval(predicate_expr.arg(i), true));
			}
			
			
			return datapoint(predicate_expr.decl(), std::move(values));
			
		}
		
		
		static std::unique_ptr<horn_counterexample> check_chc(z3::context & ctx, const constrainted_horn_clause & chc, const std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> & conjectures)
		{
			
			// // // std::cout << std::endl << "========== PERFORMING CHECK OF CHC ==========" << std::endl << std::endl;


			//
			// 1. Get CHC to check
			//
			z3::expr chc_expr = chc.expr;
			// // // std::cout << "Original expression is " << std::endl << chc_expr << std::endl;

			
			//
			// 2. Substitute uninterpreted predicates in CHC with conjectures
			//
			for (const auto & decl : chc.uninterpreted_predicates)
			{
			
				// Get conjecture
				auto & pred_conjecture = conjectures.at(decl);
				
				// Check that variable count matches
				assert (pred_conjecture.variables.size() == decl.arity());

				// // // std::cout << "Substitutinng " << decl << " with " << pred_conjecture << std::endl;
				
				// Do substitution
				chc_expr = z3_helper::substitute(ctx, chc_expr, decl, pred_conjecture.expr, pred_conjecture.variables);
			
			}
			// // // std::cout << "---------- CHC after substituting ----------" << std::endl;
			// // // std::cout << chc_expr << std::endl;
			
			
			//
			// 3. Create solver and create final satifiability problem
			//
			
			// Create solver
			z3::solver solver(ctx);
			// Add negated CHC to solver
			solver.add(!chc_expr);
			// // // std::cout << "---------- Solver ----------" << std::endl << solver << std::endl;
			
			
			//
			// 4. Solve
			//
			auto result = solver.check();
			
			
			//
			// 5. Return success or extract counterexample
			//
			
			// Unsat
			if (result == z3::check_result::unsat)
			{
				// // // std::cout << "UNSAT!!" << std::endl;
				return std::unique_ptr<horn_counterexample>();
			}
			
			// Unknown (i.e., error)
			else if (result == z3::check_result::unknown)
			{
				throw std::runtime_error("Solver reported UNKNOWN");
			}
			
			// Sat, extract counterexample
			else
			{
				
				// // // std::cout << "SAT!!" << std::endl;
				
				// Get model
				auto model = solver.get_model();
				// // // std::cout << "---------- Model ----------" << std::endl << model << std::endl;
				
				
				std::list<datapoint> lhs;
				std::list<datapoint> rhs;
				
				// Extract data points of left-hand-side
				for (const auto & pred : chc.predicates_in_lhs)
				{
					lhs.push_back(extract_datapoint(pred, model));
				}
				
				// Extract data points of right-hand-side
				for (const auto & pred : chc.predicates_in_rhs)
				{
					rhs.push_back(extract_datapoint(pred, model));
				}
				

				return std::make_unique<horn_counterexample>(std::move(lhs), std::move(rhs));
				
			}
			
			
		}

	};

}; // End namespace chc_teacher

#endif
