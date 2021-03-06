/*
 * Copyright (c) 2012 Andrzej Surowiec <emeryth@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_nvic.h"
#include "inc/hw_gpio.h"
#include "inc/hw_hibernate.h"
#include "driverlib/debug.h"
#include "driverlib/fpu.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/udma.h"
#include "driverlib/sysctl.h"
#include "driverlib/hibernate.h"
#include "utils/uartstdio.h"

#include "usblib/usblib.h"
#include "usblib/usb-ids.h"
#include "usblib/device/usbdevice.h"
#include "usblib/device/usbdmsc.h"

#include "usb_config.h"
#include "common.h"
#include "ramdisk.h"

#ifdef CRYPTO
#include "crypto.h"
#endif

tDMAControlTable uDMAControlTable[64] __attribute__ ((aligned(1024)));

unsigned long massStorageEventCallback(void* callback, unsigned long event, unsigned long messageParameters, void* messageData)
{
	switch(event) {
		case USBD_MSC_EVENT_WRITING:
			break;
		case USBD_MSC_EVENT_READING:
			break;
		case USBD_MSC_EVENT_IDLE:
			break;
		default:
			break;
	}
	return 0;
}

void __attribute__((naked))
JumpToProgram(unsigned long ulStartAddr)
{
	// Set the vector table to the beginning of the app in flash
	HWREG(NVIC_VTABLE) = ulStartAddr;

	// Load the stack pointer from the application's vector table
	__asm("    ldr     r1, [r0]\n"
	      "    mov     sp, r1");

	// Load the initial PC from the application's vector table and branch to the application's entry point
	__asm("    ldr     r0, [r0, #4]\n"
	      "    bx      r0\n");
}


void CallUserProgram()
{
#ifdef CRYPTO
	if (checkCryptoSignature()) {
#endif
#ifdef DEBUGUART
		UARTprintf("Jumping to user program.\n\n");
#endif
		// Shortly blink with green LED to indicate that signature is OK
		ROM_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, LED_GREEN);
		ROM_GPIOPinWrite(GPIO_PORTF_BASE, LED_GREEN, LED_GREEN);
		ROM_SysCtlDelay(ROM_SysCtlClockGet() / 4 / 8);
		ROM_GPIOPinWrite(GPIO_PORTF_BASE, LED_GREEN, 0);
		JumpToProgram(UPLOAD_CODE_START);
#ifdef CRYPTO
	} else {
		// Blink the red LED and halt if the crypto signature does not match
		ROM_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, LED_RED);
		while (1) {
			ROM_GPIOPinWrite(GPIO_PORTF_BASE, LED_RED, LED_RED);
			ROM_SysCtlDelay(ROM_SysCtlClockGet() / 4 / 4);
			ROM_GPIOPinWrite(GPIO_PORTF_BASE, LED_RED, 0);
			ROM_SysCtlDelay(ROM_SysCtlClockGet() / 4 / 4);
		}
	}
#endif
}

int main(void)
{
	// We are waking from hibernation, jump to the user program
	if (HWREG(HIB_RIS) & HIBERNATE_INT_PIN_WAKE) {
		JumpToProgram(UPLOAD_CODE_START);
	}

	// Set the clocking
	ROM_SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN);

#ifdef DEBUGUART
	// In debug mode, the bootloader prints out debug info via UART
	ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
	ROM_GPIOPinConfigure(GPIO_PA0_U0RX);
	ROM_GPIOPinConfigure(GPIO_PA1_U0TX);
	ROM_GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
	UARTStdioInit(0);
#endif

	// Setup GPIO for buttons
	ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
	HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY_DD;
	HWREG(GPIO_PORTF_BASE + GPIO_O_CR) |= 0x01;
	HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = 0;

	ROM_GPIODirModeSet(GPIO_PORTF_BASE, BTN_LEFT,  GPIO_DIR_MODE_IN);
	ROM_GPIODirModeSet(GPIO_PORTF_BASE, BTN_RIGHT, GPIO_DIR_MODE_IN);
	ROM_GPIOPadConfigSet(GPIO_PORTF_BASE, BTN_LEFT,  GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
	ROM_GPIOPadConfigSet(GPIO_PORTF_BASE, BTN_RIGHT, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

	// If one of the buttons is not pressed, jump to the user program
	if (ROM_GPIOPinRead(GPIO_PORTF_BASE, BTN_LEFT) || ROM_GPIOPinRead(GPIO_PORTF_BASE, BTN_RIGHT)) {
		CallUserProgram();
	}

	// Both buttons pressed, start up the bootloader ...

	// Enable lazy stacking for interrupt handlers.  This allows floating-point
	// instructions to be used within interrupt handlers, but at the expense of
	// extra stack usage
	ROM_FPULazyStackingEnable();

	// Configure and enable uDMA
	ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_UDMA);
	ROM_SysCtlDelay(10);
	ROM_uDMAControlBaseSet(&uDMAControlTable[0]);
	ROM_uDMAEnable();

	// Configure the required pins for USB operation
	ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
	ROM_GPIOPinTypeUSBAnalog(GPIO_PORTD_BASE, GPIO_PIN_5 | GPIO_PIN_4);

	// Set the USB stack mode to Device mode with VBUS monitoring.
	USBStackModeSet(0, USB_MODE_DEVICE, 0);

	// Pass our device information to the USB library and place the device on the bus
	USBDMSCInit(0, (tUSBDMSCDevice*)&massStorageDevice);

#ifdef DEBUGUART
	UARTprintf("Bootloader started\n\n");
#endif

	ROM_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, LED_BLUE);
	while(1) {
		// Blink the blue LED so the user knows we're in a bootloader mode
		ROM_GPIOPinWrite(GPIO_PORTF_BASE, LED_BLUE, LED_BLUE);
		ROM_SysCtlDelay(ROM_SysCtlClockGet() / 4 / 2);
		ROM_GPIOPinWrite(GPIO_PORTF_BASE, LED_BLUE, 0);
		ROM_SysCtlDelay(ROM_SysCtlClockGet() / 4 / 2);
	}
}
