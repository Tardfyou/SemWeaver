enum { EXTENT=7 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; cursor<EXTENT-1; ++cursor) { observe_value(enabled); observe_value(slots[cursor+1]); }
}
