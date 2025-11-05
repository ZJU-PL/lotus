#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {

	int w = 1;
	
	int z = 0;

	int x = 0;

	int y = 0;
  
        while (__VERIFIER_nondet_int()) {

	    	if (w%2 == 1) {x++; w++;};
	    
          	if (z%2 == 0) {y++; z++;};
	}

	sassert(x <= 1);

	return 0;
}

