/*
 * IC3 motivating example
 */ 

#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main ()
{
 	int x=1; 

	int y=1;
 	
	while (__VERIFIER_nondet_int()) {
   
		int t1 = x;
   
		int t2 = y;
   
		x = t1 + t2;
   
		y = t1 + t2;
 	}
 
	sassert(y >= 1);

	return 0;
}

