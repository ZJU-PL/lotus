#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * Based on ex3 from NECLA Static Analysis Benchmarks
 */


int main () {
  
	int j = 0;
  
	int i;
  
	int x = 100;
   
  	for (i = 0; i < x ; i++){
    
		j = j + 2;
  	}

  	sassert(j == 2*x);
}


