enum { EXTENT=19 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; cursor<=EXTENT-1; ++cursor) { observe_value(enabled); observe_value(slots[cursor+1]); }
}

volatile int sink;
__attribute__((noinline)) void observe_value(int x) { sink ^= x; }
int main(void) { inspect_window(0); inspect_window(1); return 0; }
