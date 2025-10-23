#include "CFL/Graspan/datastructures/vit.h"

//Constructor
//ALL small methods should be inlined instead!!!

//Setters

void VIT::setDegree(int numPartition)
{
	if (numPartition > degree.size())
		for (int i = 0; i < numPartition - degree.size(); i++)
			degree.push_back(0);
}

int VIT::partition(vertexid_t vid)
{
	for (int i = 0; i < vit.size(); i++) {
		if (vid >= vit[i].first && vid <= vit[i].second)
			return i;
	}
	return -1;
}

bool VIT::writeToFile(string fname)
{
	FILE *fp;
	fp = fopen(fname.c_str(), "w");
	if (fp != NULL) {

		for (int i = 0; i < vit.size(); i++) {
			fprintf(fp, "%d\t%d\t%d\n", i, vit[i].first, vit[i].second);
		}
		fclose(fp);
	}
	else {
		return false;
	}
	return true;
}

bool VIT::loadFromFile(string fname)
{
	FILE *fp;
	vit.clear();
	int i,start,end;

	fp = fopen(fname.c_str(), "r");

	if (fp != NULL) { 
		while (fscanf(fp, "%d\t%d\t%d\n", &i, &start, &end) != EOF) {
			vit.push_back(std::make_pair(start, end));
		}
		fclose(fp);
	}else {
		return false;
	}
	return true;	
}

string VIT::toString() {
  
  std::stringstream output;
  
  for (int i = 0; i < vit.size(); i++) {
    output << i << " " <<  vit[i].first << " " <<  vit[i].second <<endl;
  }
  
  return output.str();
}

