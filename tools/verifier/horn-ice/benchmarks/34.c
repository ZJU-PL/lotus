#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {
	
	int n = __VERIFIER_nondet_int();
  
	int x = 0;
  
	int y = 0;
  
	int i = 0;
  
	int m = 10;
   
  	while (i < n) {
    
		i++;
    
		x++;
    
		if (i%2 == 0) y++;
  	}
  
  	if (i == m) sassert (x == 2*y);

	return 0;
}

