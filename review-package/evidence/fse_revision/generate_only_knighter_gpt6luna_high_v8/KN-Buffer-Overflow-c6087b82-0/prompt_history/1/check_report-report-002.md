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

scsi: lpfc: Prevent lpfc_debugfs_lockstat_write() buffer overflow

A static code analysis tool flagged the possibility of buffer overflow when
using copy_from_user() for a debugfs entry.

Currently, it is possible that copy_from_user() copies more bytes than what
would fit in the mybuf char array.  Add a min() restriction check between
sizeof(mybuf) - 1 and nbytes passed from the userspace buffer to protect
against buffer overflow.

Link: https://lore.kernel.org/r/20230301231626.9621-2-justintee8345@gmail.com
Signed-off-by: Justin Tee <justin.tee@broadcom.com>
Signed-off-by: Martin K. Petersen <martin.petersen@oracle.com>

## Buggy Code

```c
// Function: lpfc_debugfs_lockstat_write in drivers/scsi/lpfc/lpfc_debugfs.c
static ssize_t
lpfc_debugfs_lockstat_write(struct file *file, const char __user *buf,
			    size_t nbytes, loff_t *ppos)
{
	struct lpfc_debug *debug = file->private_data;
	struct lpfc_hba *phba = (struct lpfc_hba *)debug->i_private;
	struct lpfc_sli4_hdw_queue *qp;
	char mybuf[64];
	char *pbuf;
	int i;

	memset(mybuf, 0, sizeof(mybuf));

	if (copy_from_user(mybuf, buf, nbytes))
		return -EFAULT;
	pbuf = &mybuf[0];

	if ((strncmp(pbuf, "reset", strlen("reset")) == 0) ||
	    (strncmp(pbuf, "zero", strlen("zero")) == 0)) {
		for (i = 0; i < phba->cfg_hdw_queue; i++) {
			qp = &phba->sli4_hba.hdwq[i];
			qp->lock_conflict.alloc_xri_get = 0;
			qp->lock_conflict.alloc_xri_put = 0;
			qp->lock_conflict.free_xri = 0;
			qp->lock_conflict.wq_access = 0;
			qp->lock_conflict.alloc_pvt_pool = 0;
			qp->lock_conflict.mv_from_pvt_pool = 0;
			qp->lock_conflict.mv_to_pub_pool = 0;
			qp->lock_conflict.mv_to_pvt_pool = 0;
			qp->lock_conflict.free_pvt_pool = 0;
			qp->lock_conflict.free_pub_pool = 0;
			qp->lock_conflict.wq_access = 0;
		}
	}
	return nbytes;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/scsi/lpfc/lpfc_debugfs.c b/drivers/scsi/lpfc/lpfc_debugfs.c
index f5252e45a48a..3e365e5e194a 100644
--- a/drivers/scsi/lpfc/lpfc_debugfs.c
+++ b/drivers/scsi/lpfc/lpfc_debugfs.c
@@ -2157,10 +2157,13 @@ lpfc_debugfs_lockstat_write(struct file *file, const char __user *buf,
 	char mybuf[64];
 	char *pbuf;
 	int i;
+	size_t bsize;

 	memset(mybuf, 0, sizeof(mybuf));

-	if (copy_from_user(mybuf, buf, nbytes))
+	bsize = min(nbytes, (sizeof(mybuf) - 1));
+
+	if (copy_from_user(mybuf, buf, bsize))
 		return -EFAULT;
 	pbuf = &mybuf[0];

@@ -2181,7 +2184,7 @@ lpfc_debugfs_lockstat_write(struct file *file, const char __user *buf,
 			qp->lock_conflict.wq_access = 0;
 		}
 	}
-	return nbytes;
+	return bsize;
 }
 #endif

```


## Bug Pattern

Copying user-provided data into a fixed-size buffer using copy_from_user() with the unbounded user length (nbytes) instead of clamping it to the buffer’s capacity (e.g., sizeof(buf) - 1). This allows a user to pass a size larger than the local array, causing a buffer overflow in debugfs write handlers (or similar file ops). The fix is to limit the copy length with min(nbytes, sizeof(buf) - 1) and return the actual consumed size.


# Report

BuildSource:| drivers/scsi/lpfc/lpfc_debugfs.c
### Report Summary

File:| lpfc_debugfs.c  
---|---  
Warning:| line 2618, column 6  
copy_from_user into fixed-size buffer uses unbounded user length  
  
### Annotated Source Code


2550  |  * than @nbytes if the end of the file was reached) or a negative error value.
2551  |  **/
2552  | static ssize_t
2553  | lpfc_debugfs_read(struct file *file, char __user *buf,
2554  | 		  size_t nbytes, loff_t *ppos)
2555  | {
2556  |  struct lpfc_debug *debug = file->private_data;
2557  |  
2558  |  return simple_read_from_buffer(buf, nbytes, ppos, debug->buffer,
2559  | 				       debug->len);
2560  | }
2561  |  
2562  | /**
2563  |  * lpfc_debugfs_release - Release the buffer used to store debugfs file data
2564  |  * @inode: The inode pointer that contains a vport pointer. (unused)
2565  |  * @file: The file pointer that contains the buffer to release.
2566  |  *
2567  |  * Description:
2568  |  * This routine frees the buffer that was allocated when the debugfs file was
2569  |  * opened.
2570  |  *
2571  |  * Returns:
2572  |  * This function returns zero.
2573  |  **/
2574  | static int
2575  | lpfc_debugfs_release(struct inode *inode, struct file *file)
2576  | {
2577  |  struct lpfc_debug *debug = file->private_data;
2578  |  
2579  | 	kfree(debug->buffer);
2580  | 	kfree(debug);
2581  |  
2582  |  return 0;
2583  | }
2584  |  
2585  | /**
2586  |  * lpfc_debugfs_multixripools_write - Clear multi-XRI pools statistics
2587  |  * @file: The file pointer to read from.
2588  |  * @buf: The buffer to copy the user data from.
2589  |  * @nbytes: The number of bytes to get.
2590  |  * @ppos: The position in the file to start reading from.
2591  |  *
2592  |  * Description:
2593  |  * This routine clears multi-XRI pools statistics when buf contains "clear".
2594  |  *
2595  |  * Return Value:
2596  |  * It returns the @nbytges passing in from debugfs user space when successful.
2597  |  * In case of error conditions, it returns proper error code back to the user
2598  |  * space.
2599  |  **/
2600  | static ssize_t
2601  | lpfc_debugfs_multixripools_write(struct file *file, const char __user *buf,
2602  | 				 size_t nbytes, loff_t *ppos)
2603  | {
2604  |  struct lpfc_debug *debug = file->private_data;
2605  |  struct lpfc_hba *phba = (struct lpfc_hba *)debug->i_private;
2606  |  char mybuf[64];
2607  |  char *pbuf;
2608  | 	u32 i;
2609  | 	u32 hwq_count;
2610  |  struct lpfc_sli4_hdw_queue *qp;
2611  |  struct lpfc_multixri_pool *multixri_pool;
2612  |  
2613  |  if (nbytes > sizeof(mybuf) - 1)
    1Assuming the condition is false→
    2←Taking false branch→
2614  | 		nbytes = sizeof(mybuf) - 1;
2615  |  
2616  |  memset(mybuf, 0, sizeof(mybuf));
2617  |  
2618  |  if (copy_from_user(mybuf, buf, nbytes))
    3←copy_from_user into fixed-size buffer uses unbounded user length
2619  |  return -EFAULT;
2620  | 	pbuf = &mybuf[0];
2621  |  
2622  |  if ((strncmp(pbuf, "clear", strlen("clear"))) == 0) {
2623  | 		hwq_count = phba->cfg_hdw_queue;
2624  |  for (i = 0; i < hwq_count; i++) {
2625  | 			qp = &phba->sli4_hba.hdwq[i];
2626  | 			multixri_pool = qp->p_multixri_pool;
2627  |  if (!multixri_pool)
2628  |  continue;
2629  |  
2630  | 			qp->empty_io_bufs = 0;
2631  | 			multixri_pool->pbl_empty_count = 0;
2632  | #ifdef LPFC_MXP_STAT
2633  | 			multixri_pool->above_limit_count = 0;
2634  | 			multixri_pool->below_limit_count = 0;
2635  | 			multixri_pool->stat_max_hwm = 0;
2636  | 			multixri_pool->local_pbl_hit_count = 0;
2637  | 			multixri_pool->other_pbl_hit_count = 0;
2638  |  
2639  | 			multixri_pool->stat_pbl_count = 0;
2640  | 			multixri_pool->stat_pvt_count = 0;
2641  | 			multixri_pool->stat_busy_count = 0;
2642  | 			multixri_pool->stat_snapshot_taken = 0;
2643  | #endif
2644  | 		}
2645  |  return strlen(pbuf);
2646  | 	}
2647  |  
2648  |  return -EINVAL;

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
