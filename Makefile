# Copyright (C) Steve Jeong, 2025. <steve@how2flow.net>
#
# Makefile for building a Linux kernel module or built-in object using GNU Make.

# Configuration Variables
ARCH ?= $(shell uname -m)
CROSS_COMPILE ?=
KERNEL ?= /lib/modules/$(shell uname -r)/build
DTS ?= $(KERNEL)/arch/$(ARCH)/boot/dts
CONFIG_NAME := IRQ_BENCH
MODULE_NAME := irq-bench
MSI_CONFIG_NAME := GENERIC_MSI
MSI_MODULE_NAME := generic-msi
DTS_DIR := $(CURDIR)/dts
INC_DIR := $(CURDIR)/include
PTH_DIR := $(CURDIR)/patches
SRC_DIR := $(CURDIR)/src

# Source Files
DTSFILE := $(wildcard $(DTS_DIR)/*.dts*)
DTSNAME := $(notdir $(DTSFILE))
HEADERS := $(wildcard $(INC_DIR)/*.h)
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(patsubst %.c, %.o, $(SOURCES))
PATCHES := $(wildcard $(PTH_DIR)/*.patch)

# Kernel Module/Object Definition
obj-$(CONFIG_IRQ_BENCH) += $(MODULE_NAME).o
$(MODULE_NAME)-y += $(OBJECTS)

# Compiler Flags
EXTRA_CFLAGS += -I$(INC_DIR) -Wall

# Build Targets
.PHONY: all modules builtin clean help check-kernel integrate

all: modules
	@echo "Default build: modules"

modules: check-kernel
	@echo "Building module: $(MODULE_NAME)"
	@echo "Sources: $(SOURCES)"
	$(MAKE) -C $(KERNEL) M=$(CURDIR) CONFIG_IRQ_BENCH=m ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

builtin: check-kernel
	@echo "Building built-in: $(MODULE_NAME)"
	@echo "Sources: $(SOURCES)"
	$(MAKE) -C $(KERNEL) M=$(CURDIR) CONFIG_IRQ_BENCH=y ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

integrate:
	@echo "Integrating $(MODULE_NAME) into kernel source tree at $(KERNEL)"
	@mkdir -p $(KERNEL)/drivers/misc
	@cp $(SOURCES) $(KERNEL)/drivers/misc/
	@cp $(HEADERS) $(KERNEL)/drivers/misc/
	@cp $(DTSFILE) $$(dirname "$(KERNEL)/$(DTS)")
	@for dtsi in $(DTSNAME); do \
		if ! grep -q "$$dtsi" $(KERNEL)/$(DTS); then \
			echo '#include "'$$dtsi'"' >> $(KERNEL)/$(DTS); \
		fi \
	done
	@if ! grep -q "$(MODULE_NAME)" $(KERNEL)/drivers/misc/Makefile; then \
		echo 'obj-$$\(CONFIG_$(CONFIG_NAME)) += $(MODULE_NAME).o' >> $(KERNEL)/drivers/misc/Makefile; \
		sed -i 's/\\(CONFIG_$(CONFIG_NAME))/(CONFIG_$(CONFIG_NAME))/g' $(KERNEL)/drivers/misc/Makefile; \
	fi
	@if ! grep -q "$(CONFIG_NAME)" $(KERNEL)/drivers/misc/Kconfig; then \
		sed -i '/^endmenu$$/d' "$(KERNEL)/drivers/misc/Kconfig"; \
		echo "" >> $(KERNEL)/drivers/misc/Kconfig; \
		echo "config $(CONFIG_NAME)" >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	bool "IRQ Benchmark Driver"' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '  default y' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	help' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	  Enable the IRQ Benchmark driver (irq-bench) for performance testing.' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo 'endmenu' >> $(KERNEL)/drivers/misc/Kconfig; \
	fi
	@if ! grep -q "$(MSI_MODULE_NAME)" $(KERNEL)/drivers/misc/Makefile; then \
		echo 'obj-$$\(CONFIG_$(MSI_CONFIG_NAME)) += $(MSI_MODULE_NAME).o' >> $(KERNEL)/drivers/misc/Makefile; \
		sed -i 's/\\(CONFIG_$(MSI_CONFIG_NAME))/(CONFIG_$(MSI_CONFIG_NAME))/g' $(KERNEL)/drivers/misc/Makefile; \
	fi
	@if ! grep -q "$(MSI_CONFIG_NAME)" $(KERNEL)/drivers/misc/Kconfig; then \
		sed -i '/^endmenu$$/d' "$(KERNEL)/drivers/misc/Kconfig"; \
		echo "" >> $(KERNEL)/drivers/misc/Kconfig; \
		echo "config $(MSI_CONFIG_NAME)" >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	bool "GENERIC MSI Driver"' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	default y' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	help' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo '	  Enable the IRQ Benchmark driver (generic-msi) for performance testing.' >> $(KERNEL)/drivers/misc/Kconfig; \
		echo 'endmenu' >> $(KERNEL)/drivers/misc/Kconfig; \
	fi
# for virtual machine of hypervisor #
	@if [ -n "$(VM)" ]; then \
		git apply --directory=$(KERNEL) --unsafe-paths -R $(PTH_DIR)/arm_arch_timer.patch; \
		git apply --directory=$(KERNEL) --unsafe-paths $(PTH_DIR)/arm_arch_timer.patch; \
	fi

clean:
	@echo "Cleaning build artifacts"
	$(MAKE) -C $(KERNEL) M=$(CURDIR) clean
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions Module.symvers modules.order *.a $(SRC_DIR)/*.o

check-kernel:
	@if [ ! -d "$(KERNEL)" ]; then \
		echo "Error: Kernel directory $(KERNEL) not found"; \
		echo "Please set KERNEL to a valid kernel source path"; \
		exit 1; \
	fi

help:
	@echo "Available targets:"
	@echo "  all        : Alias for 'modules' (builds $(MODULE_NAME).ko)"
	@echo "  modules    : Build $(MODULE_NAME) as a kernel module (.ko)"
	@echo "  builtin    : Build $(MODULE_NAME) as a built-in object, archived as $(MODULE_NAME).a"
	@echo "  clean      : Remove all build artifacts"
	@echo "  check-kernel : Verify kernel directory existence"
	@echo "  integrate  : Copy $(MODULE_NAME) to kernel source tree and update Kconfig/Makefile"
	@echo "  help       : Show this help message"
	@echo "Cross-compilation example:"
