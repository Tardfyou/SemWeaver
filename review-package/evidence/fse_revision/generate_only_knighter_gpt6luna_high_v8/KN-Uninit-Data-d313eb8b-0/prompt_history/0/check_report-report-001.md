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

net/sched: act_skbmod: prevent kernel-infoleak

syzbot found that tcf_skbmod_dump() was copying four bytes
from kernel stack to user space [1].

The issue here is that 'struct tc_skbmod' has a four bytes hole.

We need to clear the structure before filling fields.

[1]
BUG: KMSAN: kernel-infoleak in instrument_copy_to_user include/linux/instrumented.h:114 [inline]
 BUG: KMSAN: kernel-infoleak in copy_to_user_iter lib/iov_iter.c:24 [inline]
 BUG: KMSAN: kernel-infoleak in iterate_ubuf include/linux/iov_iter.h:29 [inline]
 BUG: KMSAN: kernel-infoleak in iterate_and_advance2 include/linux/iov_iter.h:245 [inline]
 BUG: KMSAN: kernel-infoleak in iterate_and_advance include/linux/iov_iter.h:271 [inline]
 BUG: KMSAN: kernel-infoleak in _copy_to_iter+0x366/0x2520 lib/iov_iter.c:185
  instrument_copy_to_user include/linux/instrumented.h:114 [inline]
  copy_to_user_iter lib/iov_iter.c:24 [inline]
  iterate_ubuf include/linux/iov_iter.h:29 [inline]
  iterate_and_advance2 include/linux/iov_iter.h:245 [inline]
  iterate_and_advance include/linux/iov_iter.h:271 [inline]
  _copy_to_iter+0x366/0x2520 lib/iov_iter.c:185
  copy_to_iter include/linux/uio.h:196 [inline]
  simple_copy_to_iter net/core/datagram.c:532 [inline]
  __skb_datagram_iter+0x185/0x1000 net/core/datagram.c:420
  skb_copy_datagram_iter+0x5c/0x200 net/core/datagram.c:546
  skb_copy_datagram_msg include/linux/skbuff.h:4050 [inline]
  netlink_recvmsg+0x432/0x1610 net/netlink/af_netlink.c:1962
  sock_recvmsg_nosec net/socket.c:1046 [inline]
  sock_recvmsg+0x2c4/0x340 net/socket.c:1068
  __sys_recvfrom+0x35a/0x5f0 net/socket.c:2242
  __do_sys_recvfrom net/socket.c:2260 [inline]
  __se_sys_recvfrom net/socket.c:2256 [inline]
  __x64_sys_recvfrom+0x126/0x1d0 net/socket.c:2256
 do_syscall_64+0xd5/0x1f0
 entry_SYSCALL_64_after_hwframe+0x6d/0x75

Uninit was stored to memory at:
  pskb_expand_head+0x30f/0x19d0 net/core/skbuff.c:2253
  netlink_trim+0x2c2/0x330 net/netlink/af_netlink.c:1317
  netlink_unicast+0x9f/0x1260 net/netlink/af_netlink.c:1351
  nlmsg_unicast include/net/netlink.h:1144 [inline]
  nlmsg_notify+0x21d/0x2f0 net/netlink/af_netlink.c:2610
  rtnetlink_send+0x73/0x90 net/core/rtnetlink.c:741
  rtnetlink_maybe_send include/linux/rtnetlink.h:17 [inline]
  tcf_add_notify net/sched/act_api.c:2048 [inline]
  tcf_action_add net/sched/act_api.c:2071 [inline]
  tc_ctl_action+0x146e/0x19d0 net/sched/act_api.c:2119
  rtnetlink_rcv_msg+0x1737/0x1900 net/core/rtnetlink.c:6595
  netlink_rcv_skb+0x375/0x650 net/netlink/af_netlink.c:2559
  rtnetlink_rcv+0x34/0x40 net/core/rtnetlink.c:6613
  netlink_unicast_kernel net/netlink/af_netlink.c:1335 [inline]
  netlink_unicast+0xf4c/0x1260 net/netlink/af_netlink.c:1361
  netlink_sendmsg+0x10df/0x11f0 net/netlink/af_netlink.c:1905
  sock_sendmsg_nosec net/socket.c:730 [inline]
  __sock_sendmsg+0x30f/0x380 net/socket.c:745
  ____sys_sendmsg+0x877/0xb60 net/socket.c:2584
  ___sys_sendmsg+0x28d/0x3c0 net/socket.c:2638
  __sys_sendmsg net/socket.c:2667 [inline]
  __do_sys_sendmsg net/socket.c:2676 [inline]
  __se_sys_sendmsg net/socket.c:2674 [inline]
  __x64_sys_sendmsg+0x307/0x4a0 net/socket.c:2674
 do_syscall_64+0xd5/0x1f0
 entry_SYSCALL_64_after_hwframe+0x6d/0x75

Uninit was stored to memory at:
  __nla_put lib/nlattr.c:1041 [inline]
  nla_put+0x1c6/0x230 lib/nlattr.c:1099
  tcf_skbmod_dump+0x23f/0xc20 net/sched/act_skbmod.c:256
  tcf_action_dump_old net/sched/act_api.c:1191 [inline]
  tcf_action_dump_1+0x85e/0x970 net/sched/act_api.c:1227
  tcf_action_dump+0x1fd/0x460 net/sched/act_api.c:1251
  tca_get_fill+0x519/0x7a0 net/sched/act_api.c:1628
  tcf_add_notify_msg net/sched/act_api.c:2023 [inline]
  tcf_add_notify net/sched/act_api.c:2042 [inline]
  tcf_action_add net/sched/act_api.c:2071 [inline]
  tc_ctl_action+0x1365/0x19d0 net/sched/act_api.c:2119
  rtnetlink_rcv_msg+0x1737/0x1900 net/core/rtnetlink.c:6595
  netlink_rcv_skb+0x375/0x650 net/netlink/af_netlink.c:2559
  rtnetlink_rcv+0x34/0x40 net/core/rtnetlink.c:6613
  netlink_unicast_kernel net/netlink/af_netlink.c:1335 [inline]
  netlink_unicast+0xf4c/0x1260 net/netlink/af_netlink.c:1361
  netlink_sendmsg+0x10df/0x11f0 net/netlink/af_netlink.c:1905
  sock_sendmsg_nosec net/socket.c:730 [inline]
  __sock_sendmsg+0x30f/0x380 net/socket.c:745
  ____sys_sendmsg+0x877/0xb60 net/socket.c:2584
  ___sys_sendmsg+0x28d/0x3c0 net/socket.c:2638
  __sys_sendmsg net/socket.c:2667 [inline]
  __do_sys_sendmsg net/socket.c:2676 [inline]
  __se_sys_sendmsg net/socket.c:2674 [inline]
  __x64_sys_sendmsg+0x307/0x4a0 net/socket.c:2674
 do_syscall_64+0xd5/0x1f0
 entry_SYSCALL_64_after_hwframe+0x6d/0x75

Local variable opt created at:
  tcf_skbmod_dump+0x9d/0xc20 net/sched/act_skbmod.c:244
  tcf_action_dump_old net/sched/act_api.c:1191 [inline]
  tcf_action_dump_1+0x85e/0x970 net/sched/act_api.c:1227

Bytes 188-191 of 248 are uninitialized
Memory access of size 248 starts at ffff888117697680
Data copied to user address 00007ffe56d855f0

Fixes: 86da71b57383 ("net_sched: Introduce skbmod action")
Signed-off-by: Eric Dumazet <edumazet@google.com>
Acked-by: Jamal Hadi Salim <jhs@mojatatu.com>
Link: https://lore.kernel.org/r/20240403130908.93421-1-edumazet@google.com
Signed-off-by: Jakub Kicinski <kuba@kernel.org>

## Buggy Code

```c
// Function: tcf_skbmod_dump in net/sched/act_skbmod.c
static int tcf_skbmod_dump(struct sk_buff *skb, struct tc_action *a,
			   int bind, int ref)
{
	struct tcf_skbmod *d = to_skbmod(a);
	unsigned char *b = skb_tail_pointer(skb);
	struct tcf_skbmod_params  *p;
	struct tc_skbmod opt = {
		.index   = d->tcf_index,
		.refcnt  = refcount_read(&d->tcf_refcnt) - ref,
		.bindcnt = atomic_read(&d->tcf_bindcnt) - bind,
	};
	struct tcf_t t;

	spin_lock_bh(&d->tcf_lock);
	opt.action = d->tcf_action;
	p = rcu_dereference_protected(d->skbmod_p,
				      lockdep_is_held(&d->tcf_lock));
	opt.flags  = p->flags;
	if (nla_put(skb, TCA_SKBMOD_PARMS, sizeof(opt), &opt))
		goto nla_put_failure;
	if ((p->flags & SKBMOD_F_DMAC) &&
	    nla_put(skb, TCA_SKBMOD_DMAC, ETH_ALEN, p->eth_dst))
		goto nla_put_failure;
	if ((p->flags & SKBMOD_F_SMAC) &&
	    nla_put(skb, TCA_SKBMOD_SMAC, ETH_ALEN, p->eth_src))
		goto nla_put_failure;
	if ((p->flags & SKBMOD_F_ETYPE) &&
	    nla_put_u16(skb, TCA_SKBMOD_ETYPE, ntohs(p->eth_type)))
		goto nla_put_failure;

	tcf_tm_dump(&t, &d->tcf_tm);
	if (nla_put_64bit(skb, TCA_SKBMOD_TM, sizeof(t), &t, TCA_SKBMOD_PAD))
		goto nla_put_failure;

	spin_unlock_bh(&d->tcf_lock);
	return skb->len;
nla_put_failure:
	spin_unlock_bh(&d->tcf_lock);
	nlmsg_trim(skb, b);
	return -1;
}
```

## Bug Fix Patch

```diff
diff --git a/net/sched/act_skbmod.c b/net/sched/act_skbmod.c
index 39945b139c48..cd0accaf844a 100644
--- a/net/sched/act_skbmod.c
+++ b/net/sched/act_skbmod.c
@@ -241,13 +241,13 @@ static int tcf_skbmod_dump(struct sk_buff *skb, struct tc_action *a,
 	struct tcf_skbmod *d = to_skbmod(a);
 	unsigned char *b = skb_tail_pointer(skb);
 	struct tcf_skbmod_params  *p;
-	struct tc_skbmod opt = {
-		.index   = d->tcf_index,
-		.refcnt  = refcount_read(&d->tcf_refcnt) - ref,
-		.bindcnt = atomic_read(&d->tcf_bindcnt) - bind,
-	};
+	struct tc_skbmod opt;
 	struct tcf_t t;

+	memset(&opt, 0, sizeof(opt));
+	opt.index   = d->tcf_index;
+	opt.refcnt  = refcount_read(&d->tcf_refcnt) - ref,
+	opt.bindcnt = atomic_read(&d->tcf_bindcnt) - bind;
 	spin_lock_bh(&d->tcf_lock);
 	opt.action = d->tcf_action;
 	p = rcu_dereference_protected(d->skbmod_p,
```


## Bug Pattern

Partially initialized stack struct (via designated initializers or field-by-field assignments) is copied to user space using sizeof(struct), leaking uninitialized padding bytes.

Example pattern:
struct S { u32 index; u32 refcnt; u32 bindcnt; /* padding/hole exists */ };
struct S s = { .index = idx, .refcnt = r, .bindcnt = b };  // only members set; padding uninitialized
nla_put(skb, ATTR, sizeof(s), &s);  // copies padding to user (info leak)

Root cause: struct contains padding and the code does not fully zero the object before exporting it; copying sizeof(struct) includes those uninitialized padding bytes.


# Report

BuildSource:| net/sched/act_skbmod.c
### Report Summary

File:| act_skbmod.c  
---|---  
Warning:| line 256, column 6  
stack struct not fully zeroed before user copy (padding leak)  
  
### Annotated Source Code


188   |  
189   | 	p = kzalloc(sizeof(struct tcf_skbmod_params), GFP_KERNEL);
190   |  if (unlikely(!p)) {
191   | 		err = -ENOMEM;
192   |  goto put_chain;
193   | 	}
194   |  
195   | 	p->flags = lflags;
196   |  
197   |  if (ovr)
198   | 		spin_lock_bh(&d->tcf_lock);
199   |  /* Protected by tcf_lock if overwriting existing action. */
200   | 	goto_ch = tcf_action_set_ctrlact(*a, parm->action, goto_ch);
201   | 	p_old = rcu_dereference_protected(d->skbmod_p, 1);
202   |  
203   |  if (lflags & SKBMOD_F_DMAC)
204   | 		ether_addr_copy(p->eth_dst, daddr);
205   |  if (lflags & SKBMOD_F_SMAC)
206   | 		ether_addr_copy(p->eth_src, saddr);
207   |  if (lflags & SKBMOD_F_ETYPE)
208   | 		p->eth_type = htons(eth_type);
209   |  
210   |  rcu_assign_pointer(d->skbmod_p, p);
211   |  if (ovr)
212   | 		spin_unlock_bh(&d->tcf_lock);
213   |  
214   |  if (p_old)
215   |  kfree_rcu(p_old, rcu);
216   |  if (goto_ch)
217   | 		tcf_chain_put_by_act(goto_ch);
218   |  
219   |  return ret;
220   | put_chain:
221   |  if (goto_ch)
222   | 		tcf_chain_put_by_act(goto_ch);
223   | release_idr:
224   | 	tcf_idr_release(*a, bind);
225   |  return err;
226   | }
227   |  
228   | static void tcf_skbmod_cleanup(struct tc_action *a)
229   | {
230   |  struct tcf_skbmod *d = to_skbmod(a);
231   |  struct tcf_skbmod_params  *p;
232   |  
233   | 	p = rcu_dereference_protected(d->skbmod_p, 1);
234   |  if (p)
235   |  kfree_rcu(p, rcu);
236   | }
237   |  
238   | static int tcf_skbmod_dump(struct sk_buff *skb, struct tc_action *a,
239   |  int bind, int ref)
240   | {
241   |  struct tcf_skbmod *d = to_skbmod(a);
242   |  unsigned char *b = skb_tail_pointer(skb);
243   |  struct tcf_skbmod_params  *p;
244   |  struct tc_skbmod opt;
245   |  struct tcf_t t;
246   |  
247   |  memset(&opt, 0, sizeof(opt));
248   | 	opt.index   = d->tcf_index;
249   | 	opt.refcnt  = refcount_read(&d->tcf_refcnt) - ref,
250   | 	opt.bindcnt = atomic_read(&d->tcf_bindcnt) - bind;
251   | 	spin_lock_bh(&d->tcf_lock);
252   |  opt.action = d->tcf_action;
253   | 	p = rcu_dereference_protected(d->skbmod_p,
    1Assuming the condition is false→
    2←Loop condition is false.  Exiting loop→
254   |  lockdep_is_held(&d->tcf_lock));
255   |  opt.flags  = p->flags;
256   |  if (nla_put(skb, TCA_SKBMOD_PARMS, sizeof(opt), &opt))
    3←stack struct not fully zeroed before user copy (padding leak)
257   |  goto nla_put_failure;
258   |  if ((p->flags & SKBMOD_F_DMAC) &&
259   | 	    nla_put(skb, TCA_SKBMOD_DMAC, ETH_ALEN, p->eth_dst))
260   |  goto nla_put_failure;
261   |  if ((p->flags & SKBMOD_F_SMAC) &&
262   | 	    nla_put(skb, TCA_SKBMOD_SMAC, ETH_ALEN, p->eth_src))
263   |  goto nla_put_failure;
264   |  if ((p->flags & SKBMOD_F_ETYPE) &&
265   | 	    nla_put_u16(skb, TCA_SKBMOD_ETYPE, ntohs(p->eth_type)))
266   |  goto nla_put_failure;
267   |  
268   | 	tcf_tm_dump(&t, &d->tcf_tm);
269   |  if (nla_put_64bit(skb, TCA_SKBMOD_TM, sizeof(t), &t, TCA_SKBMOD_PAD))
270   |  goto nla_put_failure;
271   |  
272   | 	spin_unlock_bh(&d->tcf_lock);
273   |  return skb->len;
274   | nla_put_failure:
275   | 	spin_unlock_bh(&d->tcf_lock);
276   | 	nlmsg_trim(skb, b);
277   |  return -1;
278   | }
279   |  
280   | static struct tc_action_ops act_skbmod_ops = {
281   | 	.kind		=	"skbmod",
282   | 	.id		=	TCA_ACT_SKBMOD,
283   | 	.owner		=	THIS_MODULE,
284   | 	.act		=	tcf_skbmod_act,
285   | 	.dump		=	tcf_skbmod_dump,
286   | 	.init		=	tcf_skbmod_init,

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
