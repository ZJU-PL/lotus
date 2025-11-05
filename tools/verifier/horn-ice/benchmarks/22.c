#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {
  
	int x = 0;
  
	int y = 0;
  
	int z = 0;
  
	int k = 0;

  	while (__VERIFIER_nondet_int()) {
     
		if (k%3 == 0)
       
			x++;
     	
		y++;
     
		z++;
     
		k = (x + y) + z;
  	}

  	sassert (x == y);
  	
	sassert (y == z);

	return 0;
}

