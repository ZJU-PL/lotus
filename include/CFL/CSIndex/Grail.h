/* Copyright (c) Hilmi Yildirim 2010,2011.

The software is provided on an as is basis for research purposes.
There is no additional support offered, nor are the author(s) 
or their institutions liable under any circumstances.
*/
#ifndef _BOX_H
#define _BOX_H

#include "AbstractQuery.h"
#include "ExceptionList.h"
#include "GraphUtil.h"

// test switch
#define _TEST_

class Grail : public AbstractQuery {
	public:
		Graph& g;
		struct timeval after_time, before_time;
		float run_time;
		int dim;
		int *visited;
		int QueryCnt;
		bool LEVEL_FILTER;
		bool POOL;
		int POOLSIZE;
		unsigned int PositiveCut, NegativeCut, TotalCall, TotalDepth, CurrentDepth;
	public:
		Grail(Graph& graph, int dim, int labelingType, bool POOL, int POOLSIZE);
		~Grail();
		static int visit(Graph& tree, int vid, int& pre_post, vector<bool>& visited);
		static int fixedreversevisit(Graph& tree, int vid, int& pre_post, vector<bool>& visited,int traversal);
		static int customvisit(Graph& tree, int vid, int& pre_post, vector<bool>& visited, int traversal);
		static void randomlabeling(Graph& tree);
		static void customlabeling(Graph& tree, int traversal);
		static void fixedreverselabeling(Graph& tree, int traversal);
		static void setIndex(Graph& tree, int traversal); 
		static void setCustomIndex(Graph& tree, int traversal, int type); 

		void set_level_filter(bool lf);
		//bool reach(int src, int trg, ExceptionList * el = nullptr);
		bool reach_lf(int src, int trg, ExceptionList * el);
		bool bidirectionalReach(int src, int trg, ExceptionList * el);
		bool bidirectionalReach_lf(int src, int trg, ExceptionList * el);
		bool reachPP(int src, int trg, ExceptionList * el);
		bool reachPP_lf(int src, int trg, ExceptionList * el);
		bool bidirectionalReachPP(int src, int trg, ExceptionList * el);
		bool bidirectionalReachPP_lf(int src, int trg, ExceptionList * el);
		bool go_for_reach(int src, int trg);
		bool go_for_reach_lf(int src, int trg);
		bool go_for_reachPP(int src, int trg);
		bool go_for_reachPP_lf(int src, int trg);
		bool contains(int src, int trg);
		int containsPP(int src, int trg);

public:
    bool reach(int src, int dst) override {
        return reachPP_lf(src, dst, nullptr);
    }

    const char *method() const override {
        return "Grail";
    }

    void reset() override {}
};

#endif
