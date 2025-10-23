#ifndef COMPUTE_H
#define COMPUTE_H

#include <iostream>
#include <string>
#include <ctime>
#include <utility>

#include "CFL/Graspan/edgecomp/edgemerger.h"
#include "CFL/Graspan/datastructures/vertex.h"
#include "CFL/Graspan/datastructures/loadedvertexinterval.h"
#include "CFL/Graspan/datastructures/computationset.h"
#include "CFL/Graspan/datastructures/context.h"
#include "CFL/Graspan/utilities/globalDefinitions.hpp"

long updateEdges(int vertInd, ComputationSet compsets[], LoadedVertexInterval intervals[], Context &context);

#endif
