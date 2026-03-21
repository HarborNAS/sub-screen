#ifndef USB_COMM_H
#define USB_COMM_H

#include <windows.h>

// USB Device constants
#define VENDOR_ID   0x5448
#define PRODUCT_ID  0x0002
#define TIMEOUT_MS  5000

// Function declarations
BOOL FindUSBDevice(HANDLE* deviceHandle);
BOOL SendDataToDevice(HANDLE deviceHandle, const unsigned char* data, DWORD dataSize);
BOOL ReceiveDataFromDevice(HANDLE deviceHandle, unsigned char* buffer, DWORD bufferSize, DWORD* bytesRead);

#endif // USB_COMM_H