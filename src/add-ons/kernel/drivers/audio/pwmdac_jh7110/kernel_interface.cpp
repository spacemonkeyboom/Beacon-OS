/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Device manager glue for the JH7110 audio driver (jack + HDMI): binds to
 * the pwmdac@100b0000 FDT node, publishes audio/hmulti/pwmdac/<n> (one
 * device for both outputs) and forwards the device hooks to the
 * multi_audio layer. The FDT attach pattern follows
 * Beacon's dwmac/dw_mmc drivers; the driver/device module split follows
 * Haiku's virtio_sound (MIT).
 *
 * This file contains no third-party code, but the add-on links the
 * GPL-2.0-only Pwmdac.cpp and I2sHdmi.cpp, so the linked add-on is
 * GPL-2.0-only.
 */


#include "Jh7110Audio.h"

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


/*
 * Beacon: true on the VisionFive 2 Lite (JH7110S). Its vendor DT disables
 * pwmdac, and GPIO33 is the on-board Wi-Fi's WL_REG_ON, not the jack's left
 * channel: muxing PWMDAC onto GPIO33/34 there switched the AIC8800D80 off
 * (vf2lite-13, 2026-11-26).
 */
static bool
root_is_lite(device_node* node)
{
	device_node* root = NULL;
	device_node* current = gDeviceManager->get_parent_node(node);
	while (current != NULL) {
		const char* bus;
		if (gDeviceManager->get_attr_string(current, B_DEVICE_BUS, &bus,
				false) != B_OK || strcmp(bus, "fdt") != 0) {
			break;
		}
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
		int length = 0;
		const char* list = (const char*)module->get_prop(device,
			"compatible", &length);
		lite = list != NULL
			&& (string_list_contains(list, length, "starfive,jh7110s")
				|| string_list_contains(list, length,
					"starfive,visionfive-2-lite")
				|| string_list_contains(list, length,
					"starfive,visionfive-2-lite-emmc"));
	}
	gDeviceManager->put_node(root);
	return lite;
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
			{ .string = "JH7110 audio (jack + HDMI)" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{}
	};
	return gDeviceManager->register_node(parent, JH7110_AUDIO_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


struct pwmdac_driver {
	device_node*	node;
	Jh7110AudioDevice*	device;
};


static status_t
pwmdac_init_driver(device_node* node, void** cookie)
{
	device_node* fdtNode = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* module;
	fdt_device* fdt;
	Jh7110AudioConfig config;
	memset(&config, 0, sizeof(config));
	bool ok = get_fdt(fdtNode, module, fdt);
	if (ok) {
		uint64 base = 0, size = 0;
		ok = module->get_reg(fdt, 0, &base, &size) && size != 0;
		config.pwmdacBase = base;
		config.dtDisabled = is_disabled(module, fdt);
	}
	config.noJack = root_is_lite(fdtNode);
	gDeviceManager->put_node(fdtNode);
	if (!ok) {
		ERROR("no usable reg property\n");
		return B_BAD_VALUE;
	}
	snprintf(config.name, sizeof(config.name), "pwmdac@%" B_PRIxPHYSADDR,
		config.pwmdacBase);

	pwmdac_driver* driver = new(std::nothrow) pwmdac_driver;
	Jh7110AudioDevice* device = new(std::nothrow) Jh7110AudioDevice();
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
	int32 id = gDeviceManager->create_id(JH7110_AUDIO_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	// Haiku's multi_audio media add-on scans /dev/audio/hmulti/.
	char name[64];
	snprintf(name, sizeof(name), "audio/hmulti/pwmdac/%" B_PRId32, id + 1);
	status_t status = gDeviceManager->publish_device(driver->node, name,
		JH7110_AUDIO_DEVICE_MODULE_NAME);
	if (status != B_OK) {
		ERROR("%s: cannot publish /dev/%s: %s\n", driver->device->Name(),
			name, strerror(status));
		gDeviceManager->free_id(JH7110_AUDIO_DEVICE_ID_GENERATOR, id);
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


struct audio_open_cookie {
	Jh7110AudioDevice*	device;
};


static status_t
pwmdac_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	// Any number of openers; the first streaming ioctl claims the stream
	// (see multi.cpp), so control-only tools work beside the media server.
	audio_open_cookie* cookie = new(std::nothrow) audio_open_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->device = (Jh7110AudioDevice*)deviceCookie;
	*_cookie = cookie;
	return B_OK;
}


static status_t
pwmdac_close(void* _cookie)
{
	audio_open_cookie* cookie = (audio_open_cookie*)_cookie;
	cookie->device->ReleaseStream(cookie);
	return B_OK;
}


static status_t
pwmdac_free(void* _cookie)
{
	audio_open_cookie* cookie = (audio_open_cookie*)_cookie;
	cookie->device->FreeStream(cookie);
	delete cookie;
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
pwmdac_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	audio_open_cookie* cookie = (audio_open_cookie*)_cookie;
	return jh7110_audio_multi_control(cookie->device, cookie, op, buffer,
		length);
}


// #pragma mark - modules


static driver_module_info sPwmdacDriver = {
	{
		JH7110_AUDIO_DRIVER_MODULE_NAME,
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


static device_module_info sJh7110AudioDevice = {
	{
		JH7110_AUDIO_DEVICE_MODULE_NAME,
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
	(module_info*)&sJh7110AudioDevice,
	NULL
};
