/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Beacon: AICSemi AIC8800D80 FullMAC Wi-Fi over SDIO.
 *
 * Ported from the Linux aic8800 driver as submitted to linux-wireless
 * (RFC v4, 2026-08-29; see THIRD_PARTY.md for the exact sources):
 *   aic8800_bsp/aicsdio.c          SDIO v3 function init, message TX with
 *                                  flow control, interrupt status, RX read
 *   aic8800_bsp/aicsdio_txrxif.c   RX aggregate parser
 *   aic8800_bsp/aic_bsp_driver.c   command framing, DBG_* messages,
 *                                  firmware upload, start from bootrom
 *   aic8800_bsp/aic_chip_8800d80.c, aic8800d80_compat.c
 *                                  D80 revision check, patch config
 *   aic8800_fdrv/rwnx_msg_tx.c     MM_RESET / MM_VERSION / MM_GET_MAC_ADDR
 *
 * Copyright (C) RivieraWaves 2012-2019
 * Copyright (C) 2020-2024 AIC semiconductor.
 * Copyright 2026, The Beacon Project (Haiku port: OpenBSD net80211 glue,
 *   Beacon SDIO bus, polled command path).
 *
 * The firmware is NOT part of Beacon. The driver loads it at run time from
 * /boot/system/non-packaged/data/firmware/aic8800/ (or the packaged
 * data/firmware/aic8800/); without it the driver says so and does not attach.
 *
 * Milestone 1 ("fw up"): upload and start the firmware, read the MAC address
 * and firmware version. Scanning and joining are not implemented yet.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdio/haiku_sdio.h>
#include <dev/sdmmc/sdmmcvar.h>

#include <KernelExport.h>
#include <OS.h>
#include <sdio_bus.h>

#include "lmac_msg.h"


#define AIC_NAME			"aic8800"

#define AIC_VENDOR_AICSEMI	0xc8a1
#define AIC_DEVICE_D80		0x0082

/* aicsdio.h: SDIO v3 function 1 registers (D80) */
#define AIC_REG_INTR_ENABLE		0x00
#define AIC_REG_INTR_PENDING	0x01	/* "sleep_reg" */
#define AIC_REG_INTR_TO_DEVICE	0x02	/* "wakeup_reg" */
#define AIC_REG_FLOW_CTRL_Q1	0x03
#define AIC_REG_MISC_INT_STATUS	0x04
#define AIC_REG_BYTEMODE_LEN	0x05
#define AIC_REG_BYTEMODE_ENABLE	0x07
#define AIC_REG_RD_FIFO			0x0f
#define AIC_REG_WR_FIFO			0x10

#define AIC_OTHER_INTERRUPT		0x80	/* SDIO_OTHER_INTERRUPT */
#define AIC_INTSTATUS_BYTEMODE	120

/* function 0 vendor registers written by aicwf_sdiov3_func_init() */
#define AIC_FN0_F0_IOPAD_CTRL	0xf0
#define AIC_FN0_F1_DELAY1		0xf1
#define AIC_FN0_F2				0xf2
#define AIC_FN0_F8_DELAY2		0xf8
#define AIC_SDIOCLK_FREE_RUNNING	0x40
#define AIC_CCCR_INT_ENABLE		0x04

#define AIC_BLOCK_SIZE			512		/* SDIOWIFI_FUNC_BLOCKSIZE */
#define AIC_FW_BUFFER_SIZE		1536	/* BUFFER_SIZE */
#define AIC_TAIL_LEN			4
#define AIC_FLOW_CTRL_RETRY		50
#define AIC_TXBUF_SIZE			2048
#define AIC_RXBUF_SIZE			(127 * AIC_BLOCK_SIZE)
#define AIC_RX_HWHDR_LEN		60		/* RX_HWHRD_LEN */

#define AIC_SDIO_TYPE_CFG		0x10
#define AIC_SDIO_TYPE_CMD_RSP	0x11

#define AIC_CMD_TIMEOUT_MS		6000	/* RWNX_80211_CMD_TIMEOUT_MS */

#define AIC_RAM_FMAC_FW_ADDR	0x00120000
#define AIC_CHIP_REV_U02		3

/* aic8800d80_compat.c */
#define AIC_PATCH_MAGIC			0x48435450	/* "PTCH" */
#define AIC_PATCH_MAGIC_2		0x50544348	/* "HCTP" */
#define AIC_PATCH_OFS_MAGIC		0x00
#define AIC_PATCH_OFS_PAIR_START	0x04
#define AIC_PATCH_OFS_MAGIC_2	0x08
#define AIC_PATCH_OFS_PAIR_COUNT	0x0c
#define AIC_PATCH_OFS_BLOCK_SIZE	0x2c	/* after 4 dst + 4 src words */


struct aic_softc {
	struct ieee80211com		 sc_ic;
	device_t				 sc_dev;
	struct sdmmc_softc		 sc_sdmmc;
	struct sdmmc_function	*sc_sf;

	int (*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);

	uint8_t					*sc_txbuf;
	uint8_t					*sc_rxbuf;

	/* the one outstanding synchronous command */
	uint16_t				 sc_cfm_id;
	uint8_t					*sc_cfm_buf;
	size_t					 sc_cfm_len;
	int						 sc_cfm_done;

	int						 sc_unsolicited_logged;
	int						 sc_data_frames;
	int						 sc_cccr_intr;

	uint32_t				 sc_chip_word;
	uint32_t				 sc_fw_version;
	uint32_t				 sc_bootstatus;
	struct mm_version_cfm	 sc_version;
	int						 sc_fw_up;
};


#define SDIO_OPS(sc)	((sc)->sc_sdmmc.sc_sdio)
#define SDIO_COOKIE(sc)	((sc)->sc_sdmmc.sc_sdio_cookie)


static int
aic_status_to_errno(status_t status)
{
	if (status == B_OK)
		return 0;
	if (status == B_TIMED_OUT)
		return ETIMEDOUT;
	if (status == B_BAD_VALUE)
		return EINVAL;
	return EIO;
}


static int
aic_read_1(struct aic_softc *sc, int fn, uint32_t reg, uint8_t *value)
{
	status_t status = SDIO_OPS(sc)->read_byte(SDIO_COOKIE(sc), fn, reg,
		value);
	if (status != B_OK)
		printf("%s: CMD52 read fn%d 0x%x failed: %s\n", AIC_NAME, fn,
			(unsigned)reg, strerror(status));
	return aic_status_to_errno(status);
}


static int
aic_write_1(struct aic_softc *sc, int fn, uint32_t reg, uint8_t value)
{
	status_t status = SDIO_OPS(sc)->write_byte(SDIO_COOKIE(sc), fn, reg,
		value);
	if (status != B_OK)
		printf("%s: CMD52 write fn%d 0x%x = 0x%x failed: %s\n", AIC_NAME, fn,
			(unsigned)reg, value, strerror(status));
	return aic_status_to_errno(status);
}


/* aicsdio.c crc8_ponl_107(): CRC-8, polynomial x^8 + x^2 + x + 1 */
static uint8_t
aic_crc8_ponl_107(const uint8_t *p, int size)
{
	uint8_t crc = 0;
	int i;

	while (size-- > 0) {
		for (i = 0x80; i > 0; i /= 2) {
			if (crc & 0x80) {
				crc *= 2;
				crc ^= 0x07;
			} else
				crc *= 2;
			if (*p & i)
				crc ^= 0x07;
		}
		p++;
	}
	return crc;
}


/* #pragma mark - SDIO function setup */


/*
 * vf2lite-12 (2026-11-26): the card enumerated fine at boot, but ~10 s
 * later, when this driver attached, it no longer answered any CMD52
 * (response timeouts). The Linux BSP powers the chip on and rescans the
 * host right before it uses it (aicbsp_platform_power_on), so do the same
 * when the card is silent: power-cycle it through WL_REG_ON and enumerate it
 * again (the bus manager's reset_card), then retry.
 */
static int
aic_card_check(struct aic_softc *sc)
{
	uint8_t cccr = 0;
	status_t status;
	int attempt;

	for (attempt = 0; ; attempt++) {
		status = SDIO_OPS(sc)->read_byte(SDIO_COOKIE(sc), 0, 0x00, &cccr);
		if (status == B_OK) {
			printf("%s: card answers (CCCR revision 0x%02x)%s, %lld ms "
				"after boot\n", AIC_NAME, cccr,
				attempt > 0 ? " after a power cycle" : "",
				(long long)(system_time() / 1000));
			return 0;
		}
		printf("%s: card does not answer CMD52 (%s), %lld ms after boot\n",
			AIC_NAME, strerror(status), (long long)(system_time() / 1000));
		if (attempt >= 2)
			return EIO;
		printf("%s: power-cycling the card and enumerating it again "
			"(try %d)\n", AIC_NAME, attempt + 1);
		if (sdmmc_io_reset_card(&sc->sc_sdmmc) != 0)
			printf("%s: card reset failed\n", AIC_NAME);
	}
}


/* aicwf_sdiov3_func_init() */
static int
aic_sdio_func_init(struct aic_softc *sc)
{
	int error;

	sdmmc_io_set_blocklen(sc->sc_sf, AIC_BLOCK_SIZE);
	if (sc->sc_sf->cur_blklen != AIC_BLOCK_SIZE) {
		printf("%s: could not set the block size (%u)\n", AIC_NAME,
			sc->sc_sf->cur_blklen);
		return EIO;
	}
	error = sdmmc_io_function_enable(sc->sc_sf);
	if (error != 0) {
		printf("%s: could not enable function 1\n", AIC_NAME);
		return error;
	}

	if ((error = aic_write_1(sc, 0, AIC_FN0_F2, 0x7f)) != 0)
		return error;
	/* sample phase 0 (not DDR50), SDIO clock free running */
	if ((error = aic_write_1(sc, 0, AIC_FN0_F0_IOPAD_CTRL,
			AIC_SDIOCLK_FREE_RUNNING)) != 0)
		return error;
	if ((error = aic_write_1(sc, 0, AIC_FN0_F8_DELAY2, 0x00)) != 0)
		return error;
	if ((error = aic_write_1(sc, 0, AIC_FN0_F1_DELAY1, 0x00)) != 0)
		return error;
	snooze(2000);

	/* 1: no byte mode */
	if ((error = aic_write_1(sc, 1, AIC_REG_BYTEMODE_ENABLE, 1)) != 0)
		return error;

	/*
	 * aicwf_sdio_bus_start(): the chip's own interrupt enable. The CCCR
	 * interrupt enable (need_func0_intr) is left off: this milestone polls
	 * MISC_INT_STATUS and has no card interrupt handler yet.
	 */
	return aic_write_1(sc, 1, AIC_REG_INTR_ENABLE, 0x07);
}


/* #pragma mark - message transport */


/* aicwf_sdio_flow_ctrl(): number of free firmware buffers, or <= 0 */
static int
aic_flow_ctrl(struct aic_softc *sc)
{
	uint8_t fc = 0;
	int count;

	for (count = 0; ; count++) {
		if (aic_read_1(sc, 1, AIC_REG_FLOW_CTRL_Q1, &fc) != 0)
			return -1;
		if (fc != 0)
			return fc;
		if (count >= AIC_FLOW_CTRL_RETRY)
			return 0;
		if (count < 30)
			spin(40);
		else if (count < 40)
			snooze(1000);
		else
			snooze(10000);
	}
}


/*
 * rwnx_set_cmd_tx() + aicwf_sdio_tx_msg(): frame one LMAC message and write
 * it to the function 1 write FIFO.
 */
static int
aic_send_msg(struct aic_softc *sc, uint16_t id, uint16_t dest,
	const void *param, uint16_t param_len)
{
	uint8_t *b = sc->sc_txbuf;
	uint16_t len = 8 + param_len;	/* struct lmac_msg + params */
	size_t payload, total;
	int buffers, retry;
	status_t status;

	if ((size_t)len + 8 + AIC_TAIL_LEN + AIC_BLOCK_SIZE > AIC_TXBUF_SIZE)
		return EINVAL;

	memset(b, 0, AIC_TXBUF_SIZE);
	b[0] = (len + 4) & 0xff;
	b[1] = ((len + 4) >> 8) & 0x0f;
	b[2] = AIC_SDIO_TYPE_CMD_RSP;
	b[3] = aic_crc8_ponl_107(b, 3);
	/* b[4..7]: a dummy word */
	b[8] = id & 0xff;
	b[9] = id >> 8;
	b[10] = dest & 0xff;
	b[11] = dest >> 8;
	b[12] = DRV_TASK_ID & 0xff;
	b[13] = DRV_TASK_ID >> 8;
	b[14] = param_len & 0xff;
	b[15] = param_len >> 8;
	if (param_len > 0)
		memcpy(b + 16, param, param_len);

	payload = roundup((size_t)len + 8, 4);
	if (payload % AIC_BLOCK_SIZE != 0) {
		/* "link tail is necessary" */
		payload += AIC_TAIL_LEN;
		total = (payload / AIC_BLOCK_SIZE + 1) * AIC_BLOCK_SIZE;
	} else
		total = payload;

	buffers = aic_flow_ctrl(sc);
	for (retry = 0; (buffers <= 0
			|| total > (size_t)buffers * AIC_FW_BUFFER_SIZE) && retry < 10;
			retry++)
		buffers = aic_flow_ctrl(sc);
	if (buffers <= 0 || total > (size_t)buffers * AIC_FW_BUFFER_SIZE) {
		printf("%s: no firmware buffer for message 0x%04x (%d free, need "
			"%u bytes)\n", AIC_NAME, id, buffers, (unsigned)total);
		return EBUSY;
	}

	status = SDIO_OPS(sc)->write(SDIO_COOKIE(sc), 1, AIC_REG_WR_FIFO, b,
		total, 0);
	if (status != B_OK) {
		printf("%s: write FIFO (%u bytes) failed: %s\n", AIC_NAME,
			(unsigned)total, strerror(status));
		return aic_status_to_errno(status);
	}
	return 0;
}


/* rwnx_rx_handle_msg() / cmd_mgr_msgind(): one firmware-to-host message */
static void
aic_rx_msg(struct aic_softc *sc, const uint8_t *msg, size_t avail)
{
	uint16_t id, param_len;

	/* struct ipc_e2a_msg: id, dest, src, param_len, pattern, param[] */
	if (avail < 12)
		return;
	id = msg[0] | (msg[1] << 8);
	param_len = msg[6] | (msg[7] << 8);
	if ((size_t)param_len + 12 > avail)
		param_len = avail - 12;

	if (sc->sc_cfm_buf != NULL || sc->sc_cfm_id != 0) {
		if (!sc->sc_cfm_done && id == sc->sc_cfm_id) {
			if (sc->sc_cfm_buf != NULL)
				memcpy(sc->sc_cfm_buf, msg + 12,
					MIN((size_t)param_len, sc->sc_cfm_len));
			sc->sc_cfm_done = 1;
			return;
		}
	}

	if (sc->sc_unsolicited_logged < 16) {
		sc->sc_unsolicited_logged++;
		printf("%s: firmware message 0x%04x (task %u, index %u), %u bytes\n",
			AIC_NAME, id, MSG_T(id), MSG_I(id), param_len);
	}
}


/*
 * aicwf_sdio_hal_irqhandler() (non-func2 path) + aicwf_sdio_readframes() +
 * aicwf_process_rxframes(). Returns 1 when something was read, 0 when the
 * chip had nothing, or a negative value on an I/O error.
 */
static int
aic_rx_poll(struct aic_softc *sc)
{
	uint8_t intstatus = 0, value;
	size_t len, adjust;
	uint8_t *p, *end;
	status_t status;

	if (aic_read_1(sc, 1, AIC_REG_MISC_INT_STATUS, &intstatus) != 0)
		return -1;

	if (intstatus & AIC_OTHER_INTERRUPT) {
		/* clear the "device to host soft irq" bit */
		if (aic_read_1(sc, 1, AIC_REG_INTR_PENDING, &value) == 0)
			aic_write_1(sc, 1, AIC_REG_INTR_PENDING, value & ~0x01);
		intstatus &= ~AIC_OTHER_INTERRUPT;
	}
	if (intstatus == 0)
		return 0;

	if (intstatus == AIC_INTSTATUS_BYTEMODE) {
		if (aic_read_1(sc, 1, AIC_REG_BYTEMODE_LEN, &value) != 0)
			return -1;
		len = (size_t)value * 4;
	} else if ((intstatus | 0x08) > AIC_INTSTATUS_BYTEMODE) {
		/* a function 2 status; the D80 has no message function */
		printf("%s: unexpected interrupt status 0x%02x\n", AIC_NAME,
			intstatus);
		return 1;
	} else
		len = (size_t)(intstatus & 0x7f) * AIC_BLOCK_SIZE;

	if (len == 0)
		return 1;
	if (len > AIC_RXBUF_SIZE)
		len = AIC_RXBUF_SIZE;

	status = SDIO_OPS(sc)->read(SDIO_COOKIE(sc), 1, AIC_REG_RD_FIFO,
		sc->sc_rxbuf, len, 0);
	if (status != B_OK) {
		printf("%s: read FIFO (%u bytes) failed: %s\n", AIC_NAME,
			(unsigned)len, strerror(status));
		return -1;
	}

	p = sc->sc_rxbuf;
	end = p + len;
	while (p + 4 <= end) {
		uint16_t pkt_len = p[0] | (p[1] << 8);
		if (pkt_len == 0)
			break;

		if ((p[2] & AIC_SDIO_TYPE_CFG) != AIC_SDIO_TYPE_CFG) {
			/* data frame: hardware header + 802.11 frame */
			adjust = roundup((size_t)pkt_len + AIC_RX_HWHDR_LEN, 4);
			sc->sc_data_frames++;
			p += adjust;
		} else {
			adjust = roundup((size_t)pkt_len, 4);
			if (p + 4 + pkt_len > end)
				break;
			if ((p[2] & 0x7f) == AIC_SDIO_TYPE_CMD_RSP)
				aic_rx_msg(sc, p + 4, pkt_len);
			p += adjust + 4;
		}
	}
	return 1;
}


/* rwnx_send_msg() with a confirmation, polled */
static int
aic_cmd(struct aic_softc *sc, uint16_t id, uint16_t dest, const void *param,
	uint16_t param_len, uint16_t cfm_id, void *cfm, size_t cfm_len,
	int timeout_ms)
{
	bigtime_t deadline;
	int error, r;

	sc->sc_cfm_id = cfm_id;
	sc->sc_cfm_buf = cfm;
	sc->sc_cfm_len = cfm_len;
	sc->sc_cfm_done = 0;

	error = aic_send_msg(sc, id, dest, param, param_len);
	if (error != 0)
		goto out;

	deadline = system_time() + (bigtime_t)timeout_ms * 1000;
	while (!sc->sc_cfm_done) {
		r = aic_rx_poll(sc);
		if (r < 0) {
			error = EIO;
			break;
		}
		if (sc->sc_cfm_done)
			break;
		if (system_time() > deadline) {
			error = ETIMEDOUT;
			break;
		}
		if (r == 0)
			snooze(100);
	}

out:
	if (error != 0)
		printf("%s: message 0x%04x failed (%s)\n", AIC_NAME, id,
			error == ETIMEDOUT ? "no confirmation" : strerror(error));
	sc->sc_cfm_id = 0;
	sc->sc_cfm_buf = NULL;
	sc->sc_cfm_len = 0;
	return error;
}


/* #pragma mark - DBG messages (aic_bsp_driver.c) */


static int
aic_mem_read(struct aic_softc *sc, uint32_t addr, uint32_t *value)
{
	struct dbg_mem_read_req req;
	struct dbg_mem_read_cfm cfm;
	int error;

	req.memaddr = htole32(addr);
	memset(&cfm, 0, sizeof(cfm));
	error = aic_cmd(sc, DBG_MEM_READ_REQ, TASK_DBG, &req, sizeof(req),
		DBG_MEM_READ_CFM, &cfm, sizeof(cfm), AIC_CMD_TIMEOUT_MS);
	if (error == 0)
		*value = le32toh(cfm.memdata);
	return error;
}


static int
aic_mem_write(struct aic_softc *sc, uint32_t addr, uint32_t value)
{
	struct dbg_mem_write_req req;
	struct dbg_mem_write_cfm cfm;

	req.memaddr = htole32(addr);
	req.memdata = htole32(value);
	return aic_cmd(sc, DBG_MEM_WRITE_REQ, TASK_DBG, &req, sizeof(req),
		DBG_MEM_WRITE_CFM, &cfm, sizeof(cfm), AIC_CMD_TIMEOUT_MS);
}


static int
aic_mem_block_write(struct aic_softc *sc, uint32_t addr, uint32_t size,
	const uint8_t *data)
{
	struct dbg_mem_block_write_req *req;
	struct dbg_mem_block_write_cfm cfm;
	int error;

	if (size > sizeof(req->memdata))
		return EINVAL;
	req = malloc(sizeof(*req), M_DEVBUF, M_WAITOK | M_ZERO);
	if (req == NULL)
		return ENOMEM;
	req->memaddr = htole32(addr);
	req->memsize = htole32(size);
	memcpy(req->memdata, data, size);
	/* the Linux driver always sends the whole 1 KB structure */
	error = aic_cmd(sc, DBG_MEM_BLOCK_WRITE_REQ, TASK_DBG, req, sizeof(*req),
		DBG_MEM_BLOCK_WRITE_CFM, &cfm, sizeof(cfm), AIC_CMD_TIMEOUT_MS);
	free(req, M_DEVBUF, sizeof(*req));
	return error;
}


static int
aic_start_app(struct aic_softc *sc, uint32_t addr, uint32_t type,
	uint32_t *bootstatus)
{
	struct dbg_start_app_req req;
	struct dbg_start_app_cfm cfm;
	int error;

	req.bootaddr = htole32(addr);
	req.boottype = htole32(type);
	memset(&cfm, 0, sizeof(cfm));
	error = aic_cmd(sc, DBG_START_APP_REQ, TASK_DBG, &req, sizeof(req),
		DBG_START_APP_CFM, &cfm, sizeof(cfm), AIC_CMD_TIMEOUT_MS);
	if (error == 0)
		*bootstatus = le32toh(cfm.bootstatus);
	return error;
}


/* #pragma mark - firmware */


/* rwnx_plat_bin_fw_upload() */
static int
aic_upload_firmware(struct aic_softc *sc, uint32_t addr, const char *name)
{
	u_char *fw = NULL;
	size_t size = 0, i = 0;
	bigtime_t start;
	int error = 0;

	if (loadfirmware(name, &fw, &size) != 0 || fw == NULL || size == 0) {
		printf("%s: firmware file %s not found. Beacon does not ship it; "
			"put it in /boot/system/non-packaged/data/firmware/" AIC_NAME
			"/\n", AIC_NAME, name);
		return ENOENT;
	}

	printf("%s: uploading %s (%u bytes) to 0x%08x\n", AIC_NAME, name,
		(unsigned)size, (unsigned)addr);
	start = system_time();

	if (size > 1024) {
		for (i = 0; i < size - 1024; i += 1024) {
			error = aic_mem_block_write(sc, addr + i, 1024, fw + i);
			if (error != 0) {
				printf("%s: upload failed at 0x%08x\n", AIC_NAME,
					(unsigned)(addr + i));
				break;
			}
		}
	}
	if (error == 0 && i < size) {
		error = aic_mem_block_write(sc, addr + i, size - i, fw + i);
		if (error != 0)
			printf("%s: upload failed at 0x%08x\n", AIC_NAME,
				(unsigned)(addr + i));
	}

	if (error == 0)
		printf("%s: upload done in %u ms\n", AIC_NAME,
			(unsigned)((system_time() - start) / 1000));
	free(fw, M_DEVBUF, size);
	return error;
}


/* aicwifi_patch_config_8800d80() (no adaptivity test, 2.4 GHz patch table) */
static const uint32_t aic_patch_tbl_8800d80[][2] = {
	{ 0x00b4, 0xf3010000 },
	{ 0x0188, 0x00000001 | (0x01U << 1) },	/* user_ext_flags, chan max txpwr */
};

static int
aic_patch_config_8800d80(struct aic_softc *sc)
{
	const uint32_t rd_patch_addr = AIC_RAM_FMAC_FW_ADDR + 0x0198;
	uint32_t config_base, patch_str_base, version, start_addr = 0x0016f800;
	uint32_t count = nitems(aic_patch_tbl_8800d80), i;
	int error;

	if ((error = aic_mem_read(sc, rd_patch_addr, &config_base)) != 0)
		return error;
	if ((error = aic_mem_read(sc, rd_patch_addr + 8, &patch_str_base)) != 0)
		return error;
	if ((error = aic_mem_read(sc, AIC_RAM_FMAC_FW_ADDR + 0x01c, &version))
			!= 0)
		return error;
	sc->sc_fw_version = version;
	printf("%s: firmware version word 0x%08x\n", AIC_NAME, version);
	if (version > 0x06090100) {
		if ((error = aic_mem_read(sc, rd_patch_addr + 12, &start_addr)) != 0)
			return error;
	}

	if ((error = aic_mem_write(sc, patch_str_base + AIC_PATCH_OFS_MAGIC,
			AIC_PATCH_MAGIC)) != 0
		|| (error = aic_mem_write(sc, patch_str_base + AIC_PATCH_OFS_MAGIC_2,
			AIC_PATCH_MAGIC_2)) != 0
		|| (error = aic_mem_write(sc,
			patch_str_base + AIC_PATCH_OFS_PAIR_START, start_addr)) != 0
		|| (error = aic_mem_write(sc,
			patch_str_base + AIC_PATCH_OFS_PAIR_COUNT, count)) != 0)
		return error;

	for (i = 0; i < count; i++) {
		if ((error = aic_mem_write(sc, start_addr + 8 * i,
				aic_patch_tbl_8800d80[i][0] + config_base)) != 0)
			return error;
		if ((error = aic_mem_write(sc, start_addr + 8 * i + 4,
				aic_patch_tbl_8800d80[i][1])) != 0)
			return error;
	}

	for (i = 0; i < 4; i++) {
		if ((error = aic_mem_write(sc,
				patch_str_base + AIC_PATCH_OFS_BLOCK_SIZE + 4 * i, 0)) != 0)
			return error;
	}
	return 0;
}


/*
 * aicbsp_driver_fw_init() + aicwifi_init() for the D80, then the first
 * fdrv requests (rwnx_send_get_macaddr_req, rwnx_send_reset,
 * rwnx_send_version_req).
 */
static int
aic_fw_init(struct aic_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mm_get_mac_addr_req macreq;
	struct mm_get_mac_addr_cfm maccfm;
	const char *fwname = "fmacfw_8800d80_u02.bin";
	uint32_t word, rev;
	int error;

	error = aic_mem_read(sc, 0x40500000, &word);
	if (error != 0) {
		/* maybe the chip only reports through the CCCR interrupt bits */
		printf("%s: no answer from the boot ROM; enabling the card "
			"interrupt bits and retrying\n", AIC_NAME);
		aic_write_1(sc, 0, AIC_CCCR_INT_ENABLE, 0x07);
		sc->sc_cccr_intr = 1;
		error = aic_mem_read(sc, 0x40500000, &word);
	}
	if (error != 0) {
		printf("%s: the boot ROM does not answer\n", AIC_NAME);
		return error;
	}
	sc->sc_chip_word = word;
	rev = (word >> 16) & 0x3f;
	printf("%s: boot ROM up, chip word 0x%08x, revision %u%s\n", AIC_NAME,
		word, rev, ((word >> 16) & 0xc0) == 0xc0 ? " (H variant)" : "");
	if (rev != AIC_CHIP_REV_U02) {
		printf("%s: AIC8800D80 U02 is required\n", AIC_NAME);
		return ENODEV;
	}
	if (((word >> 16) & 0xc0) == 0xc0)
		fwname = "fmacfw_8800d80_h_u02.bin";

	/* aicbsp_system_config_8800d80(): its table is empty */

	error = aic_upload_firmware(sc, AIC_RAM_FMAC_FW_ADDR, fwname);
	if (error != 0)
		return error;
	error = aic_patch_config_8800d80(sc);
	if (error != 0) {
		printf("%s: patch config failed\n", AIC_NAME);
		return error;
	}
	/* aicwifi_sys_config_8800d80(): nothing without CONFIG_OOB */

	error = aic_start_app(sc, AIC_RAM_FMAC_FW_ADDR, HOST_START_APP_AUTO,
		&sc->sc_bootstatus);
	if (error != 0) {
		printf("%s: firmware start failed\n", AIC_NAME);
		return error;
	}
	printf("%s: firmware started, boot status 0x%x\n", AIC_NAME,
		sc->sc_bootstatus);

	memset(&macreq, 0, sizeof(macreq));
	macreq.get = htole32(1);
	memset(&maccfm, 0, sizeof(maccfm));
	error = aic_cmd(sc, MM_GET_MAC_ADDR_REQ, TASK_MM, &macreq, sizeof(macreq),
		MM_GET_MAC_ADDR_CFM, &maccfm, sizeof(maccfm), AIC_CMD_TIMEOUT_MS);
	if (error != 0)
		return error;
	if ((maccfm.mac_addr[0] | maccfm.mac_addr[1] | maccfm.mac_addr[2]
			| maccfm.mac_addr[3]) != 0)
		memcpy(ic->ic_myaddr, maccfm.mac_addr, ETHER_ADDR_LEN);
	else {
		/* rwnx_main.c dflt_mac */
		static const uint8_t dflt[ETHER_ADDR_LEN]
			= { 0x88, 0x00, 0x33, 0x77, 0x10, 0x99 };
		memcpy(ic->ic_myaddr, dflt, ETHER_ADDR_LEN);
		printf("%s: no MAC address in efuse, using the default\n", AIC_NAME);
	}

	error = aic_cmd(sc, MM_RESET_REQ, TASK_MM, NULL, 0, MM_RESET_CFM, NULL, 0,
		AIC_CMD_TIMEOUT_MS);
	if (error != 0)
		return error;

	memset(&sc->sc_version, 0, sizeof(sc->sc_version));
	error = aic_cmd(sc, MM_VERSION_REQ, TASK_MM, NULL, 0, MM_VERSION_CFM,
		&sc->sc_version, sizeof(sc->sc_version), AIC_CMD_TIMEOUT_MS);
	if (error != 0)
		return error;

	printf("%s: firmware up: lmac %u.%u.%u.%u, features 0x%08x, max %u "
		"stations, address %s\n", AIC_NAME,
		(sc->sc_version.version_lmac >> 24) & 0xff,
		(sc->sc_version.version_lmac >> 16) & 0xff,
		(sc->sc_version.version_lmac >> 8) & 0xff,
		sc->sc_version.version_lmac & 0xff,
		sc->sc_version.features, sc->sc_version.max_sta_nb,
		ether_sprintf(ic->ic_myaddr));
	if (sc->sc_data_frames > 0)
		printf("%s: %d data frames seen during start\n", AIC_NAME,
			sc->sc_data_frames);
	sc->sc_fw_up = 1;
	return 0;
}


/* #pragma mark - net80211 (milestone 1: interface only, no scan yet) */


static int
aic_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct aic_softc *sc = ic->ic_softc;

	if (nstate == IEEE80211_S_SCAN) {
		/* TODO milestone 2: SCANU_START_REQ */
		printf("%s: scanning is not implemented yet\n", AIC_NAME);
		ic->ic_state = nstate;
		return 0;
	}
	return sc->sc_newstate(ic, nstate, arg);
}


static void
aic_start(struct ifnet *ifp)
{
	/* nothing goes out before milestone 3; drop what is queued */
	struct mbuf *m;

	for (;;) {
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		m_freem(m);
	}
}


static int
aic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct aic_softc *sc = ifp->if_softc;
	int error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
			ifp->if_flags |= IFF_RUNNING;
		else
			ifp->if_flags &= ~IFF_RUNNING;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
	}
	if (error == ENETRESET)
		error = 0;
	(void)sc;
	return error;
}


static int
aic_media_change(struct ifnet *ifp)
{
	int error = ieee80211_media_change(ifp);
	return error == ENETRESET ? 0 : error;
}


static void
aic_net_attach(struct aic_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int chan;

	ic->ic_softc = sc;
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_WEP | IEEE80211_C_RSN | IEEE80211_C_SCANALL;

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (chan = 1; chan <= 13; chan++) {
		ic->ic_channels[chan].ic_freq =
			ieee80211_ieee2mhz(chan, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_CCK
			| IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN
			| IEEE80211_CHAN_2GHZ;
	}
	ic->ic_ibss_chan = &ic->ic_channels[1];

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = aic_ioctl;
	ifp->if_start = aic_start;
	strlcpy(ifp->if_xname, AIC_NAME, IFNAMSIZ);

	if_attach(ifp);
	ieee80211_ifattach(ifp);

	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = aic_newstate;

	IEEE80211_ADDR_COPY(IF_LLADDR(ifp), ic->ic_myaddr);
	IEEE80211_ADDR_COPY(ic->ic_ac.ac_enaddr, ic->ic_myaddr);

	ieee80211_media_init(ifp, aic_media_change, ieee80211_media_status);
}


/* #pragma mark - Haiku attachment */


static int
aic_sdio_probe(device_t dev)
{
	struct haiku_sdio_device *hdev = haiku_sdio_get_device(dev);

	if (hdev == NULL || hdev->vendor != AIC_VENDOR_AICSEMI
		|| hdev->device != AIC_DEVICE_D80 || hdev->function != 1)
		return ENXIO;

	printf("%s: found AIC8800D80 SDIO %04x:%04x function %u (%u functions)\n",
		AIC_NAME, hdev->vendor, hdev->device, hdev->function,
		hdev->function_count);
	return BUS_PROBE_DEFAULT;
}


static int
aic_sdio_attach(device_t dev)
{
	struct aic_softc *sc = device_get_softc(dev);
	struct haiku_sdio_device *hdev = haiku_sdio_get_device(dev);
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int error;

	sc->sc_dev = dev;

	if (sdmmc_haiku_attach(&sc->sc_sdmmc, hdev) != 0) {
		printf("%s: cannot set up the SDIO card\n", AIC_NAME);
		return ENXIO;
	}
	if (sc->sc_sdmmc.sc_function_count < 1) {
		printf("%s: card has no I/O function\n", AIC_NAME);
		return ENXIO;
	}
	sc->sc_sf = &sc->sc_sdmmc.sc_functions[1];

	sc->sc_txbuf = malloc(AIC_TXBUF_SIZE, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_rxbuf = malloc(AIC_RXBUF_SIZE, M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc->sc_txbuf == NULL || sc->sc_rxbuf == NULL) {
		error = ENOMEM;
		goto fail;
	}

	rw_enter_write(&sc->sc_sdmmc.sc_lock);
	error = aic_card_check(sc);
	if (error == 0)
		error = aic_sdio_func_init(sc);
	if (error == 0)
		error = aic_fw_init(sc);
	rw_exit(&sc->sc_sdmmc.sc_lock);
	if (error != 0) {
		printf("%s: chip start failed, not attaching\n", AIC_NAME);
		goto fail;
	}

	if_alloc_inplace(ifp, IFT_ETHER);
	aic_net_attach(sc);
	printf("%s: attached (scan and join come in the next milestones)\n",
		AIC_NAME);
	return 0;

fail:
	free(sc->sc_txbuf, M_DEVBUF, AIC_TXBUF_SIZE);
	free(sc->sc_rxbuf, M_DEVBUF, AIC_RXBUF_SIZE);
	sc->sc_txbuf = sc->sc_rxbuf = NULL;
	sdmmc_haiku_detach(&sc->sc_sdmmc);
	return ENXIO;
}


static int
aic_sdio_detach(device_t dev)
{
	struct aic_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	if (sc->sc_fw_up) {
		ieee80211_ifdetach(ifp);
		if_detach(ifp);
	}
	sdmmc_haiku_detach(&sc->sc_sdmmc);
	free(sc->sc_txbuf, M_DEVBUF, AIC_TXBUF_SIZE);
	free(sc->sc_rxbuf, M_DEVBUF, AIC_RXBUF_SIZE);
	return 0;
}


static device_method_t aic_sdio_methods[] = {
	DEVMETHOD(device_probe,		aic_sdio_probe),
	DEVMETHOD(device_attach,	aic_sdio_attach),
	DEVMETHOD(device_detach,	aic_sdio_detach),
	DEVMETHOD_END
};

static driver_t aic_sdio_driver = {
	"aic8800",
	aic_sdio_methods,
	sizeof(struct aic_softc)
};

DRIVER_MODULE(aic8800, sdio, aic_sdio_driver, NULL, NULL);
