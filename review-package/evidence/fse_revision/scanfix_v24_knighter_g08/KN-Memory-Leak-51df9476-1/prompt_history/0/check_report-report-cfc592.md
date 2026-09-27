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

octeontx2-pf: fix netdev memory leak in rvu_rep_create()

When rvu_rep_devlink_port_register() fails, free_netdev(ndev) for this
incomplete iteration before going to "exit:" label.

Fixes: 9ed0343f561e ("octeontx2-pf: Add devlink port support")
Reviewed-by: Przemek Kitszel <przemyslaw.kitszel@intel.com>
Signed-off-by: Harshit Mogalapalli <harshit.m.mogalapalli@oracle.com>
Link: https://patch.msgid.link/20241217052326.1086191-1-harshit.m.mogalapalli@oracle.com
Signed-off-by: Jakub Kicinski <kuba@kernel.org>

## Buggy Code

```c
// Function: rvu_rep_create in drivers/net/ethernet/marvell/octeontx2/nic/rep.c
int rvu_rep_create(struct otx2_nic *priv, struct netlink_ext_ack *extack)
{
	int rep_cnt = priv->rep_cnt;
	struct net_device *ndev;
	struct rep_dev *rep;
	int rep_id, err;
	u16 pcifunc;

	err = rvu_rep_rsrc_init(priv);
	if (err)
		return -ENOMEM;

	priv->reps = kcalloc(rep_cnt, sizeof(struct rep_dev *), GFP_KERNEL);
	if (!priv->reps)
		return -ENOMEM;

	for (rep_id = 0; rep_id < rep_cnt; rep_id++) {
		ndev = alloc_etherdev(sizeof(*rep));
		if (!ndev) {
			NL_SET_ERR_MSG_FMT_MOD(extack,
					       "PFVF representor:%d creation failed",
					       rep_id);
			err = -ENOMEM;
			goto exit;
		}

		rep = netdev_priv(ndev);
		priv->reps[rep_id] = rep;
		rep->mdev = priv;
		rep->netdev = ndev;
		rep->rep_id = rep_id;

		ndev->min_mtu = OTX2_MIN_MTU;
		ndev->max_mtu = priv->hw.max_mtu;
		ndev->netdev_ops = &rvu_rep_netdev_ops;
		pcifunc = priv->rep_pf_map[rep_id];
		rep->pcifunc = pcifunc;

		snprintf(ndev->name, sizeof(ndev->name), "Rpf%dvf%d",
			 rvu_get_pf(pcifunc), (pcifunc & RVU_PFVF_FUNC_MASK));

		ndev->hw_features = (NETIF_F_RXCSUM | NETIF_F_IP_CSUM |
			       NETIF_F_IPV6_CSUM | NETIF_F_RXHASH |
			       NETIF_F_SG | NETIF_F_TSO | NETIF_F_TSO6);

		ndev->hw_features |= NETIF_F_HW_TC;
		ndev->features |= ndev->hw_features;
		eth_hw_addr_random(ndev);
		err = rvu_rep_devlink_port_register(rep);
		if (err)
			goto exit;

		SET_NETDEV_DEVLINK_PORT(ndev, &rep->dl_port);
		err = register_netdev(ndev);
		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "PFVF representor registration failed");
			free_netdev(ndev);
			goto exit;
		}

		INIT_DELAYED_WORK(&rep->stats_wrk, rvu_rep_get_stats);
	}
	err = rvu_rep_napi_init(priv, extack);
	if (err)
		goto exit;

	rvu_eswitch_config(priv, true);
	return 0;
exit:
	while (--rep_id >= 0) {
		rep = priv->reps[rep_id];
		unregister_netdev(rep->netdev);
		rvu_rep_devlink_port_unregister(rep);
		free_netdev(rep->netdev);
	}
	kfree(priv->reps);
	rvu_rep_rsrc_free(priv);
	return err;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/net/ethernet/marvell/octeontx2/nic/rep.c b/drivers/net/ethernet/marvell/octeontx2/nic/rep.c
index 232b10740c13..9e3fcbae5dee 100644
--- a/drivers/net/ethernet/marvell/octeontx2/nic/rep.c
+++ b/drivers/net/ethernet/marvell/octeontx2/nic/rep.c
@@ -680,8 +680,10 @@ int rvu_rep_create(struct otx2_nic *priv, struct netlink_ext_ack *extack)
 		ndev->features |= ndev->hw_features;
 		eth_hw_addr_random(ndev);
 		err = rvu_rep_devlink_port_register(rep);
-		if (err)
+		if (err) {
+			free_netdev(ndev);
 			goto exit;
+		}

 		SET_NETDEV_DEVLINK_PORT(ndev, &rep->dl_port);
 		err = register_netdev(ndev);
```


## Bug Pattern

In a loop that creates multiple net_devices, an error occurs after alloc_etherdev() but before the device is fully registered/owned (e.g., devlink port registration fails). The code jumps to a common exit handler whose cleanup loop frees only previously created items using a pre-decrement index (while (--idx >= 0) ...), which skips the current iteration. Because the current net_device is not explicitly freed before goto exit, it leaks.


# Report

BuildSource:| drivers/net/ethernet/marvell/octeontx2/nic/rep.c
### Report Summary

File:| rep.c  
---|---  
Warning:| line 653, column 4  
Missing free_netdev before goto exit; leaks current net_device  
  
### Annotated Source Code


534   | 		otx2_write64(priv, NIX_LF_CINTX_INT(qidx), BIT_ULL(0));
535   | 		otx2_write64(priv, NIX_LF_CINTX_ENA_W1S(qidx), BIT_ULL(0));
536   | 	}
537   | 	priv->flags &= ~OTX2_FLAG_INTF_DOWN;
538   |  return 0;
539   |  
540   | err_free_cints:
541   | 	otx2_free_cints(priv, qidx);
542   | 	otx2_disable_napi(priv);
543   |  return err;
544   | }
545   |  
546   | static void rvu_rep_free_cq_rsrc(struct otx2_nic *priv)
547   | {
548   |  struct otx2_qset *qset = &priv->qset;
549   |  struct otx2_cq_poll *cq_poll = NULL;
550   |  int qidx, vec;
551   |  
552   |  /* Cleanup CQ NAPI and IRQ */
553   | 	vec = priv->hw.nix_msixoff + NIX_LF_CINT_VEC_START;
554   |  for (qidx = 0; qidx < priv->hw.cint_cnt; qidx++) {
555   |  /* Disable interrupt */
556   | 		otx2_write64(priv, NIX_LF_CINTX_ENA_W1C(qidx), BIT_ULL(0));
557   |  
558   | 		synchronize_irq(pci_irq_vector(priv->pdev, vec));
559   |  
560   | 		cq_poll = &qset->napi[qidx];
561   | 		napi_synchronize(&cq_poll->napi);
562   | 		vec++;
563   | 	}
564   | 	otx2_free_cints(priv, priv->hw.cint_cnt);
565   | 	otx2_disable_napi(priv);
566   | }
567   |  
568   | static void rvu_rep_rsrc_free(struct otx2_nic *priv)
569   | {
570   |  struct otx2_qset *qset = &priv->qset;
571   |  struct delayed_work *work;
572   |  int wrk;
573   |  
574   |  for (wrk = 0; wrk < priv->qset.cq_cnt; wrk++) {
575   | 		work = &priv->refill_wrk[wrk].pool_refill_work;
576   | 		cancel_delayed_work_sync(work);
577   | 	}
578   | 	devm_kfree(priv->dev, priv->refill_wrk);
579   |  
580   | 	otx2_free_hw_resources(priv);
581   | 	otx2_free_queue_mem(qset);
582   | }
583   |  
584   | static int rvu_rep_rsrc_init(struct otx2_nic *priv)
585   | {
586   |  struct otx2_qset *qset = &priv->qset;
587   |  int err;
588   |  
589   | 	err = otx2_alloc_queue_mem(priv);
590   |  if (err)
591   |  return err;
592   |  
593   | 	priv->hw.max_mtu = otx2_get_max_mtu(priv);
594   | 	priv->tx_max_pktlen = priv->hw.max_mtu + OTX2_ETH_HLEN;
595   | 	priv->rbsize = ALIGN(priv->hw.rbuf_len, OTX2_ALIGN) + OTX2_HEAD_ROOM;
596   |  
597   | 	err = otx2_init_hw_resources(priv);
598   |  if (err)
599   |  goto err_free_rsrc;
600   |  
601   |  /* Set maximum frame size allowed in HW */
602   | 	err = otx2_hw_set_mtu(priv, priv->hw.max_mtu);
603   |  if (err) {
604   |  dev_err(priv->dev, "Failed to set HW MTU\n");
605   |  goto err_free_rsrc;
606   | 	}
607   |  return 0;
608   |  
609   | err_free_rsrc:
610   | 	otx2_free_hw_resources(priv);
611   | 	otx2_free_queue_mem(qset);
612   |  return err;
613   | }
614   |  
615   | void rvu_rep_destroy(struct otx2_nic *priv)
616   | {
617   |  struct rep_dev *rep;
618   |  int rep_id;
619   |  
620   | 	rvu_eswitch_config(priv, false);
621   | 	priv->flags |= OTX2_FLAG_INTF_DOWN;
622   | 	rvu_rep_free_cq_rsrc(priv);
623   |  for (rep_id = 0; rep_id < priv->rep_cnt; rep_id++) {
624   | 		rep = priv->reps[rep_id];
625   | 		unregister_netdev(rep->netdev);
626   | 		rvu_rep_devlink_port_unregister(rep);
627   | 		free_netdev(rep->netdev);
628   | 		kfree(rep->flow_cfg);
629   | 	}
630   | 	kfree(priv->reps);
631   | 	rvu_rep_rsrc_free(priv);
632   | }
633   |  
634   | int rvu_rep_create(struct otx2_nic *priv, struct netlink_ext_ack *extack)
635   | {
636   |  int rep_cnt = priv->rep_cnt;
637   |  struct net_device *ndev;
638   |  struct rep_dev *rep;
639   |  int rep_id, err;
640   | 	u16 pcifunc;
641   |  
642   | 	err = rvu_rep_rsrc_init(priv);
643   |  if (err0.1'err' is 0)
    1Taking false branch→
644   |  return -ENOMEM;
645   |  
646   |  priv->reps = kcalloc(rep_cnt, sizeof(struct rep_dev *), GFP_KERNEL);
    2←Loop condition is false.  Exiting loop→
647   |  if (!priv->reps)
    3←Assuming field 'reps' is non-null→
    4←Taking false branch→
648   |  return -ENOMEM;
649   |  
650   |  for (rep_id = 0; rep_id < rep_cnt; rep_id++) {
    5←Assuming 'rep_id' is < 'rep_cnt'→
    6←Loop condition is true.  Entering loop body→
651   |  ndev = alloc_etherdev(sizeof(*rep));
652   |  if (!ndev7.1'ndev' is null) {
    7←Assuming 'ndev' is null→
    8←Taking true branch→
653   |  NL_SET_ERR_MSG_FMT_MOD(extack,
    9←Assuming '__extack' is non-null→
    10←Taking false branch→
    11←Assuming the condition is false→
    12←Taking false branch→
    13←Missing free_netdev before goto exit; leaks current net_device
654   |  "PFVF representor:%d creation failed",
655   |  rep_id);
656   |  err = -ENOMEM;
657   |  goto exit;
658   |  }
659   |  
660   | 		rep = netdev_priv(ndev);
661   | 		priv->reps[rep_id] = rep;
662   | 		rep->mdev = priv;
663   | 		rep->netdev = ndev;
664   | 		rep->rep_id = rep_id;
665   |  
666   | 		ndev->min_mtu = OTX2_MIN_MTU;
667   | 		ndev->max_mtu = priv->hw.max_mtu;
668   | 		ndev->netdev_ops = &rvu_rep_netdev_ops;
669   | 		pcifunc = priv->rep_pf_map[rep_id];
670   | 		rep->pcifunc = pcifunc;
671   |  
672   | 		snprintf(ndev->name, sizeof(ndev->name), "Rpf%dvf%d",
673   | 			 rvu_get_pf(pcifunc), (pcifunc & RVU_PFVF_FUNC_MASK));
674   |  
675   | 		ndev->hw_features = (NETIF_F_RXCSUM | NETIF_F_IP_CSUM |
676   |  NETIF_F_IPV6_CSUM | NETIF_F_RXHASH |
677   |  NETIF_F_SG | NETIF_F_TSO | NETIF_F_TSO6);
678   |  
679   | 		ndev->hw_features |= NETIF_F_HW_TC;
680   | 		ndev->features |= ndev->hw_features;
681   | 		eth_hw_addr_random(ndev);
682   | 		err = rvu_rep_devlink_port_register(rep);
683   |  if (err) {

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
