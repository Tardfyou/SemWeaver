typedef void (*cleanup_fn)(void *);
extern int __devm_add_action_or_reset(void *device, cleanup_fn action,
                                       void *object);
extern void scmi_debugfs_common_cleanup(void *object);
extern void renamed_cleanup(void *object);
extern void observe(void *object);
