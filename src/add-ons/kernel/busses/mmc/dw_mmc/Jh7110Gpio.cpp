/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Minimal StarFive JH7110 SYS GPIO helper, see Jh7110Gpio.h.
 */


#include "Jh7110Gpio.h"

#include <stdio.h>

#include <vm/vm.h>


#define GPIO_INFO(x...)		dprintf("jh7110_gpio: " x)
#define GPIO_ERROR(x...)	dprintf("jh7110_gpio: ERROR: " x)


static inline void
gpio_barrier()
{
#if defined(__riscv)
	__asm__ __volatile__("fence iorw,iorw" ::: "memory");
#else
	__sync_synchronize();
#endif
}


Jh7110SysGpio::Jh7110SysGpio()
	:
	fArea(-1),
	fBase(NULL)
{
	B_INITIALIZE_SPINLOCK(&fLock);
}


Jh7110SysGpio::~Jh7110SysGpio()
{
	if (fArea >= 0)
		delete_area(fArea);
}


status_t
Jh7110SysGpio::Init(phys_addr_t base)
{
	void* address = NULL;
	fArea = map_physical_memory("jh7110 sysgpio (dw_mmc)", base, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (fArea < 0) {
		GPIO_ERROR("cannot map sysgpio at %#" B_PRIxPHYSADDR "\n", base);
		return fArea;
	}
	fBase = (volatile uint8*)address;
	GPIO_INFO("sysgpio mapped at %#" B_PRIxPHYSADDR "\n", base);
	return B_OK;
}


uint32
Jh7110SysGpio::_Read(uint32 offset)
{
	uint32 value = *(volatile uint32*)(fBase + offset);
	gpio_barrier();
	return value;
}


void
Jh7110SysGpio::_Write(uint32 offset, uint32 value)
{
	gpio_barrier();
	*(volatile uint32*)(fBase + offset) = value;
}


void
Jh7110SysGpio::_Update(uint32 offset, uint32 mask, uint32 value)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&fLock);
	_Write(offset, (_Read(offset) & ~mask) | (value & mask));
	release_spinlock(&fLock);
	restore_interrupts(state);
}


static const char*
dout_name(uint32 dout, char* buffer, size_t size)
{
	if (dout == kJh7110GpoutLow)
		return "LOW";
	if (dout == kJh7110GpoutHigh)
		return "HIGH";
	snprintf(buffer, size, "peripheral signal %" B_PRIu32, dout);
	return buffer;
}


static const char*
doen_name(uint32 doen, char* buffer, size_t size)
{
	if (doen == kJh7110GpoenEnable)
		return "output";
	if (doen == kJh7110GpoenDisable)
		return "input (output off)";
	snprintf(buffer, size, "OE from signal %" B_PRIu32, doen);
	return buffer;
}


static void
padcfg_describe(uint32 pad, char* buffer, size_t size)
{
	const char* bias = "none";
	if ((pad & kJh7110PadPu) != 0 && (pad & kJh7110PadPd) != 0)
		bias = "up+down?";
	else if ((pad & kJh7110PadPu) != 0)
		bias = "pull-up";
	else if ((pad & kJh7110PadPd) != 0)
		bias = "pull-down";
	static const int kDriveMa[] = { 2, 4, 8, 12 };
	snprintf(buffer, size, "IE %d, SMT %d, bias %s, drive %d mA, slew %s",
		(pad & kJh7110PadIe) ? 1 : 0, (pad & kJh7110PadSmt) ? 1 : 0, bias,
		kDriveMa[(pad & kJh7110PadDsMask) >> 1],
		(pad & kJh7110PadSlew) ? "fast" : "slow");
}


void
Jh7110SysGpio::LogPin(int pin, const char* label)
{
	if (fBase == NULL || pin < 0 || pin >= kJh7110SysGpioCount)
		return;

	uint32 word = 4 * (pin / 4);
	uint32 shift = 8 * (pin % 4);
	uint32 doen = (_Read(kJh7110GpioDoen + word) >> shift)
		& kJh7110GpioDoenMask;
	uint32 dout = (_Read(kJh7110GpioDout + word) >> shift)
		& kJh7110GpioDoutMask;
	uint32 pad = _Read(kJh7110GpioPadcfg + 4 * pin);
	bool level = GetInput(pin);

	char doutBuffer[32];
	char doenBuffer[32];
	char padBuffer[80];
	padcfg_describe(pad, padBuffer, sizeof(padBuffer));
	GPIO_INFO("GPIO%d (%s): DOEN %" B_PRIu32 " = %s, DOUT %" B_PRIu32
		" = %s, input level %d, PADCFG %#" B_PRIx32 " (%s)\n", pin, label,
		doen, doen_name(doen, doenBuffer, sizeof(doenBuffer)), dout,
		dout_name(dout, doutBuffer, sizeof(doutBuffer)), level ? 1 : 0, pad,
		padBuffer);
}


void
Jh7110SysGpio::LogSd0Pads()
{
	// SD0 pads are dedicated (no function select); only PADCFG matters.
	static const char* const kNames[] = { "CLK", "CMD", "DATA0", "DATA1",
		"DATA2", "DATA3", "DATA4", "DATA5", "DATA6", "DATA7", "STRB" };
	if (fBase == NULL)
		return;
	for (int pad = kJh7110PadSd0Clk; pad <= kJh7110PadSd0Strb; pad++) {
		uint32 value = _Read(kJh7110GpioPadcfg + 4 * pad);
		char buffer[80];
		padcfg_describe(value, buffer, sizeof(buffer));
		GPIO_INFO("pad SD0_%s (%d): PADCFG %#" B_PRIx32 " (%s)\n",
			kNames[pad - kJh7110PadSd0Clk], pad, value, buffer);
	}
}


status_t
Jh7110SysGpio::SetMux(int pin, uint32 dout, uint32 doen, int din,
	uint32 padcfg)
{
	if (fBase == NULL)
		return B_NO_INIT;
	if (pin < 0 || pin >= kJh7110SysGpioCount)
		return B_BAD_VALUE;

	uint32 word = 4 * (pin / 4);
	uint32 shift = 8 * (pin % 4);
	_Update(kJh7110GpioDout + word, kJh7110GpioDoutMask << shift,
		(dout & kJh7110GpioDoutMask) << shift);
	_Update(kJh7110GpioDoen + word, kJh7110GpioDoenMask << shift,
		(doen & kJh7110GpioDoenMask) << shift);
	if (din >= 0) {
		uint32 inWord = 4 * (din / 4);
		uint32 inShift = 8 * (din % 4);
		_Update(kJh7110GpioGpi + inWord, 0x7fu << inShift,
			(uint32)(pin + 2) << inShift);
	}
	_Update(kJh7110GpioPadcfg + 4 * pin, 0xff, padcfg & 0xff);
	return B_OK;
}


status_t
Jh7110SysGpio::SetOutput(int pin, bool high)
{
	if (fBase == NULL)
		return B_NO_INIT;
	if (pin < 0 || pin >= kJh7110SysGpioCount)
		return B_BAD_VALUE;

	uint32 word = 4 * (pin / 4);
	uint32 shift = 8 * (pin % 4);
	_Update(kJh7110GpioDout + word, kJh7110GpioDoutMask << shift,
		(uint32)(high ? kJh7110GpoutHigh : kJh7110GpoutLow) << shift);
	_Update(kJh7110GpioDoen + word, kJh7110GpioDoenMask << shift,
		(uint32)kJh7110GpoenEnable << shift);
	_Update(kJh7110GpioPadcfg + 4 * pin,
		kJh7110PadIe | kJh7110PadSmt | kJh7110PadPu | kJh7110PadPd, 0);

	GPIO_INFO("GPIO%d driven %s\n", pin, high ? "HIGH" : "LOW");
	return B_OK;
}


bool
Jh7110SysGpio::GetInput(int pin)
{
	if (fBase == NULL || pin < 0 || pin >= kJh7110SysGpioCount)
		return false;
	return (_Read(kJh7110GpioIn + 4 * (pin / 32)) & (1u << (pin % 32))) != 0;
}
