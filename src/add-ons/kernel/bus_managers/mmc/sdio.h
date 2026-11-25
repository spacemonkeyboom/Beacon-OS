/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2006 Uwe Stuehler <uwe@openbsd.org>
 * Copyright 2026, The Beacon Project (modifications, under the same terms).
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * SDIO card support for the Beacon MMC bus manager. Adapted from OpenBSD
 * sys/dev/sdmmc/sdmmc_io.c (v1.42) and sdmmc_cis.c; see THIRD_PARTY.md.
 */
#ifndef SDIO_H
#define SDIO_H


#include <lock.h>

#include "mmc.h"
#include "sdio_bus.h"


class MMCBus;


class SdioCard {
public:
								SdioCard(MMCBus* bus);
								~SdioCard();

	// Called by the scan thread with the bus lock held.
			status_t			Enumerate(uint32 ocr);
			status_t			Reset();
	// Called by the scan thread WITHOUT the bus lock.
			void				PublishFunctions(device_node* busNode);

			uint16				Rca() const { return fRca; }
			uint8				FunctionCount() const
									{ return fFunctionCount; }

	// sdio_bus_interface back end; each takes the bus lock.
			status_t			GetCardInfo(uint8 function,
									sdio_card_info* info);
			status_t			ReadByte(uint8 function, uint32 address,
									uint8* _value);
			status_t			WriteByte(uint8 function, uint32 address,
									uint8 value, uint8* _readBack);
			status_t			RwExtended(uint8 function, uint32 address,
									bool write, void* buffer, uint32 count,
									uint32 flags);
			status_t			Transfer(uint8 function, uint32 address,
									bool write, uint8* buffer, size_t length,
									uint32 flags);
			status_t			SetBlockSize(uint8 function, uint16 size);
			status_t			EnableFunction(uint8 function,
									bigtime_t timeout);
			status_t			DisableFunction(uint8 function);
			status_t			InstallInterruptHandler(uint8 function,
									sdio_interrupt_handler handler,
									void* data);
			status_t			RemoveInterruptHandler(uint8 function);
			status_t			SetBusClock(uint32 kilohertz);
			status_t			Abort(uint8 function);

private:
	// Bus lock held by the caller.
			status_t			_Direct(bool write, uint8 function,
									uint32 address, uint8 in, bool raw,
									uint8* _out);
			status_t			_Extended(uint8 function, uint32 address,
									bool write, void* buffer, uint32 count,
									uint32 flags);
			status_t			_TransferLocked(uint8 function,
									uint32 address, bool write, uint8* buffer,
									size_t length, uint32 flags);
			status_t			_ReadByteLocked(uint8 function,
									uint32 address, uint8* _value);
			status_t			_WriteByteLocked(uint8 function,
									uint32 address, uint8 value);
			status_t			_SetBlockSizeLocked(uint8 function,
									uint16 size);
			status_t			_ReadCis(uint8 function, uint32 pointer);
			status_t			_CheckDataLines(const char* when);
			void				_SetupBus();
			status_t			_SetClock(uint32 kilohertz);
			bool				_ValidFunction(uint8 function) const;

	static	status_t			_IrqThread(void* data);
			void				_IrqLoop();
			void				_DispatchInterrupts(bool fromHost);

private:
			MMCBus*				fBus;
			uint16				fRca;
			uint8				fFunctionCount;
			uint32				fOcr;
			uint32				fClockKhz;
			uint32				fMaxClockKhz;
			sdio_card_info		fInfo;
			uint8				fCccr[0x20];

			mutex				fHandlerLock;
			sdio_interrupt_handler fHandlers[8];
			void*				fHandlerData[8];
			int32				fHandlerCount;
			sem_id				fIrqSemaphore;
			thread_id			fIrqThread;
			volatile bool		fQuit;
			int32				fIrqTrace;
			bool				fPollNoticeShown;
			int32				fSpurious;
};


#endif	// SDIO_H
