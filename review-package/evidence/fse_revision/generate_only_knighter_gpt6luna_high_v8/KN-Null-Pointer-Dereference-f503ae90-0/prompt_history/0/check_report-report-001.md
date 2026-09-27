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

wifi: mt76: mt7996: fix NULL pointer dereference in mt7996_mcu_sta_bfer_he

Fix the NULL pointer dereference in mt7996_mcu_sta_bfer_he
routine adding an sta interface to the mt7996 driver.

Found by code review.

Cc: stable@vger.kernel.org
Fixes: 98686cd21624 ("wifi: mt76: mt7996: add driver for MediaTek Wi-Fi 7 (802.11be) devices")
Signed-off-by: Ma Ke <make24@iscas.ac.cn>
Link: https://patch.msgid.link/20240813081242.3991814-1-make24@iscas.ac.cn
Signed-off-by: Felix Fietkau <nbd@nbd.name>

## Buggy Code

```c
// Function: mt7996_mcu_sta_bfer_he in drivers/net/wireless/mediatek/mt76/mt7996/mcu.c
static void
mt7996_mcu_sta_bfer_he(struct ieee80211_sta *sta, struct ieee80211_vif *vif,
		       struct mt7996_phy *phy, struct sta_rec_bf *bf)
{
	struct ieee80211_sta_he_cap *pc = &sta->deflink.he_cap;
	struct ieee80211_he_cap_elem *pe = &pc->he_cap_elem;
	const struct ieee80211_sta_he_cap *vc =
		mt76_connac_get_he_phy_cap(phy->mt76, vif);
	const struct ieee80211_he_cap_elem *ve = &vc->he_cap_elem;
	u16 mcs_map = le16_to_cpu(pc->he_mcs_nss_supp.rx_mcs_80);
	u8 nss_mcs = mt7996_mcu_get_sta_nss(mcs_map);
	u8 snd_dim, sts;

	bf->tx_mode = MT_PHY_TYPE_HE_SU;

	mt7996_mcu_sta_sounding_rate(bf);

	bf->trigger_su = HE_PHY(CAP6_TRIG_SU_BEAMFORMING_FB,
				pe->phy_cap_info[6]);
	bf->trigger_mu = HE_PHY(CAP6_TRIG_MU_BEAMFORMING_PARTIAL_BW_FB,
				pe->phy_cap_info[6]);
	snd_dim = HE_PHY(CAP5_BEAMFORMEE_NUM_SND_DIM_UNDER_80MHZ_MASK,
			 ve->phy_cap_info[5]);
	sts = HE_PHY(CAP4_BEAMFORMEE_MAX_STS_UNDER_80MHZ_MASK,
		     pe->phy_cap_info[4]);
	bf->nrow = min_t(u8, snd_dim, sts);
	bf->ncol = min_t(u8, nss_mcs, bf->nrow);
	bf->ibf_ncol = bf->ncol;

	if (sta->deflink.bandwidth != IEEE80211_STA_RX_BW_160)
		return;

	/* go over for 160MHz and 80p80 */
	if (pe->phy_cap_info[0] &
	    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G) {
		mcs_map = le16_to_cpu(pc->he_mcs_nss_supp.rx_mcs_160);
		nss_mcs = mt7996_mcu_get_sta_nss(mcs_map);

		bf->ncol_gt_bw80 = nss_mcs;
	}

	if (pe->phy_cap_info[0] &
	    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G) {
		mcs_map = le16_to_cpu(pc->he_mcs_nss_supp.rx_mcs_80p80);
		nss_mcs = mt7996_mcu_get_sta_nss(mcs_map);

		if (bf->ncol_gt_bw80)
			bf->ncol_gt_bw80 = min_t(u8, bf->ncol_gt_bw80, nss_mcs);
		else
			bf->ncol_gt_bw80 = nss_mcs;
	}

	snd_dim = HE_PHY(CAP5_BEAMFORMEE_NUM_SND_DIM_ABOVE_80MHZ_MASK,
			 ve->phy_cap_info[5]);
	sts = HE_PHY(CAP4_BEAMFORMEE_MAX_STS_ABOVE_80MHZ_MASK,
		     pe->phy_cap_info[4]);

	bf->nrow_gt_bw80 = min_t(int, snd_dim, sts);
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/net/wireless/mediatek/mt76/mt7996/mcu.c b/drivers/net/wireless/mediatek/mt76/mt7996/mcu.c
index e8d34bfbb41a..8855095fef10 100644
--- a/drivers/net/wireless/mediatek/mt76/mt7996/mcu.c
+++ b/drivers/net/wireless/mediatek/mt76/mt7996/mcu.c
@@ -1544,6 +1544,9 @@ mt7996_mcu_sta_bfer_he(struct ieee80211_sta *sta, struct ieee80211_vif *vif,
 	u8 nss_mcs = mt7996_mcu_get_sta_nss(mcs_map);
 	u8 snd_dim, sts;

+	if (!vc)
+		return;
+
 	bf->tx_mode = MT_PHY_TYPE_HE_SU;

 	mt7996_mcu_sta_sounding_rate(bf);
```


## Bug Pattern

Dereferencing a pointer returned by a capability-retrieval helper without a NULL check. Specifically:
- A helper like mt76_connac_get_he_phy_cap(...) can return NULL when the capability is unsupported.
- The code immediately accesses a member of this pointer (e.g., const struct ieee80211_he_cap_elem *ve = &vc->he_cap_elem; and ve->phy_cap_info[...]) before verifying vc != NULL.
- This leads to a NULL pointer dereference when the capability is absent.


# Report

BuildSource:| drivers/net/wireless/mediatek/mt76/mt7996/mcu.c
### Report Summary

File:| mcu.c  
---|---  
Warning:| line 1542, column 2  
Dereference of pointer returned by mt76_connac_get_he_phy_cap without NULL
check  
  
### Annotated Source Code


1363  |  struct sta_rec_muru *muru;
1364  |  struct tlv *tlv;
1365  |  
1366  |  if (vif->type != NL80211_IFTYPE_STATION &&
1367  | 	    vif->type != NL80211_IFTYPE_AP)
1368  |  return;
1369  |  
1370  | 	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_MURU, sizeof(*muru));
1371  |  
1372  | 	muru = (struct sta_rec_muru *)tlv;
1373  | 	muru->cfg.mimo_dl_en = vif->bss_conf.eht_mu_beamformer ||
1374  | 			       vif->bss_conf.he_mu_beamformer ||
1375  | 			       vif->bss_conf.vht_mu_beamformer ||
1376  | 			       vif->bss_conf.vht_mu_beamformee;
1377  | 	muru->cfg.ofdma_dl_en = true;
1378  |  
1379  |  if (sta->deflink.vht_cap.vht_supported)
1380  | 		muru->mimo_dl.vht_mu_bfee =
1381  | 			!!(sta->deflink.vht_cap.cap & IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE);
1382  |  
1383  |  if (!sta->deflink.he_cap.has_he)
1384  |  return;
1385  |  
1386  | 	muru->mimo_dl.partial_bw_dl_mimo =
1387  |  HE_PHY(CAP6_PARTIAL_BANDWIDTH_DL_MUMIMO, elem->phy_cap_info[6]);
1388  |  
1389  | 	muru->mimo_ul.full_ul_mimo =
1390  |  HE_PHY(CAP2_UL_MU_FULL_MU_MIMO, elem->phy_cap_info[2]);
1391  | 	muru->mimo_ul.partial_ul_mimo =
1392  |  HE_PHY(CAP2_UL_MU_PARTIAL_MU_MIMO, elem->phy_cap_info[2]);
1393  |  
1394  | 	muru->ofdma_dl.punc_pream_rx =
1395  |  HE_PHY(CAP1_PREAMBLE_PUNC_RX_MASK, elem->phy_cap_info[1]);
1396  | 	muru->ofdma_dl.he_20m_in_40m_2g =
1397  |  HE_PHY(CAP8_20MHZ_IN_40MHZ_HE_PPDU_IN_2G, elem->phy_cap_info[8]);
1398  | 	muru->ofdma_dl.he_20m_in_160m =
1399  |  HE_PHY(CAP8_20MHZ_IN_160MHZ_HE_PPDU, elem->phy_cap_info[8]);
1400  | 	muru->ofdma_dl.he_80m_in_160m =
1401  |  HE_PHY(CAP8_80MHZ_IN_160MHZ_HE_PPDU, elem->phy_cap_info[8]);
1402  |  
1403  | 	muru->ofdma_ul.t_frame_dur =
1404  |  HE_MAC(CAP1_TF_MAC_PAD_DUR_MASK, elem->mac_cap_info[1]);
1405  | 	muru->ofdma_ul.mu_cascading =
1406  |  HE_MAC(CAP2_MU_CASCADING, elem->mac_cap_info[2]);
1407  | 	muru->ofdma_ul.uo_ra =
1408  |  HE_MAC(CAP3_OFDMA_RA, elem->mac_cap_info[3]);
1409  | 	muru->ofdma_ul.rx_ctrl_frame_to_mbss =
1410  |  HE_MAC(CAP3_RX_CTRL_FRAME_TO_MULTIBSS, elem->mac_cap_info[3]);
1411  | }
1412  |  
1413  | static inline bool
1414  | mt7996_is_ebf_supported(struct mt7996_phy *phy, struct ieee80211_vif *vif,
1415  |  struct ieee80211_sta *sta, bool bfee)
1416  | {
1417  |  int sts = hweight16(phy->mt76->chainmask);
1418  |  
1419  |  if (vif->type != NL80211_IFTYPE_STATION &&
1420  | 	    vif->type != NL80211_IFTYPE_AP)
1421  |  return false;
1422  |  
1423  |  if (!bfee && sts < 2)
1424  |  return false;
1425  |  
1426  |  if (sta->deflink.eht_cap.has_eht) {
1427  |  struct ieee80211_sta_eht_cap *pc = &sta->deflink.eht_cap;
1428  |  struct ieee80211_eht_cap_elem_fixed *pe = &pc->eht_cap_elem;
1429  |  
1430  |  if (bfee)
1431  |  return vif->bss_conf.eht_su_beamformee &&
1432  |  EHT_PHY(CAP0_SU_BEAMFORMER, pe->phy_cap_info[0]);
1433  |  else
1434  |  return vif->bss_conf.eht_su_beamformer &&
1435  |  EHT_PHY(CAP0_SU_BEAMFORMEE, pe->phy_cap_info[0]);
1436  | 	}
1437  |  
1438  |  if (sta->deflink.he_cap.has_he) {
1439  |  struct ieee80211_he_cap_elem *pe = &sta->deflink.he_cap.he_cap_elem;
1440  |  
1441  |  if (bfee)
1442  |  return vif->bss_conf.he_su_beamformee &&
1443  |  HE_PHY(CAP3_SU_BEAMFORMER, pe->phy_cap_info[3]);
1444  |  else
1445  |  return vif->bss_conf.he_su_beamformer &&
1446  |  HE_PHY(CAP4_SU_BEAMFORMEE, pe->phy_cap_info[4]);
1447  | 	}
1448  |  
1449  |  if (sta->deflink.vht_cap.vht_supported) {
1450  | 		u32 cap = sta->deflink.vht_cap.cap;
1451  |  
1452  |  if (bfee)
1453  |  return vif->bss_conf.vht_su_beamformee &&
1454  | 			       (cap & IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE);
1455  |  else
1456  |  return vif->bss_conf.vht_su_beamformer &&
1457  | 			       (cap & IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE);
1458  | 	}
1459  |  
1460  |  return false;
1461  | }
1462  |  
1463  | static void
1464  | mt7996_mcu_sta_sounding_rate(struct sta_rec_bf *bf)
1465  | {
1466  | 	bf->sounding_phy = MT_PHY_TYPE_OFDM;
1467  | 	bf->ndp_rate = 0;				/* mcs0 */
1468  | 	bf->ndpa_rate = MT7996_CFEND_RATE_DEFAULT;	/* ofdm 24m */
1469  | 	bf->rept_poll_rate = MT7996_CFEND_RATE_DEFAULT;	/* ofdm 24m */
1470  | }
1471  |  
1472  | static void
1473  | mt7996_mcu_sta_bfer_ht(struct ieee80211_sta *sta, struct mt7996_phy *phy,
1474  |  struct sta_rec_bf *bf)
1475  | {
1476  |  struct ieee80211_mcs_info *mcs = &sta->deflink.ht_cap.mcs;
1484  |  mcs->tx_params);
1485  |  else if (mcs->rx_mask[3])
1486  | 		n = 3;
1487  |  else if (mcs->rx_mask[2])
1488  | 		n = 2;
1489  |  else if (mcs->rx_mask[1])
1490  | 		n = 1;
1491  |  
1492  | 	bf->nrow = hweight8(phy->mt76->antenna_mask) - 1;
1493  | 	bf->ncol = min_t(u8, bf->nrow, n);
1494  | 	bf->ibf_ncol = n;
1495  | }
1496  |  
1497  | static void
1498  | mt7996_mcu_sta_bfer_vht(struct ieee80211_sta *sta, struct mt7996_phy *phy,
1499  |  struct sta_rec_bf *bf, bool explicit)
1500  | {
1501  |  struct ieee80211_sta_vht_cap *pc = &sta->deflink.vht_cap;
1502  |  struct ieee80211_sta_vht_cap *vc = &phy->mt76->sband_5g.sband.vht_cap;
1503  | 	u16 mcs_map = le16_to_cpu(pc->vht_mcs.rx_mcs_map);
1504  | 	u8 nss_mcs = mt7996_mcu_get_sta_nss(mcs_map);
1505  | 	u8 tx_ant = hweight8(phy->mt76->antenna_mask) - 1;
1506  |  
1507  | 	bf->tx_mode = MT_PHY_TYPE_VHT;
1508  |  
1509  |  if (explicit) {
1510  | 		u8 sts, snd_dim;
1511  |  
1512  | 		mt7996_mcu_sta_sounding_rate(bf);
1513  |  
1514  | 		sts = FIELD_GET(IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK,
1515  |  pc->cap);
1516  | 		snd_dim = FIELD_GET(IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_MASK,
1517  |  vc->cap);
1518  | 		bf->nrow = min_t(u8, min_t(u8, snd_dim, sts), tx_ant);
1519  | 		bf->ncol = min_t(u8, nss_mcs, bf->nrow);
1520  | 		bf->ibf_ncol = bf->ncol;
1521  |  
1522  |  if (sta->deflink.bandwidth == IEEE80211_STA_RX_BW_160)
1523  | 			bf->nrow = 1;
1524  | 	} else {
1525  | 		bf->nrow = tx_ant;
1526  | 		bf->ncol = min_t(u8, nss_mcs, bf->nrow);
1527  | 		bf->ibf_ncol = nss_mcs;
1528  |  
1529  |  if (sta->deflink.bandwidth == IEEE80211_STA_RX_BW_160)
1530  | 			bf->ibf_nrow = 1;
1531  | 	}
1532  | }
1533  |  
1534  | static void
1535  | mt7996_mcu_sta_bfer_he(struct ieee80211_sta *sta, struct ieee80211_vif *vif,
1536  |  struct mt7996_phy *phy, struct sta_rec_bf *bf)
1537  | {
1538  |  struct ieee80211_sta_he_cap *pc = &sta->deflink.he_cap;
1539  |  struct ieee80211_he_cap_elem *pe = &pc->he_cap_elem;
1540  |  const struct ieee80211_sta_he_cap *vc =
1541  | 		mt76_connac_get_he_phy_cap(phy->mt76, vif);
1542  |  const struct ieee80211_he_cap_elem *ve = &vc->he_cap_elem;
    15←Dereference of pointer returned by mt76_connac_get_he_phy_cap without NULL check
1543  | 	u16 mcs_map = le16_to_cpu(pc->he_mcs_nss_supp.rx_mcs_80);
1544  | 	u8 nss_mcs = mt7996_mcu_get_sta_nss(mcs_map);
1545  | 	u8 snd_dim, sts;
1546  |  
1547  |  if (!vc)
1548  |  return;
1549  |  
1550  | 	bf->tx_mode = MT_PHY_TYPE_HE_SU;
1551  |  
1552  | 	mt7996_mcu_sta_sounding_rate(bf);
1553  |  
1554  | 	bf->trigger_su = HE_PHY(CAP6_TRIG_SU_BEAMFORMING_FB,
1555  |  pe->phy_cap_info[6]);
1556  | 	bf->trigger_mu = HE_PHY(CAP6_TRIG_MU_BEAMFORMING_PARTIAL_BW_FB,
1557  |  pe->phy_cap_info[6]);
1558  | 	snd_dim = HE_PHY(CAP5_BEAMFORMEE_NUM_SND_DIM_UNDER_80MHZ_MASK,
1559  |  ve->phy_cap_info[5]);
1560  | 	sts = HE_PHY(CAP4_BEAMFORMEE_MAX_STS_UNDER_80MHZ_MASK,
1561  |  pe->phy_cap_info[4]);
1562  | 	bf->nrow = min_t(u8, snd_dim, sts);
1563  | 	bf->ncol = min_t(u8, nss_mcs, bf->nrow);
1564  | 	bf->ibf_ncol = bf->ncol;
1565  |  
1566  |  if (sta->deflink.bandwidth != IEEE80211_STA_RX_BW_160)
1567  |  return;
1568  |  
1569  |  /* go over for 160MHz and 80p80 */
1570  |  if (pe->phy_cap_info[0] &
1571  |  IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G) {
1572  | 		mcs_map = le16_to_cpu(pc->he_mcs_nss_supp.rx_mcs_160);
1603  |  struct ieee80211_eht_mcs_nss_supp *eht_nss = &pc->eht_mcs_nss_supp;
1604  |  const struct ieee80211_sta_eht_cap *vc =
1605  | 		mt76_connac_get_eht_phy_cap(phy->mt76, vif);
1606  |  const struct ieee80211_eht_cap_elem_fixed *ve = &vc->eht_cap_elem;
1607  | 	u8 nss_mcs = u8_get_bits(eht_nss->bw._80.rx_tx_mcs9_max_nss,
1608  |  IEEE80211_EHT_MCS_NSS_RX) - 1;
1609  | 	u8 snd_dim, sts;
1610  |  
1611  | 	bf->tx_mode = MT_PHY_TYPE_EHT_MU;
1612  |  
1613  | 	mt7996_mcu_sta_sounding_rate(bf);
1614  |  
1615  | 	bf->trigger_su = EHT_PHY(CAP3_TRIG_SU_BF_FDBK, pe->phy_cap_info[3]);
1616  | 	bf->trigger_mu = EHT_PHY(CAP3_TRIG_MU_BF_PART_BW_FDBK, pe->phy_cap_info[3]);
1617  | 	snd_dim = EHT_PHY(CAP2_SOUNDING_DIM_80MHZ_MASK, ve->phy_cap_info[2]);
1618  | 	sts = EHT_PHY(CAP0_BEAMFORMEE_SS_80MHZ_MASK, pe->phy_cap_info[0]) +
1619  | 	      (EHT_PHY(CAP1_BEAMFORMEE_SS_80MHZ_MASK, pe->phy_cap_info[1]) << 1);
1620  | 	bf->nrow = min_t(u8, snd_dim, sts);
1621  | 	bf->ncol = min_t(u8, nss_mcs, bf->nrow);
1622  | 	bf->ibf_ncol = bf->ncol;
1623  |  
1624  |  if (sta->deflink.bandwidth < IEEE80211_STA_RX_BW_160)
1625  |  return;
1626  |  
1627  |  switch (sta->deflink.bandwidth) {
1628  |  case IEEE80211_STA_RX_BW_160:
1629  | 		snd_dim = EHT_PHY(CAP2_SOUNDING_DIM_160MHZ_MASK, ve->phy_cap_info[2]);
1630  | 		sts = EHT_PHY(CAP1_BEAMFORMEE_SS_160MHZ_MASK, pe->phy_cap_info[1]);
1631  | 		nss_mcs = u8_get_bits(eht_nss->bw._160.rx_tx_mcs9_max_nss,
1632  |  IEEE80211_EHT_MCS_NSS_RX) - 1;
1633  |  
1634  | 		bf->nrow_gt_bw80 = min_t(u8, snd_dim, sts);
1635  | 		bf->ncol_gt_bw80 = nss_mcs;
1636  |  break;
1637  |  case IEEE80211_STA_RX_BW_320:
1638  | 		snd_dim = EHT_PHY(CAP2_SOUNDING_DIM_320MHZ_MASK, ve->phy_cap_info[2]) +
1639  | 			  (EHT_PHY(CAP3_SOUNDING_DIM_320MHZ_MASK,
1640  |  ve->phy_cap_info[3]) << 1);
1641  | 		sts = EHT_PHY(CAP1_BEAMFORMEE_SS_320MHZ_MASK, pe->phy_cap_info[1]);
1642  | 		nss_mcs = u8_get_bits(eht_nss->bw._320.rx_tx_mcs9_max_nss,
1643  |  IEEE80211_EHT_MCS_NSS_RX) - 1;
1644  |  
1645  | 		bf->nrow_gt_bw80 = min_t(u8, snd_dim, sts) << 4;
1646  | 		bf->ncol_gt_bw80 = nss_mcs << 4;
1647  |  break;
1648  |  default:
1649  |  break;
1650  | 	}
1651  | }
1652  |  
1653  | static void
1654  | mt7996_mcu_sta_bfer_tlv(struct mt7996_dev *dev, struct sk_buff *skb,
1655  |  struct ieee80211_vif *vif, struct ieee80211_sta *sta)
1656  | {
1657  |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
1658  |  struct mt7996_phy *phy = mvif->phy;
1659  |  int tx_ant = hweight16(phy->mt76->chainmask) - 1;
    9←'?' condition is false→
1660  |  struct sta_rec_bf *bf;
1661  |  struct tlv *tlv;
1662  |  static const u8 matrix[4][4] = {
1663  | 		{0, 0, 0, 0},
1664  | 		{1, 1, 0, 0},	/* 2x1, 2x2, 2x3, 2x4 */
1665  | 		{2, 4, 4, 0},	/* 3x1, 3x2, 3x3, 3x4 */
1666  | 		{3, 5, 6, 0}	/* 4x1, 4x2, 4x3, 4x4 */
1667  | 	};
1668  | 	bool ebf;
1669  |  
1670  |  if (!(sta->deflink.ht_cap.ht_supported || sta->deflink.he_cap.has_he))
    10←Assuming field 'ht_supported' is true→
    11←Taking false branch→
1671  |  return;
1672  |  
1673  |  ebf = mt7996_is_ebf_supported(phy, vif, sta, false);
1674  |  if (!ebf && !dev->ibf)
    12←Assuming 'ebf' is true→
1675  |  return;
1676  |  
1677  |  tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_BF, sizeof(*bf));
1678  |  bf = (struct sta_rec_bf *)tlv;
1679  |  
1680  |  /* he/eht: eBF only, in accordance with spec
1681  |  * vht: support eBF and iBF
1682  |  * ht: iBF only, since mac80211 lacks of eBF support
1683  |  */
1684  |  if (sta->deflink.eht_cap.has_eht12.1Field 'has_eht' is false && ebf)
1685  | 		mt7996_mcu_sta_bfer_eht(sta, vif, phy, bf);
1686  |  else if (sta->deflink.he_cap.has_he12.2Field 'has_he' is true && ebf12.3'ebf' is true)
    13←Taking true branch→
1687  |  mt7996_mcu_sta_bfer_he(sta, vif, phy, bf);
    14←Calling 'mt7996_mcu_sta_bfer_he'→
1688  |  else if (sta->deflink.vht_cap.vht_supported)
1689  | 		mt7996_mcu_sta_bfer_vht(sta, phy, bf, ebf);
1690  |  else if (sta->deflink.ht_cap.ht_supported)
1691  | 		mt7996_mcu_sta_bfer_ht(sta, phy, bf);
1692  |  else
1693  |  return;
1694  |  
1695  | 	bf->bf_cap = ebf ? ebf : dev->ibf << 1;
1696  | 	bf->bw = sta->deflink.bandwidth;
1697  | 	bf->ibf_dbw = sta->deflink.bandwidth;
1698  | 	bf->ibf_nrow = tx_ant;
1699  |  
1700  |  if (!ebf && sta->deflink.bandwidth <= IEEE80211_STA_RX_BW_40 && !bf->ncol)
1701  | 		bf->ibf_timeout = 0x48;
1702  |  else
1703  | 		bf->ibf_timeout = 0x18;
1704  |  
1705  |  if (ebf && bf->nrow != tx_ant)
1706  | 		bf->mem_20m = matrix[tx_ant][bf->ncol];
1707  |  else
1708  | 		bf->mem_20m = matrix[bf->nrow][bf->ncol];
1709  |  
1710  |  switch (sta->deflink.bandwidth) {
1711  |  case IEEE80211_STA_RX_BW_160:
1712  |  case IEEE80211_STA_RX_BW_80:
1713  | 		bf->mem_total = bf->mem_20m * 2;
1714  |  break;
1715  |  case IEEE80211_STA_RX_BW_40:
1716  | 		bf->mem_total = bf->mem_20m;
1717  |  break;
1718  |  case IEEE80211_STA_RX_BW_20:
1719  |  default:
1720  |  break;
1721  | 	}
1722  | }
1723  |  
1724  | static void
1725  | mt7996_mcu_sta_bfee_tlv(struct mt7996_dev *dev, struct sk_buff *skb,
1726  |  struct ieee80211_vif *vif, struct ieee80211_sta *sta)
1727  | {
1728  |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
1729  |  struct mt7996_phy *phy = mvif->phy;
1730  |  int tx_ant = hweight8(phy->mt76->antenna_mask) - 1;
1731  |  struct sta_rec_bfee *bfee;
1732  |  struct tlv *tlv;
1733  | 	u8 nrow = 0;
1734  |  
1735  |  if (!(sta->deflink.vht_cap.vht_supported || sta->deflink.he_cap.has_he))
1736  |  return;
1737  |  
1738  |  if (!mt7996_is_ebf_supported(phy, vif, sta, true))
1739  |  return;
1740  |  
1741  | 	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_BFEE, sizeof(*bfee));
1742  | 	bfee = (struct sta_rec_bfee *)tlv;
1743  |  
1744  |  if (sta->deflink.he_cap.has_he) {
1745  |  struct ieee80211_he_cap_elem *pe = &sta->deflink.he_cap.he_cap_elem;
1746  |  
1747  | 		nrow = HE_PHY(CAP5_BEAMFORMEE_NUM_SND_DIM_UNDER_80MHZ_MASK,
1748  |  pe->phy_cap_info[5]);
1749  | 	} else if (sta->deflink.vht_cap.vht_supported) {
1750  |  struct ieee80211_sta_vht_cap *pc = &sta->deflink.vht_cap;
1751  |  
1752  | 		nrow = FIELD_GET(IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_MASK,
1753  |  pc->cap);
1754  | 	}
1755  |  
1756  |  /* reply with identity matrix to avoid 2x2 BF negative gain */
1757  | 	bfee->fb_identity_matrix = (nrow == 1 && tx_ant == 2);
1758  | }
1759  |  
1760  | static void
1761  | mt7996_mcu_sta_tx_proc_tlv(struct sk_buff *skb)
1762  | {
1763  |  struct sta_rec_tx_proc *tx_proc;
1764  |  struct tlv *tlv;
1765  |  
1766  | 	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_TX_PROC, sizeof(*tx_proc));
1767  |  
1768  | 	tx_proc = (struct sta_rec_tx_proc *)tlv;
1769  | 	tx_proc->flag = cpu_to_le32(0);
1770  | }
1771  |  
1772  | static void
1773  | mt7996_mcu_sta_hdrt_tlv(struct mt7996_dev *dev, struct sk_buff *skb)
1774  | {
1775  |  struct sta_rec_hdrt *hdrt;
1776  |  struct tlv *tlv;
1777  |  
1778  | 	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_HDRT, sizeof(*hdrt));
1779  |  
1780  | 	hdrt = (struct sta_rec_hdrt *)tlv;
1781  | 	hdrt->hdrt_mode = 1;
1782  | }
1783  |  
1784  | static void
1785  | mt7996_mcu_sta_hdr_trans_tlv(struct mt7996_dev *dev, struct sk_buff *skb,
1786  |  struct ieee80211_vif *vif,
1787  |  struct ieee80211_sta *sta)
1788  | {
1789  |  struct sta_rec_hdr_trans *hdr_trans;
1790  |  struct mt76_wcid *wcid;
1791  |  struct tlv *tlv;
1792  |  
1793  | 	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_HDR_TRANS, sizeof(*hdr_trans));
1794  | 	hdr_trans = (struct sta_rec_hdr_trans *)tlv;
1795  | 	hdr_trans->dis_rx_hdr_tran = true;
1796  |  
1797  |  if (vif->type == NL80211_IFTYPE_STATION)
1798  | 		hdr_trans->to_ds = true;
1799  |  else
1800  | 		hdr_trans->from_ds = true;
1801  |  
1802  |  if (!sta)
1803  |  return;
1804  |  
1805  | 	wcid = (struct mt76_wcid *)sta->drv_priv;
1806  | 	hdr_trans->dis_rx_hdr_tran = !test_bit(MT_WCID_FLAG_HDR_TRANS, &wcid->flags);
1807  |  if (test_bit(MT_WCID_FLAG_4ADDR, &wcid->flags)) {
1808  | 		hdr_trans->to_ds = true;
1809  | 		hdr_trans->from_ds = true;
1810  | 	}
1811  |  
1812  |  if (vif->type == NL80211_IFTYPE_MESH_POINT) {
1813  | 		hdr_trans->to_ds = true;
1814  | 		hdr_trans->from_ds = true;
1815  | 		hdr_trans->mesh = true;
1816  | 	}
1817  | }
1818  |  
1819  | static enum mcu_mmps_mode
1820  | mt7996_mcu_get_mmps_mode(enum ieee80211_smps_mode smps)
1821  | {
1822  |  switch (smps) {
1823  |  case IEEE80211_SMPS_OFF:
1824  |  return MCU_MMPS_DISABLE;
1825  |  case IEEE80211_SMPS_STATIC:
1826  |  return MCU_MMPS_STATIC;
1827  |  case IEEE80211_SMPS_DYNAMIC:
1828  |  return MCU_MMPS_DYNAMIC;
1829  |  default:
1830  |  return MCU_MMPS_DISABLE;
1831  | 	}
1832  | }
1833  |  
1834  | int mt7996_mcu_set_fixed_rate_ctrl(struct mt7996_dev *dev,
1835  |  void *data, u16 version)
1836  | {
1837  |  struct ra_fixed_rate *req;
1838  |  struct uni_header hdr;
1839  |  struct sk_buff *skb;
1840  |  struct tlv *tlv;
1841  |  int len;
1842  |  
2109  |  * once dev->rc_work changes the settings driver should also
2110  |  * update sta_rec_he here.
2111  |  */
2112  |  if (changed)
2113  | 		mt7996_mcu_sta_he_tlv(skb, sta);
2114  |  
2115  |  /* sta_rec_ra accommodates BW, NSS and only MCS range format
2116  |  * i.e 0-{7,8,9} for VHT.
2117  |  */
2118  | 	mt7996_mcu_sta_rate_ctrl_tlv(skb, dev, vif, sta);
2119  |  
2120  | 	ret = mt76_mcu_skb_send_msg(&dev->mt76, skb,
2121  |  MCU_WMWA_UNI_CMD(STA_REC_UPDATE), true);
2122  |  if (ret)
2123  |  return ret;
2124  |  
2125  |  return mt7996_mcu_add_rate_ctrl_fixed(dev, vif, sta);
2126  | }
2127  |  
2128  | static int
2129  | mt7996_mcu_add_group(struct mt7996_dev *dev, struct ieee80211_vif *vif,
2130  |  struct ieee80211_sta *sta)
2131  | {
2132  | #define MT_STA_BSS_GROUP		1
2133  |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
2134  |  struct mt7996_sta *msta;
2135  |  struct {
2136  | 		u8 __rsv1[4];
2137  |  
2138  | 		__le16 tag;
2139  | 		__le16 len;
2140  | 		__le16 wlan_idx;
2141  | 		u8 __rsv2[2];
2142  | 		__le32 action;
2143  | 		__le32 val;
2144  | 		u8 __rsv3[8];
2145  | 	} __packed req = {
2146  | 		.tag = cpu_to_le16(UNI_VOW_DRR_CTRL),
2147  | 		.len = cpu_to_le16(sizeof(req) - 4),
2148  | 		.action = cpu_to_le32(MT_STA_BSS_GROUP),
2149  | 		.val = cpu_to_le32(mvif->mt76.idx % 16),
2150  | 	};
2151  |  
2152  | 	msta = sta ? (struct mt7996_sta *)sta->drv_priv : &mvif->sta;
2153  | 	req.wlan_idx = cpu_to_le16(msta->wcid.idx);
2154  |  
2155  |  return mt76_mcu_send_msg(&dev->mt76, MCU_WM_UNI_CMD(VOW), &req,
2156  |  sizeof(req), true);
2157  | }
2158  |  
2159  | int mt7996_mcu_add_sta(struct mt7996_dev *dev, struct ieee80211_vif *vif,
2160  |  struct ieee80211_sta *sta, bool enable, bool newly)
2161  | {
2162  |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
2163  |  struct ieee80211_link_sta *link_sta;
2164  |  struct mt7996_sta *msta;
2165  |  struct sk_buff *skb;
2166  |  int ret;
2167  |  
2168  |  msta = sta ? (struct mt7996_sta *)sta->drv_priv : &mvif->sta;
    1Assuming 'sta' is non-null→
    2←'?' condition is true→
2169  |  link_sta = sta2.1'sta' is non-null ? &sta->deflink : NULL;
    3←'?' condition is true→
2170  |  
2171  | 	skb = __mt76_connac_mcu_alloc_sta_req(&dev->mt76, &mvif->mt76,
2172  | 					      &msta->wcid,
2173  |  MT7996_STA_UPDATE_MAX_SIZE);
2174  |  if (IS_ERR(skb))
    4←Taking false branch→
2175  |  return PTR_ERR(skb);
2176  |  
2177  |  /* starec basic */
2178  |  mt76_connac_mcu_sta_basic_tlv(&dev->mt76, skb, vif, link_sta,
2179  | 				      enable, newly);
2180  |  
2181  |  if (!enable)
    5←Assuming 'enable' is true→
    6←Taking false branch→
2182  |  goto out;
2183  |  
2184  |  /* starec hdr trans */
2185  |  mt7996_mcu_sta_hdr_trans_tlv(dev, skb, vif, sta);
2186  |  /* starec tx proc */
2187  | 	mt7996_mcu_sta_tx_proc_tlv(skb);
2188  |  
2189  |  /* tag order is in accordance with firmware dependency. */
2190  |  if (sta6.1'sta' is non-null) {
    7←Taking true branch→
2191  |  /* starec hdrt mode */
2192  |  mt7996_mcu_sta_hdrt_tlv(dev, skb);
2193  |  /* starec bfer */
2194  |  mt7996_mcu_sta_bfer_tlv(dev, skb, vif, sta);
    8←Calling 'mt7996_mcu_sta_bfer_tlv'→
2195  |  /* starec ht */
2196  | 		mt7996_mcu_sta_ht_tlv(skb, sta);
2197  |  /* starec vht */
2198  | 		mt7996_mcu_sta_vht_tlv(skb, sta);
2199  |  /* starec uapsd */
2200  | 		mt76_connac_mcu_sta_uapsd(skb, vif, sta);
2201  |  /* starec amsdu */
2202  | 		mt7996_mcu_sta_amsdu_tlv(dev, skb, vif, sta);
2203  |  /* starec he */
2204  | 		mt7996_mcu_sta_he_tlv(skb, sta);
2205  |  /* starec he 6g*/
2206  | 		mt7996_mcu_sta_he_6g_tlv(skb, sta);
2207  |  /* starec eht */
2208  | 		mt7996_mcu_sta_eht_tlv(skb, sta);
2209  |  /* starec muru */
2210  | 		mt7996_mcu_sta_muru_tlv(dev, skb, vif, sta);
2211  |  /* starec bfee */
2212  | 		mt7996_mcu_sta_bfee_tlv(dev, skb, vif, sta);
2213  | 	}
2214  |  
2215  | 	ret = mt7996_mcu_add_group(dev, vif, sta);
2216  |  if (ret) {
2217  |  dev_kfree_skb(skb);
2218  |  return ret;
2219  | 	}
2220  | out:
2221  |  return mt76_mcu_skb_send_msg(&dev->mt76, skb,
2222  |  MCU_WMWA_UNI_CMD(STA_REC_UPDATE), true);
2223  | }
2224  |  

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
