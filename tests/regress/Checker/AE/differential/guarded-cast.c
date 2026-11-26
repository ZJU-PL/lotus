// Both branches constrain the same signed-char value used by the GEP.
int main(int n, char **argv) {
  int a[4] = {0};
  signed char i = n;
  if (i >= 0 && i < 4)
    return a[i];
  return 0;
}
