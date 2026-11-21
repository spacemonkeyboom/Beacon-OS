/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright 2026, The Beacon Project.
 *
 * StarFive JH7110 PWM-DAC hardware engine for Beacon: clocks, reset,
 * pinmux, FIFO probe and a timer-driven PIO feeder.
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
 * Linux and the vendor kernel use the dw-axi-dmac DMA engine (channel 22).
 * Beacon has no JH7110 DMA driver yet, so this engine is a periodic kernel
 * timer that tops up the FIFO; see README.md for the numbers.
 */


#include "Pwmdac.h"
#include "PwmdacRegs.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <driver_settings.h>


// JH7110 SYSCRG (0x13020000): clock n is the 32-bit word at n * 4
// (clk-starfive-jh71x0.c), bit 31 = gate enable, bits 23:0 = divider.
static const phys_addr_t kSysCrgBase = 0x13020000;
static const uint32 kClkApb0 = 12 * 4;				// JH7110_SYSCLK_APB0
static const uint32 kClkAudioRoot = 16 * 4;			// JH7110_SYSCLK_AUDIO_ROOT
static const uint32 kClkPwmdacApb = 157 * 4;		// JH7110_SYSCLK_PWMDAC_APB
static const uint32 kClkPwmdacCore = 158 * 4;		// JH7110_SYSCLK_PWMDAC_CORE
static const uint32 kClkEnable = 1u << 31;
static const uint32 kClkDivMask = 0xffffff;
static const uint32 kAudioRootDivMax = 8;			// "audio_root", max 8
static const uint32 kPwmdacCoreDivMax = 256;		// "pwmdac_core", max 256

// SYS resets: assert words at 0x2f8, status words at 0x308; reset 96 =
// JH7110_SYSRST_PWMDAC_APB -> word 3, bit 0. Status bit 1 = deasserted
// (reset-starfive-jh71x0.c with asserted == NULL).
static const uint32 kRstAssert = 0x2f8 + 4 * (96 / 32);
static const uint32 kRstStatus = 0x308 + 4 * (96 / 32);
static const uint32 kRstMask = 1u << (96 % 32);

// PLL2 is set to 1188 MHz by the vendor SPL (board/starfive/visionfive2/
// spl.c); audio_root = PLL2 / 8 = 148.5 MHz.
static const uint64 kPll2Hz = 1188000000ULL;
static const uint32 kAudioRootDiv = 8;
static_assert(kAudioRootDiv <= kAudioRootDivMax, "audio_root divider");
static_assert(33 / 4 == 34 / 4, "GPIO33/34 share one mux word");

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

static const uint32 kProbeMaxWrites = 1024;
static const uint32 kAssumedFifoDepth = 16;		// DMA maxburst in Linux
static const bigtime_t kMinPeriod = 100;
static const bigtime_t kMaxPeriod = 2000;
static const bigtime_t kStatsInterval = 10000000;


static inline void
io_fence()
{
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
}


static area_id
map_page(const char* name, phys_addr_t address, volatile uint32*& regs)
{
	void* virt = NULL;
	area_id area = map_physical_memory(name, address, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virt);
	regs = area >= 0 ? (volatile uint32*)virt : NULL;
	return area;
}


static uint32
rate_code_to_hz(uint32 code)
{
	switch (code) {
		case B_SR_8000:		return 8000;
		case B_SR_11025:	return 11025;
		case B_SR_16000:	return 16000;
		case B_SR_22050:	return 22050;
		case B_SR_32000:	return 32000;
		case B_SR_44100:	return 44100;
		case B_SR_48000:	return 48000;
	}
	return 0;
}


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
	uint64 rootHz = kPll2Hz / kAudioRootDiv;
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


PwmdacDevice::PwmdacDevice()
	:
	fRegsArea(-1),
	fRegs(NULL),
	fSysCrgArea(-1),
	fSysCrg(NULL),
	fIomuxArea(-1),
	fIomux(NULL),
	fTenBit(false),
	fRateCode(B_SR_48000),
	fCountN(1),
	fActualMilliHz(0),
	fFifoDepth(kAssumedFifoDepth),
	fFullMask(0),
	fEmptyMask(0),
	fBufferArea(-1),
	fAliasArea(-1),
	fUserBase(NULL),
	fKernelBase(NULL),
	fBufferCount(0),
	fBufferFrames(0),
	fPeriod(500),
	fPeriodOverride(0),
	fRunning(false),
	fOpen(0),
	fBufferSem(-1)
{
	memset(&fConfig, 0, sizeof(fConfig));
	B_INITIALIZE_SPINLOCK(&fLock);
	memset(&fTimer, 0, sizeof(fTimer));
	fTimer.device = this;
	_ResetCounters();
}


PwmdacDevice::~PwmdacDevice()
{
	Stop();
	DeleteBuffers();
	if (fRegs != NULL)
		_Write(PWMDAC_CTRL, 0);
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
	if (fSysCrgArea >= 0)
		delete_area(fSysCrgArea);
	if (fIomuxArea >= 0)
		delete_area(fIomuxArea);
}


void
PwmdacDevice::_ResetCounters()
{
	fCurBuffer = 0;
	fCurFrame = 0;
	fCycle = 0;
	fFramesDone = 0;
	fCycleTime = 0;
	fStartTime = 0;
	fWordsWritten = 0;
	fTicks = 0;
	fUnderruns = 0;
	fFullTicks = 0;
	fHookTime = 0;
	fMaxLate = 0;
	fNextTick = 0;
	fStateSeen = 0;
	fLastStats = 0;
	fLastStatsWords = 0;
	fLastStatsHook = 0;
	fLastStatsTicks = 0;
}


status_t
PwmdacDevice::Init(const PwmdacConfig& config)
{
	fConfig = config;

	void* settings = load_driver_settings("pwmdac_jh7110");
	if (settings != NULL) {
		fTenBit = get_driver_boolean_parameter(settings, "10bit", false, true);
		const char* period = get_driver_parameter(settings, "period_us", NULL,
			NULL);
		if (period != NULL)
			fPeriodOverride = strtoul(period, NULL, 10);
		unload_driver_settings(settings);
	}

	status_t status = _MapBlocks();
	if (status != B_OK)
		return status;

	INFO("%s: attaching (phys 0x%" B_PRIxPHYSADDR "%s), %s PWM\n",
		Name(), fConfig.physBase,
		fConfig.dtDisabled ? ", DT status \"disabled\", attaching anyway"
			: "", fTenBit ? "10-bit" : "8-bit");

	_LogFirmwareState();

	status = _EnableClocks();
	if (status != B_OK)
		return status;

	_SetupPinmux();

	if (_ProgramRate(B_SR_48000) != B_OK
		&& _ProgramRate(B_SR_44100) != B_OK) {
		ERROR("%s: no usable sample rate\n", Name());
		return B_ERROR;
	}
	_ProbeFifo();

	uint32 rates = SupportedRates();
	INFO("%s: ready: rates%s%s, S16 stereo, FIFO %" B_PRIu32 " frames (%s)\n",
		Name(), (rates & B_SR_44100) != 0 ? " 44100" : "",
		(rates & B_SR_48000) != 0 ? " 48000" : "", fFifoDepth,
		fFullMask != 0 ? "full flag" : "time-paced");
	return B_OK;
}


status_t
PwmdacDevice::_MapBlocks()
{
	fRegsArea = map_page("pwmdac regs", fConfig.physBase, fRegs);
	fSysCrgArea = map_page("pwmdac jh7110 syscrg", kSysCrgBase, fSysCrg);
	fIomuxArea = map_page("pwmdac jh7110 sys iomux", kSysIomuxBase, fIomux);
	if (fRegs == NULL || fSysCrg == NULL || fIomux == NULL) {
		ERROR("%s: cannot map registers\n", Name());
		return B_NO_MEMORY;
	}
	return B_OK;
}


void
PwmdacDevice::_LogFirmwareState()
{
	uint32 doutWord = fIomux[(kIomuxDout + 4 * (kGpioLeft / 4)) / 4];
	uint32 doenWord = fIomux[(kIomuxDoen + 4 * (kGpioLeft / 4)) / 4];
	INFO("%s: firmware left: apb0 0x%08" B_PRIx32 ", audio_root 0x%08"
		B_PRIx32 ", pwmdac_apb 0x%08" B_PRIx32 ", pwmdac_core 0x%08" B_PRIx32
		", reset assert 0x%08" B_PRIx32 " status 0x%08" B_PRIx32 "\n", Name(),
		fSysCrg[kClkApb0 / 4], fSysCrg[kClkAudioRoot / 4],
		fSysCrg[kClkPwmdacApb / 4], fSysCrg[kClkPwmdacCore / 4],
		fSysCrg[kRstAssert / 4], fSysCrg[kRstStatus / 4]);
	INFO("%s: firmware left: GPIO32-35 dout 0x%08" B_PRIx32 " doen 0x%08"
		B_PRIx32 " (GPIO33 dout %" B_PRIu32 " doen %" B_PRIu32 ", GPIO34 dout %"
		B_PRIu32 " doen %" B_PRIu32 ")\n", Name(), doutWord, doenWord,
		(doutWord >> 8) & 0x7f, (doenWord >> 8) & 0x3f,
		(doutWord >> 16) & 0x7f, (doenWord >> 16) & 0x3f);
}


status_t
PwmdacDevice::_SetReset(bool assert)
{
	uint32 value = fSysCrg[kRstAssert / 4];
	if (assert)
		value |= kRstMask;
	else
		value &= ~kRstMask;
	fSysCrg[kRstAssert / 4] = value;
	io_fence();

	// Same 1 ms poll as Linux jh71x0_reset_update().
	uint32 done = assert ? 0 : kRstMask;
	for (int i = 0; i < 1000; i++) {
		if ((fSysCrg[kRstStatus / 4] & kRstMask) == done)
			return B_OK;
		spin(1);
	}
	ERROR("%s: reset %s timed out (status 0x%08" B_PRIx32 ")\n", Name(),
		assert ? "assert" : "deassert", fSysCrg[kRstStatus / 4]);
	return B_TIMED_OUT;
}


status_t
PwmdacDevice::_EnableClocks()
{
	// Linux jh7110_pwmdac_crg_enable(): clocks on, then APB reset
	// deasserted. apb0 is CLK_IS_CRITICAL and must already be running.
	if ((fSysCrg[kClkApb0 / 4] & kClkEnable) == 0) {
		INFO("%s: apb0 was gated, enabling\n", Name());
		fSysCrg[kClkApb0 / 4] |= kClkEnable;
	}

	uint32 root = fSysCrg[kClkAudioRoot / 4];
	if ((root & kClkDivMask) != kAudioRootDiv) {
		INFO("%s: audio_root divider %" B_PRIu32 " -> %" B_PRIu32
			" (%" B_PRIu64 " Hz, assuming PLL2 = %" B_PRIu64 " Hz)\n", Name(),
			root & kClkDivMask, kAudioRootDiv, kPll2Hz / kAudioRootDiv,
			kPll2Hz);
		fSysCrg[kClkAudioRoot / 4] = (root & ~kClkDivMask) | kAudioRootDiv;
	}

	fSysCrg[kClkPwmdacApb / 4] |= kClkEnable;
	uint32 core = fSysCrg[kClkPwmdacCore / 4];
	if ((core & kClkDivMask) == 0)
		core |= 12;
	fSysCrg[kClkPwmdacCore / 4] = core | kClkEnable;
	io_fence();

	// A reset pulse gives a defined, empty FIFO.
	_SetReset(true);
	status_t status = _SetReset(false);
	if (status != B_OK)
		return status;

	INFO("%s: clocks on: pwmdac_apb 0x%08" B_PRIx32 " pwmdac_core 0x%08"
		B_PRIx32 ", reset deasserted (status 0x%08" B_PRIx32 ")\n", Name(),
		fSysCrg[kClkPwmdacApb / 4], fSysCrg[kClkPwmdacCore / 4],
		fSysCrg[kRstStatus / 4]);
	return B_OK;
}


void
PwmdacDevice::_SetupPinmux()
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
		INFO("%s: pinmux GPIO33/34 already PWMDAC left/right\n", Name());
		return;
	}
	fIomux[doutOffset / 4] = newDout;
	fIomux[doenOffset / 4] = newDoen;
	io_fence();
	INFO("%s: pinmux GPIO33/34 -> PWMDAC left/right: dout 0x%08" B_PRIx32
		" -> 0x%08" B_PRIx32 ", doen 0x%08" B_PRIx32 " -> 0x%08" B_PRIx32
		" (pad drive/bias left as firmware set them)\n", Name(), dout,
		fIomux[doutOffset / 4], doen, fIomux[doenOffset / 4]);
}


uint32
PwmdacDevice::SupportedRates() const
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
PwmdacDevice::_ProgramRate(uint32 rateCode)
{
	uint32 hz = rate_code_to_hz(rateCode);
	uint32 countN, coreDiv;
	uint32 milliHz = hz != 0 ? compute_rate(hz, fTenBit, countN, coreDiv) : 0;
	if (milliHz == 0)
		return B_BAD_VALUE;

	uint32 core = fSysCrg[kClkPwmdacCore / 4];
	fSysCrg[kClkPwmdacCore / 4] = (core & ~kClkDivMask) | coreDiv
		| kClkEnable;
	io_fence();

	bool changed = fRateCode != rateCode || fActualMilliHz != milliHz;
	fRateCode = rateCode;
	fCountN = countN;
	fActualMilliHz = milliHz;
	if (changed) {
		INFO("%s: rate %" B_PRIu32 " Hz: pwmdac_core = %" B_PRIu64 " / %"
			B_PRIu32 ", N = %" B_PRIu32 " -> real %" B_PRIu32 ".%03" B_PRIu32
			" Hz\n", Name(), hz, kPll2Hz / kAudioRootDiv, coreDiv, countN,
			milliHz / 1000, milliHz % 1000);
	}
	return B_OK;
}


status_t
PwmdacDevice::SetRate(uint32 rateCode)
{
	if (fRunning)
		return rateCode == fRateCode ? B_OK : B_BUSY;
	return _ProgramRate(rateCode);
}


/*!	Find the FIFO depth and which PWMDAC_STATE bit means "full": the DAC is
	enabled at silence and filled as fast as the CPU can write (one frame
	drains every ~21 us at 48 kHz, an APB write takes well under 1 us).
*/
void
PwmdacDevice::_ProbeFifo()
{
	_Write(PWMDAC_CTRL, 0);
	_SetReset(true);
	_SetReset(false);
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
	_SetReset(true);
	_SetReset(false);

	INFO("%s: FIFO probe: state empty 0x%" B_PRIx32 ", after %" B_PRIu32
		" writes 0x%" B_PRIx32 " (%" B_PRId64 " us); bit0 rose@%" B_PRIu32
		" fell@%" B_PRIu32 ", bit1 rose@%" B_PRIu32 " fell@%" B_PRIu32 "\n",
		Name(), empty, written, last, elapsed, risen[0], fallen[0], risen[1],
		fallen[1]);

	int fullBit = risen[0] != 0 ? 0 : (risen[1] != 0 ? 1 : -1);
	if (fullBit >= 0 && risen[fullBit] >= 2) {
		fFullMask = 1u << fullBit;
		fFifoDepth = risen[fullBit];
		int other = 1 - fullBit;
		if (fallen[other] != 0)
			fEmptyMask = 1u << other;
		INFO("%s: FIFO depth %" B_PRIu32 " frames, full = STATE bit %d (%s),"
			" empty = %s\n", Name(), fFifoDepth, fullBit,
			fullBit == 0 ? "as in the JH7110 TRM" : "as in the vendor driver",
			fEmptyMask != 0 ? (other == 0 ? "bit 0" : "bit 1") : "unknown");
	} else {
		fFullMask = 0;
		fEmptyMask = 0;
		fFifoDepth = kAssumedFifoDepth;
		INFO("%s: no FIFO-full flag seen in %" B_PRIu32 " writes: "
			"time-paced mode, assuming %" B_PRIu32 " frames of FIFO\n", Name(),
			written, fFifoDepth);
	}

	uint32 hz = fActualMilliHz / 1000;
	bigtime_t fifoTime = (bigtime_t)fFifoDepth * 1000000 / hz;
	if (fifoTime < 2 * kMinPeriod) {
		INFO("%s: WARNING: FIFO holds only %" B_PRId64 " us of audio; timer "
			"PIO will underrun. A DMA engine (dw-axi-dmac) is needed\n",
			Name(), fifoTime);
	}
}


bigtime_t
PwmdacDevice::LatencyUs() const
{
	uint32 hz = fActualMilliHz / 1000;
	if (hz == 0)
		return 0;
	return (bigtime_t)fFifoDepth * 1000000 / hz + fPeriod;
}


// #pragma mark - buffers


status_t
PwmdacDevice::CreateBuffers(uint32 count, uint32 frames)
{
	if (fRunning)
		return B_BUSY;
	DeleteBuffers();

	size_t size = (size_t)count * frames * PWMDAC_FRAME_BYTES;
	size = (size + B_PAGE_SIZE - 1) & ~((size_t)B_PAGE_SIZE - 1);

	// Userland (the multi_audio media node) writes into these pages, like
	// with every Haiku audio driver. On riscv64, user-protected pages get
	// the U bit, which the kernel may not touch while sstatus.SUM is clear,
	// so the timer hook reads through a second, kernel-only mapping.
	void* address = NULL;
	fBufferArea = create_area("pwmdac buffers", &address,
		B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fBufferArea < 0) {
		ERROR("%s: cannot create %" B_PRIuSIZE " bytes of buffers: %s\n",
			Name(), size, strerror(fBufferArea));
		return fBufferArea;
	}
	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status != B_OK || entry.size < size) {
		ERROR("%s: buffer area is not contiguous\n", Name());
		DeleteBuffers();
		return status != B_OK ? status : B_ERROR;
	}
	void* alias = NULL;
	fAliasArea = map_physical_memory("pwmdac buffers (kernel)", entry.address,
		size, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&alias);
	if (fAliasArea < 0) {
		ERROR("%s: cannot map buffer alias: %s\n", Name(),
			strerror(fAliasArea));
		DeleteBuffers();
		return fAliasArea;
	}
	memset(alias, 0, size);

	fBufferSem = create_sem(0, "pwmdac buffer ready");
	if (fBufferSem < 0) {
		status = fBufferSem;
		DeleteBuffers();
		return status;
	}

	fUserBase = (uint8*)address;
	fKernelBase = (const uint8*)alias;
	fBufferCount = count;
	fBufferFrames = frames;
	INFO("%s: %" B_PRIu32 " buffers x %" B_PRIu32 " frames at phys 0x%"
		B_PRIxPHYSADDR "\n", Name(), count, frames, entry.address);
	return B_OK;
}


void
PwmdacDevice::DeleteBuffers()
{
	if (fRunning)
		Stop();
	if (fAliasArea >= 0)
		delete_area(fAliasArea);
	if (fBufferArea >= 0)
		delete_area(fBufferArea);
	if (fBufferSem >= 0)
		delete_sem(fBufferSem);
	fAliasArea = fBufferArea = -1;
	fBufferSem = -1;
	fUserBase = NULL;
	fKernelBase = NULL;
	fBufferCount = fBufferFrames = 0;
}


void*
PwmdacDevice::UserBuffer(uint32 index) const
{
	if (fUserBase == NULL || index >= fBufferCount)
		return NULL;
	return fUserBase + (size_t)index * fBufferFrames * PWMDAC_FRAME_BYTES;
}


// #pragma mark - engine


status_t
PwmdacDevice::Open()
{
	if (atomic_test_and_set(&fOpen, 1, 0) != 0)
		return B_BUSY;
	return B_OK;
}


void
PwmdacDevice::Close()
{
	Stop();
	atomic_set(&fOpen, 0);
}


status_t
PwmdacDevice::Start()
{
	if (fRunning)
		return B_OK;
	if (fKernelBase == NULL)
		return B_NO_INIT;
	status_t status = _ProgramRate(fRateCode);
	if (status != B_OK)
		return status;

	uint32 hz = fActualMilliHz / 1000;
	fPeriod = (bigtime_t)fFifoDepth * 1000000 / (2 * hz);
	if (fPeriodOverride != 0)
		fPeriod = fPeriodOverride;
	if (fPeriod < kMinPeriod)
		fPeriod = kMinPeriod;
	if (fPeriod > kMaxPeriod)
		fPeriod = kMaxPeriod;

	// Linux jh7110_pwmdac_set() then trigger START.
	uint32 ctrl = pwmdac_ctrl_value(fTenBit, fCountN);
	_Write(PWMDAC_CTRL, ctrl);
	_Write(PWMDAC_CTRL, ctrl | PWMDAC_CTRL_ENABLE);
	io_fence();

	// Drop wake-ups left over from a previous Stop().
	while (acquire_sem_etc(fBufferSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;

	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	_ResetCounters();
	fStartTime = system_time();
	fCycleTime = fStartTime;
	fLastStats = fStartTime;
	fRunning = true;
	_Fill();
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	status = add_timer(&fTimer.t, &_TimerHook, fPeriod, B_PERIODIC_TIMER);
	if (status != B_OK) {
		ERROR("%s: add_timer failed: %s\n", Name(), strerror(status));
		fRunning = false;
		_Write(PWMDAC_CTRL, ctrl);
		return status;
	}
	INFO("%s: start: %" B_PRIu32 " Hz, %" B_PRIu32 " x %" B_PRIu32
		" frames, timer every %" B_PRId64 " us (%" B_PRId64 " ticks/s), "
		"latency ~%" B_PRId64 " us\n", Name(), hz, fBufferCount, fBufferFrames,
		fPeriod, 1000000 / fPeriod, LatencyUs());
	return B_OK;
}


void
PwmdacDevice::Stop()
{
	if (!fRunning)
		return;
	cancel_timer(&fTimer.t);

	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	fRunning = false;
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	// Linux trigger STOP: clear ENABLE only.
	_Write(PWMDAC_CTRL, _Read(PWMDAC_CTRL) & ~PWMDAC_CTRL_ENABLE);
	MaybeLogStats(true);
	INFO("%s: stop\n", Name());
	// Wake a waiting buffer_exchange so it can return.
	if (fBufferSem >= 0)
		release_sem_etc(fBufferSem, 1, B_DO_NOT_RESCHEDULE);
}


/*static*/ int32
PwmdacDevice::_TimerHook(timer* t)
{
	PwmdacDevice* device = ((pwmdac_timer*)t)->device;
	acquire_spinlock(&device->fLock);
	int32 result = device->fRunning ? device->_Fill() : B_HANDLED_INTERRUPT;
	release_spinlock(&device->fLock);
	return result;
}


/*!	Tops up the FIFO from the current buffer. Interrupts are off and fLock
	is held.
*/
int32
PwmdacDevice::_Fill()
{
	bigtime_t now = system_time();
	if (fNextTick != 0 && now > fNextTick && now - fNextTick > fMaxLate)
		fMaxLate = now - fNextTick;
	fNextTick = now + fPeriod;

	uint32 state = _Read(PWMDAC_STATE);
	fStateSeen |= state;
	if (fEmptyMask != 0 && (state & fEmptyMask) != 0 && fTicks > 0)
		fUnderruns++;

	uint32 limit = fFifoDepth;
	if (fFullMask == 0) {
		// No usable full flag: pace by time at the computed rate.
		int64 due = (int64)(now - fStartTime) * fActualMilliHz / 1000000000
			+ fFifoDepth;
		limit = due > fWordsWritten
			? (uint32)min_c(due - fWordsWritten, (int64)fFifoDepth) : 0;
	}

	const uint32* frames = (const uint32*)(fKernelBase
		+ (size_t)fCurBuffer * fBufferFrames * PWMDAC_FRAME_BYTES);
	bool released = false;
	uint32 written = 0;
	while (written < limit) {
		if (fFullMask != 0) {
			if (written > 0)
				state = _Read(PWMDAC_STATE);
			if ((state & fFullMask) != 0)
				break;
		}
		// Little-endian S16 frame: left in 15:0, right in 31:16.
		_Write(PWMDAC_WDATA, frames[fCurFrame]);
		written++;
		if (++fCurFrame == fBufferFrames) {
			fCurFrame = 0;
			fCurBuffer = (fCurBuffer + 1) % fBufferCount;
			frames = (const uint32*)(fKernelBase
				+ (size_t)fCurBuffer * fBufferFrames * PWMDAC_FRAME_BYTES);
			fCycle = fCurBuffer;
			fFramesDone += fBufferFrames;
			fCycleTime = now;
			release_sem_etc(fBufferSem, 1, B_DO_NOT_RESCHEDULE);
			released = true;
		}
	}
	if (fFullMask != 0 && written == fFifoDepth && fTicks > 0)
		fFullTicks++;

	fWordsWritten += written;
	fTicks++;
	fHookTime += system_time() - now;
	return released ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


status_t
PwmdacDevice::WaitBuffer(bigtime_t& realTime, int64& frames, int32& cycle)
{
	if (!fRunning) {
		status_t status = Start();
		if (status != B_OK)
			return status;
	}

	status_t status = acquire_sem_etc(fBufferSem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 1000000);
	if (status != B_OK) {
		if (status == B_TIMED_OUT)
			ERROR("%s: no buffer completed in 1 s (timer not firing?)\n",
				Name());
		return status;
	}
	if (!fRunning)
		return B_INTERRUPTED;

	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	realTime = fCycleTime;
	frames = fFramesDone;
	cycle = fCycle;
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	MaybeLogStats(false);
	return B_OK;
}


void
PwmdacDevice::MaybeLogStats(bool force)
{
	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	bigtime_t now = system_time();
	bigtime_t since = now - fLastStats;
	if (!force && since < kStatsInterval) {
		release_spinlock(&fLock);
		restore_interrupts(cpu);
		return;
	}
	int64 words = fWordsWritten - fLastStatsWords;
	int64 ticks = fTicks - fLastStatsTicks;
	bigtime_t hook = fHookTime - fLastStatsHook;
	int64 totalTicks = fTicks;
	int64 underruns = fUnderruns;
	int64 fullTicks = fFullTicks;
	bigtime_t maxLate = fMaxLate;
	uint32 seen = fStateSeen;
	fLastStats = now;
	fLastStatsWords = fWordsWritten;
	fLastStatsTicks = fTicks;
	fLastStatsHook = fHookTime;
	fMaxLate = 0;
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	if (since <= 0)
		return;
	int64 rate = words * 1000000 / since;
	int64 perMille = hook * 1000 / since;
	INFO("%s: stats: %" B_PRId64 " s window: %" B_PRId64 " frames -> %"
		B_PRId64 " Hz measured, %" B_PRId64 " ticks, hook %" B_PRId64 ".%"
		B_PRId64 "%% of one core (avg %" B_PRId64 " us/tick), max late %"
		B_PRId64 " us; total %" B_PRId64 " ticks, underruns %" B_PRId64
		" (empty flag) / %" B_PRId64 " (whole-FIFO refills), state bits seen 0x%"
		B_PRIx32 "\n", Name(), since / 1000000, words, rate, ticks,
		perMille / 10, perMille % 10, ticks > 0 ? hook / ticks : 0, maxLate,
		totalTicks, underruns, fullTicks, seen);
}
