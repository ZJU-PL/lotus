#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * From "Path Invariants" PLDI 07 by Beyer et al.
 */

int main () {

  	int i, n, a, b;

	n = __VERIFIER_nondet_int(); 
	
	if ( n >= 0 ) {
  
		i = 0; a = 0; b = 0;
  
		while (i < n) {
    
			if (__VERIFIER_nondet_int()) {
      
				a = a + 1;
      			
				b = b + 2;
    		
			} else {
      
				a = a + 2;
      
				b = b + 1;
    			}

    			i = i + 1;
  		}

		sassert( a + b == 3*n );
	}

	return 0;
}
