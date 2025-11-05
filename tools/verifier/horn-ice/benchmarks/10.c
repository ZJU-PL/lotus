#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {

	int w = 1;

	int z = 0;

	int x= 0;

	int y=0;
 
        while (__VERIFIER_nondet_int()) {
	    
		if (w) {
		
			x++; 
		
			w = !w;	    
		};
	    
		if(!z) {
		
			y++; 
		
			z=!z;
	    	};
	}

	sassert(x==y); 

	return 0; 
}
