enum { EXTENT=3 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; cursor<EXTENT; ++cursor) { observe_value(enabled); if(cursor+1<EXTENT+2) observe_value(slots[cursor+1]); }
}
