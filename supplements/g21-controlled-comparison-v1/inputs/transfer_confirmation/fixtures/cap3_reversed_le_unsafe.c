enum { EXTENT=3 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; EXTENT-1>=cursor; ++cursor) { observe_value(enabled); observe_value(slots[cursor+1]); }
}
