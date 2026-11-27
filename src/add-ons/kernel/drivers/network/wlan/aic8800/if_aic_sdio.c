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
 *   aic8800_fdrv/rwnx_msg_tx.c     MM_RESET / MM_VERSION / MM_GET_MAC_ADDR,
 *                                  MM_SET_STACK_START, MM_SET_TXPWR_IDX_LVL,
 *                                  MM_SET_RF_CALIB, ME_CONFIG, ME_CHAN_CONFIG,
 *                                  MM_START, MM_ADD_IF, SCANU_START
 *   aic8800_fdrv/aicwf_compat_8800d80.c, aicwf_chip_8800d80.c,
 *   rwnx_platform.c                RF init order and the built-in userconfig
 *                                  TX power defaults (txpwr_lvl_v3 gp[0])
 *   aic8800_fdrv/rwnx_msg_rx.c     SCANU_RESULT_IND / SCANU_START_CFM
 *   aic8800_fdrv/rwnx_msg_tx.c, rwnx_msg_rx.c (cont.)
 *                                  SM_CONNECT, SM_DISCONNECT, MM_KEY_ADD/DEL,
 *                                  ME_SET_CONTROL_PORT
 *   aic8800_fdrv/rwnx_tx.c, aicwf_sdio.c, ipc_shared.h
 *                                  data TX (struct hostdesc, SDIO data header)
 *   aic8800_fdrv/rwnx_rx.c, rwnx_rx.h, aicwf_txrxif.c
 *                                  data RX (struct hw_rxhdr, 802.11 to 802.3)
 *   aic8800_fdrv/rwnx_main.c, rwnx_mod_params.c
 *                                  open order, module parameter defaults
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
 * and firmware version. Milestone 2 (scan): RF init, ME config, a station
 * interface, SCANU_START, scan results into net80211. Milestone 3 (join):
 * SM_CONNECT (the firmware does auth/assoc), the WPA handshake in net80211
 * (as with bwfm), keys into the firmware, data TX/RX. RX is polled by a
 * thread: the JH7110 host has no usable SDIO card interrupt.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/task.h>

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
#define AIC_CHIP_REV_U03		7	/* also U04 (same id, Radxa aic_bsp_driver.h) */

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

	/* milestone 2: scan */
	int						 sc_5g;			/* MM_SET_STACK_START_CFM */
	int						 sc_vif_up;		/* MM_START + MM_ADD_IF done */
	uint8_t					 sc_vif_idx;
	int						 sc_scanning;	/* SCANU_START sent, no CFM yet */
	int						 sc_scan_results;
	int						 sc_scans;
#define AIC_SCAN_BSS_MAX		64
	uint8_t					 sc_scan_bss[AIC_SCAN_BSS_MAX][ETHER_ADDR_LEN];
	int						 sc_scan_bss_count;
	struct timeout			 sc_watchdog_to;

	/* RX poller thread (no usable card interrupt on the JH7110) */
	thread_id				 sc_poll_thread;
	volatile int			 sc_poll_quit;

	/* milestone 3: join */
	int						 sc_connecting;	/* SM_CONNECT sent, no IND yet */
	int						 sc_connected;
	uint8_t					 sc_ap_idx;		/* firmware station index */
	int						 sc_port_open;
#define AIC_NO_KEY				0xff
	uint8_t					 sc_pairwise_hw;
	uint8_t					 sc_group_hw[4];
	int						 sc_eapol_logged;
	int						 sc_rx_mgmt;

	/* firmware indications, handed from the RX path to a task */
	TAILQ_HEAD(, aic_ind)	 sc_indq;
	struct mutex			 sc_indq_mtx;
	int						 sc_indq_len;
	struct task				 sc_ind_task;
};

struct aic_ind {
	TAILQ_ENTRY(aic_ind)	 link;
	uint16_t				 id;
	uint16_t				 len;
	uint8_t					 data[];
};

#define AIC_INDQ_MAX			256
#define AIC_IND_DATA			0xffff	/* an RX data frame, not a message */
#define AIC_IND_WATCHDOG		0xfffe
#define AIC_SCAN_TIMEOUT_S		15
#define AIC_CONNECT_TIMEOUT_S	15

static void aic_ind_task(void *);
static void aic_start(struct ifnet *);
static void aic_watchdog(void *);


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

	/*
	 * An indication (scan result, scan done, ...). The RX path runs with
	 * the SDIO lock held, and handling these calls into net80211, which
	 * may send commands back; so queue them for aic_ind_task().
	 */
	if (sc->sc_fw_up && sc->sc_ic.ic_if.if_softc == sc) {
		struct aic_ind *ind;

		mtx_enter(&sc->sc_indq_mtx);
		if (sc->sc_indq_len >= AIC_INDQ_MAX) {
			mtx_leave(&sc->sc_indq_mtx);
			return;
		}
		mtx_leave(&sc->sc_indq_mtx);
		ind = malloc(sizeof(*ind) + param_len, M_DEVBUF, M_NOWAIT);
		if (ind == NULL)
			return;
		ind->id = id;
		ind->len = param_len;
		memcpy(ind->data, msg + 12, param_len);
		mtx_enter(&sc->sc_indq_mtx);
		TAILQ_INSERT_TAIL(&sc->sc_indq, ind, link);
		sc->sc_indq_len++;
		mtx_leave(&sc->sc_indq_mtx);
		task_add(systq, &sc->sc_ind_task);
		return;
	}

	if (sc->sc_unsolicited_logged < 16) {
		sc->sc_unsolicited_logged++;
		printf("%s: firmware message 0x%04x (task %u, index %u), %u bytes\n",
			AIC_NAME, id, MSG_T(id), MSG_I(id), param_len);
	}
}


/* copy one RX data frame (hardware header + MPDU) for aic_ind_task() */
static void
aic_queue_data(struct aic_softc *sc, const uint8_t *p, size_t len)
{
	struct aic_ind *ind;

	if (len > 0xffff || sc->sc_indq_len >= AIC_INDQ_MAX)
		return;
	ind = malloc(sizeof(*ind) + len, M_DEVBUF, M_NOWAIT);
	if (ind == NULL)
		return;
	ind->id = AIC_IND_DATA;
	ind->len = len;
	memcpy(ind->data, p, len);
	mtx_enter(&sc->sc_indq_mtx);
	TAILQ_INSERT_TAIL(&sc->sc_indq, ind, link);
	sc->sc_indq_len++;
	mtx_leave(&sc->sc_indq_mtx);
	task_add(systq, &sc->sc_ind_task);
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
			if (p + pkt_len + AIC_RX_HWHDR_LEN > end)
				break;
			if (sc->sc_connecting || sc->sc_connected)
				aic_queue_data(sc, p, pkt_len + AIC_RX_HWHDR_LEN);
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


/* #pragma mark - LMAC configuration (aic8800_fdrv rwnx_msg_tx.c) */


/* non-DFS 5 GHz channels (no radar detection here) */
static const uint8_t aic_5ghz_channels[] = {
	36, 40, 44, 48, 149, 153, 157, 161, 165
};

#define AIC_CHAN_TX_POWER		20		/* dBm, as the Linux regdb gives */


/* rwnx_send_set_stack_start_req(1, 0, BIT(5), 0) (aic8800d80_set_stack_start) */
static int
aic_stack_start(struct aic_softc *sc)
{
	struct mm_set_stack_start_req req;
	struct mm_set_stack_start_cfm cfm;
	int error;

	memset(&req, 0, sizeof(req));
	req.is_stack_start = 1;
	req.efuse_valid = 0;
	req.set_vendor_info = 1 << 5;
	req.fwtrace_redir = 0;
	memset(&cfm, 0, sizeof(cfm));
	error = aic_cmd(sc, MM_SET_STACK_START_REQ, TASK_MM, &req, sizeof(req),
		MM_SET_STACK_START_CFM, &cfm, sizeof(cfm), AIC_CMD_TIMEOUT_MS);
	if (error == 0) {
		sc->sc_5g = cfm.is_5g_support != 0;
		printf("%s: stack start: 5 GHz %s, vendor info 0x%02x\n", AIC_NAME,
			sc->sc_5g ? "supported" : "not supported", cfm.vendor_info);
	}
	return error;
}


/*
 * aicwf_set_rf_config_8800d80(): TX power levels (the driver's built-in
 * userconfig defaults, txpwr_lvl_v3_gp[0]; the adj and ofst2x tables are
 * disabled by default so the Linux driver does not send them), then the RF
 * calibration.
 */
static int
aic_rf_init(struct aic_softc *sc)
{
	static const s8 lvl_11b_11ag_2g4[12]
		= { 20, 20, 20, 20, 20, 20, 20, 20, 18, 18, 16, 16 };
	static const s8 lvl_11n_11ac_2g4[10]
		= { 20, 20, 20, 20, 18, 18, 16, 16, 16, 16 };
	static const s8 lvl_11ax_2g4[12]
		= { 20, 20, 20, 20, 18, 18, 16, 16, 16, 16, 15, 15 };
	static const s8 lvl_11a_5g[12]
		= { (s8)0x80, (s8)0x80, (s8)0x80, (s8)0x80, 20, 20, 20, 20, 18, 18,
			16, 16 };
	static const s8 lvl_11n_11ac_5g[10]
		= { 20, 20, 20, 20, 18, 18, 16, 16, 16, 15 };
	static const s8 lvl_11ax_5g[12]
		= { 20, 20, 20, 20, 18, 18, 16, 16, 16, 15, 14, 14 };
	struct mm_set_txpwr_lvl_req lvl;
	struct mm_set_rf_calib_req cal;
	struct mm_set_rf_calib_cfm calcfm;
	int error;

	memset(&lvl, 0, sizeof(lvl));
	lvl.txpwr_lvl_v3.enable = 1;
	memcpy(lvl.txpwr_lvl_v3.pwrlvl_11b_11ag_2g4, lvl_11b_11ag_2g4, 12);
	memcpy(lvl.txpwr_lvl_v3.pwrlvl_11n_11ac_2g4, lvl_11n_11ac_2g4, 10);
	memcpy(lvl.txpwr_lvl_v3.pwrlvl_11ax_2g4, lvl_11ax_2g4, 12);
	memcpy(lvl.txpwr_lvl_v3.pwrlvl_11a_5g, lvl_11a_5g, 12);
	memcpy(lvl.txpwr_lvl_v3.pwrlvl_11n_11ac_5g, lvl_11n_11ac_5g, 10);
	memcpy(lvl.txpwr_lvl_v3.pwrlvl_11ax_5g, lvl_11ax_5g, 12);
	error = aic_cmd(sc, MM_SET_TXPWR_IDX_LVL_REQ, TASK_MM, &lvl, sizeof(lvl),
		MM_SET_TXPWR_IDX_LVL_CFM, NULL, 0, AIC_CMD_TIMEOUT_MS);
	if (error != 0)
		return error;

	memset(&cal, 0, sizeof(cal));
	cal.cal_cfg_24g = 0x0f8f;		/* aic8800d80_set_rf_calib_cfg() */
	cal.cal_cfg_5g = 0x0f0f;
	cal.param_alpha = 0x0c34c008;
	cal.bt_calib_en = 0;
	cal.bt_calib_param = 0x264203;
	cal.xtal_cap = 0;				/* userconfig xtal_cap disabled */
	cal.xtal_cap_fine = 0;
	memset(&calcfm, 0, sizeof(calcfm));
	error = aic_cmd(sc, MM_SET_RF_CALIB_REQ, TASK_MM, &cal, sizeof(cal),
		MM_SET_RF_CALIB_CFM, &calcfm, sizeof(calcfm), AIC_CMD_TIMEOUT_MS);
	if (error == 0)
		printf("%s: RF calibrated (rx gain 2.4G 0x%08x)\n", AIC_NAME,
			calcfm.rxgain_24g_addr);
	return error;
}


/*
 * rwnx_send_me_config_req() with the Linux driver's defaults: HT on
 * (MCS 0-7, 20/40 MHz, SGI, LDPC, STBC), VHT/HE off for now, power save off
 * (the Linux default is on; keep it simple while the driver polls).
 */
static int
aic_me_config(struct aic_softc *sc)
{
	struct me_config_req req;

	memset(&req, 0, sizeof(req));
	req.ht_supp = true;
	req.ht_cap.ht_capa_info = htole16(0x0001 | 0x0002 | 0x0020 | 0x0040
		| 0x0080 | 0x0100);
	req.ht_cap.a_mpdu_param = 3 | (7 << 2);	/* 64K, 16 us density */
	req.ht_cap.mcs_rate[0] = 0xff;
	req.ht_cap.mcs_rate[12] = 0x01;			/* TX MCS set defined */
	req.vht_supp = false;
	req.he_supp = false;
	req.ps_on = false;
	req.dpsm = false;
	req.tx_lft = 1000;						/* RWNX_TX_LIFETIME_MS */
	req.ant_div_on = false;
	req.phy_bw_max = PHY_CHNL_BW_40;
	return aic_cmd(sc, ME_CONFIG_REQ, TASK_ME, &req, sizeof(req),
		ME_CONFIG_CFM, NULL, 0, AIC_CMD_TIMEOUT_MS);
}


/* rwnx_send_me_chan_config_req(): 2.4 GHz 1-13, non-DFS 5 GHz */
static int
aic_chan_config(struct aic_softc *sc)
{
	struct me_chan_config_req *req;
	unsigned i;
	int error;

	req = malloc(sizeof(*req), M_DEVBUF, M_WAITOK | M_ZERO);
	if (req == NULL)
		return ENOMEM;
	for (i = 1; i <= 13; i++) {
		struct mac_chan_def *c = &req->chan2G4[req->chan2G4_cnt++];
		c->band = 0;	/* NL80211_BAND_2GHZ */
		c->freq = ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		c->flags = 0;
		c->tx_power = AIC_CHAN_TX_POWER;
	}
	if (sc->sc_5g) {
		for (i = 0; i < nitems(aic_5ghz_channels); i++) {
			struct mac_chan_def *c = &req->chan5G[req->chan5G_cnt++];
			c->band = 1;	/* NL80211_BAND_5GHZ */
			c->freq = ieee80211_ieee2mhz(aic_5ghz_channels[i],
				IEEE80211_CHAN_5GHZ);
			c->flags = 0;
			c->tx_power = AIC_CHAN_TX_POWER;
		}
	}
	error = aic_cmd(sc, ME_CHAN_CONFIG_REQ, TASK_ME, req, sizeof(*req),
		ME_CHAN_CONFIG_CFM, NULL, 0, AIC_CMD_TIMEOUT_MS);
	free(req, M_DEVBUF, sizeof(*req));
	return error;
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
	/*
	 * The RFC accepts U02 only. AIC's own SDIO BSP (Radxa radxa-pkg/aic8800,
	 * aicbsp_driver_fw_init) runs U02 and U03 (id 7, shared with U04) the
	 * same way, with the *_u02 firmware set. The VF2 Lite's module is id 7
	 * (vf2lite-13: chip word 0xf3078820).
	 */
	if (rev != AIC_CHIP_REV_U02 && rev != AIC_CHIP_REV_U03) {
		printf("%s: unsupported AIC8800D80 revision %u (U02 = 3 and "
			"U03/U04 = 7 are known)\n", AIC_NAME, rev);
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

	/* rwnx_platform_init order: stack start, RF init, MAC, reset, version */
	if ((error = aic_stack_start(sc)) != 0)
		return error;
	if ((error = aic_rf_init(sc)) != 0) {
		printf("%s: RF init failed\n", AIC_NAME);
		return error;
	}

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
	if ((error = aic_me_config(sc)) != 0
		|| (error = aic_chan_config(sc)) != 0) {
		printf("%s: ME configuration failed\n", AIC_NAME);
		return error;
	}
	if (sc->sc_data_frames > 0)
		printf("%s: %d data frames seen during start\n", AIC_NAME,
			sc->sc_data_frames);
	sc->sc_fw_up = 1;
	return 0;
}


/* #pragma mark - RX poller */


/*
 * The JH7110's dw_mmc cannot be used for the SDIO card interrupt (StarFive's
 * own device trees leave cap-sdio-irq out, so their Linux polls too; see
 * FINDINGS.md). Poll the chip's own status register instead, like Linux's
 * sdio_irq_thread() without MMC_CAP_SDIO_IRQ: every 10 ms when idle, the
 * period halved (down to 1 ms) while there is traffic.
 */
#define AIC_POLL_IDLE_US		10000
#define AIC_POLL_MIN_US			1000

static int32
aic_poll_thread(void *arg)
{
	struct aic_softc *sc = arg;
	bigtime_t period = AIC_POLL_IDLE_US;

	while (!sc->sc_poll_quit) {
		int i, got = 0, r;

		rw_enter_write(&sc->sc_sdmmc.sc_lock);
		for (i = 0; i < 32; i++) {
			r = aic_rx_poll(sc);
			if (r <= 0)
				break;
			got++;
		}
		rw_exit(&sc->sc_sdmmc.sc_lock);

		if (got > 0)
			period = MAX(period / 2, AIC_POLL_MIN_US);
		else
			period = MIN(period + 1000, AIC_POLL_IDLE_US);
		snooze(period);
	}
	return 0;
}


/* #pragma mark - net80211 */


/* send one LMAC request from net80211 context (takes the SDIO lock) */
static int
aic_cmd_locked(struct aic_softc *sc, uint16_t id, uint16_t dest,
	const void *param, uint16_t param_len, uint16_t cfm_id, void *cfm,
	size_t cfm_len)
{
	int error;

	rw_enter_write(&sc->sc_sdmmc.sc_lock);
	error = aic_cmd(sc, id, dest, param, param_len, cfm_id, cfm, cfm_len,
		AIC_CMD_TIMEOUT_MS);
	rw_exit(&sc->sc_sdmmc.sc_lock);
	return error;
}


/* queue a driver-internal event for aic_ind_task() */
static void
aic_post(struct aic_softc *sc, uint16_t id)
{
	struct aic_ind *ind = malloc(sizeof(*ind), M_DEVBUF, M_NOWAIT);

	if (ind == NULL)
		return;
	ind->id = id;
	ind->len = 0;
	mtx_enter(&sc->sc_indq_mtx);
	TAILQ_INSERT_TAIL(&sc->sc_indq, ind, link);
	sc->sc_indq_len++;
	mtx_leave(&sc->sc_indq_mtx);
	task_add(systq, &sc->sc_ind_task);
}


/* scan or connect that never finished (a reply lost to a bus error) */
static void
aic_watchdog(void *arg)
{
	aic_post(arg, AIC_IND_WATCHDOG);
}


/* rwnx_open(): MM_START once, then one station interface (MM_ADD_IF) */
static int
aic_vif_up(struct aic_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mm_start_req start;
	struct mm_add_if_req add;
	struct mm_add_if_cfm addcfm;
	int error;

	if (sc->sc_vif_up)
		return 0;

	memset(&start, 0, sizeof(start));
	start.uapsd_timeout = 300;		/* rwnx_mod_params defaults */
	start.lp_clk_accuracy = 20;
	error = aic_cmd_locked(sc, MM_START_REQ, TASK_MM, &start, sizeof(start),
		MM_START_CFM, NULL, 0);
	if (error != 0)
		return error;

	memset(&add, 0, sizeof(add));
	add.type = MM_STA;
	memcpy(add.addr.array, ic->ic_myaddr, ETHER_ADDR_LEN);
	add.p2p = false;
	memset(&addcfm, 0, sizeof(addcfm));
	error = aic_cmd_locked(sc, MM_ADD_IF_REQ, TASK_MM, &add, sizeof(add),
		MM_ADD_IF_CFM, &addcfm, sizeof(addcfm));
	if (error != 0)
		return error;
	if (addcfm.status != 0) {
		printf("%s: firmware refused the station interface (status %u)\n",
			AIC_NAME, addcfm.status);
		return EIO;
	}
	sc->sc_vif_idx = addcfm.inst_nbr;
	sc->sc_vif_up = 1;
	printf("%s: station interface up (vif %u)\n", AIC_NAME, sc->sc_vif_idx);
	return 0;
}


/* rwnx_send_scanu_req(): all configured channels, wildcard or desired SSID */
static int
aic_scan(struct aic_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct scanu_start_req *req;
	int chan, error;

	if (sc->sc_scanning)
		return 0;
	req = malloc(sizeof(*req), M_DEVBUF, M_WAITOK | M_ZERO);
	if (req == NULL)
		return ENOMEM;

	req->vif_idx = sc->sc_vif_idx;
	for (chan = 1; chan <= IEEE80211_CHAN_MAX
			&& req->chan_cnt < SCAN_CHANNEL_MAX; chan++) {
		struct ieee80211_channel *c = &ic->ic_channels[chan];
		struct mac_chan_def *d;
		if (c->ic_freq == 0)
			continue;
		d = &req->chan[req->chan_cnt++];
		d->freq = c->ic_freq;
		d->band = IEEE80211_IS_CHAN_5GHZ(c) ? 1 : 0;
		d->flags = 0;
		d->tx_power = AIC_CHAN_TX_POWER;
	}
	/* one SSID: the wanted one (finds hidden networks) or the wildcard */
	req->ssid_cnt = 1;
	if (ic->ic_des_esslen > 0 && ic->ic_des_esslen <= MAC_SSID_LEN) {
		req->ssid[0].length = ic->ic_des_esslen;
		memcpy(req->ssid[0].array, ic->ic_des_essid, ic->ic_des_esslen);
	}
	memset(&req->bssid, 0xff, sizeof(req->bssid));
	req->no_cck = false;
	req->add_ies = 0;
	req->add_ie_len = 0;

	sc->sc_scanning = 1;
	sc->sc_scan_results = 0;
	sc->sc_scan_bss_count = 0;
	error = aic_cmd_locked(sc, SCANU_START_REQ, TASK_SCANU, req, sizeof(*req),
		SCANU_START_CFM_ADDTIONAL, NULL, 0);
	if (error != 0)
		sc->sc_scanning = 0;
	else {
		timeout_add_sec(&sc->sc_watchdog_to, AIC_SCAN_TIMEOUT_S);
		if (sc->sc_scans++ < 4)
			printf("%s: scan started on %u channels\n", AIC_NAME,
				req->chan_cnt);
	}
	free(req, M_DEVBUF, sizeof(*req));
	return error;
}


static struct mbuf *
aic_newbuf(void)
{
	struct mbuf *m;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return NULL;
	MCLGET(m, M_DONTWAIT);
	if (!(m->m_flags & M_EXT)) {
		m_freem(m);
		return NULL;
	}
	return m;
}


/*
 * rwnx_rx_scanu_result_ind(): the payload is the received beacon or probe
 * response; hand it to net80211 as it is (like bwfm_scan_node()).
 * Nothing about the networks found (names, addresses) is logged.
 */
static void
aic_scan_result(struct aic_softc *sc, const uint8_t *data, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	const struct scanu_result_ind *ind = (const void *)data;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct ieee80211_rxinfo rxi;
	struct mbuf *m;
	size_t flen;
	int i;

	if (len < sizeof(*ind) || ic->ic_state != IEEE80211_S_SCAN)
		return;
	flen = le16toh(ind->length);
	if (flen < sizeof(struct ieee80211_frame) + 12
		|| flen > len - sizeof(*ind) || flen > MCLBYTES)
		return;

	m = aic_newbuf();
	if (m == NULL)
		return;
	memcpy(mtod(m, uint8_t *), ind->payload, flen);
	m->m_pkthdr.len = m->m_len = flen;
	wh = mtod(m, struct ieee80211_frame *);

	/* the firmware reports one result per frame (beacon and probe
	 * response), but counts networks: count distinct BSSIDs too */
	for (i = 0; i < sc->sc_scan_bss_count; i++) {
		if (IEEE80211_ADDR_EQ(sc->sc_scan_bss[i], wh->i_addr3))
			break;
	}
	if (i == sc->sc_scan_bss_count && i < AIC_SCAN_BSS_MAX)
		IEEE80211_ADDR_COPY(sc->sc_scan_bss[sc->sc_scan_bss_count++],
			wh->i_addr3);

	ni = ieee80211_find_rxnode(ic, wh);
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = ind->rssi;
	rxi.rxi_chan = ieee80211_mhz2ieee(le16toh(ind->center_freq), 0);
	ieee80211_input(ifp, m, ni, &rxi);
	ieee80211_release_node(ic, ni);
	sc->sc_scan_results++;
}


static void
aic_scan_done(struct aic_softc *sc, const uint8_t *data, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct scanu_start_cfm *cfm = (const void *)data;

	sc->sc_scanning = 0;
	timeout_del(&sc->sc_watchdog_to);
	if (sc->sc_scans <= 4)
		printf("%s: scan done, status %u, %d frames from %d networks "
			"(firmware counts %u)\n", AIC_NAME,
			len >= sizeof(*cfm) ? cfm->status : 0xff, sc->sc_scan_results,
			sc->sc_scan_bss_count, len >= sizeof(*cfm) ? cfm->result_cnt : 0);
	if (ic->ic_state == IEEE80211_S_SCAN)
		ieee80211_end_scan(&ic->ic_if);
}


/* #pragma mark - join (milestone 3) */


/* rwnx_send_sm_connect_req(): the firmware does auth + assoc itself */
static int
aic_connect(struct aic_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct sm_connect_req *req;
	struct sm_connect_cfm cfm;
	uint8_t *frm;
	int error;

	req = malloc(sizeof(*req), M_DEVBUF, M_WAITOK | M_ZERO);
	if (req == NULL)
		return ENOMEM;

	req->ssid.length = MIN(ni->ni_esslen, MAC_SSID_LEN);
	memcpy(req->ssid.array, ni->ni_essid, req->ssid.length);
	memcpy(req->bssid.array, ni->ni_bssid, ETHER_ADDR_LEN);
	req->chan.freq = ni->ni_chan->ic_freq;
	req->chan.band = IEEE80211_IS_CHAN_5GHZ(ni->ni_chan) ? 1 : 0;
	req->chan.flags = 0;
	req->chan.tx_power = AIC_CHAN_TX_POWER;
	req->vif_idx = sc->sc_vif_idx;
	req->auth_type = 0;				/* WLAN_AUTH_OPEN */
	req->listen_interval = 0;		/* rwnx_mod_params listen_itv */
	req->dont_wait_bcmc = false;	/* !listen_bcmc */
	req->uapsd_queues = 0;			/* no power save here */
	req->ctrl_port_ethertype = htons(ETHERTYPE_EAPOL);

	if (ic->ic_flags & IEEE80211_F_RSNON) {
		/* net80211 runs the 4-way handshake (like bwfm): the host owns
		 * the control port, the firmware adds our RSN/WPA IE */
		req->flags = CONTROL_PORT_HOST | WPA_WPA2_IN_USE;
		if (ni->ni_rsncipher == IEEE80211_CIPHER_TKIP)
			req->flags |= DISABLE_HT;
		if (ni->ni_rsnprotos == IEEE80211_PROTO_RSN)
			frm = ieee80211_add_rsn((uint8_t *)req->ie_buf, ic, ni);
		else
			frm = ieee80211_add_wpa((uint8_t *)req->ie_buf, ic, ni);
		req->ie_len = frm - (uint8_t *)req->ie_buf;
	}

	memset(&cfm, 0, sizeof(cfm));
	sc->sc_connecting = 1;
	error = aic_cmd_locked(sc, SM_CONNECT_REQ, TASK_SM, req, sizeof(*req),
		SM_CONNECT_CFM, &cfm, sizeof(cfm));
	free(req, M_DEVBUF, sizeof(*req));
	if (error == 0 && cfm.status != 0) {
		printf("%s: firmware refused to connect (status %u)\n", AIC_NAME,
			cfm.status);
		error = EIO;
	}
	if (error != 0) {
		sc->sc_connecting = 0;
		return error;
	}
	timeout_add_sec(&sc->sc_watchdog_to, AIC_CONNECT_TIMEOUT_S);
	printf("%s: connecting (%s, channel %u)\n", AIC_NAME,
		(ic->ic_flags & IEEE80211_F_RSNON) ? "WPA" : "open",
		ieee80211_chan2ieee(ic, ni->ni_chan));
	return 0;
}


/* rwnx_send_sm_disconnect_req() */
static void
aic_disconnect(struct aic_softc *sc)
{
	struct sm_disconnect_req req;

	if (!sc->sc_connected && !sc->sc_connecting)
		return;
	memset(&req, 0, sizeof(req));
	req.reason_code = IEEE80211_REASON_AUTH_LEAVE;
	req.vif_idx = sc->sc_vif_idx;
	aic_cmd_locked(sc, SM_DISCONNECT_REQ, TASK_SM, &req, sizeof(req),
		SM_DISCONNECT_CFM, NULL, 0);
	sc->sc_connected = sc->sc_connecting = 0;
	sc->sc_port_open = 0;
	sc->sc_pairwise_hw = AIC_NO_KEY;
	memset(sc->sc_group_hw, AIC_NO_KEY, sizeof(sc->sc_group_hw));
}


/* rwnx_rx_sm_connect_ind() */
static void
aic_connect_ind(struct aic_softc *sc, const uint8_t *data, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct sm_connect_ind *ind = (const void *)data;

	if (len < offsetof(struct sm_connect_ind, assoc_ie_buf))
		return;
	sc->sc_connecting = 0;
	timeout_del(&sc->sc_watchdog_to);
	if (ic->ic_state != IEEE80211_S_AUTH)
		return;
	if (ind->status_code != 0) {
		printf("%s: association failed (status %u)\n", AIC_NAME,
			ind->status_code);
		ic->ic_bss->ni_fails++;
		ieee80211_begin_scan(&ic->ic_if);
		return;
	}
	sc->sc_connected = 1;
	sc->sc_ap_idx = ind->ap_idx;
	printf("%s: associated (station %u, aid %u%s)\n", AIC_NAME,
		ind->ap_idx, ind->aid & 0x3fff, ind->qos ? ", QoS" : "");
	ieee80211_new_state(ic, IEEE80211_S_ASSOC, -1);
	ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
}


static void
aic_disconnect_ind(struct aic_softc *sc, const uint8_t *data, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct sm_disconnect_ind *ind = (const void *)data;

	printf("%s: disconnected by the firmware (reason %u)\n", AIC_NAME,
		len >= sizeof(*ind) ? ind->reason_code : 0);
	sc->sc_connected = sc->sc_connecting = 0;
	sc->sc_port_open = 0;
	sc->sc_pairwise_hw = AIC_NO_KEY;
	memset(sc->sc_group_hw, AIC_NO_KEY, sizeof(sc->sc_group_hw));
	if (ic->ic_state > IEEE80211_S_SCAN)
		ieee80211_begin_scan(&ic->ic_if);
}


static void
aic_watchdog_fired(struct aic_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	if (sc->sc_scanning) {
		printf("%s: scan did not finish, starting again\n", AIC_NAME);
		sc->sc_scanning = 0;
		if (ic->ic_state == IEEE80211_S_SCAN)
			ieee80211_end_scan(&ic->ic_if);
	} else if (sc->sc_connecting && ic->ic_state == IEEE80211_S_AUTH) {
		printf("%s: no answer to the connect request, scanning again\n",
			AIC_NAME);
		aic_disconnect(sc);
		ic->ic_bss->ni_fails++;
		ieee80211_begin_scan(&ic->ic_if);
	}
}


/* net80211 does no management frames itself: the firmware does them */
static int
aic_send_mgmt(struct ieee80211com *ic, struct ieee80211_node *ni, int type,
	int arg1, int arg2)
{
	return 0;
}


static int
aic_cipher(u_int cipher)
{
	switch (cipher) {
	case IEEE80211_CIPHER_WEP40:	return MAC_CIPHER_WEP40;
	case IEEE80211_CIPHER_WEP104:	return MAC_CIPHER_WEP104;
	case IEEE80211_CIPHER_TKIP:		return MAC_CIPHER_TKIP;
	case IEEE80211_CIPHER_CCMP:		return MAC_CIPHER_CCMP;
	default:						return -1;
	}
}


/*
 * rwnx_cfg80211_add_key() -> rwnx_send_key_add(); after the pairwise key,
 * open the control port (what wpa_supplicant's "authorized" does on Linux,
 * rwnx_send_me_set_control_port_req()).
 */
static int
aic_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
	struct ieee80211_key *k)
{
	struct aic_softc *sc = ic->ic_softc;
	struct mm_key_add_req req;
	struct mm_key_add_cfm cfm;
	int pairwise = (k->k_flags & IEEE80211_KEY_GROUP) == 0;
	int cipher = aic_cipher(k->k_cipher);
	int error;

	if (cipher < 0 || k->k_len > MAC_SEC_KEY_LEN || !sc->sc_connected) {
		printf("%s: cannot install key (cipher %u)\n", AIC_NAME,
			k->k_cipher);
		return EINVAL;
	}

	/* the 4-way handshake's last message must leave unencrypted */
	aic_start(&ic->ic_if);

	memset(&req, 0, sizeof(req));
	req.sta_idx = pairwise ? sc->sc_ap_idx : 0xff;
	req.key_idx = pairwise ? 0 : k->k_id;
	req.pairwise = pairwise;
	req.inst_nbr = sc->sc_vif_idx;
	req.key.length = k->k_len;
	memcpy(req.key.array, k->k_key, k->k_len);
	req.cipher_suite = cipher;
	memset(&cfm, 0, sizeof(cfm));
	error = aic_cmd_locked(sc, MM_KEY_ADD_REQ, TASK_MM, &req, sizeof(req),
		MM_KEY_ADD_CFM, &cfm, sizeof(cfm));
	explicit_bzero(&req, sizeof(req));
	if (error == 0 && cfm.status != 0)
		error = EIO;
	if (error != 0) {
		printf("%s: %s key install failed (%d)\n", AIC_NAME,
			pairwise ? "pairwise" : "group", error);
		return error;
	}
	if (pairwise)
		sc->sc_pairwise_hw = cfm.hw_key_idx;
	else if (k->k_id < nitems(sc->sc_group_hw))
		sc->sc_group_hw[k->k_id] = cfm.hw_key_idx;
	printf("%s: %s key installed\n", AIC_NAME, pairwise ? "pairwise" : "group");

	if (pairwise && !sc->sc_port_open) {
		struct me_set_control_port_req port;

		memset(&port, 0, sizeof(port));
		port.sta_idx = sc->sc_ap_idx;
		port.control_port_open = true;
		if (aic_cmd_locked(sc, ME_SET_CONTROL_PORT_REQ, TASK_ME, &port,
				sizeof(port), ME_SET_CONTROL_PORT_CFM, NULL, 0) == 0)
			sc->sc_port_open = 1;
	}
	return 0;
}


static void
aic_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
	struct ieee80211_key *k)
{
	struct aic_softc *sc = ic->ic_softc;
	struct mm_key_del_req req;
	uint8_t *slot;

	if ((k->k_flags & IEEE80211_KEY_GROUP) == 0)
		slot = &sc->sc_pairwise_hw;
	else if (k->k_id < nitems(sc->sc_group_hw))
		slot = &sc->sc_group_hw[k->k_id];
	else
		return;
	if (*slot == AIC_NO_KEY || !sc->sc_connected)
		return;
	memset(&req, 0, sizeof(req));
	req.hw_key_idx = *slot;
	*slot = AIC_NO_KEY;
	aic_cmd_locked(sc, MM_KEY_DEL_REQ, TASK_MM, &req, sizeof(req),
		MM_KEY_DEL_CFM, NULL, 0);
}


/* #pragma mark - data path */


/*
 * rwnx_rxdataind_aicwf(): a data frame is the 60-byte hardware RX header
 * (struct hw_rxhdr, plus 4 bytes of SDIO alignment) and the received 802.11
 * MPDU, already decrypted but with its security header still in place.
 * Turn it into Ethernet frames as the Linux driver does (also splitting
 * A-MSDUs); EAPOL goes to net80211's supplicant, the rest up the stack.
 */
#define AIC_RXH_DECR_WORD		36	/* hwvect status word: decr_status 4:2 */
#define AIC_RXH_RSSI			17	/* rx_vect1.rssi1 */
#define AIC_RXH_FLAGS_WORD		48	/* is_amsdu 0, is_80211_mpdu 1, ... */
#define AIC_RXH_FLAG_80211_MPDU	0x02

static void
aic_rx_deliver(struct aic_softc *sc, const uint8_t *da, const uint8_t *sa,
	const uint8_t *type, const uint8_t *body, size_t blen,
	struct mbuf_list *ml)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ether_header *eh;
	struct mbuf *m;

	if (blen + sizeof(*eh) + ETHER_ALIGN > MCLBYTES)
		return;
	m = aic_newbuf();
	if (m == NULL) {
		ifp->if_ierrors++;
		return;
	}
	m->m_data += ETHER_ALIGN;
	eh = mtod(m, struct ether_header *);
	memcpy(eh->ether_dhost, da, ETHER_ADDR_LEN);
	memcpy(eh->ether_shost, sa, ETHER_ADDR_LEN);
	memcpy(&eh->ether_type, type, 2);
	memcpy(eh + 1, body, blen);
	m->m_pkthdr.len = m->m_len = sizeof(*eh) + blen;

	if ((ic->ic_flags & IEEE80211_F_RSNON)
			&& ntohs(eh->ether_type) == ETHERTYPE_EAPOL) {
		if (sc->sc_eapol_logged < 8) {
			sc->sc_eapol_logged++;
			printf("%s: EAPOL rx %u bytes\n", AIC_NAME,
				(unsigned)m->m_pkthdr.len);
		}
		ifp->if_ipackets++;
		ieee80211_eapol_key_input(ic, m, ic->ic_bss);
	} else
		ml_enqueue(ml, m);
}


static void
aic_rx_data(struct aic_softc *sc, const uint8_t *p, size_t len)
{
	static const uint8_t llc[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	const uint8_t *f, *da, *sa, *body;
	size_t flen, hdr, sec, off;
	uint32_t flags, status;

	if (len < AIC_RX_HWHDR_LEN)
		return;
	flen = p[0] | (p[1] << 8);
	if (flen + AIC_RX_HWHDR_LEN > len)
		return;
	flags = le32dec(p + AIC_RXH_FLAGS_WORD);
	status = le32dec(p + AIC_RXH_DECR_WORD);
	f = p + AIC_RX_HWHDR_LEN;

	if ((flags & AIC_RXH_FLAG_80211_MPDU) != 0) {
		/* management frames: the firmware handles auth/assoc itself */
		sc->sc_rx_mgmt++;
		return;
	}
	if (ic->ic_state != IEEE80211_S_RUN || flen < 24
			|| (f[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA)
		return;

	hdr = 24;
	int amsdu = 0;
	if (f[0] & IEEE80211_FC0_SUBTYPE_QOS) {
		hdr += 2;
		amsdu = (f[24] & IEEE80211_QOS_AMSDU) != 0;
	}
	if (f[1] & IEEE80211_FC1_ORDER)
		hdr += 4;		/* HT control */
	if ((f[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_FROMDS) {
		da = f + 4;		/* addr1 */
		sa = f + 16;	/* addr3 */
	} else {
		da = f + 4;
		sa = f + 10;
	}
	switch ((status >> 2) & 7) {
	case 0:				sec = 0; break;	/* unencrypted */
	case 1:				sec = 4; break;	/* WEP */
	case 7:				sec = 18; break;	/* WAPI */
	default:			sec = 8; break;	/* TKIP, CCMP, GCMP */
	}
	if (hdr + sec > flen)
		return;
	off = hdr + sec;

	if (!amsdu) {
		if (off + 8 > flen || memcmp(f + off, llc, 6) != 0) {
			ifp->if_ierrors++;
			return;
		}
		aic_rx_deliver(sc, da, sa, f + off + 6, f + off + 8,
			flen - off - 8, &ml);
	} else {
		/* A-MSDU subframes: DA, SA, length, LLC + payload, pad to 4 */
		while (off + 14 + 8 <= flen) {
			size_t slen = (f[off + 12] << 8) | f[off + 13];
			if (slen < 8 || off + 14 + slen > flen)
				break;
			body = f + off + 14;
			if (memcmp(body, llc, 6) == 0)
				aic_rx_deliver(sc, f + off, f + off + 6, body + 6,
					body + 8, slen - 8, &ml);
			off += roundup(14 + slen, 4);
		}
	}
	if_input(ifp, &ml);
}


/*
 * rwnx_start_xmit() + aicwf_sdio_aggr() (one frame per write): 4-byte SDIO
 * header (type 0x01 data, CRC-8 on the D80), struct txdesc_api, then the
 * frame without its Ethernet header; padded to 4, tail, 512-byte blocks.
 */
struct aic_hostdesc {			/* ipc_shared.h struct hostdesc, FULLMAC */
	uint16_t	packet_len;
	uint16_t	flags_ext;
	uint32_t	hostid;
	uint8_t		eth_dest_addr[6];
	uint8_t		eth_src_addr[6];
	uint8_t		ethertype[2];	/* big endian, as on the wire */
	uint8_t		ac;
	uint8_t		tid;
	uint8_t		vif_idx;
	uint8_t		staid;
	uint16_t	flags;
} __packed;

#define AIC_SDIO_TYPE_DATA		0x01
#define AIC_DATA_FLOW_THRESH	2	/* DATA_FLOW_CTRL_THRESH */

static int
aic_tx(struct aic_softc *sc, struct mbuf *m)
{
	uint8_t *b = sc->sc_txbuf;
	struct aic_hostdesc *d = (struct aic_hostdesc *)(b + 4);
	struct ether_header eh;
	size_t plen, len, total;
	int buffers;
	status_t status;

	if (m->m_pkthdr.len < (int)sizeof(eh))
		return EINVAL;
	plen = m->m_pkthdr.len - sizeof(eh);
	if (4 + sizeof(*d) + plen + AIC_TAIL_LEN + 4 > AIC_TXBUF_SIZE)
		return EMSGSIZE;
	m_copydata(m, 0, sizeof(eh), (caddr_t)&eh);

	memset(b, 0, 4 + sizeof(*d));
	len = sizeof(*d) + plen;
	b[0] = len & 0xff;
	b[1] = (len >> 8) & 0x0f;
	b[2] = AIC_SDIO_TYPE_DATA;
	b[3] = aic_crc8_ponl_107(b, 3);
	d->packet_len = htole16(plen);
	memcpy(d->eth_dest_addr, eh.ether_dhost, 6);
	memcpy(d->eth_src_addr, eh.ether_shost, 6);
	memcpy(d->ethertype, &eh.ether_type, 2);
	d->tid = 0;
	d->ac = AC_BE;
	d->vif_idx = sc->sc_vif_idx;
	d->staid = sc->sc_ap_idx;
	m_copydata(m, sizeof(eh), plen, (caddr_t)(b + 4 + sizeof(*d)));

	total = roundup(4 + len, 4);
	memset(b + 4 + len, 0, total - (4 + len) + AIC_TAIL_LEN);
	if (total % AIC_BLOCK_SIZE != 0)
		total += AIC_TAIL_LEN;
	total = roundup(total, AIC_BLOCK_SIZE);
	if (total > AIC_TXBUF_SIZE)
		return EMSGSIZE;

	buffers = aic_flow_ctrl(sc);
	if (buffers <= AIC_DATA_FLOW_THRESH)
		return ENOBUFS;
	status = SDIO_OPS(sc)->write(SDIO_COOKIE(sc), 1, AIC_REG_WR_FIFO, b,
		total, 0);
	if (status != B_OK) {
		printf("%s: data write (%u bytes) failed: %s\n", AIC_NAME,
			(unsigned)total, strerror(status));
		return aic_status_to_errno(status);
	}
	return 0;
}


static void
aic_start(struct ifnet *ifp)
{
	struct aic_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m;

	for (;;) {
		if (ic->ic_state != IEEE80211_S_RUN || !sc->sc_connected) {
			/* nothing can leave yet; drop what is queued */
			IFQ_DEQUEUE(&ifp->if_snd, m);
			if (m == NULL)
				break;
			m_freem(m);
			continue;
		}
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		if (m->m_pkthdr.len >= (int)sizeof(struct ether_header)
				&& ntohs(mtod(m, struct ether_header *)->ether_type)
					== ETHERTYPE_EAPOL && sc->sc_eapol_logged < 16) {
			sc->sc_eapol_logged++;
			printf("%s: EAPOL tx %d bytes\n", AIC_NAME, m->m_pkthdr.len);
		}
		rw_enter_write(&sc->sc_sdmmc.sc_lock);
		int error = aic_tx(sc, m);
		rw_exit(&sc->sc_sdmmc.sc_lock);
		if (error != 0)
			ifp->if_oerrors++;
		else
			ifp->if_opackets++;
		m_freem(m);
	}
}


/* firmware indications and RX data, outside the SDIO lock, in task context */
static void
aic_ind_task(void *arg)
{
	struct aic_softc *sc = arg;
	struct aic_ind *ind;

	for (;;) {
		mtx_enter(&sc->sc_indq_mtx);
		ind = TAILQ_FIRST(&sc->sc_indq);
		if (ind != NULL) {
			TAILQ_REMOVE(&sc->sc_indq, ind, link);
			sc->sc_indq_len--;
		}
		mtx_leave(&sc->sc_indq_mtx);
		if (ind == NULL)
			break;

		switch (ind->id) {
		case AIC_IND_DATA:
			aic_rx_data(sc, ind->data, ind->len);
			break;
		case AIC_IND_WATCHDOG:
			aic_watchdog_fired(sc);
			break;
		case SCANU_RESULT_IND:
			aic_scan_result(sc, ind->data, ind->len);
			break;
		case SCANU_START_CFM:
			aic_scan_done(sc, ind->data, ind->len);
			break;
		case SM_CONNECT_IND:
			aic_connect_ind(sc, ind->data, ind->len);
			break;
		case SM_DISCONNECT_IND:
			aic_disconnect_ind(sc, ind->data, ind->len);
			break;
		case MM_CHANNEL_SURVEY_IND:
			/* per-channel busy time and noise after each scanned
			 * channel (Linux keeps it for nl80211 survey); not used */
			break;
		default:
			if (sc->sc_unsolicited_logged < 16) {
				sc->sc_unsolicited_logged++;
				printf("%s: firmware indication 0x%04x (task %u, index "
					"%u), %u bytes\n", AIC_NAME, ind->id, MSG_T(ind->id),
					MSG_I(ind->id), ind->len);
			}
			break;
		}
		free(ind, M_DEVBUF, sizeof(*ind) + ind->len);
	}
}


static int
aic_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct aic_softc *sc = ic->ic_softc;

	/* leaving a join or a link: tell the firmware */
	if (nstate <= IEEE80211_S_SCAN && ic->ic_state >= IEEE80211_S_AUTH)
		aic_disconnect(sc);

	switch (nstate) {
	case IEEE80211_S_SCAN:
		if (aic_vif_up(sc) != 0 || aic_scan(sc) != 0) {
			printf("%s: cannot start a scan\n", AIC_NAME);
			return 0;
		}
		if (ic->ic_state == IEEE80211_S_SCAN)
			return 0;
		ieee80211_set_link_state(ic, LINK_STATE_DOWN);
		ieee80211_free_allnodes(ic, 1);
		ic->ic_state = nstate;
		return 0;
	case IEEE80211_S_AUTH:
		/* as bwfm_newstate(): the firmware joins, net80211 waits */
		ic->ic_bss->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
		if (aic_connect(sc) != 0) {
			ic->ic_bss->ni_fails++;
			ieee80211_begin_scan(&ic->ic_if);
			return 0;
		}
		ic->ic_state = nstate;
		if (ic->ic_flags & IEEE80211_F_RSNON)
			ic->ic_bss->ni_rsn_supp_state = RSNA_SUPP_PTKSTART;
		return 0;
	default:
		break;
	}
	return sc->sc_newstate(ic, nstate, arg);
}


static void
aic_init(struct ifnet *ifp)
{
	struct aic_softc *sc = ifp->if_softc;

	if (aic_vif_up(sc) != 0) {
		printf("%s: cannot bring the station interface up\n", AIC_NAME);
		return;
	}
	ifp->if_flags |= IFF_RUNNING;
	ieee80211_begin_scan(ifp);
}


static void
aic_stop(struct ifnet *ifp)
{
	struct aic_softc *sc = ifp->if_softc;

	ifp->if_flags &= ~IFF_RUNNING;
	ieee80211_new_state(&sc->sc_ic, IEEE80211_S_INIT, -1);
}


static int
aic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING))
				aic_init(ifp);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				aic_stop(ifp);
		}
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
	}
	if (error == ENETRESET) {
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING))
				== (IFF_UP | IFF_RUNNING)) {
			aic_stop(ifp);
			aic_init(ifp);
		}
		error = 0;
	}
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
	unsigned i;
	int chan;

	ic->ic_softc = sc;
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_WEP | IEEE80211_C_RSN | IEEE80211_C_SCANALL
		| IEEE80211_C_SCANALLBAND;

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (chan = 1; chan <= 13; chan++) {
		ic->ic_channels[chan].ic_freq =
			ieee80211_ieee2mhz(chan, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_CCK
			| IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN
			| IEEE80211_CHAN_2GHZ;
	}
	if (sc->sc_5g) {
		ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
		for (i = 0; i < nitems(aic_5ghz_channels); i++) {
			chan = aic_5ghz_channels[i];
			ic->ic_channels[chan].ic_freq =
				ieee80211_ieee2mhz(chan, IEEE80211_CHAN_5GHZ);
			ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_A;
		}
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
	ic->ic_send_mgmt = aic_send_mgmt;
	ic->ic_set_key = aic_set_key;
	ic->ic_delete_key = aic_delete_key;

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
	TAILQ_INIT(&sc->sc_indq);
	mtx_init(&sc->sc_indq_mtx, IPL_NET);
	task_set(&sc->sc_ind_task, aic_ind_task, sc);
	timeout_set(&sc->sc_watchdog_to, aic_watchdog, sc);
	sc->sc_pairwise_hw = AIC_NO_KEY;
	memset(sc->sc_group_hw, AIC_NO_KEY, sizeof(sc->sc_group_hw));
	sc->sc_poll_thread = -1;

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

	/* from here on replies, indications and data come by polling */
	sc->sc_poll_thread = spawn_kernel_thread(aic_poll_thread, "aic8800 rx",
		B_REAL_TIME_DISPLAY_PRIORITY, sc);
	if (sc->sc_poll_thread >= 0)
		resume_thread(sc->sc_poll_thread);
	printf("%s: attached, RX by polling (%s)\n", AIC_NAME,
		sc->sc_poll_thread >= 0 ? "1-10 ms" : "NO poll thread");
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

	if (sc->sc_poll_thread >= 0) {
		status_t ret;
		sc->sc_poll_quit = 1;
		wait_for_thread(sc->sc_poll_thread, &ret);
		sc->sc_poll_thread = -1;
	}
	timeout_del(&sc->sc_watchdog_to);
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
