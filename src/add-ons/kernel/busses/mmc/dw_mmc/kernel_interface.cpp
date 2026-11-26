/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Device manager glue for the DesignWare MMC host driver: binds to FDT
 * nodes, reads the device tree, and exports the mmc_bus_interface that
 * Haiku's MMC bus manager (bus_managers/mmc) drives. The FDT attach pattern
 * follows the Beacon StarFive PCIe driver (busses/pci/starfive).
 *
 * This file contains no third-party code, but it includes the
 * GPL-2.0-or-later DwMmc.h, so the linked add-on is GPL-2.0-or-later.
 */


#include "DwMmc.h"
#include "Jh7110Gpio.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <driver_settings.h>


#define INFO(x...)	dprintf("dw_mmc: " x)
#define ERROR(x...)	dprintf("dw_mmc: ERROR: " x)
#define SDIO_INFO(x...)	dprintf("sdio: " x)


device_manager_info* gDeviceManager;


// Default card input clock if the DT says nothing. JH7110 firmware and
// both Linux and U-Boot DTs set the SDIO card clock to 50 MHz.
static const uint32 kDefaultBusHz = 50000000;

static const char* const kCompatibles[] = {
	"starfive,jh7110-mmc",
	"starfive,jh7110-sdio",
	"snps,dw-mshc",
	NULL
};


// #pragma mark - device tree helpers


static bool
get_fdt(device_node* fdtNode, fdt_device_module_info*& module,
	fdt_device*& device)
{
	return gDeviceManager->get_driver(fdtNode, (driver_module_info**)&module,
		(void**)&device) == B_OK;
}


static bool
read_u32(fdt_device_module_info* module, fdt_device* device, const char* name,
	uint32& value)
{
	int length = 0;
	const void* prop = module->get_prop(device, name, &length);
	if (prop == NULL || length < 4)
		return false;
	// DT cells are big-endian. Assemble by hand: on rv64gc (no Zbb) a byte
	// swap builtin becomes a libgcc __bswapsi2 call.
	const uint8* bytes = (const uint8*)prop;
	value = ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16)
		| ((uint32)bytes[2] << 8) | bytes[3];
	return true;
}


static bool
has_prop(fdt_device_module_info* module, fdt_device* device, const char* name)
{
	int length = 0;
	return module->get_prop(device, name, &length) != NULL;
}


static bool
string_list_contains(const char* list, int length, const char* string)
{
	for (const char* it = list; it < list + length; it += strlen(it) + 1) {
		if (strcmp(it, string) == 0)
			return true;
	}
	return false;
}


static bool
has_compatible(fdt_device_module_info* module, fdt_device* device,
	const char* compatible)
{
	int length = 0;
	const char* list = (const char*)module->get_prop(device, "compatible",
		&length);
	return list != NULL && string_list_contains(list, length, compatible);
}


// #pragma mark - driver hooks


static float
dw_mmc_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return -1.0f;
	}
	if (strcmp(bus, "fdt") != 0)
		return 0.0f;

	fdt_device_module_info* module;
	fdt_device* device;
	if (!get_fdt(parent, module, device))
		return 0.0f;

	bool match = false;
	for (int i = 0; kCompatibles[i] != NULL && !match; i++)
		match = has_compatible(module, device, kCompatibles[i]);
	if (!match)
		return 0.0f;

	// Haiku's FDT bus registers every node, including disabled ones.
	int length = 0;
	const char* okay = (const char*)module->get_prop(device, "status",
		&length);
	if (okay != NULL && strcmp(okay, "okay") != 0 && strcmp(okay, "ok") != 0)
		return 0.0f;

	// Beacon: SDIO-only slots (mmc-pwrseq, a wifi@ child node) attach too
	// now; the MMC bus manager enumerates SDIO cards.
	return 0.8f;
}


static status_t
dw_mmc_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DesignWare MMC host controller" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = MMC_BUS_MODULE_NAME }},
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "mmc" }},

		// Restrictions for the DMAResource of mmc_disk. PIO copies through
		// a bounce buffer, so any physical address and alignment works. Cap
		// one operation at 256 KiB to bound the time the bus is held.
		{ B_DMA_MAX_TRANSFER_BLOCKS, B_UINT32_TYPE, { .ui32 = 512 }},
		{ B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE, { .ui32 = 64 }},
		{}
	};

	return gDeviceManager->register_node(parent, DW_MMC_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


// Mirrors the private struct fdt_device of bus_managers/fdt/fdt_module.cpp.
// fdt_device_get_prop()/get_reg() only use ->node (its "fdt/node"
// attribute), so this lets us read properties of a DT node that has no driver
// attached (an mmc-pwrseq or GPIO controller node) without initialising it.
struct beacon_fdt_device_alias {
	device_node*	node;
	device_node*	bus;
};


static device_node*
node_by_phandle(fdt_device_module_info* module, fdt_device* device,
	uint32 phandle)
{
	device_node* busNode = module->get_bus(device);
	fdt_bus_module_info* busModule;
	struct fdt_bus* bus;
	if (busNode == NULL || gDeviceManager->get_driver(busNode,
			(driver_module_info**)&busModule, (void**)&bus) != B_OK) {
		return NULL;
	}
	return busModule->node_by_phandle(bus, (int)phandle);
}


static uint32
be32_at(const void* prop, int index)
{
	const uint8* bytes = (const uint8*)prop + 4 * index;
	return ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16)
		| ((uint32)bytes[2] << 8) | bytes[3];
}


static void
read_pwrseq(fdt_device_module_info* module, fdt_device* device,
	DwMmcConfig& config)
{
	// mmc-pwrseq = <&wifi_pwrseq>; wifi_pwrseq: mmc-pwrseq-simple with
	// reset-gpios = <&sysgpio 62 GPIO_ACTIVE_LOW> (Linux
	// jh7110-orangepi-rv.dts, bindings mmc-pwrseq-simple.yaml).
	int length = 0;
	const void* prop = module->get_prop(device, "mmc-pwrseq", &length);
	if (prop == NULL || length < 4)
		return;

	device_node* pwrseqNode = node_by_phandle(module, device,
		be32_at(prop, 0));
	if (pwrseqNode == NULL) {
		ERROR("%s: mmc-pwrseq phandle not found\n", config.name);
		return;
	}
	beacon_fdt_device_alias alias = { pwrseqNode, NULL };
	fdt_device* pwrseq = (fdt_device*)&alias;
	if (!has_compatible(module, pwrseq, "mmc-pwrseq-simple")) {
		ERROR("%s: mmc-pwrseq is not mmc-pwrseq-simple; ignored\n",
			config.name);
		return;
	}

	const void* gpios = module->get_prop(pwrseq, "reset-gpios", &length);
	if (gpios == NULL || length < 12) {
		SDIO_INFO("%s: mmc-pwrseq-simple without reset-gpios\n",
			config.name);
		return;
	}
	if (length > 12) {
		SDIO_INFO("%s: mmc-pwrseq has %d reset GPIOs, using the first\n",
			config.name, length / 12);
	}

	device_node* gpioNode = node_by_phandle(module, device, be32_at(gpios, 0));
	uint64 gpioBase = 0;
	uint64 gpioSize = 0;
	if (gpioNode != NULL) {
		beacon_fdt_device_alias gpioAlias = { gpioNode, NULL };
		module->get_reg((fdt_device*)&gpioAlias, 0, &gpioBase, &gpioSize);
	}
	if (gpioBase != kJh7110SysGpioPhys) {
		ERROR("%s: reset GPIO controller at %#" B_PRIx64 " is not the JH7110 "
			"sysgpio; cannot drive it\n", config.name, gpioBase);
		return;
	}

	config.resetGpio = (int32)be32_at(gpios, 1);
	config.resetActiveLow = (be32_at(gpios, 2) & 1) != 0;
	config.resetFromDt = true;
	config.resetHeuristic = false;

	const void* delay = module->get_prop(pwrseq, "post-power-on-delay-ms",
		&length);
	if (delay != NULL && length >= 4)
		config.resetPostDelayMs = be32_at(delay, 0);
	SDIO_INFO("%s: DT mmc-pwrseq-simple: reset GPIO%" B_PRId32 " active %s, "
		"post-power-on-delay %" B_PRIu32 " ms\n", config.name,
		config.resetGpio, config.resetActiveLow ? "low" : "high",
		config.resetPostDelayMs);
}


static void
read_settings(DwMmcConfig& config)
{
	// Optional ~/config/settings/kernel/drivers/dw_mmc:
	//   max_clock_khz 25000          cap every dw_mmc bus clock
	//   jh7110_sdio0_reset_gpio 62   reset GPIO for the JH7110 sdio0 (Wi-Fi)
	//                                when the DT has no mmc-pwrseq; "none"
	//                                disables the Orange Pi RV fallback
	void* handle = load_driver_settings("dw_mmc");
	if (handle == NULL)
		return;

	const char* value = get_driver_parameter(handle, "max_clock_khz", NULL,
		NULL);
	if (value != NULL) {
		config.maxKhzSetting = strtoul(value, NULL, 0);
		INFO("%s: settings: max_clock_khz %" B_PRIu32 "\n", config.name,
			config.maxKhzSetting);
	}

	if (config.isJh7110Sdio0 && !config.resetFromDt) {
		value = get_driver_parameter(handle, "jh7110_sdio0_reset_gpio", NULL,
			NULL);
		if (value != NULL) {
			if (strcmp(value, "none") == 0)
				config.resetGpio = -1;
			else {
				config.resetGpio = strtol(value, NULL, 0);
				config.resetActiveLow = true;
			}
			config.resetHeuristic = config.resetGpio >= 0;
			SDIO_INFO("%s: settings: jh7110_sdio0_reset_gpio %s\n",
				config.name, value);
		}
	}
	unload_driver_settings(handle);
}


static status_t
read_config(device_node* fdtNode, DwMmcConfig& config)
{
	fdt_device_module_info* module;
	fdt_device* device;
	if (!get_fdt(fdtNode, module, device))
		return B_ERROR;

	memset(&config, 0, sizeof(config));
	config.resetGpio = -1;

	uint64 base = 0;
	uint64 size = 0;
	if (!module->get_reg(device, 0, &base, &size) || size == 0) {
		ERROR("no reg property\n");
		return B_BAD_VALUE;
	}
	config.physBase = base;
	config.physSize = size;
	snprintf(config.name, sizeof(config.name), "mmc@%" B_PRIx64, base);

	if (!module->get_interrupt(device, 0, NULL, &config.irq)) {
		// Beacon: the StarFive/Orange Pi vendor U-Boot DT leaves out
		// "interrupts" on its sdio nodes; Linux's jh7110.dtsi uses PLIC 74/75.
		if (base == kJh7110Sdio0Base)
			config.irq = 74;
		else if (base == kJh7110Sdio1Base)
			config.irq = 75;
		else {
			ERROR("%s: no interrupts property\n", config.name);
			return B_BAD_VALUE;
		}
		ERROR("%s: no interrupts property, using JH7110 PLIC source %"
			B_PRIu32 "\n", config.name, (uint32)config.irq);
	}

	config.hasStarfiveCompat
		= has_compatible(module, device, "starfive,jh7110-mmc")
		|| has_compatible(module, device, "starfive,jh7110-sdio");
	config.isJh7110 = config.hasStarfiveCompat
		|| base == kJh7110Sdio0Base || base == kJh7110Sdio1Base;

	// Card input ("ciu") clock. No clock driver: trust what firmware set,
	// as described by clock-frequency or the assigned rate.
	uint32 value = 0;
	int length = 0;
	if (read_u32(module, device, "clock-frequency", value) && value != 0) {
		config.busHz = value;
	} else if (module->get_prop(device, "assigned-clock-rates", &length)
			!= NULL && length == 4
		&& read_u32(module, device, "assigned-clock-rates", value)
		&& value != 0) {
		config.busHz = value;
	} else {
		config.busHz = kDefaultBusHz;
		INFO("%s: no clock rate in DT, assuming %" B_PRIu32 " Hz\n",
			config.name, config.busHz);
	}

	if (read_u32(module, device, "max-frequency", value))
		config.maxHz = value;
	if (read_u32(module, device, "fifo-depth", value))
		config.fifoDepth = value;
	if (read_u32(module, device, "data-addr", value))
		config.dataAddr = value;
	config.busWidth = 4;
	if (read_u32(module, device, "bus-width", value)
		&& (value == 1 || value == 4 || value == 8)) {
		config.busWidth = value;
	}
	config.powerOnDelayMs = 10;
	if (read_u32(module, device, "post-power-on-delay-ms", value))
		config.powerOnDelayMs = value;

	// The CDETECT pin is only trustworthy if nothing says otherwise. The
	// JH7110 boards route card detect to a GPIO (cd-gpios in the Linux DT,
	// nothing at all in the vendor U-Boot DT), so probe by command there.
	// Beacon: SDIO power sequence. The mainline DT describes it with
	// mmc-pwrseq. The vendor U-Boot DT that Haiku gets on the Orange Pi RV is
	// the VisionFive 2 one (sdio0 = 8-bit eMMC, no pwrseq), so on the JH7110
	// sdio0 fall back to the Orange Pi RV WL_REG_ON (GPIO62, active low), but
	// only if nothing answers on the bus first (a VisionFive 2 eMMC answers
	// CMD1, so its GPIO62 is never touched).
	// Beacon (VisionFive 2 Lite): only the VisionFive 2 vendor DT describes
	// sdio0 as an 8-bit eMMC slot, and that is the DT the Orange Pi RV gets.
	// On the VF2 Lite sdio0 is the 4-bit microSD slot (vendor and mainline
	// DTs) and GPIO62 drives its USB0 switch, so never touch GPIO62 there.
	config.isJh7110Sdio0 = config.isJh7110 && base == kJh7110Sdio0Base;
	read_pwrseq(module, device, config);
	if (config.isJh7110Sdio0 && !config.resetFromDt
		&& config.busWidth == 8) {
		config.resetGpio = 62;
		config.resetActiveLow = true;
		config.resetHeuristic = true;
	}
	read_settings(config);

	config.cdUsable = !config.isJh7110
		&& !has_prop(module, device, "cd-gpios")
		&& !has_prop(module, device, "broken-cd")
		&& !has_prop(module, device, "non-removable");

	return B_OK;
}


static status_t
dw_mmc_init_driver(device_node* node, void** cookie)
{
	device_node* fdtNode = gDeviceManager->get_parent_node(node);
	DwMmcConfig config;
	status_t status = read_config(fdtNode, config);
	gDeviceManager->put_node(fdtNode);
	if (status != B_OK)
		return status;

	DwMmcHost* host = new(std::nothrow) DwMmcHost();
	if (host == NULL)
		return B_NO_MEMORY;

	status = host->Init(config);
	if (status != B_OK) {
		ERROR("%s: init failed: %s\n", config.name, strerror(status));
		delete host;
		return status;
	}

	*cookie = host;
	return B_OK;
}


static void
dw_mmc_uninit_driver(void* cookie)
{
	delete (DwMmcHost*)cookie;
}


// #pragma mark - mmc_bus_interface


static status_t
dw_mmc_set_clock(void* controller, uint32_t kilohertz)
{
	return ((DwMmcHost*)controller)->SetClock(kilohertz);
}


static status_t
dw_mmc_execute_command(void* controller, uint8_t command, uint32_t argument,
	uint32_t* result)
{
	return ((DwMmcHost*)controller)->ExecuteCommand(command, argument, result);
}


static status_t
dw_mmc_do_io(void* controller, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	return ((DwMmcHost*)controller)->DoIO(command, operation, offsetAsSectors);
}


static void
dw_mmc_set_scan_semaphore(void* controller, sem_id semaphore)
{
	((DwMmcHost*)controller)->SetScanSemaphore(semaphore);
}


static void
dw_mmc_set_bus_width(void* controller, int width)
{
	((DwMmcHost*)controller)->SetBusWidth(width);
}


static void
dw_mmc_terminate_bus(void* controller)
{
	((DwMmcHost*)controller)->TerminateBus();
}


static void
dw_mmc_set_card_type(void* controller, card_type type)
{
	((DwMmcHost*)controller)->SetCardType(type);
}


static status_t
dw_mmc_execute_buffer_command(void* controller, uint8_t command,
	uint32_t argument, uint32_t* result, const mmc_buffer_transfer* data)
{
	return ((DwMmcHost*)controller)->ExecuteBufferCommand(command, argument,
		result, data);
}


static void
dw_mmc_set_sdio_irq_semaphore(void* controller, sem_id semaphore)
{
	((DwMmcHost*)controller)->SetSdioIrqSemaphore(semaphore);
}


static void
dw_mmc_enable_sdio_irq(void* controller, bool enable)
{
	((DwMmcHost*)controller)->EnableSdioIrq(enable);
}


static status_t
dw_mmc_power_cycle(void* controller)
{
	return ((DwMmcHost*)controller)->PowerCycle();
}


mmc_bus_interface gDwMmcBusModule = {
	.info = {
		.info = {
			.name = DW_MMC_DRIVER_MODULE_NAME,
		},
		.supports_device = dw_mmc_supports_device,
		.register_device = dw_mmc_register_device,
		.init_driver = dw_mmc_init_driver,
		.uninit_driver = dw_mmc_uninit_driver,
	},

	.set_clock = dw_mmc_set_clock,
	.execute_command = dw_mmc_execute_command,
	.do_io = dw_mmc_do_io,
	.set_scan_semaphore = dw_mmc_set_scan_semaphore,
	.set_bus_width = dw_mmc_set_bus_width,
	.terminate_bus = dw_mmc_terminate_bus,
	.set_card_type = dw_mmc_set_card_type,

	.execute_buffer_command = dw_mmc_execute_buffer_command,
	.set_sdio_irq_semaphore = dw_mmc_set_sdio_irq_semaphore,
	.enable_sdio_irq = dw_mmc_enable_sdio_irq,
	.power_cycle = dw_mmc_power_cycle,
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


_EXPORT module_info* modules[] = {
	(module_info*)&gDwMmcBusModule,
	NULL
};
