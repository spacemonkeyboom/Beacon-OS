/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * JH7110 HDMI audio path register maps: Synopsys DesignWare I2S TX0
 * (0x120b0000) and the audio part of the Innosilicon HDMI TX (0x29590000).
 *
 * Adapted from:
 *  - Linux v6.12 sound/soc/dwc/local.h (GPL-2.0, Copyright (ST) 2012
 *    Rajeev Kumar) and dwc-i2s.c (Copyright (C) 2010 ST Microelectronics,
 *    Rajeev Kumar): DW I2S register offsets, ISR bits, COMP_PARAM_1
 *    fields, S16 CCR/TCR values.
 *  - StarFive vendor kernel drivers/gpu/drm/verisilicon/
 *    starfive_hdmi_audio.c (GPL-2.0, Copyright (C) 2022 StarFive
 *    Technology Co., Ltd., Xingyu Wu), branch JH7110_VisionFive2_devel:
 *    HDMI audio register numbers and bit fields.
 * Modified for Beacon (2026): C++ constants. See THIRD_PARTY.md.
 */
#ifndef I2S_HDMI_REGS_H
#define I2S_HDMI_REGS_H


#include <SupportDefs.h>


// DesignWare I2S (Linux local.h)
#define I2S_IER					0x000
#define I2S_ITER				0x008
#define I2S_CER					0x00c
#define I2S_CCR					0x010
#define I2S_TXFFR				0x018
#define I2S_LTHR(x)				(0x40 * (x) + 0x020)
#define I2S_RTHR(x)				(0x40 * (x) + 0x024)
#define I2S_TER(x)				(0x40 * (x) + 0x02c)
#define I2S_TCR(x)				(0x40 * (x) + 0x034)
#define I2S_ISR(x)				(0x40 * (x) + 0x038)
#define I2S_IMR(x)				(0x40 * (x) + 0x03c)
#define I2S_TOR(x)				(0x40 * (x) + 0x044)
#define I2S_TFCR(x)				(0x40 * (x) + 0x04c)
#define I2S_TFF(x)				(0x40 * (x) + 0x054)
#define I2S_COMP_PARAM_2		0x1f0
#define I2S_COMP_PARAM_1		0x1f4

#define I2S_ISR_TXFO			(1u << 5)
#define I2S_ISR_TXFE			(1u << 4)
#define I2S_IMR_TX_MASK			0x30u		// TXFO | TXFE
#define I2S_TER_TXCHEN			(1u << 0)

#define I2S_COMP1_TX_WORDSIZE_0(r)	(((r) >> 16) & 0x7)
#define I2S_COMP1_TX_CHANNELS(r)	(((r) >> 9) & 0x3)
#define I2S_COMP1_TX_ENABLED(r)		(((r) >> 5) & 0x1)
#define I2S_COMP1_MODE_EN(r)		(((r) >> 4) & 0x1)
#define I2S_COMP1_FIFO_DEPTH_GLOBAL(r)	(((r) >> 2) & 0x3)
// Linux dw_configure_dai(): fifo_depth = 1 << (1 + FIFO_DEPTH_GLOBAL)

// dw_i2s_hw_params(), S16_LE: ccr = 0x00, xfer_resolution = 0x02;
// S32_LE: ccr = 0x10, xfer_resolution = 0x05.
#define I2S_CCR_WSS_16			0x00
#define I2S_CCR_WSS_32			0x10
#define I2S_TCR_WLEN_16			0x02


// Innosilicon HDMI TX: byte registers at a 4-byte stride (vendor
// hdmi_writeb(reg) = writel(base + reg * 4)).
#define HDMI_DEV_MUTE			0x005
#define HDMI_AUDIO_CFG			0x035
#define HDMI_SAMPLE_FRE			0x037
#define HDMI_PINS_ENA			0x038
#define HDMI_CHANNEL_INPUT		0x039
#define HDMI_N_VALUE1			0x03f
#define HDMI_N_VALUE2			0x040
#define HDMI_N_VALUE3			0x041
#define HDMI_CTS_VALUE1			0x045
#define HDMI_CTS_VALUE2			0x046
#define HDMI_CTS_VALUE3			0x047

// DEV_MUTE (shared with video mute bits: read-modify-write only)
#define HDMI_AUDIO_MUTE_MASK	(1u << 1)
#define HDMI_AUDIO_MUTE			(1u << 1)
#define HDMI_AUDIO_NO_MUTE		0x0

// AUDIO_CFG
#define HDMI_MCLK_RATIO_MASK	0x03u
#define HDMI_MCLK_256FS			0x01u
#define HDMI_AUDIO_TYPE_SEL_MASK 0x18u
#define HDMI_AUDIO_SEL_I2S		0x00u
#define HDMI_CTS_SOURCE_SEL_MASK 0x80u
#define HDMI_CTS_EXTER			0x80u

// SAMPLE_FRE
#define HDMI_FREQ_32K			0x3
#define HDMI_FREQ_44K			0x0
#define HDMI_FREQ_48K			0x2

// PINS_ENA
#define HDMI_I2S_FORMAT_MASK	0x03u
#define HDMI_I2S_STANDARD_MODE	0x00u
#define HDMI_I2S_PIN_ENA_MASK	0x3cu
#define HDMI_I2S0_ENA			(1u << 2)

// CHANNEL_INPUT: vendor init value CHANNEL0_I2S0 | CHANNEL1_I2S1 |
// CHANNEL2_I2S2 | CHANNEL3_I2S3 (every field 0).
#define HDMI_CHANNEL_INPUT_DEFAULT	0x00u


#endif	// I2S_HDMI_REGS_H
