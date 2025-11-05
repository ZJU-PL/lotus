#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * ex49 from NECLA Static Analysis Benchmarks
 */


int main () {

	int n = __VERIFIER_nondet_int();

   	int i, sum = 0;

   	assume( n >= 0);

   	for (i = 0; i < n; ++i)
      
		sum = sum + i;

   	sassert(sum >= 0);
}

