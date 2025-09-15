extern int source();
extern void sink(int p);

int main() {
  int a = source();
  sink(a);
  return 0;
}
