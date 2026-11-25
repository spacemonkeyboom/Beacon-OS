/*
 * Copyright 2019-2022 Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
 */


// Beacon: restore the StarFive JH7110 display after ExitBootServices
// (only runs when the loader found a JH7110, see arch_dtb.cpp).
#define BEACON_JH7110_DISPLAY_RESTORE 1
// TEST: replays vendor U-Boot (GPL-2.0+) DC8200 writes; move into the GPL
// display add-on before any release (see PROGRESS.md).
#define BEACON_DC_REPLAY 1
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <string.h>

#include <arch_cpu_defs.h>

#include "arch_traps.h"
#include "efi_platform.h"
#include "generic_mmu.h"
#include "mmu.h"
#include "serial.h"
#include "smp.h"


// From entry.S
extern "C" void arch_enter_kernel(uint64 satp, addr_t kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop);

// From arch_mmu.cpp
extern void arch_mmu_post_efi_setup(size_t memoryMapSize,
	efi_memory_descriptor *memoryMap, size_t descriptorSize,
	uint32_t descriptorVersion);

extern uint64_t arch_mmu_generate_post_efi_page_tables(size_t memoryMapSize,
	efi_memory_descriptor *memoryMap, size_t descriptorSize,
	uint32_t descriptorVersion);


#include <arch/riscv64/arch_uart_sifive.h>


void
arch_convert_kernel_args(void)
{
	fix_address(gKernelArgs.arch_args.fdt);
}


#ifdef BEACON_JH7110_DISPLAY_RESTORE
// Beacon experiment (MIT, own code): StarFive vendor U-Boot powers off the
// VOUT domain and resets the display/HDMI when ExitBootServices removes its
// devices. Snapshot the display-related register blocks while U-Boot still
// drives the screen, then power VOUT back on and write back only the
// registers whose value changed. Stopgap until a real DC8200/HDMI driver.
struct beacon_reg_block {
	const char*	name;
	uint64		base;
	uint32		words;
	uint32		saved[2560];
};

static beacon_reg_block sBeaconBlocks[] = {
	{"syscrg", 0x13020000, 0x320 / 4, {}},
	{"voutcrg", 0x295c0000, 0x100 / 4, {}},
	{"dc8200", 0x29400000, 0x2800 / 4, {}},
	{"hdmi", 0x29590000, 0x800 / 4, {}},
};
static const uint64 kPmu = 0x17030000;
static const uint32 kPdVout = 1u << 4;
static uint32 sBeaconPowerMode = 0;


static void
beacon_display_snapshot()
{
	sBeaconPowerMode = *(volatile uint32*)(kPmu + 0x80);
	for (auto& b : sBeaconBlocks) {
		for (uint32 i = 0; i < b.words; i++)
			b.saved[i] = ((volatile uint32*)b.base)[i];
	}
}


static void
beacon_display_restore()
{
	uint32 mode = *(volatile uint32*)(kPmu + 0x80);
	dprintf("BEACON-DISP: PMU power mode before %#x, now %#x\n",
		(unsigned)sBeaconPowerMode, (unsigned)mode);
	if ((sBeaconPowerMode & kPdVout) != 0 && (mode & kPdVout) == 0) {
		*(volatile uint32*)(kPmu + 0x0c) = kPdVout;
		*(volatile uint32*)(kPmu + 0x44) = 0xff;
		*(volatile uint32*)(kPmu + 0x44) = 0x05;
		*(volatile uint32*)(kPmu + 0x44) = 0x50;
		for (int i = 0; i < 1000000; i++) {
			if ((*(volatile uint32*)(kPmu + 0x80) & kPdVout) != 0)
				break;
		}
		dprintf("BEACON-DISP: VOUT power on -> mode %#x\n",
			(unsigned)*(volatile uint32*)(kPmu + 0x80));
	}
	for (auto& b : sBeaconBlocks) {
#ifdef BEACON_DC_REPLAY
		if (strcmp(b.name, "dc8200") == 0)
			continue;	// programmed by the replayed sequence below
#endif
		// two passes: the DC/HDMI latch some settings only once others are set
		for (int pass = 0; pass < 2; pass++) {
			uint32 changed = 0;
			for (uint32 i = 0; i < b.words; i++) {
				volatile uint32* r = (volatile uint32*)b.base + i;
				uint32 now = *r;
				if (now != b.saved[i]) {
					if (pass == 0 && changed < 12) {
						dprintf("BEACON-DISP: %s +%#x: %#x -> %#x\n", b.name,
							(unsigned)(i * 4), (unsigned)now,
							(unsigned)b.saved[i]);
					}
					*r = b.saved[i];
					changed++;
				}
			}
			dprintf("BEACON-DISP: %s pass %d: %u registers rewritten\n",
				b.name, pass, (unsigned)changed);
		}
	}
#ifdef BEACON_DC_REPLAY
	// SPDX-License-Identifier: GPL-2.0-or-later (this block only): register
	// sequence from vendor U-Boot drivers/video/starfive (sf_vop.c sf_display_init,
	// HDMI 1080p path) DC8200 programming sequence. To move into a GPL display
	// add-on later; until then the riscv64 loader binary is GPL-2.0.
	struct { uint32 off; uint32 val; } kSeq[] = {
		{0x14, 0xc0001fff}, {0x1cc0, 0x2000}, {0x1800, 0}, {0x24d8, 0},
		{0x24e0, 0x021c0780}, {0x1810, 0x021c0780}, {0x1400, 0xfe000000},
		{0x1408, 0x1e00}, {0x1ce8, 0xf61}, {0x2510, 0x2042},
		{0x2508, 0x808a3156}, {0x2500, 0x8008e1b2}, {0x1518, 0x18000000},
		{0x1cc0, 0x3000}, {0x1540, 0x60000}, {0x2540, 1}, {0x1540, 0x80060000},
		{0x1544, 0x60000}, {0x2544, 2}, {0x1544, 0x80060000},
		{0x1548, 0x60000}, {0x2548, 0xc}, {0x1548, 0x80060000},
		{0x154c, 0x60000}, {0x254c, 0xd}, {0x154c, 0x80060000},
		{0x2518, 1}, {0x1a28, 0}, {0x1430, 0x08980780}, {0x1438, 0x440207d8},
		{0x1440, 0x04650438}, {0x1448, 0x4220843c}, {0x14b0, 0},
		{0x1cd0, 0xd2}, {0x14b8, 5}, {0x14d0, 0x52}, {0x1528, 0xdeadbeef},
		{0x1418, 0x1111}, {0x1410, 0}, {0x2518, 0}, {0x1468, 0x200024},
		{0x1484, 0}, {0x1468, 0x200024}, {0x24e8, 0xc24}, {0x24fc, 0},
		{0x24e8, 0xc24}, {0x1ccc, 1},
	};
	volatile uint32* dc = (volatile uint32*)0x29400000;
	// The display controller needs the real DDR address, not the CPU's
	// uncached alias (gFrameBufferAliasOffset).
	kSeq[6].val = (uint32)(gKernelArgs.frame_buffer.physical_buffer.start
		- gFrameBufferAliasOffset);
	for (auto& w : kSeq)
		dc[w.off / 4] = w.val;
	dprintf("BEACON-DISP: dc8200 replayed %u writes; 04=%#x 1400=%#x 1cc0=%#x\n",
		(unsigned)(sizeof(kSeq) / sizeof(kSeq[0])), (unsigned)dc[0x04 / 4],
		(unsigned)dc[0x1400 / 4], (unsigned)dc[0x1cc0 / 4]);
#endif
}
#endif


void
arch_start_kernel(addr_t kernelEntry)
{
	// Allocate virtual memory for kernel args
	struct kernel_args *kernelArgs = NULL;
	if (platform_allocate_region((void **)&kernelArgs,
			sizeof(struct kernel_args), 0) != B_OK)
		panic("Failed to allocate kernel args.");

	addr_t virtKernelArgs;
	platform_bootloader_address_to_kernel_address((void*)kernelArgs,
		&virtKernelArgs);

	// EFI assumed to be SBI booted
	gKernelArgs.arch_args.machine_platform = kPlatformSbi;

	// Prepare to exit EFI boot services.
	// Read the memory map.
	// First call is to determine the buffer size.
	size_t memoryMapSize = 0;
	efi_memory_descriptor dummy;
	size_t mapKey;
	size_t descriptorSize;
	uint32_t descriptorVersion;
	if (kBootServices->GetMemoryMap(&memoryMapSize, &dummy, &mapKey,
			&descriptorSize, &descriptorVersion) != EFI_BUFFER_TOO_SMALL) {
		panic("Unable to determine size of system memory map");
	}

	// Allocate a buffer twice as large as needed just in case it gets bigger
	// between calls to ExitBootServices.
	size_t actualMemoryMapSize = memoryMapSize * 2;
	efi_memory_descriptor *memoryMap
		= (efi_memory_descriptor *)kernel_args_malloc(actualMemoryMapSize, 8);

	if (memoryMap == NULL)
		panic("Unable to allocate memory map.");

	// Read (and print) the memory map.
	memoryMapSize = actualMemoryMapSize;
	if (kBootServices->GetMemoryMap(&memoryMapSize, memoryMap, &mapKey,
			&descriptorSize, &descriptorVersion) != EFI_SUCCESS) {
		panic("Unable to fetch system memory map.");
	}

	addr_t addr = (addr_t)memoryMap;
	dprintf("System provided memory map:\n");
	for (size_t i = 0; i < memoryMapSize / descriptorSize; ++i) {
		efi_memory_descriptor *entry
			= (efi_memory_descriptor *)(addr + i * descriptorSize);
		dprintf("  phys: 0x%08" PRIx64 "-0x%08" PRIx64
			", virt: 0x%08" PRIx64 "-0x%08" PRIx64
			", type: %s (%#x), attr: %#" PRIx64 "\n",
			entry->PhysicalStart,
			entry->PhysicalStart + entry->NumberOfPages * B_PAGE_SIZE,
			entry->VirtualStart,
			entry->VirtualStart + entry->NumberOfPages * B_PAGE_SIZE,
			memory_region_type_str(entry->Type), entry->Type,
			entry->Attribute);
	}

	// Generate page tables for use after ExitBootServices.
	uint64_t satp = arch_mmu_generate_post_efi_page_tables(
		memoryMapSize, memoryMap, descriptorSize, descriptorVersion);
	dprintf("SATP: 0x%016" B_PRIx64 "\n", satp);

	// Attempt to fetch the memory map and exit boot services.
	// This needs to be done in a loop, as ExitBootServices can change the
	// memory map.
	// Even better: Only GetMemoryMap and ExitBootServices can be called after
	// the first call to ExitBootServices, as the firmware is permitted to
	// partially exit. This is why twice as much space was allocated for the
	// memory map, as it's impossible to allocate more now.
	// A changing memory map shouldn't affect the generated page tables, as
	// they only needed to know about the maximum address, not any specific
	// entry.

#ifdef BEACON_JH7110_DISPLAY_RESTORE
	if (gKernelArgs.arch_args.ccache.size != 0)
		beacon_display_snapshot();
#endif
	dprintf("Calling ExitBootServices. So long, EFI!\n");
	serial_disable();

	while (true) {
		if (kBootServices->ExitBootServices(kImage, mapKey) == EFI_SUCCESS) {
			// Disconnect from EFI serial_io / stdio services
			serial_kernel_handoff();
			dprintf("Unhooked from EFI serial services\n");
			break;
		}

		memoryMapSize = actualMemoryMapSize;
		if (kBootServices->GetMemoryMap(&memoryMapSize, memoryMap, &mapKey,
				&descriptorSize, &descriptorVersion) != EFI_SUCCESS) {
			panic("Unable to fetch system memory map.");
		}
	}

	arch_traps_init();

	// Update EFI, generate final kernel physical memory map, etc.
	arch_mmu_post_efi_setup(memoryMapSize, memoryMap,
		descriptorSize, descriptorVersion);

	// Re-init and activate serial in a horrific post-EFI landscape. Clowns roam the land freely.
	serial_init();
	serial_enable();

#ifdef BEACON_JH7110_DISPLAY_RESTORE
	if (gKernelArgs.arch_args.ccache.size != 0)
		beacon_display_restore();
#endif

#ifdef BEACON_FB_PROBE
	// Beacon experiment (JH7110): is the display still scanning out after
	// ExitBootServices, and does it need an L2 (ccache) flush to see writes?
	// Red band + ccache Flush64 for 4 s, then green band without a flush.
	if (gKernelArgs.frame_buffer.enabled) {
		uint64 fb = gKernelArgs.frame_buffer.physical_buffer.start;
		uint32 bpr = gKernelArgs.frame_buffer.bytes_per_row;
		uint32 h = gKernelArgs.frame_buffer.height;
		dprintf("BEACON-FB: base %#" B_PRIx64 " size %#" B_PRIx64 " %ux%u depth %u bpr %u\n",
			fb, (uint64)gKernelArgs.frame_buffer.physical_buffer.size,
			(unsigned)gKernelArgs.frame_buffer.width, (unsigned)h,
			(unsigned)gKernelArgs.frame_buffer.depth, (unsigned)bpr);
		auto wait = [](uint64 ticks) {
			uint64 t0, t;
			asm volatile("rdtime %0" : "=r"(t0));
			do { asm volatile("rdtime %0" : "=r"(t)); } while (t - t0 < ticks);
		};
		// Dump DC8200 state after ExitBootServices (compare with U-Boot md.l)
		static const uint64 kDcRegs[] = {0x29400000ULL, 0x29401400ULL, 0x29401800ULL};
		for (uint64 base : kDcRegs) {
			dprintf("BEACON-FB: reading %#" B_PRIx64 "...\n", base);
			volatile uint32* r = (volatile uint32*)base;
			for (int i = 0; i < 16; i += 4) {
				dprintf("BEACON-FB: %08" B_PRIx64 ": %08x %08x %08x %08x\n",
					base + i * 4, (unsigned)r[i], (unsigned)r[i + 1],
					(unsigned)r[i + 2], (unsigned)r[i + 3]);
			}
		}
		volatile uint64* flush64 = (volatile uint64*)0x2010200;
		uint64 redStart = fb, redEnd = fb + (uint64)bpr * (h / 4);
		for (uint64 a = redStart; a < redEnd; a += 4)
			*(volatile uint32*)a = 0x00ff0000;
		for (uint64 a = redStart & ~63ULL; a < redEnd; a += 64)
			*flush64 = a;
		asm volatile("fence rw, rw" ::: "memory");
		dprintf("BEACON-FB: red band drawn + flushed, waiting 4 s\n");
		wait(4ULL * 4000000);
		uint64 greenEnd = fb + (uint64)bpr * (h / 2);
		for (uint64 a = redEnd; a < greenEnd; a += 4)
			*(volatile uint32*)a = 0x0000ff00;
		asm volatile("fence rw, rw" ::: "memory");
		dprintf("BEACON-FB: green band drawn, NOT flushed, waiting 4 s\n");
		wait(4ULL * 4000000);
		dprintf("BEACON-FB: probe done\n");
	}
#endif

	// Copy final kernel args
	// This should be the last step before jumping to the kernel
	// as there are some fixups happening to kernel_args even in the last minute
	memcpy((void *)kernelArgs, &gKernelArgs, sizeof(struct kernel_args));

	smp_boot_other_cpus(satp, kernelEntry, virtKernelArgs);

	// Enter the kernel!
	dprintf("arch_enter_kernel(satp: %#" B_PRIxADDR ", kernelArgs: %#" B_PRIxADDR
		", kernelEntry: %#" B_PRIxADDR ", sp: %#" B_PRIxADDR ")\n",	satp,
		(addr_t)&kernelArgs, (addr_t)kernelEntry, kernelArgs->cpu_kstack[0].start
			+ kernelArgs->cpu_kstack[0].size);

	arch_enter_kernel(satp, virtKernelArgs, kernelEntry,
		kernelArgs->cpu_kstack[0].start + kernelArgs->cpu_kstack[0].size);
}
