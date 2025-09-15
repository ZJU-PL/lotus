extern int source();
extern void sink(int p);

int main() {
  int a = source();
  if (a > 0) {
    sink(a);
  } else {
    sink(a);
  }
  return 0;
}
