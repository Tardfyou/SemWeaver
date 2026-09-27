enum { EXTENT=19 };
extern void observe_value(int);
void inspect_window(int enabled) {
  int slots[EXTENT]={0};
  for(int cursor=0; cursor<EXTENT; ++cursor) { observe_value(enabled); if(enabled) { if(cursor>=EXTENT-1) break; } observe_value(slots[cursor+1]); }
}
