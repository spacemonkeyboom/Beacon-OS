/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * SDIO bus support for the FreeBSD/OpenBSD network compatibility layer
 * (freebsd_sdio.a). A driver built with
 * HAIKU_FBSD_WLAN_DRIVER_GLUE(name, driver, sdio) is probed once for every
 * SDIO function node that the MMC bus manager registered (see
 * headers/private/drivers/sdio_bus.h). During probe and attach the device's
 * ivars point to a struct haiku_sdio_device.
 */
#ifndef _FBSD_COMPAT_DEV_SDIO_HAIKU_SDIO_H_
#define _FBSD_COMPAT_DEV_SDIO_HAIKU_SDIO_H_


#include <sys/bus.h>


#ifdef __cplusplus
extern "C" {
#endif

struct sdio_bus_interface;
struct device_node;

struct haiku_sdio_device {
	struct device_node*			node;		// function node (acquired)
	struct sdio_bus_interface*	sdio;		// node's driver module
	void*						cookie;		// node's driver cookie

	uint16_t	vendor;			// "sdio/vendor"
	uint16_t	device;			// "sdio/device"
	uint8_t		function;		// "sdio/function", 1..7
	uint8_t		function_count;	// "sdio/function_count"
	uint8_t		interface_class;	// "sdio/class"
};


/* The SDIO function of a device attached by the compat layer (the ivars). */
static inline struct haiku_sdio_device*
haiku_sdio_get_device(device_t dev)
{
	return (struct haiku_sdio_device*)device_get_ivars(dev);
}

#ifdef __cplusplus
}
#endif


#endif	/* _FBSD_COMPAT_DEV_SDIO_HAIKU_SDIO_H_ */
