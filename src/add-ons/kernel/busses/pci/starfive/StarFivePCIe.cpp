/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * PCIe host controller driver for the StarFive JH7110 SoC (PLDA XpressRICH
 * root ports), for Beacon (a Haiku-derived operating system).
 *
 * Derived from Linux v6.12:
 *   drivers/pci/controller/plda/pcie-starfive.c   (GPL-2.0+)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *   drivers/pci/controller/plda/pcie-plda-host.c  (GPL-2.0)
 *     Copyright (C) 2023 Microchip Co. Ltd
 *                        StarFive Co. Ltd
 *     Author: Daire McNamara <daire.mcnamara@microchip.com>
 *   drivers/phy/starfive/phy-jh7110-pcie.c        (GPL-2.0+)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *     Author: Minda Chen <minda.chen@starfivetech.com>
 *   drivers/reset/starfive/reset-starfive-jh71x0.c (GPL-2.0-or-later)
 *     Copyright (C) 2021 Emil Renner Berthing <kernel@esmil.dk>
 *   drivers/pinctrl/starfive/pinctrl-starfive-jh7110.c (GPL-2.0)
 *     Copyright (C) 2022 Emil Renner Berthing <kernel@esmil.dk>
 *     Copyright (C) 2022 StarFive Technology Co., Ltd.
 * and U-Boot (orangepi-xunlong u-boot-orangepi v2021.10-jh7110,
 * commit d7274dfee9dbcc8b8f39d459d2dcb596abd9e157)
 *   drivers/pci/pcie_starfive.c (GPL-2.0+)
 *     Copyright (c) 2023 Starfive, Inc.
 *     Author: Mason Huo <mason.huo@starfivetech.com>
 *
 * The FDT "ranges" parser, the device manager glue and the MSI dispatch
 * shape are adapted from Haiku's DWPCIController.cpp and
 * MsiInterruptCtrlDW.cpp (Copyright 2022, Haiku, Inc., MIT License; MIT
 * code may be relicensed under the GPL as part of this file).
 *
 * Modified by Beacon contributors.
 * See THIRD_PARTY.md in this directory for exact versions.
 */


#include "StarFivePCIe.h"

#include <bus/FDT.h>
#include <interrupts.h>
#include <KernelExport.h>

#include <AutoDeleterDrivers.h>
#include <util/AutoLock.h>

#include <string.h>
#include <new>


//#define TRACE_STARFIVE_PCIE
#define ERROR(x...) dprintf("starfive_pcie: " x)
#define INFO(x...) dprintf("starfive_pcie: " x)
#ifdef TRACE_STARFIVE_PCIE
#	define TRACE(x...) dprintf("starfive_pcie: " x)
#else
#	define TRACE(x...)
#endif


// Serialises read-modify-write of SoC registers shared by both ports
// (STG syscon, STG/SYS CRG, GPIO).
static spinlock sSharedRegLock = B_SPINLOCK_INITIALIZER;


static inline uint32
ReadReg(addr_t address)
{
	return *(vuint32*)address;
}


static inline void
WriteReg(addr_t address, uint32 value)
{
	*(vuint32*)address = value;
}


static void
UpdateReg(addr_t address, uint32 mask, uint32 value)
{
	InterruptsSpinLocker locker(sSharedRegLock);
	uint32 old = ReadReg(address);
	WriteReg(address, (old & ~mask) | (value & mask));
}


static uint32
ReadBE32(const void* prop, int index)
{
	return B_BENDIAN_TO_HOST_INT32(*((const uint32*)prop + index));
}


//#pragma mark - shared MSI controller


PldaMsiController*
PldaMsiController::Get()
{
	static PldaMsiController sController;
	return &sController;
}


status_t
PldaMsiController::Init()
{
	if (fStartVector >= 0)
		return B_OK;

	int32 start;
	status_t status = allocate_io_interrupt_vectors(kPldaMaxMsiVectors, &start,
		INTERRUPT_TYPE_IRQ);
	if (status != B_OK) {
		ERROR("unable to allocate MSI vectors: %s\n", strerror(status));
		return status;
	}
	if (start + kPldaMaxMsiVectors > 256) {
		// Not fatal for MSI (vectors are 32 bit), only noteworthy.
		INFO("MSI vectors start at %" B_PRId32 "\n", start);
	}

	fStartVector = start;
	msi_set_interface(static_cast<MSIInterface*>(this));
	INFO("MSI: %d vectors at %" B_PRId32 ", doorbell PCI address %#x\n",
		kPldaMaxMsiVectors, start, kPldaImsiAddr);
	return B_OK;
}


status_t
PldaMsiController::AllocateVectors(uint32 count, uint32& startVector,
	uint64& address, uint32& data)
{
	// Multi-message MSI needs a naturally aligned power-of-two block,
	// because the device ORs the message number into the low data bits.
	if (count == 0 || count > kPldaMaxMsiVectors || (count & (count - 1)) != 0)
		return B_BAD_VALUE;

	InterruptsSpinLocker locker(fLock);
	uint32 blockMask = count == 32 ? 0xffffffff : ((1u << count) - 1);
	for (uint32 bit = 0; bit < kPldaMaxMsiVectors; bit += count) {
		uint32 mask = blockMask << bit;
		if ((fAllocated & mask) != 0)
			continue;

		fAllocated |= mask;
		startVector = fStartVector + bit;
		// Linux plda_set_default_msi(): vector_phy = IMSI_ADDR, data = hwirq
		address = kPldaImsiAddr;
		data = bit;
		return B_OK;
	}

	return B_BUSY;
}


void
PldaMsiController::FreeVectors(uint32 count, uint32 startVector)
{
	InterruptsSpinLocker locker(fLock);
	int32 bit = (int32)startVector - fStartVector;
	for (; count > 0; count--, bit++) {
		if (bit >= 0 && bit < kPldaMaxMsiVectors)
			fAllocated &= ~(1u << bit);
	}
}


int32
PldaMsiController::Dispatch(uint32 status)
{
	int32 result = B_UNHANDLED_INTERRUPT;
	for (int32 bit = 0; bit < kPldaMaxMsiVectors; bit++) {
		if ((status & (1u << bit)) == 0)
			continue;
		int32 res = io_interrupt_handler(fStartVector + bit, B_EDGE_TRIGGERED);
		if (res == B_INVOKE_SCHEDULER || result == B_UNHANDLED_INTERRUPT)
			result = res;
	}
	return result;
}


//#pragma mark - driver


float
StarFivePCIeController::SupportsDevice(device_node* parent)
{
	const char* bus;
	status_t status = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS,
		&bus, false);
	if (status < B_OK)
		return -1.0f;

	if (strcmp(bus, "fdt") != 0)
		return 0.0f;

	const char* compatible;
	status = gDeviceManager->get_attr_string(parent, "fdt/compatible",
		&compatible, false);
	if (status < B_OK)
		return -1.0f;

	if (strcmp(compatible, "starfive,jh7110-pcie") != 0)
		return 0.0f;

	// Haiku's FDT bus registers every node, including disabled ones.
	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdtModule,
			(void**)&fdtDev) == B_OK) {
		int propLen;
		const char* okay = (const char*)fdtModule->get_prop(fdtDev, "status",
			&propLen);
		if (okay != NULL && strcmp(okay, "okay") != 0
			&& strcmp(okay, "ok") != 0) {
			return 0.0f;
		}
	}

	return 1.0f;
}


status_t
StarFivePCIeController::RegisterDevice(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "StarFive JH7110 PCIe Host Controller"} },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = "bus_managers/pci/root/driver_v1"} },
		{}
	};

	return gDeviceManager->register_node(parent,
		STARFIVE_PCI_DRIVER_MODULE_NAME, attrs, NULL, NULL);
}


status_t
StarFivePCIeController::InitDriver(device_node* node,
	StarFivePCIeController*& outDriver)
{
	ObjectDeleter<StarFivePCIeController> driver(
		new(std::nothrow) StarFivePCIeController());
	if (!driver.IsSet())
		return B_NO_MEMORY;

	CHECK_RET(driver->InitDriverInt(node));
	outDriver = driver.Detach();
	return B_OK;
}


void
StarFivePCIeController::UninitDriver()
{
	// Interrupts stay masked in hardware once the handler is gone.
	if (fApbBase != 0)
		ApbWrite(kPldaImaskLocal, 0);
	if (fIrq != 0)
		remove_io_interrupt_handler(fIrq, InterruptReceived, this);
	delete this;
}


//#pragma mark - device tree parsing


static int
FindRegIndex(fdt_device_module_info* fdtModule, fdt_device* fdtDev,
	const char* name)
{
	int propLen;
	const char* names = (const char*)fdtModule->get_prop(fdtDev, "reg-names",
		&propLen);
	if (names == NULL)
		return -1;

	int index = 0;
	for (const char* it = names; it < names + propLen;
			it += strlen(it) + 1, index++) {
		if (strcmp(it, name) == 0)
			return index;
	}
	return -1;
}


status_t
StarFivePCIeController::ReadResourceInfo()
{
	DeviceNodePutter<&gDeviceManager> fdtNode(
		gDeviceManager->get_parent_node(fNode));

	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	CHECK_RET(gDeviceManager->get_driver(fdtNode.Get(),
		(driver_module_info**)&fdtModule, (void**)&fdtDev));

	// Two bindings exist in the wild:
	//  upstream Linux: reg-names = "cfg", "apb" (apb 1 MB, cfg 16 MB)
	//  vendor U-Boot/SDK: reg-names = "reg", "config" (16 MB, 256 MB)
	int apbIndex = FindRegIndex(fdtModule, fdtDev, "apb");
	if (apbIndex < 0)
		apbIndex = FindRegIndex(fdtModule, fdtDev, "reg");
	int cfgIndex = FindRegIndex(fdtModule, fdtDev, "cfg");
	if (cfgIndex < 0)
		cfgIndex = FindRegIndex(fdtModule, fdtDev, "config");

	if (apbIndex < 0 || cfgIndex < 0) {
		// No usable names: the config window is the one above 4 GB.
		uint64 base0 = 0, size0 = 0;
		if (!fdtModule->get_reg(fdtDev, 0, &base0, &size0))
			return B_ERROR;
		cfgIndex = base0 >= 0x100000000ULL ? 0 : 1;
		apbIndex = 1 - cfgIndex;
	}

	uint64 base, size;
	if (!fdtModule->get_reg(fdtDev, apbIndex, &base, &size))
		return B_ERROR;
	fApbPhys = base;
	fApbSize = size;
	if (!fdtModule->get_reg(fdtDev, cfgIndex, &base, &size))
		return B_ERROR;
	fCfgPhys = base;
	fCfgSize = size;
	INFO("apb %#" B_PRIx64 " (%#" B_PRIx64 "), cfg %#" B_PRIx64 " (%#"
		B_PRIx64 ")\n", (uint64)fApbPhys, fApbSize, (uint64)fCfgPhys,
		fCfgSize);

	if (fApbSize < 0x2000 || fCfgSize < 0x100000) {
		ERROR("register windows too small\n");
		return B_ERROR;
	}

	// Port number: the STG syscon block differs between the two ports.
	if (fApbPhys == kJh7110Pcie0ApbBase)
		fPort = 0;
	else if (fApbPhys == kJh7110Pcie1ApbBase)
		fPort = 1;
	else {
		int propLen;
		const void* prop = fdtModule->get_prop(fdtDev, "linux,pci-domain",
			&propLen);
		if (prop != NULL && propLen == 4)
			fPort = ReadBE32(prop, 0);
	}
	if (fPort != 0 && fPort != 1) {
		ERROR("cannot tell which root port this is\n");
		return B_ERROR;
	}
	fStgPcieBase = fPort == 0 ? kStgPcie0Base : kStgPcie1Base;

	// Vendor DT carries the syscon offsets in the phandle cells; they must
	// match the Linux constants (AR, AW, RP_NEP).
	{
		int propLen;
		const void* prop = fdtModule->get_prop(fdtDev, "starfive,stg-syscon",
			&propLen);
		if (prop != NULL && propLen >= 16) {
			uint32 ar = ReadBE32(prop, 1), aw = ReadBE32(prop, 2),
				nep = ReadBE32(prop, 3);
			if (ar != fStgPcieBase + kStgArOffset
				|| aw != fStgPcieBase + kStgAwOffset
				|| nep != fStgPcieBase + kStgRpNepOffset) {
				ERROR("warning: DT syscon offsets %#" B_PRIx32 "/%#" B_PRIx32
					"/%#" B_PRIx32 " differ from built-in values\n", ar, aw,
					nep);
			}
		}
	}

	if (!fdtModule->get_interrupt(fdtDev, 0, NULL, &fIrq)) {
		ERROR("no interrupt\n");
		return B_ERROR;
	}
	INFO("port %" B_PRId32 ", PLIC interrupt %" B_PRIu64 "\n", fPort, fIrq);

	// "ranges" (from DWPCIController::ReadResourceInfo, Haiku, MIT)
	int propLen;
	const void* prop = fdtModule->get_prop(fdtDev, "ranges", &propLen);
	if (prop == NULL) {
		ERROR("\"ranges\" property not found\n");
		return B_ERROR;
	}
	for (const uint32* it = (const uint32*)prop;
			(const uint8*)(it + 7) <= (const uint8*)prop + propLen; it += 7) {
		uint32 type = B_BENDIAN_TO_HOST_INT32(*(it + 0));
		uint64 childAdr = ((uint64)ReadBE32(it, 1) << 32) | ReadBE32(it, 2);
		uint64 parentAdr = ((uint64)ReadBE32(it, 3) << 32) | ReadBE32(it, 4);
		uint64 len = ((uint64)ReadBE32(it, 5) << 32) | ReadBE32(it, 6);

		pci_resource_range range = {};
		range.host_address = parentAdr;
		range.pci_address = childAdr;
		range.size = len;
		if ((type & kFdtPciRangePrefetchable) != 0)
			range.address_type |= PCI_address_prefetchable;

		switch (type & kFdtPciRangeTypeMask) {
			case kFdtPciRangeIoPort:
				// The PLDA outbound windows are memory only in the Linux
				// driver; JH7110 DTs declare no I/O range.
				ERROR("ignoring I/O range\n");
				continue;
			case kFdtPciRangeMmio32Bit:
				range.type = B_IO_MEMORY;
				range.address_type |= PCI_address_type_32;
				break;
			case kFdtPciRangeMmio64Bit:
				range.type = B_IO_MEMORY;
				range.address_type |= PCI_address_type_64;
				break;
			default:
				continue;
		}
		fResourceRanges.Add(range);
		INFO("  range %s%s: pci %#" B_PRIx64 " cpu %#" B_PRIx64 " size %#"
			B_PRIx64 "\n",
			(type & kFdtPciRangeTypeMask) == kFdtPciRangeMmio64Bit
				? "MMIO64" : "MMIO32",
			(type & kFdtPciRangePrefetchable) != 0 ? " pref" : "",
			childAdr, parentAdr, len);
	}

	// PHY: optional "phys" phandle, else the fixed PHY of this port.
	uint64 phyBase = fPort == 0 ? kJh7110PciePhy0Base : kJh7110PciePhy1Base;
	prop = fdtModule->get_prop(fdtDev, "phys", &propLen);
	if (prop != NULL && propLen >= 4)
		ResolvePhandleReg(ReadBE32(prop, 0), phyBase, phyBase);

	uint64 sysconBase = kJh7110StgSysconBase;
	prop = fdtModule->get_prop(fdtDev, "starfive,stg-syscon", &propLen);
	if (prop != NULL && propLen >= 4)
		ResolvePhandleReg(ReadBE32(prop, 0), sysconBase, sysconBase);

	CHECK_RET(MapRegisters());

	fPhyArea.SetTo(map_physical_memory("PCIe PHY", phyBase, kJh7110BlockSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fPhyBase));
	CHECK_RET(fPhyArea.Get());
	fStgSysconArea.SetTo(map_physical_memory("PCIe STG syscon", sysconBase,
		kJh7110SysconSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fStgSysconBase));
	CHECK_RET(fStgSysconArea.Get());

	ParsePerstGpio();
	return B_OK;
}


/*!	Tries to get the first "reg" of the node a phandle points to. The node's
	FDT driver may not be initialised yet, in which case the SoC constant in
	\a fallbackBase is kept.
*/
status_t
StarFivePCIeController::ResolvePhandleReg(uint32 phandle, uint64 fallbackBase,
	uint64& base)
{
	base = fallbackBase;

	DeviceNodePutter<&gDeviceManager> fdtNode(
		gDeviceManager->get_parent_node(fNode));
	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	CHECK_RET(gDeviceManager->get_driver(fdtNode.Get(),
		(driver_module_info**)&fdtModule, (void**)&fdtDev));

	device_node* busNode = fdtModule->get_bus(fdtDev);
	fdt_bus_module_info* busModule;
	struct fdt_bus* bus;
	CHECK_RET(gDeviceManager->get_driver(busNode,
		(driver_module_info**)&busModule, (void**)&bus));

	device_node* target = busModule->node_by_phandle(bus, phandle);
	if (target == NULL)
		return B_ENTRY_NOT_FOUND;

	fdt_device_module_info* targetModule;
	fdt_device* targetDev;
	if (gDeviceManager->get_driver(target, (driver_module_info**)&targetModule,
			(void**)&targetDev) != B_OK) {
		TRACE("phandle %#" B_PRIx32 ": driver not ready, using %#" B_PRIx64
			"\n", phandle, fallbackBase);
		return B_NO_INIT;
	}

	uint64 regBase, regSize;
	if (!targetModule->get_reg(targetDev, 0, &regBase, &regSize))
		return B_ERROR;
	if (regBase != fallbackBase) {
		ERROR("warning: phandle %#" B_PRIx32 " reg %#" B_PRIx64
			" differs from JH7110 constant %#" B_PRIx64 "\n", phandle, regBase,
			fallbackBase);
	}
	base = regBase;
	return B_OK;
}


void
StarFivePCIeController::ParsePerstGpio()
{
	DeviceNodePutter<&gDeviceManager> fdtNode(
		gDeviceManager->get_parent_node(fNode));
	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	if (gDeviceManager->get_driver(fdtNode.Get(),
			(driver_module_info**)&fdtModule, (void**)&fdtDev) != B_OK) {
		return;
	}

	// Upstream Linux binding: perst-gpios = <&sysgpio 26 GPIO_ACTIVE_LOW>;
	// older drafts used reset-gpios. The vendor DT drives PERST# through
	// pinctrl states instead, so fall back to the VisionFive 2 / Orange Pi
	// RV pins (GPIO 26 for port 0, GPIO 28 for port 1, both active low).
	int propLen;
	const void* prop = fdtModule->get_prop(fdtDev, "perst-gpios", &propLen);
	if (prop == NULL)
		prop = fdtModule->get_prop(fdtDev, "reset-gpios", &propLen);

	if (prop != NULL && propLen >= 12) {
		uint64 gpioBase;
		ResolvePhandleReg(ReadBE32(prop, 0), kJh7110SysGpioBase, gpioBase);
		if (gpioBase != kJh7110SysGpioBase) {
			ERROR("PERST# GPIO is not on the sys pinctrl; not driving it\n");
			return;
		}
		fPerstPin = ReadBE32(prop, 1);
		fPerstActiveLow = (ReadBE32(prop, 2) & kGpioActiveLow) != 0;
	} else {
		fPerstPin = fPort == 0 ? 26 : 28;
		fPerstActiveLow = true;
		INFO("no perst-gpios in DT, assuming GPIO %" B_PRId32 "\n", fPerstPin);
	}

	if (fPerstPin < 0 || fPerstPin >= kSysGpioCount) {
		fPerstPin = -1;
		return;
	}

	fGpioArea.SetTo(map_physical_memory("PCIe PERST GPIO", kJh7110SysGpioBase,
		kJh7110BlockSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fGpioBase));
	if (fGpioArea.Get() < B_OK)
		fPerstPin = -1;
}


status_t
StarFivePCIeController::MapRegisters()
{
	fApbArea.SetTo(map_physical_memory("PCIe PLDA bridge", fApbPhys, fApbSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fApbBase));
	CHECK_RET(fApbArea.Get());

	fCfgArea.SetTo(map_physical_memory("PCIe config", fCfgPhys, fCfgSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fCfgBase));
	CHECK_RET(fCfgArea.Get());
	fCfgBusCount = fCfgSize >> 20;
	if (fCfgBusCount > 256)
		fCfgBusCount = 256;

	fStgCrgArea.SetTo(map_physical_memory("PCIe STG CRG", kJh7110StgCrgBase,
		kJh7110BlockSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fStgCrgBase));
	CHECK_RET(fStgCrgArea.Get());

	fSysCrgArea.SetTo(map_physical_memory("PCIe SYS CRG", kJh7110SysCrgBase,
		kJh7110BlockSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fSysCrgBase));
	CHECK_RET(fSysCrgArea.Get());

	return B_OK;
}


//#pragma mark - bring-up


void
StarFivePCIeController::SysconUpdate(uint32 offset, uint32 mask, uint32 value)
{
	UpdateReg(fStgSysconBase + offset, mask, value);
}


void
StarFivePCIeController::SetPerst(bool asserted)
{
	if (fPerstPin < 0)
		return;

	bool high = fPerstActiveLow ? !asserted : asserted;
	uint32 offset = 4 * (fPerstPin / 4);
	uint32 shift = 8 * (fPerstPin % 4);

	// Linux jh7110_set_gpiomux(): output value first, then output enable.
	UpdateReg(fGpioBase + kSysGpioDout + offset, kSysGpioDoutMask << shift,
		(high ? kGpoutHigh : kGpoutLow) << shift);
	UpdateReg(fGpioBase + kSysGpioDoen + offset, kSysGpioDoenMask << shift,
		kGpoenEnable << shift);
	TRACE("PERST# %s (GPIO %" B_PRId32 " %s)\n",
		asserted ? "asserted" : "released", fPerstPin, high ? "high" : "low");
}


void
StarFivePCIeController::InitPhy()
{
	// PCIe PHY 0 can be switched to USB 3.0 mode. If firmware left it there,
	// put it back into PCIe mode (Linux phy_pcie_mode_set(), without the
	// sys syscon USB_PDRSTN_SPLIT part, which belongs to the USB 2.0 PHY).
	if (fPort == 0) {
		uint32 mode = SysconRead(kStgPhy0ModeOffset);
		if ((mode & kStgPciePhyModeMask) != 0) {
			INFO("PHY0 was in USB 3.0 mode (%#" B_PRIx32 "), switching to "
				"PCIe\n", mode);
			SysconUpdate(kStgPhy0ModeOffset, kStgPciePhyModeMask, 0);
			SysconUpdate(kStgPhy0UsbOffset, kStgUsb3BusWidthMask,
				kStgUsb3BusWidth);
			SysconUpdate(kStgPhy0UsbOffset, kStgUsb3PhyEnable, 0);
			uint32 pll = ReadReg(fPhyBase + kPhyUsb3PllCtlOff);
			WriteReg(fPhyBase + kPhyUsb3PllCtlOff, pll & ~kPhyUsb3PhyEnable);
		}
	}

	// jh7110_pcie_phy_probe(): PLL KVCO gain fine tuning
	WriteReg(fPhyBase + kPhyKvcoLevelOff, kPhyKvcoFineTuneLevel);
	WriteReg(fPhyBase + kPhyKvcoTuneSignalOff, kPhyKvcoFineTuneSignals);
}


void
StarFivePCIeController::InitSyscon()
{
	// starfive_pcie_host_init()
	SysconUpdate(fStgPcieBase + kStgRpNepOffset, kStgKRpNep, kStgKRpNep);
	SysconUpdate(fStgPcieBase + kStgAwOffset, kStgCkRefSrcMask,
		2 << kStgCkRefSrcShift);
	SysconUpdate(fStgPcieBase + kStgAwOffset, kStgClkReq, kStgClkReq);
}


static status_t
EnableClock(addr_t crgBase, uint32 index, const char* name)
{
	addr_t reg = crgBase + 4 * index;
	if ((ReadReg(reg) & kCrgClkEnable) == 0) {
		INFO("enabling clock %s\n", name);
		UpdateReg(reg, kCrgClkEnable, kCrgClkEnable);
	}
	return (ReadReg(reg) & kCrgClkEnable) != 0 ? B_OK : B_ERROR;
}


/*!	reset-starfive-jh71x0.c: a set bit in the assert register holds the
	reset; the status register bit reads 1 once the block is out of reset.
*/
static status_t
DeassertReset(addr_t crgBase, uint32 assertOffset, uint32 statusOffset,
	uint32 id, const char* name)
{
	addr_t assertReg = crgBase + assertOffset + 4 * (id / 32);
	addr_t statusReg = crgBase + statusOffset + 4 * (id / 32);
	uint32 mask = 1u << (id % 32);

	if ((ReadReg(assertReg) & mask) != 0) {
		INFO("deasserting reset %s\n", name);
		UpdateReg(assertReg, mask, 0);
	}

	for (int i = 0; i < 1000; i++) {
		if ((ReadReg(statusReg) & mask) != 0)
			return B_OK;
		spin(1);
	}
	ERROR("reset %s did not deassert (clock gated?)\n", name);
	return B_TIMED_OUT;
}


status_t
StarFivePCIeController::EnableClocksAndResets()
{
	// Clocks named in the DT "clocks" property, plus the critical parents
	// Linux keeps on (CLK_IS_CRITICAL).
	EnableClock(fSysCrgBase, kSysClkNocBusStgAxi, "noc_bus_stg_axi");
	EnableClock(fStgCrgBase, kStgClkPcieSlvMain, "pcie_slv_main");

	uint32 first = fPort == 0 ? kStgClkPcie0AxiMst0 : kStgClkPcie1AxiMst0;
	CHECK_RET(EnableClock(fStgCrgBase, first + 0, "pcie_axi_mst0"));
	CHECK_RET(EnableClock(fStgCrgBase, first + 1, "pcie_apb"));
	CHECK_RET(EnableClock(fStgCrgBase, first + 2, "pcie_tl"));

	DeassertReset(fSysCrgBase, kSysRstAssertOffset, kSysRstStatusOffset,
		kSysRstNocBusStgAxi, "noc_bus_stg_axi");

	static const char* const kResetNames[kStgRstPcieCount] = {
		"axi_mst0", "axi_slv0", "axi_slv", "brg", "core", "apb"
	};
	uint32 rst = fPort == 0 ? kStgRstPcie0First : kStgRstPcie1First;
	for (uint32 i = 0; i < kStgRstPcieCount; i++) {
		CHECK_RET(DeassertReset(fStgCrgBase, kStgRstAssertOffset,
			kStgRstStatusOffset, rst + i, kResetNames[i]));
	}
	return B_OK;
}


void
StarFivePCIeController::InitBridge()
{
	// Disable physical functions except #0 (starfive_pcie_host_init())
	for (uint32 i = 1; i < kPldaNumFunctions; i++) {
		SysconUpdate(fStgPcieBase + kStgArOffset, kStgAxi4SlvlArMask,
			i << kStgAxi4SlvlPhyArShift);
		SysconUpdate(fStgPcieBase + kStgAwOffset, kStgAxi4SlvlAwMask,
			i << kStgAxi4SlvlPhyAwShift);
		ApbWrite(kPldaPciMisc, ApbRead(kPldaPciMisc) | kPldaPhyFunctionDis);
	}
	SysconUpdate(fStgPcieBase + kStgArOffset, kStgAxi4SlvlArMask, 0);
	SysconUpdate(fStgPcieBase + kStgAwOffset, kStgAxi4SlvlAwMask, 0);

	// plda_pcie_enable_root_port()
	ApbWrite(kPldaGenSettings, ApbRead(kPldaGenSettings) | kPldaRpEnable);

	// plda_pcie_write_rc_bar(0): BAR0 at PCI address 0 makes the bridge
	// registers (incl. IMSI_ADDR, the MSI doorbell) visible to devices.
	ApbWrite(kPldaConfigSpaceOffset + PCI_base_registers, 0);
	ApbWrite(kPldaConfigSpaceOffset + PCI_base_registers + 4, 0);

	// plda_pcie_set_standard_class(): PCI-to-PCI bridge, keep revision
	uint32 ids = ApbRead(kPldaPciIdsDw1) & kPldaRevisionIdMask;
	ids |= 0x0604u << kPldaIdsClassCodeShift;
	ApbWrite(kPldaPciIdsDw1, ids);

	// plda_pcie_disable_ltr(): LTR forwarding hangs the system otherwise
	ApbWrite(kPldaPmsgSupportRx,
		ApbRead(kPldaPmsgSupportRx) & ~kPldaPmsgLtrSupport);

	// plda_pcie_set_pref_win_64bit()
	ApbWrite(kPldaWinRom, ApbRead(kPldaWinRom) | kPldaPrefMemWin64Support);
}


static uint32
Log2(uint64 value)
{
	uint32 result = 0;
	while (value > 1) {
		value >>= 1;
		result++;
	}
	return result;
}


/*!	plda_pcie_setup_window(): outbound AXI -> PCIe translation entry. Entry
	0 carries config requests, the rest memory requests. The window size is
	encoded as 2^(n + 1) bytes.
*/
static void
SetupWindow(addr_t apb, uint32 index, uint64 axiAddr, uint64 pciAddr,
	uint64 size)
{
	uint32 atrSize = Log2(size) - 1;
	addr_t entry = apb + index * kPldaAtrEntrySize;

	WriteReg(entry + kPldaAtr0Axi4Slv0TrslParam,
		index == 0 ? kPldaTrslIdPcieConfig : kPldaTrslIdPcieTxRx);
	WriteReg(entry + kPldaAtr0Axi4Slv0SrcAddrParam,
		(uint32)axiAddr | (atrSize << kPldaAtrSizeShift) | kPldaAtrImplEnable);
	WriteReg(entry + kPldaAtr0Axi4Slv0SrcAddr, (uint32)(axiAddr >> 32));
	WriteReg(entry + kPldaAtr0Axi4Slv0TrslAddrLsb, (uint32)pciAddr);
	WriteReg(entry + kPldaAtr0Axi4Slv0TrslAddrUdw, (uint32)(pciAddr >> 32));

	// Inbound window 0: identity map 2^38 bytes of PCI space to AXI (DMA)
	uint32 val = ReadReg(apb + kPldaAtr0PcieWin0SrcAddrParam);
	val |= kPldaAtr0PcieAtrSize << kPldaAtr0PcieAtrSizeShift;
	WriteReg(apb + kPldaAtr0PcieWin0SrcAddrParam, val);
	WriteReg(apb + kPldaAtr0PcieWin0SrcAddr, 0);

	INFO("  ATR %" B_PRIu32 ": cpu %#" B_PRIx64 " -> pci %#" B_PRIx64
		" size 2^%" B_PRIu32 " (%s)\n", index, axiAddr, pciAddr, atrSize + 1,
		index == 0 ? "config" : "mem");
}


void
StarFivePCIeController::SetupWindows()
{
	// U-Boot: "the two host bridges have the same default address
	// translation table", so the config entry must always be written.
	SetupWindow(fApbBase, 0, fCfgPhys, 0, fCfgSize);

	uint32 index = 1;
	for (int32 i = 0; i < fResourceRanges.Count(); i++) {
		const pci_resource_range& range = fResourceRanges[i];
		if (range.type != B_IO_MEMORY)
			continue;
		if (index >= kPldaAtrMaxEntries) {
			ERROR("too many ranges for the ATR\n");
			break;
		}
		if ((range.size & (range.size - 1)) != 0)
			ERROR("warning: range size %#" B_PRIx64 " not a power of two\n",
				range.size);
		SetupWindow(fApbBase, index++, range.host_address, range.pci_address,
			range.size);
	}
}


bool
StarFivePCIeController::LinkUp()
{
	return (SysconRead(fStgPcieBase + kStgLnkStaOffset)
		& kStgDataLinkActive) != 0;
}


void
StarFivePCIeController::WaitForLink()
{
	// starfive_pcie_host_wait_for_link(): 10 tries, ~100 ms apart
	for (int i = 0; i < 10; i++) {
		if (LinkUp()) {
			fLinkUp = true;
			INFO("port %" B_PRId32 " link up\n", fPort);
			return;
		}
		snooze(100000);
	}
	INFO("port %" B_PRId32 " link down (LNKSTA %#" B_PRIx32 ")\n", fPort,
		SysconRead(fStgPcieBase + kStgLnkStaOffset));
}


void
StarFivePCIeController::PrintLinkStatus()
{
	uint32 value;
	if (ReadConfig(0, 0, 0, PCI_vendor_id, 4, value) != B_OK)
		return;
	INFO("root port %04" B_PRIx32 ":%04" B_PRIx32 "\n", value & 0xffff,
		value >> 16);

	uint32 cap = 0;
	ReadConfig(0, 0, 0, PCI_capabilities_ptr, 1, cap);
	for (int guard = 0; cap >= 0x40 && cap < 0x100 && guard < 48; guard++) {
		uint32 header = 0;
		ReadConfig(0, 0, 0, cap, 2, header);
		if ((header & 0xff) == PCI_cap_id_pcie) {
			uint32 linkCap = 0, linkStatus = 0;
			ReadConfig(0, 0, 0, cap + 0x0c, 4, linkCap);
			ReadConfig(0, 0, 0, cap + 0x12, 2, linkStatus);
			INFO("link: max Gen%" B_PRIu32 " x%" B_PRIu32 ", current Gen%"
				B_PRIu32 " x%" B_PRIu32 "\n", linkCap & 0xf,
				(linkCap >> 4) & 0x3f, linkStatus & 0xf,
				(linkStatus >> 4) & 0x3f);
			return;
		}
		cap = (header >> 8) & 0xfc;
	}
}


status_t
StarFivePCIeController::InitInterrupts()
{
	// The riscv64 arch code reserves all 1024 PLIC sources (Beacon change in
	// arch_int.cpp), so dynamic INTx/MSI vectors can't alias a real source.

	ApbWrite(kPldaImaskLocal, 0);
	ApbWrite(kPldaIstatusLocal, 0xffffffff);
	ApbWrite(kPldaIstatusMsi, 0xffffffff);
	ApbWrite(kPldaImaskHost, 0);
	ApbWrite(kPldaIstatusHost, 0xffffffff);

	status_t status = allocate_io_interrupt_vectors(kPciNumIntx, &fIntxBase,
		INTERRUPT_TYPE_IRQ);
	if (status != B_OK) {
		ERROR("cannot allocate INTx vectors\n");
		fIntxBase = -1;
	} else if (fIntxBase + kPciNumIntx > 255) {
		// PCI_interrupt_line is 8 bits wide; drivers read it back as IRQ.
		ERROR("INTx vectors %" B_PRId32 "+ do not fit the 8-bit interrupt "
			"line; INTx disabled\n", fIntxBase);
		free_io_interrupt_vectors(kPciNumIntx, fIntxBase);
		fIntxBase = -1;
	} else {
		INFO("INTA..INTD -> vectors %" B_PRId32 "..%" B_PRId32 "\n",
			fIntxBase, fIntxBase + kPciNumIntx - 1);
	}

	bool msiOk = PldaMsiController::Get()->Init() == B_OK;

	CHECK_RET(install_io_interrupt_handler(fIrq, InterruptReceived, this, 0));

	uint32 mask = kPldaIntErrorMask;
	if (fIntxBase >= 0)
		mask |= kPldaIntIntxMask;
	if (msiOk)
		mask |= kPldaIntMsi;
	ApbWrite(kPldaImaskLocal, mask);
	return B_OK;
}


status_t
StarFivePCIeController::InitDriverInt(device_node* node)
{
	fNode = node;
	INFO("init\n");

	CHECK_RET(ReadResourceInfo());

	if (LinkUp())
		INFO("link already up from firmware; re-initialising\n");

	// Linux requests PERST# as GPIOD_OUT_HIGH (asserted) in parse_dt.
	SetPerst(true);

	InitPhy();
	InitSyscon();
	status_t status = EnableClocksAndResets();
	if (status != B_OK) {
		ERROR("clock/reset bring-up failed\n");
		return status;
	}

	InitBridge();

	// T_PVPERL: PERST# asserted >= 100 ms, then 100 ms before config cycles
	snooze(100000);
	SetPerst(false);
	snooze(100000);
	WaitForLink();

	SetupWindows();

	// Beacon: on a cold start (firmware never enumerated PCIe) the root
	// port's bus numbers are still 0, and Haiku's PCI bus manager then
	// recurses into "bus 0 behind bus 0". Give the root port bus 1 below it
	// (the PLDA root port only ever has one device behind it).
	{
		vuint32* busNumbers = (vuint32*)(fCfgBase + PCI_primary_bus);
		uint32 value = *busNumbers;
		if (((value >> 8) & 0xff) == 0) {
			*busNumbers = (value & 0xff000000) | (1 << 16) | (1 << 8) | 0;
			INFO("root port bus numbers set: primary 0, secondary 1, "
				"subordinate 1 (was %#" B_PRIx32 ")\n", value);
		}

		// Cold start: firmware also left the root port's memory window
		// empty, and Haiku only allocates BARs behind a bridge from inside
		// its window. Open it over the 32-bit MMIO range from the DT and
		// close the prefetchable window (base > limit).
		vuint32* memWindow = (vuint32*)(fCfgBase + PCI_memory_base);
		if ((*memWindow & 0xffff) == 0 && ((*memWindow >> 16) & 0xffff) == 0) {
			for (int32 i = 0; i < fResourceRanges.Count(); i++) {
				const pci_resource_range& range = fResourceRanges[i];
				if ((range.address_type & PCI_address_type) != PCI_address_type_32
					|| (range.address_type & PCI_address_prefetchable) != 0)
					continue;
				uint64 base = range.pci_address;
				uint64 limit = range.pci_address + range.size - 1;
				*memWindow = (uint32)((base >> 16) & 0xfff0)
					| ((uint32)((limit >> 16) & 0xfff0) << 16);
				*(vuint32*)(fCfgBase + PCI_prefetchable_memory_base)
					= 0x0000fff0;
				*(vuint32*)(fCfgBase + PCI_prefetchable_memory_base_upper32) = 0;
				*(vuint32*)(fCfgBase + PCI_prefetchable_memory_limit_upper32) = 0;
				INFO("root port memory window set: %#" B_PRIx64 "-%#" B_PRIx64
					"\n", base, limit);
				break;
			}
		}
	}

	CHECK_RET(InitInterrupts());
	PrintLinkStatus();

	INFO("init done\n");
	return B_OK;
}


//#pragma mark - config space


bool
StarFivePCIeController::ValidDevice(uint8 bus, uint8 device)
{
	if (bus >= fCfgBusCount)
		return false;
	if (bus == 0)
		return device == 0;
	if (!fLinkUp)
		return false;

	// Only one device below the root port (U-Boot
	// starfive_pcie_addr_valid(), Linux only_one_child()).
	uint8 secondary = *(vuint8*)(fCfgBase + PCI_secondary_bus);
	if (secondary == 0)
		secondary = 1;	// not programmed yet
	if (bus == secondary)
		return device == 0;
	return true;
}


addr_t
StarFivePCIeController::ConfigAddress(uint8 bus, uint8 device, uint8 function,
	uint16 offset)
{
	// PCIE_ECAM_OFFSET(): bus << 20 | devfn << 12 | where
	return fCfgBase + ((addr_t)bus << 20) + ((addr_t)device << 15)
		+ ((addr_t)function << 12) + offset;
}


status_t
StarFivePCIeController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	if (offset >= 4096 || device >= 32 || function >= 8
		|| (offset & (size - 1)) != 0) {
		return B_BAD_VALUE;
	}

	if (!ValidDevice(bus, device)) {
		value = size == 4 ? 0xffffffff : (1u << (size * 8)) - 1;
		return B_OK;
	}

	// starfive_pcie_hide_rc_bar(): root port BAR0/1 map the bridge registers
	if (bus == 0 && device == 0 && function == 0
		&& offset >= PCI_base_registers && offset < PCI_base_registers + 8) {
		value = 0;
		return B_OK;
	}

	addr_t address = ConfigAddress(bus, device, function, offset);
	switch (size) {
		case 1: value = *(vuint8*)address; break;
		case 2: value = *(vuint16*)address; break;
		case 4: value = *(vuint32*)address; break;
		default:
			return B_BAD_VALUE;
	}
	return B_OK;
}


status_t
StarFivePCIeController::WriteConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32 value)
{
	if (offset >= 4096 || device >= 32 || function >= 8
		|| (offset & (size - 1)) != 0) {
		return B_BAD_VALUE;
	}

	if (!ValidDevice(bus, device))
		return B_OK;

	if (bus == 0 && device == 0 && function == 0
		&& offset >= PCI_base_registers && offset < PCI_base_registers + 8) {
		return B_OK;
	}

	addr_t address = ConfigAddress(bus, device, function, offset);
	switch (size) {
		case 1: *(vuint8*)address = value; break;
		case 2: *(vuint16*)address = value; break;
		case 4: *(vuint32*)address = value; break;
		default:
			return B_BAD_VALUE;
	}
	return B_OK;
}


status_t
StarFivePCIeController::GetMaxBusDevices(int32& count)
{
	count = 32;
	return B_OK;
}


status_t
StarFivePCIeController::ReadIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8& irq)
{
	return B_UNSUPPORTED;
}


status_t
StarFivePCIeController::WriteIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8 irq)
{
	return B_UNSUPPORTED;
}


status_t
StarFivePCIeController::GetRange(uint32 index, pci_resource_range* range)
{
	if (index >= (uint32)fResourceRanges.Count())
		return B_BAD_INDEX;

	*range = fResourceRanges[index];
	return B_OK;
}


//#pragma mark - INTx routing


/*!	Writes the Haiku vector of each device's INTx pin into its
	PCI_interrupt_line register; the PCI bus manager reads it back right after
	finalize() (_RefreshDeviceInfo) and drivers use it as their IRQ.

	The DT interrupt-map (mask 0 0 0 7) maps pin N of the root port slot to
	PLDA INTx N. The upstream DT targets the PCIe-local intc node; the vendor
	DT wrongly targets PLIC 1..4, so it is not used. Devices below bridges are
	swizzled the standard way: pin' = ((pin - 1 + device) % 4) + 1 per hop.
*/
void
StarFivePCIeController::AssignIntx(uint8 bus, uint32 swizzle, int depth)
{
	if (depth > 8)
		return;

	for (uint8 device = 0; device < 32; device++) {
		if (!ValidDevice(bus, device))
			continue;
		uint32 vendor;
		ReadConfig(bus, device, 0, PCI_vendor_id, 2, vendor);
		if (vendor == 0xffff || vendor == 0)
			continue;

		uint32 headerType;
		ReadConfig(bus, device, 0, PCI_header_type, 1, headerType);
		int functions = (headerType & PCI_multifunction) != 0 ? 8 : 1;

		for (uint8 function = 0; function < functions; function++) {
			ReadConfig(bus, device, function, PCI_vendor_id, 2, vendor);
			if (vendor == 0xffff || vendor == 0)
				continue;

			uint32 pin;
			ReadConfig(bus, device, function, PCI_interrupt_pin, 1, pin);
			// The root port itself is on the root bus: no swizzle.
			uint32 hop = bus == 0 ? 0 : device;
			if (pin >= 1 && pin <= 4 && fIntxBase >= 0) {
				uint32 rootPin = ((pin - 1 + swizzle + hop) % 4) + 1;
				uint32 vector = fIntxBase + rootPin - 1;
				WriteConfig(bus, device, function, PCI_interrupt_line, 1,
					vector);
				INFO("%u:%u.%u INT%c -> INT%c, vector %" B_PRIu32 "\n", bus,
					device, function, (char)('A' + pin - 1),
					(char)('A' + rootPin - 1), vector);
			}

			uint32 type;
			ReadConfig(bus, device, function, PCI_header_type, 1, type);
			if ((type & PCI_header_type_mask)
					== PCI_header_type_PCI_to_PCI_bridge) {
				uint32 secondary;
				ReadConfig(bus, device, function, PCI_secondary_bus, 1,
					secondary);
				if (secondary > bus && secondary < fCfgBusCount)
					AssignIntx(secondary, swizzle + hop, depth + 1);
			}
		}
	}
}


status_t
StarFivePCIeController::Finalize()
{
	AssignIntx(0, 0, 0);
	return B_OK;
}


//#pragma mark - interrupt handling


int32
StarFivePCIeController::InterruptReceived(void* arg)
{
	return static_cast<StarFivePCIeController*>(arg)->InterruptReceivedInt();
}


static inline void
MergeResult(int32& result, int32 res)
{
	if (res == B_INVOKE_SCHEDULER || result == B_UNHANDLED_INTERRUPT)
		result = res;
}


int32
StarFivePCIeController::InterruptReceivedInt()
{
	SpinLocker locker(fIrqLock);
	int32 result = B_UNHANDLED_INTERRUPT;

	uint32 status = ApbRead(kPldaIstatusLocal) & ApbRead(kPldaImaskLocal);
	if (status == 0)
		return result;

	// plda_handle_intx(): ack each pin, then run the handlers
	if ((status & kPldaIntIntxMask) != 0 && fIntxBase >= 0) {
		uint32 intx = (status & kPldaIntIntxMask) >> kPldaIntIntxShift;
		for (int32 pin = 0; pin < kPciNumIntx; pin++) {
			if ((intx & (1u << pin)) == 0)
				continue;
			ApbWrite(kPldaIstatusLocal, 1u << (pin + kPldaIntIntxShift));
			MergeResult(result,
				io_interrupt_handler(fIntxBase + pin, B_LEVEL_TRIGGERED));
		}
	}

	// plda_handle_msi(): clear the summary bit, then ack + dispatch vectors
	if ((status & kPldaIntMsi) != 0) {
		ApbWrite(kPldaIstatusLocal, kPldaIntMsi);
		uint32 msi = ApbRead(kPldaIstatusMsi);
		ApbWrite(kPldaIstatusMsi, msi);
		if (msi != 0)
			MergeResult(result, PldaMsiController::Get()->Dispatch(msi));
		else
			MergeResult(result, B_HANDLED_INTERRUPT);
	}

	uint32 errors = status & kPldaIntErrorMask;
	if (errors != 0) {
		ApbWrite(kPldaIstatusLocal, errors);
		static int32 sReported = 0;
		if (sReported < 16) {
			sReported++;
			ERROR("port %" B_PRId32 " bridge error event %#" B_PRIx32 "\n",
				fPort, errors);
		}
		MergeResult(result, B_HANDLED_INTERRUPT);
	}

	return result;
}
