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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

static int __init irq_bench_init(void)
{
	printk(KERN_INFO "IRQ Bench: Initialized\n");
	return 0;
}

static void __exit irq_bench_exit(void)
{
	printk(KERN_INFO "IRQ Bench: Exited\n");
}

module_init(irq_bench_init);
module_exit(irq_bench_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("IRQ Benchmark Module");
MODULE_VERSION("1.0");
