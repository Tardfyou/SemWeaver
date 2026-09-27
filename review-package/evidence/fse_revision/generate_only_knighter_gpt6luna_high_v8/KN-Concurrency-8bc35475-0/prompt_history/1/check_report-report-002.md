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
Warning:| line 4708, column 6  
Speculative read of shared state before guard; move the read inside the
guarded branch  
  
### Annotated Source Code


4568  |  INIT_WORK(&ew->work, fn);
4569  | 	schedule_work(&ew->work);
4570  |  
4571  |  return 1;
4572  | }
4573  | EXPORT_SYMBOL_GPL(execute_in_process_context);
4574  |  
4575  | /**
4576  |  * free_workqueue_attrs - free a workqueue_attrs
4577  |  * @attrs: workqueue_attrs to free
4578  |  *
4579  |  * Undo alloc_workqueue_attrs().
4580  |  */
4581  | void free_workqueue_attrs(struct workqueue_attrs *attrs)
4582  | {
4583  |  if (attrs) {
4584  | 		free_cpumask_var(attrs->cpumask);
4585  | 		free_cpumask_var(attrs->__pod_cpumask);
4586  | 		kfree(attrs);
4587  | 	}
4588  | }
4589  |  
4590  | /**
4591  |  * alloc_workqueue_attrs - allocate a workqueue_attrs
4592  |  *
4593  |  * Allocate a new workqueue_attrs, initialize with default settings and
4594  |  * return it.
4595  |  *
4596  |  * Return: The allocated new workqueue_attr on success. %NULL on failure.
4597  |  */
4598  | struct workqueue_attrs *alloc_workqueue_attrs(void)
4599  | {
4600  |  struct workqueue_attrs *attrs;
4601  |  
4602  | 	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
4603  |  if (!attrs)
4604  |  goto fail;
4605  |  if (!alloc_cpumask_var(&attrs->cpumask, GFP_KERNEL))
4606  |  goto fail;
4607  |  if (!alloc_cpumask_var(&attrs->__pod_cpumask, GFP_KERNEL))
4608  |  goto fail;
4609  |  
4610  | 	cpumask_copy(attrs->cpumask, cpu_possible_mask);
4611  | 	attrs->affn_scope = WQ_AFFN_DFL;
4612  |  return attrs;
4613  | fail:
4614  | 	free_workqueue_attrs(attrs);
4615  |  return NULL;
4616  | }
4617  |  
4618  | static void copy_workqueue_attrs(struct workqueue_attrs *to,
4619  |  const struct workqueue_attrs *from)
4620  | {
4621  | 	to->nice = from->nice;
4622  | 	cpumask_copy(to->cpumask, from->cpumask);
4623  | 	cpumask_copy(to->__pod_cpumask, from->__pod_cpumask);
4624  | 	to->affn_strict = from->affn_strict;
4625  |  
4626  |  /*
4627  |  * Unlike hash and equality test, copying shouldn't ignore wq-only
4628  |  * fields as copying is used for both pool and wq attrs. Instead,
4629  |  * get_unbound_pool() explicitly clears the fields.
4630  |  */
4631  | 	to->affn_scope = from->affn_scope;
4632  | 	to->ordered = from->ordered;
4633  | }
4634  |  
4635  | /*
4636  |  * Some attrs fields are workqueue-only. Clear them for worker_pool's. See the
4637  |  * comments in 'struct workqueue_attrs' definition.
4638  |  */
4639  | static void wqattrs_clear_for_pool(struct workqueue_attrs *attrs)
4640  | {
4641  | 	attrs->affn_scope = WQ_AFFN_NR_TYPES;
4642  | 	attrs->ordered = false;
4643  |  if (attrs->affn_strict)
4644  | 		cpumask_copy(attrs->cpumask, cpu_possible_mask);
4645  | }
4646  |  
4647  | /* hash value of the content of @attr */
4648  | static u32 wqattrs_hash(const struct workqueue_attrs *attrs)
4649  | {
4650  | 	u32 hash = 0;
4651  |  
4652  | 	hash = jhash_1word(attrs->nice, hash);
4653  | 	hash = jhash_1word(attrs->affn_strict, hash);
4654  | 	hash = jhash(cpumask_bits(attrs->__pod_cpumask),
4655  |  BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
4656  |  if (!attrs->affn_strict)
4657  | 		hash = jhash(cpumask_bits(attrs->cpumask),
4658  |  BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
4659  |  return hash;
4660  | }
4661  |  
4662  | /* content equality test */
4663  | static bool wqattrs_equal(const struct workqueue_attrs *a,
4664  |  const struct workqueue_attrs *b)
4665  | {
4666  |  if (a->nice != b->nice)
4667  |  return false;
4668  |  if (a->affn_strict != b->affn_strict)
4669  |  return false;
4670  |  if (!cpumask_equal(a->__pod_cpumask, b->__pod_cpumask))
4671  |  return false;
4672  |  if (!a->affn_strict && !cpumask_equal(a->cpumask, b->cpumask))
4673  |  return false;
4674  |  return true;
4675  | }
4676  |  
4677  | /* Update @attrs with actually available CPUs */
4678  | static void wqattrs_actualize_cpumask(struct workqueue_attrs *attrs,
4679  |  const cpumask_t *unbound_cpumask)
4680  | {
4681  |  /*
4682  |  * Calculate the effective CPU mask of @attrs given @unbound_cpumask. If
4683  |  * @attrs->cpumask doesn't overlap with @unbound_cpumask, we fallback to
4684  |  * @unbound_cpumask.
4685  |  */
4686  | 	cpumask_and(attrs->cpumask, attrs->cpumask, unbound_cpumask);
4687  |  if (unlikely(cpumask_empty(attrs->cpumask)))
4688  | 		cpumask_copy(attrs->cpumask, unbound_cpumask);
4689  | }
4690  |  
4691  | /* find wq_pod_type to use for @attrs */
4692  | static const struct wq_pod_type *
4693  | wqattrs_pod_type(const struct workqueue_attrs *attrs)
4694  | {
4695  |  enum wq_affn_scope scope;
4696  |  struct wq_pod_type *pt;
4697  |  
4698  |  /* to synchronize access to wq_affn_dfl */
4699  |  lockdep_assert_held(&wq_pool_mutex);
    12←Assuming 'debug_locks' is 0→
    13←Taking false branch→
    14←Loop condition is false.  Exiting loop→
4700  |  
4701  |  if (attrs->affn_scope == WQ_AFFN_DFL)
    15←Assuming field 'affn_scope' is equal to WQ_AFFN_DFL→
    16←Taking true branch→
4702  |  scope = wq_affn_dfl;
4703  |  else
4704  | 		scope = attrs->affn_scope;
4705  |  
4706  |  pt = &wq_pod_types[scope];
4707  |  
4708  |  if (!WARN_ON_ONCE(attrs->affn_scope == WQ_AFFN_NR_TYPES) &&
    17←Taking false branch→
    18←Speculative read of shared state before guard; move the read inside the guarded branch
4709  |  likely(pt->nr_pods))
4710  |  return pt;
4711  |  
4712  |  /*
4713  |  * Before workqueue_init_topology(), only SYSTEM is available which is
4714  |  * initialized in workqueue_init_early().
4715  |  */
4716  | 	pt = &wq_pod_types[WQ_AFFN_SYSTEM];
4717  |  BUG_ON(!pt->nr_pods);
4718  |  return pt;
4719  | }
4720  |  
4721  | /**
4722  |  * init_worker_pool - initialize a newly zalloc'd worker_pool
4723  |  * @pool: worker_pool to initialize
4724  |  *
4725  |  * Initialize a newly zalloc'd @pool.  It also allocates @pool->attrs.
4726  |  *
4727  |  * Return: 0 on success, -errno on failure.  Even on failure, all fields
4728  |  * inside @pool proper are initialized and put_unbound_pool() can be called
4729  |  * on @pool safely to release it.
4730  |  */
4731  | static int init_worker_pool(struct worker_pool *pool)
4732  | {
4733  |  raw_spin_lock_init(&pool->lock);
4734  | 	pool->id = -1;
4735  | 	pool->cpu = -1;
4736  | 	pool->node = NUMA_NO_NODE;
4737  | 	pool->flags |= POOL_DISASSOCIATED;
4738  | 	pool->watchdog_ts = jiffies;
5112  | 	list_add_tail_rcu(&pwq->pwqs_node, &wq->pwqs);
5113  | }
5114  |  
5115  | /* obtain a pool matching @attr and create a pwq associating the pool and @wq */
5116  | static struct pool_workqueue *alloc_unbound_pwq(struct workqueue_struct *wq,
5117  |  const struct workqueue_attrs *attrs)
5118  | {
5119  |  struct worker_pool *pool;
5120  |  struct pool_workqueue *pwq;
5121  |  
5122  |  lockdep_assert_held(&wq_pool_mutex);
5123  |  
5124  | 	pool = get_unbound_pool(attrs);
5125  |  if (!pool)
5126  |  return NULL;
5127  |  
5128  | 	pwq = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL, pool->node);
5129  |  if (!pwq) {
5130  | 		put_unbound_pool(pool);
5131  |  return NULL;
5132  | 	}
5133  |  
5134  | 	init_pwq(pwq, wq, pool);
5135  |  return pwq;
5136  | }
5137  |  
5138  | static void apply_wqattrs_lock(void)
5139  | {
5140  |  mutex_lock(&wq_pool_mutex);
5141  | }
5142  |  
5143  | static void apply_wqattrs_unlock(void)
5144  | {
5145  | 	mutex_unlock(&wq_pool_mutex);
5146  | }
5147  |  
5148  | /**
5149  |  * wq_calc_pod_cpumask - calculate a wq_attrs' cpumask for a pod
5150  |  * @attrs: the wq_attrs of the default pwq of the target workqueue
5151  |  * @cpu: the target CPU
5152  |  *
5153  |  * Calculate the cpumask a workqueue with @attrs should use on @pod.
5154  |  * The result is stored in @attrs->__pod_cpumask.
5155  |  *
5156  |  * If pod affinity is not enabled, @attrs->cpumask is always used. If enabled
5157  |  * and @pod has online CPUs requested by @attrs, the returned cpumask is the
5158  |  * intersection of the possible CPUs of @pod and @attrs->cpumask.
5159  |  *
5160  |  * The caller is responsible for ensuring that the cpumask of @pod stays stable.
5161  |  */
5162  | static void wq_calc_pod_cpumask(struct workqueue_attrs *attrs, int cpu)
5163  | {
5164  |  const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
    11←Calling 'wqattrs_pod_type'→
5165  |  int pod = pt->cpu_pod[cpu];
5166  |  
5167  |  /* calculate possible CPUs in @pod that @attrs wants */
5168  | 	cpumask_and(attrs->__pod_cpumask, pt->pod_cpus[pod], attrs->cpumask);
5169  |  /* does @pod have any online CPUs @attrs wants? */
5170  |  if (!cpumask_intersects(attrs->__pod_cpumask, wq_online_cpumask)) {
5171  | 		cpumask_copy(attrs->__pod_cpumask, attrs->cpumask);
5172  |  return;
5173  | 	}
5174  | }
5175  |  
5176  | /* install @pwq into @wq and return the old pwq, @cpu < 0 for dfl_pwq */
5177  | static struct pool_workqueue *install_unbound_pwq(struct workqueue_struct *wq,
5178  |  int cpu, struct pool_workqueue *pwq)
5179  | {
5180  |  struct pool_workqueue __rcu **slot = unbound_pwq_slot(wq, cpu);
5181  |  struct pool_workqueue *old_pwq;
5182  |  
5183  |  lockdep_assert_held(&wq_pool_mutex);
5184  |  lockdep_assert_held(&wq->mutex);
5185  |  
5186  |  /* link_pwq() can handle duplicate calls */
5187  | 	link_pwq(pwq);
5188  |  
5189  | 	old_pwq = rcu_access_pointer(*slot);
5190  |  rcu_assign_pointer(*slot, pwq);
5191  |  return old_pwq;
5192  | }
5193  |  
5194  | /* context to store the prepared attrs & pwqs before applying */
5332  |  
5333  |  return 0;
5334  | }
5335  |  
5336  | /**
5337  |  * apply_workqueue_attrs - apply new workqueue_attrs to an unbound workqueue
5338  |  * @wq: the target workqueue
5339  |  * @attrs: the workqueue_attrs to apply, allocated with alloc_workqueue_attrs()
5340  |  *
5341  |  * Apply @attrs to an unbound workqueue @wq. Unless disabled, this function maps
5342  |  * a separate pwq to each CPU pod with possibles CPUs in @attrs->cpumask so that
5343  |  * work items are affine to the pod it was issued on. Older pwqs are released as
5344  |  * in-flight work items finish. Note that a work item which repeatedly requeues
5345  |  * itself back-to-back will stay on its current pwq.
5346  |  *
5347  |  * Performs GFP_KERNEL allocations.
5348  |  *
5349  |  * Return: 0 on success and -errno on failure.
5350  |  */
5351  | int apply_workqueue_attrs(struct workqueue_struct *wq,
5352  |  const struct workqueue_attrs *attrs)
5353  | {
5354  |  int ret;
5355  |  
5356  |  mutex_lock(&wq_pool_mutex);
5357  | 	ret = apply_workqueue_attrs_locked(wq, attrs);
5358  | 	mutex_unlock(&wq_pool_mutex);
5359  |  
5360  |  return ret;
5361  | }
5362  |  
5363  | /**
5364  |  * unbound_wq_update_pwq - update a pwq slot for CPU hot[un]plug
5365  |  * @wq: the target workqueue
5366  |  * @cpu: the CPU to update the pwq slot for
5367  |  *
5368  |  * This function is to be called from %CPU_DOWN_PREPARE, %CPU_ONLINE and
5369  |  * %CPU_DOWN_FAILED.  @cpu is in the same pod of the CPU being hot[un]plugged.
5370  |  *
5371  |  *
5372  |  * If pod affinity can't be adjusted due to memory allocation failure, it falls
5373  |  * back to @wq->dfl_pwq which may not be optimal but is always correct.
5374  |  *
5375  |  * Note that when the last allowed CPU of a pod goes offline for a workqueue
5376  |  * with a cpumask spanning multiple pods, the workers which were already
5377  |  * executing the work items for the workqueue will lose their CPU affinity and
5378  |  * may execute on any CPU. This is similar to how per-cpu workqueues behave on
5379  |  * CPU_DOWN. If a workqueue user wants strict affinity, it's the user's
5380  |  * responsibility to flush the work item from CPU_DOWN_PREPARE.
5381  |  */
5382  | static void unbound_wq_update_pwq(struct workqueue_struct *wq, int cpu)
5383  | {
5384  |  struct pool_workqueue *old_pwq = NULL, *pwq;
5385  |  struct workqueue_attrs *target_attrs;
5386  |  
5387  |  lockdep_assert_held(&wq_pool_mutex);
    5←Assuming 'debug_locks' is 0→
    6←Taking false branch→
5388  |  
5389  |  if (!(wq->flags & WQ_UNBOUND) || wq->unbound_attrs->ordered)
    7←Assuming the condition is false→
    8←Assuming field 'ordered' is false→
    9←Taking false branch→
5390  |  return;
5391  |  
5392  |  /*
5393  |  * We don't wanna alloc/free wq_attrs for each wq for each CPU.
5394  |  * Let's use a preallocated one.  The following buf is protected by
5395  |  * CPU hotplug exclusion.
5396  |  */
5397  |  target_attrs = unbound_wq_update_pwq_attrs_buf;
5398  |  
5399  | 	copy_workqueue_attrs(target_attrs, wq->unbound_attrs);
5400  | 	wqattrs_actualize_cpumask(target_attrs, wq_unbound_cpumask);
5401  |  
5402  |  /* nothing to do if the target cpumask matches the current pwq */
5403  |  wq_calc_pod_cpumask(target_attrs, cpu);
    10←Calling 'wq_calc_pod_cpumask'→
5404  |  if (wqattrs_equal(target_attrs, unbound_pwq(wq, cpu)->pool->attrs))
5405  |  return;
5406  |  
5407  |  /* create a new pwq */
5408  | 	pwq = alloc_unbound_pwq(wq, target_attrs);
5409  |  if (!pwq) {
5410  |  pr_warn("workqueue: allocation failed while updating CPU pod affinity of \"%s\"\n",
5411  |  wq->name);
5412  |  goto use_dfl_pwq;
5413  | 	}
5414  |  
5415  |  /* Install the new pwq. */
5416  |  mutex_lock(&wq->mutex);
5417  | 	old_pwq = install_unbound_pwq(wq, cpu, pwq);
5418  |  goto out_unlock;
5419  |  
5420  | use_dfl_pwq:
5421  |  mutex_lock(&wq->mutex);
5422  | 	pwq = unbound_pwq(wq, -1);
5423  |  raw_spin_lock_irq(&pwq->pool->lock);
5424  | 	get_pwq(pwq);
5425  |  raw_spin_unlock_irq(&pwq->pool->lock);
5426  | 	old_pwq = install_unbound_pwq(wq, cpu, pwq);
5427  | out_unlock:
5428  | 	mutex_unlock(&wq->mutex);
5429  | 	put_pwq_unlocked(old_pwq);
5430  | }
5431  |  
5432  | static int alloc_and_link_pwqs(struct workqueue_struct *wq)
5433  | {
7871  | 				pt->cpu_pod[cur] = pt->nr_pods++;
7872  |  break;
7873  | 			}
7874  |  if (cpus_share_pod(cur, pre)) {
7875  | 				pt->cpu_pod[cur] = pt->cpu_pod[pre];
7876  |  break;
7877  | 			}
7878  | 		}
7879  | 	}
7880  |  
7881  |  /* init the rest to match @pt->cpu_pod[] */
7882  | 	pt->pod_cpus = kcalloc(pt->nr_pods, sizeof(pt->pod_cpus[0]), GFP_KERNEL);
7883  | 	pt->pod_node = kcalloc(pt->nr_pods, sizeof(pt->pod_node[0]), GFP_KERNEL);
7884  |  BUG_ON(!pt->pod_cpus || !pt->pod_node);
7885  |  
7886  |  for (pod = 0; pod < pt->nr_pods; pod++)
7887  |  BUG_ON(!zalloc_cpumask_var(&pt->pod_cpus[pod], GFP_KERNEL));
7888  |  
7889  |  for_each_possible_cpu(cpu) {
7890  | 		cpumask_set_cpu(cpu, pt->pod_cpus[pt->cpu_pod[cpu]]);
7891  | 		pt->pod_node[pt->cpu_pod[cpu]] = cpu_to_node(cpu);
7892  | 	}
7893  | }
7894  |  
7895  | static bool __init cpus_dont_share(int cpu0, int cpu1)
7896  | {
7897  |  return false;
7898  | }
7899  |  
7900  | static bool __init cpus_share_smt(int cpu0, int cpu1)
7901  | {
7902  | #ifdef CONFIG_SCHED_SMT
7903  |  return cpumask_test_cpu(cpu0, cpu_smt_mask(cpu1));
7904  | #else
7905  |  return false;
7906  | #endif
7907  | }
7908  |  
7909  | static bool __init cpus_share_numa(int cpu0, int cpu1)
7910  | {
7911  |  return cpu_to_node(cpu0) == cpu_to_node(cpu1);
7912  | }
7913  |  
7914  | /**
7915  |  * workqueue_init_topology - initialize CPU pods for unbound workqueues
7916  |  *
7917  |  * This is the third step of three-staged workqueue subsystem initialization and
7918  |  * invoked after SMP and topology information are fully initialized. It
7919  |  * initializes the unbound CPU pods accordingly.
7920  |  */
7921  | void __init workqueue_init_topology(void)
7922  | {
7923  |  struct workqueue_struct *wq;
7924  |  int cpu;
7925  |  
7926  | 	init_pod_type(&wq_pod_types[WQ_AFFN_CPU], cpus_dont_share);
7927  | 	init_pod_type(&wq_pod_types[WQ_AFFN_SMT], cpus_share_smt);
7928  | 	init_pod_type(&wq_pod_types[WQ_AFFN_CACHE], cpus_share_cache);
7929  | 	init_pod_type(&wq_pod_types[WQ_AFFN_NUMA], cpus_share_numa);
7930  |  
7931  | 	wq_topo_initialized = true;
7932  |  
7933  |  mutex_lock(&wq_pool_mutex);
7934  |  
7935  |  /*
7936  |  * Workqueues allocated earlier would have all CPUs sharing the default
7937  |  * worker pool. Explicitly call unbound_wq_update_pwq() on all workqueue
7938  |  * and CPU combinations to apply per-pod sharing.
7939  |  */
7940  |  list_for_each_entry(wq, &workqueues, list) {
    1Loop condition is true.  Entering loop body→
7941  |  for_each_online_cpu(cpu)
    2←Assuming 'cpu' is < 'nr_cpu_ids'→
    3←Loop condition is true.  Entering loop body→
7942  |  unbound_wq_update_pwq(wq, cpu);
    4←Calling 'unbound_wq_update_pwq'→
7943  |  if (wq->flags & WQ_UNBOUND) {
7944  |  mutex_lock(&wq->mutex);
7945  | 			wq_update_node_max_active(wq, -1);
7946  | 			mutex_unlock(&wq->mutex);
7947  | 		}
7948  | 	}
7949  |  
7950  | 	mutex_unlock(&wq_pool_mutex);
7951  | }
7952  |  
7953  | void __warn_flushing_systemwide_wq(void)
7954  | {
7955  |  pr_warn("WARNING: Flushing system-wide workqueues will be prohibited in near future.\n");
7956  | 	dump_stack();
7957  | }
7958  | EXPORT_SYMBOL(__warn_flushing_systemwide_wq);
7959  |  
7960  | static int __init workqueue_unbound_cpus_setup(char *str)
7961  | {
7962  |  if (cpulist_parse(str, &wq_cmdline_cpumask) < 0) {
7963  | 		cpumask_clear(&wq_cmdline_cpumask);
7964  |  pr_warn("workqueue.unbound_cpus: incorrect CPU range, using default\n");
7965  | 	}
7966  |  
7967  |  return 1;
7968  | }
7969  | __setup("workqueue.unbound_cpus=", workqueue_unbound_cpus_setup);

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
