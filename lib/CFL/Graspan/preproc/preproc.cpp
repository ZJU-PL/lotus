#include "CFL/Graspan/preproc/preproc.h"

bool compareV(const pair<int, string> &lhs, const pair<int, string> &rhs);

Preproc::Preproc(Context &context) {
	vitSize = context.getNumPartitions();
	numVertex = 0;
}

void Preproc::countNum(Context &context)
{
	FILE *fp;
	char buf[512];
	char *p_token = NULL;
	char *text = NULL;
	int src, dst;
	char ctemp[10];
	dataSize = 0;
	count = 0;

	//fisrt file scan for count how many src is in the file
	//for malloc
	fp = fopen(context.getGraphFile().c_str(), "r");
	if (fp != NULL) {
		while (NULL != fgets(buf, sizeof(buf), fp)) {
			p_token = strtok_r(buf, "\n", &text);
			p_token = strtok_r(buf, "\t", &text);
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
}

void Preproc::saveData(Context & context)
{
	data = new vector<pair<int, string>>[dataSize];
	dataCheck = new int[dataSize];

	for (int i = 0; i < dataSize; i++)
		dataCheck[i] = 0;

	FILE *fp;
	string label;
	char buf[512];
	char ctemp[10];
	int src, dst;
	int i;
	set<string>::iterator it_e; //for eRules

	fp = fopen(context.getGraphFile().c_str(), "r");
	if (fp != NULL) {
		while (fscanf(fp, "%d\t%d\t%s\n", &src, &dst, ctemp) != EOF) {
			label += ctemp;
			data[src].push_back(std::make_pair(dst, label));
			dataCheck[dst] = 1;
			dataCheck[src] = 1;
			count++;
			label = "";
		}
		fclose(fp);
	}
	else {
		assert(false, "cannot open graph_file");
	}
	cout << "count " << count << endl;
	//add E-rules
	for (i = 0; i < dataSize; i++) {
		if (data[i].size() == 0 && dataCheck[i] == 0)
			continue;
		for (it_e = eRules.begin(); it_e != eRules.end(); it_e++) {
			label = *it_e;
			data[i].push_back(std::make_pair(i, label));
		}
		std::sort(data[i].begin(), data[i].end(), compareV);
		data[i].erase(unique(data[i].begin(), data[i].end()), data[i].end());
		numVertex++;
	}
}

void Preproc::makeVIT(Context &context) {

	clock_t begin, end;
	int src, dst;
	unsigned long long int mSize = 0;
	int size;
	int i = 0, j = 0;
	int startS = -1, endS = 0;
	int sum = 0;
	count = 0;
	vector<pair<vertexid_t, vertexid_t>> &tempVIT = context.vit.getVIT();
	vector<int> &vitDegree = context.vit.getDegree();

	//read file and save on the memory
	//add edges according to E-rules and count srcs.
	for (i = 0; i < dataSize; i++)
		count += data[i].size();

	cout << "count =" << count << endl;
	//calculate mSize following the bytes.
	mSize = numVertex * 8 + count * 5;
	cout << "numVer =" << numVertex << endl;
	vitSize = 0;
	startS = -1;
	size = context.getNumPartitions();

	cout << "size =" << size << endl;
	if (mSize / (unsigned long long int)size > context.getMemBudget() / (unsigned long long int)4) {
		size = mSize / (context.getMemBudget() / (unsigned long long int) 4);
		size++;
	}
	cout << "size =" << size << endl;

	//mSize is limit size per parititons.
	mSize /= (unsigned long long int)size;
	vitSize = 0;

	startS = -1;
	for (i = 0; i < dataSize; i++) {
		if (data[i].size() == 0)
			continue;
		if (startS == -1) {
			startS = i;
			endS = i;
		}
		sum += data[i].size() * 5;
		sum += 8;
		//if size is last one then we put data over the limit
		//because it exceeds just around 1kb.
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
	j = 0;
	int mSum = 0;

	//this is for remember the each size of partition for get DDM.
	for (i = 0; i < dataSize; i++) {
		if (data[i].size() == 0)
			continue;
		sum += data[i].size();
		mSum += data[i].size() * 5;
		mSum += 8;
		if (mSum > mSize && j != size - 1) {
			if (i != dataSize - 1)
				sum -= data[i].size();
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
	//make vit file
	context.vit.writeToFile(context.getGraphFile() + ".vit");
}

void Preproc::makePart(Context &context) {
	FILE *f;
	string str;
	string name;
	int start = 0;

	//make partition files
	for (int i = 0; i < vitSize; i++) {
		//if (f != NULL) {
			for (int j = start; j <= context.vit.getEnd(i); j++) {
				if (data[j].size() != 0) {
					//fprintf(f, "%d\t%d\t", j, data[j].size());
					for (int k = 0; k < data[j].size(); k++) {
						for (int l = 1; l < mapInfo.size(); l++) {
							if (strcmp(data[j][k].second.c_str(), mapInfo[l].c_str()) == 0) {
								//fprintf(f, "%d\t%d\t", data[j][k].first, l);
								break;
							}
						}
					}
            //fprintf(f, "\n");
				}
			}
			start = context.vit.getEnd(i) + 1;
	}
}

void Preproc::makeBinaryPart(Context &context) {
	FILE *f;
	string str;
	string name;
	int start = 0;
	int degree, dst;
	char label;
	//make partition files in binary files
	for (int i = 0; i < vitSize; i++) {
		str = std::to_string((long long)i);
		name = context.getGraphFile() + "." + PART + "." + BINA + "." + str.c_str();

		f = fopen(name.c_str(), "ab");
		if (f != NULL) {
			for (int j = start; j <= context.vit.getEnd(i); j++) {
				if (data[j].size() != 0) {
					fwrite((const void*)& j, sizeof(int), 1, f);
					degree = data[j].size();
					fwrite((const void*)&degree, sizeof(int), 1, f);
					for (int k = 0; k < data[j].size(); k++) {
						for (int l = 0; l < mapInfo.size(); l++) {
							if (strcmp(data[j][k].second.c_str(), mapInfo[l].c_str()) == 0) {
								dst = data[j][k].first;
								fwrite((const void*)& dst, sizeof(int), 1, f);
								label = l;
								fwrite((const void*)& label, sizeof(char), 1, f);
								continue;
							}
						}
					}
				}
				else {
					fwrite((const void*)& j, sizeof(int), 1, f);
					degree = 0;
					fwrite((const void*)&degree, sizeof(int), 1, f);
				}
			}
			fclose(f);
			start = context.vit.getEnd(i) + 1;
		}
		else {
			assert(false, "Cannot make Binary file ");
		}
	}
}

void Preproc::makeDDM(Context & context)
{
	vector<vector<double> > &ddmMap = context.ddm.getDdmMap();
	int start = 0;

	for (int i = 0; i < vitSize; i++) {
		for (int j = start; j <= context.vit.getEnd(i); j++) {
			if (data[j].size() != 0) {
				for (int k = 0; k < data[j].size(); k++) {
					if (i != context.vit.partition(data[j][k].first) && context.vit.partition(data[j][k].first) != -1) {
						ddmMap[i][context.vit.partition(data[j][k].first)] ++;
					}
				}
			}
		}
		start = context.vit.getEnd(i) + 1;
	}

	for (int i = 0; i < ddmMap[0].size(); i++) {
		for (int j = 0; j < ddmMap[0].size(); j++) {
			ddmMap[i][j] /= (double)context.vit.getDegree(i);
		}
	}
	context.ddm.save_DDM(context.getGraphFile() + ".ddm");
}


void Preproc::setMapInfo(vector<string> mapInfo, set<char> eRules)
{
	set<char>::iterator it_e; //for eRules
	this->mapInfo.assign(mapInfo.begin(), mapInfo.end());

	for (it_e = eRules.begin(); it_e != eRules.end(); it_e++)
		this->eRules.insert(mapInfo[(int)*it_e]);
}



int Preproc::getNumOfPartitions()
{
	return vitSize;
}

Preproc::~Preproc()
{
	for (int i = 0; i < dataSize; i++)
		data[i].clear();
	delete[]data;
}

bool compareV(const pair<int, string> &lhs, const pair<int, string> &rhs)
{
	if (lhs.first == rhs.first)
		return lhs.second < rhs.second;
	return lhs.first < rhs.first;
}
