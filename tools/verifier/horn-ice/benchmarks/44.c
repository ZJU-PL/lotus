/*
 * Adapted from ex20 from NECLA Static Analysis Benchmarks
 */

#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main() {

	int k = __VERIFIER_nondet_int();

	int flag = __VERIFIER_nondet_int();

  	int i = 0;
  
	int j = 0;
  
	int n;
  
	int __BLAST_NONDET;

  	if (flag == 1) {
     
		n = 1;

  	} else {
     
		n = 2;
  	}

  	i = 0;

  	while (i <= k) {
    
		i++;
    
		j = j + n;
  	}
  
	if (flag == 1)
      
		sassert(j == i);

	return 0;	
}
