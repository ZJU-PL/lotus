#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * "nested2.c" from InvGen benchmark suite
 */


int main() {
  
	int i, k, n, l = __VERIFIER_nondet_int();

   	assume (l > 0);

  	for (k = 1; k < n; k++) {
    
		for (i = l; i < n; i++) {

    		}
    
		for (i = l; i < n; i++) {
      
			sassert(1 <= k);
    		}
  	}

	return 0;
}
