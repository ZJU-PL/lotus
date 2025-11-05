/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __HORNDINI_H__
#define __HORNDINI_H__

// C++ includes
#include <exception>
#include <iostream>
#include <list>
#include <numeric>
#include <stdexcept>
#include <vector>

// C includes
#include <cassert>

// Project includes
#include "attributes_metadata.h"
#include "datapoint.h"
#include "decision_tree.h"
#include "horn_constraint.h"


namespace horn_verification
{

	/**
	 * This class represents the situation that no conjunctive hypothesis exists.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class no_conjunction_exists_exception : public std::logic_error
	{

	public:
	
		/**
		 * Constructs a new no conjunction exists exception.
		 *
		 * @param what_arg The error message
		 */
		explicit no_conjunction_exists_exception(const std::string & what_arg)
			: logic_error(what_arg)
		{
			// Nothing
		}


		/**
		 * Constructs a new no conjunction exists exception.
		 *
		 * @param what_arg The error message
		 */
		explicit no_conjunction_exists_exception(const char * what_arg)
			: logic_error(what_arg)
		{
			// Nothing
		}
	
	};


	/**
	 * This class represents a derived predicate of the form x <= c, x >= c, and
	 * x = c, which are derived from an integer attribute.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class derived_predicate
	{
		
	public:

		/**
		 * Enum defining the type of a derived attribute.
		 */
		enum type
		{
			GE, LE, EQ
		};
	
		/// The index of the original predicate this derived predicate refers to
		std::size_t _index;
	
		/// The type of this derived attribute
		type _type;
		
		/// The threshold of this derived attribute
		int _threshold;
	
		
		/**
		 * Creates a new derived predicate.
		 *
		 * @param index The index this derived predicate refers to
		 * @param t The type of the derived predicate
		 * @param threshold The threshold used in the derived predicate
		 */
		derived_predicate(std::size_t index, type t, int threshold)
			: _index(index), _type(t), _threshold(threshold)
		{
			// Nothing
		}
		
		/**
		 * Writes a textual representation of this object to an output stream.
		 *
		 * @param out A reference to the output stream to write to
		 * @param pred The derived predicate
		 *
		 * @return the given reference to the output stream
		 */
		friend std::ostream & operator<<(std::ostream & out, const derived_predicate & pred)
		{
			
			out << "[index=" << pred._index << "; type=" << pred._type << "; threshold=" << pred._threshold << "]";
			
			return out;
			
		}
		
	};
	

	/**
	 * This class implements the Houdini algorithm for Horn clauses.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class horndini
	{
	
	public:
	
		/**
		 * Generates derived predicates from integer attributes and corresponding
		 * intervals. The predicates are of the form x <= threshold,
		 * x >= threshold, and x = threshold.
		 *
		 * @param intervals The intervals of positions in a data point
		 * @param threshold The value to use as threshold
		 *
		 * @return the derived predicates together with a corresponding vector of intervals for these predicates
		 */
		static std::pair<std::vector<derived_predicate>, std::vector<std::pair<unsigned, unsigned>>> generate_derived_predicates_and_intervals(const std::vector<std::pair<unsigned, unsigned>> & intervals, int threshold)
		{
			
			assert (threshold >= 0);
			
			//
			// Create derived Boolean predicates for each original int predicate
			//
			std::vector<derived_predicate> derived_predicates;
			std::vector<std::pair<unsigned, unsigned>> derived_intervals;
			unsigned left;
			unsigned right = 0;
			
			for (const auto & interval : intervals)
			{
				
				left = right;

				for (unsigned attribute = interval.first; attribute <= interval.second; ++attribute)
				{
				
					for (int t = -threshold; t <= threshold; ++t)
					{
						
						derived_predicates.push_back(derived_predicate(attribute, derived_predicate::type::GE, t));
						derived_predicates.push_back(derived_predicate(attribute, derived_predicate::type::LE, t));
						derived_predicates.push_back(derived_predicate(attribute, derived_predicate::type::EQ, t));

						right += 3;
						
					}
					
				}
				
				derived_intervals.push_back(std::make_pair(left, right - 1));
				
			}
			
			
			return std::make_pair(derived_predicates, derived_intervals);
			
		}
		

		/**
		 * Generates derived data point for derived attributes.
		 *
		 * @param datapoints The original data points
		 * @param derived_predicates The derived predicates
		 *
		 * @return derived data point for the given derived attributes.
		 */
		static std::vector<datapoint<bool>> generate_derived_datapoints(const std::vector<datapoint<bool>> & datapoints, const std::vector<derived_predicate> & derived_predicates)
		{
			
			std::vector<datapoint<bool>> derived_datapoints;
			derived_datapoints.reserve(datapoints.size());

			
			for (unsigned i = 0; i < datapoints.size(); ++i)
			{

				//
				// Generate boolean data from int data
				//
				std::vector<int> bool_data(derived_predicates.size());
	
				for (unsigned j = 0; j < derived_predicates.size(); ++j)
				{
					
					switch (derived_predicates[j]._type)
					{
						
						case derived_predicate::type::GE:
						{
							bool_data[j] = (datapoints[i]._int_data[derived_predicates[j]._index] >= derived_predicates[j]._threshold);
							break;
						}
						
						case derived_predicate::type::LE:
						{
							bool_data[j] = (datapoints[i]._int_data[derived_predicates[j]._index] <= derived_predicates[j]._threshold);
							break;
						}
						
						case derived_predicate::type::EQ:
						{
							bool_data[j] = (datapoints[i]._int_data[derived_predicates[j]._index] == derived_predicates[j]._threshold);
							break;
						}
						
					}
					
				}
	
				
				//
				// Add data point
				//
				datapoint<bool> derived_datapoint(datapoints[i]._classification, datapoints[i]._is_classified);
				derived_datapoint._categorical_data = datapoints[i]._categorical_data;
				derived_datapoint._int_data = std::move(bool_data);
				
				derived_datapoints.push_back(std::move(derived_datapoint));
		
			}
			
			return derived_datapoints;
		
		}
	
		
		/**
		 * Runs Houdini.
		 * An initial (vector of) set of predicates must be given. Houdini
		 * then removes predicates from this set.
		 *
		 * @param datapoints The data points to learn from
		 * @param horn_constraints The Horn constraints to learn from
		 * @param conjunctions Reference to an initial set of predicates. Houdini removes predicates from this set
		 */
		static void learn(const std::vector<datapoint<bool>> & datapoints, std::vector<horn_constraint<bool>> & horn_constraints, std::vector<std::list<unsigned>> & conjunctions)
		{
		
			
			//
			// Create list of positive data points to start with
			//
			std::list<const datapoint<bool> *> positive_ptrs;
			for (const auto & dp : datapoints)
			{
				if (dp._is_classified && dp._classification)
				{
					positive_ptrs.push_back(&dp);
				}
			}
		
		
			//
			// Run Horndini
			//
			do
			{
				
				//
				// Process positive datapoints
				//
				if (!positive_ptrs.empty())
				{
										
					// For each positive data point
					for (const auto & dp : positive_ptrs)
					{
					
						// Get function associated with current data point
						auto cur_function = dp->_categorical_data[0];
						assert (cur_function < conjunctions.size());
					
						// Consider all remaning predicates of the function associated with the current data point
						auto pred_it = conjunctions[cur_function].begin();
						while (pred_it !=  conjunctions[cur_function].end())
						{					
					
							// Knock off if entry is false
							if (!dp->_int_data[*pred_it])
							{
								pred_it =  conjunctions[cur_function].erase(pred_it);
							}
							// Skip if entry is true
							else
							{
								++pred_it;
							}
					
						}
					
					}
					
					positive_ptrs.clear();

				}
			
				//
				// Process Horn constraints
				//
				auto horn_it = horn_constraints.begin();
				while (horn_it != horn_constraints.end())
				{

					// Check whether data point of left-hand-side is satisfied
					auto lhs_it = horn_it->_premises.begin();
					while (lhs_it != horn_it->_premises.end())
					{
						
						auto cur_function = (*lhs_it)->_categorical_data[0];
						assert (cur_function < conjunctions.size());
						
						// Remove data point from lhs if it satisfies the conjunction
						if (satisfies(**lhs_it, conjunctions[cur_function]))
						{
							lhs_it = horn_it->_premises.erase(lhs_it);
						}
						else
						{
							++lhs_it;
						}
						
					}
			
					// If left-hand-side is empty, add right-hand-side to positive
					if (horn_it->_premises.empty())
					{
						
						if (horn_it->_conclusion == nullptr)
						{
							throw no_conjunction_exists_exception("No consistent conjunction exists");
						}
						else
						{
							positive_ptrs.push_back(horn_it->_conclusion);
							horn_it = horn_constraints.erase(horn_it);
						}
						
					}
					else
					{
						++ horn_it;
					}
					
				}

			} while(!positive_ptrs.empty());
		
		}


		/**
		 * Translates a set of conjunction of predicates into an equivalent decision tree.
		 *
		 * @param metadata The meta data (used for giving names to predicates)
		 * @param derived_predicates A list of derived predicates, one for each interval
		 * @param conjunctions A list of predicates (interpreted as conjunction), one for each interval
		 *
		 * @return a decision tree equivalent to the given conjunctions
		 */
		static decision_tree conjunctions2tree(const attributes_metadata & metadata, const std::vector<derived_predicate> & derived_predicates, const std::vector<std::list<unsigned>> & conjunctions)
		{

			categorical_node * root = new categorical_node(0, metadata.number_of_categories()[0]);
			
			for (unsigned i = 0; i < conjunctions.size(); ++i)
			{
				auto cur_it = conjunctions[i].cbegin();
				root->children()[i] = conjunction2tree(derived_predicates, cur_it, conjunctions[i].cend());
			}
			
			return decision_tree(root);
			
		}
	
	
		/**
		 * Checks whether a data point satisfies a conjunction.
		 *
		 * @param dp The data point
		 * @param conjunction The conjunction
		 *
		 * @return true if the data point satisfies the conjunction and false otherwise
		 */
		static bool satisfies(const datapoint<bool> & dp, const std::list<unsigned> & conjunction)
		{

			for (const auto & c : conjunction)
			{
				if (!dp._int_data[c])
				{
					return false;
				}
			}

			return true;

		}


		protected:
		
		/**
		 * Translates a set of conjunction of predicates into an equivalent decision tree.
		 *
		 * @param derived_predicates A list of derived predicates, one for each interval
		 * @param cur_it An iterator pointing to the current position in the conjunction
		 * @param end_it An iterator pointing the element after the end of the conjunction
		 *
		 * @return the root node of a decision tree equivalent to the given conjunctions so far
		 */
		static base_node * conjunction2tree(const std::vector<derived_predicate> & derived_predicates, std::list<unsigned>::const_iterator & cur_it, const std::list<unsigned>::const_iterator & end_it)
		{
			
			//
			// End of recursion
			//
			if (cur_it == end_it)
			{
				return new leaf_node(true);
			}
			
			
			switch (derived_predicates[*cur_it]._type)
			{
				
				case derived_predicate::type::GE:
				{
					
					int_node * ret = new int_node(derived_predicates[*cur_it]._index, derived_predicates[*cur_it]._threshold - 1);
					ret->children() = std::vector<base_node *>({new leaf_node(false), conjunction2tree(derived_predicates, ++cur_it, end_it)});
					return ret;
					
				}
				
				case derived_predicate::type::LE:
				{
					
					int_node * ret = new int_node(derived_predicates[*cur_it]._index, derived_predicates[*cur_it]._threshold);
					ret->children() = std::vector<base_node *>({conjunction2tree(derived_predicates, ++cur_it, end_it), new leaf_node(false)});
					return ret;

				}
				
				case derived_predicate::type::EQ:
				{
					
					int_node * ret1 = new int_node(derived_predicates[*cur_it]._index, derived_predicates[*cur_it]._threshold);
					int_node * ret2 = new int_node(derived_predicates[*cur_it]._index, derived_predicates[*cur_it]._threshold - 1);
					ret2->children() = std::vector<base_node *>({new leaf_node(false), conjunction2tree(derived_predicates, ++cur_it, end_it)});
					ret1->children() = std::vector<base_node *>({ret2, new leaf_node(false)});
					
					return ret1;
					
				}
				
			}

			return nullptr;
			
		}
		
	}; // End class Horndini

}; // End namespace horn_verification

#endif