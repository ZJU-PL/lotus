#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * "split.c" from InvGen benchmark suite
 */


int main () {
  
	int k = 100;
  
	int b = __VERIFIER_nondet_int();
  
	int i;
  
	int j = __VERIFIER_nondet_int();
  
	int n;
  
	i = j;
  
	for(n = 0; n < 2*k; n++) {
    
		if (b) {
      
			i++;
    		} else {
      
			j++;
    		}
    
		b = !b;
  	}
  
	sassert(i == j);

	return 0;
}
