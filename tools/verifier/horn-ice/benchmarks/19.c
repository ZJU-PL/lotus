#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * From "Simplifying Loop Invariant Generation using Splitter Predicates", Sharma et al. CAV'11
 */


int main () {

	int n = __VERIFIER_nondet_int();

 	int m = __VERIFIER_nondet_int();

	if (n >= 0 && m >= 0 && m < n) {

		int x = 0; 
  
		int y = m;
  
		while (x < n) {
    
			x++;
    
			if (x > m) y++;
  		}
  		
		sassert(y == n);
	}

	return 0;
}
