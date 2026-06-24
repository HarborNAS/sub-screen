#ifndef SERVICE_H
#define SERVICE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUBSCREEN_SERVICE_NAME L"HarborOSSubscreenService"

BOOL InstallService(void);
BOOL UninstallService(void);
int RunSubscreenConsole(BOOL mockUsb, BOOL once);
int RunFirmwareVersion(void);
int RunFirmwareUpdate(const char* firmwarePath);
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

#ifdef __cplusplus
}
#endif

#endif // SERVICE_H
