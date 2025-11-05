#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();
/*
 * "nest-if8" from InvGen benchmark suite
 */


int main() {
  
	int i, j, k, n, m = __VERIFIER_nondet_int();

	n = __VERIFIER_nondet_int();
  
	if (m + 1 < n); else return 0;
  
	for (i = 0; i < n; i += 4) {
    
		for (j = i; j < m; ) {
      
			if (__VERIFIER_nondet_int()) {
        
				sassert(j >= 0);
        
				j++;
        			
				k = 0;
        
				while (k < j) {
          
					k++;
        			}

      			} else { 
	
				sassert ((n + j) + 5 > i);
	
				j += 2;
      
			}
    		}
  	}

	return 0;
}
