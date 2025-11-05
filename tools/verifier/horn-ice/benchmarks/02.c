#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

int main()
{
	int i = 1;
	
	int j = 0;

	int z = i-j;

	int x = 0;

	int y = 0;

	int w = 0;

	while(__VERIFIER_nondet_int()) 
	{
		z += (x + y) + w;
		
		y++;

		if(z%2 == 1) 

		  	x++;
		w+=2; 
	}

	sassert(x==y);

	return 0;
}
