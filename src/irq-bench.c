// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Steve Jeong <steve@how2flow.net>
 */

/*
 * This driver is a kernel module to measure the interrupt latency
 * when the linux kernel goes up to guest in a hypervisor interrupt virtualization environment.
 * Interrupts triggered by software generation, such as ipi and lpi,
 * are benchmarked by default during the kernel boot process.
 * This is because there is uncertainty in the bench due to process preemption
 * during kernel runtime.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/smp.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MAX_COUNT 6
#define GIC_DIST_PRIO 0x400
#define GIC_PPI 0
#define GIC_SPI 1

struct bench_list {
	const char *type;
	bool valid;
	int irq;
	int stats;
	int target_cpu;
	int total_ns;
	ktime_t start_time;
	ktime_t end_time;
	void (*setup)(struct bench_list *list);
	void (*start)(struct bench_list *list);
	void (*end)(struct bench_list *list);
};

static struct attribute **sysfs_attrs;
static struct bench_list *benchmark_list;
static struct kobject *irq_kobj;

static void ipi_bench_handler(void *info);
static irqreturn_t spi_bench_handler(int irq, void *dev_id);

static void __iomem *gic_base;
static void __iomem *pic_base;

static int benchmark_count;
static int bench_times = 5000;

static int get_hwirq_from_virq(int virq)
{
	struct irq_desc *desc = irq_to_desc(virq);
	struct irq_data *data;

	if (!desc) {
		pr_err("No irq_desc for VIRQ %u\n", virq);
		return -EINVAL;
	}

	data = irq_desc_get_irq_data(desc);
	if (!data) {
		pr_err("No irq_data for VIRQ %u\n", virq);
		return -EINVAL;
	}

	return (int)irqd_to_hwirq(data);
}

/* IPI benchmark functions */
static void ipi_bench_setup(struct bench_list *list)
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

	list->stats = 0;
}

static void ipi_bench_end(struct bench_list *list)
{
	list->end_time = ktime_get();
	list->total_ns = ktime_to_ns(ktime_sub(list->end_time, list->start_time));
	list->valid = true;
}

static void ipi_bench_start(struct bench_list *list)
{
	int ntimes = 0, ret;

	while (ntimes < bench_times) {
		ret = smp_call_function_single(list->target_cpu, ipi_bench_handler, NULL, true);
		if (ret) {
			pr_err("IPI call failed: %d\n", ret);
			return;
		}
		ntimes++;
	}

	ipi_bench_end(list);
}

/* SPI benchmark functions */
static void spi_bench_setup(struct bench_list *list)
{
	uint32_t offset;
	uint32_t value;
	int hwirq;

	hwirq = get_hwirq_from_virq(list->irq);

	offset = (hwirq / 4) * 0x4;
	value = readl_relaxed(gic_base + GIC_DIST_PRIO + offset);
	value |= BIT(((hwirq % 4) * 0x8));
	writel_relaxed(value, gic_base + GIC_DIST_PRIO + offset);

	list->stats = 0;
}

static void spi_bench_end(struct bench_list *list)
{
	uint32_t offset;
	uint32_t value;
	int hwirq;

	/* time stamp */
	list->end_time = ktime_get();
	list->total_ns = ktime_to_ns(ktime_sub(list->end_time, list->start_time));
	list->valid = true;

	hwirq = get_hwirq_from_virq(list->irq);

	/* convert irqnr to pic index */
	offset = ((hwirq - 32) / 32) * 0x4;
	value = readl_relaxed(pic_base + offset);
	value &= ~BIT(hwirq % 32);
	writel_relaxed(value, pic_base + offset);
}

static void spi_bench_start(struct bench_list *list)
{
	uint32_t offset;
	uint32_t value;
	int hwirq;

	hwirq = get_hwirq_from_virq(list->irq);

	/* convert irqnr to pic index */
	offset = ((hwirq - 32) / 32) * 0x4;
	value = readl_relaxed(pic_base + offset);
	value |= BIT(hwirq % 32);
	writel_relaxed(value, pic_base + offset);
}

/* Benchmark command definitions */
static struct bench_list ipi_bench = {
	.type = "ipi",
	.setup = ipi_bench_setup,
	.start = ipi_bench_start,
	.end = ipi_bench_end,
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
	benchmark_list[GIC_SPI].stats++;
	if (benchmark_list[GIC_SPI].stats >= bench_times)
		spi_bench_end(&benchmark_list[GIC_SPI]);

	return IRQ_HANDLED;
}

static void ipi_bench_handler(void *info)
{
	/* Dummy ipi handler */
}

/* Run a benchmark */
static void run_benchmark(struct bench_list *list)
{
	list->setup(list);
	list->start_time = ktime_get();
	list->start(list);
}

/* Sysfs attribute handlers */
static ssize_t get_bench_result(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const char *type = attr->attr.name;
	int i;

	for (i = 0; i < benchmark_count; i++) {
		if (strcmp(type, benchmark_list[i].type) == 0) {
			if (!benchmark_list[i].valid)
				return scnprintf(buf, PAGE_SIZE, "No valid %s benchmark result\n", type);
			return scnprintf(buf, PAGE_SIZE, "Benchmark: %s\nTotal time: %d ns\n",
							 benchmark_list[i].type, benchmark_list[i].total_ns);
		}
	}

	return scnprintf(buf, PAGE_SIZE, "Invalid benchmark type: %s\n", type);
}

static ssize_t set_bench_start(struct kobject *kobj, struct kobj_attribute *attr,
							  const char *buf, size_t count)
{
	char irq_bench_type[MAX_COUNT];
	int i;

	if (count >= MAX_COUNT)
		return -EINVAL;

	strscpy(irq_bench_type, buf, count + 1);
	strim(irq_bench_type);

	for (i = 0; i < benchmark_count; i++) {
		if (strcmp(irq_bench_type, benchmark_list[i].type) == 0) {
			run_benchmark(&benchmark_list[i]);
			while (!benchmark_list[i].valid) {
				/* Wait until the benchmark completes */
			}
			return count;
		}
	}

	return -EINVAL;
}

static ssize_t get_bench_times(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", bench_times);
}

static ssize_t set_bench_times(struct kobject *kobj, struct kobj_attribute *attr,
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
static struct kobj_attribute ipi_result_attr = __ATTR(ipi, 0444, get_bench_result, NULL);
static struct kobj_attribute spi_result_attr = __ATTR(spi, 0444, get_bench_result, NULL);
static struct kobj_attribute bench_start_attr = __ATTR(set_bench, 0200, NULL, set_bench_start);
static struct kobj_attribute bench_times_attr = __ATTR(times, 0644, get_bench_times, set_bench_times);

static struct attribute_group attr_group = {
	.attrs = NULL,
};

/* Setup sysfs attributes dynamically */
static int setup_sysfs_attrs(void)
{
	int attr_count = benchmark_count + 2;

	sysfs_attrs = kzalloc((attr_count + 1) * sizeof(struct attribute *), GFP_KERNEL);
	if (!sysfs_attrs)
		return -ENOMEM;

	sysfs_attrs[0] = &ipi_result_attr.attr;
	sysfs_attrs[1] = &bench_start_attr.attr;
	sysfs_attrs[2] = &bench_times_attr.attr;
	if (pic_base)
		sysfs_attrs[3] = &spi_result_attr.attr;

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
		pr_err("No GIC memory resource found\n");
		return -EINVAL;
	} else {
		gic_base = devm_ioremap_resource(&pdev->dev, res);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		pr_warn("No PIC memory resource found\n");
		pr_warn("There is no spi benchmark!\n");
	} else {
		pic_base = devm_ioremap_resource(&pdev->dev, res);
	}

	/* Check PIC accessibility */
	benchmark_count = pic_base ? 2 : 1;

	/* Allocate bench commands */
	benchmark_list = kzalloc(benchmark_count * sizeof(struct bench_list), GFP_KERNEL);
	if (!benchmark_list) {
		kfree(benchmark_list);
		return -ENOMEM;
	}

	/* Initialize bench commands */
	benchmark_list[GIC_PPI] = ipi_bench;
	if (pic_base) {
		benchmark_list[GIC_SPI] = spi_bench;
		benchmark_list[GIC_SPI].irq = platform_get_irq(pdev, 0);
		if (benchmark_list[GIC_SPI].irq < 0) {
			pr_err("Failed to get IRQ: %d\n", benchmark_list[GIC_SPI].irq);
			goto cleanup_sysfs;
		}

		ret = request_irq(benchmark_list[GIC_SPI].irq, spi_bench_handler, IRQF_SHARED, "spi_bench", pdev);
		if (ret) {
			pr_err("Failed to request IRQ %d: %d\n", benchmark_list[GIC_SPI].irq, ret);
			goto cleanup_sysfs;
		}
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

	/* Run benchmarks at boot */
	for (i = 0; i < benchmark_count; i++)
		run_benchmark(&benchmark_list[i]);

	dev_info(&pdev->dev, "irq-bench module loaded (PIC_BASE %s, IRQ %d)\n",
			 pic_base ? "accessible" : "not accessible", benchmark_list[GIC_SPI].irq);

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
		free_irq(benchmark_list[GIC_SPI].irq, pdev);

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
