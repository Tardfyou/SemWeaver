enum { EXTENT=7 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; cursor<EXTENT; ++cursor) { observe_value(enabled); if(enabled && cursor+1<EXTENT) observe_value(slots[cursor+1]); }
}
