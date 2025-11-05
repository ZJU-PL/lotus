/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __DECISION_TREE_H__
#define __DECISION_TREE_H__

// C++ includes
#include <algorithm>
#include <vector>


namespace horn_verification
{

	// Forward declarations
	class base_visitor;

	
	/**
	 * This class represents the base class for a node in a decision tree and defines the interface
	 * that specific types of nodes have to implement.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class base_node
	{

	public:

		/**
		 * Destructor.
		 */
		virtual ~base_node()
		{
			// Nothing
		}


		/**
		 * Applies a visitor (i.e., implements the visitor pattern).
		 *
		 * @param visitor The visitor to apply
		 */
		virtual void accept(base_visitor & visitor) = 0;


		/**
		 * Clones (i.e., copies) the tree that tis rooted at this node.
		 *
		 * @return a pointer to the root of the copied tree
		 */
		virtual base_node * clone() const = 0;
	
	};

	
	/**
	 * This class represents a categorical decision node, representing a decision
	 * of the form <code> switch(x.attribute)</code>. The number of children of
	 * a categorical decision node depends on the number of categories of the 
	 * considered attribute.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class categorical_node : public base_node
	{
	
		/// The decision attribute
		std::size_t _attribute;

		/// The children of this node
		std::vector<base_node *> _children;
	
	
	public:
	
		/**
		 * Creates a categorical decision node.
		 *
		 * @param attribute The decision attribute
		 * @param number_of_categories The number of categories
		 */
		categorical_node(size_t attribute, size_t number_of_categories)
			: _attribute(attribute), _children(number_of_categories)
		{
			// Nothing
		}
	
		/**
		 * Copy constructor.
		 *
		 * @param other The object to copy
		 */
		categorical_node(const categorical_node & other)
			: _attribute(other._attribute), _children(other._children.size())
		{
			
			// Clone children
			for (std::size_t i = 0; i < other._children.size(); ++i)
			{
				if (other._children[i])
				{
					_children[i] = other._children[i]->clone();
				}
			}
			
		}
		
	
		/**
		 * Move constructor.
		 *
		 * @param other The object to move
		 */
		categorical_node(categorical_node && other)
			: _attribute(other._attribute), _children(std::move(other._children))
		{
			// Nothing
		}
		
	
		/**
		 * Destructor.
		 */
		~categorical_node()
		{
			for_each (_children.begin(), _children.end(), [](base_node * child){ if (child) delete child; });
		}
	
	
		/**
		 * Assignment operator.
		 *
		 * @param rhs The object to assign
		 *
		 * @return the reference to the newly assigned object
		 */
		categorical_node & operator=(const categorical_node & rhs);
		
		
		/**
		 * Move assignment operator.
		 *
		 * @param rhs The object to move
		 *
		 * @return the reference to the newly assigned object
		 */
		categorical_node & operator=(categorical_node && rhs);


		/*
		 * Overrides base class method.
		 */
		void accept(base_visitor & visitor) override;


		/*
		 * Overrides base class method.
		 */
		categorical_node * clone() const override;
	
	
		/**
		 * Return the decision attribute of this node.
		 *
		 * @return the decision attribute of this node
		 */
		inline std::size_t attribute() const
		{
			return _attribute;
		}
		
		
		/**
		 * Returns the children of this node.
		 *
		 * @return the children of this node
		 */
		inline std::vector<base_node *> & children()
		{
			return _children;
		}
	
	};
	

	/**
	 * This class represents an integer decision node, representing a decision of
	 * the form <code>x.attribute <= threshold </code>. Consequently, an
	 * \ref int_node has exactly two children.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class int_node : public base_node
	{
	
		/// The decision attribute
		std::size_t _attribute;
		
		/// The decision threshold
		int _threshold;

		/// The children of this node
		std::vector<base_node *> _children;
	
	
	public:
	
		/**
		 * Creates a new integer decision node.
		 *
		 * @param attribute The decision attribute
		 * @param threshold The decision threshold
		 */
		int_node(std::size_t attribute, int threshold)
			: _attribute(attribute), _threshold(threshold), _children(2)
		{
			// Nothing
		}
	
		
		/**
		 * Copy constructor.
		 *
		 * @param other The object to copy
		 */
		int_node(const int_node & other)
			: _attribute(other._attribute), _threshold(other._threshold), _children(2)
		{
			
			// Clone children
			if (other._children[0])
			{
				_children[0] = other._children[0]->clone();
			}
			if (other._children[1])
			{
				_children[1] = other._children[1]->clone();
			}
			
		}
	
	
		/**
		 * Move constructor.
		 *
		 * @param other The object to move
		 */
		int_node(int_node && other)
			: _attribute(other._attribute), _threshold(other._threshold), _children(std::move(other._children))
		{
			// Nothing
		}
		
	
		/**
		 * Destructor.
		 */
		~int_node()
		{
			for_each (_children.begin(), _children.end(), [](base_node * child){ if (child) delete child; });
		}
	
	
		/**
		 * Assignment operator.
		 *
		 * @param rhs The object to assign
		 *
		 * @return the reference to the newly assigned object
		 */
		int_node & operator=(const int_node & rhs);
	
	
		/**
		 * Move assignment operator.
		 *
		 * @param rhs The object to move
		 *
		 * @return the reference to the newly assigned object
		 */
		int_node & operator=(int_node && rhs);

		
		/*
		 * Overrides base class method.
		 */
		void accept(base_visitor & visitor) override;
	
	
		/*
		 * Overrides base class method.
		 */
		int_node * clone() const override;
		
	
		/**
		 * Returns the decision attribute of this node.
		 *
		 * @return the decision attribute of this node
		 */
		inline std::size_t attribute() const
		{
			return _attribute;
		}
		
		
		/**
		 * Returns the decision threshold of this node.
		 *
		 * @return the decision threshold of this node
		 */
		inline int threshold() const
		{
			return _threshold;
		}

		
		/**
		 * Returns the children of this node.
		 *
		 * @return the children of this node
		 */
		inline std::vector<base_node *> & children()
		{
			return _children;
		}
	
	
	};
	
	
	/**
	 * This class represents a leaf node.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class leaf_node : public base_node
	{
	
		/// The output of this node
		bool _output;
	
	
	public:
	
		/**
		 * Creates a new leaf node.
		 *
		 * @param output The output of this node
		 */
		leaf_node(bool output)
			: _output(output)
		{
			// Nothing	
		}
	
	
		/**
		 * Copy constructor.
		 *
		 * @param other The object to copy
		 */
		leaf_node(const leaf_node & other)
			: _output(other._output)
		{
			// Nothing
		}
		
	
		/**
		 * Move constructor.
		 *
		 * @param other The object to move
		 */
		leaf_node(leaf_node && other)
			: _output(other._output)
		{
			// Nothing
		}
		
	
		/**
		 * Assignment operator.
		 *
		 * @param rhs The object to assign
		 *
		 * @return the reference to the newly assigned object
		 */
		leaf_node & operator=(const leaf_node & rhs);


		/**
		 * Move assignment operator.
		 *
		 * @param rhs The object to move
		 *
		 * @return the reference to the newly assigned object
		 */
		leaf_node & operator=(leaf_node && rhs);


		/*
		 * Overrides base class method.
		 */		
		void accept(base_visitor & visitor) override;


		/*
		 * Overrides base class method.
		 */
		leaf_node * clone() const override;
	
	
		/**
		 * Returns the output of this node.
		 *
		 * @return the output of this node
		 */
		inline bool output() const
		{
			return _output;
		}
	
	};
	
	
	/**
	 * This class represents a decision tree.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class decision_tree
	{

		/// Gives the learner access to the root
		template <class JobManager> friend class learner;

	
		/// Root node of the decision tree
		base_node * _root;
		
		
	public:
	
		/**
		 * Creates an empty decision tree.
		 */
		decision_tree()
			: _root(nullptr) 
		{
			// Nothing
		}
		
		
		/**
		 * Creates a decision tree with ethe given root.
		 */
		decision_tree(base_node * root)
			: _root(root) 
		{
			// Nothing
		}
		
		
		/**
		 * Copy constructor.
		 *
		 * @param other The decision tree to copy
		 */
		decision_tree(const decision_tree & other)
			: _root(other._root->clone())
		{
			// Nothing
		}
		
		
		/**
		 * Move constructor.
		 *
		 * @param other The decision tree to be movedata
		 */
		decision_tree(decision_tree && other)
			: _root(other._root)
		{
			other._root = nullptr;
		}
		
		
		/**
		 * Destructor.
		 */
		~decision_tree()
		{
			if(_root)
			{
				delete _root;
			}
		}
		
		
		/**
		 * Assignment operator.
		 *
		 * @param rhs The object to assign
		 *
		 * @return the reference to the newly assigned object
		 */
		decision_tree & operator=(const decision_tree & rhs)
		{
			
			decision_tree tmp(rhs);
			
			std::swap(_root, tmp._root);
			
			return *this;
			
		}
		
		
		/**
		 * Move assignment operator.
		 *
		 * @param rhs The object to move
		 *
		 * @return the reference to the newly assigned object
		 */
		decision_tree & operator=(decision_tree && rhs)
		{
			
			if (this != &rhs)
			{
				
				if (_root)
				{
					delete _root;
				}
				
				_root = rhs._root;
				rhs._root = nullptr;
				
			}
			
			return *this;
			
		}
		
		
		/**
		 * Applies a visitor to the root of the tree if the tree is non-empty.
		 *
		 * @param visitor The visitor to apply
		 */
		void accept(base_visitor & visitor)
		{
			if(_root)
			{
				_root->accept(visitor);
			}
		}
		
		
		/**
		 * Returns a pointer to the root node.
		 *
		 * @returns a pointer to the root node
		 */
		base_node * root()
		{
			return _root;
		}
		
	};

}; // End namespace horn_verification

#endif