#ifndef REPART_H
#define REPART_H

#include <iostream>
#include <fstream>
#include <vector>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <algorithm>
#include <stdio.h>
#include <time.h>
#include <set>

#include "CFL/Graspan/utilities/globalDefinitions.hpp"
#include "CFL/Graspan/datastructures/partition.h"
#include "CFL/Graspan/datastructures/vertex.h"
#include "CFL/Graspan/datastructures/context.h"
#include "CFL/Graspan/datastructures/loader.h"


class Repart {
private:
	Partition p1_2;
	Partition p2_2;
public:
	//p1 is exceeded partition and p2 is new partition
	void repartition(Partition &p1, Partition &p2, Context &context);
	void run(Partition &p1, Partition &p2, Context &context, long newIterEdges);

	inline void setPartitionP1_2(Partition &p1_2) { this->p1_2 = p1_2; }
	inline void setPartitionP2_2(Partition &p2_2) { this->p2_2 = p2_2; }

	inline Partition &getPartitionP1_2() { return p1_2; }
	inline Partition &getPartitionP2_2() { return p2_2; }

};

#endif
