#pragma once

#include "CFL/InterleavedDyck/SPDS/Solver.h"
#include "CFL/InterleavedDyck/SPDS/Synchronized.h"
#include <functional>
#include <iostream>
#include <random>
#include <sstream>

namespace lotus_spds_test {
namespace d = lotus::cfl::interleaved_dyck;
namespace s = d::spds;
inline void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Exception, class Function> void throws(Function f) {
  bool caught = false;
  try {
    f();
  } catch (const Exception &) {
    caught = true;
  } catch (const std::exception &e) {
    if (dynamic_cast<const Exception *>(&e) != nullptr ||
        std::string(typeid(e).name()) == typeid(Exception).name()) {
      caught = true;
    } else {
      throw;
    }
  }
  require(caught, "expected exception was not thrown");
}
inline d::Graph chain(const std::vector<d::Label> &word) {
  d::Graph graph;
  for (std::size_t i = 0; i <= word.size(); ++i) graph.addVertex(static_cast<d::Vertex>(i));
  for (std::size_t i = 0; i < word.size(); ++i)
    graph.addEdge(static_cast<d::Vertex>(i), static_cast<d::Vertex>(i + 1), word[i]);
  return graph;
}
inline bool step(const d::Label &l, std::vector<unsigned> &calls, std::vector<unsigned> &fields) {
  using K = d::LabelKind;
  if (l.kind == K::Neutral) return true;
  auto &stack = l.kind == K::OpenParenthesis || l.kind == K::CloseParenthesis ? calls : fields;
  if (l.kind == K::OpenParenthesis || l.kind == K::OpenBracket) stack.push_back(l.id);
  else {
    if (stack.empty() || stack.back() != l.id) return false;
    stack.pop_back();
  }
  return true;
}
inline const std::vector<d::Label> &alphabet() {
  static const std::vector<d::Label> labels{
    d::Label::neutral(), d::Label::openParenthesis(0), d::Label::closeParenthesis(0),
    d::Label::openParenthesis(7), d::Label::closeParenthesis(7),
    d::Label::openBracket(0), d::Label::closeBracket(0),
    d::Label::openBracket(7), d::Label::closeBracket(7)};
  return labels;
}
inline void basics() {
  s::Solver solver;
  d::Graph empty;
  require(solver.prepare(empty).analyzeAll().upper_bound.empty(), "empty graph");
  empty.addVertex(-13); empty.addVertex(91);
  auto empty_analysis = solver.prepare(empty);
  auto r = empty_analysis.analyzeAll();
  require(r.upper_bound.size() == 2 && r.mayReach(-13,-13) && !r.mayReach(-13,91), "isolated vertices");
  auto q = empty_analysis.queryFrom(-13);
  require(!q.mayReach(99), "unknown query endpoint");
  throws<std::invalid_argument>([&] { empty_analysis.queryFrom(99); });
  auto cross = chain({d::Label::openParenthesis(3), d::Label::openBracket(9),
                      d::Label::closeParenthesis(3), d::Label::closeBracket(9)});
  auto cross_analysis = solver.prepare(cross);
  auto forward = cross_analysis.queryFrom(0);
  require(forward.mayReach(4) && !forward.mayReach(3), "crossing balanced word");
  require(!cross_analysis.queryFrom(4).mayReach(0), "directed arcs preserved");
  require(forward.mayAccept(2, {3}, {9}), "nonempty stack membership");
  require(!forward.mayAccept(2, {9}, {3}), "typed stack membership");
  auto backward = cross_analysis.queryTo(4);
  require(backward.mayReach(0), "backward crossing");
  require(backward.mayAccept(2, {3}, {9}), "pre* nonempty predecessor stacks");
  auto underflow = solver.prepare(chain({d::Label::closeBracket(2)}));
  require(!underflow.queryFrom(0).mayReach(1), "no underflow");
}
inline void extremesAndNeutral() {
  d::Graph graph;
  const auto lo = std::numeric_limits<d::Vertex>::min();
  const auto hi = std::numeric_limits<d::Vertex>::max();
  const unsigned max_id = std::numeric_limits<unsigned>::max();
  graph.addEdge(lo,-1,d::Label::neutral()); graph.addEdge(-1,0,d::Label::openParenthesis(max_id));
  graph.addEdge(0,1,d::Label::neutral()); graph.addEdge(1,hi,d::Label::closeParenthesis(max_id));
  require(s::Solver().prepare(graph).queryFrom(lo).mayReach(hi), "signed vertices/maximum label id");
  graph.addEdge(1,0,d::Label::neutral()); graph.addEdge(hi,hi,d::Label::neutral());
  require(s::Solver().prepare(graph).queryTo(hi).mayReach(lo), "epsilon cycles");
  graph.addEdge(1,hi,d::Label::closeParenthesis(0));
  require(s::Solver().prepare(graph).queryFrom(lo).mayReach(hi), "parallel label alternatives");
  d::Graph malformed;
  malformed.addEdge(0,1,{static_cast<d::LabelKind>(999),0});
  throws<std::invalid_argument>([&]{(void)s::Solver().prepare(malformed);});
}
inline void exhaustiveWords() {
  std::size_t cases = 0;
  std::vector<d::Label> word;
  std::function<void(unsigned)> enumerate = [&](unsigned remaining) {
    auto g = chain(word);
    auto r = s::Solver().prepare(g).queryFrom(0);
    std::vector<unsigned> calls, fields;
    bool feasible = true;
    for (const auto &l : word) if (feasible) feasible = step(l,calls,fields);
    const auto end = static_cast<d::Vertex>(word.size());
    require(r.mayReach(end) == (feasible && calls.empty() && fields.empty()), "word oracle/empty");
    std::reverse(calls.begin(), calls.end()); std::reverse(fields.begin(), fields.end());
    if (feasible) require(r.mayAccept(end,calls,fields), "word oracle/concrete endpoint stacks");
    s::Options prefix;
    prefix.parentheses = s::StackAcceptance::Any; prefix.brackets = s::StackAcceptance::Any;
    require(s::Solver(prefix).prepare(g).queryFrom(0).mayReach(end) == feasible,
            "word oracle/prefix");
    ++cases;
    if (remaining) for (auto l : alphabet()) {
      word.push_back(l); enumerate(remaining - 1); word.pop_back();
    }
  };
  enumerate(4);
  require(cases == 7381, "exhaustive word count");
}
// Independent exact one-stack CFL closure, not PDS saturation or a depth bound.
inline d::PairSet referenceProjection(const d::Graph &g, bool calls) {
  d::PairSet relation;
  for (auto v : g.vertices()) relation.insert({v,v});
  const auto open = calls ? d::LabelKind::OpenParenthesis : d::LabelKind::OpenBracket;
  const auto close = calls ? d::LabelKind::CloseParenthesis : d::LabelKind::CloseBracket;
  for (const auto &e : g.edges()) if (e.label.kind != open && e.label.kind != close)
    relation.insert({e.source,e.target});
  bool changed;
  do {
    changed = false;
    for (auto a : g.vertices()) for (auto b : g.vertices()) for (auto c : g.vertices())
      if (relation.count({a,b}) && relation.count({b,c}))
        changed |= relation.insert({a,c}).second;
    for (const auto &left : g.edges()) if (left.label.kind == open)
      for (const auto &right : g.edges())
        if (right.label.kind == close && left.label.id == right.label.id &&
            relation.count({left.target,right.source}))
          changed |= relation.insert({left.source,right.target}).second;
  } while(changed);
  return relation;
}
inline void projectionOracle() {
  std::mt19937 random(912341);
  for (int trial = 0; trial < 250; ++trial) {
    d::Graph g;
    const int n = 1 + static_cast<int>(random()%6);
    for (int v = 0; v < n; ++v) g.addVertex(v);
    for (int a = 0; a < n; ++a) for (int b = 0; b < n; ++b)
      for (unsigned k = 0; k < 2; ++k) if (random()%5 == 0)
        g.addEdge(a,b,alphabet()[random()%alphabet().size()]);
    const auto calls = referenceProjection(g,true), fields = referenceProjection(g,false);
    auto analysis = s::Solver().prepare(g);
    auto result = analysis.analyzeAll();
    require(result.parenthesis_pairs == calls, "exact call-CFL oracle");
    require(result.bracket_pairs == fields, "exact field-CFL oracle");
    for (int target = 0; target < n; ++target) {
      auto back = analysis.queryTo(target);
      for (int source = 0; source < n; ++source) {
        bool expected = calls.count({source,target}) && fields.count({source,target});
        require(result.mayReach(source,target) == expected, "SPDS conjunction oracle");
        require(back.mayReach(source) == expected, "post*/pre* equivalence");
      }
    }
  }
}
inline d::PairSet concreteDag(const d::Graph &g) {
  d::PairSet result;
  for (auto source : g.vertices()) {
    std::function<void(d::Vertex,std::vector<unsigned>,std::vector<unsigned>)> dfs;
    dfs = [&](d::Vertex node, std::vector<unsigned> calls, std::vector<unsigned> fields) {
      if (calls.empty() && fields.empty()) result.insert({source,node});
      for (const auto &e : g.edges()) if (e.source == node) {
        require(e.target > e.source, "DAG oracle used on cyclic graph");
        auto a=calls,b=fields;
        if (step(e.label,a,b)) dfs(e.target,a,b);
      }
    };
    dfs(source,{},{});
  }
  return result;
}
inline void soundnessDag() {
  std::mt19937 random(73841);
  for (int trial=0;trial<200;++trial) {
    d::Graph g;
    for(int v=0;v<7;++v) g.addVertex(v);
    for(int a=0;a<7;++a) for(int b=a+1;b<7;++b)
      for(int k=0;k<2;++k) if(random()%3==0)
        g.addEdge(a,b,alphabet()[random()%alphabet().size()]);
    auto exact=concreteDag(g), upper=s::Solver().prepare(g).analyzeAll().upper_bound;
    for(const auto &pair:exact) require(upper.count(pair), "two-stack DAG soundness");
  }
}
inline void differentPathFalsePositive() {
  // Route A is call-balanced but has a field mismatch; route B vice versa.
  d::Graph g;
  const std::vector<std::vector<d::Label>> routes{
    {d::Label::openParenthesis(1),d::Label::openBracket(1),d::Label::closeParenthesis(1),d::Label::closeBracket(2)},
    {d::Label::openParenthesis(2),d::Label::openBracket(2),d::Label::closeParenthesis(1),d::Label::closeBracket(2)}};
  for(int branch=0;branch<2;++branch) {
    d::Vertex prev=0;
    for(int j=0;j<4;++j) {
      d::Vertex next=j==3?8:1+branch*3+j;
      g.addEdge(prev,next,routes[branch][j]); prev=next;
    }
  }
  require(!concreteDag(g).count({0,8}), "false-positive fixture has no concrete path");
  require(s::Solver().prepare(g).queryFrom(0).mayReach(8),
          "SPDS must retain different-path approximation");
}
inline void paperFigure7() {
  // Follow the DRAWN labels of Figure 7 (p.15); the following paragraph has
  // an inconsistent h/f spelling. Outstanding call 62 is intentional.
  d::Graph g;
  g.addEdge(0,1,d::Label::openParenthesis(62)); // a -> u
  g.addEdge(1,2,d::Label::openBracket(1));      // u -> v: h
  g.addEdge(2,5,d::Label::openParenthesis(53));// v -> p
  g.addEdge(0,3,d::Label::openParenthesis(63));// a -> r
  g.addEdge(3,4,d::Label::openBracket(2));     // r -> s: f
  g.addEdge(4,5,d::Label::openParenthesis(67));// s -> p
  g.addEdge(5,6,d::Label::openBracket(3));     // p -> q: g
  g.addEdge(6,7,d::Label::closeParenthesis(53));// q -> w
  g.addEdge(7,8,d::Label::closeBracket(3));
  g.addEdge(8,9,d::Label::closeBracket(2));
  s::Options options; options.parentheses=s::StackAcceptance::Any;
  auto r=s::Solver(options).prepare(g).queryFrom(0);
  require(r.mayReach(9), "Figure 7 approximation with pending outer call");
  require(!s::Solver().prepare(g).queryFrom(0).mayReach(9),
          "Figure 7 not an empty-call-stack query");
  // The same phenomenon with the paper's V / (V x S) control encodings.
  s::SynchronizedSystem<> flows;
  flows.addCall({0,61},{1,51},62); flows.addStore({1,51},{2,52},1);
  flows.addCall({2,52},{5,57},53);
  flows.addCall({0,61},{3,65},63); flows.addStore({3,65},{4,66},2);
  flows.addCall({4,66},{5,57},67);
  flows.addStore({5,57},{6,58},3); flows.addNormal({6,58},{6,59});
  flows.addReturn({6,59},{7,53}); flows.addLoad({7,53},{8,54},3);
  flows.addLoad({8,54},{9,55},2);
  auto synchronized=flows.postStar({{0,61},{},{}});
  require(synchronized.mayAccept({{9,55},{62},{}}),"Figure 7 explicit synchronized configuration");
  require(!synchronized.mayAccept({{9,55},{},{}}),"Figure 7 synchronized pending context required");
}
inline void recursion() {
  d::Graph g;
  g.addEdge(0,0,d::Label::openParenthesis(3));
  g.addEdge(0,0,d::Label::openBracket(4));
  g.addEdge(0,1,d::Label::neutral());
  g.addEdge(1,1,d::Label::closeParenthesis(3));
  g.addEdge(1,1,d::Label::closeBracket(4));
  auto q=s::Solver().prepare(g).queryFrom(0);
  require(q.mayReach(1), "recursive balanced reachability");
  require(q.mayAccept(0,std::vector<unsigned>(250,3),std::vector<unsigned>(250,4)), "unbounded regular stacks");
  require(q.callAutomaton().states()<20 && q.fieldAutomaton().states()<20, "finite automata for infinite stacks");
}
inline void orderIndependence() {
  std::mt19937 rng(743); d::Graph g;
  for(int i=0;i<7;++i) g.addVertex(i);
  for(int i=0;i<28;++i) g.addEdge(rng()%7,rng()%7,alphabet()[rng()%9]);
  auto expected=s::Solver().prepare(g).analyzeAll().upper_bound;
  auto edges=g.edges();
  for(int trial=0;trial<20;++trial) {
    std::shuffle(edges.begin(),edges.end(),rng); d::Graph other;
    for(int i=6;i>=0;--i) other.addVertex(i);
    for(const auto&e:edges) {other.addEdge(e.source,e.target,e.label);other.addEdge(e.source,e.target,e.label);}
    require(s::Solver().prepare(other).analyzeAll().upper_bound==expected,
            "edge order/duplicate independence");
  }
}
inline void preparedDemands() {
  auto g=chain({d::Label::openParenthesis(1),d::Label::openBracket(2),
                d::Label::closeParenthesis(1),d::Label::closeBracket(2)});
  for(d::Vertex v=10;v<17;++v)g.addVertex(v);
  auto analysis=s::Solver().prepare(g);
  require(analysis.analyzeFrom(0).mayReach(0,4),"bulk source readout");
  require(analysis.analyzeTo(4).mayReach(0,4),"bulk target readout");
  std::vector<d::Pair> demands{{0,4},{1,4},{0,4}};
  for(d::Vertex v=10;v<17;++v)demands.push_back({v,4});
  auto automatic=analysis.analyzeDemands(demands);
  require(automatic.upper_bound==d::PairSet{{0,4}},"automatic demand result");
  require(automatic.statistics.rules==8,"batch groups by the single target");
  auto post=analysis.analyzeDemands(demands,s::DemandDirection::Post);
  require(post.upper_bound==automatic.upper_bound,"post demand result");
  require(post.statistics.rules>automatic.statistics.rules,
          "forced post uses more sliced anchors");
  throws<std::invalid_argument>([&]{
    (void)analysis.analyzeDemands({{0,99}});
  });
}
inline void wildcardRules() {
  s::PushdownSystem<> p;
  const auto a=p.addControl(),b=p.addControl(),c=p.addControl();
  p.addPreserveRule(a,b);
  p.addPushRule(b,c,9);
  auto seed=s::RegularSet::singleton(p.controls(),{a,{2,0}});
  auto post=s::postStar(p,seed);
  require(post.accepts(b,{2,0}) && post.accepts(c,{9,2,0}),
          "wildcard post preserve/push");
  auto target=s::RegularSet::singleton(p.controls(),{c,{9,2,0}});
  auto pre=s::preStar(p,target);
  require(pre.accepts(b,{2,0}) && pre.accepts(a,{2,0}),
          "wildcard pre preserve/push");
}
inline void regularSets() {
  s::PushdownSystem<> p; p.addControl(); p.addControl();
  p.addRule(1,2,1,{3});
  s::RegularSet seed(2); auto f=seed.addState(); seed.addFinal(f);
  seed.addTransition(0,1,1); seed.addTransition(1,2,f);
  auto a=s::postStar(p,seed);
  require(a.accepts(0,{1,2}) && a.accepts(1,{3}),"regular seed union");
  require(!a.accepts(0,{1,3}),"normalization protects incoming PDS controls");
  s::RegularSet cyclic(2); cyclic.addFinal(0); cyclic.addTransition(0,1,0);
  cyclic.addTransition(0,s::Epsilon,0);
  p.addRule(0,1,1,{2}); p.addRule(1,2,1,{2,2});
  auto b=s::postStar(p,cyclic);
  std::vector<s::Symbol> word(120,2); word.insert(word.end(),80,1);
  require(b.accepts(1,word) && b.accepts(0,{}),"infinite regular seed and accepting control");
  require(!b.accepts(0,{2}),"seed normalization no pollution");
  auto c=s::preStar(p,seed);
  require(c.accepts(0,{1,2}) && c.accepts(1,{2}),"pre* regular target");
}
inline void emptyStacks() {
  s::PushdownSystem<> p; p.addControl();p.addControl();p.addControl();
  p.addRule(0,1,1,{}); p.addRule(1,2,2,{3});
  auto a=s::postStar(p,s::RegularSet::singleton(3,{0,{1}}));
  require(a.accepts(1,{})&&!a.acceptsPrefix(2,{}),"genuinely empty PDS stack cannot match a top");
  auto b=s::preStar(p,s::RegularSet::singleton(3,{1,{}}));
  require(b.accepts(0,{1})&&!b.accepts(0,{1,2}),"pre* pop to empty");
  auto c=s::postStar(p,s::RegularSet::singleton(3,{0,{}}));
  require(c.accepts(0,{})&&!c.acceptsPrefix(1,{}),"empty source stack");
}
inline void semiringLaws() {
  s::RelationSemiring d(70);
  auto a=d.transition(0,65), b=d.transition(65,69);
  require(d.extend(a,b).contains(0,69),"relation crosses machine-word boundary");
  require(d.extend(b,a)==d.zero(),"noncommutative relation composition");
  require(d.combine(a,a)==a && d.extend(d.one(),a)==a && d.extend(a,d.one())==a,"semiring units/idempotence");
  require(d.extend(a,d.combine(b,d.one()))==d.combine(d.extend(a,b),a),"semiring distributivity");
  require(d.extend(d.zero(),a)==d.zero(),"annihilating zero");
  auto joined=a;
  require(d.combineWith(joined,b)&&joined==d.combine(a,b),"mutating combine changes");
  require(!d.combineWith(joined,a),"mutating combine reports no change");
  throws<std::invalid_argument>([&]{d.combine(a,s::RelationSemiring(2).one());});
}
using CConfig=std::pair<s::State,std::vector<s::Symbol>>;
template<class Domain>
std::map<CConfig,typename Domain::Weight> enumerateDagPds(
    const s::PushdownSystem<Domain> &p, const CConfig &source) {
  using W=typename Domain::Weight;
  const auto &d=p.domain();
  std::map<CConfig,W> result; std::deque<CConfig> queue;
  result.emplace(source,d.one());queue.push_back(source);
  while(!queue.empty()) {
    auto c=queue.front();queue.pop_front(); auto value=result.at(c);
    for(const auto &r:p.rules()) {
      require(r.from<r.to,"acyclic PDS oracle used on cyclic controls");
      if(r.from!=c.first || c.second.empty() || r.top!=c.second.front()) continue;
      auto word=r.replacement;word.insert(word.end(),c.second.begin()+1,c.second.end());
      CConfig next{r.to,word}; W w=d.extend(value,r.weight);
      if(w==d.zero()) continue;
      auto it=result.find(next); W old=it==result.end()?d.zero():it->second;
      W joined=d.combine(old,w);
      if(joined!=old) {result.insert_or_assign(next,joined);queue.push_back(next);}
    }
  }
  return result;
}
inline std::vector<std::vector<s::Symbol>> binaryWords(unsigned depth) {
  std::vector<std::vector<s::Symbol>> result;
  std::vector<s::Symbol> word;
  std::function<void(unsigned)> visit=[&](unsigned left){
    result.push_back(word);
    if(left) for(s::Symbol a:{0,1}) {word.push_back(a);visit(left-1);word.pop_back();}
  };
  visit(depth);return result;
}
inline void weightedDagOracle() {
  s::RelationSemiring d(3); std::mt19937 rng(483728);
  auto words=binaryWords(6); // Complete: 2 seed symbols + <=3 pushes = <=5.
  for(int trial=0;trial<60;++trial) {
    s::PushdownSystem<s::RelationSemiring> p(d);
    for(int i=0;i<4;++i)p.addControl();
    for(s::State from=0;from<4;++from)for(s::State to=from+1;to<4;++to)
      for(int choice=0;choice<5;++choice) {
        auto w=d.zero();
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)if(rng()%4==0)w.insert(i,j);
        std::vector<s::Symbol> rhs;
        unsigned length=rng()%3;
        for(unsigned i=0;i<length;++i)rhs.push_back(rng()%2);
        p.addRule(from,rng()%2,to,rhs,w);
      }
    CConfig source{0,{rng()%2,rng()%2}}, target{3,{rng()%2,rng()%2}};
    auto expected=enumerateDagPds(p,source);
    auto post=s::postStar(p,s::RegularSet::singleton(4,{source.first,source.second}));
    auto pre=s::preStar(p,s::RegularSet::singleton(4,{target.first,target.second}));
    std::map<CConfig,s::RelationWeight> expected_pre;
    for(s::State control=0;control<4;++control)for(const auto &word:words) {
      auto found=expected.find({control,word});
      auto weight=found==expected.end()?d.zero():found->second;
      require(post.weight(control,word)==weight,"weighted post* versus complete DAG execution");
      auto reachable=enumerateDagPds(p,CConfig{control,word});
      auto t=reachable.find(target);auto back=t==reachable.end()?d.zero():t->second;
      require(pre.weight(control,word)==back,"weighted pre* versus complete DAG execution");
      if(back!=d.zero())expected_pre.emplace(CConfig{control,word},back);
    }
    for(s::State control=0;control<4;++control)
      for(const auto &prefix:binaryWords(2)) {
        auto aggregate=[&](const auto &configurations){
          auto weight=d.zero();
          for(const auto &entry:configurations)
            if(entry.first.first==control && entry.first.second.size()>=prefix.size() &&
               std::equal(prefix.begin(),prefix.end(),entry.first.second.begin()))
              weight=d.combine(weight,entry.second);
          return weight;
        };
        require(post.weightWithPrefix(control,prefix)==aggregate(expected),"weighted existential post* stack");
        require(pre.weightWithPrefix(control,prefix)==aggregate(expected_pre),"weighted existential pre* stack");
      }
  }
}
inline void weightedSharedPush() {
  s::RelationSemiring d(4);s::PushdownSystem<s::RelationSemiring> p(d);
  for(int i=0;i<4;++i)p.addControl();
  p.addRule(0,0,1,{1,2},d.transition(0,1));
  p.addRule(0,0,1,{1,3},d.transition(2,3)); // SAME generated (control,first symbol)
  p.addRule(1,1,2,{4},d.one());
  p.addRule(2,4,3,{},d.one());
  auto a=s::postStar(p,s::RegularSet::singleton(4,{0,{0}}));
  require(a.weight(3,{2})==d.transition(0,1),"shared push continuation A weight");
  require(a.weight(3,{3})==d.transition(2,3),"shared push continuation B weight");
  require(!a.weight(3,{2}).contains(2,3),"no weight leakage through generated state");
}
inline void weightedCycles() {
  s::RelationSemiring d(3); s::PushdownSystem<s::RelationSemiring> p(d); p.addControl();
  auto step=d.combine(d.transition(0,1),d.transition(1,2));
  p.addRule(0,1,0,{1,1},step);
  auto a=s::postStar(p,s::RegularSet::singleton(1,{0,{1}}));
  auto power=d.one();
  for(unsigned depth=1;depth<=8;++depth) {
    require(a.weight(0,std::vector<s::Symbol>(depth,1))==power,"weighted push cycle respects length and order");
    power=d.extend(power,step);
  }
  auto star=d.combine(d.one(),d.combine(step,d.extend(step,step)));
  require(a.weightWithPrefix(0,{})==star,"weighted infinite-stack language aggregate");
  p.addRule(0,1,0,{},d.one());
  auto b=s::postStar(p,s::RegularSet::singleton(1,{0,{1}}));
  require(b.weight(0,{1})==star && b.weight(0,{})==star,"weighted epsilon/pop cycles");
}
inline void incremental() {
  s::RelationSemiring d(3);
  for(auto direction:{s::Direction::Post,s::Direction::Pre}) {
    s::PushdownSystem<s::RelationSemiring> p(d);
    for(int i=0;i<4;++i)p.addControl();
    p.addRule(0,0,1,{1},d.one());p.addRule(1,1,2,{2},d.transition(0,1));
    p.addRule(2,2,3,{3},d.transition(2,0));
    auto seed=s::RegularSet::singleton(4,direction==s::Direction::Post?s::Configuration{0,{0}}:s::Configuration{3,{3}});
    s::SaturationSession<s::RelationSemiring> session(p,seed,direction);
    throws<std::logic_error>([&]{(void)session.result();});session.run();
    session.addRule(1,1,2,{2},d.transition(1,2));
    p.addRule(1,1,2,{2},d.transition(1,2));
    throws<std::logic_error>([&]{(void)session.result();});session.run();
    session.addRule(0,0,1,{1,0},d.one());p.addRule(0,0,1,{1,0},d.one());
    auto &actual=session.run();
    auto expected=direction==s::Direction::Post?s::postStar(p,seed):s::preStar(p,seed);
    for(s::State c=0;c<4;++c)for(const auto &word:std::vector<std::vector<s::Symbol>>{{},{0},{1},{2},{3},{3,0},{1,0},{2,0}})
      require(actual.weight(c,word)==expected.weight(c,word),"incremental rule/weight promotion equals fresh saturation");
    if(direction==s::Direction::Post)
      require(actual.weight(3,{3}).contains(1,0),"late weight reaches old downstream transition");
  }
  s::PushdownSystem<> base;base.addControl();base.addControl();
  auto seed=s::RegularSet::singleton(2,{0,{0}});
  s::SaturationSession<> snapshot(base,seed);
  base.addRule(0,0,1,{0});
  require(!snapshot.run().accepts(1,{0}),
          "session keeps immutable base-rule index snapshot");
}
inline void fieldRules() {
  // Table 4 / Figure 3: u=0, v=1, w=2, x=3; f=1,g=2,h=3.
  s::SynchronizedSystem<> p;
  for(auto edge:std::vector<std::pair<int,int>>{{35,36},{36,37},{37,38},{38,39},{39,42},{38,40},{40,41},{41,42}})
    p.addNormal({0,edge.first},{0,edge.second});
  for(auto edge:std::vector<std::pair<int,int>>{{36,37},{37,38},{38,39},{39,42},{38,40},{40,41},{41,42}})
    p.addNormal({1,edge.first},{1,edge.second});
  p.addNormal({2,39},{2,42});p.addNormal({2,41},{2,42});
  p.addStore({0,35},{1,36},1);p.addStore({1,38},{2,39},2);p.addStore({1,40},{2,41},3);
  p.addLoad({2,36},{3,37},1);
  auto a=p.postStar({{0,35},{},{}});
  require(a.mayAccept({{2,42},{},{2,1}})&&a.mayAccept({{2,42},{},{3,1}}),"Table 4 g.f and h.f");
  require(!a.mayAccept({{2,42},{},{1,2}})&&!a.mayAlias({2,42}),"Table 4 field order and empty distinction");
  require(a.mayReachNode({2,42})&&!a.mayReachNode({3,37}),"Table 4 failed load");
  auto initial_field=p.postStar({{0,35},{},{999}});
  require(initial_field.mayAccept({{2,42},{},{2,1,999}}),"wildcard includes seed-only field symbol");
}
inline void fieldLoop() {
  s::SynchronizedSystem<> p;
  p.addNormal({0,44},{0,46});p.addStore({0,46},{1,47},1);
  p.addNormal({1,47},{0,48});p.addNormal({0,48},{0,46});p.addNormal({1,47},{1,48});
  auto a=p.postStar({{0,44},{},{}});
  require(a.mayAccept({{1,48},{},std::vector<unsigned>(180,1)}),"Figure 4 unbounded access path");
  require(!a.mayAlias({1,48}),"Figure 4 strictly positive field length");
  require(a.fieldAutomaton().states()<30,"Figure 4 finite automaton");
}
inline void synchronization() {
  s::SynchronizedSystem<> p;
  p.addStore({0,51},{1,52},1);p.addCall({1,52},{2,57},53);
  p.addStore({2,57},{3,58},2);p.addNormal({3,58},{3,59});
  p.addReturn({3,59},{4,53});p.addLoad({4,53},{5,54},2);p.addLoad({5,54},{6,55},3);
  auto a=p.postStar({{0,51},{},{}});
  require(a.mayAccept({{5,54},{},{1}}),"Figure 5/6 x.h");
  require(!a.mayAccept({{6,55},{},{}}),"Figure 5/6 mismatched load");
  require(a.mayAccept({{2,57},{53},{1}}),"call-PDS pending context");
  require(!a.mayAccept({{2,57},{},{1}}),"empty context differs from pending call");
  p.addLoad({5,54},{6,55},1);
  auto b=p.postStar({{0,51},{},{}});
  require(b.mayAccept({{6,55},{},{}}),"matching call and field projections");
  auto back=p.preStar({{6,55},{},{}});
  require(back.mayAccept({{0,51},{},{}}),"synchronized pre*");
  s::SynchronizedSystem<> mismatch;
  mismatch.addCall({0,1},{1,10},4);mismatch.addReturn({1,10},{2,3});
  auto c=mismatch.postStar({{0,1},{},{}});
  require(!c.mayAlias({2,3}),"wrong saved return statement cannot satisfy synchronization");
}
inline void weightedSynchronization() {
  s::RelationSemiring d(3);s::SynchronizedSystem<s::RelationSemiring> p(d);
  p.addNormal({0,1},{1,2},d.transition(0,1));p.addStore({1,2},{2,3},7);
  p.addCall({2,3},{3,10},4,d.transition(1,2));p.addReturn({3,10},{4,4});
  p.addLoad({4,4},{5,5},7);
  auto a=p.postStar({{0,1},{},{}});
  require(a.weight({{5,5},{},{}})==d.transition(0,2),"typestate weights on call-PDS");
  require(a.weightAt({3,10},{7})==d.transition(0,2),"typestate existential calling context");
  require(a.weightAt({3,10},{8})==d.zero(),"field projection filters weighted results");
  auto b=p.preStar({{5,5},{},{}});
  require(b.weight({{0,1},{},{}})==d.transition(0,2),"backward weights keep FORWARD execution order");
}
inline void aliasInjection() {
  // Section 5.1 / Figure 8: the client coordinates one SPDS per allocation.
  s::SynchronizedSystem<> base;
  base.addNormal({0,70},{0,71});base.addNormal({0,70},{1,71});
  base.addNormal({0,71},{0,73});base.addNormal({1,71},{1,73});
  auto aliases=base.postStar({{0,70},{},{}});
  s::SynchronizedSystem<> value;
  value.addStore({2,72},{0,73},1);value.addLoad({1,73},{3,74},1);
  require(!value.postStar({{2,72},{},{}}).mayAlias({3,74}),"no indirect flow before alias injection");
  require(aliases.mayAlias({0,73})&&aliases.mayAlias({1,73}),"base object aliases at store");
  value.addNormal({0,73},{1,73}); // supplied by the alias-aware client
  require(value.postStar({{2,72},{},{}}).mayAlias({3,74}),"Figure 8 indirect store-to-alias flow");
  require(!aliases.mayAlias({3,74}),"allocation instances remain separate");
}
inline void compactTransitionIndex() {
  struct Record { s::Transition edge; };
  std::vector<Record> records;
  s::TransitionIndex index;
  for(std::size_t i=0;i<4096;++i) {
    records.push_back({{i,(i*65537U)^17U,i+1}});
    index.insert(records.back().edge,i,records);
  }
  for(std::size_t i=0;i<records.size();++i)
    require(index.find(records[i].edge,records)==i,
            "compact transition index survives growth and collisions");
  require(index.find({7,9,11},records)==s::TransitionIndex::Missing,
          "compact transition index reports a missing edge");
}
inline void limitsAndValidation() {
  s::PushdownSystem<> p;p.addControl();p.addControl();p.addRule(0,0,1,{1});
  auto seed=s::RegularSet::singleton(2,{0,{0}});
  for(auto limit:std::vector<s::Limits>{{1,0,0},{0,1,0},{0,0,1}})
    throws<s::ResourceLimit>([&]{(void)s::postStar(p,seed,limit);});
  s::SaturationSession<> session(p,seed,s::Direction::Post,{0,0,3});
  throws<s::ResourceLimit>([&]{session.run();});
  throws<std::logic_error>([&]{session.result();});
  throws<std::logic_error>([&]{(void)session.run();});
  throws<std::invalid_argument>([&]{p.addRule(0,s::Epsilon,1,{});});
  throws<std::invalid_argument>([&]{p.addRule(0,0,1,{1,2,3});});
  throws<std::out_of_range>([&]{p.addRule(99,0,1,{});});
  throws<std::invalid_argument>([&]{s::postStar(p,s::RegularSet(3));});
  auto a=s::postStar(p,seed);
  throws<std::invalid_argument>([&]{a.accepts(0,{s::Epsilon});});
  throws<std::out_of_range>([&]{a.accepts(3,{});});
  throws<std::invalid_argument>([&]{s::RegularSet::singleton(2,{0,{s::Epsilon}});});
}
inline void parseDot() {
  std::istringstream input("digraph {\n -3 -> 1 [label=\"op--19\"];\n 1 -> 8 [label=\"eps\"];\n 8 -> 12 [label=\"cp--19\"];\n}\n");
  auto g=d::Graph::parseDot(input);
  require(s::Solver().prepare(g).queryFrom(-3).mayReach(12),
          "shared Lotus DOT parser");
}
using Test=std::pair<const char *,void(*)()>;
inline std::vector<Test> tests() {
  return {{"basics",basics},{"extremes_and_neutral",extremesAndNeutral},
    {"exhaustive_7381_words",exhaustiveWords},{"exact_250_projection_oracles",projectionOracle},
    {"concrete_200_dags",soundnessDag},{"different_path_false_positive",differentPathFalsePositive},
    {"paper_figure7",paperFigure7},{"unbounded_recursion",recursion},
    {"insertion_order",orderIndependence},{"prepared_demands",preparedDemands},
    {"wildcard_rules",wildcardRules},{"regular_seed_normalization",regularSets},
    {"empty_pds_stacks",emptyStacks},{"relation_semiring",semiringLaws},
    {"weighted_60_dag_oracles",weightedDagOracle},{"weighted_shared_push",weightedSharedPush},
    {"weighted_cycles",weightedCycles},{"incremental_rules_and_weights",incremental},
    {"paper_table4",fieldRules},{"paper_figure4",fieldLoop},
    {"paper_figure5_synchronization",synchronization},{"weighted_synchronization",weightedSynchronization},
    {"paper_figure8_alias_injection",aliasInjection},
    {"compact_transition_index",compactTransitionIndex},
    {"limits_and_invalid_input",limitsAndValidation},
    {"lotus_dot_parser",parseDot}};
}
} // namespace lotus_spds_test
