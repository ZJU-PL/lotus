/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __JSON_VISITOR_H__
#define __JSON_VISITOR_H__

// C++ includes
#include <iostream>

// Project includes
#include "attributes_metadata.h"
#include "decision_tree.h"
#include "visitor.h"


namespace horn_verification
{

	/**
	 * Visitor that writes a JSON representation of a decision tree to an output stream.
	 *
	 * The JSON output has the following format. Each node of the tree is represented by four
	 * entries:
	 * <ol>
	 *   <li>\e attribute: a string with the name of the attribute to split on (defaults to the
	 *       empty string; is ignored if the node is a leaf).</li>
	 *   <li>\e cut: the value of threshold of an \ref int_node (defaults to 0, is ignored if the
	 *       node is not an \ref int_node).</li>
	 *   <li>\e classification: the label of a leaf, either \c true or \c false (delauts to \c true;
	 *       is ignored if the node is not a leaf).</li>
	 *   <li>\e children: an array of child nodes.
	 *   <ul>
	 *     <li>If the node is an \ref int_node, the left child corresponds to the
	 *         <code>true</code>-branch and the right child to the <code>false</code>-branch.</li>
	 *     <li>If the node is a \ref categorical_node, the i-th child corresponds to the i-th
	 *         category; a child might be \c null.</li>
	 *     <li>If the node is a \ref leaf_node, this field is set to \c null.</li>
	 *   </ul></li>
	 * </ol>
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class json_visitor : public base_visitor
	{
	
		/// The attribute's meta data
		const attributes_metadata & _metadata;

		/// The output stream to write to
		std::ostream & _out;
		
		
	public:
	
		/**
		 * Creates a \ref json_visitor.
		 *
		 * @param out The output stream to write to
		 * @param metadata The meta data of the attributes
		 */
		json_visitor(const attributes_metadata & metadata, std::ostream & out = std::cout)
			: _metadata(metadata), _out(out)
		{
			// Nothing
		}
	
		void visit(categorical_node & node) override;
	
		void visit(int_node & node) override;
	
		void visit(leaf_node & node) override;
	
	};	
	
}; // End namespace horn_verification

#endif