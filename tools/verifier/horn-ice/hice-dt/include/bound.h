/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __BOUND_H__
#define __BOUND_H__

// C++ includes
#include <fstream>


namespace horn_verification
{

	/**
	 * This class provides an mechanism to monitor and increase the bounds used to
	 * bound the decision tree learning algorithm.
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	template <int MAX_BOUND = 20>
	class bound
	{
		
		/// The current bound
		int _bound;
		
		/// Indicates whether the bound is to be used or not
		bool _use_bound;
		
	public:
		
		/**
		 * Creates a new object with the given parameters.
		 *
		 * @param bound The bound to use_bound
		 * @param use_bound Indicates whether the bound is to be used
		 */
		bound(int bound = 1, bool use_bound = true)
			: _bound(bound), _use_bound(use_bound)
		{
			// Nothing
		}
		
		
		/**
		 * Returns the current bound.
		 *
		 * @return the current bound
		 */
		int get_bound() const
		{
			return _bound;
		}
		
		
		/**
		 * Returns whether the bound is to be used. This depends on the whether
		 * the bound should be used and whether it has not yet surpassed its
		 * maximum value.
		 *
		 * @return whether the bound is to be used
		 */
		bool use_bound() const
		{
			return _use_bound && _bound <= MAX_BOUND;
		}
		
		
		/**
		 * Sets whether the bound is to be used.
		 *
		 * @param b whether the bound is to be used
		 */
		void set_use_bound(bool b)
		{
			_use_bound = b;
		}

		
		/**
		 * Increases the bound (by doubling it).
		 */
		void increase_bound()
		{
			_bound = _bound * 2;

		}
		
		
		/**
		 * Sets a new bound.
		 *
		 * @param bound the new bound
		 */
		void set_bound(int bound)
		{
			_bound = bound;
		}
		
		
		/**
		 * Writes a textual representation to an output stream.
		 *
		 * @param out the output stream to write to
		 * @param b the bound to write
		 *
		 * @return a reference to the output stream given as argument
		 */
		friend std::ostream & operator<<(std::ostream & out, const bound & b)
		{
			
			out << "[bound=" << b._bound << "; use_bound=" << b._use_bound << "; MAX_BOUND=" << MAX_BOUND << "]";
			
			return out;
			
		}
		
		
		/**
		 * Writes the current bounds to file.
		 *
		 * @param filename the name of the file to write to
		 * @param b the bounds to write
		 */
		static void write_bound_file(const std::string & filename, const bound & b)
		{
			
			// Open file
			std::ofstream outfile;
			outfile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
			outfile.open(filename, std::ofstream::out);
			
			// Write bound
			outfile << b.get_bound();
			
			// Close file
			outfile.close();
			
		}
		
	};

}; // End namespace horn_verification

#endif