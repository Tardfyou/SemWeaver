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
Warning:| line 2965, column 6  
copy_from_user into fixed-size buffer uses unbounded user length  
  
### Annotated Source Code


2899  | 		phba->ktime_seg5_min = 0xffffffff;
2900  | 		phba->ktime_seg6_total = 0;
2901  | 		phba->ktime_seg6_max = 0;
2902  | 		phba->ktime_seg6_min = 0xffffffff;
2903  | 		phba->ktime_seg7_total = 0;
2904  | 		phba->ktime_seg7_max = 0;
2905  | 		phba->ktime_seg7_min = 0xffffffff;
2906  | 		phba->ktime_seg8_total = 0;
2907  | 		phba->ktime_seg8_max = 0;
2908  | 		phba->ktime_seg8_min = 0xffffffff;
2909  | 		phba->ktime_seg9_total = 0;
2910  | 		phba->ktime_seg9_max = 0;
2911  | 		phba->ktime_seg9_min = 0xffffffff;
2912  | 		phba->ktime_seg10_total = 0;
2913  | 		phba->ktime_seg10_max = 0;
2914  | 		phba->ktime_seg10_min = 0xffffffff;
2915  |  return strlen(pbuf);
2916  | 	}
2917  |  return -EINVAL;
2918  | }
2919  |  
2920  | static int
2921  | lpfc_debugfs_nvmeio_trc_open(struct inode *inode, struct file *file)
2922  | {
2923  |  struct lpfc_hba *phba = inode->i_private;
2924  |  struct lpfc_debug *debug;
2925  |  int rc = -ENOMEM;
2926  |  
2927  | 	debug = kmalloc(sizeof(*debug), GFP_KERNEL);
2928  |  if (!debug)
2929  |  goto out;
2930  |  
2931  |  /* Round to page boundary */
2932  | 	debug->buffer = kmalloc(LPFC_NVMEIO_TRC_SIZE, GFP_KERNEL);
2933  |  if (!debug->buffer) {
2934  | 		kfree(debug);
2935  |  goto out;
2936  | 	}
2937  |  
2938  | 	debug->len = lpfc_debugfs_nvmeio_trc_data(phba, debug->buffer,
2939  |  LPFC_NVMEIO_TRC_SIZE);
2940  |  
2941  | 	debug->i_private = inode->i_private;
2942  | 	file->private_data = debug;
2943  |  
2944  | 	rc = 0;
2945  | out:
2946  |  return rc;
2947  | }
2948  |  
2949  | static ssize_t
2950  | lpfc_debugfs_nvmeio_trc_write(struct file *file, const char __user *buf,
2951  | 			      size_t nbytes, loff_t *ppos)
2952  | {
2953  |  struct lpfc_debug *debug = file->private_data;
2954  |  struct lpfc_hba *phba = (struct lpfc_hba *)debug->i_private;
2955  |  int i;
2956  |  unsigned long sz;
2957  |  char mybuf[64];
2958  |  char *pbuf;
2959  |  
2960  |  if (nbytes > sizeof(mybuf) - 1)
    1Assuming the condition is false→
    2←Taking false branch→
2961  | 		nbytes = sizeof(mybuf) - 1;
2962  |  
2963  |  memset(mybuf, 0, sizeof(mybuf));
2964  |  
2965  |  if (copy_from_user(mybuf, buf, nbytes))
    3←copy_from_user into fixed-size buffer uses unbounded user length
2966  |  return -EFAULT;
2967  | 	pbuf = &mybuf[0];
2968  |  
2969  |  if ((strncmp(pbuf, "off", sizeof("off") - 1) == 0)) {
2970  |  lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
2971  |  "0570 nvmeio_trc_off\n");
2972  | 		phba->nvmeio_trc_output_idx = 0;
2973  | 		phba->nvmeio_trc_on = 0;
2974  |  return strlen(pbuf);
2975  | 	} else if ((strncmp(pbuf, "on", sizeof("on") - 1) == 0)) {
2976  |  lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
2977  |  "0571 nvmeio_trc_on\n");
2978  | 		phba->nvmeio_trc_output_idx = 0;
2979  | 		phba->nvmeio_trc_on = 1;
2980  |  return strlen(pbuf);
2981  | 	}
2982  |  
2983  |  /* We must be off to allocate the trace buffer */
2984  |  if (phba->nvmeio_trc_on != 0)
2985  |  return -EINVAL;
2986  |  
2987  |  /* If not on or off, the parameter is the trace buffer size */
2988  | 	i = kstrtoul(pbuf, 0, &sz);
2989  |  if (i)
2990  |  return -EINVAL;
2991  | 	phba->nvmeio_trc_size = (uint32_t)sz;
2992  |  
2993  |  /* It must be a power of 2 - round down */
2994  | 	i = 0;
2995  |  while (sz > 1) {

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
