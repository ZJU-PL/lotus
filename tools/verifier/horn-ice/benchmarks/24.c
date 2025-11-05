#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * "nested5.c" from InvGen test suite
 */

int main() {
  
	int i, j, k, n;
  
  	for (i = 0; i < n; i++)
    
		for (j = i; j < n; j++)
      
			for (k = j; k < n; k++)
	
				sassert(k >= i);

	return 0;
}
