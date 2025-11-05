#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {

	int x = __VERIFIER_nondet_int();

 	int y = __VERIFIER_nondet_int();

	int k = __VERIFIER_nondet_int();

 	int j = __VERIFIER_nondet_int();

	int i = __VERIFIER_nondet_int();

 	int n = __VERIFIER_nondet_int();

	if ((x + y) == k) {
    
		int m = 0;
    
		j = 0;
    
		while (j < n) {
      
		if (j == i) {
         
			x++;
         
			y--;
      		
		} else {
         
			y++;
         
			x--;
      		}

		if(__VERIFIER_nondet_int())
  		
			m = j;
      
			j++;
    		}
    
		sassert((x + y) == k);
    
		if(n > 0) {

	   		sassert (0 <= m); 
	
			sassert (m < n);
    		}
	}

	return 0;
}

