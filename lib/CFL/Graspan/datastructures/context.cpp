#include "CFL/Graspan/datastructures/context.h"

Context::Context(int argc, char** argv) {
	//if user don't typing input value
	cout << "argc ==" << argc << "\n";
	char *p_token = NULL;
	char *context = NULL;

	//default values
	memBudget = (unsigned long long int)8 * (unsigned long long int)1073741824; // 1073741824
	parameters.insert(std::make_pair(NUM_PARTITION_KEY, 2));
	flags.insert(std::make_pair(INSERT_SORT_FLAG, false));
	flags.insert(std::make_pair(ALTER_SCHEDULE_FLAG, false));

	for (int i = 0; i < argc; i++) {
		p_token = strtok_r(argv[i], "=", &context);
		if (!strncmp(argv[i], INPUT_GRAPH_FILE_KEY.c_str(), INPUT_GRAPH_FILE_KEY.length())) {
			filePath.insert(std::make_pair(INPUT_GRAPH_FILE_KEY, context));
			cout << "INPUT_GRAPH_FILE_KEY =" << context << "\n";
		}
		else if (!strncmp(argv[i], GRAMMAR_FILE_KEY.c_str(), GRAMMAR_FILE_KEY.length())) {
			filePath.insert(std::make_pair(GRAMMAR_FILE_KEY, context));
			cout << "GRAMMAR_FILE_KEY =" << context << "\n";
		}
		else if (!strncmp(argv[i], MEM_BUDGET_KEY.c_str(), MEM_BUDGET_KEY.length())) {
			parameters.insert(std::make_pair(MEM_BUDGET_KEY, (unsigned long long int)atoi(context)*(unsigned long long int)1073741824));
			memBudget = (unsigned long long int)atoi(context)*(unsigned long long int)1073741824;
			cout << "MEM_BUDGET_KEY =" << memBudget << "\n";
		}
		else if (!strncmp(argv[i], NUM_THREADS_KEY.c_str(), NUM_THREADS_KEY.length())) {
			parameters.insert(std::make_pair(NUM_THREADS_KEY, atoi(context)));
			cout << "NUM_THREADS_KEY = " << context << "\n";
		}
		else if (!strncmp(argv[i], NUM_PARTITION_KEY.c_str(), NUM_PARTITION_KEY.length())) {
			parameters.find(NUM_PARTITION_KEY)->second = atoi(context);
			cout << "NUM_PARTITION_KEY =" << atoi(context) << "\n";
		}
		else if (!strncmp(argv[i], MAX_EDGES_PER_PARTITION_KEY.c_str(), MAX_EDGES_PER_PARTITION_KEY.length())) {
			parameters.insert(std::make_pair(MAX_EDGES_PER_PARTITION_KEY, atoi(context)));
			cout << "MAX_EDGES_PER_PARTITION_KEY =" << atoi(context) << "\n";
		}
		else if (!strncmp(argv[i], INSERT_SORT_FLAG.c_str(), INSERT_SORT_FLAG.length())) {
			if (atoi(context) == 0)
				flags.find(INSERT_SORT_FLAG)->second = false;
			else
				flags.find(INSERT_SORT_FLAG)->second = true;
			cout << "INSERT_SORT_FLAG =" << atoi(context) << "\n";
		}
		else if (!strncmp(argv[i], ALTER_SCHEDULE_FLAG.c_str(), ALTER_SCHEDULE_FLAG.length())) {
			if (atoi(context) == 0)
				flags.find(ALTER_SCHEDULE_FLAG)->second = false;
			else
				flags.find(ALTER_SCHEDULE_FLAG)->second = true;
			cout << "ALTER_SCHEDULE_FLAG =" << atoi(context) << "\n";
		}
	}

	//if there are no inputs about file paths.
	if (filePath.size() != 2)
		assert(false, "Need a file path ");
}

unsigned long long int Context::getMemBudget() {
	return memBudget;
}

int Context::getNumPartitions() {
	return parameters[NUM_PARTITION_KEY];
}

int Context::getMaxEdges() {
	return parameters[MAX_EDGES_PER_PARTITION_KEY];
}

int Context::getNumThreads()
{
	return parameters[NUM_THREADS_KEY];
}

string Context::getGraphFile() {
	return filePath[INPUT_GRAPH_FILE_KEY];
}
string Context::getGrammarFile() {
	return filePath[GRAMMAR_FILE_KEY];
}


bool Context::getInsertFlag() {
	return flags[INSERT_SORT_FLAG];
}

bool Context::getAlterScheduleFlag() {
	return flags[ALTER_SCHEDULE_FLAG];
}

void Context::setNumPartitions(int numPartitions) {
	parameters.find(NUM_PARTITION_KEY)->second = numPartitions;
}
void Context::setMaxEdges(int maxEdges) {
	parameters.find(MAX_EDGES_PER_PARTITION_KEY)->second = maxEdges;
}

void Context::setInsertFlag(bool flag) {
	flags.find(INSERT_SORT_FLAG)->second = flag;
}
void Context::setAlterScheduleFlag(bool flag) {
	flags.find(ALTER_SCHEDULE_FLAG)->second = flag;
}
