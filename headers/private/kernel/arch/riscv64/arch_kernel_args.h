/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef KERNEL_ARCH_RISCV64_KERNEL_ARGS_H
#define KERNEL_ARCH_RISCV64_KERNEL_ARGS_H

#ifndef KERNEL_BOOT_KERNEL_ARGS_H
#	error This file is included from <boot/kernel_args.h> only
#endif


#include <util/FixedWidthPointer.h>
#include <boot/uart.h>


#define _PACKED __attribute__((packed))

#define MAX_VIRTUAL_RANGES_TO_KEEP      32


enum {
	kPlatformNone,
	kPlatformMNative,
	kPlatformSbi,
};


// kernel args
typedef struct {
	// Virtual address range of RAM physical memory mapping region
	addr_range physMap;

	// The virtual ranges we want to keep in the kernel.
	uint32		num_virtual_ranges_to_keep;
	addr_range	virtual_ranges_to_keep[MAX_VIRTUAL_RANGES_TO_KEEP];

	// MNative hooks, or SBI
	uint32 machine_platform;

	uint64 timerFrequency; // in Hz

	// All following address are virtual
	FixedWidthPointer<void> acpi_root;
	FixedWidthPointer<void> fdt;

	addr_range	htif;
	addr_range	plic;
	addr_range	clint;

	uart_info	uart;

	uint32 hartIds[SMP_MAX_CPUS];
	uint32 plicContexts[SMP_MAX_CPUS];

	// Beacon: SiFive L2 cache controller (physical), set only on SoCs whose
	// display engine is not cache coherent (StarFive JH7110). The kernel then
	// flushes the boot frame buffer to RAM periodically. size 0 = unused.
	addr_range	ccache;
} _PACKED arch_kernel_args;

#endif	/* KERNEL_ARCH_RISCV64_KERNEL_ARGS_H */
