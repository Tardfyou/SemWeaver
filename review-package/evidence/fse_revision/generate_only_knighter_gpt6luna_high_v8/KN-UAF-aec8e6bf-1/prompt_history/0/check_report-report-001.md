# Instruction

Determine whether the static analyzer report is a real bug in the Linux kernel and matches the target bug pattern

Your analysis should:
- **Compare the report against the provided target bug pattern specification,** using the **buggy function (pre-patch)** and the **fix patch** as the reference.
- Explain your reasoning for classifying this as either:
  - **A true positive** (matches the target bug pattern **and** is a real bug), or
  - **A false positive** (does **not** match the target bug pattern **or** is **not** a real bug).

Please evaluate thoroughly using the following process:

- **First, understand** the reported code pattern and its control/data flow.
- **Then, compare** it against the target bug pattern characteristics.
- **Finally, validate** against the **pre-/post-patch** behavior:
  - The reported case demonstrates the same root cause pattern as the target bug pattern/function and would be addressed by a similar fix.

- **Numeric / bounds feasibility** (if applicable):
  - Infer tight **min/max** ranges for all involved variables from types, prior checks, and loop bounds.
  - Show whether overflow/underflow or OOB is actually triggerable (compute the smallest/largest values that violate constraints).

- **Null-pointer dereference feasibility** (if applicable):
  1. **Identify the pointer source** and return convention of the producing function(s) in this path (e.g., returns **NULL**, **ERR_PTR**, negative error code via cast, or never-null).
  2. **Check real-world feasibility in this specific driver/socket/filesystem/etc.**:
     - Enumerate concrete conditions under which the producer can return **NULL/ERR_PTR** here (e.g., missing DT/ACPI property, absent PCI device/function, probe ordering, hotplug/race, Kconfig options, chip revision/quirks).
     - Verify whether those conditions can occur given the driver’s init/probe sequence and the kernel helpers used.
  3. **Lifetime & concurrency**: consider teardown paths, RCU usage, refcounting (`get/put`), and whether the pointer can become invalid/NULL across yields or callbacks.
  4. If the producer is provably non-NULL in this context (by spec or preceding checks), classify as **false positive**.

If there is any uncertainty in the classification, **err on the side of caution and classify it as a false positive**. Your analysis will be used to improve the static analyzer's accuracy.

## Patch Description

btrfs: fix use-after-free of block device file in __btrfs_free_extra_devids()

Mounting btrfs from two images (which have the same one fsid and two
different dev_uuids) in certain executing order may trigger an UAF for
variable 'device->bdev_file' in __btrfs_free_extra_devids(). And
following are the details:

1. Attach image_1 to loop0, attach image_2 to loop1, and scan btrfs
   devices by ioctl(BTRFS_IOC_SCAN_DEV):

             /  btrfs_device_1 → loop0
   fs_device
             \  btrfs_device_2 → loop1
2. mount /dev/loop0 /mnt
   btrfs_open_devices
    btrfs_device_1->bdev_file = btrfs_get_bdev_and_sb(loop0)
    btrfs_device_2->bdev_file = btrfs_get_bdev_and_sb(loop1)
   btrfs_fill_super
    open_ctree
     fail: btrfs_close_devices // -ENOMEM
	    btrfs_close_bdev(btrfs_device_1)
             fput(btrfs_device_1->bdev_file)
	      // btrfs_device_1->bdev_file is freed
	    btrfs_close_bdev(btrfs_device_2)
             fput(btrfs_device_2->bdev_file)

3. mount /dev/loop1 /mnt
   btrfs_open_devices
    btrfs_get_bdev_and_sb(&bdev_file)
     // EIO, btrfs_device_1->bdev_file is not assigned,
     // which points to a freed memory area
    btrfs_device_2->bdev_file = btrfs_get_bdev_and_sb(loop1)
   btrfs_fill_super
    open_ctree
     btrfs_free_extra_devids
      if (btrfs_device_1->bdev_file)
       fput(btrfs_device_1->bdev_file) // UAF !

Fix it by setting 'device->bdev_file' as 'NULL' after closing the
btrfs_device in btrfs_close_one_device().

Fixes: 142388194191 ("btrfs: do not background blkdev_put()")
CC: stable@vger.kernel.org # 4.19+
Link: https://bugzilla.kernel.org/show_bug.cgi?id=219408
Signed-off-by: Zhihao Cheng <chengzhihao1@huawei.com>
Reviewed-by: David Sterba <dsterba@suse.com>
Signed-off-by: David Sterba <dsterba@suse.com>

## Buggy Code

```c
// Function: btrfs_close_one_device in fs/btrfs/volumes.c
static void btrfs_close_one_device(struct btrfs_device *device)
{
	struct btrfs_fs_devices *fs_devices = device->fs_devices;

	if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state) &&
	    device->devid != BTRFS_DEV_REPLACE_DEVID) {
		list_del_init(&device->dev_alloc_list);
		fs_devices->rw_devices--;
	}

	if (device->devid == BTRFS_DEV_REPLACE_DEVID)
		clear_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state);

	if (test_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state)) {
		clear_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state);
		fs_devices->missing_devices--;
	}

	btrfs_close_bdev(device);
	if (device->bdev) {
		fs_devices->open_devices--;
		device->bdev = NULL;
	}
	clear_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state);
	btrfs_destroy_dev_zone_info(device);

	device->fs_info = NULL;
	atomic_set(&device->dev_stats_ccnt, 0);
	extent_io_tree_release(&device->alloc_state);

	/*
	 * Reset the flush error record. We might have a transient flush error
	 * in this mount, and if so we aborted the current transaction and set
	 * the fs to an error state, guaranteeing no super blocks can be further
	 * committed. However that error might be transient and if we unmount the
	 * filesystem and mount it again, we should allow the mount to succeed
	 * (btrfs_check_rw_degradable() should not fail) - if after mounting the
	 * filesystem again we still get flush errors, then we will again abort
	 * any transaction and set the error state, guaranteeing no commits of
	 * unsafe super blocks.
	 */
	device->last_flush_error = 0;

	/* Verify the device is back in a pristine state  */
	WARN_ON(test_bit(BTRFS_DEV_STATE_FLUSH_SENT, &device->dev_state));
	WARN_ON(test_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state));
	WARN_ON(!list_empty(&device->dev_alloc_list));
	WARN_ON(!list_empty(&device->post_commit_list));
}
```

## Bug Fix Patch

```diff
diff --git a/fs/btrfs/volumes.c b/fs/btrfs/volumes.c
index 8f340ad1d938..eb51b609190f 100644
--- a/fs/btrfs/volumes.c
+++ b/fs/btrfs/volumes.c
@@ -1105,6 +1105,7 @@ static void btrfs_close_one_device(struct btrfs_device *device)
 	if (device->bdev) {
 		fs_devices->open_devices--;
 		device->bdev = NULL;
+		device->bdev_file = NULL;
 	}
 	clear_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state);
 	btrfs_destroy_dev_zone_info(device);
```


## Bug Pattern

A long-lived struct stores a reference-counted pointer (here, device->bdev_file) that is released (fput/close) but not cleared to NULL. Later code uses a non-NULL check on that field as a validity flag and reuses/fputs it again, leading to use-after-free. Specifically, cleanup sets related fields (device->bdev) to NULL but leaves device->bdev_file dangling, so subsequent paths that test if (device->bdev_file) will operate on freed memory.


# Report

BuildSource:| fs/btrfs/volumes.c
### Report Summary

File:| ./include/linux/list.h  
---|---  
Warning:| line 218, column 2  
released struct field not cleared before return  
Note:| line 2322, column 2  
released here  
  
### Annotated Source Code


352   |  * - filesystem or device errors leading to forced read-only
353   |  *
354   |  * The status of exclusive operation is set and cleared atomically.
355   |  * During the course of Paused state, fs_info::exclusive_operation remains set.
356   |  * A device operation in Paused or Running state can be canceled or resumed
357   |  * either by ioctl (Balance only) or when remounted as read-write.
358   |  * The exclusive status is cleared when the device operation is canceled or
359   |  * completed.
360   |  */
361   |  
362   | DEFINE_MUTEX(uuid_mutex);
363   | static LIST_HEAD(fs_uuids);
364   | struct list_head * __attribute_const__ btrfs_get_fs_uuids(void)
365   | {
366   |  return &fs_uuids;
367   | }
368   |  
369   | /*
370   |  * Allocate new btrfs_fs_devices structure identified by a fsid.
371   |  *
372   |  * @fsid:    if not NULL, copy the UUID to fs_devices::fsid and to
373   |  *           fs_devices::metadata_fsid
374   |  *
375   |  * Return a pointer to a new struct btrfs_fs_devices on success, or ERR_PTR().
376   |  * The returned struct is not linked onto any lists and can be destroyed with
377   |  * kfree() right away.
378   |  */
379   | static struct btrfs_fs_devices *alloc_fs_devices(const u8 *fsid)
380   | {
381   |  struct btrfs_fs_devices *fs_devs;
382   |  
383   | 	fs_devs = kzalloc(sizeof(*fs_devs), GFP_KERNEL);
384   |  if (!fs_devs)
385   |  return ERR_PTR(-ENOMEM);
386   |  
387   |  mutex_init(&fs_devs->device_list_mutex);
388   |  
389   | 	INIT_LIST_HEAD(&fs_devs->devices);
390   | 	INIT_LIST_HEAD(&fs_devs->alloc_list);
391   | 	INIT_LIST_HEAD(&fs_devs->fs_list);
392   | 	INIT_LIST_HEAD(&fs_devs->seed_list);
393   |  
394   |  if (fsid) {
395   |  memcpy(fs_devs->fsid, fsid, BTRFS_FSID_SIZE);
396   |  memcpy(fs_devs->metadata_uuid, fsid, BTRFS_FSID_SIZE);
397   | 	}
398   |  
399   |  return fs_devs;
400   | }
401   |  
402   | static void btrfs_free_device(struct btrfs_device *device)
403   | {
404   |  WARN_ON(!list_empty(&device->post_commit_list));
405   | 	rcu_string_free(device->name);
406   | 	extent_io_tree_release(&device->alloc_state);
407   | 	btrfs_destroy_dev_zone_info(device);
408   | 	kfree(device);
409   | }
410   |  
411   | static void free_fs_devices(struct btrfs_fs_devices *fs_devices)
412   | {
413   |  struct btrfs_device *device;
414   |  
415   |  WARN_ON(fs_devices->opened);
416   |  while (!list_empty(&fs_devices->devices)) {
417   | 		device = list_entry(fs_devices->devices.next,
418   |  struct btrfs_device, dev_list);
419   | 		list_del(&device->dev_list);
420   | 		btrfs_free_device(device);
421   | 	}
422   | 	kfree(fs_devices);
423   | }
424   |  
425   | void __exit btrfs_cleanup_fs_uuids(void)
426   | {
427   |  struct btrfs_fs_devices *fs_devices;
428   |  
429   |  while (!list_empty(&fs_uuids)) {
430   | 		fs_devices = list_entry(fs_uuids.next,
431   |  struct btrfs_fs_devices, fs_list);
432   | 		list_del(&fs_devices->fs_list);
433   | 		free_fs_devices(fs_devices);
434   | 	}
435   | }
436   |  
437   | static bool match_fsid_fs_devices(const struct btrfs_fs_devices *fs_devices,
438   |  const u8 *fsid, const u8 *metadata_fsid)
1023  | 				*latest_dev = device;
1024  | 			}
1025  |  continue;
1026  | 		}
1027  |  
1028  |  /*
1029  |  * We have already validated the presence of BTRFS_DEV_REPLACE_DEVID,
1030  |  * in btrfs_init_dev_replace() so just continue.
1031  |  */
1032  |  if (device->devid == BTRFS_DEV_REPLACE_DEVID)
1033  |  continue;
1034  |  
1035  |  if (device->bdev_file) {
1036  | 			fput(device->bdev_file);
1037  | 			device->bdev = NULL;
1038  | 			device->bdev_file = NULL;
1039  | 			fs_devices->open_devices--;
1040  | 		}
1041  |  if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state)) {
1042  | 			list_del_init(&device->dev_alloc_list);
1043  | 			clear_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state);
1044  | 			fs_devices->rw_devices--;
1045  | 		}
1046  | 		list_del_init(&device->dev_list);
1047  | 		fs_devices->num_devices--;
1048  | 		btrfs_free_device(device);
1049  | 	}
1050  |  
1051  | }
1052  |  
1053  | /*
1054  |  * After we have read the system tree and know devids belonging to this
1055  |  * filesystem, remove the device which does not belong there.
1056  |  */
1057  | void btrfs_free_extra_devids(struct btrfs_fs_devices *fs_devices)
1058  | {
1059  |  struct btrfs_device *latest_dev = NULL;
1060  |  struct btrfs_fs_devices *seed_dev;
1061  |  
1062  |  mutex_lock(&uuid_mutex);
1063  | 	__btrfs_free_extra_devids(fs_devices, &latest_dev);
1064  |  
1065  |  list_for_each_entry(seed_dev, &fs_devices->seed_list, seed_list)
1066  | 		__btrfs_free_extra_devids(seed_dev, &latest_dev);
1067  |  
1068  | 	fs_devices->latest_dev = latest_dev;
1069  |  
1070  | 	mutex_unlock(&uuid_mutex);
1071  | }
1072  |  
1073  | static void btrfs_close_bdev(struct btrfs_device *device)
1074  | {
1075  |  if (!device->bdev)
1076  |  return;
1077  |  
1078  |  if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state)) {
1079  | 		sync_blockdev(device->bdev);
1080  | 		invalidate_bdev(device->bdev);
1081  | 	}
1082  |  
1083  | 	fput(device->bdev_file);
1084  | }
1085  |  
1086  | static void btrfs_close_one_device(struct btrfs_device *device)
1087  | {
1088  |  struct btrfs_fs_devices *fs_devices = device->fs_devices;
1089  |  
1090  |  if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state) &&
1091  | 	    device->devid != BTRFS_DEV_REPLACE_DEVID) {
1092  | 		list_del_init(&device->dev_alloc_list);
1093  | 		fs_devices->rw_devices--;
1094  | 	}
1095  |  
1096  |  if (device->devid == BTRFS_DEV_REPLACE_DEVID)
1097  | 		clear_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state);
1098  |  
1099  |  if (test_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state)) {
1100  | 		clear_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state);
1101  | 		fs_devices->missing_devices--;
1102  | 	}
1103  |  
1104  | 	btrfs_close_bdev(device);
1105  |  if (device->bdev) {
1106  | 		fs_devices->open_devices--;
1107  | 		device->bdev = NULL;
1108  | 		device->bdev_file = NULL;
1109  | 	}
1110  | 	clear_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state);
1111  | 	btrfs_destroy_dev_zone_info(device);
1112  |  
1113  | 	device->fs_info = NULL;
2266  |  */
2267  |  if (cur_devices->num_devices == 0) {
2268  | 		list_del_init(&cur_devices->seed_list);
2269  |  ASSERT(cur_devices->opened == 1);
2270  | 		cur_devices->opened--;
2271  | 		free_fs_devices(cur_devices);
2272  | 	}
2273  |  
2274  | 	ret = btrfs_commit_transaction(trans);
2275  |  
2276  |  return ret;
2277  |  
2278  | error_undo:
2279  |  if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state)) {
2280  |  mutex_lock(&fs_info->chunk_mutex);
2281  | 		list_add(&device->dev_alloc_list,
2282  | 			 &fs_devices->alloc_list);
2283  | 		device->fs_devices->rw_devices++;
2284  | 		mutex_unlock(&fs_info->chunk_mutex);
2285  | 	}
2286  |  return ret;
2287  | }
2288  |  
2289  | void btrfs_rm_dev_replace_remove_srcdev(struct btrfs_device *srcdev)
2290  | {
2291  |  struct btrfs_fs_devices *fs_devices;
2292  |  
2293  |  lockdep_assert_held(&srcdev->fs_info->fs_devices->device_list_mutex);
2294  |  
2295  |  /*
2296  |  * in case of fs with no seed, srcdev->fs_devices will point
2297  |  * to fs_devices of fs_info. However when the dev being replaced is
2298  |  * a seed dev it will point to the seed's local fs_devices. In short
2299  |  * srcdev will have its correct fs_devices in both the cases.
2300  |  */
2301  | 	fs_devices = srcdev->fs_devices;
2302  |  
2303  | 	list_del_rcu(&srcdev->dev_list);
2304  | 	list_del(&srcdev->dev_alloc_list);
2305  | 	fs_devices->num_devices--;
2306  |  if (test_bit(BTRFS_DEV_STATE_MISSING, &srcdev->dev_state))
2307  | 		fs_devices->missing_devices--;
2308  |  
2309  |  if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &srcdev->dev_state))
2310  | 		fs_devices->rw_devices--;
2311  |  
2312  |  if (srcdev->bdev)
2313  | 		fs_devices->open_devices--;
2314  | }
2315  |  
2316  | void btrfs_rm_dev_replace_free_srcdev(struct btrfs_device *srcdev)
2317  | {
2318  |  struct btrfs_fs_devices *fs_devices = srcdev->fs_devices;
2319  |  
2320  |  mutex_lock(&uuid_mutex);
2321  |  
2322  | 	btrfs_close_bdev(srcdev);
2323  | 	synchronize_rcu();
2324  | 	btrfs_free_device(srcdev);
2325  |  
2326  |  /* if this is no devs we rather delete the fs_devices */
2327  |  if (!fs_devices->num_devices) {
    1Assuming field 'num_devices' is 0→
    2←Taking true branch→
2328  |  /*
2329  |  * On a mounted FS, num_devices can't be zero unless it's a
2330  |  * seed. In case of a seed device being replaced, the replace
2331  |  * target added to the sprout FS, so there will be no more
2332  |  * device left under the seed FS.
2333  |  */
2334  |  ASSERT(fs_devices->seeding);
    3←Assuming field 'seeding' is true→
    4←'?' condition is true→
2335  |  
2336  |  list_del_init(&fs_devices->seed_list);
    5←Calling 'list_del_init'→
2337  | 		close_fs_devices(fs_devices);
2338  | 		free_fs_devices(fs_devices);
2339  | 	}
2340  | 	mutex_unlock(&uuid_mutex);
2341  | }
2342  |  
2343  | void btrfs_destroy_dev_replace_tgtdev(struct btrfs_device *tgtdev)
2344  | {
2345  |  struct btrfs_fs_devices *fs_devices = tgtdev->fs_info->fs_devices;
2346  |  
2347  |  mutex_lock(&fs_devices->device_list_mutex);
2348  |  
2349  | 	btrfs_sysfs_remove_device(tgtdev);
2350  |  
2351  |  if (tgtdev->bdev)
2352  | 		fs_devices->open_devices--;
2353  |  
2354  | 	fs_devices->num_devices--;
2355  |  
2356  | 	btrfs_assign_next_active_device(tgtdev, NULL);
2357  |  
2358  | 	list_del_rcu(&tgtdev->dev_list);
2359  |  
2360  | 	mutex_unlock(&fs_devices->device_list_mutex);
2361  |  
2362  | 	btrfs_scratch_superblocks(tgtdev->fs_info, tgtdev);
2363  |  
2364  | 	btrfs_close_bdev(tgtdev);
2365  | 	synchronize_rcu();
2366  | 	btrfs_free_device(tgtdev);
56    |  
57    | /*
58    |  * Performs list corruption checks before __list_add(). Returns false if a
59    |  * corruption is detected, true otherwise.
60    |  *
61    |  * With CONFIG_LIST_HARDENED only, performs minimal list integrity checking
62    |  * inline to catch non-faulting corruptions, and only if a corruption is
63    |  * detected calls the reporting function __list_add_valid_or_report().
64    |  */
65    | static __always_inline bool __list_add_valid(struct list_head *new,
66    |  struct list_head *prev,
67    |  struct list_head *next)
68    | {
69    | 	bool ret = true;
70    |  
71    |  if (!IS_ENABLED(CONFIG_DEBUG_LIST)) {
72    |  /*
73    |  * With the hardening version, elide checking if next and prev
74    |  * are NULL, since the immediate dereference of them below would
75    |  * result in a fault if NULL.
76    |  *
77    |  * With the reduced set of checks, we can afford to inline the
78    |  * checks, which also gives the compiler a chance to elide some
79    |  * of them completely if they can be proven at compile-time. If
80    |  * one of the pre-conditions does not hold, the slow-path will
81    |  * show a report which pre-condition failed.
82    |  */
83    |  if (likely(next->prev == prev && prev->next == next && new != prev && new != next))
84    |  return true;
85    | 		ret = false;
86    | 	}
87    |  
88    | 	ret &= __list_add_valid_or_report(new, prev, next);
89    |  return ret;
90    | }
91    |  
92    | /*
93    |  * Performs the full set of list corruption checks before __list_del_entry().
94    |  * On list corruption reports a warning, and returns false.
95    |  */
96    | extern bool __list_valid_slowpath __list_del_entry_valid_or_report(struct list_head *entry);
97    |  
98    | /*
99    |  * Performs list corruption checks before __list_del_entry(). Returns false if a
100   |  * corruption is detected, true otherwise.
101   |  *
102   |  * With CONFIG_LIST_HARDENED only, performs minimal list integrity checking
103   |  * inline to catch non-faulting corruptions, and only if a corruption is
104   |  * detected calls the reporting function __list_del_entry_valid_or_report().
105   |  */
106   | static __always_inline bool __list_del_entry_valid(struct list_head *entry)
107   | {
108   | 	bool ret = true;
109   |  
110   |  if (!IS_ENABLED(CONFIG_DEBUG_LIST)) {
111   |  struct list_head *prev = entry->prev;
112   |  struct list_head *next = entry->next;
113   |  
114   |  /*
115   |  * With the hardening version, elide checking if next and prev
116   |  * are NULL, LIST_POISON1 or LIST_POISON2, since the immediate
117   |  * dereference of them below would result in a fault.
118   |  */
119   |  if (likely(prev->next == entry && next->prev == entry))
120   |  return true;
121   | 		ret = false;
122   | 	}
123   |  
124   | 	ret &= __list_del_entry_valid_or_report(entry);
125   |  return ret;
126   | }
127   | #else
128   | static inline bool __list_add_valid(struct list_head *new,
129   |  struct list_head *prev,
130   |  struct list_head *next)
131   | {
132   |  return true;
133   | }
134   | static inline bool __list_del_entry_valid(struct list_head *entry)
135   | {
136   |  return true;
137   | }
138   | #endif
139   |  
140   | /*
141   |  * Insert a new entry between two known consecutive entries.
142   |  *
143   |  * This is only for internal list manipulation where we know
144   |  * the prev/next entries already!
145   |  */
146   | static inline void __list_add(struct list_head *new,
147   |  struct list_head *prev,
148   |  struct list_head *next)
149   | {
150   |  if (!__list_add_valid(new, prev, next))
151   |  return;
152   |  
153   | 	next->prev = new;
154   | 	new->next = next;
155   | 	new->prev = prev;
163   |  *
164   |  * Insert a new entry after the specified head.
165   |  * This is good for implementing stacks.
166   |  */
167   | static inline void list_add(struct list_head *new, struct list_head *head)
168   | {
169   | 	__list_add(new, head, head->next);
170   | }
171   |  
172   |  
173   | /**
174   |  * list_add_tail - add a new entry
175   |  * @new: new entry to be added
176   |  * @head: list head to add it before
177   |  *
178   |  * Insert a new entry before the specified head.
179   |  * This is useful for implementing queues.
180   |  */
181   | static inline void list_add_tail(struct list_head *new, struct list_head *head)
182   | {
183   | 	__list_add(new, head->prev, head);
184   | }
185   |  
186   | /*
187   |  * Delete a list entry by making the prev/next entries
188   |  * point to each other.
189   |  *
190   |  * This is only for internal list manipulation where we know
191   |  * the prev/next entries already!
192   |  */
193   | static inline void __list_del(struct list_head * prev, struct list_head * next)
194   | {
195   | 	next->prev = prev;
196   |  WRITE_ONCE(prev->next, next);
197   | }
198   |  
199   | /*
200   |  * Delete a list entry and clear the 'prev' pointer.
201   |  *
202   |  * This is a special-purpose list clearing method used in the networking code
203   |  * for lists allocated as per-cpu, where we don't want to incur the extra
204   |  * WRITE_ONCE() overhead of a regular list_del_init(). The code that uses this
205   |  * needs to check the node 'prev' pointer instead of calling list_empty().
206   |  */
207   | static inline void __list_del_clearprev(struct list_head *entry)
208   | {
209   | 	__list_del(entry->prev, entry->next);
210   | 	entry->prev = NULL;
211   | }
212   |  
213   | static inline void __list_del_entry(struct list_head *entry)
214   | {
215   |  if (!__list_del_entry_valid(entry))
    7←Assuming the condition is false→
    8←Taking false branch→
216   |  return;
217   |  
218   |  __list_del(entry->prev, entry->next);
    9←released struct field not cleared before return
219   | }
220   |  
221   | /**
222   |  * list_del - deletes entry from list.
223   |  * @entry: the element to delete from the list.
224   |  * Note: list_empty() on entry does not return true after this, the entry is
225   |  * in an undefined state.
226   |  */
227   | static inline void list_del(struct list_head *entry)
228   | {
229   | 	__list_del_entry(entry);
230   | 	entry->next = LIST_POISON1;
231   | 	entry->prev = LIST_POISON2;
232   | }
233   |  
234   | /**
235   |  * list_replace - replace old entry by new one
236   |  * @old : the element to be replaced
237   |  * @new : the new element to insert
238   |  *
239   |  * If @old was empty, it will be overwritten.
240   |  */
241   | static inline void list_replace(struct list_head *old,
242   |  struct list_head *new)
243   | {
244   | 	new->next = old->next;
245   | 	new->next->prev = new;
246   | 	new->prev = old->prev;
247   | 	new->prev->next = new;
248   | }
249   |  
250   | /**
251   |  * list_replace_init - replace old entry by new one and initialize the old one
252   |  * @old : the element to be replaced
253   |  * @new : the new element to insert
254   |  *
255   |  * If @old was empty, it will be overwritten.
256   |  */
257   | static inline void list_replace_init(struct list_head *old,
258   |  struct list_head *new)
259   | {
260   | 	list_replace(old, new);
261   | 	INIT_LIST_HEAD(old);
262   | }
263   |  
264   | /**
265   |  * list_swap - replace entry1 with entry2 and re-add entry1 at entry2's position
266   |  * @entry1: the location to place entry2
267   |  * @entry2: the location to place entry1
268   |  */
269   | static inline void list_swap(struct list_head *entry1,
270   |  struct list_head *entry2)
271   | {
272   |  struct list_head *pos = entry2->prev;
273   |  
274   | 	list_del(entry2);
275   | 	list_replace(entry1, entry2);
276   |  if (pos == entry1)
277   | 		pos = entry2;
278   | 	list_add(entry1, pos);
279   | }
280   |  
281   | /**
282   |  * list_del_init - deletes entry from list and reinitialize it.
283   |  * @entry: the element to delete from the list.
284   |  */
285   | static inline void list_del_init(struct list_head *entry)
286   | {
287   |  __list_del_entry(entry);
    6←Calling '__list_del_entry'→
288   | 	INIT_LIST_HEAD(entry);
289   | }
290   |  
291   | /**
292   |  * list_move - delete from one list and add as another's head
293   |  * @list: the entry to move
294   |  * @head: the head that will precede our entry
295   |  */
296   | static inline void list_move(struct list_head *list, struct list_head *head)
297   | {
298   | 	__list_del_entry(list);
299   | 	list_add(list, head);
300   | }
301   |  
302   | /**
303   |  * list_move_tail - delete from one list and add as another's tail
304   |  * @list: the entry to move
305   |  * @head: the head that will follow our entry
306   |  */
307   | static inline void list_move_tail(struct list_head *list,
308   |  struct list_head *head)
309   | {
310   | 	__list_del_entry(list);
311   | 	list_add_tail(list, head);
312   | }
313   |  
314   | /**
315   |  * list_bulk_move_tail - move a subsection of a list to its tail
316   |  * @head: the head that will follow our entry
317   |  * @first: first entry to move
321   |  * All three entries must belong to the same linked list.
322   |  */
323   | static inline void list_bulk_move_tail(struct list_head *head,
324   |  struct list_head *first,
325   |  struct list_head *last)
326   | {
327   | 	first->prev->next = last->next;
328   | 	last->next->prev = first->prev;
329   |  
330   | 	head->prev->next = first;
331   | 	first->prev = head->prev;
332   |  
333   | 	last->next = head;
334   | 	head->prev = last;
335   | }
336   |  
337   | /**
338   |  * list_is_first -- tests whether @list is the first entry in list @head
339   |  * @list: the entry to test
340   |  * @head: the head of the list
341   |  */
342   | static inline int list_is_first(const struct list_head *list, const struct list_head *head)
343   | {
344   |  return list->prev == head;
345   | }
346   |  
347   | /**
348   |  * list_is_last - tests whether @list is the last entry in list @head
349   |  * @list: the entry to test
350   |  * @head: the head of the list
351   |  */
352   | static inline int list_is_last(const struct list_head *list, const struct list_head *head)
353   | {
354   |  return list->next == head;
355   | }
356   |  
357   | /**
358   |  * list_is_head - tests whether @list is the list @head
359   |  * @list: the entry to test
360   |  * @head: the head of the list
361   |  */
362   | static inline int list_is_head(const struct list_head *list, const struct list_head *head)
363   | {
364   |  return list == head;
365   | }
366   |  
367   | /**
368   |  * list_empty - tests whether a list is empty
369   |  * @head: the list to test.
370   |  */
371   | static inline int list_empty(const struct list_head *head)
372   | {
373   |  return READ_ONCE(head->next) == head;
374   | }
375   |  
376   | /**
377   |  * list_del_init_careful - deletes entry from list and reinitialize it.
378   |  * @entry: the element to delete from the list.
379   |  *
380   |  * This is the same as list_del_init(), except designed to be used
381   |  * together with list_empty_careful() in a way to guarantee ordering
382   |  * of other memory operations.
383   |  *
384   |  * Any memory operations done before a list_del_init_careful() are
385   |  * guaranteed to be visible after a list_empty_careful() test.
386   |  */
387   | static inline void list_del_init_careful(struct list_head *entry)
388   | {
389   | 	__list_del_entry(entry);
390   |  WRITE_ONCE(entry->prev, entry);
391   |  smp_store_release(&entry->next, entry);
392   | }
393   |  
394   | /**
395   |  * list_empty_careful - tests whether a list is empty and not being modified
396   |  * @head: the list to test
397   |  *
398   |  * Description:
399   |  * tests whether a list is empty _and_ checks that no other CPU might be
400   |  * in the process of modifying either member (next or prev)
401   |  *
402   |  * NOTE: using list_empty_careful() without synchronization
403   |  * can only be safe if the only activity that can happen

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
