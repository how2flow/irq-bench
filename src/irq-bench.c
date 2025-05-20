// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Steve Jeong <steve@how2flow.net>
 */

/*
 * This driver is a kernel module to measure the interrupt latency
 * when the linux kernel goes up to guest in a hypervisor interrupt virtualization environment.
 * Interrupts triggered by software generation, such as sgi(ipi) and lpi,
 * are benchmarked by default during the kernel boot process.
 * This is because there is uncertainty in the bench due to process preemption
 * during kernel runtime.
 */

#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>

#include "irq-bench.h"


static int bench_times = 5000;

static int bench_of_irq_init(struct platform_device *pdev, struct bench_list *list)
{
	int ret;

	list->irq = platform_get_irq(pdev, 0);
	if (list->irq < 0) {
		pr_err("Failed to get irq-bench irqnr %d\n", list->irq);
		return -EINVAL;
	}

	list->desc = irq_to_desc(list->irq);
	if (!list->desc) {
		pr_err("No irq_desc for IRQ %u\n", list->irq);
		return -EINVAL;
	}

	list->chip = irq_desc_get_chip(list->desc);
	if (!list->chip) {
		pr_err("No irq_chip for IRQ %u\n", list->irq);
		return -EINVAL;
	}

	list->data = irq_desc_get_irq_data(list->desc);
	if (!list->data) {
		pr_err("No irq_data for IRQ %u\n", list->irq);
		return -EINVAL;
	}

	irq_bench_hwirq = irqd_to_hwirq(list->data);

	ret = request_irq(list->irq, spi_bench_handler, IRQF_SHARED, "spi_bench", pdev);
	if (ret) {
		pr_err("Failed to request IRQ %d: %d\n", list->irq, ret);
		return -EINVAL;
	}

	return 0;
}

/* SGI benchmark functions */
static void sgi_bench_setup(struct bench_list *list)
{
	int this_cpu = smp_processor_id();
	int cpu;

	list->target_cpu = -1;
	for_each_online_cpu(cpu) {
		if (cpu != this_cpu) {
			list->target_cpu = cpu;
			break;
		}
	}

	if (list->target_cpu < 0) {
		pr_warn("No suitable target CPU found, using current CPU %d\n", this_cpu);
		list->target_cpu = this_cpu;
	}

	list->valid = false;
	list->stats = 0;
}

static void sgi_bench_start(struct bench_list *list)
{
	int ntimes, ret;

	for (ntimes = 0; bench_times > ntimes; ntimes++) {
		ret = smp_call_function_single(list->target_cpu, sgi_bench_handler, NULL, true);
		if (ret) {
			pr_err("IPI call failed: %d\n", ret);
			break;
		}
	}
}

static void sgi_bench_end(struct bench_list *list)
{
	list->end_time = ktime_get();
	list->total_ns = ktime_to_ns(ktime_sub(list->end_time, list->start_time));
	list->valid = true;
}

static void __maybe_unused spi_bench_prio(irq_hw_number_t irq)
{
	uint32_t mask, offset, value;

	offset = (irq / 4) * 0x4;
	value = readl_relaxed(gic_base + GIC_DIST_PRIO + offset);
	mask = 0xFF << ((irq % 4) * 8);
	value = (value & ~mask) | (PRIORITY_VALUE << ((irq % 4) * 8));
	writel_relaxed(value, gic_base + GIC_DIST_PRIO + offset);
}

/* SPI benchmark functions */
static void spi_bench_setup(struct bench_list *list)
{
	irq_hw_number_t hwirq;

	hwirq = irq_bench_hwirq;

	if (gic_base)
		spi_bench_prio(hwirq);

	list->valid = false;
	list->stats = 0;
}

static void spi_bench_start(struct bench_list *list)
{
	uint32_t offset, value;
	irq_hw_number_t hwirq;

	hwirq = irq_bench_hwirq;
	/* convert irqnr to pic index */
	offset = ((hwirq - 32) / 32) * 0x4;
	value = readl_relaxed(pic_base + offset);
	value |= BIT((hwirq - 32) % 32);
	writel_relaxed(value, pic_base + offset);
}

static void spi_bench_end(struct bench_list *list)
{
	uint32_t offset, value;
	irq_hw_number_t hwirq;

	/* time stamp */
	list->end_time = ktime_get();
	list->total_ns = ktime_to_ns(ktime_sub(list->end_time, list->start_time));
	list->valid = true;

	hwirq = irq_bench_hwirq;

	/* convert irqnr to pic index */
	offset = ((hwirq - 32) / 32) * 0x4;
	value = readl_relaxed(pic_base + offset);
	value &= ~BIT((hwirq - 32) % 32);
	writel_relaxed(value, pic_base + offset);
	dsb(sy);
}

/* Benchmark list definitions */
static struct bench_list sgi_bench = {
	.type = "sgi",
	.setup = sgi_bench_setup,
	.start = sgi_bench_start,
	.end = sgi_bench_end,
};

static struct bench_list spi_bench = {
	.type = "spi",
	.setup = spi_bench_setup,
	.start = spi_bench_start,
	.end = spi_bench_end,
};

/* Interrupt handlers */
static irqreturn_t spi_bench_handler(int irq, void *dev_id)
{
	benchmark_list[BENCH_SPI].stats++;
	if (benchmark_list[BENCH_SPI].stats >= bench_times)
		spi_bench_end(&benchmark_list[BENCH_SPI]);

	return IRQ_HANDLED;
}

static void sgi_bench_handler(void *info)
{
	/* Dummy ipi handler */
}

/* Run a benchmark */
static void run_benchmark(struct bench_list *list)
{
	if (strcmp(list->type, "")) {
		list->setup(list);
		list->start_time = ktime_get();
		list->start(list);
		if (!list->irq) {
			/* It can be terminated in a polling */
			list->end(list);
		}
	} else {
		/* Unable to support under certain conditions */
		pr_warn("irq-gench: No Support bench type..\n");
	}
}

/* Sysfs attribute handlers */
static ssize_t get_bench_result(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const char *type = attr->attr.name;
	int i;

	for (i = 0; i < BENCH_TYPES; i++) {
		if (!strcmp(type, benchmark_list[i].type)) {
			if (benchmark_list[i].valid) {
			return scnprintf(buf, PAGE_SIZE, "Benchmark: %s count: %d\nTotal time: %d ns  |  Avg: %d ns\n",
			                 benchmark_list[i].type, bench_times,
			                 benchmark_list[i].total_ns,
			                 (benchmark_list[i].total_ns / bench_times));
			}
		}
	}

	return scnprintf(buf, PAGE_SIZE, "Invalid or Still %s benchmarking in progress.\n", type);
}

static ssize_t set_benchmark(struct kobject *kobj, struct kobj_attribute *attr,
							  const char *buf, size_t count)
{
	char irq_bench_type[MAX_COUNT];
	int i;

	if (count >= MAX_COUNT)
		return -EINVAL;

	strscpy(irq_bench_type, buf, count + 1);
	strim(irq_bench_type);

	for (i = 0; i < BENCH_TYPES; i++) {
		if (!strcmp(irq_bench_type, benchmark_list[i].type)) {
			/* setup -> start -> end */
			run_benchmark(&benchmark_list[i]);
		}
	}

	return count;
}

static ssize_t get_benchmark_times(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", bench_times);
}

static ssize_t set_benchmark_times(struct kobject *kobj, struct kobj_attribute *attr,
							  const char *buf, size_t count)
{
	int ret, new_times;

	if (count >= MAX_COUNT) {
		pr_err("Input too long, max %d bytes\n", MAX_COUNT - 1);
		return -EINVAL;
	}

	ret = kstrtoint(strim((char *)buf), 10, &new_times);
	if (ret || new_times <= 0) {
		pr_err("Invalid bench times: must be > 0\n");
		return -EINVAL;
	}
	bench_times = new_times;

	return count;
}

/* Sysfs attributes */
static struct kobj_attribute bench_setup_attr = __ATTR(benchmark, 0200, NULL, set_benchmark);
static struct kobj_attribute bench_times_attr = __ATTR(times, 0644, get_benchmark_times, set_benchmark_times);
static struct kobj_attribute sgi_result_attr = __ATTR(sgi, 0444, get_bench_result, NULL);
static struct kobj_attribute spi_result_attr = __ATTR(spi, 0444, get_bench_result, NULL);

static struct attribute_group attr_group = {
	.attrs = NULL,
};

/* Setup sysfs attributes dynamically */
static int setup_sysfs_attrs(void)
{
	int attr_count = BENCH_TYPES + BENCH_COMMON;

	sysfs_attrs = kzalloc((attr_count + 1) * sizeof(struct attribute *), GFP_KERNEL);
	if (!sysfs_attrs)
		return -ENOMEM;

	/* benchmark irqs */
	sysfs_attrs[BENCH_SGI] = &sgi_result_attr.attr;
	if (pic_base)
		sysfs_attrs[BENCH_SPI] = &spi_result_attr.attr;

	/* benchmark common */
	sysfs_attrs[BENCH_TYPES + BENCH_SETUP] = &bench_setup_attr.attr;
	sysfs_attrs[BENCH_TYPES + BENCH_TIMES] = &bench_times_attr.attr;

	/* Null termination */
	sysfs_attrs[attr_count] = NULL;

	/* Alloc attr gruops */
	attr_group.attrs = sysfs_attrs;

	return 0;
}

/* Platform driver probe function */
static int irq_bench_probe(struct platform_device *pdev)
{
	int ret, i;
	struct resource *res;

	/* Initialize IO maps */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_warn("No PIC memory resource found\n");
		pr_warn("There is no spi benchmark!\n");
	} else {
		pic_base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		pr_warn("No GIC memory resource found\n");
		pr_warn("There is no spi prio benchmark!\n");
	} else {
		gic_base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	}

	/* Allocate bench commands */
	benchmark_list = kzalloc(BENCH_TYPES * sizeof(struct bench_list), GFP_KERNEL);
	if (!benchmark_list) {
		kfree(benchmark_list);
		return -ENOMEM;
	}

	/* Setup sysfs attributes */
	if (setup_sysfs_attrs()) {
		pr_err("Failed to setup sysfs attributes\n");
		goto cleanup_memory;
	}

	/* Create sysfs kobject */
	irq_kobj = kobject_create_and_add("irq-bench", kernel_kobj);
	if (!irq_kobj) {
		pr_err("Failed to create kobject\n");
		goto cleanup_attrs;
	}

	/* Create sysfs group */
	ret = sysfs_create_group(irq_kobj, &attr_group);
	if (ret) {
		pr_err("Failed to create sysfs group: %d\n", ret);
		goto cleanup_kobj;
	}

	/* Initialize bench commands */
	benchmark_list[BENCH_SGI] = sgi_bench;
	if (pic_base) {
		benchmark_list[BENCH_SPI] = spi_bench;
	} else {
		/* No support SPI benchmark */
		benchmark_list[BENCH_SPI].type = '\0';
	}

	/* Initialize IRQ data */
	if (pic_base) {
		ret = bench_of_irq_init(pdev, &benchmark_list[BENCH_SPI]);
		if (ret < 0) {
			pr_err("Failed to initializing irq-bench %d: %d\n", benchmark_list[BENCH_SPI].irq, ret);
			goto cleanup_sysfs;
		}
	}

	/* Run benchmarks at boot */
	for (i = 0; i < BENCH_TYPES; i++)
		run_benchmark(&benchmark_list[i]);

	dev_info(&pdev->dev, "irq-bench module loaded (PIC_BASE %s, IRQ %d)\n",
			 pic_base ? "accessible" : "not accessible", benchmark_list[BENCH_SPI].irq);

	return 0;

cleanup_sysfs:
	sysfs_remove_group(irq_kobj, &attr_group);
cleanup_kobj:
	kobject_put(irq_kobj);
cleanup_attrs:
	kfree(sysfs_attrs);
cleanup_memory:
	kfree(benchmark_list);
	return ret;
}

/* Platform driver remove function */
static int irq_bench_remove(struct platform_device *pdev)
{
	if (irq_kobj) {
		sysfs_remove_group(irq_kobj, &attr_group);
		kobject_put(irq_kobj);
	}

	if (pic_base)
		free_irq(benchmark_list[BENCH_SPI].irq, pdev);

	kfree(sysfs_attrs);
	kfree(benchmark_list);
	dev_info(&pdev->dev, "irq-bench module unloaded\n");

	return 0;
}

/* Platform driver definition */
static const struct of_device_id irq_bench_of_match[] = {
	{ .compatible = "generic,irq-bench" },
	{ }
};
MODULE_DEVICE_TABLE(of, irq_bench_of_match);

static struct platform_driver irq_bench_driver = {
	.probe = irq_bench_probe,
	.remove = irq_bench_remove,
	.driver = {
		.name = "irq-bench",
		.of_match_table = irq_bench_of_match,
	},
};

/* Module initialization and cleanup */
module_platform_driver(irq_bench_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("Enhanced interrupt benchmark module with dynamic SPI support");
MODULE_VERSION("1.0");
