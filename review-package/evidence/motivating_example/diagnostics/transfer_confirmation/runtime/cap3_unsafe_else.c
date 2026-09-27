enum { EXTENT=3 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; cursor<EXTENT; ++cursor) { observe_value(enabled); if(cursor+1<EXTENT) { observe_value(0); } else { observe_value(slots[cursor+1]); } }
}

volatile int sink;
__attribute__((noinline)) void observe_value(int x) { sink ^= x; }
int main(void) { inspect_window(0); inspect_window(1); return 0; }
