/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __JOB_H__
#define __JOB_H__

// C++ includes
#include <unordered_set>
#include <vector>

// Project includes
#include "attributes_metadata.h"
#include "datapoint.h"
#include "slice.h"


namespace horn_verification
{

	/**
	 * This class represents an (abstract) job, which can either be a job to split a node or
	 * a job to turn a node into a leaf.
	 *
	 * @author Daniel Neider
	 * @version 1.0
	 */
	class abstract_job
	{

	protected:

		/// The slice of data points to be processed
		slice _slice;


		/**
		 * Creates a new job.
		 *
		 * @param sl The slice of this job
		 */
		abstract_job(const slice & sl)
			: _slice(sl)
		{
			// Nothing
		}
	
		/**
		 * Creates a new job.
		 *
		 * @param sl The slice of this job
		 */
		abstract_job(slice && sl)
			: _slice(std::move(sl))
		{
			// Nothing
		}
	
	
	public:
	
		/**
		 * Destructor.
		 */
		virtual ~abstract_job()
		{
			// Nothing
		}
	

		/**
		 * Performs the job on the given data points (and thier meta data). This method returns a list of
		 * new slices (e.g., in the case that a node was split).
		 *
		 * @param datapoint_ptrs The data points to run this job on
		 * @param metadata Meta data describing the data points
		 */
		virtual std::vector<slice> run(std::vector<datapoint<bool> *> & datapoint_ptrs, const attributes_metadata & metadata) = 0;
	
	};

	
	/**
	 * This class represents a split on a categorical attribute.
	 *
	 * @author Daniel Neider
	 * @version 1.0
	 */
	class categorical_split_job : public abstract_job
	{
		
		/// The attribute to split on
		std::size_t _attribute;

		
	public:

		/**
		 * Creates a new job for splitting on an categorical attribute.
		 *
		 * @param sl The slice of data points to be split
		 * @param attribute The categorical attribute to split on
		 */
		categorical_split_job(const slice & sl, std::size_t attribute)
			: abstract_job(sl), _attribute(attribute)
		{
			// Nothing
		}
	

		/*
		 * Implements super class method.
		 */
		std::vector<slice> run(std::vector<datapoint<bool> *> & datapoint_ptrs, const attributes_metadata & metadata) override;
		
	};
	
	
	/**
	 * This class represents a split on an integer attribute.
	 *
	 * @author Daniel Neider
	 * @version 1.0
	 */
	class int_split_job : public abstract_job
	{
		
		/// The attribute to split on
		std::size_t _attribute;

		/// The threshold to split on
		int _threshold;
	
	
	public:

		/**
		 * Creates a new job for splitting on an integer attribute.
		 *
		 * @param sl The slice of data points to be split
		 * @param attribute The categorical attribute to split on
		 * @param threshold The threshold to split on
		 */
		int_split_job(const slice & sl, std::size_t attribute, int threshold)
			: abstract_job(sl), _attribute(attribute), _threshold(threshold)
		{
			// Nothing
		}

		
		/*
		 * Implements super class method.
		 */	
		std::vector<slice> run(std::vector<datapoint<bool> *> & datapoint_ptrs, const attributes_metadata & metadata) override;
		
	};

	
	/**
	 * This class represents the creation of a leaf node.
	 *
	 * @author Daniel Neider
	 * @version 1.0
	 */
	class leaf_creation_job : public abstract_job
	{
		
		/// The label of the node to create
		bool _label;
		
		/// Data points to be turned positive
		std::unordered_set<datapoint<bool> * > _positive_ptrs;
		
		/// Data points to be turned negative
		std::unordered_set<datapoint<bool> * > _negative_ptrs;
		
	
	public:

		/**
		 * Creates a new job for creating a leaf node.
		 *
		 * @param sl The slice of data points to turn into a leaf node
		 * @param label The label of the leaf node
		 * @param positive_ptrs A set of pointers to data points that need to be labeled positively
		 * @param negative_ptrs A set of pointers to data points that need to be labeled negatively
		 */
		leaf_creation_job(const slice & sl, bool label, std::unordered_set<datapoint<bool> * > && positive_ptrs, std::unordered_set<datapoint<bool> * > && negative_ptrs)
			: abstract_job(sl), _label(label), _positive_ptrs(std::move(positive_ptrs)), _negative_ptrs(std::move(negative_ptrs))
		{
			// Nothing
		}


		/*
		 * Implements super class method.
		 */
		std::vector<slice> run(std::vector<datapoint<bool> *> & datapoint_ptrs, const attributes_metadata & metadata) override;
		
	};
	

}; // End namespace horn_verification

#endif