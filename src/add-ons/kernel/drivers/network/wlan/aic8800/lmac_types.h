/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Beacon replacement for the Linux aic8800_fdrv lmac_types.h, which only
 * pulled in <linux/types.h>, <linux/version.h> and <linux/bits.h>. Gives
 * lmac_mac.h and lmac_msg.h (copied unchanged from the Linux driver) the
 * kernel type names they use.
 *
 * Copyright 2026, The Beacon Project.
 */
#ifndef _LMAC_INT_H_
#define _LMAC_INT_H_

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
typedef int8_t		s8;
typedef int16_t		s16;
typedef int32_t		s32;
typedef int64_t		s64;
typedef uint16_t	__le16;
typedef uint32_t	__le32;
typedef uint16_t	__be16;

#ifndef BIT
#define BIT(n)		(1UL << (n))
#endif

#endif /* _LMAC_INT_H_ */
