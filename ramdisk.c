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

#include "ramdisk.h"
#include "common.h"

#include "inc/hw_flash.h"
#include "inc/hw_memmap.h"
#include "inc/hw_sysctl.h"
#include "inc/hw_types.h"
#include "driverlib/flash.h"

#ifdef DEBUGUART
#include "utils/uartstdio.h"
#endif

#define WBVAL(x) ((x) & 0xFF), (((x) >> 8) & 0xFF)
#define QBVAL(x) ((x) & 0xFF), (((x) >> 8) & 0xFF), (((x) >> 16) & 0xFF), (((x) >> 24) & 0xFF)

#define BLOCK_SIZE 512
#define BYTES_PER_SECTOR 512
#define SECTORS_PER_CLUSTER 4
#define RESERVED_SECTORS 1
#define FAT_COPIES 2
#define ROOT_ENTRIES 512
#define ROOT_ENTRY_LENGTH 32
#define FIRMWARE_BIN_CLUSTER 3
#define DATA_REGION_SECTOR (RESERVED_SECTORS + FAT_COPIES + (ROOT_ENTRIES * ROOT_ENTRY_LENGTH) / BYTES_PER_SECTOR)
#define FIRMWARE_START_SECTOR (DATA_REGION_SECTOR + (firmware_start_cluster - 2) * SECTORS_PER_CLUSTER)

int massStorageDrive = 0;
int newFirmwareStartSet = 0;
unsigned long firmware_start_cluster = FIRMWARE_BIN_CLUSTER;

unsigned char bootSector[] = {
	0xeb, 0x3c, 0x90,                                      // Code to jump to the bootstrap code
	'm', 'k', 'd', 'o', 's', 'f', 's', 0x00,               // OEM ID
	WBVAL(BYTES_PER_SECTOR),                               // Bytes per sector (512)
	SECTORS_PER_CLUSTER,                                   // Sectors per cluster (4)
	WBVAL(RESERVED_SECTORS),                               // Reserved sectors (1)
	FAT_COPIES,                                            // Number of FAT copies (2)
	WBVAL(ROOT_ENTRIES),                                   // Number of possible root entries (512)
	0x00, 0x04,                                            // Small number of sectors (1024)
	0xf8,                                                  // Media descriptor (0xf8 - Fixed disk)
	0x01, 0x00,                                            // Sectors per FAT (1)
	0x20, 0x00,                                            // Sectors per track (32)
	0x40, 0x00,                                            // Number of heads (64)
	0x00, 0x00, 0x00, 0x00,                                // Hidden sectors (0)
	0x00, 0x00, 0x00, 0x00,                                // Large number of sectors (0)
	0x00,                                                  // Drive number (0)
	0x00,                                                  // Reserved
	0x29,                                                  // Extended boot signature
	0x69, 0x17, 0xad, 0x53,                                // Volume serial number
	'F', 'I', 'R', 'M', 'W', 'A', 'R', 'E', ' ', ' ', ' ', // Volume label
	'F', 'A', 'T', '1', '2', ' ', ' ', ' ',                // Filesystem type
	// Bootstrap code, not really needed
	/*
	0x0e, 0x1f, 0xbe, 0x5b, 0x7c, 0xac, 0x22, 0xc0, 0x74, 0x0b,
	0x56, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0x5e, 0xeb, 0xf0, 0x32,
	0xe4, 0xcd, 0x16, 0xcd, 0x19, 0xeb, 0xfe, 0x54, 0x68, 0x69, 0x73, 0x20,
	0x69, 0x73, 0x20, 0x6e, 0x6f, 0x74, 0x20, 0x61, 0x20, 0x62, 0x6f, 0x6f,
	0x74, 0x61, 0x62, 0x6c, 0x65, 0x20, 0x64, 0x69, 0x73, 0x6b, 0x2e, 0x20,
	0x20, 0x50, 0x6c, 0x65, 0x61, 0x73, 0x65, 0x20, 0x69, 0x6e, 0x73, 0x65,
	0x72, 0x74, 0x20, 0x61, 0x20, 0x62, 0x6f, 0x6f, 0x74, 0x61, 0x62, 0x6c,
	0x65, 0x20, 0x66, 0x6c, 0x6f, 0x70, 0x70, 0x79, 0x20, 0x61, 0x6e, 0x64,
	0x0d, 0x0a, 0x70, 0x72, 0x65, 0x73, 0x73, 0x20, 0x61, 0x6e, 0x79, 0x20,
	0x6b, 0x65, 0x79, 0x20, 0x74, 0x6f, 0x20, 0x74, 0x72, 0x79, 0x20, 0x61,
	0x67, 0x61, 0x69, 0x6e, 0x20, 0x2e, 0x2e, 0x2e, 0x20, 0x0d, 0x0a, 0x00
	*/
};

unsigned char fatTable[] = {
	0xf8, 0xff, 0xff, 0x00, 0x40, 0x00, 0x05, 0x60, 0x00, 0x07, 0x80, 0x00,
	0x09, 0xa0, 0x00, 0x0b, 0xc0, 0x00, 0x0d, 0xe0, 0x00, 0x0f, 0x00, 0x01,
	0x11, 0x20, 0x01, 0x13, 0x40, 0x01, 0x15, 0x60, 0x01, 0x17, 0x80, 0x01,
	0x19, 0xa0, 0x01, 0x1b, 0xc0, 0x01, 0x1d, 0xe0, 0x01, 0x1f, 0x00, 0x02,
	0x21, 0x20, 0x02, 0x23, 0x40, 0x02, 0x25, 0x60, 0x02, 0x27, 0x80, 0x02,
	0x29, 0xa0, 0x02, 0x2b, 0xc0, 0x02, 0x2d, 0xe0, 0x02, 0x2f, 0x00, 0x03,
	0x31, 0x20, 0x03, 0x33, 0x40, 0x03, 0x35, 0x60, 0x03, 0x37, 0x80, 0x03,
	0x39, 0xa0, 0x03, 0x3b, 0xc0, 0x03, 0x3d, 0xe0, 0x03, 0x3f, 0x00, 0x04,
	0x41, 0x20, 0x04, 0x43, 0x40, 0x04, 0x45, 0x60, 0x04, 0x47, 0x80, 0x04,
	0x49, 0xa0, 0x04, 0x4b, 0xc0, 0x04, 0x4d, 0xe0, 0x04, 0x4f, 0x00, 0x05,
	0x51, 0x20, 0x05, 0x53, 0x40, 0x05, 0x55, 0x60, 0x05, 0x57, 0x80, 0x05,
	0x59, 0xa0, 0x05, 0x5b, 0xc0, 0x05, 0x5d, 0xe0, 0x05, 0x5f, 0x00, 0x06,
	0x61, 0x20, 0x06, 0x63, 0x40, 0x06, 0x65, 0x60, 0x06, 0x67, 0x80, 0x06,
	0x69, 0xa0, 0x06, 0x6b, 0xc0, 0x06, 0x6d, 0xe0, 0x06, 0x6f, 0x00, 0x07,
	0x71, 0x20, 0x07, 0x73, 0x40, 0x07, 0x75, 0x60, 0x07, 0x77, 0x80, 0x07,
	0x79, 0xa0, 0x07, 0x7b, 0xc0, 0x07, 0x7d, 0xe0, 0x07, 0x7f, 0x00, 0x08,
	0x81, 0x20, 0x08, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

unsigned char dirEntry[] = {
	// A long filename entry for firmware.bin
	0x41,                                                             // Sequence number
	'f', 0x00, 'i', 0x00, 'r', 0x00, 'm', 0x00, 'w', 0x00,            // Five name characters in UTF-16
	0x0f,                                                             // Attributes
	0x00,                                                             // Type
#ifdef CRYPTO
	0x14,                                                             // Checksum of DOS filename
	'a', 0x00, 'r', 0x00, 'e', 0x00, '.', 0x00, 's', 0x00, 'i', 0x00, // Six name characters in UTF-16
	0x00, 0x00,                                                       // First cluster
	'g', 0x00, 0x00, 0x00,                                            // Two name characters in UTF-16
#else
	0x57,                                                             // Checksum of DOS filename
	'a', 0x00, 'r', 0x00, 'e', 0x00, '.', 0x00, 'b', 0x00, 'i', 0x00, // Six name characters in UTF-16
	0x00, 0x00,                                                       // First cluster
	'n', 0x00, 0x00, 0x00,                                            // Two name characters in UTF-16
#endif
	// 8.3 entry
	'F', 'I', 'R', 'M', 'W', 'A', 'R', 'E', // Filename
#ifdef CRYPTO
	'S', 'I', 'G',                          // Extension
#else
	'B', 'I', 'N',                          // Extension
#endif
	0x20,                                   // Attribute byte
	0x00,                                   // Reserved for Windows NT
	0x00,                                   // Creation millisecond
	0xce, 0x01,                             // Creation time
	0x86, 0x41,                             // Creation date
	0x86, 0x41,                             // Last access date
	0x00, 0x00,                             // Reserved for FAT32
	0xce, 0x01,                             // Last write time
	0x86, 0x41,                             // Last write date
	WBVAL(FIRMWARE_BIN_CLUSTER),            // Starting cluster
	QBVAL(UPLOAD_LENGTH)                    // File size in bytes
};

void *massStorageOpen(unsigned long drive)
{
	return ((void *)&massStorageDrive);
}

void massStorageClose(void *drive)
{
}

unsigned long massStorageRead(void *drive, unsigned char *data, unsigned long blockNumber, unsigned long numberOfBlocks)
{
#ifdef DEBUGUART
	UARTprintf("Reading %d block(s) starting at %d\n", numberOfBlocks, blockNumber);
#endif
	for (int i = 0; i < BLOCK_SIZE; i++) {
		data[i] = 0;
	}
	if (blockNumber == 0) {
		for (int i = 0; i < sizeof(bootSector); i++) {
			data[i] = bootSector[i];
		}
		// The boot sector signature AA55h at the end
		data[510] = 0x55;
		data[511] = 0xAA;
	}
	else if (blockNumber == 1 || blockNumber == 2) {
		for (int i = 0; i < sizeof(fatTable); i++) {
			data[i] = fatTable[i];
		}
	}
	else if (blockNumber == 3) {
		for (int i = 0; i < sizeof(dirEntry); i++) {
			data[i] = dirEntry[i];
		}
	}
	else if (blockNumber >= FIRMWARE_START_SECTOR && blockNumber < FIRMWARE_START_SECTOR + UPLOAD_LENGTH / BLOCK_SIZE) {
#ifdef NOREAD
		unsigned char dummy[] = "READ DISABLED   ";
		for (int i = 0; i < BLOCK_SIZE; i++) {
			data[i] = dummy[i % 16];
		}
#else
		for (int i = 0; i < BLOCK_SIZE; i++) {
			data[i] = ((unsigned char *)(UPLOAD_START + (blockNumber - FIRMWARE_START_SECTOR) * BLOCK_SIZE))[i];
		}
#endif
	}
	return BLOCK_SIZE;
}

unsigned long massStorageWrite(void *drive, unsigned char *data, unsigned long blockNumber, unsigned long numberOfBlocks)
{
#ifdef DEBUGUART
	UARTprintf("Writing %d block(s) starting at %d\n", numberOfBlocks, blockNumber);
	UARTprintf("Firmware start cluster: %d\n", firmware_start_cluster);
	for (int j = 0; j < BLOCK_SIZE * numberOfBlocks; j += 16) {
		for (int i = 0; i < 16; i++) {
			UARTprintf("%02x ",data[j+i]);
		}
		UARTprintf("\n");
	}
#endif
	if (blockNumber == 0) {
		for (int i = 0; i < sizeof(bootSector); i++) {
			bootSector[i] = data[i];
		}
	}
	else if (blockNumber == 1 || blockNumber == 2) {
		for (int i = 0; i < sizeof(fatTable); i++) {
			fatTable[i] = data[i];
		}
	}
	else if (blockNumber == 3) {
		for (int i = 0; i < sizeof(dirEntry); i++) {
			dirEntry[i] = data[i];
		}
	}
	else if (blockNumber >= DATA_REGION_SECTOR) {
		if (!newFirmwareStartSet) {
			// the host tried to write actual data to the data region, we assume this is the new firmware
			newFirmwareStartSet = 1;
			firmware_start_cluster = (blockNumber - DATA_REGION_SECTOR) / SECTORS_PER_CLUSTER + 2;
		}
		// new firmware is being uploaded
		if (blockNumber < FIRMWARE_START_SECTOR + UPLOAD_LENGTH / BLOCK_SIZE) {
			unsigned long address = (blockNumber - FIRMWARE_START_SECTOR) * BLOCK_SIZE + UPLOAD_START;
			// erase
			if (blockNumber == FIRMWARE_START_SECTOR) {
				for (int counter = 0; counter < UPLOAD_LENGTH / 1024; counter++) {
					FlashErase(UPLOAD_START + counter * 1024);
				}
			}
			FlashProgram((unsigned long *)data, address, BLOCK_SIZE * numberOfBlocks);
			return BLOCK_SIZE * numberOfBlocks;
		}
	}
	return BLOCK_SIZE * numberOfBlocks;
}

unsigned long massStorageNumBlocks(void *drive)
{
	// filesystem size is 512kB
	return 1024;
}
