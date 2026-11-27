/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * JH7110 audio engine: one timer-driven PIO feeder shared by the two
 * outputs (PWM-DAC jack and I2S -> HDMI), the multi_audio buffers, the
 * single owner of audio_root, the on-the-fly output switch and its
 * persistence in the driver settings file. New Beacon code (the
 * hardware-specific parts live in Pwmdac.cpp and I2sHdmi.cpp).
 */


#include "Jh7110Audio.h"

#include <new>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FindDirectory.h>
#include <driver_settings.h>
#include <syscalls.h>


static const uint32 kClkAudioRoot = JH7110_SYSCLK(16);
static const uint32 kAudioRootDivMax = 8;
static_assert(JH7110_AUDIO_ROOT_DIV <= kAudioRootDivMax, "audio_root divider");

static const bigtime_t kMinPeriod = 100;
static const bigtime_t kMaxPeriod = 2000;
static const bigtime_t kStatsInterval = 10000000;

static const char* const kOutputNames[AUDIO_OUTPUT_COUNT] = {
	"Headphones", "HDMI"
};
static const char* const kOutputKeys[AUDIO_OUTPUT_COUNT] = {
	"headphones", "hdmi"
};


// #pragma mark - helpers shared with the sinks


area_id
jh7110_map_page(const char* name, phys_addr_t address, volatile uint32*& regs)
{
	void* virt = NULL;
	area_id area = map_physical_memory(name, address, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virt);
	regs = area >= 0 ? (volatile uint32*)virt : NULL;
	return area;
}


status_t
jh7110_sys_reset(volatile uint32* sysCrg, uint32 index, bool assert,
	const char* who)
{
	uint32 mask = JH7110_RST_BIT(index);
	uint32 value = sysCrg[JH7110_SYSRST_ASSERT(index) / 4];
	if (assert)
		value |= mask;
	else
		value &= ~mask;
	sysCrg[JH7110_SYSRST_ASSERT(index) / 4] = value;
	jh7110_io_fence();

	// 1 ms poll, status bit set = deasserted.
	uint32 done = assert ? 0 : mask;
	for (int i = 0; i < 1000; i++) {
		if ((sysCrg[JH7110_SYSRST_STATUS(index) / 4] & mask) == done)
			return B_OK;
		spin(1);
	}
	ERROR("%s: reset %" B_PRIu32 " %s timed out (status 0x%08" B_PRIx32 ")\n",
		who, index, assert ? "assert" : "deassert",
		sysCrg[JH7110_SYSRST_STATUS(index) / 4]);
	return B_TIMED_OUT;
}


uint32
jh7110_rate_code_to_hz(uint32 code)
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


static uint32
parse_output(const char* value)
{
	if (value == NULL)
		return AUDIO_OUTPUT_COUNT;
	if (strcasecmp(value, "hdmi") == 0)
		return AUDIO_OUTPUT_HDMI;
	if (strcasecmp(value, "headphones") == 0 || strcasecmp(value, "jack") == 0
		|| strcasecmp(value, "headphone") == 0)
		return AUDIO_OUTPUT_HEADPHONES;
	return AUDIO_OUTPUT_COUNT;
}


// #pragma mark - device


Jh7110AudioDevice::Jh7110AudioDevice()
	:
	fSysCrgArea(-1),
	fSysCrg(NULL),
	fActive(NULL),
	fOutput(AUDIO_OUTPUT_HEADPHONES),
	fRateCode(B_SR_48000),
	fActualMilliHz(0),
	fBufferArea(-1),
	fAliasArea(-1),
	fUserBase(NULL),
	fKernelBase(NULL),
	fBufferCount(0),
	fBufferFrames(0),
	fOwner(NULL),
	fLastOwner(NULL),
	fEngineSem(-1),
	fPeriod(500),
	fPeriodOverride(0),
	fRunning(false),
	fBufferSem(-1)
{
	memset(&fConfig, 0, sizeof(fConfig));
	fSinks[0] = fSinks[1] = NULL;
	B_INITIALIZE_SPINLOCK(&fLock);
	memset(&fTimer, 0, sizeof(fTimer));
	fTimer.device = this;
	fSwitches = 0;
	_ResetCounters();
}


Jh7110AudioDevice::~Jh7110AudioDevice()
{
	// No other user is left at this point: no locking.
	_DeleteBuffersLocked();
	for (int i = 0; i < AUDIO_OUTPUT_COUNT; i++)
		delete fSinks[i];
	if (fSysCrgArea >= 0)
		delete_area(fSysCrgArea);
	if (fEngineSem >= 0)
		delete_sem(fEngineSem);
}


void
Jh7110AudioDevice::_ResetCounters()
{
	fCurBuffer = 0;
	fCurFrame = 0;
	fCycle = 0;
	fFramesDone = 0;
	fCycleTime = 0;
	fStartTime = 0;
	fFramesWritten = 0;
	fTicks = 0;
	fLateUnderruns = 0;
	fHookTime = 0;
	fMaxLate = 0;
	fNextTick = 0;
	fLastTick = 0;
	fLastStats = 0;
	fLastStatsFrames = 0;
	fLastStatsHook = 0;
	fLastStatsTicks = 0;
}


status_t
Jh7110AudioDevice::Init(const Jh7110AudioConfig& config)
{
	fConfig = config;
	fEngineSem = create_sem(1, "jh7110 audio engine");
	if (fEngineSem < 0)
		return fEngineSem;

	bool tenBit = false;
	uint32 bclkFs = 32, tmdsKHz = 0, forcedN = 0;
	uint32 wanted = AUDIO_OUTPUT_HEADPHONES;
	void* settings = load_driver_settings(JH7110_AUDIO_SETTINGS);
	if (settings != NULL) {
		tenBit = get_driver_boolean_parameter(settings, "10bit", false, true);
		const char* value = get_driver_parameter(settings, "period_us", NULL,
			NULL);
		if (value != NULL)
			fPeriodOverride = strtoul(value, NULL, 10);
		value = get_driver_parameter(settings, "i2s_bclk_fs", NULL, NULL);
		if (value != NULL)
			bclkFs = strtoul(value, NULL, 10);
		value = get_driver_parameter(settings, "hdmi_tmds_khz", NULL, NULL);
		if (value != NULL)
			tmdsKHz = strtoul(value, NULL, 10);
		value = get_driver_parameter(settings, "hdmi_n", NULL, NULL);
		if (value != NULL)
			forcedN = strtoul(value, NULL, 10);
		value = get_driver_parameter(settings, "output", NULL, NULL);
		uint32 parsed = parse_output(value);
		if (parsed < AUDIO_OUTPUT_COUNT)
			wanted = parsed;
		else if (value != NULL)
			INFO("%s: settings: unknown output \"%s\"\n", Name(), value);
		unload_driver_settings(settings);
	}

	fSysCrgArea = jh7110_map_page("jh7110 audio syscrg", JH7110_SYSCRG_BASE,
		fSysCrg);
	if (fSysCrg == NULL) {
		ERROR("%s: cannot map SYSCRG\n", Name());
		return B_NO_MEMORY;
	}
	fSinks[AUDIO_OUTPUT_HEADPHONES] = create_pwmdac_sink(fSysCrg,
		fConfig.pwmdacBase, Name(), tenBit);
	fSinks[AUDIO_OUTPUT_HDMI] = create_hdmi_sink(fSysCrg, Name(), bclkFs,
		tmdsKHz, forcedN);
	if (fSinks[0] == NULL || fSinks[1] == NULL)
		return B_NO_MEMORY;

	INFO("%s: attaching (PWM-DAC phys 0x%" B_PRIxPHYSADDR "%s), outputs: "
		"headphones (PWM-DAC), HDMI (I2S TX0 -> HDMI TX)\n", Name(),
		fConfig.pwmdacBase,
		fConfig.dtDisabled ? ", DT status \"disabled\", attaching anyway" : "");

	// 1. Read-only dump of everything the firmware left, before any write.
	INFO("%s: firmware left: audio_root 0x%08" B_PRIx32 "\n", Name(),
		fSysCrg[kClkAudioRoot / 4]);
	for (int i = 0; i < AUDIO_OUTPUT_COUNT; i++)
		fSinks[i]->LogFirmwareState();

	// 2. audio_root: this engine is its only owner; both sinks divide it.
	uint32 root = fSysCrg[kClkAudioRoot / 4];
	if ((root & JH7110_CLK_DIV_MASK) != JH7110_AUDIO_ROOT_DIV) {
		INFO("%s: audio_root divider %" B_PRIu32 " -> %d (%" B_PRIu64
			" Hz, assuming PLL2 = %" B_PRIu64 " Hz)\n", Name(),
			root & JH7110_CLK_DIV_MASK, JH7110_AUDIO_ROOT_DIV,
			JH7110_AUDIO_ROOT_HZ, JH7110_PLL2_HZ);
		fSysCrg[kClkAudioRoot / 4] = (root & ~JH7110_CLK_DIV_MASK)
			| JH7110_AUDIO_ROOT_DIV;
		jh7110_io_fence();
	}

	// 3. Bring up both paths; HDMI failing leaves the jack usable.
	status_t jackStatus = B_NOT_SUPPORTED;
	if (fConfig.noJack) {
		// Beacon: never touch GPIO33/34 here (VF2 Lite: Wi-Fi WL_REG_ON).
		INFO("%s: VF2 Lite: no PWM-DAC jack on this board (GPIO33 is the "
			"Wi-Fi enable); headphones output disabled, pinmux untouched\n",
			Name());
	} else
		jackStatus = fSinks[AUDIO_OUTPUT_HEADPHONES]->Init();
	status_t hdmiStatus = fSinks[AUDIO_OUTPUT_HDMI]->Init();
	if (jackStatus != B_OK && hdmiStatus != B_OK) {
		ERROR("%s: neither output works\n", Name());
		return jackStatus;
	}

	if (!OutputAvailable(wanted)) {
		INFO("%s: settings ask for output %s, which is unavailable; using "
			"%s\n", Name(), kOutputKeys[wanted], kOutputKeys[1 - wanted]);
		wanted = 1 - wanted;
	}
	fOutput = wanted;
	fActive = fSinks[wanted];
	uint32 milliHz;
	if (fActive->ProgramRate(fRateCode, milliHz) == B_OK)
		fActualMilliHz = milliHz;

	uint32 rates = SupportedRates();
	INFO("%s: ready: output %s (choices: headphones%s, hdmi%s), rates%s%s, "
		"S16 stereo\n", Name(), kOutputKeys[fOutput],
		OutputAvailable(AUDIO_OUTPUT_HEADPHONES) ? "" : " [unavailable]",
		OutputAvailable(AUDIO_OUTPUT_HDMI) ? "" : " [unavailable]",
		(rates & B_SR_44100) != 0 ? " 44100" : "",
		(rates & B_SR_48000) != 0 ? " 48000" : "");
	return B_OK;
}


bool
Jh7110AudioDevice::OutputAvailable(uint32 output) const
{
	return output < AUDIO_OUTPUT_COUNT && fSinks[output] != NULL
		&& fSinks[output]->Available();
}


const char*
Jh7110AudioDevice::OutputName(uint32 output) const
{
	return output < AUDIO_OUTPUT_COUNT ? kOutputNames[output] : "?";
}


uint32
Jh7110AudioDevice::SupportedRates() const
{
	// Rates both outputs can do, so a switch never needs a new rate.
	uint32 rates = B_SR_44100 | B_SR_48000;
	for (int i = 0; i < AUDIO_OUTPUT_COUNT; i++) {
		if (OutputAvailable(i))
			rates &= fSinks[i]->SupportedRates();
	}
	return rates;
}


bigtime_t
Jh7110AudioDevice::_ComputePeriod(uint32 milliHz) const
{
	// One period for both sinks (the smaller FIFO decides), so a switch
	// does not have to re-arm the timer.
	uint32 hz = milliHz / 1000;
	if (hz == 0)
		hz = 48000;
	bigtime_t period = kMaxPeriod;
	for (int i = 0; i < AUDIO_OUTPUT_COUNT; i++) {
		if (!OutputAvailable(i))
			continue;
		bigtime_t p = (bigtime_t)fSinks[i]->FifoDepth() * 1000000 / (2 * hz);
		if (p < period)
			period = p;
	}
	if (fPeriodOverride != 0)
		period = fPeriodOverride;
	if (period < kMinPeriod)
		period = kMinPeriod;
	if (period > kMaxPeriod)
		period = kMaxPeriod;
	return period;
}


bigtime_t
Jh7110AudioDevice::LatencyUs() const
{
	uint32 hz = fActualMilliHz / 1000;
	if (hz == 0 || fActive == NULL)
		return 0;
	return (bigtime_t)fActive->FifoDepth() * 1000000 / hz + fPeriod;
}


status_t
Jh7110AudioDevice::SetRate(uint32 rateCode)
{
	_Lock();
	status_t status;
	if (fRunning)
		status = rateCode == fRateCode ? B_OK : B_BUSY;
	else if ((SupportedRates() & rateCode) == 0)
		status = B_BAD_VALUE;
	else {
		uint32 milliHz;
		status = fActive->ProgramRate(rateCode, milliHz);
		if (status == B_OK) {
			fRateCode = rateCode;
			fActualMilliHz = milliHz;
		}
	}
	_Unlock();
	return status;
}


// #pragma mark - stream ownership


status_t
Jh7110AudioDevice::ClaimStream(void* cookie)
{
	_Lock();
	status_t status = B_OK;
	if (fOwner == NULL) {
		fOwner = cookie;
		fLastOwner = cookie;
	} else if (fOwner != cookie)
		status = B_BUSY;
	_Unlock();
	if (status != B_OK)
		INFO("%s: stream busy (another program streams; control ioctls "
			"still work)\n", Name());
	return status;
}


void
Jh7110AudioDevice::ReleaseStream(void* cookie)
{
	_Lock();
	if (fOwner == cookie) {
		_StopLocked();
		fOwner = NULL;
	}
	_Unlock();
}


void
Jh7110AudioDevice::FreeStream(void* cookie)
{
	_Lock();
	if (fOwner == NULL && fLastOwner == cookie) {
		_DeleteBuffersLocked();
		fLastOwner = NULL;
	}
	_Unlock();
}


// #pragma mark - buffers


status_t
Jh7110AudioDevice::CreateBuffers(uint32 count, uint32 frames)
{
	_Lock();
	if (fRunning) {
		_Unlock();
		return B_BUSY;
	}
	_DeleteBuffersLocked();

	size_t size = (size_t)count * frames * AUDIO_FRAME_BYTES;
	size = (size + B_PAGE_SIZE - 1) & ~((size_t)B_PAGE_SIZE - 1);

	// Userland (the multi_audio media node) writes into these pages, like
	// with every Haiku audio driver. On riscv64, user-protected pages get
	// the U bit, which the kernel may not touch while sstatus.SUM is clear,
	// so the timer hook reads through a second, kernel-only mapping.
	void* address = NULL;
	status_t status = B_OK;
	fBufferArea = create_area("jh7110 audio buffers", &address,
		B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fBufferArea < 0) {
		ERROR("%s: cannot create %" B_PRIuSIZE " bytes of buffers: %s\n",
			Name(), size, strerror(fBufferArea));
		status = fBufferArea;
		fBufferArea = -1;
		_Unlock();
		return status;
	}
	physical_entry entry;
	status = get_memory_map(address, size, &entry, 1);
	if (status != B_OK || entry.size < size) {
		ERROR("%s: buffer area is not contiguous\n", Name());
		_DeleteBuffersLocked();
		_Unlock();
		return status != B_OK ? status : B_ERROR;
	}
	void* alias = NULL;
	fAliasArea = map_physical_memory("jh7110 audio buffers (kernel)",
		entry.address, size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &alias);
	if (fAliasArea < 0) {
		status = fAliasArea;
		ERROR("%s: cannot map buffer alias: %s\n", Name(), strerror(status));
		_DeleteBuffersLocked();
		_Unlock();
		return status;
	}
	memset(alias, 0, size);

	fBufferSem = create_sem(0, "jh7110 audio buffer ready");
	if (fBufferSem < 0) {
		status = fBufferSem;
		_DeleteBuffersLocked();
		_Unlock();
		return status;
	}

	fUserBase = (uint8*)address;
	fKernelBase = (const uint8*)alias;
	fBufferCount = count;
	fBufferFrames = frames;
	INFO("%s: %" B_PRIu32 " buffers x %" B_PRIu32 " frames at phys 0x%"
		B_PRIxPHYSADDR "\n", Name(), count, frames, entry.address);
	_Unlock();
	return B_OK;
}


void
Jh7110AudioDevice::DeleteBuffers()
{
	_Lock();
	_DeleteBuffersLocked();
	_Unlock();
}


void
Jh7110AudioDevice::_DeleteBuffersLocked()
{
	_StopLocked();
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
Jh7110AudioDevice::UserBuffer(uint32 index) const
{
	if (fUserBase == NULL || index >= fBufferCount)
		return NULL;
	return fUserBase + (size_t)index * fBufferFrames * AUDIO_FRAME_BYTES;
}


// #pragma mark - engine


status_t
Jh7110AudioDevice::Start()
{
	_Lock();
	status_t status = _StartLocked();
	_Unlock();
	return status;
}


void
Jh7110AudioDevice::Stop()
{
	if (fEngineSem < 0)
		return;
	_Lock();
	_StopLocked();
	_Unlock();
}


status_t
Jh7110AudioDevice::_StartLocked()
{
	if (fRunning)
		return B_OK;
	if (fKernelBase == NULL || fActive == NULL)
		return B_NO_INIT;
	uint32 milliHz;
	status_t status = fActive->ProgramRate(fRateCode, milliHz);
	if (status != B_OK)
		return status;
	fActualMilliHz = milliHz;
	fPeriod = _ComputePeriod(milliHz);

	status = fActive->HwStart();
	if (status != B_OK) {
		ERROR("%s: %s: start failed: %s\n", Name(), fActive->Label(),
			strerror(status));
		return status;
	}

	// Drop wake-ups left over from a previous stop.
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
		fActive->HwStop();
		return status;
	}
	uint32 hz = milliHz / 1000;
	INFO("%s: start on %s: %" B_PRIu32 " Hz, %" B_PRIu32 " x %" B_PRIu32
		" frames, FIFO depth %" B_PRIu32 " frames, timer every %" B_PRId64
		" us (%" B_PRId64 " ticks/s), latency ~%" B_PRId64 " us\n", Name(),
		fActive->Label(), hz, fBufferCount, fBufferFrames,
		fActive->FifoDepth(), fPeriod, 1000000 / fPeriod, LatencyUs());
	return B_OK;
}


void
Jh7110AudioDevice::_StopLocked()
{
	if (!fRunning)
		return;
	cancel_timer(&fTimer.t);

	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	fRunning = false;
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	fActive->HwStop();
	MaybeLogStats(true);
	INFO("%s: stop (%s)\n", Name(), fActive->Label());
	// Wake a waiting buffer_exchange so it can return.
	if (fBufferSem >= 0)
		release_sem_etc(fBufferSem, 1, B_DO_NOT_RESCHEDULE);
}


status_t
Jh7110AudioDevice::SetOutput(uint32 output, bool persist)
{
	if (output >= AUDIO_OUTPUT_COUNT)
		return B_BAD_VALUE;
	if (!OutputAvailable(output)) {
		INFO("%s: output switch to %s refused: unavailable\n", Name(),
			kOutputKeys[output]);
		return B_DEV_NOT_READY;
	}

	_Lock();
	uint32 old = fOutput;
	if (old == output) {
		_Unlock();
		return B_OK;
	}
	AudioSink* oldSink = fSinks[old];
	AudioSink* newSink = fSinks[output];
	bool running = fRunning;
	uint32 milliHz = fActualMilliHz;
	status_t status = newSink->ProgramRate(fRateCode, milliHz);
	if (status == B_OK && running)
		status = newSink->HwStart();
	if (status != B_OK) {
		_Unlock();
		ERROR("%s: output switch %s -> %s failed: %s\n", Name(),
			kOutputKeys[old], kOutputKeys[output], strerror(status));
		return status;
	}

	// Same buffers, same position: only the sink the hook feeds changes.
	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	fActive = newSink;
	fOutput = output;
	fActualMilliHz = milliHz;
	fNextTick = 0;
	fLastTick = 0;
	fSwitches++;
	if (running)
		_Fill();
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	if (running)
		oldSink->HwStop();
	_Unlock();

	INFO("%s: output switch: %s -> %s (%s, real rate %" B_PRIu32 ".%03" B_PRIu32
		" Hz, switch #%" B_PRId64 ")\n", Name(), kOutputKeys[old],
		kOutputKeys[output], running ? "while playing" : "idle",
		milliHz / 1000, milliHz % 1000, fSwitches);

	if (persist)
		_SaveOutputSetting(output);
	return B_OK;
}


/*!	Rewrites the "output" line of the driver settings file, keeping all
	other lines. Runs in the caller's (ioctl) thread, never in the hook.
*/
void
Jh7110AudioDevice::_SaveOutputSetting(uint32 output)
{
	char dir[B_PATH_NAME_LENGTH];
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, dir,
			sizeof(dir)) != B_OK) {
		INFO("%s: cannot save output: no settings directory\n", Name());
		return;
	}
	strlcat(dir, "/kernel", sizeof(dir));
	_kern_create_dir(-1, dir, 0755);
	strlcat(dir, "/drivers", sizeof(dir));
	_kern_create_dir(-1, dir, 0755);

	char path[B_PATH_NAME_LENGTH];
	char tempPath[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/%s", dir, JH7110_AUDIO_SETTINGS);
	snprintf(tempPath, sizeof(tempPath), "%s.new", path);

	const size_t kMax = 4096;
	char* oldText = (char*)malloc(kMax + 1);
	char* newText = (char*)malloc(kMax + 64);
	if (oldText == NULL || newText == NULL) {
		free(oldText);
		free(newText);
		return;
	}
	ssize_t length = 0;
	int fd = _kern_open(-1, path, O_RDONLY, 0);
	if (fd >= 0) {
		length = _kern_read(fd, 0, oldText, kMax);
		_kern_close(fd);
		if (length < 0)
			length = 0;
	}
	oldText[length] = '\0';

	// Copy every line except "output ...".
	size_t out = 0;
	const char* line = oldText;
	while (*line != '\0') {
		const char* end = strchr(line, '\n');
		size_t lineLength = end != NULL ? (size_t)(end - line + 1)
			: strlen(line);
		const char* word = line;
		while (*word == ' ' || *word == '\t')
			word++;
		bool isOutput = strncmp(word, "output", 6) == 0
			&& (word[6] == ' ' || word[6] == '\t' || word[6] == '\n'
				|| word[6] == '\0');
		if (!isOutput && out + lineLength < kMax) {
			memcpy(newText + out, line, lineLength);
			out += lineLength;
			if (end == NULL)
				newText[out++] = '\n';
		}
		line += lineLength;
	}
	out += snprintf(newText + out, 64, "output %s\n", kOutputKeys[output]);

	status_t status = B_OK;
	fd = _kern_open(-1, tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		status = fd;
	else {
		ssize_t written = _kern_write(fd, 0, newText, out);
		_kern_close(fd);
		if (written != (ssize_t)out)
			status = written < 0 ? (status_t)written : B_IO_ERROR;
		else
			status = _kern_rename(-1, tempPath, -1, path);
	}
	free(oldText);
	free(newText);
	if (status == B_OK)
		INFO("%s: saved \"output %s\" in %s\n", Name(), kOutputKeys[output],
			path);
	else {
		INFO("%s: cannot save output in %s: %s\n", Name(), path,
			strerror(status));
	}
}


/*static*/ int32
Jh7110AudioDevice::_TimerHook(timer* t)
{
	Jh7110AudioDevice* device = ((audio_engine_timer*)t)->device;
	acquire_spinlock(&device->fLock);
	int32 result = device->fRunning ? device->_Fill() : B_HANDLED_INTERRUPT;
	release_spinlock(&device->fLock);
	return result;
}


/*!	Tops up the active sink's FIFO from the current buffer. Interrupts are
	off and fLock is held.
*/
int32
Jh7110AudioDevice::_Fill()
{
	bigtime_t now = system_time();
	if (fNextTick != 0 && now > fNextTick && now - fNextTick > fMaxLate)
		fMaxLate = now - fNextTick;
	fNextTick = now + fPeriod;

	// A gap longer than the FIFO holds means the FIFO ran dry (the I2S has
	// no TX underrun flag; for the jack this adds to its empty flag).
	uint32 hz = fActualMilliHz / 1000;
	if (fLastTick != 0 && hz != 0) {
		bigtime_t fifoTime = (bigtime_t)fActive->FifoDepth() * 1000000 / hz;
		if (now - fLastTick > fifoTime)
			fLateUnderruns++;
	}
	fLastTick = now;

	fActive->BeginTick(now);

	const uint32 maxFrames = 2 * fActive->FifoDepth();
	uint32 total = 0;
	bool released = false;
	while (total < maxFrames) {
		const uint32* frames = (const uint32*)(fKernelBase
			+ (size_t)fCurBuffer * fBufferFrames * AUDIO_FRAME_BYTES);
		uint32 run = fBufferFrames - fCurFrame;
		if (run > maxFrames - total)
			run = maxFrames - total;
		uint32 written = fActive->Push(frames + fCurFrame, run);
		total += written;
		fCurFrame += written;
		if (fCurFrame == fBufferFrames) {
			fCurFrame = 0;
			fCurBuffer = (fCurBuffer + 1) % fBufferCount;
			fCycle = fCurBuffer;
			fFramesDone += fBufferFrames;
			fCycleTime = now;
			release_sem_etc(fBufferSem, 1, B_DO_NOT_RESCHEDULE);
			released = true;
		}
		if (written < run || written == 0)
			break;
	}

	fFramesWritten += total;
	fTicks++;
	fHookTime += system_time() - now;
	return released ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


status_t
Jh7110AudioDevice::WaitBuffer(bigtime_t& realTime, int64& frames,
	int32& cycle)
{
	if (!fRunning) {
		status_t status = Start();
		if (status != B_OK)
			return status;
	}

	status_t status = acquire_sem_etc(fBufferSem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 1000000);
	if (status != B_OK) {
		if (status == B_TIMED_OUT) {
			ERROR("%s: no buffer completed in 1 s on %s (timer not firing, "
				"or the FIFO does not drain?)\n", Name(),
				fActive != NULL ? fActive->Label() : "?");
		}
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
Jh7110AudioDevice::MaybeLogStats(bool force)
{
	char extra[160];
	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&fLock);
	bigtime_t now = system_time();
	bigtime_t since = now - fLastStats;
	if (!force && since < kStatsInterval) {
		release_spinlock(&fLock);
		restore_interrupts(cpu);
		return;
	}
	int64 frames = fFramesWritten - fLastStatsFrames;
	int64 ticks = fTicks - fLastStatsTicks;
	bigtime_t hook = fHookTime - fLastStatsHook;
	int64 totalTicks = fTicks;
	int64 lateUnderruns = fLateUnderruns;
	int64 flagUnderruns = fActive->FlagUnderruns();
	bigtime_t maxLate = fMaxLate;
	const char* label = fActive->Label();
	fActive->StatsExtra(extra, sizeof(extra));
	fLastStats = now;
	fLastStatsFrames = fFramesWritten;
	fLastStatsTicks = fTicks;
	fLastStatsHook = fHookTime;
	fMaxLate = 0;
	release_spinlock(&fLock);
	restore_interrupts(cpu);

	if (since <= 0)
		return;
	int64 rate = frames * 1000000 / since;
	int64 perMille = hook * 1000 / since;
	INFO("%s: stats (%s): %" B_PRId64 " s window: %" B_PRId64 " frames -> %"
		B_PRId64 " Hz measured, %" B_PRId64 " ticks, hook %" B_PRId64 ".%"
		B_PRId64 "%% of one core (avg %" B_PRId64 " us/tick), max late %"
		B_PRId64 " us; total %" B_PRId64 " ticks, underruns %" B_PRId64
		" (late > FIFO time) / %" B_PRId64 " (empty flag); %s\n", Name(),
		label, since / 1000000, frames, rate, ticks, perMille / 10,
		perMille % 10, ticks > 0 ? hook / ticks : 0, maxLate, totalTicks,
		lateUnderruns, flagUnderruns, extra);
}
