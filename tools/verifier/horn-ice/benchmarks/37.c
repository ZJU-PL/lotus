#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * Taken from "Counterexample Driven Refinement for Abstract Interpretation" (TACAS'06) by Gulavani
 */

int main () {
	
	int n = __VERIFIER_nondet_int();
  
	int x = 0;
  
	int m = 0;
  
	while (x < n) {
     
		if (__VERIFIER_nondet_int()) {
	
			m = x;
     		}
     
		x= x + 1;
  	}
  
	if (n > 0) sassert (0 <= m && m < n);

	return 0;
}
