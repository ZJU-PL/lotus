#include "CFL/Graspan/edgecomp/repart.h"

void Repart::repartition(Partition &p1, Partition &p2, Context &context) {
	unsigned long long int size = (unsigned long long int)(p1.getNumVertices()) * (unsigned long long int)423 + (unsigned long long int)(p1.getNumEdges()) * (unsigned long long int)8;
	if (size < (context.getMemBudget() / (unsigned long long int) 4)) {
		p2.setExist(false);
		return;
	}
	else
		size /= (unsigned long long int) 2;

	int sum = 0, check = 0;

	int i = 0;
	int end = 0;
	vector<Vertex> data;
	vector<pair<vertexid_t, vertexid_t>> &tempVIT = context.vit.getVIT();
	vector<Vertex> &dataTemp = p1.getData();

	check += dataTemp[0].getNumOutEdges() * 8;
	check += 423;
	for (i ; i < p1.getNumVertices() - 1; i++) {
		sum += dataTemp[i].getNumOutEdges();
		check += dataTemp[i + 1].getNumOutEdges() * 8;
		check += 423;
		if (check > size) {
			break;
		}
	}
	if (check <=size | p1.getNumVertices() == 1) {
		p2.setExist(false);
		return;
	}
	p2.setExist(true);
	context.vit.setVITID(p1.getID(), context.vit.getStart(p1.getID()), dataTemp[i].getVertexID());
	i++;

	tempVIT.push_back(std::make_pair(dataTemp[i].getVertexID(), dataTemp[p1.getNumVertices() - 1].getVertexID()));

	data.resize(p1.getNumVertices() - i);
	std::copy(dataTemp.begin() + i, dataTemp.end(), data.begin());

	for (int idx = dataTemp.size()-1; idx >= i; --idx) {
		dataTemp.erase(dataTemp.begin() + idx);
	}
	dataTemp.resize(i);

	p2.setData(data);	
	p2.setID(context.getNumPartitions());
	context.setNumPartitions(context.getNumPartitions() + 1);
	p2.setNumEdges(p1.getNumEdges() - sum);
	p2.setNumVertices(p1.getNumVertices() - i);

	p1.setNumEdges(sum);
	p1.setNumVertices(i);
	context.vit.writeToFile("graph.vit");
}

void Repart::run(Partition &p1, Partition &p2, Context &context, long newIterEdges) {

	repartition(p1, p1_2, context);
	repartition(p2, p2_2, context);

	context.ddm.setNumPartition(context.getNumPartitions());
	context.ddm.reSize();

	if (newIterEdges > 0) {
		p1.calc_ddr(context);
	}
	if (newIterEdges > 0) {
		p2.calc_ddr(context);
	}
	cout << "exist = " <<  p1_2.getExist() << " " << p2_2.getExist() << endl;
	if (p1_2.getExist()) {
		cout << "p1 =" << p1.getID() << " p12 =" << p1_2.getID() << endl;
		//Partition::writeToFile(p1_2, false, context);
		context.ddm.copy(p1.getID(), p1_2.getID());
		p1_2.calc_ddr(context);
		context.ddm.adjustRow(p1_2.getID());
		if (newIterEdges <= 0) {
			context.ddm.markTerminate(p1.getID(), p1_2.getID(), newIterEdges, newIterEdges);
			context.ddm.markTerminate(p2.getID(), p1_2.getID(), newIterEdges, newIterEdges);
		}
	}
	if (p2_2.getExist()) {
		cout << "p2 =" << p2.getID() << " p12 =" << p2_2.getID() << endl;
		//Partition::writeToFile(p2_2, false, context);
		context.ddm.copy(p2.getID(), p2_2.getID());
		p2_2.calc_ddr(context);
		context.ddm.adjustRow(p2_2.getID());
		if (newIterEdges <= 0) {
			context.ddm.markTerminate(p1.getID(), p2_2.getID(), newIterEdges, newIterEdges);
			context.ddm.markTerminate(p2.getID(), p2_2.getID(), newIterEdges, newIterEdges);
		}
	}
}
