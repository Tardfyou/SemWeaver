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

smb: client: fix possible double free in smb2_set_ea()

Clang static checker(scan-build) warning：
fs/smb/client/smb2ops.c:1304:2: Attempt to free released memory.
 1304 |         kfree(ea);
      |         ^~~~~~~~~

There is a double free in such case:
'ea is initialized to NULL' -> 'first successful memory allocation for
ea' -> 'something failed, goto sea_exit' -> 'first memory release for ea'
-> 'goto replay_again' -> 'second goto sea_exit before allocate memory
for ea' -> 'second memory release for ea resulted in double free'.

Re-initialie 'ea' to NULL near to the replay_again label, it can fix this
double free problem.

Fixes: 4f1fffa23769 ("cifs: commands that are retried should have replay flag set")
Reviewed-by: Dan Carpenter <dan.carpenter@linaro.org>
Signed-off-by: Su Hui <suhui@nfschina.com>
Signed-off-by: Steve French <stfrench@microsoft.com>

## Buggy Code

```c
// Function: smb2_set_ea in fs/smb/client/smb2ops.c
static int
smb2_set_ea(const unsigned int xid, struct cifs_tcon *tcon,
	    const char *path, const char *ea_name, const void *ea_value,
	    const __u16 ea_value_len, const struct nls_table *nls_codepage,
	    struct cifs_sb_info *cifs_sb)
{
	struct smb2_compound_vars *vars;
	struct cifs_ses *ses = tcon->ses;
	struct TCP_Server_Info *server;
	struct smb_rqst *rqst;
	struct kvec *rsp_iov;
	__le16 *utf16_path = NULL;
	int ea_name_len = strlen(ea_name);
	int flags = CIFS_CP_CREATE_CLOSE_OP;
	int len;
	int resp_buftype[3];
	struct cifs_open_parms oparms;
	__u8 oplock = SMB2_OPLOCK_LEVEL_NONE;
	struct cifs_fid fid;
	unsigned int size[1];
	void *data[1];
	struct smb2_file_full_ea_info *ea = NULL;
	struct smb2_query_info_rsp *rsp;
	int rc, used_len = 0;
	int retries = 0, cur_sleep = 1;

replay_again:
	/* reinitialize for possible replay */
	flags = CIFS_CP_CREATE_CLOSE_OP;
	oplock = SMB2_OPLOCK_LEVEL_NONE;
	server = cifs_pick_channel(ses);

	if (smb3_encryption_required(tcon))
		flags |= CIFS_TRANSFORM_REQ;

	if (ea_name_len > 255)
		return -EINVAL;

	utf16_path = cifs_convert_path_to_utf16(path, cifs_sb);
	if (!utf16_path)
		return -ENOMEM;

	resp_buftype[0] = resp_buftype[1] = resp_buftype[2] = CIFS_NO_BUFFER;
	vars = kzalloc(sizeof(*vars), GFP_KERNEL);
	if (!vars) {
		rc = -ENOMEM;
		goto out_free_path;
	}
	rqst = vars->rqst;
	rsp_iov = vars->rsp_iov;

	if (ses->server->ops->query_all_EAs) {
		if (!ea_value) {
			rc = ses->server->ops->query_all_EAs(xid, tcon, path,
							     ea_name, NULL, 0,
							     cifs_sb);
			if (rc == -ENODATA)
				goto sea_exit;
		} else {
			/* If we are adding a attribute we should first check
			 * if there will be enough space available to store
			 * the new EA. If not we should not add it since we
			 * would not be able to even read the EAs back.
			 */
			rc = smb2_query_info_compound(xid, tcon, path,
				      FILE_READ_EA,
				      FILE_FULL_EA_INFORMATION,
				      SMB2_O_INFO_FILE,
				      CIFSMaxBufSize -
				      MAX_SMB2_CREATE_RESPONSE_SIZE -
				      MAX_SMB2_CLOSE_RESPONSE_SIZE,
				      &rsp_iov[1], &resp_buftype[1], cifs_sb);
			if (rc == 0) {
				rsp = (struct smb2_query_info_rsp *)rsp_iov[1].iov_base;
				used_len = le32_to_cpu(rsp->OutputBufferLength);
			}
			free_rsp_buf(resp_buftype[1], rsp_iov[1].iov_base);
			resp_buftype[1] = CIFS_NO_BUFFER;
			memset(&rsp_iov[1], 0, sizeof(rsp_iov[1]));
			rc = 0;

			/* Use a fudge factor of 256 bytes in case we collide
			 * with a different set_EAs command.
			 */
			if (CIFSMaxBufSize - MAX_SMB2_CREATE_RESPONSE_SIZE -
			   MAX_SMB2_CLOSE_RESPONSE_SIZE - 256 <
			   used_len + ea_name_len + ea_value_len + 1) {
				rc = -ENOSPC;
				goto sea_exit;
			}
		}
	}

	/* Open */
	rqst[0].rq_iov = vars->open_iov;
	rqst[0].rq_nvec = SMB2_CREATE_IOV_SIZE;

	oparms = (struct cifs_open_parms) {
		.tcon = tcon,
		.path = path,
		.desired_access = FILE_WRITE_EA,
		.disposition = FILE_OPEN,
		.create_options = cifs_create_options(cifs_sb, 0),
		.fid = &fid,
		.replay = !!(retries),
	};

	rc = SMB2_open_init(tcon, server,
			    &rqst[0], &oplock, &oparms, utf16_path);
	if (rc)
		goto sea_exit;
	smb2_set_next_command(tcon, &rqst[0]);


	/* Set Info */
	rqst[1].rq_iov = vars->si_iov;
	rqst[1].rq_nvec = 1;

	len = sizeof(*ea) + ea_name_len + ea_value_len + 1;
	ea = kzalloc(len, GFP_KERNEL);
	if (ea == NULL) {
		rc = -ENOMEM;
		goto sea_exit;
	}

	ea->ea_name_length = ea_name_len;
	ea->ea_value_length = cpu_to_le16(ea_value_len);
	memcpy(ea->ea_data, ea_name, ea_name_len + 1);
	memcpy(ea->ea_data + ea_name_len + 1, ea_value, ea_value_len);

	size[0] = len;
	data[0] = ea;

	rc = SMB2_set_info_init(tcon, server,
				&rqst[1], COMPOUND_FID,
				COMPOUND_FID, current->tgid,
				FILE_FULL_EA_INFORMATION,
				SMB2_O_INFO_FILE, 0, data, size);
	if (rc)
		goto sea_exit;
	smb2_set_next_command(tcon, &rqst[1]);
	smb2_set_related(&rqst[1]);

	/* Close */
	rqst[2].rq_iov = &vars->close_iov;
	rqst[2].rq_nvec = 1;
	rc = SMB2_close_init(tcon, server,
			     &rqst[2], COMPOUND_FID, COMPOUND_FID, false);
	if (rc)
		goto sea_exit;
	smb2_set_related(&rqst[2]);

	if (retries) {
		smb2_set_replay(server, &rqst[0]);
		smb2_set_replay(server, &rqst[1]);
		smb2_set_replay(server, &rqst[2]);
	}

	rc = compound_send_recv(xid, ses, server,
				flags, 3, rqst,
				resp_buftype, rsp_iov);
	/* no need to bump num_remote_opens because handle immediately closed */

 sea_exit:
	kfree(ea);
	SMB2_open_free(&rqst[0]);
	SMB2_set_info_free(&rqst[1]);
	SMB2_close_free(&rqst[2]);
	free_rsp_buf(resp_buftype[0], rsp_iov[0].iov_base);
	free_rsp_buf(resp_buftype[1], rsp_iov[1].iov_base);
	free_rsp_buf(resp_buftype[2], rsp_iov[2].iov_base);
	kfree(vars);
out_free_path:
	kfree(utf16_path);

	if (is_replayable_error(rc) &&
	    smb2_should_replay(tcon, &retries, &cur_sleep))
		goto replay_again;

	return rc;
}
```

## Bug Fix Patch

```diff
diff --git a/fs/smb/client/smb2ops.c b/fs/smb/client/smb2ops.c
index 6b385fce3f2a..24a2aa04a108 100644
--- a/fs/smb/client/smb2ops.c
+++ b/fs/smb/client/smb2ops.c
@@ -1158,7 +1158,7 @@ smb2_set_ea(const unsigned int xid, struct cifs_tcon *tcon,
 	struct cifs_fid fid;
 	unsigned int size[1];
 	void *data[1];
-	struct smb2_file_full_ea_info *ea = NULL;
+	struct smb2_file_full_ea_info *ea;
 	struct smb2_query_info_rsp *rsp;
 	int rc, used_len = 0;
 	int retries = 0, cur_sleep = 1;
@@ -1179,6 +1179,7 @@ smb2_set_ea(const unsigned int xid, struct cifs_tcon *tcon,
 	if (!utf16_path)
 		return -ENOMEM;

+	ea = NULL;
 	resp_buftype[0] = resp_buftype[1] = resp_buftype[2] = CIFS_NO_BUFFER;
 	vars = kzalloc(sizeof(*vars), GFP_KERNEL);
 	if (!vars) {
```


## Bug Pattern

Retry/replay loop with shared cleanup that frees a pointer without resetting it to NULL:

replay_again:
    ...
    // ptr not reinitialized here after previous cleanup
    if (early_error)
        goto out;
    ptr = kmalloc(...);
    if (!ptr)
        goto out;
    ...
out:
    kfree(ptr);              // frees ptr
    if (should_retry)
        goto replay_again;   // next iteration may hit 'out' before re-allocating ptr -> kfree(ptr) again

Root cause: A pointer freed in a common error/cleanup label is not reinitialized before a retry path jumps back, allowing a second kfree of the stale, already-freed pointer if an error occurs before reallocation.


# Report

BuildSource:| fs/smb/client/smb2ops.c
### Report Summary

File:| smb2ops.c  
---|---  
Warning:| line 1318, column 3  
Pointer freed in cleanup but not reset before retry; possible double free on
replay path  
Note:| line 1314, column 2  
Freed pointer: utf16_path  
  
### Annotated Source Code


1264  | 	}
1265  |  
1266  | 	ea->ea_name_length = ea_name_len;
1267  | 	ea->ea_value_length = cpu_to_le16(ea_value_len);
1268  |  memcpy(ea->ea_data, ea_name, ea_name_len + 1);
1269  |  memcpy(ea->ea_data + ea_name_len + 1, ea_value, ea_value_len);
1270  |  
1271  | 	size[0] = len;
1272  | 	data[0] = ea;
1273  |  
1274  | 	rc = SMB2_set_info_init(tcon, server,
1275  | 				&rqst[1], COMPOUND_FID,
1276  |  COMPOUND_FID, current->tgid,
1277  |  FILE_FULL_EA_INFORMATION,
1278  |  SMB2_O_INFO_FILE, 0, data, size);
1279  |  if (rc)
1280  |  goto sea_exit;
1281  | 	smb2_set_next_command(tcon, &rqst[1]);
1282  | 	smb2_set_related(&rqst[1]);
1283  |  
1284  |  /* Close */
1285  | 	rqst[2].rq_iov = &vars->close_iov;
1286  | 	rqst[2].rq_nvec = 1;
1287  | 	rc = SMB2_close_init(tcon, server,
1288  | 			     &rqst[2], COMPOUND_FID, COMPOUND_FID, false);
1289  |  if (rc)
1290  |  goto sea_exit;
1291  | 	smb2_set_related(&rqst[2]);
1292  |  
1293  |  if (retries) {
1294  | 		smb2_set_replay(server, &rqst[0]);
1295  | 		smb2_set_replay(server, &rqst[1]);
1296  | 		smb2_set_replay(server, &rqst[2]);
1297  | 	}
1298  |  
1299  | 	rc = compound_send_recv(xid, ses, server,
1300  | 				flags, 3, rqst,
1301  | 				resp_buftype, rsp_iov);
1302  |  /* no need to bump num_remote_opens because handle immediately closed */
1303  |  
1304  |  sea_exit:
1305  | 	kfree(ea);
1306  | 	SMB2_open_free(&rqst[0]);
1307  | 	SMB2_set_info_free(&rqst[1]);
1308  | 	SMB2_close_free(&rqst[2]);
1309  | 	free_rsp_buf(resp_buftype[0], rsp_iov[0].iov_base);
1310  | 	free_rsp_buf(resp_buftype[1], rsp_iov[1].iov_base);
1311  | 	free_rsp_buf(resp_buftype[2], rsp_iov[2].iov_base);
1312  | 	kfree(vars);
1313  | out_free_path:
1314  | 	kfree(utf16_path);
1315  |  
1316  |  if (is_replayable_error(rc) &&
1317  | 	    smb2_should_replay(tcon, &retries, &cur_sleep))
1318  |  goto replay_again;
    Pointer freed in cleanup but not reset before retry; possible double free on replay path
1319  |  
1320  |  return rc;
1321  | }
1322  | #endif
1323  |  
1324  | static bool
1325  | smb2_can_echo(struct TCP_Server_Info *server)
1326  | {
1327  |  return server->echoes;
1328  | }
1329  |  
1330  | static void
1331  | smb2_clear_stats(struct cifs_tcon *tcon)
1332  | {
1333  |  int i;
1334  |  
1335  |  for (i = 0; i < NUMBER_OF_SMB2_COMMANDS; i++) {
1336  | 		atomic_set(&tcon->stats.smb2_stats.smb2_com_sent[i], 0);
1337  | 		atomic_set(&tcon->stats.smb2_stats.smb2_com_failed[i], 0);
1338  | 	}
1339  | }
1340  |  
1341  | static void
1342  | smb2_dump_share_caps(struct seq_file *m, struct cifs_tcon *tcon)
1343  | {
1344  | 	seq_puts(m, "\n\tShare Capabilities:");
1345  |  if (tcon->capabilities & SMB2_SHARE_CAP_DFS)
1346  | 		seq_puts(m, " DFS,");
1347  |  if (tcon->capabilities & SMB2_SHARE_CAP_CONTINUOUS_AVAILABILITY)
1348  | 		seq_puts(m, " CONTINUOUS AVAILABILITY,");

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
