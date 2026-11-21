/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * StarFive JH7110 PWM-DAC (3.5 mm headphone jack) driver for Beacon:
 * device state shared by the hardware engine (Pwmdac.cpp, GPL-2.0-only)
 * and the multi_audio glue (multi.cpp, MIT). This header contains no
 * third-party code.
 */
#ifndef PWMDAC_H
#define PWMDAC_H


#include <KernelExport.h>
#include <OS.h>
#include <device_manager.h>

#include <hmulti_audio.h>


#define PWMDAC_DRIVER_MODULE_NAME	"drivers/audio/hmulti/pwmdac_jh7110/driver_v1"
#define PWMDAC_DEVICE_MODULE_NAME	"drivers/audio/hmulti/pwmdac_jh7110/device_v1"
#define PWMDAC_DEVICE_ID_GENERATOR	"pwmdac_jh7110/device_id"

#define INFO(x...)		dprintf("pwmdac: " x)
#define ERROR(x...)		dprintf("pwmdac: ERROR: " x)

#define PWMDAC_MIN_BUFFERS			2
#define PWMDAC_MAX_BUFFERS			4
#define PWMDAC_DEFAULT_FRAMES		1024
#define PWMDAC_MIN_FRAMES			256
#define PWMDAC_MAX_FRAMES			8192
#define PWMDAC_FRAME_BYTES			4		// S16 stereo


struct PwmdacConfig {
	phys_addr_t		physBase;
	char			name[32];
	bool			dtDisabled;		// node said status = "disabled"
};


struct pwmdac_timer {
	timer			t;				// must stay first
	class PwmdacDevice*	device;
};


class PwmdacDevice {
public:
							PwmdacDevice();
							~PwmdacDevice();

			status_t		Init(const PwmdacConfig& config);
			const char*		Name() const { return fConfig.name; }

			// multi_audio side (called from ioctl context)
			status_t		Open();
			void			Close();
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

private:
	static	int32			_TimerHook(timer* t);
			int32			_Fill();
			void			_ResetCounters();
			status_t		_MapBlocks();
			void			_LogFirmwareState();
			status_t		_EnableClocks();
			status_t		_SetReset(bool assert);
			void			_SetupPinmux();
			status_t		_ProgramRate(uint32 rateCode);
			void			_ProbeFifo();
			uint32			_Read(uint32 offset) const
								{ return fRegs[offset / 4]; }
			void			_Write(uint32 offset, uint32 value)
								{ fRegs[offset / 4] = value; }

			PwmdacConfig	fConfig;
			area_id			fRegsArea;
			volatile uint32* fRegs;
			area_id			fSysCrgArea;
			volatile uint32* fSysCrg;
			area_id			fIomuxArea;
			volatile uint32* fIomux;

			bool			fTenBit;
			uint32			fRateCode;
			uint32			fCountN;
			uint32			fActualMilliHz;

			// FIFO probe results
			uint32			fFifoDepth;
			uint32			fFullMask;		// 0 = unknown, time-paced mode
			uint32			fEmptyMask;		// 0 = unknown

			// buffers
			area_id			fBufferArea;	// U-bit pages, handed to userland
			area_id			fAliasArea;		// kernel-only alias for the hook
			uint8*			fUserBase;
			const uint8*	fKernelBase;
			uint32			fBufferCount;
			uint32			fBufferFrames;

			// engine state (shared with the timer hook, under fLock)
			spinlock		fLock;
			pwmdac_timer	fTimer;
			bigtime_t		fPeriod;
			bigtime_t		fPeriodOverride;
			bool			fRunning;
			int32			fOpen;
			sem_id			fBufferSem;
			uint32			fCurBuffer;
			uint32			fCurFrame;
			int32			fCycle;
			int64			fFramesDone;
			bigtime_t		fCycleTime;
			bigtime_t		fStartTime;
			int64			fWordsWritten;

			// statistics
			int64			fTicks;
			int64			fUnderruns;
			int64			fFullTicks;
			bigtime_t		fHookTime;
			bigtime_t		fMaxLate;
			bigtime_t		fNextTick;
			uint32			fStateSeen;
			bigtime_t		fLastStats;
			int64			fLastStatsWords;
			bigtime_t		fLastStatsHook;
			int64			fLastStatsTicks;
};


extern device_manager_info* gDeviceManager;

status_t pwmdac_multi_control(PwmdacDevice* device, uint32 op, void* arg,
	size_t length);


#endif	// PWMDAC_H
