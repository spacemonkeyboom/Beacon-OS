/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * StarFive JH7110 PWM-DAC register map and default configuration.
 *
 * Adapted from Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c
 * (GPL-2.0, Copyright (C) 2021-2023 StarFive Technology Co., Ltd.;
 * authors Jenny Zhang, Curry Zhang, Xingyu Wu, Hal Feng) and the StarFive
 * vendor kernel sound/soc/starfive/pwmdac.h + starfive_pwmdac_pcm.c
 * (GPL-2.0, Copyright (C) 2022 StarFive Technology Co., Ltd.), branch
 * JH7110_VisionFive2_devel. Modified for Beacon (2026): C++ constants,
 * JH7110 TRM state register notes. See THIRD_PARTY.md.
 */
#ifndef PWMDAC_REGS_H
#define PWMDAC_REGS_H


#include <SupportDefs.h>


// PWM-DAC block (reg = <0x100b0000 0x1000> in both the vendor and the Linux
// device tree).
#define PWMDAC_WDATA				0x00	// write one FIFO entry
#define PWMDAC_CTRL					0x04
#define PWMDAC_STATE				0x08	// vendor "SATAE"; FIFO flags

#define PWMDAC_CTRL_ENABLE			(1u << 0)
#define PWMDAC_CTRL_SHIFT_10BIT		(1u << 1)	// 0 = 8-bit PWM, 1 = 10-bit
#define PWMDAC_CTRL_DUTY_SHIFT		2
#define PWMDAC_CTRL_DUTY_MASK		(3u << 2)
#define PWMDAC_CTRL_CNT_N_SHIFT		4
#define PWMDAC_CTRL_CNT_N_MASK		(0x1ffu << 4)
#define PWMDAC_CTRL_DATA_CHANGE		(1u << 13)	// swap left/right
#define PWMDAC_CTRL_DATA_MODE		(1u << 14)	// 1 = invert MSB (signed in)
#define PWMDAC_CTRL_DATA_SHIFT_SHIFT 15
#define PWMDAC_CTRL_DATA_SHIFT_MASK	(7u << 15)

#define PWMDAC_DUTY_LEFT			0
#define PWMDAC_DUTY_RIGHT			1
#define PWMDAC_DUTY_CENTER			2

// FIFO flags in PWMDAC_STATE. The two sources disagree:
//  - JH7110 TRM (register_info_pwmdac): bit 0 = FIFO full, bit 1 = FIFO empty
//  - vendor kernel get_pwmdac_fifo_state(): (value & 0x02) != 0 = FIFO full
// The driver therefore finds out at attach time which bit rises while the
// FIFO is being filled (see PwmdacDevice::_ProbeFifo()).
#define PWMDAC_STATE_BIT0			(1u << 0)
#define PWMDAC_STATE_BIT1			(1u << 1)

// A FIFO entry holds one stereo frame: left sample in bits 15:0, right in
// 31:16 (vendor sf_pwmdac_pcm_tx_16(); the Linux DMA path moves the same
// little-endian S16 frames with a 4-byte bus width). In 8-bit PWM mode the
// upper byte of each half is used (vendor sf_pwmdac_pcm_tx_8() places 8-bit
// samples at bits 15:8 and 31:24).


// Linux jh7110_pwmdac_init_params() defaults: 8-bit, centre-aligned, N = 1,
// no L/R swap, MSB inverted (S16_LE input), no data shift. The sample rate
// is core_clock / (256 * N) in 8-bit mode (Linux hw_params table: 48 kHz =
// 12.288 MHz with N = 1, 16 kHz = 12.288 MHz with N = 3, 8 kHz = 6.144 MHz
// with N = 3), and core_clock / (1024 * N) in 10-bit mode.
static inline uint32
pwmdac_ctrl_value(bool tenBit, uint32 countN)
{
	uint32 value = 0;
	if (tenBit)
		value |= PWMDAC_CTRL_SHIFT_10BIT;
	value |= PWMDAC_DUTY_CENTER << PWMDAC_CTRL_DUTY_SHIFT;
	value |= ((countN - 1) & 0x1ff) << PWMDAC_CTRL_CNT_N_SHIFT;
	value |= PWMDAC_CTRL_DATA_MODE;
	// DATA_CHANGE = 0, DATA_SHIFT = 0
	return value;
}


#endif	// PWMDAC_REGS_H
