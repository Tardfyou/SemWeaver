enum { CAP=11 };
extern void use(int);
void f(int flag) {
  int a[CAP]={0};
  for(int i=0; CAP-2>=i; ++i) { use(a[i+1]); }
}
