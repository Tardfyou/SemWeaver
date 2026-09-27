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
Warning:| line 2837, column 6  
copy_from_user into fixed-size buffer uses unbounded user length  
  
### Annotated Source Code


2772  |  struct lpfc_debug *debug = file->private_data;
2773  |  struct lpfc_vport *vport = (struct lpfc_vport *)debug->i_private;
2774  |  struct lpfc_hba *phba = vport->phba;
2775  |  char mybuf[6] = {0};
2776  |  int i;
2777  |  
2778  |  if (copy_from_user(mybuf, buf, (nbytes >= sizeof(mybuf)) ?
2779  | 				       (sizeof(mybuf) - 1) : nbytes))
2780  |  return -EFAULT;
2781  |  
2782  |  if ((strncmp(&mybuf[0], "reset", strlen("reset")) == 0) ||
2783  | 	    (strncmp(&mybuf[0], "zero", strlen("zero")) == 0)) {
2784  |  for (i = 0; i < phba->cfg_hdw_queue; i++) {
2785  |  memset(&phba->sli4_hba.hdwq[i].scsi_cstat, 0,
2786  |  sizeof(phba->sli4_hba.hdwq[i].scsi_cstat));
2787  | 		}
2788  | 	}
2789  |  
2790  |  return nbytes;
2791  | }
2792  |  
2793  | static int
2794  | lpfc_debugfs_ioktime_open(struct inode *inode, struct file *file)
2795  | {
2796  |  struct lpfc_vport *vport = inode->i_private;
2797  |  struct lpfc_debug *debug;
2798  |  int rc = -ENOMEM;
2799  |  
2800  | 	debug = kmalloc(sizeof(*debug), GFP_KERNEL);
2801  |  if (!debug)
2802  |  goto out;
2803  |  
2804  |  /* Round to page boundary */
2805  | 	debug->buffer = kmalloc(LPFC_IOKTIME_SIZE, GFP_KERNEL);
2806  |  if (!debug->buffer) {
2807  | 		kfree(debug);
2808  |  goto out;
2809  | 	}
2810  |  
2811  | 	debug->len = lpfc_debugfs_ioktime_data(vport, debug->buffer,
2812  |  LPFC_IOKTIME_SIZE);
2813  |  
2814  | 	debug->i_private = inode->i_private;
2815  | 	file->private_data = debug;
2816  |  
2817  | 	rc = 0;
2818  | out:
2819  |  return rc;
2820  | }
2821  |  
2822  | static ssize_t
2823  | lpfc_debugfs_ioktime_write(struct file *file, const char __user *buf,
2824  | 			   size_t nbytes, loff_t *ppos)
2825  | {
2826  |  struct lpfc_debug *debug = file->private_data;
2827  |  struct lpfc_vport *vport = (struct lpfc_vport *)debug->i_private;
2828  |  struct lpfc_hba   *phba = vport->phba;
2829  |  char mybuf[64];
2830  |  char *pbuf;
2831  |  
2832  |  if (nbytes > sizeof(mybuf) - 1)
    1Assuming the condition is false→
    2←Taking false branch→
2833  | 		nbytes = sizeof(mybuf) - 1;
2834  |  
2835  |  memset(mybuf, 0, sizeof(mybuf));
2836  |  
2837  |  if (copy_from_user(mybuf, buf, nbytes))
    3←copy_from_user into fixed-size buffer uses unbounded user length
2838  |  return -EFAULT;
2839  | 	pbuf = &mybuf[0];
2840  |  
2841  |  if ((strncmp(pbuf, "on", sizeof("on") - 1) == 0)) {
2842  | 		phba->ktime_data_samples = 0;
2843  | 		phba->ktime_status_samples = 0;
2844  | 		phba->ktime_seg1_total = 0;
2845  | 		phba->ktime_seg1_max = 0;
2846  | 		phba->ktime_seg1_min = 0xffffffff;
2847  | 		phba->ktime_seg2_total = 0;
2848  | 		phba->ktime_seg2_max = 0;
2849  | 		phba->ktime_seg2_min = 0xffffffff;
2850  | 		phba->ktime_seg3_total = 0;
2851  | 		phba->ktime_seg3_max = 0;
2852  | 		phba->ktime_seg3_min = 0xffffffff;
2853  | 		phba->ktime_seg4_total = 0;
2854  | 		phba->ktime_seg4_max = 0;
2855  | 		phba->ktime_seg4_min = 0xffffffff;
2856  | 		phba->ktime_seg5_total = 0;
2857  | 		phba->ktime_seg5_max = 0;
2858  | 		phba->ktime_seg5_min = 0xffffffff;
2859  | 		phba->ktime_seg6_total = 0;
2860  | 		phba->ktime_seg6_max = 0;
2861  | 		phba->ktime_seg6_min = 0xffffffff;
2862  | 		phba->ktime_seg7_total = 0;
2863  | 		phba->ktime_seg7_max = 0;
2864  | 		phba->ktime_seg7_min = 0xffffffff;
2865  | 		phba->ktime_seg8_total = 0;
2866  | 		phba->ktime_seg8_max = 0;
2867  | 		phba->ktime_seg8_min = 0xffffffff;

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
