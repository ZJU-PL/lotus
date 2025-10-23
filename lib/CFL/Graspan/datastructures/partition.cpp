#include "CFL/Graspan/datastructures/partition.h"
#include "CFL/Graspan/datastructures/loader.h"

//Constructor
Partition::Partition(int id, int numVertices, int numEdges, vector<Vertex> data) // this will lead to copy of the vector
{
	this->id = id;
	this->numVertices = numVertices;
	this->numEdges = numEdges;
	this->data = data;
	this->exist = true;
}

// Getters
int Partition::getID() { return id; }
int Partition::getNumVertices()
{
	return numVertices;
}
int Partition::getNumEdges()
{
	return numEdges;
}
vector<Vertex> &Partition::getData() { return data; }

bool Partition::getExist()
{
	return exist;
}

// Setters
void Partition::setID(int id) { this->id = id; }
void Partition::setNumVertices(int numVertices)
{
	this->numVertices = numVertices;
}
void Partition::setNumEdges(int numEdges)
{
	this->numEdges = numEdges;
}
void Partition::setData(vector<Vertex> data) { this->data = data; }

void Partition::setExist(bool exist)
{
	this->exist = exist;
}

bool Partition::checkPart()
{
	vector<vertexid_t>::iterator firstA, lastA, resultA;
	vector<label_t>::iterator firstB, lastB, resultB;

	for (int i = 0; i < numVertices; i++) {
		vector<vertexid_t> &outEdges = data[i].getOutEdges();
		vector<label_t> &outEdgeValues = data[i].getOutEdgeValues();

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
			else {
				cout << "PARTITION " << id << " DUPLICATED HAPPEND";
				return false;
			}

		}
		return true;

		//++resultA;
		//++resultB;

		//outEdges.erase(resultA, lastA);
		//outEdgeValues.erase(resultB, lastB);
	}
	return true;
}

void Partition::writeToFile(Partition & part, bool readable, Context c)
{
	if (!part.exist)
		return;
	//cout << part.getID() << endl;
	//cout << "write partition file" << endl;
	//cout << part.id << endl;
	FILE *fp;
	string str;
	int src, degree, dst;
	char label;
	//cout << readable << endl;
	if (readable) {
		//cout << HUMA << endl;
		str = c.getGraphFile() + "." + PART + "." + HUMA + "." + std::to_string((long long)part.id);
		fp = fopen(str.c_str(), "w");
		//fprintf(fp, "%d\t%d\t%d\n", part.id, part.numVertices, part.numEdges);
		for (int i = 0; i < part.data.size(); i++) {
			//cout << part.data[i].getVertexID() << " " << part.data[i].getNumOutEdges() << endl;
			fprintf(fp, "%d\t%d\t", part.data[i].getVertexID(), part.data[i].getNumOutEdges());
			for (int j = 0; j < part.data[i].getNumOutEdges(); j++)
				fprintf(fp, "%d\t%d\t", part.data[i].getOutEdge(j), part.data[i].getOutEdgeValue(j));
			fprintf(fp, "\n");
		}
		fclose(fp);
	}
	else {
		//cout << BINA << endl;
		str = c.getGraphFile() + "." + PART + "." + BINA + "." + std::to_string((long long)part.id);
		fp = fopen(str.c_str(), "wb");

		for (int i = 0; i < part.data.size(); i++) {
			src = part.data[i].getVertexID();
			degree = part.data[i].getNumOutEdges();

			fwrite((const void*) & src, sizeof(int), 1, fp);
			fwrite((const void*) & degree, sizeof(int), 1, fp);
			for (int j = 0; j < part.data[i].getNumOutEdges(); j++) {
				dst = part.data[i].getOutEdge(j);
				label = part.data[i].getOutEdgeValue(j);
				fwrite((const void*) & dst, sizeof(int), 1, fp);
				fwrite((const void*) & label, sizeof(char), 1, fp);
			}
		}
		fclose(fp);
	}
}


void Partition::loadFromFile(int id, Partition &part, bool readable, Context c)
{
	Loader::loadPartition(id, part, readable, c);
}

string Partition::toString()
{
	std::stringstream output;

	output << endl << "Partition id : " << id << "  Vertices : " << numVertices << "  Edges : " << numEdges << endl;

	for (int i = 0; i < numVertices; i++)
		output << data[i].toString();

	return output.str();
}

void Partition::clear()
{
	id = -1;
	numEdges = 0;
	for (int i = 0; i < numVertices; i++)
		data[i].clearVector();
	numVertices = 0;
	data.clear();
	vector<Vertex>().swap(data);

	exist = false;
}

void Partition::calc_ddr(Context &context)	{
	if (!exist)
		return;
	vector<vector<double> > &ddmMap = context.ddm.getDdmMap();
	vector<vector<double> > &d_ddmMap = context.ddm.getD_ddmMap();
	vector<double> tempDdm; // TODO: why don't you just get the vector[id] from ddmMap? it would save the copying  later
	//cout << "ddmMap[id].size " << ddmMap[id].size() << endl;
	double count = 0, sum = 0;
	ddmMap[id].resize(ddmMap[id].size(), 0);

	for (int i = 0; i < data.size(); i++) {
		for (int j = 0; j < data[i].getNumOutEdges(); j++) {
			if (id != context.vit.partition(data[i].getOutEdge(j)) && context.vit.partition(data[i].getOutEdge(j)) != -1) {
			//	if (ddmMap[id][context.vit.partition(data[i].getOutEdge(j))] < 0)
			//		tempDdm[context.vit.partition(data[i].getOutEdge(j))] -= 1;
				//else 
				ddmMap[id][context.vit.partition(data[i].getOutEdge(j))] += 1;
			}
		}
	}

	for (int i = 0; i < ddmMap[0].size(); i++) {
		ddmMap[id][i] /= (double)numEdges;
		d_ddmMap[id][i] = ddmMap[id][i];	
	}
	
	//cout << "###BONUS =" << sum / (count * count) << "###" << endl;
//	return sum / (count * count);
	return;
}

Partition::~Partition() {
}
