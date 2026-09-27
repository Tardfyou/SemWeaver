enum { CAP=11 };
extern void use(int);
void f(int flag) {
  int a[CAP]={0};
  for(int i=0; i<CAP; ++i) { if(i>=CAP-1) break; use(a[i+1]); }
}
