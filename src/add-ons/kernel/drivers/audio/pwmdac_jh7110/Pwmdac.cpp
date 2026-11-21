/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright 2026, The Beacon Project.
 *
 * StarFive JH7110 PWM-DAC (3.5 mm headphone jack) sink for Beacon's
 * jh7110 audio driver: clocks, reset, pinmux, FIFO probe and the PIO push.
 *
 * Adapted from (GPL-2.0, see THIRD_PARTY.md):
 *  - Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c (StarFive Technology;
 *    Jenny Zhang, Curry Zhang, Xingyu Wu, Hal Feng): CTRL programming
 *    order and defaults, sample-rate table, clock/reset bring-up order.
 *  - StarFive vendor kernel sound/soc/starfive/starfive_pwmdac.c and
 *    starfive_pwmdac_pcm.c (branch JH7110_VisionFive2_devel): the PIO
 *    path (write a stereo frame per WDATA access while the FIFO is not
 *    full) and the frame packing.
 * Facts (register offsets, no code) from Linux clk-starfive-jh7110-sys.c,
 * reset-starfive-jh71x0.c, pinctrl-starfive-jh7110.c, jh7110-pinfunc.h,
 * jh7110-common.dtsi and the JH7110 TRM.
 *
 * The timer engine that calls BeginTick()/Push() is Engine.cpp (MIT).
 */


#include "Jh7110Audio.h"
#include "PwmdacRegs.h"

#include <new>
#include <stdio.h>
#include <string.h>


static const uint32 kClkApb0 = JH7110_SYSCLK(12);		// APB0
static const uint32 kClkAudioRoot = JH7110_SYSCLK(16);	// AUDIO_ROOT
static const uint32 kClkPwmdacApb = JH7110_SYSCLK(157);	// PWMDAC_APB
static const uint32 kClkPwmdacCore = JH7110_SYSCLK(158);	// PWMDAC_CORE
static const uint32 kPwmdacCoreDivMax = 256;
static const uint32 kRstPwmdacApb = 96;					// SYSRST_PWMDAC_APB

// SYS IOMUX (0x13040000): DOEN words at 0x000, DOUT words at 0x040, four
// GPIOs per word, 8 bits each (dout mask 0x7f, doen mask 0x3f).
// jh7110-common.dtsi pwmdac_pins: GPIO33 = GPOUT_SYS_PWMDAC_LEFT (28),
// GPIO34 = GPOUT_SYS_PWMDAC_RIGHT (29), output enabled (GPOEN_ENABLE 0).
static const phys_addr_t kSysIomuxBase = 0x13040000;
static const uint32 kIomuxDoen = 0x000;
static const uint32 kIomuxDout = 0x040;
static const uint32 kGpioLeft = 33;
static const uint32 kGpioRight = 34;
static const uint32 kGpoutPwmdacLeft = 28;
static const uint32 kGpoutPwmdacRight = 29;
static_assert(33 / 4 == 34 / 4, "GPIO33/34 share one mux word");

static const uint32 kProbeMaxWrites = 1024;
static const uint32 kAssumedFifoDepth = 16;		// DMA maxburst in Linux


// Pick N (Linux table) and the pwmdac_core divider for a nominal rate.
// Returns the real rate in milli-Hz, 0 if it is more than 2% off.
static uint32
compute_rate(uint32 hz, bool tenBit, uint32& countN, uint32& coreDiv)
{
	// Linux jh7110_pwmdac_hw_params(): N = 3 for 8 and 16 kHz, 2 for
	// 11.025 kHz, 1 otherwise.
	switch (hz) {
		case 8000:
		case 16000:
			countN = 3;
			break;
		case 11025:
			countN = 2;
			break;
		default:
			countN = 1;
			break;
	}
	uint64 rootHz = JH7110_AUDIO_ROOT_HZ;
	uint64 perSample = (tenBit ? 1024 : 256) * (uint64)countN;
	uint64 want = perSample * hz;
	uint64 div = (rootHz + want / 2) / want;
	if (div < 1)
		div = 1;
	if (div > kPwmdacCoreDivMax)
		div = kPwmdacCoreDivMax;
	coreDiv = (uint32)div;
	uint64 milliHz = rootHz * 1000 / (div * perSample);
	uint64 nominal = (uint64)hz * 1000;
	uint64 error = milliHz > nominal ? milliHz - nominal : nominal - milliHz;
	if (error * 50 > nominal)
		return 0;
	return (uint32)milliHz;
}


class PwmdacSink : public AudioSink {
public:
							PwmdacSink(volatile uint32* sysCrg,
								phys_addr_t base, const char* name,
								bool tenBit);
	virtual					~PwmdacSink();

	virtual	const char*		Label() const { return "headphones"; }
	virtual	bool			Available() const { return fReady; }
	virtual	void			LogFirmwareState();
	virtual	status_t		Init();
	virtual	uint32			SupportedRates() const;
	virtual	status_t		ProgramRate(uint32 rateCode, uint32& milliHz);
	virtual	uint32			FifoDepth() const { return fFifoDepth; }
	virtual	status_t		HwStart();
	virtual	void			HwStop();
	virtual	void			BeginTick(bigtime_t now);
	virtual	uint32			Push(const uint32* frames, uint32 count);
	virtual	void			StatsExtra(char* buffer, size_t size) const;
	virtual	int64			FlagUnderruns() const { return fUnderruns; }

private:
			status_t		_EnableClocks();
			void			_SetupPinmux();
			void			_ProbeFifo();
			uint32			_Read(uint32 offset) const
								{ return fRegs[offset / 4]; }
			void			_Write(uint32 offset, uint32 value)
								{ fRegs[offset / 4] = value; }

			volatile uint32* fSysCrg;
			phys_addr_t		fBase;
			char			fName[40];
			bool			fTenBit;
			bool			fReady;
			area_id			fRegsArea;
			volatile uint32* fRegs;
			area_id			fIomuxArea;
			volatile uint32* fIomux;

			uint32			fCountN;
			uint32			fMilliHz;
			uint32			fFifoDepth;
			uint32			fFullMask;		// 0 = unknown, time-paced mode
			uint32			fEmptyMask;		// 0 = unknown

			// per-tick state (timer hook)
			uint32			fState;
			uint32			fBudget;
			uint32			fTickWritten;
			bool			fFirstTick;
			bigtime_t		fPaceStart;
			int64			fPaceWritten;
			int64			fUnderruns;
			int64			fFullTicks;
			uint32			fStateSeen;
};


PwmdacSink::PwmdacSink(volatile uint32* sysCrg, phys_addr_t base,
		const char* name, bool tenBit)
	:
	fSysCrg(sysCrg),
	fBase(base),
	fTenBit(tenBit),
	fReady(false),
	fRegsArea(-1),
	fRegs(NULL),
	fIomuxArea(-1),
	fIomux(NULL),
	fCountN(1),
	fMilliHz(0),
	fFifoDepth(kAssumedFifoDepth),
	fFullMask(0),
	fEmptyMask(0),
	fState(0),
	fBudget(0),
	fTickWritten(0),
	fFirstTick(true),
	fPaceStart(0),
	fPaceWritten(0),
	fUnderruns(0),
	fFullTicks(0),
	fStateSeen(0)
{
	snprintf(fName, sizeof(fName), "%s", name);
	fRegsArea = jh7110_map_page("pwmdac regs", fBase, fRegs);
	fIomuxArea = jh7110_map_page("pwmdac jh7110 sys iomux", kSysIomuxBase,
		fIomux);
}


PwmdacSink::~PwmdacSink()
{
	if (fRegs != NULL && fReady)
		_Write(PWMDAC_CTRL, 0);
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
	if (fIomuxArea >= 0)
		delete_area(fIomuxArea);
}


void
PwmdacSink::LogFirmwareState()
{
	if (fIomux == NULL)
		return;
	uint32 doutWord = fIomux[(kIomuxDout + 4 * (kGpioLeft / 4)) / 4];
	uint32 doenWord = fIomux[(kIomuxDoen + 4 * (kGpioLeft / 4)) / 4];
	INFO("%s: firmware left (jack): apb0 0x%08" B_PRIx32 ", pwmdac_apb 0x%08"
		B_PRIx32 ", pwmdac_core 0x%08" B_PRIx32 ", reset assert 0x%08" B_PRIx32
		" status 0x%08" B_PRIx32 "\n", fName,
		fSysCrg[kClkApb0 / 4], fSysCrg[kClkPwmdacApb / 4],
		fSysCrg[kClkPwmdacCore / 4],
		fSysCrg[JH7110_SYSRST_ASSERT(kRstPwmdacApb) / 4],
		fSysCrg[JH7110_SYSRST_STATUS(kRstPwmdacApb) / 4]);
	INFO("%s: firmware left (jack): GPIO32-35 dout 0x%08" B_PRIx32 " doen 0x%08"
		B_PRIx32 " (GPIO33 dout %" B_PRIu32 " doen %" B_PRIu32 ", GPIO34 dout %"
		B_PRIu32 " doen %" B_PRIu32 ")\n", fName, doutWord, doenWord,
		(doutWord >> 8) & 0x7f, (doenWord >> 8) & 0x3f,
		(doutWord >> 16) & 0x7f, (doenWord >> 16) & 0x3f);
}


status_t
PwmdacSink::Init()
{
	if (fRegs == NULL || fIomux == NULL) {
		ERROR("%s: cannot map PWM-DAC/IOMUX registers\n", fName);
		return B_NO_MEMORY;
	}
	INFO("%s: jack: PWM-DAC at 0x%" B_PRIxPHYSADDR ", %s PWM\n", fName,
		fBase, fTenBit ? "10-bit" : "8-bit");

	status_t status = _EnableClocks();
	if (status != B_OK)
		return status;
	_SetupPinmux();

	uint32 milliHz;
	if (ProgramRate(B_SR_48000, milliHz) != B_OK
		&& ProgramRate(B_SR_44100, milliHz) != B_OK) {
		ERROR("%s: jack: no usable sample rate\n", fName);
		return B_ERROR;
	}
	_ProbeFifo();
	fReady = true;

	uint32 rates = SupportedRates();
	INFO("%s: jack ready: rates%s%s, S16 stereo, FIFO depth %" B_PRIu32
		" frames (%s)\n", fName, (rates & B_SR_44100) != 0 ? " 44100" : "",
		(rates & B_SR_48000) != 0 ? " 48000" : "", fFifoDepth,
		fFullMask != 0 ? "full flag" : "time-paced");
	return B_OK;
}


status_t
PwmdacSink::_EnableClocks()
{
	// Linux jh7110_pwmdac_crg_enable(): clocks on, then APB reset
	// deasserted. apb0 is CLK_IS_CRITICAL and must already be running.
	// audio_root is set by the engine (its single owner).
	if ((fSysCrg[kClkApb0 / 4] & JH7110_CLK_ENABLE) == 0) {
		INFO("%s: apb0 was gated, enabling\n", fName);
		fSysCrg[kClkApb0 / 4] |= JH7110_CLK_ENABLE;
	}

	fSysCrg[kClkPwmdacApb / 4] |= JH7110_CLK_ENABLE;
	uint32 core = fSysCrg[kClkPwmdacCore / 4];
	if ((core & JH7110_CLK_DIV_MASK) == 0)
		core |= 12;
	fSysCrg[kClkPwmdacCore / 4] = core | JH7110_CLK_ENABLE;
	jh7110_io_fence();

	// A reset pulse gives a defined, empty FIFO.
	jh7110_sys_reset(fSysCrg, kRstPwmdacApb, true, fName);
	status_t status = jh7110_sys_reset(fSysCrg, kRstPwmdacApb, false, fName);
	if (status != B_OK)
		return status;

	INFO("%s: jack clocks on: pwmdac_apb 0x%08" B_PRIx32 " pwmdac_core 0x%08"
		B_PRIx32 ", reset deasserted (status 0x%08" B_PRIx32 ")\n", fName,
		fSysCrg[kClkPwmdacApb / 4], fSysCrg[kClkPwmdacCore / 4],
		fSysCrg[JH7110_SYSRST_STATUS(kRstPwmdacApb) / 4]);
	return B_OK;
}


void
PwmdacSink::_SetupPinmux()
{
	uint32 doutOffset = kIomuxDout + 4 * (kGpioLeft / 4);
	uint32 doenOffset = kIomuxDoen + 4 * (kGpioLeft / 4);
	uint32 leftShift = 8 * (kGpioLeft % 4);
	uint32 rightShift = 8 * (kGpioRight % 4);

	uint32 dout = fIomux[doutOffset / 4];
	uint32 doen = fIomux[doenOffset / 4];
	uint32 newDout = (dout & ~((0x7fu << leftShift) | (0x7fu << rightShift)))
		| (kGpoutPwmdacLeft << leftShift) | (kGpoutPwmdacRight << rightShift);
	uint32 newDoen = doen & ~((0x3fu << leftShift) | (0x3fu << rightShift));
	if (newDout == dout && newDoen == doen) {
		INFO("%s: pinmux GPIO33/34 already PWMDAC left/right\n", fName);
		return;
	}
	fIomux[doutOffset / 4] = newDout;
	fIomux[doenOffset / 4] = newDoen;
	jh7110_io_fence();
	INFO("%s: pinmux GPIO33/34 -> PWMDAC left/right: dout 0x%08" B_PRIx32
		" -> 0x%08" B_PRIx32 ", doen 0x%08" B_PRIx32 " -> 0x%08" B_PRIx32
		" (pad drive/bias left as firmware set them)\n", fName, dout,
		fIomux[doutOffset / 4], doen, fIomux[doenOffset / 4]);
}


uint32
PwmdacSink::SupportedRates() const
{
	uint32 rates = 0;
	uint32 countN, coreDiv;
	if (compute_rate(44100, fTenBit, countN, coreDiv) != 0)
		rates |= B_SR_44100;
	if (compute_rate(48000, fTenBit, countN, coreDiv) != 0)
		rates |= B_SR_48000;
	return rates;
}


status_t
PwmdacSink::ProgramRate(uint32 rateCode, uint32& milliHz)
{
	uint32 hz = jh7110_rate_code_to_hz(rateCode);
	uint32 countN, coreDiv;
	milliHz = hz != 0 ? compute_rate(hz, fTenBit, countN, coreDiv) : 0;
	if (milliHz == 0)
		return B_BAD_VALUE;

	uint32 core = fSysCrg[kClkPwmdacCore / 4];
	fSysCrg[kClkPwmdacCore / 4] = (core & ~JH7110_CLK_DIV_MASK) | coreDiv
		| JH7110_CLK_ENABLE;
	jh7110_io_fence();

	if (fMilliHz != milliHz || fCountN != countN) {
		INFO("%s: jack rate %" B_PRIu32 " Hz: pwmdac_core = %" B_PRIu64 " / %"
			B_PRIu32 ", N = %" B_PRIu32 " -> real %" B_PRIu32 ".%03" B_PRIu32
			" Hz\n", fName, hz, JH7110_AUDIO_ROOT_HZ, coreDiv, countN,
			milliHz / 1000, milliHz % 1000);
	}
	fCountN = countN;
	fMilliHz = milliHz;
	return B_OK;
}


/*!	Find the FIFO depth and which PWMDAC_STATE bit means "full": the DAC is
	enabled at silence and filled as fast as the CPU can write (one frame
	drains every ~21 us at 48 kHz, an APB write takes well under 1 us).
*/
void
PwmdacSink::_ProbeFifo()
{
	_Write(PWMDAC_CTRL, 0);
	jh7110_sys_reset(fSysCrg, kRstPwmdacApb, true, fName);
	jh7110_sys_reset(fSysCrg, kRstPwmdacApb, false, fName);
	uint32 empty = _Read(PWMDAC_STATE);

	uint32 ctrl = pwmdac_ctrl_value(fTenBit, fCountN);
	_Write(PWMDAC_CTRL, ctrl);
	_Write(PWMDAC_CTRL, ctrl | PWMDAC_CTRL_ENABLE);

	cpu_status cpu = disable_interrupts();
	uint32 risen[2] = { 0, 0 };
	uint32 fallen[2] = { 0, 0 };
	uint32 last = empty;
	uint32 written = 0;
	bigtime_t start = system_time();
	while (written < kProbeMaxWrites) {
		_Write(PWMDAC_WDATA, 0);	// signed zero = mid level
		written++;
		uint32 state = _Read(PWMDAC_STATE);
		for (int b = 0; b < 2; b++) {
			uint32 bit = 1u << b;
			if (risen[b] == 0 && (empty & bit) == 0 && (state & bit) != 0)
				risen[b] = written;
			if (fallen[b] == 0 && (empty & bit) != 0 && (state & bit) == 0)
				fallen[b] = written;
		}
		last = state;
		if (risen[0] != 0 || risen[1] != 0)
			break;
	}
	bigtime_t elapsed = system_time() - start;
	restore_interrupts(cpu);

	_Write(PWMDAC_CTRL, ctrl);
	jh7110_sys_reset(fSysCrg, kRstPwmdacApb, true, fName);
	jh7110_sys_reset(fSysCrg, kRstPwmdacApb, false, fName);

	INFO("%s: jack FIFO probe: state empty 0x%" B_PRIx32 ", after %" B_PRIu32
		" writes 0x%" B_PRIx32 " (%" B_PRId64 " us); bit0 rose@%" B_PRIu32
		" fell@%" B_PRIu32 ", bit1 rose@%" B_PRIu32 " fell@%" B_PRIu32 "\n",
		fName, empty, written, last, elapsed, risen[0], fallen[0], risen[1],
		fallen[1]);

	int fullBit = risen[0] != 0 ? 0 : (risen[1] != 0 ? 1 : -1);
	if (fullBit >= 0 && risen[fullBit] >= 2) {
		fFullMask = 1u << fullBit;
		fFifoDepth = risen[fullBit];
		int other = 1 - fullBit;
		if (fallen[other] != 0)
			fEmptyMask = 1u << other;
		INFO("%s: jack FIFO depth %" B_PRIu32 " frames, full = STATE bit %d "
			"(%s), empty = %s\n", fName, fFifoDepth, fullBit,
			fullBit == 0 ? "as in the JH7110 TRM" : "as in the vendor driver",
			fEmptyMask != 0 ? (other == 0 ? "bit 0" : "bit 1") : "unknown");
	} else {
		fFullMask = 0;
		fEmptyMask = 0;
		fFifoDepth = kAssumedFifoDepth;
		INFO("%s: jack: no FIFO-full flag seen in %" B_PRIu32 " writes: "
			"time-paced mode, assuming FIFO depth %" B_PRIu32 " frames\n",
			fName, written, fFifoDepth);
	}
}


status_t
PwmdacSink::HwStart()
{
	// Linux jh7110_pwmdac_set() then trigger START.
	uint32 ctrl = pwmdac_ctrl_value(fTenBit, fCountN);
	_Write(PWMDAC_CTRL, ctrl);
	_Write(PWMDAC_CTRL, ctrl | PWMDAC_CTRL_ENABLE);
	jh7110_io_fence();
	fFirstTick = true;
	fBudget = 0;
	fTickWritten = 0;
	fPaceStart = system_time();
	fPaceWritten = 0;
	return B_OK;
}


void
PwmdacSink::HwStop()
{
	// Linux trigger STOP: clear ENABLE only.
	_Write(PWMDAC_CTRL, _Read(PWMDAC_CTRL) & ~PWMDAC_CTRL_ENABLE);
	jh7110_io_fence();
}


void
PwmdacSink::BeginTick(bigtime_t now)
{
	if (!fFirstTick && fFullMask != 0 && fTickWritten == fFifoDepth)
		fFullTicks++;

	fState = _Read(PWMDAC_STATE);
	fStateSeen |= fState;
	if (fEmptyMask != 0 && (fState & fEmptyMask) != 0 && !fFirstTick)
		fUnderruns++;
	fFirstTick = false;
	fTickWritten = 0;

	if (fFullMask != 0) {
		fBudget = fFifoDepth;
		return;
	}
	// No usable full flag: pace by time at the computed rate.
	int64 due = (int64)(now - fPaceStart) * fMilliHz / 1000000000
		+ fFifoDepth;
	fBudget = due > fPaceWritten
		? (uint32)min_c(due - fPaceWritten, (int64)fFifoDepth) : 0;
}


uint32
PwmdacSink::Push(const uint32* frames, uint32 count)
{
	uint32 written = 0;
	while (written < count && fBudget > 0) {
		if (fFullMask != 0) {
			if (fTickWritten > 0)
				fState = _Read(PWMDAC_STATE);
			if ((fState & fFullMask) != 0) {
				fBudget = 0;
				break;
			}
		}
		// Little-endian S16 frame: left in 15:0, right in 31:16.
		_Write(PWMDAC_WDATA, frames[written]);
		written++;
		fTickWritten++;
		fBudget--;
	}
	fPaceWritten += written;
	return written;
}


void
PwmdacSink::StatsExtra(char* buffer, size_t size) const
{
	snprintf(buffer, size, "jack: empty-flag underruns %" B_PRId64
		", whole-FIFO refills %" B_PRId64 ", state bits seen 0x%" B_PRIx32,
		fUnderruns, fFullTicks, fStateSeen);
}


AudioSink*
create_pwmdac_sink(volatile uint32* sysCrg, phys_addr_t base,
	const char* name, bool tenBit)
{
	return new(std::nothrow) PwmdacSink(sysCrg, base, name, tenBit);
}
