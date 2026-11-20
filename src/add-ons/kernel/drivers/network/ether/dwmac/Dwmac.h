/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Synopsys DesignWare Ethernet QoS ("dwmac" / "eqos", v5.20) network
 * driver for Beacon (a Haiku-derived operating system), first target the
 * StarFive JH7110 SoC (Orange Pi RV, VisionFive 2).
 *
 * Register map, bit definitions and the controller bring-up sequence are
 * taken from:
 *   U-Boot drivers/net/dwc_eth_qos.c (GPL-2.0), vendor tree
 *     orangepi-xunlong/u-boot-orangepi v2021.10-jh7110 (commit d7274dfe)
 *     Copyright (c) 2016, NVIDIA CORPORATION.
 *     Portions based on U-Boot's rtl8169.c.
 *   Linux drivers/net/ethernet/stmicro/stmmac/dwmac4.h, dwmac4_dma.h,
 *     dwmac4_descs.h (GPL-2.0-only), v6.12
 *     Copyright (C) 2015 STMicroelectronics Ltd
 *   JH7110 clock/reset/syscon facts: Linux dt-bindings starfive,jh7110-crg.h
 *     (GPL-2.0 OR MIT), Linux reset-starfive-jh7110.c (GPL-2.0),
 *     vendor U-Boot jh7110-regs.h / starfive_visionfive2.c (GPL-2.0+),
 *     Linux dwmac-starfive.c (GPL-2.0+).
 *
 * Modified for Beacon/Haiku, 2026 (The Beacon Project).
 * See THIRD_PARTY.md in this directory for exact upstream versions.
 */
#ifndef _DWMAC_H_
#define _DWMAC_H_


#include <KernelExport.h>
#include <device_manager.h>

#include <lock.h>

#include <ether_driver.h>


#define DWMAC_DRIVER_MODULE_NAME	"drivers/network/dwmac/driver_v1"
#define DWMAC_DEVICE_MODULE_NAME	"drivers/network/dwmac/device_v1"
#define DWMAC_DEVICE_ID_GENERATOR	"dwmac/device_id"

struct net_buffer;
struct net_buffer_module_info;

extern device_manager_info* gDeviceManager;
extern net_buffer_module_info* gBufferModule;


// #pragma mark - MAC registers (U-Boot struct eqos_mac_regs, Linux dwmac4.h)

enum {
	kMacConfig			= 0x000,
	kMacPacketFilter	= 0x008,
	kMacQ0TxFlowCtrl	= 0x070,
	kMacRxFlowCtrl		= 0x090,
	kMacTxqPrtyMap0		= 0x098,
	kMacRxqCtrl0		= 0x0a0,
	kMacRxqCtrl1		= 0x0a4,
	kMacRxqCtrl2		= 0x0a8,
	kMacIntStatus		= 0x0b0,
	kMacIntEnable		= 0x0b4,
	kMacUsTicCounter	= 0x0dc,
	kMacPhyifCtrlStatus	= 0x0f8,
	kMacVersion			= 0x110,
	kMacDebug			= 0x114,
	kMacHwFeature0		= 0x11c,
	kMacHwFeature1		= 0x120,
	kMacHwFeature2		= 0x124,
	kMacHwFeature3		= 0x128,
	kMacMdioAddress		= 0x200,
	kMacMdioData		= 0x204,
	kMacAddr0High		= 0x300,
	kMacAddr0Low		= 0x304,

	// MMC counters block (dwmac4 base 0x700), interrupt masks
	kMmcControl			= 0x700,
	kMmcRxIntrMask		= 0x70c,
	kMmcTxIntrMask		= 0x710,
	kMmcIpcRxIntrMask	= 0x800,
};

// MAC_CONFIGURATION
enum {
	kMacConfigRe		= 1u << 0,
	kMacConfigTe		= 1u << 1,
	kMacConfigLm		= 1u << 12,
	kMacConfigDm		= 1u << 13,
	kMacConfigFes		= 1u << 14,
	kMacConfigPs		= 1u << 15,
	kMacConfigJe		= 1u << 16,
	kMacConfigJd		= 1u << 17,
	kMacConfigWd		= 1u << 19,
	kMacConfigAcs		= 1u << 20,
	kMacConfigCst		= 1u << 21,
	kMacConfigGpslce	= 1u << 23,
};

// MAC_PACKET_FILTER
enum {
	kMacFilterPr		= 1u << 0,
	kMacFilterPm		= 1u << 4,
	kMacFilterRa		= 1u << 31,
};

enum {
	kMacQ0TxFlowCtrlPtShift	= 16,
	kMacQ0TxFlowCtrlTfe		= 1u << 1,
	kMacRxFlowCtrlRfe		= 1u << 0,
	kMacRxqCtrl0Rxq0EnMask	= 3,
	kMacRxqCtrl0Rxq0EnDcb	= 2,
	kMacRxqCtrl1Mcbcqen		= 1u << 20,
};

// HW_FEATURE1
enum {
	kMacHwFeat1TxFifoShift	= 6,
	kMacHwFeat1RxFifoShift	= 0,
	kMacHwFeat1FifoMask		= 0x1f,
	kMacHwFeat1Addr64Shift	= 14,
	kMacHwFeat1Addr64Mask	= 3,
};

// MDIO_ADDRESS
enum {
	kMdioAddrPaShift	= 21,
	kMdioAddrRdaShift	= 16,
	kMdioAddrCrShift	= 8,
	kMdioAddrCr250_300	= 5,	// CSR clock 250-300 MHz (vendor JH7110 value)
	kMdioAddrSkap		= 1u << 4,
	kMdioAddrGocShift	= 2,
	kMdioAddrGocRead	= 3,
	kMdioAddrGocWrite	= 1,
	kMdioAddrC45e		= 1u << 1,
	kMdioAddrGb			= 1u << 0,
	kMdioDataMask		= 0xffff,
};


// #pragma mark - MTL registers (U-Boot struct eqos_mtl_regs)

enum {
	kMtlIntStatus			= 0xc20,
	kMtlTxq0OpMode			= 0xd00,
	kMtlTxq0Debug			= 0xd08,
	kMtlTxq0QuantumWeight	= 0xd18,
	kMtlQ0IntCtrl			= 0xd2c,
	kMtlRxq0OpMode			= 0xd30,
	kMtlRxq0Debug			= 0xd38,
};

enum {
	kMtlTxqTqsShift		= 16,
	kMtlTxqTqsMask		= 0x1ff,
	kMtlTxqTxqenShift	= 2,
	kMtlTxqTxqenEnabled	= 2,
	kMtlTxqTsf			= 1u << 1,
	kMtlTxqFtq			= 1u << 0,

	kMtlTxqDebugTxqsts		= 1u << 4,
	kMtlTxqDebugTrcstsShift	= 1,
	kMtlTxqDebugTrcstsMask	= 3,

	kMtlRxqRqsShift		= 20,
	kMtlRxqRqsMask		= 0x3ff,
	kMtlRxqRfdShift		= 14,
	kMtlRxqRfdMask		= 0x3f,
	kMtlRxqRfaShift		= 8,
	kMtlRxqRfaMask		= 0x3f,
	kMtlRxqEhfc			= 1u << 7,
	kMtlRxqRsf			= 1u << 5,

	kMtlRxqDebugPrxqShift	= 16,
	kMtlRxqDebugPrxqMask	= 0x7fff,
	kMtlRxqDebugRxqstsShift	= 4,
	kMtlRxqDebugRxqstsMask	= 3,
};


// #pragma mark - DMA registers (U-Boot struct eqos_dma_regs, dwmac4_dma.h)

enum {
	kDmaMode			= 0x1000,
	kDmaSysbusMode		= 0x1004,
	kDmaStatus			= 0x1008,
	kDmaCh0Control		= 0x1100,
	kDmaCh0TxControl	= 0x1104,
	kDmaCh0RxControl	= 0x1108,
	kDmaCh0TxListHigh	= 0x1110,
	kDmaCh0TxList		= 0x1114,
	kDmaCh0RxListHigh	= 0x1118,
	kDmaCh0RxList		= 0x111c,
	kDmaCh0TxTail		= 0x1120,
	kDmaCh0RxTail		= 0x1128,
	kDmaCh0TxRingLen	= 0x112c,
	kDmaCh0RxRingLen	= 0x1130,
	kDmaCh0IntEnable	= 0x1134,
	kDmaCh0CurTxDesc	= 0x1144,
	kDmaCh0CurRxDesc	= 0x114c,
	kDmaCh0Status		= 0x1160,
	kDmaCh0MissFrames	= 0x116c,
};

enum {
	kDmaModeSwr				= 1u << 0,

	kDmaSysbusRdOsrShift	= 16,
	kDmaSysbusEame			= 1u << 11,
	kDmaSysbusBlen16		= 1u << 3,
	kDmaSysbusBlen8			= 1u << 2,
	kDmaSysbusBlen4			= 1u << 1,

	kDmaStatusMtl			= 1u << 16,
	kDmaStatusMac			= 1u << 17,

	kDmaChControlDslShift	= 18,
	kDmaChControlPblx8		= 1u << 16,

	kDmaChTxPblShift		= 16,
	kDmaChTxPblMask			= 0x3f,
	kDmaChTxOsp				= 1u << 4,
	kDmaChTxSt				= 1u << 0,

	kDmaChRxPblShift		= 16,
	kDmaChRxPblMask			= 0x3f,
	kDmaChRxRbszShift		= 1,
	kDmaChRxRbszMask		= 0x3fff,
	kDmaChRxSr				= 1u << 0,
};

// DMA_CHAN_INTR_ENA / DMA_CHAN_STATUS (layout for core version >= 4.10)
enum {
	kDmaChIntTi		= 1u << 0,
	kDmaChIntTps	= 1u << 1,
	kDmaChIntTbu	= 1u << 2,
	kDmaChIntRi		= 1u << 6,
	kDmaChIntRbu	= 1u << 7,
	kDmaChIntRps	= 1u << 8,
	kDmaChIntRwt	= 1u << 9,
	kDmaChIntEti	= 1u << 10,
	kDmaChIntEri	= 1u << 11,
	kDmaChIntFbe	= 1u << 12,
	kDmaChIntCde	= 1u << 13,
	kDmaChIntAis	= 1u << 14,
	kDmaChIntNis	= 1u << 15,
};


// #pragma mark - descriptors (dwmac4_descs.h, U-Boot struct eqos_desc)

struct dwmac_desc {
	uint32	des0;
	uint32	des1;
	uint32	des2;
	uint32	des3;
};

enum {
	kDesc3Own			= 1u << 31,
	kDesc3Ctxt			= 1u << 30,		// write-back
	kDesc3RxIoc			= 1u << 30,		// RX read format
	kDesc3Fd			= 1u << 29,
	kDesc3Ld			= 1u << 28,
	kDesc3Buf1v			= 1u << 24,		// RX read format
	kDesc3ErrorSummary	= 1u << 15,		// write-back
	kDesc3LengthMask	= 0x7fff,
	kTxDesc2Ioc			= 1u << 31,
	kTxDesc2LengthMask	= 0x3fff,
};


// #pragma mark - standard MII registers (IEEE 802.3 clause 22)

enum {
	kMiiBmcr		= 0x00,
	kMiiBmsr		= 0x01,
	kMiiPhyId1		= 0x02,
	kMiiPhyId2		= 0x03,
	kMiiAnar		= 0x04,
	kMiiAnlpar		= 0x05,
	kMiiCtrl1000	= 0x09,
	kMiiStat1000	= 0x0a,

	kBmcrAnRestart	= 1u << 9,
	kBmcrAnEnable	= 1u << 12,
	kBmcrReset		= 1u << 15,
	kBmsrLink		= 1u << 2,
	kBmsrAnComplete	= 1u << 5,
	kAnarCsma		= 0x0001,
	kAnar10Hd		= 0x0020,
	kAnar10Fd		= 0x0040,
	kAnar100Hd		= 0x0080,
	kAnar100Fd		= 0x0100,
	kAnarPause		= 0x0400,
	kAnarAsymPause	= 0x0800,
	kCtrl1000Hd		= 0x0100,
	kCtrl1000Fd		= 0x0200,
	kStat1000Hd		= 0x0400,
	kStat1000Fd		= 0x0800,
};


// #pragma mark - JH7110 clock, reset and syscon facts

enum {
	kJh7110SysCrgBase		= 0x13020000,
	kJh7110SysSysconBase	= 0x13030000,
	kJh7110AonCrgBase		= 0x17000000,
	kJh7110AonSysconBase	= 0x17010000,
	kJh7110CcacheBase		= 0x02010000,
	kJh7110CcacheFlush64	= 0x200,
	kJh7110CacheLine		= 64,

	kJh7110Gmac0Base		= 0x16030000,
	kJh7110Gmac1Base		= 0x16040000,
	kJh7110Gmac0Irq			= 7,	// "macirq", Linux + vendor jh7110.dtsi
	kJh7110Gmac1Irq			= 78,

	// CRG clock registers: bit 31 gate, bit 30 invert, 27:24 mux, 23:0 div.
	kCrgEnable				= 1u << 31,
	kCrgInvert				= 1u << 30,
	kCrgMuxShift			= 24,
	kCrgMuxMask				= 0xf,
	kCrgDivMask				= 0x00ffffff,

	// reset blocks: assert register (1 = held in reset), status register
	// (1 = out of reset)
	kSysRstAssert			= 0x2f8,
	kSysRstStatus			= 0x308,
	kAonRstAssert			= 0x038,
	kAonRstStatus			= 0x03c,
	kSysRstGmac1Axi			= 66,
	kSysRstGmac1Ahb			= 67,
	kAonRstGmac0Axi			= 0,
	kAonRstGmac0Ahb			= 1,

	// PHY interface select: 1 = RGMII, 4 = RMII, 3-bit field
	kGmac0IfSelReg			= 0x0c,		// AON syscon, starfive,syscon args
	kGmac0IfSelShift		= 18,
	kGmac1IfSelReg			= 0x90,		// SYS syscon
	kGmac1IfSelShift		= 2,
	kIfSelMask				= 0x7,
	kIfSelRgmii				= 1,
	kIfSelRmii				= 4,

	// PLL0 (parent of the GTX clocks) in SYS syscon
	kPll0Reg18				= 0x18,		// dsmpd bit 25
	kPll0FbdivReg			= 0x1c,		// 11:0
	kPll0FracReg			= 0x20,		// frac 23:0, postdiv1 29:28
	kPll0PredivReg			= 0x24,		// 5:0
	kGtxDivMax				= 15,
};

// Clock register indices (register offset = index * 4), Linux
// dt-bindings/clock/starfive,jh7110-crg.h
enum {
	kSysClkGmac1Ahb		= 97,
	kSysClkGmac1Axi		= 98,
	kSysClkGmacSrc		= 99,
	kSysClkGmac1Gtx		= 100,
	kSysClkGmac1RmiiRtx	= 101,
	kSysClkGmac1Ptp		= 102,
	kSysClkGmac1Rx		= 103,
	kSysClkGmac1RxInv	= 104,
	kSysClkGmac1Tx		= 105,
	kSysClkGmac1TxInv	= 106,
	kSysClkGmac1Gtxc	= 107,
	kSysClkGmac0Gtx		= 108,
	kSysClkGmac0Ptp		= 109,
	kSysClkGmacPhy		= 110,
	kSysClkGmac0Gtxc	= 111,

	kAonClkGmac0Ahb		= 2,
	kAonClkGmac0Axi		= 3,
	kAonClkGmac0RmiiRtx	= 4,
	kAonClkGmac0Tx		= 5,
	kAonClkGmac0TxInv	= 6,
	kAonClkGmac0Rx		= 7,
	kAonClkGmac0RxInv	= 8,
};


// #pragma mark - driver configuration from the device tree

// Motorcomm YT8531 board tuning from the vendor DT PHY node; -1 = absent.
struct YtPhyConfig {
	int32	rxcDlyEn;
	int32	rgmiiSwDr;
	int32	rgmiiSwDr2;
	int32	rgmiiSwDrRxc;
	int32	rxDelaySel;
	int32	txDelaySelFe;
	int32	txDelaySel;
	int32	txInverted10;
	int32	txInverted100;
	int32	txInverted1000;
};

struct DwmacConfig {
	char		name[32];
	phys_addr_t	physBase;
	size_t		physSize;
	uint64		irq;
	bool		irqFromDt;
	bool		isJh7110;
	int32		jh7110Index;		// 0 = gmac0, 1 = gmac1, -1 = other
	char		phyMode[24];
	int32		phyAddress;			// from the PHY DT node, -1 = scan
	bool		phyNodeFound;
	char		phyNodeName[32];
	YtPhyConfig	yt;
	bool		hasMac;
	uint8		mac[6];
	const char*	macSource;
};


// #pragma mark - device


class DwmacDevice {
public:
								DwmacDevice();
								~DwmacDevice();

			status_t			Init(const DwmacConfig& config);

			status_t			Open(bool nonblocking);
			void				Close();
			void				Free();

			status_t			Control(uint32 op, void* buffer,
									size_t length);
			status_t			Read(void* buffer, size_t* _length);
			status_t			Write(const void* buffer, size_t* _length);

			const char*			Name() const { return fConfig.name; }

	// MDIO, used by the PHY code (YtPhy.cpp)
			int32				MdioRead(int32 phy, int32 reg);
			status_t			MdioWrite(int32 phy, int32 reg, uint16 value);

private:
	inline	uint32				_Read(uint32 offset)
									{ return fRegs[offset / 4]; }
	inline	void				_Write(uint32 offset, uint32 value)
									{ fRegs[offset / 4] = value; }
	inline	void				_SetBits(uint32 offset, uint32 bits)
									{ _Write(offset, _Read(offset) | bits); }
	inline	void				_ClearBits(uint32 offset, uint32 bits)
									{ _Write(offset, _Read(offset) & ~bits); }
	inline	void				_ClearSetBits(uint32 offset, uint32 clear,
									uint32 set)
									{ _Write(offset,
										(_Read(offset) & ~clear) | set); }

			status_t			_Jh7110Bringup();
			status_t			_Jh7110MapBlocks();
			void				_Jh7110SetTxClock(uint32 speed);
			uint64				_Jh7110Pll0Hz();
			void				_Jh7110ClockOn(volatile uint32* crg,
									uint32 index, const char* what);

			status_t			_ResetDma();
			status_t			_FindPhy();
			void				_ConfigurePhy();
			void				_LogFirmwareState();
			void				_ChooseMacAddress(const uint8* firmwareMac);
			void				_WriteMacAddress();
			status_t			_AllocateRings();

			status_t			_Start();
			void				_Stop();
			void				_SetupRings();

			void				_UpdateLink(bool log);
			void				_ApplyLink();
			bool				_ReadPhyLink(bool& up, uint32& speed,
									bool& fullDuplex);

			// cache maintenance (JH7110 DMA is treated as non-coherent)
			void				_FlushPhys(phys_addr_t address, size_t size);
	inline	phys_addr_t			_Phys(const void* address) const
									{ return fDmaPhys
										+ ((addr_t)address - (addr_t)fDmaBase); }
			volatile dwmac_desc* _TxDesc(uint32 index) const;
			volatile dwmac_desc* _RxDesc(uint32 index) const;
			uint8*				_TxBuffer(uint32 index) const;
			uint8*				_RxBuffer(uint32 index) const;
			void				_FlushDesc(volatile dwmac_desc* desc);

			status_t			_WaitForRxFrame(uint32& index,
									uint32& length);
			void				_RecycleRx(uint32 index);
			status_t			_WaitForTxSlot(uint32& index);
			void				_ReclaimTx();
			void				_QueueTx(uint32 index, uint32 length);
			status_t			_Wait(sem_id sem);

			status_t			_Receive(net_buffer** _buffer);
			status_t			_Send(net_buffer* buffer);

	static	int32				_InterruptHandler(void* data);
			int32				_Interrupt();

private:
			DwmacConfig			fConfig;

			area_id				fRegsArea;
			volatile uint32*	fRegs;

			area_id				fSysCrgArea;
			volatile uint32*	fSysCrg;
			area_id				fAonCrgArea;
			volatile uint32*	fAonCrg;
			area_id				fSysSysconArea;
			volatile uint32*	fSysSyscon;
			area_id				fAonSysconArea;
			volatile uint32*	fAonSyscon;
			area_id				fCcacheArea;
			volatile uint64*	fCcacheFlush64;

			area_id				fDmaArea;
			uint8*				fDmaBase;
			phys_addr_t			fDmaPhys;
			size_t				fDmaSize;

			uint32				fVersion;
			uint32				fHwFeature1;
			uint32				fTqs;

			mutex				fPhyLock;
			int32				fPhyAddress;
			uint32				fPhyId;
			bool				fPhyIsYt8531;
			bool				fLinkUp;
			uint32				fLinkSpeed;		// Mbit/s
			bool				fLinkFullDuplex;
			sem_id				fLinkStateSem;
			uint32				fLinkPolls;

			uint8				fMac[6];

			mutex				fRxLock;
			sem_id				fRxSem;
			uint32				fRxNext;

			mutex				fTxLock;
			sem_id				fTxSem;
			uint32				fTxNext;		// next free slot
			uint32				fTxDirty;		// oldest in-flight slot

			bool				fInterruptInstalled;
			bool				fResetPending;
			int32				fOpenCount;
			bool				fStarted;
			bool				fNonblocking;
			bool				fPromiscuous;
			volatile bool		fClosing;

			// statistics and diagnostics
			uint32				fRxPackets;
			uint32				fTxPackets;
			uint32				fRxErrors;
			uint32				fTxErrors;
			int32				fIrqCount;
			int32				fRxIrqs;
			int32				fTxIrqs;
			int32				fRbuCount;
			int32				fMacIrqs;
			int32				fSpuriousIrqs;
			uint32				fFatalStatus;
			bool				fFatalLogged;
			bool				fPollWarned;
};


// YtPhy.cpp (GPL-2.0-or-later, from vendor U-Boot motorcomm.c)
void yt8531_configure(DwmacDevice* device, int32 phy,
	const YtPhyConfig& config);
void yt8531_set_tx_inverted(DwmacDevice* device, int32 phy,
	const YtPhyConfig& config, uint32 speed);
bool yt8531_read_status(DwmacDevice* device, int32 phy, bool& up,
	uint32& speed, bool& fullDuplex);
void yt8531_log_state(DwmacDevice* device, int32 phy, const char* when);


#endif	// _DWMAC_H_
