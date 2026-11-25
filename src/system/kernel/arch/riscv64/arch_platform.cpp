/* Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * Distributed under the terms of the MIT License.
 */


#include <arch/platform.h>
#include <boot/kernel_args.h>
#include <Htif.h>
#include <Plic.h>
#include <Clint.h>
#include <platform/sbi/sbi_syscalls.h>

#include <debug.h>
#include <arch/debug_console.h>
#include <KernelExport.h>
#include <kernel.h>
#include <OS.h>
#include <vm/vm.h>


uint32 gPlatform;

void* gFDT = NULL;

HtifRegs  *volatile gHtifRegs  = (HtifRegs *volatile)0;
PlicRegs  *volatile gPlicRegs;
ClintRegs *volatile gClintRegs;


status_t
arch_platform_init(struct kernel_args *args)
{
	gPlatform = args->arch_args.machine_platform;

	debug_early_boot_message("machine_platform: ");
	switch (gPlatform) {
		case kPlatformMNative:
			debug_early_boot_message("Native mmode hooks\n");
			break;
		case kPlatformSbi:
			debug_early_boot_message("SBI\n");
			break;
		default:
			debug_early_boot_message("?\n");
			break;
	}

	gFDT = args->arch_args.fdt;

	gHtifRegs  = (HtifRegs *volatile)args->arch_args.htif.start;
	gPlicRegs  = (PlicRegs *volatile)args->arch_args.plic.start;
	gClintRegs = (ClintRegs *volatile)args->arch_args.clint.start;

	return B_OK;
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	if (gPlatform == kPlatformSbi) {
		sbiret res;
		res = sbi_get_spec_version();
		dprintf("SBI spec version: %#lx\n", res.value);
		res = sbi_get_impl_id();
		dprintf("SBI implementation ID: %#lx\n", res.value);
		res = sbi_get_impl_version();
		dprintf("SBI implementation version: %#lx\n", res.value);
		res = sbi_get_mvendorid();
		dprintf("SBI vendor ID: %#lx\n", res.value);
		res = sbi_get_marchid();
		dprintf("SBI arch ID: %#lx\n", res.value);
	}
	return B_OK;
}


// #pragma mark - Beacon: frame buffer flusher for non-coherent displays

// SiFive composable cache (L2) controller: writing a physical address to
// FLUSH64 writes back and invalidates the 64-byte line holding it.
static const size_t kCcacheFlush64 = 0x200;
static const size_t kCcacheLineSize = 64;
static const bigtime_t kFrameBufferFlushInterval = 50000;	// 20 Hz

static volatile uint64* sCcacheFlush64 = NULL;
static phys_addr_t sFlushFrameBuffer = 0;
static size_t sFlushFrameBufferSize = 0;


[[maybe_unused]] static uint32* sFlushTestPattern = NULL;	// Beacon diagnostic, see below
static volatile uint32* sDiagDc = NULL;
static volatile uint32* sDiagHdmi = NULL;
static volatile uint32* sDiagVoutCrg = NULL;
static volatile uint32* sDiagPmu = NULL;


static volatile uint32*
diag_map(const char* name, phys_addr_t base, size_t size)
{
	void* address;
	area_id area = map_physical_memory(name, base, size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	return area >= 0 ? (volatile uint32*)address : NULL;
}


// Beacon diagnostic (JH7110): print the display pipeline state so we can see
// whether something turns the display off after the boot loader restored it.
static void
dump_display_state()
{
	if (sDiagPmu != NULL)
		dprintf("disp: PMU power mode %#" B_PRIx32 "\n", sDiagPmu[0x80 / 4]);
	if (sDiagVoutCrg != NULL) {
		dprintf("disp: voutcrg 04=%#" B_PRIx32 " 10=%#" B_PRIx32 " 3c=%#" B_PRIx32
			" 48(rst)=%#" B_PRIx32 " 4c(rst st)=%#" B_PRIx32 "\n",
			sDiagVoutCrg[0x04 / 4], sDiagVoutCrg[0x10 / 4], sDiagVoutCrg[0x3c / 4],
			sDiagVoutCrg[0x48 / 4], sDiagVoutCrg[0x4c / 4]);
	}
	if (sDiagDc != NULL) {
		dprintf("disp: dc 00=%#" B_PRIx32 " 04=%#" B_PRIx32 " 24=%#" B_PRIx32
			" 1400=%#" B_PRIx32 " 1408=%#" B_PRIx32 " 1418=%#" B_PRIx32 " 1430=%#"
			B_PRIx32 " 1518=%#" B_PRIx32 " 1cc0=%#" B_PRIx32 " 1ccc=%#" B_PRIx32
			" 1ce8=%#" B_PRIx32 "\n",
			sDiagDc[0x00 / 4], sDiagDc[0x04 / 4], sDiagDc[0x24 / 4],
			sDiagDc[0x1400 / 4], sDiagDc[0x1408 / 4], sDiagDc[0x1418 / 4],
			sDiagDc[0x1430 / 4], sDiagDc[0x1518 / 4], sDiagDc[0x1cc0 / 4],
			sDiagDc[0x1ccc / 4], sDiagDc[0x1ce8 / 4]);
	}
	if (sDiagHdmi != NULL) {
		dprintf("disp: hdmi 00=%#" B_PRIx32 " 1a9=%#" B_PRIx32 " 1af=%#" B_PRIx32
			" 1b2=%#" B_PRIx32 " 1be=%#" B_PRIx32 " 1cc=%#" B_PRIx32 "\n",
			sDiagHdmi[0x00], sDiagHdmi[0x1a9], sDiagHdmi[0x1af],
			sDiagHdmi[0x1b2], sDiagHdmi[0x1be], sDiagHdmi[0x1cc]);
	}
}
[[maybe_unused]] static uint32 sFlushBytesPerRow = 0;


static status_t
frame_buffer_flusher(void*)
{
	[[maybe_unused]] uint32 pass = 0;
	while (true) {
		// Beacon diagnostic: a 48x48 square at the top left that changes colour
		// about once a second, so we can see whether flushed writes reach the
		// display. Remove once the display path is understood.
#ifdef BEACON_FLUSH_TEST
		if (sFlushTestPattern != NULL && (pass % 20) == 0) {
			static const uint32 kColors[] = {0x00ff0000, 0x0000ff00, 0x000000ff,
				0x00ffffff};
			uint32 color = kColors[(pass / 20) % 4];
			for (uint32 y = 8; y < 56; y++) {
				uint32* row = (uint32*)((uint8*)sFlushTestPattern
					+ y * sFlushBytesPerRow);
				for (uint32 x = 8; x < 56; x++)
					row[x] = color;
			}
			if (pass == 0 || (pass % 200) == 0) {
				dprintf("ccache: flusher alive, pass %" B_PRIu32 ", pixel(8,8)=%#"
					B_PRIx32 "\n", pass,
					((uint32*)((uint8*)sFlushTestPattern + 8 * sFlushBytesPerRow))[8]);
				dump_display_state();
			}
		}
#endif
		pass++;
		phys_addr_t end = sFlushFrameBuffer + sFlushFrameBufferSize;
		for (phys_addr_t line = sFlushFrameBuffer; line < end;
				line += kCcacheLineSize) {
			*sCcacheFlush64 = line;
		}
		__sync_synchronize();
		snooze(kFrameBufferFlushInterval);
	}
	return B_OK;
}


static void
start_frame_buffer_flusher(kernel_args* args)
{
	if (args->arch_args.ccache.size == 0 || !args->frame_buffer.enabled)
		return;

	void* regs;
	area_id area = map_physical_memory("ccache regs",
		args->arch_args.ccache.start, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (area < 0) {
		dprintf("ccache: mapping registers failed: %s\n", strerror(area));
		return;
	}
	sCcacheFlush64 = (volatile uint64*)((addr_t)regs + kCcacheFlush64);
	sFlushFrameBuffer = ROUNDDOWN(
		(phys_addr_t)args->frame_buffer.physical_buffer.start, kCcacheLineSize);
	sFlushFrameBufferSize = ROUNDUP(args->frame_buffer.physical_buffer.size,
		kCcacheLineSize);

#ifdef BEACON_FLUSH_TEST
	void* fb;
	area_id fbArea = map_physical_memory("fb flush test",
		sFlushFrameBuffer, sFlushFrameBufferSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &fb);
	if (fbArea >= 0) {
		sFlushTestPattern = (uint32*)fb;
		sFlushBytesPerRow = args->frame_buffer.bytes_per_row;
	}
#endif

	sDiagPmu = diag_map("diag pmu", 0x17030000, B_PAGE_SIZE);
	sDiagVoutCrg = diag_map("diag voutcrg", 0x295c0000, B_PAGE_SIZE);
	sDiagDc = diag_map("diag dc8200", 0x29400000, 0x3000);
	sDiagHdmi = diag_map("diag hdmi", 0x29590000, 0x1000);
	dump_display_state();

	// The loader hands over the uncached DDR alias (0x4_4000_0000 and up)
	// on the JH7110; writes there reach the display without a flush.
	if (args->frame_buffer.physical_buffer.start >= 0x400000000ULL) {
		dprintf("ccache: frame buffer is uncached, no flusher needed\n");
		return;
	}

	thread_id thread = spawn_kernel_thread(frame_buffer_flusher,
		"frame buffer flusher", B_DISPLAY_PRIORITY, NULL);
	if (thread < 0)
		return;
	resume_thread(thread);
	dprintf("ccache: flushing frame buffer %#" B_PRIxPHYSADDR " (%#" B_PRIxSIZE
		" bytes) every %" B_PRIdBIGTIME " us\n", sFlushFrameBuffer,
		sFlushFrameBufferSize, kFrameBufferFlushInterval);
}


// #pragma mark - Beacon: enter KDL from the serial console

// Boards without a keyboard (or a keyboard without SysRq) can't use the
// Alt+SysRq+D emergency key, so watch the serial console instead: sending
// Ctrl+] (0x1d) drops into the kernel debugger.
static const char kSerialDebuggerKey = 0x1d;


static status_t
serial_debugger_key_watcher(void*)
{
	while (true) {
		int c = arch_debug_serial_try_getchar();
		if (c == kSerialDebuggerKey)
			kernel_debugger("Ctrl+] on the serial console");
		snooze(100000);
	}
	return B_OK;
}


static void
start_serial_debugger_key_watcher()
{
	thread_id thread = spawn_kernel_thread(serial_debugger_key_watcher,
		"serial KDL key", B_LOW_PRIORITY, NULL);
	if (thread >= 0) {
		resume_thread(thread);
		dprintf("serial console: send Ctrl+] to enter the kernel debugger\n");
	}
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	start_frame_buffer_flusher(kernelArgs);
	start_serial_debugger_key_watcher();
	return B_OK;
}
