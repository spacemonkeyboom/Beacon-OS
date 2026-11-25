/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Haiku replacement for OpenBSD's <dev/sdmmc/sdmmcvar.h>: just the parts of
 * the OpenBSD sdmmc(4) SDIO API that bwfm(4) uses, implemented by
 * sdmmc_io_haiku.c on top of the Beacon SDIO bus interface
 * (headers/private/drivers/sdio_bus.h). The structure and function names
 * follow OpenBSD so that if_bwfm_sdio.c needs few changes; the code is new.
 */
#ifndef _BEACON_SDMMCVAR_H_
#define _BEACON_SDMMCVAR_H_


#include <sys/param.h>
#include <sys/rwlock.h>


#define SDMMC_MAX_FUNCTIONS	8	/* function 0 (CCCR) + 1..7 */

struct sdio_bus_interface;
struct haiku_sdio_device;
struct sdmmc_softc;

struct sdmmc_cis {
	uint16_t	manufacturer;	/* CISTPL_MANFID of the card */
	uint16_t	product;
	uint8_t		function;		/* FBR standard interface code */
};

struct sdmmc_function {
	struct sdmmc_softc	*sc;
	int					 number;		/* 0..7 */
	unsigned int		 cur_blklen;	/* as last written by set_blocklen */
	struct sdmmc_cis	 cis;
	void				*cookie;		/* OpenBSD: FDT node; unused */
	int					 present;
};

struct sdmmc_softc {
	struct rwlock			 sc_lock;		/* held around every I/O */
	struct sdmmc_function	*sc_fn0;
	int						 sc_function_count;	/* functions 1..count */
	struct sdmmc_function	 sc_functions[SDMMC_MAX_FUNCTIONS];

	/* Beacon SDIO bus */
	struct sdio_bus_interface	*sc_sdio;
	void						*sc_sdio_cookie;
	struct haiku_sdio_device	*sc_haiku;
	volatile int				 sc_io_timeouts;
		/* Beacon: consecutive timed-out SDIO operations (reset on
		 * success); a dead card makes this climb fast */

	/* one card interrupt handler (as in OpenBSD, per sdmmc bus) */
	int		(*sc_intr_fn)(void *);
	void	 *sc_intr_arg;
	int		  sc_intr_enabled;	/* bit n = function n handler installed */
	char	  sc_intr_name[16];
};

struct sdmmc_attach_args {
	struct sdmmc_function	*sf;
};


/* Beacon: set up / tear down the softc for one SDIO card. */
int		sdmmc_haiku_attach(struct sdmmc_softc *, struct haiku_sdio_device *);
void	sdmmc_haiku_detach(struct sdmmc_softc *);

/* OpenBSD sdmmc_io.c API (subset). The caller holds sc_lock for writing. */
uint8_t		sdmmc_io_read_1(struct sdmmc_function *, int);
void		sdmmc_io_write_1(struct sdmmc_function *, int, uint8_t);
uint16_t	sdmmc_io_read_2(struct sdmmc_function *, int);
void		sdmmc_io_write_2(struct sdmmc_function *, int, uint16_t);
uint32_t	sdmmc_io_read_4(struct sdmmc_function *, int);
void		sdmmc_io_write_4(struct sdmmc_function *, int, uint32_t);
int			sdmmc_io_read_multi_1(struct sdmmc_function *, int, u_char *, int);
int			sdmmc_io_write_multi_1(struct sdmmc_function *, int, u_char *, int);
int			sdmmc_io_read_region_1(struct sdmmc_function *, int, u_char *,
				int);
int			sdmmc_io_write_region_1(struct sdmmc_function *, int, u_char *,
				int);
void		sdmmc_io_set_blocklen(struct sdmmc_function *, unsigned int);
int			sdmmc_io_function_ready(struct sdmmc_function *);
int			sdmmc_io_function_enable(struct sdmmc_function *);
void		sdmmc_io_function_disable(struct sdmmc_function *);
int			sdmmc_io_reset_card(struct sdmmc_softc *);
			/* Beacon: power-cycle and re-enumerate the card */

/*
 * Card interrupt. OpenBSD takes the sdmmc bus device; here it is the
 * softc. The handler runs in the SDIO bus manager's "sdio irq" thread with
 * the card interrupt masked and Giant held (like OpenBSD's sdmmc_intr_task,
 * which runs with the kernel lock), so it may sleep and do SDIO I/O.
 */
void	*sdmmc_intr_establish(struct sdmmc_softc *, int (*)(void *), void *,
			const char *);
void	 sdmmc_intr_disestablish(void *);
void	 sdmmc_intr_enable(struct sdmmc_function *);
void	 sdmmc_intr_disable(struct sdmmc_function *);


#endif	/* _BEACON_SDMMCVAR_H_ */
