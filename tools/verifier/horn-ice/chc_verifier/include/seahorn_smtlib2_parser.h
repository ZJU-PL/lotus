/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_SEAHORN_SMTLIB2_PARSER_H__
#define __CHCTEACHER_SEAHORN_SMTLIB2_PARSER_H__

// C++ includes
#include <ostream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

// Z3 includes
#include "z3++.h"

// Project includes
#include "chc.h"
#include "problem.h"
#include "z3_helper.h"


namespace chc_teacher
{

	class seahorn_smtlib2_parser
	{

	public:

		/**
		 * Replaces every occurrence of an application of a 0-ary relation <em>R</em> with
		 * the application of a new 1-ary relation <em>R'(true)</em> in a vector of expressions.
		 * While processing the expressions, this method also records the declaration of each encountered
		 * relation. Only the declarations after the substitution are recorded.
		 *
		 * @param ctx The Z3 context to use
		 * @param expressions A vector of expressions
		 * @param relations The set in which to record all encountered relation declarations
		 *
		 * @return Returns a vector of expressions in which all application of a 0-ary relation <em>R</em>
		 *         have been substituted with the application of a new 1-ary relation <em>R'(true)</em>.
		 */
		static z3::expr_vector preprocess_expressions(z3::context & ctx, const z3::expr_vector & expressions, decl_set & relations)
		{
			
			// Maps 0-ary relations to 1-ary relations
			std::unordered_map<z3::func_decl, z3::func_decl, ASTHasher, ASTComparer> old2new_relations;
			for (unsigned i = 0; i < expressions.size(); ++i)
			{
				collect_and_replace_relations(ctx, expressions[i], old2new_relations, relations);
			}

			// Create expression vectors
			z3::expr_vector source(ctx);
			z3::expr_vector destination(ctx);
			z3::expr_vector new_rules(ctx);
			
			// Create source and destination vectors
			for (const auto & e : old2new_relations)
			{
				source.push_back(e.first());
				destination.push_back(e.second(ctx.bool_val(true)));
			}
			
			// Substitute
			for (unsigned i = 0; i < expressions.size(); ++i)
			{
				new_rules.push_back(expressions[i].substitute(source, destination));
			}
			
			return new_rules;
			
		}
	
	
		/**
		 * Traverses the AST of a given expression and records all (uninterpreted) relations. For each
		 * 0-ary relation that is encondered and that is not yet contained in <code>old2new_relations</code>,
		 * a new 1-ary relation declaration is generated. Only n-ary relations with n > 0 are recorded.
		 *
		 * @param ctx The Z3 context to use
		 * @param expr The expression to process
		 * @param old2new_relations A mapping that maps old 0-ary relations declarations to 1-ary relations declarations
		 * @param relations The set in which to record all encountered relation declarations
		 */
		static void collect_and_replace_relations(z3::context & ctx, const z3::expr & expr, std::unordered_map<z3::func_decl, z3::func_decl, ASTHasher, ASTComparer> & old2new_relations, decl_set & relations)
		{
			
			// Function application
			if (expr.is_app())
			{
				
				const auto & decl = expr.decl();

				// Relation
				if (decl.decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED)
				{
			
					// 0-ary relation. Replace with 1-ary relation and record.
					if (decl.arity() == 0 && old2new_relations.count(decl) == 0)
					{
						auto new_decl = ctx.function((decl.name().str() + "@").c_str(), ctx.bool_sort(), decl.range());
						old2new_relations.emplace(decl, new_decl);
						relations.insert(new_decl);
					}

					// n-ary relation (n > 0). Just record if not verifier.error.
					else if (decl.arity() > 0 && decl.name().str() != "verifier.error")
					{

						// Insert
						auto insert_result = relations.insert(decl);

						// If not already contained, heck that type of arguments are supported (currently bool and int)
						if (insert_result.second)
						{
							for (unsigned i = 0; i < decl.arity(); ++i)
							{
								if (!(decl.domain(i).is_bool() || decl.domain(i).is_int()))
								{
									throw std::runtime_error("Domain of " + decl.name().str() + " not supported");
								}
							}
						}


					}

				}
					
				// Process arguments (if existing)
				for (unsigned i=0; i < expr.num_args(); ++i)
				{
					collect_and_replace_relations(ctx, expr.arg(i), old2new_relations, relations);
				}
				
			}
			
			// Quantified expression
			else if (expr.is_quantifier())
			{
				collect_and_replace_relations(ctx, expr.body(), old2new_relations, relations);
			}
			
			// Quantified variables (skip over)
			else if (expr.is_var())
			{
				
			}
			
			// Unsupported expression
			else
			{
				// std::cout << "ERROR: " << expr << std::endl;
				throw std::runtime_error("Unsupported expression");
			}
			
		}
	
	
	
		static void extract_relations(const z3::expr & expr, decl_set & relations, expr_set & relation_expressions, bool is_negated = false)
		{
			
			//
			// First, we check that the expression's type is supported
			//
			if (expr.is_quantifier())
			{
				throw std::runtime_error("Nested quantifier not supported");
			}
			
			
			//
			// Function application (note that almost everything in Z3 is a function application)
			//
			if (expr.is_app())
			{

				//
				// Predicate to synthesize (we understand all uninterpreted functions as a predicate to synthesize)
				//
				if (expr.decl().decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED && expr.decl().arity() > 0)
				{
					
					// Checks
					assert (expr.decl().name().str() != "verifier.error");
					assert (!is_negated);
					// TODO: check that the expressions in the arguments do not contain predicates to synthesize
					
					// Store expression of predicate
					relation_expressions.insert(expr);
					
					// Store declaration of predicates
					relations.insert(expr.decl());
					
				}

				//
				// Any other function application
				//
				else
				{
					
					// Do recursive descent on any argument
					for (unsigned i = 0; i < expr.num_args(); ++i)
					{
						extract_relations(expr.arg(i), relations, relation_expressions, expr.decl().decl_kind() == Z3_OP_NOT ? !is_negated : is_negated);		
					}
					
				}
				
			}
			
		}
		
		
		static problem parse(z3::context & ctx, const std::string & filename)
		{
			
			//
			// Prepare data structures
			//
			unsigned constant_id = 0;
			decl_set relations;
			std::vector<constrainted_horn_clause> chcs;

			
			//
			// Parse from file
			//
			z3::fixedpoint fp(ctx);
			Z3_ast_vector r = Z3_fixedpoint_from_file(ctx, fp, filename.c_str());
			// std::cout << fp << std::endl;
			fp.check_error();
			auto z3_queries = z3::expr_vector(ctx, r);
			
			
			//
			// Preprocess expressions (i.e., record all relations and substitute
			// 0-ary relations with 1-ary relations).
			//
			auto rules = preprocess_expressions(ctx, fp.rules(), relations);
			auto queries = preprocess_expressions(ctx, z3_queries, relations);
			// std::cout << std::endl << "Read " << rules.size() << " rules and " << queries.size() << " queries (" << z3_queries.size() << ")" << std::endl;
						
			
			//
			// Process queries
			//
			for (unsigned i = 0; i < queries.size(); ++i)
			{
				
				const auto & expr = queries[i];
				
				// Normal query
				if (expr.is_app() && expr.decl().decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED && expr.decl().arity() > 0)
				{
					
					assert (expr.decl().name().str() != "verifier.error");
					
					// Check that arguments are constants
					for (unsigned j = 0; j < expr.num_args(); ++j)
					{
						if (!(expr.arg(j).is_const() && expr.arg(j).decl().decl_kind() != Z3_decl_kind::Z3_OP_UNINTERPRETED))
						{
							throw std::runtime_error("Argument of query is not a constant");
						}
					}

					// Extract predicate
					expr_set lhs_predicate_expressions;
					decl_set chc_declarations;
					extract_relations(expr, chc_declarations, lhs_predicate_expressions);
					assert (lhs_predicate_expressions.size() == 1);

					// Store as Horn constraint
					chcs.push_back(constrainted_horn_clause(!expr, chc_declarations, lhs_predicate_expressions, expr_set()));
					
				}
				
				//
				// Unsupported
				//
				else
				{
					throw std::runtime_error("Don't know what to do with query");
				}
				
			}

			
			//
			// Process rules
			//
			for (unsigned i = 0; i < rules.size(); ++i)
			{
				
				const auto & expr = rules[i];
				
				// std::cout << "\n---------- Rule " << i << " ----------" << std::endl;
				// std::cout << expr << std::endl;
				
				
				//
				// Rule is a simple fact involving a predicate to synthesize
				//
				if (expr.is_app() && expr.decl().decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED && expr.decl().arity() > 0)
				{
					
					// Skip verifier.error
					if(expr.decl().name().str() != "verifier.error")
					{

						// Check that arguments are constants
						for (unsigned j = 0; j < expr.num_args(); ++j)
						{
							if (!(expr.arg(j).is_const() && expr.arg(j).decl().decl_kind() != Z3_decl_kind::Z3_OP_UNINTERPRETED))
							{
								throw std::runtime_error("Argument of fact is not a constant");
							}
						}

						// Extract predicate
						expr_set rhs_predicate_expressions;
						decl_set chc_declarations;
						extract_relations(expr, chc_declarations, rhs_predicate_expressions);
						assert (rhs_predicate_expressions.size() == 1);

						// Store as Horn constraint
						chcs.push_back(constrainted_horn_clause(expr, chc_declarations, expr_set(), rhs_predicate_expressions));
						
					}
					
				}
				
				//
				// Rule is a forall expression
				//
				else if(expr.is_quantifier() && Z3_is_quantifier_forall(ctx, expr))
				{

					//
					// Prepare some variables
					//
					// Total number of quantifier variables
					unsigned num_vars = Z3_get_quantifier_num_bound(ctx, expr);
					// Set of uninterpreted predicates occurring in the CHC
					decl_set chc_declarations;
			
			
					//
					// Substitute quantified variables by new constants
					//
					z3::expr_vector new_constants(ctx);
					for (unsigned i = 0; i < num_vars; ++i)
					{
						new_constants.push_back(ctx.constant(ctx.int_symbol(constant_id++), z3::sort(ctx, Z3_get_quantifier_bound_sort(ctx, expr, num_vars - i - 1))));
					}
					auto body = expr.body().substitute(new_constants);
			
			
					//
					// Forall fact
					//
					if (body.is_app() && body.decl().decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED && body.decl().arity() > 0)
					{
					
						// Skip verifier.error
						if(body.decl().name().str() != "verifier.error")
						{
					
							// Check that arguments are constants
							for (unsigned j = 0; j < body.num_args(); ++j)
							{
								if (!body.arg(j).is_const())
								{
									throw std::runtime_error("Argument of forall fact is not a constant");
								}
							}

							// Extract predicate
							expr_set rhs_predicate_expressions;
							decl_set chc_declarations;
							extract_relations(body, chc_declarations, rhs_predicate_expressions);
							assert (rhs_predicate_expressions.size() == 1);

							// Store as Horn constraint
							chcs.push_back(constrainted_horn_clause(body, chc_declarations, expr_set(), rhs_predicate_expressions));					
							
						}
							
					}
			
					//
					// Horn constraint
					//
					else if (body.is_app() && body.decl().decl_kind() == Z3_decl_kind::Z3_OP_IMPLIES)
					{

						// Check that left-hand-side is a conjunction or a single application of an uninterpreted predicate
						if (!((body.arg(0).is_app() && body.arg(0).decl().decl_kind() == Z3_decl_kind::Z3_OP_AND) || (body.arg(0).is_app() && body.arg(0).decl().decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED)))
						{
							throw std::runtime_error("Left-hand-side of Horn clause has to be a conjunction");
						}
						// Check that right-hand-side is an application of an uninterpreted predicate
						if (!(body.arg(1).is_app() && body.arg(1).decl().decl_kind() == Z3_decl_kind::Z3_OP_UNINTERPRETED))
						{
							throw std::runtime_error("Right-hand-side of Horn clause has to be a predicate");
						}


						//
						// Process left-hand-side
						//
						expr_set lhs_predicate_expressions;
						extract_relations(body.arg(0), chc_declarations, lhs_predicate_expressions);
						
						//
						// Process right-hand-side
						//
						expr_set rhs_predicate_expressions;
						extract_relations(body.arg(1), chc_declarations, rhs_predicate_expressions);
						
						
						//
						// Store CHC
						//
						chcs.push_back(constrainted_horn_clause(body, chc_declarations, lhs_predicate_expressions, rhs_predicate_expressions));
						
					}

					//
					// Unknown Rule
					//
					else
					{
						throw std::runtime_error("Cannot parse rule");
					}

					
				}

				//
				// Unsupported
				//
				else
				{
					throw std::runtime_error("Don't know what to do with rule");
				}
				
			}
			
			
			return problem(std::move(relations), std::move(chcs));
			
		}

	};

}; // End namespace chc_teacher

#endif
