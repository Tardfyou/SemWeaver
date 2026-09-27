enum { CAP = 8 };
struct link { int value; };
extern void consume(struct link *);
extern void observe(unsigned);
void check_links(struct link *seed) {
  struct link *links[CAP] = {0};
  links[0] = seed;
  for (unsigned i = 0; i < (CAP - 1); ++i) {
    consume(links[i + 1]);
  }
}
