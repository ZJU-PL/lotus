#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {
  
	int w = 1, z = 0, x = 0, y = 0;
  
	while (__VERIFIER_nondet_int()) {
    
		while (__VERIFIER_nondet_int()) {
      
			if (w%2 == 1) 
        
				x++;
      			
			if (z%2 == 0)
        
				y++;
    		}
    
		while (__VERIFIER_nondet_int()) {
      
			z = x + y;
      
			w = z + 1;
    		}
  	}
  
	sassert (x == y);

	return 0;
}
