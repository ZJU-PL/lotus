#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * Based on ex16 from NECLA Static Analysis Benchmarks
 */

int main() {

	int x = __VERIFIER_nondet_int();

	int y = __VERIFIER_nondet_int();

	int i = 0;
  
	int t = y;
   
  	if (x == y) return x;
  
  	while (__VERIFIER_nondet_int()) {
    
		if (x > 0)   
      
			y = y + x;
  	}
   
  	sassert (y >= t);
	
	return 0;
}


