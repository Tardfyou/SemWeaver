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

## Bug Pattern

The bug pattern is the missing NULL pointer check for a function's return value before using it. In this patch, the pointer 'vc' (obtained from mt76_connac_get_he_phy_cap) is dereferenced without verifying that it is non-NULL, which can lead to a NULL pointer dereference if the function fails and returns NULL.

## Bug Pattern

The bug pattern is the missing NULL pointer check for a function's return value before using it. In this patch, the pointer 'vc' (obtained from mt76_connac_get_he_phy_cap) is dereferenced without verifying that it is non-NULL, which can lead to a NULL pointer dereference if the function fails and returns NULL.

# Report

BuildSource:| drivers/net/wireless/mediatek/mt76/mt7996/mcu.c
### Report Summary

File:| mcu.c  
---|---  
Warning:| line 814, column 38  
Missing NULL check for mt76_connac_get_he_phy_cap return value  
  
### Annotated Source Code


685   | 	}
686   |  default:
687   |  break;
688   | 	}
689   | }
690   |  
691   | static void
692   | mt7996_mcu_uni_rx_unsolicited_event(struct mt7996_dev *dev, struct sk_buff *skb)
693   | {
694   |  struct mt7996_mcu_rxd *rxd = (struct mt7996_mcu_rxd *)skb->data;
695   |  
696   |  switch (rxd->eid) {
697   |  case MCU_UNI_EVENT_FW_LOG_2_HOST:
698   | 		mt7996_mcu_rx_log_message(dev, skb);
699   |  break;
700   |  case MCU_UNI_EVENT_IE_COUNTDOWN:
701   | 		mt7996_mcu_ie_countdown(dev, skb);
702   |  break;
703   |  case MCU_UNI_EVENT_RDD_REPORT:
704   | 		mt7996_mcu_rx_radar_detected(dev, skb);
705   |  break;
706   |  case MCU_UNI_EVENT_ALL_STA_INFO:
707   | 		mt7996_mcu_rx_all_sta_info_event(dev, skb);
708   |  break;
709   |  case MCU_UNI_EVENT_WED_RRO:
710   | 		mt7996_mcu_wed_rro_event(dev, skb);
711   |  break;
712   |  default:
713   |  break;
714   | 	}
715   |  dev_kfree_skb(skb);
716   | }
717   |  
718   | void mt7996_mcu_rx_event(struct mt7996_dev *dev, struct sk_buff *skb)
719   | {
720   |  struct mt7996_mcu_rxd *rxd = (struct mt7996_mcu_rxd *)skb->data;
721   |  
722   |  if (rxd->option & MCU_UNI_CMD_UNSOLICITED_EVENT) {
723   | 		mt7996_mcu_uni_rx_unsolicited_event(dev, skb);
724   |  return;
725   | 	}
726   |  
727   |  /* WA still uses legacy event*/
728   |  if (rxd->ext_eid == MCU_EXT_EVENT_FW_LOG_2_HOST ||
729   | 	    !rxd->seq)
730   | 		mt7996_mcu_rx_unsolicited_event(dev, skb);
731   |  else
732   | 		mt76_mcu_rx_event(&dev->mt76, skb);
733   | }
734   |  
735   | static struct tlv *
736   | mt7996_mcu_add_uni_tlv(struct sk_buff *skb, u16 tag, u16 len)
737   | {
738   |  struct tlv *ptlv = skb_put(skb, len);
739   |  
740   | 	ptlv->tag = cpu_to_le16(tag);
741   | 	ptlv->len = cpu_to_le16(len);
742   |  
743   |  return ptlv;
744   | }
745   |  
746   | static void
747   | mt7996_mcu_bss_rfch_tlv(struct sk_buff *skb, struct ieee80211_vif *vif,
748   |  struct mt7996_phy *phy)
749   | {
750   |  static const u8 rlm_ch_band[] = {
751   | 		[NL80211_BAND_2GHZ] = 1,
752   | 		[NL80211_BAND_5GHZ] = 2,
753   | 		[NL80211_BAND_6GHZ] = 3,
754   | 	};
755   |  struct cfg80211_chan_def *chandef = &phy->mt76->chandef;
756   |  struct bss_rlm_tlv *ch;
757   |  struct tlv *tlv;
758   |  int freq1 = chandef->center_freq1;
759   |  
760   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_RLM, sizeof(*ch));
761   |  
762   | 	ch = (struct bss_rlm_tlv *)tlv;
763   | 	ch->control_channel = chandef->chan->hw_value;
764   | 	ch->center_chan = ieee80211_frequency_to_channel(freq1);
765   | 	ch->bw = mt76_connac_chan_bw(chandef);
766   | 	ch->tx_streams = hweight8(phy->mt76->antenna_mask);
767   | 	ch->rx_streams = hweight8(phy->mt76->antenna_mask);
768   | 	ch->band = rlm_ch_band[chandef->chan->band];
769   |  
770   |  if (chandef->width == NL80211_CHAN_WIDTH_80P80) {
771   |  int freq2 = chandef->center_freq2;
772   |  
773   | 		ch->center_chan2 = ieee80211_frequency_to_channel(freq2);
774   | 	}
775   | }
776   |  
777   | static void
778   | mt7996_mcu_bss_ra_tlv(struct sk_buff *skb, struct ieee80211_vif *vif,
779   |  struct mt7996_phy *phy)
780   | {
781   |  struct bss_ra_tlv *ra;
782   |  struct tlv *tlv;
783   |  
784   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_RA, sizeof(*ra));
785   |  
786   | 	ra = (struct bss_ra_tlv *)tlv;
787   | 	ra->short_preamble = true;
788   | }
789   |  
790   | static void
791   | mt7996_mcu_bss_he_tlv(struct sk_buff *skb, struct ieee80211_vif *vif,
792   |  struct mt7996_phy *phy)
793   | {
794   | #define DEFAULT_HE_PE_DURATION		4
795   | #define DEFAULT_HE_DURATION_RTS_THRES	1023
796   |  const struct ieee80211_sta_he_cap *cap;
797   |  struct bss_info_uni_he *he;
798   |  struct tlv *tlv;
799   |  
800   | 	cap = mt76_connac_get_he_phy_cap(phy->mt76, vif);
801   |  
802   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_HE_BASIC, sizeof(*he));
803   |  
804   | 	he = (struct bss_info_uni_he *)tlv;
805   | 	he->he_pe_duration = vif->bss_conf.htc_trig_based_pkt_ext;
806   |  if (!he->he_pe_duration)
    10←Assuming field 'he_pe_duration' is not equal to 0→
    11←Taking false branch→
807   | 		he->he_pe_duration = DEFAULT_HE_PE_DURATION;
808   |  
809   |  he->he_rts_thres = cpu_to_le16(vif->bss_conf.frame_time_rts_th);
810   |  if (!he->he_rts_thres)
    12←Assuming field 'he_rts_thres' is not equal to 0→
    13←Taking false branch→
811   | 		he->he_rts_thres = cpu_to_le16(DEFAULT_HE_DURATION_RTS_THRES);
812   |  
813   |  he->max_nss_mcs[CMD_HE_MCS_BW80] = cap->he_mcs_nss_supp.tx_mcs_80;
814   |  he->max_nss_mcs[CMD_HE_MCS_BW160] = cap->he_mcs_nss_supp.tx_mcs_160;
    14←Missing NULL check for mt76_connac_get_he_phy_cap return value
815   | 	he->max_nss_mcs[CMD_HE_MCS_BW8080] = cap->he_mcs_nss_supp.tx_mcs_80p80;
816   | }
817   |  
818   | static void
819   | mt7996_mcu_bss_mbssid_tlv(struct sk_buff *skb, struct ieee80211_vif *vif,
820   |  struct mt7996_phy *phy, int enable)
821   | {
822   |  struct bss_info_uni_mbssid *mbssid;
823   |  struct tlv *tlv;
824   |  
825   |  if (!vif->bss_conf.bssid_indicator && enable)
826   |  return;
827   |  
828   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_11V_MBSSID, sizeof(*mbssid));
829   |  
830   | 	mbssid = (struct bss_info_uni_mbssid *)tlv;
831   |  
832   |  if (enable) {
833   | 		mbssid->max_indicator = vif->bss_conf.bssid_indicator;
834   | 		mbssid->mbss_idx = vif->bss_conf.bssid_index;
835   | 		mbssid->tx_bss_omac_idx = 0;
836   | 	}
837   | }
838   |  
839   | static void
840   | mt7996_mcu_bss_bmc_tlv(struct sk_buff *skb, struct ieee80211_vif *vif,
841   |  struct mt7996_phy *phy)
842   | {
843   |  struct mt76_vif *mvif = (struct mt76_vif *)vif->drv_priv;
844   |  struct bss_rate_tlv *bmc;
845   |  struct cfg80211_chan_def *chandef = &phy->mt76->chandef;
846   |  enum nl80211_band band = chandef->chan->band;
847   |  struct tlv *tlv;
848   | 	u8 idx = mvif->mcast_rates_idx ?
849   | 		 mvif->mcast_rates_idx : mvif->basic_rates_idx;
850   |  
851   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_RATE, sizeof(*bmc));
852   |  
853   | 	bmc = (struct bss_rate_tlv *)tlv;
854   |  
855   | 	bmc->short_preamble = (band == NL80211_BAND_2GHZ);
856   | 	bmc->bc_fixed_rate = idx;
857   | 	bmc->mc_fixed_rate = idx;
858   | }
859   |  
860   | static void
861   | mt7996_mcu_bss_txcmd_tlv(struct sk_buff *skb, bool en)
862   | {
863   |  struct bss_txcmd_tlv *txcmd;
864   |  struct tlv *tlv;
865   |  
866   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_TXCMD, sizeof(*txcmd));
867   |  
868   | 	txcmd = (struct bss_txcmd_tlv *)tlv;
869   | 	txcmd->txcmd_mode = en;
870   | }
871   |  
872   | static void
873   | mt7996_mcu_bss_mld_tlv(struct sk_buff *skb, struct ieee80211_vif *vif)
874   | {
875   |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
876   |  struct bss_mld_tlv *mld;
877   |  struct tlv *tlv;
878   |  
879   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_MLD, sizeof(*mld));
880   |  
881   | 	mld = (struct bss_mld_tlv *)tlv;
882   | 	mld->group_mld_id = 0xff;
883   | 	mld->own_mld_id = mvif->mt76.idx;
884   | 	mld->remap_idx = 0xff;
885   | }
886   |  
887   | static void
888   | mt7996_mcu_bss_sec_tlv(struct sk_buff *skb, struct ieee80211_vif *vif)
889   | {
890   |  struct mt76_vif *mvif = (struct mt76_vif *)vif->drv_priv;
891   |  struct bss_sec_tlv *sec;
892   |  struct tlv *tlv;
893   |  
894   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_SEC, sizeof(*sec));
895   |  
896   | 	sec = (struct bss_sec_tlv *)tlv;
897   | 	sec->cipher = mvif->cipher;
898   | }
899   |  
900   | static int
901   | mt7996_mcu_muar_config(struct mt7996_phy *phy, struct ieee80211_vif *vif,
902   | 		       bool bssid, bool enable)
903   | {
904   | #define UNI_MUAR_ENTRY 2
905   |  struct mt7996_dev *dev = phy->dev;
906   |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
907   | 	u32 idx = mvif->mt76.omac_idx - REPEATER_BSSID_START;
908   |  const u8 *addr = vif->addr;
909   |  
910   |  struct {
911   |  struct {
912   | 			u8 band;
913   | 			u8 __rsv[3];
914   | 		} hdr;
915   |  
916   | 		__le16 tag;
917   | 		__le16 len;
918   |  
919   | 		bool smesh;
920   | 		u8 bssid;
921   | 		u8 index;
922   | 		u8 entry_add;
923   | 		u8 addr[ETH_ALEN];
924   | 		u8 __rsv[2];
925   | 	} __packed req = {
926   | 		.hdr.band = phy->mt76->band_idx,
927   | 		.tag = cpu_to_le16(UNI_MUAR_ENTRY),
928   | 		.len = cpu_to_le16(sizeof(req) - sizeof(req.hdr)),
929   | 		.smesh = false,
930   | 		.index = idx * 2 + bssid,
931   | 		.entry_add = true,
932   | 	};
933   |  
934   |  if (bssid)
935   | 		addr = vif->bss_conf.bssid;
936   |  
937   |  if (enable)
938   |  memcpy(req.addr, addr, ETH_ALEN);
939   |  
940   |  return mt76_mcu_send_msg(&dev->mt76, MCU_WM_UNI_CMD(REPT_MUAR), &req,
941   |  sizeof(req), true);
942   | }
943   |  
944   | static void
945   | mt7996_mcu_bss_ifs_timing_tlv(struct sk_buff *skb, struct ieee80211_vif *vif)
946   | {
947   |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
948   |  struct mt7996_phy *phy = mvif->phy;
949   |  struct bss_ifs_time_tlv *ifs_time;
950   |  struct tlv *tlv;
951   | 	bool is_2ghz = phy->mt76->chandef.chan->band == NL80211_BAND_2GHZ;
952   |  
953   | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_IFS_TIME, sizeof(*ifs_time));
954   |  
955   | 	ifs_time = (struct bss_ifs_time_tlv *)tlv;
956   | 	ifs_time->slot_valid = true;
957   | 	ifs_time->sifs_valid = true;
958   | 	ifs_time->rifs_valid = true;
959   | 	ifs_time->eifs_valid = true;
960   |  
961   | 	ifs_time->slot_time = cpu_to_le16(phy->slottime);
962   | 	ifs_time->sifs_time = cpu_to_le16(10);
963   | 	ifs_time->rifs_time = cpu_to_le16(2);
964   | 	ifs_time->eifs_time = cpu_to_le16(is_2ghz ? 78 : 84);
965   |  
966   |  if (is_2ghz) {
967   | 		ifs_time->eifs_cck_valid = true;
968   | 		ifs_time->eifs_cck_time = cpu_to_le16(314);
969   | 	}
970   | }
971   |  
972   | static int
973   | mt7996_mcu_bss_basic_tlv(struct sk_buff *skb,
974   |  struct ieee80211_vif *vif,
975   |  struct ieee80211_sta *sta,
976   |  struct mt76_phy *phy, u16 wlan_idx,
977   | 			 bool enable)
978   | {
979   |  struct mt76_vif *mvif = (struct mt76_vif *)vif->drv_priv;
980   |  struct cfg80211_chan_def *chandef = &phy->chandef;
981   |  struct mt76_connac_bss_basic_tlv *bss;
982   | 	u32 type = CONNECTION_INFRA_AP;
983   | 	u16 sta_wlan_idx = wlan_idx;
984   |  struct tlv *tlv;
985   |  int idx;
986   |  
987   |  switch (vif->type) {
988   |  case NL80211_IFTYPE_MESH_POINT:
989   |  case NL80211_IFTYPE_AP:
990   |  case NL80211_IFTYPE_MONITOR:
991   |  break;
992   |  case NL80211_IFTYPE_STATION:
993   |  if (enable) {
994   | 			rcu_read_lock();
995   |  if (!sta)
996   | 				sta = ieee80211_find_sta(vif,
997   | 							 vif->bss_conf.bssid);
998   |  /* TODO: enable BSS_INFO_UAPSD & BSS_INFO_PM */
999   |  if (sta) {
1000  |  struct mt76_wcid *wcid;
1001  |  
1002  | 				wcid = (struct mt76_wcid *)sta->drv_priv;
1003  | 				sta_wlan_idx = wcid->idx;
1004  | 			}
1005  | 			rcu_read_unlock();
1006  | 		}
1007  | 		type = CONNECTION_INFRA_STA;
1008  |  break;
1009  |  case NL80211_IFTYPE_ADHOC:
1010  | 		type = CONNECTION_IBSS_ADHOC;
1011  |  break;
1012  |  default:
1013  |  WARN_ON(1);
1014  |  break;
1015  | 	}
1016  |  
1017  | 	tlv = mt7996_mcu_add_uni_tlv(skb, UNI_BSS_INFO_BASIC, sizeof(*bss));
1018  |  
1019  | 	bss = (struct mt76_connac_bss_basic_tlv *)tlv;
1020  | 	bss->bcn_interval = cpu_to_le16(vif->bss_conf.beacon_int);
1021  | 	bss->dtim_period = vif->bss_conf.dtim_period;
1022  | 	bss->bmc_tx_wlan_idx = cpu_to_le16(wlan_idx);
1023  | 	bss->sta_idx = cpu_to_le16(sta_wlan_idx);
1024  | 	bss->conn_type = cpu_to_le32(type);
1025  | 	bss->omac_idx = mvif->omac_idx;
1026  | 	bss->band_idx = mvif->band_idx;
1027  | 	bss->wmm_idx = mvif->wmm_idx;
1028  | 	bss->conn_state = !enable;
1029  | 	bss->active = enable;
1030  |  
1031  | 	idx = mvif->omac_idx > EXT_BSSID_START ? HW_BSSID_0 : mvif->omac_idx;
1032  | 	bss->hw_bss_idx = idx;
1033  |  
1034  |  if (vif->type == NL80211_IFTYPE_MONITOR) {
1035  |  memcpy(bss->bssid, phy->macaddr, ETH_ALEN);
1036  |  return 0;
1037  | 	}
1038  |  
1039  |  memcpy(bss->bssid, vif->bss_conf.bssid, ETH_ALEN);
1040  | 	bss->bcn_interval = cpu_to_le16(vif->bss_conf.beacon_int);
1041  | 	bss->dtim_period = vif->bss_conf.dtim_period;
1042  | 	bss->phymode = mt76_connac_get_phy_mode(phy, vif,
1043  | 						chandef->chan->band, NULL);
1044  | 	bss->phymode_ext = mt76_connac_get_phy_mode_ext(phy, vif,
1045  | 							chandef->chan->band);
1046  |  
1047  |  return 0;
1048  | }
1049  |  
1050  | static struct sk_buff *
1051  | __mt7996_mcu_alloc_bss_req(struct mt76_dev *dev, struct mt76_vif *mvif, int len)
1052  | {
1053  |  struct bss_req_hdr hdr = {
1054  | 		.bss_idx = mvif->idx,
1055  | 	};
1056  |  struct sk_buff *skb;
1057  |  
1058  | 	skb = mt76_mcu_msg_alloc(dev, NULL, len);
1059  |  if (!skb)
1060  |  return ERR_PTR(-ENOMEM);
1061  |  
1062  | 	skb_put_data(skb, &hdr, sizeof(hdr));
1063  |  
1064  |  return skb;
1065  | }
1066  |  
1067  | int mt7996_mcu_add_bss_info(struct mt7996_phy *phy,
1068  |  struct ieee80211_vif *vif, int enable)
1069  | {
1070  |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
1071  |  struct mt7996_dev *dev = phy->dev;
1072  |  struct sk_buff *skb;
1073  |  
1074  |  if (mvif->mt76.omac_idx >= REPEATER_BSSID_START) {
    1Assuming field 'omac_idx' is < REPEATER_BSSID_START→
    2←Taking false branch→
1075  | 		mt7996_mcu_muar_config(phy, vif, false, enable);
1076  | 		mt7996_mcu_muar_config(phy, vif, true, enable);
1077  | 	}
1078  |  
1079  |  skb = __mt7996_mcu_alloc_bss_req(&dev->mt76, &mvif->mt76,
1080  |  MT7996_BSS_UPDATE_MAX_SIZE);
1081  |  if (IS_ERR(skb))
    3←Taking false branch→
1082  |  return PTR_ERR(skb);
1083  |  
1084  |  /* bss_basic must be first */
1085  |  mt7996_mcu_bss_basic_tlv(skb, vif, NULL, phy->mt76,
1086  | 				 mvif->sta.wcid.idx, enable);
1087  | 	mt7996_mcu_bss_sec_tlv(skb, vif);
1088  |  
1089  |  if (vif->type == NL80211_IFTYPE_MONITOR)
    4←Assuming field 'type' is not equal to NL80211_IFTYPE_MONITOR→
    5←Taking false branch→
1090  |  goto out;
1091  |  
1092  |  if (enable5.1'enable' is not equal to 0) {
    6←Taking true branch→
1093  |  mt7996_mcu_bss_rfch_tlv(skb, vif, phy);
1094  | 		mt7996_mcu_bss_bmc_tlv(skb, vif, phy);
1095  | 		mt7996_mcu_bss_ra_tlv(skb, vif, phy);
1096  | 		mt7996_mcu_bss_txcmd_tlv(skb, true);
1097  | 		mt7996_mcu_bss_ifs_timing_tlv(skb, vif);
1098  |  
1099  |  if (vif->bss_conf.he_support)
    7←Assuming field 'he_support' is true→
    8←Taking true branch→
1100  |  mt7996_mcu_bss_he_tlv(skb, vif, phy);
    9←Calling 'mt7996_mcu_bss_he_tlv'→
1101  |  
1102  |  /* this tag is necessary no matter if the vif is MLD */
1103  | 		mt7996_mcu_bss_mld_tlv(skb, vif);
1104  | 	}
1105  |  
1106  | 	mt7996_mcu_bss_mbssid_tlv(skb, vif, phy, enable);
1107  |  
1108  | out:
1109  |  return mt76_mcu_skb_send_msg(&dev->mt76, skb,
1110  |  MCU_WMWA_UNI_CMD(BSS_INFO_UPDATE), true);
1111  | }
1112  |  
1113  | int mt7996_mcu_set_timing(struct mt7996_phy *phy, struct ieee80211_vif *vif)
1114  | {
1115  |  struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
1116  |  struct mt7996_dev *dev = phy->dev;
1117  |  struct sk_buff *skb;
1118  |  
1119  | 	skb = __mt7996_mcu_alloc_bss_req(&dev->mt76, &mvif->mt76,
1120  |  MT7996_BSS_UPDATE_MAX_SIZE);
1121  |  if (IS_ERR(skb))
1122  |  return PTR_ERR(skb);
1123  |  
1124  | 	mt7996_mcu_bss_ifs_timing_tlv(skb, vif);
1125  |  
1126  |  return mt76_mcu_skb_send_msg(&dev->mt76, skb,
1127  |  MCU_WMWA_UNI_CMD(BSS_INFO_UPDATE), true);
1128  | }
1129  |  
1130  | static int

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
