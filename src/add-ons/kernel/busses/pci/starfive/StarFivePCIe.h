/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * PCIe host controller driver for the StarFive JH7110 SoC (PLDA XpressRICH
 * root ports), for Beacon (a Haiku-derived operating system).
 *
 * Register definitions and the bring-up sequence are taken from Linux:
 *   drivers/pci/controller/plda/pcie-plda.h      (GPL-2.0)
 *     no copyright line in the file; the PLDA host core it belongs to is
 *     Copyright (C) 2023 Microchip Co. Ltd, StarFive Co. Ltd
 *     Author: Daire McNamara <daire.mcnamara@microchip.com>
 *   drivers/pci/controller/plda/pcie-starfive.c   (GPL-2.0+)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *   drivers/phy/starfive/phy-jh7110-pcie.c        (GPL-2.0+)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *     Author: Minda Chen <minda.chen@starfivetech.com>
 *   drivers/clk/starfive/clk-starfive-jh71x0.h    (GPL-2.0)
 *   drivers/reset/starfive/reset-starfive-jh7110.c (GPL-2.0-or-later)
 *   drivers/pinctrl/starfive/pinctrl-starfive-jh7110-sys.c (GPL-2.0)
 *   include/dt-bindings/{clock,reset}/starfive,jh7110-crg.h (GPL-2.0 OR MIT)
 * and from U-Boot drivers/pci/pcie_starfive.c (orangepi-xunlong
 * v2021.10-jh7110, GPL-2.0+, Copyright (c) 2023 Starfive, Inc.,
 * Author: Mason Huo <mason.huo@starfivetech.com>).
 * The class layout follows Haiku's DWPCIController
 * (Copyright 2022, Haiku, Inc., MIT License).
 *
 * Modified by Beacon contributors.
 * See THIRD_PARTY.md in this directory for exact versions.
 */
#ifndef _STARFIVE_PCIE_H_
#define _STARFIVE_PCIE_H_


#include <bus/PCI.h>
#include <arch/generic/msi.h>

#include <AutoDeleterOS.h>
#include <lock.h>
#include <util/Vector.h>


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define STARFIVE_PCI_DRIVER_MODULE_NAME "busses/pci/starfive/driver_v1"


// #pragma mark - FDT "ranges" flags (IEEE 1275 PCI binding)

enum {
	kFdtPciRangeConfig		= 0x00000000,
	kFdtPciRangeIoPort		= 0x01000000,
	kFdtPciRangeMmio32Bit	= 0x02000000,
	kFdtPciRangeMmio64Bit	= 0x03000000,
	kFdtPciRangeTypeMask	= 0x03000000,
	kFdtPciRangePrefetchable = 0x40000000,
};


// #pragma mark - PLDA XpressRICH bridge registers ("apb" reg window)
// Source: Linux drivers/pci/controller/plda/pcie-plda.h

enum {
	kPldaGenSettings			= 0x080,
	kPldaRpEnable				= 1 << 0,
	kPldaPciIdsDw1				= 0x09c,
	kPldaIdsClassCodeShift		= 16,
	kPldaRevisionIdMask			= 0xff,
	kPldaPciMisc				= 0x0b4,
	kPldaPhyFunctionDis			= 1 << 15,
	kPldaWinRom					= 0x0fc,
	kPldaPrefMemWin64Support	= 1 << 3,

	kPldaImaskLocal				= 0x180,
	kPldaIstatusLocal			= 0x184,
	kPldaImaskHost				= 0x188,
	kPldaIstatusHost			= 0x18c,
	kPldaImsiAddr				= 0x190,
	kPldaIstatusMsi				= 0x194,
	kPldaPmsgSupportRx			= 0x3f0,
	kPldaPmsgLtrSupport			= 1 << 2,

	// Inbound (PCIe -> AXI) translation, table 0
	kPldaAtr0PcieWin0SrcAddrParam	= 0x600,
	kPldaAtr0PcieAtrSize			= 0x25,
	kPldaAtr0PcieAtrSizeShift		= 1,
	kPldaAtr0PcieWin0SrcAddr		= 0x604,

	// Outbound (AXI slave 0 -> PCIe) translation tables, 32 bytes apart
	kPldaAtr0Axi4Slv0SrcAddrParam	= 0x800,
	kPldaAtrSizeShift				= 1,
	kPldaAtrImplEnable				= 1 << 0,
	kPldaAtr0Axi4Slv0SrcAddr		= 0x804,
	kPldaAtr0Axi4Slv0TrslAddrLsb	= 0x808,
	kPldaAtr0Axi4Slv0TrslAddrUdw	= 0x80c,
	kPldaAtr0Axi4Slv0TrslParam		= 0x810,
	kPldaTrslIdPcieTxRx				= 0,
	kPldaTrslIdPcieConfig			= 1,
	kPldaAtrEntrySize				= 0x20,
	kPldaAtrMaxEntries				= 8,

	// Root port's own type 1 config header lives here inside "apb"
	kPldaConfigSpaceOffset		= 0x1000,
};

// IMASK_LOCAL / ISTATUS_LOCAL bits
enum {
	kPldaIntAxiPostErr		= 1u << 16,
	kPldaIntAxiFetchErr		= 1u << 17,
	kPldaIntAxiDiscardErr	= 1u << 18,
	kPldaIntAxiDoorbell		= 1u << 19,
	kPldaIntPciePostErr		= 1u << 20,
	kPldaIntPcieFetchErr	= 1u << 21,
	kPldaIntPcieDiscardErr	= 1u << 22,
	kPldaIntPcieDoorbell	= 1u << 23,
	kPldaIntIntxShift		= 24,
	kPldaIntIntxMask		= 0xfu << 24,
	kPldaIntMsi				= 1u << 28,
	kPldaIntAerEvent		= 1u << 29,
	kPldaIntMiscEvents		= 1u << 30,
	kPldaIntSysErr			= 1u << 31,

	kPldaIntErrorMask = kPldaIntAxiPostErr | kPldaIntAxiFetchErr
		| kPldaIntAxiDiscardErr | kPldaIntPciePostErr | kPldaIntPcieFetchErr
		| kPldaIntPcieDiscardErr | kPldaIntSysErr,
};

enum {
	kPldaMaxMsiVectors	= 32,
	kPldaNumFunctions	= 4,
	kPciNumIntx			= 4,
};


// #pragma mark - JH7110 SoC blocks used by this driver

// Fixed SoC addresses; used when the device tree phandle cannot be
// resolved. Source: Linux arch/riscv/boot/dts/starfive/jh7110.dtsi (v6.12)
enum {
	kJh7110StgCrgBase		= 0x10230000,
	kJh7110StgSysconBase	= 0x10240000,
	kJh7110SysCrgBase		= 0x13020000,
	kJh7110SysGpioBase		= 0x13040000,
	kJh7110PciePhy0Base		= 0x10210000,
	kJh7110PciePhy1Base		= 0x10220000,
	kJh7110Pcie0ApbBase		= 0x2b000000,
	kJh7110Pcie1ApbBase		= 0x2c000000,
	kJh7110BlockSize		= 0x10000,
	kJh7110SysconSize		= 0x1000,
};

// STG syscon (Linux pcie-starfive.c). Per-port base + register offset.
enum {
	kStgPcie0Base			= 0x048,
	kStgPcie1Base			= 0x1f8,

	kStgArOffset			= 0x78,
	kStgAxi4SlvlArMask		= 0x007fff00,	// GENMASK(22, 8)
	kStgAxi4SlvlPhyArShift	= 17,			// FIELD_PREP(GENMASK(20, 17))
	kStgAwOffset			= 0x7c,
	kStgAxi4SlvlAwMask		= 0x00007fff,	// GENMASK(14, 0)
	kStgAxi4SlvlPhyAwShift	= 9,			// FIELD_PREP(GENMASK(12, 9))
	kStgClkReq				= 1 << 22,
	kStgCkRefSrcMask		= 0x000c0000,	// GENMASK(19, 18)
	kStgCkRefSrcShift		= 18,
	kStgRpNepOffset			= 0xe8,
	kStgKRpNep				= 1 << 8,
	kStgLnkStaOffset		= 0x170,
	kStgDataLinkActive		= 1 << 5,

	// PCIe PHY 0 is shared with the USB 3.0 controller. Mode registers
	// (Linux phy-jh7110-pcie.c; offsets from U-Boot STG_SYSCON_328/500 and
	// the "starfive,stg-syscon = <&stg_syscon 0x148 0x1f4>" binding).
	kStgPhy0ModeOffset		= 0x148,
	kStgPciePhyModeMask		= 0x00300000,	// GENMASK(21, 20)
	kStgPhy0UsbOffset		= 0x1f4,
	kStgUsb3BusWidthMask	= 0x0000000c,	// GENMASK(3, 2)
	kStgUsb3BusWidth		= 1 << 3,
	kStgUsb3PhyEnable		= 1 << 4,
};

// PCIe PHY registers (Linux phy-jh7110-pcie.c)
enum {
	kPhyKvcoLevelOff		= 0x28,
	kPhyUsb3PllCtlOff		= 0x7c,
	kPhyKvcoTuneSignalOff	= 0x80,
	kPhyUsb3PhyEnable		= 1 << 4,
	kPhyKvcoFineTuneLevel	= 0x91,
	kPhyKvcoFineTuneSignals	= 0xc,
};

// Clock/reset generators (Linux clk-starfive-jh71x0.{c,h},
// reset-starfive-jh7110.c, dt-bindings starfive,jh7110-crg.h)
enum {
	kCrgClkEnable			= 1u << 31,		// gate bit of each clock register

	kSysClkNocBusStgAxi		= 96,			// JH7110_SYSCLK_NOC_BUS_STG_AXI
	kSysRstAssertOffset		= 0x2f8,
	kSysRstStatusOffset		= 0x308,
	kSysRstNocBusStgAxi		= 30,			// JH7110_SYSRST_NOC_BUS_STG_AXI

	kStgClkPcie0AxiMst0		= 8,
	kStgClkPcie0Apb			= 9,
	kStgClkPcie0Tl			= 10,
	kStgClkPcie1AxiMst0		= 11,
	kStgClkPcie1Apb			= 12,
	kStgClkPcie1Tl			= 13,
	kStgClkPcieSlvMain		= 14,
	kStgRstAssertOffset		= 0x74,
	kStgRstStatusOffset		= 0x78,
	kStgRstPcie0First		= 11,	// AXI_MST0, AXI_SLV0, AXI_SLV, BRG, CORE, APB
	kStgRstPcie1First		= 17,
	kStgRstPcieCount		= 6,
};

// sys pinctrl GPIO output registers (Linux pinctrl-starfive-jh7110{,-sys}.c)
enum {
	kSysGpioDoen			= 0x000,
	kSysGpioDout			= 0x040,
	kSysGpioDoenMask		= 0x3f,
	kSysGpioDoutMask		= 0x7f,
	kGpoutLow				= 0,
	kGpoutHigh				= 1,
	kGpoenEnable			= 0,
	kSysGpioCount			= 64,
	kGpioActiveLow			= 1,	// dt-bindings/gpio/gpio.h
};


// #pragma mark - classes


struct PldaMsiController;


class StarFivePCIeController {
public:
	static	float		SupportsDevice(device_node* parent);
	static	status_t	RegisterDevice(device_node* parent);
	static	status_t	InitDriver(device_node* node,
							StarFivePCIeController*& outDriver);
			void		UninitDriver();

			status_t	ReadConfig(uint8 bus, uint8 device, uint8 function,
							uint16 offset, uint8 size, uint32& value);
			status_t	WriteConfig(uint8 bus, uint8 device, uint8 function,
							uint16 offset, uint8 size, uint32 value);
			status_t	GetMaxBusDevices(int32& count);
			status_t	ReadIrq(uint8 bus, uint8 device, uint8 function,
							uint8 pin, uint8& irq);
			status_t	WriteIrq(uint8 bus, uint8 device, uint8 function,
							uint8 pin, uint8 irq);
			status_t	GetRange(uint32 index, pci_resource_range* range);
			status_t	Finalize();

private:
			status_t	InitDriverInt(device_node* node);
			status_t	ReadResourceInfo();
			status_t	MapRegisters();
			status_t	ResolvePhandleReg(uint32 phandle, uint64 fallbackBase,
							uint64& base);
			void		ParsePerstGpio();

			// bring-up steps
			void		InitPhy();
			void		InitSyscon();
			status_t	EnableClocksAndResets();
			void		InitBridge();
			void		SetupWindows();
			bool		LinkUp();
			void		WaitForLink();
			void		PrintLinkStatus();
			status_t	InitInterrupts();

			void		SetPerst(bool asserted);

			bool		ValidDevice(uint8 bus, uint8 device);
			addr_t		ConfigAddress(uint8 bus, uint8 device, uint8 function,
							uint16 offset);
			void		AssignIntx(uint8 bus, uint32 swizzle, int depth);

	static	int32		InterruptReceived(void* arg);
			int32		InterruptReceivedInt();

			uint32		ApbRead(uint32 offset)
							{ return *(vuint32*)(fApbBase + offset); }
			void		ApbWrite(uint32 offset, uint32 value)
							{ *(vuint32*)(fApbBase + offset) = value; }
			void		SysconUpdate(uint32 offset, uint32 mask, uint32 value);
			uint32		SysconRead(uint32 offset)
							{ return *(vuint32*)(fStgSysconBase + offset); }

private:
			device_node*	fNode {};
			int32			fPort {-1};		// 0 or 1
			uint32			fStgPcieBase {};

			AreaDeleter		fApbArea;
			phys_addr_t		fApbPhys {};
			uint64			fApbSize {};
			addr_t			fApbBase {};

			AreaDeleter		fCfgArea;
			phys_addr_t		fCfgPhys {};
			uint64			fCfgSize {};
			addr_t			fCfgBase {};
			uint32			fCfgBusCount {};

			AreaDeleter		fStgSysconArea;
			addr_t			fStgSysconBase {};
			AreaDeleter		fStgCrgArea;
			addr_t			fStgCrgBase {};
			AreaDeleter		fSysCrgArea;
			addr_t			fSysCrgBase {};
			AreaDeleter		fGpioArea;
			addr_t			fGpioBase {};
			AreaDeleter		fPhyArea;
			addr_t			fPhyBase {};

			int32			fPerstPin {-1};
			bool			fPerstActiveLow {true};

			Vector<pci_resource_range> fResourceRanges;

			uint64			fIrq {};
			int32			fIntxBase {-1};
			bool			fLinkUp {};
			spinlock		fIrqLock = B_SPINLOCK_INITIALIZER;
};


/*! One MSI vector pool shared by both root ports.

	Haiku has a single global MSIInterface. Both PLDA ports decode MSI writes
	the same way (address IMSI_ADDR = 0x190 through the root port BAR0 at
	PCI address 0, data = vector number 0..31), so a device cannot tell which
	port it sits on and does not need to: each port's interrupt handler
	dispatches the bits latched in its own ISTATUS_MSI to the same shared
	vector range.
*/
struct PldaMsiController : public MSIInterface {
public:
	static	PldaMsiController* Get();

			status_t	Init();
			bool		IsReady() const { return fStartVector >= 0; }

			status_t	AllocateVectors(uint32 count, uint32& startVector,
							uint64& address, uint32& data) final;
			void		FreeVectors(uint32 count, uint32 startVector) final;

			int32		Dispatch(uint32 status);

private:
			spinlock	fLock = B_SPINLOCK_INITIALIZER;
			uint32		fAllocated {};
			int32		fStartVector {-1};
};


extern device_manager_info* gDeviceManager;


#endif	// _STARFIVE_PCIE_H_
