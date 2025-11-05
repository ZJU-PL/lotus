/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __SLICE_H__
#define __SLICE_H__

// C++ includes
#include <ostream>

// Project includes
#include "decision_tree.h"


namespace horn_verification
{

	/**
	 * A slice represents a contigouos set of data points, defined by 
	 * \p _left_index and a \p _right_index, which index into a vector of
	 * data points. The invariant <code>_left_index <= _right_index</code>
	 * should be maintained, but is not enforced by this class.
	 *
	 * In addition to the indexes, a slice also maintains a pointer to
	 * a memory address at which a node of the decision tree corresponding
	 * to the set of data points needs to be created.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	struct slice
	{
		
		/// The left bound of the range of data points
		std::size_t _left_index;
		
		/// The right bound of the range of data points
		std::size_t _right_index;
	
		/// The address at which to create a new node
		base_node ** _node_ptr;
		
		
		/**
		 * Creates a new slice.
		 *
		 * @param left_index The left bound of the range of data points
		 * @param right_index The right bound of the range of data points
		 * @param node_ptr The address at which to create a new node
		 */
		slice(std::size_t left_index, std::size_t right_index, base_node ** node_ptr)
			: _left_index(left_index), _right_index(right_index), _node_ptr(node_ptr)
		{
			// Nothing
		}
		
		
		/**
		 * Output operator that writes a textual representation to some output stream.
		 *
		 * @param out The output stream to write to
		 * @param sl The slice to output
		 *
		 * @return The output stream \p out
		 */
		friend std::ostream & operator<<(std::ostream & out, const slice & sl)
		{
			
			out << "[" << sl._left_index << " - " << sl._right_index << "; " << sl._node_ptr << "]";
			
			return out;
			
		}
		
	};
	
}; // End namespace horn_verification

#endif