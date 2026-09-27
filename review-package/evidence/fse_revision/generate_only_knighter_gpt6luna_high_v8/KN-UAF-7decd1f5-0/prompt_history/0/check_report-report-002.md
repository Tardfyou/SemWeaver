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

mptcp: pm: fix UaF read in mptcp_pm_nl_rm_addr_or_subflow

Syzkaller reported this splat:

  ==================================================================
  BUG: KASAN: slab-use-after-free in mptcp_pm_nl_rm_addr_or_subflow+0xb44/0xcc0 net/mptcp/pm_netlink.c:881
  Read of size 4 at addr ffff8880569ac858 by task syz.1.2799/14662

  CPU: 0 UID: 0 PID: 14662 Comm: syz.1.2799 Not tainted 6.12.0-rc2-syzkaller-00307-g36c254515dc6 #0
  Hardware name: QEMU Standard PC (Q35 + ICH9, 2009), BIOS 1.16.3-debian-1.16.3-2~bpo12+1 04/01/2014
  Call Trace:
   <TASK>
   __dump_stack lib/dump_stack.c:94 [inline]
   dump_stack_lvl+0x116/0x1f0 lib/dump_stack.c:120
   print_address_description mm/kasan/report.c:377 [inline]
   print_report+0xc3/0x620 mm/kasan/report.c:488
   kasan_report+0xd9/0x110 mm/kasan/report.c:601
   mptcp_pm_nl_rm_addr_or_subflow+0xb44/0xcc0 net/mptcp/pm_netlink.c:881
   mptcp_pm_nl_rm_subflow_received net/mptcp/pm_netlink.c:914 [inline]
   mptcp_nl_remove_id_zero_address+0x305/0x4a0 net/mptcp/pm_netlink.c:1572
   mptcp_pm_nl_del_addr_doit+0x5c9/0x770 net/mptcp/pm_netlink.c:1603
   genl_family_rcv_msg_doit+0x202/0x2f0 net/netlink/genetlink.c:1115
   genl_family_rcv_msg net/netlink/genetlink.c:1195 [inline]
   genl_rcv_msg+0x565/0x800 net/netlink/genetlink.c:1210
   netlink_rcv_skb+0x165/0x410 net/netlink/af_netlink.c:2551
   genl_rcv+0x28/0x40 net/netlink/genetlink.c:1219
   netlink_unicast_kernel net/netlink/af_netlink.c:1331 [inline]
   netlink_unicast+0x53c/0x7f0 net/netlink/af_netlink.c:1357
   netlink_sendmsg+0x8b8/0xd70 net/netlink/af_netlink.c:1901
   sock_sendmsg_nosec net/socket.c:729 [inline]
   __sock_sendmsg net/socket.c:744 [inline]
   ____sys_sendmsg+0x9ae/0xb40 net/socket.c:2607
   ___sys_sendmsg+0x135/0x1e0 net/socket.c:2661
   __sys_sendmsg+0x117/0x1f0 net/socket.c:2690
   do_syscall_32_irqs_on arch/x86/entry/common.c:165 [inline]
   __do_fast_syscall_32+0x73/0x120 arch/x86/entry/common.c:386
   do_fast_syscall_32+0x32/0x80 arch/x86/entry/common.c:411
   entry_SYSENTER_compat_after_hwframe+0x84/0x8e
  RIP: 0023:0xf7fe4579
  Code: b8 01 10 06 03 74 b4 01 10 07 03 74 b0 01 10 08 03 74 d8 01 00 00 00 00 00 00 00 00 00 00 00 00 00 51 52 55 89 e5 0f 34 cd 80 <5d> 5a 59 c3 90 90 90 90 8d b4 26 00 00 00 00 8d b4 26 00 00 00 00
  RSP: 002b:00000000f574556c EFLAGS: 00000296 ORIG_RAX: 0000000000000172
  RAX: ffffffffffffffda RBX: 000000000000000b RCX: 0000000020000140
  RDX: 0000000000000000 RSI: 0000000000000000 RDI: 0000000000000000
  RBP: 0000000000000000 R08: 0000000000000000 R09: 0000000000000000
  R10: 0000000000000000 R11: 0000000000000296 R12: 0000000000000000
  R13: 0000000000000000 R14: 0000000000000000 R15: 0000000000000000
   </TASK>

  Allocated by task 5387:
   kasan_save_stack+0x33/0x60 mm/kasan/common.c:47
   kasan_save_track+0x14/0x30 mm/kasan/common.c:68
   poison_kmalloc_redzone mm/kasan/common.c:377 [inline]
   __kasan_kmalloc+0xaa/0xb0 mm/kasan/common.c:394
   kmalloc_noprof include/linux/slab.h:878 [inline]
   kzalloc_noprof include/linux/slab.h:1014 [inline]
   subflow_create_ctx+0x87/0x2a0 net/mptcp/subflow.c:1803
   subflow_ulp_init+0xc3/0x4d0 net/mptcp/subflow.c:1956
   __tcp_set_ulp net/ipv4/tcp_ulp.c:146 [inline]
   tcp_set_ulp+0x326/0x7f0 net/ipv4/tcp_ulp.c:167
   mptcp_subflow_create_socket+0x4ae/0x10a0 net/mptcp/subflow.c:1764
   __mptcp_subflow_connect+0x3cc/0x1490 net/mptcp/subflow.c:1592
   mptcp_pm_create_subflow_or_signal_addr+0xbda/0x23a0 net/mptcp/pm_netlink.c:642
   mptcp_pm_nl_fully_established net/mptcp/pm_netlink.c:650 [inline]
   mptcp_pm_nl_work+0x3a1/0x4f0 net/mptcp/pm_netlink.c:943
   mptcp_worker+0x15a/0x1240 net/mptcp/protocol.c:2777
   process_one_work+0x958/0x1b30 kernel/workqueue.c:3229
   process_scheduled_works kernel/workqueue.c:3310 [inline]
   worker_thread+0x6c8/0xf00 kernel/workqueue.c:3391
   kthread+0x2c1/0x3a0 kernel/kthread.c:389
   ret_from_fork+0x45/0x80 arch/x86/kernel/process.c:147
   ret_from_fork_asm+0x1a/0x30 arch/x86/entry/entry_64.S:244

  Freed by task 113:
   kasan_save_stack+0x33/0x60 mm/kasan/common.c:47
   kasan_save_track+0x14/0x30 mm/kasan/common.c:68
   kasan_save_free_info+0x3b/0x60 mm/kasan/generic.c:579
   poison_slab_object mm/kasan/common.c:247 [inline]
   __kasan_slab_free+0x51/0x70 mm/kasan/common.c:264
   kasan_slab_free include/linux/kasan.h:230 [inline]
   slab_free_hook mm/slub.c:2342 [inline]
   slab_free mm/slub.c:4579 [inline]
   kfree+0x14f/0x4b0 mm/slub.c:4727
   kvfree+0x47/0x50 mm/util.c:701
   kvfree_rcu_list+0xf5/0x2c0 kernel/rcu/tree.c:3423
   kvfree_rcu_drain_ready kernel/rcu/tree.c:3563 [inline]
   kfree_rcu_monitor+0x503/0x8b0 kernel/rcu/tree.c:3632
   kfree_rcu_shrink_scan+0x245/0x3a0 kernel/rcu/tree.c:3966
   do_shrink_slab+0x44f/0x11c0 mm/shrinker.c:435
   shrink_slab+0x32b/0x12a0 mm/shrinker.c:662
   shrink_one+0x47e/0x7b0 mm/vmscan.c:4818
   shrink_many mm/vmscan.c:4879 [inline]
   lru_gen_shrink_node mm/vmscan.c:4957 [inline]
   shrink_node+0x2452/0x39d0 mm/vmscan.c:5937
   kswapd_shrink_node mm/vmscan.c:6765 [inline]
   balance_pgdat+0xc19/0x18f0 mm/vmscan.c:6957
   kswapd+0x5ea/0xbf0 mm/vmscan.c:7226
   kthread+0x2c1/0x3a0 kernel/kthread.c:389
   ret_from_fork+0x45/0x80 arch/x86/kernel/process.c:147
   ret_from_fork_asm+0x1a/0x30 arch/x86/entry/entry_64.S:244

  Last potentially related work creation:
   kasan_save_stack+0x33/0x60 mm/kasan/common.c:47
   __kasan_record_aux_stack+0xba/0xd0 mm/kasan/generic.c:541
   kvfree_call_rcu+0x74/0xbe0 kernel/rcu/tree.c:3810
   subflow_ulp_release+0x2ae/0x350 net/mptcp/subflow.c:2009
   tcp_cleanup_ulp+0x7c/0x130 net/ipv4/tcp_ulp.c:124
   tcp_v4_destroy_sock+0x1c5/0x6a0 net/ipv4/tcp_ipv4.c:2541
   inet_csk_destroy_sock+0x1a3/0x440 net/ipv4/inet_connection_sock.c:1293
   tcp_done+0x252/0x350 net/ipv4/tcp.c:4870
   tcp_rcv_state_process+0x379b/0x4f30 net/ipv4/tcp_input.c:6933
   tcp_v4_do_rcv+0x1ad/0xa90 net/ipv4/tcp_ipv4.c:1938
   sk_backlog_rcv include/net/sock.h:1115 [inline]
   __release_sock+0x31b/0x400 net/core/sock.c:3072
   __tcp_close+0x4f3/0xff0 net/ipv4/tcp.c:3142
   __mptcp_close_ssk+0x331/0x14d0 net/mptcp/protocol.c:2489
   mptcp_close_ssk net/mptcp/protocol.c:2543 [inline]
   mptcp_close_ssk+0x150/0x220 net/mptcp/protocol.c:2526
   mptcp_pm_nl_rm_addr_or_subflow+0x2be/0xcc0 net/mptcp/pm_netlink.c:878
   mptcp_pm_nl_rm_subflow_received net/mptcp/pm_netlink.c:914 [inline]
   mptcp_nl_remove_id_zero_address+0x305/0x4a0 net/mptcp/pm_netlink.c:1572
   mptcp_pm_nl_del_addr_doit+0x5c9/0x770 net/mptcp/pm_netlink.c:1603
   genl_family_rcv_msg_doit+0x202/0x2f0 net/netlink/genetlink.c:1115
   genl_family_rcv_msg net/netlink/genetlink.c:1195 [inline]
   genl_rcv_msg+0x565/0x800 net/netlink/genetlink.c:1210
   netlink_rcv_skb+0x165/0x410 net/netlink/af_netlink.c:2551
   genl_rcv+0x28/0x40 net/netlink/genetlink.c:1219
   netlink_unicast_kernel net/netlink/af_netlink.c:1331 [inline]
   netlink_unicast+0x53c/0x7f0 net/netlink/af_netlink.c:1357
   netlink_sendmsg+0x8b8/0xd70 net/netlink/af_netlink.c:1901
   sock_sendmsg_nosec net/socket.c:729 [inline]
   __sock_sendmsg net/socket.c:744 [inline]
   ____sys_sendmsg+0x9ae/0xb40 net/socket.c:2607
   ___sys_sendmsg+0x135/0x1e0 net/socket.c:2661
   __sys_sendmsg+0x117/0x1f0 net/socket.c:2690
   do_syscall_32_irqs_on arch/x86/entry/common.c:165 [inline]
   __do_fast_syscall_32+0x73/0x120 arch/x86/entry/common.c:386
   do_fast_syscall_32+0x32/0x80 arch/x86/entry/common.c:411
   entry_SYSENTER_compat_after_hwframe+0x84/0x8e

  The buggy address belongs to the object at ffff8880569ac800
   which belongs to the cache kmalloc-512 of size 512
  The buggy address is located 88 bytes inside of
   freed 512-byte region [ffff8880569ac800, ffff8880569aca00)

  The buggy address belongs to the physical page:
  page: refcount:1 mapcount:0 mapping:0000000000000000 index:0x0 pfn:0x569ac
  head: order:2 mapcount:0 entire_mapcount:0 nr_pages_mapped:0 pincount:0
  flags: 0x4fff00000000040(head|node=1|zone=1|lastcpupid=0x7ff)
  page_type: f5(slab)
  raw: 04fff00000000040 ffff88801ac42c80 dead000000000100 dead000000000122
  raw: 0000000000000000 0000000080100010 00000001f5000000 0000000000000000
  head: 04fff00000000040 ffff88801ac42c80 dead000000000100 dead000000000122
  head: 0000000000000000 0000000080100010 00000001f5000000 0000000000000000
  head: 04fff00000000002 ffffea00015a6b01 ffffffffffffffff 0000000000000000
  head: 0000000000000004 0000000000000000 00000000ffffffff 0000000000000000
  page dumped because: kasan: bad access detected
  page_owner tracks the page as allocated
  page last allocated via order 2, migratetype Unmovable, gfp_mask 0xd20c0(__GFP_IO|__GFP_FS|__GFP_NOWARN|__GFP_NORETRY|__GFP_COMP|__GFP_NOMEMALLOC), pid 10238, tgid 10238 (kworker/u32:6), ts 597403252405, free_ts 597177952947
   set_page_owner include/linux/page_owner.h:32 [inline]
   post_alloc_hook+0x2d1/0x350 mm/page_alloc.c:1537
   prep_new_page mm/page_alloc.c:1545 [inline]
   get_page_from_freelist+0x101e/0x3070 mm/page_alloc.c:3457
   __alloc_pages_noprof+0x223/0x25a0 mm/page_alloc.c:4733
   alloc_pages_mpol_noprof+0x2c9/0x610 mm/mempolicy.c:2265
   alloc_slab_page mm/slub.c:2412 [inline]
   allocate_slab mm/slub.c:2578 [inline]
   new_slab+0x2ba/0x3f0 mm/slub.c:2631
   ___slab_alloc+0xd1d/0x16f0 mm/slub.c:3818
   __slab_alloc.constprop.0+0x56/0xb0 mm/slub.c:3908
   __slab_alloc_node mm/slub.c:3961 [inline]
   slab_alloc_node mm/slub.c:4122 [inline]
   __kmalloc_cache_noprof+0x2c5/0x310 mm/slub.c:4290
   kmalloc_noprof include/linux/slab.h:878 [inline]
   kzalloc_noprof include/linux/slab.h:1014 [inline]
   mld_add_delrec net/ipv6/mcast.c:743 [inline]
   igmp6_leave_group net/ipv6/mcast.c:2625 [inline]
   igmp6_group_dropped+0x4ab/0xe40 net/ipv6/mcast.c:723
   __ipv6_dev_mc_dec+0x281/0x360 net/ipv6/mcast.c:979
   addrconf_leave_solict net/ipv6/addrconf.c:2253 [inline]
   __ipv6_ifa_notify+0x3f6/0xc30 net/ipv6/addrconf.c:6283
   addrconf_ifdown.isra.0+0xef9/0x1a20 net/ipv6/addrconf.c:3982
   addrconf_notify+0x220/0x19c0 net/ipv6/addrconf.c:3781
   notifier_call_chain+0xb9/0x410 kernel/notifier.c:93
   call_netdevice_notifiers_info+0xbe/0x140 net/core/dev.c:1996
   call_netdevice_notifiers_extack net/core/dev.c:2034 [inline]
   call_netdevice_notifiers net/core/dev.c:2048 [inline]
   dev_close_many+0x333/0x6a0 net/core/dev.c:1589
  page last free pid 13136 tgid 13136 stack trace:
   reset_page_owner include/linux/page_owner.h:25 [inline]
   free_pages_prepare mm/page_alloc.c:1108 [inline]
   free_unref_page+0x5f4/0xdc0 mm/page_alloc.c:2638
   stack_depot_save_flags+0x2da/0x900 lib/stackdepot.c:666
   kasan_save_stack+0x42/0x60 mm/kasan/common.c:48
   kasan_save_track+0x14/0x30 mm/kasan/common.c:68
   unpoison_slab_object mm/kasan/common.c:319 [inline]
   __kasan_slab_alloc+0x89/0x90 mm/kasan/common.c:345
   kasan_slab_alloc include/linux/kasan.h:247 [inline]
   slab_post_alloc_hook mm/slub.c:4085 [inline]
   slab_alloc_node mm/slub.c:4134 [inline]
   kmem_cache_alloc_noprof+0x121/0x2f0 mm/slub.c:4141
   skb_clone+0x190/0x3f0 net/core/skbuff.c:2084
   do_one_broadcast net/netlink/af_netlink.c:1462 [inline]
   netlink_broadcast_filtered+0xb11/0xef0 net/netlink/af_netlink.c:1540
   netlink_broadcast+0x39/0x50 net/netlink/af_netlink.c:1564
   uevent_net_broadcast_untagged lib/kobject_uevent.c:331 [inline]
   kobject_uevent_net_broadcast lib/kobject_uevent.c:410 [inline]
   kobject_uevent_env+0xacd/0x1670 lib/kobject_uevent.c:608
   device_del+0x623/0x9f0 drivers/base/core.c:3882
   snd_card_disconnect.part.0+0x58a/0x7c0 sound/core/init.c:546
   snd_card_disconnect+0x1f/0x30 sound/core/init.c:495
   snd_usx2y_disconnect+0xe9/0x1f0 sound/usb/usx2y/usbusx2y.c:417
   usb_unbind_interface+0x1e8/0x970 drivers/usb/core/driver.c:461
   device_remove drivers/base/dd.c:569 [inline]
   device_remove+0x122/0x170 drivers/base/dd.c:561

That's because 'subflow' is used just after 'mptcp_close_ssk(subflow)',
which will initiate the release of its memory. Even if it is very likely
the release and the re-utilisation will be done later on, it is of
course better to avoid any issues and read the content of 'subflow'
before closing it.

Fixes: 1c1f72137598 ("mptcp: pm: only decrement add_addr_accepted for MPJ req")
Cc: stable@vger.kernel.org
Reported-by: syzbot+3c8b7a8e7df6a2a226ca@syzkaller.appspotmail.com
Closes: https://lore.kernel.org/670d7337.050a0220.4cbc0.004f.GAE@google.com
Signed-off-by: Matthieu Baerts (NGI0) <matttbe@kernel.org>
Acked-by: Paolo Abeni <pabeni@redhat.com>
Link: https://patch.msgid.link/20241015-net-mptcp-uaf-pm-rm-v1-1-c4ee5d987a64@kernel.org
Signed-off-by: Paolo Abeni <pabeni@redhat.com>

## Buggy Code

```c
// Function: mptcp_pm_nl_rm_addr_or_subflow in net/mptcp/pm_netlink.c
static void mptcp_pm_nl_rm_addr_or_subflow(struct mptcp_sock *msk,
					   const struct mptcp_rm_list *rm_list,
					   enum linux_mptcp_mib_field rm_type)
{
	struct mptcp_subflow_context *subflow, *tmp;
	struct sock *sk = (struct sock *)msk;
	u8 i;

	pr_debug("%s rm_list_nr %d\n",
		 rm_type == MPTCP_MIB_RMADDR ? "address" : "subflow", rm_list->nr);

	msk_owned_by_me(msk);

	if (sk->sk_state == TCP_LISTEN)
		return;

	if (!rm_list->nr)
		return;

	if (list_empty(&msk->conn_list))
		return;

	for (i = 0; i < rm_list->nr; i++) {
		u8 rm_id = rm_list->ids[i];
		bool removed = false;

		mptcp_for_each_subflow_safe(msk, subflow, tmp) {
			struct sock *ssk = mptcp_subflow_tcp_sock(subflow);
			u8 remote_id = READ_ONCE(subflow->remote_id);
			int how = RCV_SHUTDOWN | SEND_SHUTDOWN;
			u8 id = subflow_get_local_id(subflow);

			if ((1 << inet_sk_state_load(ssk)) &
			    (TCPF_FIN_WAIT1 | TCPF_FIN_WAIT2 | TCPF_CLOSING | TCPF_CLOSE))
				continue;
			if (rm_type == MPTCP_MIB_RMADDR && remote_id != rm_id)
				continue;
			if (rm_type == MPTCP_MIB_RMSUBFLOW && id != rm_id)
				continue;

			pr_debug(" -> %s rm_list_ids[%d]=%u local_id=%u remote_id=%u mpc_id=%u\n",
				 rm_type == MPTCP_MIB_RMADDR ? "address" : "subflow",
				 i, rm_id, id, remote_id, msk->mpc_endpoint_id);
			spin_unlock_bh(&msk->pm.lock);
			mptcp_subflow_shutdown(sk, ssk, how);

			/* the following takes care of updating the subflows counter */
			mptcp_close_ssk(sk, ssk, subflow);
			spin_lock_bh(&msk->pm.lock);

			removed |= subflow->request_join;
			if (rm_type == MPTCP_MIB_RMSUBFLOW)
				__MPTCP_INC_STATS(sock_net(sk), rm_type);
		}

		if (rm_type == MPTCP_MIB_RMADDR)
			__MPTCP_INC_STATS(sock_net(sk), rm_type);

		if (!removed)
			continue;

		if (!mptcp_pm_is_kernel(msk))
			continue;

		if (rm_type == MPTCP_MIB_RMADDR && rm_id &&
		    !WARN_ON_ONCE(msk->pm.add_addr_accepted == 0)) {
			/* Note: if the subflow has been closed before, this
			 * add_addr_accepted counter will not be decremented.
			 */
			if (--msk->pm.add_addr_accepted < mptcp_pm_get_add_addr_accept_max(msk))
				WRITE_ONCE(msk->pm.accept_addr, true);
		}
	}
}
```

## Bug Fix Patch

```diff
diff --git a/net/mptcp/pm_netlink.c b/net/mptcp/pm_netlink.c
index 1a78998fe1f4..db586a5b3866 100644
--- a/net/mptcp/pm_netlink.c
+++ b/net/mptcp/pm_netlink.c
@@ -873,12 +873,12 @@ static void mptcp_pm_nl_rm_addr_or_subflow(struct mptcp_sock *msk,
 				 i, rm_id, id, remote_id, msk->mpc_endpoint_id);
 			spin_unlock_bh(&msk->pm.lock);
 			mptcp_subflow_shutdown(sk, ssk, how);
+			removed |= subflow->request_join;

 			/* the following takes care of updating the subflows counter */
 			mptcp_close_ssk(sk, ssk, subflow);
 			spin_lock_bh(&msk->pm.lock);

-			removed |= subflow->request_join;
 			if (rm_type == MPTCP_MIB_RMSUBFLOW)
 				__MPTCP_INC_STATS(sock_net(sk), rm_type);
 		}
```


## Bug Pattern

Using a pointer after calling a function that can free/release the pointed object.

Concrete form:
- An object (e.g., subflow) is referenced.
- Lock is dropped.
- A teardown/close function is called that may free or schedule freeing of the object (e.g., mptcp_close_ssk(sk, ssk, subflow)).
- Lock is re-acquired.
- The code then reads a field from the same object (e.g., subflow->request_join).

This leads to a use-after-free read because the object’s memory may have been released between the close and the subsequent access. The necessary data must be read or copied before invoking the destructor (or a proper lifetime/reference must be held).


# Report

BuildSource:| net/mptcp/pm_netlink.c
### Report Summary

File:| ./include/linux/list.h  
---|---  
Warning:| line 218, column 13  
use-after-free: pointer used after it was released; released by call to kfree  
  
### Annotated Source Code


1     | // SPDX-License-Identifier: GPL-2.0
2     | /* Multipath TCP
3     |  *
4     |  * Copyright (c) 2020, Red Hat, Inc.
5     |  */
6     |  
7     | #define pr_fmt(fmt) "MPTCP: " fmt
8     |  
9     | #include <linux/inet.h>
10    | #include <linux/kernel.h>
11    | #include <net/inet_common.h>
12    | #include <net/netns/generic.h>
13    | #include <net/mptcp.h>
14    |  
15    | #include "protocol.h"
16    | #include "mib.h"
17    | #include "mptcp_pm_gen.h"
18    |  
19    | static int pm_nl_pernet_id;
20    |  
21    | struct mptcp_pm_add_entry {
22    |  struct list_head	list;
23    |  struct mptcp_addr_info	addr;
24    | 	u8			retrans_times;
25    |  struct timer_list	add_timer;
26    |  struct mptcp_sock	*sock;
27    | };
28    |  
29    | struct pm_nl_pernet {
30    |  /* protects pernet updates */
31    | 	spinlock_t		lock;
32    |  struct list_head	local_addr_list;
33    |  unsigned int		addrs;
34    |  unsigned int		stale_loss_cnt;
35    |  unsigned int		add_addr_signal_max;
36    |  unsigned int		add_addr_accept_max;
37    |  unsigned int		local_addr_max;
38    |  unsigned int		subflows_max;
39    |  unsigned int		next_id;
40    |  DECLARE_BITMAP(id_bitmap, MPTCP_PM_MAX_ADDR_ID + 1);
41    | };
42    |  
43    | #define MPTCP_PM_ADDR_MAX	8
44    | #define ADD_ADDR_RETRANS_MAX	3
45    |  
46    | static struct pm_nl_pernet *pm_nl_get_pernet(const struct net *net)
47    | {
48    |  return net_generic(net, pm_nl_pernet_id);
49    | }
50    |  
51    | static struct pm_nl_pernet *
52    | pm_nl_get_pernet_from_msk(const struct mptcp_sock *msk)
53    | {
54    |  return pm_nl_get_pernet(sock_net((struct sock *)msk));
55    | }
56    |  
57    | bool mptcp_addresses_equal(const struct mptcp_addr_info *a,
58    |  const struct mptcp_addr_info *b, bool use_port)
59    | {
60    | 	bool addr_equals = false;
61    |  
62    |  if (a->family == b->family) {
63    |  if (a->family == AF_INET)
64    | 			addr_equals = a->addr.s_addr == b->addr.s_addr;
65    | #if IS_ENABLED(CONFIG_MPTCP_IPV6)
66    |  else
67    | 			addr_equals = !ipv6_addr_cmp(&a->addr6, &b->addr6);
68    | 	} else if (a->family == AF_INET) {
69    |  if (ipv6_addr_v4mapped(&b->addr6))
70    | 			addr_equals = a->addr.s_addr == b->addr6.s6_addr32[3];
71    | 	} else if (b->family == AF_INET) {
72    |  if (ipv6_addr_v4mapped(&a->addr6))
73    | 			addr_equals = a->addr6.s6_addr32[3] == b->addr.s_addr;
74    | #endif
75    | 	}
76    |  
77    |  if (!addr_equals)
78    |  return false;
911   | static void mptcp_pm_nl_rm_subflow_received(struct mptcp_sock *msk,
912   |  const struct mptcp_rm_list *rm_list)
913   | {
914   | 	mptcp_pm_nl_rm_addr_or_subflow(msk, rm_list, MPTCP_MIB_RMSUBFLOW);
915   | }
916   |  
917   | void mptcp_pm_nl_work(struct mptcp_sock *msk)
918   | {
919   |  struct mptcp_pm_data *pm = &msk->pm;
920   |  
921   | 	msk_owned_by_me(msk);
922   |  
923   |  if (!(pm->status & MPTCP_PM_WORK_MASK))
924   |  return;
925   |  
926   | 	spin_lock_bh(&msk->pm.lock);
927   |  
928   |  pr_debug("msk=%p status=%x\n", msk, pm->status);
929   |  if (pm->status & BIT(MPTCP_PM_ADD_ADDR_RECEIVED)) {
930   | 		pm->status &= ~BIT(MPTCP_PM_ADD_ADDR_RECEIVED);
931   | 		mptcp_pm_nl_add_addr_received(msk);
932   | 	}
933   |  if (pm->status & BIT(MPTCP_PM_ADD_ADDR_SEND_ACK)) {
934   | 		pm->status &= ~BIT(MPTCP_PM_ADD_ADDR_SEND_ACK);
935   | 		mptcp_pm_nl_addr_send_ack(msk);
936   | 	}
937   |  if (pm->status & BIT(MPTCP_PM_RM_ADDR_RECEIVED)) {
938   | 		pm->status &= ~BIT(MPTCP_PM_RM_ADDR_RECEIVED);
939   | 		mptcp_pm_nl_rm_addr_received(msk);
940   | 	}
941   |  if (pm->status & BIT(MPTCP_PM_ESTABLISHED)) {
942   | 		pm->status &= ~BIT(MPTCP_PM_ESTABLISHED);
943   | 		mptcp_pm_nl_fully_established(msk);
944   | 	}
945   |  if (pm->status & BIT(MPTCP_PM_SUBFLOW_ESTABLISHED)) {
946   | 		pm->status &= ~BIT(MPTCP_PM_SUBFLOW_ESTABLISHED);
947   | 		mptcp_pm_nl_subflow_established(msk);
948   | 	}
949   |  
950   | 	spin_unlock_bh(&msk->pm.lock);
951   | }
952   |  
953   | static bool address_use_port(struct mptcp_pm_addr_entry *entry)
954   | {
955   |  return (entry->flags &
956   | 		(MPTCP_PM_ADDR_FLAG_SIGNAL | MPTCP_PM_ADDR_FLAG_SUBFLOW)) ==
957   |  MPTCP_PM_ADDR_FLAG_SIGNAL;
958   | }
959   |  
960   | /* caller must ensure the RCU grace period is already elapsed */
961   | static void __mptcp_pm_release_addr_entry(struct mptcp_pm_addr_entry *entry)
962   | {
963   |  if (entry->lsk)
964   | 		sock_release(entry->lsk);
965   | 	kfree(entry);
966   | }
967   |  
968   | static int mptcp_pm_nl_append_new_local_addr(struct pm_nl_pernet *pernet,
969   |  struct mptcp_pm_addr_entry *entry,
970   | 					     bool needs_id)
971   | {
972   |  struct mptcp_pm_addr_entry *cur, *del_entry = NULL;
973   |  unsigned int addr_max;
974   |  int ret = -EINVAL;
975   |  
976   | 	spin_lock_bh(&pernet->lock);
977   |  /* to keep the code simple, don't do IDR-like allocation for address ID,
978   |  * just bail when we exceed limits
979   |  */
980   |  if (pernet->next_id == MPTCP_PM_MAX_ADDR_ID)
981   | 		pernet->next_id = 1;
982   |  if (pernet->addrs >= MPTCP_PM_ADDR_MAX) {
983   | 		ret = -ERANGE;
984   |  goto out;
985   | 	}
986   |  if (test_bit(entry->addr.id, pernet->id_bitmap)) {
987   | 		ret = -EBUSY;
988   |  goto out;
989   | 	}
990   |  
991   |  /* do not insert duplicate address, differentiate on port only
992   |  * singled addresses
993   |  */
994   |  if (!address_use_port(entry))
995   | 		entry->addr.port = 0;
1667  |  struct mptcp_rm_list alist = { .nr = 0 }, slist = { .nr = 0 };
1668  |  struct mptcp_pm_addr_entry *entry;
1669  |  
1670  |  list_for_each_entry(entry, rm_list, list) {
1671  |  if (slist.nr < MPTCP_RM_IDS_MAX &&
1672  | 		    lookup_subflow_by_saddr(&msk->conn_list, &entry->addr))
1673  | 			slist.ids[slist.nr++] = mptcp_endp_get_local_id(msk, &entry->addr);
1674  |  
1675  |  if (alist.nr < MPTCP_RM_IDS_MAX &&
1676  | 		    remove_anno_list_by_saddr(msk, &entry->addr))
1677  | 			alist.ids[alist.nr++] = mptcp_endp_get_local_id(msk, &entry->addr);
1678  | 	}
1679  |  
1680  | 	spin_lock_bh(&msk->pm.lock);
1681  |  if (alist.nr) {
1682  | 		msk->pm.add_addr_signaled -= alist.nr;
1683  | 		mptcp_pm_remove_addr(msk, &alist);
1684  | 	}
1685  |  if (slist.nr)
1686  | 		mptcp_pm_nl_rm_subflow_received(msk, &slist);
1687  |  /* Reset counters: maybe some subflows have been removed before */
1688  | 	bitmap_fill(msk->pm.id_avail_bitmap, MPTCP_PM_MAX_ADDR_ID + 1);
1689  | 	msk->pm.local_addr_used = 0;
1690  | 	spin_unlock_bh(&msk->pm.lock);
1691  | }
1692  |  
1693  | static void mptcp_nl_flush_addrs_list(struct net *net,
1694  |  struct list_head *rm_list)
1695  | {
1696  |  long s_slot = 0, s_num = 0;
1697  |  struct mptcp_sock *msk;
1698  |  
1699  |  if (list_empty(rm_list))
1700  |  return;
1701  |  
1702  |  while ((msk = mptcp_token_iter_next(net, &s_slot, &s_num)) != NULL) {
1703  |  struct sock *sk = (struct sock *)msk;
1704  |  
1705  |  if (!mptcp_pm_is_userspace(msk)) {
1706  | 			lock_sock(sk);
1707  | 			mptcp_pm_flush_addrs_and_subflows(msk, rm_list);
1708  | 			release_sock(sk);
1709  | 		}
1710  |  
1711  | 		sock_put(sk);
1712  |  cond_resched();
1713  | 	}
1714  | }
1715  |  
1716  | /* caller must ensure the RCU grace period is already elapsed */
1717  | static void __flush_addrs(struct list_head *list)
1718  | {
1719  |  while (!list_empty(list)) {
    3←Loop condition is true.  Entering loop body→
    4←Loop condition is true.  Entering loop body→
1720  |  struct mptcp_pm_addr_entry *cur;
1721  |  
1722  | 		cur = list_entry(list->next,
1723  |  struct mptcp_pm_addr_entry, list);
1724  |  list_del_rcu(&cur->list);
    5←Calling 'list_del_rcu'→
1725  |  __mptcp_pm_release_addr_entry(cur);
1726  |  }
1727  | }
1728  |  
1729  | static void __reset_counters(struct pm_nl_pernet *pernet)
1730  | {
1731  |  WRITE_ONCE(pernet->add_addr_signal_max, 0);
1732  |  WRITE_ONCE(pernet->add_addr_accept_max, 0);
1733  |  WRITE_ONCE(pernet->local_addr_max, 0);
1734  | 	pernet->addrs = 0;
1735  | }
1736  |  
1737  | int mptcp_pm_nl_flush_addrs_doit(struct sk_buff *skb, struct genl_info *info)
1738  | {
1739  |  struct pm_nl_pernet *pernet = genl_info_pm_nl(info);
1740  |  LIST_HEAD(free_list);
1741  |  
1742  | 	spin_lock_bh(&pernet->lock);
1743  | 	list_splice_init(&pernet->local_addr_list, &free_list);
1744  | 	__reset_counters(pernet);
1745  | 	pernet->next_id = 1;
1746  | 	bitmap_zero(pernet->id_bitmap, MPTCP_PM_MAX_ADDR_ID + 1);
1747  | 	spin_unlock_bh(&pernet->lock);
1748  | 	mptcp_nl_flush_addrs_list(sock_net(skb->sk), &free_list);
1749  | 	synchronize_rcu();
1750  | 	__flush_addrs(&free_list);
1751  |  return 0;
1752  | }
1753  |  
1754  | int mptcp_nl_fill_addr(struct sk_buff *skb,
1755  |  struct mptcp_pm_addr_entry *entry)
1756  | {
2389  |  goto nla_put_failure;
2390  |  break;
2391  |  case MPTCP_EVENT_SUB_CLOSED:
2392  |  if (mptcp_event_sub_closed(skb, msk, ssk) < 0)
2393  |  goto nla_put_failure;
2394  |  break;
2395  |  case MPTCP_EVENT_LISTENER_CREATED:
2396  |  case MPTCP_EVENT_LISTENER_CLOSED:
2397  |  break;
2398  | 	}
2399  |  
2400  | 	genlmsg_end(skb, nlh);
2401  | 	mptcp_nl_mcast_send(net, skb, gfp);
2402  |  return;
2403  |  
2404  | nla_put_failure:
2405  | 	nlmsg_free(skb);
2406  | }
2407  |  
2408  | struct genl_family mptcp_genl_family __ro_after_init = {
2409  | 	.name		= MPTCP_PM_NAME,
2410  | 	.version	= MPTCP_PM_VER,
2411  | 	.netnsok	= true,
2412  | 	.module		= THIS_MODULE,
2413  | 	.ops		= mptcp_pm_nl_ops,
2414  | 	.n_ops		= ARRAY_SIZE(mptcp_pm_nl_ops),
2415  | 	.resv_start_op	= MPTCP_PM_CMD_SUBFLOW_DESTROY + 1,
2416  | 	.mcgrps		= mptcp_pm_mcgrps,
2417  | 	.n_mcgrps	= ARRAY_SIZE(mptcp_pm_mcgrps),
2418  | };
2419  |  
2420  | static int __net_init pm_nl_init_net(struct net *net)
2421  | {
2422  |  struct pm_nl_pernet *pernet = pm_nl_get_pernet(net);
2423  |  
2424  | 	INIT_LIST_HEAD_RCU(&pernet->local_addr_list);
2425  |  
2426  |  /* Cit. 2 subflows ought to be enough for anybody. */
2427  | 	pernet->subflows_max = 2;
2428  | 	pernet->next_id = 1;
2429  | 	pernet->stale_loss_cnt = 4;
2430  |  spin_lock_init(&pernet->lock);
2431  |  
2432  |  /* No need to initialize other pernet fields, the struct is zeroed at
2433  |  * allocation time.
2434  |  */
2435  |  
2436  |  return 0;
2437  | }
2438  |  
2439  | static void __net_exit pm_nl_exit_net(struct list_head *net_list)
2440  | {
2441  |  struct net *net;
2442  |  
2443  |  list_for_each_entry(net, net_list, exit_list) {
    1Loop condition is true.  Entering loop body→
2444  |  struct pm_nl_pernet *pernet = pm_nl_get_pernet(net);
2445  |  
2446  |  /* net is removed from namespace list, can't race with
2447  |  * other modifiers, also netns core already waited for a
2448  |  * RCU grace period.
2449  |  */
2450  |  __flush_addrs(&pernet->local_addr_list);
    2←Calling '__flush_addrs'→
2451  | 	}
2452  | }
2453  |  
2454  | static struct pernet_operations mptcp_pm_pernet_ops = {
2455  | 	.init = pm_nl_init_net,
2456  | 	.exit_batch = pm_nl_exit_net,
2457  | 	.id = &pm_nl_pernet_id,
2458  | 	.size = sizeof(struct pm_nl_pernet),
2459  | };
2460  |  
2461  | void __init mptcp_pm_nl_init(void)
2462  | {
2463  |  if (register_pernet_subsys(&mptcp_pm_pernet_ops) < 0)
2464  | 		panic("Failed to register MPTCP PM pernet subsystem.\n");
2465  |  
2466  |  if (genl_register_family(&mptcp_genl_family))
2467  | 		panic("Failed to register MPTCP PM netlink family\n");
2468  | }
105   | {
106   | 	__list_add_rcu(new, head, head->next);
107   | }
108   |  
109   | /**
110   |  * list_add_tail_rcu - add a new entry to rcu-protected list
111   |  * @new: new entry to be added
112   |  * @head: list head to add it before
113   |  *
114   |  * Insert a new entry before the specified head.
115   |  * This is useful for implementing queues.
116   |  *
117   |  * The caller must take whatever precautions are necessary
118   |  * (such as holding appropriate locks) to avoid racing
119   |  * with another list-mutation primitive, such as list_add_tail_rcu()
120   |  * or list_del_rcu(), running on this same list.
121   |  * However, it is perfectly legal to run concurrently with
122   |  * the _rcu list-traversal primitives, such as
123   |  * list_for_each_entry_rcu().
124   |  */
125   | static inline void list_add_tail_rcu(struct list_head *new,
126   |  struct list_head *head)
127   | {
128   | 	__list_add_rcu(new, head->prev, head);
129   | }
130   |  
131   | /**
132   |  * list_del_rcu - deletes entry from list without re-initialization
133   |  * @entry: the element to delete from the list.
134   |  *
135   |  * Note: list_empty() on entry does not return true after this,
136   |  * the entry is in an undefined state. It is useful for RCU based
137   |  * lockfree traversal.
138   |  *
139   |  * In particular, it means that we can not poison the forward
140   |  * pointers that may still be used for walking the list.
141   |  *
142   |  * The caller must take whatever precautions are necessary
143   |  * (such as holding appropriate locks) to avoid racing
144   |  * with another list-mutation primitive, such as list_del_rcu()
145   |  * or list_add_rcu(), running on this same list.
146   |  * However, it is perfectly legal to run concurrently with
147   |  * the _rcu list-traversal primitives, such as
148   |  * list_for_each_entry_rcu().
149   |  *
150   |  * Note that the caller is not permitted to immediately free
151   |  * the newly deleted entry.  Instead, either synchronize_rcu()
152   |  * or call_rcu() must be used to defer freeing until an RCU
153   |  * grace period has elapsed.
154   |  */
155   | static inline void list_del_rcu(struct list_head *entry)
156   | {
157   |  __list_del_entry(entry);
    6←Calling '__list_del_entry'→
158   | 	entry->prev = LIST_POISON2;
159   | }
160   |  
161   | /**
162   |  * hlist_del_init_rcu - deletes entry from hash list with re-initialization
163   |  * @n: the element to delete from the hash list.
164   |  *
165   |  * Note: list_unhashed() on the node return true after this. It is
166   |  * useful for RCU based read lockfree traversal if the writer side
167   |  * must know if the list entry is still hashed or already unhashed.
168   |  *
169   |  * In particular, it means that we can not poison the forward pointers
170   |  * that may still be used for walking the hash list and we can only
171   |  * zero the pprev pointer so list_unhashed() will return true after
172   |  * this.
173   |  *
174   |  * The caller must take whatever precautions are necessary (such as
175   |  * holding appropriate locks) to avoid racing with another
176   |  * list-mutation primitive, such as hlist_add_head_rcu() or
177   |  * hlist_del_rcu(), running on this same list.  However, it is
178   |  * perfectly legal to run concurrently with the _rcu list-traversal
179   |  * primitives, such as hlist_for_each_entry_rcu().
180   |  */
181   | static inline void hlist_del_init_rcu(struct hlist_node *n)
182   | {
183   |  if (!hlist_unhashed(n)) {
184   | 		__hlist_del(n);
185   |  WRITE_ONCE(n->pprev, NULL);
186   | 	}
187   | }
188   |  
56    |  
57    | /*
58    |  * Performs list corruption checks before __list_add(). Returns false if a
59    |  * corruption is detected, true otherwise.
60    |  *
61    |  * With CONFIG_LIST_HARDENED only, performs minimal list integrity checking
62    |  * inline to catch non-faulting corruptions, and only if a corruption is
63    |  * detected calls the reporting function __list_add_valid_or_report().
64    |  */
65    | static __always_inline bool __list_add_valid(struct list_head *new,
66    |  struct list_head *prev,
67    |  struct list_head *next)
68    | {
69    | 	bool ret = true;
70    |  
71    |  if (!IS_ENABLED(CONFIG_DEBUG_LIST)) {
72    |  /*
73    |  * With the hardening version, elide checking if next and prev
74    |  * are NULL, since the immediate dereference of them below would
75    |  * result in a fault if NULL.
76    |  *
77    |  * With the reduced set of checks, we can afford to inline the
78    |  * checks, which also gives the compiler a chance to elide some
79    |  * of them completely if they can be proven at compile-time. If
80    |  * one of the pre-conditions does not hold, the slow-path will
81    |  * show a report which pre-condition failed.
82    |  */
83    |  if (likely(next->prev == prev && prev->next == next && new != prev && new != next))
84    |  return true;
85    | 		ret = false;
86    | 	}
87    |  
88    | 	ret &= __list_add_valid_or_report(new, prev, next);
89    |  return ret;
90    | }
91    |  
92    | /*
93    |  * Performs the full set of list corruption checks before __list_del_entry().
94    |  * On list corruption reports a warning, and returns false.
95    |  */
96    | extern bool __list_valid_slowpath __list_del_entry_valid_or_report(struct list_head *entry);
97    |  
98    | /*
99    |  * Performs list corruption checks before __list_del_entry(). Returns false if a
100   |  * corruption is detected, true otherwise.
101   |  *
102   |  * With CONFIG_LIST_HARDENED only, performs minimal list integrity checking
103   |  * inline to catch non-faulting corruptions, and only if a corruption is
104   |  * detected calls the reporting function __list_del_entry_valid_or_report().
105   |  */
106   | static __always_inline bool __list_del_entry_valid(struct list_head *entry)
107   | {
108   | 	bool ret = true;
109   |  
110   |  if (!IS_ENABLED(CONFIG_DEBUG_LIST)) {
111   |  struct list_head *prev = entry->prev;
112   |  struct list_head *next = entry->next;
113   |  
114   |  /*
115   |  * With the hardening version, elide checking if next and prev
116   |  * are NULL, LIST_POISON1 or LIST_POISON2, since the immediate
117   |  * dereference of them below would result in a fault.
118   |  */
119   |  if (likely(prev->next == entry && next->prev == entry))
120   |  return true;
121   | 		ret = false;
122   | 	}
123   |  
124   | 	ret &= __list_del_entry_valid_or_report(entry);
125   |  return ret;
126   | }
127   | #else
128   | static inline bool __list_add_valid(struct list_head *new,
129   |  struct list_head *prev,
130   |  struct list_head *next)
131   | {
132   |  return true;
133   | }
134   | static inline bool __list_del_entry_valid(struct list_head *entry)
135   | {
136   |  return true;
137   | }
138   | #endif
139   |  
140   | /*
141   |  * Insert a new entry between two known consecutive entries.
142   |  *
143   |  * This is only for internal list manipulation where we know
144   |  * the prev/next entries already!
145   |  */
146   | static inline void __list_add(struct list_head *new,
147   |  struct list_head *prev,
148   |  struct list_head *next)
149   | {
150   |  if (!__list_add_valid(new, prev, next))
151   |  return;
152   |  
153   | 	next->prev = new;
154   | 	new->next = next;
155   | 	new->prev = prev;
163   |  *
164   |  * Insert a new entry after the specified head.
165   |  * This is good for implementing stacks.
166   |  */
167   | static inline void list_add(struct list_head *new, struct list_head *head)
168   | {
169   | 	__list_add(new, head, head->next);
170   | }
171   |  
172   |  
173   | /**
174   |  * list_add_tail - add a new entry
175   |  * @new: new entry to be added
176   |  * @head: list head to add it before
177   |  *
178   |  * Insert a new entry before the specified head.
179   |  * This is useful for implementing queues.
180   |  */
181   | static inline void list_add_tail(struct list_head *new, struct list_head *head)
182   | {
183   | 	__list_add(new, head->prev, head);
184   | }
185   |  
186   | /*
187   |  * Delete a list entry by making the prev/next entries
188   |  * point to each other.
189   |  *
190   |  * This is only for internal list manipulation where we know
191   |  * the prev/next entries already!
192   |  */
193   | static inline void __list_del(struct list_head * prev, struct list_head * next)
194   | {
195   | 	next->prev = prev;
196   |  WRITE_ONCE(prev->next, next);
197   | }
198   |  
199   | /*
200   |  * Delete a list entry and clear the 'prev' pointer.
201   |  *
202   |  * This is a special-purpose list clearing method used in the networking code
203   |  * for lists allocated as per-cpu, where we don't want to incur the extra
204   |  * WRITE_ONCE() overhead of a regular list_del_init(). The code that uses this
205   |  * needs to check the node 'prev' pointer instead of calling list_empty().
206   |  */
207   | static inline void __list_del_clearprev(struct list_head *entry)
208   | {
209   | 	__list_del(entry->prev, entry->next);
210   | 	entry->prev = NULL;
211   | }
212   |  
213   | static inline void __list_del_entry(struct list_head *entry)
214   | {
215   |  if (!__list_del_entry_valid(entry))
    7←Assuming the condition is false→
    8←Taking false branch→
216   |  return;
217   |  
218   |  __list_del(entry->prev, entry->next);
    9←use-after-free: pointer used after it was released; released by call to kfree
219   | }
220   |  
221   | /**
222   |  * list_del - deletes entry from list.
223   |  * @entry: the element to delete from the list.
224   |  * Note: list_empty() on entry does not return true after this, the entry is
225   |  * in an undefined state.
226   |  */
227   | static inline void list_del(struct list_head *entry)
228   | {
229   | 	__list_del_entry(entry);
230   | 	entry->next = LIST_POISON1;
231   | 	entry->prev = LIST_POISON2;
232   | }
233   |  
234   | /**
235   |  * list_replace - replace old entry by new one
236   |  * @old : the element to be replaced
237   |  * @new : the new element to insert
238   |  *
239   |  * If @old was empty, it will be overwritten.
240   |  */
241   | static inline void list_replace(struct list_head *old,
242   |  struct list_head *new)
243   | {
244   | 	new->next = old->next;
245   | 	new->next->prev = new;
246   | 	new->prev = old->prev;
247   | 	new->prev->next = new;
248   | }
312   | }
313   |  
314   | /**
315   |  * list_bulk_move_tail - move a subsection of a list to its tail
316   |  * @head: the head that will follow our entry
317   |  * @first: first entry to move
318   |  * @last: last entry to move, can be the same as first
319   |  *
320   |  * Move all entries between @first and including @last before @head.
321   |  * All three entries must belong to the same linked list.
322   |  */
323   | static inline void list_bulk_move_tail(struct list_head *head,
324   |  struct list_head *first,
325   |  struct list_head *last)
326   | {
327   | 	first->prev->next = last->next;
328   | 	last->next->prev = first->prev;
329   |  
330   | 	head->prev->next = first;
331   | 	first->prev = head->prev;
332   |  
333   | 	last->next = head;
334   | 	head->prev = last;
335   | }
336   |  
337   | /**
338   |  * list_is_first -- tests whether @list is the first entry in list @head
339   |  * @list: the entry to test
340   |  * @head: the head of the list
341   |  */
342   | static inline int list_is_first(const struct list_head *list, const struct list_head *head)
343   | {
344   |  return list->prev == head;
345   | }
346   |  
347   | /**
348   |  * list_is_last - tests whether @list is the last entry in list @head
349   |  * @list: the entry to test
350   |  * @head: the head of the list
351   |  */
352   | static inline int list_is_last(const struct list_head *list, const struct list_head *head)
353   | {
354   |  return list->next == head;
355   | }
356   |  
357   | /**
358   |  * list_is_head - tests whether @list is the list @head
359   |  * @list: the entry to test
360   |  * @head: the head of the list
361   |  */
362   | static inline int list_is_head(const struct list_head *list, const struct list_head *head)
363   | {
364   |  return list == head;
365   | }
366   |  
367   | /**
368   |  * list_empty - tests whether a list is empty
369   |  * @head: the list to test.
370   |  */
371   | static inline int list_empty(const struct list_head *head)
372   | {
373   |  return READ_ONCE(head->next) == head;
374   | }
375   |  
376   | /**
377   |  * list_del_init_careful - deletes entry from list and reinitialize it.
378   |  * @entry: the element to delete from the list.
379   |  *
380   |  * This is the same as list_del_init(), except designed to be used
381   |  * together with list_empty_careful() in a way to guarantee ordering
382   |  * of other memory operations.
383   |  *
384   |  * Any memory operations done before a list_del_init_careful() are
385   |  * guaranteed to be visible after a list_empty_careful() test.
386   |  */
387   | static inline void list_del_init_careful(struct list_head *entry)
388   | {
389   | 	__list_del_entry(entry);
390   |  WRITE_ONCE(entry->prev, entry);
391   |  smp_store_release(&entry->next, entry);
392   | }
393   |  
394   | /**
395   |  * list_empty_careful - tests whether a list is empty and not being modified
396   |  * @head: the list to test
397   |  *
398   |  * Description:
399   |  * tests whether a list is empty _and_ checks that no other CPU might be
400   |  * in the process of modifying either member (next or prev)
401   |  *
402   |  * NOTE: using list_empty_careful() without synchronization
403   |  * can only be safe if the only activity that can happen

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
