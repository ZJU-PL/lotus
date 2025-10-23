#include "CFL/Graspan/preproc/run_pre.h"

int run_preprocessing(Context &context) {
	cout << "##### PREPROCESSING START #####" << endl;

	clock_t begin, end;
	//new Preproc
	
	begin = clock();

	Preproc_new preN(context);
	preN.setMapInfo(context.grammar.getMapInfo(), context.grammar.getErules());
	preN.countNum(context);
	preN.setVIT(context);
	context.ddm.setNumPartition(context.getNumPartitions());
	context.ddm.reSize();
	preN.saveData(context);
	preN.mergePart(context);
	//preN.test_saveData(context);
	//preN.test_mergePart(context);
	end = clock();
	cout << "NEWPREPROC TIME: " << ((end - begin) / CLOCKS_PER_SEC) << std::endl;
	return 0;
	
	begin = clock();
	Preproc pre(context);
	pre.setMapInfo(context.grammar.getMapInfo(), context.grammar.getErules());

	//save vit file

	pre.countNum(context);
	pre.saveData(context);
	pre.makeVIT(context);		

	context.ddm.setNumPartition(pre.getNumOfPartitions());
	context.ddm.reSize();

	//save text file
	//begin = clock();
	//pre.makePart(context); // Make human readable file and ddm :TODO: factor the part of ddm out
	//end = clock();
	//cout << "makePart time : " << ((end - begin) / CLOCKS_PER_SEC) << std::endl;

	//save binary file and DDM
	//begin = clock();
	pre.makeBinaryPart(context);

	pre.makeDDM(context);
	end = clock();
	//cout << "makeBinaryPart time : " << ((end - begin) / CLOCKS_PER_SEC) << std::endl;
	cout << "PREPROC TIME: " << ((end - begin) / CLOCKS_PER_SEC) << std::endl;
	return 0;
}
