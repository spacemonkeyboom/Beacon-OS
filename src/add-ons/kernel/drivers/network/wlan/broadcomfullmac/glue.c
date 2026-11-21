/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Copyright 2026, The Beacon Project.
 * Distributed under the terms of the MIT License.
 *
 * Glue for OpenBSD bwfm(4) on SDIO (Broadcom/Cypress/Synaptics FullMAC).
 * Modelled on the iaxwifi200 glue.
 */

#include <sys/haiku-module.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(broadcomfullmac, bwfm, sdio)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// The card interrupt comes from the SDIO bus manager's thread, not
	// through the compat layer's interrupt code.
	return 0;
}
