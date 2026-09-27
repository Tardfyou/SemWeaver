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
Warning:| line 6636, column 4  
Speculative read of shared state before guard; move the read inside the
guarded branch  
  
### Annotated Source Code


6375  |  mutex_lock(&wq_pool_attach_mutex);
6376  |  
6377  |  if (task->flags & PF_WQ_WORKER) {
6378  |  struct worker *worker = kthread_data(task);
6379  |  struct worker_pool *pool = worker->pool;
6380  |  int off;
6381  |  
6382  | 		off = format_worker_id(buf, size, worker, pool);
6383  |  
6384  |  if (pool) {
6385  |  raw_spin_lock_irq(&pool->lock);
6386  |  /*
6387  |  * ->desc tracks information (wq name or
6388  |  * set_worker_desc()) for the latest execution.  If
6389  |  * current, prepend '+', otherwise '-'.
6390  |  */
6391  |  if (worker->desc[0] != '\0') {
6392  |  if (worker->current_work)
6393  | 					scnprintf(buf + off, size - off, "+%s",
6394  | 						  worker->desc);
6395  |  else
6396  | 					scnprintf(buf + off, size - off, "-%s",
6397  | 						  worker->desc);
6398  | 			}
6399  |  raw_spin_unlock_irq(&pool->lock);
6400  | 		}
6401  | 	} else {
6402  |  strscpy(buf, task->comm, size);
6403  | 	}
6404  |  
6405  | 	mutex_unlock(&wq_pool_attach_mutex);
6406  | }
6407  |  
6408  | #ifdef CONFIG_SMP
6409  |  
6410  | /*
6411  |  * CPU hotplug.
6412  |  *
6413  |  * There are two challenges in supporting CPU hotplug.  Firstly, there
6414  |  * are a lot of assumptions on strong associations among work, pwq and
6415  |  * pool which make migrating pending and scheduled works very
6416  |  * difficult to implement without impacting hot paths.  Secondly,
6417  |  * worker pools serve mix of short, long and very long running works making
6418  |  * blocked draining impractical.
6419  |  *
6420  |  * This is solved by allowing the pools to be disassociated from the CPU
6421  |  * running as an unbound one and allowing it to be reattached later if the
6422  |  * cpu comes back online.
6423  |  */
6424  |  
6425  | static void unbind_workers(int cpu)
6426  | {
6427  |  struct worker_pool *pool;
6428  |  struct worker *worker;
6429  |  
6430  |  for_each_cpu_worker_pool(pool, cpu) {
6431  |  mutex_lock(&wq_pool_attach_mutex);
6432  |  raw_spin_lock_irq(&pool->lock);
6433  |  
6434  |  /*
6435  |  * We've blocked all attach/detach operations. Make all workers
6436  |  * unbound and set DISASSOCIATED.  Before this, all workers
6437  |  * must be on the cpu.  After this, they may become diasporas.
6438  |  * And the preemption disabled section in their sched callbacks
6439  |  * are guaranteed to see WORKER_UNBOUND since the code here
6440  |  * is on the same cpu.
6441  |  */
6442  |  for_each_pool_worker(worker, pool)
6443  | 			worker->flags |= WORKER_UNBOUND;
6444  |  
6445  | 		pool->flags |= POOL_DISASSOCIATED;
6446  |  
6447  |  /*
6448  |  * The handling of nr_running in sched callbacks are disabled
6449  |  * now.  Zap nr_running.  After this, nr_running stays zero and
6450  |  * need_more_worker() and keep_working() are always true as
6451  |  * long as the worklist is not empty.  This pool now behaves as
6452  |  * an unbound (in terms of concurrency management) pool which
6453  |  * are served by workers tied to the pool.
6454  |  */
6455  | 		pool->nr_running = 0;
6456  |  
6457  |  /*
6458  |  * With concurrency management just turned off, a busy
6459  |  * worker blocking could lead to lengthy stalls.  Kick off
6460  |  * unbound chain execution of currently pending work items.
6564  |  if (!create_worker(pool))
6565  |  return -ENOMEM;
6566  | 	}
6567  |  return 0;
6568  | }
6569  |  
6570  | int workqueue_online_cpu(unsigned int cpu)
6571  | {
6572  |  struct worker_pool *pool;
6573  |  struct workqueue_struct *wq;
6574  |  int pi;
6575  |  
6576  |  mutex_lock(&wq_pool_mutex);
6577  |  
6578  | 	cpumask_set_cpu(cpu, wq_online_cpumask);
6579  |  
6580  |  for_each_pool(pool, pi) {
6581  |  /* BH pools aren't affected by hotplug */
6582  |  if (pool->flags & POOL_BH)
6583  |  continue;
6584  |  
6585  |  mutex_lock(&wq_pool_attach_mutex);
6586  |  if (pool->cpu == cpu)
6587  | 			rebind_workers(pool);
6588  |  else if (pool->cpu < 0)
6589  | 			restore_unbound_workers_cpumask(pool, cpu);
6590  | 		mutex_unlock(&wq_pool_attach_mutex);
6591  | 	}
6592  |  
6593  |  /* update pod affinity of unbound workqueues */
6594  |  list_for_each_entry(wq, &workqueues, list) {
6595  |  struct workqueue_attrs *attrs = wq->unbound_attrs;
6596  |  
6597  |  if (attrs) {
6598  |  const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
6599  |  int tcpu;
6600  |  
6601  |  for_each_cpu(tcpu, pt->pod_cpus[pt->cpu_pod[cpu]])
6602  | 				unbound_wq_update_pwq(wq, tcpu);
6603  |  
6604  |  mutex_lock(&wq->mutex);
6605  | 			wq_update_node_max_active(wq, -1);
6606  | 			mutex_unlock(&wq->mutex);
6607  | 		}
6608  | 	}
6609  |  
6610  | 	mutex_unlock(&wq_pool_mutex);
6611  |  return 0;
6612  | }
6613  |  
6614  | int workqueue_offline_cpu(unsigned int cpu)
6615  | {
6616  |  struct workqueue_struct *wq;
6617  |  
6618  |  /* unbinding per-cpu workers should happen on the local CPU */
6619  |  if (WARN_ON(cpu != smp_processor_id()))
    1Assuming the condition is false→
    2←Taking false branch→
    3←Taking false branch→
6620  |  return -1;
6621  |  
6622  |  unbind_workers(cpu);
6623  |  
6624  |  /* update pod affinity of unbound workqueues */
6625  |  mutex_lock(&wq_pool_mutex);
6626  |  
6627  |  cpumask_clear_cpu(cpu, wq_online_cpumask);
6628  |  
6629  |  list_for_each_entry(wq, &workqueues, list) {
    4←Loop condition is true.  Entering loop body→
6630  |  struct workqueue_attrs *attrs = wq->unbound_attrs;
6631  |  
6632  |  if (attrs) {
    5←Assuming 'attrs' is non-null→
    6←Taking true branch→
6633  |  const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
6634  |  int tcpu;
6635  |  
6636  |  for_each_cpu(tcpu, pt->pod_cpus[pt->cpu_pod[cpu]])
    7←Assuming 'tcpu' is >= 'nr_cpu_ids'→
    8←Speculative read of shared state before guard; move the read inside the guarded branch
6637  | 				unbound_wq_update_pwq(wq, tcpu);
6638  |  
6639  |  mutex_lock(&wq->mutex);
6640  | 			wq_update_node_max_active(wq, cpu);
6641  | 			mutex_unlock(&wq->mutex);
6642  | 		}
6643  | 	}
6644  | 	mutex_unlock(&wq_pool_mutex);
6645  |  
6646  |  return 0;
6647  | }
6648  |  
6649  | struct work_for_cpu {
6650  |  struct work_struct work;
6651  |  long (*fn)(void *);
6652  |  void *arg;
6653  |  long ret;
6654  | };
6655  |  
6656  | static void work_for_cpu_fn(struct work_struct *work)
6657  | {
6658  |  struct work_for_cpu *wfc = container_of(work, struct work_for_cpu, work);
6659  |  
6660  | 	wfc->ret = wfc->fn(wfc->arg);
6661  | }
6662  |  
6663  | /**
6664  |  * work_on_cpu_key - run a function in thread context on a particular cpu
6665  |  * @cpu: the cpu to run on
6666  |  * @fn: the function to run

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
