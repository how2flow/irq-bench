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
