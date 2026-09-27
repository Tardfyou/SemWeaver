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

wifi: brcmfmac: fweh: Fix boot crash on Raspberry Pi 4

Fix boot crash on Raspberry Pi by moving the update to `event->datalen`
before data is copied into flexible-array member `data` via `memcpy()`.

Flexible-array member `data` was annotated with `__counted_by(datalen)`
in commit 62d19b358088 ("wifi: brcmfmac: fweh: Add __counted_by for
struct brcmf_fweh_queue_item and use struct_size()"). The intention of
this is to gain visibility into the size of `data` at run-time through
its _counter_ (in this case `datalen`), and with this have its accesses
bounds-checked at run-time via CONFIG_FORTIFY_SOURCE and
CONFIG_UBSAN_BOUNDS.

To effectively accomplish the above, we shall update the counter
(`datalen`), before the first access to the flexible array (`data`),
which was also done in the mentioned commit.

However, commit edec42821911 ("wifi: brcmfmac: allow per-vendor event
handling") inadvertently caused a buffer overflow, detected by
FORTIFY_SOURCE. It moved the `event->datalen = datalen;` update to after
the first `data` access, at which point `event->datalen` was not yet
updated from zero (after calling `kzalloc()`), leading to the overflow
issue.

This fix repositions the `event->datalen = datalen;` update before
accessing `data`, restoring the intended buffer overflow protection. :)

Fixes: edec42821911 ("wifi: brcmfmac: allow per-vendor event handling")
Reported-by: Nathan Chancellor <nathan@kernel.org>
Closes: https://gist.github.com/nathanchance/e22f681f3bfc467f15cdf6605021aaa6
Tested-by: Nathan Chancellor <nathan@kernel.org>
Signed-off-by: Gustavo A. R. Silva <gustavoars@kernel.org>
Reviewed-by: Kees Cook <keescook@chromium.org>
Acked-by: Arend van Spriel <arend.vanspriel@broadcom.com>
Signed-off-by: Kalle Valo <kvalo@kernel.org>
Link: https://msgid.link/Zc+3PFCUvLoVlpg8@neat

## Buggy Code

```c
// Function: brcmf_fweh_process_event in drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c
void brcmf_fweh_process_event(struct brcmf_pub *drvr,
			      struct brcmf_event *event_packet,
			      u32 packet_len, gfp_t gfp)
{
	u32 fwevt_idx;
	struct brcmf_fweh_info *fweh = drvr->fweh;
	struct brcmf_fweh_queue_item *event;
	void *data;
	u32 datalen;

	/* get event info */
	fwevt_idx = get_unaligned_be32(&event_packet->msg.event_type);
	datalen = get_unaligned_be32(&event_packet->msg.datalen);
	data = &event_packet[1];

	if (fwevt_idx >= fweh->num_event_codes)
		return;

	if (fwevt_idx != BRCMF_E_IF && !fweh->evt_handler[fwevt_idx])
		return;

	if (datalen > BRCMF_DCMD_MAXLEN ||
	    datalen + sizeof(*event_packet) > packet_len)
		return;

	event = kzalloc(struct_size(event, data, datalen), gfp);
	if (!event)
		return;

	event->code = fwevt_idx;
	event->ifidx = event_packet->msg.ifidx;

	/* use memcpy to get aligned event message */
	memcpy(&event->emsg, &event_packet->msg, sizeof(event->emsg));
	memcpy(event->data, data, datalen);
	event->datalen = datalen;
	memcpy(event->ifaddr, event_packet->eth.h_dest, ETH_ALEN);

	brcmf_fweh_queue_event(fweh, event);
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c b/drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c
index 0774f6c59226..f0b6a7607f16 100644
--- a/drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c
+++ b/drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c
@@ -497,12 +497,12 @@ void brcmf_fweh_process_event(struct brcmf_pub *drvr,
 		return;

 	event->code = fwevt_idx;
+	event->datalen = datalen;
 	event->ifidx = event_packet->msg.ifidx;

 	/* use memcpy to get aligned event message */
 	memcpy(&event->emsg, &event_packet->msg, sizeof(event->emsg));
 	memcpy(event->data, data, datalen);
-	event->datalen = datalen;
 	memcpy(event->ifaddr, event_packet->eth.h_dest, ETH_ALEN);

 	brcmf_fweh_queue_event(fweh, event);
```


## Bug Pattern

Accessing a flexible-array member annotated with __counted_by(counter) before initializing its counter field. Specifically, performing memcpy()/memset()/etc. on struct->data (flexible array) while struct->datalen is still zero (e.g., right after kzalloc), causing FORTIFY/UBSAN to see a zero-sized buffer and flag an overflow.

Example:
struct S {
	size_t len;
	u8 data[] __counted_by(len);
};

S *p = kzalloc(struct_size(p, data, n), GFP_KERNEL);
memcpy(p->data, src, n);   // BUG: p->len is 0, bounds check thinks data size is 0
p->len = n;                // should be set before accessing p->data


# Report

BuildSource:| drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c
### Report Summary

File:| fweh.c  
---|---  
Warning:| line 505, column 2  
Flexible-array accessed before initializing its __counted_by counter  
  
### Annotated Source Code


420   |  brcmf_dbg(TRACE, "event handler cleared for %s\n",
421   |  brcmf_fweh_event_name(code));
422   | 	brcmf_fweh_map_event_code(drvr->fweh, code, &evt_handler_idx);
423   | 	drvr->fweh->evt_handler[evt_handler_idx] = NULL;
424   | }
425   |  
426   | /**
427   |  * brcmf_fweh_activate_events() - enables firmware events registered.
428   |  *
429   |  * @ifp: primary interface object.
430   |  */
431   | int brcmf_fweh_activate_events(struct brcmf_if *ifp)
432   | {
433   |  struct brcmf_fweh_info *fweh = ifp->drvr->fweh;
434   |  enum brcmf_fweh_event_code code;
435   |  int i, err;
436   |  
437   |  memset(fweh->event_mask, 0, fweh->event_mask_len);
438   |  for (i = 0; i < fweh->num_event_codes; i++) {
439   |  if (fweh->evt_handler[i]) {
440   | 			brcmf_fweh_map_fwevt_code(fweh, i, &code);
441   |  brcmf_dbg(EVENT, "enable event %s\n",
442   |  brcmf_fweh_event_name(code));
443   |  setbit(fweh->event_mask, i);
444   | 		}
445   | 	}
446   |  
447   |  /* want to handle IF event as well */
448   |  brcmf_dbg(EVENT, "enable event IF\n");
449   |  setbit(fweh->event_mask, BRCMF_E_IF);
450   |  
451   | 	err = brcmf_fil_iovar_data_set(ifp, "event_msgs", fweh->event_mask,
452   | 				       fweh->event_mask_len);
453   |  if (err)
454   |  bphy_err(fweh->drvr, "Set event_msgs error (%d)\n", err);
455   |  
456   |  return err;
457   | }
458   |  
459   | /**
460   |  * brcmf_fweh_process_event() - process skb as firmware event.
461   |  *
462   |  * @drvr: driver information object.
463   |  * @event_packet: event packet to process.
464   |  * @packet_len: length of the packet
465   |  * @gfp: memory allocation flags.
466   |  *
467   |  * If the packet buffer contains a firmware event message it will
468   |  * dispatch the event to a registered handler (using worker).
469   |  */
470   | void brcmf_fweh_process_event(struct brcmf_pub *drvr,
471   |  struct brcmf_event *event_packet,
472   | 			      u32 packet_len, gfp_t gfp)
473   | {
474   |  u32 fwevt_idx;
475   |  struct brcmf_fweh_info *fweh = drvr->fweh;
476   |  struct brcmf_fweh_queue_item *event;
477   |  void *data;
478   | 	u32 datalen;
479   |  
480   |  /* get event info */
481   | 	fwevt_idx = get_unaligned_be32(&event_packet->msg.event_type);
482   | 	datalen = get_unaligned_be32(&event_packet->msg.datalen);
483   | 	data = &event_packet[1];
484   |  
485   |  if (fwevt_idx >= fweh->num_event_codes)
    1Assuming 'fwevt_idx' is < field 'num_event_codes'→
486   |  return;
487   |  
488   |  if (fwevt_idx != BRCMF_E_IF && !fweh->evt_handler[fwevt_idx])
    2←Assuming 'fwevt_idx' is equal to BRCMF_E_IF→
489   |  return;
490   |  
491   |  if (datalen > BRCMF_DCMD_MAXLEN ||
    3←Assuming 'datalen' is <= BRCMF_DCMD_MAXLEN→
    5←Taking false branch→
492   |  datalen + sizeof(*event_packet) > packet_len)
    4←Assuming the condition is false→
493   |  return;
494   |  
495   |  event = kzalloc(struct_size(event, data, datalen), gfp);
496   |  if (!event)
    6←Assuming 'event' is non-null→
    7←Taking false branch→
497   |  return;
498   |  
499   |  event->code = fwevt_idx;
500   | 	event->datalen = datalen;
501   |  event->ifidx = event_packet->msg.ifidx;
502   |  
503   |  /* use memcpy to get aligned event message */
504   |  memcpy(&event->emsg, &event_packet->msg, sizeof(event->emsg));
    8←Taking false branch→
    9←Taking false branch→
505   |  memcpy(event->data, data, datalen);
    10←Taking false branch→
    11←Taking false branch→
    12←Flexible-array accessed before initializing its __counted_by counter
506   |  memcpy(event->ifaddr, event_packet->eth.h_dest, ETH_ALEN);
507   |  
508   | 	brcmf_fweh_queue_event(fweh, event);
509   | }

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
