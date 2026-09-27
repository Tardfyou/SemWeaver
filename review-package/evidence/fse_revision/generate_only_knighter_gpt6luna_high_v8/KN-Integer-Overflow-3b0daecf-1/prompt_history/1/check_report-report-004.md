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

amdkfd: use calloc instead of kzalloc to avoid integer overflow

This uses calloc instead of doing the multiplication which might
overflow.

Cc: stable@vger.kernel.org
Signed-off-by: Dave Airlie <airlied@redhat.com>

## Buggy Code

```c
// Function: kfd_ioctl_get_process_apertures_new in drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
static int kfd_ioctl_get_process_apertures_new(struct file *filp,
				struct kfd_process *p, void *data)
{
	struct kfd_ioctl_get_process_apertures_new_args *args = data;
	struct kfd_process_device_apertures *pa;
	int ret;
	int i;

	dev_dbg(kfd_device, "get apertures for PASID 0x%x", p->pasid);

	if (args->num_of_nodes == 0) {
		/* Return number of nodes, so that user space can alloacate
		 * sufficient memory
		 */
		mutex_lock(&p->mutex);
		args->num_of_nodes = p->n_pdds;
		goto out_unlock;
	}

	/* Fill in process-aperture information for all available
	 * nodes, but not more than args->num_of_nodes as that is
	 * the amount of memory allocated by user
	 */
	pa = kzalloc((sizeof(struct kfd_process_device_apertures) *
				args->num_of_nodes), GFP_KERNEL);
	if (!pa)
		return -ENOMEM;

	mutex_lock(&p->mutex);

	if (!p->n_pdds) {
		args->num_of_nodes = 0;
		kfree(pa);
		goto out_unlock;
	}

	/* Run over all pdd of the process */
	for (i = 0; i < min(p->n_pdds, args->num_of_nodes); i++) {
		struct kfd_process_device *pdd = p->pdds[i];

		pa[i].gpu_id = pdd->dev->id;
		pa[i].lds_base = pdd->lds_base;
		pa[i].lds_limit = pdd->lds_limit;
		pa[i].gpuvm_base = pdd->gpuvm_base;
		pa[i].gpuvm_limit = pdd->gpuvm_limit;
		pa[i].scratch_base = pdd->scratch_base;
		pa[i].scratch_limit = pdd->scratch_limit;

		dev_dbg(kfd_device,
			"gpu id %u\n", pdd->dev->id);
		dev_dbg(kfd_device,
			"lds_base %llX\n", pdd->lds_base);
		dev_dbg(kfd_device,
			"lds_limit %llX\n", pdd->lds_limit);
		dev_dbg(kfd_device,
			"gpuvm_base %llX\n", pdd->gpuvm_base);
		dev_dbg(kfd_device,
			"gpuvm_limit %llX\n", pdd->gpuvm_limit);
		dev_dbg(kfd_device,
			"scratch_base %llX\n", pdd->scratch_base);
		dev_dbg(kfd_device,
			"scratch_limit %llX\n", pdd->scratch_limit);
	}
	mutex_unlock(&p->mutex);

	args->num_of_nodes = i;
	ret = copy_to_user(
			(void __user *)args->kfd_process_device_apertures_ptr,
			pa,
			(i * sizeof(struct kfd_process_device_apertures)));
	kfree(pa);
	return ret ? -EFAULT : 0;

out_unlock:
	mutex_unlock(&p->mutex);
	return 0;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c b/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
index f9631f4b1a02..55aa74cbc532 100644
--- a/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
+++ b/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
@@ -779,8 +779,8 @@ static int kfd_ioctl_get_process_apertures_new(struct file *filp,
 	 * nodes, but not more than args->num_of_nodes as that is
 	 * the amount of memory allocated by user
 	 */
-	pa = kzalloc((sizeof(struct kfd_process_device_apertures) *
-				args->num_of_nodes), GFP_KERNEL);
+	pa = kcalloc(args->num_of_nodes, sizeof(struct kfd_process_device_apertures),
+		     GFP_KERNEL);
 	if (!pa)
 		return -ENOMEM;

```


## Bug Pattern

Manually multiplying count by element size when allocating an array with kmalloc/kzalloc:
ptr = kzalloc(count * sizeof(*ptr), GFP_KERNEL);
This risks integer overflow in the size calculation, leading to undersized allocations and subsequent out-of-bounds writes/reads. Use kcalloc(count, sizeof(*ptr), GFP_KERNEL) which performs overflow checking.


# Report

BuildSource:| drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
### Report Summary

File:|
/work/SemWeaver/artifacts/external/linux/drivers/gpu/drm/amd/amdgpu/../amdkfd/kfd_chardev.c  
---|---  
Warning:| line 1895, column 13  
Use kcalloc(count, size, ...) instead of count*sizeof in allocation to avoid
integer overflow  
  
### Annotated Source Code


1830  |  return ret;
1831  | }
1832  |  
1833  | static uint32_t get_process_num_bos(struct kfd_process *p)
1834  | {
1835  | 	uint32_t num_of_bos = 0;
1836  |  int i;
1837  |  
1838  |  /* Run over all PDDs of the process */
1839  |  for (i = 0; i < p->n_pdds; i++) {
1840  |  struct kfd_process_device *pdd = p->pdds[i];
1841  |  void *mem;
1842  |  int id;
1843  |  
1844  |  idr_for_each_entry(&pdd->alloc_idr, mem, id) {
1845  |  struct kgd_mem *kgd_mem = (struct kgd_mem *)mem;
1846  |  
1847  |  if (!kgd_mem->va || kgd_mem->va > pdd->gpuvm_base)
1848  | 				num_of_bos++;
1849  | 		}
1850  | 	}
1851  |  return num_of_bos;
1852  | }
1853  |  
1854  | static int criu_get_prime_handle(struct kgd_mem *mem,
1855  |  int flags, u32 *shared_fd)
1856  | {
1857  |  struct dma_buf *dmabuf;
1858  |  int ret;
1859  |  
1860  | 	ret = amdgpu_amdkfd_gpuvm_export_dmabuf(mem, &dmabuf);
1861  |  if (ret) {
1862  |  pr_err("dmabuf export failed for the BO\n");
1863  |  return ret;
1864  | 	}
1865  |  
1866  | 	ret = dma_buf_fd(dmabuf, flags);
1867  |  if (ret < 0) {
1868  |  pr_err("dmabuf create fd failed, ret:%d\n", ret);
1869  |  goto out_free_dmabuf;
1870  | 	}
1871  |  
1872  | 	*shared_fd = ret;
1873  |  return 0;
1874  |  
1875  | out_free_dmabuf:
1876  | 	dma_buf_put(dmabuf);
1877  |  return ret;
1878  | }
1879  |  
1880  | static int criu_checkpoint_bos(struct kfd_process *p,
1881  | 			       uint32_t num_bos,
1882  | 			       uint8_t __user *user_bos,
1883  | 			       uint8_t __user *user_priv_data,
1884  | 			       uint64_t *priv_offset)
1885  | {
1886  |  struct kfd_criu_bo_bucket *bo_buckets;
1887  |  struct kfd_criu_bo_priv_data *bo_privs;
1888  |  int ret = 0, pdd_index, bo_index = 0, id;
1889  |  void *mem;
1890  |  
1891  | 	bo_buckets = kvzalloc(num_bos * sizeof(*bo_buckets), GFP_KERNEL);
1892  |  if (!bo_buckets)
    1Assuming 'bo_buckets' is non-null→
    2←Taking false branch→
1893  |  return -ENOMEM;
1894  |  
1895  |  bo_privs = kvzalloc(num_bos * sizeof(*bo_privs), GFP_KERNEL);
    3←Use kcalloc(count, size, ...) instead of count*sizeof in allocation to avoid integer overflow
1896  |  if (!bo_privs) {
1897  | 		ret = -ENOMEM;
1898  |  goto exit;
1899  | 	}
1900  |  
1901  |  for (pdd_index = 0; pdd_index < p->n_pdds; pdd_index++) {
1902  |  struct kfd_process_device *pdd = p->pdds[pdd_index];
1903  |  struct amdgpu_bo *dumper_bo;
1904  |  struct kgd_mem *kgd_mem;
1905  |  
1906  |  idr_for_each_entry(&pdd->alloc_idr, mem, id) {
1907  |  struct kfd_criu_bo_bucket *bo_bucket;
1908  |  struct kfd_criu_bo_priv_data *bo_priv;
1909  |  int i, dev_idx = 0;
1910  |  
1911  |  if (!mem) {
1912  | 				ret = -ENOMEM;
1913  |  goto exit;
1914  | 			}
1915  |  
1916  | 			kgd_mem = (struct kgd_mem *)mem;
1917  | 			dumper_bo = kgd_mem->bo;
1918  |  
1919  |  /* Skip checkpointing BOs that are used for Trap handler
1920  |  * code and state. Currently, these BOs have a VA that
1921  |  * is less GPUVM Base
1922  |  */
1923  |  if (kgd_mem->va && kgd_mem->va <= pdd->gpuvm_base)
1924  |  continue;
1925  |  

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
