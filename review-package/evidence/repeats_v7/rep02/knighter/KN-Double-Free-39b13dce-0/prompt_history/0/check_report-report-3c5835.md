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

The bug pattern is the redundant cleanup invocation in the error path where an automatic cleanup mechanism is already in place. In this patch, the cleanup function (scmi_debugfs_common_cleanup) is called explicitly after devm_add_action_or_reset() fails, even though devm_add_action_or_reset() already ensures that the cleanup is performed. This redundancy leads to a double free of the resource dbg->name. The underlying issue is mixing manual cleanup calls with device-managed (automatic) cleanup actions, which can result in freeing memory twice.

## Bug Pattern

The bug pattern is the redundant cleanup invocation in the error path where an automatic cleanup mechanism is already in place. In this patch, the cleanup function (scmi_debugfs_common_cleanup) is called explicitly after devm_add_action_or_reset() fails, even though devm_add_action_or_reset() already ensures that the cleanup is performed. This redundancy leads to a double free of the resource dbg->name. The underlying issue is mixing manual cleanup calls with device-managed (automatic) cleanup actions, which can result in freeing memory twice.

# Report

BuildSource:| drivers/firmware/arm_scmi/driver.c
### Report Summary

File:| driver.c  
---|---  
Warning:| line 2978, column 6  
Redundant cleanup call leads to double free  
  
### Annotated Source Code


2527  |  int i;
2528  |  struct scmi_xfer *xfer;
2529  |  struct device *dev = sinfo->dev;
2530  |  const struct scmi_desc *desc = sinfo->desc;
2531  |  
2532  |  /* Pre-allocated messages, no more than what hdr.seq can support */
2533  |  if (WARN_ON(!info->max_msg || info->max_msg > MSG_TOKEN_MAX)) {
2534  |  dev_err(dev,
2535  |  "Invalid maximum messages %d, not in range [1 - %lu]\n",
2536  |  info->max_msg, MSG_TOKEN_MAX);
2537  |  return -EINVAL;
2538  | 	}
2539  |  
2540  |  hash_init(info->pending_xfers);
2541  |  
2542  |  /* Allocate a bitmask sized to hold MSG_TOKEN_MAX tokens */
2543  | 	info->xfer_alloc_table = devm_bitmap_zalloc(dev, MSG_TOKEN_MAX,
2544  |  GFP_KERNEL);
2545  |  if (!info->xfer_alloc_table)
2546  |  return -ENOMEM;
2547  |  
2548  |  /*
2549  |  * Preallocate a number of xfers equal to max inflight messages,
2550  |  * pre-initialize the buffer pointer to pre-allocated buffers and
2551  |  * attach all of them to the free list
2552  |  */
2553  |  INIT_HLIST_HEAD(&info->free_xfers);
2554  |  for (i = 0; i < info->max_msg; i++) {
2555  | 		xfer = devm_kzalloc(dev, sizeof(*xfer), GFP_KERNEL);
2556  |  if (!xfer)
2557  |  return -ENOMEM;
2558  |  
2559  | 		xfer->rx.buf = devm_kcalloc(dev, sizeof(u8), desc->max_msg_size,
2560  |  GFP_KERNEL);
2561  |  if (!xfer->rx.buf)
2562  |  return -ENOMEM;
2563  |  
2564  | 		xfer->tx.buf = xfer->rx.buf;
2565  | 		init_completion(&xfer->done);
2566  |  spin_lock_init(&xfer->lock);
2567  |  
2568  |  /* Add initialized xfer to the free list */
2569  | 		hlist_add_head(&xfer->node, &info->free_xfers);
2570  | 	}
2571  |  
2572  |  spin_lock_init(&info->xfer_lock);
2573  |  
2574  |  return 0;
2575  | }
2576  |  
2577  | static int scmi_channels_max_msg_configure(struct scmi_info *sinfo)
2578  | {
2579  |  const struct scmi_desc *desc = sinfo->desc;
2580  |  
2581  |  if (!desc->ops->get_max_msg) {
2582  | 		sinfo->tx_minfo.max_msg = desc->max_msg;
2583  | 		sinfo->rx_minfo.max_msg = desc->max_msg;
2584  | 	} else {
2585  |  struct scmi_chan_info *base_cinfo;
2586  |  
2587  | 		base_cinfo = idr_find(&sinfo->tx_idr, SCMI_PROTOCOL_BASE);
2588  |  if (!base_cinfo)
2589  |  return -EINVAL;
2590  | 		sinfo->tx_minfo.max_msg = desc->ops->get_max_msg(base_cinfo);
2591  |  
2592  |  /* RX channel is optional so can be skipped */
2593  | 		base_cinfo = idr_find(&sinfo->rx_idr, SCMI_PROTOCOL_BASE);
2594  |  if (base_cinfo)
2595  | 			sinfo->rx_minfo.max_msg =
2596  | 				desc->ops->get_max_msg(base_cinfo);
2597  | 	}
2598  |  
2599  |  return 0;
2600  | }
2601  |  
2602  | static int scmi_xfer_info_init(struct scmi_info *sinfo)
2603  | {
2604  |  int ret;
2605  |  
2606  | 	ret = scmi_channels_max_msg_configure(sinfo);
2607  |  if (ret)
2608  |  return ret;
2609  |  
2610  | 	ret = __scmi_xfer_info_init(sinfo, &sinfo->tx_minfo);
2611  |  if (!ret && !idr_is_empty(&sinfo->rx_idr))
2612  | 		ret = __scmi_xfer_info_init(sinfo, &sinfo->rx_minfo);
2613  |  
2614  |  return ret;
2615  | }
2616  |  
2617  | static int scmi_chan_setup(struct scmi_info *info, struct device_node *of_node,
2618  |  int prot_id, bool tx)
2619  | {
2620  |  int ret, idx;
2621  |  char name[32];
2622  |  struct scmi_chan_info *cinfo;
2623  |  struct idr *idr;
2624  |  struct scmi_device *tdev = NULL;
2625  |  
2626  |  /* Transmit channel is first entry i.e. index 0 */
2627  | 	idx = tx ? 0 : 1;
2628  | 	idr = tx ? &info->tx_idr : &info->rx_idr;
2629  |  
2630  |  if (!info->desc->ops->chan_available(of_node, idx)) {
2631  | 		cinfo = idr_find(idr, SCMI_PROTOCOL_BASE);
2632  |  if (unlikely(!cinfo)) /* Possible only if platform has no Rx */
2633  |  return -EINVAL;
2634  |  goto idr_alloc;
2635  | 	}
2636  |  
2637  | 	cinfo = devm_kzalloc(info->dev, sizeof(*cinfo), GFP_KERNEL);
2638  |  if (!cinfo)
2639  |  return -ENOMEM;
2640  |  
2641  | 	cinfo->rx_timeout_ms = info->desc->max_rx_timeout_ms;
2642  |  
2643  |  /* Create a unique name for this transport device */
2644  | 	snprintf(name, 32, "__scmi_transport_device_%s_%02X",
2869  |  "err_msg_unexpected",
2870  |  "err_msg_invalid",
2871  |  "err_msg_nomem",
2872  |  "err_protocol",
2873  | };
2874  |  
2875  | static ssize_t reset_all_on_write(struct file *filp, const char __user *buf,
2876  | 				  size_t count, loff_t *ppos)
2877  | {
2878  |  struct scmi_debug_info *dbg = filp->private_data;
2879  |  
2880  |  for (int i = 0; i < SCMI_DEBUG_COUNTERS_LAST; i++)
2881  | 		atomic_set(&dbg->counters[i], 0);
2882  |  
2883  |  return count;
2884  | }
2885  |  
2886  | static const struct file_operations fops_reset_counts = {
2887  | 	.owner = THIS_MODULE,
2888  | 	.open = simple_open,
2889  | 	.write = reset_all_on_write,
2890  | };
2891  |  
2892  | static void scmi_debugfs_counters_setup(struct scmi_debug_info *dbg,
2893  |  struct dentry *trans)
2894  | {
2895  |  struct dentry *counters;
2896  |  int idx;
2897  |  
2898  | 	counters = debugfs_create_dir("counters", trans);
2899  |  
2900  |  for (idx = 0; idx < SCMI_DEBUG_COUNTERS_LAST; idx++)
2901  | 		debugfs_create_atomic_t(dbg_counter_strs[idx], 0600, counters,
2902  | 					&dbg->counters[idx]);
2903  |  
2904  | 	debugfs_create_file("reset", 0200, counters, dbg, &fops_reset_counts);
2905  | }
2906  |  
2907  | static void scmi_debugfs_common_cleanup(void *d)
2908  | {
2909  |  struct scmi_debug_info *dbg = d;
2910  |  
2911  |  if (!dbg)
2912  |  return;
2913  |  
2914  |  debugfs_remove_recursive(dbg->top_dentry);
2915  | 	kfree(dbg->name);
2916  | 	kfree(dbg->type);
2917  | }
2918  |  
2919  | static struct scmi_debug_info *scmi_debugfs_common_setup(struct scmi_info *info)
2920  | {
2921  |  char top_dir[16];
2922  |  struct dentry *trans, *top_dentry;
2923  |  struct scmi_debug_info *dbg;
2924  |  const char *c_ptr = NULL;
2925  |  
2926  | 	dbg = devm_kzalloc(info->dev, sizeof(*dbg), GFP_KERNEL);
2927  |  if (!dbg)
    19←Assuming 'dbg' is non-null→
    20←Taking false branch→
2928  |  return NULL;
2929  |  
2930  |  dbg->name = kstrdup(of_node_full_name(info->dev->of_node), GFP_KERNEL);
2931  |  if (!dbg->name) {
    21←Assuming field 'name' is non-null→
    22←Taking false branch→
2932  | 		devm_kfree(info->dev, dbg);
2933  |  return NULL;
2934  | 	}
2935  |  
2936  |  of_property_read_string(info->dev->of_node, "compatible", &c_ptr);
2937  | 	dbg->type = kstrdup(c_ptr, GFP_KERNEL);
2938  |  if (!dbg->type) {
    23←Assuming field 'type' is non-null→
    24←Taking false branch→
2939  | 		kfree(dbg->name);
2940  | 		devm_kfree(info->dev, dbg);
2941  |  return NULL;
2942  | 	}
2943  |  
2944  |  snprintf(top_dir, 16, "%d", info->id);
2945  | 	top_dentry = debugfs_create_dir(top_dir, scmi_top_dentry);
2946  | 	trans = debugfs_create_dir("transport", top_dentry);
2947  |  
2948  |  dbg->is_atomic = info->desc->atomic_enabled &&
    25←Assuming field 'atomic_enabled' is false→
2949  | 				is_transport_polling_capable(info->desc);
2950  |  
2951  | 	debugfs_create_str("instance_name", 0400, top_dentry,
2952  | 			   (char **)&dbg->name);
2953  |  
2954  | 	debugfs_create_u32("atomic_threshold_us", 0400, top_dentry,
2955  | 			   &info->atomic_threshold);
2956  |  
2957  | 	debugfs_create_str("type", 0400, trans, (char **)&dbg->type);
2958  |  
2959  | 	debugfs_create_bool("is_atomic", 0400, trans, &dbg->is_atomic);
2960  |  
2961  | 	debugfs_create_u32("max_rx_timeout_ms", 0400, trans,
2962  | 			   (u32 *)&info->desc->max_rx_timeout_ms);
2963  |  
2964  | 	debugfs_create_u32("max_msg_size", 0400, trans,
2965  | 			   (u32 *)&info->desc->max_msg_size);
2966  |  
2967  | 	debugfs_create_u32("tx_max_msg", 0400, trans,
2968  | 			   (u32 *)&info->tx_minfo.max_msg);
2969  |  
2970  |  debugfs_create_u32("rx_max_msg", 0400, trans,
2971  | 			   (u32 *)&info->rx_minfo.max_msg);
2972  |  
2973  |  if (IS_ENABLED(CONFIG_ARM_SCMI_DEBUG_COUNTERS))
    26←Taking true branch→
2974  |  scmi_debugfs_counters_setup(dbg, trans);
2975  |  
2976  |  dbg->top_dentry = top_dentry;
2977  |  
2978  |  if (devm_add_action_or_reset(info->dev,
    27←Redundant cleanup call leads to double free
2979  |  scmi_debugfs_common_cleanup, dbg))
2980  |  return NULL;
2981  |  
2982  |  return dbg;
2983  | }
2984  |  
2985  | static int scmi_debugfs_raw_mode_setup(struct scmi_info *info)
2986  | {
2987  |  int id, num_chans = 0, ret = 0;
2988  |  struct scmi_chan_info *cinfo;
2989  | 	u8 channels[SCMI_MAX_CHANNELS] = {};
2990  |  DECLARE_BITMAP(protos, SCMI_MAX_CHANNELS) = {};
2991  |  
2992  |  if (!info->dbg)
2993  |  return -EINVAL;
2994  |  
2995  |  /* Enumerate all channels to collect their ids */
2996  |  idr_for_each_entry(&info->tx_idr, cinfo, id) {
2997  |  /*
2998  |  * Cannot happen, but be defensive.
2999  |  * Zero as num_chans is ok, warn and carry on.
3000  |  */
3001  |  if (num_chans >= SCMI_MAX_CHANNELS || !cinfo) {
3002  |  dev_warn(info->dev,
3003  |  "SCMI RAW - Error enumerating channels\n");
3004  |  break;
3005  | 		}
3006  |  
3007  |  if (!test_bit(cinfo->id, protos)) {
3008  | 			channels[num_chans++] = cinfo->id;
3009  | 			set_bit(cinfo->id, protos);
3010  | 		}
3011  | 	}
3012  |  
3013  | 	info->raw = scmi_raw_mode_init(&info->handle, info->dbg->top_dentry,
3014  | 				       info->id, channels, num_chans,
3015  | 				       info->desc, info->tx_minfo.max_msg);
3016  |  if (IS_ERR(info->raw)) {
3017  |  dev_err(info->dev, "Failed to initialize SCMI RAW Mode !\n");
3018  | 		ret = PTR_ERR(info->raw);
3019  | 		info->raw = NULL;
3020  | 	}
3021  |  
3022  |  return ret;
3023  | }
3024  |  
3025  | static const struct scmi_desc *scmi_transport_setup(struct device *dev)
3026  | {
3027  |  struct scmi_transport *trans;
3028  |  int ret;
3029  |  
3030  | 	trans = dev_get_platdata(dev);
3031  |  if (!trans || !trans->desc || !trans->supplier || !trans->core_ops)
3032  |  return NULL;
3033  |  
3034  |  if (!device_link_add(dev, trans->supplier, DL_FLAG_AUTOREMOVE_CONSUMER)) {
3035  |  dev_err(dev,
3036  |  "Adding link to supplier transport device failed\n");
3037  |  return NULL;
3038  | 	}
3039  |  
3040  |  /* Provide core transport ops */
3041  | 	*trans->core_ops = &scmi_trans_core_ops;
3042  |  
3043  |  dev_info(dev, "Using %s\n", dev_driver_string(trans->supplier));
3044  |  
3045  | 	ret = of_property_read_u32(dev->of_node, "max-rx-timeout-ms",
3046  | 				   &trans->desc->max_rx_timeout_ms);
3047  |  if (ret && ret != -EINVAL)
3048  |  dev_err(dev, "Malformed max-rx-timeout-ms DT property.\n");
3049  |  
3050  |  dev_info(dev, "SCMI max-rx-timeout: %dms\n",
3051  |  trans->desc->max_rx_timeout_ms);
3052  |  
3053  |  return trans->desc;
3054  | }
3055  |  
3056  | static int scmi_probe(struct platform_device *pdev)
3057  | {
3058  |  int ret;
3059  |  char *err_str = "probe failure\n";
3060  |  struct scmi_handle *handle;
3061  |  const struct scmi_desc *desc;
3062  |  struct scmi_info *info;
3063  | 	bool coex = IS_ENABLED(CONFIG_ARM_SCMI_RAW_MODE_SUPPORT_COEX);
3064  |  struct device *dev = &pdev->dev;
3065  |  struct device_node *child, *np = dev->of_node;
3066  |  
3067  | 	desc = scmi_transport_setup(dev);
3068  |  if (!desc0.1'desc' is non-null) {
    1Taking false branch→
3069  | 		err_str = "transport invalid\n";
3070  | 		ret = -EINVAL;
3071  |  goto out_err;
3072  | 	}
3073  |  
3074  |  info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
3075  |  if (!info)
    2←Assuming 'info' is non-null→
    3←Taking false branch→
3076  |  return -ENOMEM;
3077  |  
3078  |  info->id = ida_alloc_min(&scmi_id, 0, GFP_KERNEL);
3079  |  if (info->id < 0)
    4←Assuming field 'id' is >= 0→
    5←Taking false branch→
3080  |  return info->id;
3081  |  
3082  |  info->dev = dev;
3083  | 	info->desc = desc;
3084  | 	info->bus_nb.notifier_call = scmi_bus_notifier;
3085  | 	info->dev_req_nb.notifier_call = scmi_device_request_notifier;
3086  | 	INIT_LIST_HEAD(&info->node);
3087  |  idr_init(&info->protocols);
3088  |  mutex_init(&info->protocols_mtx);
    6←Loop condition is false.  Exiting loop→
3089  |  idr_init(&info->active_protocols);
3090  |  mutex_init(&info->devreq_mtx);
    7←Loop condition is false.  Exiting loop→
3091  |  
3092  |  platform_set_drvdata(pdev, info);
3093  | 	idr_init(&info->tx_idr);
3094  | 	idr_init(&info->rx_idr);
3095  |  
3096  | 	handle = &info->handle;
3097  | 	handle->dev = info->dev;
3098  | 	handle->version = &info->version;
3099  | 	handle->devm_protocol_acquire = scmi_devm_protocol_acquire;
3100  | 	handle->devm_protocol_get = scmi_devm_protocol_get;
3101  | 	handle->devm_protocol_put = scmi_devm_protocol_put;
3102  |  
3103  |  /* System wide atomic threshold for atomic ops .. if any */
3104  |  if (!of_property_read_u32(np, "atomic-threshold-us",
    8←Taking false branch→
3105  | 				  &info->atomic_threshold))
3106  |  dev_info(dev,
3107  |  "SCMI System wide atomic threshold set to %d us\n",
3108  |  info->atomic_threshold);
3109  |  handle->is_transport_atomic = scmi_is_transport_atomic;
3110  |  
3111  |  /* Setup all channels described in the DT at first */
3112  | 	ret = scmi_channels_setup(info);
3113  |  if (ret) {
    9←Assuming 'ret' is 0→
    10←Taking false branch→
3114  | 		err_str = "failed to setup channels\n";
3115  |  goto clear_ida;
3116  | 	}
3117  |  
3118  |  ret = bus_register_notifier(&scmi_bus_type, &info->bus_nb);
3119  |  if (ret) {
    11←Assuming 'ret' is 0→
    12←Taking false branch→
3120  | 		err_str = "failed to register bus notifier\n";
3121  |  goto clear_txrx_setup;
3122  | 	}
3123  |  
3124  |  ret = blocking_notifier_chain_register(&scmi_requested_devices_nh,
3125  | 					       &info->dev_req_nb);
3126  |  if (ret) {
    13←Assuming 'ret' is 0→
    14←Taking false branch→
3127  | 		err_str = "failed to register device notifier\n";
3128  |  goto clear_bus_notifier;
3129  | 	}
3130  |  
3131  |  ret = scmi_xfer_info_init(info);
3132  |  if (ret14.1'ret' is 0) {
    15←Taking false branch→
3133  | 		err_str = "failed to init xfers pool\n";
3134  |  goto clear_dev_req_notifier;
3135  | 	}
3136  |  
3137  |  if (scmi_top_dentry) {
    16←Assuming 'scmi_top_dentry' is non-null→
    17←Taking true branch→
3138  |  info->dbg = scmi_debugfs_common_setup(info);
    18←Calling 'scmi_debugfs_common_setup'→
3139  |  if (!info->dbg)
3140  |  dev_warn(dev, "Failed to setup SCMI debugfs.\n");
3141  |  
3142  |  if (IS_ENABLED(CONFIG_ARM_SCMI_RAW_MODE_SUPPORT)) {
3143  | 			ret = scmi_debugfs_raw_mode_setup(info);
3144  |  if (!coex) {
3145  |  if (ret)
3146  |  goto clear_dev_req_notifier;
3147  |  
3148  |  /* Bail out anyway when coex disabled. */
3149  |  return 0;
3150  | 			}
3151  |  
3152  |  /* Coex enabled, carry on in any case. */
3153  |  dev_info(dev, "SCMI RAW Mode COEX enabled !\n");
3154  | 		}
3155  | 	}
3156  |  
3157  |  if (scmi_notification_init(handle))
3158  |  dev_err(dev, "SCMI Notifications NOT available.\n");
3159  |  
3160  |  if (info->desc->atomic_enabled &&
3161  | 	    !is_transport_polling_capable(info->desc))
3162  |  dev_err(dev,
3163  |  "Transport is not polling capable. Atomic mode not supported.\n");
3164  |  
3165  |  /*
3166  |  * Trigger SCMI Base protocol initialization.
3167  |  * It's mandatory and won't be ever released/deinit until the
3168  |  * SCMI stack is shutdown/unloaded as a whole.

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
