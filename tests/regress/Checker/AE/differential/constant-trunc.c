// Safe before and after mem2reg: trunc i64 4294967299 to i32 is 3.
int main(void) {
  int a[4] = {0};
  long i = 0x100000003L;
  int j = (int)i;
  return a[j];
}
