/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Device manager glue for the DesignWare Ethernet QoS driver: binds to FDT
 * nodes, reads the device tree (registers, interrupt, phy-mode, MAC
 * address, the PHY child node), publishes net/dwmac/<n> and forwards the
 * device hooks. The FDT attach pattern follows the Beacon dw_mmc driver;
 * the driver/device module split follows Haiku's virtio_net (MIT).
 *
 * This file contains no third-party code, but it includes the
 * GPL-2.0-only Dwmac.h, so the linked add-on is GPL-2.0-only.
 */


#include "Dwmac.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <bus/FDT.h>
#include <fcntl.h>
#include <net_buffer.h>


#define INFO(x...)	dprintf("dwmac: " x)
#define ERROR(x...)	dprintf("dwmac: ERROR: " x)


device_manager_info* gDeviceManager;
net_buffer_module_info* gBufferModule;


static const char* const kCompatibles[] = {
	"starfive,jh7110-eqos-5.20",	// vendor U-Boot 2021.10 DT
	"starfive,jh7110-dwmac",		// Linux DT
	"snps,dwmac-5.20",
	NULL
};


struct dwmac_handle {
	DwmacDevice*	device;
};


// #pragma mark - device tree helpers


// Same layout as struct fdt_device in bus_managers/fdt/fdt_module.cpp. The
// FDT module's get_prop()/get_name() only look at ->node, which lets us
// read properties of a child DT node (the PHY) whose own driver is not
// loaded at the moment.
struct fdt_device_proxy {
	device_node*	node;
	device_node*	bus;
};


static bool
get_fdt(device_node* fdtNode, fdt_device_module_info*& module,
	fdt_device*& device)
{
	return gDeviceManager->get_driver(fdtNode, (driver_module_info**)&module,
		(void**)&device) == B_OK && device != NULL;
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


static int32
read_optional(fdt_device_module_info* module, fdt_device* device,
	const char* name)
{
	uint32 value;
	if (!read_u32(module, device, name, value))
		return -1;
	return (int32)(value & 0x7fffffff);
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


static int32
unit_address(const char* name)
{
	const char* at = strchr(name, '@');
	if (at == NULL || at[1] == '\0')
		return -1;
	int32 value = 0;
	for (const char* it = at + 1; *it != '\0'; it++) {
		char c = *it;
		int digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			return -1;
		value = value * 16 + digit;
		if (value > 0xffff)
			return -1;
	}
	return value;
}


static void
read_phy_node(fdt_device_module_info* module, device_node* child,
	const char* name, DwmacConfig& config)
{
	fdt_device_proxy proxy = { child, NULL };
	fdt_device* phy = (fdt_device*)&proxy;

	config.phyNodeFound = true;
	strlcpy(config.phyNodeName, name, sizeof(config.phyNodeName));

	uint32 reg;
	if (read_u32(module, phy, "reg", reg) && reg < 32)
		config.phyAddress = (int32)reg;
	else {
		int32 unit = unit_address(name);
		if (unit >= 0 && unit < 32)
			config.phyAddress = unit;
	}

	YtPhyConfig& yt = config.yt;
	yt.rxcDlyEn = read_optional(module, phy, "rxc_dly_en");
	yt.rgmiiSwDr = read_optional(module, phy, "rgmii_sw_dr");
	yt.rgmiiSwDr2 = read_optional(module, phy, "rgmii_sw_dr_2");
	yt.rgmiiSwDrRxc = read_optional(module, phy, "rgmii_sw_dr_rxc");
	yt.rxDelaySel = read_optional(module, phy, "rx_delay_sel");
	yt.txDelaySelFe = read_optional(module, phy, "tx_delay_sel_fe");
	yt.txDelaySel = read_optional(module, phy, "tx_delay_sel");
	yt.txInverted10 = read_optional(module, phy, "tx_inverted_10");
	yt.txInverted100 = read_optional(module, phy, "tx_inverted_100");
	yt.txInverted1000 = read_optional(module, phy, "tx_inverted_1000");

	// Linux-style tuning (motorcomm.c upstream) is not interpreted.
	int length;
	if (module->get_prop(phy, "rx-internal-delay-ps", &length) != NULL
		|| module->get_prop(phy, "tx-internal-delay-ps", &length) != NULL
		|| module->get_prop(phy, "motorcomm,tx-clk-1000-inverted", &length)
			!= NULL) {
		INFO("%s: PHY node %s has Linux-style delay properties; they are "
			"NOT applied (only the vendor rx_delay_sel/tx_delay_sel set is)\n",
			config.name, name);
	}
}


static void
find_phy_node(fdt_device_module_info* module, device_node* parent,
	DwmacConfig& config, int depth)
{
	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "fdt" }},
		{}
	};
	device_node* child = NULL;
	while (!config.phyNodeFound
		&& gDeviceManager->get_next_child_node(parent, attrs, &child)
			== B_OK) {
		const char* name = NULL;
		if (gDeviceManager->get_attr_string(child, "fdt/name", &name, false)
				!= B_OK || name == NULL) {
			continue;
		}
		if (strncmp(name, "ethernet-phy", 12) == 0
			|| strncmp(name, "phy@", 4) == 0) {
			read_phy_node(module, child, name, config);
		} else if (strncmp(name, "mdio", 4) == 0 && depth == 0) {
			// Linux style: ethernet { mdio { ethernet-phy@0 { ... } } }
			find_phy_node(module, child, config, depth + 1);
		}
	}
	// get_next_child_node() releases the previous node itself, including
	// when it runs out; only the node we stopped at is still referenced.
	if (config.phyNodeFound && child != NULL)
		gDeviceManager->put_node(child);
}


// #pragma mark - driver hooks


static float
dwmac_supports_device(device_node* parent)
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
	if (okay != NULL && strcmp(okay, "okay") != 0 && strcmp(okay, "ok") != 0) {
		const char* name = module->get_name(device);
		INFO("%s: status \"%s\", not attaching\n", name != NULL ? name : "?",
			okay);
		return 0.0f;
	}

	return 0.8f;
}


static status_t
dwmac_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DesignWare Ethernet QoS" }},
		// Initialise once at boot (one complete attach log), stay loaded.
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{}
	};

	return gDeviceManager->register_node(parent, DWMAC_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
read_config(device_node* fdtNode, DwmacConfig& config)
{
	fdt_device_module_info* module;
	fdt_device* device;
	if (!get_fdt(fdtNode, module, device))
		return B_ERROR;

	memset(&config, 0, sizeof(config));
	config.phyAddress = -1;
	config.jh7110Index = -1;
	YtPhyConfig& yt = config.yt;
	yt.rxcDlyEn = yt.rgmiiSwDr = yt.rgmiiSwDr2 = yt.rgmiiSwDrRxc = -1;
	yt.rxDelaySel = yt.txDelaySelFe = yt.txDelaySel = -1;
	yt.txInverted10 = yt.txInverted100 = yt.txInverted1000 = -1;

	uint64 base = 0;
	uint64 size = 0;
	if (!module->get_reg(device, 0, &base, &size) || size == 0) {
		ERROR("no reg property\n");
		return B_BAD_VALUE;
	}
	config.physBase = base;
	config.physSize = size;
	snprintf(config.name, sizeof(config.name), "ethernet@%" B_PRIx64, base);

	if (base == kJh7110Gmac0Base)
		config.jh7110Index = 0;
	else if (base == kJh7110Gmac1Base)
		config.jh7110Index = 1;
	config.isJh7110 = config.jh7110Index >= 0
		&& (has_compatible(module, device, "starfive,jh7110-eqos-5.20")
			|| has_compatible(module, device, "starfive,jh7110-dwmac"));

	// Interrupt 0 is "macirq" in both the vendor and the Linux DT.
	config.irqFromDt = module->get_interrupt(device, 0, NULL, &config.irq);
	if (!config.irqFromDt) {
		if (config.jh7110Index == 0)
			config.irq = kJh7110Gmac0Irq;
		else if (config.jh7110Index == 1)
			config.irq = kJh7110Gmac1Irq;
		else {
			ERROR("%s: no interrupts property\n", config.name);
			return B_BAD_VALUE;
		}
		ERROR("%s: no interrupts property, using JH7110 PLIC source %"
			B_PRIu64 "\n", config.name, config.irq);
	}

	int length = 0;
	const char* mode = (const char*)module->get_prop(device, "phy-mode",
		&length);
	if (mode == NULL)
		mode = (const char*)module->get_prop(device, "phy-connection-type",
			&length);
	strlcpy(config.phyMode, mode != NULL && length > 0 ? mode : "rgmii-id",
		sizeof(config.phyMode));

	const char* macProps[] = { "local-mac-address", "mac-address" };
	for (int i = 0; i < 2 && !config.hasMac; i++) {
		const void* mac = module->get_prop(device, macProps[i], &length);
		if (mac != NULL && length == 6) {
			memcpy(config.mac, mac, 6);
			config.hasMac = true;
			config.macSource = macProps[i];
		}
	}

	find_phy_node(module, fdtNode, config, 0);
	return B_OK;
}


struct dwmac_driver {
	device_node*	node;
	DwmacDevice*	device;
};


static status_t
dwmac_init_driver(device_node* node, void** cookie)
{
	device_node* fdtNode = gDeviceManager->get_parent_node(node);
	DwmacConfig config;
	status_t status = read_config(fdtNode, config);
	gDeviceManager->put_node(fdtNode);
	if (status != B_OK)
		return status;

	dwmac_driver* driver = new(std::nothrow) dwmac_driver;
	DwmacDevice* device = new(std::nothrow) DwmacDevice();
	if (driver == NULL || device == NULL) {
		delete driver;
		delete device;
		return B_NO_MEMORY;
	}

	status = device->Init(config);
	if (status != B_OK) {
		ERROR("%s: attach failed: %s\n", config.name, strerror(status));
		delete device;
		delete driver;
		return status;
	}

	driver->node = node;
	driver->device = device;
	*cookie = driver;
	return B_OK;
}


static void
dwmac_uninit_driver(void* cookie)
{
	dwmac_driver* driver = (dwmac_driver*)cookie;
	delete driver->device;
	delete driver;
}


static status_t
dwmac_register_child_devices(void* cookie)
{
	dwmac_driver* driver = (dwmac_driver*)cookie;
	int32 id = gDeviceManager->create_id(DWMAC_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/dwmac/%" B_PRId32, id);

	status_t status = gDeviceManager->publish_device(driver->node, name,
		DWMAC_DEVICE_MODULE_NAME);
	if (status != B_OK) {
		ERROR("%s: cannot publish /dev/%s: %s\n", driver->device->Name(),
			name, strerror(status));
		gDeviceManager->free_id(DWMAC_DEVICE_ID_GENERATOR, id);
	} else
		INFO("%s: published /dev/%s\n", driver->device->Name(), name);
	return status;
}


// #pragma mark - device hooks


static status_t
dwmac_init_device(void* driverCookie, void** deviceCookie)
{
	*deviceCookie = ((dwmac_driver*)driverCookie)->device;
	return B_OK;
}


static void
dwmac_uninit_device(void* deviceCookie)
{
}


static status_t
dwmac_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	DwmacDevice* device = (DwmacDevice*)deviceCookie;
	dwmac_handle* handle = new(std::nothrow) dwmac_handle;
	if (handle == NULL)
		return B_NO_MEMORY;
	handle->device = device;

	status_t status = device->Open((openMode & O_NONBLOCK) != 0);
	if (status != B_OK) {
		delete handle;
		return status;
	}
	*_cookie = handle;
	return B_OK;
}


static status_t
dwmac_close(void* cookie)
{
	((dwmac_handle*)cookie)->device->Close();
	return B_OK;
}


static status_t
dwmac_free(void* cookie)
{
	dwmac_handle* handle = (dwmac_handle*)cookie;
	handle->device->Free();
	delete handle;
	return B_OK;
}


static status_t
dwmac_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	return ((dwmac_handle*)cookie)->device->Read(buffer, _length);
}


static status_t
dwmac_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return ((dwmac_handle*)cookie)->device->Write(buffer, _length);
}


static status_t
dwmac_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return ((dwmac_handle*)cookie)->device->Control(op, buffer, length);
}


// #pragma mark - modules


static driver_module_info sDwmacDriver = {
	{
		DWMAC_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	dwmac_supports_device,
	dwmac_register_device,
	dwmac_init_driver,
	dwmac_uninit_driver,
	dwmac_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static device_module_info sDwmacDevice = {
	{
		DWMAC_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	dwmac_init_device,
	dwmac_uninit_device,
	NULL,	// remove

	dwmac_open,
	dwmac_close,
	dwmac_free,
	dwmac_read,
	dwmac_write,
	NULL,	// io
	dwmac_control,

	NULL,	// select
	NULL,	// deselect
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule },
	{}
};


_EXPORT module_info* modules[] = {
	(module_info*)&sDwmacDriver,
	(module_info*)&sDwmacDevice,
	NULL
};
