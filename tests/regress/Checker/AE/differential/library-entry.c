// Use --entry=no-main. Only entry is a root of the call graph.
void helper(int i) {
  int a[4];
  a[i] = 1;
}

void entry(void) { helper(0); }
