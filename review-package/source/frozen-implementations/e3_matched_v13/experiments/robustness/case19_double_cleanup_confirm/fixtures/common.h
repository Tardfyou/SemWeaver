typedef void (*cleanup_fn)(void *);
extern int register_or_reset(cleanup_fn action, void *object);
extern int register_plain(cleanup_fn action, void *object);
extern void cleanup_primary(void *object);
