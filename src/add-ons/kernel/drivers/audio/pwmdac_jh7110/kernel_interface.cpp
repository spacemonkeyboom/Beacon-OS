/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Device manager glue for the JH7110 PWM-DAC driver: binds to the
 * pwmdac@100b0000 FDT node, publishes audio/hmulti/pwmdac/<n> and forwards
 * the device hooks to the multi_audio layer. The FDT attach pattern follows
 * Beacon's dwmac/dw_mmc drivers; the driver/device module split follows
 * Haiku's virtio_sound (MIT).
 *
 * This file contains no third-party code, but the add-on links the
 * GPL-2.0-only Pwmdac.cpp, so the linked add-on is GPL-2.0-only.
 */


#include "Pwmdac.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <bus/FDT.h>


device_manager_info* gDeviceManager;


// "starfive,pwmdac" is the vendor U-Boot 2021.10 DT (the DT U-Boot hands
// to Beacon via bootefi ${fdtcontroladdr}); the other is the Linux DT.
static const char* const kCompatibles[] = {
	"starfive,jh7110-pwmdac",
	"starfive,pwmdac",
	NULL
};


static bool
get_fdt(device_node* fdtNode, fdt_device_module_info*& module,
	fdt_device*& device)
{
	return gDeviceManager->get_driver(fdtNode, (driver_module_info**)&module,
		(void**)&device) == B_OK && device != NULL;
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
matches(fdt_device_module_info* module, fdt_device* device)
{
	int length = 0;
	const char* list = (const char*)module->get_prop(device, "compatible",
		&length);
	if (list == NULL)
		return false;
	for (int i = 0; kCompatibles[i] != NULL; i++) {
		if (string_list_contains(list, length, kCompatibles[i]))
			return true;
	}
	return false;
}


static bool
is_disabled(fdt_device_module_info* module, fdt_device* device)
{
	int length = 0;
	const char* status = (const char*)module->get_prop(device, "status",
		&length);
	return status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0;
}


// #pragma mark - driver hooks


static float
pwmdac_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	fdt_device_module_info* module;
	fdt_device* device;
	if (!get_fdt(parent, module, device) || !matches(module, device))
		return 0.0f;

	// The vendor U-Boot DT leaves pwmdac "disabled" (no board override),
	// although the Orange Pi RV and VisionFive 2 wire GPIO33/34 to the
	// headphone jack (the vendor Linux image registers
	// Starfive-PWMDAC-Sound-Card on this board). Attach anyway; Init()
	// logs it.
	if (is_disabled(module, device)) {
		const char* name = module->get_name(device);
		INFO("%s: DT status is not \"okay\"; attaching anyway (JH7110 "
			"boards route it to the 3.5 mm jack)\n",
			name != NULL ? name : "pwmdac");
	}
	return 0.8f;
}


static status_t
pwmdac_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "JH7110 PWM-DAC audio" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{}
	};
	return gDeviceManager->register_node(parent, PWMDAC_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


struct pwmdac_driver {
	device_node*	node;
	PwmdacDevice*	device;
};


static status_t
pwmdac_init_driver(device_node* node, void** cookie)
{
	device_node* fdtNode = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* module;
	fdt_device* fdt;
	PwmdacConfig config;
	memset(&config, 0, sizeof(config));
	bool ok = get_fdt(fdtNode, module, fdt);
	if (ok) {
		uint64 base = 0, size = 0;
		ok = module->get_reg(fdt, 0, &base, &size) && size != 0;
		config.physBase = base;
		config.dtDisabled = is_disabled(module, fdt);
	}
	gDeviceManager->put_node(fdtNode);
	if (!ok) {
		ERROR("no usable reg property\n");
		return B_BAD_VALUE;
	}
	snprintf(config.name, sizeof(config.name), "pwmdac@%" B_PRIxPHYSADDR,
		config.physBase);

	pwmdac_driver* driver = new(std::nothrow) pwmdac_driver;
	PwmdacDevice* device = new(std::nothrow) PwmdacDevice();
	if (driver == NULL || device == NULL) {
		delete driver;
		delete device;
		return B_NO_MEMORY;
	}
	status_t status = device->Init(config);
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
pwmdac_uninit_driver(void* cookie)
{
	pwmdac_driver* driver = (pwmdac_driver*)cookie;
	delete driver->device;
	delete driver;
}


static status_t
pwmdac_register_child_devices(void* cookie)
{
	pwmdac_driver* driver = (pwmdac_driver*)cookie;
	int32 id = gDeviceManager->create_id(PWMDAC_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	// Haiku's multi_audio media add-on scans /dev/audio/hmulti/.
	char name[64];
	snprintf(name, sizeof(name), "audio/hmulti/pwmdac/%" B_PRId32, id + 1);
	status_t status = gDeviceManager->publish_device(driver->node, name,
		PWMDAC_DEVICE_MODULE_NAME);
	if (status != B_OK) {
		ERROR("%s: cannot publish /dev/%s: %s\n", driver->device->Name(),
			name, strerror(status));
		gDeviceManager->free_id(PWMDAC_DEVICE_ID_GENERATOR, id);
	} else
		INFO("%s: published /dev/%s\n", driver->device->Name(), name);
	return status;
}


// #pragma mark - device hooks


static status_t
pwmdac_init_device(void* driverCookie, void** deviceCookie)
{
	*deviceCookie = ((pwmdac_driver*)driverCookie)->device;
	return B_OK;
}


static void
pwmdac_uninit_device(void* deviceCookie)
{
}


static status_t
pwmdac_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	PwmdacDevice* device = (PwmdacDevice*)deviceCookie;
	status_t status = device->Open();
	if (status != B_OK) {
		INFO("%s: open refused (already open)\n", device->Name());
		return status;
	}
	*_cookie = device;
	return B_OK;
}


static status_t
pwmdac_close(void* cookie)
{
	((PwmdacDevice*)cookie)->Close();
	return B_OK;
}


static status_t
pwmdac_free(void* cookie)
{
	((PwmdacDevice*)cookie)->DeleteBuffers();
	return B_OK;
}


static status_t
pwmdac_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
pwmdac_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
pwmdac_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return pwmdac_multi_control((PwmdacDevice*)cookie, op, buffer, length);
}


// #pragma mark - modules


static driver_module_info sPwmdacDriver = {
	{
		PWMDAC_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	pwmdac_supports_device,
	pwmdac_register_device,
	pwmdac_init_driver,
	pwmdac_uninit_driver,
	pwmdac_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static device_module_info sPwmdacDevice = {
	{
		PWMDAC_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	pwmdac_init_device,
	pwmdac_uninit_device,
	NULL,	// remove

	pwmdac_open,
	pwmdac_close,
	pwmdac_free,
	pwmdac_read,
	pwmdac_write,
	NULL,	// io
	pwmdac_control,

	NULL,	// select
	NULL,	// deselect
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


_EXPORT module_info* modules[] = {
	(module_info*)&sPwmdacDriver,
	(module_info*)&sPwmdacDevice,
	NULL
};
