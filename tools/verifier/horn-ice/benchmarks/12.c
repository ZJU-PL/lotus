#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main () {

	int flag = __VERIFIER_nondet_int();
  
	int t = 0;
  
	int s = 0;
  
	int a = 0;
  
	int b = 0;
  
	while (__VERIFIER_nondet_int()) {
    
		a++;
    
		b++;
    
		s += a;
    
		t += b;
    
		if (flag) {
      
			t += a;
    		}
  	} 
  
	//2s >= t
  	int x = 1;
  
	if (flag) {
    
		x = (t - 2*s) + 2;
  	}
  
	//x <= 2
  	int y = 0;

  	while (y <= x) {
    
		if(__VERIFIER_nondet_int()) 
       
			y++;
    		else 
       
			y += 2;
  	}
  
	sassert (y <= 4);

	return 0;
}

