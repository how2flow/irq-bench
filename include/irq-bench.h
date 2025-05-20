// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Steve Jeong <steve@how2flow.net>
 */

#ifndef _IRQ_BENCH_H
#define _IRQ_BENCH_H

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/sysfs.h>

#define MAX_COUNT 10
#define GIC_DIST_PRIO 0x400
#define PRIORITY_VALUE 0x20

enum bench_type {
	BENCH_EOI,
	BENCH_SGI,
	BENCH_SPI,
/* count termination */
	BENCH_TYPES,
};

enum bench_common {
	BENCH_SETUP,
	BENCH_TIMES,
/* count termination */
	BENCH_COMMON
};

struct bench_list {
	bool valid;
	const char *type;
	int irq;
	int stats;
	int target_cpu;
	int total_ns;
	ktime_t end_time;
	ktime_t start_time;
	void (*end)(struct bench_list *list);
	void (*setup)(struct bench_list *list);
	void (*start)(struct bench_list *list);
	struct irq_chip *chip;
	struct irq_data *data;
	struct irq_desc *desc;
};

static struct attribute **sysfs_attrs;
static struct bench_list *benchmark_list;
static struct kobject *irq_kobj;

static void sgi_bench_handler(void *info);
static irqreturn_t spi_bench_handler(int irq, void *dev_id);

static void __iomem *gic_base;
static void __iomem *pic_base;

static irq_hw_number_t irq_bench_hwirq;

#endif
