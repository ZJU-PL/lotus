/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __OUTPUT_VISITOR_H__
#define __OUTPUT_VISITOR_H__

// Project includes
#include "datapoint.h"
#include "visitor.h"

namespace horn_verification
{

	/**
	 * This class computes the output of a decision tree on a given data point.
	 * Use the method output(abstract_node & root, datapoint<bool> & datapoint)
	 * for this purpose.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class output_visitor : public base_visitor
	{
		
		/// The datapoint of which to compute the output
		datapoint<bool> * _datapoint;

		/// Stores the (last) output computed by the visitor
		bool _output;
		
	public:
	
		/**
		 * Creates an \ref output_visitor.
		 */
		output_visitor()
			: _datapoint(nullptr), _output(false)
		{
			// Nothing
		}		
	
		/**
		 * Computes the output of a decision tree, rooted at \p root, for a given
		 * data point.
		 *
		 * @param root The root node of the decision tree
		 * @param datapoint The data point of which to compute the output
		 *
		 * @return The output of the tree on the data point
		 */
		bool output(base_node & root, datapoint<bool> & datapoint);


		void visit(int_node & n) override;

		
		void visit(categorical_node & n) override;
		
		
		void visit(leaf_node & n) override;
		
	};
	
}; // End namespace horn_verification

#endif