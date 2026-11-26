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
 * SDIO card support for the Beacon MMC bus manager.
 *
 * Adapted from OpenBSD sys/dev/sdmmc/sdmmc_io.c v1.42 (CMD5 OCR loop, CMD52
 * and CMD53 argument encoding, function enable/ready, block size, bus width,
 * high speed, card interrupt dispatch, region/multi transfer splitting) and
 * sdmmc_cis.c (CIS tuple walk). Rewritten for Haiku's device manager and
 * MMCBus. Register numbers and the FUNCE layouts are from the SDIO
 * Simplified Specification; the byte-mode size limit and the per-function
 * MANFID fallback follow the behaviour of Linux drivers/mmc/core (facts
 * only, no Linux code). See THIRD_PARTY.md.
 */


#include "sdio.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include "mmc_bus.h"


#define SDIO_TRACE(x...)	dprintf("sdio: " x)
#define SDIO_ERROR(x...)	dprintf("sdio: ERROR: " x)


// CMD52 / CMD53 argument bits (SDIO spec 5.1, 5.3; OpenBSD sdmmc_ioreg.h)
static const uint32 kArgWrite			= 1u << 31;
static const uint32 kArgFunctionShift	= 28;
static const uint32 kArg52Raw			= 1u << 27;
static const uint32 kArg53BlockMode		= 1u << 27;
static const uint32 kArg53Increment		= 1u << 26;
static const uint32 kArgAddressShift	= 9;
static const uint32 kArgAddressMask		= 0x1ffff;
static const uint32 kArg53CountMask		= 0x1ff;
static const uint32 kMaxBlockCount		= 511;

// R5 flags (SDIO spec 5.2)
static const uint32 kR5ComCrcError		= 1u << 15;
static const uint32 kR5IllegalCommand	= 1u << 14;
static const uint32 kR5Error			= 1u << 11;
static const uint32 kR5FunctionNumber	= 1u << 9;
static const uint32 kR5OutOfRange		= 1u << 8;

// R4 / OCR
static const uint32 kOcrReady			= 1u << 31;
static const uint32 kOcrMemoryPresent	= 1u << 27;
static const uint32 kOcrVoltageMask		= 0x00ffff00;
static const uint32 kOcr32To34			= 0x00300000;	// host: 3.3 V only

// CCCR bits
static const uint8 kCapSmb				= 1 << 1;	// multi-block CMD53
static const uint8 kCapLsc				= 1 << 6;	// low-speed card
static const uint8 k4bls				= 1 << 7;	// 4-bit low-speed
static const uint8 kSpeedShs			= 1 << 0;
static const uint8 kSpeedEhs			= 1 << 1;
static const uint8 kBusWidthMask		= 0x03;
static const uint8 kBusWidth4			= 0x02;
static const uint8 kAbortRes			= 1 << 3;

// CIS tuples
static const uint8 kTupleNull			= 0x00;
static const uint8 kTupleVers1			= 0x15;
static const uint8 kTupleManfid			= 0x20;
static const uint8 kTupleFuncid			= 0x21;
static const uint8 kTupleFunce			= 0x22;
static const uint8 kTupleEnd			= 0xff;
static const uint32 kCisStart			= 0x01000;
static const uint32 kCisEnd				= 0x17fff;

static const uint16 kHostMaxBlockSize	= 2048;
static const bigtime_t kIrqPollInterval	= 100000;
static const int32 kIrqTraceBudget		= 8;


SdioCard::SdioCard(MMCBus* bus)
	:
	fBus(bus),
	fRca(0),
	fFunctionCount(0),
	fOcr(0),
	fClockKhz(400),
	fMaxClockKhz(25000),
	fHandlerCount(0),
	fIrqSemaphore(-1),
	fIrqThread(-1),
	fQuit(false),
	fIrqTrace(kIrqTraceBudget),
	fPollNoticeShown(false),
	fSpurious(0)
{
	memset(&fInfo, 0, sizeof(fInfo));
	memset(fCccr, 0, sizeof(fCccr));
	memset(fHandlers, 0, sizeof(fHandlers));
	memset(fHandlerData, 0, sizeof(fHandlerData));
	mutex_init(&fHandlerLock, "sdio irq handlers");
}


SdioCard::~SdioCard()
{
	fQuit = true;
	if (fIrqSemaphore >= 0) {
		if (fBus->fController->set_sdio_irq_semaphore != NULL)
			fBus->fController->set_sdio_irq_semaphore(fBus->fCookie, -1);
		delete_sem(fIrqSemaphore);
	}
	if (fIrqThread >= 0) {
		status_t result;
		wait_for_thread(fIrqThread, &result);
	}
	mutex_destroy(&fHandlerLock);
}


bool
SdioCard::_ValidFunction(uint8 function) const
{
	return function <= fFunctionCount && function < 8;
}


// #pragma mark - CMD52 / CMD53 (bus lock held)


status_t
SdioCard::_Direct(bool write, uint8 function, uint32 address, uint8 in,
	bool raw, uint8* _out)
{
	// OpenBSD sdmmc_io_rw_direct()
	uint32 argument = (write ? kArgWrite : 0)
		| ((uint32)(function & 7) << kArgFunctionShift)
		| (raw ? kArg52Raw : 0)
		| ((address & kArgAddressMask) << kArgAddressShift)
		| in;
	uint32_t response = 0;
	status_t status = fBus->ExecuteCommand(fRca, SD_IO_RW_DIRECT, argument,
		&response);
	if (status != B_OK)
		return status;

	if ((response & (kR5Error | kR5FunctionNumber | kR5OutOfRange)) != 0) {
		SDIO_ERROR("CMD52 %s fn%u %#" B_PRIx32 ": R5 flags %#" B_PRIx32 "%s%s"
			"%s\n", write ? "write" : "read", function, address,
			response & 0xff00, (response & kR5Error) ? " ERROR" : "",
			(response & kR5FunctionNumber) ? " FUNCTION_NUMBER" : "",
			(response & kR5OutOfRange) ? " OUT_OF_RANGE" : "");
		return (response & kR5FunctionNumber) != 0 ? B_BAD_VALUE
			: B_IO_ERROR;
	}
	if ((response & (kR5ComCrcError | kR5IllegalCommand)) != 0) {
		// These describe the previous command; log only.
		SDIO_TRACE("CMD52 fn%u %#" B_PRIx32 ": R5 reports %s%s on the "
			"previous command\n", function, address,
			(response & kR5ComCrcError) ? "COM_CRC_ERROR " : "",
			(response & kR5IllegalCommand) ? "ILLEGAL_COMMAND" : "");
	}
	if (_out != NULL)
		*_out = response & 0xff;
	return B_OK;
}


status_t
SdioCard::_ReadByteLocked(uint8 function, uint32 address, uint8* _value)
{
	return _Direct(false, function, address, 0, false, _value);
}


status_t
SdioCard::_WriteByteLocked(uint8 function, uint32 address, uint8 value)
{
	return _Direct(true, function, address, value, false, NULL);
}


status_t
SdioCard::_Extended(uint8 function, uint32 address, bool write, void* buffer,
	uint32 count, uint32 flags)
{
	// OpenBSD sdmmc_io_rw_extended_subr()
	if (!_ValidFunction(function) || buffer == NULL || count == 0)
		return B_BAD_VALUE;

	bool blockMode = (flags & SDIO_RW_BLOCK_MODE) != 0;
	mmc_buffer_transfer transfer;
	transfer.buffer = buffer;
	transfer.is_write = write;
	if (blockMode) {
		uint16 blockSize = fInfo.functions[function].block_size;
		if ((fInfo.card_capability & kCapSmb) == 0)
			return B_NOT_SUPPORTED;
		if (count > kMaxBlockCount || blockSize == 0)
			return B_BAD_VALUE;
		transfer.block_size = blockSize;
		transfer.block_count = count;
	} else {
		if (count > 512)
			return B_BAD_VALUE;
		transfer.block_size = count;
		transfer.block_count = 1;
	}

	uint32 argument = (write ? kArgWrite : 0)
		| ((uint32)function << kArgFunctionShift)
		| (blockMode ? kArg53BlockMode : 0)
		| ((flags & SDIO_RW_INCREMENT) != 0 ? kArg53Increment : 0)
		| ((address & kArgAddressMask) << kArgAddressShift)
		| (count & kArg53CountMask);	// 512 bytes is sent as 0

	uint32_t response = 0;
	status_t status = fBus->ExecuteBufferCommand(fRca, SD_IO_RW_EXTENDED,
		argument, &response, &transfer);
	if (status == B_OK
		&& (response & (kR5Error | kR5FunctionNumber | kR5OutOfRange)) != 0) {
		SDIO_ERROR("CMD53 fn%u %#" B_PRIx32 ": R5 flags %#" B_PRIx32 "\n",
			function, address, response & 0xff00);
		status = B_IO_ERROR;
	}
	if (status != B_OK) {
		SDIO_ERROR("CMD53 %s fn%u addr %#" B_PRIx32 " %s %" B_PRIu32
			" failed: %s; aborting\n", write ? "write" : "read", function,
			address, blockMode ? "blocks" : "bytes", count, strerror(status));
		_Direct(true, 0, SDIO_CCCR_ABORT, function, false, NULL);
	}
	return status;
}


status_t
SdioCard::_TransferLocked(uint8 function, uint32 address, bool write,
	uint8* buffer, size_t length, uint32 flags)
{
	// OpenBSD sdmmc_io_{read,write}_{region,multi}_1(): whole blocks in
	// block mode, then the rest in byte mode. Like Linux, a byte-mode command
	// carries at most min(block size, 512) bytes.
	if (!_ValidFunction(function) || (buffer == NULL && length > 0))
		return B_BAD_VALUE;

	bool increment = (flags & SDIO_RW_INCREMENT) != 0;
	uint32 rwFlags = increment ? SDIO_RW_INCREMENT : 0;
	uint16 blockSize = fInfo.functions[function].block_size;
	bool multiBlock = (fInfo.card_capability & kCapSmb) != 0
		&& blockSize != 0;
	size_t byteChunk = blockSize != 0 && blockSize < 512 ? blockSize : 512;

	while (length > 0) {
		uint32 count;
		size_t bytes;
		status_t status;
		if (multiBlock && length >= blockSize) {
			count = (uint32)(length / blockSize);
			if (count > kMaxBlockCount)
				count = kMaxBlockCount;
			bytes = (size_t)count * blockSize;
			status = _Extended(function, address, write, buffer, count,
				rwFlags | SDIO_RW_BLOCK_MODE);
		} else {
			bytes = length < byteChunk ? length : byteChunk;
			count = (uint32)bytes;
			status = _Extended(function, address, write, buffer, count,
				rwFlags);
		}
		if (status != B_OK)
			return status;
		buffer += bytes;
		length -= bytes;
		if (increment)
			address += bytes;
	}
	return B_OK;
}


status_t
SdioCard::_SetBlockSizeLocked(uint8 function, uint16 size)
{
	// OpenBSD sdmmc_io_set_blocklen()
	if (!_ValidFunction(function))
		return B_BAD_VALUE;
	uint16 functionMax = fInfo.functions[function].max_block_size;
	if (size == 0) {
		size = 512;
		if (functionMax != 0 && functionMax < size)
			size = functionMax;
	}
	if (size > kHostMaxBlockSize
		|| (functionMax != 0 && size > functionMax)) {
		SDIO_ERROR("fn%u: block size %u above the maximum %u\n", function,
			size, functionMax != 0 ? functionMax : kHostMaxBlockSize);
		return B_BAD_VALUE;
	}

	uint32 base = function == 0 ? (uint32)SDIO_CCCR_FN0_BLOCK_SIZE
		: SDIO_FBR_BASE(function) + SDIO_FBR_BLOCK_SIZE;
	status_t status = _WriteByteLocked(0, base, size & 0xff);
	if (status == B_OK)
		status = _WriteByteLocked(0, base + 1, size >> 8);
	if (status != B_OK)
		return status;
	fInfo.functions[function].block_size = size;
	SDIO_TRACE("fn%u: block size %u\n", function, size);
	return B_OK;
}


// #pragma mark - enumeration (bus lock held)


status_t
SdioCard::_ReadCis(uint8 function, uint32 pointer)
{
	// OpenBSD sdmmc_read_cis(); FUNCE layouts from the SDIO spec 16.7.
	sdio_function_info& info = fInfo.functions[function];
	if (pointer < kCisStart || pointer > kCisEnd - 16) {
		SDIO_ERROR("fn%u: bad CIS pointer %#" B_PRIx32 "\n", function,
			pointer);
		return B_BAD_DATA;
	}
	info.cis_pointer = pointer;

	uint32 address = pointer;
	for (int tuples = 0; tuples < 64; tuples++) {
		uint8 code;
		status_t status = _ReadByteLocked(0, address++, &code);
		if (status != B_OK)
			return status;
		if (code == kTupleEnd)
			return B_OK;
		if (code == kTupleNull)
			continue;

		uint8 length;
		status = _ReadByteLocked(0, address++, &length);
		if (status != B_OK)
			return status;

		uint8 body[256];
		for (uint32 i = 0; i < length; i++) {
			status = _ReadByteLocked(0, address + i, &body[i]);
			if (status != B_OK)
				return status;
		}
		address += length;

		switch (code) {
			case kTupleManfid:
				if (length >= 4) {
					info.vendor = body[0] | (body[1] << 8);
					info.device = body[2] | (body[3] << 8);
					SDIO_TRACE("fn%u CIS: MANFID vendor %#06x device %#06x\n",
						function, info.vendor, info.device);
				}
				break;
			case kTupleFuncid:
				if (length >= 1) {
					SDIO_TRACE("fn%u CIS: FUNCID %#04x%s\n", function,
						body[0], body[0] == 0x0c ? " (SDIO)" : "");
				}
				break;
			case kTupleFunce:
				if (function == 0 && length >= 4 && body[0] == 0x00) {
					// TPLFE_FN0_BLK_SIZE, TPLFE_MAX_TRAN_SPEED
					info.max_block_size = body[1] | (body[2] << 8);
					SDIO_TRACE("fn0 CIS: FUNCE max block %u, max speed code "
						"%#04x\n", info.max_block_size, body[3]);
				} else if (function != 0 && length >= 14 && body[0] == 0x01) {
					// TPLFE_MAX_BLK_SIZE at offset 12
					info.max_block_size = body[12] | (body[13] << 8);
					SDIO_TRACE("fn%u CIS: FUNCE max block %u\n", function,
						info.max_block_size);
				}
				break;
			case kTupleVers1:
				if (function == 0 && length > 2) {
					// major, minor, then NUL-separated strings, 0xff ends
					size_t out = 0;
					for (uint32 i = 2; i < length && body[i] != 0xff; i++) {
						if (out >= sizeof(fInfo.product) - 3)
							break;
						if (body[i] == 0) {
							if (out > 0 && i + 1 < length && body[i + 1] != 0
								&& body[i + 1] != 0xff) {
								fInfo.product[out++] = ',';
								fInfo.product[out++] = ' ';
							}
						} else if (body[i] >= 0x20 && body[i] < 0x7f)
							fInfo.product[out++] = body[i];
					}
					fInfo.product[out] = '\0';
					SDIO_TRACE("fn0 CIS: VERS_1 %u.%u \"%s\"\n", body[0],
						body[1], fInfo.product);
				}
				break;
			default:
				break;
		}
	}
	SDIO_TRACE("fn%u CIS: no END tuple within 64 tuples\n", function);
	return B_OK;
}


status_t
SdioCard::_CheckDataLines(const char* when)
{
	// Read CCCR 0x00..0x07 through the DAT lines (CMD53, fn0, byte mode)
	// and compare with what CMD52 read over CMD. Proves the data path, the
	// bus width and the clock in one go.
	uint8 data[8] = {};
	status_t status = _Extended(0, 0, false, data, sizeof(data),
		SDIO_RW_INCREMENT);
	if (status != B_OK) {
		SDIO_ERROR("CMD53 data check (%s) failed: %s\n", when,
			strerror(status));
		return status;
	}
	// IO_READY and INT_PENDING may change; compare the stable bytes.
	static const int kStable[] = { 0, 1, 2, 4, 7 };
	for (size_t i = 0; i < sizeof(kStable) / sizeof(kStable[0]); i++) {
		int index = kStable[i];
		if (data[index] != fCccr[index]) {
			SDIO_ERROR("CMD53 data check (%s): CCCR[%d] reads %#04x over DAT, "
				"%#04x over CMD\n", when, index, data[index], fCccr[index]);
			return B_IO_ERROR;
		}
	}
	SDIO_TRACE("CMD53 data check (%s): OK, CCCR %02x %02x %02x %02x %02x %02x"
		" %02x %02x\n", when, data[0], data[1], data[2], data[3], data[4],
		data[5], data[6], data[7]);
	return B_OK;
}


status_t
SdioCard::_SetClock(uint32 kilohertz)
{
	fBus->SetClock(kilohertz);
	fClockKhz = kilohertz;
	fInfo.clock_khz = kilohertz;
	return B_OK;
}


void
SdioCard::_SetupBus()
{
	// OpenBSD sdmmc_io_init() for function 0: high speed if the card has
	// SHS, then 4-bit unless it is a low-speed card without 4BLS. Each step
	// is checked with a CMD53 read and undone if it fails.
	uint8 speed = fCccr[SDIO_CCCR_SPEED];
	uint8 caps = fCccr[SDIO_CCCR_CARD_CAPABILITY];

	_SetClock(25000);
	fMaxClockKhz = 25000;
	// Beacon: high speed (50 MHz) is skipped for now: without sample-phase
	// tuning, long CMD53 block reads failed on the Orange Pi RV (rv29).
	if (false && (speed & kSpeedShs) != 0) {
		if (_WriteByteLocked(0, SDIO_CCCR_SPEED, speed | kSpeedEhs) == B_OK) {
			_SetClock(50000);
			if (_CheckDataLines("1-bit, 50 MHz high speed") == B_OK) {
				fInfo.high_speed = 1;
				fMaxClockKhz = 50000;
			} else {
				SDIO_TRACE("high speed does not work, back to 25 MHz\n");
				_SetClock(25000);
				_WriteByteLocked(0, SDIO_CCCR_SPEED, speed & ~kSpeedEhs);
			}
		}
	} else
		SDIO_TRACE("card has no high-speed support (CCCR speed %#04x)\n",
			speed);

	if ((caps & kCapLsc) != 0 && (caps & k4bls) == 0) {
		SDIO_TRACE("low-speed card without 4-bit support, staying 1-bit\n");
		return;
	}

	uint8 width = fCccr[SDIO_CCCR_BUS_INTERFACE];
	if (_WriteByteLocked(0, SDIO_CCCR_BUS_INTERFACE,
			(width & ~kBusWidthMask) | kBusWidth4) != B_OK) {
		return;
	}
	fBus->SetBusWidth(4);
	fCccr[SDIO_CCCR_BUS_INTERFACE] = (width & ~kBusWidthMask) | kBusWidth4;
	if (_CheckDataLines("4-bit") == B_OK) {
		fInfo.bus_width = 4;
		return;
	}

	SDIO_TRACE("4-bit bus does not work, back to 1-bit\n");
	_WriteByteLocked(0, SDIO_CCCR_BUS_INTERFACE, width & ~kBusWidthMask);
	fCccr[SDIO_CCCR_BUS_INTERFACE] = width & ~kBusWidthMask;
	fBus->SetBusWidth(1);
}


status_t
SdioCard::Enumerate(uint32 ocr)
{
	fOcr = ocr;
	fFunctionCount = (ocr >> 28) & 7;
	fInfo.size = sizeof(fInfo);
	fInfo.ocr = ocr;
	fInfo.function_count = fFunctionCount;
	fInfo.bus_width = 1;
	fInfo.host_max_block_size = kHostMaxBlockSize;
	fInfo.clock_khz = 400;

	SDIO_TRACE("card OCR %#" B_PRIx32 ": %u I/O function(s), memory %s, "
		"voltage window %#" B_PRIx32 "\n", ocr, fFunctionCount,
		(ocr & kOcrMemoryPresent) != 0 ? "present (combo card; memory part "
			"ignored)" : "absent", ocr & kOcrVoltageMask);

	// OpenBSD sdmmc_io_send_op_cond(): repeat CMD5 with our voltage window
	// until the card reports ready (C bit).
	uint32 window = ocr & kOcrVoltageMask & kOcr32To34;
	if (window == 0) {
		SDIO_TRACE("card OCR has no 3.2-3.4 V bits, offering its own window\n");
		window = ocr & kOcrVoltageMask;
	}
	uint32_t response = 0;
	status_t status = B_TIMED_OUT;
	int tries;
	for (tries = 1; tries <= 100; tries++) {
		status = fBus->ExecuteCommand(0, SD_IO_SEND_OP_COND, window, &response);
		if (status == B_OK && (response & kOcrReady) != 0)
			break;
		snooze(10000);
	}
	if (status != B_OK || (response & kOcrReady) == 0) {
		SDIO_ERROR("card not ready after CMD5 arg %#" B_PRIx32 " (%s, R4 %#"
			B_PRIx32 ")\n", window, strerror(status), response);
		return status != B_OK ? status : B_TIMED_OUT;
	}
	SDIO_TRACE("card ready after %d CMD5 (arg %#" B_PRIx32 ", R4 %#" B_PRIx32
		")\n", tries, window, response);

	fBus->SetCardType(CARD_TYPE_SDIO);

	// CMD3 (R6): the card publishes its RCA. Then select it with CMD7.
	status = fBus->ExecuteCommand(0, SD_SEND_RELATIVE_ADDR, 0, &response);
	if (status != B_OK) {
		SDIO_ERROR("CMD3 failed: %s\n", strerror(status));
		return status;
	}
	fRca = response >> 16;
	fInfo.rca = fRca;
	SDIO_TRACE("CMD3: RCA %#06x (R6 %#" B_PRIx32 ")\n", fRca, response);

	status = fBus->_ActivateDevice(fRca);
	if (status != B_OK) {
		SDIO_ERROR("CMD7 select of RCA %#x failed: %s\n", fRca,
			strerror(status));
		return status;
	}
	SDIO_TRACE("CMD7: card selected\n");

	// CCCR
	for (uint32 i = 0; i < sizeof(fCccr); i++) {
		status = _ReadByteLocked(0, i, &fCccr[i]);
		if (status != B_OK) {
			SDIO_ERROR("CMD52 read of CCCR[%#" B_PRIx32 "] failed: %s\n", i,
				strerror(status));
			return status;
		}
	}
	fInfo.cccr_revision = fCccr[SDIO_CCCR_REVISION] & 0x0f;
	fInfo.sdio_revision = fCccr[SDIO_CCCR_REVISION] >> 4;
	fInfo.sd_revision = fCccr[SDIO_CCCR_SD_REVISION];
	fInfo.card_capability = fCccr[SDIO_CCCR_CARD_CAPABILITY];
	uint32 cisPointer = fCccr[SDIO_CCCR_CIS_POINTER]
		| (fCccr[SDIO_CCCR_CIS_POINTER + 1] << 8)
		| (fCccr[SDIO_CCCR_CIS_POINTER + 2] << 16);
	SDIO_TRACE("CCCR: rev %#04x (CCCR %u, SDIO %u), SD rev %#04x, IOE %#04x, "
		"IOR %#04x, IEN %#04x, bus %#04x, caps %#04x%s%s, speed %#04x, CIS "
		"%#" B_PRIx32 "\n", fCccr[0], fInfo.cccr_revision, fInfo.sdio_revision,
		fInfo.sd_revision, fCccr[2], fCccr[3], fCccr[4], fCccr[7], fCccr[8],
		(fCccr[8] & kCapSmb) ? " SMB" : "", (fCccr[8] & kCapLsc) ? " LSC" : "",
		fCccr[SDIO_CCCR_SPEED], cisPointer);

	status = _ReadCis(0, cisPointer);
	if (status != B_OK)
		return status;
	fInfo.vendor = fInfo.functions[0].vendor;
	fInfo.device = fInfo.functions[0].device;
	SDIO_TRACE("common CIS: vendor %#06x device %#06x \"%s\"\n", fInfo.vendor,
		fInfo.device, fInfo.product);

	// Function 0 block size, needed for the CMD53 data-line checks.
	uint16 fn0Block = fInfo.functions[0].max_block_size;
	if (fn0Block == 0 || fn0Block > 512)
		fn0Block = fn0Block == 0 ? 64 : 512;
	_SetBlockSizeLocked(0, fn0Block);

	if (_CheckDataLines("1-bit, 400 kHz") != B_OK)
		SDIO_ERROR("CMD53 does not work at 400 kHz, 1 bit; data transfers "
			"will fail\n");

	// FBR + per-function CIS
	for (uint8 function = 1; function <= fFunctionCount; function++) {
		sdio_function_info& info = fInfo.functions[function];
		uint32 fbr = SDIO_FBR_BASE(function);
		uint8 code = 0;
		uint8 ext = 0;
		uint8 pointer[3] = {};
		_ReadByteLocked(0, fbr + SDIO_FBR_STD_INTERFACE, &code);
		_ReadByteLocked(0, fbr + SDIO_FBR_EXT_INTERFACE, &ext);
		for (int i = 0; i < 3; i++)
			_ReadByteLocked(0, fbr + SDIO_FBR_CIS_POINTER + i, &pointer[i]);
		info.interface_class = code & 0x0f;
		info.extended_class = info.interface_class == 0x0f ? ext : 0;
		uint32 functionCis = pointer[0] | (pointer[1] << 8)
			| (pointer[2] << 16);
		SDIO_TRACE("fn%u FBR: interface %#04x (ext %#04x), CIS %#" B_PRIx32
			"\n", function, info.interface_class, info.extended_class,
			functionCis);
		if (_ReadCis(function, functionCis) != B_OK)
			SDIO_TRACE("fn%u: CIS unreadable, using the common ids\n",
				function);
		if (info.vendor == 0 && info.device == 0) {
			// Linux does the same: functions without MANFID inherit the
			// card's ids.
			info.vendor = fInfo.vendor;
			info.device = fInfo.device;
		}
		_SetBlockSizeLocked(function, 0);
	}

	_SetupBus();

	SDIO_TRACE("card ready: RCA %#06x, vendor %#06x device %#06x, %u "
		"function(s), %u-bit, %" B_PRIu32 " kHz%s\n", fRca, fInfo.vendor,
		fInfo.device, fFunctionCount, fInfo.bus_width, fClockKhz,
		fInfo.high_speed ? " (high speed)" : "");
	return B_OK;
}


void
SdioCard::PublishFunctions(device_node* busNode)
{
	for (uint8 function = 1; function <= fFunctionCount; function++) {
		const sdio_function_info& info = fInfo.functions[function];
		char name[64];
		snprintf(name, sizeof(name), "SDIO function %u (%04x:%04x)",
			function, info.vendor, info.device);

		device_attr attrs[] = {
			{ B_DEVICE_BUS, B_STRING_TYPE, { .string = SDIO_BUS_NAME }},
			{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = name }},
			{ B_DEVICE_VENDOR_ID, B_UINT16_TYPE, { .ui16 = info.vendor }},
			{ B_DEVICE_ID, B_UINT16_TYPE, { .ui16 = info.device }},
			{ SDIO_FUNCTION_ITEM, B_UINT8_TYPE, { .ui8 = function }},
			{ SDIO_VENDOR_ITEM, B_UINT16_TYPE, { .ui16 = info.vendor }},
			{ SDIO_DEVICE_ITEM, B_UINT16_TYPE, { .ui16 = info.device }},
			{ SDIO_CLASS_ITEM, B_UINT8_TYPE, { .ui8 = info.interface_class }},
			{ SDIO_FUNCTION_COUNT_ITEM, B_UINT8_TYPE,
				{ .ui8 = fFunctionCount }},
			{ SDIO_RCA_ITEM, B_UINT16_TYPE, { .ui16 = fRca }},
			{ kMmcTypeAttribute, B_UINT8_TYPE, { .ui8 = CARD_TYPE_SDIO }},
			// Legacy (compat) network drivers such as broadcomfullmac find
			// this node themselves and call get_driver() on it, so it must
			// stay initialised (wlan agent request R1).
			{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
			{}
		};

		status_t status = gDeviceManager->register_node(busNode,
			SDIO_FUNCTION_MODULE_NAME, attrs, NULL, NULL);
		SDIO_TRACE("published function %u: vendor %#06x device %#06x class "
			"%#04x: %s\n", function, info.vendor, info.device,
			info.interface_class, strerror(status));
	}
}


// #pragma mark - public API (takes the bus lock)


status_t
SdioCard::GetCardInfo(uint8 function, sdio_card_info* info)
{
	if (info == NULL || info->size < sizeof(uint32))
		return B_BAD_VALUE;
	size_t size = info->size < sizeof(fInfo) ? info->size : sizeof(fInfo);
	fBus->AcquireBus();
	sdio_card_info copy = fInfo;
	fBus->ReleaseBus();
	copy.size = (uint32)size;
	copy.function = function;
	memcpy(info, &copy, size);
	return B_OK;
}


status_t
SdioCard::ReadByte(uint8 function, uint32 address, uint8* _value)
{
	if (!_ValidFunction(function) || _value == NULL)
		return B_BAD_VALUE;
	fBus->AcquireBus();
	status_t status = _ReadByteLocked(function, address, _value);
	fBus->ReleaseBus();
	return status;
}


status_t
SdioCard::WriteByte(uint8 function, uint32 address, uint8 value,
	uint8* _readBack)
{
	if (!_ValidFunction(function))
		return B_BAD_VALUE;
	fBus->AcquireBus();
	status_t status = _Direct(true, function, address, value,
		_readBack != NULL, _readBack);
	fBus->ReleaseBus();
	return status;
}


status_t
SdioCard::RwExtended(uint8 function, uint32 address, bool write, void* buffer,
	uint32 count, uint32 flags)
{
	fBus->AcquireBus();
	status_t status = _Extended(function, address, write, buffer, count,
		flags);
	fBus->ReleaseBus();
	return status;
}


status_t
SdioCard::Transfer(uint8 function, uint32 address, bool write, uint8* buffer,
	size_t length, uint32 flags)
{
	fBus->AcquireBus();
	status_t status = _TransferLocked(function, address, write, buffer, length,
		flags);
	fBus->ReleaseBus();
	return status;
}


status_t
SdioCard::SetBlockSize(uint8 function, uint16 size)
{
	fBus->AcquireBus();
	status_t status = _SetBlockSizeLocked(function, size);
	fBus->ReleaseBus();
	return status;
}


status_t
SdioCard::EnableFunction(uint8 function, bigtime_t timeout)
{
	// OpenBSD sdmmc_io_function_enable(), with a caller timeout.
	if (!_ValidFunction(function))
		return B_BAD_VALUE;
	if (function == 0)
		return B_OK;
	if (timeout <= 0)
		timeout = 1000000;

	fBus->AcquireBus();
	uint8 enable = 0;
	status_t status = _ReadByteLocked(0, SDIO_CCCR_IO_ENABLE, &enable);
	if (status == B_OK) {
		enable |= 1 << function;
		status = _WriteByteLocked(0, SDIO_CCCR_IO_ENABLE, enable);
	}
	if (status == B_OK)
		fInfo.functions[function].enabled = 1;
	fBus->ReleaseBus();
	if (status != B_OK)
		return status;

	bigtime_t start = system_time();
	for (;;) {
		uint8 ready = 0;
		fBus->AcquireBus();
		status = _ReadByteLocked(0, SDIO_CCCR_IO_READY, &ready);
		fBus->ReleaseBus();
		if (status == B_OK && (ready & (1 << function)) != 0) {
			SDIO_TRACE("fn%u enabled and ready after %" B_PRId64 " us\n",
				function, system_time() - start);
			return B_OK;
		}
		if (system_time() - start > timeout) {
			SDIO_ERROR("fn%u not ready after %" B_PRId64 " us (IOR %#04x)\n",
				function, timeout, ready);
			return B_TIMED_OUT;
		}
		snooze(10000);
	}
}


status_t
SdioCard::DisableFunction(uint8 function)
{
	if (!_ValidFunction(function))
		return B_BAD_VALUE;
	if (function == 0)
		return B_OK;
	fBus->AcquireBus();
	uint8 enable = 0;
	status_t status = _ReadByteLocked(0, SDIO_CCCR_IO_ENABLE, &enable);
	if (status == B_OK) {
		enable &= ~(1 << function);
		status = _WriteByteLocked(0, SDIO_CCCR_IO_ENABLE, enable);
	}
	if (status == B_OK)
		fInfo.functions[function].enabled = 0;
	fBus->ReleaseBus();
	SDIO_TRACE("fn%u disabled: %s\n", function, strerror(status));
	return status;
}


status_t
SdioCard::SetBusClock(uint32 kilohertz)
{
	if (kilohertz == 0)
		return B_BAD_VALUE;
	if (kilohertz > fMaxClockKhz)
		kilohertz = fMaxClockKhz;
	fBus->AcquireBus();
	_SetClock(kilohertz);
	fBus->ReleaseBus();
	SDIO_TRACE("bus clock set to %" B_PRIu32 " kHz\n", kilohertz);
	return B_OK;
}


/*!	Beacon: revive a card that stopped answering. Power-cycles it through the
	host (reset GPIO), then enumerates it again (CMD5, CMD3, CMD7, CCCR, CIS,
	bus width and clock). Function enables, block sizes and interrupt enables
	are back at their power-on values: the function driver sets them again.
	Installed interrupt handlers are kept.
*/
status_t
SdioCard::Reset()
{
	// Beacon: snapshot the installed handlers BEFORE taking the bus.
	// _DispatchInterrupts() takes fHandlerLock and then the bus; taking them
	// the other way round here deadlocked the IRQ thread against the reset
	// (seen on the Orange Pi RV: the Wi-Fi restart never returned and every
	// later txctl timed out).
	uint8 enable = 0;
	{
		MutexLocker locker(fHandlerLock);
		for (uint8 function = 1; function <= 7; function++) {
			if (fHandlers[function] != NULL)
				enable |= 1 | (1 << function);
		}
	}

	fBus->AcquireBus();
	status_t status = fBus->PowerCycle();
	if (status != B_OK) {
		fBus->ReleaseBus();
		SDIO_ERROR("reset: host cannot power-cycle the card: %s\n",
			strerror(status));
		return status;
	}
	uint32_t ocr = 0;
	for (int tries = 0; tries < 10; tries++) {
		status = fBus->ExecuteCommand(0, SD_IO_SEND_OP_COND, 0, &ocr);
		if (status == B_OK)
			break;
		snooze(20000);
	}
	if (status == B_OK)
		status = Enumerate(ocr);
	if (status == B_OK) {
		// The power cycle cleared CCCR INT_ENABLE; restore it for the
		// functions whose handlers are still installed.
		if (enable != 0)
			_WriteByteLocked(0, SDIO_CCCR_INT_ENABLE, enable);
	}
	fBus->ReleaseBus();
	if (status != B_OK)
		SDIO_ERROR("reset: card did not come back: %s\n", strerror(status));
	else
		dprintf("sdio: card reset and enumerated again\n");
	return status;
}


status_t
SdioCard::Abort(uint8 function)
{
	if (!_ValidFunction(function))
		return B_BAD_VALUE;
	fBus->AcquireBus();
	status_t status = _Direct(true, 0, SDIO_CCCR_ABORT, function & 7, false,
		NULL);
	fBus->ReleaseBus();
	return status;
}


// #pragma mark - card interrupt


status_t
SdioCard::InstallInterruptHandler(uint8 function,
	sdio_interrupt_handler handler, void* data)
{
	// OpenBSD sdmmc_intr_establish() + sdmmc_intr_enable()
	if (function == 0 || !_ValidFunction(function) || handler == NULL)
		return B_BAD_VALUE;
	mmc_bus_interface* host = fBus->fController;
	if (host->set_sdio_irq_semaphore == NULL || host->enable_sdio_irq == NULL)
		return B_NOT_SUPPORTED;

	MutexLocker locker(fHandlerLock);
	if (fHandlers[function] != NULL)
		return B_BUSY;

	if (fIrqSemaphore < 0) {
		fIrqSemaphore = create_sem(0, "sdio irq");
		if (fIrqSemaphore < 0)
			return fIrqSemaphore;
		host->set_sdio_irq_semaphore(fBus->fCookie, fIrqSemaphore);
		fIrqThread = spawn_kernel_thread(_IrqThread, "sdio irq",
			B_REAL_TIME_DISPLAY_PRIORITY, this);
		if (fIrqThread < 0)
			return fIrqThread;
		resume_thread(fIrqThread);
	}

	fBus->AcquireBus();
	uint8 enable = 0;
	status_t status = _ReadByteLocked(0, SDIO_CCCR_INT_ENABLE, &enable);
	if (status == B_OK) {
		enable |= 1 | (1 << function);	// master IEN + IENx
		status = _WriteByteLocked(0, SDIO_CCCR_INT_ENABLE, enable);
	}
	fBus->ReleaseBus();
	if (status != B_OK)
		return status;

	fHandlers[function] = handler;
	fHandlerData[function] = data;
	fInfo.functions[function].has_interrupt_handler = 1;
	if (fHandlerCount++ == 0)
		host->enable_sdio_irq(fBus->fCookie, true);
	SDIO_TRACE("fn%u: interrupt handler installed (CCCR IEN %#04x)\n",
		function, enable);
	return B_OK;
}


status_t
SdioCard::RemoveInterruptHandler(uint8 function)
{
	if (function == 0 || !_ValidFunction(function))
		return B_BAD_VALUE;

	MutexLocker locker(fHandlerLock);
	if (fHandlers[function] == NULL)
		return B_ENTRY_NOT_FOUND;
	fHandlers[function] = NULL;
	fHandlerData[function] = NULL;
	fInfo.functions[function].has_interrupt_handler = 0;
	bool last = --fHandlerCount == 0;

	if (last)
		fBus->fController->enable_sdio_irq(fBus->fCookie, false);

	fBus->AcquireBus();
	uint8 enable = 0;
	status_t status = _ReadByteLocked(0, SDIO_CCCR_INT_ENABLE, &enable);
	if (status == B_OK) {
		enable &= ~(1 << function);
		if (last)
			enable &= ~1;
		status = _WriteByteLocked(0, SDIO_CCCR_INT_ENABLE, enable);
	}
	fBus->ReleaseBus();
	SDIO_TRACE("fn%u: interrupt handler removed\n", function);
	return status;
}


status_t
SdioCard::_IrqThread(void* data)
{
	((SdioCard*)data)->_IrqLoop();
	return B_OK;
}


void
SdioCard::_IrqLoop()
{
	// Like OpenBSD sdmmc_intr_task() / Linux sdio_irq_thread(): the host ISR
	// masks its SDIO interrupt and releases the semaphore; here we read
	// CCCR INT_PENDING, run the handlers and unmask again. If the host never
	// signals (broken PLIC route, host poll mode), INT_PENDING is polled every
	// 100 ms while handlers are installed.
	while (!fQuit) {
		status_t status = acquire_sem_etc(fIrqSemaphore, 1,
			B_RELATIVE_TIMEOUT, kIrqPollInterval);
		if (fQuit || status == B_BAD_SEM_ID)
			break;
		bool fromHost = status == B_OK;
		if (!fromHost && atomic_get(&fHandlerCount) == 0)
			continue;
		_DispatchInterrupts(fromHost);
	}
}


void
SdioCard::_DispatchInterrupts(bool fromHost)
{
	MutexLocker locker(fHandlerLock);

	uint8 pending = 0;
	fBus->AcquireBus();
	status_t status = _ReadByteLocked(0, SDIO_CCCR_INT_PENDING, &pending);
	fBus->ReleaseBus();
	if (status != B_OK) {
		SDIO_ERROR("cannot read CCCR INT_PENDING: %s\n", strerror(status));
		pending = 0;
	}

	if (!fromHost && pending == 0)
		return;
	if (!fromHost && !fPollNoticeShown) {
		fPollNoticeShown = true;
		SDIO_TRACE("card interrupt found by polling INT_PENDING (%#04x); the "
			"host SDIO interrupt did not fire\n", pending);
	}
	if (fIrqTrace > 0) {
		fIrqTrace--;
		SDIO_TRACE("card interrupt (%s): INT_PENDING %#04x\n",
			fromHost ? "host" : "poll", pending);
	}

	int called = 0;
	for (uint8 function = 1; function < 8; function++) {
		if ((pending & (1 << function)) != 0 && fHandlers[function] != NULL) {
			fHandlers[function](fHandlerData[function]);
			called++;
		}
	}
	if (called == 0 && fromHost && fHandlerCount == 1) {
		// Linux sdio_irq: with a single handler, call it even if the
		// pending bit already dropped.
		for (uint8 function = 1; function < 8; function++) {
			if (fHandlers[function] != NULL)
				fHandlers[function](fHandlerData[function]);
		}
		fSpurious++;
	}

	if (fromHost && fHandlerCount > 0)
		fBus->fController->enable_sdio_irq(fBus->fCookie, true);
}
