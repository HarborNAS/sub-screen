# HarborOS Subscreen - Windows Version

A Windows service that monitors system statistics and displays them on a USB-connected screen device.

## Features

- **System Monitoring**: CPU usage, memory usage, disk usage, network activity, GPU temperature
- **USB Communication**: Communicates with HarborOS subscreen device via HID interface
- **Windows Service**: Runs as a background Windows service
- **Console Mode**: Can also run in console mode for testing

## Prerequisites

- Windows 10/11
- Visual Studio 2019/2022 with C++ development tools
- Windows SDK
- Administrator privileges (for service installation)

## Building

1. Open the project folder in Visual Studio Code
2. Make sure you have the "C/C++" extension installed
3. Run the build task: `Ctrl+Shift+P` → "Tasks: Run Build Task"

This will compile all source files into `subscreen.exe`.

## Installation

### As a Windows Service (Recommended)

1. Open Command Prompt as Administrator
2. Navigate to the build directory
3. Install the service:
   ```
   subscreen.exe install
   ```
4. Start the service:
   ```
   net start HarborOSSubscreenService
   ```

### Console Mode (For Testing)

Run the application in console mode to see system stats without installing as a service:

```
subscreen.exe console
```

## Usage

Once installed and running, the service will:

1. Monitor system statistics every second
2. Send formatted data to the connected USB screen device
3. Display CPU, memory, disk, network, and temperature information

## Protocol Compatibility

This Windows version now uses the same USB protocol as the Linux version, ensuring full compatibility with the HarborOS subscreen hardware. The implementation includes:

- **Protocol Structures**: Compatible Request/Ack structures with proper packing
- **Command Definitions**: GET, SET, AUTOSET, UPDATE commands
- **AIM Targets**: All system monitoring targets (CPU, Memory, Disk, Network, etc.)
- **CRC Validation**: Data integrity checking
- **USB Communication**: HID API with proper report formatting

### Data Format

The service now sends structured binary data packets instead of simple text:

```
Header: 0x5aa5 (signature)
Sequence: incremental counter
Length: packet length
Command: SET (0x02)
AIM: target system (System_AIM, Disk_AIM, etc.)
Data: system monitoring values
CRC: checksum
```

This ensures the Windows version works seamlessly with the same USB screen device as the Linux version.

## Troubleshooting

### Device Not Found
- Ensure the USB screen device is properly connected
- Check Device Manager for HID devices
- Run in console mode to verify device detection

### Service Won't Start
- Check Windows Event Viewer for error messages
- Ensure you have administrator privileges
- Try running in console mode first

### Build Errors
- Ensure Visual Studio and Windows SDK are properly installed
- Check that all required libraries are available
- Verify include paths in c_cpp_properties.json

## Uninstallation

To uninstall the service:

1. Stop the service:
   ```
   net stop HarborOSSubscreenService
   ```
2. Uninstall the service:
   ```
   subscreen.exe uninstall
   ```

## Development

### Project Structure
```
windows-version/
├── src/
│   ├── main.c              # Main entry point and service logic
│   ├── system_monitor.c    # System statistics monitoring
│   ├── usb_comm.c          # USB/HID communication
│   └── service.c           # Windows service functions
├── include/
│   ├── system_monitor.h
│   ├── usb_comm.h
│   └── service.h
├── .vscode/
│   ├── tasks.json          # Build tasks
│   ├── launch.json         # Debug configurations
│   └── c_cpp_properties.json # IntelliSense settings
└── README.md
```

### Adding New Features

1. Add new monitoring functions in `system_monitor.c`
2. Update the `SystemStats` structure if needed
3. Modify the data formatting in the service worker thread
4. Update USB communication protocol as required

## License

This project is part of the HarborOS ecosystem.