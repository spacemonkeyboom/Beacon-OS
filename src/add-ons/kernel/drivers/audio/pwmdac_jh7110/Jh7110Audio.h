/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * StarFive JH7110 audio driver for Beacon: one multi_audio device with two
 * outputs (3.5 mm jack via the PWM-DAC, HDMI via I2S TX0 -> Innosilicon
 * HDMI TX) and an on-the-fly output selector. This header holds the
 * shared engine (timer-driven PIO feeder, buffers, output switch) and the
 * sink interface. It contains no third-party code; register offsets in the
 * helpers below are hardware facts (JH7110 TRM, Linux reset/clk drivers).
 */
#ifndef JH7110_AUDIO_H
#define JH7110_AUDIO_H


#include <KernelExport.h>
#include <OS.h>
#include <device_manager.h>

#include <hmulti_audio.h>


#define JH7110_AUDIO_DRIVER_MODULE_NAME	"drivers/audio/hmulti/pwmdac_jh7110/driver_v1"
#define JH7110_AUDIO_DEVICE_MODULE_NAME	"drivers/audio/hmulti/pwmdac_jh7110/device_v1"
#define JH7110_AUDIO_DEVICE_ID_GENERATOR	"pwmdac_jh7110/device_id"
// Driver settings file: ~/config/settings/kernel/drivers/pwmdac_jh7110
#define JH7110_AUDIO_SETTINGS		"pwmdac_jh7110"

#define INFO(x...)		dprintf("jh7110_audio: " x)
#define ERROR(x...)		dprintf("jh7110_audio: ERROR: " x)

#define AUDIO_MIN_BUFFERS			2
#define AUDIO_MAX_BUFFERS			4
#define AUDIO_DEFAULT_FRAMES		1024
#define AUDIO_MIN_FRAMES			256
#define AUDIO_MAX_FRAMES			8192
#define AUDIO_FRAME_BYTES			4		// S16 stereo

// audio_root = PLL2 / 8 = 148.5 MHz (PLL2 = 1188 MHz, set by the vendor
// SPL). The engine owns audio_root; both sinks divide it further.
#define JH7110_PLL2_HZ				((uint64)1188000000)
#define JH7110_AUDIO_ROOT_DIV		8
#define JH7110_AUDIO_ROOT_HZ		(JH7110_PLL2_HZ / JH7110_AUDIO_ROOT_DIV)

// SYSCRG (0x13020000): clock n = word n * 4, bit 31 gate, 27:24 mux,
// 23:0 divider. SYS resets: assert words 0x2f8.., status words 0x308..
// (status bit 1 = deasserted).
#define JH7110_SYSCRG_BASE			0x13020000
#define JH7110_CLK_ENABLE			(1u << 31)
#define JH7110_CLK_MUX_SHIFT		24
#define JH7110_CLK_MUX_MASK			(0xfu << 24)
#define JH7110_CLK_DIV_MASK			0xffffffu
#define JH7110_SYSCLK(n)			((n) * 4)
#define JH7110_SYSRST_ASSERT(n)		(0x2f8 + 4 * ((n) / 32))
#define JH7110_SYSRST_STATUS(n)		(0x308 + 4 * ((n) / 32))
#define JH7110_RST_BIT(n)			(1u << ((n) % 32))


enum {
	AUDIO_OUTPUT_HEADPHONES = 0,
	AUDIO_OUTPUT_HDMI = 1,
	AUDIO_OUTPUT_COUNT = 2
};


static inline void
jh7110_io_fence()
{
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
}


area_id jh7110_map_page(const char* name, phys_addr_t address,
	volatile uint32*& regs);
status_t jh7110_sys_reset(volatile uint32* sysCrg, uint32 index, bool assert,
	const char* who);
uint32 jh7110_rate_code_to_hz(uint32 code);


/*!	One output path. All Push()/BeginTick() calls happen in the timer hook
	with interrupts off and the engine spinlock held; everything else runs
	in thread context under the engine lock.
*/
class AudioSink {
public:
	virtual					~AudioSink() {}

	virtual	const char*		Label() const { return "?"; }
	virtual	bool			Available() const { return false; }
	// Read-only dump of what the firmware left (no writes).
	virtual	void			LogFirmwareState() {}
	virtual	status_t		Init() { return B_ERROR; }
	virtual	uint32			SupportedRates() const { return 0; }
	// Program the sink's clocks for a rate; only while the sink is stopped.
	virtual	status_t		ProgramRate(uint32 rateCode, uint32& milliHz)
								{ return B_ERROR; }
	virtual	uint32			FifoDepth() const { return 16; }
	// Start/stop the hardware path (FIFO empty, output unmuted / muted).
	virtual	status_t		HwStart() { return B_ERROR; }
	virtual	void			HwStop() {}
	// Timer hook: once per tick, then Push() until it writes less than
	// asked. Returns frames written.
	virtual	void			BeginTick(bigtime_t now) {}
	virtual	uint32			Push(const uint32* frames, uint32 count)
								{ return 0; }
	// Sink-specific part of the stats line (called with the lock held).
	virtual	void			StatsExtra(char* buffer, size_t size) const
								{ buffer[0] = '\0'; }
	virtual	int64			FlagUnderruns() const { return 0; }
};


struct audio_engine_timer {
	timer			t;				// must stay first
	class Jh7110AudioDevice* device;
};


struct Jh7110AudioConfig {
	phys_addr_t		pwmdacBase;
	char			name[32];
	bool			dtDisabled;		// pwmdac node said status = "disabled"
	bool			noJack;			// board has no PWM-DAC jack (VF2 Lite:
									// GPIO33 is WL_REG_ON there)
};


class Jh7110AudioDevice {
public:
							Jh7110AudioDevice();
							~Jh7110AudioDevice();

			status_t		Init(const Jh7110AudioConfig& config);
			const char*		Name() const { return fConfig.name; }

			// open / stream ownership (one streaming opener, any number
			// of control-only openers, e.g. audio_output)
			status_t		ClaimStream(void* cookie);
			void			ReleaseStream(void* cookie);
			void			FreeStream(void* cookie);

			status_t		SetRate(uint32 rateCode);
			uint32			RateCode() const { return fRateCode; }
			uint32			SupportedRates() const;
			uint32			ActualRateMilliHz() const { return fActualMilliHz; }
			bigtime_t		LatencyUs() const;

			status_t		CreateBuffers(uint32 count, uint32 frames);
			void			DeleteBuffers();
			uint32			BufferCount() const { return fBufferCount; }
			uint32			BufferFrames() const { return fBufferFrames; }
			void*			UserBuffer(uint32 index) const;

			status_t		Start();
			void			Stop();
			bool			IsRunning() const { return fRunning; }
			status_t		WaitBuffer(bigtime_t& realTime, int64& frames,
								int32& cycle);
			void			MaybeLogStats(bool force);

			// output selector
			uint32			Output() const { return fOutput; }
			bool			OutputAvailable(uint32 output) const;
			const char*		OutputName(uint32 output) const;
			status_t		SetOutput(uint32 output, bool persist);

private:
	static	int32			_TimerHook(timer* t);
			int32			_Fill();
			void			_ResetCounters();
			void			_Lock() { acquire_sem(fEngineSem); }
			void			_Unlock() { release_sem(fEngineSem); }
			status_t		_StartLocked();
			void			_StopLocked();
			void			_DeleteBuffersLocked();
			bigtime_t		_ComputePeriod(uint32 milliHz) const;
			void			_SaveOutputSetting(uint32 output);

			Jh7110AudioConfig fConfig;
			area_id			fSysCrgArea;
			volatile uint32* fSysCrg;
			AudioSink*		fSinks[AUDIO_OUTPUT_COUNT];
			AudioSink*		fActive;		// read by the hook under fLock
			uint32			fOutput;

			uint32			fRateCode;
			uint32			fActualMilliHz;

			// buffers
			area_id			fBufferArea;	// U-bit pages, handed to userland
			area_id			fAliasArea;		// kernel-only alias for the hook
			uint8*			fUserBase;
			const uint8*	fKernelBase;
			uint32			fBufferCount;
			uint32			fBufferFrames;

			// ownership
			void*			fOwner;
			void*			fLastOwner;

			// engine state (shared with the timer hook, under fLock)
			sem_id			fEngineSem;		// serialises thread-side calls
			spinlock		fLock;
			audio_engine_timer fTimer;
			bigtime_t		fPeriod;
			bigtime_t		fPeriodOverride;
			bool			fRunning;
			sem_id			fBufferSem;
			uint32			fCurBuffer;
			uint32			fCurFrame;
			int32			fCycle;
			int64			fFramesDone;
			bigtime_t		fCycleTime;
			bigtime_t		fStartTime;
			int64			fFramesWritten;

			// statistics
			int64			fTicks;
			int64			fLateUnderruns;
			int64			fSwitches;
			bigtime_t		fHookTime;
			bigtime_t		fMaxLate;
			bigtime_t		fNextTick;
			bigtime_t		fLastTick;
			bigtime_t		fLastStats;
			int64			fLastStatsFrames;
			bigtime_t		fLastStatsHook;
			int64			fLastStatsTicks;
};


extern device_manager_info* gDeviceManager;

status_t jh7110_audio_multi_control(Jh7110AudioDevice* device, void* cookie,
	uint32 op, void* arg, size_t length);

AudioSink* create_pwmdac_sink(volatile uint32* sysCrg, phys_addr_t base,
	const char* name, bool tenBit);
AudioSink* create_hdmi_sink(volatile uint32* sysCrg, const char* name,
	uint32 bclkFs, uint32 tmdsKHz, uint32 forcedN);


#endif	// JH7110_AUDIO_H
