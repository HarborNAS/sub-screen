#ifndef USB_COMM_H
#define USB_COMM_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUBSCREEN_VENDOR_ID          0x5448
#define SUBSCREEN_PRODUCT_ID         0x0002
#define SUBSCREEN_TIMEOUT_MS         5000
#define SUBSCREEN_EXPECTED_EP_IN     0x81
#define SUBSCREEN_EXPECTED_EP_OUT    0x02

typedef struct SubscreenUsbDevice SubscreenUsbDevice;

const GUID* SubscreenDeviceInterfaceGuid(void);
BOOL UsbOpen(SubscreenUsbDevice** outDevice);
void UsbClose(SubscreenUsbDevice* device);
BOOL UsbWrite(SubscreenUsbDevice* device, const uint8_t* data, DWORD dataSize, DWORD timeoutMs);
BOOL UsbRead(SubscreenUsbDevice* device, uint8_t* buffer, DWORD bufferSize, DWORD* bytesRead, DWORD timeoutMs);
BOOL UsbProbe(FILE* output);
BOOL UsbSetupCheck(FILE* output);
DWORD UsbLastError(void);

#ifdef __cplusplus
}
#endif

#endif // USB_COMM_H
