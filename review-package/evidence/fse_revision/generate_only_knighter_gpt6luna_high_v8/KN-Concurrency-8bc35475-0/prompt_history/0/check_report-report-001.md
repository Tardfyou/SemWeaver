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

workqueue: Fix spruious data race in __flush_work()

When flushing a work item for cancellation, __flush_work() knows that it
exclusively owns the work item through its PENDING bit. 134874e2eee9
("workqueue: Allow cancel_work_sync() and disable_work() from atomic
contexts on BH work items") added a read of @work->data to determine whether
to use busy wait for BH work items that are being canceled. While the read
is safe when @from_cancel, @work->data was read before testing @from_cancel
to simplify code structure:

	data = *work_data_bits(work);
	if (from_cancel &&
	    !WARN_ON_ONCE(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_BH)) {

While the read data was never used if !@from_cancel, this could trigger
KCSAN data race detection spuriously:

  ==================================================================
  BUG: KCSAN: data-race in __flush_work / __flush_work

  write to 0xffff8881223aa3e8 of 8 bytes by task 3998 on cpu 0:
   instrument_write include/linux/instrumented.h:41 [inline]
   ___set_bit include/asm-generic/bitops/instrumented-non-atomic.h:28 [inline]
   insert_wq_barrier kernel/workqueue.c:3790 [inline]
   start_flush_work kernel/workqueue.c:4142 [inline]
   __flush_work+0x30b/0x570 kernel/workqueue.c:4178
   flush_work kernel/workqueue.c:4229 [inline]
   ...

  read to 0xffff8881223aa3e8 of 8 bytes by task 50 on cpu 1:
   __flush_work+0x42a/0x570 kernel/workqueue.c:4188
   flush_work kernel/workqueue.c:4229 [inline]
   flush_delayed_work+0x66/0x70 kernel/workqueue.c:4251
   ...

  value changed: 0x0000000000400000 -> 0xffff88810006c00d

Reorganize the code so that @from_cancel is tested before @work->data is
accessed. The only problem is triggering KCSAN detection spuriously. This
shouldn't need READ_ONCE() or other access qualifiers.

No functional changes.

Signed-off-by: Tejun Heo <tj@kernel.org>
Reported-by: syzbot+b3e4f2f51ed645fd5df2@syzkaller.appspotmail.com
Fixes: 134874e2eee9 ("workqueue: Allow cancel_work_sync() and disable_work() from atomic contexts on BH work items")
Link: http://lkml.kernel.org/r/000000000000ae429e061eea2157@google.com
Cc: Jens Axboe <axboe@kernel.dk>

## Buggy Code

```c
// Function: __flush_work in kernel/workqueue.c
static bool __flush_work(struct work_struct *work, bool from_cancel)
{
	struct wq_barrier barr;
	unsigned long data;

	if (WARN_ON(!wq_online))
		return false;

	if (WARN_ON(!work->func))
		return false;

	if (!start_flush_work(work, &barr, from_cancel))
		return false;

	/*
	 * start_flush_work() returned %true. If @from_cancel is set, we know
	 * that @work must have been executing during start_flush_work() and
	 * can't currently be queued. Its data must contain OFFQ bits. If @work
	 * was queued on a BH workqueue, we also know that it was running in the
	 * BH context and thus can be busy-waited.
	 */
	data = *work_data_bits(work);
	if (from_cancel &&
	    !WARN_ON_ONCE(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_BH)) {
		/*
		 * On RT, prevent a live lock when %current preempted soft
		 * interrupt processing or prevents ksoftirqd from running by
		 * keeping flipping BH. If the BH work item runs on a different
		 * CPU then this has no effect other than doing the BH
		 * disable/enable dance for nothing. This is copied from
		 * kernel/softirq.c::tasklet_unlock_spin_wait().
		 */
		while (!try_wait_for_completion(&barr.done)) {
			if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
				local_bh_disable();
				local_bh_enable();
			} else {
				cpu_relax();
			}
		}
	} else {
		wait_for_completion(&barr.done);
	}

	destroy_work_on_stack(&barr.work);
	return true;
}
```

## Bug Fix Patch

```diff
diff --git a/kernel/workqueue.c b/kernel/workqueue.c
index d56bd2277e58..ef174d8c1f63 100644
--- a/kernel/workqueue.c
+++ b/kernel/workqueue.c
@@ -4166,7 +4166,6 @@ static bool start_flush_work(struct work_struct *work, struct wq_barrier *barr,
 static bool __flush_work(struct work_struct *work, bool from_cancel)
 {
 	struct wq_barrier barr;
-	unsigned long data;

 	if (WARN_ON(!wq_online))
 		return false;
@@ -4184,29 +4183,35 @@ static bool __flush_work(struct work_struct *work, bool from_cancel)
 	 * was queued on a BH workqueue, we also know that it was running in the
 	 * BH context and thus can be busy-waited.
 	 */
-	data = *work_data_bits(work);
-	if (from_cancel &&
-	    !WARN_ON_ONCE(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_BH)) {
-		/*
-		 * On RT, prevent a live lock when %current preempted soft
-		 * interrupt processing or prevents ksoftirqd from running by
-		 * keeping flipping BH. If the BH work item runs on a different
-		 * CPU then this has no effect other than doing the BH
-		 * disable/enable dance for nothing. This is copied from
-		 * kernel/softirq.c::tasklet_unlock_spin_wait().
-		 */
-		while (!try_wait_for_completion(&barr.done)) {
-			if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
-				local_bh_disable();
-				local_bh_enable();
-			} else {
-				cpu_relax();
+	if (from_cancel) {
+		unsigned long data = *work_data_bits(work);
+
+		if (!WARN_ON_ONCE(data & WORK_STRUCT_PWQ) &&
+		    (data & WORK_OFFQ_BH)) {
+			/*
+			 * On RT, prevent a live lock when %current preempted
+			 * soft interrupt processing or prevents ksoftirqd from
+			 * running by keeping flipping BH. If the BH work item
+			 * runs on a different CPU then this has no effect other
+			 * than doing the BH disable/enable dance for nothing.
+			 * This is copied from
+			 * kernel/softirq.c::tasklet_unlock_spin_wait().
+			 */
+			while (!try_wait_for_completion(&barr.done)) {
+				if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
+					local_bh_disable();
+					local_bh_enable();
+				} else {
+					cpu_relax();
+				}
 			}
+			goto out_destroy;
 		}
-	} else {
-		wait_for_completion(&barr.done);
 	}

+	wait_for_completion(&barr.done);
+
+out_destroy:
 	destroy_work_on_stack(&barr.work);
 	return true;
 }
```


## Bug Pattern

Unconditionally reading a shared/concurrently modified field before checking the precondition that guarantees safe access, even when the value is only needed in one guarded branch. This speculative/early read happens without synchronization and can race with concurrent writers, triggering KCSAN/data-race reports despite the value being discarded when the guard is false.

Pattern example:
- Buggy:
  - data = *shared_field;
  - if (safe_to_access) use data;
- Correct:
  - if (safe_to_access) {
      data = *shared_field;
      use data;
    }

In the patch: reading work->data occurred before verifying from_cancel (the condition that implies exclusive ownership), causing a spurious data race. Moving the read inside the from_cancel branch fixes it.


# Report

BuildSource:| kernel/workqueue.c
### Report Summary

File:| kernel/workqueue.c  
---|---  
Warning:| line 7499, column 7  
Speculative read of shared state before guard; move the read inside the
guarded branch  
  
### Annotated Source Code


7415  |  int bkt;
7416  |  
7417  |  raw_spin_lock_irqsave(&pool->lock, irq_flags);
7418  |  
7419  |  hash_for_each(pool->busy_hash, bkt, worker, hentry) {
7420  |  if (task_is_running(worker->task)) {
7421  |  /*
7422  |  * Defer printing to avoid deadlocks in console
7423  |  * drivers that queue work while holding locks
7424  |  * also taken in their write paths.
7425  |  */
7426  |  printk_deferred_enter();
7427  |  
7428  |  pr_info("pool %d:\n", pool->id);
7429  | 			sched_show_task(worker->task);
7430  |  
7431  |  printk_deferred_exit();
7432  | 		}
7433  | 	}
7434  |  
7435  |  raw_spin_unlock_irqrestore(&pool->lock, irq_flags);
7436  | }
7437  |  
7438  | static void show_cpu_pools_hogs(void)
7439  | {
7440  |  struct worker_pool *pool;
7441  |  int pi;
7442  |  
7443  |  pr_info("Showing backtraces of running workers in stalled CPU-bound worker pools:\n");
7444  |  
7445  | 	rcu_read_lock();
7446  |  
7447  |  for_each_pool(pool, pi) {
7448  |  if (pool->cpu_stall)
7449  | 			show_cpu_pool_hog(pool);
7450  |  
7451  | 	}
7452  |  
7453  | 	rcu_read_unlock();
7454  | }
7455  |  
7456  | static void wq_watchdog_reset_touched(void)
7457  | {
7458  |  int cpu;
7459  |  
7460  | 	wq_watchdog_touched = jiffies;
7461  |  for_each_possible_cpu(cpu)
7462  |  per_cpu(wq_watchdog_touched_cpu, cpu) = jiffies;
7463  | }
7464  |  
7465  | static void wq_watchdog_timer_fn(struct timer_list *unused)
7466  | {
7467  |  unsigned long thresh = READ_ONCE(wq_watchdog_thresh) * HZ;
    1Taking false branch→
    2←Loop condition is false.  Exiting loop→
7468  | 	bool lockup_detected = false;
7469  | 	bool cpu_pool_stall = false;
7470  |  unsigned long now = jiffies;
7471  |  struct worker_pool *pool;
7472  |  int pi;
7473  |  
7474  |  if (!thresh)
    3←Assuming 'thresh' is not equal to 0→
    4←Taking false branch→
7475  |  return;
7476  |  
7477  |  rcu_read_lock();
7478  |  
7479  |  for_each_pool(pool, pi) {
    5←Assuming the condition is true→
    6←Loop condition is true.  Entering loop body→
    7←Assuming the condition is false→
    8←Loop condition is false.  Exiting loop→
    9←Taking false branch→
7480  |  unsigned long pool_ts, touched, ts;
7481  |  
7482  | 		pool->cpu_stall = false;
7483  |  if (list_empty(&pool->worklist))
    10←Assuming the condition is false→
    11←Taking false branch→
7484  |  continue;
7485  |  
7486  |  /*
7487  |  * If a virtual machine is stopped by the host it can look to
7488  |  * the watchdog like a stall.
7489  |  */
7490  |  kvm_check_and_clear_guest_paused();
7491  |  
7492  |  /* get the latest of pool and touched timestamps */
7493  |  if (pool->cpu >= 0)
    12←Assuming field 'cpu' is < 0→
7494  | 			touched = READ_ONCE(per_cpu(wq_watchdog_touched_cpu, pool->cpu));
7495  |  else
7496  |  touched = READ_ONCE(wq_watchdog_touched);
    13←Taking false branch→
    14←Taking false branch→
    15←Loop condition is false.  Exiting loop→
7497  |  pool_ts = READ_ONCE(pool->watchdog_ts);
    16←Taking false branch→
    17←Loop condition is false.  Exiting loop→
7498  |  
7499  |  if (time_after(pool_ts, touched))
    18←Speculative read of shared state before guard; move the read inside the guarded branch
7500  | 			ts = pool_ts;
7501  |  else
7502  | 			ts = touched;
7503  |  
7504  |  /* did we stall? */
7505  |  if (time_after(now, ts + thresh)) {
7506  | 			lockup_detected = true;
7507  |  if (pool->cpu >= 0 && !(pool->flags & POOL_BH)) {
7508  | 				pool->cpu_stall = true;
7509  | 				cpu_pool_stall = true;
7510  | 			}
7511  |  pr_emerg("BUG: workqueue lockup - pool");
7512  | 			pr_cont_pool_info(pool);
7513  |  pr_cont(" stuck for %us!\n",
7514  |  jiffies_to_msecs(now - pool_ts) / 1000);
7515  | 		}
7516  |  
7517  |  
7518  | 	}
7519  |  
7520  | 	rcu_read_unlock();
7521  |  
7522  |  if (lockup_detected)
7523  | 		show_all_workqueues();
7524  |  
7525  |  if (cpu_pool_stall)
7526  | 		show_cpu_pools_hogs();
7527  |  
7528  | 	wq_watchdog_reset_touched();
7529  | 	mod_timer(&wq_watchdog_timer, jiffies + thresh);

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
