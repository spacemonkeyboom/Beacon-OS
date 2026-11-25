/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Synopsys DesignWare Mobile Storage Host Controller bus driver for Beacon.
 *
 * Controller sequences (reset, CIU clock update, card clock divider, command
 * register flags, FIFO PIO, error recovery) are adapted from:
 *   Linux drivers/mmc/host/dw_mmc.c (GPL-2.0-or-later)
 *     Copyright (C) 2009 NXP Semiconductors
 *     Copyright (C) 2009, 2010 Imagination Technologies Ltd.
 *   U-Boot drivers/mmc/dw_mmc.c (GPL-2.0+), including the StarFive
 *   "send init clocks on the first command" change in the orangepi-xunlong
 *   v2021.10-jh7110 tree
 *     (C) Copyright 2012 SAMSUNG Electronics
 *     Jaehoon Chung <jh80.chung@samsung.com>
 *     Rajeshawari Shinde <rajeshwari.s@samsung.com>
 * The command/response-type mapping follows Haiku's sdhci.cpp
 * (Copyright 2018-2025 Haiku, Inc., MIT License).
 *
 * Modified for Beacon/Haiku, 2026 (The Beacon Project).
 * See THIRD_PARTY.md in this directory for exact upstream versions.
 *
 * Design (see README.md): data moves by PIO through the controller FIFO,
 * driven from the calling thread. The interrupt only wakes that thread; the
 * thread always re-reads RINTSTS/STATUS itself, so a lost or misrouted
 * interrupt costs speed, not correctness. PIO needs no cache maintenance,
 * which matters on the JH7110 (DMA is not cache coherent there).
 */


#include "DwMmc.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <util/AutoLock.h>
#include <util/iovec_support.h>
#include <vm/vm.h>

#include "IORequest.h"
#include "Jh7110Gpio.h"


#define INFO(x...)	dprintf("dw_mmc: " x)
#define ERROR(x...)	dprintf("dw_mmc: ERROR: " x)
#define SDIO_INFO(x...)	dprintf("sdio: " x)


// How many commands are logged one by one after boot (bring-up aid).
static const int32 kTraceCommandBudget = 64;
// How many data transfers are logged one by one after boot.
static const int32 kTraceIoBudget = 8;
// Longest sleep while waiting for an event, before re-polling registers in
// case the interrupt never comes.
static const bigtime_t kInterruptFallbackPoll = 20000;
// Busy-poll this long before sleeping; FIFO events at 25 MHz come every few
// microseconds.
static const bigtime_t kSpinBeforeSleep = 30;

static const uint32 kBlockSize = 512;

// Beacon SDIO power sequence. Linux mmc_power_up() holds the pwrseq reset
// for power_delay_ms (10 ms) and waits 10 ms more after releasing it; we hold
// the reset longer so a chip left running by a previous boot fully resets.
static const bigtime_t kSdioResetHold = 50000;
static const uint32 kSdioMinPostDelayMs = 10;
// CMD5 inquiry retries after releasing the reset, 20 ms apart.
static const int kSdioProbeTries = 25;
// SDIO CCCR ABORT register and its RES (I/O reset) bit.
static const uint32 kSdioCccrAbort = 0x06;
static const uint32 kSdioAbortRes = 0x08;
static const uint32 kSdioMaxBlockSize = 2048;


// #pragma mark - MMIO access


static inline void
io_barrier_before_write()
{
#if defined(__riscv)
	// Linux riscv __io_bw(): order earlier memory writes before the I/O write.
	__asm__ __volatile__("fence w,o" ::: "memory");
#else
	__sync_synchronize();
#endif
}


static inline void
io_barrier_after_read()
{
#if defined(__riscv)
	// Linux riscv __io_ar(): the I/O read completes before later accesses.
	__asm__ __volatile__("fence i,ir" ::: "memory");
#else
	__sync_synchronize();
#endif
}


uint32
DwMmcHost::_Read(uint32 reg) const
{
	uint32 value = *(volatile uint32*)(fRegs + reg);
	io_barrier_after_read();
	return value;
}


void
DwMmcHost::_Write(uint32 reg, uint32 value)
{
	io_barrier_before_write();
	*(volatile uint32*)(fRegs + reg) = value;
}


// #pragma mark - construction


DwMmcHost::DwMmcHost()
	:
	fRegsArea(-1),
	fRegs(NULL),
	fFifo(NULL),
	fFifoWidth(4),
	fFifoDepth(0),
	fFifoth(0),
	fVersion(0),
	fIdstsReg(kDwIdsts),
	fIdintenReg(kDwIdinten),
	fInterruptInstalled(false),
	fInterruptCount(0),
	fMissedInterrupts(0),
	fPollMode(false),
	fTraceCommands(kTraceCommandBudget),
	fTraceIo(kTraceIoBudget),
	fScanSemaphore(-1),
	fSdioIrqSemaphore(-1),
	fSdioIrqEnabled(false),
	fSdioIrqCount(0),
	fUnhandledCount(0),
	fSoftStatus(0),
	fNoLowPower(false),
	fResetDone(false),
	fGpio(NULL),
	fCardType(CARD_TYPE_UNKNOWN),
	fCurrentHz(0),
	fBusWidth(1),
	fSendInit(true)
{
	memset(&fConfig, 0, sizeof(fConfig));
	memset(fBounce, 0, sizeof(fBounce));
	mutex_init(&fLock, "dw_mmc host");
	B_INITIALIZE_SPINLOCK(&fIntmaskLock);
	fInterruptCondition.Init(this, "dw_mmc interrupt");
}


DwMmcHost::~DwMmcHost()
{
	if (fRegs != NULL) {
		_Write(kDwIntmask, 0);
		_Write(kDwCtrl, _Read(kDwCtrl) & ~(uint32)kDwCtrlIntEnable);
	}
	if (fInterruptInstalled)
		remove_io_interrupt_handler(fConfig.irq, _InterruptHandler, this);
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
	delete fGpio;
	mutex_destroy(&fLock);
}


status_t
DwMmcHost::_CheckJh7110Clocks()
{
	// Read-only look at the JH7110 SYSCRG gates for this controller. We do
	// not drive clocks (firmware does), but if the AHB/card clocks are gated
	// a register access to the controller can stall the bus, so bail out.
	int index;
	if (fConfig.physBase == kJh7110Sdio0Base)
		index = 0;
	else if (fConfig.physBase == kJh7110Sdio1Base)
		index = 1;
	else
		return B_OK;

	void* crg = NULL;
	area_id area = map_physical_memory("jh7110 syscrg (dw_mmc check)",
		kJh7110SysCrgBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA, &crg);
	if (area < 0) {
		INFO("%s: could not map SYSCRG to check clocks, going on\n",
			fConfig.name);
		return B_OK;
	}

	volatile uint32* clocks = (volatile uint32*)crg;
	uint32 ahb = clocks[kJh7110SysClkSdio0Ahb + index];
	uint32 card = clocks[kJh7110SysClkSdio0Card + index];
	uint32 resetBit = kJh7110SysRstSdio0Ahb + index;
	uint32 resetStatus = clocks[(kJh7110SysRstStatus / 4) + resetBit / 32];
	bool outOfReset = (resetStatus & (1u << (resetBit % 32))) != 0;
	delete_area(area);

	INFO("%s: SYSCRG sdio%d_ahb %#" B_PRIx32 ", sdio%d_sdcard %#" B_PRIx32
		" (gate %s, divider %" B_PRIu32 " from axi_cfg0)\n", fConfig.name,
		index, ahb, index, card, (card & kJh7110ClkEnable) ? "on" : "OFF",
		card & kJh7110ClkDivMask);

	INFO("%s: SYSRST status word %#" B_PRIx32 ": sdio%d AHB %s\n",
		fConfig.name, resetStatus, index,
		outOfReset ? "out of reset" : "IN RESET");

	if ((ahb & kJh7110ClkEnable) == 0 || (card & kJh7110ClkEnable) == 0
		|| !outOfReset) {
		ERROR("%s: firmware left the controller gated or in reset; not "
			"touching it (an access could stall the bus)\n", fConfig.name);
		return B_DEV_NOT_READY;
	}
	return B_OK;
}


status_t
DwMmcHost::Init(const DwMmcConfig& config)
{
	fConfig = config;

	if (fConfig.isJh7110) {
		status_t status = _CheckJh7110Clocks();
		if (status != B_OK)
			return status;
	}

	void* regs = NULL;
	fRegsArea = map_physical_memory("dw_mmc registers", fConfig.physBase,
		fConfig.physSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (fRegsArea < 0) {
		ERROR("%s: cannot map registers\n", fConfig.name);
		return fRegsArea;
	}
	fRegs = (volatile uint8*)regs;

	uint32 verid = _Read(kDwVerid);
	uint32 hcon = _Read(kDwHcon);
	if (verid == 0 || verid == 0xffffffff) {
		ERROR("%s: VERID reads %#" B_PRIx32 "; controller not responding "
			"(held in reset or unclocked?)\n", fConfig.name, verid);
		return B_DEVICE_NOT_FOUND;
	}
	fVersion = verid & 0xffff;

	// Host data (FIFO) width, Linux dw_mci_probe().
	switch (dw_hcon_data_width(hcon)) {
		case 1:
			fFifoWidth = 4;
			break;
		case 2:
			fFifoWidth = 8;
			break;
		case 0:
			ERROR("%s: 16-bit host data width is not supported\n",
				fConfig.name);
			return B_NOT_SUPPORTED;
		default:
			INFO("%s: HCON reports a reserved data width, using 32 bit\n",
				fConfig.name);
			fFifoWidth = 4;
			break;
	}

	if (dw_hcon_addr_config(hcon) != 0) {
		// 64-bit IDMAC register layout
		fIdstsReg = 0x090;
		fIdintenReg = 0x094;
	} else {
		fIdstsReg = kDwIdsts;
		fIdintenReg = kDwIdinten;
	}

	uint32 dataOffset = fConfig.dataAddr;
	if (dataOffset == 0) {
		dataOffset = fVersion < kDwVersion240a
			? kDwDataOffsetOld : kDwDataOffset240a;
	}
	fFifo = fRegs + dataOffset;

	INFO("%s: VERID %#" B_PRIx32 " (version %#" B_PRIx32 "), HCON %#" B_PRIx32
		", %" B_PRIu32 "-bit FIFO at +%#" B_PRIx32 ", %u-bit IDMAC addressing"
		"\n", fConfig.name, verid, fVersion, hcon, fFifoWidth * 8, dataOffset,
		dw_hcon_addr_config(hcon) ? 64 : 32);
	INFO("%s: firmware left CTRL %#" B_PRIx32 " PWREN %#" B_PRIx32 " CLKDIV %#"
		B_PRIx32 " CLKENA %#" B_PRIx32 " CTYPE %#" B_PRIx32 " UHS %#" B_PRIx32
		" UHS_EXT %#" B_PRIx32 " CDETECT %#" B_PRIx32 " FIFOTH %#" B_PRIx32
		" BMOD %#" B_PRIx32 "\n", fConfig.name, _Read(kDwCtrl),
		_Read(kDwPwren), _Read(kDwClkdiv), _Read(kDwClkena), _Read(kDwCtype),
		_Read(kDwUhsReg), _Read(kDwUhsRegExt), _Read(kDwCdetect),
		_Read(kDwFifoth), _Read(kDwBmod));

	// Quiesce before reset: no controller or IDMAC interrupts. Firmware that
	// used the IDMAC may leave IDINTEN set, which would raise the line with
	// MINTSTS == 0.
	_Write(kDwIntmask, 0);
	_Write(kDwCtrl, _Read(kDwCtrl) & ~(uint32)(kDwCtrlIntEnable
		| kDwCtrlDmaEnable | kDwCtrlUseIdmac));
	_Write(fIdintenReg, 0);
	_Write(fIdstsReg, 0x3ff);

	if (!_ResetController(kDwCtrlAllResetFlags)) {
		ERROR("%s: controller reset did not complete\n", fConfig.name);
		return B_IO_ERROR;
	}

	// PIO only for now: keep the IDMAC in reset and disabled.
	_Write(kDwBmod, kDwBmodSwReset);

	_Ack(0xffffffff);
	_Write(kDwTmout, 0xffffffff);

	// FIFO thresholds, Linux dw_mci_probe(): RX mark = depth/2 - 1,
	// TX mark = depth/2, MSIZE 2 (only matters for DMA).
	if (fConfig.fifoDepth != 0)
		fFifoDepth = fConfig.fifoDepth;
	else {
		fFifoDepth = ((_Read(kDwFifoth) >> 16) & 0xfff) + 1;
		INFO("%s: no fifo-depth in DT, FIFOTH says %" B_PRIu32 " (may be "
			"half the real size if firmware already programmed it)\n",
			fConfig.name, fFifoDepth);
	}
	if (fFifoDepth < 2)
		fFifoDepth = 2;
	fFifoth = dw_fifoth(2, fFifoDepth / 2 - 1, fFifoDepth / 2);
	_Write(kDwFifoth, fFifoth);

	// Card power. U-Boot normally left it on; only wait if we switch it on.
	if ((_Read(kDwPwren) & 1) == 0) {
		_Write(kDwPwren, 1);
		snooze(std::max(fConfig.powerOnDelayMs, (uint32)10) * 1000);
	}

	// 3.3 V single data rate. Leave the 1.8 V bit alone (on some boards it
	// drives a regulator) but say so: the stack only does 3.3 V modes.
	uint32 uhs = _Read(kDwUhsReg);
	if ((uhs & kDwUhsDdr) != 0)
		_Write(kDwUhsReg, uhs & ~(uint32)kDwUhsDdr);
	if ((uhs & kDwUhs18V) != 0) {
		INFO("%s: firmware left 1.8 V signalling selected (UHS_REG %#"
			B_PRIx32 ")\n", fConfig.name, uhs);
	}

	_Write(kDwCtype, kDwCtype1Bit);
	fBusWidth = 1;

	_Write(kDwClkena, 0);
	_Write(kDwClksrc, 0);
	fCurrentHz = 0;
	status_t status = _SetClockHz(400000);
	if (status != B_OK) {
		ERROR("%s: cannot program the card clock\n", fConfig.name);
		return status;
	}

	fSendInit = true;

	status = install_io_interrupt_handler(fConfig.irq, _InterruptHandler,
		this, 0);
	if (status == B_OK)
		fInterruptInstalled = true;
	else {
		ERROR("%s: cannot install handler for interrupt %" B_PRIu64 " (%s); "
			"polling instead\n", fConfig.name, fConfig.irq, strerror(status));
		fPollMode = true;
	}

	_Ack(0xffffffff);
	_Write(kDwIntmask, fConfig.cdUsable ? (uint32)kDwIntCd : 0);
	_Write(kDwCtrl, _Read(kDwCtrl) | kDwCtrlIntEnable);

	// Beacon: SDIO power sequence (Wi-Fi reset GPIO) and pin logging.
	_SetupJh7110Gpio();
	if (fConfig.resetGpio >= 0 && fConfig.resetFromDt)
		_SdioPowerSequence("mmc-pwrseq in DT");

	INFO("%s: ready, ciu %" B_PRIu32 " Hz, max %" B_PRIu32 " Hz, bus width %"
		B_PRIu32 ", FIFO %" B_PRIu32 " x %" B_PRIu32 " bytes, irq %" B_PRIu64
		", card detect %s\n", fConfig.name, fConfig.busHz, fConfig.maxHz,
		fConfig.busWidth, fFifoDepth, fFifoWidth, fConfig.irq,
		fConfig.cdUsable ? "CDETECT register" : "probe by command");
	return B_OK;
}


// #pragma mark - low level helpers


bool
DwMmcHost::_ResetController(uint32 flags)
{
	// Linux dw_mci_ctrl_reset()
	_Write(kDwCtrl, _Read(kDwCtrl) | flags);

	bigtime_t deadline = system_time() + 500000;
	while ((_Read(kDwCtrl) & flags) != 0) {
		if (system_time() > deadline) {
			ERROR("%s: reset timeout (CTRL %#" B_PRIx32 ", flags %#" B_PRIx32
				")\n", fConfig.name, _Read(kDwCtrl), flags);
			return false;
		}
		spin(1);
	}
	return true;
}


status_t
DwMmcHost::_UpdateClockRegisters()
{
	// Tell the CIU to load CLKDIV/CLKSRC/CLKENA (Linux mci_send_cmd() with
	// SDMMC_CMD_UPD_CLK, U-Boot dwmci_setup_bus()). No command-done follows.
	_Write(kDwCmdarg, 0);
	_Write(kDwCmd, kDwCmdStart | kDwCmdUpdClk | kDwCmdPrvDatWait);

	bigtime_t deadline = system_time() + 500000;
	while ((_Read(kDwCmd) & kDwCmdStart) != 0) {
		if (system_time() > deadline) {
			ERROR("%s: clock update not taken (CMD %#" B_PRIx32 ", STATUS %#"
				B_PRIx32 ")\n", fConfig.name, _Read(kDwCmd),
				_Read(kDwStatus));
			return B_TIMED_OUT;
		}
		spin(1);
	}

	uint32 rintsts = _Status();
	if ((rintsts & kDwIntHle) != 0) {
		_Ack(kDwIntHle);
		ERROR("%s: hardware locked error on clock update\n", fConfig.name);
		return B_ERROR;
	}
	return B_OK;
}


status_t
DwMmcHost::_SetClockHz(uint32 hz)
{
	if (hz == 0) {
		_Write(kDwClkena, 0);
		fCurrentHz = 0;
		return _UpdateClockRegisters();
	}

	uint32 busHz = fConfig.busHz;
	uint32 maxHz = fConfig.maxHz != 0 ? std::min(fConfig.maxHz, busHz) : busHz;
	if (hz > maxHz)
		hz = maxHz;
	if (fConfig.maxKhzSetting != 0 && hz > fConfig.maxKhzSetting * 1000)
		hz = fConfig.maxKhzSetting * 1000;
	if (hz == fCurrentHz)
		return B_OK;

	// Linux dw_mci_setup_bus(): card clock = ciu / (2 * CLKDIV), CLKDIV 0
	// means bypass. Round the divider up so the card is never overclocked.
	uint32 div = busHz / hz;
	if (busHz % hz != 0 && busHz > hz)
		div++;
	div = (busHz != hz) ? (div + 1) / 2 : 0;
	if (div > 0xff) {
		INFO("%s: %" B_PRIu32 " Hz is below the divider range, using the "
			"slowest clock\n", fConfig.name, hz);
		div = 0xff;
	}

	_Write(kDwClkena, 0);
	_Write(kDwClksrc, 0);
	status_t status = _UpdateClockRegisters();
	if (status != B_OK)
		return status;

	_Write(kDwClkdiv, div);
	status = _UpdateClockRegisters();
	if (status != B_OK)
		return status;

	// Low-power mode stops the card clock while idle; fine for SD/eMMC
	// memory cards. Linux turns it off for SDIO interrupts
	// (dw_mci_prepare_sdio_irq), we turn it off for any SDIO card.
	_Write(kDwClkena, kDwClkenEnable | (fNoLowPower ? 0 : kDwClkenLowPower));
	status = _UpdateClockRegisters();
	if (status != B_OK)
		return status;

	fCurrentHz = hz;
	uint32 actual = div != 0 ? busHz / (2 * div) : busHz;
	INFO("%s: card clock %" B_PRIu32 " Hz (ciu %" B_PRIu32 " Hz, CLKDIV %"
		B_PRIu32 ")\n", fConfig.name, actual, busHz, div);
	return B_OK;
}


void
DwMmcHost::_EnableInterrupts(uint32 mask)
{
	InterruptsSpinLocker locker(fIntmaskLock);
	_Write(kDwIntmask, _Read(kDwIntmask) | mask);
}


uint32
DwMmcHost::_Status()
{
	// Hardware status plus what the interrupt handler already took and
	// cleared (see _HandleInterrupt).
	return _Read(kDwRintsts) | (uint32)atomic_get((int32*)&fSoftStatus);
}


void
DwMmcHost::_Ack(uint32 bits)
{
	atomic_and((int32*)&fSoftStatus, ~(int32)bits);
	_Write(kDwRintsts, bits);
}


status_t
DwMmcHost::_WaitForStatus(uint32 mask, bigtime_t deadline, uint32& status)
{
	// The interrupt is only a doorbell: RINTSTS is the truth.
	bigtime_t spinUntil = system_time() + kSpinBeforeSleep;
	for (;;) {
		status = _Status();
		if ((status & mask) != 0)
			return B_OK;

		bigtime_t now = system_time();
		if (now >= deadline)
			return B_TIMED_OUT;
		if (now < spinUntil)
			continue;

		if (fPollMode) {
			snooze(20);
			continue;
		}

		ConditionVariableEntry entry;
		fInterruptCondition.Add(&entry);
		_EnableInterrupts(mask);

		status = _Status();
		if ((status & mask) != 0)
			return B_OK;

		bigtime_t timeout = std::min(deadline - now, kInterruptFallbackPoll);
		status_t result = entry.Wait(B_RELATIVE_TIMEOUT, timeout);
		if (result == B_TIMED_OUT) {
			status = _Status();
			if ((status & mask) != 0) {
				// The event is there but no interrupt woke us.
				int32 missed = atomic_add(&fMissedInterrupts, 1) + 1;
				if (missed == 8 && atomic_get(&fInterruptCount) == 0) {
					ERROR("%s: no interrupts arrive from PLIC source %"
						B_PRIu64 "; switching to polling\n", fConfig.name,
						fConfig.irq);
					fPollMode = true;
				}
				return B_OK;
			}
		}
	}
}


status_t
DwMmcHost::_WaitWhileBusy(bigtime_t timeout)
{
	// Card holds DAT0 low while busy (R1b, after writes).
	bigtime_t start = system_time();
	bigtime_t deadline = start + timeout;
	while ((_Read(kDwStatus) & kDwStatusBusy) != 0) {
		bigtime_t now = system_time();
		if (now > deadline)
			return B_TIMED_OUT;
		if (now - start < 100)
			continue;
		snooze(now - start < 10000 ? 50 : 1000);
	}
	return B_OK;
}


// #pragma mark - interrupt


int32
DwMmcHost::_InterruptHandler(void* data)
{
	return ((DwMmcHost*)data)->_HandleInterrupt();
}


int32
DwMmcHost::_HandleInterrupt()
{
	uint32 pending = _Read(kDwMintsts);

	// Defensive: IDMAC status is never enabled by us, but ack it if set.
	uint32 idsts = _Read(fIdstsReg) & 0x3ff;
	if (idsts != 0)
		_Write(fIdstsReg, idsts);

	// Beacon: like Linux dw_mci_interrupt(), take the event bits into a
	// software copy and clear them in hardware here. On the JH7110 the line
	// stays up while a (masked) raw status bit is set (seen: RINTSTS 0x8 =
	// data-over with INTMASK not including it, interrupts back to back),
	// and the kernel disables a line that keeps firing unclaimed.
	uint32 raw = _Read(kDwRintsts) & kDwIntAllButCdSdio;
	if (raw != 0) {
		atomic_or((int32*)&fSoftStatus, (int32)raw);
		_Write(kDwRintsts, raw);
	}

	if (pending == 0) {
		if (idsts != 0 || raw != 0) {
			if (raw != 0) {
				atomic_add(&fInterruptCount, 1);
				fInterruptCondition.NotifyAll();
			}
			return B_HANDLED_INTERRUPT;
		}
		// Beacon: the kernel disables a line when >99% of 10000 interrupts
		// are unclaimed ("Disabling unhandled io interrupt 74" took the
		// Wi-Fi SDIO host down under load). Record the state that fired.
		int32 n = atomic_add(&fUnhandledCount, 1) + 1;
		if (n <= 8 || n % 2000 == 0) {
			dprintf("dw_mmc: %s: interrupt with nothing pending #%" B_PRId32
				": CTRL %#" B_PRIx32 " INTMASK %#" B_PRIx32 " RINTSTS %#"
				B_PRIx32 " STATUS %#" B_PRIx32 " IDSTS %#" B_PRIx32
				" (handled %" B_PRId32 ", sdio %" B_PRId32 ")\n",
				fConfig.name, n, _Read(kDwCtrl), _Read(kDwIntmask),
				_Read(kDwRintsts), _Read(kDwStatus), _Read(fIdstsReg),
				atomic_get(&fInterruptCount), atomic_get(&fSdioIrqCount));
		}
		return B_UNHANDLED_INTERRUPT;
	}

	// Beacon: SDIO card interrupt (Linux dw_mci_interrupt): ack, mask until
	// the bus manager's handlers have run, wake the bus manager's thread.
	if ((pending & kDwIntSdio) != 0) {
		_Write(kDwRintsts, kDwIntSdio);
		{
			SpinLocker locker(fIntmaskLock);
			_Write(kDwIntmask, _Read(kDwIntmask) & ~(uint32)kDwIntSdio);
		}
		atomic_add(&fSdioIrqCount, 1);
		if (fSdioIrqSemaphore >= 0)
			release_sem_etc(fSdioIrqSemaphore, 1, B_DO_NOT_RESCHEDULE);
	}

	// Mask what fired so the line drops; the waiting thread clears RINTSTS
	// and re-enables what it needs next.
	uint32 events = pending & ~(uint32)(kDwIntCd | kDwIntSdio);
	if (events != 0) {
		SpinLocker locker(fIntmaskLock);
		_Write(kDwIntmask, _Read(kDwIntmask) & ~events);
	}

	if ((pending & kDwIntCd) != 0) {
		_Write(kDwRintsts, kDwIntCd);
		if (fScanSemaphore >= 0 && (_Read(kDwCdetect) & 1) == 0)
			release_sem_etc(fScanSemaphore, 1, B_DO_NOT_RESCHEDULE);
	}

	// Beacon: read back so the INTMASK write reaches the controller (and
	// its interrupt line drops) before the PLIC is told we are done.
	_Read(kDwIntmask);

	atomic_add(&fInterruptCount, 1);
	fInterruptCondition.NotifyAll();
	return B_HANDLED_INTERRUPT;
}


// #pragma mark - commands


DwMmcHost::ReplyType
DwMmcHost::_ReplyTypeFor(uint8_t command, uint32_t argument) const
{
	// Same mapping as Haiku's SdhciBus::ExecuteCommand().
	bool mmc = fCardType == CARD_TYPE_MMC
		|| fCardType == CARD_TYPE_MMC_EXTENDED_CAPACITY;

	switch (command) {
		case GO_IDLE_STATE:
			return kReplyNone;
		case SD_APP_CMD:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
		case 13:	// SEND_STATUS
			return kReplyR1;
		case SELECT_DESELECT_CARD:
			// Deselect (RCA 0) gets no response.
			return (argument >> 16) == 0 ? kReplyNone : kReplyR1b;
		case SD_ERASE:
		case SD_STOP_TRANSMISSION:
			return kReplyR1b;
		case ALL_SEND_CID:
		case SEND_CSD:
			return kReplyR2;
		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND:
			return kReplyR3;
		case SD_SET_BUS_WIDTH:	// also MMC_SWITCH
			return mmc ? kReplyR1b : kReplyR1;
		case SD_SEND_RELATIVE_ADDR:	// also MMC_SET_RELATIVE_ADDR
			return mmc ? kReplyR1 : kReplyR6;
		case SD_SEND_IF_COND:	// also MMC_SEND_EXT_CSD
			return mmc ? kReplyR1 : kReplyR7;
		case SD_READ_SINGLE_BLOCK:
		case SD_READ_MULTIPLE_BLOCKS:
		case SD_WRITE_SINGLE_BLOCK:
		case SD_WRITE_MULTIPLE_BLOCKS:
			return kReplyR1;
		// Beacon: SDIO. R4 has no valid CRC (like R3); R5 has the R1 shape.
		case SD_IO_SEND_OP_COND:
			return kReplyR3;
		case SD_IO_RW_DIRECT:
		case SD_IO_RW_EXTENDED:
			return kReplyR1;
		default:
			return kReplyInvalid;
	}
}


status_t
DwMmcHost::_ExecuteCommandLocked(uint8_t command, uint32_t argument,
	uint32_t* response, const DataPhase* data)
{
	ReplyType reply = _ReplyTypeFor(command, argument);
	if (reply == kReplyInvalid) {
		ERROR("%s: unknown command %u\n", fConfig.name, command);
		return B_BAD_DATA;
	}

	// Command register flags, Linux dw_mci_prepare_command().
	uint32 cmdr = (command & kDwCmdIndexMask) | kDwCmdStart | kDwCmdUseHoldReg;
	// Linux dw_mci_prepare_command(): a CMD52 write to CCCR ABORT is a stop.
	if (command == GO_IDLE_STATE || command == SD_STOP_TRANSMISSION
		|| (command == SD_IO_RW_DIRECT
			&& ((argument >> 9) & 0x1ffff) == kSdioCccrAbort))
		cmdr |= kDwCmdStopAbort;
	else if (data != NULL)
		cmdr |= kDwCmdPrvDatWait;

	switch (reply) {
		case kReplyNone:
			break;
		case kReplyR2:
			cmdr |= kDwCmdRespExp | kDwCmdRespLong | kDwCmdRespCrc;
			break;
		case kReplyR3:
			// OCR replies have no valid CRC
			cmdr |= kDwCmdRespExp;
			break;
		default:
			cmdr |= kDwCmdRespExp | kDwCmdRespCrc;
			break;
	}

	if (data != NULL) {
		cmdr |= kDwCmdDataExp;
		if (data->isWrite)
			cmdr |= kDwCmdDataWrite;
	}

	// First command after power-up: send 80 init clocks (Linux
	// DW_MMC_CARD_NEED_INIT, StarFive U-Boot DWMCI_CMD_SEND_INIT).
	if (fSendInit) {
		cmdr |= kDwCmdInit;
		fSendInit = false;
	}

	if ((cmdr & kDwCmdPrvDatWait) != 0 && _WaitWhileBusy(500000) != B_OK)
		INFO("%s: card busy before cmd%u; trying anyway\n", fConfig.name,
			command);

	// The CIU must have taken the previous command.
	bigtime_t deadline = system_time() + 500000;
	while ((_Read(kDwCmd) & kDwCmdStart) != 0) {
		if (system_time() > deadline) {
			ERROR("%s: previous command stuck (CMD %#" B_PRIx32 ")\n",
				fConfig.name, _Read(kDwCmd));
			_RecoverFromDataError();
			return B_BUSY;
		}
		spin(1);
	}

	_Ack(kDwIntAllButCdSdio);
	_Write(kDwCmdarg, argument);
	_Write(kDwCmd, cmdr);

	uint32 status = 0;
	status_t result = _WaitForStatus(kDwIntCmdDone | kDwIntHle,
		system_time() + 1000000, status);
	// RTO/RCRC/RE are raised together with CMD_DONE.
	_Ack(status & (kDwIntCmdDone | kDwIntCmdError));

	if (result == B_OK) {
		if ((status & kDwIntHle) != 0)
			result = B_ERROR;
		else if ((status & kDwIntRto) != 0)
			result = B_TIMED_OUT;
		else if ((cmdr & kDwCmdRespCrc) != 0 && (status & kDwIntRcrc) != 0)
			result = B_BAD_VALUE;
		else if ((status & kDwIntRespErr) != 0) {
			ERROR("%s: cmd%u: response error, RINTSTS %#" B_PRIx32 "\n",
				fConfig.name, command, status);
			result = B_IO_ERROR;
		}
	} else {
		ERROR("%s: cmd%u got no command-done (RINTSTS %#" B_PRIx32
			", STATUS %#" B_PRIx32 ")\n", fConfig.name, command, status,
			_Read(kDwStatus));
	}

	if (result == B_OK && response != NULL) {
		if (reply == kReplyR2) {
			// DW keeps all 136-bit reply bits R[127:0] in RESP0..3, with the
			// CRC in RESP0[7:0]. Haiku's stack expects the SDHCI layout,
			// R[127:8] in bits 119:0 (see SDCid/mmc_disk_csd), so drop the
			// CRC byte.
			uint32 r0 = _Read(kDwResp0);
			uint32 r1 = _Read(kDwResp1);
			uint32 r2 = _Read(kDwResp2);
			uint32 r3 = _Read(kDwResp3);
			response[0] = (r0 >> 8) | (r1 << 24);
			response[1] = (r1 >> 8) | (r2 << 24);
			response[2] = (r2 >> 8) | (r3 << 24);
			response[3] = r3 >> 8;
		} else if (reply != kReplyNone)
			response[0] = _Read(kDwResp0);
	}

	if (fTraceCommands > 0) {
		fTraceCommands--;
		INFO("%s: cmd%u arg %#" B_PRIx32 " cmdr %#" B_PRIx32 " -> %s "
			"(RINTSTS %#" B_PRIx32 ", resp0 %#" B_PRIx32 ")\n", fConfig.name,
			command, argument, cmdr, strerror(result), status,
			_Read(kDwResp0));
	}

	if (result != B_OK) {
		if ((status & kDwIntHle) != 0 || status == 0)
			_RecoverFromDataError();
		return result;
	}

	if (reply == kReplyR1b && data == NULL) {
		bigtime_t timeout = command == SD_ERASE ? 60000000 : 5000000;
		if (_WaitWhileBusy(timeout) != B_OK) {
			ERROR("%s: card still busy after cmd%u\n", fConfig.name, command);
			return B_TIMED_OUT;
		}
	}

	return B_OK;
}


status_t
DwMmcHost::ExecuteCommand(uint8_t command, uint32_t argument,
	uint32_t* response)
{
	MutexLocker locker(fLock);
	return _ExecuteCommandLocked(command, argument, response, NULL);
}


void
DwMmcHost::_RecoverFromDataError()
{
	// Linux dw_mci_reset() for the non-DMA case: reset the CIU and FIFO,
	// clear interrupts, then reload the clock registers into the CIU.
	if (!_ResetController(kDwCtrlReset | kDwCtrlFifoReset))
		ERROR("%s: recovery reset failed\n", fConfig.name);
	_Ack(kDwIntAllButCdSdio);
	_UpdateClockRegisters();
}


// #pragma mark - data transfer


status_t
DwMmcHost::_CopyToVecs(const DataPhase& data, size_t& vecIndex,
	size_t& vecOffset, const uint8* from, size_t length)
{
	while (length > 0) {
		if (vecIndex >= data.vecCount) {
			ERROR("%s: I/O vectors shorter than the transfer\n",
				fConfig.name);
			return B_BAD_VALUE;
		}
		const generic_io_vec& vec = data.vecs[vecIndex];
		size_t chunk = std::min(length, (size_t)(vec.length - vecOffset));
		if (chunk > 0) {
			status_t status = vm_memcpy_to_physical(
				(phys_addr_t)(vec.base + vecOffset), from, chunk, false);
			if (status != B_OK)
				return status;
			from += chunk;
			length -= chunk;
			vecOffset += chunk;
		}
		if (vecOffset >= vec.length) {
			vecIndex++;
			vecOffset = 0;
		}
	}
	return B_OK;
}


status_t
DwMmcHost::_CopyFromVecs(const DataPhase& data, size_t& vecIndex,
	size_t& vecOffset, uint8* to, size_t length)
{
	while (length > 0) {
		if (vecIndex >= data.vecCount) {
			ERROR("%s: I/O vectors shorter than the transfer\n",
				fConfig.name);
			return B_BAD_VALUE;
		}
		const generic_io_vec& vec = data.vecs[vecIndex];
		size_t chunk = std::min(length, (size_t)(vec.length - vecOffset));
		if (chunk > 0) {
			status_t status = vm_memcpy_from_physical(to,
				(phys_addr_t)(vec.base + vecOffset), chunk, false);
			if (status != B_OK)
				return status;
			to += chunk;
			length -= chunk;
			vecOffset += chunk;
		}
		if (vecOffset >= vec.length) {
			vecIndex++;
			vecOffset = 0;
		}
	}
	return B_OK;
}


static status_t
data_error_to_status(uint32 rintsts)
{
	if ((rintsts & (kDwIntDrto | kDwIntHto)) != 0)
		return B_TIMED_OUT;
	if ((rintsts & kDwIntHle) != 0)
		return B_ERROR;
	return B_IO_ERROR;
}


status_t
DwMmcHost::_TransferPio(const DataPhase& data, bigtime_t deadline)
{
	// FIFO PIO, after Linux dw_mci_read_data_pio()/dw_mci_write_data_pio()
	// and U-Boot dwmci_data_transfer(), but run from the calling thread.
	// Beacon: the length need not be a multiple of the FIFO width (SDIO
	// CMD53 byte mode). The last FIFO entry then carries the tail bytes
	// (Linux dw_mci_pull_final_bytes); on writes it is zero padded.
	size_t vecIndex = 0;
	size_t vecOffset = 0;
	size_t bufferOffset = 0;
	size_t remaining = data.length;
	const uint32 width = fFifoWidth;
	const size_t bounceEntries = sizeof(fBounce) / width;
	const uint32 ready = data.isWrite ? kDwIntTxdr : kDwIntRxdr;
	uint8* bounce = (uint8*)fBounce;

	while (remaining > 0) {
		uint32 fifoCount = dw_status_fifo_count(_Read(kDwStatus));
		size_t entries = data.isWrite
			? (fifoCount < fFifoDepth ? fFifoDepth - fifoCount : 0)
			: fifoCount;
		entries = std::min(entries, bounceEntries);
		entries = std::min(entries, (remaining + width - 1) / width);

		if (entries > 0) {
			size_t bytes = std::min(entries * width, remaining);
			status_t status = B_OK;
			if (data.isWrite) {
				if (bytes < entries * width)
					memset(bounce + bytes, 0, entries * width - bytes);
				if (data.buffer != NULL) {
					memcpy(bounce, data.buffer + bufferOffset, bytes);
					bufferOffset += bytes;
				} else {
					status = _CopyFromVecs(data, vecIndex, vecOffset, bounce,
						bytes);
					if (status != B_OK)
						return status;
				}
				io_barrier_before_write();
				if (width == 8) {
					for (size_t i = 0; i < entries; i++)
						*(volatile uint64*)fFifo = ((uint64*)bounce)[i];
				} else {
					for (size_t i = 0; i < entries; i++)
						*(volatile uint32*)fFifo = ((uint32*)bounce)[i];
				}
			} else {
				if (width == 8) {
					for (size_t i = 0; i < entries; i++)
						((uint64*)bounce)[i] = *(volatile uint64*)fFifo;
				} else {
					for (size_t i = 0; i < entries; i++)
						((uint32*)bounce)[i] = *(volatile uint32*)fFifo;
				}
				io_barrier_after_read();
				if (data.buffer != NULL) {
					memcpy(data.buffer + bufferOffset, bounce, bytes);
					bufferOffset += bytes;
				} else {
					status = _CopyToVecs(data, vecIndex, vecOffset, bounce,
						bytes);
					if (status != B_OK)
						return status;
				}
			}
			remaining -= bytes;
			continue;
		}

		uint32 rintsts = _Status();
		if ((rintsts & kDwIntDataError) != 0) {
			ERROR("%s: data error, RINTSTS %#" B_PRIx32 ", %" B_PRIuSIZE
				" of %" B_PRIuSIZE " bytes left\n", fConfig.name, rintsts,
				remaining, data.length);
			return data_error_to_status(rintsts);
		}
		if (!data.isWrite && (rintsts & kDwIntDataOver) != 0) {
			// Data-over with an empty FIFO and bytes still missing.
			if (dw_status_fifo_count(_Read(kDwStatus)) == 0) {
				ERROR("%s: short read, %" B_PRIuSIZE " bytes missing\n",
					fConfig.name, remaining);
				return B_IO_ERROR;
			}
			continue;
		}

		// Clear the ready flag before looking again, so a FIFO threshold
		// crossing from now on is not lost.
		_Ack(ready);
		fifoCount = dw_status_fifo_count(_Read(kDwStatus));
		if (data.isWrite ? fifoCount < fFifoDepth : fifoCount > 0)
			continue;

		status_t status = _WaitForStatus(ready | kDwIntDataOver
			| kDwIntDataError, deadline, rintsts);
		if (status != B_OK) {
			ERROR("%s: data timeout, %" B_PRIuSIZE " of %" B_PRIuSIZE
				" bytes left (RINTSTS %#" B_PRIx32 ", STATUS %#" B_PRIx32
				")\n", fConfig.name, remaining, data.length, rintsts,
				_Read(kDwStatus));
			return B_TIMED_OUT;
		}
	}

	uint32 rintsts = 0;
	status_t status = _WaitForStatus(kDwIntDataOver | kDwIntDataError,
		deadline, rintsts);
	if (status != B_OK) {
		ERROR("%s: no data-over (RINTSTS %#" B_PRIx32 ", STATUS %#" B_PRIx32
			")\n", fConfig.name, rintsts, _Read(kDwStatus));
		return B_TIMED_OUT;
	}
	if ((rintsts & kDwIntDataError) != 0) {
		ERROR("%s: data error at end, RINTSTS %#" B_PRIx32 "\n", fConfig.name,
			rintsts);
		return data_error_to_status(rintsts);
	}

	_Ack(kDwIntDataOver | kDwIntRxdr | kDwIntTxdr);
	return B_OK;
}


status_t
DwMmcHost::DoIO(uint8_t command, IOOperation* operation, bool offsetAsSectors)
{
	MutexLocker locker(fLock);

	off_t offset = operation->Offset();
	generic_size_t length = operation->Length();
	bool isWrite = operation->IsWrite();

	if (offset < 0 || offset % kBlockSize != 0 || length == 0
		|| length % kBlockSize != 0 || length > 0xffffffffULL) {
		ERROR("%s: bad I/O %s %" B_PRIuGENADDR " bytes at %" B_PRIdOFF "\n",
			fConfig.name, isWrite ? "write" : "read", length, offset);
		return B_BAD_VALUE;
	}

	uint64 address = offsetAsSectors ? (uint64)offset / kBlockSize
		: (uint64)offset;
	if (address > 0xffffffffULL) {
		ERROR("%s: I/O address %#" B_PRIx64 " out of range\n", fConfig.name,
			address);
		return B_BAD_VALUE;
	}

	DataPhase data;
	data.isWrite = isWrite;
	data.vecs = operation->Vecs();
	data.vecCount = operation->VecCount();
	data.buffer = NULL;
	data.length = (size_t)length;

	bool multiBlock = command == SD_READ_MULTIPLE_BLOCKS
		|| command == SD_WRITE_MULTIPLE_BLOCKS;

	if (fTraceIo > 0) {
		fTraceIo--;
		INFO("%s: %s %" B_PRIuGENADDR " bytes at %" B_PRIdOFF " (cmd%u, %"
			B_PRIuSIZE " vecs)\n", fConfig.name, isWrite ? "write" : "read",
			length, offset, command, data.vecCount);
	}

	// Data path setup, U-Boot dwmci_send_cmd() fifo_mode branch.
	if (!_ResetController(kDwCtrlFifoReset))
		return B_IO_ERROR;
	_Write(kDwFifoth, fFifoth);
	_Write(kDwTmout, 0xffffffff);
	_Write(kDwBlksiz, kBlockSize);
	_Write(kDwBytcnt, (uint32)length);

	// Generous deadline: 10x the wire time plus 2 s.
	uint64 hz = fCurrentHz != 0 ? fCurrentHz : 400000;
	bigtime_t wireTime = (bigtime_t)((uint64)length * 8 * 1000000
		/ (hz * fBusWidth));
	bigtime_t deadline = system_time() + 2000000 + wireTime * 10;

	uint32 response = 0;
	status_t status = _ExecuteCommandLocked(command, (uint32)address,
		&response, &data);
	if (status != B_OK) {
		_RecoverFromDataError();
		return status;
	}

	status = _TransferPio(data, deadline);
	if (status != B_OK) {
		_RecoverFromDataError();
		if (multiBlock) {
			uint32 stopResponse = 0;
			_ExecuteCommandLocked(SD_STOP_TRANSMISSION, 0, &stopResponse, NULL);
		}
		return status;
	}

	if (multiBlock) {
		uint32 stopResponse = 0;
		status = _ExecuteCommandLocked(SD_STOP_TRANSMISSION, 0, &stopResponse,
			NULL);
		if (status != B_OK)
			ERROR("%s: stop after transfer failed: %s\n", fConfig.name,
				strerror(status));
	} else if (isWrite && _WaitWhileBusy(5000000) != B_OK) {
		ERROR("%s: card still busy after write\n", fConfig.name);
		status = B_TIMED_OUT;
	}

	return status;
}


// #pragma mark - bus control


status_t
DwMmcHost::SetClock(uint32 kilohertz)
{
	MutexLocker locker(fLock);
	return _SetClockHz(kilohertz * 1000);
}


void
DwMmcHost::SetBusWidth(int width)
{
	MutexLocker locker(fLock);

	uint32 ctype;
	switch (width) {
		case 1:
			ctype = kDwCtype1Bit;
			break;
		case 4:
			ctype = kDwCtype4Bit;
			break;
		case 8:
			ctype = kDwCtype8Bit;
			break;
		default:
			ERROR("%s: bad bus width %d\n", fConfig.name, width);
			return;
	}
	if ((uint32)width > fConfig.busWidth) {
		// The card has already been switched by the time we are told.
		INFO("%s: %d-bit bus requested but DT bus-width is %" B_PRIu32 "\n",
			fConfig.name, width, fConfig.busWidth);
	}
	_Write(kDwCtype, ctype);
	fBusWidth = width;
	INFO("%s: %d-bit data bus\n", fConfig.name, width);
}


void
DwMmcHost::SetCardType(card_type type)
{
	MutexLocker locker(fLock);
	fCardType = type;

	if (type == CARD_TYPE_SDIO && !fNoLowPower) {
		// Low-power mode stops the card clock when idle, and then the card
		// cannot signal its interrupt (Linux dw_mci_prepare_sdio_irq()).
		fNoLowPower = true;
		if (fCurrentHz != 0)
			_ApplyClockEnable();
		SDIO_INFO("%s: SDIO card: card clock low-power gating off\n",
			fConfig.name);
	}
}


status_t
DwMmcHost::_ApplyClockEnable()
{
	_Write(kDwClkena, kDwClkenEnable | (fNoLowPower ? 0 : kDwClkenLowPower));
	return _UpdateClockRegisters();
}


bool
DwMmcHost::_ProbeSdio(int tries)
{
	// Linux mmc_rescan_try_freq() order: SDIO I/O reset (CMD52 write of RES
	// to CCCR ABORT), CMD0, CMD8, then CMD5 with argument 0 (inquiry). Only
	// SDIO cards answer CMD5. An unselected card does not answer the CMD52;
	// that timeout is expected.
	uint32_t response[4] = {};
	_ExecuteCommandLocked(SD_IO_RW_DIRECT, (1u << 31) | (kSdioCccrAbort << 9)
		| kSdioAbortRes, response, NULL);
	_ExecuteCommandLocked(GO_IDLE_STATE, 0, NULL, NULL);
	snooze(2000);
	_ExecuteCommandLocked(SD_SEND_IF_COND, 0x1aa, response, NULL);

	status_t status = B_ERROR;
	for (int i = 0; i < tries; i++) {
		status = _ExecuteCommandLocked(SD_IO_SEND_OP_COND, 0, response, NULL);
		if (status == B_OK) {
			uint32 ocr = response[0];
			SDIO_INFO("%s: card answers CMD5 (try %d): OCR %#" B_PRIx32
				", %" B_PRIu32 " I/O function(s), memory %s\n", fConfig.name,
				i + 1, ocr, (ocr >> 28) & 7,
				(ocr & (1u << 27)) != 0 ? "present" : "absent");
			return true;
		}
		if (i + 1 < tries)
			snooze(20000);
	}
	SDIO_INFO("%s: no answer to CMD5 after %d tr%s (%s)\n", fConfig.name,
		tries, tries == 1 ? "y" : "ies", strerror(status));
	return false;
}


void
DwMmcHost::_SdioPowerSequence(const char* why)
{
	// Linux pwrseq_simple + mmc_power_up(): reset asserted, power stable,
	// reset released, clock running, wait. VMMC/VQMMC are always on on the
	// Orange Pi RV and PWREN is already set.
	if (fGpio == NULL || fConfig.resetGpio < 0)
		return;

	bool assertLevel = !fConfig.resetActiveLow;
	uint32 postDelay = fConfig.resetPostDelayMs > kSdioMinPostDelayMs
		? fConfig.resetPostDelayMs : kSdioMinPostDelayMs;
	SDIO_INFO("%s: power sequence (%s): GPIO%" B_PRId32 " reset asserted "
		"(%s) for %" B_PRId64 " ms, then released, then %" B_PRIu32 " ms\n",
		fConfig.name, why, fConfig.resetGpio, assertLevel ? "HIGH" : "LOW",
		kSdioResetHold / 1000, postDelay);

	fGpio->SetOutput(fConfig.resetGpio, assertLevel);
	snooze(kSdioResetHold);
	fGpio->SetOutput(fConfig.resetGpio, !assertLevel);
	snooze(postDelay * 1000);

	// The card was just powered up: send the 80 init clocks again.
	fSendInit = true;
	fResetDone = true;
	fGpio->LogPin(fConfig.resetGpio, "Wi-Fi reset, after power sequence");
}


/*!	Beacon: revive an SDIO card that stopped answering (all commands time
	out): power-cycle it through the reset GPIO, then reset the controller
	and go back to 400 kHz, 1-bit, so the bus manager can enumerate it
	again.
*/
status_t
DwMmcHost::PowerCycle()
{
	MutexLocker locker(fLock);
	if (fGpio == NULL || fConfig.resetGpio < 0)
		return B_NOT_SUPPORTED;

	_SdioPowerSequence("recovery");
	if (!_ResetController(kDwCtrlAllResetFlags))
		ERROR("%s: controller reset after power cycle failed\n", fConfig.name);
	_Ack(0xffffffff);
	_Write(kDwCtype, kDwCtype1Bit);
	fBusWidth = 1;
	return _SetClockHz(400000);
}


void
DwMmcHost::_SetupJh7110Gpio()
{
	// Read-only pin logging on the JH7110 sdio0 (the Orange Pi RV Wi-Fi),
	// plus the reset line if we drive one.
	if (!fConfig.isJh7110Sdio0 && fConfig.resetGpio < 0)
		return;

	fGpio = new(std::nothrow) Jh7110SysGpio();
	if (fGpio == NULL || fGpio->Init(kJh7110SysGpioPhys) != B_OK) {
		delete fGpio;
		fGpio = NULL;
		if (fConfig.resetGpio >= 0) {
			ERROR("%s: no sysgpio access, cannot drive reset GPIO%" B_PRId32
				"\n", fConfig.name, fConfig.resetGpio);
		}
		return;
	}

	if (fConfig.isJh7110Sdio0) {
		SDIO_INFO("%s: firmware pin state for the Wi-Fi slot:\n",
			fConfig.name);
		fGpio->LogPin(62, "WL_REG_ON on Orange Pi RV");
		fGpio->LogPin(21, "WL_HOST_WAKE (OOB IRQ, unused)");
		fGpio->LogSd0Pads();
	}
	if (fConfig.resetGpio >= 0) {
		SDIO_INFO("%s: reset GPIO%" B_PRId32 " active %s, %s\n", fConfig.name,
			fConfig.resetGpio, fConfig.resetActiveLow ? "low" : "high",
			fConfig.resetFromDt ? "from DT mmc-pwrseq"
				: "board fallback, used only if no card answers");
		if (fConfig.resetGpio != 62 || !fConfig.isJh7110Sdio0)
			fGpio->LogPin(fConfig.resetGpio, "SDIO reset");
	}
}


bool
DwMmcHost::_ProbeForCard()
{
	// Without a usable card-detect signal (JH7110 SD slots use a GPIO we do
	// not drive), ask the bus whether anything answers before letting the
	// MMC bus manager scan: its scan loops forever on an empty slot. SDIO
	// cards answer CMD5, SD cards answer CMD55, eMMC answers CMD1.
	uint32_t response[4] = {};

	// Beacon: SDIO first, like Linux. With a DT pwrseq the reset was just
	// released, so give the chip time to come up.
	if (_ProbeSdio(fConfig.resetFromDt ? kSdioProbeTries : 1))
		return true;

	_ExecuteCommandLocked(GO_IDLE_STATE, 0, NULL, NULL);
	snooze(2000);
	status_t ifCond = _ExecuteCommandLocked(SD_SEND_IF_COND, 0x1aa, response,
		NULL);
	status_t appCmd = _ExecuteCommandLocked(SD_APP_CMD, 0, response, NULL);
	if (appCmd == B_OK) {
		INFO("%s: SD card answers (CMD8 %s, CMD55 status %#" B_PRIx32 ")\n",
			fConfig.name, strerror(ifCond), response[0]);
		_ExecuteCommandLocked(GO_IDLE_STATE, 0, NULL, NULL);
		return true;
	}

	_ExecuteCommandLocked(GO_IDLE_STATE, 0, NULL, NULL);
	snooze(2000);
	status_t opCond = _ExecuteCommandLocked(MMC_SEND_OP_COND, 0, response,
		NULL);
	_ExecuteCommandLocked(GO_IDLE_STATE, 0, NULL, NULL);
	if (opCond == B_OK) {
		INFO("%s: MMC device answers (OCR %#" B_PRIx32 ")\n", fConfig.name,
			response[0]);
		return true;
	}

	// Beacon: nothing answered. On the Orange Pi RV the Wi-Fi chip is held
	// in reset by WL_REG_ON; release it and look again.
	if (fConfig.resetGpio >= 0 && fConfig.resetHeuristic && !fResetDone) {
		_SdioPowerSequence("no card answered, board fallback");
		if (_ProbeSdio(kSdioProbeTries))
			return true;
	}

	INFO("%s: no card answers (CMD5, CMD8 %s, CMD55 %s, CMD1 %s); not "
		"scanning. Insert a card and reboot.\n", fConfig.name,
		strerror(ifCond), strerror(appCmd), strerror(opCond));
	return false;
}


void
DwMmcHost::SetScanSemaphore(sem_id semaphore)
{
	MutexLocker locker(fLock);
	fScanSemaphore = semaphore;

	bool present;
	if (fConfig.cdUsable) {
		uint32 cdetect = _Read(kDwCdetect);
		present = (cdetect & 1) == 0;
		INFO("%s: CDETECT %#" B_PRIx32 ", card %s\n", fConfig.name, cdetect,
			present ? "present" : "absent");
		_EnableInterrupts(kDwIntCd);
	} else
		present = _ProbeForCard();

	if (present)
		release_sem(semaphore);
}


void
DwMmcHost::TerminateBus()
{
	MutexLocker locker(fLock);
	// Stop the card clock. Card power (PWREN) stays on: on the JH7110 boards
	// the SD supply is not switched by this controller and firmware expects
	// it on.
	_Write(kDwIntmask, fConfig.cdUsable ? (uint32)kDwIntCd : 0);
	_SetClockHz(0);
}


// #pragma mark - SDIO (Beacon)


status_t
DwMmcHost::ExecuteBufferCommand(uint8_t command, uint32_t argument,
	uint32_t* response, const mmc_buffer_transfer* transfer)
{
	if (transfer == NULL || transfer->buffer == NULL
		|| transfer->block_size == 0
		|| transfer->block_size > kSdioMaxBlockSize
		|| transfer->block_count == 0 || transfer->block_count > 511) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(fLock);

	size_t length = (size_t)transfer->block_size * transfer->block_count;
	DataPhase data;
	data.isWrite = transfer->is_write;
	data.vecs = NULL;
	data.vecCount = 0;
	data.buffer = (uint8*)transfer->buffer;
	data.length = length;

	if (fTraceIo > 0) {
		fTraceIo--;
		SDIO_INFO("%s: cmd%u arg %#" B_PRIx32 " %s %" B_PRIuSIZE " bytes (%"
			B_PRIu32 " x %" B_PRIu32 ")\n", fConfig.name, command, argument,
			data.isWrite ? "write" : "read", length, transfer->block_count,
			transfer->block_size);
	}

	// Same data path setup as DoIO(), with the SDIO block size.
	if (!_ResetController(kDwCtrlFifoReset)) {
		ERROR("%s: cmd%u: FIFO reset did not complete (STATUS %#" B_PRIx32
			", RINTSTS %#" B_PRIx32 ")\n", fConfig.name, command,
			_Read(kDwStatus), _Read(kDwRintsts));
		return B_IO_ERROR;
	}
	_Write(kDwFifoth, fFifoth);
	_Write(kDwTmout, 0xffffffff);
	_Write(kDwBlksiz, transfer->block_size);
	_Write(kDwBytcnt, (uint32)length);

	uint64 hz = fCurrentHz != 0 ? fCurrentHz : 400000;
	bigtime_t wireTime = (bigtime_t)((uint64)length * 8 * 1000000
		/ (hz * fBusWidth));
	bigtime_t deadline = system_time() + 1000000 + wireTime * 10;

	uint32_t localResponse = 0;
	status_t status = _ExecuteCommandLocked(command, argument,
		response != NULL ? response : &localResponse, &data);
	if (status != B_OK) {
		_RecoverFromDataError();
		return status;
	}

	status = _TransferPio(data, deadline);
	if (status != B_OK) {
		// No CMD12 for SDIO: the bus manager aborts through CCCR ABORT.
		_RecoverFromDataError();
		return status;
	}

	if (data.isWrite && _WaitWhileBusy(1000000) != B_OK) {
		ERROR("%s: card still busy after cmd%u write\n", fConfig.name,
			command);
		return B_TIMED_OUT;
	}
	return B_OK;
}


void
DwMmcHost::SetSdioIrqSemaphore(sem_id semaphore)
{
	MutexLocker locker(fLock);
	fSdioIrqSemaphore = semaphore;
}


void
DwMmcHost::EnableSdioIrq(bool enable)
{
	if (enable != fSdioIrqEnabled) {
		SDIO_INFO("%s: host SDIO interrupt %s (INTMASK bit 16, %" B_PRId32
			" seen so far%s)\n", fConfig.name, enable ? "on" : "off",
			atomic_get(&fSdioIrqCount),
			fPollMode ? "; host is in poll mode, the bus manager polls"
				: "");
		fSdioIrqEnabled = enable;
	}

	InterruptsSpinLocker locker(fIntmaskLock);
	uint32 mask = _Read(kDwIntmask);
	if (enable)
		mask |= kDwIntSdio;
	else
		mask &= ~(uint32)kDwIntSdio;
	_Write(kDwIntmask, mask);
}
