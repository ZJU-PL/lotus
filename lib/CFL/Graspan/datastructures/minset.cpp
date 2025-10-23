#include "CFL/Graspan/datastructures/minset.h"

// constructor
MinSet::MinSet()
{
	ptr = 0;
}


string MinSet::toString()
{
	std::stringstream output;
	
	output << currentVID << ": ";
	for (vector<char>::iterator it = evals.begin(); it != evals.end(); it++)
		output << (short)*it << ", ";

	output << endl;

	return output.str();
}

