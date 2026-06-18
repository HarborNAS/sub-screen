#include "protocol.h"
#include "service.h"
#include "usb_comm.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static void PrintUsage(void)
{
    printf("HarborOS Subscreen Windows service\n");
    printf("\n");
    printf("Usage:\n");
    printf("  subscreen.exe install\n");
    printf("  subscreen.exe uninstall\n");
    printf("  subscreen.exe probe\n");
    printf("  subscreen.exe setup-check\n");
    printf("  subscreen.exe selftest\n");
    printf("  subscreen.exe fw-version\n");
    printf("  subscreen.exe fw-update <firmware.bin>\n");
    printf("  subscreen.exe console [--mock-usb] [--once]\n");
    printf("\n");
    printf("No arguments: run as Windows service.\n");
}

int main(int argc, char* argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "install") == 0) {
            return InstallService() ? 0 : 1;
        }

        if (strcmp(argv[1], "uninstall") == 0) {
            return UninstallService() ? 0 : 1;
        }

        if (strcmp(argv[1], "probe") == 0) {
            return UsbProbe(stdout) ? 0 : 1;
        }

        if (strcmp(argv[1], "setup-check") == 0) {
            return UsbSetupCheck(stdout) ? 0 : 1;
        }

        if (strcmp(argv[1], "selftest") == 0) {
            return ProtocolSelfTest(TRUE) ? 0 : 1;
        }

        if (strcmp(argv[1], "fw-version") == 0) {
            return RunFirmwareVersion();
        }

        if (strcmp(argv[1], "fw-update") == 0) {
            if (argc < 3) {
                printf("fw-update requires a firmware file path.\n");
                PrintUsage();
                return 2;
            }
            return RunFirmwareUpdate(argv[2]);
        }

        if (strcmp(argv[1], "console") == 0) {
            BOOL mockUsb = FALSE;
            BOOL once = FALSE;

            for (int i = 2; i < argc; ++i) {
                if (strcmp(argv[i], "--mock-usb") == 0) {
                    mockUsb = TRUE;
                } else if (strcmp(argv[i], "--once") == 0) {
                    once = TRUE;
                } else {
                    printf("Unknown console option: %s\n", argv[i]);
                    PrintUsage();
                    return 2;
                }
            }

            return RunSubscreenConsole(mockUsb, once);
        }

        PrintUsage();
        return 2;
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)SUBSCREEN_SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD err = GetLastError();
        printf("Failed to start service control dispatcher (%lu).\n", err);
        PrintUsage();
        return (int)err;
    }

    return 0;
}
