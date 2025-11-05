#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * Adapted from ex17.c in NECLA test suite
 */

int main () {

	int flag = __VERIFIER_nondet_int();

	int a = __VERIFIER_nondet_int();

	int b;
   
	int j = 0;

   	for (b = 0; b < 100 ; ++b){
      
		if (flag)
         
			j = j + 1;
   	}

   	if(flag)
      
		sassert(j == 100);
}
