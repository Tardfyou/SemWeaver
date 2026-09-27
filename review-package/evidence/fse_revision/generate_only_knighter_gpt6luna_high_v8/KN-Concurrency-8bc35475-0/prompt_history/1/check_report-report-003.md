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

File:| ./include/linux/refcount.h  
---|---  
Warning:| line 186, column 6  
Speculative read of shared state before guard; move the read inside the
guarded branch  
  
### Annotated Source Code


908   | 					WORK_OFFQ_POOL_BITS);
909   | 	offqd->disable = shift_and_mask(data, WORK_OFFQ_DISABLE_SHIFT,
910   | 					WORK_OFFQ_DISABLE_BITS);
911   | 	offqd->flags = data & WORK_OFFQ_FLAG_MASK;
912   | }
913   |  
914   | static unsigned long work_offqd_pack_flags(struct work_offq_data *offqd)
915   | {
916   |  return ((unsigned long)offqd->disable << WORK_OFFQ_DISABLE_SHIFT) |
917   | 		((unsigned long)offqd->flags);
918   | }
919   |  
920   | /*
921   |  * Policy functions.  These define the policies on how the global worker
922   |  * pools are managed.  Unless noted otherwise, these functions assume that
923   |  * they're being called with pool->lock held.
924   |  */
925   |  
926   | /*
927   |  * Need to wake up a worker?  Called from anything but currently
928   |  * running workers.
929   |  *
930   |  * Note that, because unbound workers never contribute to nr_running, this
931   |  * function will always return %true for unbound pools as long as the
932   |  * worklist isn't empty.
933   |  */
934   | static bool need_more_worker(struct worker_pool *pool)
935   | {
936   |  return !list_empty(&pool->worklist) && !pool->nr_running;
937   | }
938   |  
939   | /* Can I start working?  Called from busy but !running workers. */
940   | static bool may_start_working(struct worker_pool *pool)
941   | {
942   |  return pool->nr_idle;
943   | }
944   |  
945   | /* Do I need to keep working?  Called from currently running workers. */
946   | static bool keep_working(struct worker_pool *pool)
947   | {
948   |  return !list_empty(&pool->worklist) && (pool->nr_running <= 1);
949   | }
950   |  
951   | /* Do we need a new worker?  Called from manager. */
952   | static bool need_to_create_worker(struct worker_pool *pool)
953   | {
954   |  return need_more_worker(pool) && !may_start_working(pool);
955   | }
956   |  
957   | /* Do we have too many workers and should some go away? */
958   | static bool too_many_workers(struct worker_pool *pool)
959   | {
960   | 	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
961   |  int nr_idle = pool->nr_idle + managing; /* manager is considered idle */
962   |  int nr_busy = pool->nr_workers - nr_idle;
963   |  
964   |  return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
965   | }
966   |  
967   | /**
968   |  * worker_set_flags - set worker flags and adjust nr_running accordingly
969   |  * @worker: self
970   |  * @flags: flags to set
971   |  *
972   |  * Set @flags in @worker->flags and adjust nr_running accordingly.
973   |  */
974   | static inline void worker_set_flags(struct worker *worker, unsigned int flags)
975   | {
976   |  struct worker_pool *pool = worker->pool;
977   |  
978   |  lockdep_assert_held(&pool->lock);
979   |  
980   |  /* If transitioning into NOT_RUNNING, adjust nr_running. */
981   |  if ((flags & WORKER_NOT_RUNNING) &&
982   | 	    !(worker->flags & WORKER_NOT_RUNNING)) {
983   | 		pool->nr_running--;
984   | 	}
985   |  
986   | 	worker->flags |= flags;
987   | }
988   |  
989   | /**
990   |  * worker_clr_flags - clear worker flags and adjust nr_running accordingly
991   |  * @worker: self
992   |  * @flags: flags to clear
993   |  *
994   |  * Clear @flags in @worker->flags and adjust nr_running accordingly.
2819  | 	worker_enter_idle(worker);
2820  |  
2821  |  /*
2822  |  * @worker is waiting on a completion in kthread() and will trigger hung
2823  |  * check if not woken up soon. As kick_pool() is noop if @pool is empty,
2824  |  * wake it up explicitly.
2825  |  */
2826  |  if (worker->task)
2827  | 		wake_up_process(worker->task);
2828  |  
2829  |  raw_spin_unlock_irq(&pool->lock);
2830  |  
2831  |  return worker;
2832  |  
2833  | fail:
2834  | 	ida_free(&pool->worker_ida, id);
2835  | 	kfree(worker);
2836  |  return NULL;
2837  | }
2838  |  
2839  | static void detach_dying_workers(struct list_head *cull_list)
2840  | {
2841  |  struct worker *worker;
2842  |  
2843  |  list_for_each_entry(worker, cull_list, entry)
2844  | 		detach_worker(worker);
2845  | }
2846  |  
2847  | static void reap_dying_workers(struct list_head *cull_list)
2848  | {
2849  |  struct worker *worker, *tmp;
2850  |  
2851  |  list_for_each_entry_safe(worker, tmp, cull_list, entry) {
2852  | 		list_del_init(&worker->entry);
2853  | 		kthread_stop_put(worker->task);
2854  | 		kfree(worker);
2855  | 	}
2856  | }
2857  |  
2858  | /**
2859  |  * set_worker_dying - Tag a worker for destruction
2860  |  * @worker: worker to be destroyed
2861  |  * @list: transfer worker away from its pool->idle_list and into list
2862  |  *
2863  |  * Tag @worker for destruction and adjust @pool stats accordingly.  The worker
2864  |  * should be idle.
2865  |  *
2866  |  * CONTEXT:
2867  |  * raw_spin_lock_irq(pool->lock).
2868  |  */
2869  | static void set_worker_dying(struct worker *worker, struct list_head *list)
2870  | {
2871  |  struct worker_pool *pool = worker->pool;
2872  |  
2873  |  lockdep_assert_held(&pool->lock);
    5←Assuming 'debug_locks' is 0→
    6←Taking false branch→
2874  |  lockdep_assert_held(&wq_pool_attach_mutex);
    7←Loop condition is false.  Exiting loop→
    8←Taking false branch→
    9←Loop condition is false.  Exiting loop→
2875  |  
2876  |  /* sanity check frenzy */
2877  |  if (WARN_ON(worker->current_work) ||
    10←Assuming field 'current_work' is null→
    11←Taking false branch→
    16←Taking false branch→
2878  |  WARN_ON(!list_empty(&worker->scheduled)) ||
    12←Assuming '__ret_warn_on' is 0→
    13←Taking false branch→
2879  |  WARN_ON(!(worker->flags & WORKER_IDLE)))
    14←Assuming the condition is false→
    15←Taking false branch→
2880  |  return;
2881  |  
2882  |  pool->nr_workers--;
2883  | 	pool->nr_idle--;
2884  |  
2885  | 	worker->flags |= WORKER_DIE;
2886  |  
2887  | 	list_move(&worker->entry, list);
2888  |  
2889  |  /* get an extra task struct reference for later kthread_stop_put() */
2890  |  get_task_struct(worker->task);
    17←Calling 'get_task_struct'→
2891  | }
2892  |  
2893  | /**
2894  |  * idle_worker_timeout - check if some idle workers can now be deleted.
2895  |  * @t: The pool's idle_timer that just expired
2896  |  *
2897  |  * The timer is armed in worker_enter_idle(). Note that it isn't disarmed in
2898  |  * worker_leave_idle(), as a worker flicking between idle and active while its
2899  |  * pool is at the too_many_workers() tipping point would cause too much timer
2900  |  * housekeeping overhead. Since IDLE_WORKER_TIMEOUT is long enough, we just let
2901  |  * it expire and re-evaluate things from there.
2902  |  */
2903  | static void idle_worker_timeout(struct timer_list *t)
2904  | {
2905  |  struct worker_pool *pool = from_timer(pool, t, idle_timer);
2906  | 	bool do_cull = false;
2907  |  
2908  |  if (work_pending(&pool->idle_cull_work))
2909  |  return;
2910  |  
2911  |  raw_spin_lock_irq(&pool->lock);
2912  |  
2913  |  if (too_many_workers(pool)) {
2914  |  struct worker *worker;
2915  |  unsigned long expires;
2916  |  
2917  |  /* idle_list is kept in LIFO order, check the last one */
2918  | 		worker = list_last_entry(&pool->idle_list, struct worker, entry);
2919  | 		expires = worker->last_active + IDLE_WORKER_TIMEOUT;
2920  | 		do_cull = !time_before(jiffies, expires);
2921  |  
2922  |  if (!do_cull)
2923  | 			mod_timer(&pool->idle_timer, expires);
2924  | 	}
2925  |  raw_spin_unlock_irq(&pool->lock);
2926  |  
2927  |  if (do_cull)
2928  | 		queue_work(system_unbound_wq, &pool->idle_cull_work);
2929  | }
2930  |  
2931  | /**
2932  |  * idle_cull_fn - cull workers that have been idle for too long.
2933  |  * @work: the pool's work for handling these idle workers
2934  |  *
2935  |  * This goes through a pool's idle workers and gets rid of those that have been
2936  |  * idle for at least IDLE_WORKER_TIMEOUT seconds.
2937  |  *
2938  |  * We don't want to disturb isolated CPUs because of a pcpu kworker being
2939  |  * culled, so this also resets worker affinity. This requires a sleepable
2940  |  * context, hence the split between timer callback and work item.
2941  |  */
2942  | static void idle_cull_fn(struct work_struct *work)
2943  | {
2944  |  struct worker_pool *pool = container_of(work, struct worker_pool, idle_cull_work);
2945  |  LIST_HEAD(cull_list);
2946  |  
2947  |  /*
2948  |  * Grabbing wq_pool_attach_mutex here ensures an already-running worker
2949  |  * cannot proceed beyong set_pf_worker() in its self-destruct path.
2950  |  * This is required as a previously-preempted worker could run after
2951  |  * set_worker_dying() has happened but before detach_dying_workers() did.
2952  |  */
2953  |  mutex_lock(&wq_pool_attach_mutex);
2954  |  raw_spin_lock_irq(&pool->lock);
2955  |  
2956  |  while (too_many_workers(pool)) {
    1Loop condition is true.  Entering loop body→
2957  |  struct worker *worker;
2958  |  unsigned long expires;
2959  |  
2960  | 		worker = list_last_entry(&pool->idle_list, struct worker, entry);
2961  |  expires = worker->last_active + IDLE_WORKER_TIMEOUT;
2962  |  
2963  |  if (time_before(jiffies, expires)) {
    2←Assuming the condition is false→
    3←Taking false branch→
2964  | 			mod_timer(&pool->idle_timer, expires);
2965  |  break;
2966  | 		}
2967  |  
2968  |  set_worker_dying(worker, &cull_list);
    4←Calling 'set_worker_dying'→
2969  | 	}
2970  |  
2971  |  raw_spin_unlock_irq(&pool->lock);
2972  | 	detach_dying_workers(&cull_list);
2973  | 	mutex_unlock(&wq_pool_attach_mutex);
2974  |  
2975  | 	reap_dying_workers(&cull_list);
2976  | }
2977  |  
2978  | static void send_mayday(struct work_struct *work)
2979  | {
2980  |  struct pool_workqueue *pwq = get_work_pwq(work);
2981  |  struct workqueue_struct *wq = pwq->wq;
2982  |  
2983  |  lockdep_assert_held(&wq_mayday_lock);
2984  |  
2985  |  if (!wq->rescuer)
2986  |  return;
2987  |  
2988  |  /* mayday mayday mayday */
2989  |  if (list_empty(&pwq->mayday_node)) {
2990  |  /*
2991  |  * If @pwq is for an unbound wq, its base ref may be put at
2992  |  * any time due to an attribute change.  Pin @pwq until the
2993  |  * rescuer is done with it.
2994  |  */
2995  | 		get_pwq(pwq);
2996  | 		list_add_tail(&pwq->mayday_node, &wq->maydays);
2997  | 		wake_up_process(wq->rescuer->task);
2998  | 		pwq->stats[PWQ_STAT_MAYDAY]++;
66    | extern void sched_cgroup_fork(struct task_struct *p, struct kernel_clone_args *kargs);
67    | extern void sched_post_fork(struct task_struct *p);
68    | extern void sched_dead(struct task_struct *p);
69    |  
70    | void __noreturn do_task_dead(void);
71    | void __noreturn make_task_dead(int signr);
72    |  
73    | extern void mm_cache_init(void);
74    | extern void proc_caches_init(void);
75    |  
76    | extern void fork_init(void);
77    |  
78    | extern void release_task(struct task_struct * p);
79    |  
80    | extern int copy_thread(struct task_struct *, const struct kernel_clone_args *);
81    |  
82    | extern void flush_thread(void);
83    |  
84    | #ifdef CONFIG_HAVE_EXIT_THREAD
85    | extern void exit_thread(struct task_struct *tsk);
86    | #else
87    | static inline void exit_thread(struct task_struct *tsk)
88    | {
89    | }
90    | #endif
91    | extern __noreturn void do_group_exit(int);
92    |  
93    | extern void exit_files(struct task_struct *);
94    | extern void exit_itimers(struct task_struct *);
95    |  
96    | extern pid_t kernel_clone(struct kernel_clone_args *kargs);
97    | struct task_struct *copy_process(struct pid *pid, int trace, int node,
98    |  struct kernel_clone_args *args);
99    | struct task_struct *create_io_thread(int (*fn)(void *), void *arg, int node);
100   | struct task_struct *fork_idle(int);
101   | extern pid_t kernel_thread(int (*fn)(void *), void *arg, const char *name,
102   |  unsigned long flags);
103   | extern pid_t user_mode_thread(int (*fn)(void *), void *arg, unsigned long flags);
104   | extern long kernel_wait4(pid_t, int __user *, int, struct rusage *);
105   | int kernel_wait(pid_t pid, int *stat);
106   |  
107   | extern void free_task(struct task_struct *tsk);
108   |  
109   | /* sched_exec is called by processes performing an exec */
110   | #ifdef CONFIG_SMP
111   | extern void sched_exec(void);
112   | #else
113   | #define sched_exec()   {}
114   | #endif
115   |  
116   | static inline struct task_struct *get_task_struct(struct task_struct *t)
117   | {
118   |  refcount_inc(&t->usage);
    18←Calling 'refcount_inc'→
119   |  return t;
120   | }
121   |  
122   | extern void __put_task_struct(struct task_struct *t);
123   | extern void __put_task_struct_rcu_cb(struct rcu_head *rhp);
124   |  
125   | static inline void put_task_struct(struct task_struct *t)
126   | {
127   |  if (!refcount_dec_and_test(&t->usage))
128   |  return;
129   |  
130   |  /*
131   |  * In !RT, it is always safe to call __put_task_struct().
132   |  * Under RT, we can only call it in preemptible context.
133   |  */
134   |  if (!IS_ENABLED(CONFIG_PREEMPT_RT) || preemptible()) {
135   |  static DEFINE_WAIT_OVERRIDE_MAP(put_task_map, LD_WAIT_SLEEP);
136   |  
137   |  lock_map_acquire_try(&put_task_map);
138   | 		__put_task_struct(t);
139   |  lock_map_release(&put_task_map);
140   |  return;
141   | 	}
142   |  
143   |  /*
144   |  * under PREEMPT_RT, we can't call put_task_struct
145   |  * in atomic context because it will indirectly
146   |  * acquire sleeping locks.
147   |  *
148   |  * call_rcu() will schedule delayed_put_task_struct_rcu()
131   |  *
132   |  * Return: the refcount's value
133   |  */
134   | static inline unsigned int refcount_read(const refcount_t *r)
135   | {
136   |  return atomic_read(&r->refs);
137   | }
138   |  
139   | static inline __must_check __signed_wrap
140   | bool __refcount_add_not_zero(int i, refcount_t *r, int *oldp)
141   | {
142   |  int old = refcount_read(r);
143   |  
144   |  do {
145   |  if (!old)
146   |  break;
147   | 	} while (!atomic_try_cmpxchg_relaxed(&r->refs, &old, old + i));
148   |  
149   |  if (oldp)
150   | 		*oldp = old;
151   |  
152   |  if (unlikely(old < 0 || old + i < 0))
153   | 		refcount_warn_saturate(r, REFCOUNT_ADD_NOT_ZERO_OVF);
154   |  
155   |  return old;
156   | }
157   |  
158   | /**
159   |  * refcount_add_not_zero - add a value to a refcount unless it is 0
160   |  * @i: the value to add to the refcount
161   |  * @r: the refcount
162   |  *
163   |  * Will saturate at REFCOUNT_SATURATED and WARN.
164   |  *
165   |  * Provides no memory ordering, it is assumed the caller has guaranteed the
166   |  * object memory to be stable (RCU, etc.). It does provide a control dependency
167   |  * and thereby orders future stores. See the comment on top.
168   |  *
169   |  * Use of this function is not recommended for the normal reference counting
170   |  * use case in which references are taken and released one at a time.  In these
171   |  * cases, refcount_inc(), or one of its variants, should instead be used to
172   |  * increment a reference count.
173   |  *
174   |  * Return: false if the passed refcount is 0, true otherwise
175   |  */
176   | static inline __must_check bool refcount_add_not_zero(int i, refcount_t *r)
177   | {
178   |  return __refcount_add_not_zero(i, r, NULL);
179   | }
180   |  
181   | static inline __signed_wrap
182   | void __refcount_add(int i, refcount_t *r, int *oldp)
183   | {
184   |  int old = atomic_fetch_add_relaxed(i, &r->refs);
185   |  
186   |  if (oldp)
    21←Speculative read of shared state before guard; move the read inside the guarded branch
187   | 		*oldp = old;
188   |  
189   |  if (unlikely(!old))
190   | 		refcount_warn_saturate(r, REFCOUNT_ADD_UAF);
191   |  else if (unlikely(old < 0 || old + i < 0))
192   | 		refcount_warn_saturate(r, REFCOUNT_ADD_OVF);
193   | }
194   |  
195   | /**
196   |  * refcount_add - add a value to a refcount
197   |  * @i: the value to add to the refcount
198   |  * @r: the refcount
199   |  *
200   |  * Similar to atomic_add(), but will saturate at REFCOUNT_SATURATED and WARN.
201   |  *
202   |  * Provides no memory ordering, it is assumed the caller has guaranteed the
203   |  * object memory to be stable (RCU, etc.). It does provide a control dependency
204   |  * and thereby orders future stores. See the comment on top.
205   |  *
206   |  * Use of this function is not recommended for the normal reference counting
207   |  * use case in which references are taken and released one at a time.  In these
208   |  * cases, refcount_inc(), or one of its variants, should instead be used to
209   |  * increment a reference count.
210   |  */
211   | static inline void refcount_add(int i, refcount_t *r)
212   | {
213   | 	__refcount_add(i, r, NULL);
214   | }
215   |  
216   | static inline __must_check bool __refcount_inc_not_zero(refcount_t *r, int *oldp)
217   | {
218   |  return __refcount_add_not_zero(1, r, oldp);
219   | }
220   |  
221   | /**
222   |  * refcount_inc_not_zero - increment a refcount unless it is 0
223   |  * @r: the refcount to increment
224   |  *
225   |  * Similar to atomic_inc_not_zero(), but will saturate at REFCOUNT_SATURATED
226   |  * and WARN.
227   |  *
228   |  * Provides no memory ordering, it is assumed the caller has guaranteed the
229   |  * object memory to be stable (RCU, etc.). It does provide a control dependency
230   |  * and thereby orders future stores. See the comment on top.
231   |  *
232   |  * Return: true if the increment was successful, false otherwise
233   |  */
234   | static inline __must_check bool refcount_inc_not_zero(refcount_t *r)
235   | {
236   |  return __refcount_inc_not_zero(r, NULL);
237   | }
238   |  
239   | static inline void __refcount_inc(refcount_t *r, int *oldp)
240   | {
241   |  __refcount_add(1, r, oldp);
    20←Calling '__refcount_add'→
242   | }
243   |  
244   | /**
245   |  * refcount_inc - increment a refcount
246   |  * @r: the refcount to increment
247   |  *
248   |  * Similar to atomic_inc(), but will saturate at REFCOUNT_SATURATED and WARN.
249   |  *
250   |  * Provides no memory ordering, it is assumed the caller already has a
251   |  * reference on the object.
252   |  *
253   |  * Will WARN if the refcount is 0, as this represents a possible use-after-free
254   |  * condition.
255   |  */
256   | static inline void refcount_inc(refcount_t *r)
257   | {
258   |  __refcount_inc(r, NULL);
    19←Calling '__refcount_inc'→
259   | }
260   |  
261   | static inline __must_check __signed_wrap
262   | bool __refcount_sub_and_test(int i, refcount_t *r, int *oldp)
263   | {
264   |  int old = atomic_fetch_sub_release(i, &r->refs);
265   |  
266   |  if (oldp)
267   | 		*oldp = old;
268   |  
269   |  if (old == i) {
270   |  smp_acquire__after_ctrl_dep();
271   |  return true;
272   | 	}
273   |  
274   |  if (unlikely(old < 0 || old - i < 0))
275   | 		refcount_warn_saturate(r, REFCOUNT_SUB_UAF);
276   |  
277   |  return false;
278   | }
279   |  
280   | /**
281   |  * refcount_sub_and_test - subtract from a refcount and test if it is 0
282   |  * @i: amount to subtract from the refcount
283   |  * @r: the refcount
284   |  *
285   |  * Similar to atomic_dec_and_test(), but it will WARN, return false and
286   |  * ultimately leak on underflow and will fail to decrement when saturated
287   |  * at REFCOUNT_SATURATED.
288   |  *

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
