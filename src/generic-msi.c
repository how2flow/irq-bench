// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Steve Jeong <steve@how2flow.net>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include "generic-msi.h"

#define DRIVER_NAME "dummy_driver"


int shared_lpi_irqnr;

struct dummy_dev {
	void __iomem *base;
	int msi_irq;
};

static irqreturn_t dummy_msi_irq_handler(int irq, void *dev_id)
{
	pr_info("hello (MSI LPI)\n");

	return IRQ_HANDLED;
}

static void dummy_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	/* In a real implementation, msg would be programmed with the ITS doorbell
	 * address and the allocated LPI vector.
	 * For this dummy driver, Do nothing.
	 */
}

static int dummy_msi_probe(struct platform_device *pdev)
{
	struct dummy_dev *dev;
	struct resource *res;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get memory resource\n");
		return -ENODEV;
	}

	dev->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	if (!pdev->dev.msi.domain) {
		dev_err(&pdev->dev,
		        "MSI domain is not set; please check the msi-parent property in DT\n");
		return -ENODEV;
	}

	/*
	 * Allocate one MSI IRQ using the upstream API.
	 * The device tree's "msi-parent" property is used by the MSI parent driver
	 * (irq-gic-v3-its-msi-parent.c) to parse the ITS phandle and MSI specifier.
	 */
	ret = platform_msi_domain_alloc_irqs(&pdev->dev, 1, dummy_write_msi_msg);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to allocate MSI IRQs: %d\n", ret);
		return ret;
	}

	dev->msi_irq=msi_get_virq(&pdev->dev, 0); // msi_irq = desc->irq; (desc = first_msi_entry(&pdev->dev);)
	shared_lpi_irqnr = dev->msi_irq;

	ret = devm_request_irq(&pdev->dev, dev->msi_irq, dummy_msi_irq_handler,
	                       0, DRIVER_NAME, dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to request MSI IRQ %d, ret: %d\n", dev->msi_irq, ret);
		return ret;
	}

	platform_set_drvdata(pdev, dev);
	dev_info(&pdev->dev, "Dummy MSI device probed, allocated MSI IRQ %d\n", dev->msi_irq);

	return 0;
}

static int dummy_msi_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Dummy MSI device removed\n");

	return 0;
}

static const struct of_device_id dummy_msi_of_match[] = {
	{ .compatible = "dummy,dev", },
	{ },
};

MODULE_DEVICE_TABLE(of, dummy_msi_of_match);

static struct platform_driver dummy_msi_driver = {
	.probe  = dummy_msi_probe,
	.remove = dummy_msi_remove,
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = dummy_msi_of_match,
	},
};

module_platform_driver(dummy_msi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong");
MODULE_DESCRIPTION("Dummy device driver using ITS-based MSI (LPIs)");
