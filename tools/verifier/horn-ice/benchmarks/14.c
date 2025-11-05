#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * From "The Octagon Abstract Domain" HOSC 2006 by Mine.
 */

int main() {
  
	int a = 0;
  
	int j;
  
	int m = __VERIFIER_nondet_int();
  
	int __BLAST_NONDET;
  
	if (m <= 0)
    
		return 0;
  
	for(j = 1; j <= m ; j++){
    
		if(__VERIFIER_nondet_int()) 
       		
			a++;
    		else
       
			a--; 
  	}
  
	sassert(a >= -m);
  
	sassert(a <= m);

	return 0;
}


