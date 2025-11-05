/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __BOOGIE_IO_H__
#define __BOOGIE_IO_H__

// C++ includes
#include <algorithm>
#include <functional> 
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Project includes
#include "attributes_metadata.h"
#include "datapoint.h"
#include "horn_constraint.h"
#include "json_visitor.h"

namespace horn_verification
{

	/**
	 * This class provides methods to interface with Boogie (i.e., to read samples and horn
	 * constraints from file and to write decision trees to file).
	 *
	 * @author Daniel Neider
	 *
	 * @version 1.0
	 */
	class boogie_io
	{
	
	public:
	
		/**
		 * Removes white spaces from the beginning of \p str in place.
		 *
		 * @param str The string to remove whitespaces from
		 */
		static void ltrim(std::string & str)
		{
			str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char c) { return !std::isspace(c); }));
		}
	
	
		/**
		 * Removes white spaces from the end of \p str in place.
		 *
		 * @param str The string to remove whitespaces from
		 */
		static void rtrim(std::string & str)
		{
			str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), str.end());
		}
	
		
		/**
		 * Removes white spaces from the beginning and the end of \p str in place.
		 *
		 * @param str The string to remove whitespaces from
		 */
		static void trim(std::string & str)
		{
			ltrim(str);
			rtrim(str);
		}


		/**
		 * Splits the given string at the positionbs of the character \p delim. The flag \skip_empty
		 * indicates whether empty strings (i.e., the empty string between two consecutive
		 * delimiters) should occur in the result. If \p delim is a blank and \p skip_empty is
		 * \c true, dublicate blanks are discarded.
		 *
		 * @param str The string to split
		 * @param delim The delimiter
		 * @param skip_empty Selects whether the empty string between two consecutive delimiters
		 *        occurs in the output
		 *
		 * @return a vector of strings that results from plitting \p str at \p delim
		 */
		static std::vector<std::string> split(const std::string & str, char delim, bool skip_empty = false)
		{

			std::vector<std::string> splitted;
			std::stringstream ss(str);
			std::string item;
			
			while (std::getline(ss, item, delim))
			{
				if (!item.empty() || !skip_empty)
				{
					splitted.push_back(std::move(item));
				}
			}
			
			return splitted;

		}
	
	
	public:
	
		/**
		 * Reads the number of invokation of the learner from file (i.e., how often
		 * the learner was already invoked during the learning process).
		 *
		 * @param filename The name of the file to read from
		 *
		 * @returns the number of invokation of the learner
		 */
		static unsigned int read_status_file(const std::string & filename);
	
	
		/**
		 * Reads attribute meta data from file. The format for meta data is as follows:
		 *
		 * <ul>
		 *   <li>Each line defines a single attribute. Each definition is a comma separated list.
		 *       The first entry of each definition describes the type of the attribute. Valid types
		 *       are \c cat (categorical attribute) and \c int (integer attribute).</li>
		 *   <li>The definition of a \e categorical attribute has \e three entries:
		 *     <ol>
		 *       <li>\c cat (indicating that the attribute is categorical)</li>
		 *       <li>The name of the attribute (can be any string without comma)</li>
		 *       <li>The number of categories of this attribute (a natural number greater 0)</li>
		 *     </ol>
		 *   <li>The definition of an \e integer attribute has \e two entries:
		 *     <ol>
		 *       <li>\c int (indicating that the attribute is an integer attribute)</li>
		 *       <li>The name of the attribute (can be any string without comma)</li>
		 *     </ol>
		 *   <li>Attributes must be defined in a specific order:</li>
		 *   <ol>
		 *     <li>Categorical attributes</li>
		 *     <li>Integer attributes</li>
		 *   </ol>
		 * </ul>
		 *
		 * There is no check whether names are distinct.
		 *
		 * Each line starting with # is treated as a comment and ignored.
		 *
		 * @param filename The name of the file to read from
		 *
		 * @returns the meta data read from file
		 */
		static attributes_metadata read_attributes_file(const std::string & filename);

		
		/**
		 * Reads data points from file. Each line is a comma separated list of values (depending on
		 * the types of attributes) and defines a single data point. Each value represents the value
		 * of an attribute in the same order as defined in the names file and given by \p metadata
		 * (see \ref read_attributes_file).
		 *
		 * The following values are valid:
		 * <ul>
		 *   <li><em>Categorical attributes</em>: natural numbers in the closed interval
		 *       <em>[0, number_of_categories - 1]</em>, where <em>number_of_categories</em> is the
		 *       total number of categories for this attribute (as defined by \p metadata).</li>
		 *   <li><em>Categorical attributes</em>: all integers</li>
		 * </ul>
		 * All values are checked for validity while parsing the file.
		 *
		 * Each line starting with # is treated as a comment and ignored.
		 *
		 * @param filename The name of the file to read from
		 * @param metadata Meta data of the attributes
		 *
		 * @returns the data points read from file
		 */
		static std::vector<datapoint<bool>> read_data_file(const std::string & filename, const attributes_metadata & metadata);
	
	
		/**
		 * Reads horn constraints from file and returns them as a pair of sets of indexes that refer to
		 * the position of the data points in the data file.
		 *
		 * Each line in a Horn file is a comma separated list of natural numbers and defines a single
		 * horn constraint. Each constraint consists of a \e body and a \e head.
		 * <ul>
		 *   <li>The body is a non-empty, comma-separated list of natural numbers. Each number is
		 *       interpreted as index into the \p datapoints vector and, hence, corresponds to a
		 *       data point.</li>
		 *   <li>The head is a single value with can be either a natural number (which is then
		 *       interpreted as an index into the \p datapoints vector) or the underscore symbol "_"
		 *       (which is interpreted as empty head).</li>
		 * </ul>
		 *
		 * Each line starting with # is treated as a comment and ignored.
		 *
		 * \post Ensures that each data point has the correct number of values for each type of
		 *       attribute type and that each value is valid.
		 *
		 * @param filename The name of the file to read from
		 *
		 * @returns the horn constraints as indexes read from file
		 */		
		static std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>> read_horn_file(const std::string & filename);
	
		
		/**
		 * Reads horn constraints from file and turns it into horn_constraint objects.
		 *
		 * @param filename The name of the file to read from
		 * @param datapoints The data points to which the Horn constraints refer
		 *
		 * @returns the horn constraints read from file
		 */		 
		static std::vector<horn_constraint<bool>> read_horn_file(const std::string & filename, std::vector<datapoint<bool>> & datapoints);
	
	
		/**
		 * Converts Horn constraints as a pair of sets of indexes into horn_constraint objects.
		 *
		 * @param horn_constraints_as_indexes the Horn constraints given as pairs of sets of indexes
		 * @param datapoints The data points to which the Horn constraints refer
		 *
		 * @param the Horn constraints as horn_constraint objects
		 */
		static std::vector<horn_constraint<bool>> indexes2horn_constraints(const std::vector<std::pair<std::set<unsigned>, std::set<unsigned>>> & horn_constraints_as_indexes, std::vector<datapoint<bool>> & datapoints);
	
	
		/**
		 * Write a JSON serialization of a decision tree (with attribute given by \p metadata) to
		 * file.
		 *
		 * @param filename The name of the file to write
		 * @param tree The decision tree to serialize
		 * @param metadata Meta data of the attributes
		 */
		static void write_json_file(const std::string & filename, decision_tree & tree, const attributes_metadata & metadata);


		/**
		 * Reads an interval file from file.
		 * Each interval is represented by a line containing two natural numbers:
		 * the lower and the upper bound of the interval.
		 *
		 * @param filename The name of the file to write
		 *
		 * @param a vector of intervals
		 */
		static std::vector<std::pair<unsigned, unsigned>> read_intervals_file(const std::string & filename);
		

		/**
		 * Get Horn contraints for all pairs of points in \p datapoint, which are indistinguishable using |thresholds| < \p threshold.
		 *
		 * @param datapoint The datapoints passed to the learner
		 * @param threshold Determines which datapoints are induistinguishable. A Pair of datapoints d1, d2 are indistinguishable 
		 * 		    if they cannot be separated by any cut with a modulus < \p threshold. 
		 *
		 * @returns Additional horn constraints which establishes equality between all indistinguishable datapoints. 
		 */
		static std::vector<horn_constraint<bool>> get_indistinguishable_datapoints(std::vector<datapoint<bool>> & datapoint, unsigned int threshold);

	};

}; // End namespace horn_verification

#endif
