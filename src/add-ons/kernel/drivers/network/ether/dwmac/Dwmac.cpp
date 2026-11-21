/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Synopsys DesignWare Ethernet QoS (dwmac / eqos v5.20) driver for Beacon.
 *
 * The bring-up order (clocks, resets, DMA soft reset, MTL/MAC/DMA setup,
 * descriptor handling, stop/drain sequence, MDIO access) is adapted from
 * U-Boot drivers/net/dwc_eth_qos.c (GPL-2.0), vendor tree
 * orangepi-xunlong/u-boot-orangepi v2021.10-jh7110, commit d7274dfe:
 *   Copyright (c) 2016, NVIDIA CORPORATION.
 *   Portions based on U-Boot's rtl8169.c.
 * Interrupt and descriptor bit layouts follow Linux dwmac4_dma.h and
 * dwmac4_descs.h (GPL-2.0-only, Copyright (C) 2015 STMicroelectronics Ltd).
 * JH7110 clock/reset/syscon handling follows Linux dwmac-starfive.c and
 * the vendor U-Boot board file (both GPL-2.0+) and the JH7110 CRG bindings.
 *
 * Modified for Beacon/Haiku, 2026 (The Beacon Project): interrupt driven
 * rings, 64-byte descriptor stride with explicit SiFive ccache maintenance,
 * Haiku net_buffer interface, serial diagnostics; 2026-11-20 (eth2):
 * watchdog (link poll, stall detection, DMA reset, per-minute stats) and
 * power-down on attach failure/uninit.
 */


#include "Dwmac.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <net/if_media.h>

#include <kernel.h>
#include <net_buffer.h>
#include <util/AutoLock.h>


#define INFO(x...)	dprintf("dwmac: " x)
#define ERROR(x...)	dprintf("dwmac: ERROR: " x)


// Ring geometry. Each descriptor gets its own 64-byte cache line (the
// controller skips the padding via DMA_CH0_CONTROL.DSL), so flushing one
// descriptor never touches a neighbour the DMA may be writing.
static const uint32 kTxCount = 64;
static const uint32 kRxCount = 128;
static const uint32 kDescStride = 64;
static const uint32 kAxiBusWidth = 8;		// 64-bit AXI on the JH7110
static const uint32 kBufferSize = 2048;		// per packet buffer
static const uint32 kMaxFrameSize = 1514;	// without FCS (stripped by ACS/CST)
static const uint32 kLogPackets = 8;		// first N RX/TX frames are logged
static const uint32 kLogErrors = 16;
static const bigtime_t kWaitTimeout = 100000;	// poll fallback, 100 ms
static const uint32 kStatsTicks = 60;		// watchdog ticks (1 s) per stats line
static const uint32 kTxStallTicks = 3;		// TX ring not moving for 3 s
static const uint32 kDmaStoppedTicks = 2;	// RX/TX DMA "stopped" seen twice
static const bigtime_t kRestartBackoff = 5000000;	// >= 5 s between resets
static const uint32 kLogRestarts = 16;		// first N resets are logged in full

// DMA_DEBUG_STATUS0 process states (Linux dwmac4_dma.h)
static const uint32 kRpsStopped = 0;
static const uint32 kRpsSuspended = 4;		// receive buffer unavailable
static const uint32 kTpsStopped = 0;

static const uint32 kDmaIntMask = kDmaChIntNis | kDmaChIntAis | kDmaChIntRi
	| kDmaChIntTi | kDmaChIntRbu | kDmaChIntFbe;


static inline void
io_fence()
{
#if defined(__riscv)
	asm volatile("fence iorw, iorw" ::: "memory");
#else
	__sync_synchronize();
#endif
}


static inline uint32
low32(phys_addr_t address)
{
	return (uint32)(address & 0xffffffff);
}


static inline uint32
high32(phys_addr_t address)
{
	return (uint32)((uint64)address >> 32);
}


static void
format_mac(char* out, size_t size, const uint8* mac)
{
	snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
		mac[2], mac[3], mac[4], mac[5]);
}


static bool
mac_is_usable(const uint8* mac)
{
	bool allZero = true;
	bool allOnes = true;
	for (int i = 0; i < 6; i++) {
		if (mac[i] != 0)
			allZero = false;
		if (mac[i] != 0xff)
			allOnes = false;
	}
	// bit 0 of the first byte = multicast, not valid as a station address
	return !allZero && !allOnes && (mac[0] & 1) == 0;
}


DwmacDevice::DwmacDevice()
	:
	fRegsArea(-1),
	fRegs(NULL),
	fSysCrgArea(-1),
	fSysCrg(NULL),
	fAonCrgArea(-1),
	fAonCrg(NULL),
	fSysSysconArea(-1),
	fSysSyscon(NULL),
	fAonSysconArea(-1),
	fAonSyscon(NULL),
	fCcacheArea(-1),
	fCcacheFlush64(NULL),
	fDmaArea(-1),
	fDmaBase(NULL),
	fDmaPhys(0),
	fDmaSize(0),
	fVersion(0),
	fHwFeature1(0),
	fTqs(0),
	fPhyAddress(-1),
	fPhyId(0),
	fPhyIsYt8531(false),
	fLinkUp(false),
	fLinkSpeed(0),
	fLinkFullDuplex(false),
	fLinkStateSem(-1),
	fLinkPolls(0),
	fRxSem(-1),
	fRxNext(0),
	fTxSem(-1),
	fTxNext(0),
	fTxDirty(0),
	fInterruptInstalled(false),
	fResetPending(false),
	fOpenCount(0),
	fStarted(false),
	fNonblocking(false),
	fPromiscuous(false),
	fClosing(false),
	fRxPackets(0),
	fTxPackets(0),
	fRxErrors(0),
	fTxErrors(0),
	fIrqCount(0),
	fRxIrqs(0),
	fTxIrqs(0),
	fRbuCount(0),
	fMacIrqs(0),
	fSpuriousIrqs(0),
	fFatalStatus(0),
	fFatalLogged(false),
	fPollWarned(false),
	fRstAssert(NULL),
	fRstStatus(NULL),
	fRstMask(0),
	fResetReleased(false),
	fEnabledClockCount(0),
	fWatchdog(-1),
	fWatchdogQuit(false),
	fTxStallIndex(0),
	fTxStallTicks(0),
	fRxStoppedTicks(0),
	fTxStoppedTicks(0),
	fStalls(0),
	fRestarts(0),
	fRestartFailures(0),
	fRxKicks(0),
	fMissedFrames(0),
	fLastRxPackets(0),
	fLastTxPackets(0),
	fLastRestart(0)
{
	memset(&fConfig, 0, sizeof(fConfig));
	memset(fMac, 0, sizeof(fMac));
	mutex_init(&fPhyLock, "dwmac phy");
	mutex_init(&fRxLock, "dwmac rx");
	mutex_init(&fTxLock, "dwmac tx");
}


DwmacDevice::~DwmacDevice()
{
	// Order matters: no thread may touch the rings, then the controller is
	// stopped and held in reset, and only then is the DMA memory freed.
	if (fWatchdog >= 0) {
		fWatchdogQuit = true;
		status_t result;
		wait_for_thread(fWatchdog, &result);
		fWatchdog = -1;
	}
	if (fStarted) {
		MutexLocker _(fPhyLock);
		MutexLocker rx(fRxLock);
		MutexLocker tx(fTxLock);
		_Stop();
	}
	if (fRegs != NULL)
		_Write(kDmaCh0IntEnable, 0);
	if (fInterruptInstalled) {
		remove_io_interrupt_handler(fConfig.irq, _InterruptHandler, this);
		fInterruptInstalled = false;
	}
	_PowerDown(fDmaArea >= 0 ? "uninit" : "attach failed");
	if (fRxSem >= 0)
		delete_sem(fRxSem);
	if (fTxSem >= 0)
		delete_sem(fTxSem);

	area_id areas[] = { fDmaArea, fRegsArea, fSysCrgArea, fAonCrgArea,
		fSysSysconArea, fAonSysconArea, fCcacheArea };
	for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); i++) {
		if (areas[i] >= 0)
			delete_area(areas[i]);
	}

	mutex_destroy(&fPhyLock);
	mutex_destroy(&fRxLock);
	mutex_destroy(&fTxLock);
}


// #pragma mark - JH7110 clocks, resets, interface select


static area_id
map_page(const char* name, phys_addr_t address, volatile uint32*& regs)
{
	void* virt = NULL;
	area_id area = map_physical_memory(name, address, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virt);
	regs = area >= 0 ? (volatile uint32*)virt : NULL;
	return area;
}


status_t
DwmacDevice::_Jh7110MapBlocks()
{
	fSysCrgArea = map_page("dwmac jh7110 syscrg", kJh7110SysCrgBase, fSysCrg);
	fAonCrgArea = map_page("dwmac jh7110 aoncrg", kJh7110AonCrgBase, fAonCrg);
	fSysSysconArea = map_page("dwmac jh7110 sys syscon", kJh7110SysSysconBase,
		fSysSyscon);
	fAonSysconArea = map_page("dwmac jh7110 aon syscon",
		kJh7110AonSysconBase, fAonSyscon);
	if (fSysCrg == NULL || fAonCrg == NULL || fSysSyscon == NULL
		|| fAonSyscon == NULL) {
		ERROR("%s: cannot map the JH7110 CRG/syscon blocks\n", fConfig.name);
		return B_NO_MEMORY;
	}

	volatile uint32* ccache = NULL;
	fCcacheArea = map_page("dwmac jh7110 ccache", kJh7110CcacheBase, ccache);
	if (ccache != NULL) {
		fCcacheFlush64 = (volatile uint64*)((addr_t)ccache
			+ kJh7110CcacheFlush64);
	} else {
		ERROR("%s: cannot map the L2 cache controller at %#x; DMA buffers "
			"will NOT be flushed (expect corrupt packets if DMA is not "
			"coherent)\n", fConfig.name, (unsigned)kJh7110CcacheBase);
	}
	return B_OK;
}


void
DwmacDevice::_Jh7110ClockOn(volatile uint32* crg, uint32 index,
	const char* what)
{
	uint32 before = crg[index];
	if ((before & kCrgEnable) == 0) {
		crg[index] = before | kCrgEnable;
		if (fEnabledClockCount < (int32)(sizeof(fEnabledClocks)
				/ sizeof(fEnabledClocks[0])))
			fEnabledClocks[fEnabledClockCount++] = &crg[index];
	}
	uint32 after = crg[index];
	INFO("%s: clock %-10s [%3" B_PRIu32 "] %#010" B_PRIx32 " -> %#010" B_PRIx32
		" (div %" B_PRIu32 ", mux %" B_PRIu32 "%s)%s\n", fConfig.name, what,
		index, before, after, after & kCrgDivMask,
		(after >> kCrgMuxShift) & kCrgMuxMask,
		(after & kCrgInvert) != 0 ? ", inverted" : "",
		(before & kCrgEnable) == 0 ? " ENABLED by driver" : "");
}


static bool
reset_wait(volatile uint32* status, uint32 mask, uint32 want)
{
	for (int i = 0; i < 1000; i++) {
		if ((*status & mask) == want)
			return true;
		spin(1);
	}
	return false;
}


status_t
DwmacDevice::_Jh7110Bringup()
{
	const int32 index = fConfig.jh7110Index;
	volatile uint32* crg;
	volatile uint32* rstAssert;
	volatile uint32* rstStatus;
	uint32 rstMask;
	volatile uint32* ifSel;
	uint32 ifShift;

	if (index == 0) {
		// gmac0: bus/TX clocks and resets in AONCRG, GTX/PTP in SYSCRG
		crg = fAonCrg;
		rstAssert = fAonCrg + kAonRstAssert / 4;
		rstStatus = fAonCrg + kAonRstStatus / 4;
		rstMask = (1u << kAonRstGmac0Axi) | (1u << kAonRstGmac0Ahb);
		ifSel = fAonSyscon + kGmac0IfSelReg / 4;
		ifShift = kGmac0IfSelShift;
	} else {
		crg = fSysCrg;
		rstAssert = fSysCrg + kSysRstAssert / 4 + kSysRstGmac1Axi / 32;
		rstStatus = fSysCrg + kSysRstStatus / 4 + kSysRstGmac1Axi / 32;
		rstMask = (1u << (kSysRstGmac1Axi % 32))
			| (1u << (kSysRstGmac1Ahb % 32));
		ifSel = fSysSyscon + kGmac1IfSelReg / 4;
		ifShift = kGmac1IfSelShift;
	}
	(void)crg;

	INFO("%s: JH7110 gmac%" B_PRId32 ": firmware left reset assert %#010"
		B_PRIx32 " status %#010" B_PRIx32 " (mask %#" B_PRIx32 ": %s)\n",
		fConfig.name, index, *rstAssert, *rstStatus, rstMask,
		(*rstStatus & rstMask) == rstMask ? "out of reset"
			: "IN RESET (normal after U-Boot eth stop)");

	// If firmware left the controller out of reset, its DMA may still own
	// descriptors in memory that now belongs to Haiku. Hold it in reset
	// BEFORE any clock is opened, so it cannot resume and write there; the
	// pulse below releases it with the clocks running.
	if ((*rstStatus & rstMask) != 0) {
		*rstAssert = *rstAssert | rstMask;
		bool held = reset_wait(rstStatus, rstMask, 0);
		INFO("%s: firmware left it (partly) out of reset; held in reset "
			"before opening clocks (status %#010" B_PRIx32 "%s)\n",
			fConfig.name, *rstStatus, held ? "" : ", did not drop");
	}

	// PHY interface select (Linux dwmac-starfive.c starfive_dwmac_set_mode,
	// vendor U-Boot jh7110_gmac_init_1000M). It is sampled at reset, so set
	// it before the reset pulse below.
	uint32 wantMode = strncmp(fConfig.phyMode, "rmii", 4) == 0
		? kIfSelRmii : kIfSelRgmii;
	uint32 ifBefore = *ifSel;
	uint32 curMode = (ifBefore >> ifShift) & kIfSelMask;
	if (curMode != wantMode) {
		*ifSel = (ifBefore & ~(kIfSelMask << ifShift)) | (wantMode << ifShift);
	}
	INFO("%s: interface select %s syscon +%#" B_PRIx32 " = %#010" B_PRIx32
		" -> %#010" B_PRIx32 " (mode %" B_PRIu32 " -> %" B_PRIu32 ", %s, "
		"phy-mode \"%s\")\n", fConfig.name, index == 0 ? "AON" : "SYS",
		index == 0 ? (uint32)kGmac0IfSelReg : (uint32)kGmac1IfSelReg,
		ifBefore, *ifSel, curMode, wantMode,
		wantMode == kIfSelRgmii ? "RGMII" : "RMII", fConfig.phyMode);

	// Clocks, like Linux (stmmaceth, pclk, ptp_ref, tx, gtx) plus the
	// vendor U-Boot list (gtxc, rmii_rtx). Only gates are touched here;
	// muxes, inverters and dividers stay as firmware set them.
	if (index == 0) {
		_Jh7110ClockOn(fAonCrg, kAonClkGmac0Ahb, "ahb");
		_Jh7110ClockOn(fAonCrg, kAonClkGmac0Axi, "axi");
		_Jh7110ClockOn(fAonCrg, kAonClkGmac0Tx, "tx");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac0Gtx, "gtxclk");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac0Ptp, "ptp");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac0Gtxc, "gtxc");
		INFO("%s: (info) aon tx_inv %#010" B_PRIx32 ", rx %#010" B_PRIx32
			", rx_inv %#010" B_PRIx32 ", rmii_rtx %#010" B_PRIx32
			", sys gmac_src %#010" B_PRIx32 ", gmac_phy %#010" B_PRIx32 "\n",
			fConfig.name, fAonCrg[kAonClkGmac0TxInv], fAonCrg[kAonClkGmac0Rx],
			fAonCrg[kAonClkGmac0RxInv], fAonCrg[kAonClkGmac0RmiiRtx],
			fSysCrg[kSysClkGmacSrc], fSysCrg[kSysClkGmacPhy]);
	} else {
		_Jh7110ClockOn(fSysCrg, kSysClkGmac1Ahb, "ahb");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac1Axi, "axi");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac1Tx, "tx");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac1Ptp, "ptp");
		_Jh7110ClockOn(fSysCrg, kSysClkGmac1Gtxc, "gtxc");
		INFO("%s: (info) sys gtxclk %#010" B_PRIx32 " (divider only), "
			"tx_inv %#010" B_PRIx32 ", rx %#010" B_PRIx32 ", rx_inv %#010"
			B_PRIx32 ", rmii_rtx %#010" B_PRIx32 ", gmac_src %#010" B_PRIx32
			"\n", fConfig.name, fSysCrg[kSysClkGmac1Gtx],
			fSysCrg[kSysClkGmac1TxInv], fSysCrg[kSysClkGmac1Rx],
			fSysCrg[kSysClkGmac1RxInv], fSysCrg[kSysClkGmac1RmiiRtx],
			fSysCrg[kSysClkGmacSrc]);
	}

	uint64 pll0 = _Jh7110Pll0Hz();
	uint32 gtx = index == 0 ? fSysCrg[kSysClkGmac0Gtx]
		: fSysCrg[kSysClkGmac1Gtx];
	uint32 gtxDiv = gtx & kCrgDivMask;
	INFO("%s: PLL0 %" B_PRIu64 " Hz -> gtxclk %" B_PRIu64 " Hz (divider %"
		B_PRIu32 ")\n", fConfig.name, pll0,
		gtxDiv != 0 ? pll0 / gtxDiv : 0, gtxDiv);

	// Reset pulse with clocks running (deasserting with a gated clock can
	// hang the reset block, Linux reset-starfive-jh71x0.c).
	fRstAssert = rstAssert;
	fRstStatus = rstStatus;
	fRstMask = rstMask;
	fResetReleased = true;
	*rstAssert = *rstAssert | rstMask;
	if (!reset_wait(rstStatus, rstMask, 0)) {
		INFO("%s: reset status did not drop while asserting (%#010" B_PRIx32
			"), going on\n", fConfig.name, *rstStatus);
	}
	spin(10);
	*rstAssert = *rstAssert & ~rstMask;
	if (!reset_wait(rstStatus, rstMask, rstMask)) {
		ERROR("%s: controller did not leave reset (status %#010" B_PRIx32
			")\n", fConfig.name, *rstStatus);
		return B_IO_ERROR;
	}
	spin(10);
	INFO("%s: reset pulse done, status %#010" B_PRIx32 "\n", fConfig.name,
		*rstStatus);
	return B_OK;
}


void
DwmacDevice::_PowerDown(const char* why)
{
	// Undo the bring-up: stop the DMA, then (JH7110) assert the resets and
	// gate the clocks this driver opened. A controller held in reset cannot
	// master the bus, so its DMA memory may be freed afterwards. Firmware
	// state (U-Boot gates and resets an unused gmac) is restored too, which
	// matters for gmac1, which has no PHY on the Orange Pi RV.
	if (fRegs != NULL && fResetReleased) {
		_Write(kDmaCh0IntEnable, 0);
		_ClearBits(kDmaCh0TxControl, kDmaChTxSt);
		_ClearBits(kDmaCh0RxControl, kDmaChRxSr);
		_ClearBits(kMacConfig, kMacConfigTe | kMacConfigRe);
		io_fence();
		spin(100);
	}
	if (fResetReleased && fRstAssert != NULL) {
		*fRstAssert = *fRstAssert | fRstMask;
		bool inReset = reset_wait(fRstStatus, fRstMask, 0);
		fResetReleased = false;
		INFO("%s: %s: controller held in reset (status %#010" B_PRIx32 "%s)"
			"\n", fConfig.name, why, *fRstStatus,
			inReset ? "" : ", status bits did not drop");
	}
	for (int32 i = fEnabledClockCount - 1; i >= 0; i--)
		*fEnabledClocks[i] = *fEnabledClocks[i] & ~(uint32)kCrgEnable;
	if (fEnabledClockCount > 0) {
		INFO("%s: %s: %" B_PRId32 " clocks gated again\n", fConfig.name, why,
			fEnabledClockCount);
	}
	fEnabledClockCount = 0;
}


uint64
DwmacDevice::_Jh7110Pll0Hz()
{
	if (fSysSyscon == NULL)
		return 0;
	uint32 r18 = fSysSyscon[kPll0Reg18 / 4];
	uint32 fbdiv = fSysSyscon[kPll0FbdivReg / 4] & 0xfff;
	uint32 r20 = fSysSyscon[kPll0FracReg / 4];
	uint32 prediv = fSysSyscon[kPll0PredivReg / 4] & 0x3f;
	uint32 frac = r20 & 0xffffff;
	uint32 postdiv1 = (r20 >> 28) & 3;
	bool dsmpd = ((r18 >> 25) & 1) != 0;
	if (prediv == 0)
		return 0;

	// rate = 24 MHz * (fbdiv + frac / 2^24) / prediv / 2^postdiv1
	uint64 hz = 24000000ULL * fbdiv;
	if (!dsmpd)
		hz += (24000000ULL * frac) >> 24;
	hz /= prediv;
	hz >>= postdiv1;
	return hz;
}


void
DwmacDevice::_Jh7110SetTxClock(uint32 speed)
{
	// Vendor U-Boot eqos_set_tx_clk_speed_jh7110: gtx and rmii_rtx get the
	// RGMII clock for the link speed. Linux dwmac-starfive.c does the same
	// through the "tx" clock.
	if (!fConfig.isJh7110 || fSysCrg == NULL)
		return;
	if (fConfig.txUseRgmiiClk) {
		// Mainline Linux dwmac-starfive.c: no fix_mac_speed with this
		// property; the TX clock comes from the RGMII side.
		INFO("%s: TX clock for %" B_PRIu32 " Mbit/s left to hardware "
			"(starfive,tx-use-rgmii-clk)\n", fConfig.name, speed);
		return;
	}
	uint64 target = speed == 1000 ? 125000000 : speed == 100 ? 25000000
		: 2500000;

	volatile uint32* gtx = fConfig.jh7110Index == 0
		? &fSysCrg[kSysClkGmac0Gtx] : &fSysCrg[kSysClkGmac1Gtx];
	volatile uint32* rtx = fConfig.jh7110Index == 0
		? &fAonCrg[kAonClkGmac0RmiiRtx] : &fSysCrg[kSysClkGmac1RmiiRtx];
	volatile uint32* txMux = fConfig.jh7110Index == 0
		? &fAonCrg[kAonClkGmac0Tx] : &fSysCrg[kSysClkGmac1Tx];

	uint64 pll0 = _Jh7110Pll0Hz();
	if (pll0 < 100000000ULL || pll0 > 4000000000ULL) {
		INFO("%s: PLL0 reads %" B_PRIu64 " Hz (implausible); TX clock left "
			"alone\n", fConfig.name, pll0);
	} else {
		uint64 div = (pll0 + target / 2) / target;
		uint32 old = *gtx;
		if (div < 1 || div > kGtxDivMax) {
			INFO("%s: WARNING: %" B_PRIu64 " Hz TX clock needs gtx divider %"
				B_PRIu64 " (max %d); left at %" B_PRIu32 ". %" B_PRIu32
				" Mbit/s may not pass traffic (known gap)\n", fConfig.name,
				target, div, (int)kGtxDivMax, old & kCrgDivMask, speed);
		} else if ((old & kCrgDivMask) != div) {
			*gtx = (old & ~(uint32)kCrgDivMask) | (uint32)div;
			INFO("%s: gtxclk divider %" B_PRIu32 " -> %" B_PRIu64 " (%" B_PRIu64
				" Hz)\n", fConfig.name, old & kCrgDivMask, div, pll0 / div);
		}
	}

	// rmii_rtx from the 50 MHz RMII reference input, divider 1..30
	uint64 rdiv = (50000000ULL + target / 2) / target;
	if (rdiv < 1)
		rdiv = 1;
	if (rdiv > 30)
		rdiv = 30;
	uint32 oldRtx = *rtx;
	if ((oldRtx & kCrgDivMask) != rdiv)
		*rtx = (oldRtx & ~(uint32)kCrgDivMask) | (uint32)rdiv;

	INFO("%s: TX clock for %" B_PRIu32 " Mbit/s: gtx %#010" B_PRIx32
		", rmii_rtx %#010" B_PRIx32 ", tx mux %" B_PRIu32 " (firmware choice,"
		" not changed)\n", fConfig.name, speed, *gtx, *rtx,
		(*txMux >> kCrgMuxShift) & kCrgMuxMask);
}


// #pragma mark - cache maintenance


void
DwmacDevice::_FlushPhys(phys_addr_t address, size_t size)
{
	// SiFive ccache Flush64 writes back and invalidates one 64-byte line by
	// physical address (vendor U-Boot arch/riscv/cpu/jh7110/cache.c uses it
	// for both flush and invalidate). On a coherent system this is merely
	// redundant.
	io_fence();
	if (fCcacheFlush64 == NULL || size == 0)
		return;
	phys_addr_t line = address & ~(phys_addr_t)(kJh7110CacheLine - 1);
	phys_addr_t end = address + size;
	for (; line < end; line += kJh7110CacheLine) {
		*fCcacheFlush64 = line;
		io_fence();
	}
}


volatile dwmac_desc*
DwmacDevice::_TxDesc(uint32 index) const
{
	return (volatile dwmac_desc*)(fDmaBase + index * kDescStride);
}


volatile dwmac_desc*
DwmacDevice::_RxDesc(uint32 index) const
{
	return (volatile dwmac_desc*)(fDmaBase + (kTxCount + index) * kDescStride);
}


uint8*
DwmacDevice::_TxBuffer(uint32 index) const
{
	return fDmaBase + (kTxCount + kRxCount) * kDescStride
		+ index * kBufferSize;
}


uint8*
DwmacDevice::_RxBuffer(uint32 index) const
{
	return fDmaBase + (kTxCount + kRxCount) * kDescStride
		+ (kTxCount + index) * kBufferSize;
}


void
DwmacDevice::_FlushDesc(volatile dwmac_desc* desc)
{
	_FlushPhys(_Phys((const void*)desc), kDescStride);
}


// #pragma mark - init


void
DwmacDevice::_LogFirmwareState()
{
	INFO("%s: VERSION %#010" B_PRIx32 " (Synopsys %x.%x, user %#x)\n",
		fConfig.name, fVersion, (fVersion >> 4) & 0xf, fVersion & 0xf,
		(fVersion >> 8) & 0xff);
	INFO("%s: HW_FEATURE0 %#010" B_PRIx32 " 1 %#010" B_PRIx32 " 2 %#010"
		B_PRIx32 " 3 %#010" B_PRIx32 "\n", fConfig.name, _Read(kMacHwFeature0),
		_Read(kMacHwFeature1), _Read(kMacHwFeature2), _Read(kMacHwFeature3));
	uint32 f1 = _Read(kMacHwFeature1);
	INFO("%s: FIFOs: TX %u bytes, RX %u bytes, address width %s\n",
		fConfig.name,
		128u << ((f1 >> kMacHwFeat1TxFifoShift) & kMacHwFeat1FifoMask),
		128u << ((f1 >> kMacHwFeat1RxFifoShift) & kMacHwFeat1FifoMask),
		((f1 >> kMacHwFeat1Addr64Shift) & kMacHwFeat1Addr64Mask) == 0 ? "32"
			: ((f1 >> kMacHwFeat1Addr64Shift) & kMacHwFeat1Addr64Mask) == 1
				? "40" : "48");
	INFO("%s: firmware left MAC_CONFIG %#010" B_PRIx32 " FILTER %#010" B_PRIx32
		" PHYIF %#010" B_PRIx32 " DEBUG %#010" B_PRIx32 " INT_EN %#010" B_PRIx32
		"\n", fConfig.name, _Read(kMacConfig), _Read(kMacPacketFilter),
		_Read(kMacPhyifCtrlStatus), _Read(kMacDebug), _Read(kMacIntEnable));
	INFO("%s: firmware left DMA_MODE %#010" B_PRIx32 " SYSBUS %#010" B_PRIx32
		" STATUS %#010" B_PRIx32 " CH0 ctl %#010" B_PRIx32 " tx %#010" B_PRIx32
		" rx %#010" B_PRIx32 " int %#010" B_PRIx32 " st %#010" B_PRIx32 "\n",
		fConfig.name, _Read(kDmaMode), _Read(kDmaSysbusMode),
		_Read(kDmaStatus), _Read(kDmaCh0Control), _Read(kDmaCh0TxControl),
		_Read(kDmaCh0RxControl), _Read(kDmaCh0IntEnable),
		_Read(kDmaCh0Status));
	INFO("%s: firmware left rings TX %#010" B_PRIx32 " (len %" B_PRIu32
		") RX %#010" B_PRIx32 " (len %" B_PRIu32 "), MTL txq %#010" B_PRIx32
		" rxq %#010" B_PRIx32 "\n", fConfig.name, _Read(kDmaCh0TxList),
		_Read(kDmaCh0TxRingLen), _Read(kDmaCh0RxList), _Read(kDmaCh0RxRingLen),
		_Read(kMtlTxq0OpMode), _Read(kMtlRxq0OpMode));
}


status_t
DwmacDevice::_ResetDma()
{
	// Linux dwmac4_dma_reset(): software reset of DMA, MTL and MAC. It only
	// completes when the PHY clocks (RGMII RX clock) are running.
	_SetBits(kDmaMode, kDmaModeSwr);
	for (int i = 0; i < 1000; i++) {
		if ((_Read(kDmaMode) & kDmaModeSwr) == 0) {
			INFO("%s: DMA software reset done after ~%d ms\n", fConfig.name,
				i);
			return B_OK;
		}
		snooze(1000);
	}
	ERROR("%s: DMA software reset stuck (DMA_MODE %#010" B_PRIx32 "). The "
		"RGMII RX clock from the PHY, or a gated clock, is the usual cause\n",
		fConfig.name, _Read(kDmaMode));
	return B_TIMED_OUT;
}


static bool
mdio_wait_idle(volatile uint32* regs)
{
	for (int i = 0; i < 20000; i++) {
		if ((regs[kMacMdioAddress / 4] & kMdioAddrGb) == 0)
			return true;
		spin(1);
	}
	return false;
}


int32
DwmacDevice::MdioRead(int32 phy, int32 reg)
{
	// U-Boot eqos_mdio_read()
	if (!mdio_wait_idle(fRegs))
		return -1;
	uint32 value = _Read(kMacMdioAddress) & (kMdioAddrSkap | kMdioAddrC45e);
	value |= ((uint32)phy << kMdioAddrPaShift)
		| ((uint32)reg << kMdioAddrRdaShift)
		| (kMdioAddrCr250_300 << kMdioAddrCrShift)
		| (kMdioAddrGocRead << kMdioAddrGocShift) | kMdioAddrGb;
	_Write(kMacMdioAddress, value);
	spin(10);
	if (!mdio_wait_idle(fRegs))
		return -1;
	return _Read(kMacMdioData) & kMdioDataMask;
}


status_t
DwmacDevice::MdioWrite(int32 phy, int32 reg, uint16 data)
{
	// U-Boot eqos_mdio_write()
	if (!mdio_wait_idle(fRegs))
		return B_TIMED_OUT;
	_Write(kMacMdioData, data);
	uint32 value = _Read(kMacMdioAddress) & (kMdioAddrSkap | kMdioAddrC45e);
	value |= ((uint32)phy << kMdioAddrPaShift)
		| ((uint32)reg << kMdioAddrRdaShift)
		| (kMdioAddrCr250_300 << kMdioAddrCrShift)
		| (kMdioAddrGocWrite << kMdioAddrGocShift) | kMdioAddrGb;
	_Write(kMacMdioAddress, value);
	spin(10);
	return mdio_wait_idle(fRegs) ? B_OK : B_TIMED_OUT;
}


status_t
DwmacDevice::_FindPhy()
{
	int32 order[33];
	int32 count = 0;
	if (fConfig.phyAddress >= 0 && fConfig.phyAddress < 32)
		order[count++] = fConfig.phyAddress;
	for (int32 i = 0; i < 32; i++) {
		if (i != fConfig.phyAddress)
			order[count++] = i;
	}

	for (int32 i = 0; i < count; i++) {
		int32 id1 = MdioRead(order[i], kMiiPhyId1);
		if (id1 < 0) {
			ERROR("%s: MDIO bus does not respond (GB stuck)\n", fConfig.name);
			return B_IO_ERROR;
		}
		int32 id2 = MdioRead(order[i], kMiiPhyId2);
		if (id1 == 0xffff || id1 == 0 || id2 < 0)
			continue;
		fPhyAddress = order[i];
		fPhyId = ((uint32)id1 << 16) | (uint32)id2;
		// Vendor U-Boot motorcomm.c matches PHY_ID_YT8531 (0x4f51e91b)
		// under mask 0xfff.
		fPhyIsYt8531 = (fPhyId & 0xfff) == 0x91b
			&& (fPhyId >> 16) == 0x4f51;
		INFO("%s: PHY at MDIO address %" B_PRId32 "%s: ID %#010" B_PRIx32
			" (%s)\n", fConfig.name, fPhyAddress,
			fPhyAddress == fConfig.phyAddress ? " (from DT)" : " (scan)",
			fPhyId, fPhyIsYt8531 ? "Motorcomm YT8531" : "unknown, generic MII");
		return B_OK;
	}
	ERROR("%s: no PHY answers on MDIO (addresses 0-31)\n", fConfig.name);
	return B_DEVICE_NOT_FOUND;
}


void
DwmacDevice::_ConfigurePhy()
{
	int32 phy = fPhyAddress;
	INFO("%s: PHY before config: BMCR %#06" B_PRIx32 " BMSR %#06" B_PRIx32
		" ANAR %#06" B_PRIx32 " CTRL1000 %#06" B_PRIx32 "\n", fConfig.name,
		MdioRead(phy, kMiiBmcr), MdioRead(phy, kMiiBmsr),
		MdioRead(phy, kMiiAnar), MdioRead(phy, kMiiCtrl1000));

	if (fPhyIsYt8531) {
		yt8531_log_state(this, phy, "before");
		if (fConfig.phyNodeFound)
			yt8531_configure(this, phy, fConfig.yt);
		else {
			INFO("%s: no PHY node in the DT; YT8531 delay/drive settings "
				"left as firmware/strap set them\n", fConfig.name);
		}
		yt8531_log_state(this, phy, "after");
	}

	// Generic clause-22 autonegotiation (what U-Boot genphy_config_aneg
	// ends up doing): advertise 10/100/1000, symmetric+asymmetric pause.
	int32 anar = MdioRead(phy, kMiiAnar);
	if (anar >= 0) {
		anar &= ~(kAnar10Hd | kAnar10Fd | kAnar100Hd | kAnar100Fd | kAnarPause
			| kAnarAsymPause);
		anar |= kAnarCsma | kAnar10Hd | kAnar10Fd | kAnar100Hd | kAnar100Fd
			| kAnarPause | kAnarAsymPause;
		MdioWrite(phy, kMiiAnar, (uint16)anar);
	}
	int32 ctrl1000 = MdioRead(phy, kMiiCtrl1000);
	if (ctrl1000 >= 0) {
		ctrl1000 |= kCtrl1000Fd | kCtrl1000Hd;
		MdioWrite(phy, kMiiCtrl1000, (uint16)ctrl1000);
	}
	int32 bmcr = MdioRead(phy, kMiiBmcr);
	if (bmcr >= 0) {
		bmcr |= kBmcrAnEnable | kBmcrAnRestart;
		bmcr &= ~kBmcrReset;
		MdioWrite(phy, kMiiBmcr, (uint16)bmcr);
	}
	INFO("%s: PHY autonegotiation restarted: ANAR %#06" B_PRIx32 " CTRL1000 %#06"
		B_PRIx32 " BMCR %#06" B_PRIx32 "\n", fConfig.name,
		MdioRead(phy, kMiiAnar), MdioRead(phy, kMiiCtrl1000),
		MdioRead(phy, kMiiBmcr));
}


void
DwmacDevice::_ChooseMacAddress(const uint8* firmwareMac)
{
	char text[24];
	if (fConfig.hasMac && mac_is_usable(fConfig.mac)) {
		memcpy(fMac, fConfig.mac, 6);
		format_mac(text, sizeof(text), fMac);
		INFO("%s: MAC address %s (from DT %s)\n", fConfig.name, text,
			fConfig.macSource);
		return;
	}
	if (fConfig.hasMac) {
		format_mac(text, sizeof(text), fConfig.mac);
		INFO("%s: DT %s %s is not usable\n", fConfig.name, fConfig.macSource,
			text);
	}
	if (mac_is_usable(firmwareMac)) {
		memcpy(fMac, firmwareMac, 6);
		format_mac(text, sizeof(text), fMac);
		INFO("%s: MAC address %s (left in MAC_ADDR0 by firmware)\n",
			fConfig.name, text);
		return;
	}

	// Last resort: a fixed locally administered address, different per
	// controller. Not unique between boards.
	const uint8 fallback[6] = { 0x02, 0xbe, 0xac, 0x07, 0x11,
		(uint8)(fConfig.jh7110Index >= 0 ? fConfig.jh7110Index
			: (fConfig.physBase >> 16) & 0xff) };
	memcpy(fMac, fallback, 6);
	format_mac(text, sizeof(text), fMac);
	INFO("%s: WARNING: no MAC address from DT or firmware; using locally "
		"administered %s (NOT unique, two boards on one LAN will clash)\n",
		fConfig.name, text);
}


void
DwmacDevice::_WriteMacAddress()
{
	// U-Boot eqos_write_hwaddr()
	_Write(kMacAddr0High, ((uint32)fMac[5] << 8) | fMac[4]);
	_Write(kMacAddr0Low, ((uint32)fMac[3] << 24) | ((uint32)fMac[2] << 16)
		| ((uint32)fMac[1] << 8) | fMac[0]);
}


status_t
DwmacDevice::_AllocateRings()
{
	fDmaSize = (kTxCount + kRxCount) * kDescStride
		+ (kTxCount + kRxCount) * kBufferSize;
	fDmaSize = (fDmaSize + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);

	char name[64];
	snprintf(name, sizeof(name), "dwmac %s dma", fConfig.name);
	void* base = NULL;
	fDmaArea = create_area(name, &base, B_ANY_KERNEL_ADDRESS, fDmaSize,
		B_32_BIT_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fDmaArea < 0) {
		ERROR("%s: cannot allocate %" B_PRIuSIZE " bytes of DMA memory: %s\n",
			fConfig.name, fDmaSize, strerror(fDmaArea));
		return fDmaArea;
	}
	fDmaBase = (uint8*)base;

	physical_entry entry;
	status_t status = get_memory_map(base, fDmaSize, &entry, 1);
	if (status != B_OK) {
		ERROR("%s: get_memory_map failed: %s\n", fConfig.name,
			strerror(status));
		return status;
	}
	fDmaPhys = entry.address;
	if (fDmaPhys + fDmaSize > 0x100000000ULL) {
		ERROR("%s: DMA memory %#" B_PRIxPHYSADDR " is above 4 GiB\n",
			fConfig.name, fDmaPhys);
		return B_NO_MEMORY;
	}
	memset(fDmaBase, 0, fDmaSize);
	_FlushPhys(fDmaPhys, fDmaSize);

	INFO("%s: DMA area %#" B_PRIxPHYSADDR "-%#" B_PRIxPHYSADDR " (%" B_PRIu32
		" TX + %" B_PRIu32 " RX descriptors at %" B_PRIu32 "-byte stride, %"
		B_PRIu32 "-byte buffers), cache maintenance: %s\n", fConfig.name,
		fDmaPhys, fDmaPhys + fDmaSize - 1, kTxCount, kRxCount, kDescStride,
		kBufferSize, fCcacheFlush64 != NULL
			? "SiFive ccache Flush64 per 64-byte line"
			: "none (assumed coherent)");
	return B_OK;
}


status_t
DwmacDevice::Init(const DwmacConfig& config)
{
	fConfig = config;

	INFO("%s: attaching: regs %#" B_PRIxPHYSADDR " (%#" B_PRIxSIZE " bytes), "
		"PLIC irq %" B_PRIu64 "%s, phy-mode \"%s\", PHY node %s\n",
		fConfig.name, fConfig.physBase, fConfig.physSize, fConfig.irq,
		fConfig.irqFromDt ? "" : " (hard-coded, no DT interrupts)",
		fConfig.phyMode, fConfig.phyNodeFound ? fConfig.phyNodeName : "none");

	status_t status;
	if (fConfig.isJh7110) {
		status = _Jh7110MapBlocks();
		if (status != B_OK)
			return status;
		status = _Jh7110Bringup();
		if (status != B_OK)
			return status;
	} else {
		INFO("%s: not a JH7110 controller; assuming firmware enabled clocks "
			"and resets, and that DMA is cache coherent\n", fConfig.name);
	}

	void* regs = NULL;
	fRegsArea = map_physical_memory("dwmac registers", fConfig.physBase,
		fConfig.physSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (fRegsArea < 0) {
		ERROR("%s: cannot map registers\n", fConfig.name);
		return fRegsArea;
	}
	fRegs = (volatile uint32*)regs;

	fVersion = _Read(kMacVersion);
	if (fVersion == 0 || fVersion == 0xffffffff) {
		ERROR("%s: VERSION reads %#" B_PRIx32 "; controller not responding\n",
			fConfig.name, fVersion);
		return B_DEVICE_NOT_FOUND;
	}
	_LogFirmwareState();

	uint32 high = _Read(kMacAddr0High);
	uint32 low = _Read(kMacAddr0Low);
	uint8 firmwareMac[6] = { (uint8)low, (uint8)(low >> 8),
		(uint8)(low >> 16), (uint8)(low >> 24), (uint8)high,
		(uint8)(high >> 8) };
	char text[24];
	format_mac(text, sizeof(text), firmwareMac);
	INFO("%s: firmware left MAC_ADDR0 %s (high %#010" B_PRIx32 ")\n",
		fConfig.name, text, high);

	// Quiesce anything firmware may have left running, then reset.
	_Write(kDmaCh0IntEnable, 0);
	_ClearBits(kDmaCh0TxControl, kDmaChTxSt);
	_ClearBits(kDmaCh0RxControl, kDmaChRxSr);
	_ClearBits(kMacConfig, kMacConfigTe | kMacConfigRe);
	status = _ResetDma();
	if (status != B_OK) {
		// Without a cable some PHYs stop the RGMII RX clock, which stalls
		// the reset. MDIO still works, so attach and retry on open.
		INFO("%s: continuing; the DMA reset is retried when the interface "
			"is opened\n", fConfig.name);
		fResetPending = true;
	}

	fHwFeature1 = _Read(kMacHwFeature1);
	_ChooseMacAddress(firmwareMac);

	// No MAC/MMC interrupts: they share the line and nothing here uses them.
	_Write(kMacIntEnable, 0);
	_Write(kMmcRxIntrMask, 0xffffffff);
	_Write(kMmcTxIntrMask, 0xffffffff);
	_Write(kMmcIpcRxIntrMask, 0xffffffff);

	status = _FindPhy();
	if (status != B_OK)
		return status;
	_ConfigurePhy();

	// 1 us tick from the 125 MHz GTX clock, as vendor U-Boot does.
	_Write(kMacUsTicCounter, 125 - 1);
	_WriteMacAddress();

	status = _AllocateRings();
	if (status != B_OK)
		return status;

	fRxSem = create_sem(0, "dwmac rx");
	fTxSem = create_sem(0, "dwmac tx");
	if (fRxSem < 0 || fTxSem < 0)
		return B_NO_MORE_SEMS;

	status = install_io_interrupt_handler(fConfig.irq, _InterruptHandler,
		this, 0);
	if (status == B_OK)
		fInterruptInstalled = true;
	else {
		ERROR("%s: cannot install handler for PLIC irq %" B_PRIu64 " (%s); "
			"running on %d ms polling\n", fConfig.name, fConfig.irq,
			strerror(status), (int)(kWaitTimeout / 1000));
	}

	{
		MutexLocker _(fPhyLock);
		_UpdateLink(true);
	}

	format_mac(text, sizeof(text), fMac);
	INFO("%s: ready: MAC %s, PHY %" B_PRId32 " id %#010" B_PRIx32 ", irq %"
		B_PRIu64 "%s, link %s (autonegotiation may still be running)\n",
		fConfig.name, text, fPhyAddress, fPhyId, fConfig.irq,
		fInterruptInstalled ? "" : " (not installed)",
		fLinkUp ? "up" : "down");

	// The network stack registers a link-state semaphore and then only asks
	// for the link when it is released, so the PHY must be polled here.
	char threadName[B_OS_NAME_LENGTH];
	snprintf(threadName, sizeof(threadName), "dwmac %s watchdog",
		fConfig.name);
	fWatchdog = spawn_kernel_thread(_WatchdogThread, threadName,
		B_LOW_PRIORITY, this);
	if (fWatchdog < 0 || resume_thread(fWatchdog) != B_OK) {
		ERROR("%s: cannot start the watchdog thread; link changes will not "
			"be seen\n", fConfig.name);
		fWatchdog = -1;
	}
	return B_OK;
}


// #pragma mark - start / stop


void
DwmacDevice::_SetupRings()
{
	memset(fDmaBase, 0, (kTxCount + kRxCount) * kDescStride);
	for (uint32 i = 0; i < kRxCount; i++) {
		volatile dwmac_desc* desc = _RxDesc(i);
		phys_addr_t buffer = _Phys(_RxBuffer(i));
		desc->des0 = low32(buffer);
		desc->des1 = high32(buffer);
		desc->des2 = 0;
		desc->des3 = kDesc3Own | kDesc3RxIoc | kDesc3Buf1v;
	}
	// Flush descriptors and drop any cached copies of the RX buffers.
	_FlushPhys(fDmaPhys, fDmaSize);
	fRxNext = 0;
	fTxNext = 0;
	fTxDirty = 0;
}


status_t
DwmacDevice::_Start()
{
	// Configuration sequence from U-Boot eqos_start() (after PHY setup).
	if (fResetPending) {
		status_t status = _ResetDma();
		if (status != B_OK)
			return status;
		fResetPending = false;
		_Write(kMacIntEnable, 0);
		_Write(kMmcRxIntrMask, 0xffffffff);
		_Write(kMmcTxIntrMask, 0xffffffff);
		_Write(kMmcIpcRxIntrMask, 0xffffffff);
		_Write(kMacUsTicCounter, 125 - 1);
	}

	fClosing = false;
	fFatalStatus = 0;
	fFatalLogged = false;
	fTxStallTicks = 0;
	fRxStoppedTicks = 0;
	fTxStoppedTicks = 0;
	_SetupRings();

	// MTL: store and forward, all FIFO RAM to queue 0
	_SetBits(kMtlTxq0OpMode, kMtlTxqTsf
		| (kMtlTxqTxqenEnabled << kMtlTxqTxqenShift));
	_Write(kMtlTxq0QuantumWeight, 0x10);
	_SetBits(kMtlRxq0OpMode, kMtlRxqRsf);

	uint32 txFifo = (fHwFeature1 >> kMacHwFeat1TxFifoShift)
		& kMacHwFeat1FifoMask;
	uint32 rxFifo = (fHwFeature1 >> kMacHwFeat1RxFifoShift)
		& kMacHwFeat1FifoMask;
	// r/tx_fifo_sz is log2(n / 128); r/tqs is (n / 256) - 1
	uint32 tqs = (128u << txFifo) / 256 - 1;
	uint32 rqs = (128u << rxFifo) / 256 - 1;
	fTqs = tqs;
	_ClearSetBits(kMtlTxq0OpMode, kMtlTxqTqsMask << kMtlTxqTqsShift,
		tqs << kMtlTxqTqsShift);
	_ClearSetBits(kMtlRxq0OpMode, kMtlRxqRqsMask << kMtlRxqRqsShift,
		rqs << kMtlRxqRqsShift);

	// Flow control only if the queue has 4 KiB or more
	if (rqs >= (4096 / 256) - 1) {
		uint32 rfd, rfa;
		_SetBits(kMtlRxq0OpMode, kMtlRxqEhfc);
		if (rqs == (4096 / 256) - 1) {
			rfd = 0x3;
			rfa = 0x1;
		} else if (rqs == (8192 / 256) - 1) {
			rfd = 0x6;
			rfa = 0xa;
		} else if (rqs == (16384 / 256) - 1) {
			rfd = 0x6;
			rfa = 0x12;
		} else {
			rfd = 0x6;
			rfa = 0x1e;
		}
		_ClearSetBits(kMtlRxq0OpMode,
			(kMtlRxqRfdMask << kMtlRxqRfdShift)
				| (kMtlRxqRfaMask << kMtlRxqRfaShift),
			(rfd << kMtlRxqRfdShift) | (rfa << kMtlRxqRfaShift));
	}

	// MAC
	_ClearSetBits(kMacRxqCtrl0, kMacRxqCtrl0Rxq0EnMask,
		kMacRxqCtrl0Rxq0EnDcb);
	_SetBits(kMacRxqCtrl1, kMacRxqCtrl1Mcbcqen);
	// Accept all multicast instead of programming the hash filter.
	_Write(kMacPacketFilter, kMacFilterPm | (fPromiscuous ? (uint32)kMacFilterPr : 0u));
	_SetBits(kMacQ0TxFlowCtrl, 0xffffu << kMacQ0TxFlowCtrlPtShift);
	_ClearBits(kMacTxqPrtyMap0, 0xff);
	_ClearBits(kMacRxqCtrl2, 0xff);
	_SetBits(kMacQ0TxFlowCtrl, kMacQ0TxFlowCtrlTfe);
	_SetBits(kMacRxFlowCtrl, kMacRxFlowCtrlRfe);
	_ClearSetBits(kMacConfig,
		kMacConfigGpslce | kMacConfigWd | kMacConfigJd | kMacConfigJe,
		kMacConfigCst | kMacConfigAcs);
	_WriteMacAddress();
	_Write(kMacIntEnable, 0);

	// DMA
	_SetBits(kDmaCh0TxControl, kDmaChTxOsp);
	_ClearSetBits(kDmaCh0RxControl, kDmaChRxRbszMask << kDmaChRxRbszShift,
		kBufferSize << kDmaChRxRbszShift);
	uint32 skip = (kDescStride - sizeof(dwmac_desc)) / kAxiBusWidth;
	_ClearSetBits(kDmaCh0Control, 0x7u << kDmaChControlDslShift,
		kDmaChControlPblx8 | (skip << kDmaChControlDslShift));
	uint32 pbl = tqs + 1;
	if (pbl > 32)
		pbl = 32;
	_ClearSetBits(kDmaCh0TxControl, kDmaChTxPblMask << kDmaChTxPblShift,
		pbl << kDmaChTxPblShift);
	_ClearSetBits(kDmaCh0RxControl, kDmaChRxPblMask << kDmaChRxPblShift,
		8u << kDmaChRxPblShift);
	_Write(kDmaSysbusMode, (2u << kDmaSysbusRdOsrShift) | kDmaSysbusEame
		| kDmaSysbusBlen16 | kDmaSysbusBlen8 | kDmaSysbusBlen4);

	phys_addr_t txRing = _Phys((const void*)_TxDesc(0));
	phys_addr_t rxRing = _Phys((const void*)_RxDesc(0));
	_Write(kDmaCh0TxListHigh, high32(txRing));
	_Write(kDmaCh0TxList, low32(txRing));
	_Write(kDmaCh0TxRingLen, kTxCount - 1);
	_Write(kDmaCh0RxListHigh, high32(rxRing));
	_Write(kDmaCh0RxList, low32(rxRing));
	_Write(kDmaCh0RxRingLen, kRxCount - 1);

	_Write(kDmaCh0Status, 0xffffffff);
	_Write(kDmaCh0IntEnable, kDmaIntMask);

	_SetBits(kDmaCh0TxControl, kDmaChTxSt);
	_SetBits(kDmaCh0RxControl, kDmaChRxSr);
	// The TX tail is written per packet. RX tail at the last descriptor,
	// as U-Boot does (tail == current would mean "nothing available").
	_Write(kDmaCh0RxTail, low32(_Phys((const void*)_RxDesc(kRxCount - 1))));

	fStarted = true;
	INFO("%s: started: MTL tqs %" B_PRIu32 " rqs %" B_PRIu32 ", TX pbl %" B_PRIu32
		" x8, DSL %" B_PRIu32 ", CH0 ctl %#010" B_PRIx32 " tx %#010" B_PRIx32
		" rx %#010" B_PRIx32 ", MAC_CONFIG %#010" B_PRIx32 "\n", fConfig.name,
		tqs, rqs, pbl, skip, _Read(kDmaCh0Control), _Read(kDmaCh0TxControl),
		_Read(kDmaCh0RxControl), _Read(kMacConfig));

	// Force the MAC speed/duplex/TE/RE setup for the current link.
	fLinkUp = false;
	_UpdateLink(true);
	return B_OK;
}


void
DwmacDevice::_Stop()
{
	// U-Boot eqos_stop() without stopping clocks or asserting resets.
	if (!fStarted)
		return;
	fStarted = false;
	_Write(kDmaCh0IntEnable, 0);

	_ClearBits(kDmaCh0TxControl, kDmaChTxSt);
	for (int i = 0; i < 100000; i++) {
		uint32 value = _Read(kMtlTxq0Debug);
		uint32 trcsts = (value >> kMtlTxqDebugTrcstsShift)
			& kMtlTxqDebugTrcstsMask;
		if (trcsts != 1 && (value & kMtlTxqDebugTxqsts) == 0)
			break;
		spin(1);
	}
	_ClearBits(kMacConfig, kMacConfigTe | kMacConfigRe);
	for (int i = 0; i < 100000; i++) {
		uint32 value = _Read(kMtlRxq0Debug);
		uint32 prxq = (value >> kMtlRxqDebugPrxqShift) & kMtlRxqDebugPrxqMask;
		uint32 rxqsts = (value >> kMtlRxqDebugRxqstsShift)
			& kMtlRxqDebugRxqstsMask;
		if (prxq == 0 && rxqsts == 0)
			break;
		spin(1);
	}
	_ClearBits(kDmaCh0RxControl, kDmaChRxSr);
	INFO("%s: stopped (rx %" B_PRIu32 " tx %" B_PRIu32 " packets, rx errors %"
		B_PRIu32 ", tx errors %" B_PRIu32 ", irqs %" B_PRId32 ")\n",
		fConfig.name, fRxPackets, fTxPackets, fRxErrors, fTxErrors, fIrqCount);
}


// #pragma mark - link


bool
DwmacDevice::_ReadPhyLink(bool& up, uint32& speed, bool& fullDuplex)
{
	int32 phy = fPhyAddress;
	if (phy < 0)
		return false;
	if (fPhyIsYt8531)
		return yt8531_read_status(this, phy, up, speed, fullDuplex);

	// Generic: BMSR link bit is latched low, read twice.
	MdioRead(phy, kMiiBmsr);
	int32 bmsr = MdioRead(phy, kMiiBmsr);
	if (bmsr < 0)
		return false;
	up = (bmsr & kBmsrLink) != 0 && (bmsr & kBmsrAnComplete) != 0;
	speed = 10;
	fullDuplex = false;
	if (!up)
		return true;
	int32 ctrl1000 = MdioRead(phy, kMiiCtrl1000);
	int32 stat1000 = MdioRead(phy, kMiiStat1000);
	int32 anar = MdioRead(phy, kMiiAnar);
	int32 anlpar = MdioRead(phy, kMiiAnlpar);
	if (ctrl1000 < 0 || stat1000 < 0 || anar < 0 || anlpar < 0)
		return false;
	int32 common = anar & anlpar;
	if ((ctrl1000 & kCtrl1000Fd) != 0 && (stat1000 & kStat1000Fd) != 0) {
		speed = 1000;
		fullDuplex = true;
	} else if ((ctrl1000 & kCtrl1000Hd) != 0
		&& (stat1000 & kStat1000Hd) != 0) {
		speed = 1000;
	} else if ((common & kAnar100Fd) != 0) {
		speed = 100;
		fullDuplex = true;
	} else if ((common & kAnar100Hd) != 0) {
		speed = 100;
	} else if ((common & kAnar10Fd) != 0) {
		fullDuplex = true;
	}
	return true;
}


void
DwmacDevice::_ApplyLink()
{
	// U-Boot eqos_adjust_link(); called with fPhyLock held.
	if (!fStarted)
		return;
	_ClearBits(kMacConfig, kMacConfigTe | kMacConfigRe);
	if (!fLinkUp)
		return;

	if (fLinkFullDuplex)
		_SetBits(kMacConfig, kMacConfigDm);
	else {
		_ClearBits(kMacConfig, kMacConfigDm);
		// U-Boot WAR: flush the TX queue when switching to half duplex
		_SetBits(kMtlTxq0OpMode, kMtlTxqFtq);
	}
	switch (fLinkSpeed) {
		case 1000:
			_ClearBits(kMacConfig, kMacConfigPs | kMacConfigFes);
			break;
		case 100:
			_SetBits(kMacConfig, kMacConfigPs | kMacConfigFes);
			break;
		default:
			_ClearSetBits(kMacConfig, kMacConfigFes, kMacConfigPs);
			break;
	}
	_Jh7110SetTxClock(fLinkSpeed);
	if (fPhyIsYt8531 && fConfig.phyNodeFound)
		yt8531_set_tx_inverted(this, fPhyAddress, fConfig.yt, fLinkSpeed);
	_SetBits(kMacConfig, kMacConfigTe | kMacConfigRe);
}


void
DwmacDevice::_UpdateLink(bool log)
{
	// called with fPhyLock held
	bool up = false;
	uint32 speed = 0;
	bool fullDuplex = false;
	fLinkPolls++;
	if (!_ReadPhyLink(up, speed, fullDuplex)) {
		if (log && fLinkPolls < 4)
			ERROR("%s: cannot read PHY status\n", fConfig.name);
		return;
	}
	if (!up) {
		speed = 0;
		fullDuplex = false;
	}
	bool changed = up != fLinkUp || speed != fLinkSpeed
		|| fullDuplex != fLinkFullDuplex;
	if (!changed)
		return;

	fLinkUp = up;
	fLinkSpeed = speed;
	fLinkFullDuplex = fullDuplex;
	_ApplyLink();

	// Link changes are always logged (they are rare).
	if (up) {
		INFO("%s: link up, %" B_PRIu32 " Mbit/s %s duplex (MAC_CONFIG %#010"
			B_PRIx32 ", PHYIF %#010" B_PRIx32 ")\n", fConfig.name, speed,
			fullDuplex ? "full" : "half", _Read(kMacConfig),
			_Read(kMacPhyifCtrlStatus));
	} else {
		INFO("%s: link down (BMSR %#06" B_PRIx32 ")\n", fConfig.name,
			MdioRead(fPhyAddress, kMiiBmsr));
	}
	if (fLinkStateSem >= 0)
		release_sem_etc(fLinkStateSem, 1, B_DO_NOT_RESCHEDULE);
}


// #pragma mark - watchdog


status_t
DwmacDevice::_WatchdogThread(void* data)
{
	DwmacDevice* device = (DwmacDevice*)data;
	uint32 tick = 0;
	while (!device->fWatchdogQuit) {
		// 1 s in 100 ms steps, so uninit does not wait long
		for (int i = 0; i < 10 && !device->fWatchdogQuit; i++)
			snooze(100000);
		if (device->fWatchdogQuit)
			break;
		device->_WatchdogTick(++tick);
	}
	return B_OK;
}


void
DwmacDevice::_WatchdogTick(uint32 tick)
{
	MutexLocker phy(fPhyLock);
	_UpdateLink(true);

	if (fOpenCount > 0 && !fClosing) {
		MutexLocker rx(fRxLock);
		MutexLocker tx(fTxLock);
		if (fStarted)
			_CheckStall();
		else if (system_time() - fLastRestart >= kRestartBackoff) {
			// A reset did not bring the DMA back (e.g. no RGMII RX clock
			// without a cable); try again.
			_Restart("DMA still down after an earlier reset");
		}
		if (tick % kStatsTicks == 0)
			_LogStats();
	}
}


void
DwmacDevice::_CheckStall()
{
	// called with fPhyLock, fRxLock and fTxLock held, DMA started
	const char* why = NULL;
	if (fFatalStatus != 0)
		why = "fatal bus error";

	uint32 debug = _Read(kDmaDebugStatus0);
	uint32 rps = (debug >> 8) & 0xf;
	uint32 tps = (debug >> 12) & 0xf;

	// RX and TX DMA must never be "stopped" while started (only SR/ST
	// clear or a fatal error stops them). Seen twice to skip transitions.
	fRxStoppedTicks = rps == kRpsStopped ? fRxStoppedTicks + 1 : 0;
	fTxStoppedTicks = tps == kTpsStopped ? fTxStoppedTicks + 1 : 0;
	if (why == NULL && fRxStoppedTicks >= kDmaStoppedTicks)
		why = "RX DMA stopped";
	if (why == NULL && fTxStoppedTicks >= kDmaStoppedTicks)
		why = "TX DMA stopped";

	// TX: in-flight descriptors that the DMA does not complete while the
	// link is up (the MAC transmitter only runs with link).
	_ReclaimTx();
	if (fTxDirty != fTxNext && fLinkUp) {
		if (fTxDirty == fTxStallIndex)
			fTxStallTicks++;
		else {
			fTxStallIndex = fTxDirty;
			fTxStallTicks = 0;
		}
		if (why == NULL && fTxStallTicks >= kTxStallTicks)
			why = "TX descriptor stall (DMA keeps OWN)";
	} else
		fTxStallTicks = 0;

	// RX suspended (no buffer): every descriptor before fRxNext is owned by
	// the DMA again, so re-arm the tail; harmless if the ring is just full.
	if (why == NULL && rps == kRpsSuspended) {
		fRxKicks++;
		_Write(kDmaCh0RxTail, low32(_Phys((const void*)_RxDesc(
			(fRxNext + kRxCount - 1) % kRxCount))));
	}

	fMissedFrames += _Read(kDmaCh0MissFrames) & 0x7ff;

	if (why == NULL)
		return;
	fStalls++;
	if (system_time() - fLastRestart < kRestartBackoff)
		return;
	_Restart(why);
}


void
DwmacDevice::_Restart(const char* why)
{
	// called with fPhyLock, fRxLock and fTxLock held. Frames in the rings
	// are lost; the readers/writers only see the rings again after _Start.
	fRestarts++;
	fLastRestart = system_time();
	bool log = fRestarts <= kLogRestarts || fRestarts % 100 == 0;
	if (log) {
		ERROR("%s: reset #%" B_PRIu32 ": %s (CH0 status %#010" B_PRIx32
			" debug %#010" B_PRIx32 " cur tx %#010" B_PRIx32 " rx %#010"
			B_PRIx32 ", tx next %" B_PRIu32 " dirty %" B_PRIu32 ", rx next %"
			B_PRIu32 ")\n", fConfig.name, fRestarts, why,
			fRegs != NULL ? _Read(kDmaCh0Status) : 0,
			fRegs != NULL ? _Read(kDmaDebugStatus0) : 0,
			fRegs != NULL ? _Read(kDmaCh0CurTxDesc) : 0,
			fRegs != NULL ? _Read(kDmaCh0CurRxDesc) : 0, fTxNext, fTxDirty,
			fRxNext);
	}
	_Stop();
	// _Start() does the software reset (clears every DMA/MTL/MAC register
	// and re-masks the MMC interrupts) before loading fresh rings.
	fResetPending = true;
	status_t status = _Start();
	if (status != B_OK) {
		fRestartFailures++;
		if (log) {
			ERROR("%s: reset #%" B_PRIu32 " failed: %s; retrying in %d s\n",
				fConfig.name, fRestarts, strerror(status),
				(int)(kRestartBackoff / 1000000));
		}
	} else if (log)
		INFO("%s: reset #%" B_PRIu32 " done, DMA running\n", fConfig.name,
			fRestarts);
	release_sem_etc(fRxSem, 1, B_DO_NOT_RESCHEDULE);
	release_sem_etc(fTxSem, 1, B_DO_NOT_RESCHEDULE);
}


void
DwmacDevice::_LogStats()
{
	// called with fPhyLock, fRxLock and fTxLock held; one line per minute
	INFO("%s: stats: link %s %" B_PRIu32 "%s, rx %" B_PRIu32 " (+%" B_PRIu32
		") errors %" B_PRIu32 " missed %" B_PRIu32 ", tx %" B_PRIu32 " (+%"
		B_PRIu32 ") errors %" B_PRIu32 ", irqs %" B_PRId32 " (rx %" B_PRId32
		" tx %" B_PRId32 " rbu %" B_PRId32 " spurious %" B_PRId32 "), "
		"descriptor stalls %" B_PRIu32 ", resets %" B_PRIu32 " (failed %"
		B_PRIu32 "), rx re-arms %" B_PRIu32 ", dma %s\n", fConfig.name,
		fLinkUp ? "up" : "down", fLinkSpeed, fLinkFullDuplex ? "FD" : "HD",
		fRxPackets, fRxPackets - fLastRxPackets, fRxErrors, fMissedFrames,
		fTxPackets, fTxPackets - fLastTxPackets, fTxErrors, fIrqCount,
		fRxIrqs, fTxIrqs, fRbuCount, fSpuriousIrqs, fStalls, fRestarts,
		fRestartFailures, fRxKicks, fStarted ? "running" : "DOWN");
	fLastRxPackets = fRxPackets;
	fLastTxPackets = fTxPackets;
}


// #pragma mark - interrupt


int32
DwmacDevice::_InterruptHandler(void* data)
{
	return ((DwmacDevice*)data)->_Interrupt();
}


int32
DwmacDevice::_Interrupt()
{
	uint32 dmaStatus = _Read(kDmaStatus);
	uint32 status = _Read(kDmaCh0Status);
	if (status == 0 && (dmaStatus & (kDmaStatusMac | kDmaStatusMtl)) == 0) {
		atomic_add(&fSpuriousIrqs, 1);
		return B_UNHANDLED_INTERRUPT;
	}
	atomic_add(&fIrqCount, 1);
	_Write(kDmaCh0Status, status);

	if ((dmaStatus & kDmaStatusMac) != 0) {
		// Should not happen (MAC_INT_EN = 0); reading clears most sources.
		_Read(kMacIntStatus);
		atomic_add(&fMacIrqs, 1);
	}
	if ((dmaStatus & kDmaStatusMtl) != 0)
		_Read(kMtlIntStatus);

	bool reschedule = false;
	if ((status & (kDmaChIntRi | kDmaChIntRbu)) != 0) {
		if ((status & kDmaChIntRi) != 0)
			atomic_add(&fRxIrqs, 1);
		if ((status & kDmaChIntRbu) != 0)
			atomic_add(&fRbuCount, 1);
		release_sem_etc(fRxSem, 1, B_DO_NOT_RESCHEDULE);
		reschedule = true;
	}
	if ((status & kDmaChIntTi) != 0) {
		atomic_add(&fTxIrqs, 1);
		release_sem_etc(fTxSem, 1, B_DO_NOT_RESCHEDULE);
		reschedule = true;
	}
	if ((status & kDmaChIntFbe) != 0) {
		fFatalStatus = status;
		release_sem_etc(fRxSem, 1, B_DO_NOT_RESCHEDULE);
		release_sem_etc(fTxSem, 1, B_DO_NOT_RESCHEDULE);
		reschedule = true;
	}
	return reschedule ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


// #pragma mark - data path


status_t
DwmacDevice::_Wait(sem_id sem)
{
	status_t status = acquire_sem_etc(sem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, kWaitTimeout);
	if (status == B_TIMED_OUT)
		return B_OK;		// poll the ring anyway
	if (status != B_OK)
		return status;
	int32 count = 0;
	if (get_sem_count(sem, &count) == B_OK && count > 0)
		acquire_sem_etc(sem, count, B_RELATIVE_TIMEOUT, 0);
	return B_OK;
}


void
DwmacDevice::_RecycleRx(uint32 index)
{
	// U-Boot eqos_free_pkt(): give the descriptor back, move the tail.
	volatile dwmac_desc* desc = _RxDesc(index);
	phys_addr_t buffer = _Phys(_RxBuffer(index));
	// Drop any lines the CPU pulled in while reading the old frame.
	_FlushPhys(buffer, kBufferSize);
	desc->des0 = low32(buffer);
	desc->des1 = high32(buffer);
	desc->des2 = 0;
	io_fence();
	desc->des3 = kDesc3Own | kDesc3RxIoc | kDesc3Buf1v;
	_FlushDesc(desc);
	_Write(kDmaCh0RxTail, low32(_Phys((const void*)desc)));
	fRxNext = (index + 1) % kRxCount;
}


status_t
DwmacDevice::_WaitForRxFrame(uint32& index, uint32& length)
{
	// called with fRxLock held; returns with it held
	while (true) {
		if (fClosing || fOpenCount == 0)
			return B_INTERRUPTED;
		if (!fStarted || fFatalStatus != 0) {
			// The watchdog is resetting the DMA (or will within a second).
			if (fFatalStatus != 0 && !fFatalLogged) {
				fFatalLogged = true;
				ERROR("%s: fatal bus error, DMA stopped (CH0 status %#010"
					B_PRIx32 ", DMA_STATUS %#010" B_PRIx32 "); the watchdog "
					"resets it\n", fConfig.name, fFatalStatus,
					_Read(kDmaStatus));
			}
			if (fNonblocking)
				return B_WOULD_BLOCK;
			mutex_unlock(&fRxLock);
			status_t status = _Wait(fRxSem);
			mutex_lock(&fRxLock);
			if (status != B_OK)
				return status;
			continue;
		}

		volatile dwmac_desc* desc = _RxDesc(fRxNext);
		_FlushDesc(desc);
		uint32 des3 = desc->des3;
		if ((des3 & kDesc3Own) == 0) {
			index = fRxNext;
			length = des3 & kDesc3LengthMask;
			bool bad = (des3 & (kDesc3Fd | kDesc3Ld)) != (kDesc3Fd | kDesc3Ld)
				|| (des3 & (kDesc3ErrorSummary | kDesc3Ctxt)) != 0
				|| length == 0 || length > kBufferSize;
			if (bad) {
				fRxErrors++;
				if (fRxErrors <= kLogErrors) {
					ERROR("%s: rx drop, desc %" B_PRIu32 " des3 %#010" B_PRIx32
						" des1 %#010" B_PRIx32 "\n", fConfig.name, index, des3,
						desc->des1);
				}
				_RecycleRx(index);
				continue;
			}
			_FlushPhys(_Phys(_RxBuffer(index)), length);
			if (!fPollWarned && fInterruptInstalled && fRxPackets >= 16
				&& fIrqCount == 0) {
				fPollWarned = true;
				INFO("%s: packets arrive but no interrupt from PLIC irq %"
					B_PRIu64 " yet (spurious %" B_PRId32 "); running on %d ms "
					"polling\n", fConfig.name, fConfig.irq, fSpuriousIrqs,
					(int)(kWaitTimeout / 1000));
			}
			return B_OK;
		}

		if (fNonblocking)
			return B_WOULD_BLOCK;
		mutex_unlock(&fRxLock);
		status_t status = _Wait(fRxSem);
		mutex_lock(&fRxLock);
		if (status != B_OK)
			return status;
	}
}


void
DwmacDevice::_ReclaimTx()
{
	// called with fTxLock held
	while (fTxDirty != fTxNext) {
		volatile dwmac_desc* desc = _TxDesc(fTxDirty);
		_FlushDesc(desc);
		uint32 des3 = desc->des3;
		if ((des3 & kDesc3Own) != 0)
			break;
		if ((des3 & kDesc3ErrorSummary) != 0) {
			fTxErrors++;
			if (fTxErrors <= kLogErrors) {
				ERROR("%s: tx error, desc %" B_PRIu32 " des3 %#010" B_PRIx32
					"\n", fConfig.name, fTxDirty, des3);
			}
		}
		fTxDirty = (fTxDirty + 1) % kTxCount;
	}
}


status_t
DwmacDevice::_WaitForTxSlot(uint32& index)
{
	// called with fTxLock held; returns with it held
	while (true) {
		if (fClosing || fOpenCount == 0)
			return B_INTERRUPTED;
		if (fStarted && fFatalStatus == 0) {
			_ReclaimTx();
			if ((fTxNext + 1) % kTxCount != fTxDirty) {
				index = fTxNext;
				return B_OK;
			}
		}
		// Ring full (or DMA being reset). With no link the MAC transmitter
		// is off and nothing drains: drop instead of blocking the stack.
		if (!fLinkUp)
			return ENETDOWN;
		if (fNonblocking)
			return B_WOULD_BLOCK;
		mutex_unlock(&fTxLock);
		status_t status = _Wait(fTxSem);
		mutex_lock(&fTxLock);
		if (status != B_OK)
			return status;
	}
}


void
DwmacDevice::_QueueTx(uint32 index, uint32 length)
{
	// U-Boot eqos_send(), without waiting for completion.
	uint8* data = _TxBuffer(index);
	phys_addr_t buffer = _Phys(data);
	_FlushPhys(buffer, length);

	volatile dwmac_desc* desc = _TxDesc(index);
	desc->des0 = low32(buffer);
	desc->des1 = high32(buffer);
	desc->des2 = (length & kTxDesc2LengthMask) | kTxDesc2Ioc;
	io_fence();
	desc->des3 = kDesc3Own | kDesc3Fd | kDesc3Ld | length;
	_FlushDesc(desc);

	fTxNext = (index + 1) % kTxCount;
	_Write(kDmaCh0TxTail, low32(_Phys((const void*)_TxDesc(fTxNext))));

	fTxPackets++;
	if (fTxPackets <= kLogPackets) {
		INFO("%s: tx #%" B_PRIu32 ": %" B_PRIu32 " bytes, type %02x%02x to "
			"%02x:%02x:%02x:%02x:%02x:%02x\n", fConfig.name, fTxPackets,
			length, data[12], data[13], data[0], data[1], data[2], data[3],
			data[4], data[5]);
	}
}


status_t
DwmacDevice::_Receive(net_buffer** _buffer)
{
	MutexLocker locker(fRxLock);
	uint32 index, length;
	status_t status = _WaitForRxFrame(index, length);
	if (status != B_OK)
		return status;

	uint8* data = _RxBuffer(index);
	fRxPackets++;
	if (fRxPackets <= kLogPackets) {
		INFO("%s: rx #%" B_PRIu32 ": %" B_PRIu32 " bytes, type %02x%02x from "
			"%02x:%02x:%02x:%02x:%02x:%02x (irqs %" B_PRId32 ")\n",
			fConfig.name, fRxPackets, length, data[12], data[13], data[6],
			data[7], data[8], data[9], data[10], data[11], fIrqCount);
	}

	net_buffer* buffer = gBufferModule->create(0);
	if (buffer != NULL
		&& gBufferModule->append(buffer, data, length) != B_OK) {
		gBufferModule->free(buffer);
		buffer = NULL;
	}
	_RecycleRx(index);
	if (buffer == NULL)
		return B_NO_MEMORY;
	*_buffer = buffer;
	return B_OK;
}


status_t
DwmacDevice::_Send(net_buffer* buffer)
{
	size_t size = buffer->size;
	if (size < 14 || size > kBufferSize)
		return B_BAD_VALUE;

	MutexLocker locker(fTxLock);
	uint32 index;
	status_t status = _WaitForTxSlot(index);
	if (status != B_OK)
		return status;
	status = gBufferModule->read(buffer, 0, _TxBuffer(index), size);
	if (status != B_OK)
		return B_BAD_DATA;
	_QueueTx(index, size);
	locker.Unlock();

	gBufferModule->free(buffer);
	return B_OK;
}


status_t
DwmacDevice::Read(void* buffer, size_t* _length)
{
	MutexLocker locker(fRxLock);
	uint32 index, length;
	status_t status = _WaitForRxFrame(index, length);
	if (status != B_OK) {
		*_length = 0;
		return status;
	}
	if (length > *_length)
		length = *_length;
	status = user_memcpy(buffer, _RxBuffer(index), length);
	fRxPackets++;
	_RecycleRx(index);
	*_length = status == B_OK ? length : 0;
	return status;
}


status_t
DwmacDevice::Write(const void* buffer, size_t* _length)
{
	size_t size = *_length;
	if (size < 14 || size > kBufferSize) {
		*_length = 0;
		return B_BAD_VALUE;
	}
	MutexLocker locker(fTxLock);
	uint32 index;
	status_t status = _WaitForTxSlot(index);
	if (status == B_OK)
		status = user_memcpy(_TxBuffer(index), buffer, size);
	if (status != B_OK) {
		*_length = 0;
		return status;
	}
	_QueueTx(index, size);
	return B_OK;
}


// #pragma mark - open / close / control


status_t
DwmacDevice::Open(bool nonblocking)
{
	MutexLocker _(fPhyLock);
	fNonblocking = nonblocking;
	if (fOpenCount++ > 0)
		return B_OK;
	INFO("%s: open\n", fConfig.name);
	MutexLocker rx(fRxLock);
	MutexLocker tx(fTxLock);
	// Reopened after close() but before free(): stop before reprogramming.
	if (fStarted)
		_Stop();
	status_t status = _Start();
	if (status != B_OK)
		fOpenCount--;
	return status;
}


void
DwmacDevice::Close()
{
	MutexLocker _(fPhyLock);
	if (--fOpenCount > 0)
		return;
	fClosing = true;
	release_sem_etc(fRxSem, 1, 0);
	release_sem_etc(fTxSem, 1, 0);
}


void
DwmacDevice::Free()
{
	MutexLocker _(fPhyLock);
	if (fOpenCount > 0 || !fStarted)
		return;
	MutexLocker rx(fRxLock);
	MutexLocker tx(fTxLock);
	_Stop();
}


status_t
DwmacDevice::Control(uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case ETHER_INIT:
			return B_OK;

		case ETHER_GETADDR:
			return user_memcpy(buffer, fMac, sizeof(fMac));

		case ETHER_GETFRAMESIZE:
		{
			uint32 size = kMaxFrameSize;
			if (length != sizeof(size))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &size, sizeof(size));
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			fNonblocking = value != 0;
			return B_OK;
		}

		case ETHER_SETPROMISC:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			MutexLocker _(fPhyLock);
			fPromiscuous = value != 0;
			if (fStarted) {
				if (fPromiscuous)
					_SetBits(kMacPacketFilter, kMacFilterPr);
				else
					_ClearBits(kMacPacketFilter, kMacFilterPr);
			}
			INFO("%s: promiscuous mode %s\n", fConfig.name,
				fPromiscuous ? "on" : "off");
			return B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			// All multicast is accepted (MAC_PACKET_FILTER.PM).
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
		{
			sem_id sem;
			if (length != sizeof(sem))
				return B_BAD_VALUE;
			if (user_memcpy(&sem, buffer, sizeof(sem)) != B_OK)
				return B_BAD_ADDRESS;
			MutexLocker _(fPhyLock);
			fLinkStateSem = sem;
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			{
				MutexLocker _(fPhyLock);
				_UpdateLink(false);
				state.media = IFM_ETHER;
				if (fLinkUp) {
					state.media |= IFM_ACTIVE
						| (fLinkFullDuplex ? IFM_FULL_DUPLEX : IFM_HALF_DUPLEX)
						| (fLinkSpeed == 1000 ? IFM_1000_T
							: fLinkSpeed == 100 ? IFM_100_TX : IFM_10_T);
				}
				state.speed = (uint64)fLinkSpeed * 1000000;
				state.quality = fLinkUp ? 1000 : 0;
			}
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return _Send((net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return _Receive((net_buffer**)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}
