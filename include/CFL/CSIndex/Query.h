#ifndef _QUERY_H_
#define _QUERY_H_

#include "AbstractQuery.h"
#include "GraphUtil.h"

#define COMPACTVECTOR

using namespace std;

class Query : public AbstractQuery {
public:
    vector<vector<pair<int, int> > > graillabels; // multiple random labeling, label[gsize][num_intervals]
    int grail_dim = 5;
protected:
    Graph &g;
    Graph gategraph;
    bit_vector *gates;
    map<int, int> gatemap; // map node id in original graph to gate graph
    vector<vector<int> > labels, lin, lout;
    string filestemstr;
    int radius, gsize, gatesize, gateedgesize, labeltype, indextype, dim, num_bits, epsilon, visitnum;
    int ref, QueryCnt;
    double preselectratio;

    string method_name;
    bool useLocalGates, usePartialLocalGates, useMultiLabels, useTopoOrder, useGlobalMultiLabels;
    // improvement options
    vector<int> topoid, que, dist, visited;
    vector<vector<vector<int> > > localgates;

    // for materialization
    bool ismaterialized;
    vector<vector<vector<int> > > inoutgates;
    vector<bit_vector *> inneigs;
    bit_vector *materialized;

    // for stat computing time
    int reachtime;

public:
    Query();

    Query(const char *grafile);

    Query(const char *filestem, const char *grafile, int _r);

    Query(const char *filestem, const char *grafile, int _r, double _ps, bool mat);

    Query(const char *filestem, Graph &ig, int _r, double _ps, bool mat);

    Query(const char *gatefile, const char *ggfile, const char *indexfile, const char *grafile);

    virtual ~Query();

    void initFlags();

    void initQueue();

    void initGateGraph(const char *gategraphfile);

    void initGates(const char *gatefile);

    virtual void initIndex(const char *indexfile);

    void outIndex(const char *out_index_file);

    void setMethodName(string _method_name);

    string getMethodName() const;

    string getFilestem() const;

    int getGateSize() const;

    int getGateEdgeSize();

    virtual long getIndexSize() const;

    virtual vector<long> indexSize() const;

    void setRadius(int);

    vector<string> makeggfilename(
            const char *filestem); // generated gate graph filename based on radius and preselectratio ***.<radius><preselect*1000>gg

    // methods for query
    bool reach(int src, int trg) override;

    virtual bool reachWithoutMat(int src, int trg);

    virtual bool test_reach(int src, int trg);

    virtual bool test_nomreach(int src, int trg);

    // options for improvement
    virtual void computeLocalGates(bool ispartial); // precollecting local gates
    virtual void computeMultiLabelsQuickRej(int num_labels); // precompute multiple random DFS labeling

    void displayIndex(ostream &out);

    void displayGates(ostream &out);

    virtual void displayLabelsByNode(int vid, ostream &out);

    virtual void displayLabels(ostream &out);

    virtual void displayGrailLabels(ostream &out);

    virtual void displayLocalGates(ostream &out);

    virtual void displayLocalGatesByNode(int vid, ostream &out);

    // methods for inheritance
    virtual void selectPartialNodes(vector<int> &nodes, int num); // the method will be called by computeLocalGates()
    virtual void mutipleLabeling(int num_labels); // the method will be called by computeMultiLabelsQuickRej();
    void computeTopoOrder();

    virtual void initMaterialization();

    virtual void materialization(int num);

    virtual void selectMaterialized(int num);

    virtual void materializeInNeighbors(int vid);

    virtual void precomputeGates(int vid, bool out);

    virtual void displayInfor(ostream &out);

    virtual void displayInfor(int vid, ostream &out);

public:
    const char *method() const override {
        return "PathTree";
    }

    void reset() override {}
};

#endif

