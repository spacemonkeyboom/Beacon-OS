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

#include <new>
#include <stdio.h>
#include <string.h>

#include <bus/FDT.h>


#define INFO(x...)	dprintf("dw_mmc: " x)
#define ERROR(x...)	dprintf("dw_mmc: ERROR: " x)


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


static bool
has_sdio_function_child(device_node* fdtNode)
{
	// An SDIO function (Wi-Fi etc.) described as a child DT node, as on the
	// Orange Pi RV's mmc0 (AP6256). The MMC stack only handles memory cards.
	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "fdt" }},
		{}
	};
	device_node* child = NULL;
	bool found = false;
	while (!found
		&& gDeviceManager->get_next_child_node(fdtNode, attrs, &child)
			== B_OK) {
		const char* compatible = NULL;
		if (gDeviceManager->get_attr_string(child, "fdt/compatible",
				&compatible, false) == B_OK) {
			found = true;
		}
	}
	// get_next_child_node() releases the previous node itself, including
	// when it runs out; only the node we stopped at is still referenced.
	if (found)
		gDeviceManager->put_node(child);
	return found;
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

	const char* name = module->get_name(device);
	if (has_prop(module, device, "mmc-pwrseq")
		|| (has_prop(module, device, "no-sd")
			&& has_prop(module, device, "no-mmc"))
		|| has_sdio_function_child(parent)) {
		INFO("%s: SDIO-only slot, not attaching\n",
			name != NULL ? name : "?");
		return 0.0f;
	}

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


static status_t
read_config(device_node* fdtNode, DwMmcConfig& config)
{
	fdt_device_module_info* module;
	fdt_device* device;
	if (!get_fdt(fdtNode, module, device))
		return B_ERROR;

	memset(&config, 0, sizeof(config));

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
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


_EXPORT module_info* modules[] = {
	(module_info*)&gDwMmcBusModule,
	NULL
};
