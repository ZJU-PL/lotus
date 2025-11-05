/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_DT_TO_Z3_EXP_H__
#define __CHCTEACHER_DT_TO_Z3_EXP_H__

// C++ includes
#include <list>
#include <unordered_map>
#include <unordered_set>

// C includes
#include <cassert>

// Z3 include
#include "z3++.h"

// Project includes
#include "decision_tree.h"


namespace chc_teacher
{

	class dt_to_z3_exp
	{
		/// Variables used to construct conjecture expressions
		std::vector<std::vector<z3::expr>> _set_of_variables;

		std::unordered_map<unsigned, z3::func_decl> _ID2relation;

		std::unordered_map<unsigned, z3::expr> _integer_identifier_to_attribute;

		z3::context & _ctx;

		public:


			dt_to_z3_exp(const std::vector<std::vector<z3::expr>> & variables, std::unordered_map<unsigned, z3::func_decl> & ID2relation, std::unordered_map<unsigned, z3::expr> & integer_identifier_to_attribute)
				: _set_of_variables(variables), _ID2relation(ID2relation), _integer_identifier_to_attribute(integer_identifier_to_attribute), _ctx((_ID2relation.begin())->second.ctx())
			{
				//Nothing to do
			}

			std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> get_unordered_map(horn_verification::base_node *node) const {
				
				std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> map_z3_expr;

				horn_verification::categorical_node* categorical_child = dynamic_cast<horn_verification::categorical_node*> (node);

				horn_verification::leaf_node* leaf_child = NULL;

				if (categorical_child != NULL) {

					get_z3_exp_categorical(categorical_child, map_z3_expr);

				} else {

					leaf_child = dynamic_cast<horn_verification::leaf_node*> (node);
				}

				for (auto categorical_identifier : _ID2relation) {

					if (map_z3_expr.find(categorical_identifier.second) == map_z3_expr.end()) {

						z3::func_decl relation_signature = categorical_identifier.second;

						z3::expr_vector  variables(_ctx);

						for (auto var : _set_of_variables.at(categorical_identifier.first)) {

							variables.push_back(var);
						}

						bool random_boolean_value;

						if (categorical_child == NULL && leaf_child != NULL) {

							random_boolean_value = leaf_child->output();

						} else if (rand()%2 == 0) {

							random_boolean_value = false;

						} else {

							random_boolean_value = true;
						}

						conjecture relation_conjecture(_ctx.bool_val(random_boolean_value), variables);

						map_z3_expr.emplace(relation_signature, relation_conjecture);
					}
				}

				return map_z3_expr;
			}


			void get_z3_exp_categorical(horn_verification::categorical_node *node, std::unordered_map<z3::func_decl, conjecture, ASTHasher, ASTComparer> & map_z3_expr) const {

				z3::expr expression = _ctx.bool_val(false);

				unsigned categorical_identifier = 0;

				for (auto child : node->children()) {

					horn_verification::int_node* int_child = dynamic_cast<horn_verification::int_node*> (child);

					horn_verification::leaf_node* leaf_child = dynamic_cast<horn_verification::leaf_node*> (child);

					if(leaf_child != NULL) {

						expression = get_z3_exp_leaf(leaf_child, _set_of_variables.at(categorical_identifier));

					} else if (int_child != NULL) {

						expression = get_z3_exp_int(int_child, _set_of_variables.at(categorical_identifier));

					}

					z3::func_decl relation_signature = (_ID2relation.find(categorical_identifier))->second;

					z3::expr_vector  variables(_ctx);

					for (auto var : _set_of_variables.at(categorical_identifier)) {

						variables.push_back(var);
					}

					conjecture relation_conjecture(expression, variables);

					map_z3_expr.emplace(relation_signature, relation_conjecture);

					//std::cout << std::endl  << "Relation: " << relation_signature << ", can be replace with expression:  " << expression << std::endl;

					categorical_identifier++;
				}
			}


			z3::expr get_z3_exp_int(horn_verification::int_node *node, std::vector<z3::expr> _variables) const {

				auto children = node->children();

				auto left_child = children[0];

				auto right_child = children[1];

				horn_verification::int_node* left_int_child = dynamic_cast<horn_verification::int_node*> (left_child);

				horn_verification::leaf_node* left_leaf_child = dynamic_cast<horn_verification::leaf_node*> (left_child);

				horn_verification::int_node* right_int_child = dynamic_cast<horn_verification::int_node*> (right_child);

				horn_verification::leaf_node* right_leaf_child = dynamic_cast<horn_verification::leaf_node*> (right_child);

				z3::expr left_expression(_ctx), right_expression(_ctx);

				bool no_left_child = false;

				bool no_right_child = false;

				if (left_int_child != NULL) {

					if ((_integer_identifier_to_attribute.find(node->attribute())->second).is_bool()) {

						bool boolean_value;

						if (node->threshold() == 1) {

							boolean_value = true;

						} else {

							boolean_value = false;
						}

						left_expression = get_z3_exp_int(left_int_child, _variables) && (_integer_identifier_to_attribute.find(node->attribute())->second && boolean_value);

					} else {

						left_expression = get_z3_exp_int(left_int_child, _variables) && (_integer_identifier_to_attribute.find(node->attribute())->second <= node->threshold());
					}

				} else if (left_leaf_child != NULL) {

					if (left_leaf_child->output() == true) {

						if ((_integer_identifier_to_attribute.find(node->attribute())->second).is_bool()) {

							bool boolean_value;

							if (node->threshold() == 1) {

								boolean_value = true;

							} else {

								boolean_value = false;
							}

							left_expression = _integer_identifier_to_attribute.find(node->attribute())->second && boolean_value;

						} else {

							left_expression = _integer_identifier_to_attribute.find(node->attribute())->second <= node->threshold();
						}

					} else {

						no_left_child = true;
					}
				}

				if (right_int_child != NULL) {

					if ((_integer_identifier_to_attribute.find(node->attribute())->second).is_bool()) {

						bool boolean_value;

						if (node->threshold() == 1) {

							boolean_value = false;

						} else {

							boolean_value = true;
						}

						right_expression = get_z3_exp_int(right_int_child, _variables) && (_integer_identifier_to_attribute.find(node->attribute())->second && boolean_value);

					} else {

						right_expression = get_z3_exp_int(right_int_child, _variables) && (_integer_identifier_to_attribute.find(node->attribute())->second > node->threshold());

					}

				} else if (right_leaf_child != NULL) {

					if (right_leaf_child->output() == true) {

						if ((_integer_identifier_to_attribute.find(node->attribute())->second).is_bool()) {

							bool boolean_value;

							if (node->threshold() == 1) {

								boolean_value = false;

							} else {

								boolean_value = true;
							}

							right_expression = _integer_identifier_to_attribute.find(node->attribute())->second && boolean_value;

						} else {

							right_expression = _integer_identifier_to_attribute.find(node->attribute())->second > node->threshold();
						}

					} else {

						no_right_child = true;
					}
				}

				if (no_right_child == false && no_left_child == false) {

					return left_expression || right_expression;

				} else if (no_right_child == true && no_left_child == false) {

					return left_expression;

				} else if (no_left_child == true && no_right_child == false) {

					return right_expression;
				}

				return _ctx.bool_val(false);

			}

			z3::expr get_z3_exp_leaf(horn_verification::leaf_node *node, std::vector<z3::expr> _variables) const {

				return _ctx.bool_val(node->output());
			}
	};

}; // End namespace chc_teacher

#endif
