/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_Z3_HELPER_H__
#define __CHCTEACHER_Z3_HELPER_H__

// C++ includes
#include <list>
#include <unordered_map>
#include <unordered_set>

// C includes
#include <cassert>

// Z3 include
#include "z3++.h"


namespace chc_teacher
{

	/// <summary >Function object to check equality of two Z3 AST objects </summary>
	struct ASTComparer
	{

		bool operator() (const z3::ast & e1, const z3::ast & e2) const
		{
			return z3::eq(e1, e2);
		}

	};

	/// <summary> Function object to hash a Z3 AST objects </summary>
	struct ASTHasher
	{

		unsigned operator() (const z3::ast & e) const
		{
			return e.hash();
		}

	};


	typedef std::unordered_set<z3::expr, ASTHasher, ASTComparer> expr_set;
	typedef std::unordered_set<z3::func_decl, ASTHasher, ASTComparer> decl_set;


	class z3_helper
	{

	public:

		/// Substitutes the occurence of the uninterpreted function f in the expression e with the expression h, where h is constructed using variables.
		static z3::expr substitute(z3::context & ctx, const z3::expr & e, const z3::func_decl & f, const z3::expr & h, const z3::expr_vector & variables)
		{

			// Check whether the number of source variables (i.e., the variables in h) and the arity of f match
			assert(variables.size() == f.arity());

			// Copy expression, which is then modified
			z3::expr result = e;

			// Create cache to store (substituted) expressions
			typedef std::unordered_map<z3::expr, z3::expr, ASTHasher, ASTComparer> ExprMap;
			ExprMap cache;

			// Initialize worklist with root node of the AST
			std::list<z3::expr> worklist;
			worklist.push_back(result);

			//
			// Process the worklist
			//
			while (!worklist.empty())
			{

				// Get worklist entry
				z3::expr cur = worklist.back();

				// We can only handle AST types AST_APP
				// (as they are the only ones that should occur in ur setting)
				assert(cur.is_app());

				// Let's assume that this AST node has been completely processed and substituted
				bool subexpressions_processed = true;

				// Process (or use) all sub-expressions of this expression
				// Hint: everything (i.e., variables, constants, functions) is a function in SMTLib2)
				for (unsigned int i = 0; i < cur.num_args(); ++i)
				{

					// Get i-th argument (sub-expression) of current expression
					z3::expr arg = cur.arg(i);

					// Sub-expression has not yet been processed
					if (cache.count(arg) == 0)
					{

						// Add sub-expression to worklist
						worklist.push_back(arg);

						// Indicate that this expression cannot be processed at this time
						subexpressions_processed = false;

					}

				}

				// If all subexpressions of this expression have been processed, process it
				if (subexpressions_processed)
				{

					// Pop expression from worklist
					worklist.pop_back();

					// Create list of expressions that are used to substitute
					z3::expr_vector dest(ctx);
					for (unsigned int i = 0; i < cur.num_args(); ++i)
					{
						dest.push_back(cache.at(cur.arg(i)));
					}

					// The current expression is the function application of f.
					// Substitute the expression with h
					if (z3::eq(f, cur.decl()))
					{

						// Copy the expression h
						z3::expr copy_of_h = h;

						// Replace the variables in copy_of_h with the processed sub-expressions of f
						z3::expr tmp = copy_of_h.substitute(variables, dest);

						// Store the resulting expression in the cache
						cache.emplace(cur, tmp);

					}

					// The current expression is not the function application of f.
					// Substitute the all subexpressions with processed subexpressions
					else
					{

						// Replace the variables in copy_of_cur with the processed sub-expressions of cur
						z3::expr tmp = cur.decl()(dest);

						// Store the resulting expression in the cache
						cache.emplace(cur, tmp);

					}

				}

			}

			// Returned processed expression
			return cache.at(e);

		}

	};

}; // End namespace chc_teacher

#endif