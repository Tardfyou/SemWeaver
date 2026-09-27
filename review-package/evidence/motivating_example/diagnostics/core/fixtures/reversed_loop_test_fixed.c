enum { CAP = 8 };
struct link { int value; };
extern void consume(struct link *);
extern void observe(unsigned);
struct context { struct link *links[CAP]; };
void check_links(struct context *dc) {
  for (unsigned i = 0; (CAP - 1) > i; ++i) {
    consume(dc->links[i + 1]);
  }
}
