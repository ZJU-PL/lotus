extern int source();
extern void sink(int p);
extern int sanitize(int x);

int main() {
  int a = source();
  int b = sanitize(a);
  sink(b);
  return 0;
}
