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

tty: n_gsm: Fix use-after-free in gsm_cleanup_mux

BUG: KASAN: slab-use-after-free in gsm_cleanup_mux+0x77b/0x7b0
drivers/tty/n_gsm.c:3160 [n_gsm]
Read of size 8 at addr ffff88815fe99c00 by task poc/3379
CPU: 0 UID: 0 PID: 3379 Comm: poc Not tainted 6.11.0+ #56
Hardware name: VMware, Inc. VMware Virtual Platform/440BX
Desktop Reference Platform, BIOS 6.00 11/12/2020
Call Trace:
 <TASK>
 gsm_cleanup_mux+0x77b/0x7b0 drivers/tty/n_gsm.c:3160 [n_gsm]
 __pfx_gsm_cleanup_mux+0x10/0x10 drivers/tty/n_gsm.c:3124 [n_gsm]
 __pfx_sched_clock_cpu+0x10/0x10 kernel/sched/clock.c:389
 update_load_avg+0x1c1/0x27b0 kernel/sched/fair.c:4500
 __pfx_min_vruntime_cb_rotate+0x10/0x10 kernel/sched/fair.c:846
 __rb_insert_augmented+0x492/0xbf0 lib/rbtree.c:161
 gsmld_ioctl+0x395/0x1450 drivers/tty/n_gsm.c:3408 [n_gsm]
 _raw_spin_lock_irqsave+0x92/0xf0 arch/x86/include/asm/atomic.h:107
 __pfx_gsmld_ioctl+0x10/0x10 drivers/tty/n_gsm.c:3822 [n_gsm]
 ktime_get+0x5e/0x140 kernel/time/timekeeping.c:195
 ldsem_down_read+0x94/0x4e0 arch/x86/include/asm/atomic64_64.h:79
 __pfx_ldsem_down_read+0x10/0x10 drivers/tty/tty_ldsem.c:338
 __pfx_do_vfs_ioctl+0x10/0x10 fs/ioctl.c:805
 tty_ioctl+0x643/0x1100 drivers/tty/tty_io.c:2818

Allocated by task 65:
 gsm_data_alloc.constprop.0+0x27/0x190 drivers/tty/n_gsm.c:926 [n_gsm]
 gsm_send+0x2c/0x580 drivers/tty/n_gsm.c:819 [n_gsm]
 gsm1_receive+0x547/0xad0 drivers/tty/n_gsm.c:3038 [n_gsm]
 gsmld_receive_buf+0x176/0x280 drivers/tty/n_gsm.c:3609 [n_gsm]
 tty_ldisc_receive_buf+0x101/0x1e0 drivers/tty/tty_buffer.c:391
 tty_port_default_receive_buf+0x61/0xa0 drivers/tty/tty_port.c:39
 flush_to_ldisc+0x1b0/0x750 drivers/tty/tty_buffer.c:445
 process_scheduled_works+0x2b0/0x10d0 kernel/workqueue.c:3229
 worker_thread+0x3dc/0x950 kernel/workqueue.c:3391
 kthread+0x2a3/0x370 kernel/kthread.c:389
 ret_from_fork+0x2d/0x70 arch/x86/kernel/process.c:147
 ret_from_fork_asm+0x1a/0x30 arch/x86/entry/entry_64.S:257

Freed by task 3367:
 kfree+0x126/0x420 mm/slub.c:4580
 gsm_cleanup_mux+0x36c/0x7b0 drivers/tty/n_gsm.c:3160 [n_gsm]
 gsmld_ioctl+0x395/0x1450 drivers/tty/n_gsm.c:3408 [n_gsm]
 tty_ioctl+0x643/0x1100 drivers/tty/tty_io.c:2818

[Analysis]
gsm_msg on the tx_ctrl_list or tx_data_list of gsm_mux
can be freed by multi threads through ioctl,which leads
to the occurrence of uaf. Protect it by gsm tx lock.

Signed-off-by: Longlong Xia <xialonglong@kylinos.cn>
Cc: stable <stable@kernel.org>
Suggested-by: Jiri Slaby <jirislaby@kernel.org>
Link: https://lore.kernel.org/r/20240926130213.531959-1-xialonglong@kylinos.cn
Signed-off-by: Greg Kroah-Hartman <gregkh@linuxfoundation.org>

## Buggy Code

```c
// Function: gsm_cleanup_mux in drivers/tty/n_gsm.c
static void gsm_cleanup_mux(struct gsm_mux *gsm, bool disc)
{
	int i;
	struct gsm_dlci *dlci;
	struct gsm_msg *txq, *ntxq;

	gsm->dead = true;
	mutex_lock(&gsm->mutex);

	dlci = gsm->dlci[0];
	if (dlci) {
		if (disc && dlci->state != DLCI_CLOSED) {
			gsm_dlci_begin_close(dlci);
			wait_event(gsm->event, dlci->state == DLCI_CLOSED);
		}
		dlci->dead = true;
	}

	/* Finish outstanding timers, making sure they are done */
	del_timer_sync(&gsm->kick_timer);
	del_timer_sync(&gsm->t2_timer);
	del_timer_sync(&gsm->ka_timer);

	/* Finish writing to ldisc */
	flush_work(&gsm->tx_work);

	/* Free up any link layer users and finally the control channel */
	if (gsm->has_devices) {
		gsm_unregister_devices(gsm_tty_driver, gsm->num);
		gsm->has_devices = false;
	}
	for (i = NUM_DLCI - 1; i >= 0; i--)
		if (gsm->dlci[i])
			gsm_dlci_release(gsm->dlci[i]);
	mutex_unlock(&gsm->mutex);
	/* Now wipe the queues */
	tty_ldisc_flush(gsm->tty);
	list_for_each_entry_safe(txq, ntxq, &gsm->tx_ctrl_list, list)
		kfree(txq);
	INIT_LIST_HEAD(&gsm->tx_ctrl_list);
	list_for_each_entry_safe(txq, ntxq, &gsm->tx_data_list, list)
		kfree(txq);
	INIT_LIST_HEAD(&gsm->tx_data_list);
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/tty/n_gsm.c b/drivers/tty/n_gsm.c
index 5d37a0984916..252849910588 100644
--- a/drivers/tty/n_gsm.c
+++ b/drivers/tty/n_gsm.c
@@ -3157,6 +3157,8 @@ static void gsm_cleanup_mux(struct gsm_mux *gsm, bool disc)
 	mutex_unlock(&gsm->mutex);
 	/* Now wipe the queues */
 	tty_ldisc_flush(gsm->tty);
+
+	guard(spinlock_irqsave)(&gsm->tx_lock);
 	list_for_each_entry_safe(txq, ntxq, &gsm->tx_ctrl_list, list)
 		kfree(txq);
 	INIT_LIST_HEAD(&gsm->tx_ctrl_list);
```


## Bug Pattern

Traversing and freeing elements of a shared kernel list without holding the list’s protecting spinlock, while other contexts can concurrently manipulate or free the same list entries. Specifically, using list_for_each_entry_safe() to kfree() nodes of tx_ctrl_list/tx_data_list after dropping the mutex, but without acquiring gsm->tx_lock, allows concurrent frees (e.g., via ioctl), causing use-after-free.


# Report

BuildSource:| drivers/tty/n_gsm.c
### Report Summary

File:| n_gsm.c  
---|---  
Warning:| line 1041, column 5  
Freeing tx_* list entries without holding tx_lock (possible UAF)  
  
### Annotated Source Code


932   | 	INIT_LIST_HEAD(&m->list);
933   |  return m;
934   | }
935   |  
936   | /**
937   |  *	gsm_send_packet	-	sends a single packet
938   |  *	@gsm: GSM Mux
939   |  *	@msg: packet to send
940   |  *
941   |  *	The given packet is encoded and sent out. No memory is freed.
942   |  *	The caller must hold the gsm tx lock.
943   |  */
944   | static int gsm_send_packet(struct gsm_mux *gsm, struct gsm_msg *msg)
945   | {
946   |  int len, ret;
947   |  
948   |  
949   |  if (gsm->encoding == GSM_BASIC_OPT) {
950   | 		gsm->txframe[0] = GSM0_SOF;
951   |  memcpy(gsm->txframe + 1, msg->data, msg->len);
952   | 		gsm->txframe[msg->len + 1] = GSM0_SOF;
953   | 		len = msg->len + 2;
954   | 	} else {
955   | 		gsm->txframe[0] = GSM1_SOF;
956   | 		len = gsm_stuff_frame(msg->data, gsm->txframe + 1, msg->len);
957   | 		gsm->txframe[len + 1] = GSM1_SOF;
958   | 		len += 2;
959   | 	}
960   |  
961   |  if (debug & DBG_DATA)
962   | 		gsm_hex_dump_bytes(__func__, gsm->txframe, len);
963   | 	gsm_print_packet("-->", msg->addr, gsm->initiator, msg->ctrl, msg->data,
964   | 			 msg->len);
965   |  
966   | 	ret = gsmld_output(gsm, gsm->txframe, len);
967   |  if (ret <= 0)
968   |  return ret;
969   |  /* FIXME: Can eliminate one SOF in many more cases */
970   | 	gsm->tx_bytes -= msg->len;
971   |  
972   |  return 0;
973   | }
974   |  
975   | /**
976   |  *	gsm_is_flow_ctrl_msg	-	checks if flow control message
977   |  *	@msg: message to check
978   |  *
979   |  *	Returns true if the given message is a flow control command of the
980   |  *	control channel. False is returned in any other case.
981   |  */
982   | static bool gsm_is_flow_ctrl_msg(struct gsm_msg *msg)
983   | {
984   |  unsigned int cmd;
985   |  
986   |  if (msg->addr > 0)
987   |  return false;
988   |  
989   |  switch (msg->ctrl & ~PF) {
990   |  case UI:
991   |  case UIH:
992   | 		cmd = 0;
993   |  if (gsm_read_ea_val(&cmd, msg->data + 2, msg->len - 2) < 1)
994   |  break;
995   |  switch (cmd & ~PF) {
996   |  case CMD_FCOFF:
997   |  case CMD_FCON:
998   |  return true;
999   | 		}
1000  |  break;
1001  | 	}
1002  |  
1003  |  return false;
1004  | }
1005  |  
1006  | /**
1007  |  *	gsm_data_kick	-	poke the queue
1008  |  *	@gsm: GSM Mux
1009  |  *
1010  |  *	The tty device has called us to indicate that room has appeared in
1011  |  *	the transmit queue. Ram more data into the pipe if we have any.
1012  |  *	If we have been flow-stopped by a CMD_FCOFF, then we can only
1013  |  *	send messages on DLCI0 until CMD_FCON. The caller must hold
1014  |  *	the gsm tx lock.
1015  |  */
1016  | static int gsm_data_kick(struct gsm_mux *gsm)
1017  | {
1018  |  struct gsm_msg *msg, *nmsg;
1019  |  struct gsm_dlci *dlci;
1020  |  int ret;
1021  |  
1022  |  clear_bit(TTY_DO_WRITE_WAKEUP, &gsm->tty->flags);
1023  |  
1024  |  /* Serialize control messages and control channel messages first */
1025  |  list_for_each_entry_safe(msg, nmsg, &gsm->tx_ctrl_list, list) {
1026  |  if (gsm->constipated && !gsm_is_flow_ctrl_msg(msg))
    6←Assuming field 'constipated' is true→
    7←Taking false branch→
1027  |  continue;
1028  |  ret = gsm_send_packet(gsm, msg);
1029  |  switch (ret) {
    8←Control jumps to the 'default' case at line 1038→
1030  |  case -ENOSPC:
1031  |  return -ENOSPC;
1032  |  case -ENODEV:
1033  |  /* ldisc not open */
1034  | 			gsm->tx_bytes -= msg->len;
1035  | 			list_del(&msg->list);
1036  | 			kfree(msg);
1037  |  continue;
1038  |  default:
1039  |  if (ret >= 0) {
    9←Assuming 'ret' is >= 0→
    10←Taking true branch→
1040  |  list_del(&msg->list);
1041  |  kfree(msg);
    11←Freeing tx_* list entries without holding tx_lock (possible UAF)
1042  | 			}
1043  |  break;
1044  | 		}
1045  | 	}
1046  |  
1047  |  if (gsm->constipated)
1048  |  return -EAGAIN;
1049  |  
1050  |  /* Serialize other channels */
1051  |  if (list_empty(&gsm->tx_data_list))
1052  |  return 0;
1053  |  list_for_each_entry_safe(msg, nmsg, &gsm->tx_data_list, list) {
1054  | 		dlci = gsm->dlci[msg->addr];
1055  |  /* Send only messages for DLCIs with valid state */
1056  |  if (dlci->state != DLCI_OPEN) {
1057  | 			gsm->tx_bytes -= msg->len;
1058  | 			list_del(&msg->list);
1059  | 			kfree(msg);
1060  |  continue;
1061  | 		}
1062  | 		ret = gsm_send_packet(gsm, msg);
1063  |  switch (ret) {
1064  |  case -ENOSPC:
1065  |  return -ENOSPC;
1066  |  case -ENODEV:
1067  |  /* ldisc not open */
1068  | 			gsm->tx_bytes -= msg->len;
1069  | 			list_del(&msg->list);
1070  | 			kfree(msg);
1071  |  continue;
3490  |  if (ret)
3491  |  return ret;
3492  |  if (gsm->initiator)
3493  | 			gsm_dlci_begin_open(gsm->dlci[0]);
3494  | 	}
3495  |  
3496  |  return 0;
3497  | }
3498  |  
3499  | /**
3500  |  *	gsmld_output		-	write to link
3501  |  *	@gsm: our mux
3502  |  *	@data: bytes to output
3503  |  *	@len: size
3504  |  *
3505  |  *	Write a block of data from the GSM mux to the data channel. This
3506  |  *	will eventually be serialized from above but at the moment isn't.
3507  |  */
3508  |  
3509  | static int gsmld_output(struct gsm_mux *gsm, u8 *data, int len)
3510  | {
3511  |  if (tty_write_room(gsm->tty) < len) {
3512  | 		set_bit(TTY_DO_WRITE_WAKEUP, &gsm->tty->flags);
3513  |  return -ENOSPC;
3514  | 	}
3515  |  if (debug & DBG_DATA)
3516  | 		gsm_hex_dump_bytes(__func__, data, len);
3517  |  return gsm->tty->ops->write(gsm->tty, data, len);
3518  | }
3519  |  
3520  |  
3521  | /**
3522  |  *	gsmld_write_trigger	-	schedule ldisc write task
3523  |  *	@gsm: our mux
3524  |  */
3525  | static void gsmld_write_trigger(struct gsm_mux *gsm)
3526  | {
3527  |  if (!gsm || !gsm->dlci[0] || gsm->dlci[0]->dead)
3528  |  return;
3529  | 	schedule_work(&gsm->tx_work);
3530  | }
3531  |  
3532  |  
3533  | /**
3534  |  *	gsmld_write_task	-	ldisc write task
3535  |  *	@work: our tx write work
3536  |  *
3537  |  *	Writes out data to the ldisc if possible. We are doing this here to
3538  |  *	avoid dead-locking. This returns if no space or data is left for output.
3539  |  */
3540  | static void gsmld_write_task(struct work_struct *work)
3541  | {
3542  |  struct gsm_mux *gsm = container_of(work, struct gsm_mux, tx_work);
3543  |  unsigned long flags;
3544  |  int i, ret;
3545  |  
3546  |  /* All outstanding control channel and control messages and one data
3547  |  * frame is sent.
3548  |  */
3549  |  ret = -ENODEV;
3550  |  spin_lock_irqsave(&gsm->tx_lock, flags);
    1Loop condition is false.  Exiting loop→
    2←Loop condition is false.  Exiting loop→
3551  |  if (gsm->tty)
    3←Assuming field 'tty' is non-null→
    4←Taking true branch→
3552  |  ret = gsm_data_kick(gsm);
    5←Calling 'gsm_data_kick'→
3553  | 	spin_unlock_irqrestore(&gsm->tx_lock, flags);
3554  |  
3555  |  if (ret >= 0)
3556  |  for (i = 0; i < NUM_DLCI; i++)
3557  |  if (gsm->dlci[i])
3558  | 				tty_port_tty_wakeup(&gsm->dlci[i]->port);
3559  | }
3560  |  
3561  | /**
3562  |  *	gsmld_attach_gsm	-	mode set up
3563  |  *	@tty: our tty structure
3564  |  *	@gsm: our mux
3565  |  *
3566  |  *	Set up the MUX for basic mode and commence connecting to the
3567  |  *	modem. Currently called from the line discipline set up but
3568  |  *	will need moving to an ioctl path.
3569  |  */
3570  |  
3571  | static void gsmld_attach_gsm(struct tty_struct *tty, struct gsm_mux *gsm)
3572  | {
3573  | 	gsm->tty = tty_kref_get(tty);
3574  |  /* Turn off tty XON/XOFF handling to handle it explicitly. */
3575  | 	gsm->old_c_iflag = tty->termios.c_iflag;
3576  | 	tty->termios.c_iflag &= (IXON | IXOFF);
3577  | }
3578  |  
3579  | /**
3580  |  *	gsmld_detach_gsm	-	stop doing 0710 mux
3581  |  *	@tty: tty attached to the mux
3582  |  *	@gsm: mux

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
