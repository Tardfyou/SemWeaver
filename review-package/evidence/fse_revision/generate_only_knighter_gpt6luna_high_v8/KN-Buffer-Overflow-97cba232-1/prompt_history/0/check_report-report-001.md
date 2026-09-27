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

drm/amd/display: Fix buffer overflow in 'get_host_router_total_dp_tunnel_bw()'

The error message buffer overflow 'dc->links' 12 <= 12 suggests that the
code is trying to access an element of the dc->links array that is
beyond its bounds. In C, arrays are zero-indexed, so an array with 12
elements has valid indices from 0 to 11. Trying to access dc->links[12]
would be an attempt to access the 13th element of a 12-element array,
which is a buffer overflow.

To fix this, ensure that the loop does not go beyond the last valid
index when accessing dc->links[i + 1] by subtracting 1 from the loop
condition.

This would ensure that i + 1 is always a valid index in the array.

Fixes the below:
drivers/gpu/drm/amd/amdgpu/../display/dc/link/protocols/link_dp_dpia_bw.c:208 get_host_router_total_dp_tunnel_bw() error: buffer overflow 'dc->links' 12 <= 12

Fixes: 59f1622a5f05 ("drm/amd/display: Add dpia display mode validation logic")
Cc: PeiChen Huang <peichen.huang@amd.com>
Cc: Aric Cyr <aric.cyr@amd.com>
Cc: Rodrigo Siqueira <rodrigo.siqueira@amd.com>
Cc: Aurabindo Pillai <aurabindo.pillai@amd.com>
Cc: Meenakshikumar Somasundaram <meenakshikumar.somasundaram@amd.com>
Signed-off-by: Srinivasan Shanmugam <srinivasan.shanmugam@amd.com>
Reviewed-by: Tom Chung <chiahsuan.chung@amd.com>
Signed-off-by: Alex Deucher <alexander.deucher@amd.com>

## Buggy Code

```c
// Function: get_host_router_total_dp_tunnel_bw in drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia_bw.c
static int get_host_router_total_dp_tunnel_bw(const struct dc *dc, uint8_t hr_index)
{
	uint8_t lowest_dpia_index = get_lowest_dpia_index(dc->links[0]);
	uint8_t hr_index_temp = 0;
	struct dc_link *link_dpia_primary, *link_dpia_secondary;
	int total_bw = 0;

	for (uint8_t i = 0; i < MAX_PIPES * 2; ++i) {

		if (!dc->links[i] || dc->links[i]->ep_type != DISPLAY_ENDPOINT_USB4_DPIA)
			continue;

		hr_index_temp = (dc->links[i]->link_index - lowest_dpia_index) / 2;

		if (hr_index_temp == hr_index) {
			link_dpia_primary = dc->links[i];
			link_dpia_secondary = dc->links[i + 1];

			/**
			 * If BW allocation enabled on both DPIAs, then
			 * HR BW = Estimated(dpia_primary) + Allocated(dpia_secondary)
			 * otherwise HR BW = Estimated(bw alloc enabled dpia)
			 */
			if ((link_dpia_primary->hpd_status &&
				link_dpia_primary->dpia_bw_alloc_config.bw_alloc_enabled) &&
				(link_dpia_secondary->hpd_status &&
				link_dpia_secondary->dpia_bw_alloc_config.bw_alloc_enabled)) {
					total_bw += link_dpia_primary->dpia_bw_alloc_config.estimated_bw +
						link_dpia_secondary->dpia_bw_alloc_config.allocated_bw;
			} else if (link_dpia_primary->hpd_status &&
					link_dpia_primary->dpia_bw_alloc_config.bw_alloc_enabled) {
				total_bw = link_dpia_primary->dpia_bw_alloc_config.estimated_bw;
			} else if (link_dpia_secondary->hpd_status &&
				link_dpia_secondary->dpia_bw_alloc_config.bw_alloc_enabled) {
				total_bw += link_dpia_secondary->dpia_bw_alloc_config.estimated_bw;
			}
			break;
		}
	}

	return total_bw;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia_bw.c b/drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia_bw.c
index dd0d2b206462..5491b707cec8 100644
--- a/drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia_bw.c
+++ b/drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia_bw.c
@@ -196,7 +196,7 @@ static int get_host_router_total_dp_tunnel_bw(const struct dc *dc, uint8_t hr_in
 	struct dc_link *link_dpia_primary, *link_dpia_secondary;
 	int total_bw = 0;

-	for (uint8_t i = 0; i < MAX_PIPES * 2; ++i) {
+	for (uint8_t i = 0; i < (MAX_PIPES * 2) - 1; ++i) {

 		if (!dc->links[i] || dc->links[i]->ep_type != DISPLAY_ENDPOINT_USB4_DPIA)
 			continue;
```


## Bug Pattern

Off-by-one array iteration when accessing a look-ahead element:
looping with i < size while dereferencing arr[i + 1] inside the loop. This makes the last iteration access arr[size], which is out of bounds. Correct pattern requires bounding the loop by size - 1 when using arr[i + 1], e.g.:

for (i = 0; i < size - 1; ++i) {
    use(arr[i]);
    use(arr[i + 1]);
}


# Report

BuildSource:| drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia_bw.c
### Report Summary

File:|
/work/SemWeaver/artifacts/external/linux/drivers/gpu/drm/amd/amdgpu/../display/dc/link/protocols/link_dp_dpia_bw.c  
---|---  
Warning:| line 208, column 26  
Off-by-one: loop uses 'i < bound' but accesses element at 'i + 1'. Use 'i <
bound - 1' or guard the access  
  
### Annotated Source Code


158   |  DC_LOG_DEBUG("%s: nrd_max_link_rate(%d), nrd_max_lane_count(%d)\n",
159   |  __func__, link->dpia_bw_alloc_config.nrd_max_link_rate,
160   |  link->dpia_bw_alloc_config.nrd_max_lane_count);
161   | }
162   |  
163   | static uint8_t get_lowest_dpia_index(struct dc_link *link)
164   | {
165   |  const struct dc *dc_struct = link->dc;
166   | 	uint8_t idx = 0xFF;
167   |  int i;
168   |  
169   |  for (i = 0; i < MAX_PIPES * 2; ++i) {
170   |  
171   |  if (!dc_struct->links[i] ||
172   | 				dc_struct->links[i]->ep_type != DISPLAY_ENDPOINT_USB4_DPIA)
173   |  continue;
174   |  
175   |  if (idx > dc_struct->links[i]->link_index) {
176   | 			idx = dc_struct->links[i]->link_index;
177   |  break;
178   | 		}
179   | 	}
180   |  
181   |  return idx;
182   | }
183   |  
184   | /*
185   |  * Get the maximum dp tunnel banwidth of host router
186   |  *
187   |  * @dc: pointer to the dc struct instance
188   |  * @hr_index: host router index
189   |  *
190   |  * return: host router maximum dp tunnel bandwidth
191   |  */
192   | static int get_host_router_total_dp_tunnel_bw(const struct dc *dc, uint8_t hr_index)
193   | {
194   | 	uint8_t lowest_dpia_index = get_lowest_dpia_index(dc->links[0]);
195   | 	uint8_t hr_index_temp = 0;
196   |  struct dc_link *link_dpia_primary, *link_dpia_secondary;
197   |  int total_bw = 0;
198   |  
199   |  for (uint8_t i = 0; i < (MAX_PIPES * 2) - 1; ++i) {
200   |  
201   |  if (!dc->links[i] || dc->links[i]->ep_type != DISPLAY_ENDPOINT_USB4_DPIA)
202   |  continue;
203   |  
204   | 		hr_index_temp = (dc->links[i]->link_index - lowest_dpia_index) / 2;
205   |  
206   |  if (hr_index_temp == hr_index) {
207   | 			link_dpia_primary = dc->links[i];
208   | 			link_dpia_secondary = dc->links[i + 1];
    Off-by-one: loop uses 'i < bound' but accesses element at 'i + 1'. Use 'i < bound - 1' or guard the access
209   |  
210   |  /**
211   |  * If BW allocation enabled on both DPIAs, then
212   |  * HR BW = Estimated(dpia_primary) + Allocated(dpia_secondary)
213   |  * otherwise HR BW = Estimated(bw alloc enabled dpia)
214   |  */
215   |  if ((link_dpia_primary->hpd_status &&
216   | 				link_dpia_primary->dpia_bw_alloc_config.bw_alloc_enabled) &&
217   | 				(link_dpia_secondary->hpd_status &&
218   | 				link_dpia_secondary->dpia_bw_alloc_config.bw_alloc_enabled)) {
219   | 					total_bw += link_dpia_primary->dpia_bw_alloc_config.estimated_bw +
220   | 						link_dpia_secondary->dpia_bw_alloc_config.allocated_bw;
221   | 			} else if (link_dpia_primary->hpd_status &&
222   | 					link_dpia_primary->dpia_bw_alloc_config.bw_alloc_enabled) {
223   | 				total_bw = link_dpia_primary->dpia_bw_alloc_config.estimated_bw;
224   | 			} else if (link_dpia_secondary->hpd_status &&
225   | 				link_dpia_secondary->dpia_bw_alloc_config.bw_alloc_enabled) {
226   | 				total_bw += link_dpia_secondary->dpia_bw_alloc_config.estimated_bw;
227   | 			}
228   |  break;
229   | 		}
230   | 	}
231   |  
232   |  return total_bw;
233   | }
234   |  
235   | /*
236   |  * Cleanup function for when the dpia is unplugged to reset struct
237   |  * and perform any required clean up
238   |  *

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
