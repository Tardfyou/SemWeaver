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

crypto: qat - resolve race condition during AER recovery

During the PCI AER system's error recovery process, the kernel driver
may encounter a race condition with freeing the reset_data structure's
memory. If the device restart will take more than 10 seconds the function
scheduling that restart will exit due to a timeout, and the reset_data
structure will be freed. However, this data structure is used for
completion notification after the restart is completed, which leads
to a UAF bug.

This results in a KFENCE bug notice.

  BUG: KFENCE: use-after-free read in adf_device_reset_worker+0x38/0xa0 [intel_qat]
  Use-after-free read at 0x00000000bc56fddf (in kfence-#142):
  adf_device_reset_worker+0x38/0xa0 [intel_qat]
  process_one_work+0x173/0x340

To resolve this race condition, the memory associated to the container
of the work_struct is freed on the worker if the timeout expired,
otherwise on the function that schedules the worker.
The timeout detection can be done by checking if the caller is
still waiting for completion or not by using completion_done() function.

Fixes: d8cba25d2c68 ("crypto: qat - Intel(R) QAT driver framework")
Cc: <stable@vger.kernel.org>
Signed-off-by: Damian Muszynski <damian.muszynski@intel.com>
Reviewed-by: Giovanni Cabiddu <giovanni.cabiddu@intel.com>
Signed-off-by: Herbert Xu <herbert@gondor.apana.org.au>

## Buggy Code

```c
// Function: adf_device_reset_worker in drivers/crypto/intel/qat/qat_common/adf_aer.c
static void adf_device_reset_worker(struct work_struct *work)
{
	struct adf_reset_dev_data *reset_data =
		  container_of(work, struct adf_reset_dev_data, reset_work);
	struct adf_accel_dev *accel_dev = reset_data->accel_dev;
	unsigned long wait_jiffies = msecs_to_jiffies(10000);
	struct adf_sriov_dev_data sriov_data;

	adf_dev_restarting_notify(accel_dev);
	if (adf_dev_restart(accel_dev)) {
		/* The device hanged and we can't restart it so stop here */
		dev_err(&GET_DEV(accel_dev), "Restart device failed\n");
		if (reset_data->mode == ADF_DEV_RESET_ASYNC)
			kfree(reset_data);
		WARN(1, "QAT: device restart failed. Device is unusable\n");
		return;
	}

	sriov_data.accel_dev = accel_dev;
	init_completion(&sriov_data.compl);
	INIT_WORK(&sriov_data.sriov_work, adf_device_sriov_worker);
	queue_work(device_sriov_wq, &sriov_data.sriov_work);
	if (wait_for_completion_timeout(&sriov_data.compl, wait_jiffies))
		adf_pf2vf_notify_restarted(accel_dev);

	adf_dev_restarted_notify(accel_dev);
	clear_bit(ADF_STATUS_RESTARTING, &accel_dev->status);

	/* The dev is back alive. Notify the caller if in sync mode */
	if (reset_data->mode == ADF_DEV_RESET_SYNC)
		complete(&reset_data->compl);
	else
		kfree(reset_data);
}
```

```c
// Function: adf_slot_reset in drivers/crypto/intel/qat/qat_common/adf_aer.c
static pci_ers_result_t adf_slot_reset(struct pci_dev *pdev)
{
	struct adf_accel_dev *accel_dev = adf_devmgr_pci_to_accel_dev(pdev);
	int res = 0;

	if (!accel_dev) {
		pr_err("QAT: Can't find acceleration device\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}

	if (!pdev->is_busmaster)
		pci_set_master(pdev);
	pci_restore_state(pdev);
	pci_save_state(pdev);
	res = adf_dev_up(accel_dev, false);
	if (res && res != -EALREADY)
		return PCI_ERS_RESULT_DISCONNECT;

	adf_reenable_sriov(accel_dev);
	adf_pf2vf_notify_restarted(accel_dev);
	adf_dev_restarted_notify(accel_dev);
	clear_bit(ADF_STATUS_RESTARTING, &accel_dev->status);
	return PCI_ERS_RESULT_RECOVERED;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/crypto/intel/qat/qat_common/adf_aer.c b/drivers/crypto/intel/qat/qat_common/adf_aer.c
index 3597e7605a14..9da2278bd5b7 100644
--- a/drivers/crypto/intel/qat/qat_common/adf_aer.c
+++ b/drivers/crypto/intel/qat/qat_common/adf_aer.c
@@ -130,7 +130,8 @@ static void adf_device_reset_worker(struct work_struct *work)
 	if (adf_dev_restart(accel_dev)) {
 		/* The device hanged and we can't restart it so stop here */
 		dev_err(&GET_DEV(accel_dev), "Restart device failed\n");
-		if (reset_data->mode == ADF_DEV_RESET_ASYNC)
+		if (reset_data->mode == ADF_DEV_RESET_ASYNC ||
+		    completion_done(&reset_data->compl))
 			kfree(reset_data);
 		WARN(1, "QAT: device restart failed. Device is unusable\n");
 		return;
@@ -146,11 +147,19 @@ static void adf_device_reset_worker(struct work_struct *work)
 	adf_dev_restarted_notify(accel_dev);
 	clear_bit(ADF_STATUS_RESTARTING, &accel_dev->status);

-	/* The dev is back alive. Notify the caller if in sync mode */
-	if (reset_data->mode == ADF_DEV_RESET_SYNC)
-		complete(&reset_data->compl);
-	else
+	/*
+	 * The dev is back alive. Notify the caller if in sync mode
+	 *
+	 * If device restart will take a more time than expected,
+	 * the schedule_reset() function can timeout and exit. This can be
+	 * detected by calling the completion_done() function. In this case
+	 * the reset_data structure needs to be freed here.
+	 */
+	if (reset_data->mode == ADF_DEV_RESET_ASYNC ||
+	    completion_done(&reset_data->compl))
 		kfree(reset_data);
+	else
+		complete(&reset_data->compl);
 }

 static int adf_dev_aer_schedule_reset(struct adf_accel_dev *accel_dev,
@@ -183,8 +192,9 @@ static int adf_dev_aer_schedule_reset(struct adf_accel_dev *accel_dev,
 			dev_err(&GET_DEV(accel_dev),
 				"Reset device timeout expired\n");
 			ret = -EFAULT;
+		} else {
+			kfree(reset_data);
 		}
-		kfree(reset_data);
 		return ret;
 	}
 	return 0;
```


## Bug Pattern

Freeing a work item’s heap-allocated context from the submitter after a timed wait while the queued worker still references that context.

Concretely:
- A structure embedding work_struct and a completion is allocated and queued to a workqueue.
- The submitter waits with wait_for_completion_timeout().
- On timeout it frees the context.
- The worker later dereferences the same context (to complete() or kfree()), causing a use-after-free or double-free.

Root cause: No ownership/serialization between the submitter and the worker about who frees the shared context when the wait times out. The fix ensures only one side frees it by checking completion_done() (or equivalent coordination) before using/freeing the context.


# Report

BuildSource:| drivers/crypto/intel/qat/qat_common/adf_aer.c
### Report Summary

File:| adf_aer.c  
---|---  
Warning:| line 137, column 3  
Missing completion_done() guard in worker; submitter with timed wait may free
the context  
  
### Annotated Source Code


71    | 	u16 bridge_ctl = 0;
72    |  
73    |  if (!parent)
74    | 		parent = pdev;
75    |  
76    |  if (!pci_wait_for_pending_transaction(pdev))
77    |  dev_info(&GET_DEV(accel_dev),
78    |  "Transaction still in progress. Proceeding\n");
79    |  
80    |  dev_info(&GET_DEV(accel_dev), "Secondary bus reset\n");
81    |  
82    | 	pci_read_config_word(parent, PCI_BRIDGE_CONTROL, &bridge_ctl);
83    | 	bridge_ctl |= PCI_BRIDGE_CTL_BUS_RESET;
84    | 	pci_write_config_word(parent, PCI_BRIDGE_CONTROL, bridge_ctl);
85    | 	msleep(100);
86    | 	bridge_ctl &= ~PCI_BRIDGE_CTL_BUS_RESET;
87    | 	pci_write_config_word(parent, PCI_BRIDGE_CONTROL, bridge_ctl);
88    | 	msleep(100);
89    | }
90    | EXPORT_SYMBOL_GPL(adf_reset_sbr);
91    |  
92    | void adf_reset_flr(struct adf_accel_dev *accel_dev)
93    | {
94    | 	pcie_flr(accel_to_pci_dev(accel_dev));
95    | }
96    | EXPORT_SYMBOL_GPL(adf_reset_flr);
97    |  
98    | void adf_dev_restore(struct adf_accel_dev *accel_dev)
99    | {
100   |  struct adf_hw_device_data *hw_device = accel_dev->hw_device;
101   |  struct pci_dev *pdev = accel_to_pci_dev(accel_dev);
102   |  
103   |  if (hw_device->reset_device) {
104   |  dev_info(&GET_DEV(accel_dev), "Resetting device qat_dev%d\n",
105   |  accel_dev->accel_id);
106   | 		hw_device->reset_device(accel_dev);
107   | 		pci_restore_state(pdev);
108   | 		pci_save_state(pdev);
109   | 	}
110   | }
111   |  
112   | static void adf_device_sriov_worker(struct work_struct *work)
113   | {
114   |  struct adf_sriov_dev_data *sriov_data =
115   |  container_of(work, struct adf_sriov_dev_data, sriov_work);
116   |  
117   | 	adf_reenable_sriov(sriov_data->accel_dev);
118   | 	complete(&sriov_data->compl);
119   | }
120   |  
121   | static void adf_device_reset_worker(struct work_struct *work)
122   | {
123   |  struct adf_reset_dev_data *reset_data =
124   |  container_of(work, struct adf_reset_dev_data, reset_work);
125   |  struct adf_accel_dev *accel_dev = reset_data->accel_dev;
126   |  unsigned long wait_jiffies = msecs_to_jiffies(10000);
127   |  struct adf_sriov_dev_data sriov_data;
128   |  
129   | 	adf_dev_restarting_notify(accel_dev);
130   |  if (adf_dev_restart(accel_dev)) {
    1Assuming the condition is true→
131   |  /* The device hanged and we can't restart it so stop here */
132   |  dev_err(&GET_DEV(accel_dev), "Restart device failed\n");
    2←Taking true branch→
    3←Taking true branch→
    4←'?' condition is true→
    5←'?' condition is true→
    6←Loop condition is false.  Exiting loop→
133   |  if (reset_data->mode == ADF_DEV_RESET_ASYNC ||
    7←Assuming field 'mode' is equal to ADF_DEV_RESET_ASYNC→
134   | 		    completion_done(&reset_data->compl))
135   |  kfree(reset_data);
136   |  WARN(1, "QAT: device restart failed. Device is unusable\n");
    8←Taking true branch→
    9←Loop condition is false.  Exiting loop→
    10←Loop condition is false.  Exiting loop→
    11←Loop condition is false.  Exiting loop→
137   |  return;
    12←Missing completion_done() guard in worker; submitter with timed wait may free the context
138   | 	}
139   |  
140   | 	sriov_data.accel_dev = accel_dev;
141   | 	init_completion(&sriov_data.compl);
142   |  INIT_WORK(&sriov_data.sriov_work, adf_device_sriov_worker);
143   | 	queue_work(device_sriov_wq, &sriov_data.sriov_work);
144   |  if (wait_for_completion_timeout(&sriov_data.compl, wait_jiffies))
145   | 		adf_pf2vf_notify_restarted(accel_dev);
146   |  
147   | 	adf_dev_restarted_notify(accel_dev);
148   | 	clear_bit(ADF_STATUS_RESTARTING, &accel_dev->status);
149   |  
150   |  /*
151   |  * The dev is back alive. Notify the caller if in sync mode
152   |  *
153   |  * If device restart will take a more time than expected,
154   |  * the schedule_reset() function can timeout and exit. This can be
155   |  * detected by calling the completion_done() function. In this case
156   |  * the reset_data structure needs to be freed here.
157   |  */
158   |  if (reset_data->mode == ADF_DEV_RESET_ASYNC ||
159   | 	    completion_done(&reset_data->compl))
160   | 		kfree(reset_data);
161   |  else
162   | 		complete(&reset_data->compl);
163   | }
164   |  
165   | static int adf_dev_aer_schedule_reset(struct adf_accel_dev *accel_dev,
166   |  enum adf_dev_reset_mode mode)
167   | {

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
