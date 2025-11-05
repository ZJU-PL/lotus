#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();


/*
 * From "A Practical and Complete Approach to Predicate Refinement" by McMillan TACAS'06
 */

int main () {
  
	int i = __VERIFIER_nondet_int();
 
	int j = __VERIFIER_nondet_int();

  	int x = i;

  	int y = j;
 
  	while (x != 0) {
	
		x--;

		y--;
  	}

  	if(i == j)
		
		sassert(y == 0);

	return 0;
}

