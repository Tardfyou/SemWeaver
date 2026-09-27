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
Warning:| line 2699, column 6  
copy_from_user into fixed-size buffer uses unbounded user length  
  
### Annotated Source Code


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
2649  | }
2650  |  
2651  | static int
2652  | lpfc_debugfs_nvmestat_open(struct inode *inode, struct file *file)
2653  | {
2654  |  struct lpfc_vport *vport = inode->i_private;
2655  |  struct lpfc_debug *debug;
2656  |  int rc = -ENOMEM;
2657  |  
2658  | 	debug = kmalloc(sizeof(*debug), GFP_KERNEL);
2659  |  if (!debug)
2660  |  goto out;
2661  |  
2662  |  /* Round to page boundary */
2663  | 	debug->buffer = kmalloc(LPFC_NVMESTAT_SIZE, GFP_KERNEL);
2664  |  if (!debug->buffer) {
2665  | 		kfree(debug);
2666  |  goto out;
2667  | 	}
2668  |  
2669  | 	debug->len = lpfc_debugfs_nvmestat_data(vport, debug->buffer,
2670  |  LPFC_NVMESTAT_SIZE);
2671  |  
2672  | 	debug->i_private = inode->i_private;
2673  | 	file->private_data = debug;
2674  |  
2675  | 	rc = 0;
2676  | out:
2677  |  return rc;
2678  | }
2679  |  
2680  | static ssize_t
2681  | lpfc_debugfs_nvmestat_write(struct file *file, const char __user *buf,
2682  | 			    size_t nbytes, loff_t *ppos)
2683  | {
2684  |  struct lpfc_debug *debug = file->private_data;
2685  |  struct lpfc_vport *vport = (struct lpfc_vport *)debug->i_private;
2686  |  struct lpfc_hba   *phba = vport->phba;
2687  |  struct lpfc_nvmet_tgtport *tgtp;
2688  |  char mybuf[64];
2689  |  char *pbuf;
2690  |  
2691  |  if (!phba->targetport)
    1Assuming field 'targetport' is non-null→
    2←Taking false branch→
2692  |  return -ENXIO;
2693  |  
2694  |  if (nbytes > sizeof(mybuf) - 1)
    3←Assuming the condition is false→
    4←Taking false branch→
2695  | 		nbytes = sizeof(mybuf) - 1;
2696  |  
2697  |  memset(mybuf, 0, sizeof(mybuf));
2698  |  
2699  |  if (copy_from_user(mybuf, buf, nbytes))
    5←copy_from_user into fixed-size buffer uses unbounded user length
2700  |  return -EFAULT;
2701  | 	pbuf = &mybuf[0];
2702  |  
2703  | 	tgtp = (struct lpfc_nvmet_tgtport *)phba->targetport->private;
2704  |  if ((strncmp(pbuf, "reset", strlen("reset")) == 0) ||
2705  | 	    (strncmp(pbuf, "zero", strlen("zero")) == 0)) {
2706  | 		atomic_set(&tgtp->rcv_ls_req_in, 0);
2707  | 		atomic_set(&tgtp->rcv_ls_req_out, 0);
2708  | 		atomic_set(&tgtp->rcv_ls_req_drop, 0);
2709  | 		atomic_set(&tgtp->xmt_ls_abort, 0);
2710  | 		atomic_set(&tgtp->xmt_ls_abort_cmpl, 0);
2711  | 		atomic_set(&tgtp->xmt_ls_rsp, 0);
2712  | 		atomic_set(&tgtp->xmt_ls_drop, 0);
2713  | 		atomic_set(&tgtp->xmt_ls_rsp_error, 0);
2714  | 		atomic_set(&tgtp->xmt_ls_rsp_cmpl, 0);
2715  |  
2716  | 		atomic_set(&tgtp->rcv_fcp_cmd_in, 0);
2717  | 		atomic_set(&tgtp->rcv_fcp_cmd_out, 0);
2718  | 		atomic_set(&tgtp->rcv_fcp_cmd_drop, 0);
2719  | 		atomic_set(&tgtp->xmt_fcp_drop, 0);
2720  | 		atomic_set(&tgtp->xmt_fcp_read_rsp, 0);
2721  | 		atomic_set(&tgtp->xmt_fcp_read, 0);
2722  | 		atomic_set(&tgtp->xmt_fcp_write, 0);
2723  | 		atomic_set(&tgtp->xmt_fcp_rsp, 0);
2724  | 		atomic_set(&tgtp->xmt_fcp_release, 0);
2725  | 		atomic_set(&tgtp->xmt_fcp_rsp_cmpl, 0);
2726  | 		atomic_set(&tgtp->xmt_fcp_rsp_error, 0);
2727  | 		atomic_set(&tgtp->xmt_fcp_rsp_drop, 0);
2728  |  
2729  | 		atomic_set(&tgtp->xmt_fcp_abort, 0);

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
