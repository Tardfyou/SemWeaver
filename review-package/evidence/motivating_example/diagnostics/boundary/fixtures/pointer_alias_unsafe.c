enum { CAP=8 }; extern void use(int);
void f(void) { int storage[CAP]; int *p=storage; for(int i=0;i<(CAP+1)-1;++i) use(p[i+1]); }
