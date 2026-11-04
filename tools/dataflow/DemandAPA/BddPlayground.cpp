#include "Dataflow/DemandAPA/Algebra.h"
#include "Dataflow/DemandAPA/DemandOmp.h"
#include "Dataflow/DemandAPA/Support.h"
#include "bdd.h"

bdd add(int i, int j) {
  // i and j are integers (binary strings) less than 2^GL. this returns a BDD
  // that accepts only the valuation <i, j>
  bdd ret = bddtrue;
  for (int k = 0; k < GL; ++k) {
    if (i & (1 << k))
      ret &= bdd_ithvar(var(k));
    else
      ret &= bdd_nithvar(var(k));

    if (j & (1 << k))
      ret &= bdd_ithvar(varPrimed(k));
    else
      ret &= bdd_nithvar(varPrimed(k));
  }
  return ret;
}

int main() {
#ifdef LOCAL
  auto stTime = omp_get_wtime();
#endif
  ios::sync_with_stdio(false);
  cout << fixed;
  cout.precision(10);
  cin.tie(0);

  V<string> fileNames;
  // IMP: use "../" for running from terminal, "../../" when using CMake
  string path = "../";
  //	string path = "../../";

  {
    GL = 3;
    int varnum = 80;
    bool b = bdd_init(10000000, 1000000);
    bdd_setvarnum(varnum);
    bdd_done();
    bdd_init(10000000, 1000000);
    bdd_setvarnum(varnum - 5);
    setPAone();
    // v0, v1, v2 := 0, 1, 1
    bdd ans = bdd_nithvar(varPrimed(0)) & bdd_ithvar(varPrimed(1)) &
              bdd_ithvar(varPrimed(2));
    print(ans);
    // v0, v1, v2 := !(v1 xor !v0), v1, !v0 => v0
    bdd rhs0 = !bdd_xor(bdd_ithvar(var(1)), !bdd_ithvar(var(0)));
    bdd rhs2 = bdd_imp(!bdd_ithvar(var(0)), bdd_ithvar(var(0)));
    // gives 110
    /*		print(PAdot(ans,
                                            setEq(1) &
                                            setVar(0, "", rhs0, bddtrue) &
                                            setVar(2, "", rhs2, bddtrue)));*/
    /*		{
                            db("AAA");
                            // same as before, but put choose for v2
                            // 111
                            print(PAdot(ans,
                                        setEq(1) &
                                        setVar(0, "", rhs0, bddtrue) &
                                        setVar(2, "choose", bdd_ithvar(var(2)),
       bdd_ithvar(var(0)))));
                            // 110
                            print(PAdot(ans,
                                        setEq(1) &
                                        setVar(0, "", rhs0, bddtrue) &
                                        setVar(2, "choose", bdd_ithvar(var(0)),
       bdd_ithvar(var(2)))));
                            // 110
                            print(PAdot(ans,
                                        setEq(1) &
                                        setVar(0, "", rhs0, bddtrue) &
                                        setVar(2, "choose",
       bdd_xor(bdd_ithvar(var(1)), bdd_ithvar(var(2))), bdd_ithvar(var(1)) &
       bdd_ithvar(var(2)))));
                            // 11X
                            print(PAdot(ans,
                                        setEq(1) &
                                        setVar(0, "", rhs0, bddtrue) &
                                        setVar(2, "choose",
       bdd_xor(bdd_ithvar(var(1)), bdd_ithvar(var(2))), bdd_ithvar(var(0)))));
                    }*/
    print(ans);
    print(deadTF(si({0, 2})));
    ans = PAdot(ans, deadTF(si({1})));
    print(ans);
    ans &= setEq(2);
    print(ans);
    ans &= setNotEq(0);
    print(ans);
    ans &= bdd_ithvar(varPrimed(1));
    print(ans);
    ans &= bdd_nithvar(varPrimed(1));
    print(ans);
    print(ans & setEq(2));
    print(ans & setEq(2) & setNotEq(0));
    print(ans & setEq(1));
    /*		bdd ans = bddfalse;

                    for (int i = 0; i < (1 << GL); ++i) {
                            db(i);
                            ans |= add(i, i);

                            bdd_allsat(add(i, i), allsatHandler);
                            if (i + 1 < (1 << GL))
                                    ans |= add(i, i + 1), bdd_allsat(add(i, i +
       1), allsatHandler);
                    }

                    cout << "----------------" << endl;
                    bdd_allsat(ans, allsatHandler);
                    cout << "----------------" << endl;
                    bdd_allsat(PAdot(ans, ans), allsatHandler);
                    cout << "----------------" << endl;
                    bdd_allsat(PAdot(ans, PAdot(ans, ans)), allsatHandler);
                    cout << "----------------" << endl;
                    bdd_allsat(PAstar(ans), allsatHandler);*/

    //		bdd_allsat(ret, allsatHandler);
    bdd_done();
  }
  /*	{
                  int varnum = 8;
                  bool b = bdd_init(10000000, 1000000);
                  bdd_setvarnum(varnum);
                  bdd ret = bddtrue;
                  bdd otherFormula = bddtrue;
                  for (int i = 0; i < 4; ++i) {
                          ret &= bdd_biimp(bdd_ithvar(2 * i), bdd_ithvar(2 * i +
     1)); otherFormula &= bdd_biimp(bdd_ithvar(i), bdd_ithvar(i + 4));
                  }


  *//*		for (int i = 0; i < (1 << 9); ++i) {
			bdd temp = ret;
			bdd restriction = bddtrue;
			for (int j = 0; j < 9; ++j)
				restriction &= ((i & (1 << j)) ? bdd_ithvar(j) : bdd_nithvar(j));

			temp = bdd_restrict(temp, restriction);
			db(bitset<9>(i), temp);
		}*//*


//		bdd_allsat(ret, allsatHandler);
		bdd_allsat(ret | otherFormula, allsatHandler);
		bdd_done();
	}*/

  /*	{
                  int varnum = 5;
                  bool b = bdd_init(10000000, 1000000);
                  bdd_setvarnum(varnum);
                  bdd ans = bddtrue;
                  ans &= bdd_biimp(bdd_ithvar(0), bdd_ithvar(1)) &
  bdd_biimp(bdd_ithvar(2), bdd_ithvar(3)); bdd_allsat(ans, allsatHandler);
                  bdd_done();
          }
          {
                  int varnum = 5;
                  bool b = bdd_init(10000000, 1000000);
                  bdd_setvarnum(varnum);
                  bdd ans;

                  for (int i = 0; i < 10; ++i) {
                          int asgn = rand() % (1 << varnum);
                          bdd Asgn = bddtrue;
                          for (int j = 0; j < varnum; ++j) {
                                  if (asgn & (1 << j))
                                          Asgn &= bdd_ithvar(j);
                                  else
                                          Asgn &= bdd_nithvar(j);
                          }
                          db(asgn);
                          ans |= Asgn;
                  }

  //		ans = bdd_exist(ans, bdd_ithvar(1));
  //		ans = bdd_exist(ans, bdd_ithvar(2));
                  ans = bdd_restrict(ans, bdd_nithvar(0));
                  ans = bdd_restrict(ans, bdd_ithvar(1));
                  ans = bdd_forall(ans, bdd_ithvar(4));

                  bdd_allsat(ans, allsatHandler);
                  bdd_done();
          }*/
  /*
          for (int i = 0; i < (1 << 4); ++i) {
                  db(i);
                  bool b = bdd_init(10000000,1000000);
                  int method=BDD_REORDER_NONE;
                  int n;
                  assert(!b);
                  bdd_setvarnum(4);

                  bdd v[3] = {bdd_ithvar(0), bdd_ithvar(1), bdd_ithvar(2)};
                  bdd vc[3] = {bdd_nithvar(0), bdd_nithvar(1), bdd_nithvar(2)};
                  bdd ans;



                  int randomSkip[2] = {rand() % 4, rand() % 4};
                  for (int k = 0; k < 2; ++k) {
                          db(randomSkip[k]);
                          bdd temp = bddtrue;
                          for (int j = 0; j < 3; ++j) {
                                  if (j == randomSkip[k])
                                          continue;
                                  if ((i & (1 << j)) || j)
                                          temp &= v[j];
                                  else
                                          temp &= vc[j];
                          }
                          ans |= temp;
                  }


  //		bdd_allsat(ans, allsatHandler);
  //		cout << ans << endl;
                  // variables are x_0 .. x_99

                  bdd_printall();
                  cout << "-----------------------------------" << endl;
                  bdd_done();
          }
  */

#ifdef LOCAL
  cout << "\n\n\nExecution time: " << (omp_get_wtime() - stTime) << " s"
       << endl;
#endif
  return 0;
}
