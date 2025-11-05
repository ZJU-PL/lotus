/*
 * Adapted from "Automated Error Diagnosis Using Abductive Inference" by Dillig et al.
 */

#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main() {

	int n = __VERIFIER_nondet_int();

	int flag = __VERIFIER_nondet_int();

   	assume (n >= 0);
   
	int k = 1;
   
	if (flag) {
	
		k = __VERIFIER_nondet_int();
	
		assume (k >= 0);
   	}
   
	int i = 0, j = 0;
   
	while (i <= n) {
     
		i++;
     
		j += i;
   	}
   
	int z = k + i + j;
   
	sassert(z > 2*n);

	return 0;
}

