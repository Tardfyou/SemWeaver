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
Warning:| line 3071, column 6  
copy_from_user into fixed-size buffer uses unbounded user length  
  
### Annotated Source Code


3004  | 	phba->nvmeio_trc_size = (uint32_t)sz;
3005  |  
3006  |  /* If one previously exists, free it */
3007  | 	kfree(phba->nvmeio_trc);
3008  |  
3009  |  /* Allocate new trace buffer and initialize */
3010  | 	phba->nvmeio_trc = kzalloc((sizeof(struct lpfc_debugfs_nvmeio_trc) *
3011  | 				    sz), GFP_KERNEL);
3012  |  if (!phba->nvmeio_trc) {
3013  |  lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
3014  |  "0573 Cannot create debugfs "
3015  |  "nvmeio_trc buffer\n");
3016  |  return -ENOMEM;
3017  | 	}
3018  | 	atomic_set(&phba->nvmeio_trc_cnt, 0);
3019  | 	phba->nvmeio_trc_on = 0;
3020  | 	phba->nvmeio_trc_output_idx = 0;
3021  |  
3022  |  return strlen(pbuf);
3023  | }
3024  |  
3025  | static int
3026  | lpfc_debugfs_hdwqstat_open(struct inode *inode, struct file *file)
3027  | {
3028  |  struct lpfc_vport *vport = inode->i_private;
3029  |  struct lpfc_debug *debug;
3030  |  int rc = -ENOMEM;
3031  |  
3032  | 	debug = kmalloc(sizeof(*debug), GFP_KERNEL);
3033  |  if (!debug)
3034  |  goto out;
3035  |  
3036  |  /* Round to page boundary */
3037  | 	debug->buffer = kcalloc(1, LPFC_SCSISTAT_SIZE, GFP_KERNEL);
3038  |  if (!debug->buffer) {
3039  | 		kfree(debug);
3040  |  goto out;
3041  | 	}
3042  |  
3043  | 	debug->len = lpfc_debugfs_hdwqstat_data(vport, debug->buffer,
3044  |  LPFC_SCSISTAT_SIZE);
3045  |  
3046  | 	debug->i_private = inode->i_private;
3047  | 	file->private_data = debug;
3048  |  
3049  | 	rc = 0;
3050  | out:
3051  |  return rc;
3052  | }
3053  |  
3054  | static ssize_t
3055  | lpfc_debugfs_hdwqstat_write(struct file *file, const char __user *buf,
3056  | 			    size_t nbytes, loff_t *ppos)
3057  | {
3058  |  struct lpfc_debug *debug = file->private_data;
3059  |  struct lpfc_vport *vport = (struct lpfc_vport *)debug->i_private;
3060  |  struct lpfc_hba   *phba = vport->phba;
3061  |  struct lpfc_hdwq_stat *c_stat;
3062  |  char mybuf[64];
3063  |  char *pbuf;
3064  |  int i;
3065  |  
3066  |  if (nbytes > sizeof(mybuf) - 1)
    1Assuming the condition is false→
    2←Taking false branch→
3067  | 		nbytes = sizeof(mybuf) - 1;
3068  |  
3069  |  memset(mybuf, 0, sizeof(mybuf));
3070  |  
3071  |  if (copy_from_user(mybuf, buf, nbytes))
    3←copy_from_user into fixed-size buffer uses unbounded user length
3072  |  return -EFAULT;
3073  | 	pbuf = &mybuf[0];
3074  |  
3075  |  if ((strncmp(pbuf, "on", sizeof("on") - 1) == 0)) {
3076  |  if (phba->nvmet_support)
3077  | 			phba->hdwqstat_on |= LPFC_CHECK_NVMET_IO;
3078  |  else
3079  | 			phba->hdwqstat_on |= (LPFC_CHECK_NVME_IO |
3080  |  LPFC_CHECK_SCSI_IO);
3081  |  return strlen(pbuf);
3082  | 	} else if ((strncmp(pbuf, "nvme_on", sizeof("nvme_on") - 1) == 0)) {
3083  |  if (phba->nvmet_support)
3084  | 			phba->hdwqstat_on |= LPFC_CHECK_NVMET_IO;
3085  |  else
3086  | 			phba->hdwqstat_on |= LPFC_CHECK_NVME_IO;
3087  |  return strlen(pbuf);
3088  | 	} else if ((strncmp(pbuf, "scsi_on", sizeof("scsi_on") - 1) == 0)) {
3089  |  if (!phba->nvmet_support)
3090  | 			phba->hdwqstat_on |= LPFC_CHECK_SCSI_IO;
3091  |  return strlen(pbuf);
3092  | 	} else if ((strncmp(pbuf, "nvme_off", sizeof("nvme_off") - 1) == 0)) {
3093  | 		phba->hdwqstat_on &= ~(LPFC_CHECK_NVME_IO |
3094  |  LPFC_CHECK_NVMET_IO);
3095  |  return strlen(pbuf);
3096  | 	} else if ((strncmp(pbuf, "scsi_off", sizeof("scsi_off") - 1) == 0)) {
3097  | 		phba->hdwqstat_on &= ~LPFC_CHECK_SCSI_IO;
3098  |  return strlen(pbuf);
3099  | 	} else if ((strncmp(pbuf, "off",
3100  |  sizeof("off") - 1) == 0)) {
3101  | 		phba->hdwqstat_on = LPFC_CHECK_OFF;

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
