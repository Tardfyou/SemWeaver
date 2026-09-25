- Decision: Bug
- Reason: `mt7996_mcu_bss_he_tlv()` obtains `cap` directly from `mt76_connac_get_he_phy_cap(phy->mt76, vif)` and unconditionally dereferences it when filling the HE MCS fields:

  ```c
  he->max_nss_mcs[CMD_HE_MCS_BW80] = cap->he_mcs_nss_supp.tx_mcs_80;
  he->max_nss_mcs[CMD_HE_MCS_BW160] = cap->he_mcs_nss_supp.tx_mcs_160;
  he->max_nss_mcs[CMD_HE_MCS_BW8080] = cap->he_mcs_nss_supp.tx_mcs_80p80;
  ```

  This exactly matches the target pattern: a missing NULL check on the return value of `mt76_connac_get_he_phy_cap()` before dereference.

  The caller reaches this function when enabling a non-monitor BSS with `vif->bss_conf.he_support` set. `he_support` establishes that HE is enabled for the virtual interface, but it does not itself prove that `mt76_connac_get_he_phy_cap()` found an HE capability entry for the current `phy`/band. That helper selects an HE capability based on the VIF and PHY band and can return `NULL` when no matching HE capability is available. Therefore, the `cap` pointer is nullable on this path.

  There is no intervening validation, locking issue, or fallback assignment that makes `cap` non-NULL. The first dereference at line 813, reported again at line 814, would fault when the helper returns `NULL`. A suitable fix is to test `cap` immediately after the call and return without adding/populating this HE TLV, or otherwise provide valid fallback MCS values. This is the same root cause and remediation direction as the supplied target bug pattern.