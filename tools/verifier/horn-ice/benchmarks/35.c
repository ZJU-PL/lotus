#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * InvGen, CAV'09 paper, fig 2
 */

int main () {
	
	int n = __VERIFIER_nondet_int();
  
	int x = 0;
  
	while (x < n) {
    
		x++;
  	} 
  
	if (n > 0) sassert (x == n);

	return 0;
}
