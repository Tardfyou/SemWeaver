enum { CAP=8 }; extern void use(int);
void f(int flag) { int a[CAP]; for(int i=0;i<CAP;++i) if(i+1<CAP || flag) use(a[i+1]); }
