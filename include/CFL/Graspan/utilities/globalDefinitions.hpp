#ifndef UTILITIES_GLOBALDEFINITIONS_HPP
#define UTILITIES_GLOBALDEFINITIONS_HPP

// This file holds all globally used constants & types, class (forward)
// declarations and a few frequently used utility functions.
#include <unistd.h>

#include <string>
#include <string.h>
#include <iostream>
#include <vector>
#include <unordered_set>
#include <set>
#include <map>
#include <mutex>
#include <condition_variable>
using std::vector;
using std::unordered_set;
using std::pair;
using std::set;
using std::cout;
using std::endl;
using std::map;

//#ifndef ASSERT
//#define ASSERT
//#endif 

//#ifndef DEBUG
//#define DEBUG
//#endif

//----------------------------------------------------------------------------------------------------
typedef char label_t;
typedef int vertexid_t;
typedef int partitionid_t;
typedef std::string raw_label_t;
typedef std::string string;

// error codes
enum Error 
{
	FOPEN, FCLOSE
};

// constant value
const int BytesPerVertexid = sizeof(vertexid_t);
const int BytesPerLabel = sizeof(label_t);
const int BytesPerPartitionid = sizeof(partitionid_t);
 
// keys for parameters
const static string GRAP = "graph";
const static string PART = "partition";
const static string BINA = "binary";
const static string HUMA = "human";

const static string INPUT_GRAPH_FILE_KEY = "inputGraph";
const static string GRAMMAR_FILE_KEY = "grammar";
const static string MEM_BUDGET_KEY = "memBudget";
const static string NUM_THREADS_KEY = "numThreads";
const static string NUM_PARTITION_KEY = "numPartitions";
const static string MAX_EDGES_PER_PARTITION_KEY = "maxEdgesPerPartiton";

// more user arguments:
// intial size of DDM, the default size can be 2x numPartition
// a temporary directory for storing things when you do comptutation
// instead of a graph, a directory that has VIT and intial partition files
// graphname to distinguish graph instances: we have that GRAP already
// whether edge should contain label (no label in case of data flow analysis) -> Vertex structure has no 2nd vector

const static string INSERT_SORT_FLAG = "insertionSort";
const static string ALTER_SCHEDULE_FLAG = "alterSchedule";

//----------------------------------------------------------------------------------------------------
#ifdef ASSERT
#define assert(p, msg)                                                \
   if (!(p)) {                                                        \
     fprintf(stderr, "assert(" #p ") failed file %s, line %d: %s \n", \
             __FILE__, __LINE__, msg);       \
     exit(1); \
   }
#else 
#define assert(p,msg)
#endif 




//----------------------------------------------------------------------------------------------------
#endif
