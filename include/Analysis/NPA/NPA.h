/**********************************************************************
 * Newtonian Program Analysis – generic C++14 header
 *
 * Based on OCaml NPA-PMA (lib/pmaf/npa.ml)

 *   ✅ Conditional expressions (T0_cond/T1_cond) with condCombine
 *   ✅ Kleene & Newton iterators with correct differential construction
 *   ✅ Ndet linearization: adds base values to branches
 *   ✅ InfClos: re-marks dirty each iteration
 *   ✅ Diff::clone: resets cached values like OCaml unmasked_copy
 *
 * KNOWN LIMITATIONS vs. OCaml version:
 *   ❌ Probabilistic expressions (T0_prob/T1_prob) - NOT IMPLEMENTED
 *   ❌ Symbolic solving infrastructure - NOT IMPLEMENTED
 *      (Interp0_symbolic, Interp1_symbolic, Newton_symbolic)
 *      Note: extend_lin in domain is for forward compatibility
 
 When you later add symbolic support, you must:
    - implement extend_lin,
    - implement the symbolic interpreters and equaliser,
    - use variable-sensitive dirty marking to avoid exponential re-evaluation.
 * MIT licence – use at will, no warranty.
 *********************************************************************/
 #ifndef NPA_HPP
 #define NPA_HPP
 #include <cassert>
 #include <chrono>
 #include <functional>
 #include <iostream>
 #include <map>
 #include <memory>
 #include <optional>
 #include <string>
 #include <utility>
 #include <vector>
 
 /**********************************************************************
  * 0. helpers
  *********************************************************************/
 using Symbol = std::string;
 template <class T> inline void hash_combine(std::size_t& h,const T& v){
     h ^= std::hash<T>{}(v)+0x9e3779b9+(h<<6)+(h>>2);}
 
 struct Stat{ double time{}; int iters{}; };
 
/**********************************************************************
 * 1. Domain concept  (semiring)
 * 
 * Required types:
 *   - value_type: the domain element type
 *   - test_type: type for conditional guards (used by T0_cond/T1_cond)
 * 
 * Required methods:
 *   - zero, equal, combine, extend, subtract, ndetCombine, condCombine
 *   - extend_lin: linear extension (required for forward compatibility,
 *                 but only used by symbolic solvers which are NOT implemented;
 *                 for non-symbolic use, can equal extend)
 * 
 * Note: probCombine for probabilistic expressions is not yet required
 *       as T0_prob/T1_prob are not implemented
 *********************************************************************/
template <class D> struct DomainHas {
  template <class T>
  static auto test(int)->decltype( T::zero()
                                 , T::combine(T::zero(),T::zero())
                                 , T::extend(T::zero(),T::zero())
                                 , T::extend_lin(T::zero(),T::zero())
                                 , T::ndetCombine(T::zero(),T::zero())
                                 , T::condCombine(typename T::test_type{},T::zero(),T::zero())
                                 , T::subtract(T::zero(),T::zero())
                                 , T::equal(T::zero(),T::zero())
                                 , std::true_type{});
  template <class> static std::false_type test(...);
public:
  static constexpr bool value = std::is_same<decltype(test<D>(0)),std::true_type>::value;
};

template <class D> using DomVal = typename D::value_type;
template <class D> using DomTest = typename D::test_type;

#define NPA_REQUIRE_DOMAIN(D) static_assert(DomainHas<D>::value,"Invalid DOMAIN: missing required methods (zero, combine, extend, extend_lin, ndetCombine, condCombine, subtract, equal) or test_type")
 
 /**********************************************************************
  * 2. Dirty-flag base
  *********************************************************************/
 struct Dirty{ mutable bool dirty_=true; void mark(bool d=true)const{dirty_=d;}};
 
 /**********************************************************************
  * 3. Exp0 – non-linear expressions
  *********************************************************************/
 template <class D> struct Exp0;
 template <class D> using E0 = std::shared_ptr<Exp0<D>>;
 
template <class D>
struct Exp0 : Dirty, std::enable_shared_from_this<Exp0<D>>{
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K{Term,Seq,Call,Cond,Ndet,Hole,Concat,InfClos};
  K k;
  /* payloads */
  V c;                           // Term / Seq const
  E0<D> t;                       // Seq tail  or Call arg
  Symbol sym;                    // Call / Hole / Concat var / InfClos loop var
  T phi;                         // Cond guard
  E0<D> t1,t2;                   // Cond / Ndet / Concat branches
  /* cache */ mutable std::optional<V> val;
  /* factories */
  static E0<D> term(V v){ auto e=std::make_shared<Exp0>(); e->k=Term; e->c=v; return e;}
  static E0<D> seq(V c,E0<D> t){ auto e=std::make_shared<Exp0>(); e->k=Seq; e->c=c; e->t=t; return e;}
  static E0<D> call(Symbol f,E0<D> arg){ auto e=std::make_shared<Exp0>(); e->k=Call; e->sym=f; e->t=arg; return e;}
  static E0<D> cond(T phi,E0<D> t_then,E0<D> t_else){ auto e=std::make_shared<Exp0>(); e->k=Cond; e->phi=phi; e->t1=t_then; e->t2=t_else; return e;}
  static E0<D> ndet(E0<D> a,E0<D> b){ auto e=std::make_shared<Exp0>(); e->k=Ndet; e->t1=a; e->t2=b; return e;}
  static E0<D> hole(Symbol x){ auto e=std::make_shared<Exp0>(); e->k=Hole; e->sym=x; return e;}
  static E0<D> concat(E0<D> a,Symbol x,E0<D> b){
        auto e=std::make_shared<Exp0>(); e->k=Concat; e->t1=a; e->t2=b; e->sym=x; return e;}
  static E0<D> inf(E0<D> body,Symbol x){
        auto e=std::make_shared<Exp0>(); e->k=InfClos; e->t=body; e->sym=x; return e;}
};
 
 /**********************************************************************
  * 4. Exp1 – linear expressions
  *********************************************************************/
 template <class D> struct Exp1;
 template <class D> using E1 = std::shared_ptr<Exp1<D>>;
 
template <class D>
struct Exp1 : Dirty{
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K{Term,Seq,Call,Cond,Ndet,Hole,Concat,InfClos,Add,Sub};
  K k;
  V c;                            // Term / Seq const / Call const
  Symbol sym;
  T phi;                          // Cond guard
  E1<D> t,t1,t2;                  // various
  mutable std::optional<V> val;
  /* factories */
  static E1<D> term(V v){ auto e=std::make_shared<Exp1>(); e->k=Term; e->c=v; return e;}
  static E1<D> add(E1<D> a,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Add; e->t1=a; e->t2=b; return e;}
  static E1<D> sub(E1<D> a,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Sub; e->t1=a; e->t2=b; return e;}
  static E1<D> seq(V c,E1<D> t){ auto e=std::make_shared<Exp1>(); e->k=Seq; e->c=c; e->t=t; return e;}
  static E1<D> call(Symbol f,V c){ auto e=std::make_shared<Exp1>(); e->k=Call; e->sym=f; e->c=c; return e;}
  static E1<D> cond(T phi,E1<D> t_then,E1<D> t_else){ auto e=std::make_shared<Exp1>(); e->k=Cond; e->phi=phi; e->t1=t_then; e->t2=t_else; return e;}
  static E1<D> ndet(E1<D> a,E1<D> b){ auto e=std::make_shared<Exp1>(); e->k=Ndet; e->t1=a; e->t2=b; return e;}
  static E1<D> hole(Symbol x){ auto e=std::make_shared<Exp1>(); e->k=Hole; e->sym=x; return e;}
  static E1<D> concat(E1<D> a,Symbol x,E1<D> b){
        auto e=std::make_shared<Exp1>(); e->k=Concat; e->t1=a; e->t2=b; e->sym=x; return e;}
  static E1<D> inf(E1<D> body,Symbol x){
        auto e=std::make_shared<Exp1>(); e->k=InfClos; e->t=body; e->sym=x; return e;}
};
 
 /**********************************************************************
  * 5. Fixed-point helper (scalar / vector)
  *********************************************************************/
 template <class D, class F>
 auto fix(bool verbose, typename DomVal<D> init, F f){
   NPA_REQUIRE_DOMAIN(D);
   int cnt=0;
   auto last=init;
   while(true){
     auto nxt=f(last);
     if(D::equal(last,nxt)){ if(verbose) std::cerr<<"[fp] "<<cnt+1<<"\n"; return nxt;}
     last=std::move(nxt); ++cnt;
   }
 }
 template <class D, class Vec, class F>
 Vec fix_vec(bool verbose,Vec init,F f){
   int cnt=0;
   while(true){
     Vec nxt=f(init); bool stable=true;
     for(size_t i=0;i<init.size();++i) if(!D::equal(init[i],nxt[i])){stable=false;break;}
     if(stable){ if(verbose) std::cerr<<"[fp] "<<cnt+1<<"\n"; return nxt;}
     init.swap(nxt); ++cnt;
   }
 }
 
 /**********************************************************************
  * 6. Interpreter Exp0
  *********************************************************************/
 template <class D>
 struct I0{
   using V = DomVal<D>; using Map = std::map<Symbol,V>;
   static V eval(bool verbose,const Map& nu,const E0<D>& e){
     mark(e); return rec(nu,{},e);
   }
 private:
   using Env = std::map<Symbol,V>;
  static void mark(const E0<D>& e){
    if(!e) return; e->mark();
    switch(e->k){
      case Exp0<D>::Seq:   mark(e->t); break;
      case Exp0<D>::Cond:  mark(e->t1); mark(e->t2); break;
      case Exp0<D>::Ndet:  mark(e->t1); mark(e->t2); break;
      case Exp0<D>::Concat:mark(e->t1); mark(e->t2); break;
      case Exp0<D>::InfClos: mark(e->t); break;
      default: break;
    }
  }
   static V rec(const Map& nu,const Env& env,const E0<D>& e){
     if(!e->dirty_) return *e->val;
     V v{};
    switch(e->k){
      case Exp0<D>::Term: v=e->c; break;
      case Exp0<D>::Seq:  v=D::extend(e->c,rec(nu,env,e->t)); break;
      case Exp0<D>::Call: v=D::extend(nu.at(e->sym), rec(nu,env,e->t)); break;
      case Exp0<D>::Cond: v=D::condCombine(e->phi, rec(nu,env,e->t1), rec(nu,env,e->t2)); break;
      case Exp0<D>::Ndet: v=D::ndetCombine(rec(nu,env,e->t1), rec(nu,env,e->t2)); break;
      case Exp0<D>::Hole: v=env.at(e->sym); break;
       case Exp0<D>::Concat:{
           auto env2=env; env2[e->sym]=rec(nu,env,e->t2);
           v=rec(nu,env2,e->t1); }
           break;
      case Exp0<D>::InfClos:{
          V init=D::zero();
          v=fix<D>(false,init,[&](V cur){
                auto env2=env; env2[e->sym]=cur;
                // Re-mark body dirty to force re-evaluation (OCaml: mark_dirty_texp0)
                mark(e->t);
                return rec(nu,env2,e->t);
              });}
          break;
     }
     e->val=v; e->mark(false); return v;
   }
 };
 
 /**********************************************************************
  * 7. Differential builder
  *********************************************************************/
 template <class D>
 struct Diff{
   using V = DomVal<D>; using M0 = E0<D>; using M1 = E1<D>; using Map = std::map<Symbol,V>;
   static M1 build(const Map& nu,const M0& e){ return aux(nu,e,clone(e)); }
 private:
  static M0 clone(const M0& e){
    auto c=std::make_shared<Exp0<D>>(*e);
    // Reset cached value and dirty flag (OCaml: unmasked_copy_texp0)
    c->val.reset();
    c->dirty_=true;
    // Recursively clone children
    if(e->t) c->t=clone(e->t);
    if(e->t1) c->t1=clone(e->t1);
    if(e->t2) c->t2=clone(e->t2);
    return c;
  }
   static M1 aux(const Map& nu,const M0& o,const M0& cur){
     using K0 = typename Exp0<D>::K;
     switch(o->k){
       case K0::Term:   return E1<D>::term(D::zero());
       case K0::Seq: {
         auto dTail = aux(nu,o->t,cur->t);
         return E1<D>::seq(o->c,dTail);
       }
      case K0::Call:{
        auto dArg = aux(nu,o->t,cur->t);
        auto left = E1<D>::seq(nu.at(o->sym), dArg);
        assert(o->t->val.has_value() && "Call arg must be evaluated before differential");
        auto right= E1<D>::call(o->sym, *o->t->val);
        return E1<D>::add(left,right);
      }
      case K0::Cond:{
        auto d1=aux(nu,o->t1,cur->t1);
        auto d2=aux(nu,o->t2,cur->t2);
        return E1<D>::cond(o->phi, d1, d2);
      }
      case K0::Ndet:{
        auto a1=aux(nu,o->t1,cur->t1);
        auto a2=aux(nu,o->t2,cur->t2);
        // Must add base values to differentials (OCaml: t1_add (t1_term v1, dt1))
        assert(o->t1->val.has_value() && "Ndet branch 1 must be evaluated before differential");
        assert(o->t2->val.has_value() && "Ndet branch 2 must be evaluated before differential");
        assert(o->val.has_value() && "Ndet node must be evaluated before differential");
        auto aug1 = E1<D>::add(E1<D>::term(*o->t1->val), a1);
        auto aug2 = E1<D>::add(E1<D>::term(*o->t2->val), a2);
        auto augmented = E1<D>::ndet(aug1, aug2);
        if(D::idempotent) return augmented;
        else return E1<D>::sub(augmented, E1<D>::term(*o->val));
      }
       case K0::Hole:   return E1<D>::hole(o->sym);
       case K0::Concat:{
         auto p1=aux(nu,o->t1,cur->t1);
         auto p2=aux(nu,o->t2,cur->t2);
         return E1<D>::concat(p1,o->sym,p2);
       }
       case K0::InfClos:{
         auto body=aux(nu,o->t,cur->t);
         return E1<D>::inf(body,o->sym);
       }
     }
     return nullptr; /*unreachable*/
   }
 };
 
 /**********************************************************************
  * 8. Interpreter Exp1
  *********************************************************************/
 template <class D>
 struct I1{
   using V = DomVal<D>; using Map = std::map<Symbol,V>;
   static V eval(bool verbose,const Map& nu,const E1<D>& e){
     mark(e); return rec(nu,{},e);
   }
 private:
   using Env=std::map<Symbol,V>;
   static void mark(const E1<D>& e){
     if(!e) return; e->mark();
     if(e->t) mark(e->t); if(e->t1) mark(e->t1); if(e->t2) mark(e->t2);
   }
   static V rec(const Map&nu,const Env&env,const E1<D>&e){
     if(!e->dirty_) return *e->val;
     V v{};
     using K = typename Exp1<D>::K;
     switch(e->k){
      case K::Term: v=e->c; break;
      case K::Seq:  v=D::extend(e->c,rec(nu,env,e->t)); break;
      case K::Call: v=D::extend(nu.at(e->sym), e->c); break; // non-symbolic uses extend
       case K::Cond: v=D::condCombine(e->phi, rec(nu,env,e->t1), rec(nu,env,e->t2)); break;
       case K::Add:  v=D::combine(rec(nu,env,e->t1),rec(nu,env,e->t2)); break;
       case K::Sub:  v=D::subtract(rec(nu,env,e->t1),rec(nu,env,e->t2)); break;
       case K::Ndet: v=D::ndetCombine(rec(nu,env,e->t1),rec(nu,env,e->t2)); break;
       case K::Hole: v=env.at(e->sym); break;
       case K::Concat:{
           auto env2=env; env2[e->sym]=rec(nu,env,e->t2);
           v=rec(nu,env2,e->t1);} break;
      case K::InfClos:{
          V init=D::zero();
          v=fix<D>(false,init,[&](V cur){
                auto env2=env; env2[e->sym]=cur;
                // Re-mark body dirty to force re-evaluation (OCaml: mark_dirty_texp1)
                mark(e->t);
                return rec(nu,env2,e->t);
              });} break;
     }
     e->val=v; e->mark(false); return v;
   }
 };
 
 /**********************************************************************
  * 9. Generic solver driver
  *********************************************************************/
 template <class D,class ITER>
 struct Solver{
   using V = DomVal<D>; using Eqn = std::pair<Symbol,E0<D>>;
   static std::pair<std::vector<std::pair<Symbol,V>>,Stat>
   solve(const std::vector<Eqn>& eqns,bool verbose=false,int max=-1){
     NPA_REQUIRE_DOMAIN(D);
     std::vector<std::pair<Symbol,V>> cur;
     for(auto& e:eqns) cur.emplace_back(e.first,D::zero());
 
     auto tic=std::chrono::high_resolution_clock::now();
     int it=0;
     while(max<0 || it<max){
       auto nxt = ITER::run(verbose,eqns,cur);
       bool stable=true;
       for(size_t i=0;i<cur.size();++i)
         if(!D::equal(cur[i].second,nxt[i].second)){stable=false;break;}
       cur.swap(nxt); ++it;
       if(stable){ if(verbose) std::cerr<<"[conv] "<<it<<"\n"; break;}
     }
     auto toc=std::chrono::high_resolution_clock::now();
     Stat st; st.iters=it;
     st.time=std::chrono::duration<double>(toc-tic).count();
     return {cur,st};
   }
 };
 
 /**********************************************************************
  * 10. Kleene iterator
  *********************************************************************/
 template <class D>
 struct KleeneIter{
   using V = DomVal<D>; using Eqn = std::pair<Symbol,E0<D>>;
   static std::vector<std::pair<Symbol,V>>
   run(bool verbose,const std::vector<Eqn>& eqns,
       const std::vector<std::pair<Symbol,V>>& binds){
     std::map<Symbol,V> nu; for(auto&b:binds) nu[b.first]=b.second;
     std::vector<std::pair<Symbol,V>> out;
     for(auto& e:eqns){
       V v=I0<D>::eval(verbose,nu,e.second);
       out.emplace_back(e.first,v);
     }
     return out;
   }
 };
 
 /**********************************************************************
  * 11. Newton iterator
  *********************************************************************/
 template <class D>
 struct NewtonIter{
   using V = DomVal<D>; using Eqn = std::pair<Symbol,E0<D>>;
   static std::vector<std::pair<Symbol,V>>
   run(bool verbose,const std::vector<Eqn>& eqns,
       const std::vector<std::pair<Symbol,V>>& binds){
     std::map<Symbol,V> nu; for(auto&b:binds) nu[b.first]=b.second;
 
     /* 1. build differential system */
     std::vector<std::pair<Symbol,E1<D>>> rhs;
     for(auto& e:eqns){
       V v=I0<D>::eval(verbose,nu,e.second);
       auto d=Diff<D>::build(nu,e.second);
       auto base=D::idempotent? v : D::subtract(v,nu[e.first]);
       rhs.emplace_back(e.first,E1<D>::add(E1<D>::term(base),d));
     }
 
     /* 2. solve linear system via Kleene star (vector fix) */
     std::vector<V> init(rhs.size(), D::zero());
     auto delta=fix_vec<D>(verbose,init,[&](const std::vector<V>& cur){
         std::map<Symbol,V> env;
         for(size_t i=0;i<cur.size();++i) env[rhs[i].first]=cur[i];
         std::vector<V> nxt;
         for(auto& p:rhs) nxt.push_back(I1<D>::eval(verbose,env,p.second));
         return nxt;
       });
 
     /* 3. new approximation */
     std::vector<std::pair<Symbol,V>> out;
     for(size_t i=0;i<binds.size();++i){
       V upd = delta[i];
       V nxt = D::idempotent? upd : D::combine(binds[i].second, upd);
       out.emplace_back(binds[i].first, nxt);
     }
     return out;
   }
 };
 
 /**********************************************************************
  * 12. public aliases
  *********************************************************************/
 template<class D> using KleeneSolver = Solver<D,KleeneIter<D>>;
 template<class D> using NewtonSolver = Solver<D,NewtonIter<D>>;
 
 /**********************************************************************
  * 13. Example Boolean domain
  *********************************************************************/
struct BoolDom{
  using value_type=bool; 
  using test_type=bool;
  static constexpr bool idempotent=true;
  static bool zero(){return false;}
  static bool equal(bool a,bool b){return a==b;}
  static bool combine(bool a,bool b){return a||b;}
  static bool extend(bool a,bool b){return a&&b;}
  static bool extend_lin(bool a,bool b){return a&&b;} // linear extension (same as extend for boolean)
  static bool condCombine(bool phi,bool t_then,bool t_else){return (phi&&t_then)||(!phi&&t_else);}
  static bool ndetCombine(bool a,bool b){return a||b;}
  static bool subtract(bool a,bool b){return a&&(!b);}
};
 
 /**********************************************************************
  * 14. Demo (remove NPA_DEMO to suppress)
  *********************************************************************/
 #ifdef NPA_DEMO
 int main(){
   using D=BoolDom; using V=DomVal<D>;
   Symbol X="X";
   auto a=E0<D>::term(true);
   auto b=E0<D>::term(true);
   auto hole=E0<D>::hole(X);
   auto seq=E0<D>::seq(true,hole);
   auto body=E0<D>::ndet(seq,b);
 
   std::vector<std::pair<Symbol,E0<D>>> eqns={{X,body}};
   auto res1=KleeneSolver<D>::solve(eqns,true,50);
   auto res2=NewtonSolver<D>::solve(eqns,true,50);
   std::cout<<"Kleene iters="<<res1.second.iters<<"\n";
   std::cout<<"Newton iters="<<res2.second.iters<<"\n";
   return 0;
 }
 #endif
 #endif /* NPA_HPP */