/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * SDIO function interface of the Beacon MMC bus manager (bus_managers/mmc).
 *
 * This is the contract between the MMC/SDIO bus manager and SDIO function
 * drivers (first user: the bwfm Broadcom FullMAC Wi-Fi port). It is plain C:
 * a struct of function pointers plus device manager attributes.
 *
 * ATTACHING
 *
 * After the bus manager has enumerated an SDIO card (CMD5, CMD3, CMD7, CCCR,
 * CIS), it registers one device node per I/O function 1..7 as a child of the
 * "MMC bus root" node. Each node carries the attributes below and uses
 * SDIO_FUNCTION_MODULE_NAME as its driver module, which implements
 * sdio_bus_interface.
 *
 * The nodes are generic (no B_DEVICE_TYPE), so the device manager looks for
 * drivers in "drivers/<context>" when something probes that context, for
 * example drivers/net for /dev/net. A driver's supports_device(parent) checks
 * B_DEVICE_BUS == SDIO_BUS_NAME and the vendor/device/function attributes.
 * Its init_driver() gets the interface and the per-function cookie with:
 *
 *	device_node* parent = gDeviceManager->get_parent_node(node);
 *	sdio_bus_interface* sdio;
 *	void* cookie;
 *	gDeviceManager->get_driver(parent, (driver_module_info**)&sdio, &cookie);
 *	gDeviceManager->put_node(parent);
 *
 * A driver may also find the node itself with get_next_child_node() and the
 * same attributes.
 *
 * Broadcom FullMAC chips (bwfm/brcmfmac) need functions 0, 1 and 2 of the
 * same card. Every call therefore takes an explicit function number 0..7, and
 * any cookie of a card gives access to all of its functions. Attach to the
 * function 1 node only (as OpenBSD bwfm does) and use 0 and 2 through it.
 *
 * CALLING RULES
 *
 * - All calls may sleep. Never call them from an interrupt handler or with
 *   a spinlock held. They are serialised by the bus lock. Each call is atomic
 *   on its own; a driver that needs a multi-call sequence to be atomic
 *   serialises with its own lock (the card has only one driver).
 * - Buffers are kernel virtual addresses, any alignment. Data moves by PIO
 *   on the first host (dw_mmc), so no DMA rules apply. User buffers are not
 *   allowed.
 * - Multi-byte values are transferred as bytes in bus order (SDIO is
 *   little endian, like riscv64).
 * - Errors: B_OK, B_TIMED_OUT (no response or data timeout), B_IO_ERROR (CRC
 *   or R5 error flags), B_BAD_VALUE (bad argument), B_NOT_SUPPORTED (the host
 *   controller lacks a feature, e.g. SDIO interrupts).
 *
 * See src/add-ons/kernel/bus_managers/mmc/README_SDIO.md.
 */
#ifndef _SDIO_BUS_H
#define _SDIO_BUS_H


#include <device_manager.h>


#define SDIO_FUNCTION_MODULE_NAME	"bus_managers/mmc/sdio_function/driver_v1"

// Interface version, increased on incompatible changes (see
// ~/beacon/shared/sdio_bus.CHANGES).
#define SDIO_BUS_INTERFACE_VERSION	1


// #pragma mark - device node attributes (one node per function 1..7)

#define SDIO_BUS_NAME				"sdio"
	// value of B_DEVICE_BUS
#define SDIO_FUNCTION_ITEM			"sdio/function"
	// uint8: I/O function number of this node, 1..7
#define SDIO_VENDOR_ITEM			"sdio/vendor"
	// uint16: CISTPL_MANFID manufacturer of the function; the card's
	// common (function 0) value if the function has no MANFID tuple.
	// Broadcom = 0x02d0. Also published as B_DEVICE_VENDOR_ID (uint16).
#define SDIO_DEVICE_ITEM			"sdio/device"
	// uint16: CISTPL_MANFID card id, same fallback. BCM43455/43456 = 0x4345.
	// Also published as B_DEVICE_ID (uint16).
#define SDIO_CLASS_ITEM				"sdio/class"
	// uint8: FBR standard SDIO function interface code (0 = none,
	// 0x07 = WLAN, 0x02 = Bluetooth type A, ...; 0x0f = see extended code)
#define SDIO_FUNCTION_COUNT_ITEM	"sdio/function_count"
	// uint8: number of I/O functions of the card (from the CMD5 OCR)
#define SDIO_RCA_ITEM				"sdio/rca"
	// uint16: relative card address


// #pragma mark - CMD53 flags

#define SDIO_RW_INCREMENT			0x01
	// Incrementing address (memory window). Without it the address is
	// fixed (a FIFO port), like OpenBSD sdmmc_io_{read,write}_multi_1.
#define SDIO_RW_BLOCK_MODE			0x02
	// rw_extended() only: 'count' is in blocks of the function's current
	// block size instead of bytes.


// #pragma mark - registers (SDIO Simplified Specification, section 6.9)

enum {
	SDIO_CCCR_REVISION			= 0x00,	// [3:0] CCCR rev, [7:4] SDIO rev
	SDIO_CCCR_SD_REVISION		= 0x01,
	SDIO_CCCR_IO_ENABLE			= 0x02,
	SDIO_CCCR_IO_READY			= 0x03,
	SDIO_CCCR_INT_ENABLE		= 0x04,	// bit 0 = master IEN, bit n = fn n
	SDIO_CCCR_INT_PENDING		= 0x05,
	SDIO_CCCR_ABORT				= 0x06,	// [2:0] abort fn, bit 3 = RES
	SDIO_CCCR_BUS_INTERFACE		= 0x07,
	SDIO_CCCR_CARD_CAPABILITY	= 0x08,
	SDIO_CCCR_CIS_POINTER		= 0x09,	// 3 bytes, little endian
	SDIO_CCCR_BUS_SUSPEND		= 0x0c,
	SDIO_CCCR_FUNCTION_SELECT	= 0x0d,
	SDIO_CCCR_EXEC_FLAGS		= 0x0e,
	SDIO_CCCR_READY_FLAGS		= 0x0f,
	SDIO_CCCR_FN0_BLOCK_SIZE	= 0x10,	// 2 bytes, little endian
	SDIO_CCCR_POWER_CONTROL		= 0x12,
	SDIO_CCCR_SPEED				= 0x13,	// bit 0 SHS, bit 1 EHS
	SDIO_CCCR_UHS_SUPPORT		= 0x14,
	SDIO_CCCR_DRIVE_STRENGTH	= 0x15,
	SDIO_CCCR_INT_EXTENSION		= 0x16,
};

#define SDIO_FBR_BASE(function)		((uint32)(function) * 0x100)
enum {
	SDIO_FBR_STD_INTERFACE		= 0x00,
	SDIO_FBR_EXT_INTERFACE		= 0x01,
	SDIO_FBR_POWER				= 0x02,
	SDIO_FBR_CIS_POINTER		= 0x09,	// 3 bytes
	SDIO_FBR_CSA_POINTER		= 0x0c,	// 3 bytes
	SDIO_FBR_CSA_DATA			= 0x0f,
	SDIO_FBR_BLOCK_SIZE			= 0x10,	// 2 bytes
};


// #pragma mark - types


// Card interrupt handler. Called from the bus manager's "sdio irq" kernel
// thread (not in interrupt context), so it may sleep and use this interface
// (CMD52/CMD53) to read and clear the interrupt source. The host's card
// interrupt stays masked until all handlers have returned. A handler must
// not call install_interrupt_handler()/remove_interrupt_handler() (they wait
// for running handlers). If the host interrupt never fires, the bus manager
// polls CCCR INT_PENDING every 100 ms while a handler is installed.
typedef void (*sdio_interrupt_handler)(void* data);


typedef struct sdio_function_info {
	uint16		vendor;			// CISTPL_MANFID (0 if none, see attributes)
	uint16		device;
	uint8		interface_class;	// FBR standard interface code
	uint8		extended_class;		// FBR extended code (if class 0x0f)
	uint16		max_block_size;	// from CISTPL_FUNCE (0 if unknown)
	uint16		block_size;		// current block size (set_block_size)
	uint8		enabled;		// IOEx bit as last written by us
	uint8		has_interrupt_handler;
	uint32		cis_pointer;
} sdio_function_info;


typedef struct sdio_card_info {
	uint32		size;			// set by the caller: sizeof(sdio_card_info)
	uint8		function;		// function number of the queried node
	uint8		function_count;	// I/O functions 1..function_count exist
	uint16		rca;
	uint32		ocr;			// CMD5 R4 OCR as first read
	uint8		cccr_revision;	// CCCR[0] bits 3:0
	uint8		sdio_revision;	// CCCR[0] bits 7:4
	uint8		sd_revision;	// CCCR[1]
	uint8		card_capability;	// CCCR[8]
	uint8		bus_width;		// 1 or 4
	uint8		high_speed;		// 1 if CCCR EHS is set and the clock is > 25 MHz
	uint16		host_max_block_size;
	uint32		clock_khz;		// requested bus clock
	uint16		vendor;			// common CIS (function 0) CISTPL_MANFID
	uint16		device;
	char		product[64];	// CISTPL_VERS_1 strings, joined with ", "
	sdio_function_info functions[8];	// index = function number, [0] = fn0
} sdio_card_info;


typedef struct sdio_bus_interface {
	driver_module_info info;

	status_t	(*get_card_info)(void* cookie, sdio_card_info* info);
		// info->size must be set by the caller.

	// CMD52 (IO_RW_DIRECT), one byte; address is 17 bits.
	status_t	(*read_byte)(void* cookie, uint8 function, uint32 address,
					uint8* _value);
	status_t	(*write_byte)(void* cookie, uint8 function, uint32 address,
					uint8 value);
	status_t	(*write_read_byte)(void* cookie, uint8 function,
					uint32 address, uint8 value, uint8* _readBack);
		// write with the RAW flag: _readBack gets the register after the
		// write

	// One CMD53 (IO_RW_EXTENDED), exactly as given:
	//  byte mode:  count = bytes, 1..512 (sent as 0 for 512)
	//  block mode (SDIO_RW_BLOCK_MODE): count = blocks of the function's
	//              current block size, 1..511; the card must support
	//              multi-block (CCCR[8] SMB) or this returns B_NOT_SUPPORTED
	// flags: SDIO_RW_INCREMENT, SDIO_RW_BLOCK_MODE.
	status_t	(*rw_extended)(void* cookie, uint8 function, uint32 address,
					bool write, void* buffer, uint32 count, uint32 flags);

	// Any length, split into CMD53s like OpenBSD sdmmc_io_*_region_1 /
	// _multi_1: whole blocks in block mode (up to 511 per command), the
	// rest in byte mode (at most min(block size, 512) bytes per command).
	// With SDIO_RW_INCREMENT the address advances per command, otherwise
	// it stays fixed (FIFO port). SDIO_RW_BLOCK_MODE is ignored here.
	status_t	(*read)(void* cookie, uint8 function, uint32 address,
					void* buffer, size_t length, uint32 flags);
	status_t	(*write)(void* cookie, uint8 function, uint32 address,
					const void* buffer, size_t length, uint32 flags);

	// Writes FBR (or CCCR for function 0) block size registers.
	// 0 = min(512, host maximum, function maximum).
	status_t	(*set_block_size)(void* cookie, uint8 function,
					uint16 blockSize);

	// Sets IOEx and waits until IORx is set or the timeout (microseconds,
	// 0 = 1 second) expires; B_TIMED_OUT then, the IOEx bit stays set.
	status_t	(*enable_function)(void* cookie, uint8 function,
					bigtime_t timeout);
	status_t	(*disable_function)(void* cookie, uint8 function);

	// Card interrupt (in-band SDIO IRQ on DAT1). One handler per function
	// 1..7. Installing the first handler sets the master IEN bit and the
	// function's IENx bit and unmasks the host's SDIO interrupt; removing
	// the last one undoes that.
	status_t	(*install_interrupt_handler)(void* cookie, uint8 function,
					sdio_interrupt_handler handler, void* data);
	status_t	(*remove_interrupt_handler)(void* cookie, uint8 function);

	// Bus clock in kHz, capped by the host and by the card (25000 without
	// high speed, 50000 with). The bus manager already picks the fastest
	// working rate at enumeration; this is for drivers that want to slow
	// down (debugging) or go back up.
	status_t	(*set_bus_clock)(void* cookie, uint32 kilohertz);

	// CMD52 write of the function number to CCCR ABORT (aborts a CMD53 in
	// progress for that function).
	status_t	(*abort)(void* cookie, uint8 function);

	// Beacon: power-cycle the card (host reset GPIO) and enumerate it again,
	// to revive a card that stopped answering. Afterwards every function is
	// disabled, block sizes and interrupt enables are at their power-on
	// values: set them again. Installed handlers are kept. B_NOT_SUPPORTED
	// if the host cannot power-cycle the slot.
	status_t	(*reset_card)(void* cookie);
} sdio_bus_interface;


#endif	/* _SDIO_BUS_H */
