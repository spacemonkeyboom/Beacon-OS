/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Device manager module for SDIO function nodes ("SDIO function N"): the
 * sdio_bus_interface (headers/private/drivers/sdio_bus.h) that function
 * drivers such as bwfm get from their parent node.
 */


#include <new>
#include <string.h>

#include "mmc_bus.h"
#include "sdio.h"
#include "sdio_bus.h"


#define SDIO_TRACE(x...)	dprintf("sdio: " x)


struct sdio_function_cookie {
	SdioCard*	card;
	uint8		function;
};


static inline SdioCard*
card_of(void* cookie)
{
	return ((sdio_function_cookie*)cookie)->card;
}


static status_t
sdio_function_init(device_node* node, void** _cookie)
{
	uint8 function = 0;
	if (gDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_ITEM, &function,
			false) != B_OK) {
		return B_BAD_VALUE;
	}

	// Parent = "MMC bus root" node, its cookie is the MMCBus.
	device_node* parent = gDeviceManager->get_parent_node(node);
	driver_module_info* module;
	void* busCookie = NULL;
	status_t status = gDeviceManager->get_driver(parent, &module, &busCookie);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	MMCBus* bus = (MMCBus*)busCookie;
	if (bus == NULL || bus->Sdio() == NULL)
		return B_NO_INIT;

	sdio_function_cookie* cookie = new(std::nothrow) sdio_function_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->card = bus->Sdio();
	cookie->function = function;
	*_cookie = cookie;
	SDIO_TRACE("function %u node initialised for a driver\n", function);
	return B_OK;
}


static void
sdio_function_uninit(void* cookie)
{
	delete (sdio_function_cookie*)cookie;
}


static status_t
sdio_get_card_info(void* cookie, sdio_card_info* info)
{
	return card_of(cookie)->GetCardInfo(
		((sdio_function_cookie*)cookie)->function, info);
}


static status_t
sdio_read_byte(void* cookie, uint8 function, uint32 address, uint8* _value)
{
	return card_of(cookie)->ReadByte(function, address, _value);
}


static status_t
sdio_write_byte(void* cookie, uint8 function, uint32 address, uint8 value)
{
	return card_of(cookie)->WriteByte(function, address, value, NULL);
}


static status_t
sdio_write_read_byte(void* cookie, uint8 function, uint32 address,
	uint8 value, uint8* _readBack)
{
	if (_readBack == NULL)
		return B_BAD_VALUE;
	return card_of(cookie)->WriteByte(function, address, value, _readBack);
}


static status_t
sdio_rw_extended(void* cookie, uint8 function, uint32 address, bool write,
	void* buffer, uint32 count, uint32 flags)
{
	return card_of(cookie)->RwExtended(function, address, write, buffer,
		count, flags);
}


static status_t
sdio_read(void* cookie, uint8 function, uint32 address, void* buffer,
	size_t length, uint32 flags)
{
	return card_of(cookie)->Transfer(function, address, false,
		(uint8*)buffer, length, flags);
}


static status_t
sdio_write(void* cookie, uint8 function, uint32 address, const void* buffer,
	size_t length, uint32 flags)
{
	// The host only reads from the buffer on writes.
	return card_of(cookie)->Transfer(function, address, true,
		(uint8*)const_cast<void*>(buffer), length, flags);
}


static status_t
sdio_set_block_size(void* cookie, uint8 function, uint16 blockSize)
{
	return card_of(cookie)->SetBlockSize(function, blockSize);
}


static status_t
sdio_enable_function(void* cookie, uint8 function, bigtime_t timeout)
{
	return card_of(cookie)->EnableFunction(function, timeout);
}


static status_t
sdio_disable_function(void* cookie, uint8 function)
{
	return card_of(cookie)->DisableFunction(function);
}


static status_t
sdio_install_interrupt_handler(void* cookie, uint8 function,
	sdio_interrupt_handler handler, void* data)
{
	return card_of(cookie)->InstallInterruptHandler(function, handler, data);
}


static status_t
sdio_remove_interrupt_handler(void* cookie, uint8 function)
{
	return card_of(cookie)->RemoveInterruptHandler(function);
}


static status_t
sdio_set_bus_clock(void* cookie, uint32 kilohertz)
{
	return card_of(cookie)->SetBusClock(kilohertz);
}


static status_t
sdio_abort(void* cookie, uint8 function)
{
	return card_of(cookie)->Abort(function);
}


static status_t
sdio_reset_card(void* cookie)
{
	return card_of(cookie)->Reset();
}


static status_t
sdio_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;
		default:
			return B_ERROR;
	}
}


sdio_bus_interface gSdioFunctionModule = {
	{
		{
			SDIO_FUNCTION_MODULE_NAME,
			0,
			sdio_std_ops
		},
		NULL,	// supports_device (nodes are registered by the bus)
		NULL,	// register_device
		sdio_function_init,
		sdio_function_uninit,
		NULL,	// register_child_devices
		NULL,	// rescan
		NULL,	// device_removed
		NULL,	// suspend
		NULL	// resume
	},
	sdio_get_card_info,
	sdio_read_byte,
	sdio_write_byte,
	sdio_write_read_byte,
	sdio_rw_extended,
	sdio_read,
	sdio_write,
	sdio_set_block_size,
	sdio_enable_function,
	sdio_disable_function,
	sdio_install_interrupt_handler,
	sdio_remove_interrupt_handler,
	sdio_set_bus_clock,
	sdio_abort,
	sdio_reset_card
};
