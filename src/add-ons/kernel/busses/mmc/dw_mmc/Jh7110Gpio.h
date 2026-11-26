/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, The Beacon Project.
 *
 * Minimal StarFive JH7110 SYS GPIO/pinctrl helper (sysgpio @ 0x13040000),
 * just enough for dw_mmc to drive the SDIO Wi-Fi reset line (Orange Pi RV:
 * WL_REG_ON = GPIO62) and to log the pin state firmware left behind.
 *
 * This is NOT a GPIO driver: there is no GPIO bus in Beacon yet. It touches
 * only the DOUT/DOEN byte and PADCFG word of the pin it drives (read-modify-
 * write of one 32-bit word with interrupts off on this CPU). The only other
 * Beacon user of this block, the StarFive PCIe driver (PERST# on GPIO26/28),
 * uses different DOUT/DOEN words. Replace with a real GPIO driver later.
 *
 * Register layout facts (no code copied) from Linux v6.12
 * drivers/pinctrl/starfive/pinctrl-starfive-jh7110.c and
 * pinctrl-starfive-jh7110-sys.c (GPL-2.0), see THIRD_PARTY.md.
 */
#ifndef _JH7110_GPIO_H_
#define _JH7110_GPIO_H_


#include <KernelExport.h>


enum {
	kJh7110SysGpioPhys		= 0x13040000,
	kJh7110SysGpioCount		= 64,

	// 4 pins per 32-bit word, one byte each
	kJh7110GpioDoen			= 0x000,	// [5:0] output-enable source, 0 = on
	kJh7110GpioDout			= 0x040,	// [6:0] output source, 0 = low, 1 = high
	kJh7110GpioGpi			= 0x080,	// input routing (signal -> pin + 2)
	kJh7110GpioIn			= 0x118,	// pin input levels, 2 words
	kJh7110GpioPadcfg		= 0x120,	// one word per pad 0..74

	kJh7110GpioDoenMask		= 0x3f,
	kJh7110GpioDoutMask		= 0x7f,
	kJh7110GpoutLow			= 0,
	kJh7110GpoutHigh		= 1,
	kJh7110GpoenEnable		= 0,
	kJh7110GpoenDisable		= 1,

	// PADCFG bits
	kJh7110PadIe			= 1 << 0,
	kJh7110PadDsMask		= 3 << 1,
	kJh7110PadPu			= 1 << 3,
	kJh7110PadPd			= 1 << 4,
	kJh7110PadSlew			= 1 << 5,
	kJh7110PadSmt			= 1 << 6,
	kJh7110PadPos			= 1 << 7,

	// Dedicated SDIO0 pads (dt-bindings PAD_SD0_CLK .. PAD_SD0_STRB)
	kJh7110PadSd0Clk		= 64,
	kJh7110PadSd0Strb		= 74,
};


class Jh7110SysGpio {
public:
							Jh7110SysGpio();
							~Jh7110SysGpio();

			status_t		Init(phys_addr_t base);

			// Read-only dumps, "jh7110_gpio:" lines.
			void			LogPin(int pin, const char* label);
			void			LogSd0Pads();

			// Pin as plain output with a constant level: DOUT, then DOEN,
			// then pad input/Schmitt/bias off (as Linux gpio direction_output
			// does).
			status_t		SetOutput(int pin, bool high);
			// Route a pin to a peripheral: DOUT/DOEN sources, the input
			// signal (din < 0: none) and the pad config (as Linux
			// jh7110_set_gpiomux() plus pinconf).
			status_t		SetMux(int pin, uint32 dout, uint32 doen, int din,
								uint32 padcfg);
			bool			GetInput(int pin);

private:
			uint32			_Read(uint32 offset);
			void			_Write(uint32 offset, uint32 value);
			void			_Update(uint32 offset, uint32 mask, uint32 value);

private:
			area_id			fArea;
			volatile uint8*	fBase;
			spinlock		fLock;
};


#endif	// _JH7110_GPIO_H_
