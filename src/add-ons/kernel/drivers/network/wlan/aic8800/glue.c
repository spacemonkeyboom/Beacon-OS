/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Copyright 2026, The Beacon Project.
 * Distributed under the terms of the MIT License.
 *
 * Glue for the AIC8800D80 SDIO driver (if_aic_sdio.c, GPL-2.0).
 * Modelled on the broadcomfullmac glue.
 */

#include <sys/haiku-module.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(aic8800, aic8800, sdio)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// No card interrupt handler yet: the driver polls the chip.
	return 0;
}
