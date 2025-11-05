/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __PRETTY_PRINT_VISITOR_H__
#define __PRETTY_PRINT_VISITOR_H__

// C++ includes
#include <iostream>

// Project includes
#include "visitor.h"


namespace horn_verification
{

	/**
	 * Visitor that writes a textual representation of a decision tree to an
	 * output stream.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class pretty_print_visitor : public base_visitor 
	{
	
		/// The output stream to write to
		std::ostream & _out;
		
		/// The number of spaces to indent subtrees
		unsigned int _indent;
	
	
	public:
	
		/**
		 * Creates a \ref pretty_print_visitor.
		 *
		 * @param out The output stream to write to
		 * @param indent The initial number of spaces to indent all text
		 */
		pretty_print_visitor(std::ostream & out = std::cout, unsigned int indent = 0)
			: _out(out), _indent(indent)
		{
			// Nothing
		}
	
		void visit(categorical_node & node) override;
	
		void visit(int_node & node) override;
	
		void visit(leaf_node & node) override;
	
	};	
	
}; // End namespace horn_verification

#endif