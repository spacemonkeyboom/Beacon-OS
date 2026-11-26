/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * OpenBSD sdmmc(4) SDIO I/O API (the subset bwfm(4) uses) on top of the
 * Beacon SDIO bus interface (headers/private/drivers/sdio_bus.h).
 *
 * OpenBSD's sdmmc_io.c builds CMD52/CMD53 itself; here the bus manager does
 * that, including the block/byte splitting of the _region_1/_multi_1 calls,
 * so this file only maps calls, errors and byte order.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/errno.h>

#include <dev/sdio/haiku_sdio.h>

#include <dev/sdmmc/sdmmcvar.h>

#include <KernelExport.h>
#include <sdio_bus.h>


#ifndef SDMMC_TAG
#define SDMMC_TAG	"bwfm"
#endif

/* Only report the first few I/O errors, a dead card would flood the log. */
static int sSdioErrorReports = 0;
#define SDMMC_MAX_ERROR_REPORTS	32


static int
sdmmc_haiku_error(struct sdmmc_function *sf, const char *what, int reg,
	status_t status)
{
	if (status == B_OK) {
		sf->sc->sc_io_timeouts = 0;
		return 0;
	}
	if (status == B_TIMED_OUT)
		sf->sc->sc_io_timeouts++;

	if (sSdioErrorReports < SDMMC_MAX_ERROR_REPORTS) {
		sSdioErrorReports++;
		dprintf("%s: SDIO %s fn %d reg 0x%x failed: %s\n", SDMMC_TAG, what,
			sf->number, reg, strerror(status));
	}

	if (status == B_TIMED_OUT)
		return ETIMEDOUT;
	if (status == B_BAD_VALUE)
		return EINVAL;
	if (status == B_NOT_SUPPORTED)
		return ENODEV;
	return EIO;
}


#define SDIO(sf)	((sf)->sc->sc_sdio)
#define COOKIE(sf)	((sf)->sc->sc_sdio_cookie)


int
sdmmc_haiku_attach(struct sdmmc_softc *sc, struct haiku_sdio_device *dev)
{
	struct sdio_card_info *info;
	int i;

	memset(sc, 0, sizeof(*sc));
	rw_init(&sc->sc_lock, "sdmmc");
	sc->sc_sdio = dev->sdio;
	sc->sc_sdio_cookie = dev->cookie;
	sc->sc_haiku = dev;

	info = malloc(sizeof(*info), M_TEMP, M_WAITOK | M_ZERO);
	if (info == NULL)
		return ENOMEM;
	info->size = sizeof(*info);

	if (sc->sc_sdio->get_card_info(sc->sc_sdio_cookie, info) != B_OK) {
		/* fall back to the node attributes */
		info->function_count = dev->function_count;
		info->vendor = dev->vendor;
		info->device = dev->device;
		info->functions[dev->function].vendor = dev->vendor;
		info->functions[dev->function].device = dev->device;
	}

	sc->sc_function_count = info->function_count;
	if (sc->sc_function_count >= SDMMC_MAX_FUNCTIONS)
		sc->sc_function_count = SDMMC_MAX_FUNCTIONS - 1;

	for (i = 0; i <= sc->sc_function_count; i++) {
		struct sdmmc_function *sf = &sc->sc_functions[i];
		sf->sc = sc;
		sf->number = i;
		sf->present = 1;
		sf->cur_blklen = info->functions[i].block_size;
		/* bwfm matches on the common (function 0) CIS */
		sf->cis.manufacturer = i == 0 ? info->vendor
			: info->functions[i].vendor;
		sf->cis.product = i == 0 ? info->device : info->functions[i].device;
		sf->cis.function = info->functions[i].interface_class;
	}
	sc->sc_fn0 = &sc->sc_functions[0];

	dprintf("%s: SDIO card %04x:%04x, %d functions, %u-bit, %u kHz%s\n",
		SDMMC_TAG, info->vendor, info->device, sc->sc_function_count,
		info->bus_width, info->clock_khz,
		info->high_speed ? ", high speed" : "");

	free(info, M_TEMP, sizeof(*info));
	return 0;
}


void
sdmmc_haiku_detach(struct sdmmc_softc *sc)
{
	int i;

	for (i = 1; i <= sc->sc_function_count; i++) {
		if (sc->sc_intr_enabled & (1 << i))
			sdmmc_intr_disable(&sc->sc_functions[i]);
	}
	sc->sc_intr_fn = NULL;
}


/* #pragma mark - CMD52 */


uint8_t
sdmmc_io_read_1(struct sdmmc_function *sf, int reg)
{
	uint8_t value = 0xff;
	sdmmc_haiku_error(sf, "read_1", reg,
		SDIO(sf)->read_byte(COOKIE(sf), sf->number, reg, &value));
	return value;
}


void
sdmmc_io_write_1(struct sdmmc_function *sf, int reg, uint8_t data)
{
	sdmmc_haiku_error(sf, "write_1", reg,
		SDIO(sf)->write_byte(COOKIE(sf), sf->number, reg, data));
}


/* #pragma mark - CMD53 */


uint16_t
sdmmc_io_read_2(struct sdmmc_function *sf, int reg)
{
	uint8_t buf[2] = { 0xff, 0xff };
	sdmmc_haiku_error(sf, "read_2", reg,
		SDIO(sf)->rw_extended(COOKIE(sf), sf->number, reg, false, buf, 2,
			SDIO_RW_INCREMENT));
	return buf[0] | (buf[1] << 8);
}


void
sdmmc_io_write_2(struct sdmmc_function *sf, int reg, uint16_t data)
{
	uint8_t buf[2] = { data & 0xff, data >> 8 };
	sdmmc_haiku_error(sf, "write_2", reg,
		SDIO(sf)->rw_extended(COOKIE(sf), sf->number, reg, true, buf, 2,
			SDIO_RW_INCREMENT));
}


uint32_t
sdmmc_io_read_4(struct sdmmc_function *sf, int reg)
{
	uint8_t buf[4] = { 0xff, 0xff, 0xff, 0xff };
	sdmmc_haiku_error(sf, "read_4", reg,
		SDIO(sf)->rw_extended(COOKIE(sf), sf->number, reg, false, buf, 4,
			SDIO_RW_INCREMENT));
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
		| ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}


void
sdmmc_io_write_4(struct sdmmc_function *sf, int reg, uint32_t data)
{
	uint8_t buf[4] = { data & 0xff, (data >> 8) & 0xff, (data >> 16) & 0xff,
		data >> 24 };
	sdmmc_haiku_error(sf, "write_4", reg,
		SDIO(sf)->rw_extended(COOKIE(sf), sf->number, reg, true, buf, 4,
			SDIO_RW_INCREMENT));
}


/* fixed address (FIFO port) */
int
sdmmc_io_read_multi_1(struct sdmmc_function *sf, int reg, u_char *data,
	int datalen)
{
	if (datalen <= 0)
		return 0;
	return sdmmc_haiku_error(sf, "read_multi", reg,
		SDIO(sf)->read(COOKIE(sf), sf->number, reg, data, datalen, 0));
}


int
sdmmc_io_write_multi_1(struct sdmmc_function *sf, int reg, u_char *data,
	int datalen)
{
	if (datalen <= 0)
		return 0;
	return sdmmc_haiku_error(sf, "write_multi", reg,
		SDIO(sf)->write(COOKIE(sf), sf->number, reg, data, datalen, 0));
}


/* incrementing address (memory window) */
int
sdmmc_io_read_region_1(struct sdmmc_function *sf, int reg, u_char *data,
	int datalen)
{
	if (datalen <= 0)
		return 0;
	return sdmmc_haiku_error(sf, "read_region", reg,
		SDIO(sf)->read(COOKIE(sf), sf->number, reg, data, datalen,
			SDIO_RW_INCREMENT));
}


int
sdmmc_io_write_region_1(struct sdmmc_function *sf, int reg, u_char *data,
	int datalen)
{
	if (datalen <= 0)
		return 0;
	return sdmmc_haiku_error(sf, "write_region", reg,
		SDIO(sf)->write(COOKIE(sf), sf->number, reg, data, datalen,
			SDIO_RW_INCREMENT));
}


/* #pragma mark - function control */


void
sdmmc_io_set_blocklen(struct sdmmc_function *sf, unsigned int blklen)
{
	if (sdmmc_haiku_error(sf, "set_blocklen", (int)blklen,
			SDIO(sf)->set_block_size(COOKIE(sf), sf->number, blklen)) == 0) {
		struct sdio_card_info *info = malloc(sizeof(*info), M_TEMP,
			M_WAITOK | M_ZERO);
		sf->cur_blklen = blklen;
		if (info != NULL) {
			/* blklen 0 means "default"; read back what the bus chose */
			info->size = sizeof(*info);
			if (SDIO(sf)->get_card_info(COOKIE(sf), info) == B_OK
				&& info->functions[sf->number].block_size != 0)
				sf->cur_blklen = info->functions[sf->number].block_size;
			free(info, M_TEMP, sizeof(*info));
		}
	}
}


int
sdmmc_io_function_ready(struct sdmmc_function *sf)
{
	uint8_t value = 0;

	if (sf->number == 0)
		return 1;
	if (SDIO(sf)->read_byte(COOKIE(sf), 0, SDIO_CCCR_IO_READY, &value)
			!= B_OK)
		return 0;
	return (value & (1 << sf->number)) != 0;
}


int
sdmmc_io_function_enable(struct sdmmc_function *sf)
{
	if (sf->number == 0)
		return 0;	/* FN0 is always enabled */

	/* OpenBSD polls IORx for up to 5 seconds. */
	return sdmmc_haiku_error(sf, "function_enable", SDIO_CCCR_IO_ENABLE,
		SDIO(sf)->enable_function(COOKIE(sf), sf->number, 5000000));
}


void
sdmmc_io_function_disable(struct sdmmc_function *sf)
{
	if (sf->number == 0)
		return;
	sdmmc_haiku_error(sf, "function_disable", SDIO_CCCR_IO_ENABLE,
		SDIO(sf)->disable_function(COOKIE(sf), sf->number));
}


/*
 * Beacon: power-cycle the card through the host's reset GPIO and enumerate
 * it again. Afterwards functions are disabled and block sizes are at their
 * defaults; the caller sets them up again.
 */
int
sdmmc_io_reset_card(struct sdmmc_softc *sc)
{
	status_t status;

	if (sc->sc_sdio->reset_card == NULL)
		return ENODEV;
	status = sc->sc_sdio->reset_card(sc->sc_sdio_cookie);
	if (status == B_OK) {
		sc->sc_io_timeouts = 0;
		sSdioErrorReports = 0;
		return 0;
	}
	return sdmmc_haiku_error(sc->sc_fn0 != NULL ? sc->sc_fn0
		: &sc->sc_functions[0], "reset_card", 0, status);
}


/* #pragma mark - card interrupt */


static void
sdmmc_haiku_card_interrupt(void *data)
{
	struct sdmmc_softc *sc = data;
	int (*fn)(void *) = sc->sc_intr_fn;

	if (fn == NULL)
		return;

	mtx_lock(&Giant);
	fn(sc->sc_intr_arg);
	mtx_unlock(&Giant);
}


void *
sdmmc_intr_establish(struct sdmmc_softc *sc, int (*fun)(void *), void *arg,
	const char *name)
{
	if (sc->sc_intr_fn != NULL)
		return NULL;

	sc->sc_intr_arg = arg;
	strlcpy(sc->sc_intr_name, name != NULL ? name : SDMMC_TAG,
		sizeof(sc->sc_intr_name));
	sc->sc_intr_fn = fun;
	return sc;
}


void
sdmmc_intr_disestablish(void *cookie)
{
	struct sdmmc_softc *sc = cookie;

	sdmmc_haiku_detach(sc);
}


void
sdmmc_intr_enable(struct sdmmc_function *sf)
{
	struct sdmmc_softc *sc = sf->sc;

	if (sf->number == 0 || (sc->sc_intr_enabled & (1 << sf->number)) != 0)
		return;

	if (sdmmc_haiku_error(sf, "install_interrupt_handler", 0,
			SDIO(sf)->install_interrupt_handler(COOKIE(sf), sf->number,
				sdmmc_haiku_card_interrupt, sc)) == 0) {
		sc->sc_intr_enabled |= 1 << sf->number;
	}
}


void
sdmmc_intr_disable(struct sdmmc_function *sf)
{
	struct sdmmc_softc *sc = sf->sc;

	if ((sc->sc_intr_enabled & (1 << sf->number)) == 0)
		return;

	SDIO(sf)->remove_interrupt_handler(COOKIE(sf), sf->number);
	sc->sc_intr_enabled &= ~(1 << sf->number);
}
