#include "CFL/Graspan/utilities/boost_throw_exception.h"
#include "CFL/Graspan/edgecomp/engine.h"

// For Boost 1.66+, we need the executor_work_guard header
#include <boost/version.hpp>
#if BOOST_VERSION >= 106600
#include <boost/asio/executor_work_guard.hpp>
#endif

long newTotalEdges;
long newRoundEdges;
long newIterEdges;
int iterNo;

std::mutex add_edges, comp_mtx;
std::condition_variable cv;
short numFinished;
bool compFinished;

// FUNCTION DEFS
void initCompSets(ComputationSet compsets[], vector<Vertex> &part1, vector<Vertex> &part2);

void initLVIs(LoadedVertexInterval intervals[], vector<Vertex> &part1, vector<Vertex> &part2);

void computeEdges(ComputationSet compsets[], int setSize, LoadedVertexInterval intervals[], Context &context, unsigned long long int sizeLim, boost_asio_compat::io_service &ioServ);

void computeOneIteration(ComputationSet compsets[], int setSize, int segsiz, int nSegs, LoadedVertexInterval intervals[], Context &context, boost_asio_compat::io_service &ioServ);

void runUpdates(int lower, int upper, int nSegs, ComputationSet compsets[], LoadedVertexInterval intervals[], Context &context);

void updatePartitions(ComputationSet compsets[], Partition &p1, Partition &p2, vector<Vertex> &part1, vector<Vertex> &part2);


/**
 * runs the edge computation for graspan
 */
long run_computation(Context &context)
{
	// load partitions into memory
	Timer loadTimer, compTimer, repartTimer;
	string str;
	string name;
	vector <Partition*> parts;
	vector <Partition*>::iterator it;
	Partition *pp, *qp;
	Repart r;

	boost_asio_compat::io_service ioServ;
	boost::thread_group threadpool;

#if BOOST_VERSION >= 106600
	// In Boost 1.66+, use executor_work_guard instead of io_service::work
	auto work = boost::asio::make_work_guard(ioServ);
#else
	boost::asio::io_service::work work(ioServ);
#endif

	for (int i = 0; i < context.getNumThreads(); i++)
		threadpool.create_thread(boost::bind(&boost_asio_compat::io_service::run, &ioServ));

	partitionid_t p, q, oldP = -1, oldQ = -1;
	newTotalEdges = 0;
	int roundNo = 0, numThreads = context.getNumThreads();
	while (context.ddm.nextPartitionPair(p, q))
	{
		cout << "##### STARTING ROUND " << ++roundNo << " #####" << endl;
		newRoundEdges = 0; 
		loadTimer.startTimer();
		context.ddm.save_DDM(name.c_str());
		pp = NULL, qp = NULL;
		//check parts
		for (it = parts.begin(); it != parts.end(); ) {
			context.ddm.adjustRow((*(*it)).getID());	
			if ((*(*it)).getID() != p && (*(*it)).getID() != q) {
				cout << "PARTITION :" << (*(*it)).getID() << " IS SAVED" << endl;
				Partition::writeToFile(*(*it), false, context);
				(*(*it)).clear();
				delete[](*it);
				it = parts.erase(it);
			}
			else {
				if ((*(*it)).getID() == p)
					pp = *it;
				else
					qp = *it;
				++it;
			}
		}
		//p or q are not exist in parts then load
		if (pp == NULL) {
			Partition *p1 = new Partition[1];
			Loader::loadPartition(p, *p1, false, context);
			parts.push_back(p1);
			pp = p1;
		}
		if (qp == NULL) {
			Partition *p2 = new Partition[1];
			Loader::loadPartition(q, *p2, false, context);
			parts.push_back(p2);
			qp = p2;
		}

		assert((*pp).checkPart() && (*qp).checkPart(), "pre comp : duplicate check");
		//if (!p1.checkPart() || !p2.checkPart()) {
		//	cout << "AGGAGAGGHGHHHHHH!" << endl;
		//	return 12;
		//}
		unsigned long long int sizeLim = (context.getMemBudget()/ (unsigned long long int)2 - (*pp).getNumVertices() * 423 - (*qp).getNumVertices() * 423 - (*pp).getNumEdges()*(unsigned long long int)(8) - (*qp).getNumEdges()*(unsigned long long int)(8)) / (unsigned long long int)(8);

		cout << "MEMBUDGET =" << context.getMemBudget() << endl;
		loadTimer.endTimer();
		cout << "P =" << p << " Q =" << q << endl;
		cout << qp->getNumVertices() << endl;
		vector<Vertex> &part1 = (*pp).getData(), &part2 = (*qp).getData();
		
		cout << (*pp).getID() << endl;
		cout << "OG NUM EDGES: " << ((*pp).getNumEdges() + (*qp).getNumEdges()) << endl;
		cout << "NUM VERTECES: " << part1.size() + part2.size() << endl;

		int setSize = part1.size() + part2.size();
		ComputationSet *compsets = new ComputationSet[setSize];
		initCompSets(compsets, part1, part2);				// Initialize the ComputationSet list

		LoadedVertexInterval intervals[2] = {LoadedVertexInterval{p}, LoadedVertexInterval{q}};
		initLVIs(intervals, part1, part2);					// Initialize the Loaded Vertex Intervals
		
		cout << "== COMP START ==" << endl;
		compTimer.startTimer();
		computeEdges(compsets, setSize, intervals, context, sizeLim, ioServ);
		newTotalEdges += newRoundEdges;
		compTimer.endTimer();
		cout << "== COMP END ==" << endl;

		updatePartitions(compsets, *pp, *qp, part1, part2);	// store information to partitions

		cout << "NEW NUM EDGES: " << ((*pp).getNumEdges() + (*qp).getNumEdges()) << endl;
		assert((*pp).checkPart() && (*qp).checkPart(), "duplicate found");

		//if (!p1.checkPart() || !p2.checkPart()) {
		//	cout << "AGGAGAGGHGHHHHHH!" << endl;
		//	return 12;
		//}

		delete[] compsets;
		if (newTotalEdges > 0) {
			Partition *p3 = new Partition[1];
			Partition *p4 = new Partition[1];
			cout << "== REPA START ==" << endl;
			repartTimer.startTimer();
			r.run(*pp, *qp, context, newIterEdges);
			repartTimer.endTimer();
			*p3 = r.getPartitionP1_2();
			*p4 = r.getPartitionP2_2();

			//if p3 or p4 exist then put in the parts, if not delete
			if (p3->getExist()) {
				parts.push_back(p3);
			}
			else
				delete[] p3;
			if (p4->getExist()) {
				parts.push_back(p4);
			}
			else
				delete[] p4;

			cout << "== REPA END ==" << endl;
		}

		if (newIterEdges <= 0) {
			context.ddm.markTerminate(p, q, 0, 0);
			context.ddm.adjustRow(p);
			context.ddm.adjustRow(q);
			cout << intervals[0].hasNewEdges() << endl;
			cout << intervals[1].hasNewEdges() << endl;
		}
		else {
			context.ddm.adjustRow(p);
			context.ddm.adjustRow(q);
		}
		str = std::to_string((long long)roundNo);
		name = context.getGraphFile() + ".ddm." + str;

		cout << "===== ROUND INFO =====" << endl;
		cout << "NEW EDGES: " << newTotalEdges << endl;
		cout << "LOAD TIME: " << loadTimer.hmsFormat() << endl;
		cout << "COMP TIME: " << compTimer.hmsFormat() << endl;
		cout << "REPA TIME: " << repartTimer.hmsFormat() << endl <<  endl << endl;
	}
#ifdef DEBUG
	cout << "P numEdges =" << (*pp).getNumEdges() << " P numVertices = " << (*pp).getNumVertices() << " Psize = " << (*pp).getData().size() << endl;
	cout << "Q numEdges =" << (*qp).getNumEdges() << " Q numVertices = " << (*qp).getNumVertices() << " Qsize = " << (*qp).getData().size() << endl;
#endif
	//save rest of the files in the parts
	for (it = parts.begin(); it != parts.end(); ) {
		cout << "PARTITION :" << (*(*it)).getID() << " IS SAVED" << endl;
		context.ddm.adjustRow((*(*it)).getID());
		Partition::writeToFile(*(*it), false, context);
		(*(*it)).clear();
		delete[](*it);
		it = parts.erase(it);
	}

	return newTotalEdges;
}

/**
 * perform computation on list of ComputationSets until no more new edges can be added or 
 * the newRoundEdges would exceed the memory Budget
 *
 * @param compsets
 */
void computeEdges(ComputationSet compsets[], int setSize, LoadedVertexInterval intervals[], Context &context, unsigned long long int sizeLim, boost_asio_compat::io_service &ioServ)
{
	Timer iterTimer;
	iterNo = 0;

	cout << "NEW EDGES LIMIT: " << sizeLim << endl;

	int segsiz = setSize / 64 + 1;
	int nSegs = setSize / segsiz + 1;
	
	do {
		cout << "===== STARTING ITERATION " << ++iterNo << endl;
		iterTimer.startTimer();
		computeOneIteration(compsets, setSize, segsiz, nSegs, intervals, context, ioServ);

		newRoundEdges += newIterEdges;

		for (int i = 0; i < setSize; i++)
		{
			compsets[i].setOldEdges(compsets[i].getoldUnewEdges());
			compsets[i].setOldVals(compsets[i].getoldUnewVals());
			compsets[i].setoldUnewEdges(compsets[i].getoUnUdEdges());
			compsets[i].setoldUnewVals(compsets[i].getoUnUdVals());
			compsets[i].setNewEdges(compsets[i].getDeltaEdges());
			compsets[i].setNewVals(compsets[i].getDeltaVals());
		}
		iterTimer.endTimer();

		cout << "EDGES PER SECND: " << (double)newIterEdges / (double)(iterTimer.totalTime() / 1000) << endl;
		cout << "EDGES THIS ITER: " << newIterEdges << endl;
		cout << "NEW EDGES TOTAL: " << newRoundEdges << endl;
		cout << "ITERATER  TIME   " << iterTimer.hmsFormat() << endl << endl;

		if (newRoundEdges > sizeLim || newRoundEdges + newIterEdges > sizeLim) break;	// if the num of new edges added this round exceeds the limit
																						// found using the mem budget, end the round
	} while (newIterEdges > 0);
}

/**
 * compute the new edges of each vertex simultaneously
 *
 * 
 */
void computeOneIteration(ComputationSet compsets[], int setSize, int segsiz, int nSegs, LoadedVertexInterval intervals[], Context &context, boost_asio_compat::io_service &ioServ)
{
	int lower, upper;
	newIterEdges = 0;
	numFinished = 0;
	compFinished = false;

	
	for (int i = 0; i < nSegs; i++)
	{
		lower = i * segsiz;
		upper = (lower + segsiz < setSize) ? lower + segsiz : setSize;
		ioServ.post(boost::bind(runUpdates, lower, upper, nSegs, compsets, intervals, context));
	}

	std::unique_lock<std::mutex> lck(comp_mtx);
	while (!compFinished) cv.wait(lck);
}


void runUpdates(int lower, int upper, int nSegs, ComputationSet compsets[], LoadedVertexInterval intervals[], Context &context)
{
	long newThreadEdges = 0;

	for (int i = lower; i < upper; i++)
	{
		newThreadEdges += updateEdges(i, compsets, intervals, context);
		if (newThreadEdges > 0 && (i >= intervals[0].getIndexStart() && i <= intervals[0].getIndexEnd()))
			intervals[0].setNewEdgeAdded(true);
		else if (newThreadEdges > 0 && (i >= intervals[1].getIndexStart() && i <= intervals[1].getIndexEnd()))
			intervals[1].setNewEdgeAdded(true);

	}

	std::unique_lock<std::mutex> lck(add_edges);
	newIterEdges += newThreadEdges;
	numFinished++;
	if (numFinished == nSegs) {
		compFinished = true;
		cv.notify_one();
	}
}


/**
 * load both partitions into a list of ComputationSet
 *
 * @param compsets[]		-list of ComputationSet
 * @param part1				-vector of Vertex representing partition 1
 * @param part2				-vector of Vertex representing partition 2
 */
void initCompSets(ComputationSet compsets[], vector<Vertex> &part1, vector<Vertex> &part2)
{
	for (int i = 0; i < part1.size(); i++)
	{
		compsets[i].setNewEdges(part1[i].getOutEdges());
		compsets[i].setNewVals(part1[i].getOutEdgeValues());
		compsets[i].setoldUnewEdges(part1[i].getOutEdges());
		compsets[i].setoldUnewVals(part1[i].getOutEdgeValues());
	}
	int offset = part1.size();
	for (int j = part1.size(); j < part1.size() + part2.size(); j++)
	{
		compsets[j].setNewEdges(part2[j - offset].getOutEdges());
		compsets[j].setNewVals(part2[j - offset].getOutEdgeValues());
		compsets[j].setoldUnewEdges(part2[j - offset].getOutEdges());
		compsets[j].setoldUnewVals(part2[j - offset].getOutEdgeValues());
	}
}

/**
 * initialize the LoadedVertexInterval list using the partition information
 */
void initLVIs(LoadedVertexInterval intervals[], vector<Vertex> &part1, vector<Vertex> &part2)
{
	intervals[0].setFirstVertex(part1[0].getVertexID());
	intervals[0].setLastVertex(part1[part1.size() - 1].getVertexID());
	intervals[0].setIndexStart(0);
	intervals[0].setIndexEnd(part1.size()-1);

	intervals[1].setFirstVertex(part2[0].getVertexID());
	intervals[1].setLastVertex(part2[part2.size() - 1].getVertexID());
	intervals[1].setIndexStart(part1.size());
	intervals[1].setIndexEnd(part1.size() + part2.size()-1);
}

/**
 * save newly computed Edges back to their respective vertices
 */
void updatePartitions(ComputationSet compsets[], Partition &p1, Partition &p2, vector<Vertex> &part1, vector<Vertex> &part2)
{
	int p1Edges = 0;
	for (int i = 0; i < part1.size(); i++)
	{
		p1Edges += compsets[i].getoldUnewEdges().size();
		part1[i].setNumOutEdges(compsets[i].getoldUnewEdges().size());
		part1[i].setOutEdges(compsets[i].getoldUnewEdges());
		part1[i].setOutEdgeValues(compsets[i].getoldUnewVals());
	}

	int offset = part1.size(), p2Edges = 0;
	for (int j = 0; j < part2.size(); j++)
	{
		p2Edges += compsets[j+offset].getoldUnewEdges().size();
		part2[j].setNumOutEdges(compsets[j+offset].getoldUnewEdges().size());
		part2[j].setOutEdges(compsets[j+offset].getoldUnewEdges());
		part2[j].setOutEdgeValues(compsets[j+offset].getoldUnewVals());
	}

	p1.setNumEdges(p1Edges);
	p2.setNumEdges(p2Edges);
}
