/*
 * Copyright 2012-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		François Revol, revol@free.fr
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */
#ifndef _KERNEL_ARCH_DEBUG_UART_8250_H
#define _KERNEL_ARCH_DEBUG_UART_8250_H


#include <sys/types.h>

#include <SupportDefs.h>

#include "debug_uart.h"


#define UART_KIND_8250 "8250"
#define UART_KIND_8250_DW "8250-dw"
	// Synopsys DesignWare APB UART: registers 4 bytes apart, 32-bit access


class DebugUART8250 : public DebugUART {
public:
							DebugUART8250(addr_t base, int64 clock);
							~DebugUART8250();

			void			InitEarly();
			void			Init();
			void			InitPort(uint32 baud);

			int				PutChar(char c);
			int				GetChar(bool wait);

			void			FlushTx();
			void			FlushRx();
};


class DebugUART8250DW : public DebugUART8250 {
public:
							DebugUART8250DW(addr_t base, int64 clock)
								: DebugUART8250(base, clock) {}
protected:
			void			Out8(int reg, uint8 value);
			uint8			In8(int reg);
};


DebugUART8250* arch_get_uart_8250(addr_t base, int64 clock);
DebugUART8250* arch_get_uart_8250_dw(addr_t base, int64 clock);


#endif /* _KERNEL_ARCH_DEBUG_UART_8250_H */
