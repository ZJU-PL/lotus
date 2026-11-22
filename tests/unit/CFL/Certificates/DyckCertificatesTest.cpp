#include "CFL/Certificates/DyckCertificates.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>
#include <random>
#include <set>

namespace {
using namespace cflcert;

// Independent bounded configuration search for tiny Dyck-labelled graphs.
// It reports unknown if the stack-depth cap is reached; only complete searches
// are used as a differential oracle.
static int bruteDyck(const Instance& g,int maxDepth=8) {
    struct C { int v; std::string st; };
    std::vector<std::vector<std::pair<int,Label>>> adj(g.n);
    for(const auto&e:g.edges)adj[e.from].push_back({e.to,e.label});
    std::queue<C> q;q.push({g.source,{}});std::set<std::string> seen;
    auto key=[](const C&c){return std::to_string(c.v)+":"+c.st;};seen.insert(key(q.front()));
    bool truncated=false;
    while(!q.empty()) {C c=std::move(q.front());q.pop();if(c.v==g.target&&c.st.empty())return 1;
        for(auto [v,l]:adj[c.v]) {C d=c;d.v=v;
            if(isOpen(l)){if((int)d.st.size()>=maxDepth){truncated=true;continue;}d.st.push_back((char)l);}
            else {Label want=l==Label::Close1?Label::Open1:Label::Open2;if(d.st.empty()||(Label)d.st.back()!=want)continue;d.st.pop_back();}
            auto k=key(d);if(seen.insert(k).second)q.push(std::move(d));
        }
    }
    return truncated?-1:0;
}
static int runImportedTest() {
    // Hand-built positive and negative examples, including epsilon and
    // mismatched bracket types.
    Instance yes;yes.n=3;yes.source=0;yes.target=2;yes.edges={{0,1,Label::Open1},{1,2,Label::Close1}};
    PositiveCertificate pc;assert(buildPositive(yes,pc));assert(verifyPositive(yes,pc));
    Instance no=yes;no.edges[1].label=Label::Close2;
    Separator nc;buildSeparator(no,nc);assert(verifyNegativeDeterministic(no,nc));assert(verifyNegativeRandomized(no,nc,17));
    auto bad=nc;bad.SS[0]^=1;assert(!verifyNegativeDeterministic(no,bad));assert(!verifyNegativeRandomized(no,bad,18,64));assert(!verifyNegativeRandomized(no,bad,18,0));

    std::mt19937 rng(7);int checked=0;
    for(int z=0;z<400;++z){Instance g;g.n=1+(rng()%4);g.source=rng()%g.n;g.target=rng()%g.n;int m=rng()%8;
        for(int i=0;i<m;++i)g.edges.push_back({(int)(rng()%g.n),(int)(rng()%g.n),(Label)(rng()%4)});
        bool p=buildPositive(g,pc);Separator s;buildSeparator(g,s);bool d=verifyNegativeDeterministic(g,s);assert(p!=d);
        assert(verifyPositive(g,pc)==p);
        int brute=bruteDyck(g,8);if(brute>=0){assert((int)p==brute);++checked;}
    }
    assert(checked>100);
    std::cout<<"Dyck-2 certificate tests passed; independent BFS checked "<<checked<<" generated graphs\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, DyckCertificates) {
    EXPECT_EQ(runImportedTest(), 0);
}
