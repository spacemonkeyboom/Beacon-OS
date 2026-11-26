/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Synopsys DesignWare Mobile Storage Host Controller ("dw_mmc", MSHC) bus
 * driver for Beacon (a Haiku-derived operating system), first target the
 * StarFive JH7110 SoC (Orange Pi RV, VisionFive 2).
 *
 * Register map, bit definitions and controller sequences are taken from:
 *   Linux drivers/mmc/host/dw_mmc.h, dw_mmc.c (GPL-2.0-or-later)
 *     Synopsys DesignWare Multimedia Card Interface driver
 *     (Based on NXP driver for lpc 31xx)
 *     Copyright (C) 2009 NXP Semiconductors
 *     Copyright (C) 2009, 2010 Imagination Technologies Ltd.
 *   U-Boot include/dwmmc.h, drivers/mmc/dw_mmc.c (GPL-2.0+)
 *     (C) Copyright 2012 SAMSUNG Electronics
 *     Jaehoon Chung <jh80.chung@samsung.com>
 *     Rajeshawari Shinde <rajeshwari.s@samsung.com>
 *
 * Modified for Beacon/Haiku, 2026 (The Beacon Project).
 * See THIRD_PARTY.md in this directory for exact upstream versions.
 */
#ifndef _DW_MMC_H_
#define _DW_MMC_H_


#include <KernelExport.h>
#include <device_manager.h>

#include <condition_variable.h>
#include <lock.h>

#include "mmc.h"


class Jh7110SysGpio;


#define DW_MMC_DRIVER_MODULE_NAME "busses/mmc/dw_mmc/driver_v1"


struct IOOperation;
struct generic_io_vec;

extern device_manager_info* gDeviceManager;
extern struct mmc_bus_interface gDwMmcBusModule;


// #pragma mark - registers (Linux dw_mmc.h SDMMC_*, U-Boot dwmmc.h DWMCI_*)

enum {
	kDwCtrl			= 0x000,
	kDwPwren		= 0x004,
	kDwClkdiv		= 0x008,
	kDwClksrc		= 0x00c,
	kDwClkena		= 0x010,
	kDwTmout		= 0x014,
	kDwCtype		= 0x018,
	kDwBlksiz		= 0x01c,
	kDwBytcnt		= 0x020,
	kDwIntmask		= 0x024,
	kDwCmdarg		= 0x028,
	kDwCmd			= 0x02c,
	kDwResp0		= 0x030,
	kDwResp1		= 0x034,
	kDwResp2		= 0x038,
	kDwResp3		= 0x03c,
	kDwMintsts		= 0x040,
	kDwRintsts		= 0x044,
	kDwStatus		= 0x048,
	kDwFifoth		= 0x04c,
	kDwCdetect		= 0x050,
	kDwWrtprt		= 0x054,
	kDwTcbcnt		= 0x05c,
	kDwTbbcnt		= 0x060,
	kDwDebnce		= 0x064,
	kDwUsrid		= 0x068,
	kDwVerid		= 0x06c,
	kDwHcon			= 0x070,
	kDwUhsReg		= 0x074,
	kDwRstN			= 0x078,
	kDwBmod			= 0x080,
	kDwPldmnd		= 0x084,
	kDwDbaddr		= 0x088,	// 32-bit IDMAC; DBADDRL with 64-bit IDMAC
	kDwIdsts		= 0x08c,	// 32-bit IDMAC; DBADDRU with 64-bit IDMAC
	kDwIdinten		= 0x090,	// 32-bit IDMAC; IDSTS64 with 64-bit IDMAC
	kDwCdthrctl		= 0x100,
	kDwUhsRegExt	= 0x108,

	// FIFO data register: 0x100 before version 2.40a, 0x200 from 2.40a on.
	kDwDataOffsetOld	= 0x100,
	kDwDataOffset240a	= 0x200,
	kDwVersion240a		= 0x240a,
};

// CTRL
enum {
	kDwCtrlReset		= 1 << 0,
	kDwCtrlFifoReset	= 1 << 1,
	kDwCtrlDmaReset		= 1 << 2,
	kDwCtrlIntEnable	= 1 << 4,
	kDwCtrlDmaEnable	= 1 << 5,
	kDwCtrlUseIdmac		= 1 << 25,
	kDwCtrlAllResetFlags = kDwCtrlReset | kDwCtrlFifoReset | kDwCtrlDmaReset,
};

// CLKENA
enum {
	kDwClkenEnable		= 1 << 0,
	kDwClkenLowPower	= 1 << 16,
};

// CTYPE
enum {
	kDwCtype1Bit		= 0,
	kDwCtype4Bit		= 1 << 0,
	kDwCtype8Bit		= 1 << 16,
};

// RINTSTS / MINTSTS / INTMASK
enum {
	kDwIntCd			= 1 << 0,
	kDwIntRespErr		= 1 << 1,
	kDwIntCmdDone		= 1 << 2,
	kDwIntDataOver		= 1 << 3,
	kDwIntTxdr			= 1 << 4,
	kDwIntRxdr			= 1 << 5,
	kDwIntRcrc			= 1 << 6,
	kDwIntDcrc			= 1 << 7,
	kDwIntRto			= 1 << 8,
	kDwIntDrto			= 1 << 9,
	kDwIntHto			= 1 << 10,
	kDwIntFrun			= 1 << 11,
	kDwIntHle			= 1 << 12,
	kDwIntSbe			= 1 << 13,
	kDwIntAcd			= 1 << 14,
	kDwIntEbe			= 1 << 15,
	// Beacon: SDIO card interrupt of slot 0 (Linux SDMMC_INT_SDIO(0); the
	// StarFive glue sets no sdio_id0 offset).
	kDwIntSdio			= 1 << 16,

	// Linux DW_MCI_DATA_ERROR_FLAGS / DW_MCI_CMD_ERROR_FLAGS. FRUN (FIFO
	// under/overrun) is added for PIO: it means our FIFO accounting is wrong.
	kDwIntDataError		= kDwIntDrto | kDwIntDcrc | kDwIntHto | kDwIntSbe
		| kDwIntEbe | kDwIntHle | kDwIntFrun,
	kDwIntCmdError		= kDwIntRto | kDwIntRcrc | kDwIntRespErr | kDwIntHle,
	kDwIntAllButCd		= 0xfffffffe,
	// Clearing RINTSTS before a command must not drop a pending SDIO IRQ.
	kDwIntAllButCdSdio	= 0xfffefffe,
};

// CMD
enum {
	kDwCmdIndexMask		= 0x3f,
	kDwCmdRespExp		= 1 << 6,
	kDwCmdRespLong		= 1 << 7,
	kDwCmdRespCrc		= 1 << 8,
	kDwCmdDataExp		= 1 << 9,
	kDwCmdDataWrite		= 1 << 10,
	kDwCmdSendStop		= 1 << 12,	// auto-stop; not used, we send CMD12
	kDwCmdPrvDatWait	= 1 << 13,
	kDwCmdStopAbort		= 1 << 14,
	kDwCmdInit			= 1 << 15,
	kDwCmdUpdClk		= 1 << 21,
	kDwCmdVoltSwitch	= 1 << 28,
	kDwCmdUseHoldReg	= 1 << 29,
	kDwCmdStart			= 1u << 31,
};

// STATUS
enum {
	kDwStatusFifoEmpty	= 1 << 2,
	kDwStatusFifoFull	= 1 << 3,
	kDwStatusBusy		= 1 << 9,
	kDwStatusDmaReq		= 1u << 31,
};

static inline uint32
dw_status_fifo_count(uint32 status)
{
	return (status >> 17) & 0x1fff;
}

static inline uint32
dw_fifoth(uint32 msize, uint32 rxWmark, uint32 txWmark)
{
	return ((msize & 0x7) << 28) | ((rxWmark & 0xfff) << 16)
		| (txWmark & 0xfff);
}

// HCON
static inline uint32
dw_hcon_data_width(uint32 hcon)
{
	// 0 = 16 bit, 1 = 32 bit, 2 = 64 bit
	return (hcon >> 7) & 0x7;
}

static inline uint32
dw_hcon_addr_config(uint32 hcon)
{
	// 1 = 64-bit IDMAC addressing (for the future IDMAC path)
	return (hcon >> 27) & 0x1;
}

// UHS_REG
enum {
	kDwUhs18V			= 1 << 0,
	kDwUhsDdr			= 1 << 16,
};

// BMOD
enum {
	kDwBmodSwReset		= 1 << 0,
	kDwBmodFixedBurst	= 1 << 1,
	kDwBmodIdmacEnable	= 1 << 7,
};


// #pragma mark - JH7110 helpers (diagnostics only, no clock driver)

enum {
	kJh7110SysCrgBase		= 0x13020000,
	// JH7110_SYSCLK_SDIOx_AHB = 91/92, JH7110_SYSCLK_SDIOx_SDCARD = 93/94,
	// one 32-bit register per clock. Bit 31 = enable, bits 23:0 = divider.
	// (Linux include/dt-bindings/clock/starfive,jh7110-crg.h and
	// drivers/clk/starfive/clk-starfive-jh71x0.h)
	kJh7110SysClkSdio0Ahb	= 91,
	kJh7110SysClkSdio0Card	= 93,
	kJh7110ClkEnable		= 1u << 31,
	kJh7110ClkDivMask		= 0x00ffffff,
	// Reset status: JH7110_SYSRST_SDIOx_AHB = 64/65, status words at 0x308,
	// bit set = out of reset (Linux reset-starfive-jh7110.c, jh71x0.c).
	kJh7110SysRstStatus		= 0x308,
	kJh7110SysRstAssert		= 0x2f8,	// bit set = held in reset
	kLiteSdcardDivider		= 8,		// axi_cfg0 / 8 = 50 MHz
	kJh7110SysRstSdio0Ahb	= 64,
	kJh7110Sdio0Base		= 0x16010000,
	kJh7110Sdio1Base		= 0x16020000,
};


// #pragma mark - configuration read from the device tree


struct DwMmcConfig {
	phys_addr_t		physBase;
	size_t			physSize;
	uint64			irq;
	uint32			busHz;			// card input clock ("ciu")
	uint32			maxHz;			// DT max-frequency, 0 = busHz
	uint32			fifoDepth;		// DT fifo-depth, 0 = read FIFOTH
	uint32			dataAddr;		// DT data-addr, 0 = by version
	uint32			busWidth;		// DT bus-width (1, 4, 8)
	uint32			powerOnDelayMs;	// DT post-power-on-delay-ms
	bool			cdUsable;		// CDETECT register can be trusted
	bool			isJh7110;
	bool			hasStarfiveCompat;
	char			name[32];

	// Beacon SDIO power sequence (mmc-pwrseq-simple reset-gpios, or the
	// Orange Pi RV fallback on the JH7110 sdio0, see README.md).
	int32			resetGpio;			// -1 = none
	bool			resetActiveLow;
	bool			resetFromDt;		// pwrseq in DT: always run at init
	bool			resetHeuristic;		// run only if nothing answered
	uint32			resetPostDelayMs;	// pwrseq post-power-on-delay-ms
	uint32			maxKhzSetting;		// driver settings max_clock_khz
	bool			isJh7110Sdio0;
	bool			liteSdio1;		// VF2 Lite on-board Wi-Fi slot: pinmux
};


// #pragma mark - host controller


class DwMmcHost {
public:
								DwMmcHost();
								~DwMmcHost();

			status_t			Init(const DwMmcConfig& config);

			status_t			SetClock(uint32 kilohertz);
			status_t			ExecuteCommand(uint8_t command,
									uint32_t argument, uint32_t* response);
			status_t			DoIO(uint8_t command, IOOperation* operation,
									bool offsetAsSectors);
			void				SetScanSemaphore(sem_id semaphore);
			void				SetBusWidth(int width);
			void				SetCardType(card_type type);
			void				TerminateBus();

			// Beacon: SDIO
			status_t			ExecuteBufferCommand(uint8_t command,
									uint32_t argument, uint32_t* response,
									const mmc_buffer_transfer* transfer);
			void				SetSdioIrqSemaphore(sem_id semaphore);
			void				EnableSdioIrq(bool enable);
			status_t			PowerCycle();

private:
	// Response kinds, see the SD/eMMC specs (R1..R7).
	enum ReplyType {
		kReplyNone,
		kReplyR1,
		kReplyR1b,
		kReplyR2,
		kReplyR3,
		kReplyR6,
		kReplyR7,
		kReplyInvalid,
	};

	// Data transfer of the command being issued, the IDMAC path would hook
	// in here (_TransferPio is the only implementation so far).
	struct DataPhase {
		bool			isWrite;
		generic_io_vec*	vecs;
		size_t			vecCount;
		uint8*			buffer;		// kernel virtual buffer instead of vecs
		size_t			length;
	};

	static	int32				_InterruptHandler(void* data);
			int32				_HandleInterrupt();

			uint32				_Read(uint32 reg) const;
			void				_Write(uint32 reg, uint32 value);

			status_t			_CheckJh7110Clocks();
			bool				_ResetController(uint32 flags);
			status_t			_UpdateClockRegisters();
			status_t			_SetClockHz(uint32 hz);
			void				_EnableInterrupts(uint32 mask);
			uint32				_Status();
			void				_Ack(uint32 bits);
			status_t			_WaitForStatus(uint32 mask, bigtime_t deadline,
									uint32& status);
			status_t			_WaitWhileBusy(bigtime_t timeout);

			ReplyType			_ReplyTypeFor(uint8_t command,
									uint32_t argument) const;
			status_t			_ExecuteCommandLocked(uint8_t command,
									uint32_t argument, uint32_t* response,
									const DataPhase* data);
			status_t			_TransferPio(const DataPhase& data,
									bigtime_t deadline);
			status_t			_CopyToVecs(const DataPhase& data,
									size_t& vecIndex, size_t& vecOffset,
									const uint8* from, size_t length);
			status_t			_CopyFromVecs(const DataPhase& data,
									size_t& vecIndex, size_t& vecOffset,
									uint8* to, size_t length);
			void				_RecoverFromDataError();
			bool				_ProbeForCard();
			bool				_ProbeSdio(int tries);
			void				_SdioPowerSequence(const char* why);
			void				_SetupJh7110Gpio();
			void				_SetupLiteSdio1Pins();
			status_t			_ApplyClockEnable();

private:
			DwMmcConfig			fConfig;
			area_id				fRegsArea;
			volatile uint8*		fRegs;
			volatile uint8*		fFifo;
			uint32				fFifoWidth;		// bytes per FIFO access, 4 or 8
			uint32				fFifoDepth;		// in FIFO entries
			uint32				fFifoth;
			uint32				fVersion;
			uint32				fIdstsReg;
			uint32				fIdintenReg;

			mutex				fLock;
			spinlock			fIntmaskLock;
			ConditionVariable	fInterruptCondition;
			bool				fInterruptInstalled;
			int32				fInterruptCount;
			int32				fMissedInterrupts;
			bool				fPollMode;
			int32				fTraceCommands;
			int32				fTraceIo;

			sem_id				fScanSemaphore;
			sem_id				fSdioIrqSemaphore;
			bool				fSdioIrqEnabled;
			int32				fSdioIrqCount;
			int32				fUnhandledCount;
			uint32				fSoftStatus;
				// Beacon: RINTSTS event bits taken by the interrupt handler
				// Beacon: interrupts with nothing pending (see _HandleInterrupt)
			bool				fNoLowPower;
			bool				fResetDone;
			Jh7110SysGpio*		fGpio;
			card_type			fCardType;
			uint32				fCurrentHz;
			uint32				fBusWidth;
			bool				fSendInit;

			// Bounce buffer between the FIFO and physical memory. One SD
			// block; aligned for 64-bit FIFO accesses.
			uint64				fBounce[512 / sizeof(uint64)];
};


#endif	// _DW_MMC_H_
