enum { CAP = 17 };
struct entry { int value; };
extern void consume(struct entry *);
extern void observe(unsigned);
struct context { struct entry *items[CAP]; };
void inspect(struct context *dc) {
  for (unsigned cursor = 0; (CAP - 1) > cursor; cursor++) {
    observe(cursor);
    {
    consume(dc->items[1 + cursor]);
    }
  }
}
