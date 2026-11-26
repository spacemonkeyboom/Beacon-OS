/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * StarFive JH7110 / JH7110S glue for the on-chip Cadence USB 3 controller
 * (cdns3) in host mode, for Beacon (a Haiku-derived operating system).
 *
 * This add-on powers and configures the controller and its PHYs, switches
 * the cdns3 dual-role logic to host, and then publishes a "xhci_platform"
 * child node. Haiku's (MIT) xhci add-on attaches to that node and drives the
 * xHCI registers; no xHCI code is in this file.
 *
 * Derived from Linux (torvalds/linux 25faec9ce61c, 2026-11-25):
 *   drivers/usb/cdns3/cdns3-starfive.c (GPL-2.0)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *     Author: Minda Chen <minda.chen@starfivetech.com>
 *   drivers/usb/cdns3/drd.c, drd.h (GPL-2.0)
 *     Copyright (C) 2018-2020 Cadence. Author: Pawel Laszczak
 *   drivers/phy/starfive/phy-jh7110-usb.c (GPL-2.0+)
 *   drivers/phy/starfive/phy-jh7110-pcie.c (GPL-2.0+)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *   drivers/clk/starfive/clk-starfive-jh7110-pll.c (GPL-2.0)
 *     Copyright (C) 2023 StarFive Technology Co., Ltd.
 *   drivers/reset/starfive/reset-starfive-jh71x0.c (GPL-2.0-or-later)
 *     Copyright (C) 2021 Emil Renner Berthing
 * Device tree facts from Linux arch/riscv/boot/dts/starfive/jh7110*.dts* and
 * the StarFive SDK U-Boot (starfive-tech/u-boot JH7110_VisionFive2_devel
 * c4c67bb66ae6) arch/riscv/dts/jh7110.dtsi, starfive_visionfive2_lite.dts.
 * Module glue shape from Beacon's dw_mmc kernel_interface.cpp (MIT).
 * See THIRD_PARTY.md.
 */


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include "Jh7110Gpio.h"


#define CDNS3_JH7110_MODULE_NAME "busses/usb/cdns3_jh7110/driver_v1"
#define XHCI_PLATFORM_MODULE_NAME "busses/usb/xhci/platform/driver_v1"

#define INFO(x...) dprintf("cdns3_jh7110: " x)
#define ERROR(x...) dprintf("cdns3_jh7110: " x)


static device_manager_info* gDeviceManager;


enum {
	// Blocks (Linux jh7110.dtsi)
	kOtgPhys			= 0x10100000,	// cdns3 "otg"
	kXhciPhys			= 0x10110000,	// cdns3 "xhci"
	kXhciSize			= 0x10000,
	kUsb2PhyPhys		= 0x10200000,	// usbphy0
	kPciePhy0Phys		= 0x10210000,	// pciephy0, the USB 3 PHY
	kStgCrgPhys			= 0x10230000,
	kStgSysconPhys		= 0x10240000,
	kSysCrgPhys			= 0x13020000,
	kSysSysconPhys		= 0x13030000,
	kHostIrq			= 100,			// cdns3 "host" interrupt

	// CRG: one register per clock, bit 31 = gate, [23:0] = divider
	kCrgEnable			= 1u << 31,
	kCrgDivMask			= 0x00ffffff,
	kSysClkUsb125m		= 95,			// JH7110_SYSCLK_USB_125M (PLL0 / div)
	kStgClkUsb0Apb		= 1,
	kStgClkUsb0UtmiApb	= 2,
	kStgClkUsb0Axi		= 3,
	kStgClkUsb0Lpm		= 4,
	kStgClkUsb0Stb		= 5,
	kStgClkUsb0App125	= 6,
	kStgClkPcie0Apb		= 9,
	// STG resets
	kStgRstAssert		= 0x74,
	kStgRstStatus		= 0x78,
	kStgRstUsb0Axi		= 7,
	kStgRstUsb0Apb		= 8,
	kStgRstUsb0UtmiApb	= 9,
	kStgRstUsb0Pwrup	= 10,
	kStgRstPcie0Apb		= 16,

	// STG syscon: USB mode word (stg-syscon arg 0x4), cdns3-starfive.c
	kStgUsbMode			= 0x4,
	kUsbStrapHost		= 1u << 17,
	kUsbStrapMask		= 7u << 16,
	kUsbSuspendmHost	= 1u << 19,
	kUsbSuspendmMask	= 1u << 19,
	kUsbMiscCfgMask		= 0xfu << 20,
	kUsbSuspendmByps	= 1u << 20,
	kUsbPllEn			= 1u << 22,
	kUsbRefclkMode		= 1u << 23,
	// STG syscon: PCIe/USB 3 PHY words (pciephy0 stg-syscon 0x148 0x1f4)
	kStgPhy0Mode		= 0x148,
	kPciePhyModeMask	= 3u << 20,
	kPciePhyModeUsb		= 1u << 20,
	kStgPhy0Usb			= 0x1f4,
	kUsb3BusWidthMask	= 3u << 2,
	kUsb3PhyEnable		= 1u << 4,
	// SYS syscon
	kSysUsbSplit		= 0x18,
	kUsbPdrstnSplit		= 1u << 17,
	kPll0Pd				= 0x18,
	kPll0Fbdiv			= 0x1c,
	kPll0Frac			= 0x20,
	kPll0Prediv			= 0x24,

	// USB 2 PHY registers (phy-jh7110-usb.c)
	kUsb2ClkMode		= 0x0,
	kUsb2RxNormalPwr	= 1u << 1,
	kUsb2LsKeepalive	= 0x4,
	kUsb2LsKeepaliveOn	= 1u << 4,
	// PCIe PHY registers (phy-jh7110-pcie.c)
	kPhyKvcoLevel		= 0x28,
	kPhyKvcoTuneSignal	= 0x80,
	kPhyUsb3PllCtl		= 0x7c,
	kPhyKvcoFineLevel	= 0x91,
	kPhyKvcoFineSignals	= 0xc,

	// cdns3 OTG (drd.h). v1: did at 0, cmd at 0x10; v0: cmd at 0.
	kOtgV1Did			= 0x00,
	kOtgV1Cmd			= 0x10,
	kOtgV1Simulate		= 0x40,
	kOtgV0Cmd			= 0x00,
	kOtgV0Simulate		= 0x3c,
	kOtgCmdHostBusReq	= 1u << 1,
	kOtgCmdOtgDis		= 1u << 3,
	kOtgStsXhciReady	= 1u << 26,
};


struct Cdns3Jh7110 {
	area_id		areas[8];
	int32		areaCount;
	vuint32*	stgCrg;
	vuint32*	stgSyscon;
	vuint32*	sysCrg;
	vuint32*	sysSyscon;
	vuint32*	usb2Phy;
	vuint32*	pciePhy0;
	vuint32*	otg;
	bool		usb3;
	bool		lite;
	uint64		xhciPhys;
	uint32		irq;
	device_node*	ownNode;

	Cdns3Jh7110() : areaCount(0), usb3(false), lite(false),
		xhciPhys(kXhciPhys), irq(kHostIrq), ownNode(NULL) {}
	~Cdns3Jh7110()
	{
		for (int32 i = 0; i < areaCount; i++)
			delete_area(areas[i]);
	}

	vuint32* Map(phys_addr_t phys, size_t size, const char* name)
	{
		void* virt = NULL;
		area_id area = map_physical_memory(name, phys, size,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			&virt);
		if (area < 0) {
			ERROR("cannot map %s at %#" B_PRIxPHYSADDR "\n", name, phys);
			return NULL;
		}
		areas[areaCount++] = area;
		return (vuint32*)virt;
	}
};


static inline void
update(vuint32* base, uint32 offset, uint32 mask, uint32 value)
{
	vuint32* reg = base + offset / 4;
	*reg = (*reg & ~mask) | (value & mask);
}


static void
clock_on(vuint32* crg, uint32 index, const char* name)
{
	uint32 before = crg[index];
	if ((before & kCrgEnable) == 0)
		crg[index] = before | kCrgEnable;
	INFO("clock %-14s [%2" B_PRIu32 "] %#010" B_PRIx32 " -> %#010" B_PRIx32
		"\n", name, index, before, (uint32)crg[index]);
}


static status_t
reset_deassert(vuint32* crg, uint32 id, const char* name)
{
	vuint32* assertReg = crg + (kStgRstAssert + 4 * (id / 32)) / 4;
	vuint32* statusReg = crg + (kStgRstStatus + 4 * (id / 32)) / 4;
	uint32 mask = 1u << (id % 32);
	if ((*assertReg & mask) != 0)
		*assertReg &= ~mask;
	for (int i = 0; i < 1000; i++) {
		if ((*statusReg & mask) != 0)
			return B_OK;
		spin(1);
	}
	ERROR("reset %s did not deassert\n", name);
	return B_TIMED_OUT;
}


// PLL0 rate (clk-starfive-jh7110-pll.c jh7110_pll_recalc_rate()).
static uint64
pll0_rate(vuint32* sysSyscon)
{
	const uint64 osc = 24000000;
	uint32 pd = sysSyscon[kPll0Pd / 4];
	uint32 dacpd = (pd >> 24) & 1, dsmpd = (pd >> 25) & 1;
	uint32 fbdiv = sysSyscon[kPll0Fbdiv / 4] & 0xfff;
	uint32 fracReg = sysSyscon[kPll0Frac / 4];
	uint32 frac = fracReg & 0xffffff;
	uint32 postdiv1 = (fracReg >> 28) & 3;
	uint32 prediv = sysSyscon[kPll0Prediv / 4] & 0x3f;
	if (prediv == 0)
		return 0;
	uint64 rate;
	if (dacpd == 0 && dsmpd == 0)
		rate = osc * frac / (1u << 24);
	else if (dacpd == 1 && dsmpd == 1)
		rate = 0;
	else
		return 0;
	rate += osc * fbdiv;
	return rate / ((uint64)prediv << postdiv1);
}


// #pragma mark - device tree helpers


static bool
get_fdt(device_node* node, fdt_device_module_info*& module,
	fdt_device*& device)
{
	return gDeviceManager->get_driver(node, (driver_module_info**)&module,
		(void**)&device) == B_OK;
}


static bool
has_string(const char* list, int length, const char* value)
{
	if (list == NULL)
		return false;
	const char* end = list + length;
	while (list < end) {
		if (strcmp(list, value) == 0)
			return true;
		list += strlen(list) + 1;
	}
	return false;
}


static bool
prop_has_string(fdt_device_module_info* module, fdt_device* device,
	const char* name, const char* value)
{
	int length = 0;
	const char* list = (const char*)module->get_prop(device, name, &length);
	return has_string(list, length, value);
}


static bool
read_u32(fdt_device_module_info* module, fdt_device* device, const char* name,
	uint32& value)
{
	int length = 0;
	const uint8* bytes = (const uint8*)module->get_prop(device, name, &length);
	if (bytes == NULL || length < 4)
		return false;
	value = ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16)
		| ((uint32)bytes[2] << 8) | bytes[3];
	return true;
}


static bool
is_fdt_node(device_node* node)
{
	const char* bus;
	return gDeviceManager->get_attr_string(node, B_DEVICE_BUS, &bus, false)
		== B_OK && strcmp(bus, "fdt") == 0;
}


//! True if the DT root says this is a JH7110S / VisionFive 2 Lite.
static bool
root_is_lite(device_node* node)
{
	device_node* root = NULL;
	device_node* current = gDeviceManager->get_parent_node(node);
	while (current != NULL && is_fdt_node(current)) {
		if (root != NULL)
			gDeviceManager->put_node(root);
		root = current;
		current = gDeviceManager->get_parent_node(current);
	}
	if (current != NULL)
		gDeviceManager->put_node(current);
	if (root == NULL)
		return false;

	bool lite = false;
	fdt_device_module_info* module;
	fdt_device* device;
	if (get_fdt(root, module, device)) {
		lite = prop_has_string(module, device, "compatible",
				"starfive,jh7110s")
			|| prop_has_string(module, device, "compatible",
				"starfive,visionfive-2-lite")
			|| prop_has_string(module, device, "compatible",
				"starfive,visionfive-2-lite-emmc");
	}
	gDeviceManager->put_node(root);
	return lite;
}


/*!	Finds the "cdns,usb3" child of the glue node. Returns a referenced node
	or NULL.
*/
static device_node*
find_core_child(device_node* node)
{
	device_attr attrs[] = {
		{ "fdt/compatible", B_STRING_TYPE, { .string = "cdns,usb3" }},
		{}
	};
	device_node* child = NULL;
	if (gDeviceManager->get_next_child_node(node, attrs, &child) != B_OK)
		return NULL;
	return child;
}


static const char*
dr_mode(fdt_device_module_info* module, fdt_device* device)
{
	int length = 0;
	return (const char*)module->get_prop(device, "dr_mode", &length);
}


// #pragma mark - driver


static float
cdns3_supports_device(device_node* parent)
{
	if (!is_fdt_node(parent))
		return 0.0f;

	fdt_device_module_info* module;
	fdt_device* device;
	if (!get_fdt(parent, module, device))
		return 0.0f;

	// Vendor U-Boot DT: "starfive,jh7110-cdns3"; Linux: "starfive,jh7110-usb".
	if (!prop_has_string(module, device, "compatible", "starfive,jh7110-cdns3")
		&& !prop_has_string(module, device, "compatible",
			"starfive,jh7110-usb")) {
		return 0.0f;
	}

	int length = 0;
	const char* status = (const char*)module->get_prop(device, "status",
		&length);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}

	// Leave gadget/OTG ports alone (VisionFive 2 and Orange Pi RV: the
	// USB-C port; their USB-A ports are on a PCIe VL805).
	bool host = false;
	const char* mode = dr_mode(module, device);
	if (mode != NULL) {
		if (strcmp(mode, "host") != 0) {
			INFO("dr_mode \"%s\" on the glue node, not attaching\n", mode);
			return 0.0f;
		}
		host = true;
	}
	device_node* child = find_core_child(parent);
	if (child != NULL) {
		fdt_device_module_info* childModule;
		fdt_device* childDevice;
		if (get_fdt(child, childModule, childDevice)) {
			mode = dr_mode(childModule, childDevice);
			if (mode != NULL && strcmp(mode, "host") == 0)
				host = true;
			else if (mode != NULL && (strcmp(mode, "peripheral") == 0
					|| strcmp(mode, "otg") == 0)) {
				gDeviceManager->put_node(child);
				INFO("dr_mode \"%s\" on the core node, not attaching\n", mode);
				return 0.0f;
			}
		}
		gDeviceManager->put_node(child);
	}

	// The VF2 Lite vendor DT says dr_mode "unknown" (host by its board
	// code); its USB-A ports hang off this controller.
	if (!host && !root_is_lite(parent))
		return 0.0f;

	return 0.8f;
}


static void read_config(device_node* fdtNode, struct Cdns3Jh7110& usb);


/*!	The xHCI (MIT <usb>xhci, platform attachment) is this node's fixed
	child; it reads xhci/phys, xhci/size and xhci/irq from here. The device
	manager initializes this node (power-up, host mode) before the child.
*/
static status_t
cdns3_register_device(device_node* parent)
{
	Cdns3Jh7110 config;
	read_config(parent, config);

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "StarFive JH7110 Cadence USB 3 (host)" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = XHCI_PLATFORM_MODULE_NAME }},
		{ "xhci/phys", B_UINT64_TYPE, { .ui64 = config.xhciPhys }},
		{ "xhci/size", B_UINT64_TYPE, { .ui64 = kXhciSize }},
		{ "xhci/irq", B_UINT32_TYPE, { .ui32 = config.irq }},
		{}
	};
	status_t status = gDeviceManager->register_node(parent,
		CDNS3_JH7110_MODULE_NAME, attrs, NULL, NULL);
	INFO("registered (xhci %#" B_PRIx64 ", irq %" B_PRIu32 "): %s\n",
		config.xhciPhys, config.irq, strerror(status));
	return status;
}


static void
read_config(device_node* fdtNode, struct Cdns3Jh7110& usb)
{
	fdt_device_module_info* module;
	fdt_device* device;
	usb.lite = root_is_lite(fdtNode);

	if (get_fdt(fdtNode, module, device)) {
		// Vendor DT: starfive,usb2-only = <0> means USB 3 (VF2 Lite).
		uint32 usb2Only = 1;
		if (read_u32(module, device, "starfive,usb2-only", usb2Only))
			usb.usb3 = usb2Only == 0;
	}

	device_node* child = find_core_child(fdtNode);
	if (child != NULL) {
		fdt_device_module_info* childModule;
		fdt_device* childDevice;
		if (get_fdt(child, childModule, childDevice)) {
			// Linux DT: the USB 3 PHY is listed as "cdns3,usb3-phy".
			if (prop_has_string(childModule, childDevice, "phy-names",
					"cdns3,usb3-phy")) {
				uint32 dummy;
				// vendor DT names it too but has no phys; trust usb2-only
				if (read_u32(childModule, childDevice, "phys", dummy))
					usb.usb3 = true;
			}
			uint64 reg, size;
			if (childModule->get_reg(childDevice, 1, &reg, &size)) {
				// Linux DT: relative to the glue's "ranges" (0x10100000)
				if (reg < kOtgPhys)
					reg += kOtgPhys;
				usb.xhciPhys = reg;
			}
			uint64 irq;
			if (childModule->get_interrupt(childDevice, 0, NULL, &irq))
				usb.irq = (uint32)irq;
		}
		gDeviceManager->put_node(child);
	}
	if (usb.lite)
		usb.usb3 = true;
}


static status_t
cdns3_power_up(Cdns3Jh7110& usb)
{
	usb.stgCrg = usb.Map(kStgCrgPhys, B_PAGE_SIZE, "cdns3 stgcrg");
	usb.stgSyscon = usb.Map(kStgSysconPhys, B_PAGE_SIZE, "cdns3 stg syscon");
	usb.sysCrg = usb.Map(kSysCrgPhys, B_PAGE_SIZE, "cdns3 syscrg");
	usb.sysSyscon = usb.Map(kSysSysconPhys, B_PAGE_SIZE, "cdns3 sys syscon");
	usb.usb2Phy = usb.Map(kUsb2PhyPhys, B_PAGE_SIZE, "cdns3 usb2 phy");
	usb.pciePhy0 = usb.Map(kPciePhy0Phys, B_PAGE_SIZE, "cdns3 usb3 phy");
	usb.otg = usb.Map(kOtgPhys, B_PAGE_SIZE, "cdns3 otg");
	if (usb.stgCrg == NULL || usb.stgSyscon == NULL || usb.sysCrg == NULL
		|| usb.sysSyscon == NULL || usb.usb2Phy == NULL
		|| usb.pciePhy0 == NULL || usb.otg == NULL) {
		return B_NO_MEMORY;
	}

	INFO("%s, USB %s, xhci %#" B_PRIx64 ", irq %" B_PRIu32 "\n",
		usb.lite ? "VisionFive 2 Lite (JH7110S)" : "JH7110",
		usb.usb3 ? "3 + 2" : "2 only", usb.xhciPhys, usb.irq);

	// VBUS and the port switch (jh7110-starfive-visionfive-2-lite.dtsi
	// usb0_pins: GPIO26 high = power, GPIO62 low = switch).
	if (usb.lite) {
		Jh7110SysGpio gpio;
		if (gpio.Init(kJh7110SysGpioPhys) == B_OK) {
			gpio.LogPin(26, "usb0 power (before)");
			gpio.LogPin(62, "usb0 switch (before)");
			gpio.SetOutput(26, true);
			gpio.SetOutput(62, false);
		}
	}

	// cdns_mode_init()
	update(usb.stgSyscon, kStgUsbMode, kUsbMiscCfgMask,
		kUsbSuspendmByps | kUsbPllEn | kUsbRefclkMode);
	update(usb.stgSyscon, kStgUsbMode, kUsbStrapMask, kUsbStrapHost);
	update(usb.stgSyscon, kStgUsbMode, kUsbSuspendmMask, kUsbSuspendmHost);
	INFO("stg usb mode %#010" B_PRIx32 "\n",
		(uint32)usb.stgSyscon[kStgUsbMode / 4]);

	// usb_125m = PLL0 / div, set to 125 MHz (PLL0 is 1.5 GHz on the JH7110
	// boards, 1.25 GHz on the VF2 Lite).
	uint64 pll0 = pll0_rate(usb.sysSyscon);
	uint32 clk = usb.sysCrg[kSysClkUsb125m];
	uint32 div = clk & kCrgDivMask;
	if (pll0 != 0) {
		uint32 want = (uint32)((pll0 + 62500000) / 125000000);
		if (want != 0 && want != div) {
			usb.sysCrg[kSysClkUsb125m] = (clk & ~kCrgDivMask) | want;
			div = want;
		}
	}
	INFO("PLL0 %" B_PRIu64 " Hz, usb_125m div %" B_PRIu32 " (%" B_PRIu64
		" Hz)\n", pll0, div, div != 0 ? pll0 / div : 0);

	// clocks (vendor DT order), then resets
	clock_on(usb.stgCrg, kStgClkUsb0App125, "usb0_app_125");
	clock_on(usb.stgCrg, kStgClkUsb0Lpm, "usb0_lpm");
	clock_on(usb.stgCrg, kStgClkUsb0Stb, "usb0_stb");
	clock_on(usb.stgCrg, kStgClkUsb0Apb, "usb0_apb");
	clock_on(usb.stgCrg, kStgClkUsb0Axi, "usb0_axi");
	clock_on(usb.stgCrg, kStgClkUsb0UtmiApb, "usb0_utmi_apb");
	if (usb.usb3)
		clock_on(usb.stgCrg, kStgClkPcie0Apb, "pcie0_apb (phy)");

	status_t status = reset_deassert(usb.stgCrg, kStgRstUsb0Pwrup, "pwrup");
	if (status == B_OK)
		status = reset_deassert(usb.stgCrg, kStgRstUsb0Apb, "apb");
	if (status == B_OK)
		status = reset_deassert(usb.stgCrg, kStgRstUsb0Axi, "axi");
	if (status == B_OK)
		status = reset_deassert(usb.stgCrg, kStgRstUsb0UtmiApb, "utmi_apb");
	if (status == B_OK && usb.usb3)
		status = reset_deassert(usb.stgCrg, kStgRstPcie0Apb, "pcie0_apb");
	if (status != B_OK)
		return status;

	// jh7110_usb2_phy_init()
	usb.usb2Phy[kUsb2ClkMode / 4] |= kUsb2RxNormalPwr;
	// jh7110_pcie_phy_probe(): KVCO fine tune
	if (usb.usb3) {
		usb.pciePhy0[kPhyKvcoLevel / 4] = kPhyKvcoFineLevel;
		usb.pciePhy0[kPhyKvcoTuneSignal / 4] = kPhyKvcoFineSignals;
	}

	// cdns_drd_init(): version detect
	vuint32* cmd;
	if (usb.otg[kOtgV0Cmd / 4] == 0) {
		INFO("DRD v0\n");
		cmd = usb.otg + kOtgV0Cmd / 4;
		usb.otg[kOtgV0Simulate / 4] = 1;
	} else {
		uint32 did = usb.otg[kOtgV1Did / 4];
		INFO("DRD did %#010" B_PRIx32 " rid %#010" B_PRIx32 "\n", did,
			(uint32)usb.otg[1]);
		if ((did & 0xffffff00) != 0x00040200) {
			ERROR("unsupported DRD did %#" B_PRIx32 "\n", did);
			return B_NOT_SUPPORTED;
		}
		cmd = usb.otg + kOtgV1Cmd / 4;
		usb.otg[kOtgV1Simulate / 4] = 1;
	}
	vuint32* sts = cmd + 1;
	INFO("OTG sts %#010" B_PRIx32 " (strap %" B_PRIu32 ")\n", (uint32)*sts,
		((uint32)*sts >> 12) & 7);

	// cdns_drd_host_on()
	*cmd = kOtgCmdHostBusReq | kOtgCmdOtgDis;
	bool ready = false;
	for (int i = 0; i < 100000; i++) {
		if ((*sts & kOtgStsXhciReady) != 0) {
			ready = true;
			break;
		}
		spin(1);
	}
	if (!ready) {
		ERROR("timeout waiting for xhci_ready (sts %#" B_PRIx32 ")\n",
			(uint32)*sts);
		return B_TIMED_OUT;
	}

	// phy_set_mode(usb2, HOST): LS keep-alive, connect the USB 2 PHY
	usb.usb2Phy[kUsb2LsKeepalive / 4] |= kUsb2LsKeepaliveOn;
	update(usb.sysSyscon, kSysUsbSplit, kUsbPdrstnSplit, kUsbPdrstnSplit);

	// phy_set_mode(usb3, HOST): PCIe PHY 0 in USB 3 mode
	if (usb.usb3) {
		update(usb.stgSyscon, kStgPhy0Mode, kPciePhyModeMask,
			kPciePhyModeUsb);
		update(usb.stgSyscon, kStgPhy0Usb, kUsb3BusWidthMask, 0);
		update(usb.stgSyscon, kStgPhy0Usb, kUsb3PhyEnable, kUsb3PhyEnable);
		update(usb.sysSyscon, kSysUsbSplit, kUsbPdrstnSplit, 0);
		usb.pciePhy0[kPhyUsb3PllCtl / 4] = kUsb3PhyEnable;
	}

	INFO("host mode on (OTG sts %#010" B_PRIx32 ")\n", (uint32)*sts);
	return B_OK;
}


static status_t
cdns3_init_driver(device_node* node, void** cookie)
{
	Cdns3Jh7110* usb = new(std::nothrow) Cdns3Jh7110();
	if (usb == NULL)
		return B_NO_MEMORY;

	usb->ownNode = node;
	device_node* fdtNode = gDeviceManager->get_parent_node(node);
	read_config(fdtNode, *usb);
	gDeviceManager->put_node(fdtNode);

	status_t status = cdns3_power_up(*usb);
	if (status != B_OK) {
		delete usb;
		return status;
	}
	*cookie = usb;
	return B_OK;
}


static void
cdns3_uninit_driver(void* cookie)
{
	delete (Cdns3Jh7110*)cookie;
}


#if 0
static status_t
cdns3_register_child_devices(void* cookie)
{
	Cdns3Jh7110* usb = (Cdns3Jh7110*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = XHCI_PLATFORM_BUS }},
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Cadence xHCI (JH7110)" }},
		{ "xhci/phys", B_UINT64_TYPE, { .ui64 = usb->xhciPhys }},
		{ "xhci/size", B_UINT64_TYPE, { .ui64 = kXhciSize }},
		{ "xhci/irq", B_UINT32_TYPE, { .ui32 = usb->irq }},
		{ "xhci/quirk", B_STRING_TYPE, { .string = "cdns3" }},
		{}
	};
	return gDeviceManager->register_node(usb->ownNode, NULL, attrs, NULL,
		NULL);
}
#endif


static driver_module_info gCdns3Jh7110Module = {
	{
		CDNS3_JH7110_MODULE_NAME,
		0,
		NULL
	},
	cdns3_supports_device,
	cdns3_register_device,
	cdns3_init_driver,
	cdns3_uninit_driver,
	NULL,	// register_child_devices: the xHCI is a fixed child
	NULL,	// rescan
	NULL,	// removed
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


_EXPORT module_info* modules[] = {
	(module_info*)&gCdns3Jh7110Module,
	NULL
};
