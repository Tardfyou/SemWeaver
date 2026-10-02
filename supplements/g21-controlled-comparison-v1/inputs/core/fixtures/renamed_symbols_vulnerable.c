enum { CAP = 8 };
struct entry { int value; };
extern void consume(struct entry *);
extern void observe(unsigned);
struct context { struct entry *items[CAP]; };
void inspect(struct context *dc) {
  for (unsigned cursor = 0; cursor < (CAP); ++cursor) {
    consume(dc->items[cursor + 1]);
  }
}
