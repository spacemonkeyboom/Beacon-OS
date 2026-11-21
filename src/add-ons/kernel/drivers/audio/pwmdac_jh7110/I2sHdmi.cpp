/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright 2026, The Beacon Project.
 *
 * JH7110 HDMI audio sink for Beacon's jh7110 audio driver: DesignWare I2S
 * TX0 (0x120b0000, I2S master, 2 channels, 16 bit) feeding the internal
 * I2S0 input of the Innosilicon HDMI TX (0x29590000). PIO from the
 * engine's timer, no DMA.
 *
 * Adapted from (GPL-2.0, see THIRD_PARTY.md):
 *  - Linux v6.12 sound/soc/dwc/dwc-i2s.c (Copyright (C) 2010 ST
 *    Microelectronics, Rajeev Kumar; JH7110 support by StarFive):
 *    i2s_start()/i2s_stop() and dw_i2s_config() register order, S16
 *    CCR/TCR values, TFCR from the FIFO depth, FIFO depth from
 *    COMP_PARAM_1, the JH7110 IMR quirk, jh7110_i2s_crg_master_init()
 *    clock/reset order (mclk parent = mclk_inner).
 *  - StarFive vendor kernel (branch JH7110_VisionFive2_devel)
 *    drivers/gpu/drm/verisilicon/starfive_hdmi_audio.c (Copyright (C)
 *    2022 StarFive Technology Co., Ltd., Xingyu Wu): HDMI audio init
 *    (CTS external, I2S source, MCLK 256 fs, standard I2S, channel map,
 *    mute), per-stream setup (SAMPLE_FRE, I2S0 enable, N = 128 fs / 1000,
 *    CTS = TMDS / 1000), trigger = DEV_MUTE audio bit (read-modify-write,
 *    as hdmi_modb()); sound/soc/dwc/dwc-i2s.c dw_i2stx_4ch0_clk_init()
 *    and the generic bit clock = rate x data width x 2 (32 fs for S16).
 * Facts (offsets, no code) from Linux clk-starfive-jh7110-sys.c,
 * clk-starfive-jh7110-vout.c, clk-starfive-jh71x0.h, reset-starfive-jh7110.c
 * and dt-bindings starfive,jh7110-crg.h.
 *
 * Only HDMI audio registers are written (0x05 bit 1 by read-modify-write,
 * 0x35, 0x37-0x39, 0x3f-0x41, 0x45-0x47). The video, PHY and PLL state the
 * EFI loader restored from U-Boot is never touched.
 */


#include "Jh7110Audio.h"
#include "I2sHdmiRegs.h"

#include <new>
#include <stdio.h>
#include <string.h>


static const phys_addr_t kI2sTx0Base = 0x120b0000;
static const phys_addr_t kHdmiBase = 0x29590000;
static const phys_addr_t kVoutCrgBase = 0x295c0000;

// SYSCRG clocks (index x 4)
static const uint32 kClkAudioRoot = JH7110_SYSCLK(16);
static const uint32 kClkMclkInner = JH7110_SYSCLK(17);	// div max 64
static const uint32 kClkMclk = JH7110_SYSCLK(18);		// mux: inner / ext
static const uint32 kClkVoutTopAhb = JH7110_SYSCLK(61);	// gate
static const uint32 kClkHdmitx0Mclk = JH7110_SYSCLK(63);	// gate of mclk
static const uint32 kClkI2stx0Apb = JH7110_SYSCLK(161);	// gate of apb0
static const uint32 kClkI2stx0BclkMst = JH7110_SYSCLK(162);	// gate+div 32
static const uint32 kClkI2stx0LrckMst = JH7110_SYSCLK(164);	// mux+div 64
static const uint32 kClkI2stx0Bclk = JH7110_SYSCLK(165);	// mux: mst / ext
static const uint32 kClkI2stx0Lrck = JH7110_SYSCLK(167);	// mux: mst / ext
static const uint32 kMclkInnerDivMax = 64;
static const uint32 kLrckMstParentBclkMst = 1;	// parents: bclk_mst_inv, bclk_mst

// SYS resets
static const uint32 kRstVoutTopSrc = 43;
static const uint32 kRstI2stx0Apb = 101;
static const uint32 kRstI2stx0Bclk = 102;

// VOUTCRG (0x295c0000): clock n = word n * 4; resets assert 0x48 /
// status 0x4c (reset-starfive-jh7110.c jh7110_vout_info).
static const uint32 kVoutClkHdmiTxMclk = 15 * 4;
static const uint32 kVoutClkHdmiTxBclk = 16 * 4;
static const uint32 kVoutClkHdmiTxSys = 17 * 4;
static const uint32 kVoutRstAssert = 0x48;
static const uint32 kVoutRstStatus = 0x4c;
static const uint32 kVoutRstHdmiTx = 1u << 9;		// VOUTRST_HDMI_TX_HDMI

static const uint32 kDefaultTmdsKHz = 148500;	// 1080p60, loader replay


class HdmiSink : public AudioSink {
public:
							HdmiSink(volatile uint32* sysCrg,
								const char* name, uint32 bclkFs,
								uint32 tmdsKHz, uint32 forcedN);
	virtual					~HdmiSink();

	virtual	const char*		Label() const { return "hdmi"; }
	virtual	bool			Available() const { return fReady; }
	virtual	void			LogFirmwareState();
	virtual	status_t		Init();
	virtual	uint32			SupportedRates() const
								{ return B_SR_44100 | B_SR_48000; }
	virtual	status_t		ProgramRate(uint32 rateCode, uint32& milliHz);
	virtual	uint32			FifoDepth() const { return fFifoDepth; }
	virtual	status_t		HwStart();
	virtual	void			HwStop();
	virtual	void			BeginTick(bigtime_t now);
	virtual	uint32			Push(const uint32* frames, uint32 count);
	virtual	void			StatsExtra(char* buffer, size_t size) const;

private:
			bool			_VoutTopRunning() const;
			bool			_HdmiRunning() const;
			void			_LogCompParams();
			uint32			_I2sRead(uint32 offset) const
								{ return fI2s[offset / 4]; }
			void			_I2sWrite(uint32 offset, uint32 value)
								{ fI2s[offset / 4] = value; }
			uint32			_HdmiRead(uint32 reg) const
								{ return fHdmi[reg] & 0xff; }
			void			_HdmiWrite(uint32 reg, uint32 value)
								{ fHdmi[reg] = value & 0xff; }
			void			_HdmiModify(uint32 reg, uint32 mask,
								uint32 value);
			void			_SetClock(uint32 offset, uint32 mask,
								uint32 value);

			volatile uint32* fSysCrg;
			char			fName[40];
			uint32			fBclkFs;		// 32 (vendor) or 64 (Linux)
			uint32			fTmdsKHz;
			uint32			fForcedN;
			bool			fReady;
			bool			fCompLogged;
			area_id			fI2sArea;
			volatile uint32* fI2s;
			area_id			fHdmiArea;
			volatile uint32* fHdmi;
			area_id			fVoutArea;
			volatile uint32* fVout;

			uint32			fFifoDepth;
			uint32			fThreshold;		// TFCR value
			uint32			fHz;
			uint32			fMilliHz;

			// timer hook state
			uint32			fBudget;
			uint32			fLastIsr;
			int64			fOverruns;
			int64			fBudgetRefills;
};


HdmiSink::HdmiSink(volatile uint32* sysCrg, const char* name, uint32 bclkFs,
		uint32 tmdsKHz, uint32 forcedN)
	:
	fSysCrg(sysCrg),
	fBclkFs(bclkFs == 64 ? 64 : 32),
	fTmdsKHz(tmdsKHz != 0 ? tmdsKHz : kDefaultTmdsKHz),
	fForcedN(forcedN),
	fReady(false),
	fCompLogged(false),
	fI2sArea(-1),
	fI2s(NULL),
	fHdmiArea(-1),
	fHdmi(NULL),
	fVoutArea(-1),
	fVout(NULL),
	fFifoDepth(16),
	fThreshold(11),
	fHz(48000),
	fMilliHz(0),
	fBudget(0),
	fLastIsr(0),
	fOverruns(0),
	fBudgetRefills(0)
{
	snprintf(fName, sizeof(fName), "%s", name);
	// Mapping does not access the blocks; reads only happen after the
	// clock/reset checks below.
	fI2sArea = jh7110_map_page("jh7110 i2stx0 regs", kI2sTx0Base, fI2s);
	fHdmiArea = jh7110_map_page("jh7110 hdmi audio regs", kHdmiBase, fHdmi);
	fVoutArea = jh7110_map_page("jh7110 voutcrg", kVoutCrgBase, fVout);
}


HdmiSink::~HdmiSink()
{
	if (fReady)
		HwStop();
	if (fI2sArea >= 0)
		delete_area(fI2sArea);
	if (fHdmiArea >= 0)
		delete_area(fHdmiArea);
	if (fVoutArea >= 0)
		delete_area(fVoutArea);
}


bool
HdmiSink::_VoutTopRunning() const
{
	// VOUTCRG and the HDMI TX sit in the display power domain; only touch
	// them when its bus clock runs and its top reset is released (the
	// loader's U-Boot display replay leaves it so).
	return (fSysCrg[kClkVoutTopAhb / 4] & JH7110_CLK_ENABLE) != 0
		&& (fSysCrg[JH7110_SYSRST_STATUS(kRstVoutTopSrc) / 4]
			& JH7110_RST_BIT(kRstVoutTopSrc)) != 0;
}


bool
HdmiSink::_HdmiRunning() const
{
	return fVout != NULL && _VoutTopRunning()
		&& (fVout[kVoutClkHdmiTxSys / 4] & JH7110_CLK_ENABLE) != 0
		&& (fVout[kVoutRstStatus / 4] & kVoutRstHdmiTx) != 0;
}


void
HdmiSink::_LogCompParams()
{
	uint32 comp1 = _I2sRead(I2S_COMP_PARAM_1);
	uint32 comp2 = _I2sRead(I2S_COMP_PARAM_2);
	INFO("%s: i2stx0: COMP_PARAM_1 0x%08" B_PRIx32 " COMP_PARAM_2 0x%08"
		B_PRIx32 " (tx %s, %" B_PRIu32 " channel pair(s), FIFO depth %"
		B_PRIu32 ", tx word size code %" B_PRIu32 ", master mode %s)\n", fName,
		comp1, comp2, I2S_COMP1_TX_ENABLED(comp1) ? "present" : "ABSENT",
		I2S_COMP1_TX_CHANNELS(comp1) + 1,
		(uint32)1 << (1 + I2S_COMP1_FIFO_DEPTH_GLOBAL(comp1)),
		I2S_COMP1_TX_WORDSIZE_0(comp1),
		I2S_COMP1_MODE_EN(comp1) ? "yes" : "no");
	fCompLogged = true;
}


void
HdmiSink::LogFirmwareState()
{
	// SYSCRG: always readable.
	INFO("%s: firmware left (hdmi): audio_root 0x%08" B_PRIx32 " mclk_inner 0x%08"
		B_PRIx32 " mclk 0x%08" B_PRIx32 " vout_top_ahb 0x%08" B_PRIx32
		" hdmitx0_mclk 0x%08" B_PRIx32 "\n", fName,
		fSysCrg[kClkAudioRoot / 4], fSysCrg[kClkMclkInner / 4],
		fSysCrg[kClkMclk / 4], fSysCrg[kClkVoutTopAhb / 4],
		fSysCrg[kClkHdmitx0Mclk / 4]);
	INFO("%s: firmware left (hdmi): i2stx0_apb 0x%08" B_PRIx32 " bclk_mst 0x%08"
		B_PRIx32 " lrck_mst 0x%08" B_PRIx32 " bclk 0x%08" B_PRIx32 " lrck 0x%08"
		B_PRIx32 "; SYSRST word3 assert 0x%08" B_PRIx32 " status 0x%08" B_PRIx32
		" (i2stx0 apb/bclk = bits 5/6), word1 status 0x%08" B_PRIx32
		" (vout_top_src = bit 11)\n", fName,
		fSysCrg[kClkI2stx0Apb / 4], fSysCrg[kClkI2stx0BclkMst / 4],
		fSysCrg[kClkI2stx0LrckMst / 4], fSysCrg[kClkI2stx0Bclk / 4],
		fSysCrg[kClkI2stx0Lrck / 4],
		fSysCrg[JH7110_SYSRST_ASSERT(kRstI2stx0Apb) / 4],
		fSysCrg[JH7110_SYSRST_STATUS(kRstI2stx0Apb) / 4],
		fSysCrg[JH7110_SYSRST_STATUS(kRstVoutTopSrc) / 4]);

	// I2S: only readable with its APB clock on and out of reset.
	uint32 rstStatus = fSysCrg[JH7110_SYSRST_STATUS(kRstI2stx0Apb) / 4];
	if (fI2s != NULL
		&& (fSysCrg[kClkI2stx0Apb / 4] & JH7110_CLK_ENABLE) != 0
		&& (rstStatus & JH7110_RST_BIT(kRstI2stx0Apb)) != 0) {
		_LogCompParams();
	} else {
		INFO("%s: i2stx0: APB clock gated or in reset: COMP_PARAM read "
			"after clock enable (before any I2S write)\n", fName);
	}

	if (fVout == NULL || !_VoutTopRunning()) {
		INFO("%s: firmware left (hdmi): display domain not running "
			"(vout_top_ahb gated or vout_top_src in reset): VOUTCRG/HDMI "
			"not read\n", fName);
		return;
	}
	INFO("%s: firmware left (hdmi): VOUTCRG hdmi_tx_mclk 0x%08" B_PRIx32
		" hdmi_tx_bclk 0x%08" B_PRIx32 " hdmi_tx_sys 0x%08" B_PRIx32
		", reset assert 0x%08" B_PRIx32 " status 0x%08" B_PRIx32
		" (hdmi_tx = bit 9)\n", fName,
		fVout[kVoutClkHdmiTxMclk / 4], fVout[kVoutClkHdmiTxBclk / 4],
		fVout[kVoutClkHdmiTxSys / 4], fVout[kVoutRstAssert / 4],
		fVout[kVoutRstStatus / 4]);

	if (fHdmi == NULL || !_HdmiRunning()) {
		INFO("%s: firmware left (hdmi): HDMI TX sys clock gated or in reset:"
			" HDMI registers not read\n", fName);
		return;
	}
	char line[200];
	int pos = 0;
	for (uint32 reg = 0x35; reg <= 0x47 && pos < (int)sizeof(line) - 8;
			reg++) {
		pos += snprintf(line + pos, sizeof(line) - pos, " %02" B_PRIx32,
			_HdmiRead(reg));
	}
	INFO("%s: firmware left (hdmi): HDMI reg 0x05 DEV_MUTE 0x%02" B_PRIx32
		"; regs 0x35-0x47:%s\n", fName, _HdmiRead(HDMI_DEV_MUTE), line);
}


void
HdmiSink::_SetClock(uint32 offset, uint32 mask, uint32 value)
{
	uint32 old = fSysCrg[offset / 4];
	fSysCrg[offset / 4] = (old & ~mask) | (value & mask);
}


void
HdmiSink::_HdmiModify(uint32 reg, uint32 mask, uint32 value)
{
	// vendor hdmi_modb()
	uint32 old = _HdmiRead(reg);
	_HdmiWrite(reg, (old & ~mask) | (value & mask));
}


status_t
HdmiSink::Init()
{
	if (fI2s == NULL || fHdmi == NULL || fVout == NULL) {
		ERROR("%s: hdmi: cannot map I2S/HDMI/VOUTCRG registers\n", fName);
		return B_NO_MEMORY;
	}
	if (!_HdmiRunning()) {
		INFO("%s: hdmi: output unavailable: the display (VOUTCRG/HDMI TX) "
			"is not running, so no HDMI audio (headphones only)\n", fName);
		return B_DEV_NOT_READY;
	}

	// Linux jh7110_i2s_crg_master_init(): apb clock on, mclk parent =
	// mclk_inner, bclk_mst on, deassert resets. We stay on mclk_inner (the
	// Orange Pi RV mclk_ext oscillator is unconfirmed).
	_SetClock(kClkI2stx0Apb, JH7110_CLK_ENABLE, JH7110_CLK_ENABLE);
	_SetClock(kClkMclk, JH7110_CLK_MUX_MASK, 0);
	uint32 milliHz;
	if (ProgramRate(B_SR_48000, milliHz) != B_OK)
		return B_ERROR;
	_SetClock(kClkI2stx0BclkMst, JH7110_CLK_ENABLE, JH7110_CLK_ENABLE);
	_SetClock(kClkI2stx0Bclk, JH7110_CLK_MUX_MASK, 0);		// bclk_mst
	_SetClock(kClkI2stx0Lrck, JH7110_CLK_MUX_MASK, 0);		// lrck_mst
	_SetClock(kClkHdmitx0Mclk, JH7110_CLK_ENABLE, JH7110_CLK_ENABLE);
	jh7110_io_fence();

	jh7110_sys_reset(fSysCrg, kRstI2stx0Apb, true, fName);
	jh7110_sys_reset(fSysCrg, kRstI2stx0Bclk, true, fName);
	status_t status = jh7110_sys_reset(fSysCrg, kRstI2stx0Apb, false, fName);
	if (status == B_OK)
		status = jh7110_sys_reset(fSysCrg, kRstI2stx0Bclk, false, fName);
	if (status != B_OK)
		return status;

	// HDMI TX audio clocks (VOUTCRG gates; parents vout_top_hdmitx0_mclk
	// and i2stx0_bclk). Gate bits only.
	fVout[kVoutClkHdmiTxMclk / 4] |= JH7110_CLK_ENABLE;
	fVout[kVoutClkHdmiTxBclk / 4] |= JH7110_CLK_ENABLE;
	jh7110_io_fence();

	INFO("%s: hdmi clocks on: i2stx0_apb 0x%08" B_PRIx32 " mclk 0x%08" B_PRIx32
		" mclk_inner 0x%08" B_PRIx32 " bclk_mst 0x%08" B_PRIx32 " lrck_mst 0x%08"
		B_PRIx32 " hdmitx0_mclk 0x%08" B_PRIx32 "; VOUTCRG hdmi_tx_mclk 0x%08"
		B_PRIx32 " hdmi_tx_bclk 0x%08" B_PRIx32 "; resets status 0x%08" B_PRIx32
		"\n", fName, fSysCrg[kClkI2stx0Apb / 4], fSysCrg[kClkMclk / 4],
		fSysCrg[kClkMclkInner / 4], fSysCrg[kClkI2stx0BclkMst / 4],
		fSysCrg[kClkI2stx0LrckMst / 4], fSysCrg[kClkHdmitx0Mclk / 4],
		fVout[kVoutClkHdmiTxMclk / 4], fVout[kVoutClkHdmiTxBclk / 4],
		fSysCrg[JH7110_SYSRST_STATUS(kRstI2stx0Apb) / 4]);

	if (!fCompLogged)
		_LogCompParams();
	uint32 comp1 = _I2sRead(I2S_COMP_PARAM_1);
	if (!I2S_COMP1_TX_ENABLED(comp1)) {
		ERROR("%s: hdmi: I2S TX0 reports no transmitter (COMP_PARAM_1 0x%08"
			B_PRIx32 ")\n", fName, comp1);
		return B_ERROR;
	}
	fFifoDepth = (uint32)1 << (1 + I2S_COMP1_FIFO_DEPTH_GLOBAL(comp1));
	// TXFE is set while the FIFO holds <= TFCR frames. A high threshold
	// (3/4 of the FIFO) leaves 3/4 FIFO of audio when the flag rises and
	// allows depth/4 frames per refill without overflowing.
	fThreshold = fFifoDepth * 3 / 4 - 1;
	if (fFifoDepth < 4)
		fThreshold = 0;

	// dw_i2s_config() + CCR, with everything stopped.
	_I2sWrite(I2S_IER, 0);
	_I2sWrite(I2S_ITER, 0);
	_I2sWrite(I2S_CER, 0);
	_I2sWrite(I2S_TER(0), 0);
	_I2sWrite(I2S_TXFFR, 1);
	_I2sWrite(I2S_TCR(0), I2S_TCR_WLEN_16);
	_I2sWrite(I2S_TFCR(0), fThreshold);
	_I2sWrite(I2S_CCR, fBclkFs == 64 ? I2S_CCR_WSS_32 : I2S_CCR_WSS_16);
	// JH7110 quirk (Linux i2s_start()): TXFE/TXFO unmasked. The I2S TX0
	// interrupt is not wired to a handler; we poll ISR.
	_I2sWrite(I2S_IMR(0), _I2sRead(I2S_IMR(0)) & ~I2S_IMR_TX_MASK);
	jh7110_io_fence();

	// vendor starfive_hdmi_audio_probe(): audio-only bits.
	_HdmiModify(HDMI_AUDIO_CFG, HDMI_CTS_SOURCE_SEL_MASK, HDMI_CTS_EXTER);
	_HdmiModify(HDMI_AUDIO_CFG, HDMI_AUDIO_TYPE_SEL_MASK, HDMI_AUDIO_SEL_I2S);
	_HdmiModify(HDMI_AUDIO_CFG, HDMI_MCLK_RATIO_MASK, HDMI_MCLK_256FS);
	_HdmiModify(HDMI_PINS_ENA, HDMI_I2S_FORMAT_MASK, HDMI_I2S_STANDARD_MODE);
	_HdmiWrite(HDMI_CHANNEL_INPUT, HDMI_CHANNEL_INPUT_DEFAULT);
	_HdmiModify(HDMI_DEV_MUTE, HDMI_AUDIO_MUTE_MASK, HDMI_AUDIO_MUTE);
	jh7110_io_fence();

	fReady = true;
	INFO("%s: hdmi ready: I2S TX0 FIFO depth %" B_PRIu32 " frames, TXFE "
		"threshold %" B_PRIu32 ", bclk %" B_PRIu32 " fs, AUDIO_CFG 0x%02"
		B_PRIx32 " PINS_ENA 0x%02" B_PRIx32 " CHANNEL_INPUT 0x%02" B_PRIx32
		" DEV_MUTE 0x%02" B_PRIx32 " (audio muted until playback)\n", fName,
		fFifoDepth, fThreshold, fBclkFs, _HdmiRead(HDMI_AUDIO_CFG),
		_HdmiRead(HDMI_PINS_ENA), _HdmiRead(HDMI_CHANNEL_INPUT),
		_HdmiRead(HDMI_DEV_MUTE));
	return B_OK;
}


status_t
HdmiSink::ProgramRate(uint32 rateCode, uint32& milliHz)
{
	uint32 hz = jh7110_rate_code_to_hz(rateCode);
	if (hz != 44100 && hz != 48000)
		return B_BAD_VALUE;

	// mclk = 256 fs (HDMI MCLK ratio), bclk = mclk / (256 / bclkFs),
	// lrck = bclk / bclkFs.
	uint64 want = 256ULL * hz;
	uint64 div = (JH7110_AUDIO_ROOT_HZ + want / 2) / want;
	if (div < 1 || div > kMclkInnerDivMax)
		return B_BAD_VALUE;
	milliHz = (uint32)(JH7110_AUDIO_ROOT_HZ * 1000 / (div * 256));
	uint32 bclkDiv = 256 / fBclkFs;

	_SetClock(kClkMclkInner, JH7110_CLK_DIV_MASK, (uint32)div);
	_SetClock(kClkI2stx0BclkMst, JH7110_CLK_DIV_MASK, bclkDiv);
	_SetClock(kClkI2stx0LrckMst, JH7110_CLK_MUX_MASK | JH7110_CLK_DIV_MASK,
		(kLrckMstParentBclkMst << JH7110_CLK_MUX_SHIFT) | fBclkFs);
	jh7110_io_fence();

	if (fMilliHz != milliHz) {
		INFO("%s: hdmi rate %" B_PRIu32 " Hz: mclk_inner = %" B_PRIu64 " / %"
			B_PRIu64 " (256 fs), bclk = mclk / %" B_PRIu32 " (%" B_PRIu32
			" fs), lrck = bclk / %" B_PRIu32 " -> real %" B_PRIu32 ".%03"
			B_PRIu32 " Hz\n", fName, hz, JH7110_AUDIO_ROOT_HZ, div, bclkDiv,
			fBclkFs, fBclkFs, milliHz / 1000, milliHz % 1000);
	}
	fHz = hz;
	fMilliHz = milliHz;
	return B_OK;
}


status_t
HdmiSink::HwStart()
{
	if (!fReady)
		return B_DEV_NOT_READY;

	// vendor starfive_hdmi_audio_hw_params()
	uint32 rateReg = fHz == 44100 ? HDMI_FREQ_44K : HDMI_FREQ_48K;
	uint32 n = fForcedN != 0 ? fForcedN : 128 * fHz / 1000;
	uint32 cts = fTmdsKHz;		// TMDS / 1000
	_HdmiModify(HDMI_AUDIO_CFG, HDMI_CTS_SOURCE_SEL_MASK, HDMI_CTS_EXTER);
	_HdmiWrite(HDMI_SAMPLE_FRE, rateReg);
	_HdmiModify(HDMI_PINS_ENA, HDMI_I2S_PIN_ENA_MASK, HDMI_I2S0_ENA);
	_HdmiWrite(HDMI_N_VALUE1, (n >> 16) & 0xf);
	_HdmiWrite(HDMI_N_VALUE2, (n >> 8) & 0xff);
	_HdmiWrite(HDMI_N_VALUE3, n & 0xff);
	_HdmiWrite(HDMI_CTS_VALUE1, (cts >> 16) & 0xf);
	_HdmiWrite(HDMI_CTS_VALUE2, (cts >> 8) & 0xff);
	_HdmiWrite(HDMI_CTS_VALUE3, cts & 0xff);
	jh7110_io_fence();

	// dw_i2s_config() then i2s_start(): IER, ITER, CER.
	_I2sWrite(I2S_ITER, 0);
	_I2sWrite(I2S_TER(0), 0);
	_I2sWrite(I2S_TXFFR, 1);
	_I2sWrite(I2S_TCR(0), I2S_TCR_WLEN_16);
	_I2sWrite(I2S_TFCR(0), fThreshold);
	_I2sWrite(I2S_TER(0), I2S_TER_TXCHEN);
	_I2sWrite(I2S_CCR, fBclkFs == 64 ? I2S_CCR_WSS_32 : I2S_CCR_WSS_16);
	_I2sWrite(I2S_IER, 1);
	_I2sWrite(I2S_ITER, 1);
	_I2sRead(I2S_TOR(0));		// clear a stale overrun
	_I2sWrite(I2S_CER, 1);
	jh7110_io_fence();

	// vendor trigger START: unmute audio (DEV_MUTE bit 1 only).
	_HdmiModify(HDMI_DEV_MUTE, HDMI_AUDIO_MUTE_MASK, HDMI_AUDIO_NO_MUTE);
	jh7110_io_fence();

	fBudget = fFifoDepth;		// flushed FIFO
	INFO("%s: hdmi audio init: fs %" B_PRIu32 " Hz (SAMPLE_FRE 0x%" B_PRIx32
		"), N %" B_PRIu32 "%s, CTS %" B_PRIu32 " (TMDS %" B_PRIu32 " kHz%s); "
		"regs AUDIO_CFG 0x%02" B_PRIx32 " SAMPLE_FRE 0x%02" B_PRIx32
		" PINS_ENA 0x%02" B_PRIx32 " N %02" B_PRIx32 " %02" B_PRIx32 " %02"
		B_PRIx32 " CTS %02" B_PRIx32 " %02" B_PRIx32 " %02" B_PRIx32
		" DEV_MUTE 0x%02" B_PRIx32 "\n", fName, fHz, rateReg, n,
		fForcedN != 0 ? " (settings)" : "", cts, fTmdsKHz,
		fTmdsKHz == kDefaultTmdsKHz ? ", assumed 1080p60" : ", settings",
		_HdmiRead(HDMI_AUDIO_CFG), _HdmiRead(HDMI_SAMPLE_FRE),
		_HdmiRead(HDMI_PINS_ENA), _HdmiRead(HDMI_N_VALUE1),
		_HdmiRead(HDMI_N_VALUE2), _HdmiRead(HDMI_N_VALUE3),
		_HdmiRead(HDMI_CTS_VALUE1), _HdmiRead(HDMI_CTS_VALUE2),
		_HdmiRead(HDMI_CTS_VALUE3), _HdmiRead(HDMI_DEV_MUTE));
	return B_OK;
}


void
HdmiSink::HwStop()
{
	// vendor trigger STOP (mute), Linux i2s_stop().
	_HdmiModify(HDMI_DEV_MUTE, HDMI_AUDIO_MUTE_MASK, HDMI_AUDIO_MUTE);
	_I2sWrite(I2S_ITER, 0);
	_I2sWrite(I2S_CER, 0);
	_I2sWrite(I2S_IER, 0);
	_I2sWrite(I2S_TER(0), 0);
	jh7110_io_fence();
}


void
HdmiSink::BeginTick(bigtime_t now)
{
	fLastIsr = _I2sRead(I2S_ISR(0));
	if ((fLastIsr & I2S_ISR_TXFO) != 0) {
		fOverruns++;
		_I2sRead(I2S_TOR(0));
	}
}


uint32
HdmiSink::Push(const uint32* frames, uint32 count)
{
	// fBudget = frames that fit for sure. The FIFO only drains between
	// calls, so a left-over budget stays valid.
	uint32 written = 0;
	while (written < count) {
		if (fBudget == 0) {
			uint32 isr = _I2sRead(I2S_ISR(0));
			if ((isr & I2S_ISR_TXFE) == 0)
				break;
			fBudget = fFifoDepth - (fThreshold + 1);
			if (fBudget == 0)
				fBudget = 1;
			fBudgetRefills++;
		}
		uint32 frame = frames[written];
		_I2sWrite(I2S_LTHR(0), frame & 0xffff);
		_I2sWrite(I2S_RTHR(0), frame >> 16);
		written++;
		fBudget--;
	}
	return written;
}


void
HdmiSink::StatsExtra(char* buffer, size_t size) const
{
	snprintf(buffer, size, "hdmi: TX overruns %" B_PRId64 ", TXFE refills %"
		B_PRId64 ", last ISR 0x%02" B_PRIx32 ", FIFO %" B_PRIu32 "/thr %" B_PRIu32,
		fOverruns, fBudgetRefills, fLastIsr, fFifoDepth, fThreshold);
}


AudioSink*
create_hdmi_sink(volatile uint32* sysCrg, const char* name, uint32 bclkFs,
	uint32 tmdsKHz, uint32 forcedN)
{
	return new(std::nothrow) HdmiSink(sysCrg, name, bclkFs, tmdsKHz,
		forcedN);
}
