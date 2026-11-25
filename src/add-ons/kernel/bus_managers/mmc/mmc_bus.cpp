/*
 * Copyright 2018-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 */
#include "mmc_bus.h"
#include "sdio.h"

#include <Errors.h>

#include <stdint.h>


MMCBus::MMCBus(device_node* node)
	:
	fNode(node),
	fController(NULL),
	fCookie(NULL),
	fStatus(B_OK),
	fWorkerThread(-1),
	fActiveDevice(0),
	fCardType(CARD_TYPE_UNKNOWN),
	fSdioCard(NULL)
{
	CALLED();

	// Get the parent info, it includes the API to send commands to the hardware
	device_node* parent = gDeviceManager->get_parent_node(node);
	fStatus = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fController, &fCookie);
	gDeviceManager->put_node(parent);

	if (fStatus != B_OK) {
		ERROR("Not able to establish the bus %s\n",
			strerror(fStatus));
		return;
	}

	fScanSemaphore = create_sem(0, "MMC bus scan");
	fLockSemaphore = create_sem(1, "MMC bus lock");
	fWorkerThread = spawn_kernel_thread(_WorkerThread, "SD bus controller",
		B_NORMAL_PRIORITY, this);
	resume_thread(fWorkerThread);

	fController->set_scan_semaphore(fCookie, fScanSemaphore);
}


MMCBus::~MMCBus()
{
	CALLED();

	// Tell the worker thread we want to stop
	fStatus = B_SHUTTING_DOWN;

	// Delete the semaphores (this will unlock the worker thread if it was
	// waiting on them)
	delete_sem(fScanSemaphore);
	delete_sem(fLockSemaphore);

	// Wait for the worker thread to terminate
	status_t result;
	if (fWorkerThread != 0)
		wait_for_thread(fWorkerThread, &result);

	delete fSdioCard;
}


status_t
MMCBus::InitCheck()
{
	return fStatus;
}


void
MMCBus::Rescan()
{
	// Just wake up the thread for a scan
	release_sem(fScanSemaphore);
}


status_t
MMCBus::ExecuteCommand(uint16_t rca, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->execute_command(fCookie, command, argument, response);
}


status_t
MMCBus::DoIO(uint16_t rca, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->do_io(fCookie, command, operation, offsetAsSectors);
}


status_t
MMCBus::ExecuteBufferCommand(uint16_t rca, uint8_t command, uint32_t argument,
	uint32_t* response, const mmc_buffer_transfer* transfer)
{
	if (fController->execute_buffer_command == NULL)
		return B_NOT_SUPPORTED;
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->execute_buffer_command(fCookie, command, argument,
		response, transfer);
}


bool
MMCBus::HostSupportsSdio() const
{
	return fController->execute_buffer_command != NULL;
}


bool
MMCBus::_TrySdio()
{
	// Beacon: SDIO cards answer CMD5 (IO_SEND_OP_COND, R4); memory cards do
	// not. Called with the bus lock held, after CMD0 and CMD8, like Linux
	// mmc_rescan_try_freq() and OpenBSD sdmmc_io_enable().
	if (!HostSupportsSdio())
		return false;

	if (fSdioCard != NULL) {
		// Rescan: the card is not removable, keep what we have.
		TRACE_ALWAYS("sdio: rescan, card RCA %#x already enumerated\n",
			fSdioCard->Rca());
		return true;
	}

	uint32_t ocr = 0;
	status_t status = ExecuteCommand(0, SD_IO_SEND_OP_COND, 0, &ocr);
	if (status != B_OK) {
		TRACE("sdio: no answer to CMD5 (%s), not an SDIO card\n",
			strerror(status));
		return false;
	}

	dprintf("sdio: card answers CMD5, OCR %#" B_PRIx32 "\n", ocr);
	if ((ocr >> 28 & 7) == 0) {
		dprintf("sdio: card reports no I/O functions; treating it as a "
			"memory card\n");
		return false;
	}

	SdioCard* card = new(std::nothrow) SdioCard(this);
	if (card == NULL)
		return true;

	status = card->Enumerate(ocr);
	if (status != B_OK) {
		dprintf("sdio: enumeration FAILED: %s\n", strerror(status));
		delete card;
		// Do not fall into the SD memory init: it loops on SDIO cards.
		return true;
	}
	fSdioCard = card;

	// Publish without the bus lock: a function driver attaching from the
	// device manager may call into the bus right away.
	ReleaseBus();
	card->PublishFunctions(fNode);
	AcquireBus();
	return true;
}


void
MMCBus::SetClock(int frequency)
{
	fController->set_clock(fCookie, frequency);
}


void
MMCBus::SetBusWidth(int width)
{
	fController->set_bus_width(fCookie, width);
}


void
MMCBus::SetCardType(card_type type)
{
	fCardType = type;
	fController->set_card_type(fCookie, type);
}


status_t
MMCBus::PowerCycle()
{
	if (fController->power_cycle == NULL)
		return B_NOT_SUPPORTED;
	return fController->power_cycle(fCookie);
}


status_t
MMCBus::_ActivateDevice(uint16_t rca)
{
	// Do nothing if the device is already activated
	if (fActiveDevice == rca)
		return B_OK;

	uint32_t response;
	status_t result;
	result = fController->execute_command(fCookie, SELECT_DESELECT_CARD, ((uint32)rca) << 16,
		&response);

	if (result == B_OK)
		fActiveDevice = rca;

	return result;
}


void MMCBus::_AcquireScanSemaphore()
{
	ReleaseBus();
	acquire_sem(fScanSemaphore);
	AcquireBus();
}


void
MMCBus::_TerminateBus()
{
	fController->terminate_bus(fCookie);
}


status_t
MMCBus::_WorkerThread(void* cookie)
{
	MMCBus* bus = (MMCBus*)cookie;
	uint32_t response;

	bus->AcquireBus();

	// We assume the bus defaults to 400kHz clock and has already powered on
	// cards.

	// Reset all cards on the bus
	// This does not work if the bus has not been powered on yet (the command
	// will timeout), in that case we wait until asked to scan again when a
	// card has been inserted and powered on.
	status_t result;
	do {
		bus->_AcquireScanSemaphore();

		// Check if we need to exit early (possible if the parent device did
		// not manage initialize itself correctly)
		if (bus->fStatus == B_SHUTTING_DOWN) {
			bus->ReleaseBus();
			return B_OK;
		}

		TRACE("Reset the bus...\n");
		result = bus->ExecuteCommand(0, GO_IDLE_STATE, 0, NULL);
		TRACE("CMD0 result: %s\n", strerror(result));
	} while (result != B_OK);

	// Need to wait at least 8 clock cycles after CMD0 before sending the next
	// command. With the default 400kHz clock that would be 20 microseconds,
	// but we need to wait at least 20ms here, otherwise the next command times
	// out
	snooze(30000);

	while (bus->fStatus != B_SHUTTING_DOWN) {
		if (bus->fSdioCard != NULL) {
			// Beacon: an enumerated SDIO card is not removable; a rescan
			// must not drop it back to 400 kHz and 1 bit.
			TRACE_ALWAYS("sdio: rescan ignored, SDIO card RCA %#x stays\n",
				bus->fSdioCard->Rca());
			bus->_AcquireScanSemaphore();
			continue;
		}
		TRACE("Scanning the bus\n");

		// Use the low speed clock and 1bit bus width for scanning
		bus->SetClock(400);
		bus->SetBusWidth(1);

		// Probe the voltage range
		enum {
			// Table 4-40 in physical layer specification v8.00
			// All other values are currently reserved
			HOST_27_36V = 1, // Host supplied voltage 2.7-3.6V
		};

		// An arbitrary value, we just need to check that the response
		// containts the same.
		static const uint8 kVoltageCheckPattern = 0xAA;
		uint8_t cardType = CARD_TYPE_SD;
		// FIXME MMC cards will not reply to this! They expect CMD1 instead
		// SD v1 cards will also not reply, but we can proceed to ACMD41
		// If ACMD41 also does not work, it may be an SDIO card, too
		uint32_t probe = (HOST_27_36V << 8) | kVoltageCheckPattern;
		uint32_t hcs = 1 << 30;
		uint32_t ocr;
		status_t status = bus->ExecuteCommand(0, SD_SEND_IF_COND, probe, &response);

		// Beacon: SDIO-only cards (Wi-Fi) answer CMD5 and nothing else of
		// the memory init. Combo cards: only the I/O part is used for now.
		if (bus->_TrySdio()) {
			bus->_AcquireScanSemaphore();
			continue;
		}
		if (status != B_OK) {
			TRACE("Card does not implement CMD8, may be a V1 SD or MMC card\n");
			// Do not check for SDHC support in this case
			hcs = 0;

			TRACE("Trying MMC CMD1 initialization...\n");
			do {
				status = bus->ExecuteCommand(0, MMC_SEND_OP_COND, 0xFF8000, &ocr);
				// full voltage window, byte addressable, should look into this.
				if (status != B_OK) {
					TRACE("MMC CMD1 failed\n");
					break;
				}
				if ((ocr & (1 << 31)) == 0) {
					TRACE("MMC card is busy\n");
					snooze(100000);
				}
			} while ((ocr & (1 << 31)) == 0);

			if (status == B_OK && (ocr & (1 << 31)) != 0) {
				TRACE("Detected MMC card after CMD1\n");
				if ((ocr & (1 << 30)) != 0)
					cardType = CARD_TYPE_MMC_EXTENDED_CAPACITY;
				else
					cardType = CARD_TYPE_MMC;
			}
		} else if (response != probe) {
			ERROR("Card does not support voltage range (expected %x, "
				"reply %x)\n", probe, response);
			bus->_TerminateBus();
			bus->ReleaseBus();
			return B_ERROR;
		}

		// Probe OCR, waiting for card to become ready
		// We keep repeating ACMD41 until the card replies that it is
		// initialized. For MMC we already probed using CMD1 above.
		if ((cardType != CARD_TYPE_MMC) && (cardType != CARD_TYPE_MMC_EXTENDED_CAPACITY)) {
			do {
				uint32_t cardStatus;
				while (bus->ExecuteCommand(0, SD_APP_CMD, 0, &cardStatus) == B_BUSY) {
					ERROR("Card locked after CMD8...\n");
					snooze(1000000);
				}
				if ((cardStatus & 0xFFFF8000) != 0)
					ERROR("SD card reports error %x\n", cardStatus);
				if ((cardStatus & (1 << 5)) == 0)
					ERROR("Card did not enter ACMD mode\n");

				bus->ExecuteCommand(0, SD_SEND_OP_COND, hcs | 0xFF8000, &ocr);

				if ((ocr & (1 << 31)) == 0) {
					TRACE("Card is busy\n");
					snooze(100000);
				}
			} while ((ocr & (1 << 31)) == 0);
		}

		// FIXME this should be asked to each card, when there are multiple
		// ones. So ACMD41 should be moved inside the probing loop below?
		if (cardType == CARD_TYPE_SD) {
			if ((ocr & hcs) != 0)
				cardType = CARD_TYPE_SDHC;
			if ((ocr & (1 << 29)) != 0)
				cardType = CARD_TYPE_UHS2;
			if ((ocr & (1 << 24)) != 0)
				TRACE("Card supports 1.8v");
		}
		TRACE("Voltage range: %x\n", ocr & 0xFFFFFF);

		// Set the card type so the next commands have the correct reply types
		// (MMC and SD commands with the same identifier sometime expect different responses)
		bus->SetCardType((card_type)cardType);

		// TODO send CMD11 to switch to low voltage mode if card supports it?

		// We use CMD2 (ALL_SEND_CID) and CMD3 (SEND_RELATIVE_ADDR) to assign
		// an RCA to all cards. Initially all cards have an RCA of 0 and will
		// all receive CMD2. But only one of them will reply (they do collision
		// detection while sending the CID in reply). We assign a new RCA to
		// that first card, and repeat the process with the remaining ones
		// until no one answers to CMD2. Then we know all cards have an RCA
		// (and a matching published device on our side).
		uint32_t cid[4];
		uint32_t vendor;
		char name[7];
		uint32_t serial;
		uint16_t revision;
		uint8_t month;
		uint16_t year;
		uint16_t rca;
		bool cardFound = false;
		// This being an if statement as opposed to a while statement restricts
		// it to one device per bus.
		if ((cardType == CARD_TYPE_MMC) || (cardType == CARD_TYPE_MMC_EXTENDED_CAPACITY)) {
			if (bus->ExecuteCommand(0, ALL_SEND_CID, 0, cid) == B_OK) {
				// We currently support only a single card, so use a fixed RCA.
				rca = 1;
				status
					= bus->ExecuteCommand(0, MMC_SET_RELATIVE_ADDR, ((uint32)rca) << 16, &response);
				TRACE("MMC RCA: %x Status: %x\n", rca, response & 0xFFFF);
				if (status != B_OK) {
					TRACE("Failed to set RCA for MMC card\n");
				} else {
					MMCCid mmcCid(cid);
					vendor = mmcCid.VendorID();
					mmcCid.ProductName(name);
					serial = mmcCid.ProductSerial();
					revision = mmcCid.ProductRevision();
					month = mmcCid.ManufactureMonth();
					year = mmcCid.ManufactureYear(true);
					TRACE("MMC CID: MID=%" B_PRIu32 ", name=\"%s\", PSN=%" B_PRIu32
						  ", PRV=%u, MDT=%u/%u\n",
						vendor, name, serial, revision, month, year);
					cardFound = true;
				}
			}
		} else if (bus->ExecuteCommand(0, ALL_SEND_CID, 0, cid) == B_OK) {
			bus->ExecuteCommand(0, SD_SEND_RELATIVE_ADDR, 0, &response);

			TRACE("RCA: %x Status: %x\n", response >> 16, response & 0xFFFF);

			if ((response & 0xFF00) != 0x500) {
				TRACE("Card did not enter data state\n");
				// This probably means there are no more cards to scan on the
				// bus, so exit the loop.
				break;
			}

			// The card now has an RCA and it entered the data phase, which
			// means our initializing job is over, we can pass it on to the
			// mmc_disk driver.
			rca = response >> 16;
			SDCid sdCid(cid);
			vendor = sdCid.VendorID();
			sdCid.ProductName(name);
			serial = sdCid.ProductSerial();
			revision = sdCid.ProductRevision();
			month = sdCid.ManufactureMonth();
			year = sdCid.ManufactureYear();
			cardFound = true;
		}

		if (cardFound) {
			device_attr attrs[] = {
				{ B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc" }},
				{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "mmc device" }},
				{ B_DEVICE_VENDOR_ID, B_UINT32_TYPE, {.ui32 = vendor}},
				{ B_DEVICE_ID, B_STRING_TYPE, {.string = name}},
				{ B_DEVICE_UNIQUE_ID, B_UINT32_TYPE, {.ui32 = serial}},
				{ "mmc/revision", B_UINT16_TYPE, {.ui16 = revision}},
				{ "mmc/month", B_UINT8_TYPE, {.ui8 = month}},
				{ "mmc/year", B_UINT16_TYPE, {.ui16 = year}},
				{ kMmcRcaAttribute, B_UINT16_TYPE, {.ui16 = rca}},
				{ kMmcTypeAttribute, B_UINT8_TYPE, {.ui8 = cardType}},
				{}
			};

			// publish child device for the card
			gDeviceManager->register_node(bus->fNode, MMC_BUS_MODULE_NAME,
				attrs, NULL, NULL);
		}

		// TODO if there is a single card active, check if it supports CMD6
		// (spec version 1.10 or later in SCR). If it does, check if CMD6 can
		// enable high speed mode, use that to go to 50MHz instead of 25.
		bus->SetClock(25000);

		// FIXME we also need to unpublish devices that are gone. Probably need
		// to "ping" all RCAs somehow? Or is there an interrupt we can look for
		// to detect added/removed cards?

		// Wait for the next scan request
		// The thread will spend most of its time waiting here
		bus->_AcquireScanSemaphore();
	}

	bus->ReleaseBus();

	TRACE("poller thread terminating");
	return B_OK;
}
