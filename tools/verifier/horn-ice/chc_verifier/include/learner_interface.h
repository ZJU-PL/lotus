/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_LEARNER_INTERFACE_H__
#define __CHCTEACHER_LEARNER_INTERFACE_H__

// C++ includes
#include <list>
#include <stdexcept>
#include <vector>
#include <iostream>

// C includes
#include <cassert>

// Z3 includes
#include "z3++.h"

// Project includes
#include "conjecture.h"
#include "decision_tree.h"
#include "horn_constraint.h"
#include "horn_counterexample.h"
#include "z3_helper.h"
#include "../../hice-dt/include/datapoint.h"
#include "datapoint.h"
#include "api.h"
#include "dt_to_z3_exp.h"
#include "pretty_print_visitor.h" // DEBUG


namespace chc_teacher
{
	
	/**
	 * This class implements an interface to the lerning algorithm.
	 */

	class learner_interface
	{

		struct datapoint_Hasher
		{

			unsigned operator() (const chc_teacher::datapoint & dp) const
			{
				return dp.hash();
			}

		};

		struct datapoint_Comparer
		{

			bool operator() (const chc_teacher::datapoint & dp1, const chc_teacher::datapoint & dp2) const
			{
				return dp1.equal(dp2);
			}

		};

		/// ID of relations
		std::unordered_map<z3::func_decl, unsigned, ASTHasher, ASTComparer> relation2ID;

		std::unordered_map<unsigned, z3::func_decl> categorical_identifier_to_relation;

		/// ID of attributes
		std::unordered_map<z3::func_decl, unsigned, ASTHasher, ASTComparer> relation_to_base_value;

		std::unordered_map<unsigned, z3::expr> integer_identifier_to_attribute;

		mutable std::unordered_map<chc_teacher::datapoint, horn_verification::datapoint<bool>, datapoint_Hasher, datapoint_Comparer> teacher_datapoint_to_learner_datapoint;

		/// Variables used to construct conjecture expressions
		std::vector<std::vector<z3::expr>> variables;

		horn_verification::api api_object;

		unsigned categorical_identifier;

		unsigned integer_identifier;

	public:

		/**
		 * Creates a new learner interface (and potentially the learner within in).
		 *
		 * @param relations The uninterpreted predicates that need to be learned
		 */
		learner_interface(const decl_set & relations, bool do_horndini_prephase, bool use_bounds) {

			categorical_identifier = 0;

			integer_identifier = 0;
			
			// For Horndini Prephase

			api_object.reserve_datapoint_ptrs(2000);

			api_object.configure_learner(do_horndini_prephase, use_bounds);

			unsigned left;

			//
			// Check number of argument of predicates
			//
			for (const auto & decl : relations)
			{
				if (decl.arity() == 0)
				{
					throw std::runtime_error("Uninterpreted predicates with no arguments are not allowed");
				}
			}
			
			//
			// Do seomthing with the uninterpreted predicated here (i.e., set of an order and store the number of arguments)
			//
			
			for (const auto & decl : relations)
			{
				//
				// Assign unique ID to relation
				//
				relation2ID.emplace(decl, categorical_identifier);

				categorical_identifier_to_relation.emplace(categorical_identifier, decl);
				
				// TODO: Perhaps we also want to store the arity of each relation so as to easily compute the position of data in a DT data point object
			
				relation_to_base_value.emplace(decl, integer_identifier);

				//
				// Create variables / expression for each argument of the relation
				//
				std::vector<z3::expr> attributes;
				attributes.reserve(decl.arity());

				left = integer_identifier;

				//
				// Adding primitive attributes
				//
				for (unsigned i = 0; i < decl.arity(); ++i) {

					auto attributeSort = decl.domain(i);
					auto attributeName = decl.name().str() + "#" + std::to_string(i);

					attributes.push_back(decl.ctx().constant(attributeName.c_str(), attributeSort));
					integer_identifier_to_attribute.emplace(integer_identifier++, decl.ctx().constant(attributeName.c_str(), attributeSort));

					api_object.add_integer_attribute(attributeName);

				}

				//
				// Adding derived attributes
				//

				for (unsigned first_index = 0; first_index < attributes.size(); first_index++) {

					for (unsigned second_index = first_index + 1; second_index < attributes.size(); second_index++) {

						if (attributes.at(first_index).get_sort().is_int() && attributes.at(second_index).get_sort().is_int()) {

							std::stringstream firstAttributeStream, secondAttributeStream;

							firstAttributeStream << attributes.at(first_index);
							secondAttributeStream << attributes.at(second_index);

							integer_identifier_to_attribute.emplace(integer_identifier++, attributes.at(first_index) + attributes.at(second_index));

							api_object.add_integer_attribute(firstAttributeStream.str() + "+" + secondAttributeStream.str());

							integer_identifier_to_attribute.emplace(integer_identifier++, attributes.at(first_index) - attributes.at(second_index));

							api_object.add_integer_attribute(firstAttributeStream.str() + "-" + secondAttributeStream.str());

						}
					}
				}

				variables.push_back(std::move(attributes));
				categorical_identifier++;
				api_object.add_intervals(left, (integer_identifier - 1));
			}

			api_object.add_categorical_attribute("$func", categorical_identifier);
		}

	horn_verification::datapoint<bool>* get_unique_learner_datapoint(const chc_teacher::datapoint &teacher_datapoint) const {

		if (teacher_datapoint_to_learner_datapoint.find(teacher_datapoint) == teacher_datapoint_to_learner_datapoint.end()) { 

			horn_verification::datapoint<bool> current_learner_datapoint(api_object.index_of_datapoint_ptrs());
			
			current_learner_datapoint._int_data = teacher_datapoint.get_int_data(relation_to_base_value, integer_identifier);
				
			current_learner_datapoint._categorical_data = teacher_datapoint.get_categorical_data(relation2ID);
				
			teacher_datapoint_to_learner_datapoint.emplace(teacher_datapoint, current_learner_datapoint);

			//assert (teacher_datapoint_to_learner_datapoint.find(teacher_datapoint)->second._categorical_data.size() == 1);

			if (teacher_datapoint_to_learner_datapoint.find(teacher_datapoint)->second._identifier == api_object.index_of_datapoint_ptrs()) {

			// This portion to re investigate why the teacher_datapoint is mapping to an existing learner_datapoint
			// Hopefully there will be an unused datapoint

			// Some one can check whether teacher_datapoint_to_learner_datapoint can be a pair or vectors rather than a Map

				api_object.add_datapoints(teacher_datapoint_to_learner_datapoint.find(teacher_datapoint)->second);
			}
		}

		return &teacher_datapoint_to_learner_datapoint.find(teacher_datapoint)->second;
	}


		/**
		 * Adds a new counterexample  to the sample.
		 *
		 * @param counterexample A pair of lists of data points representing a
		 *                       Horn counterexample. Positive and negative counterexamples
		 *                       are also given in Horn form with empty left-hand-side and
		 *                       empty right-hand-side, respectively.
		 *
		 * @returns Returns whether the current counterexample already exists in the sample.
		 */
		bool add_counterexample(const horn_counterexample & counterexample)
		{
			//
			// This function should distinguish between real Horn counterexamples or positive
			// counterexamples (left-hand-side is empty) or negative counterexample
			// (right-hand-side is empty).
			//
			
			//
			// Example to distinguish types of counterexamples
			//
			
			//	Positive counterexample
			if (counterexample.lhs.size() == 0 && counterexample.rhs.size() == 1) {

				horn_verification::datapoint<bool> *current_learner_datapoint = get_unique_learner_datapoint(*(counterexample.rhs.begin()));

				if (current_learner_datapoint->_is_classified == true) {

					if (current_learner_datapoint->_classification == false) {

						throw std::runtime_error("Contradicting counterexample");
					}
				} else {

					current_learner_datapoint->set_classification(true);
				}
			}

			//	Negative counterexample
			else if (counterexample.lhs.size() == 1 && counterexample.rhs.size() == 0) {

				horn_verification::datapoint<bool> *current_learner_datapoint = get_unique_learner_datapoint(*(counterexample.lhs.begin()));

				if (current_learner_datapoint->_is_classified == true) {

					if (current_learner_datapoint->_classification == true) {

						throw std::runtime_error("Contradicting counterexample");
					}
				} else {

					current_learner_datapoint->set_classification(false);
				}
			}

			// Horn counterexample
			else if (counterexample.lhs.size() > 0 && counterexample.rhs.size() <= 1) {

				std::vector<horn_verification::datapoint<bool> *> lhs_of_horn;

				horn_verification::datapoint<bool> *rhs_of_horn = NULL;

				/* Unsigned datapoints*/

				if (counterexample.rhs.size() == 1) {

					rhs_of_horn = get_unique_learner_datapoint(*(counterexample.rhs.begin()));

					/* return intervals */
				}

				for (auto const& current_datapoint : counterexample.lhs) {

					lhs_of_horn.push_back(get_unique_learner_datapoint(current_datapoint));
				}

				horn_verification::horn_constraint<bool> horn_constraint(lhs_of_horn, rhs_of_horn, false);

				api_object.add_horn_constraints(horn_constraint);
			}

			// Malformed counterexample
			else {

				throw std::runtime_error("Malformed counterexample");
			}
			
			return true;
		}
		
		/**
		 * Generates (i.e., learns) a new set of conjectures that is consistent with
		 * the current sample.
		 *
		 * @return a map that maps declarations of the uninterpreted predicates to their
		 *         implementation in form of a conjecture object.
		 */
		std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> get_conjectures()
		{
			auto decision_tree = api_object.learn_decision_tree();
			
			horn_verification::pretty_print_visitor printer;

			//decision_tree.accept(printer);

			//std::cout << "Hi" << std::endl;

			dt_to_z3_exp map_dt_z3(variables, categorical_identifier_to_relation, integer_identifier_to_attribute);

			return map_dt_z3.get_unordered_map(decision_tree.root());
		}

	};

}; // End namespace chc_teacher

#endif
