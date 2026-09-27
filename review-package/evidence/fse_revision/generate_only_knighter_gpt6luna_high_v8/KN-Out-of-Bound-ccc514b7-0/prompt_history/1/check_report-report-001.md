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

drm/amd/display: Fix possible buffer overflow in 'find_dcfclk_for_voltage()'

when 'find_dcfclk_for_voltage()' function is looping over
VG_NUM_SOC_VOLTAGE_LEVELS (which is 8), but the size of the DcfClocks
array is VG_NUM_DCFCLK_DPM_LEVELS (which is 7).

When the loop variable i reaches 7, the function tries to access
clock_table->DcfClocks[7]. However, since the size of the DcfClocks
array is 7, the valid indices are 0 to 6. Index 7 is beyond the size of
the array, leading to a buffer overflow.

Reported by smatch & thus fixing the below:
drivers/gpu/drm/amd/amdgpu/../display/dc/clk_mgr/dcn301/vg_clk_mgr.c:550 find_dcfclk_for_voltage() error: buffer overflow 'clock_table->DcfClocks' 7 <= 7

Fixes: 3a83e4e64bb1 ("drm/amd/display: Add dcn3.01 support to DC (v2)")
Cc: Roman Li <Roman.Li@amd.com>
Cc: Rodrigo Siqueira <Rodrigo.Siqueira@amd.com>
Cc: Aurabindo Pillai <aurabindo.pillai@amd.com>
Signed-off-by: Srinivasan Shanmugam <srinivasan.shanmugam@amd.com>
Reviewed-by: Roman Li <roman.li@amd.com>
Signed-off-by: Alex Deucher <alexander.deucher@amd.com>

## Buggy Code

```c
// Function: find_dcfclk_for_voltage in drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
static unsigned int find_dcfclk_for_voltage(const struct vg_dpm_clocks *clock_table,
		unsigned int voltage)
{
	int i;

	for (i = 0; i < VG_NUM_SOC_VOLTAGE_LEVELS; i++) {
		if (clock_table->SocVoltage[i] == voltage)
			return clock_table->DcfClocks[i];
	}

	ASSERT(0);
	return 0;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c b/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
index a5489fe6875f..aa9fd1dc550a 100644
--- a/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
+++ b/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
@@ -546,6 +546,8 @@ static unsigned int find_dcfclk_for_voltage(const struct vg_dpm_clocks *clock_ta
 	int i;

 	for (i = 0; i < VG_NUM_SOC_VOLTAGE_LEVELS; i++) {
+		if (i >= VG_NUM_DCFCLK_DPM_LEVELS)
+			break;
 		if (clock_table->SocVoltage[i] == voltage)
 			return clock_table->DcfClocks[i];
 	}
```


## Bug Pattern

Iterating over two parallel arrays with the same index while using the length/limit of the larger array as the loop bound, and then indexing into the smaller array without an additional bound check. Concretely:

for (i = 0; i < SIZE_A; i++) {   // SIZE_A > SIZE_B
    if (A[i] == key)
        return B[i];             // out-of-bounds when i >= SIZE_B
}

Here, A has SIZE_A elements and B has SIZE_B elements; the loop uses SIZE_A but also accesses B[i], causing a buffer overflow when i reaches SIZE_B..


# Report

BuildSource:| drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
### Report Summary

File:|
/work/SemWeaver/artifacts/external/linux/drivers/gpu/drm/amd/amdgpu/../display/dc/clk_mgr/dcn301/vg_clk_mgr.c  
---|---  
Warning:| line 552, column 11  
Loop bound 8 exceeds array 'DcfClocks' size 7; DcfClocks[i] may be out of
bounds  
  
### Annotated Source Code


502   | 			{
503   | 				.voltage = 0,
504   | 				.dcfclk_mhz = 483,
505   | 				.fclk_mhz = 800,
506   | 				.memclk_mhz = 1600,
507   | 				.socclk_mhz = 0,
508   | 			},
509   | 			{
510   | 				.voltage = 0,
511   | 				.dcfclk_mhz = 602,
512   | 				.fclk_mhz = 1067,
513   | 				.memclk_mhz = 1067,
514   | 				.socclk_mhz = 0,
515   | 			},
516   | 			{
517   | 				.voltage = 0,
518   | 				.dcfclk_mhz = 738,
519   | 				.fclk_mhz = 1333,
520   | 				.memclk_mhz = 1600,
521   | 				.socclk_mhz = 0,
522   | 			},
523   | 		},
524   |  
525   | 		.num_entries = 4,
526   | 	},
527   |  
528   | };
529   |  
530   | static uint32_t find_max_clk_value(const uint32_t clocks[], uint32_t num_clocks)
531   | {
532   | 	uint32_t max = 0;
533   |  int i;
534   |  
535   |  for (i = 0; i < num_clocks; ++i) {
536   |  if (clocks[i] > max)
537   | 			max = clocks[i];
538   | 	}
539   |  
540   |  return max;
541   | }
542   |  
543   | static unsigned int find_dcfclk_for_voltage(const struct vg_dpm_clocks *clock_table,
544   |  unsigned int voltage)
545   | {
546   |  int i;
547   |  
548   |  for (i = 0; i < VG_NUM_SOC_VOLTAGE_LEVELS; i++) {
549   |  if (i >= VG_NUM_DCFCLK_DPM_LEVELS)
550   |  break;
551   |  if (clock_table->SocVoltage[i] == voltage)
552   |  return clock_table->DcfClocks[i];
    Loop bound 8 exceeds array 'DcfClocks' size 7; DcfClocks[i] may be out of bounds
553   | 	}
554   |  
555   |  ASSERT(0);
556   |  return 0;
557   | }
558   |  
559   | static void vg_clk_mgr_helper_populate_bw_params(
560   |  struct clk_mgr_internal *clk_mgr,
561   |  struct integrated_info *bios_info,
562   |  const struct vg_dpm_clocks *clock_table)
563   | {
564   |  int i, j;
565   |  struct clk_bw_params *bw_params = clk_mgr->base.bw_params;
566   |  
567   | 	j = -1;
568   |  
569   |  ASSERT(VG_NUM_FCLK_DPM_LEVELS <= MAX_NUM_DPM_LVL);
570   |  
571   |  /* Find lowest DPM, FCLK is filled in reverse order*/
572   |  
573   |  for (i = VG_NUM_FCLK_DPM_LEVELS - 1; i >= 0; i--) {
574   |  if (clock_table->DfPstateTable[i].fclk != 0) {
575   | 			j = i;
576   |  break;
577   | 		}
578   | 	}
579   |  
580   |  if (j == -1) {
581   |  /* clock table is all 0s, just use our own hardcode */
582   |  ASSERT(0);

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
