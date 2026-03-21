#ifndef SERVICE_H
#define SERVICE_H

#include <windows.h>

// Function declarations
BOOL InstallService(void);
BOOL UninstallService(void);
void WINAPI ServiceMain(DWORD argc, LPTSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

#endif // SERVICE_H