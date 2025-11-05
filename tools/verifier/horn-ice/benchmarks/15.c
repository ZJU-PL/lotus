#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * from Invgen test suite
 */

int main (int argc, char* argv[]) {

  	int n;
  
	int i, k, j;

  	n = __VERIFIER_nondet_int();
  
	k = __VERIFIER_nondet_int();

	if (n > 0 && k > n) {  

		j = 0;
  
		while( j < n ) {
    
			j++;
    
			k--;
  
		} 
  
		sassert (k >= 0);
  
		return 0;
	}
}
