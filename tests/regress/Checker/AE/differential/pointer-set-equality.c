// n == 2 makes p != q. Identical sets of possible targets do not imply p == q.
// The index computation consumes the comparison value directly.
int main(int n, char **argv) {
  int x = 0, y = 0;
  int *p = n > 1 ? &x : &y;
  int *q = n > 2 ? &x : &y;
  int i = 4 * (p != q);
  int a[4] = {0};
  return a[i];
}
