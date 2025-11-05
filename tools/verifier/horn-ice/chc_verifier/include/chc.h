/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_CONSTRAINED_HORN_CLAUSE_H__
#define __CHCTEACHER_CONSTRAINED_HORN_CLAUSE_H__

// C++ includes
#include <ostream>

// Z3 includes
#include "z3++.h"

// Project includes
#include "z3_helper.h"


namespace chc_teacher
{

	class constrainted_horn_clause
	{
		
	public:


		/// The expression constituting the CHC (this is what is sent to Z3)
		z3::expr expr;

		/// Set of all uninterpreted predicates occurring on left- and right-hand-side
		decl_set uninterpreted_predicates;
		
		/// All predicates occurring in the left-hand-side of the Horn clause
		expr_set predicates_in_lhs;
		
		/// All predicates occurring in the right-hand-side of the Horn clause
		expr_set predicates_in_rhs;
		
		
		constrainted_horn_clause(const z3::expr & expr, const decl_set & uninterpreted_predicates, const expr_set & predicates_in_lhs, const expr_set & predicates_in_rhs)
			: expr(expr), uninterpreted_predicates(uninterpreted_predicates), predicates_in_lhs(predicates_in_lhs), predicates_in_rhs(predicates_in_rhs)
		{
			// Nothing
		}
		
		
		friend std::ostream & operator<<(std::ostream & out, const constrainted_horn_clause & chc)
		{
			
			out << " -- CHC" << std::endl;
			out << chc.expr << std::endl;
			
			out << " -- Uninterpreted predicates" << std::endl;
			for (const auto & decl : chc.uninterpreted_predicates)
			{
				out << decl << std::endl;
			}
			
			out << " -- LHS" << std::endl;
			for (const auto & expr : chc.predicates_in_lhs)
			{
				out << expr << std::endl;
			}
			
			out << " -- RHS" << std::endl;
			for (const auto & expr : chc.predicates_in_rhs)
			{
				out << expr << std::endl;
			}
			
			return out;
			
		}
		
	};

}; // End namespace chc_teacher
	
#endif