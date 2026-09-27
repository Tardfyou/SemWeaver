enum { CAP=11 };
extern void use(int);
void f(int flag) {
  int a[CAP]={0};
  for(int i=0; CAP-1>=i; ++i) { use(a[i+1]); }
}

volatile int sink;
__attribute__((noinline)) void use(int x) { sink ^= x; }
int main(void) { f(0); f(1); return 0; }
