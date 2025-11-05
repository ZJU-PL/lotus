#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main()
{
  	int a = 1;
  
	int b = 1;
  
	int c = 2;
  
	int d = 2;
  
	int x = 3;
  
	int y = 3;
  
	while (__VERIFIER_nondet_int()) {
    
		x = a + c;
    
		y = b + d;
    
		if ((x + y)%2 == 0) {
      
			a++;
      
			d++;
    		
		} else {
      
			a--;
    		}
    
		while (__VERIFIER_nondet_int()) {
       	
			c--;
       
			b--;
    		}
  	}
  
	sassert ((a + c) == (b + d));

	return 0;
}
