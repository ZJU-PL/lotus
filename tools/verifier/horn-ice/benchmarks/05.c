#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main ()
{

	int flag = __VERIFIER_nondet_int();

	int x = 0;

	int y = 0;

	int j = 0;

	int i = 0;

	while (__VERIFIER_nondet_int()) {
	  
		x++;
	  	
		y++;

	  	i += x;
	  
		j += y;
	  
		if (flag)  j += 1;
	}
 
	sassert (j >= i);

	return 0;	
}
