/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __CHCTEACHER_HORN_COUNTEREXAMPLE_H__
#define __CHCTEACHER_HORN_COUNTEREXAMPLE_H__

// C++ includes
#include <list>
#include <ostream>

// C includes
#include <cassert>

// Project includes
#include "datapoint.h"


namespace chc_teacher
{
	
	class horn_counterexample
	{
		
	public:

		/// Concrete data points of the left-hand-side
		std::list<datapoint> lhs;
		
		/// Concrete data points of the right-hand-side
		std::list<datapoint> rhs;
		

		horn_counterexample(std::list<datapoint> && lhs, std::list<datapoint> && rhs)
			: lhs(std::move(lhs)), rhs(std::move(rhs))
		{
			assert (rhs.size() <= 1);
		}
		
		
		friend std::ostream & operator<<(std::ostream & out, const horn_counterexample & ce)
		{
			
			out << "{";
			unsigned i = 0;
			for (const auto & dp : ce.lhs)
			{
				out << (i++ > 0 ? ", " : "") << dp;
			}
			out << "} => {";
			
			i = 0; 
			for (const auto & dp : ce.rhs)
			{
				out << (i++ > 0 ? ", " : "") << dp;
			}
			out << "}";
			
			
			return out;
			
		}
		
	};

}; // End namespace chc_teacher

#endif