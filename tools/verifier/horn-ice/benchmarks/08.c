#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 *  Based on "Automatically refining abstract interpretations" fig.1
 */


int main () {
 
	int x = 0, y = 0;
 
	while (__VERIFIER_nondet_int()) {
   
		if (__VERIFIER_nondet_int()) { 
      
			x++; 
      
			y += 100; 

   		} else if (__VERIFIER_nondet_int()) { 
      
			if (x >= 4) { 
          		
				x++; 

	          		y++; 
      			} 
      		
			if (x < 0) {
          		
				y--;
      			}
   		}
  
 	}
 
	sassert(x < 4 || y > 2);
	
	return 0;
}
