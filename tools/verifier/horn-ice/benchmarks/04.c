#include "seahorn/seahorn.h"

/*
 * Taken from Gulwani PLDI'08:
 * Program Analysis as Constraint Solving
 */

int main () {
  
	int x, y;

  	x = -50;

  	while (x < 0) {
	
		x = x + y;
		
		y++;
  	}

  	sassert(y > 0);

	return 0;
}

