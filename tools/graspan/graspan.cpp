#include "CFL/Graspan/utilities/timer.h"
#include "CFL/Graspan/edgecomp/engine.h"
#include "CFL/Graspan/preproc/run_pre.h"
#include "CFL/Graspan/datastructures/vit.h"
#include "CFL/Graspan/utilities/globalDefinitions.hpp"


int main(int argc, char *argv[])
{
	Timer graspanTimer, prepTimer, compTimer;
	graspanTimer.startTimer();
	Context c(argc, argv);

	if (!c.grammar.loadGrammar(c.getGrammarFile())) {
		cout << "execution failed: couldn't load grammar\n";
		return 12;
	}

	if (!c.ddm.load_DDM(c.getGraphFile() + ".ddm"))
		cout << "couldn't load DDM\n";

	if (!c.vit.loadFromFile(c.getGraphFile() + ".vit")) 
		cout << "couldn't load VIT\n";
	

	// PREPROCESSING
	prepTimer.startTimer();

	cout << "###### STARTING PREPROCESSING #####\n";
	if (c.ddm.getNumPartition() != c.vit.getNumPartition() || c.vit.getNumPartition() == 0) {
		run_preprocessing(c);
	}

	prepTimer.endTimer();
	// COMPUTATION
	compTimer.startTimer();

	cout << "###### STARTING COMPUTATION #####\n";
	long newEdges = run_computation(c);

	compTimer.endTimer();

	graspanTimer.endTimer();
	std::cerr << "===== GRASPAN FINISHED =====" << "\n";
	cout << "TOTAL PREPROC TIME: " << prepTimer.hmsFormat() << "\n" << "\n";
	cout << "TOTAL NUM NEW EDGE: " << newEdges << "\n";
	cout << "TOTAL COMPUTE TIME: " << compTimer.hmsFormat() << "\n" << "\n";
	cout << "TOTAL GRASPAN TIME: " << graspanTimer.hmsFormat() << "\n" << "\n";

	return 0;
}
