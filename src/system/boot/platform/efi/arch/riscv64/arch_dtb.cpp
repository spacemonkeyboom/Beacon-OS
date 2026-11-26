/*
 * Copyright 2019-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *   Alexander von Gluck IV <kallisti5@unixzen.com>
 */

#include <arch_cpu_defs.h>
#include <arch_dtb.h>
#include <arch_smp.h>
#include <boot/platform.h>
#include <boot/stage2.h>

extern "C" {
#include <libfdt.h>
}

#include "dtb.h"
#include "efi_platform.h"


uint32 gBootHart = 0;
static uint64 sTimerFrequency = 10000000;

static addr_range sPlic = {0};
static addr_range sClint = {0};
static addr_range sCcache = {0};
static bool sNonCoherentDisplaySoC = false;


void
arch_handle_fdt(const void* fdt, int node)
{
	const char* deviceType = (const char*)fdt_getprop(fdt, node,
		"device_type", NULL);

	const char* name = fdt_get_name(fdt, node, NULL);
	if (strcmp(name, "chosen") == 0) {
		if (uint32* prop = (uint32*)fdt_getprop(fdt, node, "boot-hartid", NULL))
			gBootHart = fdt32_to_cpu(*prop);
	} else if (strcmp(name, "cpus") == 0) {
		if (uint32* prop = (uint32*)fdt_getprop(fdt, node, "timebase-frequency", NULL))
			sTimerFrequency = fdt32_to_cpu(*prop);
	}

	if (deviceType != NULL) {
		if (strcmp(deviceType, "cpu") == 0) {
			// TODO: improve incompatible CPU detection
			if (!(fdt_getprop(fdt, node, "mmu-type", NULL) != NULL))
				return;

			// Beacon: skip harts marked unavailable, and monitor-only cores
			// that have no S-mode/MMU even when the DT claims otherwise
			// (the JH7110 U-Boot DT marks its S7 core "okay" with an
			// mmu-type; starting it traps inside OpenSBI).
			int statusLen;
			const char* status = (const char*)fdt_getprop(fdt, node,
				"status", &statusLen);
			if (status != NULL && strcmp(status, "okay") != 0
				&& strcmp(status, "ok") != 0) {
				dprintf("cpu node %s: status \"%s\", skipped\n", name, status);
				return;
			}
			int cpuCompatLen;
			const char* cpuCompat = (const char*)fdt_getprop(fdt, node,
				"compatible", &cpuCompatLen);
			if (cpuCompat != NULL
				&& (dtb_has_fdt_string(cpuCompat, cpuCompatLen, "sifive,s7")
					|| dtb_has_fdt_string(cpuCompat, cpuCompatLen, "sifive,e51")
					|| dtb_has_fdt_string(cpuCompat, cpuCompatLen, "sifive,e7"))) {
				dprintf("cpu node %s: monitor core (no S-mode), skipped\n", name);
				return;
			}

			platform_cpu_info* info;
			arch_smp_register_cpu(&info);
			if (info == NULL)
				return;
			info->id = fdt32_to_cpu(*(uint32*)fdt_getprop(fdt, node,
				"reg", NULL));
			dprintf("cpu\n");
			dprintf("  id: %" B_PRIu32 "\n", info->id);

			int subNode = fdt_subnode_offset(fdt, node, "interrupt-controller");
			if (subNode < 0) {
				dprintf("  [!] no interrupt controller\n");
			} else {
				info->phandle = fdt_get_phandle(fdt, subNode);
				dprintf("  phandle: %" B_PRIu32 "\n", info->phandle);
			}
		}
	}
	int compatibleLen;
	const char* compatible = (const char*)fdt_getprop(fdt, node,
		"compatible", &compatibleLen);

	if (compatible == NULL)
		return;

	// Root node: the StarFive JH7110's display engine (DC8200) does not snoop
	// the CPU caches.
	// The VisionFive 2 Lite vendor U-Boot DT lists only "starfive,jh7110s"
	// (the lower-clocked JH7110S, same display engine and memory map).
	if (dtb_has_fdt_string(compatible, compatibleLen, "starfive,jh7110")
		|| dtb_has_fdt_string(compatible, compatibleLen, "starfive,jh7110s")) {
		sNonCoherentDisplaySoC = true;
		// From RenkuOS (PR #490, atomozero), MIT: the display controller
		// reads memory behind the L2 cache, so draw through the uncached
		// "system port" view of the DDR (0x4_4000_0000, the memory port at
		// 0x4000_0000 plus this offset, JH7110 TRM table 2-4).
		gFrameBufferAliasOffset = 0x400000000ULL;
		dprintf("JH7110: frame buffer through the uncached DDR port\n");
	}

	if (dtb_has_fdt_string(compatible, compatibleLen, "starfive,jh7110-ccache")
		|| dtb_has_fdt_string(compatible, compatibleLen, "sifive,fu740-c000-ccache")
		|| dtb_has_fdt_string(compatible, compatibleLen, "sifive,ccache0")) {
		dtb_get_reg(fdt, node, 0, sCcache);
		return;
	}

	if (dtb_has_fdt_string(compatible, compatibleLen, "riscv,clint0")) {
		dtb_get_reg(fdt, node, 0, sClint);
		return;
	}

	if (dtb_has_fdt_string(compatible, compatibleLen, "riscv,plic0")
		|| dtb_has_fdt_string(compatible, compatibleLen, "sifive,plic-1.0.0")) {
		dtb_get_reg(fdt, node, 0, sPlic);
		int propSize;
		if (uint32* prop = (uint32*)fdt_getprop(fdt, node, "interrupts-extended", &propSize)) {
			dprintf("PLIC contexts\n");
			uint32 contextId = 0;
			for (uint32 *it = prop; (uint8_t*)it - (uint8_t*)prop < propSize; it += 2) {
				uint32 phandle = fdt32_to_cpu(*it);
				uint32 interrupt = fdt32_to_cpu(*(it + 1));
				if (interrupt == sExternInt) {
					platform_cpu_info* cpuInfo = arch_smp_find_cpu(phandle);
					dprintf("  context %" B_PRIu32 ": %" B_PRIu32 "\n", contextId, phandle);
					if (cpuInfo != NULL) {
						cpuInfo->plicContext = contextId;
						dprintf("    cpu id: %" B_PRIu32 "\n", cpuInfo->id);
					}
				}
				contextId++;
			}
		}
		return;
	}
}


void
arch_dtb_set_kernel_args(void)
{
	dprintf("bootHart: %" B_PRIu32 "\n", gBootHart);
	dprintf("timerFrequency: %" B_PRIu64 "\n", sTimerFrequency);
	gKernelArgs.arch_args.timerFrequency = sTimerFrequency;

//	gKernelArgs.arch_args.htif  = {.start = 0x40008000, .size = 0x10};
	gKernelArgs.arch_args.htif  = {.start = 0, .size = 0};
	gKernelArgs.arch_args.plic  = sPlic;
	gKernelArgs.arch_args.clint = sClint;

	if (sNonCoherentDisplaySoC && sCcache.size != 0) {
		gKernelArgs.arch_args.ccache = sCcache;
		dprintf("ccache: %#" B_PRIx64 " (%#" B_PRIx64 "), display not cache "
			"coherent: kernel will flush the frame buffer\n",
			(uint64)sCcache.start, (uint64)sCcache.size);
	} else
		gKernelArgs.arch_args.ccache = {.start = 0, .size = 0};
}


// Beacon: the JH7110 display engine does not snoop the CPU caches, so frame
// buffer writes made by the loader show up as noise until they leave the L2
// cache. Push a range out through the SiFive ccache Flush64 register (the
// kernel does the same periodically once it runs). Physical addresses are
// identity mapped while EFI boot services are active.
extern "C" void
beacon_flush_display_range(addr_t start, size_t size)
{
	if (!sNonCoherentDisplaySoC || sCcache.size == 0 || size == 0)
		return;
	// Flush64 takes the cached (memory port) address; map an uncached-alias
	// address back to it.
	if (gFrameBufferAliasOffset != 0 && start >= gFrameBufferAliasOffset)
		start -= gFrameBufferAliasOffset;

	const size_t kLineSize = 64;
	const size_t kFlush64 = 0x200;
	volatile uint64* flush64 = (volatile uint64*)(addr_t)(sCcache.start + kFlush64);
	addr_t end = start + size;
	__sync_synchronize();
	for (addr_t line = start & ~(addr_t)(kLineSize - 1); line < end;
			line += kLineSize)
		*flush64 = line;
	__sync_synchronize();
}
