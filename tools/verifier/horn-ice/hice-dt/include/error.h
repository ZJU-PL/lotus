/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __ERROR_H__
#define __ERROR_H__

// C++ includes
#include <stdexcept>
#include <string>


namespace horn_verification
{

	/**
	 * This class represents the error that a contiguous set of data points cannot be split.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class split_not_possible_error : public std::runtime_error
	{

	public:
	
		/**
		 * Constructs a new split not possible error.
		 *
		 * @param what_arg The error message
		 */
		explicit split_not_possible_error(const std::string & what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}


		/**
		 * Constructs a new split not possible error.
		 *
		 * @param what_arg The error message
		 */
		explicit split_not_possible_error(const char * what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}
	
	};

	
	/**
	 * This class represents the error that a sample is contradictory.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class sample_error : public std::runtime_error
	{

	public:
	
		/**
		 * Constructs a new sample error.
		 *
		 * @param what_arg The error message
		 */
		explicit sample_error(const std::string & what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}


		/**
		 * Constructs a new sample error.
		 *
		 * @param what_arg The error message
		 */
		explicit sample_error(const char * what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}
	
	};
	
	
	/**
	 * This class represents an internal error of the learner.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class internal_error : public std::runtime_error
	{
	
	public:
	
	
		/**
		 * Constructs a new internal error.
		 *
		 * @param what_arg The error message
		 */
		explicit internal_error(const std::string & what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}


		/**
		 * Constructs a new internal error.
		 *
		 * @param what_arg The error message
		 */
		explicit internal_error(const char * what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}
		
	};
	
	
	/**
	 * This class represents errors occurring when reading a sample from file.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class boogie_io_error : public std::runtime_error
	{
	
	public:
	
	
		/**
		 * Constructs a new Boogie IO error.
		 *
		 * @param what_arg The error message
		 */
		explicit boogie_io_error(const std::string & what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}


		/**
		 * Constructs a new Boogio IO error.
		 *
		 * @param what_arg The error message
		 */
		explicit boogie_io_error(const char * what_arg)
			: runtime_error(what_arg)
		{
			// Nothing
		}

	};
	
}; // End namespace horn_verification

#endif