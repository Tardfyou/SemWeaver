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

spi: mchp-pci1xxx: Fix a possible null pointer dereference in pci1xxx_spi_probe

In function pci1xxxx_spi_probe, there is a potential null pointer that
may be caused by a failed memory allocation by the function devm_kzalloc.
Hence, a null pointer check needs to be added to prevent null pointer
dereferencing later in the code.

To fix this issue, spi_bus->spi_int[iter] should be checked. The memory
allocated by devm_kzalloc will be automatically released, so just directly
return -ENOMEM without worrying about memory leaks.

Fixes: 1cc0cbea7167 ("spi: microchip: pci1xxxx: Add driver for SPI controller of PCI1XXXX PCIe switch")
Signed-off-by: Huai-Yuan Liu <qq810974084@gmail.com>
Link: https://msgid.link/r/20240403014221.969801-1-qq810974084@gmail.com
Signed-off-by: Mark Brown <broonie@kernel.org>

## Buggy Code

```c
// Function: pci1xxxx_spi_probe in drivers/spi/spi-pci1xxxx.c
static int pci1xxxx_spi_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	u8 hw_inst_cnt, iter, start, only_sec_inst;
	struct pci1xxxx_spi_internal *spi_sub_ptr;
	struct device *dev = &pdev->dev;
	struct pci1xxxx_spi *spi_bus;
	struct spi_controller *spi_host;
	u32 regval;
	int ret;

	hw_inst_cnt = ent->driver_data & 0x0f;
	start = (ent->driver_data & 0xf0) >> 4;
	if (start == 1)
		only_sec_inst = 1;
	else
		only_sec_inst = 0;

	spi_bus = devm_kzalloc(&pdev->dev,
			       struct_size(spi_bus, spi_int, hw_inst_cnt),
			       GFP_KERNEL);
	if (!spi_bus)
		return -ENOMEM;

	spi_bus->dev = pdev;
	spi_bus->total_hw_instances = hw_inst_cnt;
	pci_set_master(pdev);

	for (iter = 0; iter < hw_inst_cnt; iter++) {
		spi_bus->spi_int[iter] = devm_kzalloc(&pdev->dev,
						      sizeof(struct pci1xxxx_spi_internal),
						      GFP_KERNEL);
		spi_sub_ptr = spi_bus->spi_int[iter];
		spi_sub_ptr->spi_host = devm_spi_alloc_host(dev, sizeof(struct spi_controller));
		if (!spi_sub_ptr->spi_host)
			return -ENOMEM;

		spi_sub_ptr->parent = spi_bus;
		spi_sub_ptr->spi_xfer_in_progress = false;

		if (!iter) {
			ret = pcim_enable_device(pdev);
			if (ret)
				return -ENOMEM;

			ret = pci_request_regions(pdev, DRV_NAME);
			if (ret)
				return -ENOMEM;

			spi_bus->reg_base = pcim_iomap(pdev, 0, pci_resource_len(pdev, 0));
			if (!spi_bus->reg_base) {
				ret = -EINVAL;
				goto error;
			}

			ret = pci_alloc_irq_vectors(pdev, hw_inst_cnt, hw_inst_cnt,
						    PCI_IRQ_ALL_TYPES);
			if (ret < 0) {
				dev_err(&pdev->dev, "Error allocating MSI vectors\n");
				goto error;
			}

			init_completion(&spi_sub_ptr->spi_xfer_done);
			/* Initialize Interrupts - SPI_INT */
			regval = readl(spi_bus->reg_base +
				       SPI_MST_EVENT_MASK_REG_OFFSET(spi_sub_ptr->hw_inst));
			regval &= ~SPI_INTR;
			writel(regval, spi_bus->reg_base +
			       SPI_MST_EVENT_MASK_REG_OFFSET(spi_sub_ptr->hw_inst));
			spi_sub_ptr->irq = pci_irq_vector(pdev, 0);

			ret = devm_request_irq(&pdev->dev, spi_sub_ptr->irq,
					       pci1xxxx_spi_isr, PCI1XXXX_IRQ_FLAGS,
					       pci_name(pdev), spi_sub_ptr);
			if (ret < 0) {
				dev_err(&pdev->dev, "Unable to request irq : %d",
					spi_sub_ptr->irq);
				ret = -ENODEV;
				goto error;
			}

			ret = pci1xxxx_spi_dma_init(spi_bus, spi_sub_ptr->irq);
			if (ret && ret != -EOPNOTSUPP)
				goto error;

			/* This register is only applicable for 1st instance */
			regval = readl(spi_bus->reg_base + SPI_PCI_CTRL_REG_OFFSET(0));
			if (!only_sec_inst)
				regval |= (BIT(4));
			else
				regval &= ~(BIT(4));

			writel(regval, spi_bus->reg_base + SPI_PCI_CTRL_REG_OFFSET(0));
		}

		spi_sub_ptr->hw_inst = start++;

		if (iter == 1) {
			init_completion(&spi_sub_ptr->spi_xfer_done);
			/* Initialize Interrupts - SPI_INT */
			regval = readl(spi_bus->reg_base +
			       SPI_MST_EVENT_MASK_REG_OFFSET(spi_sub_ptr->hw_inst));
			regval &= ~SPI_INTR;
			writel(regval, spi_bus->reg_base +
			       SPI_MST_EVENT_MASK_REG_OFFSET(spi_sub_ptr->hw_inst));
			spi_sub_ptr->irq = pci_irq_vector(pdev, iter);
			ret = devm_request_irq(&pdev->dev, spi_sub_ptr->irq,
					       pci1xxxx_spi_isr, PCI1XXXX_IRQ_FLAGS,
					       pci_name(pdev), spi_sub_ptr);
			if (ret < 0) {
				dev_err(&pdev->dev, "Unable to request irq : %d",
					spi_sub_ptr->irq);
				ret = -ENODEV;
				goto error;
			}
		}

		spi_host = spi_sub_ptr->spi_host;
		spi_host->num_chipselect = SPI_CHIP_SEL_COUNT;
		spi_host->mode_bits = SPI_MODE_0 | SPI_MODE_3 | SPI_RX_DUAL |
				      SPI_TX_DUAL | SPI_LOOP;
		spi_host->can_dma = pci1xxxx_spi_can_dma;
		spi_host->transfer_one = pci1xxxx_spi_transfer_one;

		spi_host->set_cs = pci1xxxx_spi_set_cs;
		spi_host->bits_per_word_mask = SPI_BPW_MASK(8);
		spi_host->max_speed_hz = PCI1XXXX_SPI_MAX_CLOCK_HZ;
		spi_host->min_speed_hz = PCI1XXXX_SPI_MIN_CLOCK_HZ;
		spi_host->flags = SPI_CONTROLLER_MUST_TX;
		spi_controller_set_devdata(spi_host, spi_sub_ptr);
		ret = devm_spi_register_controller(dev, spi_host);
		if (ret)
			goto error;
	}
	pci_set_drvdata(pdev, spi_bus);

	return 0;

error:
	pci_release_regions(pdev);
	return ret;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/spi/spi-pci1xxxx.c b/drivers/spi/spi-pci1xxxx.c
index 969965d7bc98..cc18d320370f 100644
--- a/drivers/spi/spi-pci1xxxx.c
+++ b/drivers/spi/spi-pci1xxxx.c
@@ -725,6 +725,8 @@ static int pci1xxxx_spi_probe(struct pci_dev *pdev, const struct pci_device_id *
 		spi_bus->spi_int[iter] = devm_kzalloc(&pdev->dev,
 						      sizeof(struct pci1xxxx_spi_internal),
 						      GFP_KERNEL);
+		if (!spi_bus->spi_int[iter])
+			return -ENOMEM;
 		spi_sub_ptr = spi_bus->spi_int[iter];
 		spi_sub_ptr->spi_host = devm_spi_alloc_host(dev, sizeof(struct spi_controller));
 		if (!spi_sub_ptr->spi_host)
```


## Bug Pattern

Dereferencing the result of a devm_* allocation without checking for NULL.

Pattern:
- Allocate a sub-structure with devm_kzalloc (or similar devm_* allocator) and immediately use it (e.g., via a local alias) without validating the allocation succeeded.

Example pattern:
```
p = devm_kzalloc(dev, size, GFP_KERNEL);
q = p;                  // alias
q->field = ...;         // NULL dereference if allocation failed
```

This commonly appears in probe paths or loops allocating per-instance objects (e.g., array elements) where the pointer (spi_bus->spi_int[iter]) is used before an explicit NULL check.


# Report

BuildSource:| drivers/spi/spi-pci1xxxx.c
### Report Summary

File:| spi-pci1xxxx.c  
---|---  
Warning:| line 728, column 25  
Unchecked devm allocation may be NULL and is dereferenced  
  
### Annotated Source Code


647   |  writel(regval, p->parent->dma_offset_bar + SPI_DMA_INTR_RD_CLR);
648   |  
649   |  /* Clear the DMA WR INT */
650   | 	regval = readl(p->parent->dma_offset_bar + SPI_DMA_INTR_WR_STS);
651   |  if (regval & SPI_DMA_DONE_INT_MASK) {
652   |  if (regval & SPI_DMA_CH0_DONE_INT)
653   | 			pci1xxxx_spi_setup_next_dma_transfer(p->parent->spi_int[SPI0]);
654   |  
655   |  if (regval & SPI_DMA_CH1_DONE_INT)
656   | 			pci1xxxx_spi_setup_next_dma_transfer(p->parent->spi_int[SPI1]);
657   |  
658   | 		spi_int_fired = IRQ_HANDLED;
659   | 	}
660   |  if (regval & SPI_DMA_ABORT_INT_MASK) {
661   | 		p->dma_aborted_wr = true;
662   | 		spi_int_fired = IRQ_HANDLED;
663   | 	}
664   |  writel(regval, p->parent->dma_offset_bar + SPI_DMA_INTR_WR_CLR);
665   | 	spin_unlock_irqrestore(&p->parent->dma_reg_lock, flags);
666   |  
667   |  /* Clear the SPI GO_BIT Interrupt */
668   | 	regval = readl(p->parent->reg_base + SPI_MST_EVENT_REG_OFFSET(p->hw_inst));
669   |  if (regval & SPI_INTR) {
670   |  writel(p->hw_inst, p->parent->dma_offset_bar + SPI_DMA_WR_DOORBELL_REG);
671   | 		spi_int_fired = IRQ_HANDLED;
672   | 	}
673   |  writel(regval, p->parent->reg_base + SPI_MST_EVENT_REG_OFFSET(p->hw_inst));
674   |  return spi_int_fired;
675   | }
676   |  
677   | static irqreturn_t pci1xxxx_spi_isr(int irq, void *dev)
678   | {
679   |  struct pci1xxxx_spi_internal *p = dev;
680   |  
681   |  if (p->spi_host->can_dma(p->spi_host, NULL, p->xfer))
682   |  return pci1xxxx_spi_isr_dma(irq, dev);
683   |  else
684   |  return pci1xxxx_spi_isr_io(irq, dev);
685   | }
686   |  
687   | static bool pci1xxxx_spi_can_dma(struct spi_controller *host,
688   |  struct spi_device *spi,
689   |  struct spi_transfer *xfer)
690   | {
691   |  struct pci1xxxx_spi_internal *p = spi_controller_get_devdata(host);
692   |  struct pci1xxxx_spi *par = p->parent;
693   |  
694   |  return par->can_dma;
695   | }
696   |  
697   | static int pci1xxxx_spi_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
698   | {
699   | 	u8 hw_inst_cnt, iter, start, only_sec_inst;
700   |  struct pci1xxxx_spi_internal *spi_sub_ptr;
701   |  struct device *dev = &pdev->dev;
702   |  struct pci1xxxx_spi *spi_bus;
703   |  struct spi_controller *spi_host;
704   | 	u32 regval;
705   |  int ret;
706   |  
707   | 	hw_inst_cnt = ent->driver_data & 0x0f;
708   | 	start = (ent->driver_data & 0xf0) >> 4;
709   |  if (start == 1)
    1Assuming 'start' is not equal to 1→
    2←Taking false branch→
710   | 		only_sec_inst = 1;
711   |  else
712   |  only_sec_inst = 0;
713   |  
714   |  spi_bus = devm_kzalloc(&pdev->dev,
715   |  struct_size(spi_bus, spi_int, hw_inst_cnt),
716   |  GFP_KERNEL);
717   |  if (!spi_bus)
    3←Assuming 'spi_bus' is non-null→
    4←Taking false branch→
718   |  return -ENOMEM;
719   |  
720   |  spi_bus->dev = pdev;
721   | 	spi_bus->total_hw_instances = hw_inst_cnt;
722   | 	pci_set_master(pdev);
723   |  
724   |  for (iter = 0; iter < hw_inst_cnt; iter++) {
    5←Assuming 'iter' is < 'hw_inst_cnt'→
    6←Loop condition is true.  Entering loop body→
725   |  spi_bus->spi_int[iter] = devm_kzalloc(&pdev->dev,
726   |  sizeof(struct pci1xxxx_spi_internal),
727   |  GFP_KERNEL);
728   |  if (!spi_bus->spi_int[iter])
    7←Unchecked devm allocation may be NULL and is dereferenced
729   |  return -ENOMEM;
730   | 		spi_sub_ptr = spi_bus->spi_int[iter];
731   | 		spi_sub_ptr->spi_host = devm_spi_alloc_host(dev, sizeof(struct spi_controller));
732   |  if (!spi_sub_ptr->spi_host)
733   |  return -ENOMEM;
734   |  
735   | 		spi_sub_ptr->parent = spi_bus;
736   | 		spi_sub_ptr->spi_xfer_in_progress = false;
737   |  
738   |  if (!iter) {
739   | 			ret = pcim_enable_device(pdev);
740   |  if (ret)
741   |  return -ENOMEM;
742   |  
743   | 			ret = pci_request_regions(pdev, DRV_NAME);
744   |  if (ret)
745   |  return -ENOMEM;
746   |  
747   | 			spi_bus->reg_base = pcim_iomap(pdev, 0, pci_resource_len(pdev, 0));
748   |  if (!spi_bus->reg_base) {
749   | 				ret = -EINVAL;
750   |  goto error;
751   | 			}
752   |  
753   | 			ret = pci_alloc_irq_vectors(pdev, hw_inst_cnt, hw_inst_cnt,
754   |  PCI_IRQ_ALL_TYPES);
755   |  if (ret < 0) {
756   |  dev_err(&pdev->dev, "Error allocating MSI vectors\n");
757   |  goto error;
758   | 			}

# Formatting

Please provide your answer in the following format:

- Decision: {Bug/NotABug}
- Reason: {Your reason here}
