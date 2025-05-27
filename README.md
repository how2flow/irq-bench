# irq-bench

For irq benchmark in Linux

## Prerequisites

```
sudo apt-get install linux-headers-$(uname -r)
sudo apt-get install linux-source-$(uname -r)
```
<br>

## Build & Integration

How to Build & Integration

### Native

```
$ make clean && make modules
```
```
$ make clean && make builtin
```
<br>

### Cross-compilation

```
$ make integrate KERNEL=/path/to/source DTS=/path/to/dts
```
<br>

e.g)
```
$ make integrate KERNEL=/home/linux DTS=arch/arm64/boot/dts/broadcom/bcm2711-rpi-4-b.dts
```
<br>

## Hypervisor benchmark

In a virtualized environment,<br>
the VM kernel running on the hypervisor makes use of the virtual timer.<br>

### Arch timer patch

Need linux kernel patch
```
patches/arm_arch_timer.patch
```
<br>

It uses the physical timer instead of the virtual timer.<br>
Without the timer patch,<br>
benchmarking in a VM refers to the virtual timer,<br>
making the results unreliable.

### Hypervisor DTS patch

For interrupt benchmarking, the added irq-bench node must also be present in the hypervisor's device tree.<br>
Additionally, the registers referenced by this node must be configured for passthrough access.<br>
If passthrough is not used, a corresponding emulation driver must be implemented and ported to the hypervisor.<br>

### Workaround

These are the workarounds identified so far that need to be applied for each hypervisor.<br>

#### xen

Xen does not allow duplicate register address definitions in the device tree.<br>
Therefore, the logic for modifying GIC priorities must be removed,<br>
and the reg property should either define only the base address of the PIC or be omitted entirely.<br>
Accordingly, the reg index must also be updated (e.g., GIC: 0 → none, PIC: 1 → 0).<br>
Additionally, to prevent irq-bench from being initialized in Dom0,<br>
the xen,passthrough property must be added to the device tree.
```
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Steve Jeong <steve@how2flow.net>
 */

#include <dt-bindings/interrupt-controller/arm-gic.h>

/ {
	/*
	 * reg: Maps the gic base address and the pic base address.
	 *      idx 0: pic_base addr (optional)
	 *
	 * interrupts: 700 is an example.
	 *             However, there are some restrictions to consider:
	 *             The SoC must include a hardware component—such as a PIC—that can generate the interrupt.
	 *             The interrupt number must be unused and physically wired on the actual SoC.
	 */
	irq_bench: irq-bench {
		compatible = "generic,irq-bench";
		/* pic_base */
		reg = <0x0 0x90000000 0x0 0x10000>;
		interrupts = <GIC_SPI 700 IRQ_TYPE_LEVEL_HIGH>;
		xen,passthrough;
		status = "okay";
	};
};
```
<br>
