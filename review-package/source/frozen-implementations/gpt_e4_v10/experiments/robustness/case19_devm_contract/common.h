typedef void (*cleanup_fn)(void *);
extern int devm_add_action_or_reset(void *dev, cleanup_fn action, void *data);
extern void cleanup_primary(void *data);
extern void cleanup_renamed(void *data);
extern void cleanup_other(void *data);
extern void observe(void *data);
