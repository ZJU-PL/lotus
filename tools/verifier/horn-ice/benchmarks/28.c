#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * From CAV'12 by Sharma et al.
 */

int main () {
  
	int x = 0;
  
	int y = 0;
  
	int n = 0;
  
	while (__VERIFIER_nondet_int()) {
      
		x++;
      
		y++;
  	}
  
	while (x != n) {
      
		x--;
      
		y--;
  	}
  
	sassert(y==n);

	return 0;
}
