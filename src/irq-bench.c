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

#define MAX_COUNT 6
#define GIC_DIST_SET_EN 0x17600104
#define GIC_DIST_CLR_EN 0x17600184
#define GIC_DIST_PRIO 0x17600404
#define PIC_BASE_ADDR 0x14600100

struct bench_command {
	const char *type;
	void (*setup)(void);
	void (*start)(void);
	void (*free)(void);
};

struct bench_result {
	const char *type;
	int total_ns;
	bool valid;
	ktime_t start_time;
	ktime_t end_time;
};

struct bench_stats {
	int ipi_stats;
	int spi_stats;
	int target_cpu;
};

static struct kobject *irq_kobj;
static struct bench_stats irq_bench;
static struct bench_command *bench_commands;
static struct bench_result *bench_results;
static struct attribute **sysfs_attrs;

static void ipi_bench_handler(void *info);
static irqreturn_t spi_bench_handler(int irq, void *dev_id);

static bool pic_module_present;
static int bench_commands_count;
static int bench_times = 65535;
static int bench_irq;
static int bench_hwirq;

/* Helper function to read from a physical address */
static int read_phys_addr(phys_addr_t addr, uint32_t *value)
{
	void __iomem *virt_addr = ioremap(addr, sizeof(u32));
	if (!virt_addr) {
		pr_err("Failed to map address 0x%llx\n", (u64)addr);
		return -ENOMEM;
	}
	*value = readl(virt_addr);
	iounmap(virt_addr);

	return 0;
}

/* Helper function to write to a physical address */
static int write_phys_addr(phys_addr_t addr, uint32_t value)
{
	void __iomem *virt_addr = ioremap(addr, sizeof(u32));
	if (!virt_addr) {
		pr_err("Failed to map address 0x%llx\n", (u64)addr);
		return -ENOMEM;
	}
	writel(value, virt_addr);
	iounmap(virt_addr);

	return 0;
}

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


/* Check if PIC_BASE is accessible */
static int check_pic_access(void)
{
	uint32_t value;
	if (read_phys_addr(PIC_BASE_ADDR, &value)) {
		pr_info("PIC_BASE inaccessible, SPI benchmark disabled\n");
		return -ENOMEM;
	}

	return 0;
}

/* IPI benchmark functions */
static void ipi_bench_setup(void)
{
	int this_cpu = smp_processor_id();
	int cpu;

	irq_bench.target_cpu = -1;
	for_each_online_cpu(cpu) {
		if (cpu != this_cpu) {
			irq_bench.target_cpu = cpu;
			break;
		}
	}
	if (irq_bench.target_cpu < 0) {
		pr_warn("No suitable target CPU found, using current CPU %d\n", this_cpu);
		irq_bench.target_cpu = this_cpu;
	}
}

static void ipi_bench_start(void)
{
	int ntimes = 0, ret;

	while (ntimes < bench_times) {
		ret = smp_call_function_single(irq_bench.target_cpu, ipi_bench_handler, NULL, true);
		if (ret) {
			pr_err("IPI call failed: %d\n", ret);
			return;
		}
		ntimes++;
	}
}

static void ipi_bench_free(void)
{
	/* No-op for IPI */
}

/* SPI benchmark functions */
static void spi_bench_setup(void)
{
	uint32_t offset;
	uint32_t value;

	bench_hwirq = get_hwirq_from_virq(bench_irq);

	offset = (bench_hwirq / 4) * 0x4;
	value = (bench_hwirq % 4) * 0x8;
	write_phys_addr(GIC_DIST_PRIO + offset, 0x1 << value);
}

static void spi_bench_start(void)
{
	uint32_t offset;
	uint32_t value;

	bench_hwirq = get_hwirq_from_virq(bench_irq);

	offset = (bench_hwirq / 32) * 0x4;
	value = BIT(bench_hwirq % 32);

	write_phys_addr(PIC_BASE_ADDR + offset, value);
}

static void spi_bench_free(void)
{
	uint32_t offset;
	uint32_t value;

	bench_hwirq = get_hwirq_from_virq(bench_irq);

	offset = (bench_hwirq / 32) * 0x4;
	value = BIT(bench_hwirq % 32);
	write_phys_addr(PIC_BASE_ADDR + offset, 0);

	offset = (bench_hwirq / 4) * 0x4;
	value = (bench_hwirq % 4) * 0x8;
	write_phys_addr(GIC_DIST_PRIO + offset, 0xa0 << value);
}

/* Benchmark command definitions */
static struct bench_command ipi_command = {
	.type = "ipi",
	.setup = ipi_bench_setup,
	.start = ipi_bench_start,
	.free = ipi_bench_free
};

static struct bench_command spi_command = {
	.type = "spi",
	.setup = spi_bench_setup,
	.start = spi_bench_start,
	.free = spi_bench_free
};

/* Interrupt handlers */
static irqreturn_t spi_bench_handler(int irq, void *dev_id)
{
	irq_bench.spi_stats++;
	if (irq_bench.spi_stats >= bench_times)
		spi_bench_free();

	return IRQ_HANDLED;
}

static void ipi_bench_handler(void *info)
{
	irq_bench.ipi_stats++;
}

/* Run a benchmark */
static void run_benchmark(struct bench_command *bench)
{
	int i;
	for (i = 0; i < bench_commands_count; i++) {
		if (strcmp(bench->type, bench_commands[i].type) == 0) {
			bench_results[i].start_time = ktime_get();
			bench->setup();
			bench->start();
			bench_results[i].end_time = ktime_get();
			bench_results[i].total_ns = ktime_to_ns(ktime_sub(bench_results[i].end_time,
															 bench_results[i].start_time));
			bench_results[i].type = bench->type;
			bench_results[i].valid = true;
			bench->free();
			break;
		}
	}
}

/* Sysfs attribute handlers */
static ssize_t get_bench_result(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	const char *type = attr->attr.name;
	int i;

	for (i = 0; i < bench_commands_count; i++) {
		if (strcmp(type, bench_results[i].type) == 0) {
			if (!bench_results[i].valid)
				return scnprintf(buf, PAGE_SIZE, "No valid %s benchmark result\n", type);
			return scnprintf(buf, PAGE_SIZE, "Benchmark: %s\nTotal time: %d ns\n",
							 bench_results[i].type, bench_results[i].total_ns);
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

	for (i = 0; i < bench_commands_count; i++) {
		if (strcmp(irq_bench_type, bench_commands[i].type) == 0) {
			run_benchmark(&bench_commands[i]);
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
	int attr_count = bench_commands_count + 2;
	sysfs_attrs = kzalloc((attr_count + 1) * sizeof(struct attribute *), GFP_KERNEL);
	if (!sysfs_attrs)
		return -ENOMEM;

	sysfs_attrs[0] = &ipi_result_attr.attr;
	sysfs_attrs[1] = &bench_start_attr.attr;
	sysfs_attrs[2] = &bench_times_attr.attr;
	if (pic_module_present)
		sysfs_attrs[3] = &spi_result_attr.attr;

	attr_group.attrs = sysfs_attrs;

	return 0;
}

/* Platform driver probe function */
static int irq_bench_probe(struct platform_device *pdev)
{
	int ret, i;

	/* Check PIC accessibility */
	pic_module_present = (check_pic_access() == 0);
	bench_commands_count = pic_module_present ? 2 : 1;

	/* Allocate bench commands */
	bench_commands = kzalloc(bench_commands_count * sizeof(struct bench_command), GFP_KERNEL);
	if (!bench_commands)
		return -ENOMEM;

	/* Allocate bench results */
	bench_results = kzalloc(bench_commands_count * sizeof(struct bench_result), GFP_KERNEL);
	if (!bench_results) {
		kfree(bench_commands);
		return -ENOMEM;
	}

	/* Initialize bench commands */
	bench_commands[0] = ipi_command;
	if (pic_module_present)
		bench_commands[1] = spi_command;

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

	/* Request SPI IRQ if PIC is accessible */
	if (pic_module_present) {
		bench_irq = platform_get_irq(pdev, 0);
		if (bench_irq < 0) {
			pr_err("Failed to get IRQ: %d\n", bench_irq);
			ret = bench_irq;
			goto cleanup_sysfs;
		}

		ret = request_irq(bench_irq, spi_bench_handler, IRQF_SHARED, "spi_bench", pdev);
		if (ret) {
			pr_err("Failed to request IRQ %d: %d\n", bench_irq, ret);
			goto cleanup_sysfs;
		}
	}

	/* Initialize bench results */
	for (i = 0; i < bench_commands_count; i++) {
		bench_results[i].type = bench_commands[i].type;
		bench_results[i].valid = false;
	}

	/* Run benchmarks at boot */
	for (i = 0; i < bench_commands_count; i++)
		run_benchmark(&bench_commands[i]);

	dev_info(&pdev->dev, "irq-bench module loaded (PIC_BASE %s, IRQ %d)\n",
			 pic_module_present ? "accessible" : "not accessible", bench_irq);
	return 0;

cleanup_sysfs:
	sysfs_remove_group(irq_kobj, &attr_group);
cleanup_kobj:
	kobject_put(irq_kobj);
cleanup_attrs:
	kfree(sysfs_attrs);
cleanup_memory:
	kfree(bench_commands);
	kfree(bench_results);
	return ret;
}

/* Platform driver remove function */
static int irq_bench_remove(struct platform_device *pdev)
{
	if (irq_kobj) {
		sysfs_remove_group(irq_kobj, &attr_group);
		kobject_put(irq_kobj);
	}

	if (pic_module_present)
		free_irq(bench_irq, pdev);

	kfree(sysfs_attrs);
	kfree(bench_commands);
	kfree(bench_results);
	dev_info(&pdev->dev, "irq-bench module unloaded\n");

	return 0;
}

/* Platform driver definition */
static const struct of_device_id irq_bench_of_match[] = {
	{ .compatible = "benchmark,irq" },
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
