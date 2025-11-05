/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __VISITOR_H__
#define __VISITOR_H__

namespace horn_verification
{

	// Forward declarations
	class categorical_node;
	class int_node;
	class leaf_node;

	
	/**
	 * This class defines a visitor interface for decision trees. Specific
	 * visitors must derive from this class and implement the interface defined
	 * by this class.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class base_visitor
	{
	
	public:
	
		/**
		 * Destructor.
		 */
		virtual ~base_visitor()
		{
			// Nothing
		}
	
	
		/**
		 * Visits an \ref categorical_node.
		 *
		 * @param node The node to visit
		 */
		virtual void visit(categorical_node & node) = 0;
	
	
		/**
		 * Visits an \ref int_node.
		 *
		 * @param node The node to visit
		 */
		virtual void visit(int_node & node) = 0;
	
	
		/**
		 * Visits an \ref leaf_node.
		 *
		 * @param node The node to visit
		 */
		virtual void visit(leaf_node & node) = 0;
	
	};
	
}; // End namespace horn_verification

#endif