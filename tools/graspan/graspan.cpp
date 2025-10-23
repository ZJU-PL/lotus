#include "utilities/timer.h"
#include "edgecomp/engine.h"
#include "preproc/run_pre.h"
#include "datastructures/vit.h"

#include "utilities/globalDefinitions.hpp"

int main(int argc, char *argv[])
{
	Timer graspanTimer, prepTimer, compTimer;
	graspanTimer.startTimer();
	Context c(argc, argv);

	if (!c.grammar.loadGrammar(c.getGrammarFile())) {
		cout << "execution failed: couldn't load grammar" << endl;
		return 12;
	}

	if (!c.ddm.load_DDM(c.getGraphFile() + ".ddm"))
		cout << "couldn't load DDM" << endl;

	if (!c.vit.loadFromFile(c.getGraphFile() + ".vit")) 
		cout << "couldn't load VIT" << endl;
	

	// PREPROCESSING
	prepTimer.startTimer();

	cout << "###### STARTING PREPROCESSING #####" << endl;
	if (c.ddm.getNumPartition() != c.vit.getNumPartition() || c.vit.getNumPartition() == 0) {
		run_preprocessing(c);
	}

	prepTimer.endTimer();
	// COMPUTATION
	compTimer.startTimer();

	cout << "###### STARTING COMPUTATION #####" << endl;
	long newEdges = run_computation(c);

	compTimer.endTimer();

	graspanTimer.endTimer();
	std::cerr << "===== GRASPAN FINISHED =====" << endl;
	cout << "TOTAL PREPROC TIME: " << prepTimer.hmsFormat() << endl << endl;
	cout << "TOTAL NUM NEW EDGE: " << newEdges << endl;
	cout << "TOTAL COMPUTE TIME: " << compTimer.hmsFormat() << endl << endl;
	cout << "TOTAL GRASPAN TIME: " << graspanTimer.hmsFormat() << "\n" << endl;

	return 0;
}
