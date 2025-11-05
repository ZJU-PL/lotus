#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main ()
{
	int k = __VERIFIER_nondet_int();
  
	int z = k;
  
	int x = 0;
  
	int y = 0;

  	while (__VERIFIER_nondet_int()) {
    
		int c = 0;
    
		while (__VERIFIER_nondet_int()) {
      
			if (z == (k + y) - c) {
        
				x++;
        
				y++;
        
				c++;
      
			} else {
        
				x++;
        
				y--;
        
				c++;
      			}
    		}
    
		while (__VERIFIER_nondet_int()) {
      
			x--;
      
			y--;
    		}
    
		z = k + y;
  
	}
  
	sassert (x == y);

	return 0;
}
