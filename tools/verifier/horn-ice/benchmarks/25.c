#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {
  
	int x = 0;
  
	int y = 0;
  
	int i = 0;
  
	int j = 0;

  	while (__VERIFIER_nondet_int()) {

    		while (__VERIFIER_nondet_int()) {
       
			if (x == y)
          
				i++;
       			else
          
				j++;
    		}
    	
		if (i >= j) {
       
			x++;
       
			y++;
    		} else
       
			y++;
  	}

  	sassert (i >= j);

	return 0;
}
