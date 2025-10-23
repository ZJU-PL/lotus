#ifndef PARTITION_H
#define PARTITION_H

#include <sstream>

#include "CFL/Graspan/datastructures/vertex.h"
// #include "CFL/Graspan/datastructures/vit.h"
#include "CFL/Graspan/datastructures/context.h"


class Partition
{
private:
	partitionid_t id;
	int numVertices;
	int numEdges;

	vector<Vertex> data;
	//for check partition exists. it is for repartition part ???? the fact you have obj means it exists already
	bool exist;
		
public:
	//Constructors
	Partition() { this->exist = true; };
	Partition(int id, int numVertices, int numEdges, vector<Vertex> data);

  // Again, all small methods should be inlined
	//Getters
	int getID();
	int getNumVertices();
	int getNumEdges();
	vector<Vertex> &getData();
	bool getExist();

	//Setters
	void setID(int id);
	void setNumVertices(int numVertices);
	void setNumEdges(int numEdges);
	void setData(vector<Vertex> data);
	void setExist(bool exist);

	bool checkPart();

	//if user want save the partition to the disk use this function
	//readable == true then text format, readable == false binary format
	static void writeToFile(Partition& part, bool readable, Context c);

	//read the partition from the harddisk
	static void loadFromFile(int id, Partition &part, bool readable, Context c);

	string toString();
	void clear();

	// ddr = destination distribution ratio, i.e., a cell in DDM return BONUS
	void calc_ddr(Context &context);
	~Partition();
};


#endif
