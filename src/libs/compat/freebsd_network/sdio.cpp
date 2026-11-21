/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * SDIO bus glue for the FreeBSD/OpenBSD network compatibility layer.
 *
 * The compat layer drivers are legacy (devfs) drivers, so they cannot be
 * probed by the device manager like mmc_disk. Instead, init_hardware()
 * walks the device manager tree once, looking for the SDIO function nodes
 * that the MMC bus manager registers (B_DEVICE_BUS == "sdio", see
 * headers/private/drivers/sdio_bus.h), and offers each one to the driver's
 * probe method. The function node's own driver module implements
 * sdio_bus_interface; its cookie gives access to all functions of the card.
 */


extern "C" {
#include "device.h"

#include <compat/sys/haiku-module.h>
#include <compat/dev/sdio/haiku_sdio.h>
}

#include <stdlib.h>
#include <string.h>

#include <device_manager.h>
#include <sdio_bus.h>


//#define TRACE_SDIO
#ifdef TRACE_SDIO
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...)
#endif


static device_manager_info* sDeviceManager;
static int32 sSdioUsers;


static status_t
init_sdio()
{
	if (sSdioUsers++ > 0)
		return B_OK;

	status_t status = get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&sDeviceManager);
	if (status != B_OK) {
		sSdioUsers--;
		dprintf("%s: no device manager: %s\n", gDriverName, strerror(status));
	}
	return status;
}


extern "C" void
uninit_sdio()
{
	if (sSdioUsers == 0 || --sSdioUsers > 0)
		return;

	put_module(B_DEVICE_MANAGER_MODULE_NAME);
	sDeviceManager = NULL;
}


static void
free_compat_sdio_device(void* cookie)
{
	haiku_sdio_device* device = (haiku_sdio_device*)cookie;
	if (device->node != NULL && sDeviceManager != NULL)
		sDeviceManager->put_node(device->node);
	free(device);
}


static void
prepare_sdio_attach(void* cookie, device_t device)
{
	haiku_sdio_device* sdioDevice = (haiku_sdio_device*)cookie;

	struct root_device_softc* rootSoftc
		= (struct root_device_softc*)device->parent->softc;
	rootSoftc->sdio_dev = sdioDevice;
	device_set_ivars(device, sdioDevice);
}


/*!	Fills in \a info for an SDIO function node. Returns false if \a node is
	not one.
*/
static bool
get_sdio_function(device_node* node, haiku_sdio_device* info)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(node, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, SDIO_BUS_NAME) != 0) {
		return false;
	}

	memset(info, 0, sizeof(*info));
	if (sDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_ITEM,
			&info->function, false) != B_OK
		|| sDeviceManager->get_attr_uint16(node, SDIO_VENDOR_ITEM,
			&info->vendor, false) != B_OK
		|| sDeviceManager->get_attr_uint16(node, SDIO_DEVICE_ITEM,
			&info->device, false) != B_OK) {
		return false;
	}
	sDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_COUNT_ITEM,
		&info->function_count, false);
	sDeviceManager->get_attr_uint8(node, SDIO_CLASS_ITEM,
		&info->interface_class, false);

	info->node = node;
	return true;
}


/*!	Returns an extra reference to \a node. The public device manager API has
	no acquire call, so this walks the parent's children (which acquires the
	one returned).
*/
static device_node*
acquire_node(device_node* node)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	if (parent == NULL)
		return NULL;

	const device_attr kAny[] = { { NULL } };
	device_node* child = NULL;
	while (sDeviceManager->get_next_child_node(parent, kAny, &child) == B_OK) {
		if (child == node)
			break;
	}
	sDeviceManager->put_node(parent);
	return child;
}


/*!	Depth first walk over the device tree below \a parent. Every SDIO function
	node that one of \a drivers accepts is reported to the compat layer.
*/
static void
probe_sdio_children(device_node* parent, device_t root, driver_t* drivers[],
	bool& found, int depth)
{
	if (depth > 16)
		return;

	const device_attr kAny[] = { { NULL } };
	device_node* node = NULL;
	while (sDeviceManager->get_next_child_node(parent, kAny, &node) == B_OK) {
		haiku_sdio_device info;
		if (get_sdio_function(node, &info)) {
			TRACE("%s: SDIO function %u %04x:%04x\n", gDriverName,
				info.function, info.vendor, info.device);

			struct device device = {};
			device.parent = root;
			device.root = root;
			device_set_ivars(&device, &info);

			driver_t* driver = __haiku_probe_drivers(&device, drivers);
			if (driver != NULL) {
				driver_module_info* module = NULL;
				void* cookie = NULL;
				status_t status = sDeviceManager->get_driver(node, &module,
					&cookie);
				if (status != B_OK) {
					dprintf("%s: SDIO function %u (%04x:%04x): no bus "
						"interface (%s); the node must be registered with "
						"B_KEEP_DRIVER_LOADED\n", gDriverName, info.function,
						info.vendor, info.device, strerror(status));
				} else {
					haiku_sdio_device* compatDevice
						= (haiku_sdio_device*)malloc(sizeof(info));
					if (compatDevice != NULL) {
						*compatDevice = info;
						compatDevice->sdio = (sdio_bus_interface*)module;
						compatDevice->cookie = cookie;

						// keep a reference to the node for the device's
						// lifetime; released in free_compat_sdio_device()
						compatDevice->node = acquire_node(node);

						report_probed_device(root_device_softc::BUS_sdio,
							compatDevice, driver, prepare_sdio_attach,
							free_compat_sdio_device);
						found = true;
					}
				}
			}
		}

		probe_sdio_children(node, root, drivers, found, depth + 1);
	}
}


extern "C" status_t
_fbsd_init_hardware_sdio(driver_t* drivers[])
{
	status_t status = init_sdio();
	if (status != B_OK)
		return status;

	device_t root;
	status = init_root_device(&root, root_device_softc::BUS_sdio);
	if (status != B_OK) {
		uninit_sdio();
		return status;
	}

	bool found = false;
	device_node* rootNode = sDeviceManager->get_root_node();
	if (rootNode != NULL) {
		probe_sdio_children(rootNode, root, drivers, found, 0);
		sDeviceManager->put_node(rootNode);
	}

	device_delete_child(NULL, root);

	if (found)
		return B_OK;

	uninit_sdio();
	return B_NOT_SUPPORTED;
}
