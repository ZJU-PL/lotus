#include "CFL/Graspan/datastructures/loader.h"


//TODO: this method is too long, make it shorter by refactoring
bool Loader::loadPartition(partitionid_t id, Partition &p, bool readable, Context c)
{
	static char s[32 + 1] = { '0', };
	vector<Vertex>& data=p.getData();
	data.clear();

	FILE *fp;
	char label;
	int size = 0;
	int numOfPartition;
	int src, degree, dst;
	int numVertices = 0;
	int i = 0;
	int temp;
	string str;
	char *ctemp[2];
	char buf[4096];
	char *bbuf;
	char *p_token = NULL;
	char *context = NULL;
	if (readable) {
		str = c.getGraphFile() + "." + PART + "." + HUMA + "." + std::to_string((long long)id);
		fp = fopen(str.c_str(), "r");
		int linecounts = 0;
		if (fp!= NULL) {
			while (NULL != fgets(buf, sizeof(buf), fp)) {
				vector<vertexid_t> outEdges;
				vector<label_t> outEdgeValues;

				p_token = strtok_r(buf, "\n", &context);
				if (p_token == NULL) {
					cout << "NULL ptoken linecount value = " << linecounts << " : " << str << endl;
					break;
				}
				if (!strcmp(p_token, "") || !strcmp(p_token, " ")) {//if (p_token == "" || p_token == " ")
					cout << "linecount value = " << linecounts << " : " << str <<  endl;
					break;
				}
				
				linecounts++;
				
				p_token = strtok_r(buf, "\t", &context);
				if (p_token == NULL) {
					cout << "2nd check NULL ptoken linecount value = " << linecounts << " : " << str << endl;
					break;
				}
				if (!strcmp(p_token, "") || !strcmp(p_token, " ")) {//if (p_token == "" || p_token == " ")
					cout << "2nd check linecount value = " << linecounts << " : " << str << endl;
					break;
				}

				src = atoi(p_token);

				p_token = strtok_r(NULL, "\t", &context);
				if (p_token == NULL) {
					cout << "3rd check NULL ptoken linecount value = " << linecounts << " : " << str << endl;
					break;
				}
				if (!strcmp(p_token, "") || !strcmp(p_token, " ")) {//if (p_token == "" || p_token == " ")
					cout << "3rd check linecount value = " << linecounts << " : " << str << endl;
					break;
				}
				degree = atoi(p_token);
				size += degree;
				numVertices++;

				p_token = strtok_r(NULL, "\t", &context);
				int i = 0;
				while (p_token != NULL ) {
					ctemp[i++] = p_token;
					#if DEBUG
					assert(p_token != "", "blank token" );
					assert(p_token != " ", "space token" );
					#endif
					//assert(p_token != "\s", "anyspace token" );
					// you don't have to do this right? you know which vector to push 
					// one vector has only elements whose index is odd, the other, even
					/*if (i % 2 ==  1) {
						outEdgeValues.push_back(atoi(p_token));
					}
					else {
						outEdges.push_back(atoi(p_token));
					}*/
					p_token = strtok_r(NULL, "\t", &context);
					//i++;
					if						(i == 2) {
						outEdges.push_back(atoi(ctemp[0]));
						outEdgeValues.push_back(atoi(ctemp[1]));
						//label = atoi(ctemp[1]);
						i = 0;
					}
				}
				Vertex v(src, outEdges, outEdgeValues);
				data.push_back(v);
			}
			p.setID(id);
			p.setNumEdges(size);
			p.setNumVertices(numVertices);
			fclose(fp);
			return 1;
		} else {
			#if DEGUB
			assert(false, "Cannot open human file " + id);
			#endif
		}
	}
	else {
		str = c.getGraphFile() + "." + PART + "." + BINA + "." + std::to_string((long long)id);
		fp = fopen(str.c_str(), "rb");
		if (fp != NULL) {
			// wouldnt this cause problem with hard-coded value 4 and 1???
			while (0 != fread(&src, 4, 1, fp)) {
				vector<vertexid_t> outEdges;
				vector<label_t> outEdgeValues;
				fread(&degree, 4, 1, fp);
				temp = degree * 5;
				size += degree;
				numVertices++;
				//
				bbuf = (char *)malloc(temp);
				fread(bbuf, temp, 1, fp);
				for (int i = 0; i < temp; i += 5) {

					dst = *((int*)(bbuf + i));
					label = *((char*)(bbuf + 4 + i));

					outEdges.push_back(dst);
					outEdgeValues.push_back(label);
					//	std::cout << dst << " " << (int)label << std::endl;
				}
				free(bbuf);
				/*
				temp = degree*5;
				size += degree;
				numVertices++;
				//
				for (int i = 0; i < degree; i++) {
				fread(&dst, 4, 1, fp);

				fread(&label, 1, 1, fp);

				outEdges.push_back(dst);
				outEdgeValues.push_back(label);
				}*/

				Vertex v(src, outEdges, outEdgeValues);
				data.push_back(v);
			}
			p.setID(id);
			p.setNumEdges(size);
			p.setNumVertices(numVertices);
			fclose(fp);
			return 1;
		}
		else {
			#if DEBUG
			assert(false, "cant open bin file " + id);
			#endif
		}
	}
	return 0; // this is dead code!!!
}
