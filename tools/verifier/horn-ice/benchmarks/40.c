#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main() {

	int flag = __VERIFIER_nondet_int();
  
   	int i, j, k;
   
	j = 1;
   
	if (flag) {i = 0;}
   
	else {i = 1;}
   

   	while (__VERIFIER_nondet_int()) {
      
		i += 2;
      
		if (i%2 == 0) {
	
			j += 2;
      		
		} else j++;
   	} 
   
   	int a = 0;
   
	int b = 0;
   
   	while (__VERIFIER_nondet_int()) {
      
		a++;      
      
		b += (j - i); 
   	}
   
	if (flag)
     
		sassert (a == b);

	return 0;
}

