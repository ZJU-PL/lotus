#include "seahorn/seahorn.h"

int __VERIFIER_nondet_int();

/*
 * "fragtest_simple" from InvGen benchmark suite
 */


int main () {
  
	int i, pvlen;
  
	int t;
  
	int k = 0;
  
	int n;
  
	i = 0;

  	//  pkt = pktq->tqh_first;
  	
	while (__VERIFIER_nondet_int())
    
		i = i + 1;
  
	if (i > pvlen) {
    
		pvlen = i;
  	
	} else {

  	}
  
	i = 0;
  	
	while (__VERIFIER_nondet_int()) {
    
		t = i;
    
		i = i + 1;
    
		k = k +1;
  	}

  	while (__VERIFIER_nondet_int());

  	int j = 0;
  
	n = i;
  
	while (1) {
    
		sassert(k >= 0);
    
		k = k -1;
    
		i = i - 1;
    
		j = j + 1;
    
		if (j < n) {
    		
		} else {
      
			break;
    		}
    	}
  
	return 0;
}
