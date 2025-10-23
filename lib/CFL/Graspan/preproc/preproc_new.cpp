#include "CFL/Graspan/preproc/preproc_new.h"


void quickSort(vector<vertexid_t>&, vector<label_t>&, int, int);
int partition(vector<vertexid_t>&, vector<label_t>&, int, int);

Preproc_new::Preproc_new(Context &context) {
	vitSize = 0;
	numVertex = 0;
	numEdges = 0;
	count = 0;
}

void Preproc_new::countNum(Context &context)
{
	FILE *fp;
	string label;
	char buf[512];
	char *p_token = NULL;
	char *text = NULL;
	char ctemp[10];
	int src, dst;
	int temp = eRules.size();
	dataSize = 0;
	count = 0;
	int count = 0;
	//fisrt file scan for count how many src is in the file
	//for malloc
	fp = fopen(context.getGraphFile().c_str(), "r");
	if (fp != NULL) {
		while (NULL != fgets(buf, sizeof(buf), fp)) {
			p_token = strtok_r(buf, "\n", &text);
			p_token = strtok_r(buf, "\t", &text);
			count ++;
			src = atoi(p_token);
			if (src > dataSize)
				dataSize = src;
		}
		fclose(fp);
		dataSize++;
	}
	else {
		assert(false, "Cannot open graph_file ");
	}
//	cout << "numEdges =" << count << endl;
	dataCount = new int[dataSize];

	for (int i = 0; i < dataSize; i++) {
		dataCount[i] = 0;
	}

	//count data per src
	fp = fopen(context.getGraphFile().c_str(), "r");
	if (fp != NULL) {
		while (fscanf(fp, "%d\t%d\t%s\n", &src, &dst, ctemp) != EOF) {
			dataCount[src]++;
			count++;
		}
		fclose(fp);
	}
	else {
		assert(false, "cannot open graph_file");
	}
	//get numVertex
	for (int i = 0; i < dataSize; i++) {
		if (dataCount[i] == 0)
			continue;
		numVertex++;
	}
	//add count numErules
	if (temp != 0)
		count += (temp * numVertex);
//	cout << "numVertex = " << numVertex << endl;
}

void Preproc_new::setVIT(Context &context)
{
	int size = context.getNumPartitions();
	int startS = -1, endS = 0;
	unsigned long long int sum = 0, mSum = 0;
	int i = 0, j = 0;
	int temp = eRules.size();
	unsigned long long int mSize = 0;
	
	mSize = (unsigned long long int)numVertex * (unsigned long long int)423 + (unsigned long long int)count*(unsigned long long int)8;


	vector<pair<vertexid_t, vertexid_t>> &tempVIT = context.vit.getVIT();
	vector<int> &vitDegree = context.vit.getDegree();

	if (mSize / (unsigned long long int)size > context.getMemBudget() / (unsigned long long int)4) {
		size = mSize / (context.getMemBudget() / ((unsigned long long int) 4));
		size++;
		context.setNumPartitions(size);
	}

	mSize /= (unsigned long long int)size;

	//make VIT
	startS = -1;
	for (i = 0; i < dataSize; i++) {
		if (dataCount[i] == 0)
			continue;
		if (startS == -1) {
			startS = i;
			endS = i;
		}
		sum += ((unsigned long long int)(dataCount[i] + temp)*(unsigned long long int)8 + (unsigned long long int)423 );
		if (sum > mSize && vitSize != size - 1) {
			vitDegree.push_back(0);
			if (i == dataSize - 1)
				endS = i;
			tempVIT.push_back(std::make_pair(startS, endS));
			startS = -1;
			sum = 0;
			vitSize++;
			if (i != dataSize - 1)
				i--;
		}
		endS = i;
	}
	if (sum != 0) {
		tempVIT.push_back(std::make_pair(startS, endS));
		vitDegree.push_back(0);
		vitSize++;
	}
	context.setNumPartitions(vitSize);
	context.vit.setDegree(vitSize);

	sum = 0;
	//make Vertx Degree
	for (i = 0; i < dataSize; i++) {
		if (dataCount[i] == 0)
			continue;
		sum += dataCount[i];
		mSum += (unsigned long long int)dataCount[i]* (unsigned long long int)8 + (unsigned long long int)423;
		if (mSum > mSize && j != size - 1) {
			if (i != dataSize - 1)
				sum -= dataCount[i];
			vitDegree[j++] = sum;
			sum = 0;
			mSum = 0;
			if (i != dataSize - 1)
				i--;
		}
	}
	if (sum != 0) {
		vitDegree[j] = sum;
	}

	context.vit.writeToFile(context.getGraphFile() + ".vit");
}

void Preproc_new::saveData(Context &context) {
	

	FILE *fp;
	string label;
	char ctemp[10];
	int src, dst;
	int temp;
	int size = context.getNumPartitions();
	int tempSize;
	unsigned long long int mSize = 0;
	int start, end;
	vertices = new Vertex *[size];
	vector<int> &vitDegree = context.vit.getDegree();

	for (int i = 0; i < size; i++) {
		vertices[i] = NULL;
	}
	
	numPartBuf = new int[size];

	for (int i = 0; i < size; i++) {
		numPartBuf[i] = 0;
	}


	fp = fopen(context.getGraphFile().c_str(), "r");
	if (fp != NULL) {
		while (fscanf(fp, "%d\t%d\t%s\n", &src, &dst, ctemp) != EOF) {

			temp = context.vit.partition(src);
			start = context.vit.getStart(temp);
			end = context.vit.getEnd(temp);
			//x = 207 y = 36
			//memory allocate when the size is not over the membudget
			
			if (vertices[temp] == NULL) {
				tempSize = end - start + 1;
				//if over size is over the membudget then save and delete the exist one.
			
				if (mSize + tempSize * (unsigned long long int)207 + vitDegree[temp] * (unsigned long long int)36 > context.getMemBudget()) {
					for (int i = 0; i < size; i++) {
						if (vertices[i] != NULL) {
							//save
							this->savePartChunk(context, i);
							numPartBuf[i] = 0;

							Vertex *&vTemp = vertices[i];
							vTemp->clearVector();
							delete[] vTemp;
							mSize -= (context.vit.getEnd(i) - context.vit.getStart(i) + 1 ) * (unsigned long long int)207 + vitDegree[i] * (unsigned long long int)36;

							vertices[i] = NULL;
							break;
						}
					}
				}
				mSize += tempSize * (unsigned long long int)207 + vitDegree[temp] * (unsigned long long int)36;
				//allocate the new one
				Vertex * vTemp = new Vertex[tempSize];
				vertices[temp] = vTemp;

			}
			Vertex *&vTemp = vertices[temp];
			vector<vertexid_t> &outEdges = vTemp[src-start].getOutEdges();
			vector<label_t> &outEdgeValues = vTemp[src-start].getOutEdgeValues();
			label += ctemp;
			outEdges.push_back(dst);
			for (int i = 1; i < mapInfo.size(); i++) {
				if (strcmp(label.c_str(), mapInfo[i].c_str()) == 0) {
					outEdgeValues.push_back(i);
					vTemp[src-start].setNumOutEdges(outEdgeValues.size());

					break;
				}
				//label is not exist in the grammar
				if (i == mapInfo.size() - 1) {
					mapInfo.push_back(label.c_str());
					continue;
				}
			}

			numPartBuf[temp]++;
			label = "";
		}
		fclose(fp);
		for (int i = 0; i < size; i++) {
			if (numPartBuf[i] == 0)
				continue;
			this->savePartChunk(context, i);

			Vertex *&vTemp = vertices[i];
			delete[] vTemp;
			vertices[i] = NULL;
		}
	}
	else {
		assert(false, "cannot open graph_file");
	}
	delete[]numPartBuf;
}
/*
void Preproc_new::saveData(Context &context) {
	FILE *fp;
	string label;
	char ctemp[10];
	int src, dst;
	int temp;
	int size = context.getNumPartitions();
	partBuf = new Vertex[dataSize];
	numPartBuf = new int[size];
	
	for (int i = 0; i < size; i++) {
		numPartBuf[i] = 0;
	}


	fp = fopen(context.getGraphFile().c_str(), "r");
	if (fp != NULL) {
		while (fscanf(fp, "%d\t%d\t%s\n", &src, &dst, ctemp) != EOF) {

			temp = context.vit.partition(src);
			vector<vertexid_t> &outEdges = partBuf[src].getOutEdges();
			vector<label_t> &outEdgeValues = partBuf[src].getOutEdgeValues();
			label += ctemp;
			outEdges.push_back(dst);
			for (int i = 1; i < mapInfo.size(); i++) {
				if (strcmp(label.c_str(), mapInfo[i].c_str()) == 0) {
					outEdgeValues.push_back(i);
					partBuf[src].setNumOutEdges(outEdgeValues.size());
					break;
				}
			}

			numPartBuf[temp]++;
			label = "";

			if (numPartBuf[temp]*8 > context.getMemBudget() / (size * 90)) {
				this->savePartChunk(context, temp);
				numPartBuf[temp] = 0;
			}
		}
		fclose(fp);
		for (int i = 0; i < size; i++) {
			if (numPartBuf[i] == 0)
				continue;
			this->savePartChunk(context, i);
		}
	}
	else {
		assert(false, "cannot open graph_file");
	}
	delete[]numPartBuf;
}
*/
void Preproc_new::savePartChunk(Context & context, int pID)
{
	FILE *f;
	string str, str2;
	string name;
	set<char>::iterator it_e; //for eRules

	Vertex *&vTemp = vertices[pID];
	int degree, dst;
	int k = 0;

	char label;
	int start, end;			
	start = context.vit.getStart(pID);
	end = context.vit.getEnd(pID);

	str = std::to_string((long long)pID);
	str2 = std::to_string((long long)k++);
	name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	while (access(name.c_str(), 0) != -1) {
		str2 = std::to_string((long long)k++);
		name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	}

	f = fopen(name.c_str(), "ab");
	if (f != NULL) {
		for (int i = context.vit.getStart(pID); i <= context.vit.getEnd(pID); i++) {
			degree = vTemp[i-start].getNumOutEdges();
			if (degree == 0)
				continue;
			fwrite((const void*)& i, sizeof(int), 1, f);
			fwrite((const void*)& degree, sizeof(int), 1, f);
			vector<vertexid_t> &outEdges = vTemp[i - start].getOutEdges();
			vector<label_t> &outEdgeValues = vTemp[i - start].getOutEdgeValues();
			
			for (int j = 0; j < outEdges.size(); j++) {
				fwrite((const void*)& outEdges[j], sizeof(int), 1, f);
				fwrite((const void*)& outEdgeValues[j], sizeof(char), 1, f);
			}
			vTemp[i - start].clearVector();
		}
		fclose(f);
	}
	else {
		assert(false, "Cannot make Binary file ");
	}
}
/*
void Preproc_new::savePartChunk(Context &context, int pID) {
	FILE *f;
	string str, str2;
	string name;
	set<char>::iterator it_e; //for eRules

	int degree, dst;
	int k = 0;
	
	char label;

	str = std::to_string((long long)pID);
	str2 = std::to_string((long long)k++);
	name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	while (access(name.c_str(), 0) != -1) {
		str2 = std::to_string((long long)k++);
		name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	}

	f = fopen(name.c_str(), "ab");
	if (f != NULL) {
		for (int i = context.vit.getStart(pID); i <= context.vit.getEnd(pID) ; i++) {
			degree = partBuf[i].getNumOutEdges();
			if (degree == 0)
				continue;
			fwrite((const void*)& i, sizeof(int), 1, f);
			fwrite((const void*)& degree, sizeof(int), 1, f);

			vector<vertexid_t> &outEdges = partBuf[i].getOutEdges();
			vector<label_t> &outEdgeValues = partBuf[i].getOutEdgeValues();

			for (int j = 0; j < outEdges.size(); j++) {
				fwrite((const void*)& outEdges[j], sizeof(int), 1, f);
				fwrite((const void*)& outEdgeValues[j], sizeof(char), 1, f);
			}
			partBuf[i].clearVector();
		}
		fclose(f);
	}
	else {
		assert(false, "Cannot make Binary file ");
	}
}*/

void Preproc_new::mergePart(Context & context)
{
	clock_t begin, end;

	numVertex = 0;
	cout << context.getNumPartitions() << endl;
	for (int i = 0; i < context.getNumPartitions(); i++) {
		Vertex * vTemp = new Vertex[context.vit.getEnd(i) - context.vit.getStart(i) + 1];
		vertices[i] = vTemp;
		loadPartChunk(context, i);
		//add erule
		begin = clock();
		addErules(context, i);
		end = clock();
		cout << "SORTING PART " << i << " TIME: " << ((end - begin) / CLOCKS_PER_SEC) << std::endl;
		//check Duplicates
		checkPart(context, i);
		//save
		savePart(context, i);

		vTemp->clearVector();
		delete[] vTemp;
	}
	context.ddm.save_DDM(context.getGraphFile() + ".ddm");
	cout << "NumVertex " << numVertex << endl;
	cout << "NumEdges " << numEdges << endl;
}

/*void Preproc_new::mergePart(Context & context)	{
	clock_t begin, end;

	numVertex = 0;
	for (int i = 0; i < context.getNumPartitions(); i++) {
		loadPartChunk(context, i);
		//add erule
		begin = clock();
		addErules(context, i);
		end = clock();
		cout << "SORTING PART " << i << " TIME: " << ((end - begin) / CLOCKS_PER_SEC) << std::endl;
		//check Duplicates
		checkPart(context, i);
		//save
		savePart(context, i);
	}
	context.ddm.save_DDM(context.getGraphFile() + ".ddm");
	cout << "NumVertex " << numVertex << endl;
}*/

void Preproc_new::loadPartChunk(Context & context, int pID)
{
	Vertex *&vTemp = vertices[pID];
	int start, end;
	start = context.vit.getStart(pID);
	end = context.vit.getEnd(pID);

	vector<vector<double> > &ddmMap = context.ddm.getDdmMap();
	FILE *f;
	string str, str2, name;
	char label;
	int src, degree, dst, temp;
	int size, numVert = 0;
	int i = 0;
	char *bbuf;

	str = std::to_string((long long)pID);
	str2 = std::to_string((long long)i++);
	name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	while (access(name.c_str(), 0) != -1) {
		f = fopen(name.c_str(), "r");
		if (f != NULL) {
			while (0 != fread(&src, 4, 1, f)) {
				fread(&degree, 4, 1, f);
				temp = degree * 5;
				size += degree;
				numVert++;
				vector<vertexid_t> &outEdges = vTemp[src-start].getOutEdges();
				vector<label_t> &outEdgeValues = vTemp[src-start].getOutEdgeValues();
				bbuf = (char *)malloc(temp);
				fread(bbuf, temp, 1, f);
				for (int j = 0; j < temp; j += 5) {

					dst = *((int*)(bbuf + j));
					label = *((char*)(bbuf + 4 + j));
					outEdges.push_back(dst);
					outEdgeValues.push_back(label);
				}
				free(bbuf);
				vTemp[src-start].setNumOutEdges(numVert);
				//for ddm
				if (pID != context.vit.partition(dst) && context.vit.partition(dst) != -1)
					ddmMap[pID][context.vit.partition(dst)] ++;
			}
			fclose(f);
		}
		else {
			assert(false, "cant open bin file " + pID);
		}
		numVert = 0;
		str2 = std::to_string((long long)i++);
		name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	}
}

/*void Preproc_new::loadPartChunk(Context &context, int pID) {
	vector<vector<double> > &ddmMap = context.ddm.getDdmMap();
	FILE *f;
	string str, str2, name;
	char label;
	int src, degree, dst, temp;
	int size, numVert = 0;
	int i = 0;
	char *bbuf;

	str = std::to_string((long long)pID);
	str2 = std::to_string((long long)i++);
	name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	while (access(name.c_str(), 0) != -1) {
		f = fopen(name.c_str(), "r");
		if (f != NULL) {
			while (0 != fread(&src, 4, 1, f)) {
				fread(&degree, 4, 1, f);
				temp = degree * 5;
				size += degree;
				numVert++;

				vector<vertexid_t> &outEdges = partBuf[src].getOutEdges();
				vector<label_t> &outEdgeValues = partBuf[src].getOutEdgeValues();
				bbuf = (char *)malloc(temp);
				fread(bbuf, temp, 1, f);
				for (int j = 0; j < temp; j += 5) {

					dst = *((int*)(bbuf + j));
					label = *((char*)(bbuf + 4 + j));

					outEdges.push_back(dst);
					outEdgeValues.push_back(label);
				}
				free(bbuf);
				partBuf[src].setNumOutEdges(numVert);
				//for ddm
				if (pID != context.vit.partition(dst) && context.vit.partition(dst) != -1)
					ddmMap[pID][context.vit.partition(dst)] ++;
			}
			fclose(f);
		}
		else {
			assert(false, "cant open bin file " + pID);
		}
		numVert = 0;
		str2 = std::to_string((long long)i++);
		name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str() + "." + str2.c_str();
	}
}*/

void Preproc_new::addErules(Context & context, int pID)
{
	Vertex *&vTemp = vertices[pID];
	int start, end;
	start = context.vit.getStart(pID);
	end = context.vit.getEnd(pID);

	set<char>::iterator it_e; //for eRules
	char label;

	for (int i = context.vit.getStart(pID); i <= context.vit.getEnd(pID); i++) {

		//	if (partBuf[j].getNumOutEdges() == 0 && dataCheck[j] == 0)
		//		continue;
		vector<vertexid_t> &outEdges = vTemp[i-start].getOutEdges();
		vector<label_t> &outEdgeValues = vTemp[i-start].getOutEdgeValues();

		for (it_e = eRules.begin(); it_e != eRules.end(); it_e++) {
			label = *it_e;
			outEdges.push_back(i);
			outEdgeValues.push_back(label);
		}
		quickSort(outEdges, outEdgeValues, 0, outEdges.size());
		vTemp[i-start].setNumOutEdges(outEdges.size());
	}
}
/*
void Preproc_new::addErules(Context &context, int pID) {
	set<char>::iterator it_e; //for eRules
	char label;

	for (int i = context.vit.getStart(pID); i <= context.vit.getEnd(pID); i++) {

		//	if (partBuf[j].getNumOutEdges() == 0 && dataCheck[j] == 0)
		//		continue;

		vector<vertexid_t> &outEdges = partBuf[i].getOutEdges();
		vector<label_t> &outEdgeValues = partBuf[i].getOutEdgeValues();

		for (it_e = eRules.begin(); it_e != eRules.end(); it_e++) {
			label = *it_e;
			outEdges.push_back(i);
			outEdgeValues.push_back(label);
		}
		quickSort(outEdges, outEdgeValues, 0, outEdges.size());
		partBuf[i].setNumOutEdges(outEdges.size());
	}
}*/

void Preproc_new::checkPart(Context & context, int pID)
{
	Vertex *&vTemp = vertices[pID];
	int start, end;
	start = context.vit.getStart(pID);
	end = context.vit.getEnd(pID);

	vector<vertexid_t>::iterator firstA, lastA, resultA;
	vector<label_t>::iterator firstB, lastB, resultB;

	for (int i = context.vit.getStart(pID); i <= context.vit.getEnd(pID); i++) {
		vector<vertexid_t> &outEdges = vTemp[i-start].getOutEdges();
		vector<label_t> &outEdgeValues = vTemp[i-start].getOutEdgeValues();

		firstA = outEdges.begin();
		lastA = outEdges.end();
		firstB = outEdgeValues.begin();
		lastB = outEdgeValues.end();

		if (firstA == lastA) continue;

		resultA = firstA;
		resultB = firstB;
		while (++firstA != lastA)
		{
			++firstB;
			if (!(*resultA == *firstA) | !(*resultB == *firstB)) {
				*(++resultA) = *firstA;
				*(++resultB) = *firstB;
			}
		}
		++resultA;
		++resultB;

		outEdges.erase(resultA, lastA);
		outEdgeValues.erase(resultB, lastB);
	}
}
/*
void Preproc_new::checkPart(Context &context, int pID) {
	
	vector<vertexid_t>::iterator firstA, lastA, resultA;
	vector<label_t>::iterator firstB, lastB, resultB;

	for (int i = context.vit.getStart(pID); i <= context.vit.getEnd(pID); i++) {
		vector<vertexid_t> &outEdges = partBuf[i].getOutEdges();
		vector<label_t> &outEdgeValues = partBuf[i].getOutEdgeValues();

		firstA = outEdges.begin(); 
		lastA = outEdges.end();
		firstB = outEdgeValues.begin();
		lastB = outEdgeValues.end();

		if (firstA == lastA) continue;

		resultA = firstA;
		resultB = firstB;
		while (++firstA != lastA)
		{
			++firstB;
			if (!(*resultA == *firstA) | !(*resultB == *firstB)) {
				*(++resultA) = *firstA;
				*(++resultB) = *firstB;
			}
		}
		++resultA;
		++resultB;

		outEdges.erase(resultA, lastA);
		outEdgeValues.erase(resultB, lastB);
		numVertex += outEdges.size();
	}
}*/

void Preproc_new::savePart(Context & context, int pID)
{
	Vertex *&vTemp = vertices[pID];
	int start, end;
	start = context.vit.getStart(pID);
	end = context.vit.getEnd(pID);
	vector<vector<double> > &ddmMap = context.ddm.getDdmMap();
	vector<vector<double> > &d_ddmMap = context.ddm.getD_ddmMap();
	FILE *f;
	string name;
	char label;
	int src, degree, dst;
	int numVertexEdges = 0;

	name = context.getGraphFile() + "." + PART + "." + BINA + "." + std::to_string((long long)pID);
	f = fopen(name.c_str(), "wb");
	for (int j = context.vit.getStart(pID); j <= context.vit.getEnd(pID); j++) {
		src = j;
		degree = vTemp[j-start].getNumOutEdges();
		numEdges += degree;
		fwrite((const void*)& src, sizeof(int), 1, f);
		fwrite((const void*)& degree, sizeof(int), 1, f);
		
		for (int k = 0; k < degree; k++) {
			dst = vTemp[j-start].getOutEdge(k);
			label = vTemp[j-start].getOutEdgeValue(k);
			fwrite((const void*)& dst, sizeof(int), 1, f);
			fwrite((const void*)& label, sizeof(char), 1, f);
		}
		if (degree != 0)
			numVertex ++;
		numVertexEdges += degree;
		vTemp[j-start].clearVector();
	}
	fclose(f);
	for (int i = 0; i < ddmMap[0].size(); i++) {
		ddmMap[pID][i] /= (double)numVertexEdges;
		d_ddmMap[pID][i] = ddmMap[pID][i];
	}
}
/*
void Preproc_new::savePart(Context &context, int pID) {
	vector<vector<double> > &ddmMap = context.ddm.getDdmMap();
	FILE *f;
	string name;
	char label;
	int src, degree, dst;
	int numVertexEdges = 0;

	name = context.getGraphFile() + "." + PART + "." + BINA + "." + std::to_string((long long)pID);
	f = fopen(name.c_str(), "wb");
	for (int j = context.vit.getStart(pID); j <= context.vit.getEnd(pID); j++) {
		src = j;
		degree = partBuf[j].getNumOutEdges();

		fwrite((const void*)& src, sizeof(int), 1, f);
		fwrite((const void*)& degree, sizeof(int), 1, f);

		for (int k = 0; k < degree; k++) {
			dst = partBuf[j].getOutEdge(k);
			label = partBuf[j].getOutEdgeValue(k);
			fwrite((const void*)& dst, sizeof(int), 1, f);
			fwrite((const void*)& label, sizeof(char), 1, f);
		}
		numVertexEdges += degree;
		partBuf[j].clearVector();
	}
	fclose(f);
	for (int i = 0; i < ddmMap[0].size(); i++)
		ddmMap[pID][i] /= (double)numVertexEdges;
}*/

void Preproc_new::setMapInfo(vector<string> mapInfo, set<char> eRules) {
	set<char>::iterator it_e; //for eRules
	this->mapInfo.assign(mapInfo.begin(), mapInfo.end());

//	for (it_e = eRules.begin(); it_e != eRules.end(); it_e++)
//		this->eRules.insert(mapInfo[(int)*it_e]);

	for (it_e = eRules.begin(); it_e != eRules.end(); it_e++) {
		this->eRules.insert(*it_e);
	}
}

Preproc_new::~Preproc_new()
{
	delete[]dataCount;
	//delete[] partBuf;
}

//make these thing in Vertex
//check the sort time
void quickSort(vector<vertexid_t>& A, vector<label_t>& B, int p, int q)
{
	int r;
	if (p<q)
	{
		r = partition(A, B, p, q);
		quickSort(A, B, p, r);
		quickSort(A, B, r + 1, q);
	}
}


int partition(vector<vertexid_t>& A, vector<label_t>& B, int p, int q)
{
	int x = A[p];
	int i = p;
	int j;

	for (j = p + 1; j<q; j++)
	{
		if (A[j] <= x)
		{
			i ++;
			std::swap(A[i], A[j]);
			std::swap(B[i], B[j]);
		}

	}

	std::swap(A[i], A[p]);
	std::swap(B[i], B[p]);
	return i;
}
