#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {
	
	int n = __VERIFIER_nondet_int();
  
	int x = 0;
  
	int y = 0;
  
	int i = 0;
  
  	while (i < n) {
    
		i++;
    
		x++;
    
		if (i%2 == 0) y++;
  	}
  
  	if (i%2 == 0) sassert (x == 2*y);
}

