/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * OpenBSD kernel API bits used by bwfm(4) that the Haiku OpenBSD compat
 * layer (src/libs/compat/openbsd_network) does not provide. Included by
 * bwfm.c and if_bwfm_sdio.c after their own includes (__HAIKU__ only).
 */
#ifndef _BEACON_BWFM_HAIKU_H_
#define _BEACON_BWFM_HAIKU_H_


#include <sys/task.h>
#include <sys/device.h>
#include <net/ifq.h>
#include <net/if_types.h>


/* Log prefix: every driver line starts with "bwfm:". */
#undef DEVNAME
#define DEVNAME(sc)		"bwfm"

#define IPL_SOFTNET		IPL_NET

#define delay(us)		DELAY(us)

#define PR_WAITOK		0
#define dma_alloc(size, flags)	malloc((size), M_DEVBUF, M_WAITOK | M_ZERO)
#define dma_free(p, size)		free((p), M_DEVBUF, (size))

#ifndef mallocarray
#define mallocarray(nmemb, size, type, flags) \
	malloc((size_t)(nmemb) * (size), (type), (flags))
#endif

typedef uintptr_t vaddr_t;

/* FreeBSD has no MT_CONTROL here; bwfm only uses it to tag its own frames. */
#ifndef MT_CONTROL
#define MT_CONTROL		14
#endif

#define htolem32(p, v)	(*(uint32_t *)(p) = htole32(v))

#define ifq_empty(IFQ)		IFQ_IS_EMPTY(IFQ)
#define ifq_restart(IFQ)	((ifp)->if_start(ifp))

/* All tasks run on the shared fast taskqueue (see compat sys/task.h). */
static inline void
taskq_barrier(struct taskq *tq)
{
	taskqueue_drain_all(taskqueue_fast);
}

static inline void
taskq_destroy(struct taskq *tq)
{
}


#endif	/* _BEACON_BWFM_HAIKU_H_ */
