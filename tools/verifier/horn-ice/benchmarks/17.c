#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();


int main () {
	int n = __VERIFIER_nondet_int();

 	int k = 1;
 
	int i = 1;
 
	int j = 0;
 
	while (i < n) {
  
		j = 0;
  
		while (j < i) {
      
			k += (i - j);
      	
			j++;
  		}

  		i++;
 	}
 
	sassert(k >= n); 

	return 0;
}
