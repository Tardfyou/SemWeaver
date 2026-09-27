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

File:| fs/btrfs/volumes.c  
---|---  
Warning:| line 422, column 2  
released struct field not cleared before return  
Note:| line 1104, column 2  
released here  
  
### Annotated Source Code


329   |  *
330   |  * Maintains the exclusivity of the following operations that apply to the
331   |  * whole filesystem and cannot run in parallel.
332   |  *
333   |  * - Balance (*)
334   |  * - Device add
335   |  * - Device remove
336   |  * - Device replace (*)
337   |  * - Resize
338   |  *
339   |  * The device operations (as above) can be in one of the following states:
340   |  *
341   |  * - Running state
342   |  * - Paused state
343   |  * - Completed state
344   |  *
345   |  * Only device operations marked with (*) can go into the Paused state for the
346   |  * following reasons:
347   |  *
348   |  * - ioctl (only Balance can be Paused through ioctl)
349   |  * - filesystem remounted as read-only
350   |  * - filesystem unmounted and mounted as read-only
351   |  * - system power-cycle and filesystem mounted as read-only
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
    28←Taking false branch→
416   |  while (!list_empty(&fs_devices->devices)) {
    29←Loop condition is false. Execution continues on line 422→
417   | 		device = list_entry(fs_devices->devices.next,
418   |  struct btrfs_device, dev_list);
419   | 		list_del(&device->dev_list);
420   | 		btrfs_free_device(device);
421   | 	}
422   |  kfree(fs_devices);
    30←released struct field not cleared before return
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
439   | {
440   |  if (memcmp(fsid, fs_devices->fsid, BTRFS_FSID_SIZE) != 0)
441   |  return false;
442   |  
443   |  if (!metadata_fsid)
444   |  return true;
445   |  
446   |  if (memcmp(metadata_fsid, fs_devices->metadata_uuid, BTRFS_FSID_SIZE) != 0)
447   |  return false;
448   |  
449   |  return true;
450   | }
451   |  
452   | static noinline struct btrfs_fs_devices *find_fsid(
453   |  const u8 *fsid, const u8 *metadata_fsid)
454   | {
455   |  struct btrfs_fs_devices *fs_devices;
456   |  
457   |  ASSERT(fsid);
458   |  
459   |  /* Handle non-split brain cases */
460   |  list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
461   |  if (match_fsid_fs_devices(fs_devices, fsid, metadata_fsid))
462   |  return fs_devices;
463   | 	}
464   |  return NULL;
465   | }
466   |  
467   | static int
468   | btrfs_get_bdev_and_sb(const char *device_path, blk_mode_t flags, void *holder,
469   |  int flush, struct file **bdev_file,
470   |  struct btrfs_super_block **disk_super)
471   | {
472   |  struct block_device *bdev;
473   |  int ret;
474   |  
475   | 	*bdev_file = bdev_file_open_by_path(device_path, flags, holder, NULL);
476   |  
477   |  if (IS_ERR(*bdev_file)) {
478   | 		ret = PTR_ERR(*bdev_file);
479   |  btrfs_err(NULL, "failed to open device for path %s with flags 0x%x: %d",
480   |  device_path, flags, ret);
481   |  goto error;
482   | 	}
483   | 	bdev = file_bdev(*bdev_file);
484   |  
485   |  if (flush)
486   | 		sync_blockdev(bdev);
487   |  if (holder) {
488   | 		ret = set_blocksize(*bdev_file, BTRFS_BDEV_BLOCKSIZE);
489   |  if (ret) {
490   | 			fput(*bdev_file);
491   |  goto error;
492   | 		}
905   |  */
906   |  if (device->bdev) {
907   |  if (device->devt != path_devt) {
908   | 				mutex_unlock(&fs_devices->device_list_mutex);
909   |  btrfs_warn_in_rcu(NULL,
910   |  "duplicate device %s devid %llu generation %llu scanned by %s (%d)",
911   |  path, devid, found_transid,
912   |  current->comm,
913   |  task_pid_nr(current));
914   |  return ERR_PTR(-EEXIST);
915   | 			}
916   |  btrfs_info_in_rcu(NULL,
917   |  "devid %llu device path %s changed to %s scanned by %s (%d)",
918   |  devid, btrfs_dev_name(device),
919   |  path, current->comm,
920   |  task_pid_nr(current));
921   | 		}
922   |  
923   | 		name = rcu_string_strdup(path, GFP_NOFS);
924   |  if (!name) {
925   | 			mutex_unlock(&fs_devices->device_list_mutex);
926   |  return ERR_PTR(-ENOMEM);
927   | 		}
928   | 		rcu_string_free(device->name);
929   |  rcu_assign_pointer(device->name, name);
930   |  if (test_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state)) {
931   | 			fs_devices->missing_devices--;
932   | 			clear_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state);
933   | 		}
934   | 		device->devt = path_devt;
935   | 	}
936   |  
937   |  /*
938   |  * Unmount does not free the btrfs_device struct but would zero
939   |  * generation along with most of the other members. So just update
940   |  * it back. We need it to pick the disk with largest generation
941   |  * (as above).
942   |  */
943   |  if (!fs_devices->opened) {
944   | 		device->generation = found_transid;
945   | 		fs_devices->latest_generation = max_t(u64, found_transid,
946   |  fs_devices->latest_generation);
947   | 	}
948   |  
949   | 	fs_devices->total_devices = btrfs_super_num_devices(disk_super);
950   |  
951   | 	mutex_unlock(&fs_devices->device_list_mutex);
952   |  return device;
953   | }
954   |  
955   | static struct btrfs_fs_devices *clone_fs_devices(struct btrfs_fs_devices *orig)
956   | {
957   |  struct btrfs_fs_devices *fs_devices;
958   |  struct btrfs_device *device;
959   |  struct btrfs_device *orig_dev;
960   |  int ret = 0;
961   |  
962   |  lockdep_assert_held(&uuid_mutex);
963   |  
964   | 	fs_devices = alloc_fs_devices(orig->fsid);
965   |  if (IS_ERR(fs_devices))
966   |  return fs_devices;
967   |  
968   | 	fs_devices->total_devices = orig->total_devices;
969   |  
970   |  list_for_each_entry(orig_dev, &orig->devices, dev_list) {
971   |  const char *dev_path = NULL;
972   |  
973   |  /*
974   |  * This is ok to do without RCU read locked because we hold the
975   |  * uuid mutex so nothing we touch in here is going to disappear.
976   |  */
977   |  if (orig_dev->name)
978   | 			dev_path = orig_dev->name->str;
979   |  
980   | 		device = btrfs_alloc_device(NULL, &orig_dev->devid,
981   | 					    orig_dev->uuid, dev_path);
982   |  if (IS_ERR(device)) {
983   | 			ret = PTR_ERR(device);
984   |  goto error;
985   | 		}
986   |  
987   |  if (orig_dev->zone_info) {
988   |  struct btrfs_zoned_device_info *zone_info;
989   |  
990   | 			zone_info = btrfs_clone_dev_zone_info(orig_dev);
991   |  if (!zone_info) {
992   | 				btrfs_free_device(device);
993   | 				ret = -ENOMEM;
994   |  goto error;
995   | 			}
996   | 			device->zone_info = zone_info;
997   | 		}
998   |  
999   | 		list_add(&device->dev_list, &fs_devices->devices);
1000  | 		device->fs_devices = fs_devices;
1001  | 		fs_devices->num_devices++;
1002  | 	}
1003  |  return fs_devices;
1004  | error:
1005  | 	free_fs_devices(fs_devices);
1006  |  return ERR_PTR(ret);
1007  | }
1008  |  
1009  | static void __btrfs_free_extra_devids(struct btrfs_fs_devices *fs_devices,
1010  |  struct btrfs_device **latest_dev)
1011  | {
1012  |  struct btrfs_device *device, *next;
1013  |  
1014  |  /* This is the initialized path, it is safe to release the devices. */
1015  |  list_for_each_entry_safe(device, next, &fs_devices->devices, dev_list) {
1016  |  if (test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &device->dev_state)) {
1017  |  if (!test_bit(BTRFS_DEV_STATE_REPLACE_TGT,
1018  |  &device->dev_state) &&
1019  | 			    !test_bit(BTRFS_DEV_STATE_MISSING,
1020  |  &device->dev_state) &&
1021  | 			    (!*latest_dev ||
1022  | 			     device->generation > (*latest_dev)->generation)) {
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
1114  | 	atomic_set(&device->dev_stats_ccnt, 0);
1115  | 	extent_io_tree_release(&device->alloc_state);
1116  |  
1117  |  /*
1118  |  * Reset the flush error record. We might have a transient flush error
1119  |  * in this mount, and if so we aborted the current transaction and set
1120  |  * the fs to an error state, guaranteeing no super blocks can be further
1121  |  * committed. However that error might be transient and if we unmount the
1122  |  * filesystem and mount it again, we should allow the mount to succeed
1123  |  * (btrfs_check_rw_degradable() should not fail) - if after mounting the
1124  |  * filesystem again we still get flush errors, then we will again abort
1125  |  * any transaction and set the error state, guaranteeing no commits of
1126  |  * unsafe super blocks.
1127  |  */
1128  | 	device->last_flush_error = 0;
1129  |  
1130  |  /* Verify the device is back in a pristine state  */
1131  |  WARN_ON(test_bit(BTRFS_DEV_STATE_FLUSH_SENT, &device->dev_state));
1132  |  WARN_ON(test_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state));
1133  |  WARN_ON(!list_empty(&device->dev_alloc_list));
1134  |  WARN_ON(!list_empty(&device->post_commit_list));
1135  | }
1136  |  
1137  | static void close_fs_devices(struct btrfs_fs_devices *fs_devices)
1138  | {
1139  |  struct btrfs_device *device, *tmp;
1140  |  
1141  |  lockdep_assert_held(&uuid_mutex);
1142  |  
1143  |  if (--fs_devices->opened > 0)
1144  |  return;
1145  |  
1146  |  list_for_each_entry_safe(device, tmp, &fs_devices->devices, dev_list)
1147  | 		btrfs_close_one_device(device);
1148  |  
1149  |  WARN_ON(fs_devices->open_devices);
1150  |  WARN_ON(fs_devices->rw_devices);
1151  | 	fs_devices->opened = 0;
1152  | 	fs_devices->seeding = false;
1153  | 	fs_devices->fs_info = NULL;
1154  | }
1155  |  
1156  | void btrfs_close_devices(struct btrfs_fs_devices *fs_devices)
1157  | {
1158  |  LIST_HEAD(list);
1159  |  struct btrfs_fs_devices *tmp;
1160  |  
1161  |  mutex_lock(&uuid_mutex);
1162  | 	close_fs_devices(fs_devices);
1163  |  if (!fs_devices->opened) {
1164  | 		list_splice_init(&fs_devices->seed_list, &list);
1165  |  
1166  |  /*
1167  |  * If the struct btrfs_fs_devices is not assembled with any
1168  |  * other device, it can be re-initialized during the next mount
1169  |  * without the needing device-scan step. Therefore, it can be
1170  |  * fully freed.
1171  |  */
1172  |  if (fs_devices->num_devices == 1) {
1173  | 			list_del(&fs_devices->fs_list);
1174  | 			free_fs_devices(fs_devices);
1175  | 		}
1176  | 	}
1177  |  
1178  |  
1179  |  list_for_each_entry_safe(fs_devices, tmp, &list, seed_list) {
1180  | 		close_fs_devices(fs_devices);
1181  | 		list_del(&fs_devices->seed_list);
1182  | 		free_fs_devices(fs_devices);
1183  | 	}
1184  | 	mutex_unlock(&uuid_mutex);
1185  | }
1186  |  
1187  | static int open_fs_devices(struct btrfs_fs_devices *fs_devices,
1188  | 				blk_mode_t flags, void *holder)
1189  | {
1190  |  struct btrfs_device *device;
1191  |  struct btrfs_device *latest_dev = NULL;
1192  |  struct btrfs_device *tmp_device;
1193  |  int ret = 0;
1194  |  
1195  |  list_for_each_entry_safe(device, tmp_device, &fs_devices->devices,
1196  |  dev_list) {
1197  |  int ret2;
1198  |  
1199  | 		ret2 = btrfs_open_one_device(fs_devices, device, flags, holder);
1200  |  if (ret2 == 0 &&
1201  | 		    (!latest_dev || device->generation > latest_dev->generation)) {
1202  | 			latest_dev = device;
1203  | 		} else if (ret2 == -ENODATA) {
1204  | 			fs_devices->num_devices--;
1205  | 			list_del(&device->dev_list);
1206  | 			btrfs_free_device(device);
1207  | 		}
1208  |  if (ret == 0 && ret2 != 0)
1209  | 			ret = ret2;
1210  | 	}
1211  |  
1212  |  if (fs_devices->open_devices == 0) {
1213  |  if (ret)
1214  |  return ret;
1215  |  return -EINVAL;
1216  | 	}
1217  |  
1218  | 	fs_devices->opened = 1;
1219  | 	fs_devices->latest_dev = latest_dev;
1220  | 	fs_devices->total_rw_bytes = 0;
1221  | 	fs_devices->chunk_alloc_policy = BTRFS_CHUNK_ALLOC_REGULAR;
1222  | 	fs_devices->read_policy = BTRFS_READ_POLICY_PID;
1223  |  
1224  |  return 0;
1225  | }
1226  |  
1227  | static int devid_cmp(void *priv, const struct list_head *a,
1228  |  const struct list_head *b)
1229  | {
1230  |  const struct btrfs_device *dev1, *dev2;
1231  |  
1232  | 	dev1 = list_entry(a, struct btrfs_device, dev_list);
1233  | 	dev2 = list_entry(b, struct btrfs_device, dev_list);
1234  |  
1235  |  if (dev1->devid < dev2->devid)
1236  |  return -1;
1237  |  else if (dev1->devid > dev2->devid)
1238  |  return 1;
1239  |  return 0;
1240  | }
1241  |  
1242  | int btrfs_open_devices(struct btrfs_fs_devices *fs_devices,
1243  | 		       blk_mode_t flags, void *holder)
1244  | {
1245  |  int ret;
1246  |  
1247  |  lockdep_assert_held(&uuid_mutex);
1248  |  /*
1249  |  * The device_list_mutex cannot be taken here in case opening the
1250  |  * underlying device takes further locks like open_mutex.
1251  |  *
1252  |  * We also don't need the lock here as this is called during mount and
1253  |  * exclusion is provided by uuid_mutex
1254  |  */
6952  |  BTRFS_UUID_SIZE);
6953  | 		args.uuid = uuid;
6954  | 		map->stripes[i].dev = btrfs_find_device(fs_info->fs_devices, &args);
6955  |  if (!map->stripes[i].dev) {
6956  | 			map->stripes[i].dev = handle_missing_device(fs_info,
6957  | 								    devid, uuid);
6958  |  if (IS_ERR(map->stripes[i].dev)) {
6959  | 				ret = PTR_ERR(map->stripes[i].dev);
6960  | 				btrfs_free_chunk_map(map);
6961  |  return ret;
6962  | 			}
6963  | 		}
6964  |  
6965  | 		set_bit(BTRFS_DEV_STATE_IN_FS_METADATA,
6966  | 				&(map->stripes[i].dev->dev_state));
6967  | 	}
6968  |  
6969  | 	ret = btrfs_add_chunk_map(fs_info, map);
6970  |  if (ret < 0) {
6971  |  btrfs_err(fs_info,
6972  |  "failed to add chunk map, start=%llu len=%llu: %d",
6973  |  map->start, map->chunk_len, ret);
6974  | 	}
6975  |  
6976  |  return ret;
6977  | }
6978  |  
6979  | static void fill_device_from_item(struct extent_buffer *leaf,
6980  |  struct btrfs_dev_item *dev_item,
6981  |  struct btrfs_device *device)
6982  | {
6983  |  unsigned long ptr;
6984  |  
6985  | 	device->devid = btrfs_device_id(leaf, dev_item);
6986  | 	device->disk_total_bytes = btrfs_device_total_bytes(leaf, dev_item);
6987  | 	device->total_bytes = device->disk_total_bytes;
6988  | 	device->commit_total_bytes = device->disk_total_bytes;
6989  | 	device->bytes_used = btrfs_device_bytes_used(leaf, dev_item);
6990  | 	device->commit_bytes_used = device->bytes_used;
6991  | 	device->type = btrfs_device_type(leaf, dev_item);
6992  | 	device->io_align = btrfs_device_io_align(leaf, dev_item);
6993  | 	device->io_width = btrfs_device_io_width(leaf, dev_item);
6994  | 	device->sector_size = btrfs_device_sector_size(leaf, dev_item);
6995  |  WARN_ON(device->devid == BTRFS_DEV_REPLACE_DEVID);
6996  | 	clear_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state);
6997  |  
6998  | 	ptr = btrfs_device_uuid(dev_item);
6999  | 	read_extent_buffer(leaf, device->uuid, ptr, BTRFS_UUID_SIZE);
7000  | }
7001  |  
7002  | static struct btrfs_fs_devices *open_seed_devices(struct btrfs_fs_info *fs_info,
7003  | 						  u8 *fsid)
7004  | {
7005  |  struct btrfs_fs_devices *fs_devices;
7006  |  int ret;
7007  |  
7008  |  lockdep_assert_held(&uuid_mutex);
    16←Assuming 'debug_locks' is 0→
    17←Taking false branch→
    18←Loop condition is false.  Exiting loop→
7009  |  ASSERT(fsid);
    19←'?' condition is true→
7010  |  
7011  |  /* This will match only for multi-device seed fs */
7012  |  list_for_each_entry(fs_devices, &fs_info->fs_devices->seed_list, seed_list)
    20←Loop condition is false. Execution continues on line 7017→
7013  |  if (!memcmp(fs_devices->fsid, fsid, BTRFS_FSID_SIZE))
7014  |  return fs_devices;
7015  |  
7016  |  
7017  |  fs_devices = find_fsid(fsid, NULL);
7018  |  if (!fs_devices) {
    21←Assuming 'fs_devices' is non-null→
    22←Taking false branch→
7019  |  if (!btrfs_test_opt(fs_info, DEGRADED))
7020  |  return ERR_PTR(-ENOENT);
7021  |  
7022  | 		fs_devices = alloc_fs_devices(fsid);
7023  |  if (IS_ERR(fs_devices))
7024  |  return fs_devices;
7025  |  
7026  | 		fs_devices->seeding = true;
7027  | 		fs_devices->opened = 1;
7028  |  return fs_devices;
7029  | 	}
7030  |  
7031  |  /*
7032  |  * Upon first call for a seed fs fsid, just create a private copy of the
7033  |  * respective fs_devices and anchor it at fs_info->fs_devices->seed_list
7034  |  */
7035  |  fs_devices = clone_fs_devices(fs_devices);
7036  |  if (IS_ERR(fs_devices))
    23←Taking false branch→
7037  |  return fs_devices;
7038  |  
7039  |  ret = open_fs_devices(fs_devices, BLK_OPEN_READ, fs_info->bdev_holder);
7040  |  if (ret23.1'ret' is 0) {
    24←Taking false branch→
7041  | 		free_fs_devices(fs_devices);
7042  |  return ERR_PTR(ret);
7043  | 	}
7044  |  
7045  |  if (!fs_devices->seeding) {
    25←Assuming field 'seeding' is false→
    26←Taking true branch→
7046  |  close_fs_devices(fs_devices);
7047  |  free_fs_devices(fs_devices);
    27←Calling 'free_fs_devices'→
7048  |  return ERR_PTR(-EINVAL);
7049  | 	}
7050  |  
7051  | 	list_add(&fs_devices->seed_list, &fs_info->fs_devices->seed_list);
7052  |  
7053  |  return fs_devices;
7054  | }
7055  |  
7056  | static int read_one_dev(struct extent_buffer *leaf,
7057  |  struct btrfs_dev_item *dev_item)
7058  | {
7059  |  BTRFS_DEV_LOOKUP_ARGS(args);
7060  |  struct btrfs_fs_info *fs_info = leaf->fs_info;
7061  |  struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
7062  |  struct btrfs_device *device;
7063  | 	u64 devid;
7064  |  int ret;
7065  | 	u8 fs_uuid[BTRFS_FSID_SIZE];
7066  | 	u8 dev_uuid[BTRFS_UUID_SIZE];
7067  |  
7068  | 	devid = btrfs_device_id(leaf, dev_item);
7069  | 	args.devid = devid;
7070  | 	read_extent_buffer(leaf, dev_uuid, btrfs_device_uuid(dev_item),
7071  |  BTRFS_UUID_SIZE);
7072  | 	read_extent_buffer(leaf, fs_uuid, btrfs_device_fsid(dev_item),
7073  |  BTRFS_FSID_SIZE);
7074  | 	args.uuid = dev_uuid;
7075  | 	args.fsid = fs_uuid;
7076  |  
7077  |  if (memcmp(fs_uuid, fs_devices->metadata_uuid, BTRFS_FSID_SIZE)) {
    13←Assuming the condition is true→
    14←Taking true branch→
7078  |  fs_devices = open_seed_devices(fs_info, fs_uuid);
    15←Calling 'open_seed_devices'→
7079  |  if (IS_ERR(fs_devices))
7080  |  return PTR_ERR(fs_devices);
7081  | 	}
7082  |  
7083  | 	device = btrfs_find_device(fs_info->fs_devices, &args);
7084  |  if (!device) {
7085  |  if (!btrfs_test_opt(fs_info, DEGRADED)) {
7086  | 			btrfs_report_missing_device(fs_info, devid,
7087  | 							dev_uuid, true);
7088  |  return -ENOENT;
7089  | 		}
7090  |  
7091  | 		device = add_missing_dev(fs_devices, devid, dev_uuid);
7092  |  if (IS_ERR(device)) {
7093  |  btrfs_err(fs_info,
7094  |  "failed to add missing dev %llu: %ld",
7095  |  devid, PTR_ERR(device));
7096  |  return PTR_ERR(device);
7097  | 		}
7098  | 		btrfs_report_missing_device(fs_info, devid, dev_uuid, false);
7099  | 	} else {
7100  |  if (!device->bdev) {
7101  |  if (!btrfs_test_opt(fs_info, DEGRADED)) {
7102  | 				btrfs_report_missing_device(fs_info,
7103  | 						devid, dev_uuid, true);
7104  |  return -ENOENT;
7105  | 			}
7106  | 			btrfs_report_missing_device(fs_info, devid,
7107  | 							dev_uuid, false);
7108  | 		}
7289  |  /* No chunk at all? Return false anyway */
7290  |  if (!map) {
7291  | 		ret = false;
7292  |  goto out;
7293  | 	}
7294  |  while (map) {
7295  |  int missing = 0;
7296  |  int max_tolerated;
7297  |  int i;
7298  |  
7299  | 		max_tolerated =
7300  | 			btrfs_get_num_tolerated_disk_barrier_failures(
7301  | 					map->type);
7302  |  for (i = 0; i < map->num_stripes; i++) {
7303  |  struct btrfs_device *dev = map->stripes[i].dev;
7304  |  
7305  |  if (!dev || !dev->bdev ||
7306  |  test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state) ||
7307  | 			    dev->last_flush_error)
7308  | 				missing++;
7309  |  else if (failing_dev && failing_dev == dev)
7310  | 				missing++;
7311  | 		}
7312  |  if (missing > max_tolerated) {
7313  |  if (!failing_dev)
7314  |  btrfs_warn(fs_info,
7315  |  "chunk %llu missing %d devices, max tolerance is %d for writable mount",
7316  |  map->start, missing, max_tolerated);
7317  | 			btrfs_free_chunk_map(map);
7318  | 			ret = false;
7319  |  goto out;
7320  | 		}
7321  | 		next_start = map->start + map->chunk_len;
7322  | 		btrfs_free_chunk_map(map);
7323  |  
7324  | 		map = btrfs_find_chunk_map(fs_info, next_start, U64_MAX - next_start);
7325  | 	}
7326  | out:
7327  |  return ret;
7328  | }
7329  |  
7330  | static void readahead_tree_node_children(struct extent_buffer *node)
7331  | {
7332  |  int i;
7333  |  const int nr_items = btrfs_header_nritems(node);
7334  |  
7335  |  for (i = 0; i < nr_items; i++)
7336  | 		btrfs_readahead_node_child(node, i);
7337  | }
7338  |  
7339  | int btrfs_read_chunk_tree(struct btrfs_fs_info *fs_info)
7340  | {
7341  |  struct btrfs_root *root = fs_info->chunk_root;
7342  |  struct btrfs_path *path;
7343  |  struct extent_buffer *leaf;
7344  |  struct btrfs_key key;
7345  |  struct btrfs_key found_key;
7346  |  int ret;
7347  |  int slot;
7348  |  int iter_ret = 0;
7349  | 	u64 total_dev = 0;
7350  | 	u64 last_ra_node = 0;
7351  |  
7352  | 	path = btrfs_alloc_path();
7353  |  if (!path)
    1Assuming 'path' is non-null→
    2←Taking false branch→
7354  |  return -ENOMEM;
7355  |  
7356  |  /*
7357  |  * uuid_mutex is needed only if we are mounting a sprout FS
7358  |  * otherwise we don't need it.
7359  |  */
7360  |  mutex_lock(&uuid_mutex);
7361  |  
7362  |  /*
7363  |  * It is possible for mount and umount to race in such a way that
7364  |  * we execute this code path, but open_fs_devices failed to clear
7365  |  * total_rw_bytes. We certainly want it cleared before reading the
7366  |  * device items, so clear it here.
7367  |  */
7368  | 	fs_info->fs_devices->total_rw_bytes = 0;
7369  |  
7370  |  /*
7371  |  * Lockdep complains about possible circular locking dependency between
7372  |  * a disk's open_mutex (struct gendisk.open_mutex), the rw semaphores
7373  |  * used for freeze procection of a fs (struct super_block.s_writers),
7374  |  * which we take when starting a transaction, and extent buffers of the
7375  |  * chunk tree if we call read_one_dev() while holding a lock on an
7376  |  * extent buffer of the chunk tree. Since we are mounting the filesystem
7377  |  * and at this point there can't be any concurrent task modifying the
7378  |  * chunk tree, to keep it simple, just skip locking on the chunk tree.
7379  |  */
7380  |  ASSERT(!test_bit(BTRFS_FS_OPEN, &fs_info->flags));
    3←Assuming the condition is true→
    4←'?' condition is true→
7381  | 	path->skip_locking = 1;
7382  |  
7383  |  /*
7384  |  * Read all device items, and then all the chunk items. All
7385  |  * device items are found before any chunk item (their object id
7386  |  * is smaller than the lowest possible object id for a chunk
7387  |  * item - BTRFS_FIRST_CHUNK_TREE_OBJECTID).
7388  |  */
7389  | 	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
7390  | 	key.offset = 0;
7391  | 	key.type = 0;
7392  |  btrfs_for_each_slot(root, &key, &found_key, path, iter_ret) {
    5←Assuming 'iter_ret' is >= 0→
    6←Assuming the condition is true→
    7←Loop condition is true.  Entering loop body→
7393  |  struct extent_buffer *node = path->nodes[1];
7394  |  
7395  | 		leaf = path->nodes[0];
7396  | 		slot = path->slots[0];
7397  |  
7398  |  if (node) {
    8←Assuming 'node' is null→
    9←Taking false branch→
7399  |  if (last_ra_node != node->start) {
7400  | 				readahead_tree_node_children(node);
7401  | 				last_ra_node = node->start;
7402  | 			}
7403  | 		}
7404  |  if (found_key.type == BTRFS_DEV_ITEM_KEY) {
    10←Assuming field 'type' is equal to BTRFS_DEV_ITEM_KEY→
    11←Taking true branch→
7405  |  struct btrfs_dev_item *dev_item;
7406  | 			dev_item = btrfs_item_ptr(leaf, slot,
7407  |  struct btrfs_dev_item);
7408  |  ret = read_one_dev(leaf, dev_item);
    12←Calling 'read_one_dev'→
7409  |  if (ret)
7410  |  goto error;
7411  | 			total_dev++;
7412  | 		} else if (found_key.type == BTRFS_CHUNK_ITEM_KEY) {
7413  |  struct btrfs_chunk *chunk;
7414  |  
7415  |  /*
7416  |  * We are only called at mount time, so no need to take
7417  |  * fs_info->chunk_mutex. Plus, to avoid lockdep warnings,
7418  |  * we always lock first fs_info->chunk_mutex before
7419  |  * acquiring any locks on the chunk tree. This is a
7420  |  * requirement for chunk allocation, see the comment on
7421  |  * top of btrfs_chunk_alloc() for details.
7422  |  */
7423  | 			chunk = btrfs_item_ptr(leaf, slot, struct btrfs_chunk);
7424  | 			ret = read_one_chunk(&found_key, leaf, chunk);
7425  |  if (ret)
7426  |  goto error;
7427  | 		}
7428  | 	}
7429  |  /* Catch error found during iteration */
7430  |  if (iter_ret < 0) {
7431  | 		ret = iter_ret;
7432  |  goto error;
7433  | 	}
7434  |  
7435  |  /*
7436  |  * After loading chunk tree, we've got all device information,
7437  |  * do another round of validation checks.
7438  |  */

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
